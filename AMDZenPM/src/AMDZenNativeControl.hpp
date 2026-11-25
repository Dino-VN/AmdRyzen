// Integration-only admission, domain and MSR prerequisite rules. Host-testable;
// the transport, native policy and permanent idle callbacks keep their contracts.
#ifndef AMDZEN_NATIVE_CONTROL_HPP
#define AMDZEN_NATIVE_CONTROL_HPP
#include "ZenNativePolicy.hpp"
#include "ZenRegs.h"

namespace amdzen {
bool pccEnableMayBeThisBoot(); // kernel integration latch; no hardware access
// Pair with a sleeping command lock: check allowed() AFTER acquiring it and
// hold it through exactly one Channel call. Revocation is nonblocking; acquiring
// and releasing that lock outside the work-loop gate drains admitted commands.
struct NativeAdmission {
    uint32_t flags = 0;
    void pause() { __atomic_fetch_or(&flags, 1u, __ATOMIC_SEQ_CST); }
    void resume() { __atomic_fetch_and(&flags, ~1u, __ATOMIC_SEQ_CST); }
    void revoke() { __atomic_fetch_or(&flags, 2u, __ATOMIC_SEQ_CST); }
    bool allowed() const { return __atomic_load_n(&flags, __ATOMIC_SEQ_CST) == 0; }
};

inline bool boostEnabled(int32_t mode, bool bootDisabled) {
    return mode < 0 ? !bootDisabled : mode == 1;
}
struct DomainBounds { uint32_t lowest, highest, nominal; bool boostOn; };
inline bool domainBounds(const zennative::Topology &top, const zencppc::Caps *caps,
                         const bool *bootDisabled, int32_t boost, uint32_t cpu,
                         DomainBounds &out) {
    if (!caps || !top.cpuCount || top.cpuCount > zennative::kMaxCpus ||
        cpu >= top.cpuCount || top.domainOfCpu[cpu] >= zennative::kMaxCpus) return false;
    out = {0, UINT32_MAX, UINT32_MAX, true};
    uint32_t members = 0;
    for (uint32_t c = 0; c < top.cpuCount; ++c) {
        if (top.domainOfCpu[c] != top.domainOfCpu[cpu]) continue;
        if (++members > 2 || !zennative::validCaps(caps[c])) return false;
        if (caps[c].lowest > out.lowest) out.lowest = caps[c].lowest;
        if (caps[c].highest < out.highest) out.highest = caps[c].highest;
        if (caps[c].nominal < out.nominal) out.nominal = caps[c].nominal;
        out.boostOn &= boostEnabled(boost, bootDisabled && bootDisabled[c]);
    }
    return out.lowest <= out.highest;
}
inline bool neutralRequests(const zennative::Topology &top, const zencppc::Caps *caps,
                            zencppc::Request *out) {
    if (!out) return false;
    for (uint32_t c = 0; c < top.cpuCount; ++c) {
        DomainBounds b {};
        if (!domainBounds(top, caps, nullptr, 1, c, b)) return false;
        out[c] = {b.lowest, b.highest, 0, 0x80};
    }
    return top.cpuCount != 0;
}
inline bool restrictRequest(zencppc::Request &r, const DomainBounds &b,
                            uint32_t floor, uint32_t ceiling) {
    uint32_t hi = r.maxPerf < b.highest ? r.maxPerf : b.highest;
    if (!b.boostOn && b.nominal < hi) hi = b.nominal;
    if (ceiling && ceiling < hi) hi = ceiling;
    if (hi < b.lowest) return false; // never raise a safety/user ceiling
    r.maxPerf = hi;
    if (r.minPerf < b.lowest) r.minPerf = b.lowest;
    if (r.minPerf < floor) r.minPerf = floor;
    if (r.minPerf > hi) r.minPerf = hi;
    return r.desiredPerf == 0 && r.epp <= 255;
}

struct PrerequisitePlan { uint64_t ctl, hwcr; };
inline bool planPrerequisites(bool enabled, bool boostOn, uint64_t ctl, uint64_t hwcr,
                              PrerequisitePlan &out) {
    // An enabled CPU cannot safely be handed back to legacy P-state control.
    // Firmware must have left its P0 prerequisite intact; otherwise quarantine.
    if (enabled && (ctl & 7u)) return false;
    out.ctl = ctl & ~7ull;
    out.hwcr = boostOn ? hwcr & ~HWCR_CPB_DIS : hwcr | HWCR_CPB_DIS;
    return true;
}
} // namespace amdzen
#endif
