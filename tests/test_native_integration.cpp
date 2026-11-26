// Review regressions adapted from review.cpp and neutral.cpp. The actual native
// worker/gate/admission/lifetime code runs against the existing PCC byte-buffer
// firmware fixture and host-only IOKit/thread-call stand-ins (ASan/UBSan).
#define main pcc_fixture_main
#include "test_pcc.cpp"
#undef main
#include "native_stubs/Kernel.hpp"
#include "ZenNativeSignals.hpp"
namespace zennative {
void initSignals(SignalState &s){s.initialized=true;}
void stopSignals(SignalState &s){s.initialized=false;}
void snapshotSignals(SignalState &s,uint64_t,uint32_t){s.polled=true;}
bool tickSignals(SignalState &s,PolicyState &p,const Topology &t,const zencppc::Caps *c,
                 const Tunables &tun,uint64_t now,TickResult &out){return tick(p,t,s.cpus,s.system,c,tun,now,out);}
}
#include "../AMDZenPM/src/AMDZenPMNative.cpp"

static void rebootFixture() {
    // Test-only reboot: NEVER done by production code. Preserve this state
    // within restart/reload scenarios to exercise the once-per-boot guards.
    hw=ReviewHardware{};registryRoot={};modulePins=0;failPin=false;failLockAllocation=false;
    gNativeEnableMayBe=false;gNativePinned=false;reviewNow=1000000000ull;
    zenpcc::reviewClaim=std::make_unique<SubspaceClaim>();
    beforeCommand={};afterCommand={};
}
struct Service:AMDZenPM {
    Service(){REQUIRE(nativeInit());}
    ~Service(){nativeFree();REQUIRE(refs==1);}
    void tick(){reviewNow+=300000000ull;nativeSchedule();execute(nativeCall);}
    void boot(){tick();REQUIRE(backend==kAMDZenBackendCppcPcc);REQUIRE(native->ready);REQUIRE(!native->degraded);}
    void stopNative(){sleeping=nativeStopped=true;nativeCancel();nativeDrain();nativeStop();}
    unsigned writes()const{return native->channel.firmware?native->channel.firmware->firmwareWrites:0;}
    uint64_t commands()const{return native->channel.commandCount();}
};
static void conflict(AMDZenPM &s) {
    // Same order as gatedAction/runGated: revoke and publish under the gate,
    // then drain admitted work outside it before the notification returns.
    {std::lock_guard<std::recursive_mutex> lock(s.gate->mutex);s.nativeCancel(true);s.blockReason=kAMDZenBlockConflict;}
    s.nativeDrainCommands();
}
static void replacement() {
    Service first;first.boot();first.stopNative();
    const unsigned old=hw.legacyWrites;
    Service next;next.tick();
    REQUIRE(hw.enabled[0] && hw.enabled[1]);REQUIRE(next.backend==kAMDZenBackendCppcPcc);
    REQUIRE(next.native->degraded && next.blockReason==kAMDZenBlockPccFailed);
    REQUIRE(next.nativeDiag.error==uint32_t(Error::SubspaceClaimed));
    REQUIRE(hw.legacyWrites==old);REQUIRE(registryRoot.latched && modulePins==1);
}
static void persistentReload() {
    {Service first;first.boot();first.stopNative();}
    // Simulate module BSS reset while the registry root remains owned by XNU.
    gNativeEnableMayBe=false;gNativePinned=false;modulePins=0;
    zenpcc::reviewClaim=std::make_unique<SubspaceClaim>();
    Service next;REQUIRE(AMDZenPM::nativeEnableMayBe());
    next.applyPolicy(true);REQUIRE(hw.legacyWrites==0);
    next.tick();REQUIRE(next.native->degraded);REQUIRE(next.writes()==0);
}
static void allocationFailureAfterReload() {
    {Service first;first.boot();first.stopNative();}
    gNativeEnableMayBe=false;failLockAllocation=true;
    AMDZenPM s;REQUIRE(!s.nativeInit());REQUIRE(AMDZenPM::nativeEnableMayBe());
    s.applyPolicy(true);REQUIRE(hw.legacyWrites==0);s.nativeFree();
}
static void preEnableFallback(){hw.discoverOk=false;Service s;s.tick();REQUIRE(s.backend==kAMDZenBackendLegacy);REQUIRE(hw.legacyWrites==1);REQUIRE(!AMDZenPM::nativeEnableMayBe());}
static void lateEnableConflict(){Service s;s.gate->afterPrepare=[](AMDZenPM *p){conflict(*p);};s.tick();REQUIRE(s.writes()==0);REQUIRE(hw.legacyWrites==0);REQUIRE(!AMDZenPM::nativeEnableMayBe());}
static void lateApplyConflict(){Service s;s.boot();unsigned before=s.writes();s.limitFast=100;s.gate->afterPrepare=[](AMDZenPM *p){conflict(*p);};s.tick();REQUIRE(s.writes()==before);s.nativeResume();s.tick();REQUIRE(s.writes()==before);}

struct Block {
    std::mutex mutex;std::condition_variable cv;bool entered=false,released=false;
    void command(){std::unique_lock<std::mutex> l(mutex);entered=true;cv.notify_all();REQUIRE(cv.wait_for(l,std::chrono::seconds(5),[&]{return released;}));}
    void wait(){std::unique_lock<std::mutex> l(mutex);REQUIRE(cv.wait_for(l,std::chrono::seconds(5),[&]{return entered;}));}
    void release(){std::lock_guard<std::mutex> l(mutex);released=true;cv.notify_all();}
};
template<class P> static void awaitPredicate(P predicate){
    const auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(5);
    while(!predicate() && std::chrono::steady_clock::now()<deadline)std::this_thread::yield();
    REQUIRE(predicate());
}
static void admittedConflict(bool enabling) {
    Service s;if(!enabling){s.boot();s.limitFast=100;}
    Block block;const auto kind=enabling?CommandKind::Enable:CommandKind::Apply;
    beforeCommand=[&](CommandKind k,uint32_t){if(k==kind)block.command();};
    reviewNow+=300000000ull;s.nativeSchedule();std::thread worker([&]{execute(s.nativeCall);});block.wait();
    {std::lock_guard<std::recursive_mutex> l(s.gate->mutex);s.nativeCancel(true);s.blockReason=kAMDZenBlockConflict;}
    std::atomic<bool> handedOff {false};
    std::thread owner([&]{s.nativeDrainCommands();handedOff=true;});
    awaitPredicate([&]{return s.native->command->waiters.load()!=0;});REQUIRE(!handedOff);
    block.release();owner.join();worker.join();const unsigned writes=s.writes();
    REQUIRE(handedOff);REQUIRE(writes==unsigned(enabling?1:2));
    beforeCommand={};s.nativeResume();s.tick();s.stopNative();REQUIRE(s.writes()==writes);
}
static void admittedEnableConflict(){admittedConflict(true);}
static void admittedApplyConflict(){admittedConflict(false);}
static void boostKeepOff(){Service s;s.boot();for(unsigned c=0;c<2;++c){REQUIRE(hw.boostDisabled[c]);REQUIRE(s.nativeDiag.requests[c].maxPerf==s.native->caps[c].nominal);}}
static void boostKeepOn(){hw.boostDisabled[0]=hw.boostDisabled[1]=false;Service s;s.boot();REQUIRE(!hw.boostDisabled[0] && !hw.boostDisabled[1]);REQUIRE(s.nativeDiag.requests[0].maxPerf==150);}
static void boostMixed(){hw.boostDisabled[0]=false;Service s;s.boot();REQUIRE(hw.boostDisabled[0] && hw.boostDisabled[1]);REQUIRE(s.nativeDiag.requests[0].maxPerf==136);REQUIRE(zencppc::sameRequest(s.nativeDiag.requests[0],s.nativeDiag.requests[1]));}
static void boostBackToKeepOff(){Service s;s.boot();s.boostMode=1;s.tick();REQUIRE(!hw.boostDisabled[0]);REQUIRE(s.nativeDiag.requests[0].maxPerf==150);s.boostMode=-1;s.tick();REQUIRE(hw.boostDisabled[0] && hw.boostDisabled[1]);REQUIRE(s.nativeDiag.requests[0].maxPerf==136);}
static void boostBackToKeepOn(){hw.boostDisabled[0]=hw.boostDisabled[1]=false;Service s;s.boot();s.boostMode=0;s.tick();REQUIRE(hw.boostDisabled[0]);REQUIRE(s.nativeDiag.requests[0].maxPerf==136);s.boostMode=-1;s.tick();REQUIRE(!hw.boostDisabled[0]);REQUIRE(s.nativeDiag.requests[0].maxPerf==150);}
static void boostKeepWhileNone(){Service s;s.boot();s.boostMode=1;s.tick();s.governor=kAMDZenGovNone;s.boostMode=-1;s.tick();REQUIRE(hw.boostDisabled[0] && hw.boostDisabled[1]);REQUIRE(s.nativeDiag.requests[0].minPerf==40);}
static void neutralAlias(){
    Service s;s.boot();s.limitFast=100;s.tick();REQUIRE(s.nativeDiag.requests[0].minPerf==100);
    s.limitFast=0;s.governor=kAMDZenGovNone;const auto before=s.commands();s.tick();
    REQUIRE(s.commands()==before+1);REQUIRE(s.native->channel.lastError()==0);
    REQUIRE(zencppc::sameRequest(s.nativeDiag.requests[0],s.nativeDiag.requests[1]));
    REQUIRE(s.nativeDiag.requests[0].minPerf==40 && s.nativeDiag.requests[0].maxPerf==150);
    REQUIRE(s.native->channel.firmware->value(0,Minimum)==40);
    const auto stopBefore=s.commands();s.stopNative();REQUIRE(s.commands()==stopBefore+1);
    REQUIRE(s.native->channel.firmware->value(1,Minimum)==40);
}
static void wakeReset(){
    hw.boostDisabled[0]=hw.boostDisabled[1]=false;Service s;s.boot();
    for(unsigned c=0;c<2;++c){hw.enabled[c]=hw.autonomy[c]=false;hw.pstate[c]=2;hw.boostDisabled[c]=true;}
    hw.prepared=hw.p0Writes=0;s.nativeWake=true;s.tick();
    REQUIRE(hw.prepared==1 && hw.p0Writes==2);REQUIRE(hw.enabled[0] && hw.enabled[1]);
    REQUIRE(hw.pstate[0]==0 && hw.pstate[1]==0);REQUIRE(!hw.boostDisabled[0] && !hw.boostDisabled[1]);REQUIRE(!s.native->degraded);
}
static void wakePartial(){Service s;s.boot();hw.enabled[1]=hw.autonomy[1]=false;hw.pstate[1]=2;hw.p0Writes=0;s.nativeWake=true;s.tick();REQUIRE(hw.p0Writes==1);REQUIRE(hw.enabled[0] && hw.enabled[1]);REQUIRE(!s.native->degraded);}
static void startupPartial(){hw.enabled[0]=hw.autonomy[0]=true;hw.pstate[0]=0;Service s;s.boot();REQUIRE(hw.p0Writes==1);REQUIRE(hw.prepared==1);REQUIRE(hw.enabled[0] && hw.enabled[1]);}
static void wakeInvalidEnabledP0(){Service s;s.boot();hw.pstate[1]=2;const auto writes=s.writes();s.nativeWake=true;s.tick();REQUIRE(s.native->degraded && s.nativeDiag.error==kPrerequisiteError);REQUIRE(s.writes()==writes && hw.pstate[1]==2);}
static void wakeValidateKeptEnable(){Service s;s.boot();hw.boostDisabled[0]=hw.boostDisabled[1]=false;const auto writes=s.writes();s.nativeWake=true;s.tick();REQUIRE(hw.boostDisabled[0] && hw.boostDisabled[1]);REQUIRE(s.writes()==writes);REQUIRE(!s.native->degraded);}
static void wakeNone(){Service s;s.boot();s.governor=kAMDZenGovNone;s.tick();for(unsigned c=0;c<2;++c){hw.enabled[c]=false;hw.pstate[c]=2;}s.nativeWake=true;s.tick();REQUIRE(!s.native->degraded);REQUIRE(hw.enabled[0] && hw.enabled[1]);REQUIRE(s.nativeDiag.requests[0].minPerf==40);}
static void prerequisitesFail(){hw.prerequisitesOk=false;Service s;s.tick();REQUIRE(s.native->degraded);REQUIRE(s.nativeDiag.error==kPrerequisiteError);REQUIRE(s.writes()==0 && hw.legacyWrites==0);REQUIRE(s.backend==kAMDZenBackendCppcPcc);}
static void queuedCancel(){Service s;s.nativeSchedule();REQUIRE(s.refs==2);s.sleeping=true;s.nativeCancel();s.nativeDrain();REQUIRE(s.refs==1);REQUIRE(s.commands()==0);REQUIRE(s.nativePending);s.sleeping=false;s.nativeResume();s.boot();}
static void cancelledSweep(CommandKind kind,bool wake,bool confirmation){
    hw.count=16;Service s;if(wake)s.boot();
    Block block;
    beforeCommand=[&](CommandKind k,uint32_t c){
        if(k==kind && c==0 && (!confirmation || hw.enabled[0]))block.command();
    };
    if(wake)s.nativeWake=true;
    const auto before=s.commands();reviewNow+=300000000ull;s.nativeSchedule();
    std::thread worker([&]{execute(s.nativeCall);});block.wait();
    {std::lock_guard<std::recursive_mutex> l(s.gate->mutex);s.sleeping=true;s.nativeCancel();}
    std::atomic<bool> acknowledged {false};
    std::thread power([&]{s.nativeDrain();acknowledged=true;});
    awaitPredicate([&]{return s.native->completion->sleepers.load()!=0;});REQUIRE(!acknowledged);
    block.release();power.join();worker.join();REQUIRE(acknowledged);
    const uint64_t expected=wake?1:(kind==CommandKind::Caps?1:(kind==CommandKind::Enable?33:(confirmation?34:17)));
    REQUIRE(s.commands()==before+expected);REQUIRE(!s.nativeBusy);
    REQUIRE(hw.legacyWrites==0);
    if(confirmation || wake || kind==CommandKind::Enable){REQUIRE(AMDZenPM::nativeEnableMayBe());REQUIRE(s.backend==kAMDZenBackendCppcPcc);REQUIRE(s.native->degraded);}
    else REQUIRE(!AMDZenPM::nativeEnableMayBe());
    beforeCommand={};
}
static void cancelCaps(){cancelledSweep(CommandKind::Caps,false,false);}
static void cancelState(){cancelledSweep(CommandKind::State,false,false);}
static void cancelConfirmation(){cancelledSweep(CommandKind::State,false,true);}
static void cancelEnable(){cancelledSweep(CommandKind::Enable,false,false);}
static void cancelWake(){cancelledSweep(CommandKind::State,true,false);}
static void stopQuarantine(){Service s;s.boot();conflict(s);const auto before=s.commands();s.stopNative();REQUIRE(s.commands()==before);}
static void pinFailure(){failPin=true;Service s;s.tick();REQUIRE(s.writes()==0);REQUIRE(s.native->degraded && s.nativeDiag.error==kLifetimeError);REQUIRE(AMDZenPM::nativeEnableMayBe());REQUIRE(registryRoot.latched);REQUIRE(hw.legacyWrites==0);}
static void registryFailure(){registryRoot.failStore=true;Service s;s.tick();REQUIRE(s.writes()==0);REQUIRE(s.native->degraded && s.nativeDiag.error==kLifetimeError);REQUIRE(AMDZenPM::nativeEnableMayBe());REQUIRE(modulePins==0 && hw.legacyWrites==0);}
static void enableTimeout(){Service s;beforeCommand=[&](CommandKind k,uint32_t){if(k==CommandKind::Enable)s.native->channel.firmware->neverComplete=true;};s.tick();REQUIRE(s.native->degraded);REQUIRE(s.nativeDiag.error==uint32_t(Error::Timeout));REQUIRE(AMDZenPM::nativeEnableMayBe() && hw.legacyWrites==0);}
static void enablePreflightFailure(){Service s;beforeCommand=[&](CommandKind k,uint32_t){if(k==CommandKind::Enable)s.native->requests[0].epp=256;};s.tick();REQUIRE(s.native->degraded);REQUIRE(s.nativeDiag.error==uint32_t(Error::BadRequest));REQUIRE(s.commands()==4 && s.writes()==0);REQUIRE(AMDZenPM::nativeEnableMayBe() && hw.legacyWrites==0);}
static void pureRules(){
    amdzen::NativeAdmission a;REQUIRE(a.allowed());a.pause();REQUIRE(!a.allowed());a.resume();REQUIRE(a.allowed());a.revoke();a.resume();REQUIRE(!a.allowed());
    amdzen::PrerequisitePlan p{};const uint64_t unrelated=HWCR_IRPERF_EN|0x100;
    REQUIRE(amdzen::planPrerequisites(false,false,0x102,unrelated,p));REQUIRE(p.ctl==0x100 && p.hwcr==(unrelated|HWCR_CPB_DIS));
    REQUIRE(!amdzen::planPrerequisites(true,true,2,unrelated,p));REQUIRE(amdzen::planPrerequisites(true,true,0x100,unrelated|HWCR_CPB_DIS,p));REQUIRE(p.hwcr==unrelated);
    zennative::Topology t{};t.cpuCount=3;t.domainOfCpu[0]=t.domainOfCpu[2]=7;t.domainOfCpu[1]=3;
    zencppc::Caps caps[]={{166,136,82,29,0,0},{180,140,80,30,0,0},{150,130,70,40,0,0}};
    zencppc::Request r[3]{};REQUIRE(amdzen::neutralRequests(t,caps,r));REQUIRE(zencppc::sameRequest(r[0],r[2]));REQUIRE(r[0].minPerf==40 && r[0].maxPerf==150);REQUIRE(r[1].maxPerf==180);
    caps[2]={200,195,190,170,0,0};REQUIRE(!amdzen::neutralRequests(t,caps,r));
    caps[2]=caps[0];t.domainOfCpu[1]=7;REQUIRE(!amdzen::neutralRequests(t,caps,r));
    amdzen::DomainBounds b{40,150,136,false};zencppc::Request q{100,150,0,128};REQUIRE(!amdzen::restrictRequest(q,b,0,30));
}
static void lpmAndAdaptiveInterval(){
    hw.boostDisabled[0]=hw.boostDisabled[1]=false;
    Service s;s.boot();
    REQUIRE((s.info->features & AMDZEN_F_PREFCORE) != 0);
    REQUIRE(s.info->cppcEffectiveHighest == 166);
    REQUIRE(hw.prefCoreCalls == 1);
    s.lpmMode=1;s.nativeNudge();REQUIRE(s.nativeIntervalMs==50 && s.timer->lastTimeoutMs==10);
    s.tick();
    REQUIRE(s.nativeDiag.requests[0].epp==224 && s.nativeDiag.requests[0].maxPerf==136);
    s.lpmMode=0;s.nativeNudge();s.tick();
    REQUIRE(s.nativeDiag.requests[0].epp==128 && s.nativeDiag.requests[0].maxPerf==150);
    for(int i=0;i<12;++i)s.tick();
    REQUIRE(s.nativeIntervalMs==200);
    s.nativeNudge();REQUIRE(s.nativeIntervalMs==50);
}
static void fixedGovernorsAndMsrNative(){
    hw.boostDisabled[0]=hw.boostDisabled[1]=false;
    Service s;s.boot();
    // Fixed performance and powersave governors work on cppc-pcc.
    s.governor=kAMDZenGovPerformance;s.tick();
    REQUIRE(s.nativeDiag.requests[0].minPerf==136 && s.nativeDiag.requests[0].maxPerf==150 && s.nativeDiag.requests[0].epp==0);
    s.governor=kAMDZenGovPowersave;s.tick();
    REQUIRE(s.nativeDiag.requests[0].minPerf==40 && s.nativeDiag.requests[0].maxPerf==82 && s.nativeDiag.requests[0].epp==255);
    // Back to active with Tdie thermal cap + AC override on fake SMBIOS.
    s.governor=kAMDZenGovCppcActive;s.info->tjMax_mC=95000;s.lastTdie_mC=55000;
    s.native->signals.system={zennative::kOff,zennative::kThermalHeavy,zennative::kPowerInternal,zennative::kPowerPlatformProperty,100};
    s.acPowerMode=1;s.thermalCapMode=zennative::kThermalCapAuto;s.tick();
    REQUIRE(s.nativeDiag.acPower==zennative::kPowerExternal);
    REQUIRE(s.nativeDiag.thermal==zennative::kThermalNominal);
    REQUIRE(s.nativeDiag.requests[0].maxPerf==150);
    // Real overheat (92 C on 95 C TjMax) triggers Heavy thermal cap even if macOS says nominal.
    s.native->signals.system.thermal=zennative::kThermalNominal;
    s.lastTdie_mC=92000;s.tick();
    REQUIRE(s.nativeDiag.thermal==zennative::kThermalHeavy);
    REQUIRE(s.nativeDiag.requests[0].maxPerf<150);
    s.stopNative();

    // Separate cppc-msr laptop path: mobile-eco profile + battery vs AC + LPM without PCC.
    rebootFixture();
    hw.boostDisabled[0]=hw.boostDisabled[1]=false;
    Service msr;msr.nativePending=false;msr.backend=kAMDZenBackendCppcMsr;msr.governor=kAMDZenGovCppcActive;
    msr.limitFast=0;msr.limitSlow=0;
    msr.info->cppcHighest=196;msr.info->cppcEffectiveHighest=166;msr.info->cppcNominal=120;
    msr.info->cppcLowestNonlinear=55;msr.info->cppcLowest=20;msr.info->tjMax_mC=95000;
    msr.profileActive=zennative::kProfileMobileEco;msr.acPowerMode=0;msr.lastTdie_mC=50000;
    msr.reqDirty=false;
    REQUIRE(msr.nativeStepCppcMsr());
    REQUIRE(msr.reqDirty);
    REQUIRE(msr.nativeDiag.requests[0].epp==192); // mobile-eco batteryEpp
    REQUIRE(msr.nativeDiag.requests[0].maxPerf==166);
    msr.lpmMode=1;msr.reqDirty=false;reviewNow+=100000000ull;
    REQUIRE(msr.nativeStepCppcMsr());
    REQUIRE(msr.reqDirty);
    REQUIRE(msr.nativeDiag.requests[0].epp==240 && msr.nativeDiag.requests[0].maxPerf==120);
    REQUIRE(msr.nativeLegacyPowerSave());
    msr.lpmMode=0;msr.acPowerMode=1;
    REQUIRE(!msr.nativeLegacyPowerSave());
}
int main(){
    const struct {const char *name;void(*run)();} tests[]={
        {"replacement service retains quarantine",replacement},{"registry survives simulated module reload",persistentReload},
        {"allocation failure after reload retains quarantine",allocationFailureAfterReload},
        {"safe discovery fallback",preEnableFallback},{"late conflict before enable",lateEnableConflict},{"late conflict before apply",lateApplyConflict},
        {"conflict drains admitted enable",admittedEnableConflict},{"conflict drains admitted apply",admittedApplyConflict},
        {"boot boost keep off",boostKeepOff},{"boot boost keep on",boostKeepOn},{"mixed boot SMT boost",boostMixed},
        {"boost on back to keep off",boostBackToKeepOff},{"boost off back to keep on",boostBackToKeepOn},
        {"boost keep while governor none",boostKeepWhileNone},
        {"neutral alias intersection / none / stop",neutralAlias},{"wake restores dropped enable prerequisites",wakeReset},
        {"wake partial enable per CPU",wakePartial},{"startup partial enable per CPU",startupPartial},
        {"wake quarantines enabled CPU without P0",wakeInvalidEnabledP0},{"wake validates kept enable prerequisites",wakeValidateKeptEnable},
        {"wake restores neutral governor none",wakeNone},
        {"unrestorable prerequisites quarantine",prerequisitesFail},{"queued cancellation / resume / retain balance",queuedCancel},
        {"sleep cancels capability sweep after one command",cancelCaps},{"sleep cancels initial state sweep",cancelState},
        {"sleep cancels confirmation with enable latch",cancelConfirmation},{"sleep cancels wake sweep",cancelWake},
        {"sleep cancels enable before confirmation",cancelEnable},
        {"stop never writes on revoked Channel",stopQuarantine},{"module pin failure blocks enable",pinFailure},
        {"registry latch failure blocks enable",registryFailure},{"pure admission / domain / prerequisite rules",pureRules},
        {"enable timeout never falls back",enableTimeout},{"enable preflight intent stays quarantined",enablePreflightFailure},
        {"preferred cores, LPM override and adaptive idle interval",lpmAndAdaptiveInterval},
        {"fixed governors, Tdie thermal immunity and cppc-msr native step",fixedGovernorsAndMsrNative}
    };
    for(const auto &t:tests){rebootFixture();t.run();++cases;printf("PASS %s\n",t.name);}
    printf("Native integration: %u tests, %u checks, 0 failures (ASan/UBSan)\n",cases,unsigned(integrationChecks)+assertions);
}
