#include "CppcLab.hpp"

namespace cppc_lab {
namespace {

uint64_t le(const uint8_t *p, size_t count) {
    uint64_t value = 0;
    for (size_t i = 0; i < count; ++i) value |= uint64_t(p[i]) << (i * 8);
    return value;
}
bool zero(const uint8_t *p, size_t count) {
    for (size_t i = 0; i < count; ++i) if (p[i] != 0) return false;
    return true;
}
bool sameString(const char *a, const char *b) {
    for (size_t i = 0; i < kPathBytes; ++i) {
        if (a[i] != b[i]) return false;
        if (a[i] == '\0') return true;
    }
    return false;
}
void copyPath(char *out, const char *in) {
    for (size_t i = 0; i < kPathBytes; ++i) out[i] = in[i];
}
bool pathValid(const uint8_t *p, size_t size) {
    if (size < 5 || size >= kPathBytes || p[0] != '\\') return false;
    size_t at = 1;
    while (at < size) {
        if (size - at < 4) return false;
        for (size_t i = 0; i < 4; ++i) {
            const uint8_t c = p[at + i];
            if (!(c == '_' || (c >= 'A' && c <= 'Z') ||
                  (i != 0 && c >= '0' && c <= '9'))) return false;
        }
        at += 4;
        if (at == size) return true;
        if (p[at++] != '.' || at == size) return false;
    }
    return false;
}
bool pathValid(const char *path) {
    size_t size = 0;
    while (size < kPathBytes && path[size] != '\0') ++size;
    return pathValid(reinterpret_cast<const uint8_t *>(path), size);
}
bool tokenIs(Bytes token, const char *word) {
    size_t i = 0;
    while (word[i] != '\0') {
        if (i == token.size || token.data[i] != static_cast<uint8_t>(word[i])) return false;
        ++i;
    }
    return i == token.size;
}
bool whitespace(uint8_t c) {
    return c == ' ' || c == '\n' || c == '\r' || c == '\t';
}
class Lexer {
public:
    explicit Lexer(Bytes input) : input_(input) {}
    Bytes next() {
        while (at_ < input_.size) {
            if (whitespace(input_.data[at_])) { ++at_; continue; }
            if (input_.data[at_] != '#') break;
            while (at_ < input_.size && input_.data[at_] != '\n') ++at_;
        }
        const size_t start = at_;
        while (at_ < input_.size && !whitespace(input_.data[at_]) &&
               input_.data[at_] != '#') ++at_;
        return {input_.data + start, at_ - start};
    }
    bool expect(const char *word) { return tokenIs(next(), word); }
private:
    Bytes input_;
    size_t at_ = 0;
};
int hexDigit(uint8_t c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}
Code number(Bytes token, uint64_t &out) {
    if (token.size == 0) return Code::Syntax;
    size_t at = 0;
    uint64_t base = 10, value = 0;
    if (token.size >= 2 && token.data[0] == '0' &&
        (token.data[1] == 'x' || token.data[1] == 'X')) { base = 16; at = 2; }
    if (at == token.size) return Code::Syntax;
    for (; at < token.size; ++at) {
        const int digit = hexDigit(token.data[at]);
        if (digit < 0 || static_cast<uint64_t>(digit) >= base) return Code::Syntax;
        const uint64_t d = static_cast<uint64_t>(digit);
        if (value > (UINT64_MAX - d) / base) return Code::Overflow;
        value = value * base + d;
    }
    out = value;
    return Code::Ok;
}
Code next32(Lexer &lexer, uint32_t &out) {
    uint64_t n = 0;
    const Code code = number(lexer.next(), n);
    if (code != Code::Ok) return code;
    if (n > UINT32_MAX) return Code::Overflow;
    out = static_cast<uint32_t>(n);
    return Code::Ok;
}
Code textValid(Bytes text) {
    if (text.size > kMaxTextBytes) return Code::Capacity;
    if (!text.data || text.size == 0) return Code::Truncated;
    return Code::Ok;
}
Code tableHeader(Bytes table, const char *signature, size_t minimum) {
    if (table.size > kMaxTableBytes) return Code::Capacity;
    if (!table.data || table.size < 36) return Code::Truncated;
    for (size_t i = 0; i < 4; ++i)
        if (table.data[i] != static_cast<uint8_t>(signature[i])) return Code::Signature;
    const uint64_t length = le(table.data + 4, 4);
    if (length > kMaxTableBytes) return Code::Capacity;
    if (length < minimum) return Code::Length;
    if (length > table.size) return Code::Truncated;
    if (length != table.size) return Code::Length;
    uint8_t sum = 0;
    for (size_t i = 0; i < table.size; ++i) sum = static_cast<uint8_t>(sum + table.data[i]);
    return sum == 0 ? Code::Ok : Code::Checksum;
}
Gas gasAt(const uint8_t *p) {
    return {p[0], p[1], p[2], p[3], le(p + 4, 8)};
}
bool fullWidth(uint8_t width) {
    return width == 8 || width == 16 || width == 32 || width == 64;
}
uint64_t widthMask(uint8_t width) {
    return width == 64 ? UINT64_MAX : (uint64_t(1) << width) - 1;
}
bool overlaps(uint64_t a, uint64_t aLength, uint64_t b, uint64_t bLength) {
    // Validated nonempty lengths; subtraction avoids endpoint overflow.
    return a <= b ? b - a < aLength : a - b < bLength;
}
Code validatePcct(const Pcct &pcct) {
    if (pcct.count == 0) return Code::Subspace;
    if (pcct.count > kMaxSubspaces) return Code::Capacity;
    for (size_t i = 0; i < pcct.count; ++i) {
        const ChannelDescriptor &a = pcct.channels[i];
        Code code = validateChannel(a);
        if (code != Code::Ok) return code;
        for (size_t j = 0; j < i; ++j) {
            const ChannelDescriptor &b = pcct.channels[j];
            if (overlaps(a.baseAddress, a.length, b.baseAddress, b.length) ||
                overlaps(a.doorbell.address, a.doorbell.width / 8,
                         b.doorbell.address, b.doorbell.width / 8) ||
                overlaps(a.baseAddress, a.length, b.doorbell.address, b.doorbell.width / 8) ||
                overlaps(b.baseAddress, b.length, a.doorbell.address, a.doorbell.width / 8))
                return Code::Aliasing;
        }
    }
    return Code::Ok;
}
Code validateMadt(const Madt &madt) {
    if (madt.count == 0) return Code::MissingIdentity;
    if (madt.count > kMaxMadtCpus) return Code::Capacity;
    for (size_t i = 0; i < madt.count; ++i) {
        const MadtCpu &a = madt.cpus[i];
        if (a.kind != IdentityKind::Lapic && a.kind != IdentityKind::X2Apic)
            return Code::Unsupported;
        for (size_t j = 0; j < i; ++j) {
            const MadtCpu &b = madt.cpus[j];
            if ((a.kind == b.kind && a.acpiId == b.acpiId) ||
                (a.enabled && b.enabled && a.apicId == b.apicId))
                return Code::DuplicateIdentity;
        }
    }
    return Code::Ok;
}
// These roles have control/acknowledgement semantics in the CPPC reference.
// This is only layout validation: the lab still has no control-write command.
bool writableRole(size_t index) {
    switch (index) {
    case Desired: case Minimum: case Maximum: case PerformanceLimited:
    case Enable: case AutonomousSelection: case AutonomousWindow: case Epp:
        return true;
    default:
        return false;
    }
}
Code validatePccFieldAliases(const CpuDescriptor *cpus, size_t count) {
    for (size_t cpu = 0; cpu < count; ++cpu) {
        for (size_t field = 0; field < kFieldCount; ++field) {
            const Field &a = cpus[cpu].fields[field];
            if (a.kind != FieldKind::PccRegister) continue;
            for (size_t peer = 0; peer <= cpu; ++peer) {
                const size_t end = peer == cpu ? field : kFieldCount;
                for (size_t other = 0; other < end; ++other) {
                    const Field &b = cpus[peer].fields[other];
                    if (b.kind != FieldKind::PccRegister || a.gas.access != b.gas.access ||
                        (!writableRole(field) && !writableRole(other))) continue;
                    if (!overlaps(a.byteOffset, uint64_t(a.gas.width) / 8,
                                  b.byteOffset, uint64_t(b.gas.width) / 8)) continue;
                    // Shared same-role registers are allowed across CPUs; a
                    // different role, width or partial overlap is not an alias.
                    if (field == other && a.byteOffset == b.byteOffset &&
                        a.gas.width == b.gas.width && a.gas.bitOffset == b.gas.bitOffset)
                        continue;
                    return Code::Aliasing;
                }
            }
        }
    }
    return Code::Ok;
}
bool optional(size_t index) { return (uint32_t(0x7fc7d0) & (uint32_t(1) << index)) != 0; }
Code importField(const StaticEntry &entry, size_t index, const Pcct &pcct, Field &out) {
    out = {};
    if (entry.kind == EntryKind::Dynamic) return Code::Unsupported;
    if (entry.kind == EntryKind::Integer) {
        const uint64_t value = entry.integer;
        bool absent = false;
        switch (index) {
        case Highest: case Nominal: case LowestNonlinear: case Lowest:
            if (value == 0) return Code::Unsupported;
            if (value > UINT32_MAX) return Code::Overflow;
            break;
        case ReferencePerformance: case LowestFrequency: case NominalFrequency:
            if (value > UINT32_MAX) return Code::Overflow;
            absent = value == 0;
            break;
        case WraparoundTime: absent = value == 0; break;
        case AutonomousSelection:
            if (value > 1) return Code::Gas;
            break; // Integer 0 here is a KNOWN false, not an absent register.
        default:
            if (value != 0 || !optional(index)) return Code::Unsupported;
            absent = true;
            break;
        }
        out.kind = absent ? FieldKind::Unsupported : FieldKind::Integer;
        out.integer = absent ? 0 : value;
        return Code::Ok;
    }
    if (entry.kind != EntryKind::Buffer) return Code::Unsupported;
    Gas gas{};
    Code code = parseGasResource({entry.buffer, entry.size}, gas);
    if (code != Code::Ok) return code;
    out.gas = gas;
    if (gas.space == kSystemMemory && gas.width == 0 && gas.bitOffset == 0 &&
        gas.access == 0 && gas.address == 0)
        return optional(index) ? Code::Ok : Code::Unsupported;
    if (gas.width == 0) return Code::Gas; // PCC offset zero is NOT a null GAS.
    if (gas.space == kPcc) {
        if (gas.access >= pcct.count) return Code::Subspace;
        if (gas.width > 32 && index != ReferenceCounter && index != DeliveredCounter &&
            index != WraparoundTime) return Code::Gas;
        uint8_t bytes = 0;
        code = pccLocation(gas, pcct.channels[gas.access], gas.access, out.byteOffset, bytes);
        if (code != Code::Ok) return code;
        out.kind = FieldKind::PccRegister;
        return Code::Ok;
    }
    if (gas.space == kFfh) {
        if (gas.width != 64 || gas.bitOffset != 0 || gas.access != 4) return Code::Gas;
        if (!((index == ReferenceCounter && gas.address == 0xe7) ||
              (index == DeliveredCounter && gas.address == 0xe8))) return Code::Unsupported;
        out.kind = FieldKind::FfhCounter; // Metadata only; no MSR reader exists.
        return Code::Ok;
    }
    return Code::Unsupported;
}

} // namespace

bool checkedAdd(uint64_t a, uint64_t b, uint64_t &result) {
    if (a > UINT64_MAX - b) return false;
    result = a + b;
    return true;
}

const char *codeName(Code code) {
    switch (code) {
#define CODE_NAME(name) case Code::name: return #name
    CODE_NAME(Ok); CODE_NAME(Syntax); CODE_NAME(Unsupported); CODE_NAME(Capacity);
    CODE_NAME(Truncated); CODE_NAME(Length); CODE_NAME(Signature); CODE_NAME(Checksum);
    CODE_NAME(Revision); CODE_NAME(Reserved); CODE_NAME(Gas); CODE_NAME(Overflow);
    CODE_NAME(Bounds); CODE_NAME(Alignment); CODE_NAME(Subspace); CODE_NAME(Aliasing);
    CODE_NAME(MissingIdentity); CODE_NAME(DuplicateIdentity); CODE_NAME(DisabledIdentity);
    CODE_NAME(Domain); CODE_NAME(NotInitialized); CODE_NAME(AlreadyInitialized);
    CODE_NAME(Busy); CODE_NAME(Pending); CODE_NAME(FirmwareError); CODE_NAME(Timeout);
    CODE_NAME(Quarantined); CODE_NAME(LateComplete); CODE_NAME(LateError);
    CODE_NAME(Protocol); CODE_NAME(Faulted); CODE_NAME(RateLimited); CODE_NAME(Turnaround);
    CODE_NAME(TimeBackwards); CODE_NAME(Deadline); CODE_NAME(NoOutstanding);
    CODE_NAME(UnknownValue);
#undef CODE_NAME
    }
    return "InvalidCode";
}

const char *fieldName(size_t index) {
    static const char *const names[kFieldCount] = {
        "HighestPerformance", "NominalPerformance", "LowestNonlinearPerformance",
        "LowestPerformance", "GuaranteedPerformance", "DesiredPerformance",
        "MinimumPerformance", "MaximumPerformance", "PerformanceReductionTolerance",
        "TimeWindow", "CounterWraparoundTime", "ReferenceCounter", "DeliveredCounter",
        "PerformanceLimited", "CPPCEnable", "AutonomousSelectionEnable",
        "AutonomousActivityWindow", "EnergyPerformancePreference", "ReferencePerformance",
        "LowestFrequency", "NominalFrequency"
    };
    return index < kFieldCount ? names[index] : "InvalidField";
}

Code decodeHex(Bytes text, uint8_t *out, size_t capacity, size_t &size) {
    size = 0;
    Code code = textValid(text);
    if (code != Code::Ok) return code;
    if (!out) return Code::Bounds;
    Lexer lexer(text);
    size_t count = 0;
    for (Bytes token = lexer.next(); token.size != 0; token = lexer.next()) {
        if (token.size != 2) return Code::Syntax;
        const int hi = hexDigit(token.data[0]), lo = hexDigit(token.data[1]);
        if (hi < 0 || lo < 0) return Code::Syntax;
        if (count == capacity) return Code::Capacity;
        out[count++] = static_cast<uint8_t>(hi * 16 + lo);
    }
    if (count == 0) return Code::Truncated;
    size = count;
    return Code::Ok;
}

Code parseGasResource(Bytes bytes, Gas &out) {
    out = {};
    if (!bytes.data || bytes.size != 17) return Code::Length;
    if (bytes.data[0] != 0x82 || le(bytes.data + 1, 2) != 12 || bytes.data[15] != 0x79)
        return Code::Gas;
    // ACPI EndTag checksum zero means checksum omitted, NOT that sum is zero.
    if (bytes.data[16] != 0) {
        uint8_t sum = 0;
        for (size_t i = 0; i < bytes.size; ++i) sum = static_cast<uint8_t>(sum + bytes.data[i]);
        if (sum != 0) return Code::Checksum;
    }
    out = gasAt(bytes.data + 3);
    return Code::Ok;
}

Code validateChannel(const ChannelDescriptor &channel) {
    if (channel.length < kPccHeaderBytes) return Code::Bounds;
    if (channel.length > kMaxSharedBytes) return Code::Capacity;
    if (channel.baseAddress == 0) return Code::Bounds;
    uint64_t last = 0;
    if (!checkedAdd(channel.baseAddress, channel.length - 1, last)) return Code::Overflow;
    if (channel.baseAddress % 8 != 0) return Code::Alignment;
    const Gas &bell = channel.doorbell;
    if (bell.space != kSystemMemory) return Code::Unsupported;
    if (!fullWidth(bell.width) || bell.bitOffset != 0 || bell.access > 4) return Code::Gas;
    if (bell.access != 0 && (uint32_t(8) << (bell.access - 1)) != bell.width) return Code::Gas;
    const uint64_t bytes = bell.width / 8;
    if (bell.address == 0) return Code::Bounds;
    if (!checkedAdd(bell.address, bytes - 1, last)) return Code::Overflow;
    if (bell.address % bytes != 0) return Code::Alignment;
    const uint64_t mask = widthMask(bell.width);
    if (((channel.preserveMask | channel.writeMask) & ~mask) != 0) return Code::Gas;
    if (overlaps(channel.baseAddress, channel.length, bell.address, bytes)) return Code::Aliasing;
    return Code::Ok;
}

Code parsePcct(Bytes table, Pcct &out) {
    out.count = 0;
    Code code = tableHeader(table, "PCCT", 48);
    if (code != Code::Ok) return code;
    if (table.data[8] != 2) return Code::Revision;
    if (le(table.data + 36, 4) != 0) return Code::Unsupported; // No interrupt path.
    if (!zero(table.data + 40, 8)) return Code::Reserved;
    size_t at = 48, count = 0;
    while (at < table.size) {
        if (table.size - at < 2) return Code::Truncated;
        const uint8_t length = table.data[at + 1];
        if (length < 2) return Code::Length;
        if (length > table.size - at) return Code::Truncated;
        if (table.data[at] != 0) return Code::Unsupported;
        if (length != 62) return Code::Length;
        if (count == kMaxSubspaces) return Code::Capacity;
        const uint8_t *p = table.data + at;
        if (!zero(p + 2, 6)) return Code::Reserved;
        ChannelDescriptor &channel = out.channels[count++];
        channel = {le(p + 8, 8), le(p + 16, 8), gasAt(p + 24), le(p + 36, 8),
                   le(p + 44, 8), static_cast<uint32_t>(le(p + 52, 4)),
                   static_cast<uint32_t>(le(p + 56, 4)), static_cast<uint16_t>(le(p + 60, 2))};
        code = validateChannel(channel);
        if (code != Code::Ok) return code;
        at += length;
    }
    out.count = count;
    code = validatePcct(out);
    if (code != Code::Ok) out.count = 0;
    return code;
}

Code pccLocation(const Gas &gas, const ChannelDescriptor &channel, uint8_t subspace,
                 uint64_t &byteOffset, uint8_t &byteWidth) {
    byteOffset = 0;
    byteWidth = 0;
    if (gas.space != kPcc) return Code::Unsupported;
    if (gas.access != subspace) return Code::Subspace;
    if (!fullWidth(gas.width) || gas.bitOffset != 0) return Code::Gas;
    uint64_t start = 0, end = 0;
    if (!checkedAdd(gas.address, kPccHeaderBytes, start) ||
        !checkedAdd(start, gas.width / 8, end)) return Code::Overflow;
    if (end > channel.length || channel.length < kPccHeaderBytes) return Code::Bounds;
    if (start % (gas.width / 8) != 0) return Code::Alignment;
    byteOffset = start;
    byteWidth = static_cast<uint8_t>(gas.width / 8);
    return Code::Ok;
}

Code parseMadt(Bytes table, Madt &out) {
    out.count = 0;
    Code code = tableHeader(table, "APIC", 44);
    if (code != Code::Ok) return code;
    if (table.data[8] < 3 || table.data[8] > 5) return Code::Revision;
    if ((le(table.data + 40, 4) & ~uint64_t(1)) != 0) return Code::Reserved;
    size_t at = 44, count = 0;
    while (at < table.size) {
        if (table.size - at < 2) return Code::Truncated;
        const uint8_t type = table.data[at], length = table.data[at + 1];
        if (length < 2) return Code::Length;
        if (length > table.size - at) return Code::Truncated;
        size_t expected = 0;
        switch (type) {
        case 0: expected = 8; break;
        case 1: expected = 12; break;
        case 2: expected = 10; break;
        case 3: expected = 8; break;
        case 4: expected = 6; break;
        case 5: expected = 12; break;
        case 9: expected = 16; break;
        case 10: expected = 12; break;
        default: return Code::Unsupported; // Never silently skip a CPU-bearing form.
        }
        if (length != expected) return Code::Length;
        const uint8_t *p = table.data + at;
        if (type == 0 || type == 9) {
            if (count == kMaxMadtCpus) return Code::Capacity;
            if (type == 9 && !zero(p + 2, 2)) return Code::Reserved;
            const uint64_t flags = le(p + (type == 0 ? 4 : 8), 4);
            if ((flags & ~uint64_t(3)) != 0) return Code::Reserved;
            out.cpus[count++] = {
                type == 0 ? IdentityKind::Lapic : IdentityKind::X2Apic,
                type == 0 ? uint32_t(p[2]) : static_cast<uint32_t>(le(p + 12, 4)),
                type == 0 ? uint32_t(p[3]) : static_cast<uint32_t>(le(p + 4, 4)),
                (flags & 1) != 0, (flags & 2) != 0
            };
        }
        at += length;
    }
    out.count = count;
    code = validateMadt(out);
    if (code != Code::Ok) out.count = 0;
    return code;
}

Code parseStaticCpc(Bytes text, StaticCpc &out) {
    out.count = 0;
    Code code = textValid(text);
    if (code != Code::Ok) return code;
    Lexer lexer(text);
    if (!lexer.expect("CPPC_STATIC_V1")) return Code::Unsupported;
    if (!lexer.expect("CPUS")) return Code::Syntax;
    uint64_t count = 0;
    code = number(lexer.next(), count);
    if (code != Code::Ok) return code;
    if (count == 0 || count > kMaxCpus) return Code::Capacity;
    for (size_t i = 0; i < count; ++i) {
        StaticCpu &cpu = out.cpus[i];
        cpu = {};
        if (!lexer.expect("CPU")) return Code::Syntax;
        const Bytes path = lexer.next();
        if (!pathValid(path.data, path.size)) return Code::Syntax;
        for (size_t j = 0; j < path.size; ++j) cpu.path[j] = static_cast<char>(path.data[j]);
        const Bytes identity = lexer.next();
        if (tokenIs(identity, "LAPIC")) cpu.identityKind = IdentityKind::Lapic;
        else if (tokenIs(identity, "X2APIC")) cpu.identityKind = IdentityKind::X2Apic;
        else return Code::Unsupported;
        code = next32(lexer, cpu.acpiId);
        if (code != Code::Ok) return code;
        if (!lexer.expect("PSD")) return Code::Syntax;
        uint64_t *const values[] = {&cpu.psdEntries, &cpu.psdRevision, &cpu.psdDomain,
                                   &cpu.psdCoordination, &cpu.psdProcessors};
        for (uint64_t *value : values) {
            code = number(lexer.next(), *value);
            if (code != Code::Ok) return code;
        }
        if (!lexer.expect("CPC")) return Code::Syntax;
        uint64_t entries = 0;
        code = number(lexer.next(), entries);
        if (code != Code::Ok) return code;
        if (entries > kCpcEntries) return Code::Capacity;
        cpu.count = static_cast<size_t>(entries);
        for (size_t j = 0; j < cpu.count; ++j) {
            StaticEntry &entry = cpu.entries[j];
            const Bytes kind = lexer.next();
            if (tokenIs(kind, "I")) {
                entry.kind = EntryKind::Integer;
                code = number(lexer.next(), entry.integer);
                if (code != Code::Ok) return code;
            } else if (tokenIs(kind, "B")) {
                entry.kind = EntryKind::Buffer;
                const Bytes hex = lexer.next();
                if (hex.size == 0 || hex.size % 2 != 0) return Code::Syntax;
                if (hex.size > sizeof(entry.buffer) * 2) return Code::Capacity;
                entry.size = hex.size / 2;
                for (size_t b = 0; b < entry.size; ++b) {
                    const int hi = hexDigit(hex.data[b * 2]), lo = hexDigit(hex.data[b * 2 + 1]);
                    if (hi < 0 || lo < 0) return Code::Syntax;
                    entry.buffer[b] = static_cast<uint8_t>(hi * 16 + lo);
                }
            } else return Code::Unsupported; // Methods, references, expressions, strings, etc.
        }
        if (!lexer.expect("ENDCPU")) return Code::Syntax;
    }
    if (!lexer.expect("END") || lexer.next().size != 0) return Code::Syntax;
    out.count = static_cast<size_t>(count);
    return Code::Ok;
}

Code parseCpuMap(Bytes text, CpuMap &out) {
    out.count = 0;
    Code code = textValid(text);
    if (code != Code::Ok) return code;
    Lexer lexer(text);
    if (!lexer.expect("XNU_MAP_V1") || !lexer.expect("CPUS")) return Code::Syntax;
    uint64_t count = 0;
    code = number(lexer.next(), count);
    if (code != Code::Ok) return code;
    if (count == 0 || count > kMaxCpus) return Code::Capacity;
    for (size_t i = 0; i < count; ++i) {
        if (!lexer.expect("MAP")) return Code::Syntax;
        code = next32(lexer, out.entries[i].apicId);
        if (code != Code::Ok) return code;
        code = next32(lexer, out.entries[i].xnuCpu);
        if (code != Code::Ok) return code;
        if (out.entries[i].xnuCpu > INT32_MAX) return Code::Bounds;
    }
    if (!lexer.expect("END") || lexer.next().size != 0) return Code::Syntax;
    out.count = static_cast<size_t>(count);
    return Code::Ok;
}

Code importDescriptors(const StaticCpc &cpc, const Pcct &pcct, const Madt &madt,
                       const CpuMap &map, Discovery &out) {
    out.cpuCount = 0;
    out.domainCount = 0;
    Code code = validatePcct(pcct);
    if (code != Code::Ok) return code;
    code = validateMadt(madt);
    if (code != Code::Ok) return code;
    if (cpc.count == 0 || cpc.count > kMaxCpus || map.count > kMaxCpus) return Code::Capacity;
    size_t enabledCount = 0;
    for (size_t i = 0; i < madt.count; ++i) if (madt.cpus[i].enabled) ++enabledCount;
    if (map.count != cpc.count) return Code::MissingIdentity;
    for (size_t i = 0; i < map.count; ++i) {
        if (map.entries[i].xnuCpu > INT32_MAX) return Code::Bounds;
        for (size_t j = 0; j < i; ++j)
            if (map.entries[i].apicId == map.entries[j].apicId ||
                map.entries[i].xnuCpu == map.entries[j].xnuCpu) return Code::DuplicateIdentity;
    }
    size_t domainCount = 0;
    for (size_t i = 0; i < cpc.count; ++i) {
        const StaticCpu &src = cpc.cpus[i];
        if (!pathValid(src.path)) return Code::Syntax;
        if (src.identityKind != IdentityKind::Lapic && src.identityKind != IdentityKind::X2Apic)
            return Code::Unsupported;
        for (size_t j = 0; j < i; ++j)
            if (sameString(src.path, cpc.cpus[j].path) ||
                (src.identityKind == cpc.cpus[j].identityKind && src.acpiId == cpc.cpus[j].acpiId))
                return Code::DuplicateIdentity;
        const MadtCpu *identity = nullptr;
        for (size_t j = 0; j < madt.count; ++j)
            if (madt.cpus[j].kind == src.identityKind && madt.cpus[j].acpiId == src.acpiId)
                identity = &madt.cpus[j];
        if (!identity) return Code::MissingIdentity;
        if (!identity->enabled) return Code::DisabledIdentity;
        const CpuMapEntry *mapping = nullptr;
        for (size_t j = 0; j < map.count; ++j)
            if (map.entries[j].apicId == identity->apicId) mapping = &map.entries[j];
        if (!mapping) return Code::MissingIdentity;
        if (src.count != kCpcEntries) return Code::Length;
        if (src.entries[0].kind != EntryKind::Integer || src.entries[1].kind != EntryKind::Integer)
            return Code::Unsupported;
        if (src.entries[0].integer != src.count) return Code::Length;
        if (src.entries[1].integer != 3) return Code::Revision;
        if (src.psdEntries != 5 || src.psdRevision != 0 || src.psdDomain > UINT32_MAX ||
            src.psdProcessors == 0 || src.psdProcessors > kMaxCpus) return Code::Domain;
        if (src.psdCoordination != 0xfe) return Code::Unsupported;
        CpuDescriptor &cpu = out.cpus[i];
        cpu = {};
        copyPath(cpu.path, src.path);
        cpu.acpiId = src.acpiId;
        cpu.apicId = identity->apicId;
        cpu.xnuCpu = mapping->xnuCpu;
        cpu.identityKind = src.identityKind;
        cpu.domain = static_cast<uint32_t>(src.psdDomain);
        bool haveSubspace = false;
        uint8_t subspace = 0;
        for (size_t f = 0; f < kFieldCount; ++f) {
            code = importField(src.entries[f + 2], f, pcct, cpu.fields[f]);
            if (code != Code::Ok) return code;
            if (cpu.fields[f].kind == FieldKind::PccRegister) {
                const uint8_t id = cpu.fields[f].gas.access;
                if (haveSubspace && subspace != id) return Code::Subspace;
                subspace = id;
                haveSubspace = true;
            }
        }
        if (!haveSubspace) return Code::Subspace; // This profile is a PCC discovery lab.
        const bool haveMin = cpu.fields[Minimum].kind == FieldKind::PccRegister;
        const bool haveMax = cpu.fields[Maximum].kind == FieldKind::PccRegister;
        if (haveMin != haveMax) return Code::Unsupported; // Both bounds or neither.
        size_t domain = 0;
        while (domain < domainCount && out.domains[domain].id != cpu.domain) ++domain;
        if (domain == domainCount)
            out.domains[domainCount++] = {cpu.domain, static_cast<uint32_t>(src.psdProcessors), 0};
        DomainDescriptor &d = out.domains[domain];
        if (d.expectedMembers != src.psdProcessors) return Code::Domain;
        ++d.members;
    }
    if (enabledCount != cpc.count) return Code::MissingIdentity;
    for (size_t i = 0; i < domainCount; ++i)
        if (out.domains[i].members != out.domains[i].expectedMembers) return Code::Domain;
    code = validatePccFieldAliases(out.cpus, cpc.count);
    if (code != Code::Ok) return code;
    out.pcct = pcct;
    out.cpuCount = cpc.count;
    out.domainCount = domainCount;
    return Code::Ok;
}

Value initialValue(const Field &field) {
    if (field.kind == FieldKind::Unsupported) return {ValueKind::Unsupported, 0};
    if (field.kind == FieldKind::Integer) return {ValueKind::StaticInteger, field.integer};
    return {ValueKind::Unknown, 0};
}

} // namespace cppc_lab
