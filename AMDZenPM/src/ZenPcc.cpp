#include "ZenPcc.hpp"
#include <IOKit/IOLib.h>
#include <IOKit/IOService.h>
#include <IOKit/acpi/IOACPIPlatformDevice.h>
#include <libkern/c++/OSArray.h>
#include <libkern/c++/OSCollectionIterator.h>
#include <libkern/c++/OSData.h>
#include <libkern/c++/OSDictionary.h>
#include <libkern/c++/OSNumber.h>
#include <libkern/c++/OSString.h>
#include <libkern/c++/OSSymbol.h>
#include <kern/clock.h>
#include <kern/sched_prim.h>
#include <machine/machine_routines.h>

namespace zenpcc {
namespace {
SubspaceClaim subspaceClaim;

bool threadContext() {
    return ml_get_interrupts_enabled() && !ml_at_interrupt_context() && preemption_enabled();
}
Bytes bytes(const OSData *data) {
    return {static_cast<const uint8_t *>(data->getBytesNoCopy()), data->getLength()};
}
bool signature(Bytes b, const char *s) {
    if (!b.data || b.size < 4) return false;
    for (size_t i = 0; i < 4; ++i) if (b.data[i] != uint8_t(s[i])) return false;
    return true;
}
bool number(OSObject *object, uint32_t &out) {
    OSNumber *n = OSDynamicCast(OSNumber, object);
    if (!n || n->unsigned64BitValue() > UINT32_MAX) return false;
    out = n->unsigned32BitValue();
    return true;
}
bool uid(OSObject *object, uint32_t &out) {
    if (number(object, out)) return true;
    // ACPI0007 numeric _UID may be represented by a decimal string.
    OSString *s = OSDynamicCast(OSString, object);
    const char *p = s ? s->getCStringNoCopy() : nullptr;
    if (!p || !*p) return false;
    uint32_t value = 0;
    for (uint32_t i = 0; p[i]; ++i) {
        if (i >= 10 || p[i] < '0' || p[i] > '9') return false;
        uint32_t digit = uint32_t(p[i] - '0');
        if (value > (UINT32_MAX - digit) / 10) return false;
        value = value * 10 + digit;
    }
    out = value;
    return true;
}
bool processorIdProperty(IOACPIPlatformDevice *device, uint32_t &out) {
    // AppleACPIPlatform publishes the Processor() ProcID as "processor-id"
    // (verified on Darwin 25.6); "cpu-id" is kept as an older/alternate spelling.
    OSObject *object = device->getProperty("processor-id");
    if (!object) object = device->getProperty("cpu-id");
    if (number(object, out)) return out <= 255;
    OSData *data = OSDynamicCast(OSData, object);
    if (!data || data->getLength() != 4) return false;
    const uint8_t *p = static_cast<const uint8_t *>(data->getBytesNoCopy());
    out = uint32_t(p[0]) | (uint32_t(p[1]) << 8) | (uint32_t(p[2]) << 16) | (uint32_t(p[3]) << 24);
    return out <= 255;
}
bool legacyIdentity(IOACPIPlatformDevice *device, const ProcessorNames &names, uint32_t &id) {
    uint32_t propertyId = 0;
    const bool haveProperty = processorIdProperty(device, propertyId);
    const char *name = device->getName(gIOACPIPlane);
    IORegistryEntry *parent = device->getParentEntry(gIOACPIPlane);
    const char *parentName = parent ? parent->getName(gIOACPIPlane) : nullptr;
    bool found = false;
    // AppleACPIPlatform flattens processors directly under the ACPI plane root
    // ("acpi"), so \_PR_ is not visible as a parent on Darwin 25.6. Accept that
    // layout only when the published ProcID independently agrees with CPUSSDT.
    const bool prScope = parentName && strcmp(parentName, "_PR_") == 0;
    const bool flatRoot = parent && parent->getParentEntry(gIOACPIPlane) == IORegistryEntry::getRegistryRoot();
    if (name && (prScope || (flatRoot && haveProperty)))
        for (uint32_t i = 0; i < names.count; ++i)
            if (strcmp(name, names.cpus[i].name) == 0) { id = names.cpus[i].acpiId; found = true; }
    if (found && haveProperty && id != propertyId) return false;
    if (found) return true;
    // This property's availability is platform-dependent; absence is never
    // replaced by parsing the digits in the processor's IORegistry name.
    if (haveProperty) { id = propertyId; return true; }
    return false;
}
Error cpc(IOACPIPlatformDevice *device, const Descriptor &descriptor, CpuRegisters &out) {
    OSObject *result = nullptr;
    IOReturn ret = device->evaluateObject("_CPC", &result);
    OSArray *array = OSDynamicCast(OSArray, result);
    if (ret != kIOReturnSuccess || !array) {
        OSSafeReleaseNULL(result);
        return Error::AcpiEvaluation;
    }
    const uint32_t count = array->getCount();
    Error error = Error::Length;
    if (count == 21 || count == kCpcEntries) {
        CpcEntry entries[kCpcEntries]{};
        for (uint32_t i = 0; i < count; ++i) {
            OSObject *entry = array->getObject(i);
            OSNumber *n = OSDynamicCast(OSNumber, entry);
            OSData *d = OSDynamicCast(OSData, entry);
            if (n) entries[i] = {EntryKind::Integer, n->unsigned64BitValue(), {nullptr, 0}};
            else if (d) entries[i] = {EntryKind::Buffer, 0, bytes(d)};
            else entries[i].kind = EntryKind::Invalid;
        }
        error = parseCpc(entries, count, descriptor, out);
    }
    result->release();
    if (error != Error::Ok) return error;
    result = nullptr;
    ret = device->evaluateObject("_PSD", &result);
    OSArray *outer = OSDynamicCast(OSArray, result);
    OSArray *psd = outer && outer->getCount() == 1 ? OSDynamicCast(OSArray, outer->getObject(0)) : nullptr;
    uint32_t n = 0, revision = 0, coordination = 0;
    if (ret != kIOReturnSuccess || !psd || psd->getCount() != 5 ||
        !number(psd->getObject(0), n) || n != 5 ||
        !number(psd->getObject(1), revision) || revision != 0 ||
        !number(psd->getObject(2), out.domain) ||
        !number(psd->getObject(3), coordination) || coordination != 0xfe || // HW_ALL
        !number(psd->getObject(4), out.domainMembers) || !out.domainMembers ||
        out.domainMembers > kMaxCpus) error = Error::Domain;
    OSSafeReleaseNULL(result);
    return error;
}

// Own the property throughout parsing, and reject duplicate PCCT/MADT records.
// No getACPITableData call-through, physical ACPI scan, or cached capture file.
Error tables(Descriptor &descriptor, Madt &madt, ProcessorNames &names) {
    OSDictionary *matching = IOService::serviceMatching("AppleACPIPlatformExpert");
    if (!matching) return Error::NoMemory;
    OSIterator *services = IOService::getMatchingServices(matching);
    matching->release();
    if (!services) return Error::AcpiTables;
    IOService *platform = OSDynamicCast(IOService, services->getNextObject());
    OSObject *property = nullptr;
    if (platform && !services->getNextObject()) property = platform->copyProperty("ACPI Tables");
    services->release();
    OSDictionary *dictionary = OSDynamicCast(OSDictionary, property);
    OSCollectionIterator *it = dictionary ? OSCollectionIterator::withCollection(dictionary) : nullptr;
    Error error = Error::AcpiTables;
    uint32_t pcctCount = 0, madtCount = 0, processorCount = 0;
    names.count = 0;
    if (dictionary && it && dictionary->getCount() && dictionary->getCount() <= 128) {
        error = Error::Ok;
        uint32_t total = 0;
        uint32_t fallbackCount = 0;
        while (OSObject *keyObject = it->getNextObject()) {
            const OSSymbol *key = OSDynamicCast(OSSymbol, keyObject);
            OSData *data = key ? OSDynamicCast(OSData, dictionary->getObject(key)) : nullptr;
            if (!data || data->getLength() < 36 || data->getLength() > 65536) { error = Error::AcpiTables; break; }
            total += data->getLength();
            if (total > 2u * 1024u * 1024u) { error = Error::Bounds; break; }
            const Bytes b = bytes(data);
            if (signature(b, "PCCT")) {
                if (++pcctCount != 1) { error = Error::Subspace; break; }
                error = parsePcct(b, descriptor);
            } else if (signature(b, "APIC")) {
                if (++madtCount != 1) { error = Error::DuplicateIdentity; break; }
                error = parseMadt(b, madt);
            } else if (signature(b, "SSDT") && !memcmp(b.data + 16, "CPUSSDT", 7)) {
                if (++processorCount != 1) { error = Error::DuplicateIdentity; break; }
                error = parseProcessorSsdt(b, names);
            } else if (signature(b, "SSDT") && processorCount == 0) {
                // Non-AGESA OEM boards/laptops may name the processor SSDT differently
                // (e.g. "CpuSsdt", "AmdTabl"). Accept it only if strictly parseable
                // as a \_PR_ Processor() table and unique. Reuse `names` to keep the
                // kernel stack frame tiny.
                if (parseProcessorSsdt(b, names) == Error::Ok) ++fallbackCount;
            }
            if (error != Error::Ok) break;
        }
        if (error == Error::Ok && processorCount == 0 && fallbackCount != 1) names.count = 0;
        if (error == Error::Ok && (pcctCount != 1 || madtCount != 1)) error = Error::AcpiTables;
    }
    OSSafeReleaseNULL(it);
    OSSafeReleaseNULL(property);
    return error;
}
} // namespace

Channel::~Channel() {
    teardown();
    if (lock_) { IOLockFree(lock_); lock_ = nullptr; }
}
bool Channel::reject(Error error) {
    // Preserve a transport's first fatal reason, even after teardown/bad args.
    // Store sticky flag and reason together so WrongContext (outside the lock)
    // cannot race publication and overwrite the first firmware failure.
    uint32_t old = __atomic_load_n(&error_, __ATOMIC_ACQUIRE);
    while (!(old & 0x80000000u))
        if (__atomic_compare_exchange_n(&error_, &old, uint32_t(error), false,
                                        __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) break;
    return false;
}
void Channel::publish() {
    __atomic_store_n(&commands_, engine_.commandCount(), __ATOMIC_RELEASE);
    __atomic_store_n(&latency_, engine_.lastLatencyUs(), __ATOMIC_RELEASE);
    __atomic_store_n(&error_, uint32_t(engine_.lastError()) | (engine_.failed() ? 0x80000000u : 0),
                     __ATOMIC_RELEASE);
}
uint32_t Channel::lastError() const { return __atomic_load_n(&error_, __ATOMIC_ACQUIRE) & 0x7fffffffu; }
uint64_t Channel::commandCount() const { return __atomic_load_n(&commands_, __ATOMIC_ACQUIRE); }
uint64_t Channel::lastLatencyUs() const { return __atomic_load_n(&latency_, __ATOMIC_ACQUIRE); }

bool Channel::discover(const uint16_t *apicIds, uint32_t ncpu, uint32_t *reason) {
    if (reason) *reason = uint32_t(Error::Ok);
    if (!threadContext()) {
        reject(Error::WrongContext);
        if (reason) *reason = lastError();
        return false;
    }
    // Initial discovery/lifetime management is owned by the service's thread.
    // Once allocated, this lock is kept through teardown so waiting commands
    // can enter, observe discovered_=false, and fail without a use-after-free.
    if (!lock_) lock_ = IOLockAlloc();
    if (!lock_) {
        reject(Error::NoMemory);
        if (reason) *reason = lastError();
        return false;
    }
    IOLockLock(lock_);
    bool ok = false;
    if (attempted_) reject(Error::AlreadyInitialized);
    else if (!apicIds || !ncpu || ncpu > kMaxCpus) reject(Error::BadArgument);
    else {
        attempted_ = true;
        const Error claimError = subspaceClaim.acquire();
        if (claimError != Error::Ok) reject(claimError);
        else {
            claimed_ = true;
            ok = discoverLocked(apicIds, ncpu);
            if (!ok) unmapLocked();
        }
    }
    if (reason) *reason = lastError();
    IOLockUnlock(lock_);
    return ok;
}
bool Channel::discoverLocked(const uint16_t *apicIds, uint32_t ncpu) {
    Descriptor descriptor{};
    Madt madt{};
    ProcessorNames names{};
    Error error = tables(descriptor, madt, names);
    if (error != Error::Ok) return reject(error);
    uint32_t enabled = 0;
    for (uint32_t i = 0; i < madt.count; ++i) if (madt.cpus[i].enabled) ++enabled;
    if (enabled != ncpu) return reject(Error::MissingIdentity);
    bool seen[kMaxCpus]{};
    OSDictionary *matching = IOService::serviceMatching("IOACPIPlatformDevice");
    if (!matching) return reject(Error::NoMemory);
    OSIterator *devices = IOService::getMatchingServices(matching);
    matching->release();
    if (!devices) return reject(Error::AcpiEvaluation);
    uint32_t found = 0;
    while (OSObject *object = devices->getNextObject()) {
        IOACPIPlatformDevice *device = OSDynamicCast(IOACPIPlatformDevice, object);
        if (!device) continue;
        const bool legacy = device->getDeviceType() == IOACPIPlatformDevice::kTypeProcessor;
        OSString *hid = OSDynamicCast(OSString, device->getProperty(gIOACPIHardwareIDKey));
        if (!legacy && (!hid || !hid->isEqualTo("ACPI0007"))) continue;
        uint32_t acpiId = 0, cpu = 0;
        if (legacy) {
            if (!legacyIdentity(device, names, acpiId)) { error = Error::MissingIdentity; break; }
        } else {
            OSObject *result = nullptr;
            const IOReturn ret = device->evaluateObject("_UID", &result);
            const bool ok = ret == kIOReturnSuccess && uid(result, acpiId);
            OSSafeReleaseNULL(result);
            if (!ok) { error = Error::MissingIdentity; break; }
        }
        error = joinCpu(madt, acpiId, !legacy, apicIds, ncpu, cpu);
        if (error != Error::Ok) break;
        if (seen[cpu]) { error = Error::DuplicateIdentity; break; }
        error = cpc(device, descriptor, cpus_[cpu]);
        if (error != Error::Ok) break;
        cpus_[cpu].acpiId = acpiId;
        cpus_[cpu].apicId = apicIds[cpu];
        cpus_[cpu].x2Apic = !legacy;
        seen[cpu] = true;
        ++found;
    }
    devices->release();
    if (error != Error::Ok) return reject(error);
    if (found != ncpu) return reject(Error::MissingIdentity);
    error = validateCpuMaps(cpus_, ncpu);
    if (error != Error::Ok) return reject(error);
    if (!map(descriptor)) return false;
    error = engine_.initialize(descriptor, cpus_, ncpu, accessor_);
    if (error != Error::Ok) return reject(error);
    discovered_ = true;
    publish();
    return true;
}

bool Channel::map(const Descriptor &d) {
    // Limit to architectural x86 physical-address width (52), with arithmetic
    // already checked by the parser. The doorbell is mapped as one UC page.
    if (d.baseAddress > (UINT64_C(1) << 52) - d.length ||
        d.doorbell.address > (UINT64_C(1) << 52) - 8) return reject(Error::Bounds);
    constexpr uint64_t pageBytes = 4096;
    const uint64_t bellPage = d.doorbell.address & ~(pageBytes - 1);
    const uint64_t bellOffset = d.doorbell.address - bellPage;
    // Mapping overlapping physical pages with different descriptors is avoided.
    const uint64_t sharedPage = d.baseAddress & ~(pageBytes - 1);
    const uint64_t sharedEnd = (d.baseAddress + d.length + pageBytes - 1) & ~(pageBytes - 1);
    if (bellPage >= sharedPage && bellPage < sharedEnd) return reject(Error::Aliasing);
    sharedMemory_ = IOMemoryDescriptor::withPhysicalAddress(d.baseAddress, d.length, kIODirectionInOut);
    doorbellMemory_ = IOMemoryDescriptor::withPhysicalAddress(bellPage, pageBytes, kIODirectionInOut);
    if (!sharedMemory_ || !doorbellMemory_) return reject(Error::NoMemory);
    sharedMap_ = sharedMemory_->map(kIOMapAnywhere | kIOMapInhibitCache);
    doorbellMap_ = doorbellMemory_->map(kIOMapAnywhere | kIOMapInhibitCache);
    if (!sharedMap_ || !doorbellMap_ || !sharedMap_->getVirtualAddress() ||
        !doorbellMap_->getVirtualAddress()) return reject(Error::Mapping);
    accessor_.shared = reinterpret_cast<volatile uint8_t *>(sharedMap_->getVirtualAddress());
    accessor_.bytes = uint32_t(d.length);
    accessor_.doorbell = reinterpret_cast<volatile uint64_t *>(doorbellMap_->getVirtualAddress() + bellOffset);
    return true;
}
bool Channel::enter() {
    if (!threadContext()) return reject(Error::WrongContext);
    if (!lock_) return reject(Error::NotInitialized);
    IOLockLock(lock_);
    if (!discovered_) { reject(Error::NotInitialized); IOLockUnlock(lock_); return false; }
    return true;
}
bool Channel::readCaps(uint32_t cpu, zencppc::Caps *out) {
    if (!enter()) return false;
    const bool ok = engine_.readCaps(cpu, out);
    publish(); IOLockUnlock(lock_);
    return ok;
}
bool Channel::readState(uint32_t cpu, uint32_t *enable, uint32_t *autonomy, zencppc::Request *cur) {
    if (!enter()) return false;
    const bool ok = engine_.readState(cpu, enable, autonomy, cur);
    publish(); IOLockUnlock(lock_);
    return ok;
}
bool Channel::enableAutonomous(const zencppc::Request *requests, uint32_t ncpu) {
    if (!enter()) return false;
    const bool ok = engine_.enableAutonomous(requests, ncpu);
    publish(); IOLockUnlock(lock_);
    return ok;
}
bool Channel::apply(const zencppc::Request *requests, const bool *changed, uint32_t ncpu) {
    if (!enter()) return false;
    const bool ok = engine_.apply(requests, changed, ncpu);
    publish(); IOLockUnlock(lock_);
    return ok;
}
void Channel::unmapLocked() {
    discovered_ = false;
    accessor_.shared = nullptr;
    accessor_.doorbell = nullptr;
    accessor_.bytes = 0;
    OSSafeReleaseNULL(doorbellMap_);
    OSSafeReleaseNULL(sharedMap_);
    OSSafeReleaseNULL(doorbellMemory_);
    OSSafeReleaseNULL(sharedMemory_);
    // A competing object cannot claim or rearm the physical subspace, even
    // after this object's quarantine/teardown. Retire only after every release.
    if (claimed_) { subspaceClaim.retire(); claimed_ = false; }
}
void Channel::teardown() {
    if (!threadContext()) { reject(Error::WrongContext); return; }
    if (!lock_) return;
    IOLockLock(lock_);
    unmapLocked(); // no command, retry, reset, or firmware-state restoration
    IOLockUnlock(lock_);
}

bool Channel::MappedAccessor::read16(uint32_t offset, uint16_t &value) {
    if (!shared || offset % 2 || bytes < 2 || offset > bytes - 2) return false;
    value = *reinterpret_cast<volatile uint16_t *>(shared + offset);
    return true;
}
bool Channel::MappedAccessor::write16(uint32_t offset, uint16_t value) {
    if (!shared || offset % 2 || bytes < 2 || offset > bytes - 2) return false;
    *reinterpret_cast<volatile uint16_t *>(shared + offset) = value;
    return true;
}
bool Channel::MappedAccessor::read32(uint32_t offset, uint32_t &value) {
    if (!shared || offset % 4 || bytes < 4 || offset > bytes - 4) return false;
    value = *reinterpret_cast<volatile uint32_t *>(shared + offset);
    return true;
}
bool Channel::MappedAccessor::write32(uint32_t offset, uint32_t value) {
    if (!shared || offset % 4 || bytes < 4 || offset > bytes - 4) return false;
    *reinterpret_cast<volatile uint32_t *>(shared + offset) = value;
    return true;
}
bool Channel::MappedAccessor::read64(uint64_t &value) {
    if (!doorbell) return false;
    value = *doorbell;
    return true;
}
bool Channel::MappedAccessor::write64(uint64_t value) {
    if (!doorbell) return false;
    *doorbell = value;
    return true;
}
void Channel::MappedAccessor::barrier() {
    // x86 UC MMIO plus an explicit full fence; no SSE or floating-point state.
    __asm__ volatile("mfence" ::: "memory");
}
uint64_t Channel::MappedAccessor::nowUs() {
    uint64_t ticks, ns;
    clock_get_uptime(&ticks);
    absolutetime_to_nanoseconds(ticks, &ns);
    return ns / 1000;
}
void Channel::MappedAccessor::waitUs(uint32_t us) {
    // Short initial completion polls use IODelay; later polls and spacing waits
    // yield in 1-ms chunks. Callers passed the thread/IF/preemption check.
    if (us >= 1000) IOSleep(us / 1000);
    else if (us) IODelay(us);
}
} // namespace zenpcc
