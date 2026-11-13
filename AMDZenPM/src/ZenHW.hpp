//
// ZenHW.hpp — hardware access layer: SMN (PCI root complex) and per-CPU MSR
// operations executed with mp_rendezvous_no_intrs.
//
// Threading contract: every method must be called from thread context (the
// AMDZenPM work loop, holding its gate) — never from interrupt context, never
// with a spinlock held. Rendezvous actions are short, run with interrupts off
// on every logical CPU, and must not allocate, block or log.
//
#ifndef ZEN_HW_HPP
#define ZEN_HW_HPP

#include <IOKit/IOLib.h>
#include <IOKit/IOLocks.h>
#include <IOKit/pci/IOPCIDevice.h>
#include "AMDZenShared.h"
#include "ZenModel.hpp"
#include "ZenIdleModel.hpp"

namespace zen {

// ------------------------------------------------------------------ SMN
class SMN {
public:
    // Locate bus0/dev0/fn0 IOPCIDevice with vendor AMD (0x1022) or Hygon (0x1D94),
    // class 0x0600xx. Waits up to timeoutMs for it to be published. Retains it.
    bool init(uint32_t timeoutMs);
    void free();
    bool available() const { return root != nullptr; }
    // Serialized index/data access (0x60/0x64). Returns false (and *val = 0) if not
    // available or the read returned 0xFFFFFFFF.
    bool read(uint32_t addr, uint32_t *val);
    uint16_t rootDeviceId() const { return rootDevId; }
private:
    IOPCIDevice *root {nullptr};
    IOLock      *lock {nullptr};
    uint16_t     rootDevId {0};
};

// ------------------------------------------------------------------ CPU
// Raw per-CPU results of one rendezvous.
struct PerCpuRaw {
    PerfCounters perf;        // aperf, mperf, tsc, irperf
    uint64_t pstateStatus;    // C001_0063
    uint64_t pstateCtl;       // C001_0062 (as read before any write in the same job)
    uint64_t coreEnergy;      // C001_029A (only on the primary thread of each core)
    uint64_t hwPstateStatus;  // C001_0293 (primary thread only)
    uint64_t hwcr;            // C001_0015
};

// Operations for one rendezvous (bitmask).
enum : uint32_t {
    kOpSamplePerf     = 1u << 0, // APERF/MPERF/TSC(/IRPERF), PSTATE_STATUS, PSTATE_CTL
    kOpSampleSlow     = 1u << 1, // core energy + HwPstateStatus (primary threads), HWCR
    kOpWritePState    = 1u << 2, // write PSTATE_CTL[2:0] = pstateReq[cpu] if != cached
    kOpWriteCppcReq   = 1u << 3, // write CPPC_REQ = cppcReq[cpu] if != cached
    kOpCppcEnable     = 1u << 4, // write CPPC_ENABLE = 1
    kOpHwcrModify     = 1u << 5, // HWCR = (HWCR & ~hwcrClear) | hwcrSet
    kOpWritePStateDef = 1u << 6, // PStateDef[defIndex] = defValue on every CPU
};

struct Topology {
    uint32_t ncpu {0};                         // logical CPUs (real_ncpus, <= AMDZEN_MAX_CPUS)
    uint32_t ncores {0};                       // physical cores
    uint32_t threadsPerCore {1};
    uint32_t smtShift {0};
    uint32_t numCcx {1};
    uint16_t apicId[AMDZEN_MAX_CPUS] {};
    uint16_t coreOfCpu[AMDZEN_MAX_CPUS] {};    // dense core index
    uint16_t primaryCpuOfCore[AMDZEN_MAX_CORES] {}; // lowest cpu_number in the core
    uint16_t ccxOfCore[AMDZEN_MAX_CORES] {};
};

class CPU {
public:
    // CPUID feature detection (current CPU), topology (rendezvous: each CPU runs
    // CPUID 1/0xB/8000_001E), and MSR probing with rdmsr_carefully on every CPU.
    // Fills `info` (identity, features, pstates, CPPC caps, ESU, microcode, PPIN).
    bool discover(AMDZenInfo *info, CpuModelInfo *model);

    const Topology &topo() const { return topology; }
    uint64_t features() const { return feat; }
    uint64_t tscHz() const { return tsc; }

    // Execute `ops` on all CPUs. `out` (size ncpu) may be null if no sample op.
    // Arrays are indexed by cpu_number(). Writes are skipped for CPUs whose cached
    // value already equals the request. Only ops whose capability was probed OK
    // are executed (others silently ignored).
    void run(uint32_t ops, PerCpuRaw *out,
             const uint8_t *pstateReq = nullptr, const uint64_t *cppcReq = nullptr,
             uint64_t hwcrSet = 0, uint64_t hwcrClear = 0,
             uint32_t defIndex = 0, uint64_t defValue = 0);

    // Convenience single-CPU reads on the calling CPU (preemption-safe enough for
    // package-scope MSRs whose value is identical on every CPU).
    bool readPkgEnergy(uint32_t *raw);
    bool readCurLimit(uint64_t *raw);
    bool readPStateDef(uint32_t idx, uint64_t *raw);

    // CC6 capability probe: one read-only rendezvous in which every CPU reads
    // C001_0073 CStateBaseAddr, C001_0296 CStateCfg, C001_0294 CStatePolicy and
    // HWCR with rdmsr_carefully. `out` must hold AMDZEN_MAX_CPUS entries (indexed
    // by cpu_number()); CPUs that did not answer keep ok = 0. Returns topo().ncpu.
    uint32_t readIdleMsrs(IdleMsrSample *out);

    // Cached values from discover()/last writes
    uint64_t cppcReqInitial(uint32_t cpu) const { return cpu < AMDZEN_MAX_CPUS ? cppcReqBoot[cpu] : 0; }
    uint64_t hwcrInitial() const { return hwcrBoot; }
    void invalidateWriteCache();   // after wake: force next writes
    // CPUs that did not answer the last discover() rendezvous (not yet running).
    uint32_t missingCpus() const { return missing; }

private:
    Topology topology;
    uint64_t feat {0};
    uint64_t tsc {0};
    bool     haveCoreEnergy {false}, haveHwPstateStatus {false}, haveIrperf {false};
    bool     haveCppc {false}, haveLegacy {false}, useRoPerf {false};
    uint32_t okMsr {0};          // ZenHW.cpp-internal: MSRs validated on all CPUs (kOk* bits)
    uint32_t pstateMax {0};      // PstateMaxVal (C001_0061[6:4]) found by discover()
    uint64_t hwcrBoot {0};
    uint32_t missing {0};
    uint64_t cppcReqBoot[AMDZEN_MAX_CPUS] {};
    // write caches (last value written / read) per CPU; UINT64_MAX = unknown
    uint64_t pstateCtlCache[AMDZEN_MAX_CPUS] {};
    uint64_t cppcReqCache[AMDZEN_MAX_CPUS] {};
};

} // namespace zen

#endif // ZEN_HW_HPP
