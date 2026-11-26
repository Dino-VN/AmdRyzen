// Autonomous CPPC policy. Pure integer code; no kernel headers or hardware I/O.
#ifndef ZEN_NATIVE_POLICY_HPP
#define ZEN_NATIVE_POLICY_HPP
#include "ZenCppcTypes.hpp"
#include "ZenNativeSnapshot.hpp"

namespace zennative {
constexpr uint32_t kMaxCpus = 256;
constexpr uint32_t kMaskWords = kMaxCpus / 64;
enum KnownBool : uint32_t { kUnknown = 0, kOff = 1, kOn = 2 };
enum ThermalLevel : uint32_t {
    kThermalUnknown = 0, kThermalNominal, kThermalModerate, kThermalHeavy,
    kThermalCritical, kThermalEmergency
};
enum PowerSource : uint32_t { kPowerUnknown = 0, kPowerExternal, kPowerInternal };
enum ThermalCapMode : uint32_t {
    kThermalCapOff = 0,    // ignore macOS and Tdie caps (rely on hardware PB2/PROCHOT)
    kThermalCapMacOs = 1,  // follow IOPMrootDomain thermal warnings only
    kThermalCapTdie = 2,   // follow real CPU Tdie vs TjMax only (immune to fake SMBIOS)
    kThermalCapAuto = 3,   // prefer real Tdie vs TjMax when available, else macOS warning
};
enum CpuProfile : uint32_t {
    kProfileAuto = 0,       // auto-selected from CPU family/model/brand/cores
    kProfileDesktop = 1,    // mainstream desktop (65-170W)
    kProfileMobilePerf = 2, // performance laptop APU (H/HS/HX, 35-55W+)
    kProfileMobileEco = 3,  // thin-and-light laptop APU (U/e/low-power, 15-28W)
    kProfileWorkstation = 4,// Threadripper / EPYC / >=24 cores
    kProfileCount = 5,
};
enum PowerEvidence : uint32_t {
    kPowerUnobserved = 0, kPowerRootAggressiveness, kPowerSourceProperty, kPowerPlatformProperty
};
struct SystemSignals {
    KnownBool lowPowerMode;
    ThermalLevel thermal;
    PowerSource powerSource;
    PowerEvidence powerEvidence;
    uint32_t rawThermal;             // IOPM warning value, 255 when unavailable
};

struct Tunables {
    uint32_t baseEpp;                // balanced, default 0x80
    uint32_t realtimeEpp;            // default 0x20 (bounded by baseEpp)
    uint32_t backgroundEpp;          // default 0xc0 (at least baseEpp)
    uint32_t lowPowerEpp;            // default 0xe0
    uint32_t batteryEpp;             // balanced battery baseline, default 0xa0
    uint32_t realtimeMinPct;         // % nominal, default 80; at least lowestNonlinear
    uint32_t rtHoldMs;               // default 100 after last RT observation
    uint32_t foregroundHoldMs;       // default 100 before treating CPU as background-only
    uint32_t relaxHoldMs;            // default 100 of stable less-demanding policy
    uint32_t batchIntervalMs;        // default 50; hard minimum 50 (20 batches/s)
    uint32_t busyBoostThreshold;     // permille (default 450) for automatic EPP/min boost on foreground load
    uint32_t busyBoostEpp;           // default 0 (bounded by baseEpp; realtimeEpp on battery)
    uint32_t rtFloorBusyThreshold;   // permille (default 250) required before RT pins minPerf when busyObserved
};
Tunables defaultTunables();
CpuProfile detectCpuProfile(uint32_t family, uint32_t model, const char *brand, uint32_t physicalCores);
Tunables tunablesForProfile(CpuProfile profile);
uint32_t recommendedIdleThresholdUs(CpuProfile profile);
const char *profileName(CpuProfile profile);
bool profileFromName(const char *name, CpuProfile &out);
bool validCaps(const zencppc::Caps &caps);
ThermalLevel decodeThermalWarning(uint32_t raw);
ThermalLevel thermalFromTdie(int32_t tdie_mC, uint32_t tjMax_mC, ThermalLevel prev);
SystemSignals resolveSystemSignals(const SystemSignals &raw, int32_t acOverride,
                                   int32_t lpmOverride, uint32_t thermalMode,
                                   ThermalLevel tdieLevel);
zencppc::Request decide(const CpuSnapshot &cpu, bool recentRT, bool recentForeground,
                       const SystemSignals &system, const zencppc::Caps &caps,
                       const Tunables &tunables);

// Use the probed coreOfCpu[]/_PSD mapping, NEVER assume adjacent OS CPU numbers
// are SMT siblings. Each domain has one or two members and shares one request.
struct Topology {
    uint32_t cpuCount;
    uint16_t domainOfCpu[kMaxCpus];
};
struct CpuHistory {
    uint32_t rtEvents, foregroundEvents;
    uint64_t lastRtMs, lastForegroundMs;
    bool seen, hasRt, hasForeground;
};
struct DomainHistory {
    zencppc::Request request, pending;
    uint64_t pendingSinceMs;
    bool valid, pendingValid;
};
struct PolicyState {
    CpuHistory cpu[kMaxCpus];
    DomainHistory domain[kMaxCpus];
    uint64_t lastTickMs, lastBatchMs;
    bool hasTick, hasBatch;
};
struct TickResult {
    zencppc::Request requests[kMaxCpus];
    uint64_t changed[kMaskWords], valid[kMaskWords];
    uint32_t cpuCount, changedCount;
};
inline bool cpuChanged(const TickResult &r, uint32_t cpu) {
    return cpu < r.cpuCount && (r.changed[cpu / 64] & (1ull << (cpu % 64))) != 0;
}
inline bool cpuValid(const TickResult &r, uint32_t cpu) {
    return cpu < r.cpuCount && (r.valid[cpu / 64] & (1ull << (cpu % 64))) != 0;
}

// Serialize all calls on one thread call/work loop. State/result should be
// long-lived member/allocated storage, not large kernel stack temporaries.
// nowMs is a monotonic elapsed clock (not TSC ticks or absolute-time units).
// Returns false on invalid arguments/topology. Bad/missing caps suppress that
// entire domain (valid bits clear). Changed CPUs include BOTH SMT siblings.
bool tick(PolicyState &state, const Topology &topology, const CpuSnapshot *cpus,
          const SystemSignals &system, const zencppc::Caps *caps,
          const Tunables &tunables, uint64_t nowMs, TickResult &result);
void reset(PolicyState &state);             // start/wake/topology change, serialized
void invalidateRequests(PolicyState &state); // transport failure: retry respecting batch limit

} // namespace zennative
#endif
