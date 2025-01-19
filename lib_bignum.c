#include <lua.h>
#include <lauxlib.h>
#include <lualib.h>
#include <string.h>
#include "bn.h"

// Helper macro to get a `struct bn*` from a Lua userdata
#define check_bignum(L, idx) ((struct bn*)luaL_checkudata(L, idx, "bignum_meta"))

// Create a new bignum object
static int l_bignum_new(lua_State *L) {
	struct bn *n = (struct bn*)lua_newuserdata(L, sizeof(struct bn));
	bignum_init(n);  // Initialize to zero
	luaL_getmetatable(L, "bignum_meta");
	lua_setmetatable(L, -2);
	return 1;
}

// Garbage collection metamethod
static int l_bignum_gc(lua_State *L) {
	struct bn *n = check_bignum(L, 1);
	(void)n;  // No action needed for now, but could free resources if needed
	return 0;
}

// Create a bignum from an integer
static int l_bignum_from_int(lua_State *L) {
	int value = luaL_checkinteger(L, 1);
	struct bn *n = (struct bn*)lua_newuserdata(L, sizeof(struct bn));
	bignum_from_int(n, value);
	luaL_getmetatable(L, "bignum_meta");
	lua_setmetatable(L, -2);
	return 1;
}

// Create a bignum from a string.
// String must be base-16 and contain an even number of bytes.
static int l_bignum_from_str(lua_State *L) {
	const char *str = luaL_checkstring(L, 1);
	struct bn *n = (struct bn*)lua_newuserdata(L, sizeof(struct bn));
	bignum_init(n);
	bignum_from_string(n, (char *)str, strlen(str));
	luaL_getmetatable(L, "bignum_meta");
	lua_setmetatable(L, -2);
	return 1;
}

// Convert a bignum to an integer
static int l_bignum_to_int(lua_State *L) {
	struct bn *n = check_bignum(L, 1);
	lua_pushinteger(L, bignum_to_int(n));
	return 1;
}

// Convert a bignum to a string
static int l_bignum_to_str(lua_State *L) {
	struct bn *n = check_bignum(L, 1);
	char buffer[1024]; // Adjust buffer size as needed
	bignum_to_string(n, buffer, sizeof(buffer));
	lua_pushstring(L, buffer);
	return 1;
}

// Add two bignums
static int l_bignum_add(lua_State *L) {
	struct bn *a = check_bignum(L, 1);
	struct bn *b = check_bignum(L, 2);
	struct bn *c = (struct bn*)lua_newuserdata(L, sizeof(struct bn));
	bignum_add(a, b, c);
	luaL_getmetatable(L, "bignum_meta");
	lua_setmetatable(L, -2);
	return 1;
}

// Subtract two bignums
static int l_bignum_sub(lua_State *L) {
	struct bn *a = check_bignum(L, 1);
	struct bn *b = check_bignum(L, 2);
	struct bn *c = (struct bn*)lua_newuserdata(L, sizeof(struct bn));
	bignum_sub(a, b, c);
	luaL_getmetatable(L, "bignum_meta");
	lua_setmetatable(L, -2);
	return 1;
}

// Multiply two bignums
static int l_bignum_mul(lua_State *L) {
	struct bn *a = check_bignum(L, 1);
	struct bn *b = check_bignum(L, 2);
	struct bn *c = (struct bn*)lua_newuserdata(L, sizeof(struct bn));
	bignum_mul(a, b, c);
	luaL_getmetatable(L, "bignum_meta");
	lua_setmetatable(L, -2);
	return 1;
}

// Divide two bignums
static int l_bignum_div(lua_State *L) {
	struct bn *a = check_bignum(L, 1);
	struct bn *b = check_bignum(L, 2);
	struct bn *c = (struct bn*)lua_newuserdata(L, sizeof(struct bn));
	bignum_div(a, b, c);
	luaL_getmetatable(L, "bignum_meta");
	lua_setmetatable(L, -2);
	return 1;
}

// Modulo operation
static int l_bignum_mod(lua_State *L) {
	struct bn *a = check_bignum(L, 1);
	struct bn *b = check_bignum(L, 2);
	struct bn *c = (struct bn*)lua_newuserdata(L, sizeof(struct bn));
	bignum_mod(a, b, c);
	luaL_getmetatable(L, "bignum_meta");
	lua_setmetatable(L, -2);
	return 1;
}

// Exponentiation
static int l_bignum_pow(lua_State *L) {
	struct bn *a = check_bignum(L, 1);
	struct bn *b = check_bignum(L, 2);
	struct bn *c = (struct bn*)lua_newuserdata(L, sizeof(struct bn));
	bignum_pow(a, b, c);
	luaL_getmetatable(L, "bignum_meta");
	lua_setmetatable(L, -2);
	return 1;
}

// Integer square root
static int l_bignum_isqrt(lua_State *L) {
	struct bn *a = check_bignum(L, 1);
	struct bn *b = (struct bn*)lua_newuserdata(L, sizeof(struct bn));
	bignum_isqrt(a, b);
	luaL_getmetatable(L, "bignum_meta");
	lua_setmetatable(L, -2);
	return 1;
}

// Increment
static int l_bignum_inc(lua_State *L) {
	struct bn *n = check_bignum(L, 1);
	bignum_inc(n);
	lua_settop(L, 1);
	return 1;
}

// Decrement
static int l_bignum_dec(lua_State *L) {
	struct bn *n = check_bignum(L, 1);
	bignum_dec(n);
	lua_settop(L, 1);
	return 1;
}

// Logical AND
static int l_bignum_and(lua_State *L) {
	struct bn *a = check_bignum(L, 1);
	struct bn *b = check_bignum(L, 2);
	struct bn *c = (struct bn*)lua_newuserdata(L, sizeof(struct bn));
	bignum_and(a, b, c);
	luaL_getmetatable(L, "bignum_meta");
	lua_setmetatable(L, -2);
	return 1;
}

// Logical OR
static int l_bignum_or(lua_State *L) {
	struct bn *a = check_bignum(L, 1);
	struct bn *b = check_bignum(L, 2);
	struct bn *c = (struct bn*)lua_newuserdata(L, sizeof(struct bn));
	bignum_or(a, b, c);
	luaL_getmetatable(L, "bignum_meta");
	lua_setmetatable(L, -2);
	return 1;
}

// Logical XOR
static int l_bignum_xor(lua_State *L) {
	struct bn *a = check_bignum(L, 1);
	struct bn *b = check_bignum(L, 2);
	struct bn *c = (struct bn*)lua_newuserdata(L, sizeof(struct bn));
	bignum_xor(a, b, c);
	luaL_getmetatable(L, "bignum_meta");
	lua_setmetatable(L, -2);
	return 1;
}

// Left shift
static int l_bignum_lshift(lua_State *L) {
	struct bn *a = check_bignum(L, 1);
	int nbits = luaL_checkinteger(L, 2);
	struct bn *b = (struct bn*)lua_newuserdata(L, sizeof(struct bn));
	bignum_lshift(a, b, nbits);
	luaL_getmetatable(L, "bignum_meta");
	lua_setmetatable(L, -2);
	return 1;
}

// Right shift
static int l_bignum_rshift(lua_State *L) {
	struct bn *a = check_bignum(L, 1);
	int nbits = luaL_checkinteger(L, 2);
	struct bn *b = (struct bn*)lua_newuserdata(L, sizeof(struct bn));
	bignum_rshift(a, b, nbits);
	luaL_getmetatable(L, "bignum_meta");
	lua_setmetatable(L, -2);
	return 1;
}

// Equality comparison
static int l_bignum_eq(lua_State *L) {
	struct bn *a = check_bignum(L, 1);
	struct bn *b = check_bignum(L, 2);
	lua_pushboolean(L, bignum_cmp(a, b) == 0);
	return 1;
}

// Less than comparison
static int l_bignum_lt(lua_State *L) {
	struct bn *a = check_bignum(L, 1);
	struct bn *b = check_bignum(L, 2);
	lua_pushboolean(L, bignum_cmp(a, b) < 0);
	return 1;
}

// Comparison
static int l_bignum_cmp(lua_State *L) {
	struct bn *a = check_bignum(L, 1);
	struct bn *b = check_bignum(L, 2);
	lua_pushinteger(L, bignum_cmp(a, b));
	return 1;
}

// Is zero
static int l_bignum_is_zero(lua_State *L) {
	struct bn *n = check_bignum(L, 1);
	lua_pushboolean(L, bignum_is_zero(n));
	return 1;
}

// Library and metamethod registration
static const luaL_Reg bignum_methods[] = {
	{"new", l_bignum_new},
	{"from_int", l_bignum_from_int},
	{"from_str", l_bignum_from_str},
	{"to_int", l_bignum_to_int},
	{"to_str", l_bignum_to_str},
	{"add", l_bignum_add},
	{"sub", l_bignum_sub},
	{"mul", l_bignum_mul},
	{"div", l_bignum_div},
	{"mod", l_bignum_mod},
	{"pow", l_bignum_pow},
	{"isqrt", l_bignum_isqrt},
	{"inc", l_bignum_inc},
	{"dec", l_bignum_dec},
	{"and", l_bignum_and},
	{"or", l_bignum_or},
	{"xor", l_bignum_xor},
	{"lshift", l_bignum_lshift},
	{"rshift", l_bignum_rshift},
	{"cmp", l_bignum_cmp},
	{"is_zero", l_bignum_is_zero},
	{"__add", l_bignum_add},
	{"__sub", l_bignum_sub},
	{"__mul", l_bignum_mul},
	{"__div", l_bignum_div},
	{"__mod", l_bignum_mod},
	{"__pow", l_bignum_pow},
	{"__tostring", l_bignum_to_str},
	{"__eq", l_bignum_eq},
	{"__lt", l_bignum_lt},
	{"__band", l_bignum_and},
	{"__bor", l_bignum_or},
	{"__bxor", l_bignum_xor},
	{"__shl", l_bignum_lshift},
	{"__shr", l_bignum_rshift},
	{NULL, NULL}
};

int luaopen_bignum(lua_State *L) {
	luaL_newmetatable(L, "bignum_meta");
	lua_pushvalue(L, -1);
	lua_setfield(L, -2, "__index");
	luaL_setfuncs(L, bignum_methods, 0);
	luaL_newlib(L, bignum_methods);
	return 1;
}
