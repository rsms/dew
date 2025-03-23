#include "dew.h"

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
		int t = lua_type(L, i);
		switch (t) {
			case LUA_TSTRING:
				fprintf(stderr, "  %d: string: '%s'\n", i, lua_tostring(L, i));
				break;
			case LUA_TBOOLEAN:
				fprintf(stderr, "  %d: boolean: %s\n", i, lua_toboolean(L, i) ? "true" : "false");
				break;
			case LUA_TNUMBER:
				fprintf(stderr, "  %d: number: %g\n", i, lua_tonumber(L, i));
				break;
			case LUA_TTABLE:
				fprintf(stderr, "  %d: table: %p\n", i, lua_topointer(L, i));
				break;
			case LUA_TTHREAD:
				fprintf(stderr, "  %d: thread: %p\n", i, lua_topointer(L, i));
				break;
			case LUA_TUSERDATA:
				fprintf(stderr, "  %d: userdata: %p\n", i, lua_topointer(L, i));
				break;
			case LUA_TFUNCTION:
				fprintf(stderr, "  %d: function: %p\n", i, lua_topointer(L, i));
				break;
			default:
				fprintf(stderr, "  %d: %s: %p\n", i, lua_typename(L, t), lua_topointer(L, i));
				break;
		}
	}
	// fprintf(stderr, "\n");
}
