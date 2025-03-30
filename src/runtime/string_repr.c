#include "string_repr.h"

static const char* hexchars = "0123456789abcdef";

usize string_hex(char* dst, usize dstcap, const u8* src, usize srclen) {
    usize srclen2 = (dstcap < srclen*2) ? dstcap/2 : srclen;
    for (usize i = 0; i < srclen2; i++) {
        u8 b = src[i];
        dst[i*2]     = hexchars[b >> 4];
        dst[i*2 + 1] = hexchars[b & 0xf];
    }
    return srclen * 2;
}

usize string_repr(char* dst, usize dstcap, const void* src, usize srclen) {
    char* p;
    char* lastp;
    char tmpc;
    usize nwrite = 0;

    if (dstcap == 0) {
        p = &tmpc;
        lastp = &tmpc;
    } else {
        p = dst;
        lastp = dst + (dstcap - 1);
    };

    for (usize i = 0; i < srclen; i++) {
        u8 c = *(u8*)src++;
        //dlog("[%zu/%zu] 0x%02x '%c'", i, srclen, c, isprint(c) ? c : ' ');
        switch (c) {
            // \xHH
            case '\1'...'\x08':
            case 0x0E ... 0x1F:
            case 0x7f ... 0xFF:
                if (LIKELY( p + 3 < lastp )) {
                    p[0] = '\\';
                    p[1] = 'x';
                    if (c < 0x10) {
                        p[2] = '0';
                        p[3] = hexchars[(int)c];
                    } else {
                        p[2] = hexchars[(int)c >> 4];
                        p[3] = hexchars[(int)c & 0xf];
                    }
                    p += 4;
                } else {
                    p = lastp;
                }
                nwrite += 4;
                break;
            // \c
            case '\t'...'\x0D':
            case '\\':
            case '"':
            case '\0': {
                static const char t[] = {'t','n','v','f','r'};
                if (LIKELY( p + 1 < lastp )) {
                    p[0] = '\\';
                    if      (c == 0)                         p[1] = '0';
                    else if (((usize)c - '\t') <= sizeof(t)) p[1] = t[c - '\t'];
                    else                                     p[1] = c;
                    p += 2;
                } else {
                    p = lastp;
                }
                nwrite += 2;
                break;
            }
            // verbatim
            default:
                *p = c;
                p = MIN(p + 1, lastp);
                nwrite++;
                break;
        }
    }

    *p = 0;
    return nwrite;
}
