#pragma once
#include "../dew.h"
API_BEGIN

usize string_hex(char* dst, usize dstcap, const u8* src, usize srclen);
usize string_repr(char* dst, usize dstcap, const void* src, usize srclen);

API_END
