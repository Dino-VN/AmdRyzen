# Separate OFFLINE build. No installation, loading, root build or dependency fetch.
ROOT := $(abspath $(dir $(lastword $(MAKEFILE_LIST))))
REPO := $(abspath $(ROOT)/..)
BUILD := $(ROOT)/build
SDK ?= $(shell xcrun --sdk macosx --show-sdk-path)
CXX := $(shell xcrun --sdk macosx -f clang++)
CC := $(shell xcrun --sdk macosx -f clang)
KFW := $(SDK)/System/Library/Frameworks/Kernel.framework
LAB := $(REPO)/tools/cppc-lab
# Explicit saved metadata for profile-specific tests; never a runtime input.
# No machine-local temporary capture path is assumed or acquired automatically.
CAPTURE ?=
# Already-built dependency; this Makefile MUST NOT rebuild or overwrite it.
PM_BUNDLE ?= $(REPO)/build/AMDZenPM.kext
MINOS := 12.0
INC := -I$(ROOT)/include -I$(ROOT)/src -I$(ROOT)/cli -I$(ROOT)/build/generated \
       -I$(LAB)/src -I$(REPO)/AMDZenPM/src
WARN := -Wall -Wextra -Werror -Wconversion -Wsign-conversion -Wshadow -Wformat=2
BASE := -std=c++17 -isysroot $(SDK) $(WARN) -fno-exceptions -fno-rtti $(INC)
CORE := $(ROOT)/src/ProbeABI.cpp $(ROOT)/src/Profile.cpp $(ROOT)/src/Sha256.cpp \
        $(ROOT)/src/ReadEngine.cpp $(LAB)/src/Descriptors.cpp
OPTIONS := $(ROOT)/cli/Options.cpp
HDRS := $(wildcard $(ROOT)/include/*.hpp $(ROOT)/src/*.hpp $(ROOT)/cli/*.hpp) \
        $(LAB)/src/CppcLab.hpp $(REPO)/AMDZenPM/src/AMDZenShared.h
GENERATED := $(BUILD)/generated/StaticCpc.inc
NAME := AMDZenPCCProbe
KEXT := $(BUILD)/$(NAME).kext
BIN := $(KEXT)/Contents/MacOS/$(NAME)
KCOMMON := -arch x86_64 -isysroot $(SDK) -mmacosx-version-min=$(MINOS) -mkernel \
           -fno-builtin -fno-common -nostdinc -isystem $(KFW)/PrivateHeaders -isystem $(KFW)/Headers \
           -DKERNEL -DKERNEL_PRIVATE -DDRIVER_PRIVATE -DAPPLE -DNeXT -O2 -g $(WARN) \
           -Wno-deprecated-declarations -Wframe-larger-than=2048 $(INC)
KCXX := $(KCOMMON) -std=gnu++17 -fapple-kext -fno-exceptions -fno-rtti -fcheck-new
KLINK := -arch x86_64 -isysroot $(SDK) -mmacosx-version-min=$(MINOS) -mkernel -nostdlib \
         -Xlinker -kext -lkmodc++ -lkmod -lcc_kext
KERNEL := $(ROOT)/kernel/AMDZenPCCProbe.cpp $(ROOT)/kernel/UserClient.cpp
OBJS := $(patsubst $(ROOT)/src/%.cpp,$(BUILD)/obj/%.o,$(filter $(ROOT)/src/%,$(CORE))) \
        $(BUILD)/obj/Descriptors.o $(BUILD)/obj/AMDZenPCCProbe.o $(BUILD)/obj/UserClient.o $(BUILD)/obj/kmod_info.o
SAN := -O1 -g -fno-omit-frame-pointer -fsanitize=address,undefined -fno-sanitize-recover=all

.PHONY: all check test sanitize linkcheck audit clean require-capture
all: $(KEXT)/Contents/_CodeSignature/CodeResources $(BUILD)/pccprobe
require-capture:
	@if [ -z "$(CAPTURE)" ]; then \
	  printf '%s\n' 'Set CAPTURE to an existing saved metadata directory; no tables are acquired automatically.' >&2; exit 1; \
	fi
	@for table in PCCT.bin APIC.bin SSDT-2.bin SSDT-3.bin; do \
	  if [ ! -f "$(CAPTURE)/$$table" ]; then printf 'Missing saved metadata: %s\n' "$(CAPTURE)/$$table" >&2; exit 1; fi; \
	done
$(GENERATED): $(ROOT)/scripts/embed_profile.py $(LAB)/fixtures/static.cpc
	python3 -I $(ROOT)/scripts/embed_profile.py $(LAB)/fixtures/static.cpc $@
$(BUILD)/obj/%.o: $(ROOT)/src/%.cpp $(HDRS) $(GENERATED)
	@mkdir -p $(@D)
	$(CXX) $(KCXX) -c $< -o $@
$(BUILD)/obj/Descriptors.o: $(LAB)/src/Descriptors.cpp $(HDRS) $(GENERATED)
	@mkdir -p $(@D)
	$(CXX) $(KCXX) -c $< -o $@
$(BUILD)/obj/%.o: $(ROOT)/kernel/%.cpp $(ROOT)/kernel/AMDZenPCCProbe.hpp $(HDRS) $(GENERATED)
	@mkdir -p $(@D)
	$(CXX) $(KCXX) -c $< -o $@
$(BUILD)/obj/kmod_info.o: $(ROOT)/kernel/kmod_info.c
	@mkdir -p $(@D)
	$(CC) $(KCOMMON) -std=gnu11 -DMODULE_NAME=org.amdzen.pccprobe -DMODULE_VERSION='"0.1.0"' -c $< -o $@
$(BIN): $(OBJS) $(ROOT)/Info.plist
	@mkdir -p $(@D)
	cp $(ROOT)/Info.plist $(KEXT)/Contents/Info.plist
	$(CXX) $(KLINK) $(OBJS) -o $@
$(KEXT)/Contents/_CodeSignature/CodeResources: $(BIN)
	codesign -f -s - $(KEXT)
$(BUILD)/pccprobe: $(CORE) $(OPTIONS) $(ROOT)/cli/main.cpp $(HDRS) $(GENERATED)
	$(CXX) $(BASE) -O2 -arch x86_64 -isysroot $(SDK) -mmacosx-version-min=$(MINOS) \
	  $(CORE) $(OPTIONS) $(ROOT)/cli/main.cpp -framework IOKit -framework CoreFoundation -o $@
	codesign -f -s - $@
$(BUILD)/tests: $(CORE) $(OPTIONS) $(ROOT)/tests/test.cpp $(HDRS) $(GENERATED)
	$(CXX) $(BASE) -Wpedantic -O2 $(CORE) $(OPTIONS) $(ROOT)/tests/test.cpp -o $@
$(BUILD)/tests-san: $(CORE) $(OPTIONS) $(ROOT)/tests/test.cpp $(HDRS) $(GENERATED)
	$(CXX) $(BASE) -Wpedantic $(SAN) $(CORE) $(OPTIONS) $(ROOT)/tests/test.cpp -o $@
test: require-capture $(BUILD)/tests
	$(BUILD)/tests "$(CAPTURE)"
sanitize: require-capture $(BUILD)/tests-san
	ASAN_OPTIONS=halt_on_error=1 UBSAN_OPTIONS=halt_on_error=1 $(BUILD)/tests-san "$(CAPTURE)"
linkcheck: all
	sh $(REPO)/tools/linkcheck.sh $(KEXT) $(PM_BUNDLE)
	codesign --verify --strict $(KEXT)
	codesign --verify --strict $(BUILD)/pccprobe
audit: all
	python3 -I $(ROOT)/scripts/audit.py $(ROOT)
check: test sanitize linkcheck audit
clean:
	rm -rf $(BUILD)
