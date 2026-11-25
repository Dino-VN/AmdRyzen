// PCC waits and registry polling require preemptible thread context. The gate
// snapshots/publishes state and runs short MSR prerequisites; a separate sleeping
// command lock protects revocable admission through ONE Channel call at a time.
#include "AMDZenPM.hpp"
#include "ZenPcc.hpp"
#include "ZenNativeSignals.hpp"
#include "AMDZenNativeControl.hpp"
#include <kern/clock.h>
#include <kern/sched_prim.h>
#include <libkern/libkern.h>
#include <libkern/OSKextLib.h>

namespace {
enum : uintptr_t { kPrepare = 1, kPublish, kCopy, kDrained };
enum : uintptr_t { kEnablePrerequisites = 1, kApplyPrerequisites, kNeutralPermission, kStopNeutral };
enum : uint32_t { kIdle = 0, kStarting, kActive, kDegraded, kFallback, kStopped };
enum : uint32_t { kPolicyError = 0x10000, kConfirmationError, kInterrupted,
                  kPrerequisiteError, kLifetimeError };
// Never clear these. Registry-root ownership survives service replacement and
// module unload; the permanent module retain additionally preserves the BSS and
// Channel's one-claim guard even with the idle hook disabled.
bool gNativeEnableMayBe;
bool gNativePinned;
constexpr const char *kEnableLatch = "AMDZenPM.CPPCEnableMayHaveBeenSent";
uint64_t nowNs() {
    uint64_t abs = 0, ns = 0;
    clock_get_uptime(&abs);
    absolutetime_to_nanoseconds(abs, &ns);
    return ns;
}
uint32_t minU(uint32_t a, uint32_t b) { return a < b ? a : b; }
}

bool amdzen::pccEnableMayBeThisBoot() {
    return __atomic_load_n(&gNativeEnableMayBe, __ATOMIC_ACQUIRE);
}
bool AMDZenPM::nativeEnableMayBe() { return amdzen::pccEnableMayBeThisBoot(); }

bool AMDZenPM::nativeRecordEnable() {
    // Conservative intent latch BEFORE calling enableAutonomous (even preflight
    // failure may now quarantine). No legacy writer can race the doorbell store.
    __atomic_store_n(&gNativeEnableMayBe, true, __ATOMIC_RELEASE);
    IORegistryEntry *root = IORegistryEntry::getRegistryRoot();
    if (!root || !root->setProperty(kEnableLatch, true)) return false;
    if (!__atomic_load_n(&gNativePinned, __ATOMIC_ACQUIRE)) {
        if (OSKextRetainKextWithLoadTag(OSKextGetCurrentLoadTag()) != kOSReturnSuccess) return false;
        __atomic_store_n(&gNativePinned, true, __ATOMIC_RELEASE); // intentionally never released
    }
    return true;
}

struct AMDZenPM::NativeRuntime {
    zenpcc::Channel channel;
    IOLock *completion = nullptr, *command = nullptr;
    amdzen::NativeAdmission admission {};
    bool outstanding = false;       // completion-lock owned, includes publication
    zennative::SignalState signals {};
    zennative::PolicyState policy {};
    zennative::Topology topology {};
    zennative::TickResult result {};
    zencppc::Caps caps[zenpcc::kMaxCpus] {};
    zencppc::Request applied[zenpcc::kMaxCpus] {}, requests[zenpcc::kMaxCpus] {};
    bool changed[zenpcc::kMaxCpus] {}, enabled[zenpcc::kMaxCpus] {};
    bool bootBoostDisabled[zenpcc::kMaxCpus] {}, boostOn[zenpcc::kMaxCpus] {};
    uint16_t apic[zenpcc::kMaxCpus] {};
    // Inputs are written under the gate only when no callback is reserved.
    uint32_t baseEpp = 128, fast = 0, slow = 0, gov = kAMDZenGovCppcActive;
    int32_t boost = -1, preparedBoost = -2, lpm = -1;
    bool boot = false, wake = false, writes = false;
    uint64_t nextTickNs = 0;
    // Worker-owned except prerequisite results, written during its gate action.
    zen::IdleSnapshot idlePrev[zenpcc::kMaxCpus] {}, idleCur[zenpcc::kMaxCpus] {};
    bool idleValid = false;
    uint32_t quietTicks = 0;
    bool enabledMayBe = false, ready = false, degraded = false, prepared = false;
    bool previousActive = true;
    uint64_t lastWriteMs = 0;
    bool haveWrite = false;
    uint32_t error = 0;

    uint32_t transportError() const {
        const uint32_t e = channel.lastError();
        return e ? e : kConfirmationError;
    }
    bool continuing() {
        if (admission.allowed()) return true;
        error = kInterrupted;
        return false;
    }
    bool beginCommand() {
        IOLockLock(command);
        if (continuing()) return true; // immediately before any staging/ringing
        IOLockUnlock(command);
        return false;
    }
    void endCommand() { IOLockUnlock(command); }
    bool readCaps(uint32_t c) {
        if (!beginCommand()) return false;
        const bool ok = channel.readCaps(c, &caps[c]);
        endCommand();
        if (!ok) error = transportError();
        else if (!zennative::validCaps(caps[c])) { error = kPolicyError; return false; }
        return ok && continuing();
    }
    bool readStates(bool confirm, bool &needsEnable) {
        needsEnable = false;
        for (uint32_t c = 0; c < topology.cpuCount; ++c) {
            if (!beginCommand()) return false;
            uint32_t enable = 0, autonomy = 0;
            zencppc::Request current {};
            const bool ok = channel.readState(c, &enable, &autonomy, &current);
            if (ok && enable) {
                enabledMayBe = true;
                if (!AMDZenPM::nativeRecordEnable()) { error = kLifetimeError; degraded = true; }
            }
            endCommand();
            if (!ok) { error = transportError(); return false; }
            enabled[c] = enable != 0; // per CPU, including partially enabled firmware
            if (degraded || !continuing()) return false;
            const bool different = enable != 1 || autonomy != 1 ||
                !zencppc::sameRequest(current, requests[c]);
            if (different && confirm) { error = kConfirmationError; return false; }
            needsEnable |= different;
        }
        return true;
    }
    bool enable() {
        if (!beginCommand()) return false;
        enabledMayBe = true;
        if (!AMDZenPM::nativeRecordEnable()) {
            error = kLifetimeError; degraded = true; endCommand(); return false;
        }
        const uint64_t before = channel.commandCount();
        const bool ok = channel.enableAutonomous(requests, topology.cpuCount);
        if (channel.commandCount() != before) { lastWriteMs = nowNs() / 1000000; haveWrite = true; }
        endCommand();
        if (!ok) { error = transportError(); return false; }
        bool unused = false;
        return readStates(true, unused); // cancellation checked before EACH confirmation READ
    }
    bool neutral() {
        if (amdzen::neutralRequests(topology, caps, requests)) return true;
        error = kPolicyError; degraded = true; return false;
    }
    bool policyRequests(bool balanced) {
        if (!continuing()) return false;
        const uint64_t nowMs = nowNs() / 1000000;
        zennative::Tunables t = zennative::defaultTunables();
        t.baseEpp = baseEpp;
        if (!balanced) {
            zennative::snapshotSignals(signals, nowMs, topology.cpuCount);
            if (zen::idle::active()) {
                zen::idle::snapshot(idleCur, topology.cpuCount);
                const bool resetIdle = !idleValid;
                idleValid = true;
                for (uint32_t c = 0; c < topology.cpuCount; ++c) {
                    const zen::IdleWindow w = zen::idleWindowAdvance(&idlePrev[c], idleCur[c], resetIdle);
                    if (w.status == zen::kIdleSampleReady) {
                        signals.cpus[c].busyPermille = static_cast<uint16_t>(w.busyPermille);
                        signals.cpus[c].busyObserved = true;
                    }
                }
            } else {
                idleValid = false;
            }
        }
        if (lpm == 0) signals.system.lowPowerMode = zennative::kOff;
        else if (lpm == 1) signals.system.lowPowerMode = zennative::kOn;
        const bool ok = zennative::tick(policy, topology, signals.cpus, signals.system,
                                        caps, t, nowMs, result);
        if (!ok) { error = kPolicyError; return false; }
        bool anyBusy = result.changedCount != 0;
        for (uint32_t c = 0; c < topology.cpuCount; ++c) {
            amdzen::DomainBounds b {};
            if (!zennative::cpuValid(result, c) ||
                !amdzen::domainBounds(topology, caps, bootBoostDisabled, boost, c, b)) {
                error = kPolicyError; return false;
            }
            zencppc::Request r = result.requests[c];
            if (!amdzen::restrictRequest(r, b, fast, slow)) { error = kPolicyError; return false; }
            boostOn[c] = b.boostOn;
            requests[c] = r;
            const auto &cs = signals.cpus[c];
            if ((cs.observed && (cs.urgency == zennative::kUrgencyRealtime ||
                                 cs.urgency == zennative::kUrgencyNormal)) ||
                (cs.busyObserved && cs.busyPermille >= 100)) {
                anyBusy = true;
            }
        }
        if (balanced || anyBusy) quietTicks = 0;
        else if (quietTicks < 100) ++quietTicks;
        return continuing();
    }
    bool apply(bool stopping = false) {
        bool any = false;
        for (uint32_t c = 0; c < topology.cpuCount; ++c) {
            changed[c] = stopping || !zencppc::sameRequest(applied[c], requests[c]);
            any |= changed[c];
        }
        const uint64_t nowMs = nowNs() / 1000000;
        if (!stopping && any && haveWrite && nowMs >= lastWriteMs && nowMs - lastWriteMs < 50) return true;
        if (!beginCommand()) return false;
        const uint64_t before = channel.commandCount();
        const bool ok = channel.apply(requests, changed, topology.cpuCount);
        endCommand();
        if (!ok) {
            error = transportError();
            zennative::invalidateRequests(policy);
            if (error != uint32_t(zenpcc::Error::RateLimited)) degraded = true;
            return false;
        }
        if (channel.commandCount() != before) { lastWriteMs = nowNs() / 1000000; haveWrite = true; }
        memcpy(applied, requests, sizeof(applied));
        return true;
    }
};

bool AMDZenPM::nativeInit() {
    // Read BEFORE allocations/backend choice: failure to allocate a new Channel
    // must not make an enabled CPU eligible for legacy fallback after reload.
    IORegistryEntry *root = IORegistryEntry::getRegistryRoot();
    if (root && root->getProperty(kEnableLatch))
        __atomic_store_n(&gNativeEnableMayBe, true, __ATOMIC_RELEASE);
    native = new NativeRuntime;
    if (!native) { nativeDiag.error = uint32_t(zenpcc::Error::NoMemory); return false; }
    native->enabledMayBe = nativeEnableMayBe();
    native->degraded = native->enabledMayBe; // this new instance owns no live Channel
    if (native->degraded) native->error = uint32_t(zenpcc::Error::SubspaceClaimed);
    native->completion = IOLockAlloc();
    native->command = IOLockAlloc();
    if (!native->completion || !native->command) {
        nativeDiag.error = uint32_t(zenpcc::Error::NoMemory); return false;
    }
    nativeCall = thread_call_allocate(&AMDZenPM::nativeThread, this);
    if (!nativeCall) { nativeDiag.error = uint32_t(zenpcc::Error::NoMemory); return false; }
    return true;
}

void AMDZenPM::nativeCancel(bool permanent) {
    if (!native) return;
    native->admission.pause();
    if (permanent) native->admission.revoke();
}
void AMDZenPM::nativeResume() {
    if (native) native->admission.resume(); // conflict/teardown revocation cannot be undone
}
void AMDZenPM::nativeDrainCommands() {
    if (!native || !native->command) return;
    IOLockLock(native->command);
    IOLockUnlock(native->command);
}
void AMDZenPM::nativeNudge() {
    nativeIntervalMs = 50;
    if (native && !nativeBusy) native->nextTickNs = 0;
    if (timer && !sleeping) timer->setTimeoutMS(10);
}

void AMDZenPM::nativeSchedule() {
    if (!nativeCall || nativeBusy || nativeStopped || sleeping || !initialized ||
        (!nativePending && backend != kAMDZenBackendCppcPcc)) return;
    NativeRuntime &r = *native;
    const uint64_t now = nowNs();
    if (!nativePending && !nativeWake && now < r.nextTickNs) return;
    IOLockLock(r.completion);
    if (r.outstanding) { IOLockUnlock(r.completion); return; }
    r.outstanding = true;
    IOLockUnlock(r.completion);
    r.nextTickNs = now + static_cast<uint64_t>(nativeIntervalMs) * 1000000ull;
    r.boot = nativePending;
    r.wake = nativeWake;
    nativeWake = false;
    r.writes = !blockReason && governor != kAMDZenGovNone;
    r.baseEpp = epp; r.boost = boostMode;
    r.lpm = __atomic_load_n(&lpmMode, __ATOMIC_RELAXED);
    r.fast = nativePending ? 0 : limitFast;
    r.slow = nativePending ? 0 : limitSlow;
    r.gov = governor;
    if (nativePending) {
        r.topology.cpuCount = minU(cpu.topo().ncpu, zenpcc::kMaxCpus);
        for (uint32_t c = 0; c < r.topology.cpuCount; ++c) {
            r.apic[c] = info->apicId[c];
            r.topology.domainOfCpu[c] = info->coreOfCpu[c];
            r.bootBoostDisabled[c] = cpu.bootBoostDisabled(c);
        }
        nativeDiag.stage = kStarting;
    }
    nativeBusy = true;
    retain();
    thread_call_enter(nativeCall);
}

void AMDZenPM::nativeThread(thread_call_param_t owner, thread_call_param_t) {
    AMDZenPM *self = static_cast<AMDZenPM *>(owner);
    NativeRuntime &r = *self->native;
    if (r.boot) {
        r.error = 0;
        bool ok = r.continuing();
        if (ok) {
            zennative::initSignals(r.signals);
            zennative::reset(r.policy);
            ok = r.channel.discover(r.apic, r.topology.cpuCount, &r.error);
        }
        if (ok) ok = r.continuing();
        for (uint32_t c = 0; ok && c < r.topology.cpuCount; ++c) ok = r.readCaps(c);
        bool unused = false;
        if (ok) ok = r.readStates(false, unused);
        if (ok) ok = r.policyRequests(true);
        if (ok && self->gate->runAction(nativeGateAction, (void *)kPrepare,
                                        (void *)kEnablePrerequisites) != kIOReturnSuccess) ok = false;
        if (ok) ok = r.enable();
        r.ready = ok;
        r.enabledMayBe |= AMDZenPM::nativeEnableMayBe();
        r.degraded |= !ok && r.enabledMayBe;
        if (ok) memcpy(r.applied, r.requests, sizeof(r.applied));
    } else if (r.wake && !r.degraded && r.continuing()) {
        r.error = 0;
        bool needsEnable = false;
        memcpy(r.requests, r.applied, sizeof(r.requests));
        bool ok = r.readStates(false, needsEnable);
        // Even when firmware kept enable/autonomy, revalidate P0 and HWCR.
        if (ok && self->gate->runAction(nativeGateAction, (void *)kPrepare,
                                        (void *)kEnablePrerequisites) != kIOReturnSuccess) ok = false;
        if (ok && needsEnable) ok = r.enable();
        zennative::reset(r.policy);
        r.idleValid = false;
        r.quietTicks = 0;
        r.signals.polled = false;
        if (r.continuing()) zennative::snapshotSignals(r.signals, nowNs() / 1000000, r.topology.cpuCount);
        r.degraded |= !ok;
    } else if ((r.ready || r.degraded) && r.continuing()) {
        if (r.degraded || !r.writes) {
            zennative::snapshotSignals(r.signals, nowNs() / 1000000, r.topology.cpuCount);
            if (!r.degraded && r.gov == kAMDZenGovNone && r.neutral()) {
                if (self->gate->runAction(nativeGateAction, (void *)kPrepare,
                                          (void *)kNeutralPermission) == kIOReturnSuccess) r.apply();
                r.previousActive = false;
                r.idleValid = false;
                r.quietTicks = 0;
            }
        } else {
            r.error = 0;
            if (!r.previousActive) { zennative::reset(r.policy); r.idleValid = false; r.quietTicks = 0; }
            r.previousActive = true;
            if (r.policyRequests(false)) {
                if (self->gate->runAction(nativeGateAction, (void *)kPrepare,
                                          (void *)kApplyPrerequisites) == kIOReturnSuccess) r.apply();
            } else if (r.admission.allowed()) r.degraded = true;
        }
    }
    self->gate->runAction(nativeGateAction, (void *)kPublish);
    IOLock *completion = r.completion;
    IOLockLock(completion);
    r.outstanding = false;
    IOLockWakeup(completion, &r.outstanding, false);
    IOLockUnlock(completion);
    self->release(); // no runtime accesses after completion
}

IOReturn AMDZenPM::nativeGateAction(OSObject *owner, void *op, void *a1, void *, void *) {
    AMDZenPM *self = static_cast<AMDZenPM *>(owner);
    if ((uintptr_t)op == kCopy) {
        *static_cast<NativeDiagnostics *>(a1) = self->nativeDiag;
        return kIOReturnSuccess;
    }
    if ((uintptr_t)op == kDrained) { self->nativeBusy = false; return kIOReturnSuccess; }
    NativeRuntime &r = *self->native;
    if ((uintptr_t)op == kPrepare) {
        const uintptr_t mode = (uintptr_t)a1;
        if (self->blockReason || !r.continuing() || r.degraded ||
            (mode != kStopNeutral && (self->sleeping || self->nativeStopped)) ||
            (mode == kEnablePrerequisites && r.boot && self->governor == kAMDZenGovNone)) {
            r.error = kInterrupted; return kIOReturnNotPermitted;
        }
        if (mode == kStopNeutral) return kIOReturnSuccess;
        if (mode == kEnablePrerequisites || r.preparedBoost != r.boost) {
            for (uint32_t c = 0; c < r.topology.cpuCount; ++c) {
                amdzen::DomainBounds b {};
                if (!amdzen::domainBounds(r.topology, r.caps, r.bootBoostDisabled, r.boost, c, b)) {
                    r.error = kPolicyError; r.degraded = true; return kIOReturnNotPermitted;
                }
                r.boostOn[c] = b.boostOn;
            }
            r.prepared = true; // restoration may partially succeed even on failure
            if (!self->nativePrepareHardware(r.enabled, r.boostOn, r.topology.cpuCount)) {
                r.error = kPrerequisiteError; r.degraded = true; return kIOReturnNotPermitted;
            }
            r.preparedBoost = r.boost;
        }
        return kIOReturnSuccess;
    }
    NativeDiagnostics &d = self->nativeDiag;
    const bool reportFailure = r.degraded && d.stage != kDegraded;
    d.commands = r.channel.commandCount(); d.latencyUs = r.channel.lastLatencyUs();
    d.error = r.error; d.cpuCount = r.topology.cpuCount; d.caps = r.caps[0];
    memcpy(d.requests, r.applied, sizeof(d.requests));
    d.acPower = r.signals.system.powerSource; d.thermal = r.signals.system.thermal;
    d.lowPower = r.signals.system.lowPowerMode;
    d.hookActive = zen::idle::nativeSignalsActive(); d.rtMask = 0; d.rtActive = 0;
    const uint64_t nowMs = nowNs() / 1000000;
    const uint32_t rtHoldMs = zennative::defaultTunables().rtHoldMs;
    for (uint32_t c = 0; c < d.cpuCount; ++c) {
        const zennative::CpuHistory &h = r.policy.cpu[c];
        if ((r.signals.cpus[c].observed && r.signals.cpus[c].urgency == zennative::kUrgencyRealtime) ||
            (h.hasRt && nowMs >= h.lastRtMs && nowMs - h.lastRtMs < rtHoldMs)) {
            d.rtMask |= 1ull << c; ++d.rtActive;
        }
    }
    if (r.boot) {
        self->nativePending = false;
        if (r.ready || r.enabledMayBe || r.degraded || AMDZenPM::nativeEnableMayBe()) {
            self->backend = kAMDZenBackendCppcPcc;
            self->governor = kAMDZenGovCppcActive;
            self->limitFast = self->limitSlow = 0;
            AMDZenInfo &in = *self->info;
            uint32_t maxHi = d.caps.highest, minHi = d.caps.highest;
            uint16_t coreRank[AMDZEN_MAX_CORES] {};
            for (uint32_t c = 0; c < d.cpuCount; ++c) {
                const uint32_t h = r.caps[c].highest;
                if (h > maxHi) maxHi = h;
                if (h && (minHi == 0 || h < minHi)) minHi = h;
                const uint16_t core = in.coreOfCpu[c];
                if (core < AMDZEN_MAX_CORES) {
                    in.cppcRank[core] = (uint8_t)minU(h, 255);
                    if (h > coreRank[core]) coreRank[core] = (uint16_t)minU(h, 0xFFFFu);
                }
            }
            in.cppcHighest = maxHi; in.cppcNominal = d.caps.nominal;
            in.cppcLowest = d.caps.lowest; in.cppcLowestNonlinear = d.caps.lowestNonlinear;
            in.cppcEffectiveHighest = maxHi;
            if (maxHi != minHi) in.features |= AMDZEN_F_PREFCORE;
            if (d.caps.nominalFreqMHz) in.baseFreqMHz = d.caps.nominalFreqMHz;
            if (in.cppcNominal) in.maxBoostFreqMHz = zen::cppcPerfToFreq(
                in.cppcEffectiveHighest, in.cppcNominal, in.baseFreqMHz);
            if (maxHi != minHi) {
                const zen::Topology &tp = self->cpu.topo();
                zen::idle::setPreferredCores(tp.primaryCpuOfCore, tp.coreOfCpu, coreRank,
                                             tp.ncpu, tp.ncores);
            }
        } else {
            if (r.prepared && !self->blockReason) self->nativeRestorePreparation();
            self->applyPolicy(true);
        }
    }
    if (r.degraded && !self->blockReason) self->blockReason = kAMDZenBlockPccFailed;
    d.stage = r.degraded ? kDegraded : (r.ready ? kActive : kFallback);
    self->nativeIntervalMs = (r.ready && !r.degraded && !r.boot && r.quietTicks >= 10 && d.rtActive == 0)
        ? 200u : 50u;
    self->info->backend = self->backend; self->info->controlBlockReason = self->blockReason;
    if (r.boot || reportFailure) {
        self->removeProperty("CPUInfo");
        IOLog("AMDZenPM: PCC %s, error %u, commands %llu, last latency %llu us\n",
              r.degraded ? "degraded (no writes until reboot)" : (r.ready ? "autonomous" : "fallback"),
              d.error, d.commands, d.latencyUs);
    }
    self->nativeBusy = false;
    return kIOReturnSuccess;
}

void AMDZenPM::nativeDrain() {
    nativeCancel();
    if (!nativeCall) return;
    NativeRuntime &r = *native;
    const bool cancelled = thread_call_cancel(nativeCall);
    IOLockLock(r.completion);
    if (cancelled) r.outstanding = false;
    // Cancellation is checked between EVERY command/phase. Only the admitted
    // command can remain, followed by short publication, never an entire sweep.
    while (r.outstanding) IOLockSleep(r.completion, &r.outstanding, THREAD_UNINT);
    IOLockUnlock(r.completion);
    if (gate) gate->runAction(nativeGateAction, (void *)kDrained);
    if (cancelled) release();
}

void AMDZenPM::nativeStop() {
    if (!native) return;
    NativeRuntime &r = *native;
    zennative::stopSignals(r.signals);
    // After drain there is no worker and enqueue remains disabled. Temporarily
    // reopen sleep cancellation for the final neutral command; permanent
    // conflict revocation still wins admission, including a concurrent handoff.
    r.admission.resume();
    if (r.enabledMayBe && r.ready && !r.degraded && r.neutral() && gate &&
        gate->runAction(nativeGateAction, (void *)kPrepare, (void *)kStopNeutral) == kIOReturnSuccess)
        r.apply(true); // best effort; never disable CPPC
    nativeCancel(true);
    r.channel.teardown();
    nativeDiag.stage = kStopped;
}

void AMDZenPM::nativeFree() {
    nativeCancel(true);
    nativeDrain();
    if (nativeCall) { thread_call_free(nativeCall); nativeCall = nullptr; }
    if (native && native->completion) IOLockFree(native->completion);
    if (native && native->command) IOLockFree(native->command);
    delete native; native = nullptr;
}

IOReturn AMDZenPM::copyNativeDiagnostics(NativeDiagnostics *out) {
    if (!out || !gate) return kIOReturnNotReady;
    return gate->runAction(nativeGateAction, (void *)kCopy, out);
}
