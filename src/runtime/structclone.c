#include "structclone.h"
#include "qsort.h"
#include "lutil.h"
#include "../hexdump.h"
#include "../lua/llimits.h" // LUAI_MAXSHORTLEN

enum SCTag {
    SCTag_HEADER = 0x0,
    SCTag_NIL    = 0x1,
    SCTag_BOOL   = 0x2, // first bit of tag val tells if its true or false
    SCTag_INTZ   = 0x3, // small integer embedded in tag, where value fits in 8-SCTagTypeBits bits
    SCTag_INT    = 0x4, // 8-byte integer
    SCTag_FLOAT  = 0x5,
    SCTag_STR1   = 0x6, // short string with 1-byte size prefix; up to 255 B
    SCTag_STR4   = 0x7, // long string with 4-byte size prefix; up to 4.0 GiB
    SCTag_ARRAY  = 0x8,
    SCTag_DICT   = 0x9,
    SCTag_FUN    = 0xA,
    SCTag_UVAL   = 0xB,
    SCTag_REFZ   = 0xC, // reference to an already-serialized value (embedded in tag)
    SCTag_REF    = 0xD, // reference to an already-serialized value (u24)
    // UNUSED    = 0xE,
    SCTag_MAX    = SCTag_REF
};

#define CODEC_VERSION 1 // stream codec version

// SCTagTypeBits: number of bits of tag used for type
#define SCTagTypeBits 4
#define SCTagTypeMask (((u8)1 << SCTagTypeBits) - 1)
#define SCTagTypeMax  SCTagTypeMask

// SCTagValBits: number of bits of tag used for embedded value
#define SCTagValBits  (8 - SCTagTypeBits)
#define SCTagValShift SCTagTypeBits
#define SCTagValMask  (((u8)1 << SCTagValBits) - 1)
#define SCTagValMax   SCTagValMask

#define SCTagValHasRef ((u8)1 << SCTagValShift)

#define SCTagValHeaderReservedMask ((u8)1 << SCTagValShift)

static_assert(SCTag_MAX <= SCTagTypeMax, "SCTagTypeBits too small for SCTag_MAX");

// REFTAB_IDX: stack index the reftab lives at
#define REFTAB_IDX 1

// COMPACT_REFMAP: set to false to disable compressed refmap (for debugging)
#ifndef COMPACT_REFMAP
    #define COMPACT_REFMAP true
#endif

// SC_TRACE: enable to dlog trace-level messages on stderr
// #define SC_TRACE 1
#if SC_TRACE
    #define sc_trace(fmt, ...) dlog("[sc_trace] " fmt, ##__VA_ARGS__)
#else
    #define sc_trace(fmt, ...) ((void)0)
#endif


typedef struct Encoder {
    Buf*  buf;
    usize buf_startoffs;
    int   err_no;
    u32   nrefs;
    bool  has_reftab;
} Encoder;

typedef struct Decoder {
    const u8* bufstart;
    const u8* bufend;
    const u8* buf;
    int       err_no;
    u32       nrefs;
    u32*      refmap; // used if nrefs > 256 (holds nrefs entries in heap memory)
    u32       refidxgen;
} Decoder;

static u8 g_reftabkey;

static void encode_value(lua_State* L, Encoder* enc, int vi);
static void decode_value(lua_State* L, Decoder* dec);


static const char* SCTagStr(u8 tag) {
    switch((enum SCTag)tag) {
        case SCTag_HEADER: return "HEADER";
        case SCTag_NIL:    return "NIL";
        case SCTag_BOOL:   return "BOOL";
        case SCTag_INTZ:   return "INTZ";
        case SCTag_INT:    return "INT";
        case SCTag_FLOAT:  return "FLOAT";
        case SCTag_STR1:   return "STR1";
        case SCTag_STR4:   return "STR4";
        case SCTag_ARRAY:  return "ARRAY";
        case SCTag_DICT:   return "DICT";
        case SCTag_FUN:    return "FUN";
        case SCTag_UVAL:   return "UVAL";
        case SCTag_REFZ:   return "REFZ";
        case SCTag_REF:    return "REF";
    }
    static char buf[4];
    sprintf(buf, "?%02x", tag);
    return buf;
}


__attribute__((format(printf, 4, 5)))
static void _codec_error(lua_State* L, int* err_no_dst, int err_no, const char* fmt, ...) {
    assert(err_no > 0);
    *err_no_dst = -err_no;

    va_list argp;
    va_start(argp, fmt);
    luaL_where(L, 1);
    lua_pushvfstring(L, fmt, argp);
    va_end(argp);
    lua_concat(L, 2);
    // dew API function will call lua_error(L)

    #if SC_TRACE
        panic("[sc_trace] %s", lua_tostring(L, -1));
    #endif
}

#define codec_error(L, enc_or_dec, _err_no, fmt, ...) \
    ( (enc_or_dec)->err_no ? ((void)0) : \
      _codec_error(L, &(enc_or_dec)->err_no, _err_no, fmt, ##__VA_ARGS__) )


__attribute__((noinline))
static void enc_error_nomem(lua_State* L, Encoder* enc) {
    codec_error(L, enc, ENOMEM, "Cannot allocate memory");
}

__attribute__((noinline))
static void dec_error_nomem(lua_State* L, Decoder* dec) {
    codec_error(L, dec, ENOMEM, "Cannot allocate memory");
}

__attribute__((noinline))
static void dec_error_short(lua_State* L, Decoder* dec) {
    codec_error(L, dec, EBADMSG, "Partial value");
}

__attribute__((noinline))
static void dec_error_bad(lua_State* L, Decoder* dec) {
    codec_error(L, dec, EBADMSG, "Invalid data");
}


static void enc_append_byte(lua_State* L, Encoder* enc, u8 byte) {
    if UNLIKELY(!buf_append_byte(enc->buf, byte))
        enc_error_nomem(L, enc);
}


static void enc_append(lua_State* L, Encoder* enc, const void* data, usize len) {
    if UNLIKELY(!buf_append(enc->buf, data, len))
        enc_error_nomem(L, enc);
}


// ———— ref encoder ————
//
// Encoder registers each referrable value in a lua table located at L[REFTAB_IDX].
// This table has the value as the key and tuple of byte offset & refno as the record value.
// The record value is encoded in a u64 as follows: (little endian)
//
// bit   1 ...                                 ... 40 41 ...            ... 64
//      ┌──────────────────── ~ ─────────────────────┬──────────── ~ ───────────┐
//      │                  offset                    │           refno          │
//      └──────────────────── ~ ─────────────────────┴──────────── ~ ───────────┘ offset  refno
// e.g.  00000100 00000000 00000000 00000000 00000000 00000010 00000000 00000000       4      2
//           0x04     0x00     0x00     0x00     0x00     0x02     0x00     0x00
//                0x0000000000000002
//                0x0000000000000000
//
//       00001111 00000000 00000000 00000000 00000000 00000000 00000000 00000000      15      0
//            0xf     0x00     0x00     0x00     0x00     0x00     0x00     0x00
//
//       00111001 00110000 00000000 00000000 00000000 00000001 00000000 00000000   12345      1
//           0x39     0x30     0x00     0x00     0x00     0x01     0x00     0x00
//
#define ENC_REFTUPLE_OFFSET(reftuple) ( (u64)(reftuple) >> 24 )
#define ENC_REFTUPLE_REFNO(reftuple)  ( (u64)(reftuple) & (((u64)1 << 24) - 1) )
#define ENC_REFTUPLE(offset, refno)   ( ((u64)(offset) << 24) | (u64)(refno) )


static bool encode_ref(lua_State* L, Encoder* enc, int idx) {
    // get integer value and remove from stack
    u64 reftuple = lua_tointeger(L, -1);
    u64 offset = ENC_REFTUPLE_OFFSET(reftuple);
    u32 refno;
    lua_pop(L, 1);

    // check if this is the first time this value is referenced
    if ((enc->buf->bytes[offset] & SCTagValHasRef) == 0) {
        // set "has_ref" flag on target's tag byte
        enc->buf->bytes[offset] |= SCTagValHasRef;

        // allocate ref index
        // store offset and index as value in table
        refno = enc->nrefs++;
        reftuple = ENC_REFTUPLE(offset, refno + 1);

        sc_trace("materializing ref#%u at offset %lu: %s", refno, offset, fmtlval(L, idx));
        sc_trace("  reftuple 0x%016lx (%lu, %lu)",
                 reftuple, ENC_REFTUPLE_OFFSET(reftuple), ENC_REFTUPLE_REFNO(reftuple));
        // hexdump(stderr, &reftuple, 8, 0, 16);

        // set reftab[key] = value
        lua_pushvalue(L, idx); // key
        lua_pushinteger(L, reftuple); // value
        // sc_trace("set reftab[%s] = %s", fmtlval(L, -2), fmtlval(L, -1));
        lua_rawset(L, REFTAB_IDX); // reftab[key] = value
    } else {
        refno = ENC_REFTUPLE_REFNO(reftuple) - 1;
        sc_trace("found ref#%u with target offset %lu: %s", refno, offset, fmtlval(L, idx));
    }

    if (refno <= (u32)SCTagValMax) {
        enc->buf->bytes[enc->buf->len++] = (u8)SCTag_REFZ | ((u8)refno << SCTagValShift);
    } else if (refno < (1u << 24)) {
        enc->buf->bytes[enc->buf->len] = SCTag_REF;
        memcpy(&enc->buf->bytes[enc->buf->len + 1], &refno, 3);
        enc->buf->len += 4;
    } else {
        // more refs than what fits in u24
        codec_error(L, enc, EOVERFLOW, "too many references");
    }

    return false;
}


static bool enc_ref_intern(lua_State* L, Encoder* enc, int idx) {
    if UNLIKELY(!buf_reserve(enc->buf, 8))
        return enc_error_nomem(L, enc), false;

    if (!enc->has_reftab) {
        // create "ref" table
        int nrec = 8; // lowball estimate of total number of refs
        lua_createtable(L, 0, nrec);
        lua_rotate(L, 1, REFTAB_IDX); // move reftab down into stack at REFTAB_IDX
        enc->has_reftab = true;
    } else {
        // look up
        // Push the key onto stack and then lookup key in reftab; push value onto stack.
        // lua_rawget returns the type of the value.
        lua_pushvalue(L, idx);
        int vtype = lua_rawget(L, REFTAB_IDX);
        if (vtype == LUA_TNUMBER)
            return encode_ref(L, enc, idx);
        lua_pop(L, 1); // remove result of 'load reftab[key]' from stack
    }

    // store offset and index as value in table
    u64 reftuple = ENC_REFTUPLE((uintptr)(enc->buf->len - enc->buf_startoffs), 0);

    // Add object: reftab[key] = value.
    // Push ref as value and object as key onto stack.
    sc_trace("registering ref at offset %lu: %s", reftuple, fmtlval(L, idx));
    lua_pushvalue(L, idx); // key
    lua_pushinteger(L, reftuple); // value
    lua_rawset(L, REFTAB_IDX); // reftab[key] = value
    return true;
}


// ———— ref decoder ————


inline static const u8* decode_compact_refmap(Decoder* dec) {
    assert(dec->nrefs <= 256);
    return dec->bufend;
}

static void decode_refx(lua_State* L, Decoder* dec, u32 refno) {
    lua_pushinteger(L, refno + 1); // +1 because lua arrays are 1-based
    // dlog_lua_stackf(L, "stack when looking up ref#%u", refno);
    int vtype = lua_rawget(L, REFTAB_IDX);
    if UNLIKELY(vtype == LUA_TNIL) {
        lua_pop(L, 1); // remove nil from stack
        return codec_error(L, dec, EBADMSG, "Unexpected ref#%d", refno);
    }
    sc_trace("decoded ref#%u to value of type %s", refno, lua_typename(L, lua_type(L, -1)));
}

static void decode_refz(lua_State* L, Decoder* dec) {
    u32 refno = dec->buf[0] >> SCTagValShift;
    dec->buf++;
    return decode_refx(L, dec, refno);
}

static void decode_ref(lua_State* L, Decoder* dec) {
    usize bufavail = dec->bufend - dec->buf;
    if UNLIKELY(bufavail < 4)
        return dec_error_short(L, dec);
    u32 refno = 0;
    memcpy(&refno, &dec->buf[1], 3);
    dec->buf += 4;
    return decode_refx(L, dec, refno);
}


static void dec_ref_register(lua_State* L, Decoder* dec) {
    assert(dec->nrefs > 0);
    u32 refidx = dec->refidxgen++;
    u32 refno;

    if UNLIKELY(refidx >= dec->nrefs) {
        // more values with ref tags than nrefs; bad data
        sc_trace("error: refidx >= dec->nrefs (%u >= %u)", refidx, dec->nrefs);
        return dec_error_bad(L, dec);
    } else if LIKELY (COMPACT_REFMAP && dec->nrefs <= 256) {
        const u8* refmap = decode_compact_refmap(dec);
        refno = refmap[refidx];
    } else {
        refno = dec->refmap[refidx];
    }

    lua_pushinteger(L, refno + 1); // key (+1 to make lua happy)
    lua_pushvalue(L, -2); // value
    sc_trace("registering ref#%u for value of type %s", refno, lua_typename(L, lua_type(L, -1)));
    lua_rawset(L, REFTAB_IDX); // reftab[key] = value
}


// ———— encode/decode functions ————


static void encode_nil(lua_State* L, Encoder* enc, int vi) {
    enc_append_byte(L, enc, SCTag_NIL);
}

static void decode_nil(lua_State* L, Decoder* dec) {
    lua_pushnil(L);
    dec->buf++;
}


static void encode_bool(lua_State* L, Encoder* enc, int vi) {
    u8 b = (u8)SCTag_BOOL | ((u8)lua_toboolean(L, vi) << SCTagValShift);
    enc_append_byte(L, enc, b);
}

static void decode_bool(lua_State* L, Decoder* dec) {
    int v = (int)((u8)dec->buf[0] >> SCTagValShift);
    lua_pushboolean(L, v);
    dec->buf++;
}


static void encode_number(lua_State* L, Encoder* enc, int vi) {
    int isint;
    lua_Integer vv = lua_tointegerx(L, vi, &isint);
    if (isint) {
        if (vv <= (lua_Integer)SCTagValMax) {
            enc_append_byte(L, enc, (u8)SCTag_INTZ | ((u8)vv << SCTagValShift));
        } else {
            enc_append_byte(L, enc, SCTag_INT);
            enc_append(L, enc, &vv, sizeof(vv));
        }
    } else {
        lua_Number vv = lua_tonumber(L, vi);
        enc_append_byte(L, enc, SCTag_FLOAT);
        enc_append(L, enc, &vv, sizeof(vv));
    }
}


static void decode_intz(lua_State* L, Decoder* dec) {
    lua_Integer n = (lua_Integer)(u64)((u8)dec->buf[0] >> SCTagValShift);
    dec->buf++;
    lua_pushinteger(L, n);
}


static void decode_int(lua_State* L, Decoder* dec) {
    usize bufavail = dec->bufend - dec->buf;
    if UNLIKELY (bufavail < 1 + sizeof(lua_Integer))
        return dec_error_short(L, dec);
    lua_Integer n;
    memcpy(&n, &dec->buf[1], sizeof(lua_Integer));
    lua_pushinteger(L, n);
    dec->buf += 1 + sizeof(lua_Integer);
}


static void decode_float(lua_State* L, Decoder* dec) {
    usize bufavail = dec->bufend - dec->buf;
    if UNLIKELY (bufavail < 1 + sizeof(lua_Number))
        return dec_error_short(L, dec);
    lua_Number n;
    memcpy(&n, &dec->buf[1], sizeof(lua_Number));
    lua_pushnumber(L, n);
    dec->buf += 1 + sizeof(lua_Number);
}


static void encode_str(lua_State* L, Encoder* enc, int vi) {
    usize len;
    const char* ptr = lua_tolstring(L, vi, &len);

    // create references for large strings
    if (len > LUAI_MAXSHORTLEN) {
        if (!enc_ref_intern(L, enc, vi))
            return;
    }

    usize header_len = (len <= 255) ? 2 : 5;
    u8* dst = buf_reserve(enc->buf, header_len + len);
    if UNLIKELY(!dst || len > U32_MAX) {
        if (!dst)
            return enc_error_nomem(L, enc);
        return codec_error(L, enc, EMSGSIZE, "String too large");
    }

    if (len <= 255) {
        dst[0] = SCTag_STR1;
        dst[1] = (u8)len;
        memcpy(&dst[2], ptr, len);
        enc->buf->len += len + 2;
    } else {
        u32 lenv = (u32)len;
        dst[0] = SCTag_STR4;
        memcpy(&dst[1], &lenv, 4);
        memcpy(&dst[5], ptr, len);
        enc->buf->len += len + 5;
    }
}

static void decode_str(lua_State* L, Decoder* dec) {
    usize bufavail = dec->bufend - dec->buf;
    usize header_len = (dec->buf[0] & SCTag_STR1) ? 2 : 5;
    if LIKELY(bufavail > header_len) {
        u32 len;
        if (dec->buf[0] & SCTag_STR1) {
            len = dec->buf[1];
        } else {
            memcpy(&len, &dec->buf[1], sizeof(len));
        }
        if LIKELY(bufavail >= len + header_len) {
            lua_pushlstring(L, (char*)&dec->buf[header_len], len);
            if (*dec->buf & SCTagValHasRef)
                dec_ref_register(L, dec);
            dec->buf += len + header_len;
            return;
        }
    }
    dec_error_short(L, dec);
}


static void encode_dict(lua_State* L, Encoder* enc, int vi) {
    sc_trace("encode dict");

    // 1 byte tag + 4 bytes count
    enc->buf->bytes[enc->buf->len] = SCTag_DICT;
    usize bufstart = enc->buf->len;
    enc->buf->len += 1 + sizeof(u32);
    u32 count = 0;

    lua_pushnil(L);
    while (lua_next(L, -2) != 0) {
        encode_value(L, enc, -2); // key
        encode_value(L, enc, -1); // value
        lua_pop(L, 1); // TODO: likely 2, not 1..?
        count++;
    }
    memcpy(&enc->buf->bytes[bufstart + 1], &count, sizeof(u32));
}


static void encode_array(lua_State* L, Encoder* enc, int vi, u32 count) {
    sc_trace("encode array len=%u", count);

    // 1 byte tag + 4 bytes count
    enc->buf->bytes[enc->buf->len] = SCTag_ARRAY;
    memcpy(&enc->buf->bytes[enc->buf->len + 1], &count, sizeof(u32));
    enc->buf->len += 1 + sizeof(u32);

    lua_pushnil(L);
    while (lua_next(L, -2) != 0) {
        encode_value(L, enc, -1);
        lua_pop(L, 1);
    }
}


static void encode_table(lua_State* L, Encoder* enc, int vi) {
    if (!enc_ref_intern(L, enc, vi))
        return;

    if UNLIKELY(!buf_reserve(enc->buf, 16))
        return enc_error_nomem(L, enc);

    u32 count = dew_lua_arraylen(L, -1);
    if (count == U32_MAX)
        return encode_dict(L, enc, vi);

    return encode_array(L, enc, vi, count);
}


static void decode_array(lua_State* L, Decoder* dec) {
    usize bufavail = dec->bufend - dec->buf;
    if UNLIKELY(bufavail < 5)
        return dec_error_short(L, dec);

    u32 count;
    memcpy(&count, dec->buf + 1, sizeof(count));

    lua_createtable(L, (int)MIN(count, (u32)I32_MAX), 0);

    if (*dec->buf & SCTagValHasRef)
        dec_ref_register(L, dec);

    dec->buf += 1 + sizeof(count); // tag + count

    for (u32 i = 1; i <= count && dec->err_no == 0 && dec->buf < dec->bufend; i++) {
        assert(dec->buf < dec->bufend);
        lua_pushinteger(L, i); // key
        decode_value(L, dec); // value
        lua_rawset(L, -3); // table[key] = value
        assert(dec->buf <= dec->bufend);
    }

    assertf(dec->buf <= dec->bufend, "buffer overrun");
}


static void decode_dict(lua_State* L, Decoder* dec) {
    usize bufavail = dec->bufend - dec->buf;
    if UNLIKELY(bufavail < 5)
        return dec_error_short(L, dec);

    u32 count;
    memcpy(&count, dec->buf + 1, sizeof(count));

    lua_createtable(L, 0, (int)MIN(count, (u32)I32_MAX));

    if (*dec->buf & SCTagValHasRef)
        dec_ref_register(L, dec);

    dec->buf += 1 + sizeof(count); // tag + count

    for (u32 i = 1; i <= count && dec->err_no == 0 && dec->buf < dec->bufend; i++) {
        assert(dec->buf < dec->bufend);
        decode_value(L, dec); // key
        decode_value(L, dec); // value
        lua_rawset(L, -3); // table[key] = value
        assert(dec->buf <= dec->bufend);
    }

    assertf(dec->buf <= dec->bufend, "buffer overrun");
}


static void encode_uval_buf(lua_State* L, Encoder* enc, int vi, Buf* buf) {
    if (!enc_ref_intern(L, enc, vi))
        return;

    usize needbytes = 2 + sizeof(u64) + buf->len; // header + uval_type + buf.len + buf.bytes
    if UNLIKELY(!buf_reserve(enc->buf, needbytes))
        return enc_error_nomem(L, enc);

    usize bufstart = enc->buf->len;
    u8* dst = &enc->buf->bytes[enc->buf->len];
    enc->buf->len += needbytes;

    *dst++ = SCTag_UVAL;
    *dst++ = buf->uval.type;
    u64 len = buf->len;
    memcpy(dst, &len, sizeof(u64)); dst += sizeof(u64);
    memcpy(dst, buf->bytes, buf->len);
}


static void decode_uval_buf(lua_State* L, Decoder* dec) {
    usize bufavail = dec->bufend - dec->buf;
    if UNLIKELY(bufavail < sizeof(u64))
        return dec_error_short(L, dec);
    u64 len;
    memcpy(&len, dec->buf, sizeof(u64)); dec->buf += sizeof(u64);
    if UNLIKELY(bufavail - sizeof(u64) < len)
        return dec_error_short(L, dec);

    Buf* buf = l_buf_createx(L, len);
    if UNLIKELY(!buf)
        return dec_error_nomem(L, dec);
    buf->len = (usize)len;
    memcpy(buf->bytes, dec->buf, len);
    dec->buf += len;

    assertf(dec->buf <= dec->bufend, "buffer overrun");
}


static void encode_uval(lua_State* L, Encoder* enc, int vi) {
    UVal* uval = assertnotnull(lua_touserdata(L, vi));
    switch ((enum UValType)uval->type) {
        case UValType_Buf:
            return encode_uval_buf(L, enc, vi, (Buf*)uval);
        case UValType_Timer:
        case UValType_RemoteTask:
        case UValType_IODesc:
            return codec_error(L, enc, EINVAL,
                               "Cannot clone value of type %s", uval_typename(L, vi));
    }
    return codec_error(L, enc, EINVAL, "Cannot clone value of type <userdata>");
}


static void decode_uval(lua_State* L, Decoder* dec) {
    usize bufavail = dec->bufend - dec->buf;
    if UNLIKELY(bufavail < 2) // header + uval_type
        return dec_error_short(L, dec);
    dec->buf++; // consume SCTag_UVAL
    u8 uval_type = *dec->buf++;
    if (uval_type == UValType_Buf)
        return decode_uval_buf(L, dec);
    dec_error_bad(L, dec);
}


static void encode_fun(lua_State* L, Encoder* enc, int vi) {
    if (!enc_ref_intern(L, enc, vi))
        return;

    if UNLIKELY(!buf_reserve(enc->buf, 32))
        return enc_error_nomem(L, enc);

    // get information about the function
    lua_Debug ar = {};
    lua_pushvalue(L, vi);
    lua_getinfo(L, ">Su", &ar);
    if UNLIKELY(ar.what[0] == 'C')
        return codec_error(L, enc, EINVAL, "Cannot encode C function");

    sc_trace("encoding function %p (args=%u, nups=%u)", lua_topointer(L, vi), ar.nparams, ar.nups);

    // 1 byte tag + 1 byte nups + 4 bytes len
    usize bufstart = enc->buf->len;
    enc->buf->bytes[bufstart++] = SCTag_FUN;
    enc->buf->bytes[bufstart++] = ar.nups;
    enc->buf->len += 2 + sizeof(u32);

    bool strip_debuginfo = false;
    int err = buf_append_luafun(enc->buf, L, strip_debuginfo);
    if UNLIKELY(err)
        return codec_error(L, enc, -err, "Failed to encode function");

    usize len = enc->buf->len - (bufstart + 4);
    if UNLIKELY(len > U32_MAX)
        return codec_error(L, enc, -err, "Function too large");
    u32 len32 = (u32)len;
    sc_trace("encoded function %p in %u B", lua_topointer(L, vi), 6 + len32);
    memcpy(&enc->buf->bytes[bufstart], &len32, sizeof(u32));

    // encode upvalues
    for (u32 i = 1; i <= (u32)ar.nups; i++) {
        lua_getupvalue(L, vi, i);
        encode_value(L, enc, -1);
        lua_pop(L, 1);
    }
}


static const char* decode_fun_reader(lua_State* L, void* ud, usize* lenp) {
    u32 len = ((uintptr*)ud)[1];
    *lenp = ((uintptr*)ud)[1];
    return (char*)((uintptr*)ud)[0];
}

static void decode_fun(lua_State* L, Decoder* dec) {
    usize bufavail = dec->bufend - dec->buf;
    if UNLIKELY(bufavail < 6)
        return dec_error_short(L, dec);

    u32 nups = dec->buf[1];
    u32 len;
    memcpy(&len, dec->buf + 2, sizeof(len));

    if UNLIKELY(bufavail < 6 + len)
        return dec_error_short(L, dec);

    uintptr v[2] = { (uintptr)(dec->buf + 6), len };
    int status = lua_load(L, decode_fun_reader, v, "=structclone", "b");
    if UNLIKELY (status != LUA_OK)
        return dec_error_bad(L, dec);

    if (*dec->buf & SCTagValHasRef)
        dec_ref_register(L, dec);

    dec->buf += 6 + len;

    // decode upvalues
    for (u32 i = 1; i <= nups; i++) {
        decode_value(L, dec);
        lua_setupvalue(L, -2, i);
    }
}


static void encode_value(lua_State* L, Encoder* enc, int vi) {
    int vt = lua_type(L, vi);
    sc_trace("encode_value: %s", fmtlval(L, vi));
    switch (vt) {
        case LUA_TNIL:      return_tail encode_nil(L, enc, vi);
        case LUA_TBOOLEAN:  return_tail encode_bool(L, enc, vi);
        case LUA_TNUMBER:   return_tail encode_number(L, enc, vi);
        case LUA_TSTRING:   return_tail encode_str(L, enc, vi);
        case LUA_TTABLE:    return_tail encode_table(L, enc, vi);
        case LUA_TFUNCTION: return_tail encode_fun(L, enc, vi);
        case LUA_TUSERDATA: return_tail encode_uval(L, enc, vi);
        // LUA_TLIGHTUSERDATA
        // LUA_TTHREAD
    }
    codec_error(L, enc, EINVAL, "Usupported value of type %s", lua_typename(L, vt));
}


static void decode_value(lua_State* L, Decoder* dec) {
    assert(dec->buf < dec->bufend);
    sc_trace("decode %s", SCTagStr(dec->buf[0] & SCTagTypeMask));
    switch ((enum SCTag)dec->buf[0] & SCTagTypeMask) {
        case SCTag_NIL:    return_tail decode_nil(L, dec);
        case SCTag_BOOL:   return_tail decode_bool(L, dec);
        case SCTag_INTZ:   return_tail decode_intz(L, dec);
        case SCTag_INT:    return_tail decode_int(L, dec);
        case SCTag_FLOAT:  return_tail decode_float(L, dec);
        case SCTag_STR1:
        case SCTag_STR4:   return_tail decode_str(L, dec);
        case SCTag_ARRAY:  return_tail decode_array(L, dec);
        case SCTag_DICT:   return_tail decode_dict(L, dec);
        case SCTag_FUN:    return_tail decode_fun(L, dec);
        case SCTag_UVAL:   return_tail decode_uval(L, dec);
        case SCTag_REFZ:   return_tail decode_refz(L, dec);
        case SCTag_REF:    return_tail decode_ref(L, dec);
        case SCTag_HEADER: break; // TODO: stop gracefully if this is encountered at top level
    }
    dlog("unexpected byte 0x%02x at offset %zu", dec->buf[0], (usize)(dec->buf - dec->bufstart));
    codec_error(L, dec, EINVAL, "Invalid encoded data");
}


static int encode_refmap_sortcmp(const void* x, const void* y, void* nullable ctx) {
    u64 a = *(const u64*)x;
    u64 b = *(const u64*)y;
    return (a > b) - (a < b);
}


static void encode_refmap(lua_State* L, Encoder* enc) {
    u64 smalltab[32];
    u64* reftab = smalltab;

    if UNLIKELY(enc->nrefs > countof(smalltab)) {
        if UNLIKELY((reftab = malloc(sizeof(u64) * enc->nrefs)) == NULL)
            return enc_error_nomem(L, enc);
    }

    // iterate over lua reftab table values
    lua_pushvalue(L, REFTAB_IDX);
    lua_pushnil(L); // dummy key for lua_next to lua_pop(L, 1)
    u32 i = 0;
    while (lua_next(L, /*table idx*/-2) != 0) {
        u64 reftuple = lua_tointeger(L, -1);
        if (ENC_REFTUPLE_REFNO(reftuple)) {
            assertf(i < enc->nrefs, "%u < %u (0x%lx)", i, enc->nrefs, reftuple);
            reftab[i++] = reftuple;
        }
        lua_pop(L, 1);
        // note: lua_next will do lua_pop(L, 1)
    }
    lua_pop(L, 1); // remove reftab

    // sort reftab by offset
    dew_qsort(reftab, enc->nrefs, sizeof(reftab[0]), encode_refmap_sortcmp, NULL);

    #if SC_TRACE
        sc_trace("encoded refmap %s[%u]:",
                 (COMPACT_REFMAP && enc->nrefs <= 256) ? "u8" : "u32", enc->nrefs);
        for (u32 i = 0; i < enc->nrefs; i++) {
            sc_trace("  #%-3u => ref#%-3lu (offset %lu)",
                     i, ENC_REFTUPLE_REFNO(reftab[i]) - 1, ENC_REFTUPLE_OFFSET(reftab[i]));
        }
    #endif

    // calculate buffer space needed
    usize needbuf = (COMPACT_REFMAP && enc->nrefs <= 256) ? enc->nrefs : enc->nrefs*4;

    // reserve buffer space for refmap array
    u8* refmap = buf_reserve(enc->buf, needbuf);
    enc->buf->len += needbuf;
    if UNLIKELY(!refmap) {
        enc->buf->len -= needbuf;
        enc_error_nomem(L, enc);
        goto end;
    }

    // write refmap (note: encoded header already contains count)
    if LIKELY(COMPACT_REFMAP && enc->nrefs <= 256) {
        // small table, 1 byte per refno
        for (u32 i = 0; i < enc->nrefs; i++)
            refmap[i] = ENC_REFTUPLE_REFNO(reftab[i]) - 1;
    } else {
        // large table, 4 bytes per refno
        for (u32 i = 0; i < enc->nrefs; i++) {
            u32 refno = ENC_REFTUPLE_REFNO(reftab[i]) - 1;
            memcpy(refmap, &refno, sizeof(u32));
            refmap += sizeof(u32);
        }
    }

end:
    if (reftab != smalltab)
        free(reftab);
}


static void decode_refmap(lua_State* L, Decoder* dec) {
    if LIKELY(COMPACT_REFMAP && dec->nrefs <= 256) {
        // adjust bufend to right before the refmap data
        dec->bufend -= dec->nrefs;
        #if SC_TRACE
            const u8* refmap = decode_compact_refmap(dec);
            sc_trace("decoded refmap u8[%u]:", dec->nrefs);
            for (u32 i = 0; i < dec->nrefs; i++)
                sc_trace("  #%-3u => ref#%-3u", i, refmap[i]);
        #endif
    } else {
        // adjust bufend to right before the refmap data
        dec->bufend -= dec->nrefs * sizeof(u32);

        // check bounds
        if UNLIKELY(dec->bufend <= dec->bufstart) {
            dec->bufend = dec->bufstart + 1;
            return dec_error_short(L, dec);
        }

        // check if refmap is aligned, else copy it to some heap memory
        if (IS_ALIGN2((uintptr)dec->bufend, _Alignof(u32))) {
            dec->refmap = (u32*)dec->bufend;
        } else {
            u32* refmap2 = malloc(sizeof(u32) * dec->nrefs);
            if (!refmap2)
                return dec_error_nomem(L, dec);
            memcpy(refmap2, dec->bufend, dec->nrefs*sizeof(u32));
            dec->refmap = refmap2;
        }

        #if SC_TRACE
            sc_trace("decoded refmap u32[%u]:", dec->nrefs);
            for (u32 i = 0; i < dec->nrefs; i++)
                sc_trace("  #%-3u => ref#%-3u", i, dec->refmap[i]);
        #endif
    }
}


int structclone_encode(lua_State* L, Buf* buf, u64 flags, int nargs) {
    Encoder enc = { .buf = buf, .buf_startoffs = buf->len };

    #if 0
        // fill the first 4 kB with AA
        void* p = buf_reserve(buf, 4*1024);
        assert(p != NULL);
        memset(p, 0xAA, 4*1024);
    #endif

    // TODO: if flags&StructCloneEnc_TRANSFER_LIST, there's a transfer_list array at L[nargs-1]

    // reserve some reasonable amount of space up front
    // Note: 120 instead of 128 for scenarios of MiniBuf
    if UNLIKELY(!buf_reserve(buf, 120)) {
        enc_error_nomem(L, &enc);
        return enc.err_no;
    }

    buf->len += 4; // reserve space for header; 1 byte tag, 3 bytes nrefs

    // reverse values, which arrive in stack order
    lua_reverse(L, (lua_gettop(L) - nargs) + 1);

    // encode values
    while (nargs-- > 0 && enc.err_no == 0) {
        encode_value(L, &enc, -1);
        lua_pop(L, 1);
    }

    // write reftab
    if (enc.nrefs)
        encode_refmap(L, &enc);

    // write header (4 bytes)
    //
    //   bit  1   2   3   4   5   6   7   8   9    ...    32
    //      ┌───────────┬───┬───────────────┬────── ~ ──────┐
    //      │  version  │   │      tag      │     nrefs     │
    //      └───────────┴─┬─┴───────────────┴────── ~ ──────┘
    //                 reserved
    buf->bytes[enc.buf_startoffs] = (u8)SCTag_HEADER
                                  //| ((u8)(enc.reserved > 0) << SCTagValShift)
                                  | ((u8)CODEC_VERSION << (SCTagValShift + 1));
    assert(enc.nrefs <= (1 << (3*8)) - 1);
    memcpy(&buf->bytes[enc.buf_startoffs + 1], &enc.nrefs, 3);
    #if SC_TRACE
        usize reslen = buf->len - enc.buf_startoffs;
        sc_trace("encode resulted in %zu B (%u refs)", reslen, enc.nrefs);
        hexdump(stderr, &buf->bytes[enc.buf_startoffs], reslen, 0, 16);
    #endif

    // if we created a reftab, remove it
    if (enc.has_reftab)
        lua_remove(L, REFTAB_IDX);

    return enc.err_no;
}


int structclone_decode(lua_State* L, const void* bufp, usize buflen) {
    const u8* buf = bufp;

    // check buflen and header
    const u8 expect_header = (u8)SCTag_HEADER | ((u8)CODEC_VERSION << (SCTagValShift + 1));
    const u8 header_mask = (~(u8)0) ^ SCTagValHeaderReservedMask;
    if UNLIKELY(buflen == 0 || (*buf & header_mask) != expect_header) {
        dlog("header: 0x%02x (0x%02x)", buflen == 0 ? 0 : (*buf & header_mask), *buf);
        luaL_error(L, "Invalid data");
        return -EBADMSG;
    }

    Decoder dec = {
        .bufstart = buf,
        .bufend = buf + buflen,
        .buf = buf + 4, // 4 bytes past header
    };

    int top_start = lua_gettop(L);
    int stack_base = top_start+1;

    // load nrefs from last 3 bytes of header
    u32 nrefs = 0;
    memcpy(&nrefs, &buf[1], 3);
    if (nrefs > 0) {
        dec.nrefs = nrefs;
        decode_refmap(L, &dec);
        if (dec.err_no)
            return dec.err_no;
        lua_createtable(L, nrefs, 0);
        lua_rotate(L, 1, REFTAB_IDX); // move reftab down into stack at REFTAB_IDX
        stack_base++;
    }

    // Decode values in buffer.
    // Make sure to stop before a potential refmap
    while (dec.buf < dec.bufend && dec.err_no == 0)
        decode_value(L, &dec);

    // free reftab & refmap, if used
    if (nrefs) {
        lua_remove(L, REFTAB_IDX);
        if (dec.refmap && (u8*)dec.refmap != dec.bufend)
            free(dec.refmap);
    } else {
        assert(dec.refmap == NULL);
    }

    // return number of decoded values, when successful
    if (dec.err_no == 0) {
        int nres = lua_gettop(L) - top_start;
        return nres;
    }

    lua_error(L);
    return dec.err_no;
}
