#include "dew.h"

static isize _snprintf_lval(
    char* buf, usize bufcap, lua_State* L, int i, const char* sep1, const char* sep2)
{
    int t = lua_type(L, i);
    #define RES(NAME, FMT, ...) \
        return snprintf(buf, bufcap, "%s%s" FMT "%s", (NAME), sep1, __VA_ARGS__, sep2)
    switch (t) {
        case LUA_TSTRING:
            RES("string", "\"%s\"", lua_tostring(L, i));
        case LUA_TBOOLEAN:
            RES("boolean", "%s", lua_toboolean(L, i) ? "true" : "false");
        case LUA_TNUMBER: {
            int isint;
            i64 val = lua_tointegerx(L, i, &isint);
            if (isint) {
                RES("integer", "%ld", val);
            } else {
                RES("number", "%g", lua_tonumber(L, i));
            }
        }
        case LUA_TTABLE:
            RES("table", "%p", lua_topointer(L, i));
        case LUA_TTHREAD:
            RES("thread", "%p", lua_topointer(L, i));
        case LUA_TUSERDATA:
            RES("userdata", "%p", lua_topointer(L, i));
        case LUA_TFUNCTION:
            RES("function", "%p", lua_topointer(L, i));
        default:
            RES(lua_typename(L, t), "%p", lua_topointer(L, i));
    }
    #undef RES
}

isize snprintf_lval(char* buf, usize bufcap, lua_State* L, int i) {
    return _snprintf_lval(buf, bufcap, L, i, "(", ")");
}


#ifdef DEBUG

static char fmtbufv[8][128];
static int  fmtbufi = 0;

const char* fmtlval(lua_State* L, int i) {
    char* buf = fmtbufv[fmtbufi++ % 8];
    snprintf_lval(buf, sizeof(fmtbufv[0]), L, i);
    return buf;
}

void dlog_lua_val(lua_State* L, int i) {
    fprintf(stderr, "%s\n", fmtlval(L, i));
}

void dlog_lua_stackf(lua_State* L, const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    _vlogmsg(3, fmt, ap);
    va_end(ap);

    int top = lua_gettop(L);
    if (top == 0) {
        fprintf(stderr, " (empty)\n");
        return;
    }

    fprintf(stderr, "\n");
    char* buf = fmtbufv[fmtbufi++ % 8];
    for (int i = 1; i <= top; i++) {
        _snprintf_lval(buf, sizeof(fmtbufv[0]), L, i, "\t", "");
        fprintf(stderr, "%4d %s\n", i, buf);
    }
}

#endif
