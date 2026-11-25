//
// ZenSysctl.cpp — kern.amdzen.* sysctl tree (secondary userspace interface).
//
// Read-only values come from the latest telemetry sample; read-write values are
// root-only (CTLFLAG_ANYBODY is never set) and are forwarded to the gated setters
// of AMDZenPM, so every hardware access still happens on the driver's work loop.
//
#include "AMDZenPM.hpp"
#include <IOKit/IOLib.h>
#include <libkern/libkern.h>
#include <sys/errno.h>
#include <sys/sysctl.h>
#include <string.h>

// kern.* is exported to kexts on macOS 26 (sysctl__kern_children, verified with
// tools/linkcheck.sh). Fall back to debug.* by building with -DAMDZEN_SYSCTL_DEBUG.
#ifdef AMDZEN_SYSCTL_DEBUG
#define ZEN_PARENT _debug
#define ZEN_NODE   _debug_amdzen
#else
#define ZEN_PARENT _kern
#define ZEN_NODE   _kern_amdzen
#endif

namespace {

bool gSysctlRegistered;

// A retained reference to the running instance for the duration of one handler,
// so a concurrent stop()/free() cannot pull the object out from under it.
struct PmRef {
    AMDZenPM *pm;
    PmRef() : pm(AMDZenPM::copyInstance()) {}
    ~PmRef() { if (pm) pm->release(); }
    PmRef(const PmRef &) = delete;
    PmRef &operator=(const PmRef &) = delete;
};

int ioReturnToErrno(IOReturn r) {
    switch (r) {
    case kIOReturnSuccess:       return 0;
    case kIOReturnNotPermitted:
    case kIOReturnNotPrivileged: return EPERM;
    case kIOReturnBadArgument:   return EINVAL;
    case kIOReturnUnsupported:   return ENOTSUP;
    case kIOReturnNoMemory:      return ENOMEM;
    case kIOReturnNotReady:
    case kIOReturnNotAttached:   return ENXIO;
    default:                     return EIO;
    }
}

// Heap copies (AMDZenSample is ~5 KB; keep it off the kernel stack).
template <typename T, IOReturn (AMDZenPM::*Copy)(T *)>
T *copyNew(AMDZenPM *pm, int *err) {
    T *p = static_cast<T *>(IOMallocZero(sizeof(T)));
    if (!p) { *err = ENOMEM; return nullptr; }
    IOReturn r = (pm->*Copy)(p);
    if (r != kIOReturnSuccess) {
        IOFree(p, sizeof(T));
        *err = ioReturnToErrno(r);
        return nullptr;
    }
    *err = 0;
    return p;
}
template <typename T> void freeCopy(T *p) { if (p) IOFree(p, sizeof(T)); }

AMDZenSample *copySampleNew(AMDZenPM *pm, int *err) {
    return copyNew<AMDZenSample, &AMDZenPM::copySample>(pm, err);
}
AMDZenInfo *copyInfoNew(AMDZenPM *pm, int *err) {
    return copyNew<AMDZenInfo, &AMDZenPM::copyInfo>(pm, err);
}

// Setting values that only exist through setProperties ("auto" governor, boost "keep").
IOReturn setViaProperties(AMDZenPM *pm, const char *key, const char *value) {
    OSDictionary *d = OSDictionary::withCapacity(1);
    OSString *s = OSString::withCString(value);
    IOReturn r = kIOReturnNoMemory;
    if (d && s && d->setObject(key, s)) r = pm->setProperties(d);
    OSSafeReleaseNULL(s);
    OSSafeReleaseNULL(d);
    return r;
}

enum SampleField {
    kFieldTctl, kFieldTdie, kFieldPkgPower, kFieldCoresPower, kFieldFreqAvg, kFieldFreqMax,
    kFieldVcore, kFieldBusy,
};

int sysctlSampleField SYSCTL_HANDLER_ARGS {
    PmRef ref;
    AMDZenPM *pm = ref.pm;
    if (!pm) return ENXIO;
    int err;
    AMDZenSample *s = copySampleNew(pm, &err);
    if (!s) return err;
    int v = 0;
    switch (arg2) {
    case kFieldTctl:       v = s->tctl_mC; break;
    case kFieldTdie:       v = s->tdie_mC; break;
    case kFieldPkgPower:   v = (int)s->pkgPower_mW; break;
    case kFieldCoresPower: v = (int)s->coresPower_mW; break;
    case kFieldFreqAvg:    v = (int)s->avgFreqMHz; break;
    case kFieldFreqMax:    v = (int)s->maxFreqMHz; break;
    case kFieldVcore:      v = (int)s->vcore_mV; break;
    case kFieldBusy:       v = (int)s->avgBusyPermille; break;
    default:               break;
    }
    freeCopy(s);
    return SYSCTL_OUT(req, &v, sizeof(v));
}

enum InfoField { kFieldBackend, kFieldBlockReason };

int sysctlInfoString SYSCTL_HANDLER_ARGS {
    PmRef ref;
    AMDZenPM *pm = ref.pm;
    if (!pm) return ENXIO;
    int err;
    AMDZenInfo *in = copyInfoNew(pm, &err);
    if (!in) return err;
    const char *str = "unknown";
    if (arg2 == kFieldBackend) {
        switch (in->backend) {
        case kAMDZenBackendLegacy:  str = "legacy"; break;
        case kAMDZenBackendCppcMsr: str = "cppc-msr"; break;
        case kAMDZenBackendCppcPcc: str = "cppc-pcc"; break;
        default:                    str = "none"; break;
        }
    } else {
        switch (in->controlBlockReason) {
        case kAMDZenBlockNone:        str = "none"; break;
        case kAMDZenBlockBootArgRO:   str = "readonly"; break;
        case kAMDZenBlockConflict:    str = "conflict"; break;
        case kAMDZenBlockUnsupported: str = "unsupported"; break;
        case kAMDZenBlockHypervisor:  str = "hypervisor"; break;
        case kAMDZenBlockPccFailed:   str = "pcc-failed"; break;
        default:                      break;
        }
    }
    freeCopy(in);
    return SYSCTL_OUT(req, str, strlen(str) + 1);
}

int sysctlGovernor SYSCTL_HANDLER_ARGS {
    PmRef ref;
    AMDZenPM *pm = ref.pm;
    if (!pm) return ENXIO;
    int err;
    AMDZenSample *s = copySampleNew(pm, &err);
    if (!s) return err;
    char buf[32];
    strlcpy(buf, zen::governorName(s->governor), sizeof(buf));
    freeCopy(s);
    // sysctl_io_* are not exported to kexts; the legacy handlers are.
    err = sysctl_handle_string(oidp, buf, sizeof(buf), req);
    if (err || !req->newptr) return err;
    buf[sizeof(buf) - 1] = '\0';
    return ioReturnToErrno(setViaProperties(pm, "Governor", buf));
}

enum IntField { kIntBoost, kIntEpp, kIntLimitFast, kIntLimitSlow, kIntUser, kIntPeriod,
                kIntIdleMode, kIntIdleThreshold };

enum NativeField {
    kNativeCommands, kNativeLatency, kNativeError, kNativeState,
    kNativeHighest, kNativeNominal, kNativeNonlinear, kNativeLowest,
    kNativeMin, kNativeMax, kNativeEpp, kNativeRequests,
    kNativeAc, kNativeThermal, kNativeLpm, kNativeRt, kNativeRtMask, kNativeHook
};
int sysctlNative SYSCTL_HANDLER_ARGS {
    PmRef ref;
    if (!ref.pm) return ENXIO;
    int err = 0;
    auto *d = copyNew<AMDZenPM::NativeDiagnostics, &AMDZenPM::copyNativeDiagnostics>(ref.pm, &err);
    if (!d) return err;
    uint64_t wide = 0;
    uint32_t value = 0;
    const char *str = nullptr;
    switch (arg2) {
    case kNativeCommands: wide = d->commands; break;
    case kNativeLatency: wide = d->latencyUs; break;
    case kNativeRtMask: wide = d->rtMask; break;
    case kNativeError: value = d->error; break;
    case kNativeHighest: value = d->caps.highest; break;
    case kNativeNominal: value = d->caps.nominal; break;
    case kNativeNonlinear: value = d->caps.lowestNonlinear; break;
    case kNativeLowest: value = d->caps.lowest; break;
    case kNativeMin: value = d->requests[0].minPerf; break;
    case kNativeMax: value = d->requests[0].maxPerf; break;
    case kNativeEpp: value = d->requests[0].epp; break;
    case kNativeThermal: value = d->thermal; break;
    case kNativeRt: value = d->rtActive; break;
    case kNativeHook: value = d->hookActive; break;
    case kNativeAc: str = d->acPower == 1 ? "ac" : (d->acPower == 2 ? "battery" : "unknown"); break;
    case kNativeLpm: str = d->lowPower == 1 ? "off" : (d->lowPower == 2 ? "on" : "unknown"); break;
    case kNativeState: {
        static const char *const states[] = {"idle", "starting", "active", "degraded", "fallback", "stopped"};
        str = d->stage < 6 ? states[d->stage] : "unknown";
        break;
    }
    case kNativeRequests: {
        constexpr size_t bytes = 8192;
        char *summary = static_cast<char *>(IOMallocZero(bytes));
        if (!summary) { freeCopy(d); return ENOMEM; }
        size_t used = 0;
        for (uint32_t c = 0; c < d->cpuCount && c < 64; ++c) {
            const auto &r = d->requests[c];
            int n = snprintf(summary + used, bytes - used, "%s%u:min=%u,max=%u,desired=%u,epp=%u",
                             c ? "; " : "", c, r.minPerf, r.maxPerf, r.desiredPerf, r.epp);
            if (n < 0 || (size_t)n >= bytes - used) break;
            used += (size_t)n;
        }
        err = SYSCTL_OUT(req, summary, used + 1);
        IOFree(summary, bytes);
        freeCopy(d);
        return err;
    }
    default: freeCopy(d); return EINVAL;
    }
    freeCopy(d);
    if (str) return SYSCTL_OUT(req, str, strlen(str) + 1);
    if (arg2 == kNativeCommands || arg2 == kNativeLatency || arg2 == kNativeRtMask)
        return SYSCTL_OUT(req, &wide, sizeof(wide));
    return SYSCTL_OUT(req, &value, sizeof(value));
}

// kern.amdzen.idle_mode / idle_threshold_us. Kept apart from sysctlInt: they do not
// need a telemetry sample (and must work before the first one exists).
int sysctlIdle SYSCTL_HANDLER_ARGS {
    PmRef ref;
    AMDZenPM *pm = ref.pm;
    if (!pm) return ENXIO;
    int32_t mode = -1;
    uint32_t us = 0;
    pm->getIdleConfig(&mode, &us);
    int nv = (arg2 == kIntIdleMode) ? (int)mode : (int)us;
    int err = sysctl_handle_int(oidp, &nv, 0, req);
    if (err || !req->newptr) return err;
    if (nv < 0) return EINVAL;
    IOReturn r = (arg2 == kIntIdleMode) ? pm->setIdleMode((uint32_t)nv) : pm->setIdleThresholdUs((uint32_t)nv);
    return ioReturnToErrno(r);
}

// kern.amdzen.fast_up_pct / gov_idle_sampling: software knobs outside the tunables ABI.
int sysctlGovKnob SYSCTL_HANDLER_ARGS {
    PmRef ref;
    AMDZenPM *pm = ref.pm;
    if (!pm) return ENXIO;
    int nv = (int)pm->getGovKnob((uint32_t)arg2);
    int err = sysctl_handle_int(oidp, &nv, 0, req);
    if (err || !req->newptr) return err;
    if (nv < 0) return EINVAL;
    return ioReturnToErrno(pm->setGovKnob((uint32_t)arg2, (uint32_t)nv));
}

// kern.amdzen.lpm: -1 auto (native signal), 0 off, 1 on.
int sysctlLpm SYSCTL_HANDLER_ARGS {
    PmRef ref;
    AMDZenPM *pm = ref.pm;
    if (!pm) return ENXIO;
    int nv = (int)pm->getLowPowerMode();
    int err = sysctl_handle_int(oidp, &nv, 0, req);
    if (err || !req->newptr) return err;
    if (nv < -1 || nv > 1) return EINVAL;
    return ioReturnToErrno(pm->setLowPowerMode((int32_t)nv));
}

int sysctlInt SYSCTL_HANDLER_ARGS {
    PmRef ref;
    AMDZenPM *pm = ref.pm;
    if (!pm) return ENXIO;
    int err;
    AMDZenSample *s = copySampleNew(pm, &err);
    if (!s) return err;
    int cur = 0;
    uint32_t curFast = s->limitFast, curSlow = s->limitSlow;
    switch (arg2) {
    case kIntBoost:     cur = (int)s->boostEnabled; break;
    case kIntEpp:       cur = (int)s->epp; break;
    case kIntLimitFast: cur = (int)s->limitFast; break;
    case kIntLimitSlow: cur = (int)s->limitSlow; break;
    case kIntUser:
        if (OSNumber *n = OSDynamicCast(OSNumber, pm->getProperty("UserValue"))) cur = (int)n->unsigned32BitValue();
        break;
    case kIntPeriod: {
        AMDZenTunables t;
        if (pm->copyTunables(&t) == kIOReturnSuccess) cur = (int)t.governorPeriodMs;
        break;
    }
    default: break;
    }
    freeCopy(s);

    int nv = cur;
    err = sysctl_handle_int(oidp, &nv, 0, req);
    if (err || !req->newptr) return err;

    IOReturn r = kIOReturnBadArgument;
    switch (arg2) {
    case kIntBoost:
        if (nv == -1)               r = setViaProperties(pm, "Boost", "keep");
        else if (nv == 0 || nv == 1) r = pm->setBoost(nv == 1);
        break;
    case kIntEpp:
        if (nv >= 0 && nv <= 255) r = pm->setEPP((uint32_t)nv);
        break;
    case kIntLimitFast:
        if (nv >= 0 && nv <= 255) r = pm->setPerfLimits((uint32_t)nv, curSlow);
        break;
    case kIntLimitSlow:
        if (nv >= 0 && nv <= 255) r = pm->setPerfLimits(curFast, (uint32_t)nv);
        break;
    case kIntUser:
        if (nv >= 0 && nv <= 255) r = pm->setUserPState((uint32_t)nv);
        break;
    case kIntPeriod:
        if (nv > 0) {
            AMDZenTunables t;
            r = pm->copyTunables(&t);
            if (r == kIOReturnSuccess) {
                t.governorPeriodMs = (uint32_t)nv;
                r = pm->setTunables(&t);
            }
        }
        break;
    default:
        break;
    }
    return ioReturnToErrno(r);
}

} // namespace

#define RO_INT  (CTLTYPE_INT | CTLFLAG_RD | CTLFLAG_LOCKED)
#define RW_INT  (CTLTYPE_INT | CTLFLAG_RW | CTLFLAG_LOCKED)
#define RO_STR  (CTLTYPE_STRING | CTLFLAG_RD | CTLFLAG_LOCKED)
#define RO_U64  (CTLTYPE_QUAD | CTLFLAG_RD | CTLFLAG_LOCKED)
#define RW_STR  (CTLTYPE_STRING | CTLFLAG_RW | CTLFLAG_LOCKED)

#define ZEN_CAT2(a, b) a##b
#define ZEN_CAT(a, b) ZEN_CAT2(a, b)
#define ZEN_OID(name) ZEN_CAT(ZEN_CAT(sysctl_, ZEN_NODE), _##name)
// The SYSCTL_* macros paste their parent argument, so expand ZEN_PARENT/ZEN_NODE first.
#define ZEN_SYSCTL_NODE(parent, ...) SYSCTL_NODE(parent, __VA_ARGS__)
#define ZEN_SYSCTL_PROC(parent, ...) SYSCTL_PROC(parent, __VA_ARGS__)

ZEN_SYSCTL_NODE(ZEN_PARENT, OID_AUTO, amdzen, CTLFLAG_RW | CTLFLAG_LOCKED, 0, "AMD Zen power management");

ZEN_SYSCTL_PROC(ZEN_NODE, OID_AUTO, tctl_mc,        RO_INT, nullptr, kFieldTctl,       sysctlSampleField, "I", "Tctl (m°C)");
ZEN_SYSCTL_PROC(ZEN_NODE, OID_AUTO, tdie_mc,        RO_INT, nullptr, kFieldTdie,       sysctlSampleField, "I", "Tdie (m°C)");
ZEN_SYSCTL_PROC(ZEN_NODE, OID_AUTO, pkg_power_mw,   RO_INT, nullptr, kFieldPkgPower,   sysctlSampleField, "I", "Package power (mW)");
ZEN_SYSCTL_PROC(ZEN_NODE, OID_AUTO, cores_power_mw, RO_INT, nullptr, kFieldCoresPower, sysctlSampleField, "I", "Sum of core power (mW)");
ZEN_SYSCTL_PROC(ZEN_NODE, OID_AUTO, freq_avg_mhz,   RO_INT, nullptr, kFieldFreqAvg,    sysctlSampleField, "I", "Mean effective frequency of busy cores (MHz)");
ZEN_SYSCTL_PROC(ZEN_NODE, OID_AUTO, freq_max_mhz,   RO_INT, nullptr, kFieldFreqMax,    sysctlSampleField, "I", "Highest core effective frequency (MHz)");
ZEN_SYSCTL_PROC(ZEN_NODE, OID_AUTO, vcore_mv,       RO_INT, nullptr, kFieldVcore,      sysctlSampleField, "I", "SVI2 core voltage (mV)");
ZEN_SYSCTL_PROC(ZEN_NODE, OID_AUTO, busy_permille,  RO_INT, nullptr, kFieldBusy,       sysctlSampleField, "I", "Mean C0 residency (permille)");
ZEN_SYSCTL_PROC(ZEN_NODE, OID_AUTO, backend,        RO_STR, nullptr, kFieldBackend,    sysctlInfoString,  "A", "Control backend");
ZEN_SYSCTL_PROC(ZEN_NODE, OID_AUTO, block_reason,   RO_STR, nullptr, kFieldBlockReason, sysctlInfoString, "A", "Why control is disabled");
ZEN_SYSCTL_PROC(ZEN_NODE, OID_AUTO, governor,       RW_STR, nullptr, 0,                sysctlGovernor,    "A", "Governor (performance|powersave|schedutil|ondemand|conservative|userspace|active|none|auto)");
ZEN_SYSCTL_PROC(ZEN_NODE, OID_AUTO, boost,          RW_INT, nullptr, kIntBoost,        sysctlInt,         "I", "Core Performance Boost: read 0/1; write -1 keep, 0 off, 1 on");
ZEN_SYSCTL_PROC(ZEN_NODE, OID_AUTO, epp,            RW_INT, nullptr, kIntEpp,          sysctlInt,         "I", "CPPC energy/performance preference 0..255");
ZEN_SYSCTL_PROC(ZEN_NODE, OID_AUTO, limit_fast,     RW_INT, nullptr, kIntLimitFast,    sysctlInt,         "I", "Legacy: fastest allowed P-state index; CPPC: min perf (0 = default)");
ZEN_SYSCTL_PROC(ZEN_NODE, OID_AUTO, limit_slow,     RW_INT, nullptr, kIntLimitSlow,    sysctlInt,         "I", "Legacy: slowest allowed P-state index; CPPC: max perf (0 = default)");
ZEN_SYSCTL_PROC(ZEN_NODE, OID_AUTO, user_pstate,    RW_INT, nullptr, kIntUser,         sysctlInt,         "I", "userspace governor: P-state index (legacy) or desired perf (CPPC)");
ZEN_SYSCTL_PROC(ZEN_NODE, OID_AUTO, period_ms,      RW_INT, nullptr, kIntPeriod,       sysctlInt,         "I", "Governor period (ms, 4..1000)");
ZEN_SYSCTL_PROC(ZEN_NODE, OID_AUTO, fast_up_pct,    RW_INT, nullptr, AMDZenPM::kKnobFastUpPct, sysctlGovKnob, "I", "schedutil: busy % that jumps straight to the fastest P-state (0 = off)");
ZEN_SYSCTL_PROC(ZEN_NODE, OID_AUTO, gov_idle_sampling, RW_INT, nullptr, AMDZenPM::kKnobGovIdleSampling, sysctlGovKnob, "I", "1: legacy governor samples load from the idle hook (no IPI); 0: APERF/MPERF rendezvous every tick");
ZEN_SYSCTL_PROC(ZEN_NODE, OID_AUTO, idle_mode,      RW_INT, nullptr, kIntIdleMode,     sysctlIdle,        "I", "Idle hook: 0 off(HLT) 1 HLT 2 MWAIT 3 CC6 (clamped to hardware); -1 = not registered");
ZEN_SYSCTL_PROC(ZEN_NODE, OID_AUTO, idle_threshold_us, RW_INT, nullptr, kIntIdleThreshold, sysctlIdle,     "I", "Predicted idle length (us) above which CC6 is used (0..1000000)");
ZEN_SYSCTL_PROC(ZEN_NODE, OID_AUTO, pcc_commands, RO_U64, nullptr, kNativeCommands, sysctlNative, "QU", "Completed/issued PCC doorbells, including READs");
ZEN_SYSCTL_PROC(ZEN_NODE, OID_AUTO, pcc_latency_us, RO_U64, nullptr, kNativeLatency, sysctlNative, "QU", "Last PCC command latency (us)");
ZEN_SYSCTL_PROC(ZEN_NODE, OID_AUTO, pcc_last_error, RO_INT, nullptr, kNativeError, sysctlNative, "I", "ZenPccModel Error, or 65536 policy/65537 confirmation/65538 interrupted");
ZEN_SYSCTL_PROC(ZEN_NODE, OID_AUTO, pcc_state, RO_STR, nullptr, kNativeState, sysctlNative, "A", "PCC lifecycle state");
ZEN_SYSCTL_PROC(ZEN_NODE, OID_AUTO, cppc_highest, RO_INT, nullptr, kNativeHighest, sysctlNative, "I", "CPU0 Highest Performance");
ZEN_SYSCTL_PROC(ZEN_NODE, OID_AUTO, cppc_nominal, RO_INT, nullptr, kNativeNominal, sysctlNative, "I", "CPU0 Nominal Performance");
ZEN_SYSCTL_PROC(ZEN_NODE, OID_AUTO, cppc_lowest_nonlinear, RO_INT, nullptr, kNativeNonlinear, sysctlNative, "I", "CPU0 Lowest Nonlinear Performance");
ZEN_SYSCTL_PROC(ZEN_NODE, OID_AUTO, cppc_lowest, RO_INT, nullptr, kNativeLowest, sysctlNative, "I", "CPU0 Lowest Performance");
ZEN_SYSCTL_PROC(ZEN_NODE, OID_AUTO, cppc_min, RO_INT, nullptr, kNativeMin, sysctlNative, "I", "CPU0 last accepted minimum request");
ZEN_SYSCTL_PROC(ZEN_NODE, OID_AUTO, cppc_max, RO_INT, nullptr, kNativeMax, sysctlNative, "I", "CPU0 last accepted maximum request");
ZEN_SYSCTL_PROC(ZEN_NODE, OID_AUTO, cppc_epp, RO_INT, nullptr, kNativeEpp, sysctlNative, "I", "CPU0 last accepted native EPP");
ZEN_SYSCTL_PROC(ZEN_NODE, OID_AUTO, cppc_requests, RO_STR, nullptr, kNativeRequests, sysctlNative, "A", "Per-XNU-CPU last accepted min/max/desired/EPP requests");
ZEN_SYSCTL_PROC(ZEN_NODE, OID_AUTO, native_ac_power, RO_STR, nullptr, kNativeAc, sysctlNative, "A", "Native power source: unknown/ac/battery");
ZEN_SYSCTL_PROC(ZEN_NODE, OID_AUTO, native_thermal, RO_INT, nullptr, kNativeThermal, sysctlNative, "I", "Native thermal: 0 unknown, 1 nominal, 2 moderate, 3 heavy, 4 critical, 5 emergency");
ZEN_SYSCTL_PROC(ZEN_NODE, OID_AUTO, native_low_power_mode, RO_STR, nullptr, kNativeLpm, sysctlNative, "A", "Native low power mode: unknown/off/on");
ZEN_SYSCTL_PROC(ZEN_NODE, OID_AUTO, lpm, RW_INT, nullptr, 0, sysctlLpm, "I", "Low power mode override: -1 auto, 0 off, 1 on");
ZEN_SYSCTL_PROC(ZEN_NODE, OID_AUTO, native_rt_active, RO_INT, nullptr, kNativeRt, sysctlNative, "I", "CPUs with current/recent RT urgency (100 ms hold)");
ZEN_SYSCTL_PROC(ZEN_NODE, OID_AUTO, native_rt_mask, RO_U64, nullptr, kNativeRtMask, sysctlNative, "QU", "Per-XNU-CPU current/recent RT urgency mask");
ZEN_SYSCTL_PROC(ZEN_NODE, OID_AUTO, native_urgency_active, RO_INT, nullptr, kNativeHook, sysctlNative, "I", "Scheduler urgency observer installed");

static struct sysctl_oid *const kZenOids[] = {
    &ZEN_OID(tctl_mc), &ZEN_OID(tdie_mc), &ZEN_OID(pkg_power_mw), &ZEN_OID(cores_power_mw),
    &ZEN_OID(freq_avg_mhz), &ZEN_OID(freq_max_mhz), &ZEN_OID(vcore_mv), &ZEN_OID(busy_permille),
    &ZEN_OID(backend), &ZEN_OID(block_reason), &ZEN_OID(governor), &ZEN_OID(boost), &ZEN_OID(epp),
    &ZEN_OID(limit_fast), &ZEN_OID(limit_slow), &ZEN_OID(user_pstate), &ZEN_OID(period_ms),
    &ZEN_OID(idle_mode), &ZEN_OID(idle_threshold_us), &ZEN_OID(fast_up_pct), &ZEN_OID(gov_idle_sampling),
    &ZEN_OID(pcc_commands), &ZEN_OID(pcc_latency_us), &ZEN_OID(pcc_last_error), &ZEN_OID(pcc_state),
    &ZEN_OID(cppc_highest), &ZEN_OID(cppc_nominal), &ZEN_OID(cppc_lowest_nonlinear), &ZEN_OID(cppc_lowest),
    &ZEN_OID(cppc_min), &ZEN_OID(cppc_max), &ZEN_OID(cppc_epp), &ZEN_OID(cppc_requests),
    &ZEN_OID(native_ac_power), &ZEN_OID(native_thermal), &ZEN_OID(native_low_power_mode), &ZEN_OID(lpm),
    &ZEN_OID(native_rt_active), &ZEN_OID(native_rt_mask), &ZEN_OID(native_urgency_active),
};

void AMDZenPM::registerSysctls() {
    if (gSysctlRegistered) return;
    sysctl_register_oid(&ZEN_CAT(sysctl_, ZEN_CAT(ZEN_PARENT, _amdzen)));
    for (struct sysctl_oid *o : kZenOids) sysctl_register_oid(o);
    gSysctlRegistered = true;
}

void AMDZenPM::unregisterSysctls() {
    if (!gSysctlRegistered) return;
    for (size_t i = sizeof(kZenOids) / sizeof(kZenOids[0]); i-- > 0;) sysctl_unregister_oid(kZenOids[i]);
    sysctl_unregister_oid(&ZEN_CAT(sysctl_, ZEN_CAT(ZEN_PARENT, _amdzen)));
    gSysctlRegistered = false;
}
