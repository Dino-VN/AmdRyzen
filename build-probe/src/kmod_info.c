// Equivalent of Xcode's generated <product>_info.c (GENERATE_KERNEL_MODULE_INFO_FILE).
// MODULE_NAME / MODULE_VERSION are passed with -D from the Makefile.
#include <mach/mach_types.h>

extern kern_return_t _start(kmod_info_t *ki, void *data);
extern kern_return_t _stop(kmod_info_t *ki, void *data);

// KMOD_EXPLICIT_DECL stringizes its first argument, so expand MODULE_NAME first.
#define AMDCPU_KMOD_DECL(n, v, s, t) KMOD_EXPLICIT_DECL(n, v, s, t)
__attribute__((visibility("default")))
AMDCPU_KMOD_DECL(MODULE_NAME, MODULE_VERSION, _start, _stop)

__private_extern__ kmod_start_func_t *_realmain = 0;
__private_extern__ kmod_stop_func_t *_antimain = 0;
__private_extern__ int _kext_apple_cc = __APPLE_CC__;
