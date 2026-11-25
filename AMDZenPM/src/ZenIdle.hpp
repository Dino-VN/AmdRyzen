//
// ZenIdle.hpp — CC6 idle hook through the exported XNU pmKextRegister KPI.
//
// The kernel's machine_idle() calls pmDispatch->MachineIdle() on every idle entry
// of every CPU once a table is registered and initComplete() has run. Our table
// sets MachineIdle plus store-only get/setMaxIntDelay and get/setMaxBusDelay
// (latency requests, which keep the hook out of CC6 while they are tighter than
// its exit latency), plus a store-only pmThreadTellUrgency observer. Other slots
// stay NULL (docs/research/xnu-pm.md §4, §9).
//
// Lifetime: a registration cannot be undone through any exported API, so it
// happens at most once per boot, and the kext retains itself so its text (the
// table and callouts) can never be unmapped. Installed slots never change: XNU
// can check a slot, then reload it to call. stop() disables idle work and signal
// visibility with an atomic flag; resident urgency recording continues.
//
// Threading: configuration runs on the AMDZenPM work loop. The urgency callout
// runs on the owning CPU with preemption disabled/interrupts off, using only
// the cpu_number KPI and an inline per-CPU atomic publication.
// machineIdle() runs on every CPU with interrupts off and reads the published
// configuration with relaxed atomic loads.
//
#ifndef ZEN_IDLE_HPP
#define ZEN_IDLE_HPP

#include <stdint.h>
#include "ZenIdleModel.hpp"

namespace zen {
namespace idle {

struct HookConfig {
    uint32_t mode;            // initial IdleMode (already clamped to maxMode)
    uint32_t maxMode;         // highest mode the hardware supports
    uint16_t port;            // CStateAddr + k, 0 when maxMode < kIdleCc6
    bool     mwait;           // MONITOR/MWAIT with IF=0 usable
    uint64_t thresholdTicks;  // deep/shallow threshold (TSC ticks)
    uint64_t thresholdNs;     // deep/shallow threshold (ns, for rtcDeadline check)
    uint64_t pollTicks;       // short pre-MWAIT spin budget (~3 us in TSC ticks)
    uint64_t pollEwmaMax;     // only pre-poll when predicted idle < ~15 us (TSC ticks)
    uint32_t ncpu;            // logical CPUs (all-cores-deep accounting)
    uint64_t deepLatencyNs;   // CC6 exit latency compared with latency requests
};

// True once pmKextRegister has been called in this boot (by any instance).
bool registered();
// Registers the hook (self-retain -> pmKextRegister -> initComplete). Returns false
// without touching the kernel if a registration already happened this boot.
bool registerHook(const HookConfig &cfg);
// After a stop() of an earlier instance: publish configuration and enable idle
// work and urgency snapshots through the permanently installed table.
void rearm(const HookConfig &cfg);
// stop(): disable idle work and urgency snapshots. Callbacks remain installed.
void unhook();

uint32_t mode();
uint32_t maxMode();
void     setMode(uint32_t m);          // caller clamps to maxMode()
void     setMaxMode(uint32_t m);       // lowers mode() too if needed
void     setMwait(bool on);            // false: MONITOR/MWAIT never executed again
void     setThresholdTicks(uint64_t t, uint64_t ns = 0);
void     setPreferredCores(const uint16_t *primaryOfCore, const uint16_t *coreOfCpu,
                           const uint16_t *rankOfCore, uint32_t ncpu, uint32_t ncores);
uint16_t port();

// Sum of the per-CPU counters of CPUs 0..ncpu-1.
void totals(IdleTotals *out, uint32_t ncpu);

// True while registered, initialized and enabled. When disabled, the resident
// MachineIdle uses sti;hlt;cli without accounting; only active snapshots are useful.
bool active();
// True while initialized urgency snapshots are enabled; independent of idle mode.
bool nativeSignalsActive();
// Latest latency requests the kernel handed to the table (ns, 0 = none).
void latencyRequests(uint64_t *maxIntDelayNs, uint64_t *maxBusDelayNs);

// Governor sampling without an IPI: each coherent CPU observation carries its own
// TSC (closed periods plus any open interval up to that time). Even seqcounts are
// stable; an independent flag marks idle. After at most eight attempts a racing
// CPU returns valid=false, never a closed-only substitute. Callers must re-prime
// invalid baselines and use each CPU's elapsed time, not a scan-wide cutoff.
// Callable from thread context on any CPU; assumes a synchronized invariant TSC.
void snapshot(IdleSnapshot *out, uint32_t ncpu);

// Evaluate _CST of processor 0 (AppleACPICPU IOCPUNumber 0 -> its
// IOACPIPlatformDevice provider). Returns true if _CST returned a package; the
// parsed entries (at most `max`) go to out/n. `where` receives the ACPI name used.
bool readAcpiCst(CstEntry *out, uint32_t max, uint32_t *n, char *where, uint32_t whereLen);

} // namespace idle
} // namespace zen

#endif // ZEN_IDLE_HPP
