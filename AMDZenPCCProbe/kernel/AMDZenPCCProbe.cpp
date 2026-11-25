#include "AMDZenPCCProbe.hpp"
#include <IOKit/IOLib.h>
#include <IOKit/IOMessage.h>
#include <IOKit/IONotifier.h>
#include <IOKit/pwr_mgt/RootDomain.h>
#include <IOKit/pwr_mgt/IOPM.h>
#include <libkern/c++/OSCollectionIterator.h>
#include <libkern/c++/OSData.h>
#include <libkern/c++/OSDictionary.h>
#include <pexpert/pexpert.h>
#include <kern/thread.h>

OSDefineMetaClassAndStructors(AMDZenPCCProbe, IOService)
namespace {
// Even if matching is repeated, only one instance is admitted per loaded module.
// Never reset this in stop/free: neither path is a way to re-arm the one-shot.
uint32_t everStarted = 0;

pcc_probe::Result currentMetadata(pcc_probe::Workspace &w, const pcc_probe::Request &request,
                                  pcc_probe::Reply &reply) {
    using namespace pcc_probe;
    // Existing read-only ABI, no rebuilding AMDZenPM and no sample/control calls.
    if (AMDZenPM_copyInfo(&w.info) != 0) return Result::MetadataUnavailable;
    OSDictionary *matching = IOService::serviceMatching("AppleACPIPlatformExpert");
    if (!matching) return Result::MetadataUnavailable;
    OSIterator *services = IOService::getMatchingServices(matching);
    matching->release();
    if (!services) return Result::MetadataUnavailable;
    IOService *platform = OSDynamicCast(IOService, services->getNextObject());
    if (!platform || services->getNextObject() != nullptr) {
        services->release(); return Result::MetadataUnavailable;
    }
    platform->retain();
    services->release();
    // Direct property lookup, not a cached/sanitized file, getProperties dump,
    // getACPITableData call-through, AML evaluation or physical-memory read.
    OSObject *property = platform->copyProperty("ACPI Tables");
    platform->release();
    OSDictionary *tables = OSDynamicCast(OSDictionary, property);
    if (!tables || tables->getCount() == 0 || tables->getCount() > 128) {
        if (property) property->release();
        return Result::MetadataUnavailable;
    }
    OSCollectionIterator *it = OSCollectionIterator::withCollection(tables);
    if (!it) { property->release(); return Result::MetadataUnavailable; }
    TableSet selected{};
    bool valid = true;
    size_t total = 0;
    while (OSObject *object = it->getNextObject()) {
        const OSSymbol *key = OSDynamicCast(OSSymbol, object);
        OSData *data = key ? OSDynamicCast(OSData, tables->getObject(key)) : nullptr;
        if (!data || data->getLength() < 4 || data->getLength() > cppc_lab::kMaxTableBytes) {
            valid = false; break;
        }
        total += data->getLength();
        if (total > 2u * 1024u * 1024u) { valid = false; break; }
        considerTable({static_cast<const uint8_t *>(data->getBytesNoCopy()), data->getLength()}, selected);
    }
    Result result = valid ? validateProfile(selected, w) : Result::MetadataUnavailable;
    if (result == Result::Ok) describe(request, selected, w, reply);
    reply.detail = w.detail;
    it->release();
    property->release();
    return result;
}
}

bool AMDZenPCCProbe::start(IOService *provider) {
    if (!IOService::start(provider)) return false;
    // Failed preparation must neither leak superclass startup nor consume the
    // one-instance admission before any diagnostic service can be published.
    drainLock_ = IOLockAlloc();
    if (!drainLock_) { IOService::stop(provider); return false; }
    uint32_t expected = 0;
    if (!__atomic_compare_exchange_n(&everStarted, &expected, 1u, false, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) {
        IOLockFree(drainLock_);
        drainLock_ = nullptr;
        IOService::stop(provider);
        return false;
    }
    int enabled = 0;
    bootGate_ = PE_parse_boot_argn("amdzenpccprobe", &enabled, sizeof(enabled)) && enabled == 1;
    configGate_ = getProperty("EnablePccReadProbe") == kOSBooleanTrue;
    powerNotifier_ = registerPrioritySleepWakeInterest(&AMDZenPCCProbe::powerMessage, this, nullptr);
    if (!powerNotifier_) control_.powerChanged(); // Cannot establish lifecycle; never arm.
    // No mappings, firmware calls, scheduler callbacks, timers or pmDispatch here.
    setProperty("PhysicalTransportAvailable", false);
    setProperty("ReadBlocker", "No verified exclusive PCC lease/power-quiescence binding; no physical transport compiled");
    registerService();
    return true;
}
IOReturn AMDZenPCCProbe::setProperties(OSObject *) {
    // The opt-in configuration is boot-time only. No runtime arm/reset API.
    return kIOReturnUnsupported;
}
void AMDZenPCCProbe::drain() {
    if (!drainLock_) return;
    IOLockLock(drainLock_);
    control_.stop();
    while ((control_.flags() & pcc_probe::ActiveCall) != 0)
        IOLockSleep(drainLock_, &control_, THREAD_UNINT);
    IOLockUnlock(drainLock_);
}
void AMDZenPCCProbe::stop(IOService *provider) {
    drain();
    if (powerNotifier_) { powerNotifier_->remove(); powerNotifier_ = nullptr; }
    IOService::stop(provider);
}
void AMDZenPCCProbe::free() {
    drain();
    // remove() synchronizes with entered handlers. Never free callback targets first.
    if (powerNotifier_) { powerNotifier_->remove(); powerNotifier_ = nullptr; }
    if (drainLock_) { IOLockFree(drainLock_); drainLock_ = nullptr; }
    IOService::free();
}
IOReturn AMDZenPCCProbe::powerMessage(void *target, void *, UInt32 message, IOService *, void *argument, vm_size_t) {
    auto *self = static_cast<AMDZenPCCProbe *>(target);
    switch (message) {
        case kIOMessageSystemCapabilityChange: {
            auto *parameters = static_cast<IOPMSystemCapabilityChangeParameters *>(argument);
            IOLockLock(self->drainLock_);
            if (parameters)
                self->control_.capabilitiesChanged(parameters->fromCapabilities, parameters->toCapabilities,
                                                  kIOPMSystemCapabilityCPU);
            else self->control_.powerChanged(); // Missing transition data cannot arm diagnostics.
            IOLockUnlock(self->drainLock_);
            // Synchronous priority-interest handling: no deferred work or wait
            // for allowPowerChange(), and a wake never clears invalidation.
            if (parameters) parameters->maxWaitForReply = 0;
            break;
        }
        case kIOMessageSystemWillSleep:
        case kIOMessageSystemWillPowerOff:
        case kIOMessageSystemWillRestart:
        case kIOMessageSystemWillPowerOn:
        case kIOMessageSystemHasPoweredOn:
            IOLockLock(self->drainLock_);
            self->control_.powerChanged();
            IOLockUnlock(self->drainLock_);
            break;
        default: break;
    }
    // This callback cancels diagnostic work only. It is NOT a proven physical
    // transport power fence, another reason no MMIO implementation is installed.
    return kIOReturnSuccess;
}
void AMDZenPCCProbe::closeClient(pcc_probe::ClientState &client) {
    if (!drainLock_) { client.close(); return; }
    IOLockLock(drainLock_);
    client.close();
    IOLockUnlock(drainLock_);
}
void AMDZenPCCProbe::perform(pcc_probe::Selector selector, const pcc_probe::Request &request, bool admin,
                             const pcc_probe::ClientState &client, pcc_probe::Reply &reply) {
    using namespace pcc_probe;
    initializeReply(request, reply);
    if (bootGate_) reply.flags |= BootEnabled;
    if (configGate_) reply.flags |= ConfigEnabled;
    Result result = validateRequest(static_cast<uint32_t>(selector), request);
    if (result == Result::Ok && selector == Selector::ReadOnce) {
        if (!admin) result = Result::NotAdministrator;
        else if (!bootGate_ || !configGate_) result = Result::GateClosed;
    }
    bool entered = false;
    if (result == Result::Ok && drainLock_) {
        IOLockLock(drainLock_);
        result = control_.tryEnter(client);
        entered = result == Result::Ok;
        IOLockUnlock(drainLock_);
    } else if (result == Result::Ok) result = Result::Stopped;
    if (entered) {
        auto *workspace = static_cast<Workspace *>(IOMallocZero(sizeof(Workspace)));
        if (!workspace) result = Result::MetadataUnavailable;
        else {
            result = currentMetadata(*workspace, request, reply);
            if (result == Result::Ok && selector == Selector::ReadOnce) {
                // Intentional, unconditional refusal boundary, NOT a missing flag.
                // Do not replace nullptr with raw IOMemoryDescriptor mappings as
                // a workaround for the absent cross-OSPM channel lease/power fence.
                result = readOnce(request, admin, bootGate_, configGate_, *workspace,
                                  control_, client, nullptr, reply);
            }
            IOFree(workspace, sizeof(Workspace));
        }
        IOLockLock(drainLock_);
        // The same mutex protects stop/power/client closure and ALL of the final
        // cancellation check, metadata clearing, leave and reply commitment.
        completeCall(request, result, control_, client, reply);
        IOLockWakeup(drainLock_, &control_, false);
        IOLockUnlock(drainLock_);
    } else {
        reply.flags = (reply.flags & ~kControlFlags) | control_.flags();
        if ((reply.flags & PlatformMayOwn) != 0)
            reply.ownership = static_cast<uint32_t>(Ownership::PlatformMayOwn);
        reply.result = static_cast<uint32_t>(result);
    }
}
IOReturn AMDZenPCCProbe::newUserClient(task_t task, void *security, UInt32 type,
                                      OSDictionary *properties, IOUserClient **handler) {
    if (!handler || type != 0 || isInactive()) return kIOReturnBadArgument;
    *handler = nullptr;
    auto *client = new AMDZenPCCProbeUserClient;
    if (!client) return kIOReturnNoMemory;
    if (!client->initWithTask(task, security, type, properties)) {
        client->release(); return kIOReturnBadArgument;
    }
    if (!client->attach(this)) { client->release(); return kIOReturnError; }
    if (!client->start(this)) {
        client->detach(this); client->release(); return kIOReturnError;
    }
    *handler = client;
    return kIOReturnSuccess;
}
