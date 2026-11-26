//
// AMDZenPM.cpp — IOService that owns the CPU power-management hardware, runs the
// in-kernel governor loop and publishes telemetry.
//
// MSR control and telemetry run on the IOWorkLoop/IOCommandGate. PCC firmware
// waits and native signal polling run on a separately reserved thread call;
// its inputs/results cross the gate, without holding it over PCC waits.
//
#include "AMDZenPM.hpp"
#include "ZenRegs.h"
#include "ZenIdle.hpp"
#include "ZenNativePolicy.hpp"
#include "ZenPccModel.hpp"
#include "XnuPrivate.h"

#include <IOKit/IOLib.h>
#include <IOKit/IOMessage.h>
#include <IOKit/pwr_mgt/RootDomain.h>
#include <libkern/libkern.h>
#include <libkern/version.h>
#include <libkern/c++/OSSymbol.h>
#include <pexpert/pexpert.h>
#include <kern/clock.h>
#include <kern/task.h>
#include <string.h>

#define AMDZEN_KEXT_VERSION "0.0.2"
#define LOG(fmt, ...) IOLog("AMDZenPM: " fmt "\n", ##__VA_ARGS__)

OSDefineMetaClassAndStructors(AMDZenPM, IOService)

// ---------------------------------------------------------------------------
// File-local state. The driver is a singleton (IOResources match, one
// IOMatchCategory), so a few flags that are not part of the class layout live here.
// All are plain zero-initialised PODs (no static constructors).
// ---------------------------------------------------------------------------
namespace {

enum GateOp : uint32_t {
    kGateCopyInfo = 1,
    kGateCopySample,
    kGateCopyTunables,
    kGateSetTunables,
    kGateSetGovernor,
    kGateSetBoostMode,   // a1 = (intptr_t)-1 / 0 / 1
    kGateSetLimits,
    kGateSetEpp,
    kGateSetUser,
    kGateWriteDef,       // a1 = idx, a2 = fid | did << 16, a3 = vid
    kGateConflict,       // a1 = class name (const char *)
    kGateApply,          // a1 = force
    kGateSleep,
    kGateWake,
    kGateStop,
    kGateSetIdleMode,      // a1 = IdleMode
    kGateSetIdleThreshold, // a1 = microseconds
    kGateSetGovKnob,       // a1 = GovKnob, a2 = value
    kGateSetLpm,           // a1 = (intptr_t)-1 / 0 / 1
    kGateSetAcPower,       // a1 = (intptr_t)-1 / 0 / 1
    kGateSetThermalCap,    // a1 = ThermalCapMode (0..3)
    kGateSetProfile,       // a1 = CpuProfile (0..4)
};

const char *const kConflictClasses[] = {
    "AMDRyzenCPUPowerManagement",   // wtf.spinach.AMDRyzenCPUPowerManagement
    "AMDRyzenCPUPMUserClient",      // its user client (implies the owner is up)
    "SMCAMDProcessor",              // VirtualSMC plugin bundled with it
    "AMDCPUSupport",
};
constexpr uint32_t kNumConflictClasses = sizeof(kConflictClasses) / sizeof(kConflictClasses[0]);

// Extra classes that only matter for the idle hook: any of them may own pmDispatch,
// and pmKextRegister panics when a table is already registered.
const char *const kIdleConflictClasses[] = {
    "AppleIntelCPUPowerManagement",
};

// gInstance is read by the exported C API and sysctl handlers on arbitrary threads;
// gInstanceLock (allocated once, never freed: the C API can be entered concurrently
// with or after stop()) serialises taking a reference against stop() clearing it.
AMDZenPM   *gInstance;
IOLock     *gInstanceLock;
bool        gCppcEnabled;     // CPPC_ENABLE written by us (write-once until reset)
bool        gControlTouched;  // we wrote at least one control MSR since last restore
bool        gIrperfByUs;      // we set HWCR.IRPerfEn at start (re-apply after S3)
bool        gBoostTouched;    // we modified HWCR.CpbDis
IONotifier *gConflictNotifiers[kNumConflictClasses];

constexpr uint32_t kMaxPlausiblePower_mW = 1000u * 1000u; // 1000 W: reject bogus deltas

inline uint64_t uptimeNs() {
    uint64_t abs = 0, ns = 0;
    clock_get_uptime(&abs);
    absolutetime_to_nanoseconds(abs, &ns);
    return ns;
}

inline uint32_t minU(uint32_t a, uint32_t b) { return a < b ? a : b; }
inline uint32_t maxU(uint32_t a, uint32_t b) { return a > b ? a : b; }
inline uint32_t clampU(uint32_t v, uint32_t lo, uint32_t hi) { return v < lo ? lo : (v > hi ? hi : v); }

inline bool isDynamicGov(uint32_t g) {
    return g == kAMDZenGovSchedutil || g == kAMDZenGovOndemand || g == kAMDZenGovConservative;
}

uint32_t autoGovernor(uint32_t backend) {
    switch (backend) {
    case kAMDZenBackendCppcMsr: return kAMDZenGovCppcActive;
    case kAMDZenBackendCppcPcc: return kAMDZenGovCppcActive;
    case kAMDZenBackendLegacy:  return kAMDZenGovSchedutil;
    default:                    return kAMDZenGovNone;
    }
}

const char *backendName(uint32_t b) {
    switch (b) {
    case kAMDZenBackendLegacy:  return "legacy";
    case kAMDZenBackendCppcMsr: return "cppc-msr";
    case kAMDZenBackendCppcPcc: return "cppc-pcc";
    default:                    return "none";
    }
}

const char *blockReasonName(uint32_t r) {
    switch (r) {
    case kAMDZenBlockNone:        return "none";
    case kAMDZenBlockBootArgRO:   return "readonly";
    case kAMDZenBlockConflict:    return "conflict";
    case kAMDZenBlockUnsupported: return "unsupported";
    case kAMDZenBlockHypervisor:  return "hypervisor";
    case kAMDZenBlockPccFailed:   return "pcc-failed";
    default:                      return "unknown";
    }
}

// Highest legacy P-state index usable (PstateMaxVal bounded by the decoded table).
uint32_t legacyMaxIndex(const AMDZenInfo *in) {
    uint32_t n = minU(in->numPStates, AMDZEN_MAX_PSTATES);
    if (n == 0) return 0;
    return minU(in->pstateMaxVal, n - 1);
}

// Move `idx` towards P0 until it names an enabled P-state with a known frequency.
uint32_t enabledAtOrFaster(const AMDZenInfo *in, uint32_t idx) {
    while (idx > 0 && !(in->pstates[idx].enabled && in->pstates[idx].freqMHz)) idx--;
    return idx;
}

// CPPC request derived from the policy (amd-pstate semantics, linux-pstate.md §3.6).
struct CppcPolicy { uint32_t minPerf, maxPerf, desPerf, epp; };

// biosMin = boot CPPC_REQ[15:8] ("Requested CPU Min Frequency" from firmware), 0 = none.
CppcPolicy cppcPolicy(const AMDZenInfo *in, uint32_t gov, bool boostOn, uint32_t limMin,
                      uint32_t limMax, uint32_t cfgEpp, uint32_t userPerf, uint32_t biosMin) {
    uint32_t lowest  = in->cppcLowest;
    uint32_t nominal = maxU(in->cppcNominal, lowest);
    // Effective highest (166 on prefcore parts, linux-pstate.md §3.4) is also what
    // amd-pstate writes as max_perf; the raw CAP1 value is only a per-core ranking.
    uint32_t highest = in->cppcEffectiveHighest ? in->cppcEffectiveHighest : in->cppcHighest;
    highest = maxU(highest, nominal);
    uint32_t lnl = in->cppcLowestNonlinear;
    if (lnl < lowest || lnl > nominal) lnl = lowest;
    uint32_t cap = boostOn ? highest : nominal;

    // amd-pstate: default min = bios_min_perf ?: lowest_nonlinear; PERFORMANCE min =
    // bios_min_perf ?: nominal (never highest, to avoid throttling power-limited parts).
    const uint32_t defMin = biosMin ? clampU(biosMin, lowest, cap) : lnl;
    CppcPolicy p { defMin, cap, 0, 0 };
    switch (gov) {
    case kAMDZenGovPerformance:            // EPP 0
        p.minPerf = biosMin ? clampU(biosMin, lowest, cap) : nominal;
        break;
    case kAMDZenGovPowersave:
        p.minPerf = lowest;
        p.maxPerf = lnl;
        p.epp = EPP_POWER;
        break;
    case kAMDZenGovCppcActive:             // default min = lowest_nonlinear, firmware picks
        p.epp = cfgEpp & 0xFF;
        break;
    case kAMDZenGovUserspace:
        p.desPerf = userPerf;
        break;
    default:                               // passive governors: des set per tick, EPP 0
        break;
    }
    if (limMax) p.maxPerf = clampU(limMax, lowest, cap);
    if (limMin) p.minPerf = clampU(limMin, lowest, cap);
    if (p.minPerf > p.maxPerf) p.minPerf = p.maxPerf;
    if (p.desPerf) p.desPerf = clampU(p.desPerf, p.minPerf, p.maxPerf);
    return p;
}

// ---------------- OSDictionary helpers ----------------
void dictNum(OSDictionary *d, const char *key, uint64_t v, uint32_t bits = 32) {
    if (OSNumber *n = OSNumber::withNumber((unsigned long long)v, bits)) {
        d->setObject(key, n);
        n->release();
    }
}
void dictStr(OSDictionary *d, const char *key, const char *s) {
    if (OSString *o = OSString::withCString(s ? s : "")) {
        d->setObject(key, o);
        o->release();
    }
}
void dictBool(OSDictionary *d, const char *key, bool b) {
    d->setObject(key, b ? kOSBooleanTrue : kOSBooleanFalse);
}
void arrNum(OSArray *a, uint64_t v, uint32_t bits = 32) {
    if (OSNumber *n = OSNumber::withNumber((unsigned long long)v, bits)) {
        a->setObject(n);
        n->release();
    }
}
void dictArr(OSDictionary *d, const char *key, OSArray *a) {
    if (a) {
        d->setObject(key, a);
        a->release();
    }
}

// Personality / setProperties value readers
bool readU32(OSObject *o, uint32_t *out) {
    if (OSNumber *n = OSDynamicCast(OSNumber, o)) {
        uint64_t v = n->unsigned64BitValue();
        if (v > 0xFFFFFFFFull) return false;
        *out = (uint32_t)v;
        return true;
    }
    return false;
}
// MinPState / MaxPState: -1 (or anything > 7) = no limit
bool readIndexLimit(OSObject *o, int32_t *out) {
    if (OSNumber *n = OSDynamicCast(OSNumber, o)) {
        int64_t v = (int64_t)n->unsigned64BitValue();
        if (n->numberOfBits() == 32) v = (int32_t)n->unsigned32BitValue();
        *out = (v < 0 || v >= (int64_t)AMDZEN_MAX_PSTATES) ? -1 : (int32_t)v;
        return true;
    }
    return false;
}
// "keep" | "on" | "off" | bool | number(-1/0/1) -> -1 / 1 / 0 ; returns false if unparsable
bool readBoostMode(OSObject *o, int32_t *out) {
    if (OSString *s = OSDynamicCast(OSString, o)) {
        if (s->isEqualTo("keep") || s->isEqualTo("auto") || s->isEqualTo("default")) { *out = -1; return true; }
        if (s->isEqualTo("on") || s->isEqualTo("1") || s->isEqualTo("true"))  { *out = 1; return true; }
        if (s->isEqualTo("off") || s->isEqualTo("0") || s->isEqualTo("false")) { *out = 0; return true; }
        return false;
    }
    if (OSBoolean *b = OSDynamicCast(OSBoolean, o)) { *out = b->isTrue() ? 1 : 0; return true; }
    if (OSNumber *n = OSDynamicCast(OSNumber, o)) {
        int32_t v = (int32_t)n->unsigned32BitValue();
        if (v < -1 || v > 1) return false;
        *out = v;
        return true;
    }
    return false;
}

} // namespace

#define ZALLOC(T, n)  static_cast<T *>(IOMallocZero(sizeof(T) * (n)))
#define ZFREE(p, T, n) do { if (p) { IOFree((p), sizeof(T) * (n)); (p) = nullptr; } } while (0)

// ===========================================================================
// Lifecycle
// ===========================================================================
AMDZenPM *AMDZenPM::copyInstance() {
    IOLock *l = __atomic_load_n(&gInstanceLock, __ATOMIC_ACQUIRE);
    if (!l) return nullptr;
    IOLockLock(l);
    AMDZenPM *p = gInstance;
    if (p) p->retain();
    IOLockUnlock(l);
    return p;
}

bool AMDZenPM::init(OSDictionary *dict) {
    if (!IOService::init(dict)) return false;
    return true;
}

void AMDZenPM::free() {
    nativeFree();
    OSSafeReleaseNULL(timer);
    OSSafeReleaseNULL(gate);
    OSSafeReleaseNULL(workLoop);
    ZFREE(info, AMDZenInfo, 1);
    ZFREE(sample, AMDZenSample, 1);
    ZFREE(rawPrev, zen::PerCpuRaw, AMDZEN_MAX_CPUS);
    ZFREE(rawCur, zen::PerCpuRaw, AMDZEN_MAX_CPUS);
    ZFREE(rawTelemPrev, zen::PerCpuRaw, AMDZEN_MAX_CPUS);
    ZFREE(idleSnapPrev, zen::IdleSnapshot, AMDZEN_MAX_CPUS);
    ZFREE(idleSnapCur, zen::IdleSnapshot, AMDZEN_MAX_CPUS);
    ZFREE(govState, zen::GovState, AMDZEN_MAX_CORES);
    ZFREE(coreEnergy, zen::EnergyAccumulator, AMDZEN_MAX_CORES);
    ZFREE(pstateReq, uint8_t, AMDZEN_MAX_CPUS);
    ZFREE(cppcReq, uint64_t, AMDZEN_MAX_CPUS);
    smn.free();
    IOService::free();
}

IOService *AMDZenPM::probe(IOService *provider, SInt32 *score) {
    if (!IOService::probe(provider, score)) return nullptr;

    int flag = 0;
    if (PE_parse_boot_argn("-amdzenoff", &flag, sizeof(flag))) {
        LOG("disabled by -amdzenoff");
        return nullptr;
    }
    uint32_t r[4];
    zen_cpuid(0, 0, r);
    char vendor[13];
    memcpy(vendor + 0, &r[1], 4);
    memcpy(vendor + 4, &r[3], 4);
    memcpy(vendor + 8, &r[2], 4);
    vendor[12] = '\0';
    if (strcmp(vendor, "AuthenticAMD") != 0 && strcmp(vendor, "HygonGenuine") != 0)
        return nullptr;

    zen_cpuid(1, 0, r);
    uint32_t fam = 0, mod = 0, step = 0;
    zen::decodeFms(r[0], &fam, &mod, &step);
    if (fam < 0x17) {
        LOG("family %02xh is not Zen, not starting", fam);
        return nullptr;
    }
    return this;
}

void AMDZenPM::loadConfig() {
    zen::defaultTunables(&tun);
    governor = kAMDZenGovCount;     // "auto" until chooseBackend()
    boostMode = -1;

    // ---- personality (lower priority)
    if (OSString *s = OSDynamicCast(OSString, getProperty("Governor"))) {
        if (!s->isEqualTo("auto")) {
            uint32_t g = zen::governorFromName(s->getCStringNoCopy());
            if (g < kAMDZenGovCount) governor = g;
            else LOG("unknown Governor '%s', using auto", s->getCStringNoCopy());
        }
    }
    int32_t bm;
    if (OSObject *o = getProperty("Boost")) {
        if (readBoostMode(o, &bm)) boostMode = bm;
    }
    uint32_t v;
    if (readU32(getProperty("EPP"), &v) && v <= 255) {
        epp = v;
        if (v != 128) eppExplicit = true;
    }
    if (readU32(getProperty("GovernorPeriodMs"), &v))   governorPeriodMs = clampU(v, 4, 1000);
    if (readU32(getProperty("TelemetryPeriodMs"), &v))  telemetryPeriodMs = clampU(v, 100, 10000);
    if (readU32(getProperty("UpThreshold"), &v))        tun.upThreshold = clampU(v, 1, 100);
    if (readU32(getProperty("DownThreshold"), &v))      tun.downThreshold = minU(v, 99);
    if (readU32(getProperty("SamplingDownFactor"), &v)) tun.samplingDownFactor = clampU(v, 1, 100);
    if (readU32(getProperty("FreqStepPct"), &v))        tun.freqStepPct = minU(v, 100);
    if (readU32(getProperty("UtilHeadroomPct"), &v))    tun.utilHeadroomPct = clampU(v, 100, 200);
    if (readU32(getProperty("UtilEwmaShift"), &v))      tun.utilEwmaShift = minU(v, 8);
    if (readU32(getProperty("FastUpPct"), &v))          tun.fastUpPct = minU(v, 100);
    if (readU32(getProperty("GovIdleSampling"), &v))    govIdleSampling = v ? 1 : 0;
    else if (OSBoolean *b = OSDynamicCast(OSBoolean, getProperty("GovIdleSampling"))) govIdleSampling = b->isTrue() ? 1 : 0;
    if (tun.downThreshold >= tun.upThreshold) tun.downThreshold = tun.upThreshold ? tun.upThreshold - 1 : 0;
    if (readU32(getProperty("TjMax"), &v) && v >= 50 && v <= 150) tjMax_mC = v * 1000;
    int32_t lim;
    if (readIndexLimit(getProperty("MinPState"), &lim)) limitFast = lim < 0 ? 0 : (uint32_t)lim;
    if (readIndexLimit(getProperty("MaxPState"), &lim)) limitSlow = lim < 0 ? AMDZEN_MAX_PSTATES - 1 : (uint32_t)lim;
    if (limitFast > limitSlow) limitFast = limitSlow;
    if (OSObject *o = getProperty("IdleMode")) {
        uint32_t m = zen::kIdleAuto;
        bool ok = false;
        if (OSString *s = OSDynamicCast(OSString, o)) ok = zen::idleModeFromString(s->getCStringNoCopy(), &m);
        else if (readU32(o, &m)) ok = m < zen::kIdleModeCount;
        if (ok) idleRequest = m;
        else LOG("invalid IdleMode ignored (auto|0|1|2|3)");
    }
    if (readU32(getProperty("IdleThresholdUs"), &v)) {
        idleThresholdUs = minU(v, zen::kIdleThresholdMaxUs);
        if (idleThresholdUs != zen::kIdleThresholdDefaultUs) idleThresholdExplicit = true;
    }
    if (OSNumber *n = OSDynamicCast(OSNumber, getProperty("ACPower"))) {
        int32_t m = (int32_t)n->unsigned32BitValue();
        if (m >= -1 && m <= 1) acPowerMode = m;
    }
    if (OSNumber *n = OSDynamicCast(OSNumber, getProperty("LowPowerMode"))) {
        int32_t m = (int32_t)n->unsigned32BitValue();
        if (m >= -1 && m <= 1) lpmMode = m;
    }
    if (readU32(getProperty("ThermalCap"), &v) && v <= zennative::kThermalCapAuto)
        thermalCapMode = v;
    if (OSString *s = OSDynamicCast(OSString, getProperty("Profile"))) {
        zennative::CpuProfile p = zennative::kProfileAuto;
        if (zennative::profileFromName(s->getCStringNoCopy(), p)) profileRequested = (uint32_t)p;
        else LOG("unknown Profile '%s', using auto", s->getCStringNoCopy());
    }

    // ---- boot-args (highest priority)
    int flag = 0;
    if (PE_parse_boot_argn("-amdzenro", &flag, sizeof(flag))) blockReason = kAMDZenBlockBootArgRO;
    if (PE_parse_boot_argn("-amdzennocppc", &flag, sizeof(flag))) cppcPccOff = true;

    char buf[32] = {};
    if (PE_parse_boot_argn("amdzen.gov", buf, sizeof(buf) - 1)) {
        buf[sizeof(buf) - 1] = '\0';
        if (strcmp(buf, "auto") == 0) {
            governor = kAMDZenGovCount;
        } else {
            uint32_t g = zen::governorFromName(buf);
            if (g < kAMDZenGovCount) governor = g;
            else LOG("unknown amdzen.gov='%s' ignored", buf);
        }
    }
    char pb[32] = {};
    if (PE_parse_boot_arg_str("amdzen.profile", pb, sizeof(pb))) {
        pb[sizeof(pb) - 1] = '\0';
        zennative::CpuProfile p = zennative::kProfileAuto;
        if (zennative::profileFromName(pb, p)) profileRequested = (uint32_t)p;
        else LOG("unknown amdzen.profile='%s' ignored", pb);
    }
    int32_t iv = 0;
    if (PE_parse_boot_argn("amdzen.boost", &iv, sizeof(iv))) boostMode = iv ? 1 : 0;
    if (PE_parse_boot_argn("amdzen.epp", &iv, sizeof(iv)) && iv >= 0 && iv <= 255) {
        epp = (uint32_t)iv;
        eppExplicit = true;
    }
    iv = -2;
    if (PE_parse_boot_argn("amdzen.ac", &iv, sizeof(iv)) && iv >= -1 && iv <= 1) acPowerMode = iv;
    iv = -2;
    if (PE_parse_boot_argn("amdzen.lpm", &iv, sizeof(iv)) && iv >= -1 && iv <= 1) lpmMode = iv;
    iv = -1;
    if (PE_parse_boot_argn("amdzen.thermal", &iv, sizeof(iv)) && iv >= 0 && iv <= (int32_t)zennative::kThermalCapAuto)
        thermalCapMode = (uint32_t)iv;
    iv = 0;
    if (PE_parse_boot_argn("amdzen.period", &iv, sizeof(iv)) && iv > 0) governorPeriodMs = clampU((uint32_t)iv, 4, 1000);
    iv = -1;
    if (PE_parse_boot_argn("amdzen.fastup", &iv, sizeof(iv)) && iv >= 0) tun.fastUpPct = minU((uint32_t)iv, 100);
    iv = -1;
    if (PE_parse_boot_argn("amdzen.govidle", &iv, sizeof(iv)) && iv >= 0) govIdleSampling = iv ? 1 : 0;
    iv = 0;
    if (PE_parse_boot_argn("amdzen.force", &iv, sizeof(iv)) && iv) forceControl = true;
    iv = 0;
    if (PE_parse_boot_argn("amdzen.pstatewrite", &iv, sizeof(iv)) && iv) allowPStateWrite = true;
    iv = 0;
    if (PE_parse_boot_argn("amdzen.smn", &iv, sizeof(iv)) && iv) allowSmnShared = true;
    // amdzen.idle takes "auto" or a digit. PE_parse_boot_arg_str keeps the value a
    // string (PE_parse_boot_argn would store a numeric value as a binary int).
    char ib[8] = {};
    if (PE_parse_boot_arg_str("amdzen.idle", ib, sizeof(ib))) {
        ib[sizeof(ib) - 1] = '\0';
        uint32_t m = zen::kIdleAuto;
        if (zen::idleModeFromString(ib, &m)) idleRequest = m;
        else LOG("invalid amdzen.idle='%s' ignored (auto|0|1|2|3)", ib);
    }
}

bool AMDZenPM::detectConflicts() {
    for (uint32_t i = 0; i < kNumConflictClasses; i++) {
        const char *name = kConflictClasses[i];
        bool found = false;
        if (OSDictionary *m = IOService::serviceMatching(name)) {
            if (OSIterator *it = IOService::getMatchingServices(m)) {
                if (it->getNextObject()) found = true;
                it->release();
            }
            m->release();
        }
        if (!found) {
            // Class loaded with live instances but not (yet) registered.
            if (const OSSymbol *sym = OSSymbol::withCString(name)) {
                const OSMetaClass *mc = OSMetaClass::getMetaClassWithName(sym);
                if (mc && mc->getInstanceCount() > 0) found = true;
                sym->release();
            }
        }
        if (found) {
            LOG("conflicting power-management driver present: %s", name);
            return true;
        }
    }
    return false;
}

void AMDZenPM::chooseBackend() {
    const uint64_t f = info->features;
    if (nativeEnableMayBe()) {
        backend = kAMDZenBackendCppcPcc;
        if (!blockReason) blockReason = kAMDZenBlockPccFailed;
        nativeDiag.stage = 3; // degraded: replacement owns no live Channel
        if (!nativeDiag.error) nativeDiag.error = uint32_t(zenpcc::Error::SubspaceClaimed);
    } else if (!cppcPccOff && (f & AMDZEN_F_CPPC) && info->cppcHighest && info->cppcNominal &&
        info->cppcLowest && info->cppcLowest <= info->cppcNominal) {
        backend = kAMDZenBackendCppcMsr;
    } else if ((f & AMDZEN_F_HWPSTATE) && info->numPStates > 0) {
        backend = kAMDZenBackendLegacy;
    } else {
        backend = kAMDZenBackendNone;
    }
    if (backend == kAMDZenBackendNone && blockReason == kAMDZenBlockNone)
        blockReason = kAMDZenBlockUnsupported;

    if (governor == kAMDZenGovCount) governor = autoGovernor(backend);
    if (governor == kAMDZenGovCppcActive && backend != kAMDZenBackendCppcMsr &&
        backend != kAMDZenBackendCppcPcc) {
        LOG("governor 'active' needs CPPC MSRs; using schedutil");
        governor = kAMDZenGovSchedutil;
    }
    if (backend == kAMDZenBackendCppcMsr || backend == kAMDZenBackendCppcPcc) {
        // MinPState/MaxPState are legacy index limits; CPPC limits default to 0 (= none).
        limitFast = 0;
        limitSlow = 0;
        userValue = info->cppcNominal;
    } else {
        uint32_t maxIdx = legacyMaxIndex(info);
        limitSlow = minU(limitSlow, maxIdx);
        limitFast = minU(limitFast, limitSlow);
        userValue = 0;
    }
    info->backend = backend;
    info->controlBlockReason = blockReason;
    // Hold legacy writes until a command-free discovery/capability/state/policy
    // check and the optional enable attempt finish on the PCC worker.
    nativePending = nativeCall && !cppcPccOff && !(f & AMDZEN_F_CPPC) &&
        backend == kAMDZenBackendLegacy && !blockReason && governor != kAMDZenGovNone &&
        cpu.topo().ncpu <= 64 && cpu.canPrepareAutonomous() && sleepNotifier;
}

bool AMDZenPM::start(IOService *provider) {
    if (!IOService::start(provider)) return false;

    auto bail = [&](const char *why) -> bool {
        LOG("start failed: %s", why);
        if (workLoop) {
            if (timer) { timer->cancelTimeout(); workLoop->removeEventSource(timer); }
            if (gate) workLoop->removeEventSource(gate);
        }
        IOService::stop(provider);
        return false;
    };

    loadConfig();
    nativeInit(); // allocation failure permits legacy only when the boot latch is clear

    info         = ZALLOC(AMDZenInfo, 1);
    sample       = ZALLOC(AMDZenSample, 1);
    rawPrev      = ZALLOC(zen::PerCpuRaw, AMDZEN_MAX_CPUS);
    rawCur       = ZALLOC(zen::PerCpuRaw, AMDZEN_MAX_CPUS);
    rawTelemPrev = ZALLOC(zen::PerCpuRaw, AMDZEN_MAX_CPUS);
    idleSnapPrev = ZALLOC(zen::IdleSnapshot, AMDZEN_MAX_CPUS);
    idleSnapCur  = ZALLOC(zen::IdleSnapshot, AMDZEN_MAX_CPUS);
    govState     = ZALLOC(zen::GovState, AMDZEN_MAX_CORES);
    coreEnergy   = ZALLOC(zen::EnergyAccumulator, AMDZEN_MAX_CORES);
    pstateReq    = ZALLOC(uint8_t, AMDZEN_MAX_CPUS);
    cppcReq      = ZALLOC(uint64_t, AMDZEN_MAX_CPUS);
    if (!info || !sample || !rawPrev || !rawCur || !rawTelemPrev || !govState || !coreEnergy ||
        !pstateReq || !cppcReq || !idleSnapPrev || !idleSnapCur)
        return bail("out of memory");

    // ---- work loop, gate, timer. Hardware discovery is deferred to the work loop:
    // this kext matches IOResources very early, possibly before every application
    // processor is registered/running and before PCI 00:00.0 is published, and
    // blocking start() would also stall other IOResources clients.
    workLoop = IOWorkLoop::workLoop();
    if (!workLoop) return bail("no work loop");
    gate = IOCommandGate::commandGate(this);
    timer = IOTimerEventSource::timerEventSource(
        this, OSMemberFunctionCast(IOTimerEventSource::Action, this, &AMDZenPM::timerFired));
    if (!gate || !timer) return bail("no event sources");
    if (workLoop->addEventSource(gate) != kIOReturnSuccess) { OSSafeReleaseNULL(gate); return bail("gate"); }
    if (workLoop->addEventSource(timer) != kIOReturnSuccess) { OSSafeReleaseNULL(timer); return bail("timer"); }

    sleepNotifier = registerPrioritySleepWakeInterest(&AMDZenPM::sleepWakeHandler, this, nullptr);
    if (!sleepNotifier) LOG("warning: no sleep/wake notifications");

    initStartNs = uptimeNs();
    if (!__atomic_load_n(&gInstanceLock, __ATOMIC_ACQUIRE)) {
        IOLock *l = IOLockAlloc();
        if (!l) return bail("no lock");
        IOLock *expected = nullptr;
        if (!__atomic_compare_exchange_n(&gInstanceLock, &expected, l, false, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE))
            IOLockFree(l);
    }
    IOLockLock(gInstanceLock);
    gInstance = this;
    IOLockUnlock(gInstanceLock);
    registerSysctls();
    registerService();
    timer->setTimeoutMS(1);
    return true;
}

namespace {
// Logical CPUs the package reports (CPUID 0xB level 1, else 8000_0008 ECX[7:0]+1).
uint32_t expectedLogicalCpus() {
    uint32_t r[4];
    zen_cpuid(0, 0, r);
    if (r[0] >= 0xB) {
        zen_cpuid(0xB, 1, r);
        if (((r[2] >> 8) & 0xFF) == 2 && (r[1] & 0xFFFF)) return r[1] & 0xFFFF;
    }
    zen_cpuid(0x80000000u, 0, r);
    if (r[0] >= 0x80000008u) {
        zen_cpuid(0x80000008u, 0, r);
        return (r[2] & 0xFF) + 1;
    }
    return 1;
}
constexpr uint64_t kInitTimeoutNs = 30ull * 1000ull * 1000ull * 1000ull;
} // namespace

void AMDZenPM::probeSmnSensors() {
    info->features &= ~(uint64_t)(AMDZEN_F_SMN | AMDZEN_F_TCTL | AMDZEN_F_CCD_TEMP | AMDZEN_F_SVI2);
    info->ccdTempMask = 0;
    if (!smn.available()) return;
    if (blockReason == kAMDZenBlockConflict && !allowSmnShared) {
        // The other driver drives the same index/data pair without our lock: interleaved
        // accesses corrupt both its readings (fan control!) and ours.
        LOG("SMN sensors disabled while a conflicting driver is loaded (amdzen.smn=1 overrides)");
        return;
    }
    info->features |= AMDZEN_F_SMN;
    uint32_t raw = 0;
    if (model.hasTctl && smn.read(SMN_THM_TCON_CUR_TMP, &raw)) info->features |= AMDZEN_F_TCTL;
    uint32_t ccdMask = 0;
    if (model.hasTctl && model.ccdOffset) {
        uint32_t n = minU(model.maxCcds, AMDZEN_MAX_CCDS);
        for (uint32_t i = 0; i < n; i++) {
            int32_t mC;
            if (smn.read(SMN_THM_TCON_CUR_TMP + model.ccdOffset + 4 * i, &raw) && zen::decodeCcdTemp(raw, &mC))
                ccdMask |= 1u << i;
        }
    }
    if (ccdMask) info->features |= AMDZEN_F_CCD_TEMP;
    info->ccdTempMask = ccdMask;

    // SVI2: on multi-node 17h model 01h/08h (Threadripper 1/2, Naples) node 0's plane A
    // is VDDSOC and there is no core plane on node 0 (linux-sensors.md §4.3).
    if (info->family == 0x17 && (info->model == 0x01 || info->model == 0x08) && info->nodes > 1) {
        model.sviCoreAddr = 0;
        model.sviSocAddr = SMN_SVI_PLANE_A;
    }
    // SVI0_TFN: bit0 = plane0 (core) telemetry disabled, bit1 = plane1 (SoC) disabled.
    // Only TFN removes a plane for good. A plane that reads VID 0 / out of range at
    // probe time is kept: telemetry may not be running yet this early in boot, and
    // every sample is range-checked anyway (an invalid reading publishes 0).
    uint32_t tfn = 0;
    if (!smn.read(SMN_SVI_TFN, &tfn)) tfn = 0;
    sviTfn = tfn;
    sviCoreProbeRaw = sviSocProbeRaw = 0;
    if (model.sviCoreAddr && !smn.read(model.sviCoreAddr, &sviCoreProbeRaw)) sviCoreProbeRaw = 0xFFFFFFFFu;
    if (model.sviSocAddr && !smn.read(model.sviSocAddr, &sviSocProbeRaw)) sviSocProbeRaw = 0xFFFFFFFFu;
    if (model.sviCoreAddr && ((tfn & 1u) || sviCoreProbeRaw == 0xFFFFFFFFu)) model.sviCoreAddr = 0;
    if (model.sviSocAddr && ((tfn & 2u) || sviSocProbeRaw == 0xFFFFFFFFu)) model.sviSocAddr = 0;
    if (model.sviCoreAddr || model.sviSocAddr) info->features |= AMDZEN_F_SVI2;
}

bool AMDZenPM::tryInitHardware() {
    const bool timedOut = uptimeNs() - initStartNs > kInitTimeoutNs;
    const uint32_t expected = expectedLogicalCpus();
    if (!timedOut && real_ncpus < expected) return false;       // APs not registered yet
    if (!smn.available()) smn.init(0);
    if (!smn.available() && !timedOut) return false;            // PCI root not published yet

    if (!cpu.discover(info, &model)) {
        LOG("CPU discovery failed (unsupported CPU); staying passive");
        initFailed = true;
        return true;
    }
    if (cpu.missingCpus() && !timedOut) return false;           // some CPU not running yet
    if (real_ncpus < expected)
        LOG("only %u of %u logical CPUs online after 30 s; managing those", real_ncpus, expected);

    // ---- control gating
    uint32_t r[4];
    zen_cpuid(1, 0, r);
    if ((r[2] & CPUID1_ECX_HYPERVISOR) && blockReason == kAMDZenBlockNone)
        blockReason = kAMDZenBlockHypervisor;
    if (cpu.missingCpus() && blockReason == kAMDZenBlockNone) blockReason = kAMDZenBlockUnsupported;
    if (detectConflicts()) {
        if (forceControl) LOG("amdzen.force=1: taking control despite the conflicting driver");
        else if (blockReason == kAMDZenBlockNone) blockReason = kAMDZenBlockConflict;
    }

    // ---- IRPERF (Linux enables it at boot except on Zen1, erratum 1054)
    zen_cpuid(0x80000008, 0, r);
    if ((r[1] & CPUID88_EBX_IRPERF) && !(info->features & AMDZEN_F_IRPERF) &&
        model.zenGeneration > 1 && blockReason == kAMDZenBlockNone && governor != kAMDZenGovNone &&
        !nativeEnableMayBe()) {
        cpu.run(zen::kOpHwcrModify, nullptr, nullptr, nullptr, HWCR_IRPERF_EN, 0);
        if (cpu.features() & AMDZEN_F_IRPERF) {
            info->features |= AMDZEN_F_IRPERF;
            gIrperfByUs = true;
            LOG("enabled IRPERF (instructions retired counter)");
        }
    }

    chooseBackend();

    // ---- resolve per-CPU power profile (independent of fake SMBIOS)
    {
        const zennative::CpuProfile detected = zennative::detectCpuProfile(
            info->family, info->model, info->brand, info->physicalCores);
        const zennative::CpuProfile eff = (profileRequested == zennative::kProfileAuto)
            ? detected : static_cast<zennative::CpuProfile>(profileRequested);
        __atomic_store_n(&profileActive, static_cast<uint32_t>(eff), __ATOMIC_RELAXED);
        if (!eppExplicit) epp = zennative::tunablesForProfile(eff).baseEpp;
        if (!idleThresholdExplicit)
            __atomic_store_n(&idleThresholdUs, zennative::recommendedIdleThresholdUs(eff), __ATOMIC_RELAXED);
    }

    // ---- static info that discover() does not own
    info->abiVersion = AMDZEN_ABI_VERSION;
    info->structSize = sizeof(AMDZenInfo);
    strlcpy(info->kextVersion, AMDZEN_KEXT_VERSION, sizeof(info->kextVersion));
    if (tjMax_mC == 0) tjMax_mC = model.defaultTjMax_mC;
    info->tjMax_mC = tjMax_mC;
    if (allowPStateWrite) info->features |= AMDZEN_F_PSTATE_WRITE;
    probeSmnSensors();
    info->controlBlockReason = blockReason;

    sample->abiVersion = AMDZEN_ABI_VERSION;
    sample->structSize = sizeof(AMDZenSample);

    // ---- prime counters and requests (before anything is written)
    const zen::Topology &tp = cpu.topo();
    cpu.run(zen::kOpSamplePerf | zen::kOpSampleSlow, rawPrev);
    memcpy(rawTelemPrev, rawPrev, sizeof(zen::PerCpuRaw) * tp.ncpu);
    for (uint32_t c = 0; c < tp.ncpu && c < AMDZEN_MAX_CPUS; c++) {
        pstateReq[c] = (uint8_t)(rawPrev[c].pstateCtl & 7);
        cppcReq[c] = cpu.cppcReqInitial(c);
    }
    rebaseEnergy = true;
    resetGovernorSampling();
    lastTelemetryNs = 0;
    initialized = true;

    applyPolicy(true);

    // ---- late-loading conflicting drivers: go monitor-only when one publishes
    if (!forceControl && blockReason == kAMDZenBlockNone) {
        IOServiceMatchingNotificationHandler onConflict =
            [](void *target, void *, IOService *svc, IONotifier *) -> bool {
                AMDZenPM *self = static_cast<AMDZenPM *>(target);
                if (self && svc)
                    self->runGated(kGateConflict, (void *)svc->getMetaClass()->getClassName());
                return true;
            };
        for (uint32_t i = 0; i < kNumConflictClasses; i++) {
            if (OSDictionary *m = IOService::serviceMatching(kConflictClasses[i])) {
                gConflictNotifiers[i] = IOService::addMatchingNotification(
                    gIOFirstPublishNotification, m, onConflict, this, nullptr, 0);
                m->release();
            }
        }
    }

    setupIdle();
    nativeSchedule();

    publishProperties();
    const uint32_t gov = (blockReason || backend == kAMDZenBackendNone) ? (uint32_t)kAMDZenGovNone : governor;
    LOG("%s (%s, fam %02xh model %02xh) %u CPUs/%u cores, profile %s, backend %s, governor %s, block %s",
        info->brand, model.codename, info->family, info->model, tp.ncpu, tp.ncores,
        zennative::profileName(static_cast<zennative::CpuProfile>(profileActive)),
        backendName(backend), zen::governorName(gov), blockReasonName(blockReason));
    return true;
}

void AMDZenPM::stop(IOService *provider) {
    if (IOLock *l = __atomic_load_n(&gInstanceLock, __ATOMIC_ACQUIRE)) {
        IOLockLock(l);
        if (gInstance == this) gInstance = nullptr;
        IOLockUnlock(l);
    }
    unregisterSysctls();
    for (uint32_t i = 0; i < kNumConflictClasses; i++) {
        if (gConflictNotifiers[i]) { gConflictNotifiers[i]->remove(); gConflictNotifiers[i] = nullptr; }
    }
    if (sleepNotifier) {
        sleepNotifier->remove();
        sleepNotifier = nullptr;
    }
    runGated(kGateStop);   // prevents enqueue; restores only legacy/MSR hardware
    nativeDrain();        // outside gate: a worker may need the gate to finish
    // Clear both MachineIdle and urgency through the existing pinned-table path.
    zen::idle::unhook();
    nativeStop();         // signals -> neutral request best-effort -> teardown
    if (workLoop) {
        if (timer) workLoop->removeEventSource(timer);
        if (gate) workLoop->removeEventSource(gate);
    }
    IOService::stop(provider);
}

// ===========================================================================
// Hardware policy
// ===========================================================================
void AMDZenPM::buildPStateTable(zen::PStateTable *t) {
    memset(t, 0, sizeof(*t));
    uint32_t n = minU(info->numPStates, AMDZEN_MAX_PSTATES);
    t->count = n;
    for (uint32_t i = 0; i < n; i++)
        t->freqMHz[i] = info->pstates[i].enabled ? info->pstates[i].freqMHz : 0;
    uint32_t maxIdx = legacyMaxIndex(info);
    uint32_t curLim = 0;
    uint64_t lim = 0;
    if (cpu.readCurLimit(&lim)) curLim = zen::pstateCurLimit(lim);
    t->slowest = minU(limitSlow, maxIdx);
    t->fastest = minU(maxU(limitFast, curLim), t->slowest);
}

void AMDZenPM::applyPolicy(bool force) {
    if (nativeEnableMayBe() || nativePending || backend == kAMDZenBackendCppcPcc) return;
    if (blockReason != kAMDZenBlockNone || backend == kAMDZenBackendNone ||
        governor == kAMDZenGovNone || sleeping)
        return;
    const zen::Topology &tp = cpu.topo();
    const uint32_t ncpu = minU(tp.ncpu, AMDZEN_MAX_CPUS);

    if (force) {
        memset(govState, 0, sizeof(zen::GovState) * AMDZEN_MAX_CORES);
        resetGovernorSampling();
        nativeResetMsr();
    }

    // Boost (HWCR.CpbDis on every thread)
    if (boostMode >= 0 && (info->features & AMDZEN_F_CPB)) {
        cpu.run(zen::kOpHwcrModify, nullptr, nullptr, nullptr,
                boostMode ? 0 : HWCR_CPB_DIS, boostMode ? HWCR_CPB_DIS : 0);
        gBoostTouched = true;
        gControlTouched = true;
    }

    if (backend == kAMDZenBackendLegacy) {
        uint32_t maxIdx = legacyMaxIndex(info);
        uint32_t fast = minU(limitFast, maxIdx);
        uint32_t slow = maxU(minU(limitSlow, maxIdx), fast);
        bool write = true;
        uint32_t idx = 0;
        switch (governor) {
        case kAMDZenGovPerformance: idx = fast; break;
        case kAMDZenGovPowersave:   idx = slow; break;
        case kAMDZenGovUserspace:   idx = clampU(userValue, fast, slow); break;
        default:                    write = false; break;   // dynamic: next ticks decide
        }
        if (write) {
            idx = enabledAtOrFaster(info, idx);
            for (uint32_t c = 0; c < ncpu; c++) pstateReq[c] = (uint8_t)idx;
            cpu.run(zen::kOpWritePState, nullptr, pstateReq);
            gControlTouched = true;
        }
        return;
    }

    // CPPC MSR backend
    if (!gCppcEnabled) {
        // amd-pstate: request P0 through PERF_CTL, then enable CPPC (write-once).
        for (uint32_t c = 0; c < ncpu; c++) pstateReq[c] = 0;
        cpu.run(zen::kOpWritePState | zen::kOpCppcEnable, nullptr, pstateReq);
        gCppcEnabled = true;
        gControlTouched = true;
    }
    bool boostOn = boostMode >= 0 ? boostMode == 1 : !(cpu.hwcrInitial() & HWCR_CPB_DIS);
    CppcPolicy p = cppcPolicy(info, governor, boostOn, limitFast, limitSlow, epp, userValue,
                              (uint32_t)((cpu.cppcReqInitial(0) >> 8) & 0xFF));
    for (uint32_t c = 0; c < ncpu; c++)
        cppcReq[c] = zen::encodeCppcReq(cpu.cppcReqInitial(c), p.maxPerf, p.minPerf, p.desPerf, p.epp);
    cpu.run(zen::kOpWriteCppcReq, nullptr, nullptr, cppcReq);
    gControlTouched = true;
}

void AMDZenPM::restoreHardware() {
    if (nativeEnableMayBe() || nativePending || backend == kAMDZenBackendCppcPcc) return;
    if (!gControlTouched || !info) return;
    const zen::Topology &tp = cpu.topo();
    const uint32_t ncpu = minU(tp.ncpu, AMDZEN_MAX_CPUS);

    if (backend == kAMDZenBackendLegacy) {
        for (uint32_t c = 0; c < ncpu; c++) pstateReq[c] = 0;      // P0 everywhere
        cpu.run(zen::kOpWritePState, nullptr, pstateReq);
    } else if (backend == kAMDZenBackendCppcMsr && gCppcEnabled) {
        for (uint32_t c = 0; c < ncpu; c++) {
            uint64_t boot = cpu.cppcReqInitial(c);
            if ((boot & 0xFF) == 0) {
                // No usable firmware request (CPPC was off at boot): leave a sane,
                // unconstrained request instead of max_perf = 0.
                uint32_t hi = info->cppcEffectiveHighest ? info->cppcEffectiveHighest : info->cppcHighest;
                boot = zen::encodeCppcReq(boot, hi, info->cppcLowest, 0, EPP_BALANCE_PERFORMANCE);
            }
            cppcReq[c] = boot;
        }
        cpu.run(zen::kOpWriteCppcReq, nullptr, nullptr, cppcReq);
    }
    if (gBoostTouched) {
        cpu.run(zen::kOpRestoreBootBoost, nullptr);
        gBoostTouched = false;
    }
    gControlTouched = false;
}

bool AMDZenPM::nativePrepareHardware(const bool *enabled, const bool *boostOn, uint32_t ncpu) {
    gBoostTouched = true;
    gControlTouched = true;
    return cpu.prepareAutonomous(enabled, boostOn, ncpu);
}

void AMDZenPM::nativeRestorePreparation() {
    restoreHardware(); // only called after clearing nativePending, before any enable
}

// ===========================================================================
// Periodic work
// ===========================================================================
void AMDZenPM::resetGovernorSampling() {
    idleSnapValid = false;
    perfFresh = false;
    govSource = zen::kGovSampleHold;
}

void AMDZenPM::timerFired(IOTimerEventSource *src) {
    if (sleeping || !info || initFailed) return;
    if (!initialized) {
        if (!tryInitHardware()) { src->setTimeoutMS(250); return; }
        if (initFailed) return;
        src->setTimeoutMS(50);
        return;
    }
    const uint64_t now = uptimeNs();
    nativeSchedule();
    const bool nativePath = nativePending || backend == kAMDZenBackendCppcPcc;
    const uint32_t gov = (blockReason || backend == kAMDZenBackendNone) ? (uint32_t)kAMDZenGovNone : governor;
    const bool control = !nativeEnableMayBe() && !nativePath && gov != kAMDZenGovNone;
    const bool msrNative = control && backend == kAMDZenBackendCppcMsr &&
        gov == kAMDZenGovCppcActive && gCppcEnabled;
    const bool dyn = control && isDynamicGov(gov);
    const bool prime = rebaseEnergy;
    const uint64_t telNs = (uint64_t)telemetryPeriodMs * 1000000ull;
    const uint64_t halfGovNs = (uint64_t)governorPeriodMs * 500000ull;

    bool doTel = prime || (!nativePath && !msrNative && !dyn) || lastTelemetryNs == 0 ||
        (now - lastTelemetryNs) + ((nativePath || msrNative) ? 0 : halfGovNs) >= telNs;

    uint32_t writeOp = 0;
    if (control) {
        if (backend == kAMDZenBackendLegacy) writeOp = zen::kOpWritePState;
        else if (backend == kAMDZenBackendCppcMsr && gCppcEnabled) writeOp = zen::kOpWriteCppcReq;
    }

    // A coherent idle window is sufficient even without exits: unchanged idle
    // counters mean fully busy, advancing open-idle counters mean idle. Only
    // priming/invalid/disabled accounting needs APERF, never the exit count alone.
    // On wake, prime BOTH sources here without deciding across the sleep interval.
    if (prime) resetGovernorSampling();
    if (msrNative) nativeStepCppcMsr();
    bool idleGov = false;
    if (dyn && writeOp == zen::kOpWritePState &&
        __atomic_load_n(&govIdleSampling, __ATOMIC_RELAXED) && zen::idle::active())
        idleGov = governorTickIdle() == zen::kIdleSampleReady;
    else
        idleSnapValid = false;          // re-prime when this path is taken again

    const bool havePerf = (info->features & AMDZEN_F_APERFMPERF) != 0;
    const zen::GovSamplePlan sampling = zen::planGovernorSample(dyn, prime, idleGov, doTel,
                                                                perfFresh, havePerf);
    // Telemetry can share a real sample with fallback, but an older multi-window
    // delta must never stand in for this tick. A first fallback round only primes
    // when the preceding dynamic tick did not sample APERF.
    const bool needPerf = sampling.samplePerf;
    bool perfAnswered = needPerf && havePerf;
    const uint32_t ncpu = minU(cpu.topo().ncpu, AMDZEN_MAX_CPUS);
    if (needPerf) {
        // An unanswered CPU must not leave an old buffer timestamp looking fresh.
        for (uint32_t c = 0; c < ncpu; c++) rawCur[c].perf.tsc = 0;
        if (!ncpu) perfAnswered = false;
    }
    uint32_t ops = needPerf ? (zen::kOpSamplePerf | (doTel ? zen::kOpSampleSlow : 0)) : 0;
    if (writeOp && (needPerf || reqDirty)) ops |= writeOp;
    if (ops) {
        cpu.run(ops, needPerf ? rawCur : nullptr, pstateReq, cppcReq);
        rendezvousCount++;
        if (ops & writeOp) {
            gControlTouched = true;
            reqDirty = false;
        }
    }
    if (needPerf) {
        for (uint32_t c = 0; c < ncpu; c++)
            if (!rawCur[c].perf.tsc) perfAnswered = false;
    }

    govSource = sampling.source;
    if (sampling.source == zen::kGovSamplePerf) {
        if (!perfAnswered || !governorTick()) govSource = zen::kGovSampleHold;
        // Apply the decision now rather than on the next tick: a second round, but
        // only when a request actually changed.
        if (reqDirty && writeOp) {
            cpu.run(writeOp, nullptr, pstateReq, cppcReq);
            rendezvousCount++;
            gControlTouched = true;
            reqDirty = false;
        }
    }
    if (doTel) {
        if (rendezvousPrevNs && now > rendezvousPrevNs)
            rendezvousRate = zen::ratePerSecond(rendezvousCount - rendezvousPrev, now - rendezvousPrevNs);
        rendezvousPrev = rendezvousCount;
        rendezvousPrevNs = now;
        telemetryTick(now);
    }
    rebaseEnergy = false;
    lastGovNs = now;

    if (needPerf) {
        zen::PerCpuRaw *t = rawPrev;
        rawPrev = rawCur;
        rawCur = t;
    }
    perfFresh = sampling.nextPerfFresh && perfAnswered;

    src->setTimeoutMS((nativePath || msrNative) ? nativeIntervalMs : (dyn ? governorPeriodMs : telemetryPeriodMs));
}

bool AMDZenPM::governorTick() {
    const zen::Topology &tp = cpu.topo();
    const uint32_t ncpu = minU(tp.ncpu, AMDZEN_MAX_CPUS);
    const uint32_t ncores = minU(tp.ncores, AMDZEN_MAX_CORES);
    if (!ncpu || !ncores) return false;

    const bool irperf = (info->features & AMDZEN_F_IRPERF) != 0;
    const uint64_t tscHz = cpu.tscHz();

    uint32_t refMHz;
    CppcPolicy pol {};
    if (backend == kAMDZenBackendCppcMsr) {
        bool boostOn = boostMode >= 0 ? boostMode == 1 : !(cpu.hwcrInitial() & HWCR_CPB_DIS);
        pol = cppcPolicy(info, governor, boostOn, limitFast, limitSlow, epp, userValue,
                              (uint32_t)((cpu.cppcReqInitial(0) >> 8) & 0xFF));
        // governCppcDesired scales util1024 by maxPerf, so util must be relative to the
        // frequency of pol.maxPerf (same linear perf<->freq map): des ~= headroom * delivered perf.
        refMHz = (info->cppcNominal && info->baseFreqMHz)
                     ? zen::cppcPerfToFreq(pol.maxPerf, info->cppcNominal, info->baseFreqMHz)
                     : info->maxBoostFreqMHz;
    } else {
        refMHz = info->baseFreqMHz ? info->baseFreqMHz : info->pstates[0].freqMHz;
    }
    if (!refMHz) return false;

    // Per-core aggregation: the core runs at the fastest request of its threads, so
    // the domain load is the max over SMT siblings.
    uint16_t coreUtil[AMDZEN_MAX_CORES];
    uint16_t coreBusy[AMDZEN_MAX_CORES];
    memset(coreUtil, 0, sizeof(coreUtil));
    memset(coreBusy, 0, sizeof(coreBusy));
    for (uint32_t c = 0; c < ncpu; c++) {
        uint32_t core = tp.coreOfCpu[c];
        if (core >= ncores) continue;
        if (!zen::governorPerfWindowValid(rawPrev[c].perf, rawCur[c].perf)) return false;
        zen::PerfDerived d = zen::derivePerf(rawPrev[c].perf, rawCur[c].perf, tscHz, refMHz, irperf);
        if (d.util1024 > coreUtil[core]) coreUtil[core] = (uint16_t)minU(d.util1024, 1024);
        if (d.busyPermille > coreBusy[core]) coreBusy[core] = (uint16_t)minU(d.busyPermille, 1000);
    }

    if (backend == kAMDZenBackendLegacy) {
        zen::PStateTable t;
        buildPStateTable(&t);
        governLegacyCores(t, coreUtil, coreBusy);
    } else if (backend == kAMDZenBackendCppcMsr && gCppcEnabled) {
        for (uint32_t k = 0; k < ncores; k++) {
            uint32_t des = zen::governCppcDesired(governor, tun, pol.minPerf, pol.maxPerf, &govState[k],
                                                  coreUtil[k], coreBusy[k], userValue);
            coreUtil[k] = (uint16_t)minU(des, 255);
        }
        for (uint32_t c = 0; c < ncpu; c++) {
            uint32_t core = tp.coreOfCpu[c];
            if (core >= ncores) continue;
            const uint64_t req = zen::encodeCppcReq(cpu.cppcReqInitial(c), pol.maxPerf, pol.minPerf,
                                                    coreUtil[core], 0);
            if (req != cppcReq[c]) reqDirty = true;
            cppcReq[c] = req;
        }
    } else return false;
    return true;
}

void AMDZenPM::governLegacyCores(const zen::PStateTable &t, const uint16_t *coreUtil,
                                 const uint16_t *coreBusy) {
    const zen::Topology &tp = cpu.topo();
    const uint32_t ncpu = minU(tp.ncpu, AMDZEN_MAX_CPUS);
    const uint32_t ncores = minU(tp.ncores, AMDZEN_MAX_CORES);
    zen::GovTunables effTun = tun;
    if (nativeLegacyPowerSave()) {
        effTun.fastUpPct = 0;
        if (effTun.utilHeadroomPct > 110) effTun.utilHeadroomPct = 110;
    }
    uint8_t idxOfCore[AMDZEN_MAX_CORES];
    for (uint32_t k = 0; k < ncores; k++) {
        uint32_t idx = zen::governLegacy(governor, effTun, t, &govState[k], coreUtil[k], coreBusy[k], userValue);
        // Never request a disabled / undecodable definition (fall back towards P0).
        idxOfCore[k] = (uint8_t)enabledAtOrFaster(info, minU(idx, AMDZEN_MAX_PSTATES - 1));
    }
    for (uint32_t c = 0; c < ncpu; c++) {
        uint32_t core = tp.coreOfCpu[c];
        if (core >= ncores) continue;
        if (pstateReq[c] != idxOfCore[core]) reqDirty = true;
        pstateReq[c] = idxOfCore[core];
    }
}

// Idle input uses each CPU's own coherent observation interval. No exit-count
// heuristic: both continuously busy and continuously open-idle windows are valid.
// Consume every CPU's baseline even if another CPU failed. Until every domain is
// known, leave requests AND governor history untouched; a fresh APERF pair may
// supply the decision instead. util still estimates delivered frequency from the
// last requested P-state (not boost/throttling), as before.
zen::IdleSampleStatus AMDZenPM::governorTickIdle() {
    const zen::Topology &tp = cpu.topo();
    const uint32_t ncpu = minU(tp.ncpu, AMDZEN_MAX_CPUS);
    const uint32_t ncores = minU(tp.ncores, AMDZEN_MAX_CORES);
    if (!ncpu || !ncores) return zen::kIdleSampleInvalid;

    zen::idle::snapshot(idleSnapCur, ncpu);
    const bool reset = !idleSnapValid;
    idleSnapValid = true;
    zen::IdleSampleStatus status = zen::kIdleSampleReady;
    uint16_t coreUtil[AMDZEN_MAX_CORES];
    uint16_t coreBusy[AMDZEN_MAX_CORES];
    memset(coreUtil, 0, sizeof(coreUtil));
    memset(coreBusy, 0, sizeof(coreBusy));
    for (uint32_t c = 0; c < ncpu; c++) {
        const zen::IdleWindow w = zen::idleWindowAdvance(&idleSnapPrev[c], idleSnapCur[c], reset);
        if (w.status < status) status = w.status;
        uint32_t core = tp.coreOfCpu[c];
        if (core >= ncores) { status = zen::kIdleSampleInvalid; continue; }
        if (w.status == zen::kIdleSampleReady && w.busyPermille > coreBusy[core])
            coreBusy[core] = (uint16_t)w.busyPermille;
    }
    if (status != zen::kIdleSampleReady) return status;
    const uint32_t refMHz = info->baseFreqMHz ? info->baseFreqMHz : info->pstates[0].freqMHz;
    if (!refMHz) return zen::kIdleSampleInvalid;
    for (uint32_t k = 0; k < ncores; k++) {
        if (tp.primaryCpuOfCore[k] >= ncpu) return zen::kIdleSampleInvalid;
        uint32_t idx = minU(pstateReq[tp.primaryCpuOfCore[k]], AMDZEN_MAX_PSTATES - 1);
        uint32_t f = info->pstates[idx].freqMHz ? info->pstates[idx].freqMHz : refMHz;
        coreUtil[k] = (uint16_t)zen::utilFromBusy(coreBusy[k], f, refMHz);
    }
    zen::PStateTable t;
    buildPStateTable(&t);
    governLegacyCores(t, coreUtil, coreBusy);
    return zen::kIdleSampleReady;
}

void AMDZenPM::telemetryTick(uint64_t nowNs) {
    const zen::Topology &tp = cpu.topo();
    const uint32_t ncpu = minU(tp.ncpu, AMDZEN_MAX_CPUS);
    const uint32_t ncores = minU(tp.ncores, AMDZEN_MAX_CORES);
    const bool prime = rebaseEnergy || lastTelemetryNs == 0;
    const uint64_t intervalNs = prime ? 0 : nowNs - lastTelemetryNs;
    const uint64_t f = info->features;
    const bool irperf = (f & AMDZEN_F_IRPERF) != 0;
    const uint32_t esu = info->energyUnitShift;
    AMDZenSample *s = sample;

    // ---- per-CPU / per-core performance over the telemetry interval
    uint32_t refMHz = info->baseFreqMHz ? info->baseFreqMHz : info->pstates[0].freqMHz;
    if (!refMHz) refMHz = 1000;
    for (uint32_t k = 0; k < ncores; k++) {
        s->core[k].effFreqMHz = 0;
        s->core[k].busyPermille = 0;
        s->core[k].ipcX100 = 0;
    }
    uint64_t busySum = 0;
    for (uint32_t c = 0; c < ncpu; c++) {
        AMDZenCpuSample &cs = s->cpu[c];
        cs.curPState = (uint8_t)(rawCur[c].pstateStatus & 7);
        cs.reqPState = (uint8_t)(rawCur[c].pstateCtl & 7);
        if (prime) {
            cs.effFreqMHz = 0;
            cs.busyPermille = 0;
            continue;
        }
        zen::PerfDerived d = zen::derivePerf(rawTelemPrev[c].perf, rawCur[c].perf, cpu.tscHz(), refMHz, irperf);
        cs.effFreqMHz = (uint16_t)minU(d.effFreqMHz, 0xFFFF);
        cs.busyPermille = (uint16_t)minU(d.busyPermille, 1000);
        busySum += cs.busyPermille;
        uint32_t core = tp.coreOfCpu[c];
        if (core >= ncores) continue;
        AMDZenCoreSample &k = s->core[core];
        if (cs.effFreqMHz > k.effFreqMHz) k.effFreqMHz = cs.effFreqMHz;
        if (cs.busyPermille > k.busyPermille) k.busyPermille = cs.busyPermille;
        uint32_t ipc = minU(d.ipcX100, 0xFFFF);
        if (ipc > k.ipcX100) k.ipcX100 = (uint16_t)ipc;
    }

    // ---- per-core energy and requested VID (primary thread values)
    uint32_t coresPower = 0;
    for (uint32_t k = 0; k < ncores; k++) {
        AMDZenCoreSample &cs = s->core[k];
        uint32_t p = tp.primaryCpuOfCore[k];
        if (p >= ncpu) continue;
        if (f & AMDZEN_F_CORE_ENERGY) {
            uint32_t raw = (uint32_t)rawCur[p].coreEnergy;
            uint32_t delta = zen::energyUpdate(&coreEnergy[k], raw, prime);
            if (!prime && intervalNs) {
                uint64_t uJ = zen::energyRawToMicroJoule(delta, esu);
                uint32_t mW = zen::powerMilliWatt(uJ, intervalNs);
                if (mW > kMaxPlausiblePower_mW) { uJ = 0; mW = 0; }
                cs.energy_uJ += uJ;
                cs.power_mW = mW;
                coresPower += mW;
            } else {
                cs.power_mW = 0;
            }
        }
        if ((f & AMDZEN_F_HWPSTATUS) && model.sviVidFormula) {
            uint32_t vid = (uint32_t)((rawCur[p].hwPstateStatus >> 14) & 0xFF);
            cs.vid_mV = (uint16_t)zen::sviVidToMilliVolt(vid);
        }
    }
    s->coresPower_mW = coresPower;

    // ---- package energy
    if (f & AMDZEN_F_RAPL) {
        uint32_t raw = 0;
        if (cpu.readPkgEnergy(&raw)) {
            uint32_t delta = zen::energyUpdate(&pkgEnergy, raw, prime);
            if (!prime && intervalNs) {
                uint64_t uJ = zen::energyRawToMicroJoule(delta, esu);
                uint32_t mW = zen::powerMilliWatt(uJ, intervalNs);
                if (mW > kMaxPlausiblePower_mW) { uJ = 0; mW = s->pkgPower_mW; }
                s->pkgEnergy_uJ += uJ;
                s->pkgPower_mW = mW;
            }
        }
    }

    // ---- temperatures (keep the previous value on a failed / implausible read)
    if (f & AMDZEN_F_TCTL) {
        uint32_t raw = 0;
        if (smn.read(SMN_THM_TCON_CUR_TMP, &raw)) {
            int32_t t = zen::clampTctl(zen::decodeTctl(raw), model.dispNegative);
            if (t > -50000 && t < 150000) {
                s->tctl_mC = t;
                s->tdie_mC = zen::clampTctl(t - model.tctlOffset_mC, model.dispNegative);
                __atomic_store_n(&lastTdie_mC, s->tdie_mC, __ATOMIC_RELAXED);
            }
        }
    }
    if (f & AMDZEN_F_CCD_TEMP) {
        for (uint32_t i = 0; i < AMDZEN_MAX_CCDS; i++) {
            if (!(info->ccdTempMask & (1u << i))) continue;
            uint32_t raw = 0;
            int32_t mC = 0;
            if (smn.read(SMN_THM_TCON_CUR_TMP + model.ccdOffset + 4 * i, &raw) &&
                zen::decodeCcdTemp(raw, &mC) && mC > -50000 && mC < 150000)
                s->tccd_mC[i] = mC;
        }
    }

    // ---- SVI2 telemetry
    if (f & AMDZEN_F_SVI2) {
        uint32_t raw = 0;
        if (model.sviCoreAddr && smn.read(model.sviCoreAddr, &raw)) {
            uint32_t mV = zen::sviPlaneMilliVolt(raw);
            if (mV >= 200 && mV <= 1520) {
                s->vcore_mV = mV;
                s->icore_mA = zen::sviPlaneMilliAmp(raw, model.sviCoreFactor_uA);
            }
        }
        if (model.sviSocAddr && smn.read(model.sviSocAddr, &raw)) {
            uint32_t mV = zen::sviPlaneMilliVolt(raw);
            if (mV >= 200 && mV <= 1520) {
                s->vsoc_mV = mV;
                s->isoc_mA = zen::sviPlaneMilliAmp(raw, model.sviSocFactor_uA);
            }
        }
    }

    // ---- summaries
    uint32_t maxF = 0, sumBusyF = 0, nBusy = 0, sumAnyF = 0, nAny = 0;
    for (uint32_t k = 0; k < ncores; k++) {
        uint32_t fr = s->core[k].effFreqMHz;
        if (!fr) continue;
        if (fr > maxF) maxF = fr;
        sumAnyF += fr; nAny++;
        if (s->core[k].busyPermille >= 20) { sumBusyF += fr; nBusy++; }
    }
    s->maxFreqMHz = maxF;
    // Legacy backend has no CPPC "highest perf": learn the boost ceiling from the
    // highest effective clock seen on a mostly-busy core (short samples are noisy).
    if (backend == kAMDZenBackendLegacy) {
        uint32_t peak = 0;
        for (uint32_t k = 0; k < ncores; k++)
            if (s->core[k].busyPermille >= 500 && s->core[k].effFreqMHz > peak) peak = s->core[k].effFreqMHz;
        peak = peak / 25 * 25;
        if (peak <= 7000 && peak >= info->maxBoostFreqMHz + 25 && peak > info->baseFreqMHz) {
            info->maxBoostFreqMHz = peak;
            removeProperty("CPUInfo");     // republished below with the new value
        }
    }
    s->avgFreqMHz = nBusy ? sumBusyF / nBusy : (nAny ? sumAnyF / nAny : 0);
    s->avgBusyPermille = ncpu ? (uint32_t)(busySum / ncpu) : 0;

    uint64_t lim = 0;
    s->pstateLimit = cpu.readCurLimit(&lim) ? zen::pstateCurLimit(lim) : 0;
    s->boostEnabled = (f & AMDZEN_F_CPB) && ncpu && !(rawCur[0].hwcr & HWCR_CPB_DIS) ? 1 : 0;

    const uint32_t gov = (blockReason || backend == kAMDZenBackendNone) ? (uint32_t)kAMDZenGovNone : governor;
    s->governor = gov;
    if (backend == kAMDZenBackendCppcMsr) {
        if (gov == kAMDZenGovCppcActive) {
            s->limitFast = limitFast;
            s->limitSlow = limitSlow;
            s->epp = epp;
        } else {
            bool boostOn = boostMode >= 0 ? boostMode == 1 : !(cpu.hwcrInitial() & HWCR_CPB_DIS);
            CppcPolicy p = cppcPolicy(info, governor, boostOn, limitFast, limitSlow, epp, userValue,
                                  (uint32_t)((cpu.cppcReqInitial(0) >> 8) & 0xFF));
            s->limitFast = p.minPerf;
            s->limitSlow = p.maxPerf;
            s->epp = (gov != kAMDZenGovNone) ? p.epp : (uint32_t)((cpu.cppcReqInitial(0) >> 24) & 0xFF);
        }
    } else if (backend == kAMDZenBackendCppcPcc) {
        // Report user-configured overrides (0 = uncapped/default), not CPU0's current
        // dynamic request (already in cppc_min/cppc_max), so sysctl limit_fast writes
        // never copy CPU0's maxPerf and silently cap higher-ranked cores.
        s->limitFast = limitFast;
        s->limitSlow = limitSlow;
        s->epp = epp; // base EPP tunable; current native request is in diagnostics
    } else {
        uint32_t maxIdx = legacyMaxIndex(info);
        s->limitFast = minU(limitFast, maxIdx);
        s->limitSlow = maxU(minU(limitSlow, maxIdx), s->limitFast);
        s->epp = epp;          // configured value; only applied on the CPPC backend
    }
    s->numCpus = ncpu;
    s->numCores = ncores;
    s->abiVersion = AMDZEN_ABI_VERSION;
    s->structSize = sizeof(AMDZenSample);
    s->timestampNs = nowNs;
    const uint64_t us = intervalNs / 1000ull;
    s->intervalUs = us > 0xFFFFFFFFull ? 0xFFFFFFFFu : (uint32_t)us;
    s->seq = ++seq;

    memcpy(rawTelemPrev, rawCur, sizeof(zen::PerCpuRaw) * ncpu);
    lastTelemetryNs = nowNs;

    idleTelemetry(nowNs);
    publishProperties();
}

// ===========================================================================
// Registry publishing
// ===========================================================================
void AMDZenPM::publishProperties() {
    if (!info || !sample) return;
    const AMDZenSample *s = sample;

    // ---- static CPUInfo (once; removed + republished when it changes)
    if (!getProperty("CPUInfo")) {
        if (OSDictionary *d = OSDictionary::withCapacity(32)) {
            dictStr(d, "Brand", info->brand);
            dictStr(d, "Codename", info->codename);
            dictStr(d, "Vendor", info->vendor == 1 ? "Hygon" : "AMD");
            dictStr(d, "KextVersion", info->kextVersion);
            dictNum(d, "Family", info->family);
            dictNum(d, "Model", info->model);
            dictNum(d, "Stepping", info->stepping);
            dictNum(d, "ZenGeneration", info->zenGeneration);
            dictNum(d, "Microcode", info->microcode);
            dictNum(d, "Features", info->features, 64);
            dictNum(d, "TscHz", info->tscHz, 64);
            dictNum(d, "LogicalCPUs", info->logicalCpus);
            dictNum(d, "PhysicalCores", info->physicalCores);
            dictNum(d, "ThreadsPerCore", info->threadsPerCore);
            dictNum(d, "CCX", info->numCcx);
            dictNum(d, "CCDTempMask", info->ccdTempMask);
            dictNum(d, "EnergyUnitShift", info->energyUnitShift);
            dictNum(d, "BaseFreqMHz", info->baseFreqMHz);
            dictNum(d, "MaxBoostFreqMHz", info->maxBoostFreqMHz);
            dictStr(d, "MaxBoostSource", ((info->features & AMDZEN_F_CPPC) || backend == kAMDZenBackendCppcPcc) ? "cppc" : "observed");
            if (info->features & AMDZEN_F_SMN) {
                dictNum(d, "SviTfn", sviTfn);
                dictNum(d, "SviCoreAddr", model.sviCoreAddr);
                dictNum(d, "SviSocAddr", model.sviSocAddr);
                dictNum(d, "SviCoreProbeRaw", sviCoreProbeRaw);
                dictNum(d, "SviSocProbeRaw", sviSocProbeRaw);
            }
            dictNum(d, "PStateMaxVal", info->pstateMaxVal);
            dictNum(d, "TjMax_mC", info->tjMax_mC);
            dictStr(d, "CPUProfile", zennative::profileName(
                static_cast<zennative::CpuProfile>(__atomic_load_n(&profileActive, __ATOMIC_RELAXED))));
            dictStr(d, "Backend", backendName(backend));
            dictStr(d, "BlockReason", blockReasonName(blockReason));
            dictNum(d, "BlockReasonCode", blockReason);
            if ((info->features & AMDZEN_F_CPPC) || backend == kAMDZenBackendCppcPcc) {
                dictNum(d, "CPPCHighest", info->cppcHighest);
                dictNum(d, "CPPCNominal", info->cppcNominal);
                dictNum(d, "CPPCLowestNonlinear", info->cppcLowestNonlinear);
                dictNum(d, "CPPCLowest", info->cppcLowest);
                dictNum(d, "CPPCEffectiveHighest", info->cppcEffectiveHighest);
            }
            static const struct { uint64_t bit; const char *name; } kFeat[] = {
                { AMDZEN_F_APERFMPERF, "APERFMPERF" }, { AMDZEN_F_EFFFREQ_RO, "EffFreqRO" },
                { AMDZEN_F_HWPSTATE, "HwPstate" },     { AMDZEN_F_TSC_INVARIANT, "TscInvariant" },
                { AMDZEN_F_CPB, "CPB" },               { AMDZEN_F_RAPL, "RAPL" },
                { AMDZEN_F_CORE_ENERGY, "CoreEnergy" },{ AMDZEN_F_CPPC, "CPPC" },
                { AMDZEN_F_IRPERF, "IRPERF" },         { AMDZEN_F_PPIN, "PPIN" },
                { AMDZEN_F_SMN, "SMN" },               { AMDZEN_F_TCTL, "Tctl" },
                { AMDZEN_F_CCD_TEMP, "CCDTemp" },      { AMDZEN_F_SVI2, "SVI2" },
                { AMDZEN_F_HWPSTATUS, "HwPstateStatus" }, { AMDZEN_F_MWAIT, "MWAIT" },
                { AMDZEN_F_TOPOEXT, "TopoExt" },       { AMDZEN_F_FAST_CPPC, "FastCPPC" },
                { AMDZEN_F_PREFCORE, "PreferredCore" },{ AMDZEN_F_PSTATE_WRITE, "PStateWrite" },
            };
            if (OSArray *fa = OSArray::withCapacity(20)) {
                for (const auto &e : kFeat) {
                    if (!(info->features & e.bit)) continue;
                    if (const OSSymbol *sym = OSSymbol::withCString(e.name)) {
                        fa->setObject(sym);
                        sym->release();
                    }
                }
                dictArr(d, "FeatureNames", fa);
            }
            if (OSArray *pa = OSArray::withCapacity(AMDZEN_MAX_PSTATES)) {
                uint32_t n = minU(info->numPStates, AMDZEN_MAX_PSTATES);
                for (uint32_t i = 0; i < n; i++) {
                    const AMDZenPStateDef &p = info->pstates[i];
                    if (OSDictionary *pd = OSDictionary::withCapacity(8)) {
                        dictNum(pd, "Index", i);
                        dictBool(pd, "Enabled", p.enabled != 0);
                        dictNum(pd, "FreqMHz", p.freqMHz);
                        dictNum(pd, "FID", p.fid);
                        dictNum(pd, "DID", p.did);
                        dictNum(pd, "VID", p.vid);
                        dictNum(pd, "Voltage_mV", p.voltage_mV);
                        dictNum(pd, "Raw", p.raw, 64);
                        pa->setObject(pd);
                        pd->release();
                    }
                }
                dictArr(d, "PStates", pa);
            }
            publishIdleInfo(d);
            setProperty("CPUInfo", d);
            d->release();
        }
    }

    // ---- live Sensors (one dictionary per telemetry tick)
    OSDictionary *d = OSDictionary::withCapacity(24);
    if (!d) return;
    const uint32_t ncores = minU(s->numCores, AMDZEN_MAX_CORES);
    const uint64_t f = info->features;
    dictNum(d, "Seq", s->seq, 64);
    dictNum(d, "IntervalUs", s->intervalUs);
    if (f & AMDZEN_F_TCTL) {
        dictNum(d, "Tctl_mC", (uint32_t)s->tctl_mC);
        dictNum(d, "Tdie_mC", (uint32_t)s->tdie_mC);
    }
    if (f & AMDZEN_F_CCD_TEMP) {
        if (OSArray *a = OSArray::withCapacity(AMDZEN_MAX_CCDS)) {
            for (uint32_t i = 0; i < AMDZEN_MAX_CCDS; i++)
                if (info->ccdTempMask & (1u << i)) arrNum(a, (uint32_t)s->tccd_mC[i]);
            dictArr(d, "Tccd_mC", a);
        }
    }
    if (f & AMDZEN_F_RAPL) {
        dictNum(d, "PackagePower_mW", s->pkgPower_mW);
        dictNum(d, "PackageEnergy_uJ", s->pkgEnergy_uJ, 64);
    }
    if (f & AMDZEN_F_CORE_ENERGY) {
        dictNum(d, "CoresPower_mW", s->coresPower_mW);
        if (OSArray *a = OSArray::withCapacity(ncores)) {
            for (uint32_t k = 0; k < ncores; k++) arrNum(a, s->core[k].power_mW);
            dictArr(d, "CorePower_mW", a);
        }
    }
    if (f & AMDZEN_F_SVI2) {
        dictNum(d, "Vcore_mV", s->vcore_mV);
        dictNum(d, "Vsoc_mV", s->vsoc_mV);
        dictNum(d, "Icore_mA", s->icore_mA);
        dictNum(d, "Isoc_mA", s->isoc_mA);
    }
    if ((f & AMDZEN_F_HWPSTATUS) && model.sviVidFormula) {
        if (OSArray *a = OSArray::withCapacity(ncores)) {
            for (uint32_t k = 0; k < ncores; k++) arrNum(a, s->core[k].vid_mV);
            dictArr(d, "CoreVID_mV", a);
        }
    }
    if (OSArray *a = OSArray::withCapacity(ncores)) {
        for (uint32_t k = 0; k < ncores; k++) arrNum(a, s->core[k].effFreqMHz);
        dictArr(d, "Frequency_MHz", a);
    }
    if (OSArray *a = OSArray::withCapacity(ncores)) {
        for (uint32_t k = 0; k < ncores; k++) arrNum(a, s->core[k].busyPermille);
        dictArr(d, "Busy_permille", a);
    }
    if (f & AMDZEN_F_IRPERF) {
        if (OSArray *a = OSArray::withCapacity(ncores)) {
            for (uint32_t k = 0; k < ncores; k++) arrNum(a, s->core[k].ipcX100);
            dictArr(d, "IPCx100", a);
        }
    }
    dictNum(d, "AvgFrequency_MHz", s->avgFreqMHz);
    dictNum(d, "MaxFrequency_MHz", s->maxFreqMHz);
    dictNum(d, "AvgBusy_permille", s->avgBusyPermille);
    dictStr(d, "Governor", zen::governorName(s->governor));
    dictBool(d, "Boost", s->boostEnabled != 0);
    dictNum(d, "PStateLimit", s->pstateLimit);
    dictNum(d, "LimitFast", s->limitFast);
    dictNum(d, "LimitSlow", s->limitSlow);
    if (backend == kAMDZenBackendCppcMsr || backend == kAMDZenBackendCppcPcc) dictNum(d, "EPP", s->epp);
    dictStr(d, "GovSource", govSource == zen::kGovSampleIdle ? "idle" :
                             (govSource == zen::kGovSamplePerf ? "aperf" : "hold"));
    dictNum(d, "GovRendezvousPerSec", rendezvousRate, 64);
    publishIdleSensors(d);
    setProperty("Sensors", d);
    d->release();
}

// ===========================================================================
// Sleep / wake
// ===========================================================================
IOReturn AMDZenPM::sleepWakeHandler(void *target, void *refCon, UInt32 messageType,
                                    IOService *provider, void *messageArgument, vm_size_t argSize) {
    (void)refCon; (void)provider; (void)messageArgument; (void)argSize;
    AMDZenPM *self = static_cast<AMDZenPM *>(target);
    if (!self) return kIOReturnSuccess;
    switch (messageType) {
    case kIOMessageSystemWillSleep:
    case kIOMessageSystemWillPowerOff:
    case kIOMessageSystemWillRestart:
        self->runGated(kGateSleep);
        self->nativeDrain(); // notifier acknowledges sleep only after PCC completes
        break;
    case kIOMessageSystemHasPoweredOn:
        self->runGated(kGateWake);
        break;
    default:
        break;
    }
    return kIOReturnSuccess;
}

void AMDZenPM::handleSleep() {
    nativeCancel(); // atomic cancellation before waiting for the current command
    if (sleeping) return;
    sleeping = true;
    resetGovernorSampling();
    if (timer) timer->cancelTimeout();
    if (!initialized) return;
    idleSleep();
    // Leave the legacy backend in its safe default (P0 requested on every thread).
    if (!nativeEnableMayBe() && !nativePending && backend == kAMDZenBackendLegacy && gControlTouched) {
        const uint32_t ncpu = minU(cpu.topo().ncpu, AMDZEN_MAX_CPUS);
        for (uint32_t c = 0; c < ncpu; c++) pstateReq[c] = 0;
        cpu.run(zen::kOpWritePState, nullptr, pstateReq);
    }
}

void AMDZenPM::handleWake() {
    if (!sleeping) return;
    sleeping = false;
    nativeResume();
    resetGovernorSampling();
    if (!initialized || initFailed) {
        if (timer && !initFailed) timer->setTimeoutMS(250);
        return;
    }
    idleWake();
    cpu.invalidateWriteCache();
    const bool control = !nativeEnableMayBe() && !blockReason &&
        backend != kAMDZenBackendNone && governor != kAMDZenGovNone;
    if (gIrperfByUs && control) cpu.run(zen::kOpHwcrModify, nullptr, nullptr, nullptr, HWCR_IRPERF_EN, 0);
    if (backend == kAMDZenBackendCppcPcc) nativeWake = true;
    if (control && backend == kAMDZenBackendCppcMsr && gCppcEnabled) {
        // Firmware resets CPPC_ENABLE across S3: P0 via PERF_CTL, then re-enable.
        const uint32_t ncpu = minU(cpu.topo().ncpu, AMDZEN_MAX_CPUS);
        for (uint32_t c = 0; c < ncpu; c++) pstateReq[c] = 0;
        cpu.run(zen::kOpWritePState | zen::kOpCppcEnable, nullptr, pstateReq);
    }
    applyPolicy(true);
    rebaseEnergy = true;
    if (timer) timer->setTimeoutMS(isDynamicGov(governor) && control ? governorPeriodMs : 100);
}

// ===========================================================================
// CC6 idle hook (ZenIdle.cpp / ZenIdleModel.cpp; docs/research/xnu-pm.md §9,
// amd-cstate.md §6). Runs on the work loop from tryInitHardware(): every CPU is
// online, so the capability rendezvous sees all of them.
// ===========================================================================
bool AMDZenPM::idlePmConflict(const char **which) {
    auto loaded = [](const char *name) -> bool {
        const OSSymbol *sym = OSSymbol::withCString(name);
        if (!sym) return true;                 // cannot tell: assume present
        const OSMetaClass *mc = OSMetaClass::getMetaClassWithName(sym);
        sym->release();
        return mc != nullptr;
    };
    // Stricter than detectConflicts(): a loaded class is enough. A PM kext may
    // register its table before (or without) publishing any IOService, and
    // pmKextRegister panics if one is there; no exported API can query pmDispatch.
    for (const char *n : kConflictClasses) if (loaded(n)) { *which = n; return true; }
    for (const char *n : kIdleConflictClasses) if (loaded(n)) { *which = n; return true; }
    return false;
}

void AMDZenPM::setupIdle() {
    // ---- capability probe: read-only, done even when the hook stays off because
    // the values are the diagnostics for "why not" (published in CPUInfo).
    idleCaps = zen::IdleCaps {};
    // C001_0292 PMGT_MISC is package scope: one careful read on this CPU (PC6En).
    havePmgtMisc = zen_rdmsr_safe(MSR_PMGT_MISC, &pmgtMisc) == 0;
    if (zen::IdleMsrSample *ms = ZALLOC(zen::IdleMsrSample, AMDZEN_MAX_CPUS)) {
        const uint32_t n = cpu.readIdleMsrs(ms);
        idleCaps = zen::aggregateIdleMsrs(ms, n);
        ZFREE(ms, zen::IdleMsrSample, AMDZEN_MAX_CPUS);
    }
    haveCst = false;
    cstCount = 0;
    cstDevice[0] = '\0';
    // _CST is AML executed by AppleACPIPlatform: skip it when the user turned the
    // hook off (amdzen.idle=0 is the "boot hangs" escape hatch).
    if (idleRequest != zen::kIdleOff)
        haveCst = zen::idle::readAcpiCst(cst, zen::kMaxCstEntries, &cstCount, cstDevice, sizeof(cstDevice));
    idlePort = zen::chooseIdlePort(idleCaps, cst, cstCount, haveCst);

    // ---- CPUID gates
    uint32_t r[4];
    zen_cpuid(0, 0, r);
    const uint32_t maxStd = r[0];
    zen_cpuid(1, 0, r);
    const bool hv = (r[2] & CPUID1_ECX_HYPERVISOR) != 0;
    const bool mwaitCpuid = (r[2] & CPUID1_ECX_MWAIT) != 0;
    bool mwaitExt = false, arat = false;
    if (maxStd >= 5) {
        zen_cpuid(5, 0, r);
        mwaitExt = (r[2] & (CPUID5_ECX_EMX | CPUID5_ECX_IBE)) == (CPUID5_ECX_EMX | CPUID5_ECX_IBE);
    }
    if (maxStd >= 6) {
        zen_cpuid(6, 0, r);
        arat = (r[0] & CPUID6_EAX_ARAT) != 0;
    }
    // ECX[0]=1 (wake on a masked interrupt) is what makes MWAIT safe with IF=0.
    idleMwait = mwaitCpuid && mwaitExt && idleCaps.mwaitAllowed;

    const char *conflict = nullptr;
    const bool pmConflict = idlePmConflict(&conflict);
    // Extra gate (xnu-pm.md §6.1): XCPM installs its own pmDispatch. It never runs on
    // AMD (model check in xcpm_bootstrap), but if it ever did, registering would panic.
    int xcpm = 0;
    size_t xlen = sizeof(xcpm);
    const bool xcpmActive = sysctlbyname("machdep.xcpm.mode", &xcpm, &xlen, nullptr, 0) == 0 && xcpm != 0;

    zen::IdleGateInput in {};
    in.requested = idleRequest;
    in.darwinMajor = version_major;
    in.hypervisor = hv;
    in.blocked = blockReason != kAMDZenBlockNone;
    in.pmConflict = pmConflict;
    in.xcpmActive = xcpmActive;
    in.arat = arat;
    in.tscInvariant = (info->features & AMDZEN_F_TSC_INVARIANT) != 0;
    // Without sleep/wake notifications nothing demotes the hook before S3 or
    // re-checks the MSRs after it (firmware re-initialises the APs; HWCR[9] or the
    // C-state trap could come back different), so only HLT is safe then.
    const bool haveSleepHooks = sleepNotifier != nullptr;
    if (!haveSleepHooks) idleMwait = false;
    in.mwait = idleMwait;
    // The predictor threshold is TSC-based: without a TSC rate there is no "long idle".
    in.cc6 = idlePort.ok && cpu.tscHz() != 0 && haveSleepHooks;
    const zen::IdleGateResult g = zen::evaluateIdleGates(in);

    zen::idle::HookConfig hc {};
    hc.mode = g.mode;
    hc.maxMode = g.maxMode;
    hc.port = in.cc6 ? (uint16_t)idlePort.port : 0;
    hc.mwait = idleMwait;
    hc.thresholdTicks = zen::usToTscTicks(idleThresholdUs, cpu.tscHz());
    hc.thresholdNs = (uint64_t)idleThresholdUs * 1000ull;
    hc.pollTicks = zen::usToTscTicks(3, cpu.tscHz());
    hc.pollEwmaMax = zen::usToTscTicks(15, cpu.tscHz());
    hc.ncpu = minU(cpu.topo().ncpu, AMDZEN_MAX_CPUS);
    hc.deepLatencyNs = (uint64_t)zen::idleDeepLatencyUs(cst, cstCount, idlePort.cstIndex) * 1000ull;

    if (info->features & AMDZEN_F_PREFCORE) {
        const zen::Topology &tp = cpu.topo();
        uint16_t coreRank[AMDZEN_MAX_CORES] {};
        for (uint32_t k = 0; k < tp.ncores && k < AMDZEN_MAX_CORES; ++k)
            coreRank[k] = info->cppcRank[k];
        zen::idle::setPreferredCores(tp.primaryCpuOfCore, tp.coreOfCpu, coreRank,
                                     tp.ncpu, tp.ncores);
    }

    if (zen::idle::registered()) {
        // An earlier instance registered this boot (start/stop/start in the dev
        // workflow): the table is still the kernel's, re-arm it instead.
        if (!g.doRegister) hc.mode = zen::kIdleOff;
        zen::idle::rearm(hc);
        idleReason = zen::kIdleRegistered;
    } else if (g.doRegister) {
        idleReason = zen::idle::registerHook(hc) ? zen::kIdleRegistered : zen::kIdleNotEvaluated;
    } else {
        idleReason = g.reason;
    }
    idleSleepDemoted = false;
    idleWakeLost = false;
    idlePrevValid = false;

    LOG("idle: CStateBaseAddr 0x%llx CStateCfg 0x%llx HWCR.INVDWBINVD %d, _CST %s (%u entries, %s), "
        "CC6 %s port 0x%x CCR%u",
        idleCaps.baseRaw, idleCaps.cfg, idleCaps.invdWbinvd ? 1 : 0, haveCst ? "found" : "absent",
        cstCount, cstDevice[0] ? cstDevice : "-", zen::cc6ReasonName(idlePort.reason), idlePort.port,
        idlePort.ccr);
    if (idleReason == zen::kIdleRegistered)
        LOG("idle: hook registered, mode %s (max %s), threshold %u us",
            zen::idleModeName(zen::idle::mode()), zen::idleModeName(zen::idle::maxMode()), idleThresholdUs);
    else
        LOG("idle: hook not registered: %s%s%s", zen::idleHookReasonName(idleReason),
            idleReason == zen::kIdlePmConflict ? " " : "",
            idleReason == zen::kIdlePmConflict && conflict ? conflict : "");
}

// Before S3: CStateAddr/CStateCfg and HWCR are restored by firmware on resume, but
// that is not verified on this board, and CPUs run machineIdle() right after wake,
// before kIOMessageSystemHasPoweredOn reaches us. An IN to base+k while the trap is
// not programmed would be a real IO access, and MONITOR with HWCR[9] MonMwaitDis set
// is #UD (a kernel panic), so use plain HLT until idleWake() has re-checked both.
void AMDZenPM::idleSleep() {
    if (!zen::idle::registered() || idleSleepDemoted) return;
    idleSavedMode = zen::idle::mode();
    if (idleSavedMode > zen::kIdleHlt) zen::idle::setMode(zen::kIdleHlt);
    idleSleepDemoted = true;
}

void AMDZenPM::idleWake() {
    if (!zen::idle::registered() || !idleSleepDemoted) return;
    idleSleepDemoted = false;
    idlePrevValid = false;      // the interval spans the sleep
    zen::IdleCaps now {};
    if (zen::idle::maxMode() > zen::kIdleHlt) {
        if (zen::IdleMsrSample *ms = ZALLOC(zen::IdleMsrSample, AMDZEN_MAX_CPUS)) {
            now = zen::aggregateIdleMsrs(ms, cpu.readIdleMsrs(ms));
            ZFREE(ms, zen::IdleMsrSample, AMDZEN_MAX_CPUS);
        }
    }
    if (idleMwait && !now.mwaitAllowed) {
        // HWCR[9] set (or unreadable) on some CPU after resume: MONITOR would #UD.
        LOG("idle: MONITOR/MWAIT not allowed after wake (HWCR[9] set or unreadable); HLT only until reboot");
        idleMwait = false;
        zen::idle::setMwait(false);
        zen::idle::setMaxMode(zen::kIdleHlt);
        idleWakeLost = true;
        removeProperty("CPUInfo");
    }
    if (zen::idle::maxMode() == zen::kIdleCc6) {
        uint32_t base = 0;
        const bool same = now.msrOk && now.baseUniform && now.baseRaw == idleCaps.baseRaw &&
                          zen::checkCStateBase(now.baseRaw, &base) == zen::kCc6Ok &&
                          zen::cstateCfgCc6En(now.cfgCc6And, idlePort.ccr) && now.invdWbinvd;
        if (!same) {
            LOG("idle: C-state MSRs changed across sleep (CStateBaseAddr 0x%llx -> 0x%llx, CStateCfg "
                "0x%llx); CC6 disabled until reboot", idleCaps.baseRaw, now.baseRaw, now.cfg);
            zen::idle::setMaxMode(zen::clampIdleMode(zen::kIdleMwait, idleMwait, false));
            idleWakeLost = true;
            removeProperty("CPUInfo");   // republished with IdleCC6 = lost-after-wake
        }
    }
    zen::idle::setMode(zen::clampIdleMode(idleSavedMode, idleMwait, zen::idle::maxMode() == zen::kIdleCc6));
}

void AMDZenPM::idleTelemetry(uint64_t nowNs) {
    if (!zen::idle::registered()) return;
    const uint32_t ncpu = minU(cpu.topo().ncpu, AMDZEN_MAX_CPUS);
    zen::IdleTotals t;
    zen::idle::totals(&t, ncpu);
    const uint64_t tsc = zen_rdtsc();
    if (idlePrevValid && tsc > idlePrevTsc && nowNs > idlePrevNs) {
        const uint64_t dTsc = tsc - idlePrevTsc, dNs = nowNs - idlePrevNs;
        idleCc6Permille = zen::residencyPermille(t.deepTicks - idlePrev.deepTicks, ncpu, dTsc);
        idleC1Permille = zen::residencyPermille(t.shallowTicks - idlePrev.shallowTicks, ncpu, dTsc);
        idleDeepRate = zen::ratePerSecond(t.deepEntries - idlePrev.deepEntries, dNs);
        idleShallowRate = zen::ratePerSecond(t.shallowEntries - idlePrev.shallowEntries, dNs);
        idleAllDeepPermille = zen::residencyPermille(t.allDeepTicks - idlePrev.allDeepTicks, 1, dTsc);
        idleAllDeepRate = zen::ratePerSecond(t.allDeepEvents - idlePrev.allDeepEvents, dNs);
        idleVetoRate = zen::ratePerSecond(t.latencyVetoes - idlePrev.latencyVetoes, dNs);
        idleMonitorWakeRate = zen::ratePerSecond(t.monitorWakes - idlePrev.monitorWakes, dNs);
        idleTimerMigrateRate = zen::ratePerSecond(t.timerMigrates - idlePrev.timerMigrates, dNs);
    } else {
        idleCc6Permille = idleC1Permille = 0;
        idleDeepRate = idleShallowRate = 0;
        idleAllDeepPermille = 0;
        idleAllDeepRate = idleVetoRate = 0;
        idleMonitorWakeRate = idleTimerMigrateRate = 0;
    }
    idlePrev = t;
    idlePrevTsc = tsc;
    idlePrevNs = nowNs;
    idlePrevValid = true;
}

void AMDZenPM::publishIdleInfo(OSDictionary *d) {
    const bool reg = zen::idle::registered();
    dictStr(d, "IdleHook", zen::idleHookReasonName(idleReason));
    dictStr(d, "IdleModeRequested", zen::idleModeName(idleRequest));
    dictStr(d, "IdleModeMax", reg ? zen::idleModeName(zen::idle::maxMode()) : "off");
    dictBool(d, "IdleMwait", idleMwait);
    dictNum(d, "CStateBaseAddr", idleCaps.baseRaw, 64);
    dictNum(d, "CStateCfg", idleCaps.cfg, 64);
    dictNum(d, "CStatePolicy", idleCaps.policy, 64);
    dictBool(d, "CStateCfgUniform", idleCaps.cfgUniform);
    if (havePmgtMisc) {
        dictNum(d, "PmgtMisc", pmgtMisc, 64);
        dictBool(d, "PC6Enabled", (pmgtMisc >> 32) & 1);
    }
    dictStr(d, "IdleCC6", idleWakeLost ? "lost-after-wake" : zen::cc6ReasonName(idlePort.reason));
    dictNum(d, "IdlePort", idlePort.port);
    dictNum(d, "IdleCcr", idlePort.ccr);
    dictStr(d, "IdlePortSource", zen::idlePortSourceName(idlePort.source));
    if (idleRequest == zen::kIdleOff) {
        dictStr(d, "AcpiCSTStatus", "not-evaluated");
    } else if (!haveCst) {
        dictStr(d, "AcpiCSTStatus", "not-found");
    } else {
        dictStr(d, "AcpiCSTStatus", "ok");
        dictStr(d, "AcpiCSTDevice", cstDevice);
        if (OSArray *a = OSArray::withCapacity(cstCount ? cstCount : 1)) {
            for (uint32_t i = 0; i < cstCount && i < zen::kMaxCstEntries; i++) {
                if (OSDictionary *e = OSDictionary::withCapacity(4)) {
                    dictNum(e, "Type", cst[i].type);
                    dictNum(e, "Space", cst[i].reg.spaceId);
                    dictNum(e, "Port", cst[i].reg.address, 64);
                    dictNum(e, "Latency", cst[i].latencyUs);
                    a->setObject(e);
                    e->release();
                }
            }
            dictArr(d, "AcpiCST", a);
        }
    }
}

void AMDZenPM::publishIdleSensors(OSDictionary *d) {
    if (!zen::idle::registered()) return;
    dictNum(d, "IdleMode", zen::idle::mode());
    dictNum(d, "IdleThresholdUs", idleThresholdUs);
    dictNum(d, "CC6Residency_permille", idleCc6Permille);
    dictNum(d, "C1Residency_permille", idleC1Permille);
    dictNum(d, "IdleEntriesDeep", idleDeepRate, 64);       // per second
    dictNum(d, "IdleEntriesShallow", idleShallowRate, 64); // per second
    // Share of time every CPU sat in CC6 at once: the window in which the package
    // can enter PC6. Near 0 with high CC6Residency means wake-ups are not aligned.
    dictNum(d, "AllCoresCC6_permille", idleAllDeepPermille);
    dictNum(d, "AllCoresCC6Entries", idleAllDeepRate, 64);   // per second
    uint64_t latInt = 0, latBus = 0;
    zen::idle::latencyRequests(&latInt, &latBus);
    dictNum(d, "LatencyRequestInt_ns", latInt, 64);
    dictNum(d, "LatencyRequestBus_ns", latBus, 64);
    dictNum(d, "LatencyVetoes", idleVetoRate, 64);           // per second
    dictNum(d, "IdleMonitorWakes", idleMonitorWakeRate, 64); // per second (IPI avoided)
    dictNum(d, "IdleTimerMigrates", idleTimerMigrateRate, 64); // per second (timers -> CPU0)
}

// ===========================================================================
// Gated command dispatch
// ===========================================================================
IOReturn AMDZenPM::runGated(uint32_t op, void *a1, void *a2, void *a3) {
    if (!gate) return kIOReturnNotReady;
    const IOReturn ret = gate->runAction(&AMDZenPM::gatedAction, (void *)(uintptr_t)op, a1, a2, a3);
    // The notification cannot complete its handoff until an admitted command
    // finishes. Never wait under the gate: the worker may need it to publish.
    if (op == kGateConflict && !forceControl) nativeDrainCommands();
    return ret;
}

IOReturn AMDZenPM::gatedAction(OSObject *owner, void *opArg, void *a1, void *a2, void *a3) {
    AMDZenPM *self = OSDynamicCast(AMDZenPM, owner);
    if (!self || !self->info) return kIOReturnNotReady;
    const uint32_t op = (uint32_t)(uintptr_t)opArg;
    if ((!self->initialized || self->initFailed) &&
        op != kGateSleep && op != kGateWake && op != kGateStop && op != kGateCopyTunables &&
        op != kGateSetTunables)
        return kIOReturnNotReady;
    AMDZenInfo *in = self->info;

    // Reads and lifecycle first (allowed while blocked)
    switch (op) {
    case kGateCopyInfo:
        memcpy(a1, in, sizeof(AMDZenInfo));
        return kIOReturnSuccess;
    case kGateCopySample:
        memcpy(a1, self->sample, sizeof(AMDZenSample));
        return kIOReturnSuccess;
    case kGateCopyTunables: {
        AMDZenTunables *t = static_cast<AMDZenTunables *>(a1);
        memset(t, 0, sizeof(*t));
        t->abiVersion = AMDZEN_ABI_VERSION;
        t->structSize = sizeof(AMDZenTunables);
        t->governorPeriodMs = self->governorPeriodMs;
        t->telemetryPeriodMs = self->telemetryPeriodMs;
        t->upThreshold = self->tun.upThreshold;
        t->downThreshold = self->tun.downThreshold;
        t->samplingDownFactor = self->tun.samplingDownFactor;
        t->freqStepPct = self->tun.freqStepPct;
        t->utilHeadroomPct = self->tun.utilHeadroomPct;
        t->utilEwmaShift = self->tun.utilEwmaShift;
        t->tjMax_mC = self->tjMax_mC;
        return kIOReturnSuccess;
    }
    case kGateSetTunables: {
        // Tunables do not touch hardware, so they are accepted in monitor-only mode.
        const AMDZenTunables *t = static_cast<const AMDZenTunables *>(a1);
        if (t->governorPeriodMs < 4 || t->governorPeriodMs > 1000) return kIOReturnBadArgument;
        if (t->telemetryPeriodMs < 100 || t->telemetryPeriodMs > 10000) return kIOReturnBadArgument;
        if (t->upThreshold < 1 || t->upThreshold > 100) return kIOReturnBadArgument;
        if (t->downThreshold >= t->upThreshold) return kIOReturnBadArgument;
        if (t->samplingDownFactor < 1 || t->samplingDownFactor > 100) return kIOReturnBadArgument;
        if (t->freqStepPct > 100) return kIOReturnBadArgument;
        if (t->utilHeadroomPct < 100 || t->utilHeadroomPct > 200) return kIOReturnBadArgument;
        if (t->utilEwmaShift > 8) return kIOReturnBadArgument;
        if (t->tjMax_mC && (t->tjMax_mC < 50000 || t->tjMax_mC > 150000)) return kIOReturnBadArgument;
        self->governorPeriodMs = t->governorPeriodMs;
        self->telemetryPeriodMs = t->telemetryPeriodMs;
        self->tun.upThreshold = t->upThreshold;
        self->tun.downThreshold = t->downThreshold;
        self->tun.samplingDownFactor = t->samplingDownFactor;
        self->tun.freqStepPct = t->freqStepPct;
        self->tun.utilHeadroomPct = t->utilHeadroomPct;
        self->tun.utilEwmaShift = t->utilEwmaShift;
        if (t->tjMax_mC) {
            self->tjMax_mC = t->tjMax_mC;
            in->tjMax_mC = t->tjMax_mC;
        }
        if (self->timer && !self->sleeping) self->timer->setTimeoutMS(10);
        return kIOReturnSuccess;
    }
    case kGateConflict:
        if (self->blockReason == kAMDZenBlockNone && !self->forceControl) {
            self->nativeCancel(true); // revoke BEFORE publishing the new owner
            LOG("conflicting driver %s appeared; switching to monitor-only",
                a1 ? static_cast<const char *>(a1) : "?");
            self->restoreHardware();
            self->blockReason = kAMDZenBlockConflict;
            in->controlBlockReason = kAMDZenBlockConflict;
            if (!self->allowSmnShared) {
                in->features &= ~(uint64_t)(AMDZEN_F_SMN | AMDZEN_F_TCTL | AMDZEN_F_CCD_TEMP | AMDZEN_F_SVI2);
                in->ccdTempMask = 0;
                LOG("SMN sensors disabled (shared index/data pair); amdzen.smn=1 overrides");
            }
            self->removeProperty("CPUInfo");
            self->publishProperties();
        }
        return kIOReturnSuccess;
    case kGateApply:
        self->applyPolicy(a1 != nullptr);
        return kIOReturnSuccess;
    case kGateSleep:
        self->handleSleep();
        return kIOReturnSuccess;
    case kGateWake:
        self->handleWake();
        return kIOReturnSuccess;
    case kGateStop:
        self->nativeCancel();
        self->nativeStopped = true;
        self->sleeping = true;
        if (self->timer) self->timer->cancelTimeout();
        self->restoreHardware();
        return kIOReturnSuccess;
    case kGateSetIdleMode: {
        // Our own hook, not a control MSR: allowed in monitor-only mode too.
        if (!zen::idle::registered()) return kIOReturnUnsupported;
        uint32_t m = (uint32_t)(uintptr_t)a1;
        if (m >= zen::kIdleModeCount) return kIOReturnBadArgument;
        m = zen::clampIdleMode(m, self->idleMwait, zen::idle::maxMode() == zen::kIdleCc6);
        if (self->idleSleepDemoted) self->idleSavedMode = m;   // applied by idleWake()
        else zen::idle::setMode(m);
        LOG("idle mode -> %s", zen::idleModeName(m));
        return kIOReturnSuccess;
    }
    case kGateSetGovKnob: {
        // Software-only knobs: allowed in monitor-only mode too.
        const uint32_t which = (uint32_t)(uintptr_t)a1, v = (uint32_t)(uintptr_t)a2;
        if (which == kKnobFastUpPct) {
            if (v > 100) return kIOReturnBadArgument;
            __atomic_store_n(&self->tun.fastUpPct, v, __ATOMIC_RELAXED);
        } else if (which == kKnobGovIdleSampling) {
            if (v > 1) return kIOReturnBadArgument;
            __atomic_store_n(&self->govIdleSampling, v, __ATOMIC_RELAXED);
            self->idleSnapValid = false;
        } else {
            return kIOReturnBadArgument;
        }
        return kIOReturnSuccess;
    }
    case kGateSetIdleThreshold: {
        uint32_t us = (uint32_t)(uintptr_t)a1;
        if (us > zen::kIdleThresholdMaxUs) return kIOReturnBadArgument;
        self->idleThresholdExplicit = true;
        __atomic_store_n(&self->idleThresholdUs, us, __ATOMIC_RELAXED);   // read lock-free by sysctl
        if (zen::idle::registered())
            zen::idle::setThresholdTicks(zen::usToTscTicks(us, self->cpu.tscHz()),
                                         (uint64_t)us * 1000ull);
        return kIOReturnSuccess;
    }
    case kGateSetLpm: {
        int32_t m = (int32_t)(intptr_t)a1;
        if (m < -1 || m > 1) return kIOReturnBadArgument;
        __atomic_store_n(&self->lpmMode, m, __ATOMIC_RELAXED);
        if (self->backend == kAMDZenBackendCppcPcc || self->backend == kAMDZenBackendCppcMsr) self->nativeNudge();
        return kIOReturnSuccess;
    }
    case kGateSetAcPower: {
        int32_t m = (int32_t)(intptr_t)a1;
        if (m < -1 || m > 1) return kIOReturnBadArgument;
        __atomic_store_n(&self->acPowerMode, m, __ATOMIC_RELAXED);
        if (self->backend == kAMDZenBackendCppcPcc || self->backend == kAMDZenBackendCppcMsr) self->nativeNudge();
        return kIOReturnSuccess;
    }
    case kGateSetThermalCap: {
        uint32_t m = (uint32_t)(uintptr_t)a1;
        if (m > zennative::kThermalCapAuto) return kIOReturnBadArgument;
        __atomic_store_n(&self->thermalCapMode, m, __ATOMIC_RELAXED);
        if (self->backend == kAMDZenBackendCppcPcc || self->backend == kAMDZenBackendCppcMsr) self->nativeNudge();
        return kIOReturnSuccess;
    }
    case kGateSetProfile: {
        uint32_t req = (uint32_t)(uintptr_t)a1;
        if (req >= zennative::kProfileCount) return kIOReturnBadArgument;
        __atomic_store_n(&self->profileRequested, req, __ATOMIC_RELAXED);
        const zennative::CpuProfile eff = (req == zennative::kProfileAuto)
            ? zennative::detectCpuProfile(in->family, in->model, in->brand, in->physicalCores)
            : static_cast<zennative::CpuProfile>(req);
        __atomic_store_n(&self->profileActive, static_cast<uint32_t>(eff), __ATOMIC_RELAXED);
        if (!self->eppExplicit) self->epp = zennative::tunablesForProfile(eff).baseEpp;
        if (!self->idleThresholdExplicit) {
            const uint32_t us = zennative::recommendedIdleThresholdUs(eff);
            __atomic_store_n(&self->idleThresholdUs, us, __ATOMIC_RELAXED);
            if (zen::idle::registered())
                zen::idle::setThresholdTicks(zen::usToTscTicks(us, self->cpu.tscHz()),
                                             (uint64_t)us * 1000ull);
        }
        self->removeProperty("CPUInfo");
        self->publishProperties();
        if (self->backend == kAMDZenBackendCppcPcc || self->backend == kAMDZenBackendCppcMsr) self->nativeNudge();
        LOG("profile -> %s (requested %s)", zennative::profileName(eff),
            zennative::profileName(static_cast<zennative::CpuProfile>(req)));
        return kIOReturnSuccess;
    }
    default:
        break;
    }

    // ---- control setters
    if (self->blockReason != kAMDZenBlockNone) return kIOReturnNotPermitted;
    if (nativeEnableMayBe() && self->backend != kAMDZenBackendCppcPcc) return kIOReturnNotPermitted;
    if (self->backend == kAMDZenBackendNone) return kIOReturnUnsupported;
    const bool pcc = self->backend == kAMDZenBackendCppcPcc;
    const bool cppc = pcc || self->backend == kAMDZenBackendCppcMsr;
    if (self->nativePending) return kIOReturnNotReady;
    // PCC is autonomous only; neither legacy governors nor P-state definitions
    // can control a CPU once CPPC has been enabled until reset.
    if (pcc && (op == kGateSetUser || op == kGateWriteDef)) return kIOReturnUnsupported;
    const uint32_t maxIdx = legacyMaxIndex(in);

    switch (op) {
    case kGateSetGovernor: {
        uint32_t g = (uint32_t)(uintptr_t)a1;
        if (g >= kAMDZenGovCount) return kIOReturnBadArgument;
        if (pcc && g != kAMDZenGovCppcActive && g != kAMDZenGovPerformance &&
            g != kAMDZenGovPowersave && g != kAMDZenGovNone) return kIOReturnUnsupported;
        if (g == kAMDZenGovCppcActive && !cppc) return kIOReturnUnsupported;
        if (g == kAMDZenGovNone) {
            self->restoreHardware();
            self->governor = g;
            self->resetGovernorSampling();
        } else {
            self->governor = g;
            self->applyPolicy(true);
        }
        if (pcc || (self->backend == kAMDZenBackendCppcMsr && g == kAMDZenGovCppcActive)) self->nativeNudge();
        else if (self->timer && !self->sleeping) self->timer->setTimeoutMS(isDynamicGov(g) ? self->governorPeriodMs : 10);
        LOG("governor -> %s", zen::governorName(g));
        return kIOReturnSuccess;
    }
    case kGateSetBoostMode: {
        int32_t m = (int32_t)(intptr_t)a1;
        if (m < -1 || m > 1) return kIOReturnBadArgument;
        if (!(in->features & AMDZEN_F_CPB)) return kIOReturnUnsupported;
        if (m < 0) {
            if (!pcc && gBoostTouched) {
                self->cpu.run(zen::kOpRestoreBootBoost, nullptr);
                gBoostTouched = false;
            }
            self->boostMode = -1;
            if (cppc) self->applyPolicy(false);   // max perf cap follows boot state again
        } else {
            self->boostMode = m;
            self->applyPolicy(false);
        }
        if (pcc || self->backend == kAMDZenBackendCppcMsr) self->nativeNudge();
        self->setProperty("BoostMode", (unsigned long long)(uint32_t)self->boostMode, 32);
        return kIOReturnSuccess;
    }
    case kGateSetLimits: {
        uint32_t fast = (uint32_t)(uintptr_t)a1, slow = (uint32_t)(uintptr_t)a2;
        if (cppc) {
            if (fast > 255 || slow > 255) return kIOReturnBadArgument;
            if (fast && slow && fast > slow) return kIOReturnBadArgument;
        } else {
            if (fast > slow || slow > maxIdx) return kIOReturnBadArgument;
        }
        self->limitFast = fast;
        self->limitSlow = slow;
        self->applyPolicy(false);
        if (pcc || self->backend == kAMDZenBackendCppcMsr) self->nativeNudge();
        return kIOReturnSuccess;
    }
    case kGateSetEpp: {
        uint32_t e = (uint32_t)(uintptr_t)a1;
        if (e > 255) return kIOReturnBadArgument;
        if (!cppc) return kIOReturnUnsupported;
        self->epp = e;
        self->eppExplicit = true;
        self->applyPolicy(false);
        if (pcc || self->backend == kAMDZenBackendCppcMsr) self->nativeNudge();
        return kIOReturnSuccess;
    }
    case kGateSetUser: {
        uint32_t v = (uint32_t)(uintptr_t)a1;
        if (cppc ? v > 255 : v > maxIdx) return kIOReturnBadArgument;
        self->userValue = v;
        self->setProperty("UserValue", (unsigned long long)v, 32);
        if (self->governor == kAMDZenGovUserspace) self->applyPolicy(false);
        return kIOReturnSuccess;
    }
    case kGateWriteDef: {
        uint32_t idx = (uint32_t)(uintptr_t)a1;
        uint32_t fd = (uint32_t)(uintptr_t)a2;
        uint32_t fid = fd & 0xFFFF, did = fd >> 16;
        uint32_t vid = (uint32_t)(uintptr_t)a3;
        if (!self->allowPStateWrite) return kIOReturnNotPermitted;
        if (!self->model.sviVidFormula || self->model.pstateDef1Ah ||
            (in->family != 0x17 && in->family != 0x19))
            return kIOReturnUnsupported;
        if (idx >= AMDZEN_MAX_PSTATES || idx > in->pstateMaxVal) return kIOReturnBadArgument;
        if (fid == 0 || fid > 0xFF || did == 0 || vid > 0xFF) return kIOReturnBadArgument;
        // Redefining P0 changes the TSC/MPERF rate XNU calibrated at boot unless the
        // firmware set HWCR[21] LockTscToCurrentP0 (it only takes effect from reset).
        if (idx == 0 && !(self->cpu.hwcrInitial() & HWCR_LOCK_TSC_TO_P0)) {
            LOG("refusing to redefine P0: HWCR.LockTscToCurrentP0 is clear");
            return kIOReturnNotPermitted;
        }
        uint32_t freq = 200u * fid / did;
        if (freq < 400 || freq > 6000) return kIOReturnBadArgument;
        uint32_t mV = zen::sviVidToMilliVolt(vid);
        if (mV < 700 || mV > 1450) return kIOReturnBadArgument;
        uint64_t raw = 0, newRaw = 0;
        if (!self->cpu.readPStateDef(idx, &raw)) return kIOReturnIOError;
        if (!zen::encodePStateDef(raw, fid, did, vid, &newRaw)) return kIOReturnBadArgument;
        self->cpu.run(zen::kOpWritePStateDef, nullptr, nullptr, nullptr, 0, 0, idx, newRaw);
        LOG("P%u definition %#llx -> %#llx (%u MHz, %u mV)", idx, raw, newRaw, freq, mV);
        uint64_t back = 0;
        if (!self->cpu.readPStateDef(idx, &back)) return kIOReturnIOError;
        zen::PStateDecoded d = zen::decodePStateDef(back, self->model.pstateDef1Ah, self->model.sviVidFormula);
        AMDZenPStateDef &p = in->pstates[idx];
        p.enabled = d.enabled;
        p.freqMHz = d.freqMHz;
        p.fid = d.fid;
        p.did = d.did;
        p.vid = d.vid;
        p.voltage_mV = d.voltage_mV;
        p.iddValue = d.iddValue;
        p.iddDiv = d.iddDiv;
        p.raw = back;
        if (idx == 0 && d.freqMHz) in->baseFreqMHz = d.freqMHz;
        // A new definition of the P-state a core currently runs takes effect only on the
        // next transition: bounce every thread through a neighbouring enabled P-state and
        // back to its current request.
        {
            const uint32_t ncpu = minU(self->cpu.topo().ncpu, AMDZEN_MAX_CPUS);
            uint32_t alt = enabledAtOrFaster(in, idx == 0 ? minU(1, legacyMaxIndex(in)) : idx - 1);
            if (alt != idx) {
                uint8_t saved[AMDZEN_MAX_CPUS];
                memcpy(saved, self->pstateReq, ncpu);
                for (uint32_t c = 0; c < ncpu; c++) self->pstateReq[c] = (uint8_t)alt;
                self->cpu.run(zen::kOpWritePState, nullptr, self->pstateReq);
                memcpy(self->pstateReq, saved, ncpu);
                self->cpu.invalidateWriteCache();
                self->cpu.run(zen::kOpWritePState, nullptr, self->pstateReq);
                gControlTouched = true;
            }
        }
        self->removeProperty("CPUInfo");
        self->publishProperties();
        return back == newRaw ? kIOReturnSuccess : kIOReturnIOError;
    }
    default:
        return kIOReturnUnsupported;
    }
}

// ===========================================================================
// Public API (thread context)
// ===========================================================================
IOReturn AMDZenPM::copyInfo(AMDZenInfo *out) {
    if (!out) return kIOReturnBadArgument;
    return runGated(kGateCopyInfo, out);
}

IOReturn AMDZenPM::copySample(AMDZenSample *out) {
    if (!out) return kIOReturnBadArgument;
    return runGated(kGateCopySample, out);
}

IOReturn AMDZenPM::copyTunables(AMDZenTunables *out) {
    if (!out) return kIOReturnBadArgument;
    return runGated(kGateCopyTunables, out);
}

IOReturn AMDZenPM::setTunables(const AMDZenTunables *in) {
    if (!in) return kIOReturnBadArgument;
    if (in->abiVersion != AMDZEN_ABI_VERSION || in->structSize != sizeof(AMDZenTunables))
        return kIOReturnBadArgument;
    return runGated(kGateSetTunables, const_cast<AMDZenTunables *>(in));
}

IOReturn AMDZenPM::setGovernor(uint32_t gov) {
    if (gov >= kAMDZenGovCount) return kIOReturnBadArgument;
    return runGated(kGateSetGovernor, (void *)(uintptr_t)gov);
}

IOReturn AMDZenPM::setBoost(bool on) {
    return runGated(kGateSetBoostMode, (void *)(intptr_t)(on ? 1 : 0));
}

IOReturn AMDZenPM::setPerfLimits(uint32_t fast, uint32_t slow) {
    if (fast > 255 || slow > 255) return kIOReturnBadArgument;
    return runGated(kGateSetLimits, (void *)(uintptr_t)fast, (void *)(uintptr_t)slow);
}

IOReturn AMDZenPM::setEPP(uint32_t value) {
    if (value > 255) return kIOReturnBadArgument;
    return runGated(kGateSetEpp, (void *)(uintptr_t)value);
}

IOReturn AMDZenPM::setUserPState(uint32_t v) {
    if (v > 255) return kIOReturnBadArgument;
    return runGated(kGateSetUser, (void *)(uintptr_t)v);
}

IOReturn AMDZenPM::writePStateDef(uint32_t idx, uint32_t fid, uint32_t did, uint32_t vid) {
    if (idx >= AMDZEN_MAX_PSTATES || fid > 0xFF || did > 0xFF || vid > 0xFF) return kIOReturnBadArgument;
    return runGated(kGateWriteDef, (void *)(uintptr_t)idx, (void *)(uintptr_t)(fid | (did << 16)),
                    (void *)(uintptr_t)vid);
}

IOReturn AMDZenPM::setIdleMode(uint32_t mode) {
    if (mode >= zen::kIdleModeCount) return kIOReturnBadArgument;
    return runGated(kGateSetIdleMode, (void *)(uintptr_t)mode);
}

IOReturn AMDZenPM::setIdleThresholdUs(uint32_t us) {
    if (us > zen::kIdleThresholdMaxUs) return kIOReturnBadArgument;
    return runGated(kGateSetIdleThreshold, (void *)(uintptr_t)us);
}

IOReturn AMDZenPM::setGovKnob(uint32_t which, uint32_t value) {
    return runGated(kGateSetGovKnob, (void *)(uintptr_t)which, (void *)(uintptr_t)value);
}

uint32_t AMDZenPM::getGovKnob(uint32_t which) {
    if (which == kKnobFastUpPct) return __atomic_load_n(&tun.fastUpPct, __ATOMIC_RELAXED);
    if (which == kKnobGovIdleSampling) return __atomic_load_n(&govIdleSampling, __ATOMIC_RELAXED);
    return 0;
}

IOReturn AMDZenPM::setLowPowerMode(int32_t mode) {
    if (mode < -1 || mode > 1) return kIOReturnBadArgument;
    return runGated(kGateSetLpm, (void *)(intptr_t)mode);
}

int32_t AMDZenPM::getLowPowerMode() {
    return __atomic_load_n(&lpmMode, __ATOMIC_RELAXED);
}

IOReturn AMDZenPM::setAcPowerMode(int32_t mode) {
    if (mode < -1 || mode > 1) return kIOReturnBadArgument;
    return runGated(kGateSetAcPower, (void *)(intptr_t)mode);
}

int32_t AMDZenPM::getAcPowerMode() {
    return __atomic_load_n(&acPowerMode, __ATOMIC_RELAXED);
}

IOReturn AMDZenPM::setThermalCapMode(uint32_t mode) {
    if (mode > zennative::kThermalCapAuto) return kIOReturnBadArgument;
    return runGated(kGateSetThermalCap, (void *)(uintptr_t)mode);
}

uint32_t AMDZenPM::getThermalCapMode() {
    return __atomic_load_n(&thermalCapMode, __ATOMIC_RELAXED);
}

IOReturn AMDZenPM::setProfile(uint32_t profile) {
    if (profile >= zennative::kProfileCount) return kIOReturnBadArgument;
    return runGated(kGateSetProfile, (void *)(uintptr_t)profile);
}

void AMDZenPM::getProfile(uint32_t *requested, uint32_t *active) {
    if (requested) *requested = __atomic_load_n(&profileRequested, __ATOMIC_RELAXED);
    if (active) *active = __atomic_load_n(&profileActive, __ATOMIC_RELAXED);
}

void AMDZenPM::getIdleConfig(int32_t *mode, uint32_t *thresholdUs) {
    if (mode) *mode = zen::idle::registered() ? (int32_t)zen::idle::mode() : -1;
    if (thresholdUs) *thresholdUs = __atomic_load_n(&idleThresholdUs, __ATOMIC_RELAXED);
}

// Runtime configuration through IORegistryEntrySetCFProperties (admin only). Accepts
// the personality keys; returns the first error encountered.
IOReturn AMDZenPM::setProperties(OSObject *properties) {
    if (IOUserClient::clientHasPrivilege(current_task(), kIOClientPrivilegeAdministrator) != kIOReturnSuccess)
        return kIOReturnNotPrivileged;
    OSDictionary *d = OSDynamicCast(OSDictionary, properties);
    if (!d) return kIOReturnBadArgument;

    IOReturn result = kIOReturnSuccess;
    bool handled = false;
    auto note = [&](IOReturn r) {
        handled = true;
        if (r != kIOReturnSuccess && result == kIOReturnSuccess) result = r;
    };

    if (OSObject *o = d->getObject("Governor")) {
        OSString *s = OSDynamicCast(OSString, o);
        if (!s) {
            note(kIOReturnBadArgument);
        } else if (s->isEqualTo("auto")) {
            note(setGovernor(autoGovernor(backend)));
        } else {
            uint32_t g = zen::governorFromName(s->getCStringNoCopy());
            note(g < kAMDZenGovCount ? setGovernor(g) : kIOReturnBadArgument);
        }
    }
    if (OSObject *o = d->getObject("Boost")) {
        int32_t m;
        note(readBoostMode(o, &m) ? runGated(kGateSetBoostMode, (void *)(intptr_t)m) : kIOReturnBadArgument);
    }
    uint32_t v;
    if (OSObject *o = d->getObject("EPP"))
        note(readU32(o, &v) ? setEPP(v) : kIOReturnBadArgument);
    if (OSObject *o = d->getObject("UserPState"))
        note(readU32(o, &v) ? setUserPState(v) : kIOReturnBadArgument);
    if (OSObject *o = d->getObject("IdleMode")) {
        uint32_t m = zen::kIdleAuto;
        bool ok = false;
        if (OSString *str = OSDynamicCast(OSString, o)) ok = zen::idleModeFromString(str->getCStringNoCopy(), &m);
        else ok = readU32(o, &m);
        // "auto" is a boot-time choice; at runtime a concrete mode is required.
        note(ok && m < zen::kIdleModeCount ? setIdleMode(m) : kIOReturnBadArgument);
    }
    if (OSObject *o = d->getObject("IdleThresholdUs"))
        note(readU32(o, &v) ? setIdleThresholdUs(v) : kIOReturnBadArgument);
    if (OSObject *o = d->getObject("FastUpPct"))
        note(readU32(o, &v) ? setGovKnob(kKnobFastUpPct, v) : kIOReturnBadArgument);
    if (OSObject *o = d->getObject("ThermalCap"))
        note(readU32(o, &v) ? setThermalCapMode(v) : kIOReturnBadArgument);
    if (OSObject *o = d->getObject("Profile")) {
        OSString *s = OSDynamicCast(OSString, o);
        zennative::CpuProfile p = zennative::kProfileAuto;
        note((s && zennative::profileFromName(s->getCStringNoCopy(), p))
             ? setProfile((uint32_t)p) : kIOReturnBadArgument);
    }
    if (OSObject *o = d->getObject("GovIdleSampling")) {
        if (OSBoolean *b = OSDynamicCast(OSBoolean, o)) note(setGovKnob(kKnobGovIdleSampling, b->isTrue() ? 1 : 0));
        else note(readU32(o, &v) ? setGovKnob(kKnobGovIdleSampling, v) : kIOReturnBadArgument);
    }

    OSObject *minO = d->getObject("MinPState"), *maxO = d->getObject("MaxPState");
    if (minO || maxO) {
        int32_t lo = -1, hi = -1;
        bool ok = (!minO || readIndexLimit(minO, &lo)) && (!maxO || readIndexLimit(maxO, &hi));
        if (!ok) {
            note(kIOReturnBadArgument);
        } else {
            uint32_t fast = lo < 0 ? 0 : (uint32_t)lo;
            uint32_t slow = hi < 0 ? ((backend == kAMDZenBackendCppcMsr || backend == kAMDZenBackendCppcPcc) ? 0 : legacyMaxIndex(info))
                                   : (uint32_t)hi;
            if (!minO) fast = limitFast;
            if (!maxO) slow = limitSlow;
            note(setPerfLimits(fast, slow));
        }
    }

    static const char *const kTunKeys[] = {
        "GovernorPeriodMs", "TelemetryPeriodMs", "UpThreshold", "DownThreshold",
        "SamplingDownFactor", "FreqStepPct", "UtilHeadroomPct", "UtilEwmaShift", "TjMax",
    };
    bool anyTun = false;
    for (const char *k : kTunKeys) anyTun |= d->getObject(k) != nullptr;
    if (anyTun) {
        AMDZenTunables t;
        IOReturn r = copyTunables(&t);
        bool ok = r == kIOReturnSuccess;
        if (ok && d->getObject("GovernorPeriodMs"))   ok = readU32(d->getObject("GovernorPeriodMs"), &t.governorPeriodMs);
        if (ok && d->getObject("TelemetryPeriodMs"))  ok = readU32(d->getObject("TelemetryPeriodMs"), &t.telemetryPeriodMs);
        if (ok && d->getObject("UpThreshold"))        ok = readU32(d->getObject("UpThreshold"), &t.upThreshold);
        if (ok && d->getObject("DownThreshold"))      ok = readU32(d->getObject("DownThreshold"), &t.downThreshold);
        if (ok && d->getObject("SamplingDownFactor")) ok = readU32(d->getObject("SamplingDownFactor"), &t.samplingDownFactor);
        if (ok && d->getObject("FreqStepPct"))        ok = readU32(d->getObject("FreqStepPct"), &t.freqStepPct);
        if (ok && d->getObject("UtilHeadroomPct"))    ok = readU32(d->getObject("UtilHeadroomPct"), &t.utilHeadroomPct);
        if (ok && d->getObject("UtilEwmaShift"))      ok = readU32(d->getObject("UtilEwmaShift"), &t.utilEwmaShift);
        if (ok && d->getObject("TjMax")) {
            uint32_t c = 0;
            ok = readU32(d->getObject("TjMax"), &c) && c <= 150;
            t.tjMax_mC = c * 1000;
        }
        note(ok ? setTunables(&t) : (r != kIOReturnSuccess ? r : kIOReturnBadArgument));
    }
    return handled ? result : kIOReturnUnsupported;
}

IOReturn AMDZenPM::newUserClient(task_t owningTask, void *securityID, UInt32 type,
                                 OSDictionary *properties, IOUserClient **handler) {
    if (!handler) return kIOReturnBadArgument;
    *handler = nullptr;
    if (isInactive() || !gate) return kIOReturnNotReady;
    AMDZenPMUserClient *client = OSTypeAlloc(AMDZenPMUserClient);
    if (!client) return kIOReturnNoMemory;
    if (!client->initWithTask(owningTask, securityID, type, properties)) {
        client->release();
        return kIOReturnBadArgument;
    }
    if (!client->attach(this)) {
        client->release();
        return kIOReturnError;
    }
    if (!client->start(this)) {
        client->detach(this);
        client->release();
        return kIOReturnError;
    }
    *handler = client;
    return kIOReturnSuccess;
}

// ===========================================================================
// C API for in-kernel consumers (AMDZenSMC.kext)
// ===========================================================================
// The reference taken by copyInstance() keeps the object (and its gate/work loop,
// released only in free()) alive even if stop() runs concurrently; after stop()
// the gate is detached and runGated() simply fails.
extern "C" __attribute__((visibility("default"))) int AMDZenPM_copyInfo(AMDZenInfo *out) {
    if (!out) return -1;
    AMDZenPM *pm = AMDZenPM::copyInstance();
    if (!pm) return -1;
    const int r = pm->copyInfo(out) == kIOReturnSuccess ? 0 : -1;
    pm->release();
    return r;
}

extern "C" __attribute__((visibility("default"))) int AMDZenPM_copySample(AMDZenSample *out) {
    if (!out) return -1;
    AMDZenPM *pm = AMDZenPM::copyInstance();
    if (!pm) return -1;
    const int r = pm->copySample(out) == kIOReturnSuccess ? 0 : -1;
    pm->release();
    return r;
}
