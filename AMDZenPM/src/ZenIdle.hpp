//
// ZenIdle.hpp — CC6 idle hook through the exported XNU pmKextRegister KPI.
//
// The kernel's machine_idle() calls pmDispatch->MachineIdle() on every idle entry
// of every CPU once a table is registered and initComplete() has run. Our table
// sets only MachineIdle; every other slot stays NULL so all other kernel paths
// behave exactly as with no PM kext (docs/research/xnu-pm.md §4, §9).
//
// Lifetime: a registration cannot be undone through any exported API, so it
// happens at most once per boot, and the kext retains itself so its text (the
// table and MachineIdle) can never be unmapped. stop() only clears the
// MachineIdle slot; the kernel re-reads it on each idle and falls back to its
// own sti;hlt;cli.
//
// Threading: everything except machineIdle() runs on the AMDZenPM work loop.
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
};

// True once pmKextRegister has been called in this boot (by any instance).
bool registered();
// Registers the hook (pmKextRegister -> self-retain -> initComplete). Returns false
// without touching the kernel if a registration already happened this boot.
bool registerHook(const HookConfig &cfg);
// After a stop() of an earlier instance: publish a new configuration and put
// MachineIdle back into the (still registered) table.
void rearm(const HookConfig &cfg);
// stop(): atomically clear the MachineIdle slot. The table stays resident.
void unhook();

uint32_t mode();
uint32_t maxMode();
void     setMode(uint32_t m);          // caller clamps to maxMode()
void     setMaxMode(uint32_t m);       // lowers mode() too if needed
void     setMwait(bool on);            // false: MONITOR/MWAIT never executed again
void     setThresholdTicks(uint64_t t);
uint16_t port();

// Sum of the per-CPU counters of CPUs 0..ncpu-1.
void totals(IdleTotals *out, uint32_t ncpu);

// Evaluate _CST of processor 0 (AppleACPICPU IOCPUNumber 0 -> its
// IOACPIPlatformDevice provider). Returns true if _CST returned a package; the
// parsed entries (at most `max`) go to out/n. `where` receives the ACPI name used.
bool readAcpiCst(CstEntry *out, uint32_t max, uint32_t *n, char *where, uint32_t whereLen);

} // namespace idle
} // namespace zen

#endif // ZEN_IDLE_HPP
