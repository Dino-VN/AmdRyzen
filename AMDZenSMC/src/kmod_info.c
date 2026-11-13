// Equivalent of Xcode's generated <product>_info.c (GENERATE_KERNEL_MODULE_INFO_FILE),
// with a stop routine that refuses unload once the plugin may have been submitted. MODULE_NAME / MODULE_VERSION come from -D.
//
// Why refuse: once the plugin is submitted, VirtualSMC keeps a retain on the
// AMDZenSMC service and pointers to its VirtualSMCValue objects and their vtable for
// the rest of the boot (docs/research/smc-keys.md §1.6). OSKext::stop calls this
// before tearing down C++ state, so KERN_FAILURE keeps the code mapped.
#include <mach/mach_types.h>

extern kern_return_t _start(kmod_info_t *ki, void *data);
extern kern_return_t _stop(kmod_info_t *ki, void *data);

// KMOD_EXPLICIT_DECL stringizes its first argument, so expand MODULE_NAME first.
#define AMDCPU_KMOD_DECL(n, v, s, t) KMOD_EXPLICIT_DECL(n, v, s, t)
__attribute__((visibility("default")))
AMDCPU_KMOD_DECL(MODULE_NAME, MODULE_VERSION, _start, _stop)

// Set by AMDZenSMC.cpp right before the plugin can be handed to VirtualSMC.
extern int gAMDZenSMCPinned;

static kern_return_t amdzensmc_refuse_stop(kmod_info_t *ki, void *data) {
    (void)ki;
    (void)data;
    // Unpinned (never registered: -amdzensmcoff, conflict, AMDZenPM not ready) means
    // nothing outside this kext points into it, so allow unload; that also lets
    // AMDZenPM be unloaded during development.
    return __atomic_load_n(&gAMDZenSMCPinned, __ATOMIC_SEQ_CST) ? KERN_FAILURE : KERN_SUCCESS;
}

__private_extern__ kmod_start_func_t *_realmain = 0;
__private_extern__ kmod_stop_func_t *_antimain = amdzensmc_refuse_stop;
__private_extern__ int _kext_apple_cc = __APPLE_CC__;
