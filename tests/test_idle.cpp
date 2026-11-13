// Host unit tests for ZenIdleModel.cpp (CC6 idle hook decision logic). Build/run: make -C tests
#include <cstdio>
#include <cstdint>
#include <cstring>
#include "ZenIdleModel.hpp"
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
// Generic Register descriptor as AML ResourceTemplate { Register(...) } emits it.
static size_t mkGas(uint8_t *b, uint8_t space, uint8_t width, uint8_t off, uint8_t acc, uint64_t addr)
{
    b[0] = 0x82; b[1] = 0x0C; b[2] = 0x00;
    b[3] = space; b[4] = width; b[5] = off; b[6] = acc;
    for (int i = 0; i < 8; i++) b[7 + i] = (uint8_t)(addr >> (8 * i));
    b[15] = 0x79; b[16] = 0x00;          // end tag
    return 17;
}

static CstEntry ioEntry(uint32_t type, uint64_t port, uint32_t lat)
{
    uint8_t b[17];
    size_t n = mkGas(b, kGasSystemIO, 8, 0, 0, port);
    CstEntry e{};
    bool ok = makeCstEntry(b, n, type, lat, 0, &e);
    (void)ok;
    return e;
}

static CstEntry ffhEntry(uint32_t type, uint64_t hint, uint32_t lat)
{
    uint8_t b[17];
    size_t n = mkGas(b, kGasFfh, 1, 2, 1, hint);
    CstEntry e{};
    bool ok = makeCstEntry(b, n, type, lat, 0, &e);
    (void)ok;
    return e;
}

static const uint64_t kHwcrBoot = HWCR_INVDWBINVD | (1ull << 24) | (1ull << 30); // INVDWBINVD, TscFreqSel, IRPerfEn

// n identical CPUs
static IdleCaps capsFor(uint64_t base, uint64_t cfg, uint64_t hwcr = kHwcrBoot, uint32_t n = 16)
{
    IdleMsrSample s[16];
    std::memset(s, 0, sizeof(s));
    for (uint32_t i = 0; i < n; i++) {
        s[i].ok = 1; s[i].hwcrOk = 1; s[i].policyOk = 1;
        s[i].base = base; s[i].cfg = cfg; s[i].hwcr = hwcr; s[i].policy = 0x1234;
    }
    return aggregateIdleMsrs(s, n);
}

// ---------------------------------------------------------------- ACPI descriptor
TEST(gas_parse)
{
    uint8_t b[32];
    size_t n = mkGas(b, kGasSystemIO, 8, 0, 1, 0x414);
    AcpiGas g{};
    CHECK(parseGenericRegister(b, n, &g));
    CHECK_EQ(g.spaceId, kGasSystemIO);
    CHECK_EQ(g.bitWidth, 8);
    CHECK_EQ(g.accessSize, 1);
    CHECK_U64(g.address, 0x414);

    n = mkGas(b, kGasFfh, 1, 2, 1, 0x0000000000000000ull);
    CHECK(parseGenericRegister(b, n, &g));
    CHECK_EQ(g.spaceId, kGasFfh);
    CHECK_EQ(g.bitOffset, 2);

    // full 64-bit address, little endian
    n = mkGas(b, kGasSystemMemory, 32, 0, 3, 0x1122334455667788ull);
    CHECK(parseGenericRegister(b, n, &g));
    CHECK_U64(g.address, 0x1122334455667788ull);

    // without end tag (15 bytes) is still a complete descriptor
    CHECK(parseGenericRegister(b, 15, &g));
    // malformed
    CHECK(!parseGenericRegister(b, 14, &g));          // truncated
    CHECK(!parseGenericRegister(nullptr, 17, &g));
    CHECK(!parseGenericRegister(b, 17, nullptr));
    b[0] = 0x86;                                       // other large resource
    CHECK(!parseGenericRegister(b, 17, &g));
    b[0] = 0x82; b[1] = 0x0B;                          // body too short
    CHECK(!parseGenericRegister(b, 17, &g));
    b[1] = 0x20;                                       // claims more than the buffer holds
    CHECK(!parseGenericRegister(b, 17, &g));
}

TEST(cst_entry)
{
    uint8_t b[17];
    size_t n = mkGas(b, kGasSystemIO, 8, 0, 0, 0x414);
    CstEntry e{};
    CHECK(makeCstEntry(b, n, 2, 18, 0, &e));
    CHECK_EQ(e.type, 2);
    CHECK_EQ(e.latencyUs, 18);
    CHECK_U64(e.reg.address, 0x414);
    CHECK(!makeCstEntry(b, n, 0, 18, 0, &e));          // invalid type
    CHECK(!makeCstEntry(b, n, 4, 18, 0, &e));
    CHECK(!makeCstEntry(b, 10, 2, 18, 0, &e));         // bad buffer
    CHECK(makeCstEntry(b, n, 3, 0x1FFFFFFFFull, 0, &e));
    CHECK_EQ(e.latencyUs, 0xFFFFFFFFu);                // saturated
}

// ---------------------------------------------------------------- MSR decode
TEST(cstate_cfg_decode)
{
    CStateCfg c = decodeCStateCfg(0x484848);           // BIOS C-states on (CoreFreq dump)
    CHECK(c.cc6en[0] && c.cc6en[1] && c.cc6en[2]);
    CHECK_EQ(c.cc1dfsid[0], 8); CHECK_EQ(c.cc1dfsid[1], 8); CHECK_EQ(c.cc1dfsid[2], 8);
    c = decodeCStateCfg(0x080808);                     // C-states off
    CHECK(!c.cc6en[0] && !c.cc6en[1] && !c.cc6en[2]);
    CHECK_EQ(c.cc1dfsid[2], 8);
    c = decodeCStateCfg(0x400000);
    CHECK(!c.cc6en[0] && !c.cc6en[1] && c.cc6en[2]);
    CHECK(cstateCfgCc6En(0x4000, 1));
    CHECK(!cstateCfgCc6En(0x4000, 2));
    CHECK(!cstateCfgCc6En(0x484848, 3));               // out of range
    CHECK_U64(kCStateCfgCc6Mask, 0x404040);
    // upper CFOH/CC1E fields do not leak into the CCR fields
    c = decodeCStateCfg(0xFFFFFFFF00000000ull | 0x48);
    CHECK(c.cc6en[0] && !c.cc6en[1]);
}

TEST(cstate_base)
{
    uint32_t b = 0;
    CHECK_EQ(checkCStateBase(0x413, &b), kCc6Ok); CHECK_EQ(b, 0x413);
    CHECK_EQ(checkCStateBase(0, &b), kCc6BaseZero);
    CHECK_EQ(checkCStateBase(0xFFF8, &b), kCc6Ok);
    CHECK_EQ(checkCStateBase(0xFFF9, &b), kCc6BaseInvalid);
    CHECK_EQ(checkCStateBase(0xABCD00000000ull, &b), kCc6BaseZero);   // reserved bits only
    CHECK_EQ(checkCStateBase(0x10410, &b), kCc6Ok); CHECK_EQ(b, 0x410);
}

TEST(aggregate_msrs)
{
    IdleCaps c = capsFor(0x413, 0x484848);
    CHECK(c.msrOk && c.baseUniform && c.cfgUniform && c.invdWbinvd && c.mwaitAllowed);
    CHECK_U64(c.baseRaw, 0x413);
    CHECK_U64(c.cfgCc6And, 0x404040);
    CHECK_U64(c.policy, 0x1234);
    CHECK_EQ(c.ncpu, 16);

    IdleMsrSample s[4];
    std::memset(s, 0, sizeof(s));
    for (int i = 0; i < 4; i++) { s[i].ok = 1; s[i].hwcrOk = 1; s[i].base = 0x413; s[i].cfg = 0x484848; s[i].hwcr = kHwcrBoot; }
    s[2].cfg = 0x480848;                                   // one core lacks CCR1 CC6EN
    c = aggregateIdleMsrs(s, 4);
    CHECK(c.msrOk && !c.cfgUniform);
    CHECK_U64(c.cfgCc6And, 0x400040);
    s[2].cfg = 0x484848;
    s[3].base = 0x410;                                     // base differs
    c = aggregateIdleMsrs(s, 4);
    CHECK(!c.baseUniform);
    s[3].base = 0x413;
    s[1].hwcr = kHwcrBoot & ~HWCR_INVDWBINVD;
    c = aggregateIdleMsrs(s, 4);
    CHECK(!c.invdWbinvd && c.mwaitAllowed);
    s[1].hwcr = kHwcrBoot | HWCR_MON_MWAIT_DIS;
    c = aggregateIdleMsrs(s, 4);
    CHECK(c.invdWbinvd && !c.mwaitAllowed);
    s[1].hwcr = kHwcrBoot;
    s[0].ok = 0;                                           // C-state MSR #GP on CPU 0
    c = aggregateIdleMsrs(s, 4);
    CHECK(!c.msrOk && !c.baseUniform && !c.invdWbinvd);
    CHECK(c.mwaitAllowed);                                 // HWCR still fine
    s[0].hwcrOk = 0;                                       // CPU 0 did not answer at all
    c = aggregateIdleMsrs(s, 4);
    CHECK(!c.mwaitAllowed);
    CHECK_U64(c.cfgCc6And, 0);
    c = aggregateIdleMsrs(nullptr, 0);
    CHECK(!c.msrOk);
    c = aggregateIdleMsrs(s, 0);
    CHECK(!c.msrOk);
}

// ---------------------------------------------------------------- port choice
TEST(port_from_cst_agesa)
{
    // Typical AM4: C1 FFH, C2 IO 0x414 (lat 18), CStateAddr 0x413 -> k = 1
    IdleCaps caps = capsFor(0x413, 0x484848);
    CstEntry cst[2] = { ffhEntry(1, 0, 1), ioEntry(2, 0x414, 18) };
    IdlePortChoice p = chooseIdlePort(caps, cst, 2, true);
    CHECK(p.ok);
    CHECK_EQ(p.port, 0x414);
    CHECK_EQ(p.ccr, 1);
    CHECK_EQ(p.source, kPortSourceCst);
    CHECK_EQ(p.cstIndex, 1);
    CHECK_EQ(p.reason, kCc6Ok);

    // With a C3 at 0x415 the deeper entry wins -> k = 2
    CstEntry cst3[3] = { ffhEntry(1, 0, 1), ioEntry(2, 0x414, 18), ioEntry(3, 0x415, 350) };
    p = chooseIdlePort(caps, cst3, 3, true);
    CHECK(p.ok); CHECK_EQ(p.port, 0x415); CHECK_EQ(p.ccr, 2); CHECK_EQ(p.cstIndex, 2);

    // Order in the package does not matter
    CstEntry rev[3] = { ioEntry(3, 0x415, 350), ioEntry(2, 0x414, 18), ffhEntry(1, 0, 1) };
    p = chooseIdlePort(caps, rev, 3, true);
    CHECK(p.ok); CHECK_EQ(p.ccr, 2); CHECK_EQ(p.cstIndex, 0);
}

TEST(port_from_cst_base410)
{
    // coreboot-style base 0x410: 0x414 is offset 4, clamped to CCR2; the hook reads base+2
    IdleCaps caps = capsFor(0x410, 0x484848);
    CstEntry cst[2] = { ffhEntry(1, 0, 1), ioEntry(2, 0x414, 18) };
    IdlePortChoice p = chooseIdlePort(caps, cst, 2, true);
    CHECK(p.ok); CHECK_EQ(p.ccr, 2); CHECK_EQ(p.port, 0x412);
    // offset 1 -> CCR1
    CstEntry c1[1] = { ioEntry(2, 0x411, 400) };
    p = chooseIdlePort(caps, c1, 1, true);
    CHECK(p.ok); CHECK_EQ(p.ccr, 1); CHECK_EQ(p.port, 0x411);
    // last port of the window still counts, first port after it does not
    CstEntry c7[1] = { ioEntry(2, 0x417, 18) };
    p = chooseIdlePort(caps, c7, 1, true);
    CHECK(p.ok); CHECK_EQ(p.ccr, 2);
    CstEntry c8[1] = { ioEntry(2, 0x418, 18) };
    p = chooseIdlePort(caps, c8, 1, true);
    CHECK(!p.ok); CHECK_EQ(p.reason, kCc6CstNoIoEntry); CHECK_EQ(p.port, 0);
}

TEST(port_from_cst_rejects)
{
    IdleCaps caps = capsFor(0x413, 0x484848);
    // IO entry outside the trap window only (e.g. legacy P_LVL2 when the base differs)
    CstEntry out[2] = { ffhEntry(1, 0, 1), ioEntry(2, 0x814, 18) };
    IdlePortChoice p = chooseIdlePort(caps, out, 2, true);
    CHECK(!p.ok); CHECK_EQ(p.reason, kCc6CstNoIoEntry);
    // _CST present but C1 only (firmware C-states off): no deep state advertised
    CstEntry c1[1] = { ffhEntry(1, 0, 1) };
    p = chooseIdlePort(caps, c1, 1, true);
    CHECK(!p.ok); CHECK_EQ(p.reason, kCc6CstNoIoEntry);
    // _CST present with zero parsable entries
    p = chooseIdlePort(caps, nullptr, 0, true);
    CHECK(!p.ok); CHECK_EQ(p.reason, kCc6CstNoIoEntry);
    // port == base -> CCR0 is never used
    CstEntry c0[1] = { ioEntry(2, 0x413, 18) };
    p = chooseIdlePort(caps, c0, 1, true);
    CHECK(!p.ok); CHECK_EQ(p.reason, kCc6CstCcr0);
    // below the window
    CstEntry lo[1] = { ioEntry(2, 0x412, 18) };
    p = chooseIdlePort(caps, lo, 1, true);
    CHECK(!p.ok); CHECK_EQ(p.reason, kCc6CstNoIoEntry);
    // chosen CCR without CC6EN
    IdleCaps noCcr1 = capsFor(0x413, 0x480848);
    CstEntry c2[1] = { ioEntry(2, 0x414, 18) };
    p = chooseIdlePort(noCcr1, c2, 1, true);
    CHECK(!p.ok); CHECK_EQ(p.reason, kCc6NoCc6En);
    CHECK_EQ(p.ccr, 1);
}

TEST(port_without_cst)
{
    IdlePortChoice p = chooseIdlePort(capsFor(0x413, 0x484848), nullptr, 0, false);
    CHECK(p.ok); CHECK_EQ(p.ccr, 1); CHECK_EQ(p.port, 0x414); CHECK_EQ(p.source, kPortSourceCfg);
    CHECK_EQ(p.cstIndex, -1);
    p = chooseIdlePort(capsFor(0x413, 0x400848), nullptr, 0, false);   // only CCR2
    CHECK(p.ok); CHECK_EQ(p.ccr, 2); CHECK_EQ(p.port, 0x415);
    p = chooseIdlePort(capsFor(0x413, 0x080848), nullptr, 0, false);   // only CCR0: never used
    CHECK(!p.ok); CHECK_EQ(p.reason, kCc6NoCc6En);
    p = chooseIdlePort(capsFor(0x413, 0x080808), nullptr, 0, false);   // C-states off
    CHECK(!p.ok); CHECK_EQ(p.reason, kCc6NoCc6En);
}

TEST(port_msr_gates)
{
    CstEntry cst[1] = { ioEntry(2, 0x414, 18) };
    IdlePortChoice p = chooseIdlePort(capsFor(0, 0x484848), cst, 1, true);
    CHECK(!p.ok); CHECK_EQ(p.reason, kCc6BaseZero);
    p = chooseIdlePort(capsFor(0xFFFC, 0x484848), cst, 1, true);
    CHECK(!p.ok); CHECK_EQ(p.reason, kCc6BaseInvalid);
    p = chooseIdlePort(capsFor(0x413, 0x484848, kHwcrBoot & ~HWCR_INVDWBINVD), cst, 1, true);
    CHECK(!p.ok); CHECK_EQ(p.reason, kCc6InvdWbinvdClear);
    IdleCaps bad = capsFor(0x413, 0x484848);
    bad.msrOk = false;
    p = chooseIdlePort(bad, cst, 1, true);
    CHECK(!p.ok); CHECK_EQ(p.reason, kCc6MsrUnreadable);
    bad = capsFor(0x413, 0x484848);
    bad.baseUniform = false;
    p = chooseIdlePort(bad, cst, 1, true);
    CHECK(!p.ok); CHECK_EQ(p.reason, kCc6BaseMismatch);
    // the hook never reads outside base+1..base+2
    for (uint32_t port = 0x400; port < 0x420; port++) {
        CstEntry e[1] = { ioEntry(2, port, 18) };
        p = chooseIdlePort(capsFor(0x413, 0x484848), e, 1, true);
        if (p.ok) CHECK(p.port == 0x414 || p.port == 0x415);
    }
}

// ---------------------------------------------------------------- modes and gates
TEST(mode_parse_clamp)
{
    uint32_t m = 99;
    CHECK(idleModeFromString("auto", &m)); CHECK_EQ(m, kIdleAuto);
    CHECK(idleModeFromString("0", &m)); CHECK_EQ(m, 0);
    CHECK(idleModeFromString("3", &m)); CHECK_EQ(m, 3);
    CHECK(!idleModeFromString("4", &m));
    CHECK(!idleModeFromString("33", &m));
    CHECK(!idleModeFromString("", &m));
    CHECK(!idleModeFromString("Auto", &m));
    CHECK(!idleModeFromString(nullptr, &m));
    CHECK_EQ(clampIdleMode(3, true, true), 3);
    CHECK_EQ(clampIdleMode(3, true, false), 2);
    CHECK_EQ(clampIdleMode(3, false, false), 1);
    CHECK_EQ(clampIdleMode(3, false, true), 3);     // CC6 deep with HLT as the shallow state
    CHECK_EQ(clampIdleMode(2, false, true), 1);
    CHECK_EQ(clampIdleMode(2, true, true), 2);
    CHECK_EQ(clampIdleMode(1, true, true), 1);
    CHECK_EQ(clampIdleMode(0, true, true), 0);
    CHECK_STR(idleModeName(kIdleCc6), "cc6");
    CHECK_STR(idleModeName(kIdleAuto), "auto");
    CHECK_STR(idleModeName(7), "unknown");
}

static IdleGateInput hostInput()
{
    // Ryzen 7 5700X on macOS 26 (Darwin 25)
    IdleGateInput in{};
    in.requested = kIdleAuto;
    in.darwinMajor = 25;
    in.arat = true;
    in.tscInvariant = true;
    in.mwait = true;
    in.cc6 = true;
    return in;
}

TEST(gates)
{
    IdleGateResult r = evaluateIdleGates(hostInput());
    CHECK(r.doRegister); CHECK_EQ(r.mode, kIdleCc6); CHECK_EQ(r.maxMode, kIdleCc6);
    CHECK_EQ(r.reason, kIdleRegistered);

    IdleGateInput in = hostInput();
    in.cc6 = false;                                     // auto without CC6: do not register
    r = evaluateIdleGates(in);
    CHECK(!r.doRegister); CHECK_EQ(r.reason, kIdleNoCc6); CHECK_EQ(r.maxMode, kIdleMwait);
    in.requested = kIdleCc6;                            // explicit 3 is clamped instead
    r = evaluateIdleGates(in);
    CHECK(r.doRegister); CHECK_EQ(r.mode, kIdleMwait);
    in.mwait = false;
    r = evaluateIdleGates(in);
    CHECK(r.doRegister); CHECK_EQ(r.mode, kIdleHlt); CHECK_EQ(r.maxMode, kIdleHlt);

    in = hostInput(); in.requested = kIdleOff;
    r = evaluateIdleGates(in);
    CHECK(!r.doRegister); CHECK_EQ(r.reason, kIdleDisabled); CHECK_EQ(r.mode, kIdleOff);
    in = hostInput(); in.requested = kIdleHlt;
    r = evaluateIdleGates(in);
    CHECK(r.doRegister); CHECK_EQ(r.mode, kIdleHlt);
    in = hostInput(); in.requested = kIdleMwait;
    r = evaluateIdleGates(in);
    CHECK(r.doRegister); CHECK_EQ(r.mode, kIdleMwait);

    struct { void (*mut)(IdleGateInput &); uint32_t reason; } cases[] = {
        { [](IdleGateInput &i) { i.darwinMajor = 19; }, kIdleDarwinVersion },
        { [](IdleGateInput &i) { i.darwinMajor = 26; }, kIdleDarwinVersion },
        { [](IdleGateInput &i) { i.hypervisor = true; }, kIdleHypervisor },
        { [](IdleGateInput &i) { i.blocked = true; }, kIdleBlocked },
        { [](IdleGateInput &i) { i.pmConflict = true; }, kIdlePmConflict },
        { [](IdleGateInput &i) { i.xcpmActive = true; }, kIdleXcpm },
        { [](IdleGateInput &i) { i.arat = false; }, kIdleNoArat },
        { [](IdleGateInput &i) { i.tscInvariant = false; }, kIdleNoInvariantTsc },
    };
    for (auto &c : cases) {
        static const uint32_t kReqs[] = { kIdleAuto, kIdleHlt, kIdleCc6 };
        for (uint32_t req : kReqs) {
            in = hostInput();
            in.requested = req;
            c.mut(in);
            r = evaluateIdleGates(in);
            CHECK(!r.doRegister);
            CHECK_EQ(r.reason, c.reason);
        }
    }
    in = hostInput(); in.darwinMajor = 20;
    CHECK(evaluateIdleGates(in).doRegister);
    CHECK_STR(idleHookReasonName(kIdleRegistered), "registered");
    CHECK_STR(idleHookReasonName(kIdlePmConflict), "pm-kext-present");
    CHECK_STR(cc6ReasonName(kCc6Ok), "ok");
    CHECK_STR(cc6ReasonName(kCc6ReasonCount), "unknown");
    for (uint32_t i = 0; i < kIdleHookReasonCount; i++) CHECK(std::strcmp(idleHookReasonName(i), "unknown") != 0);
    for (uint32_t i = 0; i < kCc6ReasonCount; i++) CHECK(std::strcmp(cc6ReasonName(i), "unknown") != 0);
}

// ---------------------------------------------------------------- predictor
TEST(predictor)
{
    const uint64_t tsc = 3400000000ull;                     // 5700X TSC
    // Fixture values below assume a 200 us threshold (independent of the default).
    const uint64_t thr = usToTscTicks(200, tsc);
    CHECK_U64(thr, 680000);
    const uint64_t clamp = idleClampTicks(thr);
    CHECK_U64(clamp, 2 * thr);

    uint64_t e = 0;
    CHECK(!idlePredictDeep(e, thr));                         // first idle is shallow
    // long idles (1 ms timer ticks): deep after a few
    int n = 0;
    while (!idlePredictDeep(e, thr) && n < 100) { e = idleEwmaUpdate(e, 3400000, clamp); n++; }
    CHECK_EQ(n, 3);
    CHECK(e <= clamp);
    // one very long idle cannot push past the clamp
    e = idleEwmaUpdate(e, 34000000000ull, clamp);
    CHECK(e <= clamp);
    // short idles (20 us) bring it back to shallow quickly
    n = 0;
    while (idlePredictDeep(e, thr) && n < 100) { e = idleEwmaUpdate(e, 68000, clamp); n++; }
    CHECK(n >= 2 && n <= 4);
    // a single long outlier among short idles does not flip to deep
    e = 68000;
    e = idleEwmaUpdate(e, 34000000ull, clamp);
    CHECK(!idlePredictDeep(e, thr));
    e = idleEwmaUpdate(e, 68000, clamp);
    CHECK(!idlePredictDeep(e, thr));
    // converges to a steady sample
    e = 0;
    for (int i = 0; i < 200; i++) e = idleEwmaUpdate(e, 500000, clamp);
    CHECK(e >= 499990 && e <= 500000);
    // threshold 0: always deep
    CHECK(idlePredictDeep(0, usToTscTicks(0, tsc)));
    // conversions
    CHECK_U64(usToTscTicks(1, 1000000000ull), 1000);
    CHECK_U64(usToTscTicks(1000000, 5000000000ull), 5000000000ull);
    CHECK_U64(usToTscTicks(5000000, 1000000ull), 1000000ull);  // us clamped to 1 s
    CHECK_U64(usToTscTicks(200, 0), 0);
}

TEST(residency_and_rates)
{
    const uint64_t iv = 3400000000ull;                      // 1 s of TSC
    CHECK_EQ(residencyPermille(0, 16, iv), 0);
    CHECK_EQ(residencyPermille(16 * iv, 16, iv), 1000);
    CHECK_EQ(residencyPermille(17 * iv, 16, iv), 1000);     // capped
    CHECK_EQ(residencyPermille(8 * iv, 16, iv), 500);
    CHECK_EQ(residencyPermille(iv, 0, iv), 0);
    CHECK_EQ(residencyPermille(iv, 16, 0), 0);
    // a day-long interval does not overflow
    const uint64_t day = 86400ull * 5000000000ull;
    CHECK_EQ(residencyPermille(day * 128, 256, day), 500);
    CHECK_EQ(residencyPermille(1, 2, UINT64_MAX), 0);       // nonsense interval
    CHECK_U64(ratePerSecond(1000, 1000000000ull), 1000);
    CHECK_U64(ratePerSecond(1000, 500000000ull), 2000);
    CHECK_U64(ratePerSecond(5, 0), 0);
    CHECK_U64(ratePerSecond(40000000000ull, 4000000000ull), 10000000000ull);
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
