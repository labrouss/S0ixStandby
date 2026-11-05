# Makefile — builds S0ixStandby.kext with plain clang + MacKernelSDK.
# Used by GitHub Actions (macOS runner). Locally on macOS:
#   git clone --depth 1 https://github.com/acidanthera/MacKernelSDK
#   make            # Release
#   make DEBUG=1    # Debug
#   make dist       # zip in build/
# SPDX-License-Identifier: BSD-3-Clause

KEXT       := S0ixStandby
BUNDLE_ID  := com.labrouss.S0ixStandby
# Kext version must be numeric (x.y.z): it comes from the VERSION file.
VERSION    := $(shell cat VERSION)
GITREV     := $(shell git describe --always --dirty 2>/dev/null || echo nogit)
SDK        ?= MacKernelSDK
MIN_OS     ?= 10.13
BUILD      := build
CONFIG     := $(if $(DEBUG),Debug,Release)
OUT        := $(BUILD)/$(CONFIG)/$(KEXT).kext

# Apple toolchain (Xcode). Override on the command line if needed.
CC         := xcrun clang
CXX        := xcrun clang++
SDKROOT    := $(shell xcrun --show-sdk-path 2>/dev/null)
RESDIR     := $(shell xcrun clang -print-resource-dir 2>/dev/null)
# Kernel C++ glue and compiler runtime: use what this Xcode actually ships.
KMODCXX    := $(if $(wildcard $(SDKROOT)/usr/lib/libkmodc++.a),-lkmodc++,)
CCKEXT     := $(firstword $(wildcard $(RESDIR)/lib/darwin/libclang_rt.cc_kext.a) -lcc_kext)
ARCH       := -arch x86_64 -mmacosx-version-min=$(MIN_OS)

COMMON     := $(ARCH) -mkernel -nostdinc -fno-builtin -fno-common \
              -DKERNEL -DKERNEL_PRIVATE -DDRIVER_PRIVATE -DAPPLE -DNeXT \
              -I$(SDK)/Headers -Wall -Wextra -Wno-unused-parameter \
              $(if $(DEBUG),-O0 -g -DDEBUG,-Os)
CXXFLAGS   := $(COMMON) -std=gnu++17 -fapple-kext -fno-rtti -fno-exceptions
CFLAGS     := $(COMMON) -std=gnu11 \
              -DKEXT_BUNDLE_ID=$(BUNDLE_ID) -DKEXT_VERSION='"$(VERSION)"'
LDFLAGS    := $(ARCH) -mkernel -nostdlib -Xlinker -kext -Xlinker -export_dynamic \
              $(SDK)/Library/x86_64/libkmod.a $(KMODCXX) $(CCKEXT)

OBJ        := $(BUILD)/$(CONFIG)/obj
SRCS_CPP   := $(KEXT)/$(KEXT).cpp $(KEXT)/S0ixGPU.cpp
SRCS_C     := $(KEXT)/kmod_info.c
OBJS       := $(SRCS_CPP:$(KEXT)/%.cpp=$(OBJ)/%.o) $(SRCS_C:$(KEXT)/%.c=$(OBJ)/%.o)

.PHONY: all clean dist
all: $(OUT)

$(OBJ)/%.o: $(KEXT)/%.cpp $(KEXT)/$(KEXT).hpp | $(OBJ)
	$(CXX) $(CXXFLAGS) -c $< -o $@
$(OBJ)/%.o: $(KEXT)/%.c | $(OBJ)
	$(CC) $(CFLAGS) -c $< -o $@
$(OBJ):
	mkdir -p $@

$(OUT): $(OBJS) $(KEXT)/Info.plist
	rm -rf $@ && mkdir -p $@/Contents/MacOS
	$(CXX) $(OBJS) $(LDFLAGS) -o $@/Contents/MacOS/$(KEXT)
	$(if $(DEBUG),,strip -x $@/Contents/MacOS/$(KEXT))
	sed 's/__VERSION__/$(VERSION)/g' $(KEXT)/Info.plist > $@/Contents/Info.plist
	@echo "Built $@ ($(VERSION), $(GITREV))"

dist: all
	cd $(BUILD)/$(CONFIG) && zip -qry ../$(KEXT)-$(VERSION)-$(GITREV)-$(CONFIG).zip $(KEXT).kext
	@echo "Packed $(BUILD)/$(KEXT)-$(VERSION)-$(GITREV)-$(CONFIG).zip"

clean:
	rm -rf $(BUILD)
