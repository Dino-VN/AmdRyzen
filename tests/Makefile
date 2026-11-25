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

$(BUILD)/test_idle_sampling: test_idle_sampling.cpp $(SRC)/ZenIdleAccounting.hpp $(SRC)/ZenIdleModel.cpp $(SRC)/ZenIdleModel.hpp $(SRC)/ZenModel.cpp $(SRC)/ZenModel.hpp $(SRC)/AMDZenShared.h $(SRC)/ZenRegs.h
	@mkdir -p $(BUILD)
	$(HOSTCXX) $(CXXFLAGS) test_idle_sampling.cpp $(SRC)/ZenIdleModel.cpp $(SRC)/ZenModel.cpp -o $@

$(BUILD)/test_pcc: test_pcc.cpp $(SRC)/ZenPccModel.cpp $(SRC)/ZenPccModel.hpp $(SRC)/ZenCppcTypes.hpp
	@mkdir -p $(BUILD)
	$(HOSTCXX) $(CXXFLAGS) -fno-exceptions -fno-rtti test_pcc.cpp $(SRC)/ZenPccModel.cpp -o $@

$(BUILD)/test_native_policy: test_native_policy.cpp $(SRC)/ZenNativePolicy.cpp $(SRC)/ZenNativePolicy.hpp $(SRC)/ZenNativeSnapshot.hpp $(SRC)/ZenCppcTypes.hpp
	@mkdir -p $(BUILD)
	$(HOSTCXX) $(CXXFLAGS) test_native_policy.cpp $(SRC)/ZenNativePolicy.cpp -o $@

$(BUILD)/test_native_integration: test_native_integration.cpp test_pcc.cpp $(wildcard native_stubs/*.hpp native_stubs/kern/*.h native_stubs/libkern/*.h) $(SRC)/AMDZenPMNative.cpp $(SRC)/AMDZenNativeControl.hpp $(SRC)/ZenNativePolicy.cpp $(SRC)/ZenPccModel.cpp $(SRC)/ZenIdleModel.cpp $(wildcard $(SRC)/*.hpp $(SRC)/*.h)
	@mkdir -p $(BUILD)
	$(HOSTCXX) $(CXXFLAGS) -Inative_stubs test_native_integration.cpp $(SRC)/ZenNativePolicy.cpp $(SRC)/ZenPccModel.cpp $(SRC)/ZenIdleModel.cpp -o $@

test: $(BUILD)/test_model $(BUILD)/test_idle $(BUILD)/test_idle_sampling $(BUILD)/test_pcc $(BUILD)/test_native_policy $(BUILD)/test_native_integration
	$(BUILD)/test_model
	$(BUILD)/test_idle
	$(BUILD)/test_idle_sampling
	$(BUILD)/test_pcc
	$(BUILD)/test_native_policy
	$(BUILD)/test_native_integration

# Kernel-flag compile of the model (same flags as build-probe/Makefile).
SDK      ?= $(shell xcrun --sdk macosx --show-sdk-path)
KCXX     := $(shell xcrun --sdk macosx -f clang++)
KFW      := $(SDK)/System/Library/Frameworks/Kernel.framework
KFLAGS   := -arch x86_64 -isysroot $(SDK) -mmacosx-version-min=11.0 -mkernel -fno-builtin \
            -fno-common -nostdinc -I$(KFW)/PrivateHeaders -I$(KFW)/Headers \
            -DKERNEL -DKERNEL_PRIVATE -DDRIVER_PRIVATE -DAPPLE -DNeXT -O2 -Wall -Wextra \
            -std=gnu++17 -fapple-kext -fno-exceptions -fno-rtti -fcheck-new \
            -Werror -Wno-unused-function -Wno-deprecated-declarations \
            -Wno-inconsistent-missing-override -Wno-overloaded-virtual

KSOURCES := ZenModel ZenIdleModel ZenPccModel ZenPcc ZenNativePolicy ZenNativeSignals AMDZenPMNative ZenHW
KOBJECTS := $(addprefix $(BUILD)/,$(addsuffix .kext.o,$(KSOURCES)))

$(BUILD)/%.kext.o: $(SRC)/%.cpp $(wildcard $(SRC)/*.h $(SRC)/*.hpp)
	@mkdir -p $(BUILD)
	$(KCXX) $(KFLAGS) -c $< -o $@

kcheck: $(KOBJECTS)
	@echo "undefined symbols:"
	@nm -u $(KOBJECTS)
	@echo "static initializers:"
	@otool -l $(KOBJECTS) | grep -c mod_init_func || true

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
