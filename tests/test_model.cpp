// Host unit tests for ZenModel.cpp. Build/run: make -C tests
#include <cstdio>
#include <cstdint>
#include <cstring>
#include "ZenModel.hpp"
#include "ZenRegs.h"

using namespace zen;

// ---------------------------------------------------------------- tiny framework
static int g_checks = 0, g_failures = 0;
static const char *g_test = "";

#define CHECK(cond) do { ++g_checks; if (!(cond)) { ++g_failures; \
    std::printf("  FAIL %s:%d [%s] %s\n", __FILE__, __LINE__, g_test, #cond); } } while (0)
#define CHECK_EQ(a, b) do { ++g_checks; long long _va = (long long)(a), _vb = (long long)(b); \
    if (_va != _vb) { ++g_failures; std::printf("  FAIL %s:%d [%s] %s == %s (%lld vs %lld)\n", \
    __FILE__, __LINE__, g_test, #a, #b, _va, _vb); } } while (0)
#define CHECK_U64(a, b) do { ++g_checks; unsigned long long _va = (a), _vb = (b); \
    if (_va != _vb) { ++g_failures; std::printf("  FAIL %s:%d [%s] %s == %s (0x%llx vs 0x%llx)\n", \
    __FILE__, __LINE__, g_test, #a, #b, _va, _vb); } } while (0)
#define CHECK_STR(a, b) do { ++g_checks; const char *_sa = (a), *_sb = (b); \
    if (!_sa || std::strcmp(_sa, _sb) != 0) { ++g_failures; std::printf("  FAIL %s:%d [%s] %s == \"%s\" (got \"%s\")\n", \
    __FILE__, __LINE__, g_test, #a, _sb, _sa ? _sa : "(null)"); } } while (0)

struct TestCase { const char *name; void (*fn)(); };
#define MAX_TESTS 64
static TestCase g_tests[MAX_TESTS];
static int g_ntests = 0;
struct Registrar { Registrar(const char *n, void (*f)()) { g_tests[g_ntests++] = {n, f}; } };
#define TEST(name) static void name(); static Registrar reg_##name(#name, name); static void name()

// ---------------------------------------------------------------- helpers
static uint64_t mkPstate(uint32_t fid, uint32_t did, uint32_t vid, uint32_t idd, uint32_t iddDiv, bool en)
{
    return (uint64_t)fid | ((uint64_t)did << 8) | ((uint64_t)vid << 14) |
           ((uint64_t)idd << 22) | ((uint64_t)iddDiv << 30) | ((uint64_t)en << 63);
}

static PStateTable table5700X()
{
    PStateTable t{};
    t.count = 3;
    t.freqMHz[0] = 3400; t.freqMHz[1] = 2800; t.freqMHz[2] = 2200;
    t.fastest = 0; t.slowest = 2;
    return t;
}

static GovTunables tunables(uint32_t ewma = 0, uint32_t sdf = 1)
{
    GovTunables g;
    defaultTunables(&g);
    g.utilEwmaShift = ewma;
    g.samplingDownFactor = sdf;
    return g;
}

// ---------------------------------------------------------------- identity
TEST(fms_decode)
{
    uint32_t f, m, s;
    decodeFms(0x00A20F12, &f, &m, &s);          // 5700X
    CHECK_EQ(f, 0x19); CHECK_EQ(m, 0x21); CHECK_EQ(s, 2);
    decodeFms(0x00870F10, &f, &m, &s);          // Matisse 3700X
    CHECK_EQ(f, 0x17); CHECK_EQ(m, 0x71); CHECK_EQ(s, 0);
    decodeFms(0x00B40F40, &f, &m, &s);          // Granite Ridge
    CHECK_EQ(f, 0x1A); CHECK_EQ(m, 0x44); CHECK_EQ(s, 0);
}

TEST(identify_vermeer_5700x)
{
    CpuModelInfo i;
    CHECK(identify(kVendorAMD, 0x19, 0x21, "AMD Ryzen 7 5700X 8-Core Processor", &i));
    CHECK_EQ(i.zenGeneration, 3);
    CHECK_STR(i.codename, "Vermeer");
    CHECK(i.hasTctl);
    CHECK_EQ(i.ccdOffset, 0x154);
    CHECK_EQ(SMN_THM_TCON_CUR_TMP + i.ccdOffset, 0x59954);
    CHECK_EQ(i.maxCcds, 8);
    CHECK_EQ(i.tctlOffset_mC, 0);
    CHECK(!i.dispNegative);
    CHECK_EQ(i.sviCoreAddr, 0x5A010);
    CHECK_EQ(i.sviSocAddr, 0x5A00C);
    CHECK_EQ(i.sviCoreFactor_uA, 658823);
    CHECK_EQ(i.sviSocFactor_uA, 294300);
    CHECK(!i.pstateDef1Ah);
    CHECK(i.sviVidFormula);
    CHECK_EQ(i.defaultTjMax_mC, 90000);
}

TEST(identify_matisse)
{
    CpuModelInfo i;
    CHECK(identify(kVendorAMD, 0x17, 0x71, "AMD Ryzen 7 3700X 8-Core Processor", &i));
    CHECK_EQ(i.zenGeneration, 2);
    CHECK_STR(i.codename, "Matisse");
    CHECK_EQ(i.ccdOffset, 0x154); CHECK_EQ(i.maxCcds, 8);
    CHECK_EQ(i.sviCoreAddr, 0x5A010); CHECK_EQ(i.sviSocAddr, 0x5A00C);
    CHECK_EQ(i.sviCoreFactor_uA, 658823); CHECK_EQ(i.sviSocFactor_uA, 294300);
    CHECK_EQ(i.tctlOffset_mC, 0);
    CHECK(i.sviVidFormula); CHECK(!i.pstateDef1Ah);
    CHECK_EQ(i.defaultTjMax_mC, 95000);
}

TEST(identify_granite_ridge)
{
    CpuModelInfo i;
    CHECK(identify(kVendorAMD, 0x1A, 0x44, "AMD Ryzen 9 9950X 16-Core Processor", &i));
    CHECK_EQ(i.zenGeneration, 5);
    CHECK_STR(i.codename, "Granite Ridge");
    CHECK_EQ(i.ccdOffset, 0x308); CHECK_EQ(i.maxCcds, 8);
    CHECK_EQ(i.sviCoreAddr, 0); CHECK_EQ(i.sviSocAddr, 0);
    CHECK(i.pstateDef1Ah); CHECK(!i.sviVidFormula);
    CHECK_EQ(i.defaultTjMax_mC, 95000);
}

TEST(identify_summit_ridge_1800x)
{
    CpuModelInfo i;
    CHECK(identify(kVendorAMD, 0x17, 0x01, "AMD Ryzen 7 1800X Eight-Core Processor", &i));
    CHECK_EQ(i.zenGeneration, 1);
    CHECK_STR(i.codename, "Summit Ridge");
    CHECK_EQ(i.tctlOffset_mC, 20000);
    CHECK_EQ(i.ccdOffset, 0x154); CHECK_EQ(i.maxCcds, 4);
    CHECK_EQ(i.sviCoreAddr, 0x5A00C); CHECK_EQ(i.sviSocAddr, 0x5A010);
    CHECK_EQ(i.sviCoreFactor_uA, 1039211); CHECK_EQ(i.sviSocFactor_uA, 360772);
    CHECK_EQ(i.defaultTjMax_mC, 95000);
    CHECK(identify(kVendorAMD, 0x17, 0x08, "AMD Ryzen 7 2700X Eight-Core Processor", &i));
    CHECK_EQ(i.tctlOffset_mC, 10000);
    CHECK_STR(i.codename, "Pinnacle Ridge");
    CHECK(identify(kVendorAMD, 0x17, 0x08, "AMD Ryzen Threadripper 2950X 16-Core Processor", &i));
    CHECK_EQ(i.tctlOffset_mC, 27000);
    CHECK(identify(kVendorAMD, 0x17, 0x01, "AMD Ryzen Embedded V1605B", &i));
    CHECK_EQ(i.tctlOffset_mC, 0);
    CHECK(!i.dispNegative);
}

TEST(identify_misc)
{
    CpuModelInfo i;
    CHECK(identify(kVendorAMD, 0x17, 0x11, "", &i));
    CHECK(identify(kVendorAMD, 0x17, 0x18, "AMD Ryzen Embedded R3255U", &i));
    CHECK(i.dispNegative);
    CHECK(identify(kVendorAMD, 0x17, 0x31, "AMD Ryzen Threadripper 3970X 32-Core Processor", &i));
    CHECK_STR(i.codename, "Castle Peak");
    CHECK_EQ(i.sviCoreAddr, 0x5A014); CHECK_EQ(i.sviSocAddr, 0x5A010);
    CHECK_EQ(i.tctlOffset_mC, 0);
    CHECK(identify(kVendorAMD, 0x17, 0x31, "AMD EPYC 7742 64-Core Processor", &i));
    CHECK_STR(i.codename, "Rome");
    CHECK(identify(kVendorAMD, 0x17, 0x60, nullptr, &i));
    CHECK_STR(i.codename, "Renoir");
    CHECK_EQ(i.sviCoreAddr, 0x6F038); CHECK_EQ(i.sviSocAddr, 0x6F03C);
    CHECK_EQ(i.sviCoreFactor_uA, 658823);
    CHECK(identify(kVendorAMD, 0x17, 0xA0, "", &i));
    CHECK_EQ(i.ccdOffset, 0x300); CHECK_EQ(i.zenGeneration, 2); CHECK(!i.sviVidFormula); // Mendocino SVI3
    CHECK(identify(kVendorAMD, 0x19, 0x01, "AMD EPYC 7763 64-Core Processor", &i));
    CHECK_STR(i.codename, "Milan");
    CHECK_EQ(i.sviCoreAddr, 0x5A014); CHECK_EQ(i.sviSocAddr, 0x5A010);
    CHECK_EQ(i.defaultTjMax_mC, 0);
    CHECK(identify(kVendorAMD, 0x19, 0x50, "AMD Ryzen 7 5700G with Radeon Graphics", &i));
    CHECK_STR(i.codename, "Cezanne");
    CHECK_EQ(i.sviCoreAddr, 0x6F038); CHECK_EQ(i.ccdOffset, 0x154); CHECK_EQ(i.defaultTjMax_mC, 95000);
    CHECK(identify(kVendorAMD, 0x19, 0x44, "", &i));
    CHECK_STR(i.codename, "Rembrandt");
    CHECK_EQ(i.ccdOffset, 0x300); CHECK_EQ(i.sviCoreAddr, 0); CHECK(!i.sviVidFormula); CHECK_EQ(i.defaultTjMax_mC, 95000);   // SVI3
    CHECK(identify(kVendorAMD, 0x19, 0x61, "AMD Ryzen 9 7950X 16-Core Processor", &i));
    CHECK_STR(i.codename, "Raphael");
    CHECK_EQ(i.zenGeneration, 4); CHECK_EQ(i.ccdOffset, 0x308); CHECK(!i.sviVidFormula);
    CHECK_EQ(i.defaultTjMax_mC, 95000);
    CHECK(identify(kVendorAMD, 0x19, 0x11, "AMD EPYC 9654 96-Core Processor", &i));
    CHECK_STR(i.codename, "Genoa");
    CHECK_EQ(i.ccdOffset, 0x300); CHECK_EQ(i.maxCcds, 12);
    CHECK(identify(kVendorAMD, 0x19, 0x74, "", &i));
    CHECK_STR(i.codename, "Phoenix"); CHECK_EQ(i.ccdOffset, 0x308); CHECK_EQ(i.defaultTjMax_mC, 95000);
    CHECK(identify(kVendorAMD, 0x1A, 0x02, "AMD EPYC 9755 128-Core Processor", &i));
    CHECK_STR(i.codename, "Turin");
    CHECK_EQ(i.ccdOffset, 0x1F0); CHECK_EQ(i.maxCcds, 16);
    CHECK(identify(kVendorAMD, 0x1A, 0x24, "", &i));
    CHECK_STR(i.codename, "Strix Point"); CHECK_EQ(i.ccdOffset, 0); CHECK_EQ(i.maxCcds, 0);
    CHECK(identify(kVendorAMD, 0x1A, 0x50, "", &i));
    CHECK_EQ(i.zenGeneration, 6);
    // Hygon Dhyana
    CHECK(identify(kVendorHygon, 0x18, 0x00, "Hygon C86 3185  8-core Processor", &i));
    CHECK(!i.hasTctl); CHECK_EQ(i.ccdOffset, 0); CHECK_EQ(i.sviCoreAddr, 0);
    CHECK(!i.pstateDef1Ah); CHECK(i.sviVidFormula); CHECK_EQ(i.zenGeneration, 1);
    CHECK_STR(i.codename, "Dhyana");
    // Unsupported
    CHECK(!identify(kVendorAMD, 0x15, 0x02, "AMD FX-8350", &i));
    CHECK_STR(i.codename, "Unknown");
    CHECK(!identify(kVendorAMD, 0x18, 0x00, "", &i));
    CHECK(!identify(kVendorOther, 0x19, 0x21, "", &i));
    CHECK(!identify(kVendorAMD, 0x19, 0x21, "", nullptr));
}

// ---------------------------------------------------------------- temperature
TEST(tctl_decode)
{
    CHECK_EQ(decodeTctl(0x2D0u << 21), 90000);                          // 720 * 125
    CHECK_EQ(decodeTctl((0x2D0u << 21) | TCTL_RANGE_SEL), 41000);
    CHECK_EQ(decodeTctl((0x2D0u << 21) | 0x30000), 41000);              // TJ_SEL == 3
    CHECK_EQ(decodeTctl((0x2D0u << 21) | 0x10000), 90000);              // TJ_SEL == 1: no offset
    CHECK_EQ(decodeTctl((0x2D0u << 21) | 0x20000), 90000);
    CHECK_EQ(decodeTctl((0x2D0u << 21) | 0x30000 | TCTL_RANGE_SEL), 41000); // applied once
    CHECK_EQ(decodeTctl(0xFFE00000u), 2047 * 125);
    int32_t neg = decodeTctl((100u << 21) | TCTL_RANGE_SEL);            // 12.5 - 49 = -36.5 C
    CHECK_EQ(neg, -36500);
    CHECK_EQ(clampTctl(neg, false), 0);
    CHECK_EQ(clampTctl(neg, true), -36500);
    CHECK_EQ(clampTctl(45000, false), 45000);
}

TEST(ccd_decode)
{
    int32_t t = 12345;
    CHECK(decodeCcdTemp(0x800u | 0x2E0u, &t));                          // 736*125 - 49000
    CHECK_EQ(t, 43000);
    t = 12345;
    CHECK(!decodeCcdTemp(0x2E0u, &t));                                  // valid bit clear
    CHECK_EQ(t, 12345);
    CHECK(!decodeCcdTemp(0xFFFFFFFFu, &t));                             // PCI error pattern
    CHECK(!decodeCcdTemp(0, &t));
    CHECK(decodeCcdTemp(0x800u, &t));
    CHECK_EQ(t, -49000);                                                // not clamped
    CHECK(decodeCcdTemp(0xFFFFF800u | 0x188u, &t));                     // high garbage bits ignored
    CHECK_EQ(t, 392 * 125 - 49000);
}

// ---------------------------------------------------------------- SVI2
TEST(svi2)
{
    CHECK_EQ(sviVidToMilliVolt(0), 1550);
    CHECK_EQ(sviVidToMilliVolt(0x48), 1100);
    CHECK_EQ(sviVidToMilliVolt(0x49), 1094);                            // 1093.75 rounds to 1094
    CHECK_EQ(sviVidToMilliVolt(0x4A), 1088);                            // 1087.5 rounds up
    CHECK_EQ(sviVidToMilliVolt(0xF7), 6);                               // 6.25
    CHECK_EQ(sviVidToMilliVolt(0xF8), 0);
    CHECK_EQ(sviVidToMilliVolt(0xFF), 0);
    uint32_t plane = 0x00480020u;                                       // VID 0x48, IDD 32
    CHECK_EQ(sviPlaneMilliVolt(plane), 1100);
    CHECK_EQ(sviPlaneMilliAmp(plane, 658823), 21082);                   // 32 * 658.823
    CHECK_EQ(sviPlaneMilliAmp(0x000000FFu, 1039211), 264998);           // 255 * 1039.211 = 264998.8 -> floor
    CHECK_EQ(sviPlaneMilliAmp(0xFFFF0000u, 658823), 0);
}

// ---------------------------------------------------------------- P-states
TEST(pstate_decode_17h_19h)
{
    PStateDecoded p = decodePStateDef(mkPstate(0x88, 0x08, 0x48, 0x5A, 1, true), false, true);
    CHECK(p.enabled);
    CHECK_EQ(p.freqMHz, 3400);
    CHECK_EQ(p.fid, 0x88); CHECK_EQ(p.did, 8); CHECK_EQ(p.vid, 0x48);
    CHECK_EQ(p.voltage_mV, 1100);
    CHECK_EQ(p.iddValue, 0x5A); CHECK_EQ(p.iddDiv, 1);
    p = decodePStateDef(mkPstate(0x88, 0x0A, 0x60, 0, 0, true), false, true);
    CHECK_EQ(p.freqMHz, 2720);                                          // /1.25
    CHECK_EQ(p.voltage_mV, 950);
    p = decodePStateDef(mkPstate(0x88, 0x10, 0x60, 0, 0, true), false, true);
    CHECK_EQ(p.freqMHz, 1700);                                          // /2
    p = decodePStateDef(mkPstate(0x8C, 0x0C, 0x6A, 0, 0, false), false, false);
    CHECK(!p.enabled);
    CHECK_EQ(p.freqMHz, 2333);                                          // 200*140/12 = 2333.3
    CHECK_EQ(p.voltage_mV, 0);                                          // no SVI formula
    p = decodePStateDef(mkPstate(0x88, 0x00, 0, 0, 0, true), false, true);
    CHECK_EQ(p.freqMHz, 0);                                             // DID 0 guard
    p = decodePStateDef(0x800000004C10A288ull, false, true);           // DID 0x22 (/4.25)
    CHECK_EQ(p.fid, 0x88); CHECK_EQ(p.did, 0x22); CHECK_EQ(p.freqMHz, 800);
}

TEST(pstate_decode_1ah)
{
    PStateDecoded p = decodePStateDef((1ull << 63) | (0x48ull << 14) | 0x2D0, true, false);
    CHECK(p.enabled);
    CHECK_EQ(p.fid, 0x2D0);
    CHECK_EQ(p.freqMHz, 3600);                                          // 720 * 5
    CHECK_EQ(p.did, 0);
    CHECK_EQ(p.vid, 0x48);
    CHECK_EQ(p.voltage_mV, 0);
    p = decodePStateDef((1ull << 63) | 0x0F, true, false);
    CHECK_EQ(p.freqMHz, 0);                                             // FID <= 0x0F invalid
    p = decodePStateDef((1ull << 63) | 0x10, true, false);
    CHECK_EQ(p.freqMHz, 80);
    p = decodePStateDef((1ull << 63) | 0x3FFF, true, false);           // bits 13:12 ignored
    CHECK_EQ(p.fid, 0xFFF);
}

TEST(pstate_encode)
{
    uint64_t raw = mkPstate(0x88, 0x08, 0x48, 0x5A, 1, true) | (0x1234ull << 32);
    uint64_t out = 0;
    CHECK(encodePStateDef(raw, 0x90, 0x08, 0x50, &out));
    PStateDecoded p = decodePStateDef(out, false, true);
    CHECK_EQ(p.freqMHz, 3600); CHECK_EQ(p.vid, 0x50);
    CHECK_EQ(p.iddValue, 0x5A); CHECK_EQ(p.iddDiv, 1); CHECK(p.enabled);
    CHECK_U64(out >> 32, (raw >> 32));                                  // upper bits preserved
    CHECK(!encodePStateDef(raw, 0x100, 0x08, 0x50, &out));
    CHECK(!encodePStateDef(raw, 0x88, 0x07, 0x50, &out));
    CHECK(!encodePStateDef(raw, 0x88, 0x31, 0x50, &out));
    CHECK(!encodePStateDef(raw, 0x88, 0x2E, 0x50, &out));               // > 0x2C reserved
    CHECK(!encodePStateDef(raw, 0x88, 0x1B, 0x50, &out));               // odd > 0x1A reserved
    CHECK(encodePStateDef(raw, 0x88, 0x1A, 0x50, &out));
    CHECK(encodePStateDef(raw, 0x88, 0x2C, 0x50, &out));
    CHECK(!encodePStateDef(raw, 0x88, 0x08, 0x100, &out));
    CHECK(!encodePStateDef(raw, 0x88, 0x08, 0xF8, &out));               // VID off refused
    CHECK(!encodePStateDef(raw, 0x05, 0x08, 0x50, &out));               // FID reserved
    CHECK(!encodePStateDef(raw, 0x88, 0x30, 0x50, &out));             // reserved
    CHECK(!encodePStateDef(raw, 0x88, 0x08, 0x50, nullptr));
    CHECK_EQ(pstateMaxVal(0x20), 2);
    CHECK_EQ(pstateCurLimit(0x21), 1);
}

// ---------------------------------------------------------------- CPPC
TEST(cppc)
{
    CppcCaps c = decodeCppcCap1(0x00000012A6781E14ull | (0x7ull << 32));
    CHECK_EQ(c.lowest, 0x14); CHECK_EQ(c.lowestNonlinear, 0x1E);
    CHECK_EQ(c.nominal, 0x78); CHECK_EQ(c.highest, 0xA6);
    uint64_t req = encodeCppcReq(0xDEADBEEF00000000ull | 0x12345678ull, 0xA6, 0x14, 0x50, 0x80);
    CHECK_U64(req, 0xDEADBEEF805014A6ull);
    CHECK_U64(encodeCppcReq(0, 0x1FF, 0, 0, 0x1FF), 0xFF0000FFull);   // fields masked to 8 bits
    CHECK_EQ(cppcPerfToFreq(120, 120, 3400), 3400);
    CHECK_EQ(cppcPerfToFreq(166, 120, 3400), 4704);                     // ceil(4703.33)
    CHECK_EQ(cppcPerfToFreq(1, 0, 3400), 0);
    CHECK_EQ(cppcFreqToPerf(3400, 120, 3400, 20, 166), 120);
    CHECK_EQ(cppcFreqToPerf(3401, 120, 3400, 20, 166), 121);            // ceil
    CHECK_EQ(cppcFreqToPerf(100, 120, 3400, 20, 166), 20);              // clamp low
    CHECK_EQ(cppcFreqToPerf(9000, 120, 3400, 20, 166), 166);            // clamp high
    uint32_t same[4] = {166, 166, 166, 166};
    uint32_t pref[4] = {231, 236, 226, 221};
    uint32_t flat[3] = {200, 200, 200};
    bool pc = true;
    CHECK_EQ(cppcEffectiveHighest(flat, 3, &pc), 200); CHECK(!pc);
    CHECK_EQ(cppcEffectiveHighest(same, 4, &pc), 166); CHECK(!pc);
    CHECK_EQ(cppcEffectiveHighest(pref, 4, &pc), 166); CHECK(pc);
    CHECK_EQ(cppcEffectiveHighest(pref, 1, &pc), 231); CHECK(!pc);
    CHECK_EQ(cppcEffectiveHighest(pref, 0, &pc), 0); CHECK(!pc);
    CHECK_EQ(cppcEffectiveHighest(pref, 4, nullptr), 166);
}

// ---------------------------------------------------------------- energy
TEST(energy)
{
    EnergyAccumulator a{};
    CHECK_EQ(energyUpdate(&a, 0xFFFFFF00u, false), 0);                  // priming
    CHECK(a.primed);
    CHECK_EQ(energyUpdate(&a, 0x10u, false), 0x110);                    // wrap
    CHECK_U64(a.totalRaw, 0x110);
    CHECK_EQ(energyUpdate(&a, 0x10u, false), 0);
    CHECK_EQ(energyUpdate(&a, 0x1010u, false), 0x1000);
    CHECK_U64(a.totalRaw, 0x1110);
    CHECK_EQ(energyUpdate(&a, 0x5u, true), 0);                          // rebase: no delta
    CHECK_U64(a.totalRaw, 0x1110);
    CHECK_EQ(energyUpdate(&a, 0x6u, false), 1);
    CHECK_EQ(energyUpdate(&a, 0x5u, false), 0xFFFFFFFFu);               // full wrap minus one

    CHECK_U64(energyRawToMicroJoule(65536, 16), 1000000);
    CHECK_U64(energyRawToMicroJoule(1, 16), 15);                        // 15.2588 floors
    CHECK_U64(energyRawToMicroJoule(0x110, 16), 4150);
    CHECK_U64(energyRawToMicroJoule(1ull << 40, 16), 16777216000000ull);
    CHECK_U64(energyRawToMicroJoule((1ull << 44) - 1, 16), 268435455999984ull);
    CHECK_U64(energyRawToMicroJoule(0xFFFFFFFFFFFFull, 16), 4294967295999984ull);
    CHECK_U64(energyRawToMicroJoule(1000, 0), 1000000000ull);
    CHECK_EQ(powerMilliWatt(1000000, 1000000000ull), 1000);             // 1 J / 1 s
    CHECK_EQ(powerMilliWatt(65000000, 1000000000ull), 65000);
    CHECK_EQ(powerMilliWatt(1234, 0), 0);
    CHECK_EQ(powerMilliWatt(~0ull, 1), 0xFFFFFFFFu);                    // saturates
    CHECK_EQ(powerMilliWatt(30000000000000ull, 1000000000ull), 0xFFFFFFFFu);
}

// ---------------------------------------------------------------- APERF/MPERF
TEST(derive_perf)
{
    const uint64_t tsc = 3400000000ull;
    PerfCounters a{1000, 2000, 3000, 4000};
    PerfCounters b{1000 + 25500000, 2000 + 17000000, 3000 + 34000000, 4000 + 51000000};
    PerfDerived d = derivePerf(a, b, tsc, 3400, true);
    CHECK_EQ(d.busyPermille, 500);
    CHECK_EQ(d.effFreqMHz, 5100);
    CHECK_EQ(d.util1024, 768);
    CHECK_EQ(d.ipcX100, 200);
    d = derivePerf(a, b, tsc, 3400, false);
    CHECK_EQ(d.ipcX100, 0);

    // fully busy, boosted beyond reference -> caps
    PerfCounters c{0, 0, 0, 0};
    PerfCounters e{45000000, 34000000, 34000000, 0};
    d = derivePerf(c, e, tsc, 3400, false);
    CHECK_EQ(d.busyPermille, 1000);
    CHECK_EQ(d.effFreqMHz, 4500);
    CHECK_EQ(d.util1024, 1024);

    // wrap-safe 64-bit deltas
    PerfCounters w0{~0ull - 99, ~0ull - 49, ~0ull - 199, 0};
    PerfCounters w1{900, 950, 1800, 0};
    d = derivePerf(w0, w1, tsc, 3400, false);
    CHECK_EQ(d.busyPermille, 500);                                      // 1000 / 2000
    CHECK_EQ(d.effFreqMHz, 3400);                                       // dA == dM
    CHECK_EQ(d.util1024, 512);

    // idle (dM = 0), no time (dT = 0)
    d = derivePerf(a, a, tsc, 3400, true);
    CHECK_EQ(d.busyPermille, 0); CHECK_EQ(d.effFreqMHz, 0);
    CHECK_EQ(d.util1024, 0); CHECK_EQ(d.ipcX100, 0);

    // MPERF > TSC (bogus) caps at 1000
    PerfCounters f{0, 40, 30, 0};
    d = derivePerf(c, f, tsc, 3400, false);
    CHECK_EQ(d.busyPermille, 1000);

    // Large intervals: tscHz * dA would overflow 64-bit without 128-bit math
    PerfCounters g{0, 0, 0, 0};
    PerfCounters h{340000000000ull, 340000000000ull, 680000000000ull, 0};   // 100/200 s
    d = derivePerf(g, h, tsc, 3400, false);
    CHECK_EQ(d.effFreqMHz, 3400);
    CHECK_EQ(d.busyPermille, 500);
    CHECK_EQ(d.util1024, 512);

    // Reference differs from TSC: util relative to 4000 MHz reference
    d = derivePerf(a, b, tsc, 4000, false);
    CHECK_EQ(d.util1024, 652);                                          // 2550/4000*1024 = 652.8
    // tscHz unknown -> util relative to TSC ticks, no eff freq
    d = derivePerf(a, b, 0, 3400, false);
    CHECK_EQ(d.effFreqMHz, 0);
    CHECK_EQ(d.util1024, 768);
}

// ---------------------------------------------------------------- governors
TEST(resolve_and_closest)
{
    PStateTable t = table5700X();
    CHECK_EQ(resolvePState(t, 0), 2);
    CHECK_EQ(resolvePState(t, 2200), 2);
    CHECK_EQ(resolvePState(t, 2201), 1);
    CHECK_EQ(resolvePState(t, 3400), 0);
    CHECK_EQ(resolvePState(t, 9999), 0);                                // none >= -> fastest
    t.fastest = 1;
    CHECK_EQ(resolvePState(t, 9999), 1);
    t.fastest = 0; t.slowest = 1;
    CHECK_EQ(resolvePState(t, 0), 1);
    t = table5700X();
    CHECK_EQ(closestPState(t, 2500), 1);                                // tie 300/300 -> faster
    CHECK_EQ(closestPState(t, 2499), 2);
    CHECK_EQ(closestPState(t, 3100), 0);                                // tie -> faster
    CHECK_EQ(closestPState(t, 100), 2);
    t.slowest = 7;                                                      // beyond count -> clamp
    CHECK_EQ(resolvePState(t, 0), 2);
    t.fastest = 5;                                                      // fastest > slowest -> clamp
    CHECK_EQ(resolvePState(t, 0), 2);
    PStateTable empty{};
    CHECK_EQ(resolvePState(empty, 1000), 0);
    CHECK_EQ(closestPState(empty, 1000), 0);
}

TEST(gov_static)
{
    PStateTable t = table5700X();
    GovTunables g = tunables();
    GovState s{};
    CHECK_EQ(governLegacy(kAMDZenGovPerformance, g, t, &s, 0, 0, 0), 0);
    CHECK_EQ(governLegacy(kAMDZenGovPowersave, g, t, &s, 1024, 1000, 0), 2);
    CHECK_EQ(s.curIdx, 2);
    CHECK_EQ(governLegacy(kAMDZenGovUserspace, g, t, &s, 0, 0, 1), 1);
    CHECK_EQ(governLegacy(kAMDZenGovUserspace, g, t, &s, 0, 0, 7), 2);  // clamp
    CHECK_EQ(governLegacy(kAMDZenGovNone, g, t, &s, 1024, 1000, 0), 2);  // keep
    CHECK_EQ(governLegacy(kAMDZenGovCppcActive, g, t, &s, 0, 0, 0), 2);
    t.fastest = 1;
    CHECK_EQ(governLegacy(kAMDZenGovPerformance, g, t, &s, 0, 0, 0), 1);
    CHECK_EQ(governLegacy(kAMDZenGovUserspace, g, t, &s, 0, 0, 0), 1);
    t.fastest = 0; t.slowest = 1;
    CHECK_EQ(governLegacy(kAMDZenGovPowersave, g, t, &s, 0, 0, 0), 1);
    s.curIdx = 2;                                                       // stale index outside range
    CHECK_EQ(governLegacy(kAMDZenGovNone, g, t, &s, 0, 0, 0), 1);
    CHECK_EQ(governLegacy(99, g, t, &s, 0, 0, 0), 1);                   // unknown -> keep
    GovState fresh{};
    CHECK_EQ(governLegacy(kAMDZenGovNone, g, table5700X(), &fresh, 0, 0, 0), 0);
    CHECK(fresh.init);
}

TEST(gov_schedutil)
{
    PStateTable t = table5700X();
    GovTunables g = tunables(0, 1);
    GovState s{};
    CHECK_EQ(governLegacy(kAMDZenGovSchedutil, g, t, &s, 1024, 0, 0), 0);  // 4250 -> fastest
    CHECK_EQ(governLegacy(kAMDZenGovSchedutil, g, t, &s, 512, 0, 0), 2);   // 2125 -> 2200
    CHECK_EQ(governLegacy(kAMDZenGovSchedutil, g, t, &s, 600, 0, 0), 1);   // 2490 -> 2800
    CHECK_EQ(governLegacy(kAMDZenGovSchedutil, g, t, &s, 820, 0, 0), 0);   // 3405 -> 3400
    CHECK_EQ(governLegacy(kAMDZenGovSchedutil, g, t, &s, 819, 0, 0), 0);   // 3399 -> 3400
    CHECK_EQ(governLegacy(kAMDZenGovSchedutil, g, t, &s, 674, 0, 0), 1);   // 2797 -> 2800
    CHECK_EQ(governLegacy(kAMDZenGovSchedutil, g, t, &s, 675, 0, 0), 0);   // 2801 -> 3400
    CHECK_EQ(governLegacy(kAMDZenGovSchedutil, g, t, &s, 0, 0, 0), 2);
    // policy clamp
    t.slowest = 1;
    CHECK_EQ(governLegacy(kAMDZenGovSchedutil, g, t, &s, 0, 0, 0), 1);
    t.slowest = 2; t.fastest = 1;
    CHECK_EQ(governLegacy(kAMDZenGovSchedutil, g, t, &s, 1024, 0, 0), 1);

    // headroom 100%: util 1024 maps to exactly P0
    t = table5700X();
    g.utilHeadroomPct = 100;
    GovState h{};
    CHECK_EQ(governLegacy(kAMDZenGovSchedutil, g, t, &h, 700, 0, 0), 1);   // 2324 -> 2800
}

TEST(gov_schedutil_hold)
{
    PStateTable t = table5700X();
    GovTunables g = tunables(0, 3);
    GovState s{};
    CHECK_EQ(governLegacy(kAMDZenGovSchedutil, g, t, &s, 1024, 0, 0), 0);
    CHECK_EQ(governLegacy(kAMDZenGovSchedutil, g, t, &s, 0, 0, 0), 0);    // hold 1
    CHECK_EQ(governLegacy(kAMDZenGovSchedutil, g, t, &s, 0, 0, 0), 0);    // hold 2
    CHECK_EQ(governLegacy(kAMDZenGovSchedutil, g, t, &s, 0, 0, 0), 2);    // 3rd tick drops
    CHECK_EQ(governLegacy(kAMDZenGovSchedutil, g, t, &s, 600, 0, 0), 1);  // increase is immediate
    CHECK_EQ(governLegacy(kAMDZenGovSchedutil, g, t, &s, 1024, 0, 0), 0);
    CHECK_EQ(governLegacy(kAMDZenGovSchedutil, g, t, &s, 0, 0, 0), 0);
    CHECK_EQ(governLegacy(kAMDZenGovSchedutil, g, t, &s, 1024, 0, 0), 0); // re-arms hold
    CHECK_EQ(governLegacy(kAMDZenGovSchedutil, g, t, &s, 0, 0, 0), 0);
    CHECK_EQ(governLegacy(kAMDZenGovSchedutil, g, t, &s, 0, 0, 0), 0);
    CHECK_EQ(governLegacy(kAMDZenGovSchedutil, g, t, &s, 0, 0, 0), 2);
}

TEST(gov_schedutil_ewma)
{
    PStateTable t = table5700X();
    GovTunables g = tunables(1, 1);
    GovState s{};
    CHECK_EQ(governLegacy(kAMDZenGovSchedutil, g, t, &s, 1024, 0, 0), 0);  // seeds ewma = 1024
    CHECK_EQ(s.utilEwma, 1024);
    CHECK_EQ(governLegacy(kAMDZenGovSchedutil, g, t, &s, 0, 0, 0), 2);     // ewma 512 -> 2125 -> P2
    CHECK_EQ(s.utilEwma, 512);
    // the rounded-up EWMA step converges exactly
    for (int i = 0; i < 20; ++i)
        governLegacy(kAMDZenGovSchedutil, g, t, &s, 0, 0, 0);
    CHECK_EQ(s.utilEwma, 0);
    CHECK_EQ(s.curIdx, 2);
    for (int i = 0; i < 20; ++i)
        governLegacy(kAMDZenGovSchedutil, g, t, &s, 1000, 0, 0);
    CHECK_EQ(s.utilEwma, 1000);
    CHECK_EQ(s.curIdx, 0);
}

TEST(gov_ondemand)
{
    PStateTable t = table5700X();
    GovTunables g = tunables(0, 1);
    GovState s{};
    CHECK_EQ(governLegacy(kAMDZenGovOndemand, g, t, &s, 0, 900, 0), 0);    // > 80% -> fastest
    CHECK_EQ(governLegacy(kAMDZenGovOndemand, g, t, &s, 0, 800, 0), 0);    // 80% -> 3160 -> closest 3400
    CHECK_EQ(governLegacy(kAMDZenGovOndemand, g, t, &s, 0, 0, 0), 2);      // fmin
    CHECK_EQ(governLegacy(kAMDZenGovOndemand, g, t, &s, 0, 500, 0), 1);    // 2800
    CHECK_EQ(governLegacy(kAMDZenGovOndemand, g, t, &s, 0, 250, 0), 1);    // 2500: tie -> faster
    CHECK_EQ(governLegacy(kAMDZenGovOndemand, g, t, &s, 0, 240, 0), 2);    // 2488 -> 2200
    t.slowest = 1;
    CHECK_EQ(governLegacy(kAMDZenGovOndemand, g, t, &s, 0, 0, 0), 1);      // clamp
    t = table5700X(); t.fastest = 1;
    CHECK_EQ(governLegacy(kAMDZenGovOndemand, g, t, &s, 0, 1000, 0), 1);   // clamp fastest
    // sampling_down_factor 2 holds the max for one extra tick
    t = table5700X();
    g = tunables(0, 2);
    GovState h{};
    CHECK_EQ(governLegacy(kAMDZenGovOndemand, g, t, &h, 0, 950, 0), 0);
    CHECK_EQ(governLegacy(kAMDZenGovOndemand, g, t, &h, 0, 0, 0), 0);
    CHECK_EQ(governLegacy(kAMDZenGovOndemand, g, t, &h, 0, 0, 0), 2);
}

TEST(gov_conservative)
{
    PStateTable t = table5700X();
    GovTunables g = tunables(0, 1);                                     // step 5% of 3400 = 170
    GovState s{};
    s.init = true; s.curIdx = 2; s.reqFreqMHz = 2200;
    CHECK_EQ(governLegacy(kAMDZenGovConservative, g, t, &s, 0, 900, 0), 2);  // 2370
    CHECK_EQ(governLegacy(kAMDZenGovConservative, g, t, &s, 0, 900, 0), 2);  // 2540
    CHECK_EQ(governLegacy(kAMDZenGovConservative, g, t, &s, 0, 900, 0), 2);  // 2710
    CHECK_EQ(governLegacy(kAMDZenGovConservative, g, t, &s, 0, 900, 0), 1);  // 2880 -> 2800
    CHECK_EQ(s.reqFreqMHz, 2880);
    CHECK_EQ(governLegacy(kAMDZenGovConservative, g, t, &s, 0, 500, 0), 1);  // between thresholds
    CHECK_EQ(s.reqFreqMHz, 2880);
    CHECK_EQ(governLegacy(kAMDZenGovConservative, g, t, &s, 0, 100, 0), 1);  // 2710 -> L 2800
    CHECK_EQ(governLegacy(kAMDZenGovConservative, g, t, &s, 0, 100, 0), 1);  // 2540
    CHECK_EQ(governLegacy(kAMDZenGovConservative, g, t, &s, 0, 100, 0), 1);  // 2370
    CHECK_EQ(governLegacy(kAMDZenGovConservative, g, t, &s, 0, 100, 0), 2);  // 2200
    CHECK_EQ(governLegacy(kAMDZenGovConservative, g, t, &s, 0, 100, 0), 2);  // floor
    CHECK_EQ(s.reqFreqMHz, 2200);
    for (int i = 0; i < 10; ++i)
        governLegacy(kAMDZenGovConservative, g, t, &s, 0, 1000, 0);
    CHECK_EQ(s.curIdx, 0);
    CHECK_EQ(s.reqFreqMHz, 3400);                                       // capped at max

    // down only every samplingDownFactor ticks
    g = tunables(0, 2);
    GovState d{};
    d.init = true; d.curIdx = 0; d.reqFreqMHz = 3400;
    CHECK_EQ(governLegacy(kAMDZenGovConservative, g, t, &d, 0, 0, 0), 0);
    CHECK_EQ(d.reqFreqMHz, 3400);                                       // skipped
    CHECK_EQ(governLegacy(kAMDZenGovConservative, g, t, &d, 0, 0, 0), 0);
    CHECK_EQ(d.reqFreqMHz, 3230);                                       // stepped
    CHECK_EQ(governLegacy(kAMDZenGovConservative, g, t, &d, 0, 0, 0), 0);
    CHECK_EQ(d.reqFreqMHz, 3230);

    // policy clamp: limited to [1..2]; request reset into range
    t.fastest = 1;
    GovState c{};
    CHECK_EQ(governLegacy(kAMDZenGovConservative, g, t, &c, 0, 1000, 0), 1);
    CHECK(c.reqFreqMHz <= 2800);
    // freq_step 0 -> no change
    g.freqStepPct = 0;
    t = table5700X();
    GovState z{};
    z.init = true; z.curIdx = 1; z.reqFreqMHz = 2800;
    CHECK_EQ(governLegacy(kAMDZenGovConservative, g, t, &z, 0, 1000, 0), 1);
}

TEST(gov_cppc)
{
    GovTunables g = tunables(0, 1);
    GovState s{};
    CHECK_EQ(governCppcDesired(kAMDZenGovPerformance, g, 20, 200, &s, 0, 0, 0), 200);
    CHECK_EQ(governCppcDesired(kAMDZenGovPowersave, g, 20, 200, &s, 0, 0, 0), 20);
    CHECK_EQ(governCppcDesired(kAMDZenGovUserspace, g, 20, 200, &s, 0, 0, 120), 120);
    CHECK_EQ(governCppcDesired(kAMDZenGovUserspace, g, 20, 200, &s, 0, 0, 255), 200);
    CHECK_EQ(governCppcDesired(kAMDZenGovUserspace, g, 20, 200, &s, 0, 0, 1), 20);
    CHECK_EQ(governCppcDesired(kAMDZenGovNone, g, 20, 200, &s, 0, 0, 0), 20);  // keep
    CHECK_EQ(governCppcDesired(kAMDZenGovSchedutil, g, 20, 200, &s, 512, 0, 0), 125);
    CHECK_EQ(governCppcDesired(kAMDZenGovSchedutil, g, 20, 200, &s, 1024, 0, 0), 200);
    CHECK_EQ(governCppcDesired(kAMDZenGovSchedutil, g, 20, 200, &s, 0, 0, 0), 20);
    CHECK_EQ(governCppcDesired(kAMDZenGovSchedutil, g, 20, 200, &s, 1, 0, 0), 20);
    CHECK_EQ(governCppcDesired(kAMDZenGovOndemand, g, 20, 200, &s, 0, 900, 0), 200);
    CHECK_EQ(governCppcDesired(kAMDZenGovOndemand, g, 20, 200, &s, 0, 500, 0), 110);
    CHECK_EQ(governCppcDesired(kAMDZenGovOndemand, g, 20, 200, &s, 0, 0, 0), 20);
    CHECK_EQ(governCppcDesired(kAMDZenGovPerformance, g, 200, 20, &s, 0, 0, 0), 200); // swapped
    // hold
    g = tunables(0, 2);
    GovState h{};
    CHECK_EQ(governCppcDesired(kAMDZenGovSchedutil, g, 20, 200, &h, 1024, 0, 0), 200);
    CHECK_EQ(governCppcDesired(kAMDZenGovSchedutil, g, 20, 200, &h, 0, 0, 0), 200);
    CHECK_EQ(governCppcDesired(kAMDZenGovSchedutil, g, 20, 200, &h, 0, 0, 0), 20);
    // conservative: step 5% of 200 = 10
    g = tunables(0, 1);
    GovState c{};
    c.init = true; c.curIdx = 20; c.reqFreqMHz = 20;
    CHECK_EQ(governCppcDesired(kAMDZenGovConservative, g, 20, 200, &c, 0, 900, 0), 30);
    CHECK_EQ(governCppcDesired(kAMDZenGovConservative, g, 20, 200, &c, 0, 900, 0), 40);
    CHECK_EQ(governCppcDesired(kAMDZenGovConservative, g, 20, 200, &c, 0, 500, 0), 40);
    CHECK_EQ(governCppcDesired(kAMDZenGovConservative, g, 20, 200, &c, 0, 100, 0), 30);
    CHECK_EQ(governCppcDesired(kAMDZenGovConservative, g, 20, 200, &c, 0, 100, 0), 20);
    CHECK_EQ(governCppcDesired(kAMDZenGovConservative, g, 20, 200, &c, 0, 100, 0), 20);
    CHECK_EQ(governCppcDesired(kAMDZenGovConservative, g, 20, 35, &c, 0, 1000, 0), 21);  // step 5% of 35 = 1
    CHECK_EQ(governCppcDesired(kAMDZenGovConservative, g, 20, 35, &c, 0, 1000, 0), 22);
}

// ---------------------------------------------------------------- misc
TEST(strings_and_names)
{
    CHECK(containsStr("AMD Ryzen 7 5700X 8-Core Processor", "5700X"));
    CHECK(containsStr("abc", ""));
    CHECK(!containsStr("abc", "abcd"));
    CHECK(containsStr("aaab", "aab"));
    CHECK(!containsStr(nullptr, "a"));
    CHECK(!containsStr("a", nullptr));
    for (uint32_t gi = 0; gi < kAMDZenGovCount; ++gi)
        CHECK_EQ(governorFromName(governorName(gi)), gi);
    CHECK_EQ(governorFromName("schedutil"), kAMDZenGovSchedutil);
    CHECK_EQ(governorFromName("Performance"), kAMDZenGovPerformance);
    CHECK_EQ(governorFromName("active"), kAMDZenGovCppcActive);
    CHECK_EQ(governorFromName("none"), kAMDZenGovNone);
    CHECK_EQ(governorFromName("bogus"), kAMDZenGovCount);
    CHECK_EQ(governorFromName("schedutilx"), kAMDZenGovCount);
    CHECK_EQ(governorFromName(""), kAMDZenGovCount);
    CHECK_EQ(governorFromName(nullptr), kAMDZenGovCount);
    CHECK_STR(governorName(kAMDZenGovConservative), "conservative");
    CHECK_STR(governorName(kAMDZenGovCount), "unknown");
    GovTunables t;
    defaultTunables(&t);
    CHECK_EQ(t.upThreshold, 80); CHECK_EQ(t.downThreshold, 20);
    CHECK_EQ(t.samplingDownFactor, 1); CHECK_EQ(t.freqStepPct, 5);
    CHECK_EQ(t.utilHeadroomPct, 125); CHECK_EQ(t.utilEwmaShift, 1);
}

int main()
{
    for (int i = 0; i < g_ntests; ++i) {
        g_test = g_tests[i].name;
        int before = g_failures;
        g_tests[i].fn();
        std::printf("%-28s %s\n", g_tests[i].name, g_failures == before ? "ok" : "FAILED");
    }
    std::printf("\n%d tests, %d checks, %d failures\n", g_ntests, g_checks, g_failures);
    return g_failures ? 1 : 0;
}
