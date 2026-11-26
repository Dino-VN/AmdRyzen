//
// ZenModel.cpp — pure AMD Zen decode tables and governor math.
//
// Builds both inside the kext (-fapple-kext, no libc) and on the host for the
// unit tests in tests/. Integer math only, no heap, no static constructors.
// Register facts follow docs/research/linux-sensors.md and linux-pstate.md;
// the code itself is original.
//
#include "ZenModel.hpp"
#include "ZenRegs.h"

namespace zen {

// =================================================================== helpers
namespace {

const uint64_t kU64Max = ~0ull;
const uint32_t kU32Max = ~0u;

inline uint32_t sat32(uint64_t v) { return v > kU32Max ? kU32Max : (uint32_t)v; }
inline uint32_t umin32(uint32_t a, uint32_t b) { return a < b ? a : b; }
inline uint32_t umax32(uint32_t a, uint32_t b) { return a > b ? a : b; }

// floor(a * b / c) with a 128-bit intermediate; saturates to UINT64_MAX when the
// quotient does not fit and returns 0 when c == 0. The 64x64->128 multiply is a
// single MULQ on x86_64; the division is done by hand so no compiler-rt helper
// (__udivti3) is pulled into the kext.
uint64_t mulDiv(uint64_t a, uint64_t b, uint64_t c)
{
    if (c == 0)
        return 0;
    __uint128_t p = (__uint128_t)a * b;
    uint64_t hi = (uint64_t)(p >> 64);
    uint64_t lo = (uint64_t)p;
    if (hi == 0)
        return lo / c;
    if (hi >= c)
        return kU64Max;
    uint64_t rem = hi, q = 0;
    for (int i = 63; i >= 0; --i) {
        bool carry = (rem >> 63) != 0;
        rem = (rem << 1) | ((lo >> i) & 1u);
        q <<= 1;
        if (carry || rem >= c) {
            rem -= c;
            q |= 1u;
        }
    }
    return q;
}

// ceil(a * b / c) for small operands (all inputs < 2^32).
inline uint64_t mulDivCeil32(uint32_t a, uint32_t b, uint32_t c)
{
    if (c == 0)
        return 0;
    return ((uint64_t)a * b + c - 1) / c;
}

inline char lowerAscii(char c) { return (c >= 'A' && c <= 'Z') ? (char)(c - 'A' + 'a') : c; }

bool equalsNoCase(const char *a, const char *b)
{
    if (!a || !b)
        return false;
    while (*a && *b) {
        if (lowerAscii(*a) != lowerAscii(*b))
            return false;
        ++a;
        ++b;
    }
    return *a == *b;
}

inline bool inRange(uint32_t v, uint32_t lo, uint32_t hi) { return v >= lo && v <= hi; }

// ---------------------------------------------------------------- identity tables
struct TctlOffsetEntry {
    const char *brandSub;
    int32_t     offset_mC;
};

// k10temp tctl_offset_table (family 17h only), matched by brand substring.
const TctlOffsetEntry kTctlOffsets17h[] = {
    { "AMD Ryzen 5 1600X",         20000 },
    { "AMD Ryzen 7 1700X",         20000 },
    { "AMD Ryzen 7 1800X",         20000 },
    { "AMD Ryzen 7 2700X",         10000 },
    { "AMD Ryzen Threadripper 19", 27000 },
    { "AMD Ryzen Threadripper 29", 27000 },
};

// SVI2 current factors (zenpower), micro-amps per LSB.
const uint32_t kZen1CoreFactor_uA = 1039211;
const uint32_t kZen1SocFactor_uA  = 360772;
const uint32_t kZen2CoreFactor_uA = 658823;
const uint32_t kZen2SocFactor_uA  = 294300;

bool brandHas(const char *brand, const char *sub) { return brand && containsStr(brand, sub); }

uint32_t zenGenerationFor(Vendor vendor, uint32_t family, uint32_t model)
{
    if (vendor == kVendorHygon)
        return family == 0x18 ? 1 : 0;           // Dhyana = Zen1 derivative
    if (vendor != kVendorAMD)
        return 0;
    switch (family) {
    case 0x17:
        if (inRange(model, 0x00, 0x2F) || inRange(model, 0x50, 0x5F))
            return 1;
        if (inRange(model, 0x30, 0x4F) || inRange(model, 0x60, 0x7F) ||
            inRange(model, 0x90, 0x91) || inRange(model, 0xA0, 0xAF))
            return 2;
        return 0;
    case 0x19:
        if (inRange(model, 0x00, 0x0F) || inRange(model, 0x20, 0x5F))
            return 3;
        if (inRange(model, 0x10, 0x1F) || inRange(model, 0x60, 0xAF))
            return 4;
        return 0;
    case 0x1A:
        if (inRange(model, 0x00, 0x2F) || inRange(model, 0x40, 0x4F) ||
            inRange(model, 0x60, 0x7F) || inRange(model, 0xD0, 0xD7))
            return 5;
        if (inRange(model, 0x50, 0x5F) || inRange(model, 0x80, 0xAF) ||
            inRange(model, 0xC0, 0xCF) || inRange(model, 0xD8, 0xEF))
            return 6;
        return 0;
    default:
        return 0;
    }
}

const char *codenameFor(Vendor vendor, uint32_t family, uint32_t model, const char *brand)
{
    if (vendor == kVendorHygon)
        return family == 0x18 ? "Dhyana" : "Unknown";
    if (vendor != kVendorAMD)
        return "Unknown";
    const bool epyc = brandHas(brand, "EPYC");
    const bool tr   = brandHas(brand, "Threadripper");
    switch (family) {
    case 0x17:
        switch (model) {
        case 0x01: return epyc ? "Naples" : tr ? "Whitehaven" : "Summit Ridge";
        case 0x08: return tr ? "Colfax" : "Pinnacle Ridge";
        case 0x11: return "Raven Ridge";
        case 0x18: return "Picasso";
        case 0x20: return "Dali";
        case 0x31: return epyc ? "Rome" : "Castle Peak";
        case 0x47: return "Cyan Skillfish";
        case 0x60: return "Renoir";
        case 0x68: return "Lucienne";
        case 0x71: return "Matisse";
        case 0x90:
        case 0x91: return "Van Gogh";
        default:
            if (inRange(model, 0xA0, 0xAF))
                return "Mendocino";
            return "Unknown";
        }
    case 0x19:
        if (inRange(model, 0x00, 0x07))
            return tr ? "Chagall" : "Milan";
        if (inRange(model, 0x08, 0x0F))
            return "Chagall";
        if (inRange(model, 0x10, 0x1F))
            return tr ? "Storm Peak" : "Genoa";
        if (inRange(model, 0x20, 0x2F))
            return "Vermeer";
        if (inRange(model, 0x40, 0x4F))
            return "Rembrandt";
        if (inRange(model, 0x50, 0x5F))
            return "Cezanne";
        if (inRange(model, 0x60, 0x6F))
            return "Raphael";
        if (inRange(model, 0x70, 0x7F))
            return "Phoenix";
        if (inRange(model, 0xA0, 0xAF))
            return "Bergamo";
        return "Unknown";
    case 0x1A:
        if (inRange(model, 0x00, 0x0F))
            return tr ? "Shimada Peak" : "Turin";
        if (inRange(model, 0x10, 0x1F))
            return "Turin Dense";
        if (inRange(model, 0x20, 0x2F))
            return "Strix Point";
        if (inRange(model, 0x40, 0x4F))
            return "Granite Ridge";
        if (inRange(model, 0x60, 0x6F))
            return "Krackan Point";
        if (inRange(model, 0x70, 0x7F))
            return "Strix Halo";
        return "Unknown";
    default:
        return "Unknown";
    }
}

// k10temp CCD map (master). Sets offset/slots, leaves both 0 when unsupported.
void ccdMapFor(uint32_t family, uint32_t model, uint32_t *offset, uint32_t *slots)
{
    *offset = 0;
    *slots = 0;
    switch (family) {
    case 0x17:
        if (model == 0x01 || model == 0x08 || model == 0x11 || model == 0x18) {
            *offset = 0x154; *slots = 4;
        } else if (model == 0x31 || model == 0x47 || model == 0x60 || model == 0x68 || model == 0x71) {
            *offset = 0x154; *slots = 8;
        } else if (inRange(model, 0xA0, 0xAF)) {
            *offset = 0x300; *slots = 8;
        }
        break;
    case 0x19:
        if (inRange(model, 0x00, 0x01) || model == 0x08 || model == 0x21 || inRange(model, 0x50, 0x5F)) {
            *offset = 0x154; *slots = 8;
        } else if (inRange(model, 0x40, 0x4F)) {
            *offset = 0x300; *slots = 8;
        } else if (inRange(model, 0x60, 0x7F)) {
            *offset = 0x308; *slots = 8;
        } else if (inRange(model, 0x10, 0x1F) || inRange(model, 0xA0, 0xAF)) {
            *offset = 0x300; *slots = 12;
        }
        break;
    case 0x1A:
        if (inRange(model, 0x00, 0x1F)) {
            *offset = 0x1F0; *slots = 16;
        } else if (inRange(model, 0x40, 0x4F)) {
            *offset = 0x308; *slots = 8;
        }
        break;
    default:
        break;
    }
}

// SVI2 telemetry plane map (k10temp v5.8-5.10, zenpower/zenpower3). AMD only.
void sviMapFor(uint32_t family, uint32_t model, CpuModelInfo *o)
{
    o->sviCoreAddr = o->sviSocAddr = 0;
    o->sviCoreFactor_uA = o->sviSocFactor_uA = 0;
    bool zen1 = false;
    if (family == 0x17) {
        switch (model) {
        case 0x01: case 0x08: case 0x11: case 0x18:
            o->sviCoreAddr = SMN_SVI_PLANE_A; o->sviSocAddr = SMN_SVI_PLANE_B; zen1 = true;
            break;
        case 0x31:
            o->sviCoreAddr = SMN_SVI_PLANE_C; o->sviSocAddr = SMN_SVI_PLANE_B;
            break;
        case 0x71:
            o->sviCoreAddr = SMN_SVI_PLANE_B; o->sviSocAddr = SMN_SVI_PLANE_A;
            break;
        case 0x60: case 0x68:
            o->sviCoreAddr = SMN_SVI_APU_CORE; o->sviSocAddr = SMN_SVI_APU_SOC;
            break;
        default:
            return;
        }
    } else if (family == 0x19) {
        if (inRange(model, 0x00, 0x01)) {
            o->sviCoreAddr = SMN_SVI_PLANE_C; o->sviSocAddr = SMN_SVI_PLANE_B;
        } else if (model == 0x21) {
            o->sviCoreAddr = SMN_SVI_PLANE_B; o->sviSocAddr = SMN_SVI_PLANE_A;
        } else if (inRange(model, 0x50, 0x5F)) {
            o->sviCoreAddr = SMN_SVI_APU_CORE; o->sviSocAddr = SMN_SVI_APU_SOC;
        } else {
            return;
        }
    } else {
        return;                                   // 1Ah and others: SVI3, unsupported
    }
    o->sviCoreFactor_uA = zen1 ? kZen1CoreFactor_uA : kZen2CoreFactor_uA;
    o->sviSocFactor_uA  = zen1 ? kZen1SocFactor_uA  : kZen2SocFactor_uA;
}

uint32_t defaultTjMaxFor(Vendor vendor, uint32_t family, uint32_t model, uint32_t gen)
{
    if (vendor != kVendorAMD)
        return 0;
    switch (gen) {
    case 1:
    case 2:
        return 95000;                             // Zen1 / Zen+ / Zen2
    case 3:
        if (family == 0x19) {
            if (inRange(model, 0x20, 0x2F))
                return 90000;                     // Vermeer desktop
            if (inRange(model, 0x40, 0x5F))
                return 95000;                     // Rembrandt / Cezanne / Barcelo mobile APU
        }
        return 0;
    case 4:
        if (family == 0x19 && inRange(model, 0x60, 0x7F))
            return 95000;                         // Raphael desktop / Phoenix mobile APU
        return 0;
    case 5:
        return 95000;
    default:
        return 0;
    }
}

// ---------------------------------------------------------------- P-state table helpers
struct Range { uint32_t lo, hi; };

// Allowed index range [fastest..slowest] intersected with [0..count-1].
Range allowedRange(const PStateTable &t)
{
    Range r;
    uint32_t last = t.count ? t.count - 1 : 0;
    if (last >= AMDZEN_MAX_PSTATES)
        last = AMDZEN_MAX_PSTATES - 1;
    r.hi = umin32(t.slowest, last);
    r.lo = umin32(t.fastest, r.hi);
    return r;
}

inline uint32_t clampIdx(uint32_t idx, Range r)
{
    if (idx < r.lo) return r.lo;
    if (idx > r.hi) return r.hi;
    return idx;
}

// Highest allowed frequency <= target (CPUFREQ_RELATION_H); slowest if none.
uint32_t resolveHigh(const PStateTable &t, uint32_t targetMHz)
{
    Range r = allowedRange(t);
    for (uint32_t i = r.lo; i <= r.hi; ++i) {
        uint32_t f = t.freqMHz[i];
        if (f != 0 && f <= targetMHz)
            return i;
    }
    return r.hi;
}

// Frequency of the first/last non-zero entries of the whole table (cpuinfo max/min).
uint32_t cpuinfoMax(const PStateTable &t)
{
    uint32_t n = umin32(t.count, AMDZEN_MAX_PSTATES);
    for (uint32_t i = 0; i < n; ++i)
        if (t.freqMHz[i])
            return t.freqMHz[i];
    return 0;
}

uint32_t cpuinfoMin(const PStateTable &t)
{
    uint32_t n = umin32(t.count, AMDZEN_MAX_PSTATES);
    for (uint32_t i = n; i > 0; --i)
        if (t.freqMHz[i - 1])
            return t.freqMHz[i - 1];
    return 0;
}

// EWMA step that always converges (rounds the step magnitude up).
uint32_t ewmaStep(uint32_t ewma, uint32_t x, uint32_t shift)
{
    if (shift == 0 || shift > 16)
        return x;
    uint32_t round = (1u << shift) - 1;
    if (x > ewma)
        return ewma + ((x - ewma + round) >> shift);
    return ewma - ((ewma - x + round) >> shift);
}

// schedutil util smoothing: a rise is taken at once (like util_est), a fall decays
// by (ewma - x) >> shift per tick, so a burst is never diluted on the way up.
uint32_t utilFilter(uint32_t ewma, uint32_t x, uint32_t shift)
{
    return x >= ewma ? x : ewmaStep(ewma, x, shift);
}

// schedutil fast-up: a core busy for at least fastUpPct of the window goes
// straight to the fastest allowed state (ondemand-style), whatever the slow
// frequency-invariant util says.
bool fastUp(const GovTunables &tun, uint32_t busyPermille)
{
    return tun.fastUpPct != 0 && busyPermille >= umin32(tun.fastUpPct, 100) * 10u;
}

// Hold logic shared by schedutil / ondemand (legacy and CPPC). The caller says
// whether `next` lowers performance. Decreases are deferred while holdTicks > 0;
// every non-decrease re-arms the hold to factor-1 ticks, so after the last tick
// that needed the current level a decrease happens `factor` ticks later.
uint32_t applyHold(GovState *st, uint32_t current, uint32_t next, bool isDecrease, uint32_t factor)
{
    uint32_t rearm = factor > 1 ? factor - 1 : 0;
    if (!isDecrease) {
        st->holdTicks = rearm;
        return next;
    }
    if (st->holdTicks > 0) {
        st->holdTicks--;
        return current;
    }
    return next;
}

} // namespace

// =================================================================== identity
void decodeFms(uint32_t eax, uint32_t *family, uint32_t *model, uint32_t *stepping)
{
    uint32_t baseFam = (eax >> 8) & 0xF;
    uint32_t baseMod = (eax >> 4) & 0xF;
    uint32_t extFam  = (eax >> 20) & 0xFF;
    uint32_t extMod  = (eax >> 16) & 0xF;
    uint32_t fam = baseFam;
    uint32_t mod = baseMod;
    if (baseFam == 0xF)
        fam += extFam;
    if (baseFam == 0xF || baseFam == 0x6)
        mod |= extMod << 4;
    if (family)   *family = fam;
    if (model)    *model = mod;
    if (stepping) *stepping = eax & 0xF;
}

bool identify(Vendor vendor, uint32_t family, uint32_t model, const char *brand, CpuModelInfo *out)
{
    if (!out)
        return false;
    CpuModelInfo o;
    o.zenGeneration = 0;
    o.codename = "Unknown";
    o.hasTctl = false;
    o.ccdOffset = 0;
    o.maxCcds = 0;
    o.tctlOffset_mC = 0;
    o.dispNegative = false;
    o.sviCoreAddr = o.sviSocAddr = 0;
    o.sviCoreFactor_uA = o.sviSocFactor_uA = 0;
    o.pstateDef1Ah = false;
    o.sviVidFormula = false;
    o.defaultTjMax_mC = 0;

    bool supported = (vendor == kVendorAMD && (family == 0x17 || family == 0x19 || family == 0x1A)) ||
                     (vendor == kVendorHygon && family == 0x18);
    if (!supported) {
        *out = o;
        return false;
    }

    o.zenGeneration = zenGenerationFor(vendor, family, model);
    o.codename = codenameFor(vendor, family, model, brand);

    if (vendor == kVendorHygon) {
        // No usable Hygon temperature path (see linux-sensors.md §1.4); 17h-style PStateDef.
        o.sviVidFormula = true;
        *out = o;
        return true;
    }

    o.hasTctl = true;
    ccdMapFor(family, model, &o.ccdOffset, &o.maxCcds);

    if (family == 0x17) {
        for (size_t i = 0; i < sizeof(kTctlOffsets17h) / sizeof(kTctlOffsets17h[0]); ++i) {
            if (brandHas(brand, kTctlOffsets17h[i].brandSub)) {
                o.tctlOffset_mC = kTctlOffsets17h[i].offset_mC;
                break;
            }
        }
        o.dispNegative = brandHas(brand, "3255");
    }

    sviMapFor(family, model, &o);

    o.pstateDef1Ah = (family == 0x1A);
    // SVI2 VID encoding (1550 - 6.25*VID) only on SVI2 platforms: AM4/SP3/TR4/sTRX4
    // and FP5/FP6 APUs. Mendocino (17h A0-AF), Rembrandt (19h 40-4F) and every Zen4/
    // Zen5 part use SVI3 with a different VID scale.
    if (family == 0x17)
        o.sviVidFormula = !inRange(model, 0xA0, 0xAF);
    else if (family == 0x19)
        o.sviVidFormula = inRange(model, 0x00, 0x0F) || inRange(model, 0x20, 0x2F) ||
                          inRange(model, 0x50, 0x5F);

    o.defaultTjMax_mC = defaultTjMaxFor(vendor, family, model, o.zenGeneration);
    *out = o;
    return true;
}

// =================================================================== temperature
int32_t decodeTctl(uint32_t raw)
{
    int32_t t = (int32_t)((raw >> TCTL_CUR_TEMP_SHIFT) * 125u);
    if ((raw & TCTL_RANGE_SEL) || (raw & TCTL_TJ_SEL_MASK) == TCTL_TJ_SEL_MASK)
        t -= 49000;
    return t;
}

int32_t clampTctl(int32_t mC, bool dispNegative)
{
    if (mC < 0 && !dispNegative)
        return 0;
    return mC;
}

bool decodeCcdTemp(uint32_t raw, int32_t *mC)
{
    if (raw == 0xFFFFFFFFu || !(raw & CCD_TEMP_VALID))
        return false;
    if (mC)
        *mC = (int32_t)((raw & CCD_TEMP_MASK) * 125u) - 49000;
    return true;
}

// =================================================================== SVI2
uint32_t sviVidToMilliVolt(uint32_t vid)
{
    if (vid >= 0xF8)
        return 0;
    // 1550 - 6.25*VID mV, rounded to nearest (values stay positive for VID < 0xF8).
    return (155000u - vid * 625u + 50u) / 100u;
}

uint32_t sviPlaneMilliVolt(uint32_t raw)
{
    return sviVidToMilliVolt((raw >> 16) & 0xFF);
}

uint32_t sviPlaneMilliAmp(uint32_t raw, uint32_t factor_uA)
{
    return sat32(((uint64_t)(raw & 0xFF) * factor_uA) / 1000u);
}

// =================================================================== P-states
PStateDecoded decodePStateDef(uint64_t raw, bool layout1Ah, bool sviVidFormula)
{
    PStateDecoded d;
    d.enabled = (raw >> 63) & 1;
    d.vid = (uint32_t)((raw >> 14) & 0xFF);
    d.iddValue = (uint32_t)((raw >> 22) & 0xFF);
    d.iddDiv = (uint32_t)((raw >> 30) & 0x3);
    if (layout1Ah) {
        d.fid = (uint32_t)(raw & 0xFFF);
        d.did = 0;
        d.freqMHz = d.fid > 0x0F ? d.fid * 5u : 0;
    } else {
        d.fid = (uint32_t)(raw & 0xFF);
        d.did = (uint32_t)((raw >> 8) & 0x3F);
        d.freqMHz = d.did ? (200u * d.fid) / d.did : 0;
    }
    d.voltage_mV = sviVidFormula ? sviVidToMilliVolt(d.vid) : 0;
    return d;
}

bool encodePStateDef(uint64_t raw, uint32_t fid, uint32_t did, uint32_t vid, uint64_t *out)
{
    // 17h/18h/19h layout only (callers must not use this for 1Ah). FID < 0x10 is
    // reserved per PPR; VID >= 0xF8 means "off" and is refused as a safety net.
    if (!out)
        return false;
    if (fid < 0x10 || fid > 0xFF)
        return false;
    // CpuDfsId: 0x08..0x1A in steps of 1 (÷1..÷3.25), then even values up to 0x2C
    // (÷5.5); odd values above 0x1A and everything above 0x2C are reserved.
    if (did < 0x08 || did > 0x2C || (did > 0x1A && (did & 1)))
        return false;
    if (vid >= 0xF8)
        return false;
    const uint64_t mask = 0xFFull | (0x3Full << 8) | (0xFFull << 14);
    uint64_t v = raw & ~mask;
    v |= (uint64_t)fid;
    v |= (uint64_t)did << 8;
    v |= (uint64_t)vid << 14;
    *out = v;
    return true;
}

// =================================================================== CPPC
CppcCaps decodeCppcCap1(uint64_t raw)
{
    CppcCaps c;
    c.lowest = (uint32_t)(raw & 0xFF);
    c.lowestNonlinear = (uint32_t)((raw >> 8) & 0xFF);
    c.nominal = (uint32_t)((raw >> 16) & 0xFF);
    c.highest = (uint32_t)((raw >> 24) & 0xFF);
    return c;
}

uint64_t encodeCppcReq(uint64_t prev, uint32_t maxPerf, uint32_t minPerf, uint32_t desPerf, uint32_t epp)
{
    return (prev & 0xFFFFFFFF00000000ull) |
           (uint64_t)(maxPerf & 0xFF) |
           ((uint64_t)(minPerf & 0xFF) << 8) |
           ((uint64_t)(desPerf & 0xFF) << 16) |
           ((uint64_t)(epp & 0xFF) << 24);
}

uint32_t cppcPerfToFreq(uint32_t perf, uint32_t nominalPerf, uint32_t nominalFreqMHz)
{
    if (nominalPerf == 0)
        return 0;
    return sat32(mulDivCeil32(nominalFreqMHz, perf, nominalPerf));
}

uint32_t cppcFreqToPerf(uint32_t freqMHz, uint32_t nominalPerf, uint32_t nominalFreqMHz,
                        uint32_t lowestPerf, uint32_t highestPerf)
{
    uint64_t p = nominalFreqMHz ? mulDivCeil32(freqMHz, nominalPerf, nominalFreqMHz) : 0;
    if (p < lowestPerf)
        p = lowestPerf;
    if (p > highestPerf)
        p = highestPerf;
    return (uint32_t)p;
}

// =================================================================== RAPL energy
uint32_t energyUpdate(EnergyAccumulator *acc, uint32_t raw, bool rebase)
{
    if (!acc)
        return 0;
    if (!acc->primed || rebase) {
        acc->lastRaw = raw;
        acc->primed = true;
        return 0;
    }
    uint32_t delta = raw - acc->lastRaw;          // modular 2^32
    acc->lastRaw = raw;
    acc->totalRaw += delta;
    return delta;
}

uint64_t energyRawToMicroJoule(uint64_t raw, uint32_t esu)
{
    if (esu > 31)
        esu = 31;                                 // ESU is a 5-bit field
    uint64_t mask = (1ull << esu) - 1;
    return (raw >> esu) * 1000000ull + (((raw & mask) * 1000000ull) >> esu);
}

uint32_t powerMilliWatt(uint64_t deltaMicroJoule, uint64_t intervalNs)
{
    // uJ / ns = 1e3 W = 1e6 mW
    return sat32(mulDiv(deltaMicroJoule, 1000000ull, intervalNs));
}

// =================================================================== APERF/MPERF
PerfDerived derivePerf(const PerfCounters &prev, const PerfCounters &cur, uint64_t tscHz,
                       uint32_t refFreqMHz, bool haveIrperf)
{
    PerfDerived d;
    d.busyPermille = 0;
    d.effFreqMHz = 0;
    d.util1024 = 0;
    d.ipcX100 = 0;

    const uint64_t dA = cur.aperf - prev.aperf;
    const uint64_t dM = cur.mperf - prev.mperf;
    const uint64_t dT = cur.tsc - prev.tsc;
    const uint64_t dI = cur.irperf - prev.irperf;

    if (dT)
        d.busyPermille = umin32(sat32(mulDiv(dM, 1000, dT)), 1000);

    if (dM && tscHz)
        d.effFreqMHz = sat32(mulDiv(dA, tscHz, dM) / 1000000ull);

    if (dT) {
        uint64_t u;
        if (tscHz && refFreqMHz) {
            uint64_t avgHz = mulDiv(dA, tscHz, dT);                  // average delivered Hz
            u = mulDiv(avgHz, 1024, (uint64_t)refFreqMHz * 1000000ull);
        } else {
            u = mulDiv(dA, 1024, dT);                                // assume ref == TSC
        }
        d.util1024 = u > 1024 ? 1024 : (uint32_t)u;
    }

    if (haveIrperf && dA)
        d.ipcX100 = sat32(mulDiv(dI, 100, dA));
    return d;
}

// =================================================================== governors
void defaultTunables(GovTunables *t)
{
    if (!t)
        return;
    t->upThreshold = 80;
    t->downThreshold = 20;
    t->samplingDownFactor = 1;
    t->freqStepPct = 5;
    t->utilHeadroomPct = 125;
    t->utilEwmaShift = 1;
    t->fastUpPct = 60;
}

uint32_t resolvePState(const PStateTable &t, uint32_t targetMHz)
{
    Range r = allowedRange(t);
    for (uint32_t i = r.hi + 1; i > r.lo; --i) {
        uint32_t f = t.freqMHz[i - 1];
        if (f != 0 && f >= targetMHz)
            return i - 1;
    }
    return r.lo;
}

uint32_t closestPState(const PStateTable &t, uint32_t targetMHz)
{
    Range r = allowedRange(t);
    uint32_t best = r.lo;
    uint32_t bestDiff = kU32Max;
    for (uint32_t i = r.lo; i <= r.hi; ++i) {
        uint32_t f = t.freqMHz[i];
        if (f == 0)
            continue;
        uint32_t diff = f > targetMHz ? f - targetMHz : targetMHz - f;
        if (diff < bestDiff) {                    // strict: ties keep the faster entry
            bestDiff = diff;
            best = i;
        }
    }
    return best;
}

uint32_t utilFromBusy(uint32_t busyPermille, uint32_t curMHz, uint32_t refMHz)
{
    if (refMHz == 0)
        return 0;
    if (busyPermille > 1000)
        busyPermille = 1000;
    uint64_t u = (uint64_t)busyPermille * 1024u * curMHz / (1000ull * refMHz);
    return u > 1024 ? 1024 : (uint32_t)u;
}

uint32_t governLegacy(uint32_t governor, const GovTunables &tun, const PStateTable &t,
                      GovState *st, uint32_t util1024, uint32_t busyPermille, uint32_t userIdx)
{
    Range r = allowedRange(t);
    if (!st)
        return r.lo;
    if (util1024 > 1024)
        util1024 = 1024;
    if (busyPermille > 1000)
        busyPermille = 1000;

    if (!st->init) {
        st->curIdx = clampIdx(st->curIdx, r);
        st->holdTicks = 0;
        st->utilEwma = util1024;
        st->reqFreqMHz = t.freqMHz[st->curIdx];
        st->init = true;
    }
    const uint32_t cur = clampIdx(st->curIdx, r);
    const uint32_t factor = umax32(tun.samplingDownFactor, 1);
    uint32_t next = cur;

    switch (governor) {
    case kAMDZenGovPerformance:
        next = r.lo;
        break;
    case kAMDZenGovPowersave:
        next = r.hi;
        break;
    case kAMDZenGovUserspace:
        next = clampIdx(userIdx, r);
        break;
    case kAMDZenGovSchedutil: {
        st->utilEwma = utilFilter(st->utilEwma, util1024, tun.utilEwmaShift);
        uint32_t want;
        if (fastUp(tun, busyPermille)) {
            want = r.lo;
        } else {
            uint64_t target = (uint64_t)tun.utilHeadroomPct * cpuinfoMax(t) * st->utilEwma / (100u * 1024u);
            want = resolvePState(t, sat32(target));
        }
        next = applyHold(st, cur, want, want > cur, factor);
        break;
    }
    case kAMDZenGovOndemand: {
        uint32_t want;
        if (busyPermille > tun.upThreshold * 10u) {
            want = r.lo;
        } else {
            uint32_t fmin = cpuinfoMin(t), fmax = cpuinfoMax(t);
            uint32_t span = fmax > fmin ? fmax - fmin : 0;
            uint32_t f = fmin + (uint32_t)(((uint64_t)busyPermille * span) / 1000u);
            want = closestPState(t, f);
        }
        next = applyHold(st, cur, want, want > cur, factor);
        break;
    }
    case kAMDZenGovConservative: {
        if (tun.freqStepPct == 0)
            break;
        const uint32_t fFast = t.freqMHz[r.lo];
        const uint32_t fSlow = t.freqMHz[r.hi];
        uint32_t req = st->reqFreqMHz;
        if (req > fFast || req < fSlow)
            req = t.freqMHz[cur];
        uint32_t step = (uint32_t)(((uint64_t)tun.freqStepPct * fFast) / 100u);
        if (step == 0)
            step = 1;
        if (busyPermille > tun.upThreshold * 10u) {
            st->holdTicks = 0;                    // down-skip counter
            if (req < fFast) {
                req = umin32(req + step, fFast);
                next = resolveHigh(t, req);
            }
        } else if (++st->holdTicks >= factor) {
            st->holdTicks = 0;
            if (busyPermille < tun.downThreshold * 10u && req > fSlow) {
                req = req > fSlow + step ? req - step : fSlow;
                next = resolvePState(t, req);
            }
        }
        st->reqFreqMHz = req;
        break;
    }
    case kAMDZenGovCppcActive:
    case kAMDZenGovNone:
    default:
        next = cur;
        break;
    }

    next = clampIdx(next, r);
    st->curIdx = next;
    return next;
}

uint32_t governCppcDesired(uint32_t governor, const GovTunables &tun, uint32_t minPerf,
                           uint32_t maxPerf, GovState *st, uint32_t util1024,
                           uint32_t busyPermille, uint32_t userPerf)
{
    if (minPerf > maxPerf) {
        uint32_t tmp = minPerf;
        minPerf = maxPerf;
        maxPerf = tmp;
    }
    if (!st)
        return maxPerf;
    if (util1024 > 1024)
        util1024 = 1024;
    if (busyPermille > 1000)
        busyPermille = 1000;

    auto clampPerf = [minPerf, maxPerf](uint32_t p) -> uint32_t {
        return p < minPerf ? minPerf : (p > maxPerf ? maxPerf : p);
    };

    if (!st->init) {
        st->curIdx = maxPerf;
        st->holdTicks = 0;
        st->utilEwma = util1024;
        st->reqFreqMHz = maxPerf;
        st->init = true;
    }
    const uint32_t cur = clampPerf(st->curIdx);
    const uint32_t factor = umax32(tun.samplingDownFactor, 1);
    uint32_t next = cur;

    switch (governor) {
    case kAMDZenGovPerformance:
        next = maxPerf;
        break;
    case kAMDZenGovPowersave:
        next = minPerf;
        break;
    case kAMDZenGovUserspace:
        next = clampPerf(userPerf);
        break;
    case kAMDZenGovSchedutil: {
        st->utilEwma = utilFilter(st->utilEwma, util1024, tun.utilEwmaShift);
        // maxPerf is the scale reference (util1024 is relative to highest perf).
        uint64_t num = (uint64_t)tun.utilHeadroomPct * maxPerf * st->utilEwma;
        uint64_t den = 100u * 1024u;
        uint32_t want = fastUp(tun, busyPermille) ? maxPerf : clampPerf(sat32((num + den - 1) / den));
        next = applyHold(st, cur, want, want < cur, factor);
        break;
    }
    case kAMDZenGovOndemand: {
        uint32_t want;
        if (busyPermille > tun.upThreshold * 10u)
            want = maxPerf;
        else
            want = minPerf + (uint32_t)(((uint64_t)busyPermille * (maxPerf - minPerf) + 500u) / 1000u);
        want = clampPerf(want);
        next = applyHold(st, cur, want, want < cur, factor);
        break;
    }
    case kAMDZenGovConservative: {
        if (tun.freqStepPct == 0)
            break;
        uint32_t req = st->reqFreqMHz;            // running request in perf units
        if (req > maxPerf || req < minPerf)
            req = cur;
        uint32_t step = (uint32_t)(((uint64_t)tun.freqStepPct * maxPerf) / 100u);
        if (step == 0)
            step = 1;
        if (busyPermille > tun.upThreshold * 10u) {
            st->holdTicks = 0;
            req = umin32(req + step, maxPerf);
        } else if (++st->holdTicks >= factor) {
            st->holdTicks = 0;
            if (busyPermille < tun.downThreshold * 10u)
                req = req > minPerf + step ? req - step : minPerf;
        }
        st->reqFreqMHz = req;
        next = req;
        break;
    }
    case kAMDZenGovCppcActive:
    case kAMDZenGovNone:
    default:
        next = cur;
        break;
    }

    next = clampPerf(next);
    st->curIdx = next;
    return next;
}

// =================================================================== misc
uint32_t cppcEffectiveHighest(const uint32_t *highestPerCore, uint32_t n, bool *prefcore)
{
    if (prefcore)
        *prefcore = false;
    if (!highestPerCore || n == 0)
        return 0;
    const uint32_t first = highestPerCore[0];
    for (uint32_t i = 1; i < n; ++i) {
        if (highestPerCore[i] != first) {
            if (prefcore)
                *prefcore = true;
            return CPPC_HIGHEST_PERF_PREFCORE;
        }
    }
    return first;
}

bool containsStr(const char *hay, const char *needle)
{
    if (!hay || !needle)
        return false;
    if (!*needle)
        return true;
    for (; *hay; ++hay) {
        const char *h = hay, *n = needle;
        while (*h && *n && *h == *n) {
            ++h;
            ++n;
        }
        if (!*n)
            return true;
    }
    return false;
}

namespace {
const char *const kGovNames[kAMDZenGovCount] = {
    "none",          // kAMDZenGovNone
    "performance",   // kAMDZenGovPerformance
    "powersave",     // kAMDZenGovPowersave
    "schedutil",     // kAMDZenGovSchedutil
    "ondemand",      // kAMDZenGovOndemand
    "conservative",  // kAMDZenGovConservative
    "userspace",     // kAMDZenGovUserspace
    "active",        // kAMDZenGovCppcActive
};
} // namespace

uint32_t governorFromName(const char *name)
{
    if (!name)
        return kAMDZenGovCount;
    for (uint32_t i = 0; i < kAMDZenGovCount; ++i)
        if (equalsNoCase(name, kGovNames[i]))
            return i;
    return kAMDZenGovCount;
}

const char *governorName(uint32_t gov)
{
    return gov < kAMDZenGovCount ? kGovNames[gov] : "unknown";
}

} // namespace zen
