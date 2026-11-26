#include "ZenNativePolicy.hpp"

namespace zennative {
namespace {
uint32_t minU(uint32_t a, uint32_t b) { return a < b ? a : b; }
uint32_t maxU(uint32_t a, uint32_t b) { return a > b ? a : b; }
uint32_t clampU(uint32_t v, uint32_t lo, uint32_t hi) { return minU(maxU(v, lo), hi); }
uint32_t epp(uint32_t v) { return minU(v, 255); }
bool held(bool present, uint64_t since, uint64_t now, uint32_t ms) {
    return present && now >= since && now - since < ms;
}
void mark(uint64_t *mask, uint32_t cpu) { mask[cpu / 64] |= 1ull << (cpu % 64); }
bool urgentChange(const zencppc::Request &old, const zencppc::Request &next) {
    // Demand increases and safety caps do not wait for the relaxation hold.
    return next.minPerf > old.minPerf || next.epp < old.epp || next.maxPerf < old.maxPerf;
}
void observe(CpuHistory &h, const CpuSnapshot &s, uint64_t now) {
    if (!s.observed) { h = {}; return; }
    // First observation primes counters: do not revive historical RT events
    // after stop/rearm or sleep. The current class still takes effect immediately.
    if ((h.seen && h.rtEvents != s.rtEvents) || s.urgency == kUrgencyRealtime) {
        h.lastRtMs = now; h.hasRt = true;
    }
    if ((h.seen && h.foregroundEvents != s.foregroundEvents) ||
        s.urgency == kUrgencyNormal || s.urgency == kUrgencyLowPriority || s.urgency == kUrgencyUnknown) {
        h.lastForegroundMs = now; h.hasForeground = true;
    }
    h.rtEvents = s.rtEvents; h.foregroundEvents = s.foregroundEvents; h.seen = true;
}
} // namespace

Tunables defaultTunables() { return {128, 32, 192, 224, 160, 80, 100, 100, 100, 50, 450, 0, 250}; }

namespace {
inline bool hasSub(const char *s, const char *sub) {
    if (!s || !sub || !*sub) return false;
    for (; *s; ++s) {
        const char *a = s, *b = sub;
        while (*a && *b && *a == *b) { ++a; ++b; }
        if (!*b) return true;
    }
    return false;
}
inline bool isDigit(char c) { return c >= '0' && c <= '9'; }
inline bool isWordEnd(char c) { return c == '\0' || c == ' ' || c == '-' || c == '\t'; }
// Matches AMD mobile SKU suffixes immediately after model digits: e.g. "5800U", "5825U", "7840HS", "6800H", "7945HX".
bool hasMobileSuffix(const char *brand, const char *suffix) {
    if (!brand || !suffix || !*suffix) return false;
    for (const char *p = brand; *p; ++p) {
        if (!isDigit(*p)) continue;
        const char *q = p;
        while (isDigit(*q)) ++q;
        if (q - p < 3) { p = q - 1; continue; } // require 3+ digit SKU (e.g. 5800, 7840, 350)
        const char *a = q, *b = suffix;
        while (*a && *b && *a == *b) { ++a; ++b; }
        if (!*b && isWordEnd(*a)) return true;
        p = q - 1;
    }
    return false;
}
inline bool isMobileApuModel(uint32_t family, uint32_t model) {
    if (family == 0x17) {
        return model == 0x11 || model == 0x18 || model == 0x20 || model == 0x60 ||
               model == 0x68 || model == 0x90 || model == 0x91 || (model >= 0xA0 && model <= 0xAF);
    }
    if (family == 0x19) {
        return (model >= 0x40 && model <= 0x5F) || (model >= 0x70 && model <= 0x7F);
    }
    if (family == 0x1A) {
        return (model >= 0x20 && model <= 0x2F) || (model >= 0x60 && model <= 0x7F);
    }
    return false;
}
inline bool eqNoCase(const char *a, const char *b) {
    if (!a || !b) return false;
    while (*a && *b) {
        char ca = (*a >= 'A' && *a <= 'Z') ? char(*a - 'A' + 'a') : *a;
        char cb = (*b >= 'A' && *b <= 'Z') ? char(*b - 'A' + 'a') : *b;
        if (ca != cb) return false;
        ++a; ++b;
    }
    return *a == *b;
}
} // namespace

CpuProfile detectCpuProfile(uint32_t family, uint32_t model, const char *brand, uint32_t physicalCores) {
    if (hasSub(brand, "Threadripper") || hasSub(brand, "EPYC") || physicalCores >= 24)
        return kProfileWorkstation;
    if (hasMobileSuffix(brand, "U") || hasMobileSuffix(brand, "C") || hasMobileSuffix(brand, "e") ||
        (family == 0x17 && (model == 0x20 || model == 0x90 || model == 0x91 ||
                            (model >= 0xA0 && model <= 0xAF)))) {
        return kProfileMobileEco;
    }
    if (hasMobileSuffix(brand, "HX") || hasMobileSuffix(brand, "HS") || hasMobileSuffix(brand, "H") ||
        hasSub(brand, "Ryzen AI")) {
        return kProfileMobilePerf;
    }
    // Desktop G/GE/GT APUs (e.g. 5600G, 5700G, 8700G) share silicon with mobile APUs
    // but run in desktop sockets with desktop cooling/power budgets.
    if (hasMobileSuffix(brand, "G") || hasMobileSuffix(brand, "GE") || hasMobileSuffix(brand, "GT"))
        return kProfileDesktop;
    if (isMobileApuModel(family, model))
        return kProfileMobilePerf;
    return kProfileDesktop;
}

Tunables tunablesForProfile(CpuProfile profile) {
    Tunables t = defaultTunables();
    switch (profile) {
        case kProfileMobileEco:
            t.baseEpp = 128;
            t.realtimeEpp = 48;
            t.backgroundEpp = 224;
            t.lowPowerEpp = 240;
            t.batteryEpp = 192;
            t.realtimeMinPct = 70;
            t.busyBoostThreshold = 500;
            t.busyBoostEpp = 16;
            t.rtFloorBusyThreshold = 300;
            break;
        case kProfileMobilePerf:
            t.baseEpp = 112;
            t.realtimeEpp = 32;
            t.backgroundEpp = 208;
            t.lowPowerEpp = 224;
            t.batteryEpp = 176;
            t.realtimeMinPct = 80;
            t.busyBoostThreshold = 450;
            t.busyBoostEpp = 0;
            t.rtFloorBusyThreshold = 250;
            break;
        case kProfileWorkstation:
            t.baseEpp = 112;
            t.realtimeEpp = 32;
            t.backgroundEpp = 192;
            t.lowPowerEpp = 224;
            t.batteryEpp = 160;
            t.realtimeMinPct = 80;
            t.busyBoostThreshold = 500;
            t.busyBoostEpp = 0;
            t.rtFloorBusyThreshold = 250;
            break;
        case kProfileDesktop:
        case kProfileAuto:
        default:
            break;
    }
    return t;
}

uint32_t recommendedIdleThresholdUs(CpuProfile profile) {
    switch (profile) {
        case kProfileMobileEco:   return 35;
        case kProfileWorkstation: return 75;
        case kProfileMobilePerf:
        case kProfileDesktop:
        case kProfileAuto:
        default:                  return 50;
    }
}

const char *profileName(CpuProfile profile) {
    switch (profile) {
        case kProfileAuto:        return "auto";
        case kProfileDesktop:     return "desktop";
        case kProfileMobilePerf:  return "mobile-perf";
        case kProfileMobileEco:   return "mobile-eco";
        case kProfileWorkstation: return "workstation";
        default:                  return "unknown";
    }
}

bool profileFromName(const char *name, CpuProfile &out) {
    if (eqNoCase(name, "auto"))        { out = kProfileAuto; return true; }
    if (eqNoCase(name, "desktop"))     { out = kProfileDesktop; return true; }
    if (eqNoCase(name, "mobile-perf") || eqNoCase(name, "laptop-perf") || eqNoCase(name, "laptop")) {
        out = kProfileMobilePerf; return true;
    }
    if (eqNoCase(name, "mobile-eco") || eqNoCase(name, "laptop-eco") || eqNoCase(name, "eco")) {
        out = kProfileMobileEco; return true;
    }
    if (eqNoCase(name, "workstation") || eqNoCase(name, "hedt") || eqNoCase(name, "server")) {
        out = kProfileWorkstation; return true;
    }
    return false;
}
bool validCaps(const zencppc::Caps &c) {
    return c.lowest != 0 && c.lowest <= c.lowestNonlinear &&
           c.lowestNonlinear <= c.nominal && c.nominal <= c.highest;
}
ThermalLevel decodeThermalWarning(uint32_t raw) {
    // IOPM.h: documented common values and the x86 platform warning/trap values.
    // Unknown platform-defined values must not be treated as numeric severity.
    switch (raw) {
        case 0: return kThermalNominal;
        case 5: return kThermalModerate;
        case 100: return kThermalHeavy;
        case 10: return kThermalCritical;
        case 110: return kThermalEmergency;
        default: return kThermalUnknown;
    }
}

ThermalLevel thermalFromTdie(int32_t tdie_mC, uint32_t tjMax_mC, ThermalLevel prev) {
    if (tjMax_mC < 50000 || tjMax_mC > 150000 || tdie_mC <= 0) return kThermalUnknown;
    const int32_t tj = static_cast<int32_t>(tjMax_mC);
    constexpr int32_t kHyst = 3000;          // 3 C hysteresis to prevent clock oscillation
    const int32_t thEmerg = tj + 5000;
    const int32_t thCrit  = tj;
    const int32_t thHeavy = tj - 3000;
    const int32_t thMod   = tj - 6000;

    if (tdie_mC >= thEmerg) return kThermalEmergency;
    if (prev == kThermalEmergency && tdie_mC >= thEmerg - kHyst) return kThermalEmergency;

    if (tdie_mC >= thCrit) return kThermalCritical;
    if (prev >= kThermalCritical && tdie_mC >= thCrit - kHyst) return kThermalCritical;

    if (tdie_mC >= thHeavy) return kThermalHeavy;
    if (prev >= kThermalHeavy && tdie_mC >= thHeavy - kHyst) return kThermalHeavy;

    if (tdie_mC >= thMod) return kThermalModerate;
    if (prev >= kThermalModerate && tdie_mC >= thMod - kHyst) return kThermalModerate;

    return kThermalNominal;
}

SystemSignals resolveSystemSignals(const SystemSignals &raw, int32_t acOverride,
                                   int32_t lpmOverride, uint32_t thermalMode,
                                   ThermalLevel tdieLevel) {
    SystemSignals out = raw;
    if (acOverride == 1) out.powerSource = kPowerExternal;
    else if (acOverride == 0) out.powerSource = kPowerInternal;

    if (lpmOverride == 0) out.lowPowerMode = kOff;
    else if (lpmOverride == 1) out.lowPowerMode = kOn;

    switch (thermalMode) {
        case kThermalCapOff:
            out.thermal = kThermalNominal;
            break;
        case kThermalCapMacOs:
            out.thermal = raw.thermal;
            break;
        case kThermalCapTdie:
            out.thermal = tdieLevel;
            break;
        case kThermalCapAuto:
        default:
            out.thermal = (tdieLevel != kThermalUnknown) ? tdieLevel : raw.thermal;
            break;
    }
    return out;
}

zencppc::Request decide(const CpuSnapshot &cpu, bool recentRT, bool recentForeground,
                       const SystemSignals &sys, const zencppc::Caps &c, const Tunables &t) {
    if (!validCaps(c)) return {};           // never send this; tick clears valid bits
    zencppc::Request r {c.lowest, c.highest, 0, epp(t.baseEpp)};
    if (sys.powerSource == kPowerInternal) r.epp = maxU(r.epp, epp(t.batteryEpp));
    const bool isRt = recentRT || (cpu.observed && cpu.urgency == kUrgencyRealtime);
    const bool isBg = cpu.observed && !recentForeground &&
                      (cpu.urgency == kUrgencyBackground || cpu.urgency == kUrgencyIdle);
    const bool heavyLoad = cpu.observed && cpu.busyObserved && !isBg &&
                           cpu.busyPermille >= t.busyBoostThreshold;
    if (isRt) {
        // Only pin minPerf when busy accounting is absent or shows real RT load;
        // brief desktop VSync/audio RT ticks on otherwise-idle cores keep low minPerf
        // so clocks can drop and CC6 stays effective.
        if (!cpu.busyObserved || cpu.busyPermille >= t.rtFloorBusyThreshold) {
            r.minPerf = maxU(c.lowestNonlinear, static_cast<uint32_t>(
                        static_cast<uint64_t>(c.nominal) * minU(t.realtimeMinPct, 100) / 100));
        }
        r.epp = minU(epp(t.baseEpp), epp(t.realtimeEpp));
    } else if (isBg) {
        r.epp = maxU(r.epp, epp(t.backgroundEpp));
    }
    if (heavyLoad) {
        const uint32_t boostEpp = sys.powerSource == kPowerInternal
            ? maxU(epp(t.busyBoostEpp), epp(t.realtimeEpp))
            : epp(t.busyBoostEpp);
        r.epp = minU(r.epp, minU(epp(t.baseEpp), boostEpp));
        // On AC, match Linux amd-pstate performance floor (nominal, never highest)
        // so bursty single-core workloads do not dip below base clock across short
        // syscalls or page faults, while multi-core PPT throttling stays unaffected.
        if (sys.powerSource != kPowerInternal)
            r.minPerf = maxU(r.minPerf, c.nominal);
    }
    if (sys.lowPowerMode == kOn) {
        r.epp = maxU(r.epp, epp(t.lowPowerEpp));
        r.maxPerf = minU(r.maxPerf, c.nominal);
    }
    uint32_t spanPct = 100;
    switch (sys.thermal) {
        case kThermalModerate: spanPct = 75; break;
        case kThermalHeavy: spanPct = 50; break;
        case kThermalCritical: spanPct = 25; break;
        case kThermalEmergency: spanPct = 0; break;
        default: break;
    }
    const uint32_t thermalMax = c.lowestNonlinear + static_cast<uint32_t>(
        static_cast<uint64_t>(c.highest - c.lowestNonlinear) * spanPct / 100);
    r.maxPerf = clampU(minU(r.maxPerf, thermalMax), c.lowest, c.highest);
    r.minPerf = clampU(r.minPerf, c.lowest, r.maxPerf);
    return r;
}

void reset(PolicyState &state) {
    for (uint32_t i = 0; i < kMaxCpus; ++i) { state.cpu[i] = {}; state.domain[i] = {}; }
    state.lastTickMs = state.lastBatchMs = 0;
    state.hasTick = state.hasBatch = false;
}
void invalidateRequests(PolicyState &state) {
    for (uint32_t i = 0; i < kMaxCpus; ++i) state.domain[i].valid = false;
}

bool tick(PolicyState &state, const Topology &top, const CpuSnapshot *cpus,
          const SystemSignals &sys, const zencppc::Caps *caps, const Tunables &tun,
          uint64_t now, TickResult &out) {
    out.cpuCount = out.changedCount = 0;
    for (uint32_t i = 0; i < kMaskWords; ++i) out.changed[i] = out.valid[i] = 0;
    if (!cpus || !caps || top.cpuCount == 0 || top.cpuCount > kMaxCpus) return false;
    // At most 1 KiB of temporary topology storage, not kMaxCpus requests/states.
    uint16_t first[kMaxCpus], second[kMaxCpus];
    for (uint32_t d = 0; d < kMaxCpus; ++d) first[d] = second[d] = kMaxCpus;
    for (uint32_t c = 0; c < top.cpuCount; ++c) {
        const uint32_t d = top.domainOfCpu[c];
        if (d >= kMaxCpus) return false;
        if (first[d] == kMaxCpus) first[d] = static_cast<uint16_t>(c);
        else if (second[d] == kMaxCpus) second[d] = static_cast<uint16_t>(c);
        else return false;                 // this API supports two-thread HW_ALL
    }
    if (state.hasTick && now < state.lastTickMs) reset(state);
    state.lastTickMs = now; state.hasTick = true;
    out.cpuCount = top.cpuCount;
    for (uint32_t c = 0; c < top.cpuCount; ++c) {
        out.requests[c] = {};
        observe(state.cpu[c], cpus[c], now);
    }
    const bool batchReady = !state.hasBatch ||
        now - state.lastBatchMs >= maxU(tun.batchIntervalMs, 50);
    for (uint32_t d = 0; d < kMaxCpus; ++d) {
        const uint32_t a = first[d], b = second[d];
        DomainHistory &h = state.domain[d];
        if (a == kMaxCpus) { h = {}; continue; }
        if (!validCaps(caps[a]) || (b != kMaxCpus && !validCaps(caps[b]))) { h = {}; continue; }
        uint32_t lo = caps[a].lowest, hi = caps[a].highest;
        const CpuHistory &ah = state.cpu[a];
        zencppc::Request next = decide(cpus[a], held(ah.hasRt, ah.lastRtMs, now, tun.rtHoldMs),
            held(ah.hasForeground, ah.lastForegroundMs, now, tun.foregroundHoldMs), sys, caps[a], tun);
        if (b != kMaxCpus) {
            const CpuHistory &bh = state.cpu[b];
            const zencppc::Request br = decide(cpus[b], held(bh.hasRt, bh.lastRtMs, now, tun.rtHoldMs),
                held(bh.hasForeground, bh.lastForegroundMs, now, tun.foregroundHoldMs), sys, caps[b], tun);
            lo = maxU(lo, caps[b].lowest); hi = minU(hi, caps[b].highest);
            next.minPerf = maxU(next.minPerf, br.minPerf);
            next.maxPerf = minU(next.maxPerf, br.maxPerf);
            next.epp = minU(next.epp, br.epp);
        }
        // Intersect safety ceilings without raising either sibling's maximum.
        // lowestNonlinear is an efficiency threshold, not a hardware floor.
        // If a ceiling is below either hardware lowest, no shared request is safe.
        if (lo > next.maxPerf) { h = {}; continue; }
        next.minPerf = clampU(next.minPerf, lo, next.maxPerf);
        if (!h.pendingValid || !zencppc::sameRequest(h.pending, next)) {
            h.pending = next; h.pendingSinceMs = now; h.pendingValid = true;
        }
        const bool boundsChanged = h.valid && (h.request.minPerf < lo || h.request.maxPerf > hi ||
                                               h.request.maxPerf < lo);
        const bool different = !h.valid || !zencppc::sameRequest(h.request, next);
        const bool settled = now - h.pendingSinceMs >= tun.relaxHoldMs;
        const bool send = different && batchReady &&
            (!h.valid || boundsChanged || urgentChange(h.request, next) || settled);
        if (send) { h.request = next; h.valid = true; }
        // A capability shrink invalidates the old request until a batch can send
        // the replacement. Never present the obsolete range as a valid request.
        if (!h.valid || (boundsChanged && !send)) continue;
        out.requests[a] = h.request; mark(out.valid, a);
        if (b != kMaxCpus) { out.requests[b] = h.request; mark(out.valid, b); }
        if (send) {
            mark(out.changed, a); ++out.changedCount;
            if (b != kMaxCpus) { mark(out.changed, b); ++out.changedCount; }
        }
    }
    if (out.changedCount) { state.lastBatchMs = now; state.hasBatch = true; }
    return true;
}
} // namespace zennative
