#include "dew.h"

void dlog_lua_val(lua_State* L, int i) {
	int t = lua_type(L, i);
	switch (t) {
		case LUA_TSTRING:
			fprintf(stderr, "string   \"%s\"\n", lua_tostring(L, i));
			break;
		case LUA_TBOOLEAN:
			fprintf(stderr, "boolean  %s\n", lua_toboolean(L, i) ? "true" : "false");
			break;
		case LUA_TNUMBER: {
            int isint;
            i64 val = lua_tointegerx(L, i, &isint);
            if (isint) {
                fprintf(stderr, "integer  %ld\n", val);
            } else {
                fprintf(stderr, "number   %g\n", lua_tonumber(L, i));
            }
			break;
		}
		case LUA_TTABLE:
			fprintf(stderr, "table    %p\n", lua_topointer(L, i));
			break;
		case LUA_TTHREAD:
			fprintf(stderr, "thread   %p\n", lua_topointer(L, i));
			break;
		case LUA_TUSERDATA:
			fprintf(stderr, "userdata %p\n", lua_topointer(L, i));
			break;
		case LUA_TFUNCTION:
			fprintf(stderr, "function %p\n", lua_topointer(L, i));
			break;
		default:
			fprintf(stderr, "%-8s %p\n", lua_typename(L, t), lua_topointer(L, i));
			break;
	}
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
	for (int i = 1; i <= top; i++) {
		fprintf(stderr, "%4d ", i);
		dlog_lua_val(L, i);
	}
}
