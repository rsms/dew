NATIVE_SYS   := $(subst Linux,linux,$(subst Darwin,darwin,$(shell uname -s)))
NATIVE_ARCH  := $(subst aarch64,arm64,$(shell uname -m))
TARGET       := $(NATIVE_SYS)
BUILDDIR     ?= o.$(TARGET)$(if $(filter $(DEBUG),1),.debug,)$(if $(filter $(TEST),1),.test,)
Q             = $(if $(filter 1,$(V)),,@)
QLOG          = $(if $(filter 1,$(V)),@#,@echo)
EMBED_SRC    := 1
OBJDIR       := $(BUILDDIR)/obj
SRCS         := dew.c runtime.c lib_bignum.c bn.c time.c array.c logmsg.c platform.c \
                $(if $(filter $(TARGET),web),runloop_wasm.c,runloop_ev.c libev/ev.c)
LUA_SRCS     := lapi.c lcode.c lctype.c ldebug.c ldo.c ldump.c lfunc.c lgc.c llex.c lmem.c lobject.c \
                lopcodes.c lparser.c lstate.c lstring.c ltable.c ltm.c lundump.c lvm.c lzio.c \
                lauxlib.c lbaselib.c lcorolib.c ldblib.c liolib.c lmathlib.c loadlib.c loslib.c \
                lstrlib.c ltablib.c lutf8lib.c linit.c
LUA_SRCS     := $(sort $(addprefix lua/src/,$(LUA_SRCS)))
JSSRCS       := $(wildcard web/*.ts web/index.html)
CFLAGS       := -std=c17 -g -fdebug-compilation-dir=/x/ \
                -Wall -Wextra -Werror=format -Wno-unused -Wno-unused-parameter \
                -Werror=incompatible-pointer-types \
                -Ilua/src $(if $(filter $(EMBED_SRC),1),-DDEW_EMBED_SRC=1 -I$(BUILDDIR),)
LDFLAGS      :=
LIBEV_CFLAGS := -Wno-comment -Wno-sign-compare -Wno-extern-initializer -Wno-bitwise-op-parentheses
LUA_CFLAGS   :=
LUA          := o.$(NATIVE_SYS)/lua
LUAC         := o.$(NATIVE_SYS)/luac
ALL          := $(BUILDDIR)/dew
ORIGPATH     := ${PATH}

ifeq ($(TARGET),darwin)
	LUA_CFLAGS += -DLUA_USE_MACOSX
else ifeq ($(TARGET),linux)
	LUA_CFLAGS += -DLUA_USE_LINUX
	LDFLAGS += -Wl,-E -ldl
else ifeq ($(TARGET),web)
	SRCS += wasm.c
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
	CFLAGS += -DDEBUG=1
	ifneq ($(TARGET),web)
		CFLAGS += -fsanitize=address,undefined
		LDFLAGS += -fsanitize=address,undefined
	endif
else ifeq ($(TEST),1)
	CFLAGS += -DDEBUG=1 -O1 -fsanitize=address,undefined
	LDFLAGS += -fsanitize=address,undefined
else
	CFLAGS += -DNDEBUG -flto=thin $(if $(filter $(TARGET),web),-Oz,-O2)
endif

ifeq ($(TARGET),darwin)
	LDFLAGS += -Wl,-cache_path_lto,$(BUILDDIR)/lto
else
	LDFLAGS += -Wl,--thinlto-cache-dir=$(BUILDDIR)/lto
endif

LUA_OBJS := $(addprefix $(OBJDIR)/,$(patsubst %,%.o,$(LUA_SRCS)))
DEW_OBJS := $(addprefix $(OBJDIR)/,$(patsubst %,%.o,$(SRCS)))
OBJS     := $(DEW_OBJS) $(LUA_OBJS)

all: $(ALL)
clean:
	rm -rf o.*

dev:
	autorun *.c *.h *.lua lua/src/*.c libev/*.* examples/*.dew \
	-- '$(MAKE) DEBUG=1 EMBED_SRC=0 _dev'
_dev: $(BUILDDIR)/dew
	$(BUILDDIR)/dew examples/dev.dew --debug-tokens --debug-parse --debug-resolve --debug-codegen

dev-web:
	autorun *.c *.h *.lua lua/src/*.c examples/*.dew web/*.* -- \
	'$(MAKE) DEBUG=1 TARGET=web'

test:
	$(MAKE) TEST=1 EMBED_SRC=0 _test
_test: $(BUILDDIR)/dew
	$(BUILDDIR)/dew --selftest$(if $(filter 1,$(V)),=v,)


$(BUILDDIR)/dew: $(OBJS)
	$(QLOG) "LINK  $@"
	$(Q)mkdir -p $(@D)
	$(Q)$(CC) -o $@ $^ $(LDFLAGS)

LSRCS := \
	util.lua \
	unit.lua \
	diag.lua \
	srcpos.lua \
	tokenize.lua \
	id.lua \
	ast.lua \
	parse.lua \
	resolve.lua \
	codegen.lua
$(BUILDDIR)/dew.lua: $(LSRCS) dew.lua
	$(QLOG) "GEN   $@"
	$(Q)mkdir -p $(@D)
	$(Q)rm -f $@
	$(Q)$(foreach f,$^,cat $(f) >> $@;)
	$(Q)sed -i '' -E 's/\s*(require\()/--\1/' $@

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

$(BUILDDIR)/dew.js: web/dew.ts $(ESBUILD) $(JSSRCS)
	$(QLOG) "ESBLD $@"
	$(Q)$(ESBUILD) --bundle $(JSFLAGS) --outfile=$@ $<

$(BUILDDIR)/index.html: web/index.html
	$(QLOG) "COPY  $@"
	$(Q)mkdir -p $(@D)
	$(Q)cp $< $@

# tools that run on build host (NATIVE)
ifeq ($(NATIVE_SYS),$(TARGET))
$(LUA):  lua/src/lua.c
$(LUAC): lua/src/luac.c
$(LUA) $(LUAC): $(LUA_OBJS)
	$(QLOG) "LINK  $@"
	$(Q)mkdir -p $(@D)
	$(Q)$(CC) -o $@ $^ $(LDFLAGS)
else
$(LUA) $(LUAC):
	$(Q)env -i 'PATH=${ORIGPATH}' sh -c '$(MAKE) -j$$(nproc) $@'
endif

ifeq ($(EMBED_SRC),1)
$(OBJDIR)/dew.c.o: $(BUILDDIR)/dew.lua.h
endif

$(OBJDIR)/libev/ev.c.o: CFLAGS += $(LIBEV_CFLAGS)

ifeq ($(TARGET),web)
$(OBJS): _deps/llvm/bin/clang
# # use c++ exceptions instead of longjmp:
# $(OBJDIR)/lua/src/ldo.c.o: lua/src/ldo.c
# 	$(QLOG) "CXX   $<"
# 	$(Q)$(CXX) -MP -MMD $(CFLAGS) $(CXXFLAGS) -std=c++17 -xc++ -c -o "$@" $<
endif

$(OBJDIR)/lua/src/%.c.o: CFLAGS += $(LUA_CFLAGS)
$(OBJDIR)/%.c.o: %.c
	$(QLOG) "CC    $<"
	$(Q)$(CC) -MP -MMD $(CFLAGS) -c -o "$@" $<
$(OBJDIR)/%.cc.o: %.cc
	$(QLOG) "CC    $<"
	$(Q)$(CXX) -MP -MMD $(CFLAGS) $(CXXFLAGS) -std=c++17 -xc++ -c -o "$@" $<

# source file dependencies
OBJ_DIRS := $(sort $(patsubst %/,%,$(dir $(OBJS))))
$(OBJS): | $(OBJ_DIRS)
$(OBJ_DIRS):
	$(QLOG) "MKDIR $@"
	$(Q)mkdir -p "$@"
-include $(wildcard $(OBJS:.o=.d))

# tooling for web
web/node_modules/.bin/esbuild: web/package-lock.json
	touch $@
web/package-lock.json: web/package.json
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
.PHONY: all clean dev _dev dev-web test _test
