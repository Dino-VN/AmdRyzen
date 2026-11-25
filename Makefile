# AMDZenPM.kext + AMDZenSMC.kext — plain-clang kext build (no Xcode project).
# Flags are the ones proven in build-probe/Makefile (Xcode 26.5 KernelExtension.xcspec).
#
#   make          -> build/AMDZenPM.kext, build/AMDZenSMC.kext (ad-hoc signed)
#   make check    -> offline link check of both kexts (tools/linkcheck.sh), plus a
#                    no-SSE check of AMDZenSMC (its readAccess runs in VirtualSMC's trap path)
#   make test     -> host unit tests (tests/Makefile, if present)
#   make clean
#
# Nothing here loads or unloads a kext.

SDK      ?= $(shell xcrun --sdk macosx --show-sdk-path)
CXX      := $(shell xcrun --sdk macosx -f clang++)
CC       := $(shell xcrun --sdk macosx -f clang)
KFW      := $(SDK)/System/Library/Frameworks/Kernel.framework
ARCH     := x86_64
MINOS    ?= 11.0
ROOT     := $(abspath $(dir $(lastword $(MAKEFILE_LIST))))
BUILD    := $(ROOT)/build

NAME     := AMDZenPM
BUNDLEID := com.dinovn.AMDZenPM
VERSION  := 0.0.1
SRCDIR   := $(ROOT)/AMDZenPM/src
PLIST    := $(ROOT)/AMDZenPM/Info.plist
KEXT     := $(BUILD)/$(NAME).kext
BIN      := $(KEXT)/Contents/MacOS/$(NAME)
OBJDIR   := $(BUILD)/obj/$(NAME)

COMMON   := -arch $(ARCH) -isysroot $(SDK) -mmacosx-version-min=$(MINOS) \
            -mkernel -fno-builtin -fno-common -nostdinc \
            -I$(KFW)/PrivateHeaders -I$(KFW)/Headers \
            -DKERNEL -DKERNEL_PRIVATE -DDRIVER_PRIVATE -DAPPLE -DNeXT \
            -O2 -g -Wall -Wno-unused-function -Wno-deprecated-declarations
CFLAGS   := $(COMMON) -std=gnu11
CXXFLAGS := $(COMMON) -std=gnu++17 -fapple-kext -fno-exceptions -fno-rtti -fcheck-new \
            -Wno-inconsistent-missing-override -Wno-overloaded-virtual
LDFLAGS  := -arch $(ARCH) -isysroot $(SDK) -mmacosx-version-min=$(MINOS) \
            -mkernel -nostdlib -Xlinker -kext -lkmodc++ -lkmod -lcc_kext

SRCS     := $(wildcard $(SRCDIR)/*.cpp)
HDRS     := $(wildcard $(SRCDIR)/*.h $(SRCDIR)/*.hpp)
OBJS     := $(patsubst $(SRCDIR)/%.cpp,$(OBJDIR)/%.o,$(SRCS)) $(OBJDIR)/kmod_info.o

# ---- AMDZenSMC.kext (VirtualSMC plugin). Links against VirtualSMC and AMDZenPM at
# load time; the SDK headers are vendored in third_party/ (BSD-3, see its README).
SMC_NAME     := AMDZenSMC
SMC_BUNDLEID := com.dinovn.AMDZenSMC
SMC_VERSION  := 0.0.1
SMC_SRCDIR   := $(ROOT)/AMDZenSMC/src
SMC_PLIST    := $(ROOT)/AMDZenSMC/Info.plist
SMC_KEXT     := $(BUILD)/$(SMC_NAME).kext
SMC_BIN      := $(SMC_KEXT)/Contents/MacOS/$(SMC_NAME)
SMC_OBJDIR   := $(BUILD)/obj/$(SMC_NAME)
# -isystem: the vendored headers are not -Wall clean and must not add warnings here.
SMC_INC      := -I$(SRCDIR) -isystem $(ROOT)/third_party/virtualsmc -isystem $(ROOT)/third_party/lilu
SMC_SRCS     := $(wildcard $(SMC_SRCDIR)/*.cpp)
SMC_HDRS     := $(wildcard $(SMC_SRCDIR)/*.h $(SMC_SRCDIR)/*.hpp) $(SRCDIR)/AMDZenShared.h \
                $(wildcard $(ROOT)/third_party/virtualsmc/VirtualSMCSDK/*) \
                $(wildcard $(ROOT)/third_party/lilu/Headers/*)
SMC_OBJS     := $(patsubst $(SMC_SRCDIR)/%.cpp,$(SMC_OBJDIR)/%.o,$(SMC_SRCS)) $(SMC_OBJDIR)/kmod_info.o
# Library kexts that AMDZenSMC's imports resolve against (nothing is loaded).
SMC_REPOS    := $(ROOT)/deps/VirtualSMC/VirtualSMC-1.3.9-RELEASE/Kexts \
                $(ROOT)/deps/Lilu/Lilu-1.7.3-RELEASE $(BUILD)

all: $(KEXT)/Contents/_CodeSignature/CodeResources $(SMC_KEXT)/Contents/_CodeSignature/CodeResources

$(OBJDIR)/%.o: $(SRCDIR)/%.cpp $(HDRS)
	@mkdir -p $(@D)
	$(CXX) $(CXXFLAGS) -c $< -o $@

$(OBJDIR)/kmod_info.o: $(SRCDIR)/kmod_info.c
	@mkdir -p $(@D)
	$(CC) $(CFLAGS) -DMODULE_NAME=$(BUNDLEID) -DMODULE_VERSION='"$(VERSION)"' -c $< -o $@

$(BIN): $(OBJS) $(PLIST)
	@mkdir -p $(@D)
	cp $(PLIST) $(KEXT)/Contents/Info.plist
	$(CXX) $(LDFLAGS) $(OBJS) -o $@

$(KEXT)/Contents/_CodeSignature/CodeResources: $(BIN)
	codesign -f -s - $(KEXT)

$(SMC_OBJDIR)/%.o: $(SMC_SRCDIR)/%.cpp $(SMC_HDRS)
	@mkdir -p $(@D)
	$(CXX) $(CXXFLAGS) $(SMC_INC) -c $< -o $@

$(SMC_OBJDIR)/kmod_info.o: $(SMC_SRCDIR)/kmod_info.c
	@mkdir -p $(@D)
	$(CC) $(CFLAGS) -DMODULE_NAME=$(SMC_BUNDLEID) -DMODULE_VERSION='"$(SMC_VERSION)"' -c $< -o $@

$(SMC_BIN): $(SMC_OBJS) $(SMC_PLIST)
	@mkdir -p $(@D)
	cp $(SMC_PLIST) $(SMC_KEXT)/Contents/Info.plist
	$(CXX) $(LDFLAGS) $(SMC_OBJS) -o $@

$(SMC_KEXT)/Contents/_CodeSignature/CodeResources: $(SMC_BIN)
	codesign -f -s - $(SMC_KEXT)

# Offline link check: every undefined symbol must resolve against the installed KCs
# (AMDZenSMC: plus the VirtualSMC, Lilu and freshly built AMDZenPM bundles).
check: all
	sh $(ROOT)/tools/linkcheck.sh $(KEXT)
	sh $(ROOT)/tools/linkcheck.sh $(SMC_KEXT) $(SMC_REPOS)
	@if otool -tV $(SMC_BIN) | grep -Eq '%([xyz]?mm[0-9]|st)'; then \
	  echo "AMDZenSMC: SSE/AVX/x87/MMX instructions found (must be integer-only):"; \
	  otool -tV $(SMC_BIN) | grep -E '%([xyz]?mm[0-9]|st)'; exit 1; \
	else echo "AMDZenSMC: no SSE/AVX/x87/MMX instructions"; fi

test:
	@if [ -f $(ROOT)/tests/Makefile ]; then \
	  if grep -q '^run:' $(ROOT)/tests/Makefile; then t=run; else t=test; fi; \
	  $(MAKE) -C $(ROOT)/tests $$t; \
	else \
	  echo "tests/Makefile not present; skipping host unit tests"; \
	fi

kcheck:
	$(MAKE) -C $(ROOT)/tests kcheck

clean:
	rm -rf $(BUILD)

.PHONY: all check test kcheck clean
