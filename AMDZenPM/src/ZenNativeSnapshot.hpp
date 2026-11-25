// Scheduler publication shared by the kernel collector and host tests.
#ifndef ZEN_NATIVE_SNAPSHOT_HPP
#define ZEN_NATIVE_SNAPSHOT_HPP
#include <stdint.h>

namespace zennative {

// osfmk/kern/sched_urgency.h, xnu-12377.121.6. LOWPRI is explicitly
// distinguished from the BACKGROUND/min-power hint by thread_get_urgency().
enum Urgency : uint32_t {
    kUrgencyIdle = 0, kUrgencyBackground = 1, kUrgencyNormal = 2,
    kUrgencyRealtime = 3, kUrgencyLowPriority = 4, kUrgencyUnknown = 7
};
constexpr uint32_t kEventMask = 0x0fffffffu;

struct CpuSnapshot {
    Urgency urgency;
    uint32_t rtEvents;
    uint32_t foregroundEvents;
    bool observed;                  // false until the first callout, or when unhooked
    uint16_t busyPermille;          // 0..1000 from lock-free idle accounting (optional)
    bool busyObserved;              // true when busyPermille holds a valid window
};

// One owning CPU writes, with preemption disabled/interrupts off. No reader
// writes or resets this word once registered, even while the observer is off.
// Continuous recording prevents frozen classes from resurfacing on rearm.
// Bits 0..2 = class, bit 3 = observed, 8..35 = RT events, 36..63 = foreground events. Counter
// wrap is intentional; consumers compare for inequality, not signed deltas.
// One atomic word makes the class and both counters a coherent observation.
struct alignas(64) CpuPublication { uint64_t word; };
static_assert(sizeof(CpuPublication) == 64, "one writer per cache line");
static_assert(__atomic_always_lock_free(sizeof(uint64_t), nullptr), "64-bit atomics must be inline");

__attribute__((always_inline)) inline void recordUrgency(CpuPublication &p, int value) {
    const uint32_t u = value >= 0 && value <= 4 ? static_cast<uint32_t>(value) : kUrgencyUnknown;
    const uint64_t old = __atomic_load_n(&p.word, __ATOMIC_RELAXED);
    uint32_t rt = static_cast<uint32_t>(old >> 8) & kEventMask;
    uint32_t fg = static_cast<uint32_t>(old >> 36) & kEventMask;
    if (u == kUrgencyRealtime) rt = (rt + 1u) & kEventMask;
    if (u == kUrgencyNormal || u == kUrgencyLowPriority || u == kUrgencyUnknown)
        fg = (fg + 1u) & kEventMask;
    __atomic_store_n(&p.word, (static_cast<uint64_t>(fg) << 36) |
                     (static_cast<uint64_t>(rt) << 8) | 8u | u, __ATOMIC_RELAXED);
}

inline CpuSnapshot readCpuSnapshot(const CpuPublication &p, bool enabled = true) {
    if (!enabled) return {kUrgencyUnknown, 0, 0, false, 0, false};
    const uint64_t word = __atomic_load_n(&p.word, __ATOMIC_RELAXED);
    const bool observed = (word & 8u) != 0;
    const uint32_t raw = static_cast<uint32_t>(word & 7u);
    return {observed && raw <= 4 ? static_cast<Urgency>(raw) : kUrgencyUnknown,
            static_cast<uint32_t>(word >> 8) & kEventMask,
            static_cast<uint32_t>(word >> 36) & kEventMask, observed, 0, false};
}

} // namespace zennative
#endif
