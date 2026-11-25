// ZenCppcTypes.hpp — shared value types between the PCC CPPC transport (ZenPcc*)
// and the native-signal policy (ZenNativePolicy*). Pure data, no kernel headers,
// usable by both the kext and host unit tests.
#ifndef ZEN_CPPC_TYPES_HPP
#define ZEN_CPPC_TYPES_HPP
#include <stdint.h>

namespace zencppc {

// Abstract CPPC performance capabilities of one CPU (ACPI 6.x _CPC, read via PCC).
struct Caps {
    uint32_t highest;          // Highest Performance
    uint32_t nominal;          // Nominal Performance
    uint32_t lowestNonlinear;  // Lowest Nonlinear Performance
    uint32_t lowest;           // Lowest Performance
    uint32_t nominalFreqMHz;   // _CPC Nominal Frequency (0 = not provided)
    uint32_t lowestFreqMHz;    // _CPC Lowest Frequency  (0 = not provided)
};

// One CPU's control request. desiredPerf == 0 selects autonomous selection
// (firmware picks the operating point inside [minPerf, maxPerf] guided by epp).
struct Request {
    uint32_t minPerf;
    uint32_t maxPerf;
    uint32_t desiredPerf;
    uint32_t epp;              // 0 = performance .. 255 = energy
};

inline bool sameRequest(const Request &a, const Request &b) {
    return a.minPerf == b.minPerf && a.maxPerf == b.maxPerf &&
           a.desiredPerf == b.desiredPerf && a.epp == b.epp;
}

} // namespace zencppc
#endif
