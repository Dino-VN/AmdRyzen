// Adapted from /private/tmp/amdzen-review.t6pfa1e0/AMDZenPM.hpp.
// These stubs run the ACTUAL AMDZenPMNative.cpp with real mutexes/conditions and
// the existing synthetic PCC Engine fixture, never kernel APIs or real MMIO.
#pragma once
#define AMDZEN_PM_HPP
#define ZEN_PCC_HPP
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <functional>
#include <memory>
#include <mutex>
#include "AMDZenShared.h"
#include "AMDZenNativeControl.hpp"
#include "ZenIdleModel.hpp"

inline std::atomic<unsigned> integrationChecks {0};
#define REQUIRE(e) do { ++integrationChecks; if (!(e)) { fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #e); abort(); } } while (0)
using IOReturn = int;
constexpr int kIOReturnSuccess=0, kIOReturnNotPermitted=1, kIOReturnNotReady=2, THREAD_UNINT=0;
inline thread_local bool inGate = false;
struct IOLock {
    std::mutex mutex;
    std::condition_variable cv;
    std::atomic<unsigned> waiters {0}, sleepers {0};
};
inline bool failLockAllocation=false;
inline IOLock *IOLockAlloc() { return failLockAllocation?nullptr:new IOLock; }
inline void IOLockFree(IOLock *l) { delete l; }
inline void IOLockLock(IOLock *l) { ++l->waiters; l->mutex.lock(); --l->waiters; }
inline void IOLockUnlock(IOLock *l) { l->mutex.unlock(); }
inline void IOLockWakeup(IOLock *l,void *,bool) { l->cv.notify_all(); }
inline void IOLockSleep(IOLock *l,void *,int) {
    REQUIRE(!inGate);
    std::unique_lock<std::mutex> guard(l->mutex,std::adopt_lock);
    ++l->sleepers;
    REQUIRE(l->cv.wait_for(guard,std::chrono::seconds(5)) != std::cv_status::timeout);
    --l->sleepers;
    guard.release();
}
using thread_call_param_t=void *;
struct Call { void (*fn)(void *,void *); void *owner; std::atomic<bool> pending {false}; };
using thread_call_t=Call *;
inline thread_call_t thread_call_allocate(void (*fn)(void *,void *),void *owner) { return new Call{fn,owner}; }
inline bool thread_call_enter(thread_call_t c) { return c->pending.exchange(true); }
inline bool thread_call_cancel(thread_call_t c) { return c->pending.exchange(false); }
inline bool thread_call_free(thread_call_t c) { delete c; return true; }
inline void execute(thread_call_t c) { if(c->pending.exchange(false)) c->fn(c->owner,nullptr); }
inline std::atomic<uint64_t> reviewNow {1000000000ull};
inline void clock_get_uptime(uint64_t *p) { *p=reviewNow.load(); }
inline void absolutetime_to_nanoseconds(uint64_t t,uint64_t *p) { *p=t; }
inline void IOLog(const char *,...) {}
struct OSObject { virtual ~OSObject()=default; };
struct IORegistryEntry {
    bool latched=false, failStore=false;
    static IORegistryEntry *getRegistryRoot();
    OSObject *getProperty(const char *) { static OSObject value; return latched?&value:nullptr; }
    bool setProperty(const char *,bool value) { if(failStore)return false; latched=value; return true; }
};
inline IORegistryEntry registryRoot;
inline IORegistryEntry *IORegistryEntry::getRegistryRoot() { return &registryRoot; }
constexpr int kOSReturnSuccess=0;
inline unsigned modulePins=0;
inline bool failPin=false;
inline unsigned OSKextGetCurrentLoadTag() {return 1;}
inline int OSKextRetainKextWithLoadTag(unsigned) {if(failPin)return 1; ++modulePins;return 0;}

class AMDZenPM;
using GateAction=IOReturn (*)(OSObject *,void *,void *,void *,void *);
struct Gate {
    AMDZenPM *owner;
    std::recursive_mutex mutex;
    std::function<void(AMDZenPM *)> afterPrepare;
    IOReturn runAction(GateAction,void *op=nullptr,void *a1=nullptr,void *a2=nullptr,void *a3=nullptr);
};
struct ReviewHardware {
    uint32_t count=2, legacyWrites=0, prepared=0, p0Writes=0, prefCoreCalls=0;
    bool prerequisitesOk=true, discoverOk=true, alias=true;
    bool enabled[64]{}, autonomy[64]{}, boostDisabled[64]{};
    uint64_t pstate[64]{};
    ReviewHardware() {for(unsigned c=0;c<64;++c){boostDisabled[c]=true;pstate[c]=2;}}
};
inline ReviewHardware hw;
struct TimerStub {
    uint32_t lastTimeoutMs=0;
    void setTimeoutMS(uint32_t ms){lastTimeoutMs=ms;}
};
namespace zen {
struct Topology {
    uint32_t ncpu, ncores;
    uint16_t primaryCpuOfCore[AMDZEN_MAX_CORES]{};
    uint16_t coreOfCpu[AMDZEN_MAX_CPUS]{};
};
struct CPU {
    Topology t {};
    bool bootDisabled[64]{};
    CPU() {
        t.ncpu = hw.count;
        t.ncores = (hw.count + 1) / 2;
        for (unsigned c = 0; c < t.ncpu; ++c) {
            bootDisabled[c] = hw.boostDisabled[c];
            t.coreOfCpu[c] = static_cast<uint16_t>(c / 2);
            if (c % 2 == 0) t.primaryCpuOfCore[c / 2] = static_cast<uint16_t>(c);
        }
    }
    const Topology &topo() const {return t;}
    bool bootBoostDisabled(uint32_t c) const {return bootDisabled[c];}
    uint64_t cppcReqInitial(uint32_t) const {return 0;}
};
inline uint32_t cppcPerfToFreq(uint32_t p,uint32_t n,uint32_t f) {return n?p*f/n:0;}
inline uint64_t encodeCppcReq(uint64_t prev,uint32_t maxP,uint32_t minP,uint32_t desP,uint32_t epp) {
    return (prev & 0xFFFFFFFF00000000ull) | (uint64_t)(maxP & 0xFF) |
           ((uint64_t)(minP & 0xFF) << 8) | ((uint64_t)(desP & 0xFF) << 16) |
           ((uint64_t)(epp & 0xFF) << 24);
}
namespace idle {
inline bool nativeSignalsActive(){return true;}
inline bool active(){return false;}
inline void snapshot(IdleSnapshot *,uint32_t){}
inline void setPreferredCores(const uint16_t *,const uint16_t *,const uint16_t *,uint32_t,uint32_t){++hw.prefCoreCalls;}
}
}
class AMDZenPM:public OSObject {
public:
    struct NativeDiagnostics {
        uint64_t commands=0,latencyUs=0,rtMask=0;
        uint32_t error=0,stage=0,cpuCount=0,acPower=0,thermal=0,lowPower=0,rtActive=0,hookActive=0;
        zencppc::Caps caps{};
        zencppc::Request requests[64]{};
    };
    struct NativeRuntime;
    NativeRuntime *native=nullptr;
    thread_call_t nativeCall=nullptr;
    NativeDiagnostics nativeDiag{};
    bool nativePending=true,nativeBusy=false,nativeStopped=false,nativeWake=false,sleeping=false,initialized=true;
    uint32_t nativeIntervalMs=50;
    int32_t lpmMode=-1, acPowerMode=-1, lastTdie_mC=0;
    uint32_t thermalCapMode=zennative::kThermalCapAuto;
    uint32_t profileRequested=zennative::kProfileAuto, profileActive=zennative::kProfileDesktop;
    uint32_t backend=kAMDZenBackendLegacy, governor=kAMDZenGovSchedutil,blockReason=0,epp=128,limitFast=0,limitSlow=2;
    int32_t boostMode=-1;
    bool reqDirty=false;
    uint64_t cppcReqStorage[64]{};
    uint64_t *cppcReq=cppcReqStorage;
    zen::CPU cpu;
    AMDZenInfo infoStorage{};
    AMDZenInfo *info=&infoStorage;
    Gate gateStorage {this, {}, {}};
    Gate *gate=&gateStorage;
    TimerStub timerStorage {};
    TimerStub *timer=&timerStorage;
    std::atomic<int> refs {1};
    AMDZenPM() {info->baseFreqMHz=3400;for(unsigned c=0;c<hw.count;++c){info->coreOfCpu[c]=c/2;info->apicId[c]=c;}}
    void retain(){++refs;} void release(){--refs;}
    void removeProperty(const char *){}
    void applyPolicy(bool) {if(!nativeEnableMayBe() && !sleeping && !blockReason && backend==kAMDZenBackendLegacy)++hw.legacyWrites;}
    bool nativePrepareHardware(const bool *enabled,const bool *boostOn,uint32_t n) {
        REQUIRE(inGate); ++hw.prepared;
        if(!hw.prerequisitesOk)return false;
        amdzen::PrerequisitePlan plans[64]{};
        for(unsigned c=0;c<n;++c)
            if(!amdzen::planPrerequisites(enabled[c],boostOn[c],hw.pstate[c],hw.boostDisabled[c]?HWCR_CPB_DIS:0,plans[c]))return false;
        for(unsigned c=0;c<n;++c){
            if(plans[c].ctl!=hw.pstate[c])++hw.p0Writes;
            hw.pstate[c]=plans[c].ctl; hw.boostDisabled[c]=(plans[c].hwcr&HWCR_CPB_DIS)!=0;
        }
        return true;
    }
    void nativeRestorePreparation() {if(!nativeEnableMayBe())for(unsigned c=0;c<hw.count;++c)hw.boostDisabled[c]=cpu.bootDisabled[c];}
    bool nativeInit(); void nativeFree(); void nativeSchedule(); void nativeDrain(); void nativeStop();
    void nativeCancel(bool permanent=false); void nativeResume(); void nativeDrainCommands(); void nativeNudge();
    bool nativeStepCppcMsr(); void nativeResetMsr(); bool nativeLegacyPowerSave();
    static bool nativeEnableMayBe(); static bool nativeRecordEnable();
    static void nativeThread(void *,void *);
    static IOReturn nativeGateAction(OSObject *,void *,void *,void *,void *);
    IOReturn copyNativeDiagnostics(NativeDiagnostics *);
};
inline IOReturn Gate::runAction(GateAction fn,void *op,void *a1,void *a2,void *a3) {
    std::function<void(AMDZenPM *)> hook;
    IOReturn result;
    {
        std::lock_guard<std::recursive_mutex> guard(mutex);
        const bool was=inGate;inGate=true;
        result=fn(owner,op,a1,a2,a3);
        inGate=was;
        if(result==0 && uintptr_t(op)==1 && afterPrepare){hook=std::move(afterPrepare);afterPrepare={};}
    }
    if(hook)hook(owner); // same race point as the review: permission gate released
    return result;
}

namespace zenpcc {
inline std::unique_ptr<SubspaceClaim> reviewClaim=std::make_unique<SubspaceClaim>();
enum class CommandKind {Caps,State,Enable,Apply};
inline std::function<void(CommandKind,uint32_t)> beforeCommand, afterCommand;
class Channel {
    bool claimed=false;
    uint32_t error=0;
public:
    std::vector<CpuRegisters> maps;
    std::unique_ptr<Fake> firmware;
    Engine engine;
    ~Channel(){teardown();}
    bool discover(const uint16_t *,uint32_t n,uint32_t *e) {
        REQUIRE(!inGate);
        if(!hw.discoverOk){error=uint32_t(Error::Unsupported);*e=error;return false;}
        error=uint32_t(reviewClaim->acquire());*e=error;claimed=error==0;
        if(!claimed)return false;
        maps.resize(n);
        for(unsigned c=0;c<n;++c)maps[c]=cpuMap(c,n);
        if(hw.alias)for(unsigned c=1;c<n;c+=2){
            maps[c].fields[Minimum].offset=maps[c-1].fields[Minimum].offset;
            maps[c].fields[Maximum].offset=maps[c-1].fields[Maximum].offset;
        }
        REQUIRE(validateCpuMaps(maps.data(),n)==Error::Ok);
        firmware=std::make_unique<Fake>(maps.data(),n);
        for(unsigned c=0;c<n;++c){
            firmware->seed(c,Enable,hw.enabled[c]);firmware->seed(c,AutonomousSelection,hw.autonomy[c]);
            if(c%2){firmware->seed(c,Highest,150);firmware->seed(c,Lowest,40);}
        }
        initialize(engine,*firmware,maps.data(),n);
        return true;
    }
    void before(CommandKind kind,uint32_t cpu=0){REQUIRE(!inGate);if(beforeCommand)beforeCommand(kind,cpu);}
    void after(CommandKind kind,uint32_t cpu=0){if(afterCommand)afterCommand(kind,cpu);}
    bool readCaps(uint32_t c,zencppc::Caps *out){before(CommandKind::Caps,c);bool ok=engine.readCaps(c,out);after(CommandKind::Caps,c);return ok;}
    bool readState(uint32_t c,uint32_t *e,uint32_t *a,zencppc::Request *r){
        before(CommandKind::State,c);
        firmware->seed(c,Enable,hw.enabled[c]);firmware->seed(c,AutonomousSelection,hw.autonomy[c]);
        bool ok=engine.readState(c,e,a,r);after(CommandKind::State,c);return ok;
    }
    bool enableAutonomous(const zencppc::Request *r,uint32_t n){
        before(CommandKind::Enable);
        REQUIRE(AMDZenPM::nativeEnableMayBe() && registryRoot.latched && modulePins==1);
        bool ok=engine.enableAutonomous(r,n);
        if(ok)for(unsigned c=0;c<n;++c)hw.enabled[c]=hw.autonomy[c]=true;
        after(CommandKind::Enable);return ok;
    }
    bool apply(const zencppc::Request *r,const bool *dirty,uint32_t n){before(CommandKind::Apply);bool ok=engine.apply(r,dirty,n);after(CommandKind::Apply);return ok;}
    void teardown(){if(claimed){reviewClaim->retire();claimed=false;}}
    uint32_t lastError()const{return error?error:uint32_t(engine.lastError());}
    uint64_t commandCount()const{return engine.commandCount();}
    uint64_t lastLatencyUs()const{return engine.lastLatencyUs();}
};
}
