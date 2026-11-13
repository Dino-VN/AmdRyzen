//
// SmcKeySet.hpp — which SMC keys AMDZenSMC publishes and where each value comes from.
//
// Header-only, depends only on AMDZenShared.h and SmcEncode.hpp, so the selection
// rules, the ASCII sort and the per-key encoding are host-tested in
// tests/test_smc_encode.cpp. The kernel side (AMDZenSMC.cpp) only turns the list
// into VirtualSMCValue objects.
//
// Rules (docs/research/smc-keys.md §6.2 and its Critic notes):
//   - The key set is fixed before submission; VirtualSMC never sees it change.
//   - Keys are sorted in ASCII order of their 4 characters (digits < upper < lower),
//     because VirtualSMC looks keys up with a binary search.
//   - No frequency keys, nothing Tjmax-relative (TC0E/F/J/T, TCXR), no PSTR / PC0R,
//     and no key VirtualSMC's base store already has (MSTc...).
//
#ifndef AMDZEN_SMC_KEYSET_HPP
#define AMDZEN_SMC_KEYSET_HPP

#include <stdint.h>
#include "AMDZenShared.h"
#include "SmcEncode.hpp"

namespace smc {

// Same value as VirtualSMC's SMC_MAKE_IDENTIFIER outside EFI: in-memory byte order
// is the key's character order. AMDZenSMC.cpp static_asserts the equivalence.
constexpr uint32_t makeKey(char a, char b, char c, char d) {
    return (uint32_t)(uint8_t)a | ((uint32_t)(uint8_t)b << 8) |
           ((uint32_t)(uint8_t)c << 16) | ((uint32_t)(uint8_t)d << 24);
}

// Sort key: the 4 characters as a big-endian number (VirtualSMCKeyValue::compare).
constexpr uint32_t keyOrder(uint32_t key) {
    return ((key & 0xFFu) << 24) | ((key & 0xFF00u) << 8) | ((key >> 8) & 0xFF00u) | (key >> 24);
}

enum class Kind : uint8_t { Sp78, Sp96, Sp3c, Flt };

constexpr uint8_t kindSize(Kind k) { return k == Kind::Flt ? 4 : 2; }

enum class Src : uint8_t {
    Tctl,        // tctl_mC
    Tdie,        // tdie_mC (Tctl minus the model offset; equal to Tctl on 19h)
    Hottest,     // max valid Tccd, else Tctl
    Ccd,         // tccd_mC[arg]
    PkgPower,    // pkgPower_mW (RAPL package)
    CorePower,   // coresPower_mW (sum of per-core RAPL)
    Vcore,       // SVI2 vcore_mV, else highest requested per-core VID
    Vsoc,        // SVI2 vsoc_mV
    Icore,       // SVI2 icore_mA
};

struct KeyDef {
    uint32_t key;
    Kind     kind;
    Src      src;
    uint8_t  arg;
};

// 12 fixed keys + per-core keys (at most 8 under singleCcdIndex()'s guard).
constexpr uint32_t kMaxKeys = 32;
constexpr uint32_t kMaxPerCoreKeys = 8;

inline uint32_t popcount32(uint32_t v) {
    uint32_t n = 0;
    for (; v; v &= v - 1) n++;
    return n;
}

// Per-core temperature keys need a core -> CCD map, which AMDZenInfo does not carry
// (ABI 1 has coreOfCpu[] and apicId[] but no ccdOfCore[]). The map is only certain
// when the part has a single CCD: then every core reads that CCD's sensor. We accept
// that when exactly one CCD sensor is present, there are at most 8 cores (one Zen2/3/4
// CCD) and at most 2 CCX (Zen2 has 2 CCX per CCD; Zen3/4 have 1). Multi-CCD parts get
// TCXc only. Returns the CCD index or -1.
inline int singleCcdIndex(const AMDZenInfo &in) {
    if (!(in.features & AMDZEN_F_TCTL) || !(in.features & AMDZEN_F_CCD_TEMP)) return -1;
    static_assert(AMDZEN_MAX_CCDS < 32, "ccdTempMask is 32 bits");
    const uint32_t mask = in.ccdTempMask & ((1u << AMDZEN_MAX_CCDS) - 1);
    if (popcount32(mask) != 1) return -1;
    if (in.physicalCores == 0 || in.physicalCores > kMaxPerCoreKeys) return -1;
    if (in.numCcx > 2) return -1;
    for (int i = 0; i < (int)AMDZEN_MAX_CCDS; i++)
        if (mask & (1u << i)) return i;
    return -1;
}

// Builds the sorted key list. `s` is the first valid sample or null when none was
// available in time; the SVI2-only keys (IC0C, VCSC) are published only when the
// sample shows a non-zero value, because a model with SVI2 decode may still lack a
// plane (multi-node Zen1 has no core plane on node 0) and an all-zero key is hidden
// by Stats anyway. Returns the number of keys written to out[].
inline uint32_t buildKeySet(const AMDZenInfo &in, const AMDZenSample *s, KeyDef *out, uint32_t max) {
    uint32_t n = 0;
    auto add = [&](uint32_t key, Kind kind, Src src, uint8_t arg) {
        if (n < max) out[n++] = KeyDef{key, kind, src, arg};
    };
    const uint64_t f = in.features;

    if (f & AMDZEN_F_TCTL) {
        add(makeKey('T', 'C', '0', 'D'), Kind::Sp78, Src::Tdie, 0);   // Stats CPU module 1st choice
        add(makeKey('T', 'C', '0', 'P'), Kind::Sp78, Src::Tctl, 0);   // proximity: no board sensor, mirror Tctl
        add(makeKey('T', 'C', '0', 'p'), Kind::Sp78, Src::Tctl, 0);   // iStat default on iMacPro / MacPro7,1
        add(makeKey('T', 'C', 'X', 'c'), Kind::Sp78, Src::Hottest, 0);
        const int ccd = singleCcdIndex(in);
        if (ccd >= 0) {
            // Lower-case 'c' only (desktop spelling); publishing TCxC too doubles the rows in Stats.
            for (uint32_t c = 0; c < in.physicalCores && c < kMaxPerCoreKeys; c++)
                add(makeKey('T', 'C', (char)('0' + c), 'c'), Kind::Sp78, Src::Ccd, (uint8_t)ccd);
        }
    }
    if (f & AMDZEN_F_RAPL) {
        add(makeKey('P', 'C', 'P', 'T'), Kind::Sp96, Src::PkgPower, 0);
        add(makeKey('P', 'C', 'P', 'R'), Kind::Sp96, Src::PkgPower, 0);
    }
    if (f & AMDZEN_F_CORE_ENERGY) {
        add(makeKey('P', 'C', 'P', 'C'), Kind::Sp96, Src::CorePower, 0);
        add(makeKey('P', 'C', '0', 'C'), Kind::Sp96, Src::CorePower, 0);
    }
    const bool svi2 = (f & AMDZEN_F_SVI2) != 0;
    if ((svi2 && s && s->vcore_mV) || (f & AMDZEN_F_HWPSTATUS))
        add(makeKey('V', 'C', '0', 'C'), Kind::Sp3c, Src::Vcore, 0);
    if (svi2 && s && s->vsoc_mV)
        add(makeKey('V', 'C', 'S', 'C'), Kind::Flt, Src::Vsoc, 0);
    if (svi2 && s && s->icore_mA)
        add(makeKey('I', 'C', '0', 'C'), Kind::Flt, Src::Icore, 0);

    // Insertion sort in ASCII order; n is small and this keeps Lilu's qsort out.
    for (uint32_t i = 1; i < n; i++) {
        KeyDef k = out[i];
        uint32_t j = i;
        while (j > 0 && keyOrder(out[j - 1].key) > keyOrder(k.key)) {
            out[j] = out[j - 1];
            j--;
        }
        out[j] = k;
    }
    return n;
}

// Raw value in AMDZenPM units for one key.
inline int64_t sourceValue(const KeyDef &k, const AMDZenInfo &in, const AMDZenSample &s) {
    switch (k.src) {
    case Src::Tctl: return s.tctl_mC;
    case Src::Tdie: return s.tdie_mC;
    case Src::Hottest: {
        bool any = false;
        int32_t best = 0;
        for (uint32_t i = 0; i < AMDZEN_MAX_CCDS; i++) {
            if (!(in.ccdTempMask & (1u << i))) continue;
            if (!any || s.tccd_mC[i] > best) best = s.tccd_mC[i];
            any = true;
        }
        return any ? best : s.tctl_mC;
    }
    case Src::Ccd: return k.arg < AMDZEN_MAX_CCDS ? s.tccd_mC[k.arg] : s.tctl_mC;
    case Src::PkgPower: return s.pkgPower_mW;
    case Src::CorePower: return s.coresPower_mW;
    case Src::Vcore: {
        if (s.vcore_mV) return s.vcore_mV;
        // Fallback: highest requested VID of any core (C001_0293), what the
        // regulator is asked for; not a measurement.
        uint32_t best = 0;
        const uint32_t nc = s.numCores < AMDZEN_MAX_CORES ? s.numCores : AMDZEN_MAX_CORES;
        for (uint32_t c = 0; c < nc; c++)
            if (s.core[c].vid_mV > best) best = s.core[c].vid_mV;
        return best;
    }
    case Src::Vsoc: return s.vsoc_mV;
    case Src::Icore: return s.icore_mA;
    }
    return 0;
}

// Encoded cell (see SmcEncode.hpp) for one key.
inline uint32_t encodeKey(const KeyDef &k, const AMDZenInfo &in, const AMDZenSample &s) {
    // Every source is a 32-bit field, so the narrowing below cannot truncate.
    const int64_t v = sourceValue(k, in, s);
    const uint32_t u = v < 0 ? 0u : (uint32_t)v;
    switch (k.kind) {
    case Kind::Sp78: return encTempSp78((int32_t)v);
    case Kind::Sp96: return encPowerSp96(u);
    case Kind::Sp3c: return encVoltSp3c(u);
    case Kind::Flt:  return encFlt(u);
    }
    return 0;
}

// 4 characters + NUL.
inline void keyName(uint32_t key, char out[5]) {
    out[0] = (char)(key & 0xFF);
    out[1] = (char)((key >> 8) & 0xFF);
    out[2] = (char)((key >> 16) & 0xFF);
    out[3] = (char)((key >> 24) & 0xFF);
    out[4] = '\0';
}

} // namespace smc

#endif // AMDZEN_SMC_KEYSET_HPP
