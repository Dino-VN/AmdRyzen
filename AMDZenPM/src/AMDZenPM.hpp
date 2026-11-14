//
// AMDZenPM.hpp — the IOService that owns the hardware, runs the in-kernel
// governor and publishes telemetry. Kernel-first: it governs autonomously from
// boot; userspace (sysctl / IOUserClient) is optional.
//
// Configuration priority: boot-args > IOKitPersonality properties > defaults.
//   boot-args:  -amdzenoff            do not start
//               -amdzenro             monitor only (never write control MSRs)
//               amdzen.gov=<name>     performance|powersave|schedutil|ondemand|
//                                     conservative|userspace|active|none
//               amdzen.boost=0|1      force Core Performance Boost off/on (default: keep)
//               amdzen.epp=0..255     CPPC EPP for "active"
//               amdzen.period=<ms>    governor period
//               amdzen.force=1        control even if a conflicting PM kext is loaded
//               amdzen.pstatewrite=1  allow P-state definition editing (user client)
//               amdzen.smn=1          read SMN sensors even when a conflicting kext is loaded
//                                     (the 0x60/0x64 index/data pair cannot be shared safely)
//               amdzen.idle=0|1|2|3|auto  CC6 idle hook: 0 = never register, 1 = HLT,
//                                     2 = MWAIT C1, 3 = CC6 (IO C-state) + MWAIT, auto
//                                     (default) = 3 when the CC6 capability checks pass,
//                                     otherwise do not register (ZenIdle.hpp)
//   personality keys: "Governor" (string), "Boost" (string keep|on|off),
//               "EPP" (number), "GovernorPeriodMs", "TelemetryPeriodMs",
//               "UpThreshold", "DownThreshold", "SamplingDownFactor",
//               "FreqStepPct", "UtilHeadroomPct", "UtilEwmaShift", "TjMax" (°C),
//               "MinPState"/"MaxPState" (legacy index limits, -1 = none),
//               "IdleMode" (string auto|0..3 or number 0..3), "IdleThresholdUs"
//
#ifndef AMDZEN_PM_HPP
#define AMDZEN_PM_HPP

#include <IOKit/IOService.h>
#include <IOKit/IOWorkLoop.h>
#include <IOKit/IOCommandGate.h>
#include <IOKit/IOTimerEventSource.h>
#include <IOKit/IOUserClient.h>
#include "AMDZenShared.h"
#include "ZenModel.hpp"
#include "ZenHW.hpp"
#include "ZenIdle.hpp"

class AMDZenPM : public IOService {
    OSDeclareDefaultStructors(AMDZenPM)
public:
    bool     init(OSDictionary *dict) override;
    void     free() override;
    IOService *probe(IOService *provider, SInt32 *score) override;
    bool     start(IOService *provider) override;
    void     stop(IOService *provider) override;
    IOReturn setProperties(OSObject *properties) override;
    IOReturn newUserClient(task_t owningTask, void *securityID, UInt32 type,
                           OSDictionary *properties, IOUserClient **handler) override;

    // ---- API used by user client / sysctl / C exports. All gated on the work loop.
    IOReturn copyInfo(AMDZenInfo *out);
    IOReturn copySample(AMDZenSample *out);
    IOReturn copyTunables(AMDZenTunables *out);
    IOReturn setTunables(const AMDZenTunables *in);
    IOReturn setGovernor(uint32_t gov);
    IOReturn setBoost(bool on);
    IOReturn setPerfLimits(uint32_t fast, uint32_t slow);
    IOReturn setEPP(uint32_t epp);
    IOReturn setUserPState(uint32_t v);
    IOReturn writePStateDef(uint32_t idx, uint32_t fid, uint32_t did, uint32_t vid);
    // Idle hook (kern.amdzen.idle_*). Mode is clamped to what the hardware supports;
    // kIOReturnUnsupported when the hook is not registered.
    IOReturn setIdleMode(uint32_t mode);
    IOReturn setIdleThresholdUs(uint32_t us);
    // mode = -1 when the hook is not registered.
    void     getIdleConfig(int32_t *mode, uint32_t *thresholdUs);
    // Governor knobs outside the AMDZenTunables ABI (kern.amdzen.fast_up_pct,
    // kern.amdzen.gov_idle_sampling). Reads are lock-free.
    enum GovKnob : uint32_t { kKnobFastUpPct = 0, kKnobGovIdleSampling = 1 };
    IOReturn setGovKnob(uint32_t which, uint32_t value);
    uint32_t getGovKnob(uint32_t which);

    // The single started instance, retained (caller must release()), or null.
    // Taken under the lock stop() uses to clear it, so a caller can never hold a
    // pointer to an instance that is being freed.
    static AMDZenPM *copyInstance();

private:
    // gated implementations (run inside commandGate->runAction)
    static IOReturn gatedAction(OSObject *owner, void *op, void *a1, void *a2, void *a3);
    IOReturn runGated(uint32_t op, void *a1 = nullptr, void *a2 = nullptr, void *a3 = nullptr);

    bool     tryInitHardware();           // deferred init on the work loop (CPUs/PCI ready)
    void     probeSmnSensors();
    void     loadConfig();                // boot-args + personality
    bool     detectConflicts();           // AMDRyzenCPUPowerManagement / SMCAMDProcessor etc.
    void     chooseBackend();
    void     applyPolicy(bool force);     // static governors, boost, limits, EPP
    void     restoreHardware();           // on stop/sleep: P0 / initial CPPC REQ / boot HWCR
    void     timerFired(IOTimerEventSource *src);
    void     governorTick(uint64_t nowNs);
    int      governorTickIdle();          // 1 decided, 0 primed, -1 unusable (see .cpp)
    void     governLegacyCores(const zen::PStateTable &t, const uint16_t *coreUtil,
                               const uint16_t *coreBusy);
    void     telemetryTick(uint64_t nowNs);
    void     publishProperties();
    void     buildPStateTable(zen::PStateTable *t);
    static IOReturn sleepWakeHandler(void *target, void *refCon, UInt32 messageType,
                                     IOService *provider, void *messageArgument, vm_size_t argSize);
    void     handleSleep();
    void     handleWake();
    void     registerSysctls();
    void     unregisterSysctls();
    void     setupIdle();                 // CC6 capability probe + gates + pmKextRegister
    bool     idlePmConflict(const char **which);
    void     idleSleep();
    void     idleWake();
    void     idleTelemetry(uint64_t nowNs);
    void     publishIdleInfo(OSDictionary *cpuInfo);
    void     publishIdleSensors(OSDictionary *sensors);

    IOWorkLoop         *workLoop {nullptr};
    IOCommandGate      *gate {nullptr};
    IOTimerEventSource *timer {nullptr};
    IONotifier         *sleepNotifier {nullptr};

    zen::SMN            smn;
    zen::CPU            cpu;
    zen::CpuModelInfo   model {};
    zen::GovTunables    tun {};

    AMDZenInfo         *info {nullptr};      // IOMalloc'd (large)
    AMDZenSample       *sample {nullptr};    // latest published sample
    zen::PerCpuRaw     *rawPrev {nullptr};   // previous fast sample per CPU
    zen::PerCpuRaw     *rawCur {nullptr};
    zen::PerCpuRaw     *rawTelemPrev {nullptr}; // counters at last telemetry tick
    zen::GovState      *govState {nullptr};  // per core
    zen::EnergyAccumulator  pkgEnergy {};
    zen::EnergyAccumulator *coreEnergy {nullptr}; // per core
    uint8_t            *pstateReq {nullptr}; // per CPU
    uint64_t           *cppcReq {nullptr};   // per CPU

    uint32_t governor {kAMDZenGovNone};
    uint32_t backend {kAMDZenBackendNone};
    uint32_t blockReason {kAMDZenBlockNone};
    int32_t  boostMode {-1};                 // -1 keep, 0 off, 1 on
    uint32_t epp {0x80};
    uint32_t limitFast {0}, limitSlow {7};   // legacy idx range / CPPC min..max perf
    uint32_t userValue {0};
    uint32_t governorPeriodMs {40};
    uint32_t telemetryPeriodMs {1000};
    uint32_t tjMax_mC {0};
    bool     allowPStateWrite {false};
    bool     forceControl {false};
    bool     sleeping {false};
    bool     initialized {false};         // tryInitHardware() completed
    bool     initFailed {false};          // unsupported CPU: stay passive
    bool     allowSmnShared {false};      // amdzen.smn=1: read SMN even with a conflicting kext
    uint64_t initStartNs {0};
    uint32_t sviTfn {0};                  // SMN SVI0_TFN at probe (diagnostics)
    uint32_t sviCoreProbeRaw {0}, sviSocProbeRaw {0};
    bool     rebaseEnergy {true};
    uint64_t lastTelemetryNs {0};
    uint64_t lastGovNs {0};
    uint64_t seq {0};

    // ---- governor sampling (timerFired). With the idle hook active the legacy
    // governor runs on the hook's per-CPU idle accounting and needs no IPI unless a
    // request changes; otherwise it samples APERF/MPERF on every CPU each tick.
    zen::idle::CpuIdleSnap *idleSnapPrev {nullptr}, *idleSnapCur {nullptr};
    uint64_t idleSnapTsc {0};
    bool     idleSnapValid {false};
    uint32_t govIdleSampling {1};         // GovIdleSampling / amdzen.govidle (atomic)
    bool     govFromIdle {false};         // the last tick decided from idle accounting
    bool     perfFresh {false};           // rawPrev holds the previous tick's sample
    bool     reqDirty {false};            // a governor request is not written yet
    uint64_t rendezvousCount {0}, rendezvousPrev {0}, rendezvousPrevNs {0};
    uint64_t rendezvousRate {0};          // CPU-wide IPI rounds per second

    // ---- CC6 idle hook (ZenIdle.cpp owns the dispatch table and per-CPU state)
    uint32_t idleRequest {zen::kIdleAuto};          // boot-arg / personality
    uint32_t idleThresholdUs {zen::kIdleThresholdDefaultUs};
    uint32_t idleReason {zen::kIdleNotEvaluated};   // IdleHookReason
    uint32_t idleSavedMode {0};                     // mode to restore after wake
    bool     idleSleepDemoted {false};
    bool     idleWakeLost {false};                  // CC6 capability changed across S3
    bool     idleMwait {false};
    bool     haveCst {false};
    uint32_t cstCount {0};
    char     cstDevice[8] {};
    zen::CstEntry       cst[zen::kMaxCstEntries] {};
    zen::IdleCaps       idleCaps {};
    zen::IdlePortChoice idlePort {};
    zen::IdleTotals     idlePrev {};
    uint64_t idlePrevTsc {0}, idlePrevNs {0};
    bool     idlePrevValid {false};
    uint32_t idleCc6Permille {0}, idleC1Permille {0};
    uint64_t idleDeepRate {0}, idleShallowRate {0};
    uint32_t idleAllDeepPermille {0};     // every CPU in CC6 at once
    uint64_t idleAllDeepRate {0}, idleVetoRate {0};
    uint64_t pmgtMisc {0};                // C001_0292 (PC6En = bit 32)
    bool     havePmgtMisc {false};
};

class AMDZenPMUserClient : public IOUserClient {
    OSDeclareDefaultStructors(AMDZenPMUserClient)
public:
    bool initWithTask(task_t owningTask, void *securityToken, UInt32 type,
                      OSDictionary *properties) override;
    bool start(IOService *provider) override;
    IOReturn clientClose() override;
    IOReturn externalMethod(uint32_t selector, IOExternalMethodArguments *args,
                            IOExternalMethodDispatch *dispatch, OSObject *target,
                            void *reference) override;
private:
    AMDZenPM *owner {nullptr};
    bool      admin {false};
};

#endif // AMDZEN_PM_HPP
