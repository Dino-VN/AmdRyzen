//
// ZenIdleModel.hpp — pure decision logic for the CC6 idle hook (ZenIdle.cpp).
//
// Same rules as ZenModel.hpp: no kernel/IOKit/libc dependencies, no floating
// point, no heap; compiled into the kext and into tests/ on the host. Everything
// that decides *whether* and *how* the hook enters CC6 lives here so it can be
// unit-tested; ZenIdle.cpp only executes the result.
//
// Facts: docs/research/amd-cstate.md (MSR layouts, port rule, IF=0 entry) and
// docs/research/xnu-pm.md (pmDispatch contract), including their critic notes.
//
#ifndef ZEN_IDLE_MODEL_HPP
#define ZEN_IDLE_MODEL_HPP

#include <stdint.h>
#include <stddef.h>

namespace zen {

// ------------------------------------------------------------------ modes
// Values of boot-arg amdzen.idle, personality "IdleMode" and kern.amdzen.idle_mode.
enum IdleMode : uint32_t {
    kIdleOff   = 0,    // boot: do not register. Runtime: HLT inside our hook (= kernel default)
    kIdleHlt   = 1,    // sti; hlt; cli
    kIdleMwait = 2,    // MONITOR + MWAIT C1 with IF=0 (interrupt-break)
    kIdleCc6   = 3,    // IO read of CStateAddr+k (CC6) when the predictor says long, else mode 2
    kIdleModeCount = 4,
    kIdleAuto  = 0xFF, // boot only: 3 when the CC6 capability passes, otherwise do not register
};
const char *idleModeName(uint32_t mode);
// "auto" or "0".."3" -> mode; returns false for anything else.
bool idleModeFromString(const char *s, uint32_t *mode);
// Highest supported mode not above `req` (req 0 stays 0: the runtime "off" fallback).
uint32_t clampIdleMode(uint32_t req, bool haveMwait, bool haveCc6);

// ------------------------------------------------------------------ ACPI _CST
// ACPI Generic Register descriptor (large resource 0x82) as found in each _CST
// package's first element (a Buffer): tag 0x82, length 0x000C, then space id,
// bit width, bit offset, access size, 64-bit address; normally followed by the
// end tag 0x79 0x00.
enum : uint8_t {
    kGasSystemMemory = 0x00,
    kGasSystemIO     = 0x01,
    kGasFfh          = 0x7F,   // functional fixed hardware (MWAIT hint on x86)
};
struct AcpiGas {
    uint8_t  spaceId, bitWidth, bitOffset, accessSize;
    uint64_t address;
};
// Returns false if `buf` is not a well-formed Generic Register descriptor.
bool parseGenericRegister(const uint8_t *buf, size_t len, AcpiGas *out);

struct CstEntry {
    uint32_t type;        // ACPI C-state type (1 = C1, 2 = C2, 3 = C3)
    uint32_t latencyUs;   // worst-case exit latency
    uint32_t power;       // average power (mW), informational
    AcpiGas  reg;
};
constexpr uint32_t kMaxCstEntries = 8;
// One _CST sub-package {Buffer reg, Integer type, Integer latency, Integer power}.
// Returns false if the register buffer does not parse or type is not 1..3.
bool makeCstEntry(const uint8_t *reg, size_t regLen, uint64_t type, uint64_t latency,
                  uint64_t power, CstEntry *out);

// ------------------------------------------------------------------ MSRs
// C001_0296 CStateCfg (per core): CCRn = bits [8n+5 : 8n] CC1DFSID, [8n+6] CC6EN.
constexpr uint64_t kCStateCfgCc6Mask = (1ull << 6) | (1ull << 14) | (1ull << 22);
struct CStateCfg {
    bool    cc6en[3];
    uint8_t cc1dfsid[3];
};
CStateCfg decodeCStateCfg(uint64_t raw);
static inline bool cstateCfgCc6En(uint64_t raw, uint32_t ccr) {
    return ccr < 3 && ((raw >> (8 * ccr + 6)) & 1u) != 0;
}

// Why CC6 entry is (not) possible. Also used as the "IdleCC6" ioreg string.
enum Cc6Reason : uint32_t {
    kCc6Ok = 0,
    kCc6MsrUnreadable,     // C001_0073 / C001_0296 / HWCR #GP (or a CPU did not answer)
    kCc6BaseZero,          // CStateAddr = 0: IO C-states disabled ("Global C-state Control")
    kCc6BaseInvalid,       // CStateAddr > 0xFFF8 (undefined per PPR)
    kCc6BaseMismatch,      // C001_0073 differs between CPUs
    kCc6InvdWbinvdClear,   // HWCR[4] = 0 on some CPU: the PPR requires it with CC6
    kCc6CstNoIoEntry,      // _CST exists but has no SystemIO entry inside base..base+7
    kCc6CstCcr0,           // the deepest in-window _CST entry is base+0 (CCR0, never used)
    kCc6NoCc6En,           // CC6EN clear for the chosen CCR on some CPU
    kCc6ReasonCount
};
const char *cc6ReasonName(uint32_t r);

// C001_0073: base = raw[15:0]; 0 and > 0xFFF8 are rejected. Returns kCc6Ok or a reason.
uint32_t checkCStateBase(uint64_t raw, uint32_t *base);

// Raw per-CPU result of the idle capability rendezvous (ZenHW).
struct IdleMsrSample {
    uint8_t  ok;          // C001_0073 and C001_0296 read without #GP
    uint8_t  hwcrOk;      // HWCR read
    uint8_t  policyOk;    // C001_0294 read (informational only)
    uint8_t  pad[5];
    uint64_t base;        // C001_0073
    uint64_t cfg;         // C001_0296
    uint64_t policy;      // C001_0294
    uint64_t hwcr;        // C001_0015
};

struct IdleCaps {
    bool     msrOk;        // every CPU answered and read C001_0073, C001_0296 and HWCR
    bool     baseUniform;  // C001_0073 identical on all CPUs
    bool     cfgUniform;   // C001_0296 identical on all CPUs (informational)
    bool     invdWbinvd;   // HWCR[4] = 1 on all CPUs
    bool     mwaitAllowed; // HWCR[9] MonMwaitDis = 0 on all CPUs
    uint32_t ncpu;
    uint64_t baseRaw;      // CPU 0 values (published in ioreg)
    uint64_t cfg;
    uint64_t policy;
    uint64_t cfgCc6And;    // AND over all CPUs of (cfg & kCStateCfgCc6Mask)
};
IdleCaps aggregateIdleMsrs(const IdleMsrSample *s, uint32_t n);

// ------------------------------------------------------------------ port choice
enum IdlePortSource : uint32_t { kPortSourceNone = 0, kPortSourceCst = 1, kPortSourceCfg = 2 };
const char *idlePortSourceName(uint32_t s);
struct IdlePortChoice {
    bool     ok;
    uint32_t port;     // base + ccr (the only port the hook ever reads), 0 if !ok
    uint32_t ccr;      // k: 1 or 2
    uint32_t source;   // IdlePortSource
    uint32_t reason;   // Cc6Reason
    int32_t  cstIndex; // _CST entry used (-1 = none)
};
// haveCst: _CST was evaluated and returned a package (cst/n may then hold 0 entries).
// With _CST: the deepest SystemIO entry whose port lies in [base, base+7] gives
// k = min(port - base, 2). Without _CST: k = 1 if CCR1 CC6EN, else 2 if CCR2 CC6EN.
// k = 0 is never used; CC6EN must be set for CCR k on every CPU.
IdlePortChoice chooseIdlePort(const IdleCaps &caps, const CstEntry *cst, uint32_t n, bool haveCst);

// ------------------------------------------------------------------ registration gates
enum IdleHookReason : uint32_t {
    kIdleRegistered = 0,
    kIdleNotEvaluated,
    kIdleDisabled,         // amdzen.idle=0 / IdleMode 0
    kIdleDarwinVersion,    // pmDispatch_t layout only verified for Darwin 20..25
    kIdleHypervisor,
    kIdleBlocked,          // -amdzenro, conflict, unsupported CPU ... (blockReason != none)
    kIdlePmConflict,       // another PM kext's class is loaded: pmKextRegister would panic
    kIdleXcpm,             // machdep.xcpm.mode != 0 (XCPM owns pmDispatch)
    kIdleNoArat,
    kIdleNoInvariantTsc,
    kIdleNoCc6,            // auto mode and the CC6 capability failed (see IdleCC6)
    kIdleHookReasonCount
};
const char *idleHookReasonName(uint32_t r);

struct IdleGateInput {
    uint32_t requested;    // IdleMode or kIdleAuto
    int32_t  darwinMajor;
    bool     hypervisor;
    bool     blocked;
    bool     pmConflict;
    bool     xcpmActive;
    bool     arat;
    bool     tscInvariant;
    bool     mwait;        // MWAIT usable with IF=0 (CPUID 1/5 + HWCR[9])
    bool     cc6;          // chooseIdlePort().ok
};
struct IdleGateResult {
    bool     doRegister;
    uint32_t mode;         // initial mode (clamped)
    uint32_t maxMode;      // highest mode the hardware supports
    uint32_t reason;       // IdleHookReason
};
constexpr int32_t kIdleDarwinMin = 20;  // Info.plist links kpi 20.0.0: nothing older can load us
constexpr int32_t kIdleDarwinMax = 25;  // newest layout verified against a running kernel
IdleGateResult evaluateIdleGates(const IdleGateInput &in);

// ------------------------------------------------------------------ predictor
// Per-CPU EWMA of measured idle length (TSC ticks): ewma += (min(x, clamp) - ewma) / 4.
// The sample is clamped to 2x the deep threshold so that one very long idle (seconds)
// cannot keep the CPU on the deep path for dozens of short idles afterwards: from
// the clamp it takes 3 short idles to drop below the threshold, and 3 long idles
// from 0 to climb above it, which gives some hysteresis against single outliers.
constexpr uint32_t kIdleEwmaShift = 2;
constexpr uint32_t kIdleClampMult = 2;
// 50 us: measured on a 5700X (tools/idlebench.sh, 2026-11-12), cores 2.1 W vs 2.7 W at
// 200 us and 4.2 W with MWAIT C1 only; still well above the 18 us _CST C2 latency.
constexpr uint32_t kIdleThresholdDefaultUs = 50;
constexpr uint32_t kIdleThresholdMaxUs = 1000000;

static inline uint64_t idleEwmaUpdate(uint64_t ewma, uint64_t sample, uint64_t clamp) {
    if (sample > clamp) sample = clamp;
    if (sample >= ewma) return ewma + ((sample - ewma) >> kIdleEwmaShift);
    return ewma - ((ewma - sample) >> kIdleEwmaShift);
}
static inline bool idlePredictDeep(uint64_t ewma, uint64_t thresholdTicks) {
    return ewma >= thresholdTicks;
}
static inline uint64_t idleClampTicks(uint64_t thresholdTicks) {
    return thresholdTicks * kIdleClampMult;
}
// Latency requests (ml_set_maxintdelay / ml_set_maxbusdelay, i.e. IOService
// requireMaxInterruptDelay / requireMaxBusStall; abs time = ns on x86). A request
// of 0 means "no constraint". On AMD Zen, CC6 is a per-core state that flushes
// L1/L2 on entry and disconnects from coherency probes, so DMA/bus-master traffic
// does not stall waiting for a CC6 core (matching Linux acpi_idle C2 and XCPM,
// which routes maxBusDelay to uncore LTR rather than core C-state gating). Only
// interrupt wake latency (maxIntDelayNs) constrains CC6 entry.
constexpr uint32_t kIdleDeepLatencyDefaultUs = 100;   // no _CST latency: be conservative
static inline bool idleLatencyAllowsDeep(uint64_t maxIntDelayNs, uint64_t maxBusDelayNs,
                                         uint64_t deepLatencyNs) {
    (void)maxBusDelayNs;
    return maxIntDelayNs == 0 || maxIntDelayNs >= deepLatencyNs;
}
// Exit latency (us) of the _CST entry the hook uses; the default when unknown.
uint32_t idleDeepLatencyUs(const CstEntry *cst, uint32_t n, int32_t cstIndex);

// Timer-deadline awareness & non-boot timer migration (mirrors XCPM's _xcpm_idle).
// Only migrate non-boot CPUs' timers whose remaining time lies in [minNs, maxNs):
// avoids moving timers about to fire immediately while consolidating medium timers
// onto CPU 0 so secondary cores stay in CC6 longer.
constexpr uint64_t kIdleTimerMigrateMinNs = 100000ull;   // 100 us
constexpr uint64_t kIdleTimerMigrateMaxNs = 5000000ull;  // 5 ms
static inline bool idleShouldMigrateTimer(uint32_t cpu, uint64_t nowNs, uint64_t deadlineNs,
                                          uint64_t minNs = kIdleTimerMigrateMinNs,
                                          uint64_t maxNs = kIdleTimerMigrateMaxNs) {
    if (cpu == 0 || deadlineNs == 0 || deadlineNs == UINT64_MAX || deadlineNs <= nowNs)
        return false;
    const uint64_t rem = deadlineNs - nowNs;
    return rem >= minNs && rem < maxNs;
}
static inline bool idleTimerAllowsDeep(uint64_t nowNs, uint64_t deadlineNs,
                                       uint64_t thresholdNs) {
    if (deadlineNs == 0 || deadlineNs == UINT64_MAX) return true;
    if (deadlineNs <= nowNs) return false;
    return (deadlineNs - nowNs) >= thresholdNs;
}

// MWAIT monitor-write wakeup protocol (exitIdle / pmCPUExitIdle).
// 0 = running / HLT / CC6 (requires LAPIC IPI); 1 = armed in MONITOR/MWAIT C1
// (store wakes target with no IPI); 2 = already woken by monitor store.
enum MonitorState : uint32_t {
    kMonRunning = 0,
    kMonArmed   = 1,
    kMonWoken   = 2,
};
// Called by exitIdle on the waking CPU. Returns true if an IPI is still required;
// returns false if a monitor-write woke (or already woke) the target CPU.
static inline bool idleTryMonitorWake(uint32_t *state) {
    uint32_t cur = __atomic_load_n(state, __ATOMIC_ACQUIRE);
    if (cur == kMonWoken) return false;
    if (cur != kMonArmed) return true;
    if (__atomic_compare_exchange_n(state, &cur, (uint32_t)kMonWoken, false,
                                    __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE))
        return false;
    return cur != kMonWoken;
}

// CPPC Preferred Core ranking for pmChooseCPU.
constexpr uint32_t kPrefMaxCpus  = 256;
constexpr uint32_t kPrefMaxCores = 128;
struct PreferredCoreTable {
    uint32_t count;                     // 0 when disabled or all cores have equal rank
    uint16_t primaryCpu[kPrefMaxCores]; // primary logical CPU per core, sorted by rank desc
    uint16_t siblingOfCpu[kPrefMaxCpus];// SMT sibling logical CPU, or 0xFFFF if none
    uint16_t rankOfCpu[kPrefMaxCpus];   // CPPC highest perf rank of each logical CPU
};
void buildPreferredCoreOrder(const uint16_t *primaryOfCore, const uint16_t *coreOfCpu,
                             const uint16_t *rankOfCore, uint32_t ncpu, uint32_t ncores,
                             PreferredCoreTable *out);

// Select the highest-ranked completely-idle primary CPU in [startCpu, endCpu].
// Preserves cache warmth when preferredCpu is in shallow MWAIT C1 AND already
// at top rank (or no strictly higher-ranked core is completely idle). When a
// workload was bumped to a weaker core and a higher-ranked core is idle, or
// when preferredCpu == -1 / in deep CC6 (L1/L2 flushed), steers to the top core.
template <typename IsIdleFn, typename IsShallowFn>
static inline int choosePreferredCpu(const PreferredCoreTable &tbl, uint32_t ncpu,
                                     int startCpu, int endCpu, int preferredCpu,
                                     IsIdleFn isIdle, IsShallowFn isShallow) {
    if (tbl.count == 0 || ncpu == 0 || startCpu < 0 || endCpu < startCpu)
        return preferredCpu;
    const uint32_t lo = (uint32_t)startCpu;
    const uint32_t hi = (uint32_t)endCpu < ncpu ? (uint32_t)endCpu : ncpu - 1;
    if (lo > hi) return preferredCpu;
    const bool hasPref = preferredCpu >= 0 && (uint32_t)preferredCpu < ncpu;
    const uint16_t prefRank = hasPref ? tbl.rankOfCpu[(uint32_t)preferredCpu] : 0;
    const uint16_t topRank = tbl.primaryCpu[0] < ncpu ? tbl.rankOfCpu[tbl.primaryCpu[0]] : 0;
    // Keep warm MWAIT C1 threads on top-ranked cores (avoids unnecessary L2 thrash
    // on rapid ping-pong between gold cores), while allowing weaker cores to
    // return to an idle gold core.
    if (hasPref && isShallow((uint32_t)preferredCpu) && prefRank >= topRank)
        return preferredCpu;
    for (uint32_t i = 0; i < tbl.count; ++i) {
        const uint32_t cand = tbl.primaryCpu[i];
        if (cand >= ncpu) continue;
        // Table is sorted by descending rank: once we reach <= prefRank, no better core exists.
        if (hasPref && tbl.rankOfCpu[cand] <= prefRank)
            return preferredCpu;
        if (cand < lo || cand > hi) continue;
        if (!isIdle(cand)) continue;
        const uint32_t sib = tbl.siblingOfCpu[cand];
        if (sib < ncpu && !isIdle(sib)) continue;
        // If preferredCpu is warm in shallow C1, only pull it onto a top-tier gold core.
        if (hasPref && isShallow((uint32_t)preferredCpu) && tbl.rankOfCpu[cand] < topRank)
            return preferredCpu;
        return (int)cand;
    }
    return preferredCpu;
}

// All-cores-in-CC6 accounting. The CPU whose deep entry made every CPU deep stamps
// `start`; the first CPU to leave adds exit - start. A stamp is only trusted if it
// lies inside the leaving CPU's own deep period (it was deep the whole time), which
// rejects a stale stamp from a lost race. Returns the span to add, 0 if rejected.
static inline uint64_t idleAllDeepSpan(uint64_t ownEntry, uint64_t start, uint64_t exit) {
    return (start >= ownEntry && exit > start) ? exit - start : 0;
}

// us * tscHz / 1e6 (us <= 1e6 and tscHz < 2^43 cannot overflow).
uint64_t usToTscTicks(uint32_t us, uint64_t tscHz);

// ------------------------------------------------------------------ telemetry
struct IdleTotals {
    uint64_t deepTicks, shallowTicks;
    uint64_t deepEntries, shallowEntries;
    uint64_t allDeepTicks, allDeepEvents;   // every CPU in CC6 at once (package-C6 window)
    uint64_t latencyVetoes;                 // deep predicted but a latency request forbade CC6
    uint64_t monitorWakes;                  // exitIdle monitor-write wakeups (IPI avoided)
    uint64_t timerMigrates;                 // non-boot timers migrated to CPU 0
};
// sum(ticks) * 1000 / (ncpu * intervalTicks), capped at 1000; 0 if the interval is empty.
uint32_t residencyPermille(uint64_t ticks, uint32_t ncpu, uint64_t intervalTicks);
// delta * 1e9 / intervalNs (0 if intervalNs == 0).
uint64_t ratePerSecond(uint64_t delta, uint64_t intervalNs);

// ------------------------------------------------------------------ governor sampling
// One coherent CPU observation, including its OWN observation time (not a common
// cutoff captured before scanning CPUs). Invalid observations must not be used as
// counters or baselines. entries counts exits; zero new exits is not invalid:
// unchanged idleAcc means busy, while an open idle period keeps idleAcc advancing.
struct IdleSnapshot {
    uint64_t idleAcc;
    uint64_t entries;
    uint64_t tsc;
    bool     valid;
};
enum IdleSampleStatus : int32_t {
    kIdleSampleInvalid = -1,
    kIdleSamplePrimed  = 0,
    kIdleSampleReady   = 1,
};
struct IdleWindow {
    IdleSampleStatus status;
    uint32_t busyPermille;    // meaningful only when status == kIdleSampleReady
};
// Consume exactly one observation. A failed read invalidates the baseline (never
// bridges a missing window). A coherent endpoint after a counter/clock regression
// re-primes it, but that delta is rejected. reset excludes sleep/source changes.
IdleWindow idleWindowAdvance(IdleSnapshot *baseline, const IdleSnapshot &cur, bool reset);
// Integer busy share; malformed/empty windows return 1000, not an idle estimate.
uint32_t idleWindowBusyPermille(uint64_t prevAcc, uint64_t curAcc, uint64_t windowTicks);

// Timer source handoff, shared with host regression tests. perfLastTick means a
// real APERF sample at the preceding DYNAMIC tick, not the last telemetry sample.
// A missing sample clears freshness even if the raw counter buffer is retained.
// Hold does not update governor history or lower a request on unknown input.
enum GovSampleSource : uint32_t { kGovSampleHold = 0, kGovSampleIdle, kGovSamplePerf };
struct GovSamplePlan {
    GovSampleSource source;
    bool samplePerf;
    bool nextPerfFresh;       // caller also verifies the sampling round answered
};
GovSamplePlan planGovernorSample(bool dynamic, bool reset, bool idleReady, bool telemetry,
                                 bool perfLastTick, bool havePerf);

} // namespace zen

#endif // ZEN_IDLE_MODEL_HPP
