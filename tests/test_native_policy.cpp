// From repository root (same host flags as tests/Makefile; no repository build dir):
// clang++ -std=c++17 -O1 -g -Wall -Wextra -Werror -Wno-unused-function -fsanitize=address,undefined -fno-sanitize-recover=all -IAMDZenPM/src tests/test_native_policy.cpp AMDZenPM/src/ZenNativePolicy.cpp -o /private/tmp/test_native_policy && /private/tmp/test_native_policy
#include "ZenNativePolicy.hpp"
#include <atomic>
#include <cstdio>
#include <thread>

using namespace zennative;
static unsigned checks, failures;
static const char *testName;
#define CHECK(expr) do { ++checks; if (!(expr)) { ++failures; \
    std::printf("FAIL %s:%d: %s\n", testName, __LINE__, #expr); } } while (0)
#define EQ(a, b) CHECK((a) == (b))

static zencppc::Caps standardCaps() { return {200, 150, 60, 20, 3400, 800}; }
static SystemSignals unknownSystem() { return {kUnknown, kThermalUnknown, kPowerUnknown, kPowerUnobserved, 255}; }
static CpuSnapshot sample(Urgency u) { return {u, 0, 0, true, 0, false}; }
static void legal(const zencppc::Request &r, const zencppc::Caps &c) {
    CHECK(c.lowest <= r.minPerf);
    CHECK(r.minPerf <= r.maxPerf);
    CHECK(r.maxPerf <= c.highest);
    CHECK(r.maxPerf >= c.lowest);
    EQ(r.desiredPerf, 0u);
    CHECK(r.epp <= 255);
}
struct Fixture {
    PolicyState state {};
    Topology top {};
    CpuSnapshot cpus[kMaxCpus] {};
    zencppc::Caps caps[kMaxCpus] {};
    TickResult out {};
    SystemSignals system = unknownSystem();
    Tunables tun = defaultTunables();
    explicit Fixture(uint32_t count = 2) {
        top.cpuCount = count;
        for (uint32_t c = 0; c < count; ++c) {
            top.domainOfCpu[c] = static_cast<uint16_t>(c / 2);
            caps[c] = standardCaps(); cpus[c] = sample(kUrgencyNormal);
        }
    }
    void run(uint64_t now) { CHECK(tick(state, top, cpus, system, caps, tun, now, out)); }
};

static void snapshot_classes_and_counters() {
    CpuPublication p {};
    CpuSnapshot s = readCpuSnapshot(p);
    CHECK(!s.observed); EQ(s.urgency, kUrgencyUnknown); EQ(s.rtEvents, 0u);
    const int classes[] = {0, 1, 2, 3, 4, -1, 5};
    unsigned rt = 0, fg = 0;
    for (int u : classes) {
        recordUrgency(p, u); s = readCpuSnapshot(p);
        if (u == 3) ++rt;
        if (u == 2 || u == 4 || u < 0 || u > 4) ++fg;
        CHECK(s.observed); EQ(s.rtEvents, rt); EQ(s.foregroundEvents, fg);
        EQ(s.urgency, u >= 0 && u <= 4 ? static_cast<Urgency>(u) : kUrgencyUnknown);
    }
    EQ(sizeof(CpuPublication), 64u); EQ(alignof(CpuPublication), 64u);
}
static void snapshot_wrap_and_cpu_isolation() {
    CpuPublication p[2] {};
    p[0].word = (static_cast<uint64_t>(kEventMask) << 36) |
                (static_cast<uint64_t>(kEventMask) << 8) | 8u | kUrgencyNormal;
    recordUrgency(p[0], kUrgencyRealtime);
    EQ(readCpuSnapshot(p[0]).rtEvents, 0u);
    EQ(readCpuSnapshot(p[0]).foregroundEvents, kEventMask);
    recordUrgency(p[0], kUrgencyNormal);
    EQ(readCpuSnapshot(p[0]).foregroundEvents, 0u);
    CHECK(!readCpuSnapshot(p[1]).observed);
    recordUrgency(p[1], kUrgencyBackground);
    EQ(readCpuSnapshot(p[0]).urgency, kUrgencyNormal);
    EQ(readCpuSnapshot(p[1]).urgency, kUrgencyBackground);
}
static void snapshot_disabled_recording_and_rearm() {
    CpuPublication p[2] {};
    recordUrgency(p[0], kUrgencyRealtime);
    recordUrgency(p[1], kUrgencyBackground);
    for (const auto &cpu : p) {
        const auto s = readCpuSnapshot(cpu, false);
        CHECK(!s.observed); EQ(s.urgency, kUrgencyUnknown);
        EQ(s.rtEvents, 0u); EQ(s.foregroundEvents, 0u);
    }
    // Permanent callbacks continue recording with snapshot visibility disabled.
    recordUrgency(p[0], kUrgencyBackground);
    recordUrgency(p[1], kUrgencyLowPriority);
    CHECK(!readCpuSnapshot(p[0], false).observed);
    const auto a = readCpuSnapshot(p[0], true), b = readCpuSnapshot(p[1], true);
    CHECK(a.observed); EQ(a.urgency, kUrgencyBackground); EQ(a.rtEvents, 1u);
    CHECK(b.observed); EQ(b.urgency, kUrgencyLowPriority); EQ(b.foregroundEvents, 1u);
}
static void snapshot_concurrent_coherence() {
    CpuPublication p {};
    std::atomic<bool> ready {false}, go {false};
    std::thread writer([&] {
        recordUrgency(p, kUrgencyRealtime);
        recordUrgency(p, kUrgencyNormal);
        ready.store(true, std::memory_order_release);
        while (!go.load(std::memory_order_acquire)) {}
        for (unsigned i = 0; i < 200000; ++i) {
            recordUrgency(p, kUrgencyRealtime); recordUrgency(p, kUrgencyNormal);
            recordUrgency(p, kUrgencyBackground); recordUrgency(p, kUrgencyIdle);
        }
    });
    while (!ready.load(std::memory_order_acquire)) {}
    go.store(true, std::memory_order_release);
    for (unsigned i = 0; i < 200000; ++i) {
        const CpuSnapshot s = readCpuSnapshot(p);
        CHECK(s.observed);
        CHECK(s.urgency <= kUrgencyRealtime);
        // A torn class/counter snapshot would violate this publication relation.
        CHECK(s.urgency == kUrgencyRealtime ? s.rtEvents == s.foregroundEvents + 1u :
                                            s.rtEvents == s.foregroundEvents);
    }
    writer.join();
    EQ(readCpuSnapshot(p).rtEvents, 200001u);
    EQ(readCpuSnapshot(p).foregroundEvents, 200001u);
}
static void balanced_and_background_classes() {
    const auto c = standardCaps(); const auto t = defaultTunables(); const auto sys = unknownSystem();
    for (Urgency u : {kUrgencyNormal, kUrgencyLowPriority, kUrgencyUnknown}) {
        const auto r = decide(sample(u), false, false, sys, c, t);
        EQ(r.epp, 128u); EQ(r.minPerf, 20u); EQ(r.maxPerf, 200u); legal(r, c);
    }
    for (Urgency u : {kUrgencyBackground, kUrgencyIdle}) {
        EQ(decide(sample(u), false, false, sys, c, t).epp, 192u);
        EQ(decide(sample(u), false, true, sys, c, t).epp, 128u);
    }
    EQ(decide(CpuSnapshot {}, false, false, sys, c, t).epp, 128u);
}
static void realtime_floor_and_epp() {
    const auto c = standardCaps(); auto t = defaultTunables(); const auto sys = unknownSystem();
    auto r = decide(sample(kUrgencyRealtime), false, false, sys, c, t);
    EQ(r.minPerf, 120u); EQ(r.epp, 32u); EQ(r.maxPerf, 200u); legal(r, c);
    r = decide(sample(kUrgencyBackground), true, false, sys, c, t);
    EQ(r.minPerf, 120u); EQ(r.epp, 32u);
    t.realtimeMinPct = 0;
    EQ(decide(sample(kUrgencyRealtime), false, false, sys, c, t).minPerf, 60u);
    t.realtimeMinPct = UINT32_MAX;
    EQ(decide(sample(kUrgencyRealtime), false, false, sys, c, t).minPerf, 150u);
}
static void low_power_overrides_rt() {
    const auto c = standardCaps(); const auto t = defaultTunables(); auto sys = unknownSystem();
    sys.lowPowerMode = kOn;
    for (Urgency u : {kUrgencyRealtime, kUrgencyNormal, kUrgencyBackground}) {
        const auto r = decide(sample(u), false, false, sys, c, t);
        EQ(r.epp, 224u); EQ(r.maxPerf, c.nominal); legal(r, c);
    }
    sys.lowPowerMode = kOff;
    EQ(decide(sample(kUrgencyRealtime), false, false, sys, c, t).epp, 32u);
}
static void battery_baseline() {
    const auto c = standardCaps(); const auto t = defaultTunables(); auto sys = unknownSystem();
    sys.powerSource = kPowerInternal;
    EQ(decide(sample(kUrgencyNormal), false, false, sys, c, t).epp, 160u);
    EQ(decide(sample(kUrgencyBackground), false, false, sys, c, t).epp, 192u);
    EQ(decide(sample(kUrgencyRealtime), false, false, sys, c, t).epp, 32u);
    sys.powerSource = kPowerExternal;
    EQ(decide(sample(kUrgencyNormal), false, false, sys, c, t).epp, 128u);
}
static void thermal_decode() {
    EQ(decodeThermalWarning(0), kThermalNominal);
    EQ(decodeThermalWarning(5), kThermalModerate);
    EQ(decodeThermalWarning(100), kThermalHeavy);
    EQ(decodeThermalWarning(10), kThermalCritical);
    EQ(decodeThermalWarning(110), kThermalEmergency);
    for (uint32_t v : {1u, 4u, 6u, 101u, 255u, UINT32_MAX}) EQ(decodeThermalWarning(v), kThermalUnknown);
}
static void thermal_progression_and_clamping() {
    const auto c = standardCaps(); const auto t = defaultTunables(); auto sys = unknownSystem();
    const ThermalLevel levels[] = {kThermalUnknown, kThermalNominal, kThermalModerate,
        kThermalHeavy, kThermalCritical, kThermalEmergency};
    const unsigned limits[] = {200, 200, 165, 130, 95, 60};
    for (unsigned i = 0; i < 6; ++i) {
        sys.thermal = levels[i]; const auto r = decide(sample(kUrgencyRealtime), true, false, sys, c, t);
        EQ(r.maxPerf, limits[i]); EQ(r.minPerf, limits[i] < 120 ? limits[i] : 120); legal(r, c);
    }
    sys.lowPowerMode = kOn; sys.thermal = kThermalModerate;
    EQ(decide(sample(kUrgencyRealtime), false, false, sys, c, t).maxPerf, 150u);
    sys.thermal = kThermalCritical;
    EQ(decide(sample(kUrgencyRealtime), false, false, sys, c, t).maxPerf, 95u);
}
static void tunable_limits_and_extreme_caps() {
    auto t = defaultTunables(); const auto c = standardCaps(); const auto sys = unknownSystem();
    t.baseEpp = 17; t.realtimeEpp = 32;
    EQ(decide(sample(kUrgencyNormal), false, false, sys, c, t).epp, 17u);
    EQ(decide(sample(kUrgencyRealtime), false, false, sys, c, t).epp, 17u);
    t.baseEpp = UINT32_MAX; t.backgroundEpp = 0;
    EQ(decide(sample(kUrgencyNormal), false, false, sys, c, t).epp, 255u);
    EQ(decide(sample(kUrgencyBackground), false, false, sys, c, t).epp, 255u);
    t = defaultTunables();
    const zencppc::Caps big {UINT32_MAX, UINT32_MAX - 1, 1, 1, 0, 0};
    const auto r = decide(sample(kUrgencyRealtime), false, false, sys, big, t);
    EQ(r.minPerf, static_cast<uint32_t>((static_cast<uint64_t>(UINT32_MAX) - 1) * 80 / 100));
    legal(r, big);
}
static void caps_validation() {
    const zencppc::Caps bad[] = {{}, {200,150,60,0,0,0}, {200,150,10,20,0,0},
        {200,50,60,20,0,0}, {100,150,60,20,0,0}};
    for (const auto &c : bad) {
        CHECK(!validCaps(c));
        CHECK(zencppc::sameRequest(decide(sample(kUrgencyRealtime), true, false,
            unknownSystem(), c, defaultTunables()), zencppc::Request {}));
    }
    CHECK(validCaps(zencppc::Caps {1,1,1,1,0,0}));
}
static void smt_nonadjacent_domains() {
    Fixture f(4);
    f.top.domainOfCpu[0] = f.top.domainOfCpu[2] = 7;
    f.top.domainOfCpu[1] = f.top.domainOfCpu[3] = 19;
    f.cpus[0] = sample(kUrgencyBackground); f.cpus[2] = sample(kUrgencyRealtime);
    f.cpus[1] = f.cpus[3] = sample(kUrgencyBackground);
    f.run(0); EQ(f.out.changedCount, 4u);
    EQ(f.out.requests[0].minPerf, 120u); EQ(f.out.requests[0].epp, 32u);
    EQ(f.out.requests[1].epp, 192u);
    CHECK(zencppc::sameRequest(f.out.requests[0], f.out.requests[2]));
    CHECK(zencppc::sameRequest(f.out.requests[1], f.out.requests[3]));
    for (unsigned c = 0; c < 4; ++c) { CHECK(cpuChanged(f.out, c)); CHECK(cpuValid(f.out, c)); }
}
static void smt_capability_intersection() {
    Fixture f;
    f.caps[1] = {180, 140, 70, 30, 0, 0};
    f.cpus[0] = sample(kUrgencyRealtime); f.cpus[1] = sample(kUrgencyBackground);
    f.run(0); EQ(f.out.requests[0].minPerf, 120u); EQ(f.out.requests[0].maxPerf, 180u);
    CHECK(zencppc::sameRequest(f.out.requests[0], f.out.requests[1]));
    legal(f.out.requests[0], f.caps[0]); legal(f.out.requests[1], f.caps[1]);
    f.system.thermal = kThermalEmergency; f.run(50);
    EQ(f.out.requests[0].maxPerf, 60u); EQ(f.out.requests[0].minPerf, 60u);
    for (unsigned c = 0; c < 2; ++c)
        CHECK(f.out.requests[c].maxPerf <= decide(f.cpus[c], true, false,
            f.system, f.caps[c], f.tun).maxPerf);
    legal(f.out.requests[0], f.caps[0]); legal(f.out.requests[1], f.caps[1]);
}
static void invalid_domain_caps_and_recovery() {
    Fixture f(4);
    f.caps[1] = {}; f.run(0);
    EQ(f.out.changedCount, 2u); CHECK(!cpuValid(f.out,0)); CHECK(!cpuValid(f.out,1));
    CHECK(cpuValid(f.out,2)); CHECK(cpuChanged(f.out,3));
    f.caps[1] = standardCaps(); f.run(1);
    EQ(f.out.changedCount, 0u); CHECK(!cpuValid(f.out,0));
    f.run(50); EQ(f.out.changedCount,2u); CHECK(cpuChanged(f.out,0)); CHECK(cpuChanged(f.out,1));
    f.caps[0] = {50,40,30,20,0,0}; f.caps[1] = {200,150,70,60,0,0};
    f.run(100); CHECK(!cpuValid(f.out,0)); CHECK(!cpuValid(f.out,1));
}
static void smt_safety_ceiling_below_nonlinear() {
    Fixture f;
    f.caps[0] = {200,80,60,20,0,0};
    f.caps[1] = {180,150,90,30,0,0};
    CHECK(validCaps(f.caps[0])); CHECK(validCaps(f.caps[1]));
    f.system.lowPowerMode = kOn;
    f.run(0); EQ(f.out.changedCount,2u);
    EQ(f.out.requests[0].maxPerf,80u); EQ(f.out.requests[0].minPerf,30u);
    CHECK(f.out.requests[1].maxPerf < f.caps[1].lowestNonlinear);
    CHECK(zencppc::sameRequest(f.out.requests[0],f.out.requests[1]));
    for (unsigned c=0;c<2;++c) legal(f.out.requests[c],f.caps[c]);
    // A looser ceiling still obeys the normal relaxation hold (100 ms default).
    f.caps[0].nominal = 100; f.run(50); EQ(f.out.changedCount,0u);
    f.run(150);
    EQ(f.out.changedCount,2u);
    for (unsigned c=0;c<2;++c) {
        legal(f.out.requests[c],f.caps[c]);
        CHECK(f.out.requests[c].maxPerf <= f.caps[c].nominal);
    }
}
static void smt_safety_ceiling_below_hardware_floor() {
    Fixture f;
    f.caps[0] = {50,40,30,20,0,0};
    f.caps[1] = {200,150,70,40,0,0};
    f.run(0); EQ(f.out.changedCount,2u);
    EQ(f.out.requests[0].minPerf,40u); EQ(f.out.requests[0].maxPerf,50u);
    for (unsigned c=0;c<2;++c) legal(f.out.requests[c],f.caps[c]);
    f.system.thermal = kThermalEmergency; f.run(50);
    EQ(f.out.changedCount,0u); CHECK(!cpuValid(f.out,0)); CHECK(!cpuValid(f.out,1));
    f.system.thermal = kThermalModerate; f.run(100);
    EQ(f.out.changedCount,2u); EQ(f.out.requests[0].maxPerf,45u);
    for (unsigned c=0;c<2;++c) legal(f.out.requests[c],f.caps[c]);
}
static void topology_validation() {
    Fixture f;
    f.top.cpuCount = 0; CHECK(!tick(f.state,f.top,f.cpus,f.system,f.caps,f.tun,0,f.out));
    f.top.cpuCount = kMaxCpus+1; CHECK(!tick(f.state,f.top,f.cpus,f.system,f.caps,f.tun,0,f.out));
    f.top.cpuCount = 2;
    CHECK(!tick(f.state,f.top,nullptr,f.system,f.caps,f.tun,0,f.out));
    CHECK(!tick(f.state,f.top,f.cpus,f.system,nullptr,f.tun,0,f.out));
    f.top.domainOfCpu[0] = kMaxCpus; CHECK(!tick(f.state,f.top,f.cpus,f.system,f.caps,f.tun,0,f.out));
    f.top.domainOfCpu[0] = 0; f.top.cpuCount = 3; f.top.domainOfCpu[2] = 0;
    CHECK(!tick(f.state,f.top,f.cpus,f.system,f.caps,f.tun,0,f.out));
    EQ(f.out.cpuCount,0u); EQ(f.out.changedCount,0u); CHECK(!f.state.hasTick);
}
static void realtime_burst_hold_and_relaxation() {
    Fixture f(1); CpuPublication p {};
    recordUrgency(p,kUrgencyBackground); f.cpus[0] = readCpuSnapshot(p); f.run(0);
    EQ(f.out.requests[0].epp,192u);
    recordUrgency(p,kUrgencyRealtime); f.cpus[0] = readCpuSnapshot(p); f.run(10);
    EQ(f.out.changedCount,0u);
    recordUrgency(p,kUrgencyNormal); recordUrgency(p,kUrgencyBackground);
    f.cpus[0] = readCpuSnapshot(p); f.run(20); EQ(f.out.changedCount,0u);
    f.run(50); EQ(f.out.changedCount,1u); EQ(f.out.requests[0].epp,32u);
    f.run(109); EQ(f.out.requests[0].epp,32u);
    f.run(120); EQ(f.out.changedCount,0u); EQ(f.out.requests[0].epp,32u); // both RT (110) and FG (120) expired
    f.run(219); EQ(f.out.changedCount,0u);
    f.run(220); EQ(f.out.changedCount,1u); EQ(f.out.requests[0].epp,192u);
}
static void rt_between_ticks_and_wrap() {
    Fixture f(1); f.cpus[0] = sample(kUrgencyBackground);
    f.cpus[0].rtEvents = kEventMask; f.run(0); EQ(f.out.requests[0].epp,192u);
    f.cpus[0].rtEvents = 0; f.run(50);
    EQ(f.out.requests[0].epp,32u); EQ(f.out.requests[0].minPerf,120u);
    f.run(149); EQ(f.out.requests[0].epp,32u);
    f.run(150); EQ(f.out.requests[0].epp,32u); // relaxation starts here
    f.run(250); EQ(f.out.requests[0].epp,192u);
}
static void foreground_bursts_prevent_background_only() {
    Fixture f(1); f.cpus[0] = sample(kUrgencyBackground); f.run(0);
    ++f.cpus[0].foregroundEvents; f.run(50);
    EQ(f.out.changedCount,1u); EQ(f.out.requests[0].epp,128u);
    f.run(149); EQ(f.out.requests[0].epp,128u);
    f.run(150); EQ(f.out.changedCount,0u);
    f.run(250); EQ(f.out.requests[0].epp,192u);
}
static void pending_hysteresis_restarts_on_flap() {
    Fixture f(1); f.run(0); EQ(f.out.requests[0].epp,128u);
    f.cpus[0] = sample(kUrgencyBackground); f.run(100); // foreground hold expires
    f.run(199); EQ(f.out.changedCount,0u);
    f.cpus[0] = sample(kUrgencyNormal); f.run(200);
    f.cpus[0] = sample(kUrgencyBackground); f.run(300);
    f.run(399); EQ(f.out.changedCount,0u);
    f.run(400); EQ(f.out.requests[0].epp,192u); EQ(f.out.changedCount,1u);
}
static void busy_driven_epp_and_fleeting_rt() {
    const auto c = standardCaps(); const auto t = defaultTunables(); auto sys = unknownSystem();
    // Fleeting desktop RT tick on an otherwise-idle core (busy 5% < 25%): responsive EPP, NO 80% minPerf pin!
    CpuSnapshot fleetingRt = sample(kUrgencyRealtime);
    fleetingRt.busyPermille = 50; fleetingRt.busyObserved = true;
    auto r = decide(fleetingRt, false, false, sys, c, t);
    EQ(r.minPerf, c.lowest); EQ(r.epp, 32u); legal(r, c);

    // Sustained RT workload (busy 40% >= 25%): pins minPerf floor to 80% nominal.
    CpuSnapshot heavyRt = fleetingRt;
    heavyRt.busyPermille = 400;
    r = decide(heavyRt, false, false, sys, c, t);
    EQ(r.minPerf, 120u); EQ(r.epp, 32u); legal(r, c);

    // Active Normal/Foreground compute (busy 50% >= 45%): auto EPP=0 + nominal floor on AC!
    CpuSnapshot heavyNormal = sample(kUrgencyNormal);
    heavyNormal.busyPermille = 500; heavyNormal.busyObserved = true;
    r = decide(heavyNormal, false, false, sys, c, t);
    EQ(r.epp, 0u); EQ(r.minPerf, c.nominal); legal(r, c);

    // On battery, heavy compute drops to realtimeEpp (32) and does not raise minPerf.
    sys.powerSource = kPowerInternal;
    r = decide(heavyNormal, false, false, sys, c, t);
    EQ(r.epp, 32u); EQ(r.minPerf, c.lowest); legal(r, c);

    // Low Power Mode overrides heavy load back to lowPowerEpp (224) + nominal cap.
    sys.lowPowerMode = kOn;
    r = decide(heavyNormal, false, false, sys, c, t);
    EQ(r.epp, 224u); EQ(r.maxPerf, c.nominal); legal(r, c);

    // Background urgency never triggers busy EPP boost even at 95% busy.
    sys = unknownSystem();
    CpuSnapshot heavyBg = sample(kUrgencyBackground);
    heavyBg.busyPermille = 950; heavyBg.busyObserved = true;
    r = decide(heavyBg, false, false, sys, c, t);
    EQ(r.epp, 192u); legal(r, c);
}
static void safety_caps_bypass_relax_hold() {
    Fixture f; f.cpus[0] = f.cpus[1] = sample(kUrgencyRealtime); f.run(0);
    f.system.thermal = kThermalEmergency; f.run(1); EQ(f.out.changedCount,0u);
    f.run(50); EQ(f.out.changedCount,2u); EQ(f.out.requests[0].maxPerf,60u);
    f.system.thermal = kThermalNominal; f.run(100); EQ(f.out.changedCount,2u); // RT floor increases
    f.system.lowPowerMode = kOn; f.run(150); EQ(f.out.changedCount,2u);
    EQ(f.out.requests[0].maxPerf,150u); EQ(f.out.requests[0].epp,224u);
}
static void capability_change_respects_bounds() {
    Fixture f; f.run(0);
    f.caps[0] = f.caps[1] = {120,100,60,40,0,0}; f.run(1);
    CHECK(!cpuValid(f.out,0)); CHECK(!cpuValid(f.out,1)); EQ(f.out.changedCount,0u);
    f.run(50); EQ(f.out.changedCount,2u); EQ(f.out.requests[0].minPerf,40u);
    EQ(f.out.requests[0].maxPerf,120u); legal(f.out.requests[0],f.caps[0]);
}
static void global_batch_rate_and_unchanged_suppression() {
    Fixture f(16); f.tun.relaxHoldMs = f.tun.rtHoldMs = f.tun.foregroundHoldMs = 0;
    f.tun.batchIntervalMs = 0; // cannot bypass the hard 50 ms minimum
    unsigned batches = 0; uint64_t last = 0;
    for (uint64_t now = 0; now < 1000; ++now) {
        for (unsigned c = 0; c < 16; ++c) f.cpus[c] = sample((now/37)%2 ? kUrgencyRealtime : kUrgencyBackground);
        f.run(now);
        if (f.out.changedCount) {
            if (batches) CHECK(now-last >= 50);
            ++batches; last = now; EQ(f.out.changedCount,16u);
        }
    }
    CHECK(batches <= 20); CHECK(batches > 10);
    f.run(1100); // allow the final rate-limited pending batch to settle
    for (unsigned i = 0; i < 20; ++i) { f.run(1150+i*50); EQ(f.out.changedCount,0u); }
}
static void retry_reset_and_unobserved_history() {
    Fixture f(1); f.cpus[0] = sample(kUrgencyBackground); f.cpus[0].rtEvents = 100;
    f.run(0); EQ(f.out.requests[0].epp,192u); // historical RT is primed
    ++f.cpus[0].rtEvents; f.run(50); EQ(f.out.requests[0].epp,32u);
    invalidateRequests(f.state); f.run(51); CHECK(!cpuValid(f.out,0)); EQ(f.out.changedCount,0u);
    f.run(100); EQ(f.out.changedCount,1u);
    f.cpus[0].observed = false; f.run(150); CHECK(!f.state.cpu[0].seen); CHECK(!f.state.cpu[0].hasRt);
    reset(f.state); f.cpus[0].observed = true; f.run(151);
    EQ(f.out.changedCount,1u); EQ(f.out.requests[0].epp,192u);
    f.cpus[0].urgency = kUrgencyRealtime; f.run(201); EQ(f.out.requests[0].epp,32u);
    f.cpus[0].urgency = kUrgencyBackground; f.run(0); // backwards elapsed clock resets state
    EQ(f.out.changedCount,1u); EQ(f.out.requests[0].epp,192u);
}
static void stop_rearm_does_not_revive_realtime() {
    Fixture f(1); CpuPublication p {};
    recordUrgency(p,kUrgencyRealtime); f.cpus[0] = readCpuSnapshot(p,true); f.run(0);
    EQ(f.out.requests[0].epp,32u); CHECK(f.state.cpu[0].hasRt);
    f.cpus[0] = readCpuSnapshot(p,false); f.run(50);
    CHECK(!f.state.cpu[0].seen); CHECK(!f.state.cpu[0].hasRt);
    // RT bursts while disabled must not be replayed, and the latest class wins.
    recordUrgency(p,kUrgencyRealtime); recordUrgency(p,kUrgencyRealtime);
    recordUrgency(p,kUrgencyBackground);
    reset(f.state); // a new started instance primes its own policy state
    f.cpus[0] = readCpuSnapshot(p,true); f.run(100);
    EQ(f.out.requests[0].epp,192u); EQ(f.out.requests[0].minPerf,20u);
    EQ(f.state.cpu[0].rtEvents,3u); CHECK(!f.state.cpu[0].hasRt);
    for (uint64_t now : {150u,350u,550u,1000u}) {
        f.run(now); EQ(f.out.changedCount,0u);
        EQ(f.out.requests[0].epp,192u); CHECK(!f.state.cpu[0].hasRt);
    }
    recordUrgency(p,kUrgencyRealtime); f.cpus[0] = readCpuSnapshot(p,true); f.run(1050);
    EQ(f.out.changedCount,1u); EQ(f.out.requests[0].epp,32u);
    EQ(f.state.cpu[0].lastRtMs,1050u); CHECK(f.state.cpu[0].hasRt);
}
static void all_cpus_masks_and_singletons() {
    Fixture f(kMaxCpus);
    for (unsigned c = 0; c < kMaxCpus; ++c) f.top.domainOfCpu[c] = static_cast<uint16_t>(c);
    f.run(0); EQ(f.out.changedCount,kMaxCpus);
    for (unsigned w = 0; w < kMaskWords; ++w) { EQ(f.out.changed[w],UINT64_MAX); EQ(f.out.valid[w],UINT64_MAX); }
    CHECK(cpuChanged(f.out,255)); CHECK(!cpuChanged(f.out,256)); CHECK(!cpuValid(f.out,UINT32_MAX));
    f.run(50); EQ(f.out.changedCount,0u);
}
static void randomized_request_invariants() {
    uint32_t seed = 0x5700;
    auto next = [&] { seed = seed*1664525u+1013904223u; return seed; };
    for (unsigned i = 0; i < 5000; ++i) {
        const uint32_t lo = next()%500+1, nl = lo+next()%500, nom = nl+next()%500, hi = nom+next()%500;
        const zencppc::Caps c {hi,nom,nl,lo,0,0};
        Tunables t = defaultTunables(); t.baseEpp=next(); t.realtimeEpp=next(); t.backgroundEpp=next();
        t.lowPowerEpp=next(); t.batteryEpp=next(); t.realtimeMinPct=next();
        SystemSignals sys {static_cast<KnownBool>(next()%3),static_cast<ThermalLevel>(next()%6),
            static_cast<PowerSource>(next()%3),kPowerUnobserved,255};
        const auto r = decide(sample(static_cast<Urgency>(next()%5)), next()%2, next()%2, sys,c,t);
        legal(r,c);
        if (sys.lowPowerMode==kOn) CHECK(r.maxPerf<=c.nominal);
    }
}
static void randomized_smt_safety_intersection() {
    uint32_t seed = 0x57005700;
    auto next = [&] { seed = seed*1664525u+1013904223u; return seed; };
    for (unsigned i = 0; i < 2000; ++i) {
        Fixture f;
        for (unsigned c=0;c<2;++c) {
            const uint32_t lo=next()%100+1, nl=lo+next()%100;
            const uint32_t nom=nl+next()%100, hi=nom+next()%100;
            f.caps[c] = {hi,nom,nl,lo,0,0};
            f.cpus[c] = sample(static_cast<Urgency>(next()%5));
        }
        f.system.lowPowerMode = static_cast<KnownBool>(next()%3);
        f.system.thermal = static_cast<ThermalLevel>(next()%6);
        const auto a = decide(f.cpus[0],false,false,f.system,f.caps[0],f.tun);
        const auto b = decide(f.cpus[1],false,false,f.system,f.caps[1],f.tun);
        const uint32_t ceiling = a.maxPerf < b.maxPerf ? a.maxPerf : b.maxPerf;
        const uint32_t floor = f.caps[0].lowest > f.caps[1].lowest ? f.caps[0].lowest : f.caps[1].lowest;
        f.run(0);
        if (ceiling < floor) {
            EQ(f.out.changedCount,0u); CHECK(!cpuValid(f.out,0)); CHECK(!cpuValid(f.out,1));
        } else {
            EQ(f.out.changedCount,2u); CHECK(cpuValid(f.out,0)); CHECK(cpuValid(f.out,1));
            CHECK(zencppc::sameRequest(f.out.requests[0],f.out.requests[1]));
            EQ(f.out.requests[0].maxPerf,ceiling);
            for (unsigned c=0;c<2;++c) legal(f.out.requests[c],f.caps[c]);
        }
    }
}

static void tdie_thermal_hysteresis_and_signal_resolution() {
    // Missing TjMax or non-positive Tdie -> unknown (safe fallback to macOS warning).
    EQ(thermalFromTdie(0, 95000, kThermalUnknown), kThermalUnknown);
    EQ(thermalFromTdie(75000, 0, kThermalUnknown), kThermalUnknown);

    // Rising edge on 95 C TjMax: <89 nominal, 89 moderate, 92 heavy, 95 critical, 100 emergency.
    ThermalLevel th = kThermalUnknown;
    th = thermalFromTdie(80000, 95000, th); EQ(th, kThermalNominal);
    th = thermalFromTdie(88999, 95000, th); EQ(th, kThermalNominal);
    th = thermalFromTdie(89000, 95000, th); EQ(th, kThermalModerate);
    th = thermalFromTdie(92000, 95000, th); EQ(th, kThermalHeavy);
    th = thermalFromTdie(95000, 95000, th); EQ(th, kThermalCritical);
    th = thermalFromTdie(100000, 95000, th); EQ(th, kThermalEmergency);

    // Falling edge uses 3 C hysteresis so clocks do not chatter around a threshold.
    th = thermalFromTdie(97000, 95000, th); EQ(th, kThermalEmergency); // 100 - 3
    th = thermalFromTdie(96999, 95000, th); EQ(th, kThermalCritical);
    th = thermalFromTdie(92000, 95000, th); EQ(th, kThermalCritical);  // 95 - 3
    th = thermalFromTdie(91999, 95000, th); EQ(th, kThermalHeavy);
    th = thermalFromTdie(89000, 95000, th); EQ(th, kThermalHeavy);     // 92 - 3
    th = thermalFromTdie(88999, 95000, th); EQ(th, kThermalModerate);
    th = thermalFromTdie(86000, 95000, th); EQ(th, kThermalModerate);  // 89 - 3
    th = thermalFromTdie(85999, 95000, th); EQ(th, kThermalNominal);

    // Desktop spoofing a low-TDP MacBook SMBIOS: fake macOS Heavy warning + battery,
    // real CPU is cool (55 C) -> auto thermal ignores fake macOS warning, acOverride=1 restores AC.
    SystemSignals raw {kOff, kThermalHeavy, kPowerInternal, kPowerPlatformProperty, 100};
    SystemSignals eff = resolveSystemSignals(raw, 1, -1, kThermalCapAuto, kThermalNominal);
    EQ(eff.powerSource, kPowerExternal);
    EQ(eff.thermal, kThermalNominal);
    EQ(eff.lowPowerMode, kOff);

    // Laptop spoofing MacPro7,1 (macOS reports nominal thermal) but real Tdie hits 93 C:
    SystemSignals laptopRaw {kOff, kThermalNominal, kPowerInternal, kPowerSourceProperty, 0};
    SystemSignals laptopEff = resolveSystemSignals(laptopRaw, -1, -1, kThermalCapAuto, kThermalHeavy);
    EQ(laptopEff.powerSource, kPowerInternal);
    EQ(laptopEff.thermal, kThermalHeavy);

    // Mode 0 (off) disables all thermal capping; mode 1 (macos) forces macOS warning;
    // mode 3 (auto) falls back to macOS warning when Tdie is unavailable.
    EQ(resolveSystemSignals(raw, -1, -1, kThermalCapOff, kThermalCritical).thermal, kThermalNominal);
    EQ(resolveSystemSignals(raw, -1, -1, kThermalCapMacOs, kThermalNominal).thermal, kThermalHeavy);
    EQ(resolveSystemSignals(raw, -1, -1, kThermalCapAuto, kThermalUnknown).thermal, kThermalHeavy);
    EQ(resolveSystemSignals(raw, -1, 1, kThermalCapAuto, kThermalNominal).lowPowerMode, kOn);
}

static void cpu_profile_detection_and_tunables() {
    EQ(detectCpuProfile(0x19, 0x21, "AMD Ryzen 7 5700X 8-Core Processor", 8), kProfileDesktop);
    EQ(detectCpuProfile(0x19, 0x50, "AMD Ryzen 7 5700G with Radeon Graphics", 8), kProfileDesktop);
    EQ(detectCpuProfile(0x19, 0x50, "AMD Ryzen 7 5800U with Radeon Graphics", 8), kProfileMobileEco);
    EQ(detectCpuProfile(0x19, 0x50, "AMD Ryzen 7 5825U with Radeon Graphics", 8), kProfileMobileEco);
    EQ(detectCpuProfile(0x19, 0x50, "AMD Ryzen 7 5800H with Radeon Graphics", 8), kProfileMobilePerf);
    EQ(detectCpuProfile(0x19, 0x74, "AMD Ryzen 7 7840HS w/ Radeon 780M Graphics", 8), kProfileMobilePerf);
    EQ(detectCpuProfile(0x19, 0x61, "AMD Ryzen 9 7945HX with Radeon Graphics", 16), kProfileMobilePerf);
    EQ(detectCpuProfile(0x1A, 0x24, "AMD Ryzen AI 9 HX 370 w/ Radeon 890M", 12), kProfileMobilePerf);
    EQ(detectCpuProfile(0x19, 0x08, "AMD Ryzen Threadripper PRO 5975WX 32-Cores", 32), kProfileWorkstation);
    EQ(detectCpuProfile(0x19, 0x01, "AMD EPYC 7763 64-Core Processor", 64), kProfileWorkstation);

    const Tunables eco = tunablesForProfile(kProfileMobileEco);
    const Tunables perf = tunablesForProfile(kProfileMobilePerf);
    const Tunables desk = tunablesForProfile(kProfileDesktop);
    const Tunables ws = tunablesForProfile(kProfileWorkstation);
    CHECK(eco.batteryEpp > desk.batteryEpp);
    CHECK(perf.batteryEpp > desk.batteryEpp);
    CHECK(perf.baseEpp < desk.baseEpp);
    CHECK(ws.busyBoostThreshold >= desk.busyBoostThreshold);
    EQ(recommendedIdleThresholdUs(kProfileMobileEco), 35u);
    EQ(recommendedIdleThresholdUs(kProfileDesktop), 50u);
    EQ(recommendedIdleThresholdUs(kProfileWorkstation), 75u);

    CpuProfile parsed = kProfileAuto;
    CHECK(profileFromName("mobile-eco", parsed) && parsed == kProfileMobileEco);
    CHECK(profileFromName("laptop", parsed) && parsed == kProfileMobilePerf);
    CHECK(profileFromName("workstation", parsed) && parsed == kProfileWorkstation);
    CHECK(!profileFromName("invalid", parsed));
}

int main() {
    struct Test { const char *name; void (*run)(); } tests[] = {
#define TEST(fn) {#fn,fn}
        TEST(snapshot_classes_and_counters), TEST(snapshot_wrap_and_cpu_isolation),
        TEST(snapshot_disabled_recording_and_rearm),
        TEST(snapshot_concurrent_coherence), TEST(balanced_and_background_classes),
        TEST(realtime_floor_and_epp), TEST(low_power_overrides_rt), TEST(battery_baseline),
        TEST(thermal_decode), TEST(thermal_progression_and_clamping), TEST(tunable_limits_and_extreme_caps),
        TEST(caps_validation), TEST(smt_nonadjacent_domains), TEST(smt_capability_intersection),
        TEST(invalid_domain_caps_and_recovery), TEST(smt_safety_ceiling_below_nonlinear),
        TEST(smt_safety_ceiling_below_hardware_floor), TEST(topology_validation),
        TEST(realtime_burst_hold_and_relaxation), TEST(rt_between_ticks_and_wrap),
        TEST(foreground_bursts_prevent_background_only), TEST(pending_hysteresis_restarts_on_flap),
        TEST(busy_driven_epp_and_fleeting_rt),
        TEST(tdie_thermal_hysteresis_and_signal_resolution),
        TEST(cpu_profile_detection_and_tunables),
        TEST(safety_caps_bypass_relax_hold), TEST(capability_change_respects_bounds),
        TEST(global_batch_rate_and_unchanged_suppression), TEST(retry_reset_and_unobserved_history),
        TEST(stop_rearm_does_not_revive_realtime),
        TEST(all_cpus_masks_and_singletons), TEST(randomized_request_invariants),
        TEST(randomized_smt_safety_intersection),
#undef TEST
    };
    for (const auto &test : tests) {
        testName=test.name; const unsigned before=failures; test.run();
        std::printf("%-49s %s\n",test.name,failures==before ? "ok" : "FAILED");
    }
    std::printf("\n%zu tests, %u checks, %u failures\n",sizeof(tests)/sizeof(tests[0]),checks,failures);
    return failures ? 1 : 0;
}
