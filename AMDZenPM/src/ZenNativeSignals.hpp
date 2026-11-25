// Kernel observer. Urgency is recorded by ZenIdle's PM table; properties are
// polled only from a serialized, preemptible thread call/work loop.
#ifndef ZEN_NATIVE_SIGNALS_HPP
#define ZEN_NATIVE_SIGNALS_HPP
#include "ZenNativePolicy.hpp"

class IOService;
class IOPMrootDomain;

namespace zennative {
extern CpuPublication gCpuUrgency[kMaxCpus]; // resident BSS, continuously recorded after registration

struct SignalState {
    IOPMrootDomain *root;
    IOService *platform, *powerSource;
    SystemSignals system;
    CpuSnapshot cpus[kMaxCpus];       // scratch storage avoids a large kernel stack
    uint64_t lastPollMs;
    bool initialized, polled;
};

// Zero-initialize state before its first use. All these functions require
// preemptible thread context and serialization with one another. No spinlocks.
// init initializes the observer; first snapshot retains registry services
// without waiting for their publication.
void initSignals(SignalState &state);
// Cancel/drain the caller's thread call before stop; idle::unhook disables
// snapshot visibility separately. The resident scheduler callout keeps recording.
// stop releases only the observer's references.
void stopSignals(SignalState &state);
// Coherent per-CPU word loads; returns unobserved snapshots when the hook is off.
// Safe in thread context on any CPU; ncpu is bounded to kMaxCpus.
void snapshotCpus(CpuSnapshot *out, uint32_t ncpu);
// Registry polling is limited to once per 250 ms, including late service
// discovery; CPU snapshots are taken on every call. No setting is changed.
void snapshotSignals(SignalState &state, uint64_t nowMs, uint32_t ncpu);
// Integrator calls every 25..50 ms from ONE thread call (50 ms recommended).
// Returns policy changes only; caller coalesces requests into PCC batches.
bool tickSignals(SignalState &signals, PolicyState &policy, const Topology &topology,
                 const zencppc::Caps *caps, const Tunables &tunables,
                 uint64_t nowMs, TickResult &result);
} // namespace zennative
#endif
