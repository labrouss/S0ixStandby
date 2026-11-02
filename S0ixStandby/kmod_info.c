// kmod_info.c — kext module info (what Xcode normally generates).
// SPDX-License-Identifier: BSD-3-Clause
#include <mach/mach_types.h>

#ifndef KEXT_BUNDLE_ID
#define KEXT_BUNDLE_ID com.labrouss.S0ixStandby
#endif
#ifndef KEXT_VERSION
#define KEXT_VERSION "0.0.0"
#endif

extern kern_return_t _start(kmod_info_t *ki, void *data);
extern kern_return_t _stop(kmod_info_t *ki, void *data);

__attribute__((visibility("default")))
KMOD_EXPLICIT_DECL(KEXT_BUNDLE_ID, KEXT_VERSION, _start, _stop)

// IOKit C++ kext: libkmod's _start runs the C++ runtime, no C entry points.
__private_extern__ kmod_start_func_t *_realmain = 0;
__private_extern__ kmod_stop_func_t  *_antimain = 0;
__private_extern__ int _kext_apple_cc = __APPLE_CC__;
