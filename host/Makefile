# STRATA host tests and offline renderer.
#
# core/ and dsp/ are built with the host compiler under the same rules as the device build
# (C++20, no exceptions, no RTTI, warnings are errors), so code that would not compile for the
# C674x does not compile here either. core/ must never include an Enaga header; dsp/ may
# include <enaga/dsp.h>, which builds on the host too.
#
#   make -C host test     build and run the tests
#   make -C host list     print the test names
#   make -C host clean
#
# There is intentionally no cmake: it is not installed on the WSL build host, and a Makefile is
# enough for a flat set of sources.

ROOT      := $(abspath $(CURDIR)/..)
ENAGA_INC := $(ROOT)/third_party/enaga/include
OUT       := $(if $(ENAGA_BUILD),$(ENAGA_BUILD)/host,$(ROOT)/build/host)

CXX      ?= g++
CXXFLAGS := -std=c++20 -fno-exceptions -fno-rtti -Wall -Wextra -Werror -O2 -g \
            -I$(ROOT)/core -I$(ROOT)/dsp -I$(ROOT)/tests -I$(ENAGA_INC)

CORE_SRC  := $(wildcard $(ROOT)/core/*.cpp)
DSP_SRC   := $(wildcard $(ROOT)/dsp/*.cpp)
TEST_SRC  := $(wildcard $(ROOT)/tests/*.cpp)
SRC       := $(CORE_SRC) $(DSP_SRC) $(TEST_SRC)

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
OBJ := $(patsubst $(ROOT)/%.cpp,$(OUT)/%.o,$(SRC)) $(ENAGA_DSP_OBJ)
DEP := $(OBJ:.o=.d)

BIN := $(OUT)/strata-tests

.PHONY: test list clean
test: $(BIN)
	@$(BIN)

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

clean:
	rm -rf $(OUT)

-include $(DEP)
