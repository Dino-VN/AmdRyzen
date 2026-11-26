//
// AMDZenSMC.hpp — VirtualSMC plugin that publishes AMDZenPM telemetry as SMC keys
// (CPU temperature, power, voltage) for iStat Menus, Stats, HWMonitorSMC2.
//
// A plain IOService (no Lilu plugin glue) that matches on the AMDZenPM service.
// Data flow (docs/research/smc-keys.md §6.1):
//   timer on our own IOWorkLoop, every 500 ms, thread context
//     -> AMDZenPM_copySample()  (gated inside AMDZenPM)
//     -> integer encoders (SmcEncode.hpp)
//     -> one atomic 32-bit cell per key
//   VirtualSMC read (possibly inside its kernel_trap hook, spinlock held, no xmm save)
//     -> AMDZenSMCValue::readAccess(): atomic load + byte stores, nothing else.
//
// Boot-arg: -amdzensmcoff  do not start (probe fails).
//
// Once submitted, VirtualSMC keeps a retain on this service and pointers to its
// values forever, so this kext refuses to unload (kmod stop returns KERN_FAILURE),
// and AMDZenPM cannot be unloaded while it is loaded.
//
#ifndef AMDZEN_SMC_HPP
#define AMDZEN_SMC_HPP

#include <IOKit/IOService.h>
#include <IOKit/IOWorkLoop.h>
#include <IOKit/IOTimerEventSource.h>
#include <IOKit/IOLocks.h>
#include <VirtualSMCSDK/kern_vsmcapi.hpp>

#include "AMDZenShared.h"
#include "SmcKeySet.hpp"

// One SMC key. The cell holds the SMC data bytes in memory order (SmcEncode.hpp).
class AMDZenSMCValue : public VirtualSMCValue {
public:
    void store(uint32_t cellValue) { __atomic_store_n(&cell, cellValue, __ATOMIC_RELAXED); }

protected:
    // Runs in VirtualSMC's read path: no locks, no allocation, no calls, no FP/SIMD.
    SMC_RESULT readAccess() override;

private:
    uint32_t cell {0};
};

class AMDZenSMC : public IOService {
    OSDeclareDefaultStructors(AMDZenSMC)
public:
    IOService *probe(IOService *provider, SInt32 *score) override;
    bool       start(IOService *provider) override;
    void       stop(IOService *provider) override;
    void       free() override;

private:
    enum Phase : uint32_t {
        kPhaseWaiting = 0,  // AMDZenPM not initialised yet: key set unknown
        kPhaseRunning,      // keys built, handler registered, refreshing cells
        kPhaseIdle,         // gave up (conflict, timeout, ABI mismatch): timer stopped
    };

    void        timerFired(IOTimerEventSource *sender);
    bool        fetchInfo();
    bool        fetchSample();
    const char *conflictingDriver();
    bool        buildAndRegister(bool haveSample);
    void        refresh();
    void        publishKeyList();
    void        clearCells();

    static bool vsmcNotification(void *target, void *refCon, IOService *vsmc, IONotifier *notifier);

    IOWorkLoop         *workLoop {nullptr};
    IOTimerEventSource *timer {nullptr};
    IONotifier         *vsmcNotifier {nullptr};

    // Over-sized copy buffers: the C API has no size argument, so a newer AMDZenPM
    // with larger structs would otherwise write past them (see AMDZenSMC.cpp).
    AMDZenInfo   *info {nullptr};
    AMDZenSample *sample {nullptr};

    uint32_t phase {kPhaseWaiting};
    uint32_t ticks {0};          // timer ticks spent waiting
    uint32_t infoTick {0};       // tick at which AMDZenInfo first arrived (0 = not yet)
    uint64_t lastSeq {0};
    bool     haveSeq {false};
    uint32_t failStreak {0};     // consecutive failed AMDZenPM_copySample calls
    uint32_t submitted {0};      // plugin accepted by VirtualSMC (written under submitLock)
    IOLock  *submitLock {nullptr};

    uint32_t        numKeys {0};
    smc::KeyDef     keys[smc::kMaxKeys] {};
    AMDZenSMCValue *values[smc::kMaxKeys] {};

    // Must stay valid and unchanged after submission; VirtualSMC keeps the pointer.
    VirtualSMCAPI::Plugin vsmcPlugin {
        "AMDZenSMC",
        parseModuleVersion("0.0.2"),
        VirtualSMCAPI::Version,
    };
};

#endif // AMDZEN_SMC_HPP
