//
// xnu_private.h - declarations for XNU x86_64 symbols that are exported to
// third-party kexts through the com.apple.kpi.unsupported / com.apple.kpi.private
// symbol sets but have no (usable) prototype in the MacOSX26.5 SDK Kernel.framework.
//
// Prototypes written from xnu-12377.121.6 (osfmk/i386/mp.h, mp_events.h,
// cpu_number.h, proc_reg.h, tsc.h, pmCPU.h, x86_64/locore.s).
// Verified against the macOS 26.7.2 (xnu-12377.161.15) kernel __LINKINFO,__symbolsets.
//
#ifndef AMDCPU_XNU_PRIVATE_H
#define AMDCPU_XNU_PRIVATE_H

#include <stdint.h>
#include <mach/mach_types.h>

#ifdef __cplusplus
extern "C" {
#endif

// ---- com.apple.kpi.unsupported -------------------------------------------

// osfmk/i386/cpu_number.h
int cpu_number(void);

// osfmk/i386/mp.h
extern unsigned int real_ncpus;          // number of logical CPUs registered
void mp_rendezvous(void (*setup_func)(void *),
                   void (*action_func)(void *),
                   void (*teardown_func)(void *),
                   void *arg);
void mp_rendezvous_no_intrs(void (*action_func)(void *), void *arg);

// osfmk/i386/proc_reg.h (also declared in SDK i386/proc_reg.h)
// returns 0 on success, 1 if the rdmsr raised #GP
int rdmsr_carefully(uint32_t msr, uint32_t *lo, uint32_t *hi);

// osfmk/i386/tsc.h  (tscFreq itself is NOT exported; use this)
struct amdcpu_tscInfo {
    uint64_t busFCvtt2n;
    uint64_t busFCvtn2t;
    uint64_t tscFreq;        // Hz
    uint64_t tscFCvtt2n;
    uint64_t tscFCvtn2t;
    uint64_t tscGranularity;
    uint64_t bus2tsc;
    uint64_t busFreq;
    uint32_t flex_ratio;
    uint32_t flex_ratio_min;
    uint32_t flex_ratio_max;
};
void tsc_get_info(struct amdcpu_tscInfo *info);

// osfmk/i386/pmCPU.h -- only ever call with cpuFuncs == NULL from a 3rd-party
// kext (non-NULL installs a pmDispatch table; a 2nd registration panics).
// version must equal PM_DISPATCH_VERSION (102 in xnu-12377) or the kernel panics.
void pmKextRegister(uint32_t version, void *cpuFuncs /* pmDispatch_t* */,
                    void *callbacks /* pmCallBacks_t* */);

// ---- com.apple.kpi.private (Apple-only when linked by kmutil) -------------

// osfmk/i386/mp_events.h / mp.h
typedef enum { AMDCPU_SYNC = 0, AMDCPU_ASYNC = 1, AMDCPU_NOSYNC = 2 } amdcpu_mp_sync_t;
typedef uint32_t amdcpu_cpu_t;
typedef volatile uint64_t amdcpu_cpumask_t;
amdcpu_cpu_t mp_cpus_call(amdcpu_cpumask_t cpus, amdcpu_mp_sync_t mode,
                          void (*action_func)(void *), void *arg);
void mp_broadcast(void (*action_func)(void *), void *arg);

// ---- NOT exported to kexts at all (need Lilu solveSymbol or avoidance) ----
//   wrmsr_carefully : real ABI is int wrmsr_carefully(uint32_t msr, uint64_t val)
//                     (locore.s ENTRY(wrmsr_carefully) implements wrmsr64_carefully)
//   tscFreq         : extern uint64_t tscFreq;  -> use tsc_get_info()
//   ml_get_max_cpus : unsigned int ml_get_max_cpus(void); -> use real_ncpus
//   kernel_sysctlbyname : exported as alias "_sysctlbyname" in com.apple.kpi.libkern
//   OSKextGetCurrentIdentifier : provided statically by libkmod.a (c_start.o)

#ifdef __cplusplus
}
#endif

// Ring-0 MSR access without any kernel symbol (what all AMD Hackintosh kexts do).
static inline uint64_t amdcpu_rdmsr(uint32_t msr) {
    uint32_t lo, hi;
    __asm__ volatile("rdmsr" : "=a"(lo), "=d"(hi) : "c"(msr));
    return ((uint64_t)hi << 32) | lo;
}
static inline void amdcpu_wrmsr(uint32_t msr, uint64_t v) {
    __asm__ volatile("wrmsr" : : "c"(msr), "a"((uint32_t)v), "d"((uint32_t)(v >> 32)));
}
static inline void amdcpu_cpuid(uint32_t leaf, uint32_t sub, uint32_t r[4]) {
    __asm__ volatile("cpuid" : "=a"(r[0]), "=b"(r[1]), "=c"(r[2]), "=d"(r[3]) : "a"(leaf), "c"(sub));
}

#endif
