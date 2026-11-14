//
// ZenIdle.cpp — the pmDispatch table, MachineIdle and the ACPI _CST reader.
// Decision logic (gates, port choice, predictor) is in ZenIdleModel.cpp.
//
// XNU contract (docs/research/xnu-pm.md §2-§4, verified against the host kernel):
//   - pmKextRegister(102, table, callbacks) stores the table pointer (no copy),
//     fills the callbacks and panics on a second registration.
//   - machine_idle() calls MachineIdle(0x7FFFFFFFFFFFFFFF) with IF=0, ignores the
//     return value and does `sti` itself afterwards; MachineIdle must return
//     with IF=0. The kernel reloads pmDispatch->MachineIdle on every idle.
//   - exitIdle NULL => scheduler wake-ups always send an IPI, which wakes HLT,
//     MWAIT (ECX[0]=1) and the IO C-state alike.
//   - pmCPUHalt/exitHalt NULL keep the kernel's own offline/INIT-SIPI path;
//     pmIPIHandler NULL because a handler can never be removed again.
//
// Private kernel layout (x86_lcpu_t, rtcDeadline) is deliberately not used: the
// idle length is predicted from our own TSC measurements instead.
//
#include "ZenIdle.hpp"
#include "ZenRegs.h"
#include "XnuPrivate.h"
#include "AMDZenShared.h"

#include <IOKit/IOLib.h>
#include <IOKit/IOService.h>
#include <IOKit/acpi/IOACPIPlatformDevice.h>
#include <libkern/OSKextLib.h>
#include <libkern/c++/OSArray.h>
#include <libkern/c++/OSData.h>
#include <libkern/c++/OSNumber.h>
#include <string.h>

#define ILOG(fmt, ...) IOLog("AMDZenPM: " fmt "\n", ##__VA_ARGS__)

namespace zen {
namespace idle {
namespace {

// ---------------------------------------------------------------- XNU ABI
// osfmk/i386/pmCPU.h, PM_DISPATCH_VERSION 102: identical for xnu-6153 .. 12377
// (Darwin 19..25), offsets checked in the host kernel's disassembly. Pointer
// arguments to kernel-private types are `void *`: we never dereference them.
struct PmDispatch {
    kern_return_t (*pmCPUStateInit)(void);                                   // 0x00
    void          (*cstateInit)(void);                                       // 0x08
    uint64_t      (*MachineIdle)(uint64_t maxIdleDuration);                  // 0x10
    uint64_t      (*GetDeadline)(void *lcpu);                                // 0x18
    uint64_t      (*SetDeadline)(void *lcpu, uint64_t);                      // 0x20
    void          (*Deadline)(void *lcpu);                                   // 0x28
    boolean_t     (*exitIdle)(void *lcpu);                                   // 0x30
    void          (*markCPURunning)(void *lcpu);                             // 0x38
    int           (*pmCPUControl)(uint32_t cmd, void *datap);                // 0x40
    void          (*pmCPUHalt)(void);                                        // 0x48
    uint64_t      (*getMaxSnoop)(void);                                      // 0x50
    void          (*setMaxBusDelay)(uint64_t time);                          // 0x58
    uint64_t      (*getMaxBusDelay)(void);                                   // 0x60
    void          (*setMaxIntDelay)(uint64_t time);                          // 0x68
    uint64_t      (*getMaxIntDelay)(void);                                   // 0x70
    void          (*pmCPUSafeMode)(void *lcpu, uint32_t flags);              // 0x78
    void          (*pmTimerStateSave)(void);                                 // 0x80
    void          (*pmTimerStateRestore)(void);                              // 0x88
    kern_return_t (*exitHalt)(void *lcpu);                                   // 0x90
    kern_return_t (*exitHaltToOff)(void *lcpu);                              // 0x98
    void          (*markAllCPUsOff)(void);                                   // 0xA0
    void          (*pmSetRunCount)(uint32_t count);                          // 0xA8
    boolean_t     (*pmIsCPUUnAvailable)(void *lcpu);                         // 0xB0
    int           (*pmChooseCPU)(int startCPU, int endCPU, int preferredCPU);// 0xB8
    int           (*pmIPIHandler)(void *state);                              // 0xC0
    void          (*pmThreadTellUrgency)(int urgency, uint64_t rt_period, uint64_t rt_deadline); // 0xC8
    void          (*pmActiveRTThreads)(boolean_t active);                    // 0xD0
    boolean_t     (*pmInterruptPrewakeApplicable)(void);                     // 0xD8
    void          (*pmThreadGoingOffCore)(void *old_thread, boolean_t transfer_load,
                                          uint64_t last_dispatch, boolean_t thread_runnable); // 0xE0
};
static_assert(sizeof(PmDispatch) == 0xE8, "pmDispatch_t is 29 pointers");
static_assert(__builtin_offsetof(PmDispatch, MachineIdle) == 0x10, "MachineIdle is slot 2");
static_assert(__builtin_offsetof(PmDispatch, exitIdle) == 0x30, "exitIdle is slot 6");
static_assert(__builtin_offsetof(PmDispatch, pmCPUHalt) == 0x48, "pmCPUHalt is slot 9");
static_assert(__builtin_offsetof(PmDispatch, exitHalt) == 0x90, "exitHalt is slot 18");
static_assert(__builtin_offsetof(PmDispatch, pmIPIHandler) == 0xC0, "pmIPIHandler is slot 24");
static_assert(__builtin_offsetof(PmDispatch, pmThreadGoingOffCore) == 0xE0, "last slot");

// pmCallBacks_t: 27 slots written by the kernel. Only initComplete is used.
struct PmCallbacks {
    void *setRTCPop;                 // 0x00
    void *resyncDeadlines;           // 0x08
    void (*initComplete)(void);      // 0x10: sets pmInitDone = 1
    void *other[24];                 // 0x18 .. 0xD0
};
static_assert(sizeof(PmCallbacks) == 0xD8, "pmCallBacks_t is 27 pointers");
static_assert(__builtin_offsetof(PmCallbacks, initComplete) == 0x10, "initComplete is slot 2");

// The kernel writes every slot it knows; a larger zeroed buffer absorbs a newer
// kernel that appended slots without bumping the version (the ABI has no size).
union PmCallbackBuffer {
    PmCallbacks cb;
    uint8_t     raw[0x200];
};

// ---------------------------------------------------------------- state
// Per-CPU predictor + counters, one cache line each so CPUs never share a line
// they write on every idle exit. Written only by the owning CPU (inside
// machineIdle), read by telemetry with relaxed atomic loads.
struct alignas(64) CpuState {
    uint64_t ewmaTicks;
    uint64_t deepEntries;
    uint64_t shallowEntries;
    uint64_t deepTicks;
    uint64_t shallowTicks;
    uint64_t since;          // TSC at entry of the open idle period (valid while gen is odd)
    uint64_t gen;            // seqcount: odd inside the hook, even outside
    uint64_t pad[1];
};
static_assert(sizeof(CpuState) == 64, "one cache line per CPU");

// Per-CPU MONITOR target. Nothing ever writes it: MWAIT ends on interrupts only
// (ECX[0]=1), and a separate line keeps the stats stores above from arming it.
struct alignas(64) MonitorLine {
    uint8_t bytes[64];
};

// All zero-initialised BSS (no static constructors). The table and callbacks
// must stay mapped for the rest of the boot (the kernel keeps the pointer).
PmDispatch       gTable;
PmCallbackBuffer gCallbacks;
CpuState         gCpu[AMDZEN_MAX_CPUS];
MonitorLine      gMonitor[AMDZEN_MAX_CPUS];

bool     gClaimed;          // registration attempted (atomic exchange: once per boot)
bool     gRegistered;       // pmKextRegister returned
bool     gInitDone;         // initComplete ran: the kernel calls MachineIdle
uint32_t gMode;             // IdleMode in effect (read every idle)
uint32_t gMaxMode;
uint32_t gPort;             // CStateAddr + k (only read when gMode == kIdleCc6)
uint32_t gMwait;            // 1 = MONITOR/MWAIT usable
uint64_t gThreshold;        // TSC ticks
uint64_t gClamp;            // predictor sample clamp (TSC ticks)
uint32_t gNcpu;             // CPUs that must be deep for the all-cores-deep window
uint64_t gDeepLatencyNs;    // CC6 exit latency (ns)
uint64_t gMaxIntDelay;      // latest ml_set_maxintdelay (ns, 0 = none)
uint64_t gMaxBusDelay;      // latest ml_set_maxbusdelay (ns, 0 = none)

// All-cores-deep window. Shared by every CPU: touched only on CC6 entry/exit
// (two atomic RMWs), never on the C1 path. Separate line from the per-CPU state.
struct alignas(64) AllDeep {
    uint32_t count;          // CPUs currently inside ioIdle()
    uint32_t pad0;
    uint64_t start;          // TSC when count reached gNcpu
    uint64_t ticks;
    uint64_t events;
    uint64_t vetoes;         // latency requests that kept a CPU out of CC6
};
AllDeep gAll;

// ---------------------------------------------------------------- idle entry
// All three primitives are entered and left with IF=0.

// STI's one-instruction interrupt shadow makes `sti; hlt` atomic: an interrupt
// that is already pending is taken after HLT starts and wakes it, so no wake-up
// can be lost. The handler runs here; CLI restores the IF=0 contract.
inline void haltIdle() {
    __asm__ volatile("sti; hlt; cli" ::: "memory");
}

// MWAIT C1 (hint EAX=0, the only sub-state CPUID 5 EDX reports past C0) with
// ECX[0]=1: a pending or arriving interrupt ends MWAIT although IF=0; it is then
// serviced when machine_idle() does `sti` after we return.
inline void mwaitIdle(const void *line) {
    __asm__ volatile("monitor" :: "a"(line), "c"(0u), "d"(0u) : "memory");
    __asm__ volatile("mwait" :: "a"(0u), "c"(1u) : "memory");
}

// IO-based C-state: a read of CStateAddr+k executes C-state action CCRk (CC6
// when CCRk_CC6EN). With IF=0 an interrupt still wakes the core and execution
// continues after the IN (BKDG 42301 §2.5.3.4), so no wake-up is lost. No
// PM-timer dummy read and no WBINVD: hardware flushes on CC6 entry
// (amd-cstate.md §0.4). The value read is always 0 and is discarded.
inline void ioIdle(uint16_t port) {
    uint8_t v;
    __asm__ volatile("inb %w1, %b0" : "=a"(v) : "Nd"(port) : "memory");
    (void)v;
}

// pmDispatch->MachineIdle. Hot path on every CPU: no locks, no allocation, no
// logging, no floating point. maxIdleDuration is always "infinite" (unused).
uint64_t machineIdle(uint64_t maxIdleDuration) {
    (void)maxIdleDuration;
    const int c = cpu_number();
    if (c < 0 || (uint32_t)c >= AMDZEN_MAX_CPUS) {
        haltIdle();
        return 0;
    }
    CpuState *st = &gCpu[c];
    const uint32_t mode = __atomic_load_n(&gMode, __ATOMIC_RELAXED);
    const uint64_t thr = __atomic_load_n(&gThreshold, __ATOMIC_RELAXED);
    const uint64_t ewma = st->ewmaTicks;
    bool deep = false;

    const uint64_t t0 = zen_rdtsc();
    // Open the idle period for the governor's lock-free reader (snapshot()).
    const uint64_t gen = st->gen;
    __atomic_store_n(&st->since, t0, __ATOMIC_RELAXED);
    __atomic_store_n(&st->gen, gen + 1, __ATOMIC_RELEASE);
    bool wantDeep = mode == kIdleCc6 && idlePredictDeep(ewma, thr);
    if (wantDeep && !idleLatencyAllowsDeep(__atomic_load_n(&gMaxIntDelay, __ATOMIC_RELAXED),
                                           __atomic_load_n(&gMaxBusDelay, __ATOMIC_RELAXED),
                                           __atomic_load_n(&gDeepLatencyNs, __ATOMIC_RELAXED))) {
        wantDeep = false;
        __atomic_fetch_add(&gAll.vetoes, 1, __ATOMIC_RELAXED);
    }
    if (wantDeep) {
        const uint32_t port = __atomic_load_n(&gPort, __ATOMIC_RELAXED);
        if (port) {
            const uint32_t n = __atomic_load_n(&gNcpu, __ATOMIC_RELAXED);
            if (__atomic_add_fetch(&gAll.count, 1, __ATOMIC_ACQ_REL) == n)
                __atomic_store_n(&gAll.start, zen_rdtsc(), __ATOMIC_RELAXED);
            ioIdle((uint16_t)port);
            const uint64_t t1 = zen_rdtsc();
            if (__atomic_fetch_sub(&gAll.count, 1, __ATOMIC_ACQ_REL) == n) {
                const uint64_t span = idleAllDeepSpan(t0, __atomic_load_n(&gAll.start, __ATOMIC_RELAXED), t1);
                if (span) {
                    __atomic_fetch_add(&gAll.ticks, span, __ATOMIC_RELAXED);
                    __atomic_fetch_add(&gAll.events, 1, __ATOMIC_RELAXED);
                }
            }
            deep = true;
        } else {
            haltIdle();
        }
    } else if (mode >= kIdleMwait && __atomic_load_n(&gMwait, __ATOMIC_RELAXED)) {
        mwaitIdle(&gMonitor[c]);
    } else {
        haltIdle();
    }
    const uint64_t dt = zen_rdtsc() - t0;

    // The measured length is the real sleep time whichever primitive ran (all of
    // them end on the next interrupt), so the predictor does not depend on the
    // choice it made. HLT includes the interrupt handler; that only errs long.
    st->ewmaTicks = idleEwmaUpdate(ewma, dt, __atomic_load_n(&gClamp, __ATOMIC_RELAXED));
    if (deep) {
        __atomic_store_n(&st->deepEntries, st->deepEntries + 1, __ATOMIC_RELAXED);
        __atomic_store_n(&st->deepTicks, st->deepTicks + dt, __ATOMIC_RELAXED);
    } else {
        __atomic_store_n(&st->shallowEntries, st->shallowEntries + 1, __ATOMIC_RELAXED);
        __atomic_store_n(&st->shallowTicks, st->shallowTicks + dt, __ATOMIC_RELAXED);
    }
    __atomic_store_n(&st->gen, gen + 2, __ATOMIC_RELEASE);   // period closed (after the ticks)
    return 0;
}

// Latency requests. Not gated by pmInitDone in the kernel: live from pmKextRegister
// on. Store-only (the idle path reads them); getters return the stored values.
void setMaxIntDelay(uint64_t ns) { __atomic_store_n(&gMaxIntDelay, ns, __ATOMIC_RELAXED); }
uint64_t getMaxIntDelay() { return __atomic_load_n(&gMaxIntDelay, __ATOMIC_RELAXED); }
void setMaxBusDelay(uint64_t ns) { __atomic_store_n(&gMaxBusDelay, ns, __ATOMIC_RELAXED); }
uint64_t getMaxBusDelay() { return __atomic_load_n(&gMaxBusDelay, __ATOMIC_RELAXED); }

void publishConfig(const HookConfig &cfg) {
    __atomic_store_n(&gNcpu, cfg.ncpu ? cfg.ncpu : 1u, __ATOMIC_RELAXED);
    __atomic_store_n(&gDeepLatencyNs, cfg.deepLatencyNs, __ATOMIC_RELAXED);
    __atomic_store_n(&gPort, (uint32_t)cfg.port, __ATOMIC_RELAXED);
    __atomic_store_n(&gMwait, cfg.mwait ? 1u : 0u, __ATOMIC_RELAXED);
    __atomic_store_n(&gMaxMode, cfg.maxMode, __ATOMIC_RELAXED);
    setThresholdTicks(cfg.thresholdTicks);
    setMode(cfg.mode);
    __atomic_thread_fence(__ATOMIC_SEQ_CST);
}

} // anonymous namespace

// ---------------------------------------------------------------- API
bool registered() { return __atomic_load_n(&gRegistered, __ATOMIC_ACQUIRE); }

bool registerHook(const HookConfig &cfg) {
    // Exactly once per boot: a second pmKextRegister with a table panics.
    if (__atomic_exchange_n(&gClaimed, true, __ATOMIC_SEQ_CST)) return false;

    publishConfig(cfg);
    // Table completely filled before the kernel can see it. Only MachineIdle and
    // the store-only latency get/set slots are set (xnu-pm.md §4, §9); every other
    // slot stays NULL.
    bzero(&gTable, sizeof(gTable));
    bzero(&gCallbacks, sizeof(gCallbacks));
    gTable.MachineIdle = &machineIdle;
    gTable.setMaxIntDelay = &setMaxIntDelay;
    gTable.getMaxIntDelay = &getMaxIntDelay;
    gTable.setMaxBusDelay = &setMaxBusDelay;
    gTable.getMaxBusDelay = &getMaxBusDelay;
    __atomic_thread_fence(__ATOMIC_SEQ_CST);

    // The kernel is about to point into our text and data: pin the kext first so
    // it can never be unloaded (the matching release is intentionally never made).
    // Without the pin a later unload would leave pmDispatch->MachineIdle dangling,
    // so do not register at all. gClaimed stays set: no retry this boot.
    OSReturn rr = OSKextRetainKextWithLoadTag(OSKextGetCurrentLoadTag());
    if (rr != kOSReturnSuccess) {
        ILOG("idle: OSKextRetainKextWithLoadTag failed (0x%x); hook not registered", rr);
        return false;
    }

    pmKextRegister(ZEN_PM_DISPATCH_VERSION, &gTable, &gCallbacks);
    __atomic_store_n(&gRegistered, true, __ATOMIC_RELEASE);

    // The kernel never calls initComplete itself; until it runs, pmInitDone = 0
    // and machine_idle() keeps its own HLT path.
    if (gCallbacks.cb.initComplete) {
        gCallbacks.cb.initComplete();
        __atomic_store_n(&gInitDone, true, __ATOMIC_RELEASE);
    } else ILOG("idle: kernel returned no initComplete callback; hook stays inactive");
    return true;
}

void rearm(const HookConfig &cfg) {
    if (!registered()) return;
    publishConfig(cfg);
    __atomic_store_n(&gTable.MachineIdle, &machineIdle, __ATOMIC_SEQ_CST);
}

void unhook() {
    if (!registered()) return;
    // CPUs already inside machineIdle() return through our (still mapped) code;
    // the next idle on every CPU takes the kernel's sti;hlt;cli.
    __atomic_store_n(&gTable.MachineIdle, (uint64_t (*)(uint64_t))nullptr, __ATOMIC_SEQ_CST);
    __atomic_store_n(&gMode, (uint32_t)kIdleOff, __ATOMIC_SEQ_CST);
}

uint32_t mode() { return __atomic_load_n(&gMode, __ATOMIC_RELAXED); }
uint32_t maxMode() { return __atomic_load_n(&gMaxMode, __ATOMIC_RELAXED); }
uint16_t port() { return (uint16_t)__atomic_load_n(&gPort, __ATOMIC_RELAXED); }

void setMode(uint32_t m) {
    const uint32_t mx = maxMode();
    if (m > mx) m = mx;
    __atomic_store_n(&gMode, m, __ATOMIC_RELEASE);
}

void setMaxMode(uint32_t m) {
    if (m >= kIdleModeCount) m = kIdleModeCount - 1;
    __atomic_store_n(&gMaxMode, m, __ATOMIC_SEQ_CST);
    if (mode() > m) __atomic_store_n(&gMode, m, __ATOMIC_SEQ_CST);
}

void setMwait(bool on) {
    // Clearing it makes every mode >= MWAIT take the HLT path on the next idle.
    __atomic_store_n(&gMwait, on ? 1u : 0u, __ATOMIC_SEQ_CST);
}

void setThresholdTicks(uint64_t t) {
    __atomic_store_n(&gClamp, idleClampTicks(t), __ATOMIC_RELAXED);
    __atomic_store_n(&gThreshold, t, __ATOMIC_RELAXED);
}

void latencyRequests(uint64_t *maxIntDelayNs, uint64_t *maxBusDelayNs) {
    if (maxIntDelayNs) *maxIntDelayNs = __atomic_load_n(&gMaxIntDelay, __ATOMIC_RELAXED);
    if (maxBusDelayNs) *maxBusDelayNs = __atomic_load_n(&gMaxBusDelay, __ATOMIC_RELAXED);
}

bool active() {
    return registered() && __atomic_load_n(&gInitDone, __ATOMIC_ACQUIRE) &&
           __atomic_load_n(&gTable.MachineIdle, __ATOMIC_ACQUIRE) != nullptr;
}

void snapshot(CpuIdleSnap *out, uint32_t ncpu, uint64_t now) {
    if (!out) return;
    if (ncpu > AMDZEN_MAX_CPUS) ncpu = AMDZEN_MAX_CPUS;
    for (uint32_t i = 0; i < ncpu; i++) {
        const CpuState &s = gCpu[i];
        uint64_t acc = 0, entries = 0;
        bool ok = false;
        // Seqlock read: retry while the CPU entered or left idle under us.
        for (int tries = 0; tries < 8 && !ok; tries++) {
            const uint64_t g1 = __atomic_load_n(&s.gen, __ATOMIC_ACQUIRE);
            const uint64_t since = __atomic_load_n(&s.since, __ATOMIC_RELAXED);
            const uint64_t closed = __atomic_load_n(&s.deepTicks, __ATOMIC_RELAXED) +
                                    __atomic_load_n(&s.shallowTicks, __ATOMIC_RELAXED);
            entries = __atomic_load_n(&s.deepEntries, __ATOMIC_RELAXED) +
                      __atomic_load_n(&s.shallowEntries, __ATOMIC_RELAXED);
            __atomic_thread_fence(__ATOMIC_ACQUIRE);
            const uint64_t g2 = __atomic_load_n(&s.gen, __ATOMIC_RELAXED);
            if (g1 != g2) continue;
            acc = idleAccumulated(closed, (g1 & 1) != 0, since, now);
            ok = true;
        }
        if (!ok) {
            // Still racing after 8 tries: count only the closed periods (errs busy).
            acc = __atomic_load_n(&s.deepTicks, __ATOMIC_RELAXED) +
                  __atomic_load_n(&s.shallowTicks, __ATOMIC_RELAXED);
        }
        out[i].idleAcc = acc;
        out[i].entries = entries;
    }
}

void totals(IdleTotals *out, uint32_t ncpu) {
    if (!out) return;
    bzero(out, sizeof(*out));
    if (ncpu > AMDZEN_MAX_CPUS) ncpu = AMDZEN_MAX_CPUS;
    for (uint32_t i = 0; i < ncpu; i++) {
        const CpuState &s = gCpu[i];
        out->deepTicks      += __atomic_load_n(&s.deepTicks, __ATOMIC_RELAXED);
        out->shallowTicks   += __atomic_load_n(&s.shallowTicks, __ATOMIC_RELAXED);
        out->deepEntries    += __atomic_load_n(&s.deepEntries, __ATOMIC_RELAXED);
        out->shallowEntries += __atomic_load_n(&s.shallowEntries, __ATOMIC_RELAXED);
    }
    out->allDeepTicks = __atomic_load_n(&gAll.ticks, __ATOMIC_RELAXED);
    out->allDeepEvents = __atomic_load_n(&gAll.events, __ATOMIC_RELAXED);
    out->latencyVetoes = __atomic_load_n(&gAll.vetoes, __ATOMIC_RELAXED);
}

// ---------------------------------------------------------------- ACPI _CST
namespace {

IOACPIPlatformDevice *findProcessor0() {
    // Preferred: the AppleACPICPU that registered logical CPU 0; its provider is
    // the processor's ACPI device (C000 on AGESA boards).
    if (OSDictionary *m = IOService::serviceMatching("AppleACPICPU")) {
        IOACPIPlatformDevice *found = nullptr;
        if (OSIterator *it = IOService::getMatchingServices(m)) {
            while (OSObject *o = it->getNextObject()) {
                IOService *svc = OSDynamicCast(IOService, o);
                if (!svc) continue;
                OSNumber *n = OSDynamicCast(OSNumber, svc->getProperty("IOCPUNumber"));
                if (!n || n->unsigned32BitValue() != 0) continue;
                found = OSDynamicCast(IOACPIPlatformDevice, svc->getProvider());
                if (found) { found->retain(); break; }
            }
            it->release();
        }
        m->release();
        if (found) return found;
    }
    // Fallback (AppleACPICPU not published yet): an ACPI device named C000, else
    // the first device matching the "processor" name. Every core of a Zen part
    // reports the same _CST, so any processor object answers the question.
    IOACPIPlatformDevice *named = nullptr, *firstProc = nullptr;
    OSDictionary *m = IOService::serviceMatching("IOACPIPlatformDevice");
    OSString *procName = OSString::withCString("processor");
    OSIterator *it = m ? IOService::getMatchingServices(m) : nullptr;
    if (it && procName) {
        while (OSObject *o = it->getNextObject()) {
            IOACPIPlatformDevice *d = OSDynamicCast(IOACPIPlatformDevice, o);
            if (!d) continue;
            const char *nm = d->getName();
            if (nm && strcmp(nm, "C000") == 0) { named = d; break; }
            if (!firstProc && d->compareName(procName, nullptr)) firstProc = d;
        }
    }
    IOACPIPlatformDevice *pick = named ? named : firstProc;
    if (pick) pick->retain();          // before the iterator drops its references
    OSSafeReleaseNULL(it);
    OSSafeReleaseNULL(procName);
    OSSafeReleaseNULL(m);
    return pick;
}

uint64_t numberAt(OSArray *a, unsigned i, bool *ok) {
    OSNumber *n = OSDynamicCast(OSNumber, a->getObject(i));
    if (!n) { *ok = false; return 0; }
    return n->unsigned64BitValue();
}

} // anonymous namespace

bool readAcpiCst(CstEntry *out, uint32_t max, uint32_t *n, char *where, uint32_t whereLen) {
    if (n) *n = 0;
    if (where && whereLen) where[0] = '\0';
    if (!out || !n) return false;
    IOACPIPlatformDevice *dev = findProcessor0();
    if (!dev) return false;
    if (where && whereLen) strlcpy(where, dev->getName() ? dev->getName() : "?", whereLen);

    OSObject *res = nullptr;
    IOReturn r = dev->evaluateObject("_CST", &res);
    dev->release();
    if (r != kIOReturnSuccess || !res) {
        OSSafeReleaseNULL(res);
        return false;
    }
    // _CST = Package { Count, Package {Buffer(GAS), Type, Latency, Power}, ... }
    bool have = false;
    if (OSArray *pkg = OSDynamicCast(OSArray, res)) {
        have = true;
        for (unsigned i = 1; i < pkg->getCount() && *n < max; i++) {
            OSArray *e = OSDynamicCast(OSArray, pkg->getObject(i));
            if (!e || e->getCount() < 4) continue;
            OSData *reg = OSDynamicCast(OSData, e->getObject(0));
            if (!reg) continue;
            bool ok = true;
            uint64_t type = numberAt(e, 1, &ok), lat = numberAt(e, 2, &ok), pwr = numberAt(e, 3, &ok);
            if (!ok) continue;
            if (makeCstEntry(static_cast<const uint8_t *>(reg->getBytesNoCopy()), reg->getLength(),
                             type, lat, pwr, &out[*n]))
                (*n)++;
        }
    }
    res->release();
    return have;
}

} // namespace idle
} // namespace zen
