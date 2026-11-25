// Standalone build+run from the repository root (does not use either Makefile):
// clang++ -std=c++17 -O1 -g -Wall -Wextra -Werror -Wno-unused-function -fsanitize=address,undefined -fno-sanitize-recover=all -fno-exceptions -fno-rtti -IAMDZenPM/src tests/test_pcc.cpp AMDZenPM/src/ZenPccModel.cpp -o /private/tmp/amdzen-test-pcc && /private/tmp/amdzen-test-pcc
// Only synthetic byte buffers/fake firmware: no kernel, device, or MMIO access.
#include "ZenPccModel.hpp"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <thread>
#include <vector>

using namespace zenpcc;
static unsigned assertions = 0, cases = 0;
#define CHECK(expr) do { ++assertions; if (!(expr)) { fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #expr); abort(); } } while (0)

static void put(uint8_t *p, size_t n, uint64_t value) {
    for (size_t i = 0; i < n; ++i) p[i] = uint8_t(value >> (8 * i));
}
static uint64_t get(const uint8_t *p, size_t n) {
    uint64_t value = 0;
    for (size_t i = 0; i < n; ++i) value |= uint64_t(p[i]) << (8 * i);
    return value;
}
static void checksum(std::vector<uint8_t> &v) {
    put(v.data() + 4, 4, v.size());
    v[9] = 0;
    uint8_t sum = 0;
    for (uint8_t c : v) sum = uint8_t(sum + c);
    v[9] = uint8_t(0 - sum);
}
static Bytes bytes(const std::vector<uint8_t> &v) { return {v.data(), v.size()}; }
static Descriptor descriptor() {
    return {0xbce90000, 65536, {0, 64, 0, 4, 0xfd010540},
            UINT64_C(0xffffffff00000000), 1, 4000, 60000, 0};
}
static std::vector<uint8_t> pcct() {
    const Descriptor d = descriptor();
    std::vector<uint8_t> v(110, 0);
    memcpy(v.data(), "PCCT", 4); v[8] = 2;
    v[49] = 62;
    put(v.data() + 56, 8, d.baseAddress); put(v.data() + 64, 8, d.length);
    v[72] = 0; v[73] = 64; v[75] = 4;
    put(v.data() + 76, 8, d.doorbell.address);
    put(v.data() + 84, 8, d.preserveMask); put(v.data() + 92, 8, d.writeMask);
    put(v.data() + 100, 4, d.nominalLatencyUs); put(v.data() + 104, 4, d.maxCommandsPerMinute);
    checksum(v); return v;
}
static const uint32_t offsets[FieldCount] = {
    0, 4, 8, 12, UINT32_MAX, 0x14, 0x18, 0x1c, UINT32_MAX, UINT32_MAX, UINT32_MAX,
    UINT32_MAX, UINT32_MAX, 0x40, 0x44, 0x48, UINT32_MAX, 0x50, UINT32_MAX,
    UINT32_MAX, UINT32_MAX
};
struct Package {
    CpcEntry entries[kCpcEntries]{};
    uint8_t resources[FieldCount][17]{};
    explicit Package(uint32_t cpu = 0) {
        entries[0] = {EntryKind::Integer, 23, {nullptr, 0}};
        entries[1] = {EntryKind::Integer, 3, {nullptr, 0}};
        for (uint32_t f = 0; f < FieldCount; ++f) {
            uint8_t *p = resources[f];
            p[0] = 0x82; p[1] = 12; p[15] = 0x79;
            if (offsets[f] != UINT32_MAX) {
                p[3] = 0x0a; p[4] = 32; put(p + 7, 8, cpu * 0x100 + offsets[f]);
            } else if (f == ReferenceCounter || f == DeliveredCounter) {
                p[3] = 0x7f; p[4] = 64; p[6] = 4;
                put(p + 7, 8, f == ReferenceCounter ? 0xe7 : 0xe8);
            }
            entries[f + 2] = {EntryKind::Buffer, 0, {p, 17}};
        }
        entries[LowestFrequency + 2] = {EntryKind::Integer, 550, {nullptr, 0}};
        entries[NominalFrequency + 2] = {EntryKind::Integer, 3401, {nullptr, 0}};
    }
    Package(const Package &) = delete;
    Package &operator=(const Package &) = delete;
};
static CpuRegisters cpuMap(uint32_t cpu, uint32_t count) {
    Package p(cpu);
    CpuRegisters out{};
    CHECK(parseCpc(p.entries, 23, descriptor(), out) == Error::Ok);
    out.acpiId = cpu; out.apicId = cpu;
    out.domain = count == 1 ? cpu : cpu / 2; out.domainMembers = count == 1 ? 1 : 2;
    return out;
}
static std::vector<uint8_t> madt(uint32_t count, bool x2 = false) {
    const uint32_t size = x2 ? 16 : 8;
    std::vector<uint8_t> v(44 + count * size, 0);
    memcpy(v.data(), "APIC", 4); v[8] = 5;
    for (uint32_t cpu = 0; cpu < count; ++cpu) {
        uint8_t *p = v.data() + 44 + cpu * size;
        p[0] = x2 ? 9 : 0; p[1] = uint8_t(size);
        if (x2) { put(p + 4, 4, cpu); put(p + 8, 4, 1); put(p + 12, 4, 100 + cpu); }
        else { p[2] = uint8_t(cpu); p[3] = uint8_t(cpu); put(p + 4, 4, 1); }
    }
    checksum(v); return v;
}
static std::vector<uint8_t> processors(uint32_t count) {
    std::vector<uint8_t> v(44 + count * 13, 0);
    memcpy(v.data(), "SSDT", 4); v[8] = 2;
    memcpy(v.data() + 16, "CPUSSDT", 7);
    v[36] = 0x10;
    const uint32_t length = uint32_t(v.size()) - 37;
    v[37] = uint8_t(0x40 | (length & 15)); v[38] = uint8_t(length >> 4);
    memcpy(v.data() + 39, "\\_PR_", 5);
    for (uint32_t cpu = 0; cpu < count; ++cpu) {
        uint8_t *p = v.data() + 44 + cpu * 13;
        p[0] = 0x5b; p[1] = 0x83; p[2] = 11;
        char name[5]; snprintf(name, sizeof(name), "C%03X", cpu);
        memcpy(p + 3, name, 4); p[7] = uint8_t(count - 1 - cpu);
        put(p + 8, 4, 0x810); p[12] = 6;
    }
    checksum(v); return v;
}

enum class Op { Read16, Write16, Read32, Write32, Read64, Ring, Fence, Wait, FirmwareComplete };
struct Event { Op op; uint32_t offset; uint64_t value, time; };
class Fake final : public Accessor {
public:
    std::vector<uint8_t> memory;
    std::vector<Event> events;
    std::vector<uint64_t> rings;
    uint64_t now = 0, bell = UINT64_C(0xaabbccdd12345678), delay = 100, ready = 0;
    bool pending = false, neverComplete = false, corruptSignature = false;
    bool failRead16 = false, failRead64 = false, failWrite64 = false;
    uint32_t failWriteOffset = UINT32_MAX;
    uint16_t completionStatus = kComplete;
    uint32_t readHighest = 0, firmwareWrites = 0;
    uint32_t ringAdvanceUs = 0, completionFenceAdvanceUs = 0;
    const CpuRegisters *cpus; uint32_t count;
    Fake(const CpuRegisters *maps, uint32_t ncpu) : memory(65536, 0), cpus(maps), count(ncpu) {
        put(memory.data(), 4, kSignature); put(memory.data() + 6, 2, kComplete);
        for (uint32_t cpu = 0; cpu < count; ++cpu) {
            seed(cpu, Highest, 166); seed(cpu, Nominal, 136);
            seed(cpu, LowestNonlinear, 82); seed(cpu, Lowest, 29);
            seed(cpu, Minimum, 29); seed(cpu, Maximum, 166);
            seed(cpu, Desired, 0); seed(cpu, Epp, 128);
        }
    }
    void seed(uint32_t cpu, FieldIndex f, uint32_t value) {
        if (cpus[cpu].fields[f].kind == FieldKind::Pcc)
            put(memory.data() + cpus[cpu].fields[f].offset, 4, value);
    }
    uint32_t value(uint32_t cpu, FieldIndex f) const {
        return uint32_t(get(memory.data() + cpus[cpu].fields[f].offset, 4));
    }
    void event(Op op, uint32_t offset = 0, uint64_t value = 0) { events.push_back({op, offset, value, now}); }
    void firmware() {
        if (pending && !neverComplete && now >= ready) {
            if (get(memory.data() + 4, 2) == 1) ++firmwareWrites;
            if (readHighest && get(memory.data() + 4, 2) == 0)
                for (uint32_t cpu = 0; cpu < count; ++cpu) seed(cpu, Highest, readHighest);
            put(memory.data() + 6, 2, completionStatus);
            if (corruptSignature) put(memory.data(), 4, kSignature ^ 1);
            pending = false; event(Op::FirmwareComplete);
        }
    }
    bool read16(uint32_t offset, uint16_t &out) override {
        if (failRead16) return false;
        firmware(); out = uint16_t(get(memory.data() + offset, 2)); event(Op::Read16, offset, out); return true;
    }
    bool write16(uint32_t offset, uint16_t value) override {
        CHECK(!pending); put(memory.data() + offset, 2, value); event(Op::Write16, offset, value); return true;
    }
    bool read32(uint32_t offset, uint32_t &out) override {
        CHECK(!pending); out = uint32_t(get(memory.data() + offset, 4)); event(Op::Read32, offset, out); return true;
    }
    bool write32(uint32_t offset, uint32_t value) override {
        CHECK(!pending);
        if (offset == failWriteOffset) return false;
        put(memory.data() + offset, 4, value); event(Op::Write32, offset, value); return true;
    }
    bool read64(uint64_t &out) override {
        if (failRead64) return false;
        out = bell; event(Op::Read64, 0, out); return true;
    }
    bool write64(uint64_t value) override {
        if (failWrite64) return false;
        CHECK(!pending); CHECK(get(memory.data() + 6, 2) == 0);
        bell = value; event(Op::Ring, 0, value); rings.push_back(now);
        pending = true; ready = now + delay; now += ringAdvanceUs; return true;
    }
    void barrier() override {
        const bool completeRead = !events.empty() && events.back().op == Op::Read16 &&
                                  (events.back().value & kComplete) && !rings.empty();
        event(Op::Fence);
        if (completeRead) { now += completionFenceAdvanceUs; completionFenceAdvanceUs = 0; }
    }
    uint64_t nowUs() override { return now; }
    void waitUs(uint32_t us) override { CHECK(us > 0 && us <= 1000); event(Op::Wait, 0, us); now += us; }
};
static size_t find(const Fake &f, Op op, size_t start = 0) {
    for (size_t i = start; i < f.events.size(); ++i) if (f.events[i].op == op) return i;
    return f.events.size();
}
static size_t occurrences(const Fake &f, Op op) {
    size_t count = 0; for (const Event &e : f.events) if (e.op == op) ++count; return count;
}
static void initialize(Engine &e, Fake &f, const CpuRegisters *cpus, uint32_t count,
                       const Descriptor &d = descriptor()) {
    CHECK(e.initialize(d, cpus, count, f) == Error::Ok);
    CHECK(f.events.empty()); CHECK(e.commandCount() == 0);
}
static void cacheCaps(Engine &e, Fake &f) {
    zencppc::Caps caps{};
    for (uint32_t cpu = 0; cpu < f.count; ++cpu) CHECK(e.readCaps(cpu,&caps));
    f.events.clear(); f.rings.clear();
}

static void pcctFacts() {
    Descriptor d{}; auto v = pcct();
    CHECK(parsePcct(bytes(v), d) == Error::Ok);
    CHECK(d.baseAddress == 0xbce90000 && d.length == 65536);
    CHECK(d.doorbell.address == 0xfd010540 && d.doorbell.width == 64 && d.doorbell.access == 4);
    CHECK(d.preserveMask == UINT64_C(0xffffffff00000000) && d.writeMask == 1);
    CHECK(d.nominalLatencyUs == 4000 && d.maxCommandsPerMinute == 60000 && d.minTurnaroundUs == 0);
}
static void pcctValidation() {
    struct Mutation { uint32_t offset; uint8_t value; Error error; };
    const Mutation mutations[] = {{8,1,Error::Revision}, {36,1,Error::Unsupported}, {40,1,Error::Reserved},
        {48,1,Error::Unsupported}, {49,61,Error::Length}, {50,1,Error::Reserved},
        {72,1,Error::Unsupported}, {73,32,Error::Gas}, {74,1,Error::Gas}, {75,3,Error::Gas},
        {56,1,Error::Alignment}, {76,1,Error::Alignment}};
    for (const auto &m : mutations) {
        auto v = pcct(); v[m.offset] = m.value; checksum(v); Descriptor d{};
        CHECK(parsePcct(bytes(v), d) == m.error);
    }
    auto v = pcct(); Descriptor d{}; v[10] ^= 1;
    CHECK(parsePcct(bytes(v), d) == Error::Checksum);
    v = pcct(); v[0] = 'X'; CHECK(parsePcct(bytes(v), d) == Error::Signature);
    v = pcct(); v.resize(100); checksum(v); CHECK(parsePcct(bytes(v), d) == Error::Truncated);
    v = pcct(); v.resize(112); checksum(v); CHECK(parsePcct(bytes(v), d) == Error::Subspace);
    v = pcct(); v[4] = 109; CHECK(parsePcct(bytes(v), d) == Error::Length);
    d = descriptor(); d.baseAddress = UINT64_MAX - 7; CHECK(validateDescriptor(d) == Error::Overflow);
    d = descriptor(); d.length = 8; CHECK(validateDescriptor(d) == Error::Bounds);
    d = descriptor(); d.doorbell.address = d.baseAddress; CHECK(validateDescriptor(d) == Error::Aliasing);
    d = descriptor(); d.writeMask = 0; CHECK(validateDescriptor(d) == Error::Gas);
}
static void gasValidation() {
    Package p; Gas g{}; auto b = p.entries[Highest + 2].buffer;
    CHECK(parseGas(b, g) == Error::Ok && g.space == 10 && g.width == 32 && g.address == 0);
    CHECK(parseGas({b.data, 16}, g) == Error::Length);
    p.resources[Highest][15] = 0; CHECK(parseGas(b, g) == Error::Gas);
    p.resources[Highest][15] = 0x79; p.resources[Highest][16] = 1;
    CHECK(parseGas(b, g) == Error::Checksum);
    uint8_t sum = 0; p.resources[Highest][16] = 0;
    for (uint8_t c : p.resources[Highest]) sum = uint8_t(sum + c);
    p.resources[Highest][16] = uint8_t(0 - sum); CHECK(parseGas(b, g) == Error::Ok);
}
static void cpcFacts() {
    CpuRegisters cpus[16]{};
    for (uint32_t cpu = 0; cpu < 16; ++cpu) {
        cpus[cpu] = cpuMap(cpu, 16);
        for (uint32_t f = 0; f < FieldCount; ++f)
            if (offsets[f] != UINT32_MAX) {
                CHECK(cpus[cpu].fields[f].kind == FieldKind::Pcc);
                CHECK(cpus[cpu].fields[f].offset == 8 + cpu * 0x100 + offsets[f]);
            }
        CHECK(cpus[cpu].fields[ReferenceCounter].kind == FieldKind::FfhCounter);
        CHECK(cpus[cpu].fields[DeliveredCounter].kind == FieldKind::FfhCounter);
        CHECK(cpus[cpu].fields[Guaranteed].kind == FieldKind::Absent);
        CHECK(cpus[cpu].fields[NominalFrequency].value == 3401);
        CHECK(cpus[cpu].fields[LowestFrequency].value == 550);
    }
    CHECK(validateCpuMaps(cpus, 16) == Error::Ok);
}
static void cpcValidation() {
    CpuRegisters out{};
    { Package p; p.resources[Minimum][4] = 16; CHECK(parseCpc(p.entries,23,descriptor(),out) == Error::Gas); }
    { Package p; p.resources[Minimum][5] = 1; CHECK(parseCpc(p.entries,23,descriptor(),out) == Error::Gas); }
    { Package p; p.resources[Minimum][6] = 1; CHECK(parseCpc(p.entries,23,descriptor(),out) == Error::Subspace); }
    { Package p; p.resources[Minimum][3] = 0; CHECK(parseCpc(p.entries,23,descriptor(),out) == Error::Unsupported); }
    { Package p; put(p.resources[Minimum]+7,8,1); CHECK(parseCpc(p.entries,23,descriptor(),out) == Error::Alignment); }
    { Package p; put(p.resources[Minimum]+7,8,65528); CHECK(parseCpc(p.entries,23,descriptor(),out) == Error::Bounds); }
    { Package p; put(p.resources[Minimum]+7,8,UINT64_MAX); CHECK(parseCpc(p.entries,23,descriptor(),out) == Error::Overflow); }
    { Package p; p.entries[Minimum+2] = {EntryKind::Integer,0,{nullptr,0}}; CHECK(parseCpc(p.entries,23,descriptor(),out) == Error::Unsupported); }
    { Package p; p.entries[Highest+2] = {EntryKind::Integer,0,{nullptr,0}}; CHECK(parseCpc(p.entries,23,descriptor(),out) == Error::BadValue); }
    { Package p; p.entries[Highest+2] = {EntryKind::Integer,UINT64_C(0x100000000),{nullptr,0}}; CHECK(parseCpc(p.entries,23,descriptor(),out) == Error::Overflow); }
    { Package p; p.entries[Highest+2] = {EntryKind::Integer,200,{nullptr,0}}; CHECK(parseCpc(p.entries,23,descriptor(),out) == Error::Ok && out.fields[Highest].value == 200); }
    { Package p; p.entries[0].integer = 22; CHECK(parseCpc(p.entries,23,descriptor(),out) == Error::Length); }
    { Package p; p.entries[1].integer = 4; CHECK(parseCpc(p.entries,23,descriptor(),out) == Error::Revision); }
    { Package p; p.entries[Epp+2].kind = EntryKind::Invalid; CHECK(parseCpc(p.entries,23,descriptor(),out) == Error::Unsupported); }
    { Package p; put(p.resources[DeliveredCounter]+7,8,0xe7); CHECK(parseCpc(p.entries,23,descriptor(),out) == Error::Gas); }
}
static void revisionTwo() {
    Package p; CpuRegisters out{}; p.entries[0].integer = 21; p.entries[1].integer = 2;
    CHECK(parseCpc(p.entries,21,descriptor(),out) == Error::Ok);
    CHECK(out.fields[NominalFrequency].kind == FieldKind::Absent);
    CHECK(out.fields[LowestFrequency].kind == FieldKind::Absent);
    CHECK(out.fields[Epp].kind == FieldKind::Pcc);
}
static void aliasValidation() {
    Package p; CpuRegisters out{}; put(p.resources[Minimum]+7,8,0);
    CHECK(parseCpc(p.entries,23,descriptor(),out) == Error::Aliasing);
    CpuRegisters cpus[] = {cpuMap(0,2), cpuMap(1,2)};
    cpus[1].fields[Minimum].offset = cpus[0].fields[Highest].offset;
    CHECK(validateCpuMaps(cpus,2) == Error::Aliasing);
    cpus[1].fields[Minimum].offset = cpus[0].fields[Minimum].offset;
    CHECK(validateCpuMaps(cpus,2) == Error::Ok);
    cpus[1].domainMembers = 1; CHECK(validateCpuMaps(cpus,2) == Error::Domain);
}
static void identityJoin() {
    auto v = madt(16); Madt table{}; CHECK(parseMadt(bytes(v),table) == Error::Ok && table.count == 16);
    uint16_t apic[16]; for (uint32_t i = 0; i < 16; ++i) apic[i] = uint16_t(15-i);
    for (uint32_t i = 0; i < 16; ++i) { uint32_t cpu; CHECK(joinCpu(table,i,false,apic,16,cpu) == Error::Ok && cpu == 15-i); }
    uint32_t cpu; CHECK(joinCpu(table,100,false,apic,16,cpu) == Error::MissingIdentity);
    table.cpus[0].enabled = false; CHECK(joinCpu(table,0,false,apic,16,cpu) == Error::DisabledIdentity);
    apic[1] = apic[0]; CHECK(joinCpu(table,1,false,apic,16,cpu) == Error::DuplicateIdentity);
    v = madt(2,true); CHECK(parseMadt(bytes(v),table) == Error::Ok);
    const uint16_t ids[] = {1,0}; CHECK(joinCpu(table,100,true,ids,2,cpu) == Error::Ok && cpu == 1);
}
static void madtValidation() {
    auto v = madt(2); Madt table{}; v[54] = 0; checksum(v);
    CHECK(parseMadt(bytes(v),table) == Error::DuplicateIdentity && table.count == 0);
    v = madt(2); v[55] = 0; checksum(v); CHECK(parseMadt(bytes(v),table) == Error::DuplicateIdentity);
    v = madt(1); v[48] = 4; checksum(v); CHECK(parseMadt(bytes(v),table) == Error::Reserved);
    v = madt(1,true); v[46] = 1; checksum(v); CHECK(parseMadt(bytes(v),table) == Error::Reserved);
    v = madt(1); v[44] = 16; checksum(v); CHECK(parseMadt(bytes(v),table) == Error::Unsupported);
}
static void processorIdentity() {
    auto v = processors(16); ProcessorNames names{};
    CHECK(parseProcessorSsdt(bytes(v),names) == Error::Ok && names.count == 16);
    CHECK(!strcmp(names.cpus[0].name,"C000") && names.cpus[0].acpiId == 15);
    CHECK(!strcmp(names.cpus[15].name,"C00F") && names.cpus[15].acpiId == 0);
    v[44+13+7] = v[44+7]; checksum(v); CHECK(parseProcessorSsdt(bytes(v),names) == Error::DuplicateIdentity);
    v = processors(16); v[46] = 12; checksum(v); CHECK(parseProcessorSsdt(bytes(v),names) == Error::Unsupported);
    v = processors(16); v[39] = '_'; checksum(v); CHECK(parseProcessorSsdt(bytes(v),names) == Error::Unsupported);
    v = processors(16); v[37] ^= 1; checksum(v); CHECK(parseProcessorSsdt(bytes(v),names) == Error::Length);
}
static void initialization() {
    const auto cpu = cpuMap(0,1); Fake f(&cpu,1); Engine e;
    const zencppc::Request r{29,166,0,128};
    CHECK(!e.enableAutonomous(&r,1) && e.lastError() == Error::NotInitialized);
    CHECK(f.events.empty()); initialize(e,f,&cpu,1);
    CHECK(e.completionTimeoutUs() == 2000000);
    CHECK(e.initialize(descriptor(),&cpu,1,f) == Error::AlreadyInitialized);
    CHECK(f.events.empty());
}
static void readOrdering() {
    const auto cpu = cpuMap(0,1); Fake f(&cpu,1); Engine e; initialize(e,f,&cpu,1);
    f.readHighest = 180; zencppc::Caps caps{}; CHECK(e.readCaps(0,&caps));
    CHECK(caps.highest == 180 && caps.nominal == 136 && caps.lowestNonlinear == 82 && caps.lowest == 29);
    CHECK(caps.nominalFreqMHz == 3401 && caps.lowestFreqMHz == 550);
    CHECK(e.commandCount() == 1 && e.lastLatencyUs() == 100);
    const size_t cmd = find(f,Op::Write16), status = find(f,Op::Write16,cmd+1);
    const size_t fence = find(f,Op::Fence,status+1), old = find(f,Op::Read64), ring = find(f,Op::Ring);
    CHECK(f.events[cmd].offset == 4 && f.events[cmd].value == 0);
    CHECK(f.events[status].offset == 6 && f.events[status].value == 0);
    CHECK(cmd < status && status < fence && fence < old && old < ring);
    const size_t done = find(f,Op::FirmwareComplete), resultFence = find(f,Op::Fence,done+1);
    const size_t sig = find(f,Op::Read32,resultFence+1), payload = find(f,Op::Read32,sig+1);
    CHECK(ring < done && done < resultFence && resultFence < sig && sig < payload);
    CHECK(f.events[sig].offset == 0 && f.events[payload].offset == cpu.fields[Highest].offset);
    CHECK(occurrences(f,Op::Write32) == 0 && get(f.memory.data(),4) == kSignature);
}
static void doorbellMasks() {
    const auto cpu = cpuMap(0,1); Fake f(&cpu,1); Engine e;
    auto d = descriptor(); d.preserveMask = UINT64_C(0xffffffff00000001); d.writeMask = 3;
    initialize(e,f,&cpu,1,d); zencppc::Caps caps{}; CHECK(e.readCaps(0,&caps));
    CHECK(f.bell == UINT64_C(0xaabbccdd00000003));
    CHECK(occurrences(f,Op::Read64) == 1 && occurrences(f,Op::Ring) == 1);
}
static void batchWrites() {
    CpuRegisters cpus[16]{}; zencppc::Request requests[16]{}; bool changed[16]{};
    for (uint32_t i = 0; i < 16; ++i) { cpus[i] = cpuMap(i,16); requests[i] = {29,166,0,128+i}; }
    Fake f(cpus,16); Engine e; initialize(e,f,cpus,16);
    cacheCaps(e,f);
    CHECK(e.enableAutonomous(requests,16)); CHECK(e.commandCount() == 17);
    CHECK(occurrences(f,Op::Write32) == 96 && occurrences(f,Op::Ring) == 1);
    const size_t command = find(f,Op::Write16), ring = find(f,Op::Ring);
    for (size_t i = 0; i < f.events.size(); ++i)
        if (f.events[i].op == Op::Write32) CHECK(i < command && i < ring);
    CHECK(f.events[command].value == 1);
    for (uint32_t i = 0; i < 16; ++i) {
        CHECK(f.value(i,Enable) == 1 && f.value(i,AutonomousSelection) == 1);
        CHECK(f.value(i,Desired) == 0 && f.value(i,Epp) == requests[i].epp);
        CHECK(f.value(i,Minimum) == 29 && f.value(i,Maximum) == 166);
        CHECK(f.value(i,Highest) == 166 && f.value(i,PerformanceLimited) == 0);
    }
    f.events.clear(); changed[3] = changed[12] = true;
    requests[3] = {40,150,90,32}; requests[12] = {50,160,0,240};
    CHECK(e.apply(requests,changed,16)); CHECK(e.commandCount() == 18);
    CHECK(occurrences(f,Op::Write32) == 8 && occurrences(f,Op::Ring) == 1);
    for (const auto &event : f.events) if (event.op == Op::Write32) {
        const bool three = event.offset >= 3*0x100+8 && event.offset < 4*0x100+8;
        const bool twelve = event.offset >= 12*0x100+8 && event.offset < 13*0x100+8;
        CHECK(three || twelve);
        CHECK(event.offset != cpus[3].fields[Enable].offset && event.offset != cpus[12].fields[Enable].offset);
    }
    CHECK(f.value(3,Desired) == 90 && f.value(12,Epp) == 240 && f.value(2,Epp) == requests[2].epp);
    f.events.clear(); changed[3] = changed[12] = false;
    CHECK(e.apply(requests,changed,16)); CHECK(f.events.empty() && e.commandCount() == 18);
}
static void stateRead() {
    const auto cpu = cpuMap(0,1); Fake f(&cpu,1); Engine e; initialize(e,f,&cpu,1);
    cacheCaps(e,f);
    const zencppc::Request r{40,160,0,255}; CHECK(e.enableAutonomous(&r,1));
    zencppc::Request state{}; uint32_t enable=0, autonomy=0;
    CHECK(e.readState(0,&enable,&autonomy,&state));
    CHECK(enable == 1 && autonomy == 1 && zencppc::sameRequest(r,state));
    CHECK(e.commandCount() == 3);
}
static void timeoutSticky() {
    const auto cpu = cpuMap(0,1); Fake f(&cpu,1); Engine e; initialize(e,f,&cpu,1); f.neverComplete = true;
    zencppc::Caps caps{9,8,7,6,5,4}; CHECK(!e.readCaps(0,&caps));
    CHECK(e.failed() && e.lastError() == Error::Timeout && e.commandCount() == 1);
    CHECK(f.now == 2000000 && e.lastLatencyUs() == 2000000 && caps.highest == 9);
    const size_t events = f.events.size(); const zencppc::Request r{29,166,0,128}; const bool changed = true;
    uint32_t a=0,b=0; zencppc::Request state{};
    CHECK(!e.readCaps(0,&caps)); CHECK(!e.readState(0,&a,&b,&state));
    CHECK(!e.enableAutonomous(&r,1)); CHECK(!e.apply(&r,&changed,1));
    CHECK(e.initialize(descriptor(),&cpu,1,f) == Error::Timeout);
    CHECK(e.lastError() == Error::Timeout && f.events.size() == events && f.rings.size() == 1);
}
static void firmwareError() {
    const auto cpu = cpuMap(0,1); Fake f(&cpu,1); Engine e; initialize(e,f,&cpu,1);
    f.completionStatus = kComplete|kError; zencppc::Caps caps{};
    CHECK(!e.readCaps(0,&caps) && e.failed() && e.lastError() == Error::FirmwareError);
    const size_t n = f.events.size(); CHECK(!e.readCaps(0,&caps) && f.events.size() == n);
}
static void errorWithoutCompletion() {
    const auto cpu = cpuMap(0,1); Fake f(&cpu,1); Engine e; initialize(e,f,&cpu,1);
    f.completionStatus = kError; zencppc::Caps caps{};
    CHECK(!e.readCaps(0,&caps) && e.lastError() == Error::Timeout && e.failed());
    CHECK(f.rings.size() == 1 && f.now == 2000000);
}
static void signatureFailure() {
    const auto cpu = cpuMap(0,1); Fake f(&cpu,1); Engine e; initialize(e,f,&cpu,1);
    f.corruptSignature = true; zencppc::Caps caps{9,8,7,6,5,4};
    CHECK(!e.readCaps(0,&caps) && e.lastError() == Error::Signature && e.failed());
    CHECK(caps.highest == 9 && f.rings.size() == 1);
    for (const Event &event : f.events) if (event.op == Op::Read32) CHECK(event.offset == 0);
}
static void initialOwnership() {
    const auto cpu = cpuMap(0,1);
    for (uint16_t initialStatus : {uint16_t(0),kComplete}) {
        Fake f(&cpu,1); Engine e; put(f.memory.data()+6,2,initialStatus);
        initialize(e,f,&cpu,1); zencppc::Caps caps{};
        CHECK(get(f.memory.data()+6,2) == initialStatus);
        f.delay = 4000;
        CHECK(e.readCaps(0,&caps) && !e.failed() && f.rings.size() == 1 && f.rings[0] == 0);
        const size_t ring = find(f,Op::Ring);
        for (size_t i = 0; i < ring; ++i) CHECK(f.events[i].op != Op::Read16);
        CHECK(occurrences(f,Op::Write16) == 2 && occurrences(f,Op::Write32) == 0);
        CHECK(f.events[find(f,Op::Write16)].offset == 4);
        CHECK(f.events[find(f,Op::Write16,find(f,Op::Write16)+1)].value == 0);
        const size_t done = find(f,Op::FirmwareComplete);
        const zencppc::Request r{29,166,0,128};
        CHECK(e.enableAutonomous(&r,1) && e.commandCount() == 2);
        CHECK(find(f,Op::Write32) > done && f.rings[1] >= f.events[done].time);
        CHECK(f.firmwareWrites == 1);
    }
}
static void rateSpacing() {
    const auto cpu = cpuMap(0,1); Fake f(&cpu,1); Engine e; initialize(e,f,&cpu,1);
    zencppc::Caps caps{}; CHECK(e.readCaps(0,&caps)); CHECK(e.readCaps(0,&caps)); CHECK(e.readCaps(0,&caps));
    CHECK(f.rings.size() == 3 && f.rings[0] == 0 && f.rings[1] == 1000 && f.rings[2] == 2000);
    CHECK(e.lastLatencyUs() == 100); // excludes rate wait
}
static void turnaroundSpacing() {
    const auto cpu = cpuMap(0,1); Fake f(&cpu,1); Engine e; auto d = descriptor(); d.minTurnaroundUs = 1500;
    initialize(e,f,&cpu,1,d); zencppc::Caps caps{};
    CHECK(e.readCaps(0,&caps)); CHECK(e.readCaps(0,&caps)); CHECK(e.readCaps(0,&caps));
    CHECK(f.rings[0] == 0 && f.rings[1] == 1600 && f.rings[2] == 3200);
}
static void lowRateBound() {
    const auto cpu = cpuMap(0,1); Fake f(&cpu,1); Engine e; auto d = descriptor(); d.maxCommandsPerMinute = 1;
    initialize(e,f,&cpu,1,d); zencppc::Caps caps{}; CHECK(e.readCaps(0,&caps));
    const size_t events = f.events.size(); const uint64_t now = f.now;
    CHECK(!e.readCaps(0,&caps) && e.lastError() == Error::RateLimited && !e.failed());
    CHECK(f.events.size() == events && f.now == now && f.rings.size() == 1);
    f.now = 60000000; CHECK(e.readCaps(0,&caps) && e.commandCount() == 2);
}
static void unlimitedRate() {
    const auto cpu = cpuMap(0,1); Fake f(&cpu,1); Engine e; auto d = descriptor(); d.maxCommandsPerMinute = 0;
    initialize(e,f,&cpu,1,d); zencppc::Caps caps{};
    CHECK(e.readCaps(0,&caps)); CHECK(e.readCaps(0,&caps)); CHECK(f.rings[1] == 100);
}
static void longTurnaroundBound() {
    const auto cpu = cpuMap(0,1); Fake f(&cpu,1); Engine e; auto d = descriptor(); d.minTurnaroundUs = 65535;
    initialize(e,f,&cpu,1,d); zencppc::Caps caps{};
    CHECK(e.readCaps(0,&caps)); // no completion/MRTT timestamp before first command
    CHECK(!e.failed() && f.rings[0] == 0);
    const uint64_t before = f.now; CHECK(!e.readCaps(0,&caps) && e.lastError() == Error::RateLimited);
    CHECK(!e.failed() && f.now == before && f.rings.size() == 1);
    f.now = 65635; CHECK(e.readCaps(0,&caps) && f.rings[1] == 65635);
}
static void invalidRequests() {
    const auto cpu = cpuMap(0,1); Fake f(&cpu,1); Engine e; initialize(e,f,&cpu,1);
    const zencppc::Request bad[] = {{170,160,0,128},{29,166,0,256},{29,166,200,128},{29,166,28,128}};
    const bool changed = true;
    for (const auto &r : bad) CHECK(!e.apply(&r,&changed,1) && e.lastError() == Error::BadRequest);
    zencppc::Request r{29,166,100,128}; CHECK(!e.enableAutonomous(&r,1));
    CHECK(f.events.empty() && !e.failed());
    r.desiredPerf = 0; CHECK(!e.apply(&r,nullptr,1)); CHECK(!e.apply(&r,&changed,0));
    CHECK(f.events.empty());
    zencppc::Caps caps{}; CHECK(e.readCaps(0,&caps)); f.events.clear();
    r.minPerf = 28; CHECK(!e.apply(&r,&changed,1) && f.events.empty());
    r.minPerf = 29; r.maxPerf = 167; CHECK(!e.apply(&r,&changed,1) && f.events.empty());
    const zencppc::Request bounds[] = {{0,UINT32_MAX,0,128},{0,166,0,128},{29,UINT32_MAX,0,128},
        {28,166,0,128},{29,167,0,128},{170,160,0,128},{29,166,0,256},{29,166,28,128},
        {29,166,167,128},{40,150,39,128},{40,150,151,128}};
    const auto memory = f.memory; const uint64_t commands = e.commandCount();
    for (const auto &request : bounds) {
        CHECK(!e.apply(&request,&changed,1) && e.lastError() == Error::BadRequest);
        CHECK(!e.enableAutonomous(&request,1) && e.lastError() == Error::BadRequest);
        CHECK(f.events.empty() && f.memory == memory && e.commandCount() == commands && !e.failed());
    }
    for (const zencppc::Request request : {zencppc::Request{29,166,29,0},
                                        zencppc::Request{29,166,166,255},
                                        zencppc::Request{29,29,0,128},
                                        zencppc::Request{166,166,166,128}})
        CHECK(e.apply(&request,&changed,1));
}
static void missingCaps() {
    CpuRegisters cpus[] = {cpuMap(0,2),cpuMap(1,2)};
    Fake f(cpus,2); Engine e; initialize(e,f,cpus,2);
    zencppc::Request r[] = {{0,UINT32_MAX,0,128},{29,166,0,128}};
    bool changed[] = {true,false}; const auto initialMemory = f.memory;
    CHECK(!e.apply(r,changed,2) && e.lastError() == Error::MissingCaps);
    CHECK(!e.enableAutonomous(r,2) && e.lastError() == Error::MissingCaps);
    CHECK(f.events.empty() && f.memory == initialMemory && !e.failed() && e.commandCount() == 0);
    r[0] = {29,166,0,128}; changed[0] = false;
    CHECK(e.apply(r,changed,2) && f.events.empty()); // no WRITE needs no caps
    changed[0] = true;
    CHECK(!e.apply(r,changed,2) && e.lastError() == Error::MissingCaps && f.events.empty());
    zencppc::Caps caps{}; CHECK(e.readCaps(0,&caps)); f.events.clear();
    const auto before = f.memory;
    CHECK(!e.enableAutonomous(r,2) && e.lastError() == Error::MissingCaps && f.events.empty());
    changed[1] = true;
    CHECK(!e.apply(r,changed,2) && e.lastError() == Error::MissingCaps);
    CHECK(f.events.empty() && f.memory == before && e.commandCount() == 1 && !e.failed());
    changed[1] = false;
    CHECK(e.apply(r,changed,2) && e.commandCount() == 2); // disjoint unchanged CPU needs no caps
    CHECK(occurrences(f,Op::Write32) == 4);
    f.seed(1,Highest,150); CHECK(e.readCaps(1,&caps)); f.events.clear();
    CHECK(!e.enableAutonomous(r,2) && e.lastError() == Error::BadRequest && f.events.empty());
    r[1].maxPerf = 150;
    CHECK(e.enableAutonomous(r,2) && e.commandCount() == 4);
}
static void aliasedMissingCaps() {
    CpuRegisters cpus[] = {cpuMap(0,2),cpuMap(1,2)};
    cpus[1].fields[Epp].offset = cpus[0].fields[Epp].offset;
    Fake f(cpus,2); Engine e; initialize(e,f,cpus,2);
    zencppc::Request r[] = {{29,166,0,128},{29,150,0,128}}; const bool changed[] = {true,false};
    zencppc::Caps caps{}; CHECK(e.readCaps(0,&caps)); f.events.clear();
    const auto memory = f.memory;
    CHECK(!e.apply(r,changed,2) && e.lastError() == Error::MissingCaps);
    CHECK(f.events.empty() && f.memory == memory && !e.failed());
    f.seed(1,Highest,150); CHECK(e.readCaps(1,&caps)); f.events.clear();
    r[1].maxPerf = 151;
    CHECK(!e.apply(r,changed,2) && e.lastError() == Error::BadRequest && f.events.empty());
    r[1].maxPerf = 150;
    CHECK(e.apply(r,changed,2) && e.commandCount() == 3);
    CHECK(occurrences(f,Op::Write32) == 4 && f.value(1,Epp) == 128);
}
static void aliasedBatch() {
    CpuRegisters cpus[] = {cpuMap(0,2),cpuMap(1,2)};
    cpus[1].fields[Epp].offset = cpus[0].fields[Epp].offset;
    CHECK(validateCpuMaps(cpus,2) == Error::Ok);
    Fake f(cpus,2); Engine e; initialize(e,f,cpus,2);
    cacheCaps(e,f);
    zencppc::Request r[] = {{29,166,0,128},{29,166,0,255}}; const bool changed[] = {true,false};
    CHECK(!e.apply(r,changed,2) && e.lastError() == Error::AliasConflict && f.events.empty());
    r[1].epp = 128; CHECK(e.enableAutonomous(r,2) && f.rings.size() == 1);
}
static void clockFailures() {
    const auto cpu = cpuMap(0,1);
    { Fake f(&cpu,1); Engine e; initialize(e,f,&cpu,1); zencppc::Caps caps{};
      f.now = 1000; CHECK(e.readCaps(0,&caps)); f.now = 100;
      const size_t n = f.events.size(); CHECK(!e.readCaps(0,&caps) && e.lastError() == Error::TimeBackwards);
      CHECK(e.failed() && f.events.size() == n); }
    { Fake f(&cpu,1); Engine e; initialize(e,f,&cpu,1); f.now = UINT64_MAX-100;
      zencppc::Caps caps{}; CHECK(!e.readCaps(0,&caps) && e.lastError() == Error::Overflow);
      CHECK(e.failed() && f.events.empty()); }
}
static void accessFailures() {
    const auto cpu = cpuMap(0,1); const zencppc::Request r{29,166,0,128};
    { Fake f(&cpu,1); Engine e; initialize(e,f,&cpu,1); cacheCaps(e,f);
      f.failWriteOffset = cpu.fields[Maximum].offset;
      CHECK(!e.enableAutonomous(&r,1) && e.lastError() == Error::Access && e.failed());
      CHECK(f.rings.empty()); const size_t n = f.events.size(); CHECK(!e.enableAutonomous(&r,1) && f.events.size() == n); }
    { Fake f(&cpu,1); Engine e; initialize(e,f,&cpu,1); f.failRead64 = true; zencppc::Caps caps{};
      CHECK(!e.readCaps(0,&caps) && e.lastError() == Error::Access && e.failed()); CHECK(f.rings.empty()); }
    { Fake f(&cpu,1); Engine e; initialize(e,f,&cpu,1); f.failRead16 = true; zencppc::Caps caps{};
      CHECK(!e.readCaps(0,&caps) && e.lastError() == Error::Access && f.rings.size() == 1); }
    { Fake f(&cpu,1); Engine e; initialize(e,f,&cpu,1); f.failWrite64 = true; zencppc::Caps caps{};
      CHECK(!e.readCaps(0,&caps) && e.lastError() == Error::Access && e.failed()); CHECK(e.commandCount() == 0); }
}
static void deadlineBoundary() {
    const auto cpu = cpuMap(0,1);
    { Fake f(&cpu,1); Engine e; initialize(e,f,&cpu,1); f.delay = 2000000; zencppc::Caps caps{};
      CHECK(!e.readCaps(0,&caps) && e.lastError() == Error::Timeout && f.now == 2000000); }
    { Fake f(&cpu,1); Engine e; initialize(e,f,&cpu,1); f.delay = 1999000; zencppc::Caps caps{};
      CHECK(e.readCaps(0,&caps) && e.lastLatencyUs() == 1999000); }
    { Fake f(&cpu,1); Engine e; auto d = descriptor(); d.nominalLatencyUs = UINT32_MAX;
      initialize(e,f,&cpu,1,d); f.neverComplete = true; zencppc::Caps caps{};
      CHECK(e.completionTimeoutUs() == 2000000); CHECK(!e.readCaps(0,&caps) && f.now == 2000000);
      bool slept = false; for (const auto &event : f.events) slept |= event.op == Op::Wait && event.value == 1000;
      CHECK(slept); }
    { Fake f(&cpu,1); Engine e; auto d = descriptor(); d.nominalLatencyUs = 37;
      initialize(e,f,&cpu,1,d); f.delay = 18500; zencppc::Caps caps{};
      CHECK(e.completionTimeoutUs() == 18500);
      CHECK(!e.readCaps(0,&caps) && e.lastError() == Error::Timeout && f.now == 18500); }
    { Fake f(&cpu,1); Engine e; auto d = descriptor(); d.nominalLatencyUs = 37;
      initialize(e,f,&cpu,1,d); f.delay = 18037; zencppc::Caps caps{};
      CHECK(e.readCaps(0,&caps) && e.lastLatencyUs() == 18037); }
    { Fake f(&cpu,1); Engine e; auto d = descriptor(); d.nominalLatencyUs = 0;
      initialize(e,f,&cpu,1,d); f.delay = 1000; zencppc::Caps caps{};
      CHECK(e.completionTimeoutUs() == 1000);
      CHECK(!e.readCaps(0,&caps) && e.lastError() == Error::Timeout && f.now == 1000); }
}
static void slowCompletion() {
    const auto cpu = cpuMap(0,1); Fake f(&cpu,1); Engine e; initialize(e,f,&cpu,1);
    f.delay = 1000000; zencppc::Caps caps{};
    CHECK(e.readCaps(0,&caps) && e.lastLatencyUs() == 1000000 && !e.failed());
    const size_t ring = find(f,Op::Ring); unsigned busy = 0, sleeping = 0;
    for (size_t i = ring + 1; i < f.events.size(); ++i) {
        const Event &event = f.events[i];
        if (event.op != Op::Wait) continue;
        if (event.time < 4000) { CHECK(event.value == 50); ++busy; }
        else { CHECK(event.value == 1000); ++sleeping; }
    }
    CHECK(busy == 80 && sleeping == 996);
    f.delay = 1500000; const zencppc::Request r{40,150,0,240};
    CHECK(e.enableAutonomous(&r,1) && e.lastLatencyUs() == 1500000 && e.commandCount() == 2);
    CHECK(!e.failed() && f.firmwareWrites == 1);
}
static void spacingCompletionBudget() {
    const auto cpu = cpuMap(0,1); Fake f(&cpu,1); Engine e;
    auto d = descriptor(); d.maxCommandsPerMinute = 1000; initialize(e,f,&cpu,1,d);
    zencppc::Caps caps{}; CHECK(e.readCaps(0,&caps)); f.events.clear();
    f.now = 10000;
    CHECK(!e.readCaps(0,&caps) && e.lastError() == Error::RateLimited && !e.failed());
    CHECK(f.now == 10000 && f.events.empty() && e.commandCount() == 1);
    f.now = 10001; f.delay = 1999000;
    CHECK(e.readCaps(0,&caps) && !e.failed());
    CHECK(f.rings[1] == 60000 && e.lastLatencyUs() == 1999000 && f.now == 2059000);
}
static void lateWriteTimeout() {
    const auto cpu = cpuMap(0,1); Fake f(&cpu,1); Engine e; initialize(e,f,&cpu,1); cacheCaps(e,f);
    const zencppc::Request r{40,150,0,240}; f.delay = 2500000;
    CHECK(!e.enableAutonomous(&r,1) && e.lastError() == Error::Timeout && e.failed());
    CHECK(e.commandCount() == 2 && e.lastLatencyUs() == 2000000 && f.pending);
    CHECK(f.value(0,Minimum) == 40 && f.value(0,Enable) == 1 && f.firmwareWrites == 0);
    f.now = f.ready; f.firmware(); // timed-out WRITE completes in firmware later
    CHECK(f.firmwareWrites == 1 && !f.pending);
    const size_t events = f.events.size(); const bool changed = true;
    CHECK(!e.apply(&r,&changed,1) && !e.enableAutonomous(&r,1));
    CHECK(e.lastError() == Error::Timeout && e.commandCount() == 2 && f.events.size() == events);
}
static void globalClaim() {
    SubspaceClaim claim; constexpr unsigned contenders = 8;
    Error results[contenders]{}; std::thread threads[contenders];
    for (unsigned i = 0; i < contenders; ++i) threads[i] = std::thread([&claim,&results,i]() {
        results[i] = claim.acquire();
    });
    for (auto &thread : threads) thread.join();
    unsigned successes = 0;
    for (Error result : results) {
        CHECK(result == Error::Ok || result == Error::SubspaceClaimed);
        successes += result == Error::Ok;
    }
    CHECK(successes == 1 && claim.acquire() == Error::SubspaceClaimed);
    claim.retire(); // model of the winning Channel finishing all mapping releases
    CHECK(claim.acquire() == Error::SubspaceClaimed);
    claim.retire(); CHECK(claim.acquire() == Error::SubspaceClaimed);
}
static void firmwareValues() {
    const auto cpu = cpuMap(0,1);
    { Fake f(&cpu,1); Engine e; initialize(e,f,&cpu,1); f.seed(0,Lowest,0); zencppc::Caps caps{};
      CHECK(!e.readCaps(0,&caps) && e.lastError() == Error::BadValue && e.failed()); }
    { Fake f(&cpu,1); Engine e; initialize(e,f,&cpu,1); f.seed(0,AutonomousSelection,2);
      zencppc::Request r{}; uint32_t a=0,b=0;
      CHECK(!e.readState(0,&a,&b,&r) && e.lastError() == Error::BadValue && e.failed()); }
}
static void mapBounds() {
    auto cpu = cpuMap(0,1); Fake f(&cpu,1); Engine e; auto d = descriptor(); d.length = 64;
    CHECK(e.initialize(d,&cpu,1,f) == Error::Bounds && f.events.empty());
    cpu.fields[Minimum].offset = 7; CHECK(validateCpuMaps(&cpu,1) == Error::Bounds);
    cpu.fields[Minimum].offset = 9; CHECK(validateCpuMaps(&cpu,1) == Error::Alignment);
    cpu = cpuMap(0,1); cpu.fields[Epp].kind = FieldKind::Integer;
    CHECK(validateCpuMaps(&cpu,1) == Error::Unsupported);
}
static void preemptionTiming() {
    const auto cpu = cpuMap(0,1);
    { Fake f(&cpu,1); Engine e; initialize(e,f,&cpu,1); f.completionFenceAdvanceUs = 2000000;
      zencppc::Caps caps{9,8,7,6,5,4};
      CHECK(!e.readCaps(0,&caps) && e.lastError() == Error::Timeout && e.failed());
      CHECK(caps.highest == 9 && f.rings.size() == 1 && e.lastLatencyUs() == 2000100); }
    { Fake f(&cpu,1); Engine e; initialize(e,f,&cpu,1); f.ringAdvanceUs = 2000000;
      zencppc::Caps caps{};
      CHECK(!e.readCaps(0,&caps) && e.lastError() == Error::Timeout && e.failed());
      CHECK(e.lastLatencyUs() == 2000000 && e.commandCount() == 1); }
    { Fake f(&cpu,1); Engine e; initialize(e,f,&cpu,1); f.ringAdvanceUs = 1500;
      zencppc::Caps caps{}; CHECK(e.readCaps(0,&caps)); CHECK(e.readCaps(0,&caps));
      CHECK(f.rings.size() == 2 && f.rings[0] == 0 && f.rings[1] == 2500);
      CHECK(e.lastLatencyUs() == 1500); }
    { Fake f(&cpu,1); Engine e; auto d = descriptor(); d.nominalLatencyUs = 0;
      initialize(e,f,&cpu,1,d); CHECK(e.completionTimeoutUs() == 1000); }
}

int main() {
    struct Test { const char *name; void (*run)(); };
    const Test tests[] = {
        {"PCCT facts",pcctFacts}, {"PCCT validation",pcctValidation}, {"GAS framing",gasValidation},
        {"16 CPU CPC facts",cpcFacts}, {"CPC validation",cpcValidation}, {"revision 2",revisionTwo},
        {"alias validation",aliasValidation}, {"MADT join",identityJoin}, {"MADT validation",madtValidation},
        {"Processor declaration identity",processorIdentity}, {"command-free initialization",initialization},
        {"READ ordering",readOrdering}, {"64-bit doorbell masks",doorbellMasks}, {"WRITE batching",batchWrites},
        {"READ state",stateRead}, {"timeout sticky",timeoutSticky}, {"firmware error",firmwareError},
        {"error without complete",errorWithoutCompletion}, {"signature failure",signatureFailure},
        {"initial ownership",initialOwnership}, {"MPAR spacing",rateSpacing}, {"MRTT spacing",turnaroundSpacing},
        {"low MPAR bounded",lowRateBound}, {"MPAR unlimited",unlimitedRate}, {"long MRTT bounded",longTurnaroundBound},
        {"request preflight",invalidRequests}, {"missing caps",missingCaps}, {"aliased missing caps",aliasedMissingCaps},
        {"aliased batch",aliasedBatch}, {"monotonic clock",clockFailures},
        {"access failures",accessFailures}, {"deadline boundary",deadlineBoundary}, {"firmware values",firmwareValues},
        {"map bounds",mapBounds}, {"preemption timing",preemptionTiming}, {"slow completion",slowCompletion},
        {"spacing and completion budgets",spacingCompletionBudget}, {"late WRITE timeout",lateWriteTimeout},
        {"global subspace claim",globalClaim}
    };
    for (const auto &test : tests) { test.run(); ++cases; printf("PASS %s\n",test.name); }
    printf("PCC: %u cases, %u assertions passed (ASan/UBSan)\n",cases,assertions);
    return 0;
}
