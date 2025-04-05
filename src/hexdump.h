#pragma once
#include "dew.h"
API_BEGIN

void hexdump(FILE* write_fp, const void* datap, usize len, uintptr start_addr, int bytes_per_row);

API_END
