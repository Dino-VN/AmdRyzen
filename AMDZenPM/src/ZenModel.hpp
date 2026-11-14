//
// ZenModel.hpp — pure, side-effect-free AMD Zen decode + governor math.
//
// No kernel, IOKit or libc dependencies (only <stdint.h>/<stddef.h>), no
// floating point, no heap. Compiled into the kext AND into tests/ on the host.
// All functions are deterministic; integer math only (kernel has no FP context).
//
#ifndef ZEN_MODEL_HPP
#define ZEN_MODEL_HPP

#include <stdint.h>
#include <stddef.h>
#include "AMDZenShared.h"

namespace zen {

// ------------------------------------------------------------------ identity
enum Vendor : uint32_t { kVendorAMD = 0, kVendorHygon = 1, kVendorOther = 2 };

struct CpuModelInfo {
    uint32_t zenGeneration;     // 1..6 (0 = unknown / pre-Zen)
    const char *codename;       // static string, never null ("Unknown")
    // Temperature
    bool     hasTctl;           // SMN Tctl usable (false for Hygon)
    uint32_t ccdOffset;         // CCD temp base = 0x59800 + ccdOffset (0 = no CCD temps)
    uint32_t maxCcds;           // CCD slots to probe
    int32_t  tctlOffset_mC;     // Tdie = Tctl - offset (brand table, family 17h only)
    bool     dispNegative;      // do not clamp negative Tctl (17h "3255")
    // SVI2 telemetry (0 addresses = unsupported)
    uint32_t sviCoreAddr, sviSocAddr;
    uint32_t sviCoreFactor_uA;  // micro-amps per LSB (e.g. 658823)
    uint32_t sviSocFactor_uA;
    // P-state definition layout
    bool     pstateDef1Ah;      // Zen5 layout (FID[11:0]*5 MHz)
    bool     sviVidFormula;     // VID -> V via 1550 - 6.25*VID (17h/18h/19h SVI2 parts)
    uint32_t defaultTjMax_mC;   // datasheet-ish default (0 = unknown)
};

// family/model from CPUID 1 EAX. brand = CPUID brand string (NUL-terminated, may be "").
// Fills `out` and returns true if the CPU is a supported Zen part (17h,18h Hygon,19h,1Ah).
bool identify(Vendor vendor, uint32_t family, uint32_t model, const char *brand, CpuModelInfo *out);

// CPUID 1 EAX -> display family / model / stepping.
void decodeFms(uint32_t eax, uint32_t *family, uint32_t *model, uint32_t *stepping);

// ------------------------------------------------------------------ temperature
// Tctl in m°C per k10temp: (raw>>21)*125, -49000 if RANGE_SEL or TJ_SEL==3.
int32_t decodeTctl(uint32_t raw);
// Apply k10temp clamp: negative -> 0 unless dispNegative.
int32_t clampTctl(int32_t mC, bool dispNegative);
// CCD temp: returns false if valid bit (11) clear or raw == 0xFFFFFFFF.
bool decodeCcdTemp(uint32_t raw, int32_t *mC);

// ------------------------------------------------------------------ SVI2
// VID (8 bit) -> mV using 1550 - 6.25*VID; returns 0 for VID >= 0xF8 (off).
uint32_t sviVidToMilliVolt(uint32_t vid);
// Telemetry plane -> mV ((raw>>16)&0xFF) and mA ((raw&0xFF)*factor_uA/1000).
uint32_t sviPlaneMilliVolt(uint32_t raw);
uint32_t sviPlaneMilliAmp(uint32_t raw, uint32_t factor_uA);

// ------------------------------------------------------------------ P-states
struct PStateDecoded {
    bool     enabled;
    uint32_t freqMHz;   // 0 if invalid (DID==0, or 1Ah FID<=0x0F)
    uint32_t fid, did, vid;
    uint32_t voltage_mV;// 0 if !sviVidFormula
    uint32_t iddValue, iddDiv;
};
PStateDecoded decodePStateDef(uint64_t raw, bool layout1Ah, bool sviVidFormula);
// Build a raw PStateDef with new FID/DID/VID, preserving all other bits of `raw`.
// Returns false if fields are out of range for the layout (FID>0xFF, DID not in
// 0x08..0x1A or even 0x1C..0x2C, VID>=0xF8, FID<0x10; 1Ah unsupported -> false).
bool encodePStateDef(uint64_t raw, uint32_t fid, uint32_t did, uint32_t vid, uint64_t *out);
// MSR C001_0061 helpers
static inline uint32_t pstateMaxVal(uint64_t curLimit) { return (uint32_t)((curLimit >> 4) & 7); }
static inline uint32_t pstateCurLimit(uint64_t curLimit) { return (uint32_t)(curLimit & 7); }

// ------------------------------------------------------------------ CPPC
struct CppcCaps { uint32_t highest, nominal, lowestNonlinear, lowest; };
CppcCaps decodeCppcCap1(uint64_t raw);
// REQ: [7:0] max [15:8] min [23:16] des [31:24] epp; keeps bits 63:32 of `prev`.
uint64_t encodeCppcReq(uint64_t prev, uint32_t maxPerf, uint32_t minPerf, uint32_t desPerf, uint32_t epp);
// Linear mapping anchored at nominal (amd-pstate.h), rounding up.
uint32_t cppcPerfToFreq(uint32_t perf, uint32_t nominalPerf, uint32_t nominalFreqMHz);
uint32_t cppcFreqToPerf(uint32_t freqMHz, uint32_t nominalPerf, uint32_t nominalFreqMHz,
                        uint32_t lowestPerf, uint32_t highestPerf);

// ------------------------------------------------------------------ RAPL energy
struct EnergyAccumulator {
    uint64_t totalRaw;     // sum of deltas in ESU units (64-bit: never wraps in practice)
    uint32_t lastRaw;      // last 32-bit counter value
    bool     primed;       // false until first sample
};
// Feed a new 32-bit counter value; returns delta in raw units (mod 2^32). First call
// only primes and returns 0. If `rebase` is true (e.g. after wake), primes again.
uint32_t energyUpdate(EnergyAccumulator *acc, uint32_t raw, bool rebase);
// raw units -> micro-joules: raw * 1e6 / 2^esu (no overflow for raw < 2^44).
uint64_t energyRawToMicroJoule(uint64_t raw, uint32_t esu);
// micro-joules over interval -> milli-watts.
uint32_t powerMilliWatt(uint64_t deltaMicroJoule, uint64_t intervalNs);

// ------------------------------------------------------------------ APERF/MPERF
struct PerfCounters { uint64_t aperf, mperf, tsc, irperf; };
struct PerfDerived {
    uint32_t busyPermille;   // dM/dT*1000 (capped 1000)
    uint32_t effFreqMHz;     // tscHz*dA/dM/1e6 (0 if dM==0)
    uint32_t util1024;       // frequency-invariant util vs refFreqMHz: dA*tscHz/(dT*ref) * 1024, capped 1024
    uint32_t ipcX100;        // dIR*100/dA (0 if unavailable)
};
// prev/cur are raw counters (64-bit, wrap-safe by unsigned subtraction).
PerfDerived derivePerf(const PerfCounters &prev, const PerfCounters &cur, uint64_t tscHz,
                       uint32_t refFreqMHz, bool haveIrperf);

// ------------------------------------------------------------------ governors
// P-state table for the legacy backend. Index 0 = fastest (P0). Only entries
// [fastest..slowest] are allowed (policy limits ∩ PstateMaxVal ∩ CurPstateLimit).
struct PStateTable {
    uint32_t count;                       // number of valid entries (P0..Pcount-1)
    uint32_t freqMHz[AMDZEN_MAX_PSTATES]; // strictly decreasing with index for enabled entries
    uint32_t fastest, slowest;            // allowed index range, fastest <= slowest
};

struct GovTunables {
    uint32_t upThreshold;        // %   (ondemand 80, conservative 80)
    uint32_t downThreshold;      // %   (conservative 20)
    uint32_t samplingDownFactor; // ticks to hold before stepping down (>=1)
    uint32_t freqStepPct;        // conservative step % of max
    uint32_t utilHeadroomPct;    // schedutil 125
    uint32_t utilEwmaShift;      // 0 = no smoothing; else decays by (ewma-x)>>shift (rises at once)
    uint32_t fastUpPct;          // schedutil: busy >= this % -> fastest at once (0 = off, default 60)
};
void defaultTunables(GovTunables *t);

// Per-domain (physical core) governor state; zero-initialise.
struct GovState {
    uint32_t curIdx;        // last chosen P-state index (legacy) or perf (CPPC)
    uint32_t holdTicks;     // remaining ticks before a decrease is allowed
    uint32_t utilEwma;      // smoothed util (0..1024)
    uint32_t reqFreqMHz;    // conservative running request
    bool     init;
};

// Lowest-frequency allowed entry with freq >= targetMHz (CPUFREQ_RELATION_L);
// if none, the fastest allowed entry.
uint32_t resolvePState(const PStateTable &t, uint32_t targetMHz);
// Allowed entry closest to targetMHz (ties -> faster).
uint32_t closestPState(const PStateTable &t, uint32_t targetMHz);

// Legacy backend decision. util1024 = max frequency-invariant util of the core's
// threads (reference = P0 freq), busyPermille = max C0 residency of the threads.
// Returns the P-state index to request. userIdx is used for kAMDZenGovUserspace.
// Frequency-invariant util (0..1024, relative to refMHz) of a core that was busy
// busyPermille of the window while running at curMHz (no APERF available).
uint32_t utilFromBusy(uint32_t busyPermille, uint32_t curMHz, uint32_t refMHz);

uint32_t governLegacy(uint32_t governor, const GovTunables &tun, const PStateTable &t,
                      GovState *st, uint32_t util1024, uint32_t busyPermille, uint32_t userIdx);

// CPPC passive decision (schedutil/ondemand/conservative/userspace): desired perf in
// [minPerf, maxPerf]. util1024 is relative to the highest perf.
uint32_t governCppcDesired(uint32_t governor, const GovTunables &tun, uint32_t minPerf,
                           uint32_t maxPerf, GovState *st, uint32_t util1024,
                           uint32_t busyPermille, uint32_t userPerf);

// ------------------------------------------------------------------ misc
// Effective highest perf (amd-pstate): prefcore (distinct highest values) -> 166, else raw.
uint32_t cppcEffectiveHighest(const uint32_t *highestPerCore, uint32_t n, bool *prefcore);
// Substring search (no libc); returns true if needle occurs in hay.
bool containsStr(const char *hay, const char *needle);
// Parse governor name ("performance","powersave","schedutil","ondemand","conservative",
// "userspace","active","none"); returns kAMDZenGovCount if unknown.
uint32_t governorFromName(const char *name);
const char *governorName(uint32_t gov);

} // namespace zen

#endif // ZEN_MODEL_HPP
