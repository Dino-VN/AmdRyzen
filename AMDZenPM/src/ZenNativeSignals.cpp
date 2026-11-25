#include "ZenNativeSignals.hpp"
#include "ZenIdle.hpp"
#include "AMDZenShared.h"
#include <IOKit/IOService.h>
#include <IOKit/pwr_mgt/RootDomain.h>
#include <IOKit/pwr_mgt/IOPM.h>
#include <libkern/c++/OSBoolean.h>
#include <libkern/c++/OSDictionary.h>
#include <libkern/c++/OSNumber.h>

namespace zennative {
static_assert(kMaxCpus == AMDZEN_MAX_CPUS, "collector must cover OS CPU indices");
CpuPublication gCpuUrgency[kMaxCpus];
namespace {
SystemSignals unknownSystem() {
    return {kUnknown, kThermalUnknown, kPowerUnknown, kPowerUnobserved, kIOPMThermalLevelUnknown};
}
IOService *findService(const char *className) {
    OSDictionary *matching = IOService::serviceMatching(className);
    if (!matching) return nullptr;
    OSIterator *it = IOService::getMatchingServices(matching);
    IOService *found = it ? OSDynamicCast(IOService, it->getNextObject()) : nullptr;
    if (found) found->retain();
    if (it) it->release();
    matching->release();
    return found;
}
void discover(SignalState &s) {
    if (!s.root) {
        s.root = IOService::getPMRootDomain();
        if (s.root) s.root->retain();
    }
    if (s.platform && s.platform->isInactive()) { s.platform->release(); s.platform = nullptr; }
    if (s.powerSource && s.powerSource->isInactive()) { s.powerSource->release(); s.powerSource = nullptr; }
    if (!s.platform) s.platform = findService("X86PlatformPlugin");
    if (!s.powerSource) s.powerSource = findService("IOPMPowerSource");
}
bool boolProperty(IOService *service, const char *key, bool &value) {
    if (!service) return false;
    OSObject *property = service->copyProperty(key); // retained, unlike getProperty
    OSBoolean *b = OSDynamicCast(OSBoolean, property);
    if (b) value = b->isTrue();
    const bool known = b != nullptr;
    if (property) property->release();
    return known;
}
void poll(SignalState &s) {
    discover(s);
    SystemSignals next = unknownSystem();
    // No documented kernel LPM setting/notification exists in this SDK, and
    // no LPM/energy-mode property is published on this MacPro7,1. "Low Power
    // Sleep" and kern.lowpowerwake describe sleep, not Low Power Mode.
    if (s.root) {
        OSObject *property = s.root->copyProperty(kIOPMRootDomainPowerStatusKey);
        OSDictionary *status = OSDynamicCast(OSDictionary, property);
        OSNumber *warning = status ? OSDynamicCast(OSNumber, status->getObject(kIOPMThermalLevelWarningKey)) : nullptr;
        if (warning && warning->unsigned64BitValue() <= UINT32_MAX) {
            next.rawThermal = warning->unsigned32BitValue();
            next.thermal = decodeThermalWarning(next.rawThermal);
        }
        if (property) property->release();
        unsigned long source = 0;
        if (s.root->getAggressiveness(kPMPowerSource, &source) == kIOReturnSuccess) {
            if (source == kIOPMExternalPower) next.powerSource = kPowerExternal;
            else if (source == kIOPMInternalPower) next.powerSource = kPowerInternal;
            if (next.powerSource != kPowerUnknown) next.powerEvidence = kPowerRootAggressiveness;
        }
    }
    bool external = false;
    if (next.powerSource == kPowerUnknown && boolProperty(s.powerSource, kIOPMPSExternalConnectedKey, external)) {
        next.powerSource = external ? kPowerExternal : kPowerInternal;
        next.powerEvidence = kPowerSourceProperty;
    }
    // Live host: X86PlatformPlugin publishes OSBoolean CPUOnACPower = Yes.
    // No X86PlatformPlugin symbols/private methods are used. This property is
    // not a documented ABI: absence/type mismatch leaves the source unknown.
    if (next.powerSource == kPowerUnknown && boolProperty(s.platform, "CPUOnACPower", external)) {
        next.powerSource = external ? kPowerExternal : kPowerInternal;
        next.powerEvidence = kPowerPlatformProperty;
    }
    s.system = next;
}
} // namespace

void initSignals(SignalState &s) {
    if (s.initialized) return;
    s.system = unknownSystem();
    s.polled = false;
    s.initialized = true;
}
void stopSignals(SignalState &s) {
    if (s.root) { s.root->release(); s.root = nullptr; }
    if (s.platform) { s.platform->release(); s.platform = nullptr; }
    if (s.powerSource) { s.powerSource->release(); s.powerSource = nullptr; }
    s.initialized = s.polled = false;
    s.system = unknownSystem();
}
void snapshotCpus(CpuSnapshot *out, uint32_t ncpu) {
    if (!out) return;
    if (ncpu > kMaxCpus) ncpu = kMaxCpus;
    const bool live = zen::idle::nativeSignalsActive();
    for (uint32_t c = 0; c < ncpu; ++c)
        out[c] = readCpuSnapshot(gCpuUrgency[c], live);
}
void snapshotSignals(SignalState &s, uint64_t now, uint32_t ncpu) {
    if (!s.initialized) initSignals(s);
    if (!s.polled || now < s.lastPollMs || now - s.lastPollMs >= 250) {
        poll(s); s.lastPollMs = now; s.polled = true;
    }
    snapshotCpus(s.cpus, ncpu);
}
bool tickSignals(SignalState &s, PolicyState &p, const Topology &top,
                 const zencppc::Caps *caps, const Tunables &tun,
                 uint64_t now, TickResult &out) {
    snapshotSignals(s, now, top.cpuCount);
    return tick(p, top, s.cpus, s.system, caps, tun, now, out);
}
} // namespace zennative
