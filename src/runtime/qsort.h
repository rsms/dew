#pragma once
#include "../dew.h"
API_BEGIN

typedef int(*dew_qsort_cmp)(const void* x, const void* y, void* nullable ctx);
void dew_qsort(void* base, usize nmemb, usize size, dew_qsort_cmp cmp, void* nullable ctx);

API_END
