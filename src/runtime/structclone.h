// structural cloning (aka serialization aka marshalling) of lua data
#pragma once
#include "../dew.h"
#include "buf.h"
API_BEGIN

int structclone_encode(lua_State* L, Buf* buf, u64 flags, int nargs);
int structclone_decode(lua_State* L, const void* buf, usize buflen);

API_END
