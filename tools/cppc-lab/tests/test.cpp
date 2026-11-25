#include "Input.hpp"
#include <stdio.h>
#include <string.h>
#include <string>
#include <type_traits>

using namespace cppc_lab;
static cppc_lab_host::Inputs fixture;
static Discovery imported;
static std::string cpcText, mapText;
static size_t checks = 0, failures = 0, groups = 0;

static void check(bool pass, const char *expression, int line) {
    ++checks;
    if (!pass) { ++failures; fprintf(stderr, "FAIL line %d: %s\n", line, expression); }
}
static void expect(Code actual, Code expected, const char *expression, int line) {
    ++checks;
    if (actual != expected) {
        ++failures;
        fprintf(stderr, "FAIL line %d: %s -> %s, expected %s\n", line, expression,
                codeName(actual), codeName(expected));
    }
}
#define CHECK(expr) check((expr), #expr, __LINE__)
#define EXPECT(expr, code) expect((expr), (code), #expr, __LINE__)
#define OK(expr) EXPECT((expr), Code::Ok)

static Bytes bytes(const std::string &s) {
    return {reinterpret_cast<const uint8_t *>(s.data()), s.size()};
}
static void put(uint8_t *p, size_t width, uint64_t value) {
    for (size_t i = 0; i < width; ++i) p[i] = static_cast<uint8_t>(value >> (i * 8));
}
static uint64_t get(const uint8_t *p, size_t width) {
    uint64_t value = 0;
    for (size_t i = 0; i < width; ++i) value |= uint64_t(p[i]) << (i * 8);
    return value;
}
struct Table {
    uint8_t data[kMaxTableBytes + 1]{};
    size_t size = 0;
    Bytes view() const { return {data, size}; }
};
static Table table(bool pcct) {
    Table out;
    out.size = pcct ? fixture.pcctSize : fixture.madtSize;
    memcpy(out.data, pcct ? fixture.pcctBytes : fixture.madtBytes, out.size);
    return out;
}
static void checksum(Table &t) {
    t.data[9] = 0;
    uint8_t sum = 0;
    for (size_t i = 0; i < t.size; ++i) sum = static_cast<uint8_t>(sum + t.data[i]);
    t.data[9] = static_cast<uint8_t>(0u - sum);
}
static void resize(Table &t, size_t size) {
    t.size = size;
    put(t.data + 4, 4, size);
    checksum(t);
}
static Code parseTable(bool pcct, Bytes raw) {
    Pcct p{};
    Madt m{};
    return pcct ? parsePcct(raw, p) : parseMadt(raw, m);
}
static void mutate(bool pcct, size_t offset, size_t width, uint64_t value, Code expected) {
    Table t = table(pcct);
    put(t.data + offset, width, value);
    checksum(t); // Isolate semantic validation from checksum validation.
    EXPECT(parseTable(pcct, t.view()), expected);
}
static Code importCpc(const StaticCpc &cpc) {
    return importDescriptors(cpc, fixture.pcct, fixture.madt, fixture.map, imported);
}
static StaticEntry &entry(StaticCpc &cpc, size_t field) { return cpc.cpus[0].entries[field + 2]; }
static void integer(StaticEntry &out, uint64_t value) {
    out = {};
    out.kind = EntryKind::Integer;
    out.integer = value;
}
static void nullResource(StaticEntry &out) {
    out = {};
    out.kind = EntryKind::Buffer;
    out.size = sizeof(out.buffer);
    out.buffer[0] = 0x82;
    out.buffer[1] = 12;
    out.buffer[15] = 0x79;
}
static std::string replaceOne(std::string text, const char *oldText, const char *newText) {
    const size_t at = text.find(oldText);
    CHECK(at != std::string::npos);
    if (at != std::string::npos) text.replace(at, strlen(oldText), newText);
    return text;
}

static void importCapture() {
    OK(importCpc(fixture.cpc));
    CHECK(imported.cpuCount == 16 && imported.domainCount == 8 && imported.pcct.count == 1);
    CHECK(fixture.madt.count == 32); // Includes 16 disabled APIC=0 placeholders.
    CHECK(fixture.madt.cpus[1].acpiId == 2 && fixture.madt.cpus[8].acpiId == 1);
    CHECK(fixture.pcct.channels[0].baseAddress == 0x100000);
    CHECK(fixture.pcct.channels[0].doorbell.address == 0x200000); // Sanitized, not hardware.
    CHECK(fixture.pcct.channels[0].length == 65536);
    CHECK(fixture.pcct.channels[0].nominalLatencyUs == 4000);
    CHECK(fixture.pcct.channels[0].maxCommandsPerMinute == 60000);
    CHECK(fixture.pcct.channels[0].minTurnaroundUs == 0);
    const size_t controls[] = {Desired, Minimum, Maximum, Enable, AutonomousSelection, Epp};
    const uint64_t offsets[] = {0x14, 0x18, 0x1c, 0x44, 0x48, 0x50};
    for (size_t i = 0; i < imported.cpuCount; ++i) {
        const CpuDescriptor &cpu = imported.cpus[i];
        CHECK(cpu.acpiId == i && cpu.apicId == i);
        CHECK(cpu.xnuCpu == (i * 5 + 3) % 16 && cpu.domain == i / 2);
        for (size_t c = 0; c < sizeof(controls) / sizeof(controls[0]); ++c) {
            const Field &f = cpu.fields[controls[c]];
            CHECK(f.kind == FieldKind::PccRegister && f.gas.width == 32 && f.gas.access == 0);
            CHECK(f.gas.address == 0x100 * i + offsets[c]);
            CHECK(f.byteOffset == f.gas.address + 8);
            CHECK(initialValue(f).kind == ValueKind::Unknown);
        }
        CHECK(cpu.fields[Highest].byteOffset == 0x100 * i + 8);
        CHECK(initialValue(cpu.fields[Highest]).kind == ValueKind::Unknown);
        CHECK(cpu.fields[ReferenceCounter].gas.address == 0xe7);
        CHECK(cpu.fields[DeliveredCounter].gas.address == 0xe8);
        CHECK(initialValue(cpu.fields[ReferenceCounter]).kind == ValueKind::Unknown);
        CHECK(initialValue(cpu.fields[AutonomousWindow]).kind == ValueKind::Unsupported);
        CHECK(initialValue(cpu.fields[LowestFrequency]).kind == ValueKind::StaticInteger);
        CHECK(cpu.fields[LowestFrequency].integer == 550 && cpu.fields[NominalFrequency].integer == 3401);
    }
    for (size_t i = 0; i < imported.domainCount; ++i)
        CHECK(imported.domains[i].members == 2 && imported.domains[i].expectedMembers == 2);
}

static void rawHeaders() {
    for (bool pcct : {true, false}) {
        Table t = table(pcct);
        for (size_t i = 0; i < t.size; ++i)
            CHECK(parseTable(pcct, {t.data, i}) != Code::Ok);
        for (size_t i = 0; i < t.size; ++i) {
            t.data[i] ^= 1;
            CHECK(parseTable(pcct, t.view()) != Code::Ok);
            t.data[i] ^= 1;
        }
        EXPECT(parseTable(pcct, {nullptr, 36}), Code::Truncated);
        mutate(pcct, 0, 1, 'X', Code::Signature);
        mutate(pcct, 4, 4, 0, Code::Length);
        mutate(pcct, 4, 4, 35, Code::Length);
        mutate(pcct, 4, 4, t.size - 1, Code::Length);
        mutate(pcct, 4, 4, t.size + 1, Code::Truncated);
        mutate(pcct, 4, 4, UINT32_MAX, Code::Capacity);
        mutate(pcct, 8, 1, 255, Code::Revision);
        t.data[t.size] = 0;
        EXPECT(parseTable(pcct, {t.data, t.size + 1}), Code::Length);
        EXPECT(parseTable(pcct, {t.data, kMaxTableBytes + 1}), Code::Capacity);
        Table unaligned;
        memcpy(unaligned.data + 1, t.data, t.size);
        OK(parseTable(pcct, {unaligned.data + 1, t.size})); // No packed struct loads.
        resize(t, t.size + 1);
        EXPECT(parseTable(pcct, t.view()), Code::Truncated);
    }
    Table t = table(true);
    t.data[10] ^= 1;
    EXPECT(parseTable(true, t.view()), Code::Checksum);
    mutate(false, 40, 4, 2, Code::Reserved);
}

static void pcctValidation() {
    mutate(true, 36, 4, 1, Code::Unsupported);
    mutate(true, 40, 8, 1, Code::Reserved);
    mutate(true, 48, 1, 1, Code::Unsupported);
    mutate(true, 48, 1, 4, Code::Unsupported);
    mutate(true, 48, 1, 255, Code::Unsupported);
    mutate(true, 49, 1, 0, Code::Length);
    mutate(true, 49, 1, 1, Code::Length);
    mutate(true, 49, 1, 61, Code::Length);
    mutate(true, 49, 1, 63, Code::Truncated);
    mutate(true, 50, 1, 1, Code::Reserved);
    mutate(true, 56, 8, 0, Code::Bounds);
    mutate(true, 56, 8, 0x100001, Code::Alignment);
    mutate(true, 56, 8, UINT64_MAX - 7, Code::Overflow);
    mutate(true, 64, 8, 0, Code::Bounds);
    mutate(true, 64, 8, 7, Code::Bounds);
    mutate(true, 64, 8, kMaxSharedBytes + 1, Code::Capacity);
    mutate(true, 72, 1, 1, Code::Unsupported);
    mutate(true, 73, 1, 0, Code::Gas);
    mutate(true, 73, 1, 24, Code::Gas);
    mutate(true, 73, 1, 32, Code::Gas); // Mismatched encoded QWord access.
    mutate(true, 74, 1, 1, Code::Gas);
    mutate(true, 75, 1, 5, Code::Gas);
    mutate(true, 76, 8, 0, Code::Bounds);
    mutate(true, 76, 8, 0x200004, Code::Alignment);
    mutate(true, 76, 8, UINT64_MAX, Code::Overflow);
    mutate(true, 76, 8, 0x100000, Code::Aliasing);
    mutate(true, 75, 1, 0, Code::Ok); // Unspecified access, full 64-bit width.
    Table t = table(true);
    resize(t, 48);
    EXPECT(parseTable(true, t.view()), Code::Subspace);
    t = table(true);
    t.data[73] = 8; t.data[75] = 1;
    checksum(t);
    EXPECT(parseTable(true, t.view()), Code::Gas); // Masks exceed 8-bit register.
    put(t.data + 84, 8, 0xf0); put(t.data + 92, 8, 1); checksum(t);
    OK(parseTable(true, t.view()));
    put(t.data + 92, 8, 0x100); checksum(t);
    EXPECT(parseTable(true, t.view()), Code::Gas);
}

static Table channels(size_t count) {
    Table t = table(true);
    for (size_t i = 0; i < count; ++i) {
        const size_t at = 48 + i * 62;
        memcpy(t.data + at, fixture.pcctBytes + 48, 62);
        put(t.data + at + 8, 8, 0x100000 + i * 0x10000);
        put(t.data + at + 28, 8, 0x200000 + i * 0x100);
    }
    resize(t, 48 + count * 62);
    return t;
}
static void subspaces() {
    Pcct pcct{};
    Table t = channels(6);
    OK(parsePcct(t.view(), pcct));
    CHECK(pcct.count == 6);
    StaticCpc cpc = fixture.cpc;
    for (size_t c = 0; c < cpc.count; ++c)
        for (size_t i = 2; i < cpc.cpus[c].count; ++i) {
            StaticEntry &e = cpc.cpus[c].entries[i];
            if (e.kind == EntryKind::Buffer && e.buffer[3] == kPcc) e.buffer[6] = 5;
        }
    OK(importDescriptors(cpc, pcct, fixture.madt, fixture.map, imported));
    CHECK(imported.cpus[0].fields[Highest].gas.width == 32);
    CHECK(imported.cpus[0].fields[Highest].gas.access == 5); // NOT an access width.
    CHECK(imported.cpus[0].fields[Highest].byteOffset == 8);
    entry(cpc, Highest).buffer[6] = 2;
    EXPECT(importDescriptors(cpc, pcct, fixture.madt, fixture.map, imported), Code::Subspace);
    entry(cpc, Highest).buffer[6] = 255;
    EXPECT(importDescriptors(cpc, pcct, fixture.madt, fixture.map, imported), Code::Subspace);
    t = channels(9);
    EXPECT(parsePcct(t.view(), pcct), Code::Capacity);
    t = channels(2);
    put(t.data + 48 + 62 + 8, 8, 0x100000); checksum(t);
    EXPECT(parsePcct(t.view(), pcct), Code::Aliasing);
    t = channels(2);
    put(t.data + 48 + 62 + 28, 8, 0x200000); checksum(t);
    EXPECT(parsePcct(t.view(), pcct), Code::Aliasing);
    t = channels(2);
    put(t.data + 48 + 62 + 28, 8, 0x100008); checksum(t);
    EXPECT(parsePcct(t.view(), pcct), Code::Aliasing);
    Pcct none = fixture.pcct;
    none.count = 0;
    EXPECT(importDescriptors(fixture.cpc, none, fixture.madt, fixture.map, imported), Code::Subspace);
}

static void identityJoins() {
    Table t = table(false);
    for (size_t i = 0; i < 32; ++i)
        memcpy(t.data + 44 + i * 8, fixture.madtBytes + 44 + (31 - i) * 8, 8);
    for (size_t i = 0; i < 32; ++i) {
        uint8_t *p = t.data + 44 + i * 8;
        if (p[4] == 1) p[3] = static_cast<uint8_t>(0x80 + p[2] * 3);
    }
    checksum(t);
    Madt madt{};
    OK(parseMadt(t.view(), madt));
    StaticCpc cpc = fixture.cpc;
    CpuMap map = fixture.map;
    for (size_t i = 0; i < 16; ++i) {
        cpc.cpus[i] = fixture.cpc.cpus[15 - i];
        const uint32_t uid = fixture.map.entries[i].apicId;
        map.entries[i] = {0x80 + uid * 3, 1000 + (uid * 7 + 9) % 16};
    }
    OK(importDescriptors(cpc, fixture.pcct, madt, map, imported));
    for (size_t i = 0; i < 16; ++i) {
        const CpuDescriptor &cpu = imported.cpus[i];
        CHECK(cpu.acpiId == 15 - i);
        CHECK(cpu.apicId == 0x80 + cpu.acpiId * 3);
        CHECK(cpu.xnuCpu == 1000 + (cpu.acpiId * 7 + 9) % 16);
        CHECK(cpu.fields[Desired].byteOffset == cpu.acpiId * 0x100 + 0x1c);
        CHECK(cpu.domain == cpu.acpiId / 2);
    }
    mutate(false, 44 + 8 + 2, 1, 0, Code::DuplicateIdentity);
    mutate(false, 44 + 8 + 3, 1, 0, Code::DuplicateIdentity);
    mutate(false, 44 + 16 * 8 + 2, 1, 0, Code::DuplicateIdentity); // Disabled UID aliases active.
    mutate(false, 44 + 17 * 8 + 2, 1, 16, Code::DuplicateIdentity); // Duplicate disabled UID.
    mutate(false, 44 + 4, 4, 4, Code::Reserved);
    for (uint64_t flags : {uint64_t(0), uint64_t(2)}) {
        t = table(false);
        put(t.data + 48, 4, flags); checksum(t);
        OK(parseMadt(t.view(), madt));
        EXPECT(importDescriptors(fixture.cpc, fixture.pcct, madt, fixture.map, imported), Code::DisabledIdentity);
    }
    t = table(false);
    memmove(t.data + 44, t.data + 52, t.size - 52);
    resize(t, t.size - 8);
    OK(parseMadt(t.view(), madt));
    EXPECT(importDescriptors(fixture.cpc, fixture.pcct, madt, fixture.map, imported), Code::MissingIdentity);
    t = table(false);
    t.data[44 + 16 * 8 + 3] = 16; t.data[44 + 16 * 8 + 4] = 1; checksum(t);
    OK(parseMadt(t.view(), madt));
    EXPECT(importDescriptors(fixture.cpc, fixture.pcct, madt, fixture.map, imported), Code::MissingIdentity);

    cpc = fixture.cpc; cpc.cpus[0].acpiId = 16;
    EXPECT(importCpc(cpc), Code::DisabledIdentity);
    cpc.cpus[0].acpiId = 200;
    EXPECT(importCpc(cpc), Code::MissingIdentity);
    cpc = fixture.cpc; cpc.cpus[0].identityKind = IdentityKind::X2Apic;
    EXPECT(importCpc(cpc), Code::MissingIdentity);
    cpc = fixture.cpc; cpc.cpus[1].acpiId = cpc.cpus[0].acpiId;
    EXPECT(importCpc(cpc), Code::DuplicateIdentity);
    cpc = fixture.cpc; memcpy(cpc.cpus[1].path, cpc.cpus[0].path, kPathBytes);
    EXPECT(importCpc(cpc), Code::DuplicateIdentity);
    cpc = fixture.cpc; memset(cpc.cpus[0].path, 'A', kPathBytes);
    EXPECT(importCpc(cpc), Code::Syntax);
    map = fixture.map; --map.count;
    EXPECT(importDescriptors(fixture.cpc, fixture.pcct, fixture.madt, map, imported), Code::MissingIdentity);
    map = fixture.map; ++map.count;
    EXPECT(importDescriptors(fixture.cpc, fixture.pcct, fixture.madt, map, imported), Code::MissingIdentity);
    map = fixture.map; map.entries[0].xnuCpu = map.entries[1].xnuCpu;
    EXPECT(importDescriptors(fixture.cpc, fixture.pcct, fixture.madt, map, imported), Code::DuplicateIdentity);
    map = fixture.map; map.entries[0].apicId = map.entries[1].apicId;
    EXPECT(importDescriptors(fixture.cpc, fixture.pcct, fixture.madt, map, imported), Code::DuplicateIdentity);
    map = fixture.map; map.entries[0].apicId = 240;
    EXPECT(importDescriptors(fixture.cpc, fixture.pcct, fixture.madt, map, imported), Code::MissingIdentity);
    map = fixture.map; map.entries[0].xnuCpu = UINT32_MAX;
    EXPECT(importDescriptors(fixture.cpc, fixture.pcct, fixture.madt, map, imported), Code::Bounds);
}

static void x2Apic() {
    Table t = table(false);
    StaticCpc cpc = fixture.cpc;
    CpuMap map = fixture.map;
    for (size_t i = 0; i < 16; ++i) {
        uint8_t *p = t.data + 44 + i * 16;
        memset(p, 0, 16);
        p[0] = 9; p[1] = 16;
        put(p + 4, 4, 0x10000 + i * 11);
        put(p + 8, 4, 1);
        put(p + 12, 4, 0x100000 + i);
        cpc.cpus[i].identityKind = IdentityKind::X2Apic;
        cpc.cpus[i].acpiId = static_cast<uint32_t>(0x100000 + i);
        map.entries[i] = {static_cast<uint32_t>(0x10000 + i * 11), static_cast<uint32_t>(15 - i)};
    }
    t.data[8] = 4;
    resize(t, 44 + 16 * 16);
    Madt madt{};
    OK(parseMadt(t.view(), madt));
    OK(importDescriptors(cpc, fixture.pcct, madt, map, imported));
    CHECK(imported.cpus[7].acpiId == 0x100007 && imported.cpus[7].apicId == 0x1004d);
    CHECK(imported.cpus[7].xnuCpu == 8);
    put(t.data + 44 + 8, 4, 2); checksum(t);
    OK(parseMadt(t.view(), madt));
    EXPECT(importDescriptors(cpc, fixture.pcct, madt, map, imported), Code::DisabledIdentity);
    put(t.data + 44 + 16 + 12, 4, 0x100000); checksum(t);
    EXPECT(parseMadt(t.view(), madt), Code::DuplicateIdentity);
    t.data[46] = 1; checksum(t);
    EXPECT(parseMadt(t.view(), madt), Code::Reserved);
    t.data[46] = 0; t.data[45] = 15; checksum(t);
    EXPECT(parseMadt(t.view(), madt), Code::Length);
    mutate(false, 44, 1, 7, Code::Unsupported);
    mutate(false, 44, 1, 11, Code::Unsupported);
    mutate(false, 45, 1, 0, Code::Length);
    mutate(false, 45, 1, 7, Code::Length);
    mutate(false, 45, 1, 255, Code::Length);
}

static void descriptorRejects() {
    StaticCpc cpc = fixture.cpc;
    --cpc.cpus[0].count; EXPECT(importCpc(cpc), Code::Length);
    cpc = fixture.cpc; cpc.cpus[0].count = 24; EXPECT(importCpc(cpc), Code::Length);
    cpc = fixture.cpc; cpc.cpus[0].entries[0].integer = 22; EXPECT(importCpc(cpc), Code::Length);
    cpc = fixture.cpc; cpc.cpus[0].entries[0].kind = EntryKind::Dynamic; EXPECT(importCpc(cpc), Code::Unsupported);
    cpc = fixture.cpc; cpc.cpus[0].entries[1].integer = 4; EXPECT(importCpc(cpc), Code::Revision);
    cpc = fixture.cpc; entry(cpc, Highest).kind = EntryKind::Dynamic; EXPECT(importCpc(cpc), Code::Unsupported);
    cpc = fixture.cpc; entry(cpc, Highest).size = 16; EXPECT(importCpc(cpc), Code::Length);
    cpc = fixture.cpc; entry(cpc, Highest).size = 18; EXPECT(importCpc(cpc), Code::Length);
    const size_t fields[] = {0, 1, 2, 3, 4, 5, 6, 15};
    const uint8_t values[] = {0x81, 13, 1, 1, 0, 1, 255, 0x78};
    const Code codes[] = {Code::Gas, Code::Gas, Code::Gas, Code::Unsupported,
                          Code::Gas, Code::Gas, Code::Subspace, Code::Gas};
    for (size_t i = 0; i < sizeof(fields) / sizeof(fields[0]); ++i) {
        cpc = fixture.cpc;
        entry(cpc, Highest).buffer[fields[i]] = values[i];
        EXPECT(importCpc(cpc), codes[i]);
    }
    cpc = fixture.cpc; entry(cpc, Highest).buffer[4] = 24; EXPECT(importCpc(cpc), Code::Gas);
    cpc = fixture.cpc; entry(cpc, Desired).buffer[4] = 64; EXPECT(importCpc(cpc), Code::Gas);
    cpc = fixture.cpc; put(entry(cpc, Highest).buffer + 7, 8, UINT64_MAX - 7);
    EXPECT(importCpc(cpc), Code::Overflow);
    cpc = fixture.cpc; put(entry(cpc, Highest).buffer + 7, 8, UINT64_MAX - 11);
    EXPECT(importCpc(cpc), Code::Overflow); // Header fits; end overflows.
    cpc = fixture.cpc; put(entry(cpc, Highest).buffer + 7, 8, 65524);
    OK(importCpc(cpc)); CHECK(imported.cpus[0].fields[Highest].byteOffset == 65532);
    put(entry(cpc, Highest).buffer + 7, 8, 65528); EXPECT(importCpc(cpc), Code::Bounds);
    cpc = fixture.cpc; put(entry(cpc, Highest).buffer + 7, 8, 2); EXPECT(importCpc(cpc), Code::Alignment);
    cpc = fixture.cpc; entry(cpc, Highest).buffer[3] = kSystemMemory; EXPECT(importCpc(cpc), Code::Unsupported);
    cpc = fixture.cpc; entry(cpc, ReferenceCounter).buffer[7] = 0xe9; EXPECT(importCpc(cpc), Code::Unsupported);
    cpc = fixture.cpc; entry(cpc, ReferenceCounter).buffer[4] = 32; EXPECT(importCpc(cpc), Code::Gas);
    cpc = fixture.cpc; entry(cpc, ReferenceCounter).buffer[6] = 0; EXPECT(importCpc(cpc), Code::Gas);
    const size_t mandatory[] = {Highest, Nominal, LowestNonlinear, Lowest, Desired,
                                ReferenceCounter, DeliveredCounter, PerformanceLimited};
    for (size_t f : mandatory) {
        cpc = fixture.cpc;
        entry(cpc, f) = entry(cpc, Guaranteed);
        EXPECT(importCpc(cpc), Code::Unsupported);
    }
    cpc = fixture.cpc;
    uint8_t *raw = entry(cpc, Highest).buffer;
    uint8_t sum = 0;
    for (size_t i = 0; i < 16; ++i) sum = static_cast<uint8_t>(sum + raw[i]);
    raw[16] = static_cast<uint8_t>(0u - sum);
    CHECK(raw[16] != 0);
    OK(importCpc(cpc)); // Nonzero resource checksum, correctly validated.
    raw[16] ^= 1; EXPECT(importCpc(cpc), Code::Checksum);
}

static void integersDomains() {
    StaticCpc cpc = fixture.cpc;
    integer(entry(cpc, Highest), 0); EXPECT(importCpc(cpc), Code::Unsupported);
    integer(entry(cpc, Highest), UINT64_MAX); EXPECT(importCpc(cpc), Code::Overflow);
    integer(entry(cpc, Highest), 200); OK(importCpc(cpc));
    CHECK(initialValue(imported.cpus[0].fields[Highest]).kind == ValueKind::StaticInteger);
    CHECK(initialValue(imported.cpus[0].fields[Highest]).number == 200);
    cpc = fixture.cpc;
    integer(entry(cpc, AutonomousSelection), 0); OK(importCpc(cpc));
    CHECK(initialValue(imported.cpus[0].fields[AutonomousSelection]).kind == ValueKind::StaticInteger);
    CHECK(initialValue(imported.cpus[0].fields[AutonomousSelection]).number == 0);
    integer(entry(cpc, AutonomousSelection), 1); OK(importCpc(cpc));
    integer(entry(cpc, AutonomousSelection), 2); EXPECT(importCpc(cpc), Code::Gas);
    cpc = fixture.cpc;
    integer(entry(cpc, Minimum), 0); integer(entry(cpc, Maximum), 0); OK(importCpc(cpc));
    CHECK(initialValue(imported.cpus[0].fields[Minimum]).kind == ValueKind::Unsupported);
    CHECK(initialValue(imported.cpus[0].fields[Maximum]).kind == ValueKind::Unsupported);
    cpc = fixture.cpc; integer(entry(cpc, LowestFrequency), 0); OK(importCpc(cpc));
    CHECK(initialValue(imported.cpus[0].fields[LowestFrequency]).kind == ValueKind::Unsupported);
    cpc = fixture.cpc; integer(entry(cpc, Desired), 0); EXPECT(importCpc(cpc), Code::Unsupported);
    cpc = fixture.cpc; integer(entry(cpc, Epp), 123); EXPECT(importCpc(cpc), Code::Unsupported);
    cpc = fixture.cpc; integer(entry(cpc, WraparoundTime), UINT64_MAX); OK(importCpc(cpc));
    CHECK(initialValue(imported.cpus[0].fields[WraparoundTime]).number == UINT64_MAX);
    cpc = fixture.cpc; cpc.cpus[0].psdEntries = 4; EXPECT(importCpc(cpc), Code::Domain);
    cpc = fixture.cpc; cpc.cpus[0].psdRevision = 1; EXPECT(importCpc(cpc), Code::Domain);
    cpc = fixture.cpc; cpc.cpus[0].psdCoordination = 0xfc; EXPECT(importCpc(cpc), Code::Unsupported);
    cpc = fixture.cpc; cpc.cpus[0].psdDomain = UINT64_MAX; EXPECT(importCpc(cpc), Code::Domain);
    cpc = fixture.cpc; cpc.cpus[0].psdProcessors = 0; EXPECT(importCpc(cpc), Code::Domain);
    cpc = fixture.cpc; cpc.cpus[0].psdProcessors = 3; EXPECT(importCpc(cpc), Code::Domain);
    cpc = fixture.cpc; cpc.cpus[1].psdDomain = 8; EXPECT(importCpc(cpc), Code::Domain);
    cpc = fixture.cpc; cpc.cpus[0].psdProcessors = 3; cpc.cpus[1].psdProcessors = 3;
    EXPECT(importCpc(cpc), Code::Domain);
    cpc = fixture.cpc; cpc.count = 0; EXPECT(importCpc(cpc), Code::Capacity);
    cpc.count = kMaxCpus + 1; EXPECT(importCpc(cpc), Code::Capacity);
    CHECK(imported.cpuCount == 0 && imported.domainCount == 0);
}

static void pairedBounds() {
    OK(importCpc(fixture.cpc)); // Both controls are present in the capture.
    for (size_t missing : {size_t(Minimum), size_t(Maximum)}) {
        for (bool useInteger : {false, true}) {
            StaticCpc cpc = fixture.cpc;
            if (useInteger) integer(entry(cpc, missing), 0);
            else nullResource(entry(cpc, missing));
            EXPECT(importCpc(cpc), Code::Unsupported);
            CHECK(imported.cpuCount == 0 && imported.domainCount == 0);
            // Apply the same malformed package to the last CPU, not just C000.
            const StaticEntry absent = entry(cpc, missing);
            cpc = fixture.cpc;
            cpc.cpus[cpc.count - 1].entries[missing + 2] = absent;
            EXPECT(importCpc(cpc), Code::Unsupported);
        }
    }
    for (bool minInteger : {false, true}) {
        for (bool maxInteger : {false, true}) {
            StaticCpc cpc = fixture.cpc;
            if (minInteger) integer(entry(cpc, Minimum), 0);
            else nullResource(entry(cpc, Minimum));
            if (maxInteger) integer(entry(cpc, Maximum), 0);
            else nullResource(entry(cpc, Maximum));
            OK(importCpc(cpc)); // Neither bound is also a valid advertisement.
            CHECK(imported.cpus[0].fields[Minimum].kind == FieldKind::Unsupported);
            CHECK(imported.cpus[0].fields[Maximum].kind == FieldKind::Unsupported);
        }
    }
}

static void fieldAliases() {
    // Conflicting control roles must not acquire each other's response values.
    const size_t controls[] = {Desired, Minimum, Maximum, PerformanceLimited,
                               Enable, AutonomousSelection, AutonomousWindow, Epp};
    for (size_t f : controls) {
        StaticCpc cpc = fixture.cpc;
        entry(cpc, f) = entry(cpc, Highest);
        EXPECT(importCpc(cpc), Code::Aliasing);
        CHECK(imported.cpuCount == 0 && imported.domainCount == 0);
    }
    StaticCpc cpc = fixture.cpc;
    entry(cpc, Enable) = entry(cpc, Desired);
    EXPECT(importCpc(cpc), Code::Aliasing);
    cpc = fixture.cpc;
    entry(cpc, Highest) = entry(cpc, Enable); // Read-only versus control, reversed.
    EXPECT(importCpc(cpc), Code::Aliasing);
    for (size_t peer : {size_t(1), size_t(2), size_t(15)}) {
        cpc = fixture.cpc;
        cpc.cpus[peer].entries[Enable + 2] = entry(cpc, Desired);
        EXPECT(importCpc(cpc), Code::Aliasing); // Within and across HW_ALL domains.
        cpc = fixture.cpc;
        cpc.cpus[peer].entries[Desired + 2] = entry(cpc, Desired);
        OK(importCpc(cpc)); // Identical same-role aliases are permitted.
        CHECK(imported.cpus[peer].fields[Desired].byteOffset == imported.cpus[0].fields[Desired].byteOffset);
        cpc.cpus[peer].entries[Desired + 2].buffer[4] = 16;
        EXPECT(importCpc(cpc), Code::Aliasing); // Same role, but a different range.
    }
    cpc = fixture.cpc;
    entry(cpc, Enable).buffer[4] = 16;
    put(entry(cpc, Enable).buffer + 7, 8, 0x16);
    EXPECT(importCpc(cpc), Code::Aliasing); // Partial overlap with Desired [0x14,0x18).
    cpc = fixture.cpc;
    put(entry(cpc, Enable).buffer + 7, 8, 0x10);
    OK(importCpc(cpc)); // Adjacent ranges are not overlapping.
    cpc = fixture.cpc;
    entry(cpc, Nominal) = entry(cpc, Highest);
    OK(importCpc(cpc)); // Read-only fields may share their storage.
    entry(cpc, Nominal).buffer[4] = 16;
    put(entry(cpc, Nominal).buffer + 7, 8, 2);
    OK(importCpc(cpc)); // Read-only sharing may also be partial.

    Pcct pcct = fixture.pcct;
    pcct.count = 2;
    pcct.channels[1] = pcct.channels[0];
    pcct.channels[1].baseAddress += 0x1000000;
    pcct.channels[1].doorbell.address += 0x1000000;
    cpc = fixture.cpc;
    cpc.cpus[1].entries[Enable + 2] = entry(cpc, Desired);
    for (size_t i = 2; i < kCpcEntries; ++i) {
        StaticEntry &e = cpc.cpus[1].entries[i];
        if (e.kind == EntryKind::Buffer && e.buffer[3] == kPcc) e.buffer[6] = 1;
    }
    OK(importDescriptors(cpc, pcct, fixture.madt, fixture.map, imported));
    CHECK(imported.cpus[1].fields[Enable].byteOffset == imported.cpus[0].fields[Desired].byteOffset);
    CHECK(imported.cpus[1].fields[Enable].gas.access != imported.cpus[0].fields[Desired].gas.access);
}

static void frontends() {
    StaticCpc cpc{};
    CpuMap map{};
    OK(parseStaticCpc(bytes(cpcText), cpc));
    EXPECT(parseStaticCpc(bytes(replaceOne(cpcText, "CPPC_STATIC_V1", "CPPC_DYNAMIC_V1")), cpc), Code::Unsupported);
    EXPECT(parseStaticCpc(bytes(replaceOne(cpcText, "CPUS 16", "CPUS 0")), cpc), Code::Capacity);
    EXPECT(parseStaticCpc(bytes(replaceOne(cpcText, "CPUS 16", "CPUS 65")), cpc), Code::Capacity);
    EXPECT(parseStaticCpc(bytes(replaceOne(cpcText, "CPUS 16", "CPUS -1")), cpc), Code::Syntax);
    EXPECT(parseStaticCpc(bytes(replaceOne(cpcText, "CPUS 16", "CPUS 18446744073709551616")), cpc), Code::Overflow);
    EXPECT(parseStaticCpc(bytes(replaceOne(cpcText, "LAPIC 0", "LAPIC 0x100000000")), cpc), Code::Overflow);
    EXPECT(parseStaticCpc(bytes(replaceOne(cpcText, "LAPIC 0", "STRING_UID abc")), cpc), Code::Unsupported);
    EXPECT(parseStaticCpc(bytes(replaceOne(cpcText, "\\_PR_.C000", "\\_PR.C000")), cpc), Code::Syntax);
    EXPECT(parseStaticCpc(bytes(replaceOne(cpcText, "I 23 # NumEntries", "METHOD _CPC")), cpc), Code::Unsupported);
    EXPECT(parseStaticCpc(bytes(replaceOne(cpcText, "B 820c000a20000000000000000000007900", "B zz")), cpc), Code::Syntax);
    EXPECT(parseStaticCpc(bytes(replaceOne(cpcText, "B 820c000a20000000000000000000007900", "B 0")), cpc), Code::Syntax);
    EXPECT(parseStaticCpc(bytes(replaceOne(cpcText, "B 820c000a20000000000000000000007900",
        "B 820c000a2000000000000000000000790000")), cpc), Code::Capacity);
    EXPECT(parseStaticCpc(bytes(cpcText + "EXEC something"), cpc), Code::Syntax);
    EXPECT(parseStaticCpc({reinterpret_cast<const uint8_t *>(cpcText.data()), cpcText.size() / 2}, cpc), Code::Syntax);
    EXPECT(parseStaticCpc({nullptr, 0}, cpc), Code::Truncated);
    EXPECT(parseStaticCpc({nullptr, kMaxTextBytes + 1}, cpc), Code::Capacity);
    OK(parseStaticCpc(bytes(replaceOne(cpcText, "CPUS 16", "CPUS 0x10")), cpc));
    OK(importCpc(cpc));
    OK(parseCpuMap(bytes(mapText), map));
    EXPECT(parseCpuMap(bytes(replaceOne(mapText, "MAP 15 14", "MAP 15 -1")), map), Code::Syntax);
    EXPECT(parseCpuMap(bytes(replaceOne(mapText, "MAP 15 14", "MAP 15 2147483648")), map), Code::Bounds);
    EXPECT(parseCpuMap(bytes(mapText + "trailing"), map), Code::Syntax);
    EXPECT(parseCpuMap({nullptr, 0}, map), Code::Truncated);
    uint8_t decoded[4]{};
    size_t size = 99;
    OK(decodeHex(bytes("# test\n 0a FF 00 7f # done\n"), decoded, sizeof(decoded), size));
    CHECK(size == 4 && decoded[0] == 10 && decoded[1] == 255 && decoded[3] == 127);
    EXPECT(decodeHex(bytes("00 11 22 33 44"), decoded, sizeof(decoded), size), Code::Capacity);
    EXPECT(decodeHex(bytes("0001"), decoded, sizeof(decoded), size), Code::Syntax);
    EXPECT(decodeHex(bytes("GG"), decoded, sizeof(decoded), size), Code::Syntax);
    EXPECT(decodeHex(bytes("# only comment"), decoded, sizeof(decoded), size), Code::Truncated);
}

static Field firstField(size_t index) {
    OK(importCpc(fixture.cpc));
    return imported.cpus[0].fields[index];
}
static void protocolSuccess() {
    const Field highest = firstField(Highest), enable = firstField(Enable);
    SimulatedReadChannel channel;
    Value value{};
    EXPECT(channel.beginRead(0, 100), Code::NotInitialized);
    EXPECT(channel.poll(0), Code::NotInitialized);
    EXPECT(channel.sample(highest, value), Code::UnknownValue);
    CHECK(value.kind == ValueKind::Unknown);
    OK(channel.initialize(fixture.pcct.channels[0], 0, 0xcafebabedeadbeefULL, 0));
    EXPECT(channel.initialize(fixture.pcct.channels[0], 0, 0, 0), Code::AlreadyInitialized);
    Bytes memory = channel.simulatedMemory();
    CHECK(memory.size == 65536 && get(memory.data, 4) == 0x50434300);
    CHECK(memory.data[0] == 0 && memory.data[1] == 0x43 && memory.data[2] == 0x43 && memory.data[3] == 0x50);
    CHECK(get(memory.data + 4, 2) == 0 && get(memory.data + 6, 2) == 1);
    CHECK(channel.state() == ChannelState::OsOwned && !channel.platformMayOwn());
    EXPECT(channel.sample(highest, value), Code::UnknownValue); // Zeroed bytes are not data.
    OK(channel.beginRead(0, 10000));
    CHECK(channel.generation() == 1 && channel.platformMayOwn());
    CHECK(channel.simulatedDoorbell() == 0xcafebabe00000001ULL);
    CHECK(get(memory.data + 4, 2) == 0 && get(memory.data + 6, 2) == 0);
    bool payloadUntouched = true;
    for (size_t i = 8; i < memory.size; ++i) if (memory.data[i] != 0) payloadUntouched = false;
    CHECK(payloadUntouched); // READ controller never writes payload controls.
    EXPECT(channel.beginRead(1, 10000), Code::Busy); // All CPUs share this one owner.
    CHECK(channel.generation() == 1 && channel.simulatedDoorbell() == 0xcafebabe00000001ULL);
    OK(channel.simulateFirmwarePayload(highest.gas, 168));
    CHECK(get(memory.data + 8, 4) == 168); // Header offset is applied exactly once.
    EXPECT(channel.sample(highest, value), Code::UnknownValue);
    EXPECT(channel.poll(3999), Code::Pending);
    EXPECT(channel.poll(4000), Code::Pending); // Nominal 4ms is not the deadline.
    const Field lastNominal = imported.cpus[15].fields[Nominal];
    OK(channel.simulateFirmwarePayload(lastNominal.gas, 190));
    CHECK(get(memory.data + 8 + 0xf04, 4) == 190);
    OK(channel.simulateFirmwareStatus(kComplete));
    OK(channel.poll(6000));
    CHECK(channel.state() == ChannelState::OsOwned && !channel.platformMayOwn());
    OK(channel.sample(highest, value));
    CHECK(value.kind == ValueKind::Simulated && value.number == 168);
    EXPECT(channel.sample(enable, value), Code::UnknownValue); // Never supplied by fake firmware.
    OK(channel.sample(lastNominal, value));
    CHECK(value.kind == ValueKind::Simulated && value.number == 190);
    EXPECT(channel.sample(imported.cpus[0].fields[ReferenceCounter], value), Code::UnknownValue);
    OK(channel.sample(imported.cpus[0].fields[LowestFrequency], value));
    CHECK(value.kind == ValueKind::StaticInteger && value.number == 550);
    EXPECT(channel.poll(6001), Code::NoOutstanding);
    EXPECT(channel.simulateFirmwarePayload(highest.gas, 1), Code::NoOutstanding);
    OK(channel.beginRead(7000, 1000));
    OK(channel.simulateFirmwareStatus(kComplete));
    OK(channel.poll(7001));
    EXPECT(channel.sample(highest, value), Code::UnknownValue); // Stale bytes not a new sample.
    OK(channel.beginRead(8000, 1000));
    OK(channel.simulateFirmwarePayload(enable.gas, 0));
    OK(channel.simulateFirmwareStatus(kComplete));
    OK(channel.poll(8001));
    OK(channel.sample(enable, value));
    CHECK(value.kind == ValueKind::Simulated && value.number == 0); // Explicit fake zero.
}

static void channelSignatures() {
    const Field original = firstField(Highest);
    for (size_t i = 0; i < kMaxSubspaces; ++i) {
        const uint8_t id = static_cast<uint8_t>(i);
        Field field = original;
        field.gas.access = id;
        SimulatedReadChannel channel;
        OK(channel.initialize(fixture.pcct.channels[0], id, 0, 0));
        CHECK(get(channel.simulatedMemory().data, 4) == (kPccSignature | id));
        CHECK(channel.simulatedMemory().data[0] == id);
        OK(channel.beginRead(0, 10000));
        OK(channel.simulateFirmwarePayload(field.gas, 200));
        OK(channel.simulateFirmwareStatus(kComplete));
        OK(channel.poll(1));
        Value value{};
        OK(channel.sample(field, value));
        CHECK(value.kind == ValueKind::Simulated && value.number == 200);
        OK(channel.beginRead(1000, 10000)); // Validate the tagged header again.
        OK(channel.simulateFirmwareStatus(kComplete));
        OK(channel.poll(1001));
        EXPECT(channel.sample(field, value), Code::UnknownValue);

        for (size_t other = 0; other <= kMaxSubspaces; ++other) {
            if (other == i) continue;
            SimulatedReadChannel mismatched;
            OK(mismatched.initialize(fixture.pcct.channels[0], id, 0, 0));
            OK(mismatched.beginRead(0, 10000));
            OK(mismatched.simulateHeaderCorruption(0, static_cast<uint8_t>(other)));
            OK(mismatched.simulateFirmwareStatus(kComplete));
            EXPECT(mismatched.poll(1), Code::Protocol);
            CHECK(mismatched.state() == ChannelState::Fault && mismatched.platformMayOwn());
            EXPECT(mismatched.beginRead(1000, 10000), Code::Faulted);
        }
    }
}

static void protocolErrors() {
    const Field highest = firstField(Highest);
    SimulatedReadChannel channel;
    OK(channel.initialize(fixture.pcct.channels[0], 0, 0, 0));
    OK(channel.beginRead(0, 10000));
    OK(channel.simulateFirmwarePayload(highest.gas, 99));
    OK(channel.simulateFirmwareStatus(kError));
    EXPECT(channel.poll(5), Code::Pending);
    CHECK(channel.platformMayOwn());
    OK(channel.simulateFirmwareStatus(kComplete | kError));
    EXPECT(channel.poll(10), Code::FirmwareError);
    CHECK(!channel.platformMayOwn());
    Value value{};
    EXPECT(channel.sample(highest, value), Code::UnknownValue);
    EXPECT(channel.beginRead(11, 1000), Code::RateLimited); // Errors consume the rate budget.
    OK(channel.beginRead(1000, 1000));
    Gas bad = highest.gas; bad.access = 1;
    EXPECT(channel.simulateFirmwarePayload(bad, 1), Code::Subspace);
    bad = highest.gas; bad.width = 0;
    EXPECT(channel.simulateFirmwarePayload(bad, 1), Code::Gas);
    bad = highest.gas; bad.bitOffset = 1;
    EXPECT(channel.simulateFirmwarePayload(bad, 1), Code::Gas);
    bad = highest.gas; bad.address = UINT64_MAX;
    EXPECT(channel.simulateFirmwarePayload(bad, 1), Code::Overflow);
    bad = highest.gas; bad.address = 65528;
    EXPECT(channel.simulateFirmwarePayload(bad, 1), Code::Bounds);
    EXPECT(channel.simulateFirmwarePayload(highest.gas, UINT64_MAX), Code::Bounds);
    EXPECT(channel.simulateHeaderCorruption(8, 1), Code::Bounds);
    OK(channel.simulateFirmwarePayload(highest.gas, 77));
    OK(channel.simulateFirmwareStatus(kComplete));
    OK(channel.poll(1001));
    Field wrong = highest; ++wrong.byteOffset;
    EXPECT(channel.sample(wrong, value), Code::Bounds);
    wrong = highest; wrong.gas.access = 1;
    EXPECT(channel.sample(wrong, value), Code::Subspace);
    wrong = highest; wrong.gas.address = UINT64_MAX;
    EXPECT(channel.sample(wrong, value), Code::Overflow);
}

static void timeoutQuarantine() {
    const Field highest = firstField(Highest);
    SimulatedReadChannel channel;
    OK(channel.initialize(fixture.pcct.channels[0], 0, 0x12340000abcdef12ULL, 100));
    OK(channel.beginRead(100, 50)); // Deliberately shorter than nominal; not a guarantee.
    EXPECT(channel.poll(149), Code::Pending);
    EXPECT(channel.poll(150), Code::Timeout);
    CHECK(channel.state() == ChannelState::Quarantine && channel.platformMayOwn());
    EXPECT(channel.beginRead(150, 1000), Code::Quarantined);
    EXPECT(channel.initialize(fixture.pcct.channels[0], 0, 0, 150), Code::AlreadyInitialized);
    OK(channel.simulateFirmwarePayload(highest.gas, 999));
    EXPECT(channel.poll(100000), Code::Quarantined);
    EXPECT(channel.beginRead(100000, 1000), Code::Quarantined);
    CHECK(channel.generation() == 1 && get(channel.simulatedMemory().data + 6, 2) == 0);
    OK(channel.simulateFirmwareStatus(kComplete));
    EXPECT(channel.poll(100001), Code::LateComplete);
    CHECK(channel.state() == ChannelState::OsOwned && !channel.platformMayOwn());
    Value value{};
    EXPECT(channel.sample(highest, value), Code::UnknownValue);
    OK(channel.beginRead(100001, 1000));
    CHECK(channel.generation() == 2);
    EXPECT(channel.poll(100001), Code::Pending); // Old completion was cleared before reuse.
    OK(channel.simulateFirmwareStatus(kError));
    EXPECT(channel.poll(101001), Code::Timeout); // Error alone does not release ownership.
    OK(channel.simulateFirmwareStatus(kError | kComplete));
    EXPECT(channel.poll(101002), Code::LateError);
    EXPECT(channel.sample(highest, value), Code::UnknownValue);

    SimulatedReadChannel late;
    OK(late.initialize(fixture.pcct.channels[0], 0, 0, 0));
    OK(late.beginRead(0, 100));
    OK(late.simulateFirmwarePayload(highest.gas, 200));
    OK(late.simulateFirmwareStatus(kComplete));
    EXPECT(late.poll(100), Code::LateComplete); // No earlier timeout poll; exact boundary is late.
    EXPECT(late.sample(highest, value), Code::UnknownValue);
    OK(late.beginRead(1000, 100));
    OK(late.simulateFirmwareStatus(kComplete | kError));
    EXPECT(late.poll(1101), Code::LateError);
    OK(late.beginRead(2000, 100));
    OK(late.simulateFirmwareStatus(kComplete));
    OK(late.poll(2099)); // Just before deadline is timely.
}

static void rateAndTurnaround() {
    SimulatedReadChannel channel;
    OK(channel.initialize(fixture.pcct.channels[0], 0, 0, 0));
    OK(channel.beginRead(0, 100));
    OK(channel.simulateFirmwareStatus(kComplete));
    OK(channel.poll(0)); // Time zero is real; not a sentinel.
    EXPECT(channel.beginRead(999, 100), Code::RateLimited);
    OK(channel.beginRead(1000, 100));
    OK(channel.simulateFirmwareStatus(kComplete));
    OK(channel.poll(1000));
    EXPECT(channel.beginRead(1999, 100), Code::RateLimited);
    OK(channel.beginRead(2000, 100));

    ChannelDescriptor d = fixture.pcct.channels[0];
    d.maxCommandsPerMinute = 2;
    SimulatedReadChannel slow;
    OK(slow.initialize(d, 0, 0, 0));
    OK(slow.beginRead(0, 100)); OK(slow.simulateFirmwareStatus(kComplete)); OK(slow.poll(0));
    EXPECT(slow.beginRead(29999999, 100), Code::RateLimited);
    OK(slow.beginRead(30000000, 100)); OK(slow.simulateFirmwareStatus(kComplete)); OK(slow.poll(30000000));
    EXPECT(slow.beginRead(59999999, 100), Code::RateLimited);
    OK(slow.beginRead(60000000, 100)); // No fixed-window burst loophole.

    d.maxCommandsPerMinute = 0; d.minTurnaroundUs = 25;
    SimulatedReadChannel turn;
    OK(turn.initialize(d, 0, 0, 0));
    OK(turn.beginRead(0, 100)); OK(turn.simulateFirmwareStatus(kComplete)); OK(turn.poll(0));
    EXPECT(turn.beginRead(24, 100), Code::Turnaround);
    OK(turn.beginRead(25, 100)); OK(turn.simulateFirmwareStatus(kComplete | kError));
    EXPECT(turn.poll(30), Code::FirmwareError);
    EXPECT(turn.beginRead(54, 100), Code::Turnaround);
    OK(turn.beginRead(55, 1)); EXPECT(turn.poll(56), Code::Timeout);
    OK(turn.simulateFirmwareStatus(kComplete)); EXPECT(turn.poll(100), Code::LateComplete);
    EXPECT(turn.beginRead(124, 100), Code::Turnaround); // Based on observed late completion.
    OK(turn.beginRead(125, 100));

    d.minTurnaroundUs = 0;
    SimulatedReadChannel unlimited;
    OK(unlimited.initialize(d, 0, 0, 0));
    for (size_t i = 0; i < 3; ++i) {
        OK(unlimited.beginRead(0, 1)); OK(unlimited.simulateFirmwareStatus(kComplete)); OK(unlimited.poll(0));
    }
    d.maxCommandsPerMinute = UINT32_MAX;
    SimulatedReadChannel fast;
    OK(fast.initialize(d, 0, 0, 0));
    OK(fast.beginRead(0, 1)); OK(fast.simulateFirmwareStatus(kComplete)); OK(fast.poll(0));
    EXPECT(fast.beginRead(0, 1), Code::RateLimited); // Integer resolution rounds UP, not to zero.
    OK(fast.beginRead(1, 1));
}

static void timeValidation() {
    uint64_t result = 123;
    CHECK(checkedAdd(UINT64_MAX, 0, result) && result == UINT64_MAX);
    CHECK(!checkedAdd(UINT64_MAX, 1, result));
    CHECK(!checkedAdd(UINT64_MAX - 7, 8, result));
    SimulatedReadChannel channel;
    OK(channel.initialize(fixture.pcct.channels[0], 0, 0, 100));
    EXPECT(channel.beginRead(99, 1), Code::TimeBackwards);
    EXPECT(channel.beginRead(100, 0), Code::Deadline);
    EXPECT(channel.beginRead(100, kMaxDeadlineUs + 1), Code::Deadline);
    CHECK(channel.generation() == 0 && get(channel.simulatedMemory().data + 6, 2) == 1);
    OK(channel.beginRead(100, 100));
    EXPECT(channel.poll(150), Code::Pending);
    EXPECT(channel.poll(149), Code::TimeBackwards);
    EXPECT(channel.beginRead(149, 100), Code::TimeBackwards);
    CHECK(channel.state() == ChannelState::InFlight && channel.generation() == 1);
    EXPECT(channel.poll(200), Code::Timeout);
    EXPECT(channel.poll(199), Code::TimeBackwards);
    EXPECT(channel.beginRead(201, 100), Code::Quarantined);
    EXPECT(channel.poll(UINT64_MAX), Code::Quarantined); // No overflow from elapsed time.

    SimulatedReadChannel overflow;
    OK(overflow.initialize(fixture.pcct.channels[0], 0, 0, UINT64_MAX - 5));
    EXPECT(overflow.beginRead(UINT64_MAX - 5, 6), Code::Overflow);
    CHECK(overflow.generation() == 0 && overflow.state() == ChannelState::OsOwned);
    OK(overflow.beginRead(UINT64_MAX - 5, 5));
    OK(overflow.simulateFirmwareStatus(kComplete));
    OK(overflow.poll(UINT64_MAX - 1));
    EXPECT(overflow.beginRead(UINT64_MAX, 1), Code::Overflow);
    CHECK(overflow.generation() == 1 && !overflow.platformMayOwn());

    ChannelDescriptor d = fixture.pcct.channels[0];
    d.maxCommandsPerMinute = 0; d.minTurnaroundUs = 65535;
    SimulatedReadChannel nearEnd;
    OK(nearEnd.initialize(d, 0, 0, UINT64_MAX - 10));
    OK(nearEnd.beginRead(UINT64_MAX - 10, 5));
    OK(nearEnd.simulateFirmwareStatus(kComplete)); OK(nearEnd.poll(UINT64_MAX - 9));
    EXPECT(nearEnd.beginRead(UINT64_MAX - 1, 1), Code::Turnaround); // No wrapping next-allowed timestamp.
}

static void protocolFaults() {
    for (uint16_t status : {kSciDoorbell, kPlatformNotify, uint16_t(0x8000),
                           uint16_t(kComplete | kSciDoorbell)}) {
        SimulatedReadChannel channel;
        OK(channel.initialize(fixture.pcct.channels[0], 0, 0, 0));
        OK(channel.beginRead(0, 100));
        OK(channel.simulateFirmwareStatus(status));
        EXPECT(channel.poll(1), Code::Protocol);
        CHECK(channel.state() == ChannelState::Fault && channel.platformMayOwn());
        EXPECT(channel.beginRead(1000, 100), Code::Faulted);
        EXPECT(channel.poll(1000), Code::Faulted);
        EXPECT(channel.initialize(fixture.pcct.channels[0], 0, 0, 1000), Code::AlreadyInitialized);
    }
    for (size_t offset : {size_t(0), size_t(4), size_t(5)}) {
        SimulatedReadChannel channel;
        OK(channel.initialize(fixture.pcct.channels[0], 0, 0, 0));
        OK(channel.beginRead(0, 100));
        OK(channel.simulateHeaderCorruption(offset, 1));
        EXPECT(channel.poll(1), Code::Protocol);
        EXPECT(channel.beginRead(1000, 100), Code::Faulted);
    }
    ChannelDescriptor d = fixture.pcct.channels[0];
    d.doorbell.width = 8; d.doorbell.access = 1; d.preserveMask = 0xf0; d.writeMask = 1;
    SimulatedReadChannel byteBell;
    EXPECT(byteBell.initialize(d, 8, 0, 0), Code::Subspace);
    EXPECT(byteBell.initialize(d, 0, 0x100, 0), Code::Bounds);
    OK(byteBell.initialize(d, 0, 0xa4, 0));
    OK(byteBell.beginRead(0, 1));
    CHECK(byteBell.simulatedDoorbell() == 0xa1);
    d.writeMask = 0x10; // Overlapping preserve/set masks: set wins by the PCC formula.
    SimulatedReadChannel overlap;
    OK(overlap.initialize(d, 0, 0x20, 0)); OK(overlap.beginRead(0, 1));
    CHECK(overlap.simulatedDoorbell() == 0x30);
    d.doorbell.width = 0;
    SimulatedReadChannel invalid;
    EXPECT(invalid.initialize(d, 0, 0, 0), Code::Gas);
}

static void boundedMutationSweep() {
    // Deterministic parser robustness, not timing/benchmarking. Every call is
    // the real importer/parser; no independent protocol transcription.
    uint32_t random = 0x91f34527;
    uint8_t raw[128]{};
    StaticCpc cpc{};
    CpuMap map{};
    Gas gas{};
    Pcct pcct{};
    Madt madt{};
    for (size_t iteration = 0; iteration < 512; ++iteration) {
        for (uint8_t &b : raw) {
            random ^= random << 13; random ^= random >> 17; random ^= random << 5;
            b = static_cast<uint8_t>(random);
        }
        const size_t size = iteration % sizeof(raw);
        CHECK(parsePcct({raw, size}, pcct) != Code::Ok);
        CHECK(parseMadt({raw, size}, madt) != Code::Ok);
        CHECK(parseStaticCpc({raw, size}, cpc) != Code::Ok);
        CHECK(parseCpuMap({raw, size}, map) != Code::Ok);
        CHECK(parseGasResource({raw, size}, gas) != Code::Ok);
    }
}

static void run(const char *name, void (*test)()) {
    const size_t before = failures, start = checks;
    test();
    ++groups;
    printf("%s %s (%zu checks)\n", before == failures ? "PASS" : "FAIL", name, checks - start);
}
int main(int argc, char **argv) {
    static_assert(!std::is_copy_constructible<SimulatedReadChannel>::value, "shared owner cannot be copied");
    static_assert(!std::is_move_constructible<SimulatedReadChannel>::value, "shared owner cannot be moved");
    if (argc != 5) { fprintf(stderr, "Usage: tests PCCT.hex MADT.hex STATIC.cpc XNU.map\n"); return 2; }
    if (!cppc_lab_host::loadInputs(true, argv[1], argv[2], argv[3], argv[4], fixture)) return 2;
    uint8_t text[kMaxTextBytes];
    size_t size = 0;
    if (!cppc_lab_host::readFile(argv[3], text, sizeof(text), size)) return 2;
    cpcText.assign(reinterpret_cast<const char *>(text), size);
    if (!cppc_lab_host::readFile(argv[4], text, sizeof(text), size)) return 2;
    mapText.assign(reinterpret_cast<const char *>(text), size);
    if (importCpc(fixture.cpc) != Code::Ok) { fprintf(stderr, "Fixture import failed\n"); return 2; }
    run("16-CPU/8-domain captured fixture and explicit unknowns", importCapture);
    run("raw header/length/checksum and every-byte mutations", rawHeaders);
    run("generic PCCT and doorbell validation", pcctValidation);
    run("PCC subspace ordinals and channel alias rejection", subspaces);
    run("reordered identities, duplicate/missing/disabled joins", identityJoins);
    run("explicit x2APIC namespace and wide CPU identities", x2Apic);
    run("GAS/resource/descriptor bounds and unsupported forms", descriptorRejects);
    run("static integer semantics and HW_ALL domains", integersDomains);
    run("paired minimum/maximum controls and both absence forms", pairedBounds);
    run("PCC field collisions and permitted shared registers", fieldAliases);
    run("normalized static and caller-map frontend mutations", frontends);
    run("simulated READ ownership, wire layout, values and provenance", protocolSuccess);
    run("all channel signatures and mismatched subspace tags", channelSignatures);
    run("completion errors and simulated payload bounds", protocolErrors);
    run("timeout quarantine, late completion/error, no premature reuse", timeoutQuarantine);
    run("rolling-safe rate spacing and completion turnaround", rateAndTurnaround);
    run("bounded deadlines, backwards and overflowing time", timeValidation);
    run("protocol faults, notifications and doorbell preservation", protocolFaults);
    run("bounded deterministic untrusted-input mutation sweep", boundedMutationSweep);
    printf("RESULT: %zu groups, %zu checks, %zu failures\n", groups, checks, failures);
    return failures == 0 ? 0 : 1;
}
