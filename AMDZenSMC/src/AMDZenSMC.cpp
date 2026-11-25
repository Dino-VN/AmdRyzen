//
// AMDZenSMC.cpp — VirtualSMC plugin publishing AMDZenPM telemetry (see AMDZenSMC.hpp).
//
// Threading model: everything except readAccess() runs on this driver's own
// IOWorkLoop from one IOTimerEventSource, plus the one-shot VirtualSMC publish
// notification. readAccess() runs in VirtualSMC's SMC read path, which may be inside
// its kernel_trap hook with AppleSMC's locks held and without an xmm save, so it only
// loads the per-key atomic cell (docs/research/smc-keys.md §1.5).
//
// Start-up order (smc-keys.md §6.1 and Critic notes):
//   1. wait (timer) until AMDZenPM has finished its deferred hardware discovery,
//      because the key set depends on its feature bits and must never change;
//   2. refuse if SMCAMDProcessor / SMCProcessor is present (duplicate keys);
//   3. build every value, add them to the plugin in ASCII order;
//   4. registerHandler() last: its notification can fire synchronously for an
//      already published VirtualSMC and submit the plugin immediately.
//
#include "AMDZenSMC.hpp"

#include <IOKit/IOLib.h>
#include <libkern/c++/OSSymbol.h>
#include <pexpert/pexpert.h>
#include <string.h>

#define LOG(fmt, ...) IOLog("AMDZenSMC: " fmt "\n", ##__VA_ARGS__)

OSDefineMetaClassAndStructors(AMDZenSMC, IOService)

// Read by the kmod stop routine (kmod_info.c): nonzero once VirtualSMC may hold
// pointers into this kext. Before that (e.g. -amdzensmcoff, conflict) unload is fine.
extern "C" int gAMDZenSMCPinned;
int gAMDZenSMCPinned = 0;

// ABI of the vendored SDK against the loaded VirtualSMC 1.3.9 (dSYM layout,
// third_party/README.md). A header update that breaks it must fail the build.
static_assert(sizeof(VirtualSMCValue) == 0x90, "VirtualSMCValue layout changed");
static_assert(sizeof(VirtualSMCKeyValue) == 0x18, "VirtualSMCKeyValue layout changed");
static_assert(sizeof(VirtualSMCAPI::Plugin) == 0x48, "VirtualSMCAPI::Plugin layout changed");
static_assert(VirtualSMCAPI::Version == 1, "VirtualSMC plugin API version changed");
static_assert(smc::makeKey('T', 'C', '0', 'D') == SMC_MAKE_KEY('T', 'C', '0', 'D'), "key packing");
static_assert(smc::makeKey('s', 'p', '7', '8') == SmcKeyTypeSp78, "type packing");

namespace {

constexpr uint32_t kDefaultPeriodMs = 1000;     // matches AMDZenPM's default 1 s telemetry
constexpr uint32_t kMinPeriodMs     = 250;
constexpr uint32_t kMaxPeriodMs     = 5000;
// AMDZenPM defers discovery up to 30 s after its start; allow for that plus slack.
constexpr uint32_t kInfoWaitTicks   = 90 * 1000 / kDefaultPeriodMs;
// Once AMDZenInfo is there, wait this long for the first telemetry sample (seq != 0)
// so that the SVI2-only keys can be decided from real values.
constexpr uint32_t kSampleWaitTicks = 10 * 1000 / kDefaultPeriodMs;
// Consecutive failed copySample calls (at the default period) before the cells are blanked.
constexpr uint32_t kStaleTicks = 10 * 1000 / kDefaultPeriodMs;

// AMDZenPM_copyInfo/copySample memcpy sizeof() of *AMDZenPM's* structs and take no
// size argument. Allocate well beyond our sizeof() so that a newer AMDZenPM with a
// larger layout cannot overrun the buffer before the abiVersion/structSize check
// rejects it.
constexpr size_t kInfoBufBytes   = 16 * 1024;
constexpr size_t kSampleBufBytes = 64 * 1024;
static_assert(sizeof(AMDZenInfo) <= kInfoBufBytes && sizeof(AMDZenSample) <= kSampleBufBytes,
              "copy buffers too small");

// Classes that publish the same CPU keys. Two plugins with the same key are not
// detected by VirtualSMC (shadowing by name, duplicates by index), and overriding
// the same base key twice frees the first plugin's value (smc-keys.md §1.4).
const char *const kConflictClasses[] = {
    "SMCAMDProcessor",
    "SMCProcessor",
};

SMC_KEY_TYPE smcType(smc::Kind k) {
    switch (k) {
    case smc::Kind::Sp78: return SmcKeyTypeSp78;
    case smc::Kind::Sp96: return SmcKeyTypeSp96;
    case smc::Kind::Sp3c: return SmcKeyTypeSp3c;
    case smc::Kind::Flt:  return SmcKeyTypeFloat;
    }
    return SmcKeyTypeSp78;
}

} // namespace

// ===========================================================================
// Key value
// ===========================================================================
SMC_RESULT AMDZenSMCValue::readAccess() {
    // Byte stores rather than memcpy: with -fno-builtin memcpy is an out-of-line call,
    // and this must stay a handful of integer instructions (make check greps the
    // binary for xmm/ymm use).
    const uint32_t v = __atomic_load_n(&cell, __ATOMIC_RELAXED);
    data[0] = (SMC_DATA)(v & 0xFF);
    data[1] = (SMC_DATA)((v >> 8) & 0xFF);
    if (size >= 4) {
        data[2] = (SMC_DATA)((v >> 16) & 0xFF);
        data[3] = (SMC_DATA)((v >> 24) & 0xFF);
    }
    return SmcSuccess;
}

// ===========================================================================
// Lifecycle
// ===========================================================================
IOService *AMDZenSMC::probe(IOService *provider, SInt32 *score) {
    if (!IOService::probe(provider, score)) return nullptr;
    int flag = 0;
    if (PE_parse_boot_argn("-amdzensmcoff", &flag, sizeof(flag))) {
        LOG("disabled by -amdzensmcoff");
        return nullptr;
    }
    return this;
}

bool AMDZenSMC::start(IOService *provider) {
    if (!IOService::start(provider)) return false;

    info = static_cast<AMDZenInfo *>(IOMallocZero(kInfoBufBytes));
    sample = static_cast<AMDZenSample *>(IOMallocZero(kSampleBufBytes));
    // Never freed: the notification handler may still run while this object stops.
    submitLock = IOLockAlloc();
    workLoop = IOWorkLoop::workLoop();
    if (workLoop)
        timer = IOTimerEventSource::timerEventSource(
            this, OSMemberFunctionCast(IOTimerEventSource::Action, this, &AMDZenSMC::timerFired));
    if (!info || !sample || !submitLock || !workLoop || !timer ||
        workLoop->addEventSource(timer) != kIOReturnSuccess) {
        LOG("start failed: out of resources");
        OSSafeReleaseNULL(timer);
        IOService::stop(provider);
        return false;
    }
    setProperty("Status", "waiting for AMDZenPM");
    // AMDZenPM finishes hardware discovery asynchronously, so even the key set is
    // decided from the timer.
    timer->setTimeoutMS(1);
    return true;
}

void AMDZenSMC::stop(IOService *provider) {
    // Values and vsmcPlugin stay alive: VirtualSMC keeps pointers to them and a
    // retain on this object. The cells simply keep their last values.
    // Timer first: an in-flight timerFired() may be inside buildAndRegister() and
    // about to store vsmcNotifier, so the notifier is only looked at afterwards.
    if (timer) {
        timer->cancelTimeout();
        // removeEventSource takes the work-loop gate, so it also waits for an
        // in-flight timerFired() (and with it any AMDZenPM_copySample call) to end
        // before AMDZenPM, our provider, is stopped after us.
        if (workLoop) workLoop->removeEventSource(timer);
    }
    phase = kPhaseIdle;
    if (vsmcNotifier) {
        vsmcNotifier->remove();
        vsmcNotifier = nullptr;
    }
    // No more updates: blank the cells rather than show a frozen, plausible reading
    // (all-zero keys are hidden by Stats).
    clearCells();
    IOService::stop(provider);
}

void AMDZenSMC::free() {
    // Only reached when the plugin was never submitted. Values already handed to
    // vsmcPlugin.data are not freed: that would need Lilu's evector::deinit, and
    // the path only exists after a failed registerHandler().
    OSSafeReleaseNULL(timer);
    OSSafeReleaseNULL(workLoop);
    if (info) { IOFree(info, kInfoBufBytes); info = nullptr; }
    if (sample) { IOFree(sample, kSampleBufBytes); sample = nullptr; }
    IOService::free();
}

// ===========================================================================
// AMDZenPM access (thread context only; both calls are gated inside AMDZenPM)
// ===========================================================================
bool AMDZenSMC::fetchInfo() {
    if (AMDZenPM_copyInfo(info) != 0) return false;   // not initialised yet
    if (info->abiVersion != AMDZEN_ABI_VERSION || info->structSize != sizeof(AMDZenInfo)) {
        LOG("AMDZenPM ABI mismatch (info abi %u size %u, expected %u/%u), not publishing",
            info->abiVersion, info->structSize, AMDZEN_ABI_VERSION, (uint32_t)sizeof(AMDZenInfo));
        phase = kPhaseIdle;
        return false;
    }
    return true;
}

bool AMDZenSMC::fetchSample() {
    if (AMDZenPM_copySample(sample) != 0) return false;
    if (sample->abiVersion != AMDZEN_ABI_VERSION || sample->structSize != sizeof(AMDZenSample)) {
        // Keep the previous cells; a mismatched layout must never be decoded.
        return false;
    }
    return true;
}

const char *AMDZenSMC::conflictingDriver() {
    for (const char *name : kConflictClasses) {
        // Neither driver calls registerService() on itself, so a service lookup would
        // never find it; the metaclass is the only handle. SMCProcessor.kext also loads
        // on AMD (its personality matches any ACPI processor) but its start() fails
        // there and frees the instance, so for it only a live instance counts.
        // SMCAMDProcessor matches on its own PM kext and may start after us, so its
        // mere presence is a conflict.
        const OSSymbol *sym = OSSymbol::withCString(name);
        if (!sym) continue;
        const OSMetaClass *mc = OSMetaClass::getMetaClassWithName(sym);
        sym->release();
        if (!mc) continue;
        if (strcmp(name, "SMCProcessor") == 0 && mc->getInstanceCount() == 0) continue;
        return name;
    }
    return nullptr;
}

// ===========================================================================
// Timer: key-set decision, then periodic refresh
// ===========================================================================
void AMDZenSMC::timerFired(IOTimerEventSource *sender) {
    if (phase == kPhaseIdle) return;

    if (phase == kPhaseWaiting) {
        ticks++;
        if (!infoTick) {
            if (fetchInfo()) infoTick = ticks;
            else if (phase == kPhaseIdle) { setProperty("Status", "AMDZenPM ABI mismatch"); return; }
        }
        if (infoTick) {
            const bool haveSample = fetchSample() && sample->seq != 0;
            if (haveSample || ticks - infoTick >= kSampleWaitTicks) {
                if (!haveSample) bzero(sample, sizeof(AMDZenSample));
                if (!buildAndRegister(haveSample)) return;   // phase is now idle
                if (haveSample) { lastSeq = sample->seq; haveSeq = true; }
            }
        } else if (ticks >= kInfoWaitTicks) {
            LOG("AMDZenPM did not initialise within %u s, not publishing SMC keys",
                kInfoWaitTicks * kDefaultPeriodMs / 1000);
            setProperty("Status", "AMDZenPM not ready");
            phase = kPhaseIdle;
            return;
        }
        sender->setTimeoutMS(kDefaultPeriodMs);
        return;
    }

    refresh();
    uint32_t period = kDefaultPeriodMs;
    if (haveSeq && sample->intervalUs) {
        // Match AMDZenPM's telemetry period so we do not wake twice per sample.
        period = sample->intervalUs / 1000;
        if (period < kMinPeriodMs) period = kMinPeriodMs;
        if (period > kMaxPeriodMs) period = kMaxPeriodMs;
    }
    sender->setTimeoutMS(period);
}

void AMDZenSMC::clearCells() {
    for (uint32_t i = 0; i < numKeys; i++)
        if (values[i]) values[i]->store(0);
}

void AMDZenSMC::refresh() {
    // On a short failure (AMDZenPM busy, re-initialising after wake...) keep the old
    // cells; if it lasts, blank them so a stale temperature is not shown as live.
    if (!fetchSample()) {
        if (failStreak < kStaleTicks && ++failStreak == kStaleTicks) {
            LOG("no telemetry from AMDZenPM for %u s, blanking SMC keys", kStaleTicks * kDefaultPeriodMs / 1000);
            clearCells();
            haveSeq = false;
        }
        return;
    }
    failStreak = 0;
    if (haveSeq && sample->seq == lastSeq) return;
    lastSeq = sample->seq;
    haveSeq = true;
    for (uint32_t i = 0; i < numKeys; i++)
        values[i]->store(smc::encodeKey(keys[i], *info, *sample));
}

bool AMDZenSMC::buildAndRegister(bool haveSample) {
    phase = kPhaseIdle;   // until everything below succeeded

    if (const char *other = conflictingDriver()) {
        LOG("%s is present; not publishing SMC keys (remove SMCAMDProcessor.kext / SMCProcessor.kext)", other);
        setProperty("Status", "conflict");
        setProperty("ConflictingClass", other);
        return false;
    }

    numKeys = smc::buildKeySet(*info, haveSample ? sample : nullptr, keys, smc::kMaxKeys);
    if (!numKeys) {
        LOG("AMDZenPM reports no usable sensors, not publishing SMC keys");
        setProperty("Status", "no sensors");
        return false;
    }

    for (uint32_t i = 0; i < numKeys; i++) {
        const uint32_t cell = smc::encodeKey(keys[i], *info, *sample);
        const SMC_DATA init[4] = {
            (SMC_DATA)(cell & 0xFF), (SMC_DATA)((cell >> 8) & 0xFF),
            (SMC_DATA)((cell >> 16) & 0xFF), (SMC_DATA)((cell >> 24) & 0xFF),
        };
        AMDZenSMCValue *v = new AMDZenSMCValue;
        // valueWithData deletes v on failure; addKey deletes it on failure.
        if (!v || !VirtualSMCAPI::valueWithData(init, smc::kindSize(keys[i].kind), smcType(keys[i].kind), v,
                                                SMC_KEY_ATTRIBUTE_READ)) {
            LOG("cannot create SMC value %u", i);
            setProperty("Status", "out of memory");
            return false;
        }
        v->store(cell);
        // Keys go into data (indexed), never dataHidden: Stats only discovers keys by
        // index. keys[] is already in ASCII order, which VirtualSMC's binary search needs.
        if (!VirtualSMCAPI::addKey(keys[i].key, vsmcPlugin.data, v)) {
            LOG("cannot add SMC key %u", i);
            setProperty("Status", "out of memory");
            return false;
        }
        values[i] = v;
    }
    publishKeyList();

    // From here VirtualSMC may hold pointers into this kext (the handler can fire
    // synchronously inside registerHandler), so the kmod stop routine must refuse.
    __atomic_store_n(&gAMDZenSMCPinned, 1, __ATOMIC_SEQ_CST);
    // Before registerHandler: the handler's "published" must be the last write.
    setProperty("Status", "waiting for VirtualSMC");
    phase = kPhaseRunning;
    vsmcNotifier = VirtualSMCAPI::registerHandler(&AMDZenSMC::vsmcNotification, this);
    if (!vsmcNotifier) {
        LOG("VirtualSMC handler registration failed");
        setProperty("Status", "registration failed");
        phase = kPhaseIdle;
        return false;
    }
    LOG("%u SMC keys ready", numKeys);
    return true;
}

void AMDZenSMC::publishKeyList() {
    // "TC0D TC0P ..." for ioreg; easier to read than an array and needs no OSArray.
    char buf[smc::kMaxKeys * 5 + 1];
    uint32_t p = 0;
    for (uint32_t i = 0; i < numKeys && p + 5 < sizeof(buf); i++) {
        char name[5];
        smc::keyName(keys[i].key, name);
        if (p) buf[p++] = ' ';
        for (uint32_t c = 0; c < 4; c++) buf[p++] = name[c];
    }
    buf[p] = '\0';
    setProperty("SMCKeys", buf);
}

bool AMDZenSMC::vsmcNotification(void *target, void *, IOService *vsmc, IONotifier *) {
    AMDZenSMC *self = static_cast<AMDZenSMC *>(target);
    if (!self || !vsmc || !self->submitLock) return false;
    // First-publish fires once per APP0001 service, possibly on several threads at
    // once (a real ACPI SMC device can be named APP0001 too). Serialise instead of a
    // CAS so that a concurrent non-VirtualSMC attempt cannot swallow VirtualSMC's only
    // notification; submit at most once.
    IOLockLock(self->submitLock);
    if (self->submitted) {
        IOLockUnlock(self->submitLock);
        return false;
    }
    IOReturn r = vsmc->callPlatformFunction(VirtualSMCAPI::SubmitPlugin, true, self, &self->vsmcPlugin,
                                            nullptr, nullptr);
    if (r == kIOReturnSuccess) {
        __atomic_store_n(&self->submitted, 1u, __ATOMIC_RELEASE);
        LOG("submitted %u SMC keys to VirtualSMC", self->numKeys);
        self->setProperty("Status", "published");
    } else if (r != kIOReturnUnsupported) {
        // kIOReturnUnsupported: a real (non-VirtualSMC) SMC device named APP0001.
        LOG("VirtualSMC rejected the plugin (0x%x)", r);
        self->setProperty("Status", "rejected by VirtualSMC");
    }
    IOLockUnlock(self->submitLock);
    return r == kIOReturnSuccess;
}
