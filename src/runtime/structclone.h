// structural cloning (aka serialization aka marshalling) of lua data
#pragma once
#include "../dew.h"
#include "buf.h"
API_BEGIN

// StructCloneEnc_ are flags for structclone_encode
#define StructCloneEnc_TRANSFER_LIST (1ul<<0) // transfer_list on L's stack

int structclone_encode(lua_State* L, Buf* buf, u64 flags, int nargs);
int structclone_decode(lua_State* L, const void* buf, usize buflen);

API_END
