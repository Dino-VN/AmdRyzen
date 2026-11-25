//
// ZenIdleAccounting.hpp — single-writer per-CPU idle publication and bounded read.
// No kernel dependencies; the SAME operations run in MachineIdle and host tests.
// Every shared word is atomic. Only the owning CPU writes, with IF=0; no locks,
// RMWs, retry loops, allocation or callbacks in the writer.
//
#ifndef ZEN_IDLE_ACCOUNTING_HPP
#define ZEN_IDLE_ACCOUNTING_HPP

#include "ZenIdleModel.hpp"

namespace zen {

struct IdleAccounting {
    uint64_t deepEntries, shallowEntries;
    uint64_t deepTicks, shallowTicks;
    uint64_t stamp;      // entry while inIdle; last exit otherwise
    uint64_t seq;        // odd ONLY during publication; even both idle and busy
    uint64_t inIdle;     // independent of seq: a stable open interval is readable
};
static_assert(__atomic_always_lock_free(sizeof(uint64_t), nullptr), "idle words must be lock-free");

// Begin BEFORE reading the transition TSC, end AFTER updating all payload words.
// The full store fence (MFENCE on x86_64) makes odd globally visible before that
// timestamp. Otherwise an observer could accept the old state at a later TSC and
// the next observation would retroactively add idle time before its baseline.
// The caller uses an ordered clock read; see accountingTsc() in ZenIdle.cpp.
static inline uint64_t idleBeginUpdate(IdleAccounting &s) {
    const uint64_t seq = __atomic_load_n(&s.seq, __ATOMIC_RELAXED);
    __atomic_store_n(&s.seq, seq + 1, __ATOMIC_RELAXED);
    __atomic_thread_fence(__ATOMIC_SEQ_CST);
    return seq + 2;
}

static inline void idleFinishEnter(IdleAccounting &s, uint64_t seq, uint64_t tsc) {
    __atomic_store_n(&s.stamp, tsc, __ATOMIC_RELAXED);
    __atomic_store_n(&s.inIdle, 1, __ATOMIC_RELAXED);
    __atomic_store_n(&s.seq, seq, __ATOMIC_RELEASE);
}

static inline uint64_t idleFinishExit(IdleAccounting &s, uint64_t seq, uint64_t tsc, bool deep) {
    const uint64_t since = __atomic_load_n(&s.stamp, __ATOMIC_RELAXED);
    // A TSC reset during sleep must not add an unsigned-underflow-sized interval.
    // Driver baselines are reset on wake; readers also reject regressing windows.
    const uint64_t dt = tsc >= since ? tsc - since : 0;
    uint64_t *entries = deep ? &s.deepEntries : &s.shallowEntries;
    uint64_t *ticks = deep ? &s.deepTicks : &s.shallowTicks;
    __atomic_store_n(entries, __atomic_load_n(entries, __ATOMIC_RELAXED) + 1, __ATOMIC_RELAXED);
    __atomic_store_n(ticks, __atomic_load_n(ticks, __ATOMIC_RELAXED) + dt, __ATOMIC_RELAXED);
    __atomic_store_n(&s.stamp, tsc, __ATOMIC_RELAXED);
    __atomic_store_n(&s.inIdle, 0, __ATOMIC_RELAXED);
    __atomic_store_n(&s.seq, seq, __ATOMIC_RELEASE);
    return dt;
}

constexpr uint32_t kIdleSnapshotAttempts = 8;

// Clock is an inlined, ordered TSC read in production. Tests inject transitions at
// this exact read point, exercising the production retry loop rather than a mock.
// The timestamp is INSIDE the validated read. A scan-wide timestamp cannot bound
// counters which may have closed after that timestamp, before this CPU was read.
template <typename Clock>
static inline IdleSnapshot idleReadSnapshot(const IdleAccounting &s, Clock clock) {
    for (uint32_t tries = 0; tries < kIdleSnapshotAttempts; ++tries) {
        const uint64_t seq = __atomic_load_n(&s.seq, __ATOMIC_ACQUIRE);
        if (seq & 1) continue;
        const uint64_t stamp = __atomic_load_n(&s.stamp, __ATOMIC_RELAXED);
        const uint64_t inIdle = __atomic_load_n(&s.inIdle, __ATOMIC_RELAXED);
        const uint64_t deepTicks = __atomic_load_n(&s.deepTicks, __ATOMIC_RELAXED);
        const uint64_t shallowTicks = __atomic_load_n(&s.shallowTicks, __ATOMIC_RELAXED);
        const uint64_t deepEntries = __atomic_load_n(&s.deepEntries, __ATOMIC_RELAXED);
        const uint64_t shallowEntries = __atomic_load_n(&s.shallowEntries, __ATOMIC_RELAXED);
        const uint64_t now = clock();
        __atomic_thread_fence(__ATOMIC_ACQUIRE);
        if (seq != __atomic_load_n(&s.seq, __ATOMIC_RELAXED)) continue;
        if (stamp > now || inIdle > 1 || deepTicks > UINT64_MAX - shallowTicks ||
            deepEntries > UINT64_MAX - shallowEntries)
            return IdleSnapshot {};
        const uint64_t closed = deepTicks + shallowTicks;
        const uint64_t open = inIdle ? now - stamp : 0;
        if (closed > UINT64_MAX - open) return IdleSnapshot {};
        return IdleSnapshot { closed + open, deepEntries + shallowEntries, now, true };
    }
    // No closed-only fallback: after a previously observed open interval it can
    // regress, then overcount the following window. Unknown is explicitly invalid.
    return IdleSnapshot {};
}

} // namespace zen
#endif // ZEN_IDLE_ACCOUNTING_HPP
