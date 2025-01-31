#if defined(__wasm__)
	#include "platform_wasm.c"
#elif defined(__APPLE__)
	#include "platform_darwin.c"
#endif
