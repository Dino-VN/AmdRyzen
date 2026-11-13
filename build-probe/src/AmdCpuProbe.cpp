// Build probe: exercises the APIs the real kext needs so that the link step
// proves every symbol resolves against kpi.{iokit,libkern,mach,bsd,unsupported}
// + com.apple.iokit.IOPCIFamily. Never loaded.
#include "AmdCpuProbe.hpp"
#include "xnu_private.h"

#include <IOKit/IOLib.h>
#include <libkern/libkern.h>
#include <libkern/OSKextLib.h>
#include <libkern/sysctl.h>
#include <sys/sysctl.h>

OSDefineMetaClassAndStructors(AmdCpuProbe, IOService)
OSDefineMetaClassAndStructors(AmdCpuProbeUserClient, IOUserClient)

// ---------------- sysctl: debug.amdcpu.tsc_hz ----------------
static uint64_t gTscHz = 0;
SYSCTL_NODE(_debug, OID_AUTO, amdcpuprobe, CTLFLAG_RW | CTLFLAG_LOCKED, nullptr, "AMD CPU probe");
SYSCTL_QUAD(_debug_amdcpuprobe, OID_AUTO, tsc_hz, CTLFLAG_RD | CTLFLAG_LOCKED, &gTscHz, "TSC Hz");

// ---------------- per-CPU MSR sampling via rendezvous ----------------
struct SampleCtx { AmdCpuProbe *self; };

static void perCpuSample(void *arg) {
    auto *ctx = static_cast<SampleCtx *>(arg);
    int cpu = cpu_number();
    if (cpu < 0 || cpu >= AMDCPU_MAX_CPUS) return;
    uint32_t lo, hi;
    if (rdmsr_carefully(0xC00000E7 /* MPERF_RO */, &lo, &hi) == 0)
        ctx->self->lastMperf[cpu] = ((uint64_t)hi << 32) | lo;
    ctx->self->lastAperf[cpu] = amdcpu_rdmsr(0xC00000E8 /* APERF_RO */);
}

void AmdCpuProbe::sample(IOTimerEventSource *src) {
    SampleCtx ctx{this};
    mp_rendezvous_no_intrs(perCpuSample, &ctx);
    src->setTimeoutMS(1000);
}

// ---------------- PCI root complex / SMN ----------------
IOPCIDevice *AmdCpuProbe::findRootComplex() {
    OSDictionary *match = IOService::serviceMatching("IOPCIDevice");
    if (!match) return nullptr;
    IOPCIDevice *found = nullptr;
    OSIterator *it = IOService::getMatchingServices(match);
    if (it) {
        while (OSObject *o = it->getNextObject()) {
            IOPCIDevice *d = OSDynamicCast(IOPCIDevice, o);
            if (!d) continue;
            if (d->getBusNumber() == 0 && d->getDeviceNumber() == 0 && d->getFunctionNumber() == 0 &&
                d->configRead16(kIOPCIConfigVendorID) == 0x1022) {
                found = d;
                found->retain();
                break;
            }
        }
        it->release();
    }
    match->release();
    // Alternative that blocks until published:
    // IOService::waitForMatchingService(IOService::serviceMatching("IOPCIDevice"), timeout_ns)
    return found;
}

bool AmdCpuProbe::smnRead(uint32_t addr, uint32_t *value) {
    if (!rootPci) return false;
    IOLockLock(smnLock);
    rootPci->configWrite32(0x60, addr);
    *value = rootPci->configRead32(0x64);
    IOLockUnlock(smnLock);
    return true;
}

// ---------------- lifecycle ----------------
bool AmdCpuProbe::start(IOService *provider) {
    if (!IOService::start(provider)) return false;

    amdcpu_tscInfo ti{};
    tsc_get_info(&ti);
    gTscHz = ti.tscFreq;

    uint32_t r[4];
    amdcpu_cpuid(0x80000007, 0, r);

    smnLock = IOLockAlloc();
    rootPci = findRootComplex();

    workLoop = IOWorkLoop::workLoop();
    timer = IOTimerEventSource::timerEventSource(
        this, OSMemberFunctionCast(IOTimerEventSource::Action, this, &AmdCpuProbe::sample));
    if (!workLoop || !timer || workLoop->addEventSource(timer) != kIOReturnSuccess) return false;
    timer->setTimeoutMS(1000);

    sysctl_register_oid(&sysctl__debug_amdcpuprobe);
    sysctl_register_oid(&sysctl__debug_amdcpuprobe_tsc_hz);

    IOLog("AmdCpuProbe: %s ncpus=%u tsc=%llu\n", OSKextGetCurrentIdentifier(), real_ncpus, gTscHz);
    IOSleep(1);
    registerService();
    return true;
}

void AmdCpuProbe::stop(IOService *provider) {
    sysctl_unregister_oid(&sysctl__debug_amdcpuprobe_tsc_hz);
    sysctl_unregister_oid(&sysctl__debug_amdcpuprobe);
    if (timer) {
        timer->cancelTimeout();
        if (workLoop) workLoop->removeEventSource(timer);
        OSSafeReleaseNULL(timer);
    }
    OSSafeReleaseNULL(workLoop);
    OSSafeReleaseNULL(rootPci);
    if (smnLock) { IOLockFree(smnLock); smnLock = nullptr; }
    IOService::stop(provider);
}

IOReturn AmdCpuProbe::setProperties(OSObject *properties) {
    OSDictionary *d = OSDynamicCast(OSDictionary, properties);
    if (!d) return kIOReturnBadArgument;
    if (OSNumber *n = OSDynamicCast(OSNumber, d->getObject("SampleIntervalMS"))) {
        if (timer) timer->setTimeoutMS(n->unsigned32BitValue());
        return kIOReturnSuccess;
    }
    return kIOReturnUnsupported;
}

// ---------------- user client ----------------
const IOExternalMethodDispatch AmdCpuProbeUserClient::sMethods[1] = {
    { &AmdCpuProbeUserClient::sReadSmn, 1, 0, 1, 0 },
};

bool AmdCpuProbeUserClient::initWithTask(task_t owningTask, void *securityToken, UInt32 type,
                                         OSDictionary *properties) {
    if (clientHasPrivilege(securityToken, kIOClientPrivilegeAdministrator) != kIOReturnSuccess)
        return false;
    return IOUserClient::initWithTask(owningTask, securityToken, type, properties);
}

bool AmdCpuProbeUserClient::start(IOService *provider) {
    owner = OSDynamicCast(AmdCpuProbe, provider);
    return owner && IOUserClient::start(provider);
}

IOReturn AmdCpuProbeUserClient::clientClose() {
    terminate();
    return kIOReturnSuccess;
}

IOReturn AmdCpuProbeUserClient::sReadSmn(OSObject *target, void *, IOExternalMethodArguments *args) {
    auto *self = OSDynamicCast(AmdCpuProbeUserClient, target);
    uint32_t v = 0;
    if (!self || !self->owner->smnRead((uint32_t)args->scalarInput[0], &v)) return kIOReturnError;
    args->scalarOutput[0] = v;
    return kIOReturnSuccess;
}

IOReturn AmdCpuProbeUserClient::externalMethod(uint32_t selector, IOExternalMethodArguments *args,
                                               IOExternalMethodDispatch *dispatch, OSObject *target,
                                               void *reference) {
    if (selector >= sizeof(sMethods) / sizeof(sMethods[0])) return kIOReturnBadArgument;
    dispatch = const_cast<IOExternalMethodDispatch *>(&sMethods[selector]);
    target = this;
    return IOUserClient::externalMethod(selector, args, dispatch, target, reference);
}
