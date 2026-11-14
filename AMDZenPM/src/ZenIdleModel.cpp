//
// ZenIdleModel.cpp — pure decision logic for the CC6 idle hook. See ZenIdleModel.hpp.
//
#include "ZenIdleModel.hpp"
#include "ZenRegs.h"

namespace zen {

// ------------------------------------------------------------------ modes
const char *idleModeName(uint32_t mode) {
    switch (mode) {
    case kIdleOff:   return "off";
    case kIdleHlt:   return "hlt";
    case kIdleMwait: return "mwait";
    case kIdleCc6:   return "cc6";
    case kIdleAuto:  return "auto";
    default:         return "unknown";
    }
}

bool idleModeFromString(const char *s, uint32_t *mode) {
    if (!s || !mode) return false;
    if (s[0] == 'a' && s[1] == 'u' && s[2] == 't' && s[3] == 'o' && s[4] == '\0') {
        *mode = kIdleAuto;
        return true;
    }
    if (s[0] >= '0' && s[0] <= '3' && s[1] == '\0') {
        *mode = (uint32_t)(s[0] - '0');
        return true;
    }
    return false;
}

uint32_t clampIdleMode(uint32_t req, bool haveMwait, bool haveCc6) {
    if (req == kIdleOff) return kIdleOff;
    if (req >= kIdleCc6 && haveCc6) return kIdleCc6;
    if (req >= kIdleMwait && haveMwait) return kIdleMwait;
    return kIdleHlt;
}

// ------------------------------------------------------------------ ACPI
bool parseGenericRegister(const uint8_t *buf, size_t len, AcpiGas *out) {
    if (!buf || !out || len < 15) return false;
    if (buf[0] != 0x82) return false;
    // Large resource length = bytes after the 3-byte header; a GAS body is 12.
    const uint32_t bodyLen = (uint32_t)buf[1] | ((uint32_t)buf[2] << 8);
    if (bodyLen < 12 || (size_t)bodyLen + 3 > len) return false;
    out->spaceId = buf[3];
    out->bitWidth = buf[4];
    out->bitOffset = buf[5];
    out->accessSize = buf[6];
    uint64_t a = 0;
    for (uint32_t i = 0; i < 8; i++) a |= (uint64_t)buf[7 + i] << (8 * i);
    out->address = a;
    return true;
}

bool makeCstEntry(const uint8_t *reg, size_t regLen, uint64_t type, uint64_t latency,
                  uint64_t power, CstEntry *out) {
    if (!out) return false;
    AcpiGas g;
    if (!parseGenericRegister(reg, regLen, &g)) return false;
    if (type < 1 || type > 3) return false;
    out->type = (uint32_t)type;
    out->latencyUs = latency > 0xFFFFFFFFull ? 0xFFFFFFFFu : (uint32_t)latency;
    out->power = power > 0xFFFFFFFFull ? 0xFFFFFFFFu : (uint32_t)power;
    out->reg = g;
    return true;
}

// ------------------------------------------------------------------ MSRs
CStateCfg decodeCStateCfg(uint64_t raw) {
    CStateCfg c;
    for (uint32_t k = 0; k < 3; k++) {
        c.cc1dfsid[k] = (uint8_t)((raw >> (8 * k)) & 0x3F);
        c.cc6en[k] = cstateCfgCc6En(raw, k);
    }
    return c;
}

const char *cc6ReasonName(uint32_t r) {
    switch (r) {
    case kCc6Ok:              return "ok";
    case kCc6MsrUnreadable:   return "msr-unreadable";
    case kCc6BaseZero:        return "cstate-base-zero";
    case kCc6BaseInvalid:     return "cstate-base-invalid";
    case kCc6BaseMismatch:    return "cstate-base-mismatch";
    case kCc6InvdWbinvdClear: return "hwcr-invdwbinvd-clear";
    case kCc6CstNoIoEntry:    return "cst-no-io-entry";
    case kCc6CstCcr0:         return "cst-ccr0-only";
    case kCc6NoCc6En:         return "cc6en-clear";
    default:                  return "unknown";
    }
}

uint32_t checkCStateBase(uint64_t raw, uint32_t *base) {
    const uint32_t b = (uint32_t)(raw & 0xFFFFu);
    if (base) *base = b;
    if (b == 0) return kCc6BaseZero;
    // PPR 56214: values above FFF8h are undefined (the 8-port window would wrap).
    if (b > 0xFFF8u) return kCc6BaseInvalid;
    return kCc6Ok;
}

IdleCaps aggregateIdleMsrs(const IdleMsrSample *s, uint32_t n) {
    IdleCaps c {};
    c.ncpu = n;
    if (!s || n == 0) return c;
    c.msrOk = true;
    c.baseUniform = true;
    c.cfgUniform = true;
    c.invdWbinvd = true;
    c.mwaitAllowed = true;
    c.baseRaw = s[0].base;
    c.cfg = s[0].cfg;
    c.policy = s[0].policyOk ? s[0].policy : 0;
    c.cfgCc6And = kCStateCfgCc6Mask;
    bool hwcrAll = true;
    for (uint32_t i = 0; i < n; i++) {
        const IdleMsrSample &x = s[i];
        // HWCR is judged on its own: MWAIT does not depend on the C-state MSRs.
        if (!x.hwcrOk) {
            hwcrAll = false;
        } else {
            if (!(x.hwcr & HWCR_INVDWBINVD)) c.invdWbinvd = false;
            if (x.hwcr & HWCR_MON_MWAIT_DIS) c.mwaitAllowed = false;
        }
        if (!x.ok) {
            c.msrOk = false;
            continue;
        }
        if (x.base != c.baseRaw) c.baseUniform = false;
        if (x.cfg != c.cfg) c.cfgUniform = false;
        c.cfgCc6And &= x.cfg;
    }
    if (!hwcrAll) {
        c.msrOk = false;
        c.invdWbinvd = c.mwaitAllowed = false;
    }
    if (!c.msrOk) {
        // A CPU that did not answer could hold anything: nothing is "uniform".
        c.baseUniform = c.cfgUniform = c.invdWbinvd = false;
        c.cfgCc6And = 0;
    }
    return c;
}

// ------------------------------------------------------------------ port choice
const char *idlePortSourceName(uint32_t s) {
    switch (s) {
    case kPortSourceCst: return "acpi-cst";
    case kPortSourceCfg: return "cstatecfg";
    default:             return "none";
    }
}

IdlePortChoice chooseIdlePort(const IdleCaps &caps, const CstEntry *cst, uint32_t n, bool haveCst) {
    IdlePortChoice r {};
    r.cstIndex = -1;
    auto fail = [&](uint32_t why) -> IdlePortChoice {
        r.ok = false;
        r.port = 0;
        r.reason = why;
        return r;
    };
    if (!caps.msrOk) return fail(kCc6MsrUnreadable);
    if (!caps.baseUniform) return fail(kCc6BaseMismatch);
    uint32_t base = 0;
    uint32_t br = checkCStateBase(caps.baseRaw, &base);
    if (br != kCc6Ok) return fail(br);
    if (!caps.invdWbinvd) return fail(kCc6InvdWbinvdClear);

    uint32_t k = 0;
    if (haveCst) {
        // Deepest SystemIO entry inside the trap window: highest type, then highest
        // latency. An IO entry outside the window would be a real port access (for
        // example the FCH P_LVL registers when CStateAddr does not cover them), so
        // such entries never select anything.
        int32_t best = -1;
        for (uint32_t i = 0; cst && i < n; i++) {
            const CstEntry &e = cst[i];
            if (e.reg.spaceId != kGasSystemIO) continue;
            if (e.reg.address < base || e.reg.address > (uint64_t)base + 7) continue;
            if (best < 0 || e.type > cst[best].type ||
                (e.type == cst[best].type && e.latencyUs >= cst[best].latencyUs))
                best = (int32_t)i;
        }
        if (best < 0) return fail(kCc6CstNoIoEntry);
        r.cstIndex = best;
        uint64_t off = cst[best].reg.address - base;
        // 15h rule (BKDG 42301 §2.5.3.2): offsets 2..7 act as offset 2.
        k = off > 2 ? 2u : (uint32_t)off;
        // CCR0's purpose on Zen is unknown (amd-cstate.md §7.2): never use base+0.
        if (k == 0) return fail(kCc6CstCcr0);
        r.source = kPortSourceCst;
    } else {
        if (cstateCfgCc6En(caps.cfgCc6And, 1)) k = 1;
        else if (cstateCfgCc6En(caps.cfgCc6And, 2)) k = 2;
        else return fail(kCc6NoCc6En);
        r.source = kPortSourceCfg;
    }
    r.ccr = k;
    if (!cstateCfgCc6En(caps.cfgCc6And, k)) return fail(kCc6NoCc6En);
    r.ok = true;
    r.port = base + k;
    r.reason = kCc6Ok;
    return r;
}

// ------------------------------------------------------------------ gates
const char *idleHookReasonName(uint32_t r) {
    switch (r) {
    case kIdleRegistered:     return "registered";
    case kIdleNotEvaluated:   return "not-evaluated";
    case kIdleDisabled:       return "disabled";
    case kIdleDarwinVersion:  return "unsupported-darwin";
    case kIdleHypervisor:     return "hypervisor";
    case kIdleBlocked:        return "blocked";
    case kIdlePmConflict:     return "pm-kext-present";
    case kIdleXcpm:           return "xcpm-active";
    case kIdleNoArat:         return "no-arat";
    case kIdleNoInvariantTsc: return "no-invariant-tsc";
    case kIdleNoCc6:          return "no-cc6";
    default:                  return "unknown";
    }
}

IdleGateResult evaluateIdleGates(const IdleGateInput &in) {
    IdleGateResult r {};
    r.maxMode = clampIdleMode(kIdleCc6, in.mwait, in.cc6);
    auto no = [&](uint32_t why) -> IdleGateResult {
        r.doRegister = false;
        r.mode = kIdleOff;
        r.reason = why;
        return r;
    };
    if (in.requested == kIdleOff) return no(kIdleDisabled);
    if (in.darwinMajor < kIdleDarwinMin || in.darwinMajor > kIdleDarwinMax) return no(kIdleDarwinVersion);
    if (in.hypervisor) return no(kIdleHypervisor);
    if (in.blocked) return no(kIdleBlocked);
    if (in.pmConflict) return no(kIdlePmConflict);
    if (in.xcpmActive) return no(kIdleXcpm);
    if (!in.arat) return no(kIdleNoArat);
    if (!in.tscInvariant) return no(kIdleNoInvariantTsc);
    if (in.requested == kIdleAuto && !in.cc6) return no(kIdleNoCc6);
    r.doRegister = true;
    r.mode = in.requested == kIdleAuto ? (uint32_t)kIdleCc6 : clampIdleMode(in.requested, in.mwait, in.cc6);
    if (r.mode == kIdleOff) r.mode = kIdleHlt;   // unreachable (requested 0 handled above)
    r.reason = kIdleRegistered;
    return r;
}

// ------------------------------------------------------------------ predictor / telemetry
uint64_t usToTscTicks(uint32_t us, uint64_t tscHz) {
    if (us > kIdleThresholdMaxUs) us = kIdleThresholdMaxUs;
    if (tscHz >= (1ull << 43)) tscHz = (1ull << 43) - 1;
    return (uint64_t)us * tscHz / 1000000ull;
}

uint32_t residencyPermille(uint64_t ticks, uint32_t ncpu, uint64_t intervalTicks) {
    if (ncpu == 0 || intervalTicks == 0) return 0;
    if (intervalTicks > UINT64_MAX / ncpu) return 0;          // nonsense interval
    const uint64_t denom = intervalTicks * ncpu;
    if (ticks >= denom) return 1000;
    if (ticks <= UINT64_MAX / 1000ull) return (uint32_t)(ticks * 1000ull / denom);
    return (uint32_t)(ticks / (denom / 1000ull));             // denom > ticks > 2^54 here
}

uint64_t ratePerSecond(uint64_t delta, uint64_t intervalNs) {
    if (intervalNs == 0) return 0;
    if (delta <= UINT64_MAX / 1000000000ull) return delta * 1000000000ull / intervalNs;
    const uint64_t s = intervalNs / 1000000000ull;
    return s ? delta / s : UINT64_MAX;
}

uint32_t idleDeepLatencyUs(const CstEntry *cst, uint32_t n, int32_t cstIndex) {
    if (cst && cstIndex >= 0 && (uint32_t)cstIndex < n && cst[cstIndex].latencyUs != 0)
        return cst[cstIndex].latencyUs;
    return kIdleDeepLatencyDefaultUs;
}

uint32_t idleWindowBusyPermille(uint64_t prevAcc, uint64_t curAcc, uint64_t windowTicks) {
    if (windowTicks == 0) return 1000;
    uint64_t idle = curAcc > prevAcc ? curAcc - prevAcc : 0;
    if (idle >= windowTicks) return 0;
    const uint64_t busy = windowTicks - idle;
    if (busy <= UINT64_MAX / 1000ull) return (uint32_t)(busy * 1000ull / windowTicks);
    return (uint32_t)(busy / (windowTicks / 1000ull));        // windowTicks > 2^54 here
}

} // namespace zen
