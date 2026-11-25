#include "Options.hpp"
#include "Profile.hpp"
#include "ServiceLookup.hpp"
#include <CoreFoundation/CoreFoundation.h>
#include <IOKit/IOKitLib.h>
#include <inttypes.h>
#include <memory>
#include <new>
#include <stdio.h>
#include <unistd.h>

namespace {
using namespace pcc_probe;
// No matching notifications/waits; success + NULL or a valid empty iterator
// means absence. Allocation/API/enumeration errors must never trigger INFO fallback.
struct IOKitLookup {
    static constexpr int32_t noMemory = static_cast<int32_t>(kIOReturnNoMemory);
    static constexpr int32_t badIterator = static_cast<int32_t>(kIOReturnError);
    CFMutableDictionaryRef matching(const char *name) { return IOServiceMatching(name); }
    int32_t enumerate(CFMutableDictionaryRef match, uint32_t &iterator) {
        return IOServiceGetMatchingServices(kIOMainPortDefault, match, &iterator);
    }
    uint32_t next(uint32_t iterator) { return IOIteratorNext(iterator); }
    bool valid(uint32_t iterator) { return IOIteratorIsValid(iterator); }
    void release(uint32_t object) { (void)IOObjectRelease(object); }
};
ServiceLookup uniqueService(const char *name) {
    IOKitLookup api;
    return lookupUniqueService(api, name);
}
Result directInfo(const Request &request, Reply &reply) {
    auto w = std::unique_ptr<Workspace>(new (std::nothrow) Workspace{});
    if (!w) return Result::MetadataUnavailable;
    const ServiceLookup pm = uniqueService(AMDZEN_SERVICE_CLASS);
    if (pm.state != LookupState::Found) return Result::MetadataUnavailable;
    io_connect_t connection = IO_OBJECT_NULL;
    kern_return_t kr = IOServiceOpen(pm.service, mach_task_self(), 0, &connection);
    IOObjectRelease(pm.service);
    if (kr != KERN_SUCCESS) return Result::MetadataUnavailable;
    size_t size = sizeof(w->info);
    kr = IOConnectCallStructMethod(connection, kAMDZenMethodGetInfo, nullptr, 0, &w->info, &size);
    IOServiceClose(connection);
    if (kr != KERN_SUCCESS || size != sizeof(w->info)) return Result::MetadataUnavailable;
    const ServiceLookup platform = uniqueService("AppleACPIPlatformExpert");
    if (platform.state != LookupState::Found) return Result::MetadataUnavailable;
    // Direct lookup is intentional: ordinary ioreg property dumps omit these values.
    CFTypeRef property = IORegistryEntryCreateCFProperty(platform.service, CFSTR("ACPI Tables"), kCFAllocatorDefault, 0);
    IOObjectRelease(platform.service);
    if (!property) return Result::MetadataUnavailable;
    if (CFGetTypeID(property) != CFDictionaryGetTypeID()) { CFRelease(property); return Result::MetadataUnavailable; }
    CFDictionaryRef tables = static_cast<CFDictionaryRef>(property);
    const CFIndex count = CFDictionaryGetCount(tables);
    if (count <= 0 || count > 128) { CFRelease(property); return Result::MetadataUnavailable; }
    const void *values[128]{};
    CFDictionaryGetKeysAndValues(tables, nullptr, values);
    TableSet selected{};
    bool valid = true;
    size_t total = 0;
    for (CFIndex i = 0; i < count; ++i) {
        const CFTypeRef value = values[i];
        if (CFGetTypeID(value) != CFDataGetTypeID()) { valid = false; break; }
        const auto data = static_cast<CFDataRef>(value);
        const CFIndex length = CFDataGetLength(data);
        if (length < 4 || length > static_cast<CFIndex>(cppc_lab::kMaxTableBytes)) { valid = false; break; }
        total += static_cast<size_t>(length);
        if (total > 2u * 1024u * 1024u) { valid = false; break; }
        considerTable({CFDataGetBytePtr(data), static_cast<size_t>(length)}, selected);
    }
    const Result result = valid ? validateProfile(selected, *w) : Result::MetadataUnavailable;
    if (result == Result::Ok) describe(request, selected, *w, reply);
    reply.detail = w->detail;
    CFRelease(property);
    return result;
}
void print(const Reply &out, const char *source, bool probePresent) {
    printf("source: %s\n", source);
    printf("result: %s (descriptor detail %u)\n", resultName(static_cast<Result>(out.result)), out.detail);
    if (out.physicalTransportAvailable != 0)
        puts("physical transport: advertised by service; not independently verified by this client");
    else puts("physical transport: unavailable in this release; no exclusive OS PCC lease/power fence established");
    printf("ownership observation: %s\n", ownershipName(static_cast<Ownership>(out.ownership)));
    puts("metadata copies and AMDZenPM cached topology are NOT fresh CPPC measurements");
    if (probePresent) {
        printf("gates: boot=%s config=%s; attempted=%s submitted=%s; may-own=%s quarantined=%s\n",
               (out.flags & BootEnabled) ? "on" : "off", (out.flags & ConfigEnabled) ? "on" : "off",
               (out.flags & Attempted) ? "yes" : "no", (out.flags & Submitted) ? "yes" : "no",
               (out.flags & PlatformMayOwn) ? "yes" : "no", (out.flags & Quarantined) ? "yes" : "no");
    } else puts("probe: not present; boot/config gate state unknown (client did not load it)");
    if ((out.flags & (ServiceStopped | PowerInvalidated)) != 0)
        puts("lifecycle invalidated: no new operations; descriptors may be stale");
    if ((out.flags & MetadataValid) == 0) {
        puts("validated current descriptors: unavailable; capability/control values: unknown");
        return;
    }
    printf("profile: 5700X 19h/21h, %u logical CPUs, %u HW_ALL domains\n", out.logicalCpus, out.domains);
    printf("identity: XNU %u <-> APIC %u <-> ACPI %u %s; domain %u\n",
           out.cpu, out.apicId, out.acpiId, out.acpiPath, out.domain);
    printf("PCC subspace %u, header %u bytes; nominal latency %u us (NOT a timeout), max %u/min, turnaround %u us\n",
           out.subspace, out.headerBytes, out.nominalLatencyUs, out.maxCommandsPerMinute, out.minTurnaroundUs);
    printf("current-table address metadata only: shared=0x%" PRIx64 " bytes=%" PRIu64 ", doorbell=0x%" PRIx64 " width=%u\n",
           out.sharedBase, out.sharedBytes, out.doorbellAddress, out.doorbellWidth);
    printf("doorbell masks: preserve=0x%" PRIx64 " write=0x%" PRIx64 " (not applied)\n",
           out.doorbellPreserve, out.doorbellWrite);
    for (uint32_t i = 0; i < kFields; ++i) {
        if ((out.fieldMask & (1u << i)) == 0) continue;
        const FieldView &f = out.fields[i];
        printf("%-23s %s", fieldOption(i), valueStateName(static_cast<ValueState>(f.state)));
        if (f.state == static_cast<uint32_t>(ValueState::StaticInteger) ||
            f.state == static_cast<uint32_t>(ValueState::FirmwareRead)) printf(" = %" PRIu64, f.value);
        if (f.descriptorKind == static_cast<uint32_t>(DescriptorKind::Pcc))
            printf("; PCC offset=0x%" PRIx64 " width=%u", f.descriptorAddress, f.bitWidth);
        if (f.descriptorKind == static_cast<uint32_t>(DescriptorKind::Ffh))
            printf("; FFH identifier=0x%" PRIx64 " (metadata only; not sampled)", f.descriptorAddress);
        putchar('\n');
    }
}
}
int main(int argc, const char *argv[]) {
    using namespace pcc_probe;
    Options options{};
    if (!parseOptions(argc, argv, options)) {
        fputs("Invalid arguments; no method called. Use pccprobe --help.\n", stderr); return 2;
    }
    if (options.help) { printHelp(); return 0; }
    if (options.selector == Selector::ReadOnce && geteuid() != 0) {
        fputs("READ requires an administrator. No method called; no automatic privilege escalation.\n", stderr); return 3;
    }
    Reply reply{};
    initializeReply(options.request, reply);
    const ServiceLookup lookup = uniqueService(kService);
    if (lookup.state == LookupState::Error) {
        fprintf(stderr, "Probe lookup failed (0x%x); absence not established. No fallback or method call.\n",
                static_cast<unsigned>(lookup.error)); return 3;
    }
    if (lookup.state == LookupState::Ambiguous) {
        fputs("Ambiguous probe instances; no method called.\n", stderr); return 3;
    }
    if (lookup.state == LookupState::Absent) {
        if (options.selector != Selector::Info) {
            fputs("Probe unavailable. This client never installs or loads it.\n", stderr); return 3;
        }
        reply.result = static_cast<uint32_t>(directInfo(options.request, reply));
        if (!validateReply(options.request, options.selector, reply, sizeof(reply))) {
            fputs("Invalid local metadata result.\n", stderr); return 4;
        }
        print(reply, "current OS tables + existing AMDZenPM GetInfo", false);
    } else {
        io_connect_t connection = IO_OBJECT_NULL;
        kern_return_t kr = IOServiceOpen(lookup.service, mach_task_self(), 0, &connection);
        IOObjectRelease(lookup.service);
        if (kr != KERN_SUCCESS) {
            fprintf(stderr, "Probe open refused (0x%x). No command submitted by client.\n", static_cast<unsigned>(kr)); return 3;
        }
        size_t size = sizeof(reply);
        kr = IOConnectCallStructMethod(connection, static_cast<uint32_t>(options.selector),
                                       &options.request, sizeof(options.request), &reply, &size);
        IOServiceClose(connection);
        if (kr != KERN_SUCCESS) {
            fprintf(stderr, "Method error 0x%x; response/ownership unknown. No retry.\n", static_cast<unsigned>(kr)); return 4;
        }
        if (!validateReply(options.request, options.selector, reply, size)) {
            fputs("Invalid response ABI/state; values and ownership unknown. No retry.\n", stderr); return 4;
        }
        print(reply, options.selector == Selector::Info ? "probe metadata/status only" : "explicit one-shot READ request", true);
    }
    return reply.result == static_cast<uint32_t>(Result::Ok) ? 0 : 5;
}
