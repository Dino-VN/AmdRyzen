# Host unit tests for the pure model code (AMDZenPM/src/ZenModel.cpp, ZenIdleModel.cpp).
#   make          build + run tests
#   make kcheck   compile the model sources with the kext flags and list undefined symbols
# Nothing here loads a kext or touches hardware.

ROOT     := $(abspath $(dir $(lastword $(MAKEFILE_LIST)))/..)
SRC      := $(ROOT)/AMDZenPM/src
BUILD    := $(ROOT)/tests/build
CXX      ?= clang++
HOSTCXX  := clang++
CXXFLAGS := -std=c++17 -O1 -g -Wall -Wextra -Werror -Wno-unused-function \
            -fsanitize=address,undefined -fno-sanitize-recover=all -I$(SRC)

all: test

$(BUILD)/test_model: test_model.cpp $(SRC)/ZenModel.cpp $(SRC)/ZenModel.hpp $(SRC)/AMDZenShared.h $(SRC)/ZenRegs.h
	@mkdir -p $(BUILD)
	$(HOSTCXX) $(CXXFLAGS) test_model.cpp $(SRC)/ZenModel.cpp -o $@

$(BUILD)/test_idle: test_idle.cpp $(SRC)/ZenIdleModel.cpp $(SRC)/ZenIdleModel.hpp $(SRC)/ZenRegs.h
	@mkdir -p $(BUILD)
	$(HOSTCXX) $(CXXFLAGS) test_idle.cpp $(SRC)/ZenIdleModel.cpp -o $@

test: $(BUILD)/test_model $(BUILD)/test_idle
	$(BUILD)/test_model
	$(BUILD)/test_idle

# Kernel-flag compile of the model (same flags as build-probe/Makefile).
SDK      ?= $(shell xcrun --sdk macosx --show-sdk-path)
KCXX     := $(shell xcrun --sdk macosx -f clang++)
KFW      := $(SDK)/System/Library/Frameworks/Kernel.framework
KFLAGS   := -arch x86_64 -isysroot $(SDK) -mmacosx-version-min=11.0 -mkernel -fno-builtin \
            -fno-common -nostdinc -I$(KFW)/PrivateHeaders -I$(KFW)/Headers \
            -DKERNEL -DKERNEL_PRIVATE -DDRIVER_PRIVATE -DAPPLE -DNeXT -O2 -Wall -Wextra \
            -std=gnu++17 -fapple-kext -fno-exceptions -fno-rtti -fcheck-new

kcheck:
	@mkdir -p $(BUILD)
	$(KCXX) $(KFLAGS) -c $(SRC)/ZenModel.cpp -o $(BUILD)/ZenModel.kext.o
	$(KCXX) $(KFLAGS) -c $(SRC)/ZenIdleModel.cpp -o $(BUILD)/ZenIdleModel.kext.o
	@echo "undefined symbols:"; nm -u $(BUILD)/ZenModel.kext.o $(BUILD)/ZenIdleModel.kext.o || true
	@echo "static initializers:"; otool -l $(BUILD)/ZenModel.kext.o $(BUILD)/ZenIdleModel.kext.o | grep -c mod_init_func || true

clean:
	rm -rf $(BUILD)

.PHONY: all test kcheck clean

# ---- AMDZenSMC: integer SMC encoders and key-set rules (header-only, AMDZenSMC/src).
# Hooked into `test` by an extra prerequisite line so the rules above stay untouched.
SMCSRC   := $(ROOT)/AMDZenSMC/src

$(BUILD)/test_smc_encode: test_smc_encode.cpp $(SMCSRC)/SmcEncode.hpp $(SMCSRC)/SmcKeySet.hpp $(SRC)/AMDZenShared.h
	@mkdir -p $(BUILD)
	$(HOSTCXX) $(CXXFLAGS) -I$(SMCSRC) test_smc_encode.cpp -o $@

test-smc: $(BUILD)/test_smc_encode
	$(BUILD)/test_smc_encode

test: test-smc

.PHONY: test-smc
