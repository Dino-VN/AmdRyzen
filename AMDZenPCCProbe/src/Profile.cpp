#include "Profile.hpp"
#include "StaticCpc.inc"

namespace pcc_probe {
namespace {
bool signature(Bytes b, const char *s) {
    if (!b.data || b.size < 4) return false;
    for (size_t i = 0; i < 4; ++i) if (b.data[i] != static_cast<uint8_t>(s[i])) return false;
    return true;
}
bool validSsdt(Bytes b, size_t expected) {
    if (!signature(b, "SSDT") || b.size != expected || b.data[8] != 2) return false;
    uint32_t length = 0;
    for (size_t i = 0; i < 4; ++i) length |= uint32_t(b.data[4+i]) << (i*8);
    if (length != b.size) return false;
    uint8_t sum = 0;
    for (size_t i = 0; i < b.size; ++i) sum = static_cast<uint8_t>(sum + b.data[i]);
    return sum == 0;
}
bool brandMatches(const char (&brand)[64]) {
    constexpr char expected[] = "AMD Ryzen 7 5700X 8-Core Processor";
    size_t at = 0;
    while (at < sizeof(brand) && brand[at] == ' ') ++at;
    for (size_t i = 0; i < sizeof(expected)-1; ++i)
        if (at >= sizeof(brand) || brand[at++] != expected[i]) return false;
    while (at < sizeof(brand) && brand[at] == ' ') ++at;
    return at < sizeof(brand) && brand[at] == '\0';
}
Result invalid(Workspace &w, Result result, cppc_lab::Code code) {
    w.discovery.cpuCount = 0;
    w.discovery.domainCount = 0;
    w.detail = static_cast<uint32_t>(code);
    return result;
}
}
Bytes normalizedProfile() {
    return {reinterpret_cast<const uint8_t *>(kStaticCpc), sizeof(kStaticCpc)-1};
}
void considerTable(Bytes bytes, TableSet &t) {
    if (signature(bytes, "PCCT")) { ++t.pcctCount; t.pcct = bytes; }
    else if (signature(bytes, "APIC")) { ++t.madtCount; t.madt = bytes; }
    else if (signature(bytes, "SSDT")) {
        if (hashMatches(bytes, kProcessorHash)) { ++t.processorCount; t.processors = bytes; }
        if (hashMatches(bytes, kCpcHash)) { ++t.cpcCount; t.cpc = bytes; }
    }
}
Result validateProfile(const TableSet &t, Workspace &w) {
    using namespace cppc_lab;
    w.discovery.cpuCount = 0;
    w.discovery.domainCount = 0;
    w.detail = 0;
    if (t.pcctCount != 1 || t.madtCount != 1 || t.processorCount != 1 || t.cpcCount != 1)
        return invalid(w, Result::ProfileMismatch, Code::MissingIdentity);
    if (!validSsdt(t.processors, 252) || !validSsdt(t.cpc, 16451) ||
        !hashMatches(t.processors, kProcessorHash) || !hashMatches(t.cpc, kCpcHash) ||
        !hashMatches(normalizedProfile(), kNormalizedHash))
        return invalid(w, Result::ProfileMismatch, Code::Checksum);
    const AMDZenInfo &info = w.info;
    if (info.abiVersion != AMDZEN_ABI_VERSION || info.structSize != sizeof(AMDZenInfo) ||
        info.vendor != 0 || info.family != 0x19 || info.model != 0x21 || !brandMatches(info.brand) ||
        info.logicalCpus != kCpuCount || info.physicalCores != 8 || info.threadsPerCore != 2 ||
        info.controlBlockReason == kAMDZenBlockHypervisor)
        return invalid(w, Result::IdentityMismatch, Code::MissingIdentity);
    Code code = parsePcct(t.pcct, w.pcct);
    if (code != Code::Ok) return invalid(w, Result::DescriptorInvalid, code);
    // Exact known generic polling layout, but NEVER fixed shared/doorbell addresses.
    const ChannelDescriptor &channel = w.pcct.channels[0];
    if (w.pcct.count != 1 || channel.length != 65536 || channel.doorbell.width != 64 ||
        channel.doorbell.access != 4 || channel.preserveMask != UINT64_C(0xffffffff00000000) ||
        channel.writeMask != 1 || channel.nominalLatencyUs != 4000 ||
        channel.maxCommandsPerMinute != 60000 || channel.minTurnaroundUs != 0 ||
        channel.baseAddress > (UINT64_C(1) << 52) - channel.length ||
        channel.doorbell.address > (UINT64_C(1) << 52) - 8)
        return invalid(w, Result::ProfileMismatch, Code::Unsupported);
    code = parseMadt(t.madt, w.madt);
    if (code != Code::Ok) return invalid(w, Result::DescriptorInvalid, code);
    code = parseStaticCpc(normalizedProfile(), w.normalized);
    if (code != Code::Ok) return invalid(w, Result::ProfileMismatch, code);
    w.runtimeMap.count = info.logicalCpus;
    for (uint32_t cpu = 0; cpu < info.logicalCpus; ++cpu) {
        if (info.coreOfCpu[cpu] >= info.physicalCores)
            return invalid(w, Result::IdentityMismatch, Code::Bounds);
        // AMDZenPM's ABI explicitly indexes apicId by cpu_number(); no name/order inference.
        w.runtimeMap.entries[cpu] = {info.apicId[cpu], cpu};
    }
    code = importDescriptors(w.normalized, w.pcct, w.madt, w.runtimeMap, w.discovery);
    if (code != Code::Ok) return invalid(w, Result::IdentityMismatch, code);
    if (w.discovery.cpuCount != kCpuCount || w.discovery.domainCount != 8)
        return invalid(w, Result::ProfileMismatch, Code::Domain);
    // HW_ALL pairs must also match the independent runtime physical-core identity.
    for (size_t a = 0; a < w.discovery.cpuCount; ++a) {
        const CpuDescriptor &cpu = w.discovery.cpus[a];
        for (size_t b = 0; b < a; ++b) {
            const CpuDescriptor &peer = w.discovery.cpus[b];
            if ((cpu.domain == peer.domain) !=
                (info.coreOfCpu[cpu.xnuCpu] == info.coreOfCpu[peer.xnuCpu]))
                return invalid(w, Result::IdentityMismatch, Code::Domain);
        }
    }
    return Result::Ok;
}
void describe(const Request &request, const TableSet &tables, const Workspace &w, Reply &out) {
    using namespace cppc_lab;
    if (w.discovery.cpuCount != kCpuCount) return;
    const CpuDescriptor *cpu = nullptr;
    for (size_t i = 0; i < w.discovery.cpuCount; ++i)
        if (w.discovery.cpus[i].xnuCpu == request.cpu) cpu = &w.discovery.cpus[i];
    if (!cpu) return;
    const ChannelDescriptor &c = w.discovery.pcct.channels[0];
    out.flags |= MetadataValid;
    out.logicalCpus = static_cast<uint32_t>(w.discovery.cpuCount);
    out.domains = static_cast<uint32_t>(w.discovery.domainCount);
    out.acpiId = cpu->acpiId; out.apicId = cpu->apicId; out.domain = cpu->domain;
    out.headerBytes = kPccHeaderBytes;
    out.sharedBase = c.baseAddress; out.sharedBytes = c.length;
    out.doorbellAddress = c.doorbell.address;
    out.doorbellPreserve = c.preserveMask; out.doorbellWrite = c.writeMask;
    out.doorbellWidth = c.doorbell.width;
    out.nominalLatencyUs = c.nominalLatencyUs;
    out.maxCommandsPerMinute = c.maxCommandsPerMinute; out.minTurnaroundUs = c.minTurnaroundUs;
    for (size_t i = 0; i < sizeof(out.acpiPath); ++i) out.acpiPath[i] = cpu->path[i];
    sha256(tables.processors, out.processorTableSha256); sha256(tables.cpc, out.cpcTableSha256);
    for (uint32_t i = 0; i < kFields; ++i) {
        if ((request.fieldMask & (1u << i)) == 0) continue;
        const Field &f = cpu->fields[i];
        FieldView &view = out.fields[i];
        view.descriptorKind = static_cast<uint32_t>(f.kind);
        view.bitWidth = f.gas.width; view.addressSpace = f.gas.space;
        view.descriptorAddress = f.gas.address; view.byteOffset = f.byteOffset;
        if (f.kind == FieldKind::Integer) {
            view.state = static_cast<uint32_t>(ValueState::StaticInteger);
            view.value = f.integer;
        } else if (f.kind == FieldKind::Unsupported) {
            view.state = static_cast<uint32_t>(ValueState::Unsupported);
        }
    }
}
} // namespace pcc_probe
