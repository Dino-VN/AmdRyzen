// Offline regression tests for the ACTUAL idle publication, reader, baseline,
// source-handoff and governor helpers used by ZenIdle.cpp / AMDZenPM.cpp.
// Synthetic clocks permit deterministic interleavings; no hardware is accessed.
#include <cstdio>
#include <cstdint>
#include <initializer_list>
#include "ZenIdleAccounting.hpp"
#include "ZenModel.hpp"

using namespace zen;

static int checks, failures;
static const char *testName;
#define CHECK(x) do { ++checks; if (!(x)) { ++failures; \
    std::printf("FAIL %s:%d [%s] %s\n", __FILE__, __LINE__, testName, #x); } } while (0)
#define EQ(a, b) do { const auto av = (a); const auto bv = (b); CHECK(av == bv); } while (0)

static IdleSnapshot readAt(const IdleAccounting &s, uint64_t now) {
    return idleReadSnapshot(s, [=] { return now; });
}
static void enterAt(IdleAccounting &s, uint64_t now) {
    const uint64_t seq = idleBeginUpdate(s);
    idleFinishEnter(s, seq, now);
}
static uint64_t exitAt(IdleAccounting &s, uint64_t now, bool deep = false) {
    const uint64_t seq = idleBeginUpdate(s);
    return idleFinishExit(s, seq, now, deep);
}
static PStateTable table() {
    PStateTable t {};
    t.count = 3;
    t.freqMHz[0] = 3400;
    t.freqMHz[1] = 2800;
    t.freqMHz[2] = 2200;
    t.slowest = 2;
    return t;
}
static uint32_t decide(GovState &g, const GovTunables &tun, const PStateTable &t,
                       uint32_t requested, uint32_t busy) {
    return governLegacy(kAMDZenGovSchedutil, tun, t, &g,
                        utilFromBusy(busy, t.freqMHz[requested], t.freqMHz[0]), busy, 0);
}

static void open_and_closed() {
    IdleAccounting s {};
    IdleSnapshot baseline {};
    EQ(idleWindowAdvance(&baseline, readAt(s, 0), false).status, kIdleSamplePrimed);
    CHECK(!(s.seq & 1));
    enterAt(s, 24000);
    CHECK(!(s.seq & 1));             // even is stable, INCLUDING an open interval
    EQ(s.inIdle, 1u);
    const IdleSnapshot open = readAt(s, 40000);
    CHECK(open.valid);
    EQ(open.tsc, 40000u);
    EQ(open.idleAcc, 16000u);
    EQ(open.entries, 0u);
    IdleSnapshot before = baseline;
    const IdleWindow w = idleWindowAdvance(&before, open, false);
    EQ(w.status, kIdleSampleReady);
    EQ(w.busyPermille, 600u);

    EQ(exitAt(s, 40000), 16000u);
    const IdleSnapshot closed = readAt(s, 40000);
    CHECK(closed.valid);
    EQ(closed.idleAcc, 16000u);      // NOT 32000: the close cannot double-count
    EQ(closed.entries, 1u);
    EQ(s.inIdle, 0u);
    EQ(idleWindowAdvance(&baseline, closed, false).busyPermille, 600u);
    enterAt(s, 41000);
    EQ(exitAt(s, 44000, true), 3000u);
    const IdleSnapshot both = readAt(s, 50000);
    EQ(both.idleAcc, 19000u);
    EQ(both.entries, 2u);
    EQ(s.deepTicks, 3000u);
    EQ(s.shallowTicks, 16000u);
    EQ(idleWindowAdvance(&baseline, both, false).busyPermille, 700u);
}

static void staged_writer_publication() {
    IdleAccounting s {};
    const uint64_t seq = idleBeginUpdate(s);
    CHECK(s.seq & 1);
    CHECK(!readAt(s, 24000).valid);  // before the entry timestamp/payload
    __atomic_store_n(&s.stamp, 24000, __ATOMIC_RELAXED);
    CHECK(!readAt(s, 40000).valid);
    __atomic_store_n(&s.inIdle, 1, __ATOMIC_RELAXED);
    CHECK(!readAt(s, 40000).valid);  // complete payload, not committed yet
    idleFinishEnter(s, seq, 24000);
    CHECK(readAt(s, 40000).valid);

    // Stage each close store while the actual begin-update keeps seq odd. The
    // critical old reproduction is closed=16000 AND still-open since=24000.
    IdleAccounting closing = s;
    idleBeginUpdate(closing);
    CHECK(!readAt(closing, 40000).valid);
    __atomic_store_n(&closing.shallowEntries, 1, __ATOMIC_RELAXED);
    CHECK(!readAt(closing, 40000).valid);
    __atomic_store_n(&closing.shallowTicks, 16000, __ATOMIC_RELAXED);
    CHECK(!readAt(closing, 40000).valid);
    __atomic_store_n(&closing.stamp, 40000, __ATOMIC_RELAXED);
    CHECK(!readAt(closing, 40000).valid);
    __atomic_store_n(&closing.inIdle, 0, __ATOMIC_RELAXED);
    CHECK(!readAt(closing, 40000).valid);

    // Normal exit uses the same production helpers and commits exactly once.
    exitAt(s, 40000);
    EQ(readAt(s, 40000).idleAcc, 16000u);
}

static void reader_retries_transitions() {
    IdleAccounting s {};
    unsigned clocks = 0;
    IdleSnapshot snap = idleReadSnapshot(s, [&] {
        if (++clocks == 1) { enterAt(s, 24000); exitAt(s, 40000); }
        return 40000ull;
    });
    EQ(clocks, 2u);                 // whole interval published between seq loads
    CHECK(snap.valid);
    EQ(snap.idleAcc, 16000u);

    enterAt(s, 50000);
    clocks = 0;
    snap = idleReadSnapshot(s, [&] {
        if (++clocks == 1) exitAt(s, 60000, true);
        return 60000ull;
    });
    EQ(clocks, 2u);
    CHECK(snap.valid);
    EQ(snap.idleAcc, 26000u);
    EQ(snap.entries, 2u);

    clocks = 0;
    snap = idleReadSnapshot(s, [&] {
        if (++clocks == 1) {
            // The old data would be valid at 65000, but an entry AFTER that
            // observation still changes seq. Retry obtains its OWN later clock.
            enterAt(s, 70000);
            return 65000ull;
        }
        return 80000ull;
    });
    EQ(clocks, 2u);
    CHECK(snap.valid);
    EQ(snap.tsc, 80000u);
    EQ(snap.idleAcc, 36000u);
}

static void delayed_reader_per_cpu_clock() {
    IdleAccounting early {}, late {};
    IdleSnapshot earlyBase {}, lateBase {};
    idleWindowAdvance(&earlyBase, readAt(early, 0), false);
    idleWindowAdvance(&lateBase, readAt(late, 0), false);
    enterAt(late, 24000);
    const IdleSnapshot a = readAt(early, 40000);   // former common scan cutoff
    exitAt(late, 44000);                         // close happens AFTER it
    const IdleSnapshot b = readAt(late, 48000);   // reader delayed reaching CPU 1
    CHECK(a.valid && b.valid);
    EQ(a.tsc, 40000u);
    EQ(b.tsc, 48000u);
    EQ(b.idleAcc, 20000u);
    EQ(idleWindowAdvance(&earlyBase, a, false).busyPermille, 1000u);
    EQ(idleWindowAdvance(&lateBase, b, false).busyPermille, 583u); // 28000/48000
    // The following window starts at 48000 for CPU 1, not the common 40000.
    enterAt(late, 64000);
    EQ(idleWindowAdvance(&lateBase, readAt(late, 80000), false).busyPermille, 500u);
    EQ(idleWindowAdvance(&earlyBase, readAt(early, 80000), false).busyPermille, 1000u);
}

static void retry_exhaustion_and_recovery() {
    IdleAccounting s {};
    enterAt(s, 1000);
    IdleSnapshot baseline {};
    idleWindowAdvance(&baseline, readAt(s, 11000), false);
    const uint64_t oldAcc = baseline.idleAcc;
    uint64_t now = 12000;
    unsigned clocks = 0;
    IdleSnapshot bad = idleReadSnapshot(s, [&] {
        ++clocks;
        exitAt(s, now);
        enterAt(s, now + 100);
        now += 1000;
        return now;
    });
    EQ(clocks, kIdleSnapshotAttempts);
    CHECK(!bad.valid);
    EQ(idleWindowAdvance(&baseline, bad, false).status, kIdleSampleInvalid);
    CHECK(!baseline.valid);
    EQ(baseline.idleAcc, oldAcc);             // no failed-read counters substituted
    EQ(idleWindowAdvance(&baseline, readAt(s, 40000), false).status, kIdleSamplePrimed);
    const IdleWindow good = idleWindowAdvance(&baseline, readAt(s, 80000), false);
    EQ(good.status, kIdleSampleReady);
    EQ(good.busyPermille, 0u);                // no inflated next-window idle delta

    // A publication stuck odd also returns immediately after the bounded budget.
    const uint64_t seq = idleBeginUpdate(s);
    clocks = 0;
    bad = idleReadSnapshot(s, [&] { ++clocks; return 90000ull; });
    CHECK(!bad.valid);
    EQ(clocks, 0u);                          // no timestamp/payload read while odd
    EQ(idleWindowAdvance(&baseline, bad, false).status, kIdleSampleInvalid);
    idleFinishExit(s, seq, 90000, false);
    EQ(idleWindowAdvance(&baseline, readAt(s, 100000), false).status, kIdleSamplePrimed);
    EQ(idleWindowAdvance(&baseline, readAt(s, 140000), false).busyPermille, 1000u);
}

static void invalid_snapshots() {
    IdleAccounting s {};
    enterAt(s, 200);
    CHECK(!readAt(s, 199).valid);             // future open timestamp
    exitAt(s, 300);
    CHECK(!readAt(s, 299).valid);             // future CLOSED timestamp too
    s.inIdle = 2;
    CHECK(!readAt(s, 400).valid);
    s = IdleAccounting {};
    s.deepTicks = UINT64_MAX;
    s.shallowTicks = 1;
    CHECK(!readAt(s, 400).valid);
    s.shallowTicks = 0;
    enterAt(s, 300);
    CHECK(!readAt(s, 400).valid);             // open + closed overflow
    s = IdleAccounting {};
    s.deepEntries = UINT64_MAX;
    s.shallowEntries = 1;
    CHECK(!readAt(s, 400).valid);
}

static void baseline_discontinuities() {
    IdleSnapshot base { 16000, 1, 40000, true };
    IdleSnapshot cur { 80000, 2, 80000, true };
    EQ(idleWindowAdvance(&base, cur, false).status, kIdleSampleInvalid); // idle > window
    CHECK(base.valid);                       // coherent new endpoint re-primes
    cur.tsc += 40000;
    EQ(idleWindowAdvance(&base, cur, false).busyPermille, 1000u);
    cur.idleAcc = 0;                         // counters reset
    cur.tsc += 40000;
    EQ(idleWindowAdvance(&base, cur, false).status, kIdleSampleInvalid);
    cur.tsc += 40000;
    EQ(idleWindowAdvance(&base, cur, false).status, kIdleSampleReady);
    cur.entries = 0;
    cur.tsc += 40000;
    EQ(idleWindowAdvance(&base, cur, false).status, kIdleSampleInvalid);
    cur.tsc = 1000;                          // clock reset
    EQ(idleWindowAdvance(&base, cur, false).status, kIdleSampleInvalid);
    cur.tsc = 41000;
    EQ(idleWindowAdvance(&base, cur, false).busyPermille, 1000u);
    EQ(idleWindowAdvance(&base, cur, false).status, kIdleSampleInvalid); // no elapsed time
    cur.tsc += 40000;
    EQ(idleWindowAdvance(&base, cur, true).status, kIdleSamplePrimed);   // explicit reset
    CHECK(base.valid);
    EQ(idleWindowAdvance(nullptr, cur, false).status, kIdleSampleInvalid);
    EQ(idleWindowBusyPermille(0, 50000, 40000), 1000u); // no malformed window clamped idle
    EQ(idleWindowBusyPermille(50000, 0, 40000), 1000u);
    EQ(idleWindowBusyPermille(0, 0, 0), 1000u);
    EQ(idleWindowBusyPermille(0, UINT64_MAX / 2, UINT64_MAX), 500u);
}

static void wake_and_writer_clock_reset() {
    IdleAccounting s {};
    IdleSnapshot base {};
    enterAt(s, 100000);
    idleWindowAdvance(&base, readAt(s, 110000), false);
    CHECK(!readAt(s, 200).valid);             // TSC reset while hook interval open
    EQ(idleWindowAdvance(&base, readAt(s, 200), true).status, kIdleSampleInvalid);
    EQ(exitAt(s, 300), 0u);                  // NOT unsigned underflow in ticks/EWMA
    EQ(s.shallowTicks, 0u);
    EQ(idleWindowAdvance(&base, readAt(s, 400), false).status, kIdleSamplePrimed);
    EQ(idleWindowAdvance(&base, readAt(s, 40400), false).busyPermille, 1000u);

    enterAt(s, 50000);
    idleWindowAdvance(&base, readAt(s, 60000), false);
    EQ(idleWindowAdvance(&base, readAt(s, 1000000000), true).status, kIdleSamplePrimed);
    EQ(idleWindowAdvance(&base, readAt(s, 1000040000), false).busyPermille, 0u);
    exitAt(s, 1000040000);
    EQ(idleWindowAdvance(&base, readAt(s, 1000080000), false).busyPermille, 1000u);
}

static void no_exits_busy_or_open_idle() {
    for (bool open : {false, true}) {
        IdleAccounting s {};
        if (open) enterAt(s, 0);             // zero is a valid entry, not a sentinel
        IdleSnapshot base {};
        idleWindowAdvance(&base, readAt(s, 1000), false);
        GovTunables tun;
        defaultTunables(&tun);
        GovState g {};
        g.curIdx = 2;
        uint32_t req = 2;
        bool perfFresh = true;
        unsigned perfRounds = 0;
        for (unsigned tick = 1; tick <= 100; ++tick) {
            const IdleSnapshot cur = readAt(s, 1000ull + tick * 40000ull);
            EQ(cur.entries, 0u);
            const IdleWindow w = idleWindowAdvance(&base, cur, false);
            EQ(w.status, kIdleSampleReady);
            EQ(w.busyPermille, open ? 0u : 1000u);
            const GovSamplePlan p = planGovernorSample(true, false, w.status == kIdleSampleReady,
                                                       tick % 25 == 0, perfFresh, true);
            EQ(p.source, kGovSampleIdle);
            perfRounds += p.samplePerf;
            perfFresh = p.nextPerfFresh;
            req = decide(g, tun, table(), req, w.busyPermille);
            EQ(req, open ? 2u : 0u);
        }
        EQ(perfRounds, 4u);                  // telemetry only, not exit-count fallback
    }
}

static void alternating_work_wait() {
    IdleAccounting s {};
    IdleSnapshot base {};
    uint64_t now = 1000;
    idleWindowAdvance(&base, readAt(s, now), false);
    GovTunables tun;
    defaultTunables(&tun);
    GovState g {};
    g.curIdx = 2;
    uint32_t req = 2;
    bool perfFresh = true;
    unsigned perfRounds = 0, saturatedFast = 0, slowTicks = 0;
    for (unsigned tick = 1; tick <= 100; ++tick) {
        const bool saturated = tick % 2 == 0;
        if (!saturated) {
            enterAt(s, now + 20000);
            exitAt(s, now + 40000);
        }
        now += 40000;
        const IdleWindow w = idleWindowAdvance(&base, readAt(s, now), false);
        EQ(w.status, kIdleSampleReady);
        EQ(w.busyPermille, saturated ? 1000u : 500u);
        const GovSamplePlan p = planGovernorSample(true, false, w.status == kIdleSampleReady,
                                                   tick % 25 == 0, perfFresh, true);
        EQ(p.source, kGovSampleIdle);
        perfRounds += p.samplePerf;
        perfFresh = p.nextPerfFresh;
        req = decide(g, tun, table(), req, w.busyPermille);
        if (saturated) { EQ(req, 0u); saturatedFast += req == 0; }
        slowTicks += req == 2;
    }
    EQ(saturatedFast, 50u);                   // old handoff lost nearly all of these
    CHECK(slowTicks < 50);                   // old reproduction: P2 on 90/100 ticks
    EQ(perfRounds, 4u);
}

static void source_handoff_and_stale_perf() {
    GovSamplePlan p = planGovernorSample(true, true, false, true, false, true);
    EQ(p.source, kGovSampleHold);            // initial/wake priming
    CHECK(p.samplePerf && p.nextPerfFresh);
    p = planGovernorSample(true, false, true, false, p.nextPerfFresh, true);
    EQ(p.source, kGovSampleIdle);
    CHECK(!p.samplePerf && !p.nextPerfFresh);

    GovTunables tun;
    defaultTunables(&tun);
    GovState g {};
    uint32_t req = decide(g, tun, table(), 0, 1000);
    EQ(req, 0u);
    const GovState saved = g;
    // A stale multi-window APERF delta looks mostly idle despite the most recent
    // saturated window. The source planner must PRIME, not apply that old delta.
    const PerfCounters old {0, 0, 1000, 0};
    const PerfCounters sample {40000, 40000, 401000, 0};
    EQ(derivePerf(old, sample, 1000000, 1, false).busyPermille, 100u);
    p = planGovernorSample(true, false, false, false, p.nextPerfFresh, true);
    EQ(p.source, kGovSampleHold);
    CHECK(p.samplePerf && p.nextPerfFresh);
    // Same dispatch as production: no governor call in the hold case.
    if (p.source == kGovSamplePerf) req = decide(g, tun, table(), req, 100);
    EQ(req, 0u);
    EQ(g.curIdx, saved.curIdx);
    EQ(g.utilEwma, saved.utilEwma);
    EQ(g.holdTicks, saved.holdTicks);

    const PerfCounters current {80000, 80000, 441000, 0};
    p = planGovernorSample(true, false, false, false, p.nextPerfFresh, true);
    EQ(p.source, kGovSamplePerf);            // consecutive real samples: usable
    CHECK(governorPerfWindowValid(sample, current));
    const PerfDerived d = derivePerf(sample, current, 1000000, 1, false);
    EQ(d.busyPermille, 1000u);
    req = governLegacy(kAMDZenGovSchedutil, tun, table(), &g, d.util1024, d.busyPermille, 0);
    EQ(req, 0u);

    // The first good idle endpoint merely primes after the missing observation;
    // APERF can continue until the second endpoint provides a complete window.
    IdleAccounting s {};
    IdleSnapshot base {};
    IdleWindow w = idleWindowAdvance(&base, readAt(s, 481000), false);
    EQ(w.status, kIdleSamplePrimed);
    p = planGovernorSample(true, false, w.status == kIdleSampleReady, false, p.nextPerfFresh, true);
    EQ(p.source, kGovSamplePerf);
    w = idleWindowAdvance(&base, readAt(s, 521000), false);
    p = planGovernorSample(true, false, w.status == kIdleSampleReady, true, p.nextPerfFresh, true);
    EQ(p.source, kGovSampleIdle);
    CHECK(p.samplePerf && p.nextPerfFresh);   // telemetry can prime the next fallback
    p = planGovernorSample(true, false, false, false, p.nextPerfFresh, true);
    EQ(p.source, kGovSamplePerf);

    // Disabled/inactive hook continues the APERF stream; switching it back on
    // re-primes idle without discarding a genuinely adjacent APERF sample.
    for (unsigned tick = 0; tick < 8; ++tick) {
        const bool idleReady = tick % 2 == 0;
        p = planGovernorSample(true, false, idleReady, false, p.nextPerfFresh, true);
        EQ(p.source, idleReady ? kGovSampleIdle : kGovSampleHold);
    }
}

static void resets_and_unavailable_perf() {
    GovSamplePlan p = planGovernorSample(false, false, false, true, true, true);
    EQ(p.source, kGovSampleHold);
    CHECK(p.samplePerf && !p.nextPerfFresh);  // static telemetry is not a dynamic window
    p = planGovernorSample(true, false, false, false, p.nextPerfFresh, true);
    EQ(p.source, kGovSampleHold);
    CHECK(p.nextPerfFresh);
    p = planGovernorSample(true, true, true, true, p.nextPerfFresh, true);
    EQ(p.source, kGovSampleHold);            // reset overrides either claimed source
    p = planGovernorSample(true, false, false, false, true, false);
    EQ(p.source, kGovSampleHold);            // no APERF capability: never fabricate it
    CHECK(!p.samplePerf && !p.nextPerfFresh);
    p = planGovernorSample(true, false, true, false, false, false);
    EQ(p.source, kGovSampleIdle);            // valid accounting still works without APERF
    p = planGovernorSample(true, false, false, true, false, false);
    EQ(p.source, kGovSampleHold);
    CHECK(p.samplePerf && !p.nextPerfFresh);

    PerfCounters a {100, 100, 1000, 0}, b {200, 200, 41000, 0};
    CHECK(governorPerfWindowValid(a, b));
    b.tsc = 0;
    CHECK(!governorPerfWindowValid(a, b));    // unanswered sampling round
    b.tsc = 1000;
    CHECK(!governorPerfWindowValid(a, b));
    b.tsc = 900;
    CHECK(!governorPerfWindowValid(a, b));
    b.tsc = 41000;
    b.aperf = 0;
    CHECK(!governorPerfWindowValid(a, b));
    b.aperf = 200;
    b.mperf = 0;
    CHECK(!governorPerfWindowValid(a, b));
    // A coherent sample after a reset primes a normal next interval.
    a = b;
    b = PerfCounters {300, 100, 81000, 0};
    CHECK(governorPerfWindowValid(a, b));
    a.tsc = 0;
    CHECK(!governorPerfWindowValid(a, b));
}

static void fast_up_threshold_and_limits() {
    GovTunables tun;
    defaultTunables(&tun);
    EQ(tun.fastUpPct, 60u);
    for (uint32_t busy : {599u, 600u, 601u, 1000u}) {
        IdleAccounting s {};
        IdleSnapshot base {};
        idleWindowAdvance(&base, readAt(s, 0), false);
        enterAt(s, busy * 40ull);
        exitAt(s, 40000);
        const IdleWindow w = idleWindowAdvance(&base, readAt(s, 40000), false);
        EQ(w.status, kIdleSampleReady);
        EQ(w.busyPermille, busy);
        GovState g {};
        g.curIdx = 2;
        EQ(decide(g, tun, table(), 2, busy), busy >= 600 ? 0u : 2u);
    }
    GovState g {};
    g.curIdx = 2;
    PStateTable t = table();
    t.fastest = 1;
    EQ(decide(g, tun, t, 2, 1000), 1u);      // policy / hardware performance cap
    t.slowest = 1;
    EQ(decide(g, tun, t, 1, 0), 1u);         // minimum-performance floor
    tun.fastUpPct = 0;
    g = GovState {};
    g.curIdx = 2;
    EQ(decide(g, tun, table(), 2, 600), 2u);
    tun.fastUpPct = 100;
    g = GovState {};
    g.curIdx = 2;
    EQ(decide(g, tun, table(), 2, 999), 1u);
    EQ(decide(g, tun, table(), 1, 1000), 0u);
    tun.fastUpPct = 60;
    GovState cppc {};
    EQ(governCppcDesired(kAMDZenGovSchedutil, tun, 30, 150, &cppc, 0, 600, 0), 150u);
}

static void sequence_wrap() {
    IdleAccounting s {};
    s.seq = UINT64_MAX - 1;                  // last even sequence before wrap
    enterAt(s, 0);
    EQ(s.seq, 0u);
    CHECK(readAt(s, 100).valid);
    EQ(readAt(s, 100).idleAcc, 100u);
    exitAt(s, 200);
    EQ(s.seq, 2u);
    EQ(readAt(s, 300).idleAcc, 200u);
}

int main() {
    struct Test { const char *name; void (*run)(); } tests[] = {
#define TEST(fn) {#fn, fn}
        TEST(open_and_closed), TEST(staged_writer_publication), TEST(reader_retries_transitions),
        TEST(delayed_reader_per_cpu_clock), TEST(retry_exhaustion_and_recovery),
        TEST(invalid_snapshots), TEST(baseline_discontinuities), TEST(wake_and_writer_clock_reset),
        TEST(no_exits_busy_or_open_idle), TEST(alternating_work_wait),
        TEST(source_handoff_and_stale_perf), TEST(resets_and_unavailable_perf),
        TEST(fast_up_threshold_and_limits), TEST(sequence_wrap),
#undef TEST
    };
    for (const Test &test : tests) {
        testName = test.name;
        const int before = failures;
        test.run();
        std::printf("%-36s %s\n", test.name, failures == before ? "ok" : "FAILED");
    }
    std::printf("\n%zu tests, %d checks, %d failures\n", sizeof(tests) / sizeof(tests[0]), checks, failures);
    return failures ? 1 : 0;
}
