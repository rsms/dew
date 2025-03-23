ifx  = $(if $(filter $($(1)),1),$(2),$(3))
ifs  = $(if $(filter $($(1)),$(2)),$(3),$(4))
Q    = $(if $(filter 1,$(V)),,@)
QLOG = $(if $(filter 1,$(V)),@#,@echo)

NATIVE_SYS  := $(subst Linux,linux,$(subst Darwin,darwin,$(shell uname -s)))
NATIVE_ARCH := $(subst aarch64,arm64,$(shell uname -m))
TARGET      := $(NATIVE_SYS)

BUILDDIR := o/$(TARGET)$(call ifx,DEBUG,.debug)$(call ifx,TEST,.test)$(call ifx,BENCHMARK,.bench)
OBJDIR   := $(BUILDDIR)/obj

DEW_SRCS := \
	src/dew.c \
	src/panic.c \
	src/logmsg.c \
	$(call ifx,DEBUG,  src/dlog_lua_stack.c) \
	src/runtime/runtime.c \
	src/runtime/lutil.c \
	src/runtime/array.c \
	src/runtime/buf.c \
	src/runtime/fifo.c \
	src/runtime/pool.c \
	src/runtime/time.c \
	src/runtime/timer.c \
	src/runtime/intconv.c \
	src/runtime/intscan.c \
	src/runtime/intfmt.c \
	src/runtime/inbox.c \
	src/runtime/chan.c \
	$(call ifs,TARGET,web,, src/runtime/tsem.c) \
	src/runtime/iopoll.c \
	$(call ifs,TARGET,darwin, src/runtime/iopoll_darwin.c) \
	$(call ifs,TARGET,linux,  src/runtime/iopoll_linux.c) \
	$(call ifs,TARGET,web,    src/runtime/iopoll_wasm.c  src/runtime/wasm.c) \

# order matters; "included" through embedding in the order listed here
DEW_LSRCS := \
	src/util.lua \
	src/unit.lua \
	src/diag.lua \
	src/srcpos.lua \
	src/tokenize.lua \
	src/id.lua \
	src/ast.lua \
	src/parse.lua \
	src/resolve.lua \
	src/codegen.lua

DEW_JSSRCS := $(wildcard web/*.ts web/index.html)

LUA_SRCS := \
	src/lua/lapi.c \
	src/lua/lauxlib.c \
	src/lua/lbaselib.c \
	src/lua/lcode.c \
	src/lua/lcorolib.c \
	src/lua/lctype.c \
	src/lua/ldblib.c \
	src/lua/ldebug.c \
	src/lua/ldo.c \
	src/lua/ldump.c \
	src/lua/lfunc.c \
	src/lua/lgc.c \
	src/lua/linit.c \
	src/lua/liolib.c \
	src/lua/llex.c \
	src/lua/lmathlib.c \
	src/lua/lmem.c \
	src/lua/loadlib.c \
	src/lua/lobject.c \
	src/lua/lopcodes.c \
	src/lua/loslib.c \
	src/lua/lparser.c \
	src/lua/lstate.c \
	src/lua/lstring.c \
	src/lua/lstrlib.c \
	src/lua/ltable.c \
	src/lua/ltablib.c \
	src/lua/ltm.c \
	src/lua/lundump.c \
	src/lua/lutf8lib.c \
	src/lua/lvm.c \
	src/lua/lzio.c \

CFLAGS := \
	-std=c17 -g -fdebug-compilation-dir=/x/ \
	-Wall -Wextra -Werror=format -Wno-unused -Wno-unused-parameter \
	-Werror=incompatible-pointer-types \
	-Werror=pointer-integer-compare \
	-Werror=int-conversion \
	-Isrc/lua \

EMBED_SRC   := 1
DEW_CFLAGS  := -D__dew__=1 $(call ifx,EMBED_SRC,-DDEW_EMBED_SRC=1 -I$(BUILDDIR))
TEST_CFLAGS := -D__dew__=1
LDFLAGS     :=
LUA_CFLAGS  :=
LUA         := o/$(NATIVE_SYS)/lua
LUAC        := o/$(NATIVE_SYS)/luac
ALL         := $(BUILDDIR)/dew
ORIGPATH    := ${PATH}

ifeq ($(TARGET),darwin)
	LUA_CFLAGS += -DLUA_USE_MACOSX
else ifeq ($(TARGET),linux)
	LUA_CFLAGS += -DLUA_USE_LINUX
	LDFLAGS += -Wl,-E -ldl
else ifeq ($(TARGET),web)
	ALL := $(BUILDDIR)/dew.wasm $(BUILDDIR)/dew.js $(BUILDDIR)/index.html
	CFLAGS += --target=wasm32-playbit -D_WASI_EMULATED_SIGNAL -D_WASI_EMULATED_PROCESS_CLOCKS
	CFLAGS += -fvisibility=hidden
	LDFLAGS += --target=wasm32-playbit -lwasi-emulated-signal -lwasi-emulated-process-clocks
	#LDFLAGS += -Wl,--import-memory
	#LDFLAGS += -Wl,--initial-memory=1048576
	LDFLAGS += -Wl,--export-dynamic
	LDFLAGS += -Wl,-allow-undefined-file,wasm.syms
	CFLAGS += $(if $(shell [ -t 2 ] && echo 1),-fcolor-diagnostics,)
	CFLAGS += -fblocks
	LDFLAGS += -fblocks -lBlocksRuntime
	WOPTFLAGS := --asyncify \
	             --no-validation \
	             --enable-bulk-memory \
	             --enable-tail-call \
	             --zero-filled-memory
	WOPTFLAGS += --pass-arg=asyncify-imports@env.syscall,env.wlongjmp_scope,env.wawait_end
	#WOPTFLAGS += --pass-arg=asyncify-addlist@lua_pcallk
	#WOPTFLAGS += --pass-arg=asyncify-ignore-indirect
	CXXFLAGS := -fno-rtti
	JSFLAGS := --format=esm --platform=browser $(if $(filter 1,$(V)),,--log-level=warning)
	ifeq ($(DEBUG),1)
		JSFLAGS += --define:DEBUG=true --inject:web/dev_debug.ts
		WOPTFLAGS += -O1 -g
		# asyncify-asserts: raises WebAssembly.RuntimeError "unreachable" when a function
		# that is not expected to be resumed is resumed. Stack trace tells us which function.
		WOPTFLAGS += --pass-arg=asyncify-asserts
		#WOPTFLAGS += --pass-arg=asyncify-verbose
	else
		JSFLAGS += --define:DEBUG=false --inject:web/dev_release.ts --minify --drop:debugger
		# For now, keep optimizations to a minimum and include debug info, even in releases.
		# Code size difference between -O1 -g vs --strip-all -O4: 1.2MiB vs 468KiB
		#LDFLAGS += -Wl,--strip-all
		WOPTFLAGS += -O1 -g
		#WOPTFLAGS += -O4
		WOPTFLAGS += --precompute-propagate
		WOPTFLAGS += --reorder-functions --reorder-globals
	endif
	# linux-arm64, linux-x64, darwin-arm64, darwin-x64
	ESBUILD_T := $(NATIVE_SYS)-$(subst x86_64,x64,$(NATIVE_ARCH))
	ESBUILD_V := 0.24.2
	ESBUILD   := _deps/esbuild-$(ESBUILD_V)-$(ESBUILD_T)/esbuild
	export PATH := $(PWD)/_deps/binaryen/bin:$(PWD)/_deps/llvm/bin:$(PATH)
else
	LUA_CFLAGS += -DLUA_USE_POSIX -DLUA_USE_DLOPEN
endif

ifeq ($(DEBUG),1)
	DEW_CFLAGS += -DDEBUG=1
	ifneq ($(TARGET),web)
		CFLAGS += -fsanitize=address,undefined
		LDFLAGS += -fsanitize=address,undefined
	endif
else ifeq ($(TEST),1)
	#DEW_CFLAGS += -DDEBUG=1
	CFLAGS += -O1 -fsanitize=address,undefined
	LDFLAGS += -fsanitize=address,undefined
else
	CFLAGS += -DNDEBUG -flto=thin $(if $(filter $(TARGET),web),-Oz,-O3)
endif

ifeq ($(TARGET),darwin)
	LDFLAGS += -Wl,-cache_path_lto,$(BUILDDIR)/lto
else
	LDFLAGS += -Wl,--thinlto-cache-dir=$(BUILDDIR)/lto
endif

DEW_OBJS    := $(addprefix $(OBJDIR)/,$(patsubst %,%.o,$(DEW_SRCS)))
LUADEW_OBJS := $(addprefix $(OBJDIR)/,$(patsubst %,%.o,$(LUA_SRCS)))
LUAEXE_OBJS := $(addprefix $(OBJDIR)/luaexe.,$(patsubst %,%.o,$(LUA_SRCS)))
OBJS        := $(DEW_OBJS) $(LUADEW_OBJS) $(LUAEXE_OBJS)

all: $(ALL)
clean:
	rm -rf o.*

dev:
	autorun *.c *.h *.lua tests/rt/*.* src/lua/*.c examples/*.dew \
	-- '$(MAKE) DEBUG=1 EMBED_SRC=0 _dev'
_dev: $(BUILDDIR)/dew
	$(BUILDDIR)/dew examples/dev.dew --debug-tokens --debug-parse --debug-resolve --debug-codegen

dev-web:
	autorun *.c *.h *.lua src/lua/*.c examples/*.dew web/*.* -- \
	'$(MAKE) DEBUG=1 TARGET=web'

test:
	$(MAKE) TEST=1 EMBED_SRC=0 \
		run_tests \
		run_dew_selftest \
		run-test-runtime

test-runtime:
	$(MAKE) TEST=1 EMBED_SRC=0 run-test-runtime

benchmark: benchmark-runtime

benchmark-runtime:
	$(MAKE) BENCHMARK=1 EMBED_SRC=0 run-benchmark-runtime

run_dew_selftest: $(BUILDDIR)/dew
	$(BUILDDIR)/dew --selftest$(if $(filter 1,$(V)),=v,)

TEST_PROGS := \
	$(BUILDDIR)/src/runtime/chan_test \
	$(BUILDDIR)/src/runtime/chan_test.opt

run_tests: $(TEST_PROGS)
	$(Q)$(foreach f,$^,echo "RUN   $(f)$(if $(filter 1,$(V)),, > $(f).log)"; $(f) $(if $(filter 1,$(V)),,>$(f).log 2>&1 || { echo "$(f): FAILED"; cat $(f).log; exit 1; }) && ) true

run-test-runtime: RUNTIME_TESTS := $(filter-out %benchmark.lua,$(wildcard tests/rt/*.lua))
run-test-runtime: $(BUILDDIR)/dew $(RUNTIME_TESTS)
	$(Q)_FC=0; $(foreach f,$(RUNTIME_TESTS),\
	      $(if $(filter 1,$(V)),echo "RUN   $(f)";,) \
	      if ! DEW_RUNTIME_TEST=1 DEW_MAIN_SCRIPT=$(f) $< \
	           $(if $(filter 1,$(V)),\
	             ; then echo "$(f): FAILED">&2; _FC=$$(( _FC + 1 )); ,\
	             > $(BUILDDIR)/$(notdir $(f)).log 2>&1; \
	               then echo "$(f): FAILED ——————————">&2; _FC=$$(( _FC + 1 )); \
	               cat $(BUILDDIR)/$(notdir $(f)).log; \
	               echo "——————————"; \
	             ) else echo "$(f): PASS"; fi; ) \
	    if [ $$_FC -eq 0 ]; then echo "all tests PASS"; \
	    else echo "$$_FC test(s) FAILED">&2; exit 1; fi

run-benchmark-runtime: RUNTIME_TESTS := $(wildcard tests/rt/*benchmark.lua)
run-benchmark-runtime: $(BUILDDIR)/dew $(RUNTIME_TESTS)
	$(foreach f,$(RUNTIME_TESTS),\
	    DEW_RUNTIME_TEST=1 DEW_MAIN_SCRIPT=$(f) $< && \
	) true

$(BUILDDIR)/src/runtime/chan_test.opt: CFLAGS += -O2 -DNDEBUG -fno-sanitize=address,undefined
$(BUILDDIR)/src/runtime/chan_test $(BUILDDIR)/src/runtime/chan_test.opt: \
		src/runtime/chan_test.c \
		src/runtime/tsem.c \
		src/runtime/time.c \
		src/panic.c \
		src/logmsg.c
	$(QLOG) "CC+LD $@"
	$(Q)mkdir -p $(@D)
	$(Q)$(CC) $(CFLAGS) $(TEST_CFLAGS) $(LDFLAGS) $^ -o $@

$(BUILDDIR)/dew: $(DEW_OBJS) $(LUADEW_OBJS)
	$(QLOG) "LINK  $@"
	$(Q)mkdir -p $(@D)
	$(Q)$(CC) -o $@ $^ $(LDFLAGS)

$(BUILDDIR)/dew.lua: $(DEW_LSRCS) src/dew.lua
	$(QLOG) "GEN   $@"
	$(Q)mkdir -p $(@D)
	$(Q)rm -f $@
	$(Q)$(foreach f,$^,cat $(f) >> $@;)
	$(Q)sed -i '' -E \
		-e 's/\s*([^\(]require\()/--\1/' \
		$@

$(BUILDDIR)/dew.lua.o: $(BUILDDIR)/dew.lua $(LUAC)
	$(QLOG) "LUAC  $@"
	$(Q)mkdir -p $(@D)
	$(Q)$(LUAC) -o $@ -s $<

$(BUILDDIR)/dew.lua.h: $(BUILDDIR)/dew.lua.o $(LUA) etc/gen-embed.lua
	$(QLOG) "GEN   $@"
	$(Q)$(LUA) etc/gen-embed.lua $< $@ kDewLuaData

$(BUILDDIR)/dew.1.wasm: $(OBJS)
	$(QLOG) "LINK  $@"
	$(Q)mkdir -p $(@D)
	$(Q)$(CC) $(LDFLAGS) $^ -o $@

$(BUILDDIR)/dew.wasm: $(BUILDDIR)/dew.1.wasm
	$(QLOG) "WOPT  $@"
	$(Q)wasm-opt $(WOPTFLAGS) $< -o $@

$(BUILDDIR)/dew.js: src/web/dew.ts $(ESBUILD) $(DEW_JSSRCS)
	$(QLOG) "ESBLD $@"
	$(Q)$(ESBUILD) --bundle $(JSFLAGS) --outfile=$@ $<

$(BUILDDIR)/index.html: src/web/index.html
	$(QLOG) "COPY  $@"
	$(Q)mkdir -p $(@D)
	$(Q)cp $< $@

# tools that run on build host (NATIVE)
ifeq ($(NATIVE_SYS),$(TARGET))
$(LUA):  src/lua/lua.c
$(LUAC): src/lua/luac.c
$(LUA) $(LUAC): $(LUAEXE_OBJS)
	$(QLOG) "LINK  $@"
	$(Q)mkdir -p $(@D)
	$(Q)$(CC) -o $@ $^ $(LDFLAGS)
else
$(LUA) $(LUAC):
	$(Q)env -i 'PATH=${ORIGPATH}' sh -c '$(MAKE) -j$$(nproc) $@'
endif

ifeq ($(EMBED_SRC),1)
$(OBJDIR)/src/dew.c.o: $(BUILDDIR)/dew.lua.h
endif

$(DEW_OBJS):    override CFLAGS := $(CFLAGS) $(DEW_CFLAGS)
$(LUADEW_OBJS): override CFLAGS := $(CFLAGS) $(DEW_CFLAGS) $(LUA_CFLAGS)
$(LUAEXE_OBJS): override CFLAGS := $(CFLAGS) $(LUA_CFLAGS)

ifeq ($(TARGET),web)
$(OBJS): _deps/llvm/bin/clang
# # use c++ exceptions instead of longjmp:
# $(OBJDIR)/src/lua/ldo.c.o: src/lua/ldo.c
# 	$(QLOG) "CXX   $<"
# 	$(Q)$(CXX) -MP -MMD $(CFLAGS) $(CXXFLAGS) -std=c++17 -xc++ -c -o "$@" $<
endif

$(OBJDIR)/luaexe.%.c.o: %.c
	$(QLOG) "CC    $<"
	$(Q)$(CC) -MP -MMD $(CFLAGS) -c -o "$@" $<
$(OBJDIR)/%.c.o: %.c
	$(QLOG) "CC    $<"
	$(Q)$(CC) -MP -MMD $(CFLAGS) -c -o "$@" $<
$(OBJDIR)/%.cc.o: %.cc
	$(QLOG) "CC    $<"
	$(Q)$(CXX) -MP -MMD $(CFLAGS) $(CXXFLAGS) -std=c++17 -xc++ -c -o "$@" $<

# source file dependencies
DIRST := $(sort $(patsubst %/,%,$(dir $(OBJS))) $(BUILDDIR))
$(OBJS): | $(DIRST)
$(DIRST):
	$(QLOG) "MKDIR $@"
	$(Q)mkdir -p "$@"
-include $(wildcard $(OBJS:.o=.d))

# tooling for web
web/node_modules/.bin/esbuild: src/web/package-lock.json
	touch $@
web/package-lock.json: src/web/package.json
	cd web && npm install
	touch $@

# esbuild for compiling JavaScript/TypeScript
# Note: archive contains package/bin/esbuild with README+etc in package/.
# We only keep what's in package/bin via --strip-components=2
$(ESBUILD):
	$(QLOG) "FETCH $@"
	$(Q)mkdir -p _deps/download
	$(Q)cd _deps/download && \
	    curl -#LO https://registry.npmjs.org/@esbuild/$(ESBUILD_T)/-/$(ESBUILD_T)-$(ESBUILD_V).tgz
	$(Q)rm -rf $(@D)
	$(Q)mkdir -p $(@D)
	$(Q)tar -C $(@D) --strip-components=2 -xf _deps/download/$(ESBUILD_T)-$(ESBUILD_V).tgz


# clang for compiling to wasm
_deps/llvm/bin/clang:
	$(MAKE) \
		_deps/download/llvm-17.0.3-toolchain-aarch64-macos.tar.xz \
		_deps/download/llvm-17.0.3-compiler-rt-wasm32-playbit.tar.xz \
		_deps/download/llvm-17.0.3-sysroot-wasm32-playbit.tar.xz
	rm -rf _deps/llvm
	mkdir -p _deps/llvm
	tar -C _deps/llvm -xf _deps/download/llvm-17.0.3-toolchain-aarch64-macos.tar.xz
	tar -C _deps/llvm -xf _deps/download/llvm-17.0.3-compiler-rt-wasm32-playbit.tar.xz
	tar -C _deps/llvm -xf _deps/download/llvm-17.0.3-sysroot-wasm32-playbit.tar.xz
	#@ Move wasm-opt aside to avoid wasm-ld running it, since we run it ourselves
	rm -rf _deps/binaryen
	mkdir -p _deps/binaryen/bin
	mv _deps/llvm/bin/wasm-opt _deps/binaryen/bin/wasm-opt

_deps/download/llvm-%:
	mkdir -p $(@D)
	cd $(@D) && curl -#LO https://github.com/playbit/llvm/releases/download/v17.0.3-1/$(@F)


MAKEFLAGS += --no-print-directory
.PHONY: all clean dev _dev dev-web
.PHONY: test run_tests run_dew_selftest test-runtime run-test-runtime
.PHONY: benchmark benchmark-runtime run-benchmark-runtime
