//
// XnuPrivate.h — XNU x86_64 symbols exported to third-party kexts through
// com.apple.kpi.unsupported that have no usable prototype in the MacOSX SDK.
// Prototypes from xnu-12377 (osfmk/i386/mp.h, cpu_number.h, proc_reg.h, tsc.h).
// Verified link-resolvable on macOS 26 (build-probe + tools/linkcheck.sh).
//
// NOT exported to third-party kexts (do not use): mp_cpus_call, mp_broadcast
// (kpi.private), wrmsr_carefully, tscFreq, ml_get_max_cpus.
//
#ifndef ZEN_XNU_PRIVATE_H
#define ZEN_XNU_PRIVATE_H

#include <stdint.h>
#include <mach/mach_types.h>

#ifdef __cplusplus
extern "C" {
#endif

int cpu_number(void);                       // current logical CPU (0..real_ncpus-1)
extern unsigned int real_ncpus;             // number of logical CPUs registered

// Runs action_func on every CPU simultaneously with interrupts disabled and
// returns after all completed. Must be called from thread context, preemption
// enabled, no spinlocks held. action_func must be short and must not block.
void mp_rendezvous_no_intrs(void (*action_func)(void *), void *arg);

// Returns 0 on success, non-zero if RDMSR raised #GP (MSR not implemented).
int rdmsr_carefully(uint32_t msr, uint32_t *lo, uint32_t *hi);

struct zen_tscInfo {
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
void tsc_get_info(struct zen_tscInfo *info);

// osfmk/i386/pmCPU.h: the CPU power-management dispatch (kpi.unsupported exports
// only _pmKextRegister and _pmCPUControl). Panics if version != 102 or callbacks
// is NULL, and if cpuFuncs != NULL while a table is already registered. The
// kernel keeps the cpuFuncs pointer (no copy) for the rest of the boot. Struct
// layouts are in ZenIdle.cpp (docs/research/xnu-pm.md §1, §2).
#define ZEN_PM_DISPATCH_VERSION 102u
void pmKextRegister(uint32_t version, void *cpuFuncs, void *callbacks);

// bsd/kern/kern_sysctl.c kernel_sysctlbyname, exported as _sysctlbyname
// (Libkern.exports) but not declared in the SDK's kernel sys/sysctl.h.
int sysctlbyname(const char *name, void *oldp, size_t *oldlenp, void *newp, size_t newlen);

#ifdef __cplusplus
}
#endif

static inline int zen_rdmsr_safe(uint32_t msr, uint64_t *val) {
    uint32_t lo = 0, hi = 0;
    if (rdmsr_carefully(msr, &lo, &hi) != 0) return -1;
    *val = ((uint64_t)hi << 32) | lo;
    return 0;
}
// Plain RDMSR/WRMSR: only use on MSRs already validated with zen_rdmsr_safe
// (there is no exported wrmsr_carefully; an invalid WRMSR panics).
static inline uint64_t zen_rdmsr_raw(uint32_t msr) {
    uint32_t lo, hi;
    __asm__ volatile("rdmsr" : "=a"(lo), "=d"(hi) : "c"(msr));
    return ((uint64_t)hi << 32) | lo;
}
static inline void zen_wrmsr_raw(uint32_t msr, uint64_t v) {
    __asm__ volatile("wrmsr" : : "c"(msr), "a"((uint32_t)v), "d"((uint32_t)(v >> 32)) : "memory");
}
static inline void zen_cpuid(uint32_t leaf, uint32_t sub, uint32_t r[4]) {
    __asm__ volatile("cpuid" : "=a"(r[0]), "=b"(r[1]), "=c"(r[2]), "=d"(r[3]) : "a"(leaf), "c"(sub));
}
static inline uint64_t zen_rdtsc(void) {
    uint32_t lo, hi;
    __asm__ volatile("rdtsc" : "=a"(lo), "=d"(hi));
    return ((uint64_t)hi << 32) | lo;
}

#endif // ZEN_XNU_PRIVATE_H
