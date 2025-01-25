#include "dew.h"
#include <signal.h>
#include "bn.h"
#ifdef DEW_EMBED_SRC
#include "dew.lua.h"
#endif

// static u8 reserved[4096];

const char* g_prog = NULL;
lua_State* g_L = NULL;


int luaopen_bignum(lua_State *L); // lib_bignum.c
int luaopen_runtime(lua_State *L); // runtime.c


static int check_status(lua_State* L, int status) {
	if (status != LUA_OK) {
		const char* msg = lua_tostring(L, -1);
		if (msg == NULL)
			msg = "(error message not a string)";
		logerr("%s", msg);
		lua_pop(L, 1);  /* remove message */
	}
	return status;
}


static int msghandler(lua_State* L) {
	const char* msg = lua_tostring(L, 1);

	// TODO: forward error to WASM runtime
	// #ifdef __wasm__
	// ...
	// #endif

	if (msg == NULL) { /* is error object not a string? */
		if (luaL_callmeta(L, 1, "__tostring") &&  /* does it have a metamethod */
		    lua_type(L, -1) == LUA_TSTRING)       /* that produces a string? */
		{
			return 1;  /* that is the message */
		} else {
			msg = lua_pushfstring(L, "(error object is a %s value)", luaL_typename(L, 1));
		}
	}
	luaL_traceback(L, L, msg, 1);  /* append a standard traceback */
	return 1; /* return the traceback */
}


// signal handling
#if defined(__wasm__)
	#define dew_setsignal(...) ((void)0)
#else
	#if defined(LUA_USE_POSIX)
		static void dew_setsignal(int sig, void (*handler)(int)) {
			struct sigaction sa;
			sa.sa_handler = handler;
			sa.sa_flags = 0;
			sigemptyset(&sa.sa_mask);  /* do not mask any signal */
			sigaction(sig, &sa, NULL);
		}
	#else
		#define dew_setsignal signal
	#endif

	// Hook set by signal function to stop the interpreter
	static void stop_process(lua_State* L, lua_Debug* ar) {
		(void)ar; // unused
		lua_sethook(L, NULL, 0, 0); // reset hook
		luaL_error(L, "[process interrupted]");
	}

	// Function to be called at a C signal. Because a C signal cannot just change a Lua state
	// (as there is no proper synchronization), this function only sets a hook that, when called,
	// will stop the interpreter.
	static void signal_handler(int signo) {
		dew_setsignal(signo, SIG_DFL); // if another SIGINT happens, terminate process
		lua_sethook(g_L, stop_process, LUA_MASKCALL | LUA_MASKRET | LUA_MASKLINE | LUA_MASKCOUNT, 1);
	}
#endif


static int do_pcall(lua_State* L, int narg, int nres) {
	int status;
	int base = lua_gettop(L) - narg;  /* function index */
	lua_pushcfunction(L, msghandler); /* push message handler */
	lua_insert(L, base);  /* put it under function and args */
	g_L = L; /* for 'signal_handler' */
	dew_setsignal(SIGINT, signal_handler); /* set C-signal handler */
	status = lua_pcall(L, narg, nres, base);
	dew_setsignal(SIGINT, SIG_DFL); /* reset C-signal handler */
	lua_remove(L, base); /* remove message handler from the stack */
	return status;
}


static int do_call(lua_State* L, int narg, int nres) {
	g_L = L;
	dew_setsignal(SIGINT, signal_handler);
	lua_call(L, narg, nres);
	dew_setsignal(SIGINT, SIG_DFL);
	return 1;
}


#ifdef DEW_EMBED_SRC
	static const char* src_readchunk(lua_State* L, void* ud, size_t* size) {
		*size = sizeof(kDewLuaData);
		return kDewLuaData;
	}
#endif


// #define ENABLE_SCHED


static int pmain(lua_State* L) {
	int argc = (int)lua_tointeger(L, 1);
	char** argv = (char**)lua_touserdata(L, 2);

	// signal for libraries to ignore env vars
	lua_pushboolean(L, 1);
	lua_setfield(L, LUA_REGISTRYINDEX, "LUA_NOENV");

	// open libraries
	luaL_openlibs(L); // standard libraries
	luaL_requiref(L, "bignum", luaopen_bignum, 1);
	lua_setglobal(L, "bignum");
	luaL_requiref(L, "__rt", luaopen_runtime, 1);
	lua_setglobal(L, "__rt");

	// create input argument table/array
	lua_createtable(L, argc, 1);
	lua_pushstring(L, g_prog);
	lua_rawseti(L, -2, 0);
	for (int i = 1; i < argc; i++) {
		lua_pushstring(L, argv[i]);
		lua_rawseti(L, -2, i); // arg[i] = argv[i]
	}
	lua_setglobal(L, "arg");

	// start GC in generational mode
	lua_gc(L, LUA_GCRESTART);
	lua_gc(L, LUA_GCGEN, 0, 0);  // ....

	#ifdef ENABLE_SCHED
		lua_State* L_co = lua_newthread(L);
	#else
		lua_State* L_co = L;
	#endif

	// load dew Lua script
	#ifdef DEW_EMBED_SRC
		// int status = luaL_loadfile(L, "o.darwin.debug/dew.lua");
		int status = lua_load(L_co, src_readchunk, (void*)kDewLuaData, "dew.lua", "bt");
	#else
		int status = luaL_loadfile(L_co, "dew.lua");
	#endif
	if (status != LUA_OK) {
		check_status(L_co, status);
		return 0; // interrupt in case of error
	}

	// push on the stack the contents of table 'arg' from 1 to #arg
	if (lua_getglobal(L_co, "arg") != LUA_TTABLE)
		assert(!"'arg' is not a table");
	int nargs = (int)luaL_len(L_co, -1);
	luaL_checkstack(L_co, nargs + 3, "too many arguments to script"); // pre-grow stack or fail
	for (int i = 1; i <= nargs; i++)
		lua_rawgeti(L_co, -i, i);
	lua_remove(L_co, -nargs); /* remove table from the stack */

	// run script

	#ifdef ENABLE_SCHED
	{
		// adapted from do_pcall
		int status;
		int base = lua_gettop(L) - nargs;  /* function index */
		lua_pushcfunction(L, msghandler); /* push message handler */
		lua_insert(L, base);  /* put it under function and args */
		g_L = L; /* for 'signal_handler' */
		dew_setsignal(SIGINT, signal_handler); /* set C-signal handler */

		int nresults;
		for (;;) {
			status = lua_resume(L_co, L, nargs, &nresults);
			dlog("lua_resume => status=%d nresults=%d", status, nresults);
			if (status != LUA_YIELD)
				break;
			nargs = 0;
		}

		dew_setsignal(SIGINT, SIG_DFL); /* reset C-signal handler */
		lua_remove(L, base); /* remove message handler from the stack */
	}
	#else
		status = do_pcall(L_co, nargs, LUA_MULTRET);
	#endif
	if (status != LUA_OK) {
		check_status(L_co, status);
		return 0; // interrupt in case of error
	}

	// TODO: check for pending coroutines and print warnings (structured concurrency)

	return 1;
	// #endif
}


int main(int argc, char* argv[]) {
	g_prog = argv[0] && *argv[0] ? argv[0] : "dew";
	lua_State* L = luaL_newstate();
	if (L == NULL) {
		logerr("cannot create state: not enough memory");
		return 1;
	}
	lua_gc(L, LUA_GCSTOP);  // stop GC while building state
	lua_pushcfunction(L, &pmain);    // to call 'pmain' in protected mode
	lua_pushinteger(L, argc);        // 1st argument to pmain
	lua_pushlightuserdata(L, argv);  // 2nd argument to pmain
	int status = lua_pcall(L, 2, 1, 0);  // do the call
	int result = lua_toboolean(L, -1);   // get result
	check_status(L, status);
	lua_close(L);
	return (result == 0 || status != LUA_OK);
}
