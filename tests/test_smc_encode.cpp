// Host unit tests for AMDZenSMC's integer SMC encoders and key-set rules
// (AMDZenSMC/src/SmcEncode.hpp, SmcKeySet.hpp). Build/run: make -C tests test-smc
#include <cstdio>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include "SmcEncode.hpp"
#include "SmcKeySet.hpp"

using namespace smc;

// ---------------------------------------------------------------- tiny framework
static int g_checks = 0, g_failures = 0;
static const char *g_test = "";

#define CHECK(cond) do { ++g_checks; if (!(cond)) { ++g_failures; \
    std::printf("  FAIL %s:%d [%s] %s\n", __FILE__, __LINE__, g_test, #cond); } } while (0)
#define CHECK_EQ(a, b) do { ++g_checks; long long _va = (long long)(a), _vb = (long long)(b); \
    if (_va != _vb) { ++g_failures; std::printf("  FAIL %s:%d [%s] %s == %s (%lld vs %lld)\n", \
    __FILE__, __LINE__, g_test, #a, #b, _va, _vb); } } while (0)
#define CHECK_HEX(a, b) do { ++g_checks; unsigned long long _va = (a), _vb = (b); \
    if (_va != _vb) { ++g_failures; std::printf("  FAIL %s:%d [%s] %s == %s (0x%llx vs 0x%llx)\n", \
    __FILE__, __LINE__, g_test, #a, #b, _va, _vb); } } while (0)
#define CHECK_STR(a, b) do { ++g_checks; const char *_sa = (a), *_sb = (b); \
    if (std::strcmp(_sa, _sb) != 0) { ++g_failures; std::printf("  FAIL %s:%d [%s] %s == \"%s\" (got \"%s\")\n", \
    __FILE__, __LINE__, g_test, #a, _sb, _sa); } } while (0)

struct TestCase { const char *name; void (*fn)(); };
#define MAX_TESTS 64
static TestCase g_tests[MAX_TESTS];
static int g_ntests = 0;
struct Registrar { Registrar(const char *n, void (*f)()) { g_tests[g_ntests++] = {n, f}; } };
#define TEST(name) static void name(); static Registrar reg_##name(#name, name); static void name()

// ---------------------------------------------------------------- helpers
// The first n bytes of a cell, as the SMC client sees them.
static bool cellBytes(uint32_t cell, unsigned n, const uint8_t *want)
{
    uint8_t b[4];
    std::memcpy(b, &cell, 4);
    return std::memcmp(b, want, n) == 0;
}

// Correctly rounded float bits of milli/1000 from libc's decimal parser (host only).
static uint32_t refFltBits(long long milli)
{
    char buf[48];
    unsigned long long a = milli < 0 ? (unsigned long long)(-milli) : (unsigned long long)milli;
    std::snprintf(buf, sizeof(buf), "%s%llu.%03llu", milli < 0 ? "-" : "", a / 1000, a % 1000);
    float f = std::strtof(buf, nullptr);
    uint32_t bits;
    std::memcpy(&bits, &f, 4);
    return bits;
}

static AMDZenInfo info5700X()
{
    AMDZenInfo in{};
    in.abiVersion = AMDZEN_ABI_VERSION;
    in.structSize = sizeof(AMDZenInfo);
    in.family = 0x19; in.model = 0x21;
    in.features = AMDZEN_F_TCTL | AMDZEN_F_CCD_TEMP | AMDZEN_F_RAPL | AMDZEN_F_CORE_ENERGY |
                  AMDZEN_F_SVI2 | AMDZEN_F_HWPSTATUS | AMDZEN_F_SMN;
    in.logicalCpus = 16; in.physicalCores = 8; in.threadsPerCore = 2; in.numCcx = 1; in.nodes = 1;
    in.ccdTempMask = 0x1;
    return in;
}

static AMDZenSample sample5700X()
{
    AMDZenSample s{};
    s.abiVersion = AMDZEN_ABI_VERSION;
    s.structSize = sizeof(AMDZenSample);
    s.seq = 7;
    s.intervalUs = 1000000;
    s.tctl_mC = 45250; s.tdie_mC = 45250;
    s.tccd_mC[0] = 47500;
    s.pkgPower_mW = 34656; s.coresPower_mW = 21000;
    s.vcore_mV = 1293; s.vsoc_mV = 1100; s.icore_mA = 18750;
    s.numCores = 8;
    return s;
}

static void keyList(const KeyDef *k, uint32_t n, char *out)
{
    out[0] = '\0';
    for (uint32_t i = 0; i < n; i++) {
        char name[5];
        keyName(k[i].key, name);
        if (i) std::strcat(out, " ");
        std::strcat(out, name);
    }
}

static const KeyDef *findKey(const KeyDef *k, uint32_t n, const char *name)
{
    const uint32_t key = makeKey(name[0], name[1], name[2], name[3]);
    for (uint32_t i = 0; i < n; i++) if (k[i].key == key) return &k[i];
    return nullptr;
}

// ---------------------------------------------------------------- fixed point
TEST(sp78_patterns)
{
    // 45.25 °C = 0x2D40 (smc-keys.md §5 example)
    CHECK_HEX(spRaw(45250, kSp78Frac), 0x2D40);
    const uint8_t b2d40[] = {0x2D, 0x40};
    CHECK(cellBytes(encTempSp78(45250), 2, b2d40));
    CHECK_HEX(encTempSp78(45250) >> 16, 0);            // upper half of a 2-byte cell is zero
    CHECK_HEX(spRaw(0, kSp78Frac), 0);
    CHECK_HEX(spRaw(1000, kSp78Frac), 0x0100);
    CHECK_HEX(spRaw(1, kSp78Frac), 0);                 // 0.256 LSB rounds down
    CHECK_HEX(spRaw(2, kSp78Frac), 1);                 // 0.512 LSB rounds up
    CHECK_HEX(spRaw(1953, kSp78Frac), 500);            // 499.968 -> 500
    CHECK_HEX(spRaw(127996, kSp78Frac), 0x7FFF);
}

TEST(sp_negative_twos_complement)
{
    // Apple dumps (MacPro7,1): TCXR = C180 = -62.5, TC0T = FFF5 = -0.043
    CHECK_HEX((uint16_t)spRaw(-62500, kSp78Frac), 0xC180);
    CHECK_HEX((uint16_t)spRaw(-43, kSp78Frac), 0xFFF5);
    CHECK_HEX((uint16_t)spRaw(-127000, kSp78Frac), 0x8100);   // "sensor absent" pattern
    // symmetric rounding: -x encodes as -(x)
    CHECK_EQ(spRaw(-2, kSp78Frac), -spRaw(2, kSp78Frac));
    CHECK_EQ(spRaw(-1953, kSp78Frac), -500);
    const uint8_t bc180[] = {0xC1, 0x80};
    CHECK(cellBytes(cellSp(-62500, kSp78Frac), 2, bc180));
}

TEST(sp_clamping)
{
    CHECK_HEX((uint16_t)spRaw(128000, kSp78Frac), 0x7FFF);
    CHECK_HEX((uint16_t)spRaw(200000, kSp78Frac), 0x7FFF);
    CHECK_HEX((uint16_t)spRaw(-128000, kSp78Frac), 0x8000);
    CHECK_HEX((uint16_t)spRaw(-200000, kSp78Frac), 0x8000);
    CHECK_HEX((uint16_t)spRaw(INT64_MAX, kSp3cFrac), 0x7FFF);
    CHECK_HEX((uint16_t)spRaw(INT64_MIN, kSp3cFrac), 0x8000);
    // key-level temperature encoder never emits a negative (Stats decodes unsigned)
    CHECK_HEX(encTempSp78(-5000), 0);
    CHECK_HEX(encTempSp78(INT32_MIN), 0);
    const uint8_t b7fff[] = {0x7F, 0xFF};
    CHECK(cellBytes(encTempSp78(INT32_MAX), 2, b7fff));
}

TEST(sp96_power)
{
    CHECK_HEX(spRaw(1000, kSp96Frac), 0x0040);
    CHECK_HEX(spRaw(34656, kSp96Frac), 0x08AA);        // 34.656 W * 64 = 2217.98
    const uint8_t b08aa[] = {0x08, 0xAA};
    CHECK(cellBytes(encPowerSp96(34656), 2, b08aa));
    CHECK_HEX((uint16_t)spRaw(511984, kSp96Frac), 0x7FFF);
    CHECK_HEX((uint16_t)spRaw(600000, kSp96Frac), 0x7FFF);
    const uint8_t b7fff[] = {0x7F, 0xFF};
    CHECK(cellBytes(encPowerSp96(0xFFFFFFFFu), 2, b7fff));
}

TEST(sp3c_voltage)
{
    CHECK_HEX(spRaw(1000, kSp3cFrac), 0x1000);
    CHECK_HEX(spRaw(1793, kSp3cFrac), 0x1CB0);        // 1.793 V * 4096 = 7344.13
    CHECK_HEX(spRaw(1293, kSp3cFrac), 0x14B0);        // 5296.13 -> 5296
    const uint8_t b1cb0[] = {0x1C, 0xB0};
    CHECK(cellBytes(encVoltSp3c(1793), 2, b1cb0));
    CHECK_HEX((uint16_t)spRaw(8000, kSp3cFrac), 0x7FFF);
    CHECK_HEX((uint16_t)spRaw(-8000, kSp3cFrac), 0x8000);
    CHECK_HEX((uint16_t)spRaw(-1000, kSp3cFrac), 0xF000);
}

// ---------------------------------------------------------------- flt
TEST(flt_patterns)
{
    CHECK_HEX(fltBits(0), 0);
    CHECK_HEX(fltBits(1000), 0x3F800000);
    CHECK_HEX(fltBits(-1000), 0xBF800000);
    CHECK_HEX(fltBits(2000), 0x40000000);
    CHECK_HEX(fltBits(500), 0x3F000000);
    CHECK_HEX(fltBits(45250), 0x42350000);
    CHECK_HEX(fltBits(34656), 0x420A9FBE);
    CHECK_HEX(fltBits(1793), 0x3FE58106);
    CHECK_HEX(fltBits(1), 0x3A83126F);                 // 0.001, smallest non-zero input
    // little-endian byte order on the wire (MacPro7,1 dump style)
    const uint8_t b1793[] = {0x06, 0x81, 0xE5, 0x3F};
    CHECK(cellBytes(encFlt(1793), 4, b1793));
}

TEST(flt_clamp_and_extremes)
{
    CHECK_HEX(fltBits(INT32_MAX), refFltBits(INT32_MAX));
    CHECK_HEX(fltBits(INT32_MIN), refFltBits(INT32_MIN));
    CHECK_HEX(fltBits(INT64_MAX), fltBits(INT32_MAX));
    CHECK_HEX(fltBits(INT64_MIN), fltBits(INT32_MIN));
    CHECK_HEX(encFlt(0xFFFFFFFFu), fltBits(INT32_MAX)); // uint32 input saturates
    CHECK_HEX(fltBits(-1), refFltBits(-1));
}

TEST(flt_matches_ieee_rounding)
{
    // Edges around mantissa/exponent boundaries, then a deterministic pseudo-random sweep.
    static const long long edges[] = {
        1, 2, 3, 7, 999, 1001, 1023, 1024, 1025, 4095, 4096, 8191, 8388607, 8388608, 8388609,
        16777215, 16777216, 16777217, 33554431, 33554432, 1000000, 1048576, 2097151, 2097152,
        2097153, 99999999, 134217727, 134217728, 1073741823, 1073741824, 2147483646,
    };
    int bad = 0;
    for (long long e : edges)
        for (int sign = -1; sign <= 1; sign += 2)
            if (fltBits(sign * e) != refFltBits(sign * e)) {
                if (bad++ < 5) std::printf("  flt mismatch at %lld\n", sign * e);
            }
    uint64_t x = 0x9E3779B97F4A7C15ull;
    for (int i = 0; i < 300000; i++) {
        x ^= x << 13; x ^= x >> 7; x ^= x << 17;
        long long v = (long long)(int32_t)(uint32_t)x;
        if (i & 1) v >>= (x >> 40) & 31;               // spread over all magnitudes
        if (fltBits(v) != refFltBits(v)) {
            if (bad++ < 5) std::printf("  flt mismatch at %lld\n", v);
        }
    }
    CHECK_EQ(bad, 0);
}

// ---------------------------------------------------------------- keys
TEST(key_packing_and_order)
{
    const uint32_t k = makeKey('T', 'C', '0', 'D');
    uint8_t b[4];
    std::memcpy(b, &k, 4);
    CHECK(b[0] == 'T' && b[1] == 'C' && b[2] == '0' && b[3] == 'D');
    CHECK_HEX(keyOrder(k), 0x54433044);
    char name[5];
    keyName(k, name);
    CHECK_STR(name, "TC0D");
    // ASCII: digits < upper < lower
    CHECK(keyOrder(makeKey('T', 'C', '0', 'P')) < keyOrder(makeKey('T', 'C', '0', 'c')));
    CHECK(keyOrder(makeKey('T', 'C', '0', 'c')) < keyOrder(makeKey('T', 'C', '0', 'p')));
    CHECK(keyOrder(makeKey('T', 'C', '0', 'p')) < keyOrder(makeKey('T', 'C', '1', 'c')));
    CHECK(keyOrder(makeKey('T', 'C', '7', 'c')) < keyOrder(makeKey('T', 'C', 'X', 'c')));
}

TEST(keyset_5700x)
{
    AMDZenInfo in = info5700X();
    AMDZenSample s = sample5700X();
    KeyDef k[kMaxKeys];
    uint32_t n = buildKeySet(in, &s, k, kMaxKeys);
    char list[kMaxKeys * 5 + 1];
    keyList(k, n, list);
    CHECK_STR(list, "IC0C PC0C PCPC PCPR PCPT TC0D TC0P TC0c TC0p TC1c TC2c TC3c TC4c TC5c "
                    "TC6c TC7c TCXc VC0C VCSC");
    CHECK_EQ(n, 19);
    for (uint32_t i = 1; i < n; i++) CHECK(keyOrder(k[i - 1].key) < keyOrder(k[i].key));

    // types
    CHECK(findKey(k, n, "TC0D")->kind == Kind::Sp78);
    CHECK(findKey(k, n, "PCPT")->kind == Kind::Sp96);
    CHECK(findKey(k, n, "PC0C")->kind == Kind::Sp96);
    CHECK(findKey(k, n, "VC0C")->kind == Kind::Sp3c);
    CHECK(findKey(k, n, "VCSC")->kind == Kind::Flt);
    CHECK(findKey(k, n, "IC0C")->kind == Kind::Flt);
    CHECK_EQ(kindSize(Kind::Sp78), 2);
    CHECK_EQ(kindSize(Kind::Flt), 4);

    // values
    CHECK_HEX(encodeKey(*findKey(k, n, "TC0D"), in, s), encTempSp78(45250));
    CHECK_HEX(encodeKey(*findKey(k, n, "TC3c"), in, s), encTempSp78(47500));   // CCD 0
    CHECK_HEX(encodeKey(*findKey(k, n, "TCXc"), in, s), encTempSp78(47500));
    CHECK_HEX(encodeKey(*findKey(k, n, "PCPR"), in, s), encPowerSp96(34656));
    CHECK_HEX(encodeKey(*findKey(k, n, "PCPC"), in, s), encPowerSp96(21000));
    CHECK_HEX(encodeKey(*findKey(k, n, "VC0C"), in, s), encVoltSp3c(1293));
    CHECK_HEX(encodeKey(*findKey(k, n, "VCSC"), in, s), fltBits(1100));
    CHECK_HEX(encodeKey(*findKey(k, n, "IC0C"), in, s), fltBits(18750));
}

TEST(keyset_never_forbidden)
{
    AMDZenInfo in = info5700X();
    in.features = ~0ull;
    in.ccdTempMask = 0x1;
    AMDZenSample s = sample5700X();
    KeyDef k[kMaxKeys];
    uint32_t n = buildKeySet(in, &s, k, kMaxKeys);
    static const char *const forbidden[] = {
        "PSTR", "PC0R", "MSTc", "TC0E", "TC0F", "TC0J", "TC0T", "TC0H", "TC0G", "TCXR", "TCXr",
        "TCXC", "TC0C", "TCAD", "MPkC", "MC0C", "FRC0", "PCTR", "PCAM",
    };
    for (const char *f : forbidden) CHECK(findKey(k, n, f) == nullptr);
    for (uint32_t i = 0; i < n; i++) {
        char name[5];
        keyName(k[i].key, name);
        CHECK(name[0] != 'F' && name[0] != 'M');       // no fan / fake frequency / base keys
    }
}

TEST(keyset_without_sample)
{
    AMDZenInfo in = info5700X();
    KeyDef k[kMaxKeys];
    uint32_t n = buildKeySet(in, nullptr, k, kMaxKeys);
    CHECK(findKey(k, n, "IC0C") == nullptr);
    CHECK(findKey(k, n, "VCSC") == nullptr);
    CHECK(findKey(k, n, "VC0C") != nullptr);           // HWPSTATUS VID fallback
    CHECK_EQ(n, 17);

    in.features &= ~(uint64_t)AMDZEN_F_HWPSTATUS;
    n = buildKeySet(in, nullptr, k, kMaxKeys);
    CHECK(findKey(k, n, "VC0C") == nullptr);
    AMDZenSample s = sample5700X();
    s.vsoc_mV = 0; s.icore_mA = 0;                     // SVI2 decode known but planes read 0
    n = buildKeySet(in, &s, k, kMaxKeys);
    CHECK(findKey(k, n, "VC0C") != nullptr);
    CHECK(findKey(k, n, "VCSC") == nullptr);
    CHECK(findKey(k, n, "IC0C") == nullptr);
}

TEST(keyset_per_core_guard)
{
    AMDZenInfo in = info5700X();
    AMDZenSample s = sample5700X();
    KeyDef k[kMaxKeys];

    CHECK_EQ(singleCcdIndex(in), 0);
    in.ccdTempMask = 0x4;                              // the one CCD sensor sits at index 2
    CHECK_EQ(singleCcdIndex(in), 2);
    s.tccd_mC[2] = 51000;
    uint32_t n = buildKeySet(in, &s, k, kMaxKeys);
    CHECK_HEX(encodeKey(*findKey(k, n, "TC5c"), in, s), encTempSp78(51000));

    in.ccdTempMask = 0x3; in.physicalCores = 12; in.numCcx = 2;   // 5900X: two CCDs
    CHECK_EQ(singleCcdIndex(in), -1);
    s.tccd_mC[0] = 48000; s.tccd_mC[1] = 55000;
    n = buildKeySet(in, &s, k, kMaxKeys);
    CHECK(findKey(k, n, "TC0c") == nullptr);
    CHECK(findKey(k, n, "TCXc") != nullptr);
    CHECK_HEX(encodeKey(*findKey(k, n, "TCXc"), in, s), encTempSp78(55000));

    in.ccdTempMask = 0x1; in.physicalCores = 16; in.numCcx = 2;   // one sensor read, 16 cores
    CHECK_EQ(singleCcdIndex(in), -1);
    in.physicalCores = 8; in.numCcx = 8;                         // EPYC 72F3-like
    CHECK_EQ(singleCcdIndex(in), -1);
    in.numCcx = 2;                                               // Zen2 single CCD
    CHECK_EQ(singleCcdIndex(in), 0);
    in.features &= ~(uint64_t)AMDZEN_F_CCD_TEMP;
    CHECK_EQ(singleCcdIndex(in), -1);
    in.ccdTempMask = 0;
    n = buildKeySet(in, &s, k, kMaxKeys);
    CHECK(findKey(k, n, "TC0c") == nullptr);
    CHECK_HEX(encodeKey(*findKey(k, n, "TCXc"), in, s), encTempSp78(s.tctl_mC));   // Tctl fallback
}

TEST(keyset_missing_features)
{
    AMDZenInfo in = info5700X();
    KeyDef k[kMaxKeys];
    in.features = 0;
    CHECK_EQ(buildKeySet(in, nullptr, k, kMaxKeys), 0);
    in.features = AMDZEN_F_RAPL;                      // monitor-only with SMN blocked by a conflict
    uint32_t n = buildKeySet(in, nullptr, k, kMaxKeys);
    char list[kMaxKeys * 5 + 1];
    keyList(k, n, list);
    CHECK_STR(list, "PCPR PCPT");
    // never writes past max
    in = info5700X();
    n = buildKeySet(in, nullptr, k, 3);
    CHECK_EQ(n, 3);
}

TEST(encode_edge_values)
{
    AMDZenInfo in = info5700X();
    AMDZenSample s = sample5700X();
    KeyDef k[kMaxKeys];
    uint32_t n = buildKeySet(in, &s, k, kMaxKeys);
    s.tdie_mC = -3000;                                 // dispNegative parts
    CHECK_HEX(encodeKey(*findKey(k, n, "TC0D"), in, s), 0);
    s.vcore_mV = 0;                                   // SVI2 read failed: requested VID fallback
    s.core[0].vid_mV = 1100; s.core[5].vid_mV = 1350; s.core[9].vid_mV = 1500;   // core 9 > numCores
    CHECK_HEX(encodeKey(*findKey(k, n, "VC0C"), in, s), encVoltSp3c(1350));
    s.numCores = 1000;                                // corrupt count is bounded
    CHECK_HEX(encodeKey(*findKey(k, n, "VC0C"), in, s), encVoltSp3c(1500));
    s.pkgPower_mW = 0;
    CHECK_HEX(encodeKey(*findKey(k, n, "PCPT"), in, s), 0);
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
