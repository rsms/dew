#pragma once
#include "../dew.h"
API_BEGIN

enum UValType {
    UValType_Buf,
    UValType_Timer,
    UValType_UWorker,
    UValType_RemoteTask,
    UValType_IODesc,
};

// UVal is the common header of Lua userdata values
typedef struct UVal {
    u8 type;
} UVal;

// uval_new throws LUA_ERRMEM if memory allocation fails
void* nullable uval_new(lua_State* L, enum UValType type, usize size, int nuvals);

void* nullable uval_check(lua_State* L, int idx, enum UValType type, const char* expectname);

// uval_typename returns the __name entry of the value's metatable
const char* uval_typename(lua_State* L, int idx);

API_END
