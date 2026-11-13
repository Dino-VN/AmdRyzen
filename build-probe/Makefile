# Plain-clang kext build (no Xcode project). Flags mirror Xcode 26.5's
# KernelExtension.xcspec (com.apple.product-type.kernel-extension):
#   -mkernel -fapple-kext -fno-builtin -fno-common -fno-exceptions -fno-rtti
#   -nostdinc -nostdlib, defines KERNEL KERNEL_PRIVATE DRIVER_PRIVATE APPLE NeXT,
#   header path Kernel.framework/{PrivateHeaders,Headers},
#   LDFLAGS -Xlinker -kext -lkmodc++ -lkmod -lcc_kext
#
# Usage:  make            -> build/AmdCpuProbe.kext + build/SymProbe.kext
#         make check      -> kextlibs / kmutil libraries reports (read-only)
# Nothing here loads a kext.

SDK      ?= $(shell xcrun --sdk macosx --show-sdk-path)
CXX      := $(shell xcrun --sdk macosx -f clang++)
CC       := $(shell xcrun --sdk macosx -f clang)
KFW      := $(SDK)/System/Library/Frameworks/Kernel.framework
ARCH     := x86_64
MINOS    ?= 11.0
BUILD    := build

COMMON   := -arch $(ARCH) -isysroot $(SDK) -mmacosx-version-min=$(MINOS) \
            -mkernel -fno-builtin -fno-common -nostdinc \
            -I$(KFW)/PrivateHeaders -I$(KFW)/Headers \
            -DKERNEL -DKERNEL_PRIVATE -DDRIVER_PRIVATE -DAPPLE -DNeXT \
            -O2 -g -Wall -Wno-unused-function -Wno-deprecated-declarations
CFLAGS   := $(COMMON) -std=gnu11
CXXFLAGS := $(COMMON) -std=gnu++17 -fapple-kext -fno-exceptions -fno-rtti -fcheck-new \
            -Wno-inconsistent-missing-override
LDFLAGS  := -arch $(ARCH) -isysroot $(SDK) -mmacosx-version-min=$(MINOS) \
            -mkernel -nostdlib -Xlinker -kext -lkmodc++ -lkmod -lcc_kext

KEXTS := AmdCpuProbe SymProbe
AmdCpuProbe_SRCS := src/AmdCpuProbe.cpp
AmdCpuProbe_ID   := com.amdcpu.probe.AmdCpuProbe
SymProbe_SRCS    := src/SymProbe.cpp
SymProbe_ID      := com.amdcpu.probe.SymProbe
VERSION          := 0.0.1

all: $(foreach k,$(KEXTS),$(BUILD)/$(k).kext/Contents/MacOS/$(k))

define KEXT_RULES
$(BUILD)/obj/$(1)/%.o: src/%.cpp src/*.h src/*.hpp
	@mkdir -p $$(@D)
	$(CXX) $(CXXFLAGS) -c $$< -o $$@

$(BUILD)/obj/$(1)/kmod_info.o: src/kmod_info.c
	@mkdir -p $$(@D)
	$(CC) $(CFLAGS) -DMODULE_NAME=$($(1)_ID) -DMODULE_VERSION='"$(VERSION)"' -c $$< -o $$@

$(BUILD)/$(1).kext/Contents/MacOS/$(1): $(patsubst src/%.cpp,$(BUILD)/obj/$(1)/%.o,$($(1)_SRCS)) $(BUILD)/obj/$(1)/kmod_info.o $(1)/Info.plist
	@mkdir -p $$(@D)
	cp $(1)/Info.plist $(BUILD)/$(1).kext/Contents/Info.plist
	$(CXX) $(LDFLAGS) $$(filter %.o,$$^) -o $$@
	codesign -f -s - $(BUILD)/$(1).kext
endef
$(foreach k,$(KEXTS),$(eval $(call KEXT_RULES,$(k))))

check: all
	@for k in $(KEXTS); do \
	  echo "=== kextlibs -xml -undef-symbols -unsupported $$k"; \
	  /usr/sbin/kextlibs -xml -undef-symbols -unsupported $(BUILD)/$$k.kext; \
	  echo "=== kmutil libraries -p $$k"; \
	  /usr/bin/kmutil libraries -p $(BUILD)/$$k.kext --undef-symbols; \
	done

clean:
	rm -rf $(BUILD)

.PHONY: all check clean
