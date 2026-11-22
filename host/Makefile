# STRATA host tests and offline renderer.
#
# core/ and dsp/ are built with the host compiler under the same rules as the device build
# (C++20, no exceptions, no RTTI, warnings are errors), so code that would not compile for the
# C674x does not compile here either. core/ must never include an Enaga header; dsp/ may
# include <enaga/dsp.h>, which builds on the host too.
#
#   make -C host test     build and run the tests
#   make -C host list     print the test names
#   make -C host build    build the test binary without running it
#   make -C host clean
#
# There is intentionally no cmake: it is not installed on the WSL build host, and a Makefile is
# enough for a flat set of sources.

ROOT      := $(abspath $(CURDIR)/..)
ENAGA_INC := $(ROOT)/third_party/enaga/include
OUT       := $(if $(ENAGA_BUILD),$(ENAGA_BUILD)/host,$(ROOT)/build/host)

CXX      ?= g++
CXXFLAGS := -std=c++20 -fno-exceptions -fno-rtti -Wall -Wextra -Werror -O2 -g \
            -I$(ROOT)/core -I$(ROOT)/dsp -I$(ROOT)/app -I$(ROOT)/tests -I$(ENAGA_INC)

CORE_SRC  := $(wildcard $(ROOT)/core/*.cpp)
DSP_SRC   := $(wildcard $(ROOT)/dsp/*.cpp)
TEST_SRC  := $(wildcard $(ROOT)/tests/*.cpp)

# app/ builds here too, against tests/fakehost.cpp.
#
# Enaga's C++ API is header-only: every call in <enaga/app.hpp>, <enaga/clock.hpp> and
# <enaga/files.hpp> is an inline function over `enaga::host()`, which returns one plain C struct
# of function pointers and is "defined by the app runtime". This Makefile does not link Enaga's
# runtime.cpp, so tests/fakehost.cpp defines it instead and nothing clashes -- app/ is compiled
# for the tests exactly as it is compiled for the device, with no #ifdef anywhere in it.
#
# app/app.cpp is NOT here, and now has nothing in it to compile: it is `#include "app.hpp"` plus
# ENAGA_APP(Strata), which defines the package's entry points and belongs to a device build. The
# App class itself moved to app/app.hpp, so the modes and the gestures ARE in this suite --
# tests/test_ui.cpp includes that header and drives the same class the unit runs. There is
# nothing left to add to APP_SRC: a header needs no translation unit of its own.
APP_SRC   := $(ROOT)/app/engine.cpp $(ROOT)/app/storage.cpp

SRC       := $(CORE_SRC) $(DSP_SRC) $(APP_SRC) $(TEST_SRC)

# dsp/*.ti.c: the hot inner loops CLAUDE.md requires to be plain C (the device build sends them
# to cl6x, this one builds them with GCC as the fallback does). They are what the C++ in dsp/
# calls, so the host tests exercise exactly the same code the unit runs.
DSP_C_SRC := $(wildcard $(ROOT)/dsp/*.c)
DSP_C_OBJ := $(patsubst $(ROOT)/%.c,$(OUT)/%.o,$(DSP_C_SRC))

# <enaga/dsp.hpp> is a C++ wrapper over block functions implemented in C under
# third_party/enaga/runtime/dsp. Nothing in dsp/ links on the host without them, so they are
# built here too -- the same recipe Enaga's own tests/test_dsp.py uses (C11, warnings as errors).
ENAGA_DSP_SRC := $(wildcard $(ROOT)/third_party/enaga/runtime/dsp/*.c)
ENAGA_DSP_OBJ := $(patsubst $(ROOT)/%.c,$(OUT)/%.o,$(ENAGA_DSP_SRC))

CC       ?= gcc
CFLAGS   := -std=c11 -Wall -Wextra -Werror -O2 -g -I$(ENAGA_INC)

# Mirror each source to an object under OUT, keeping its directory so that identically named
# files in core/ and dsp/ do not collide here. The device build forbids that collision anyway
# (tools/build.sh refuses it), but the host build should not be the thing that discovers it.
OBJ := $(patsubst $(ROOT)/%.cpp,$(OUT)/%.o,$(SRC)) $(DSP_C_OBJ) $(ENAGA_DSP_OBJ)
DEP := $(OBJ:.o=.d)

BIN := $(OUT)/strata-tests

.PHONY: test list build clean render
test: $(BIN)
	@$(BIN)

build: $(BIN)

list: $(BIN)
	@$(BIN) --list

$(BIN): $(OBJ)
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) $(OBJ) -o $@

$(OUT)/%.o: $(ROOT)/%.cpp
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) -MMD -MP -c $< -o $@

$(OUT)/%.o: $(ROOT)/%.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -MMD -MP -c $< -o $@

# The offline renderer (host/render.cpp): every object the tests use except the runner and the
# tests themselves, plus tests/fakehost.o for the clock. Writes WAVs into $(OUT)/render.
RENDER_OBJ := $(filter-out $(OUT)/tests/%,$(OBJ)) $(OUT)/tests/fakehost.o $(OUT)/host/render.o
RENDER_BIN := $(OUT)/strata-render

render: $(RENDER_BIN)
	@mkdir -p $(OUT)/render
	@$(RENDER_BIN) $(OUT)/render $(BARS)

$(RENDER_BIN): $(RENDER_OBJ)
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) $(RENDER_OBJ) -o $@

clean:
	rm -rf $(OUT)

-include $(DEP) $(OUT)/host/render.d
