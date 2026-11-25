#include "ZenPccModel.hpp"

namespace zenpcc {
namespace {
uint64_t le(const uint8_t *p, size_t bytes) {
    uint64_t value = 0;
    for (size_t i = 0; i < bytes; ++i) value |= uint64_t(p[i]) << (8 * i);
    return value;
}
bool zero(const uint8_t *p, size_t bytes) {
    for (size_t i = 0; i < bytes; ++i) if (p[i]) return false;
    return true;
}
bool add(uint64_t a, uint64_t b, uint64_t &out) {
    if (a > UINT64_MAX - b) return false;
    out = a + b;
    return true;
}
bool overlap(uint64_t a, uint64_t an, uint64_t b, uint64_t bn) {
    return a <= b ? b - a < an : a - b < bn;
}
Gas gasAt(const uint8_t *p) { return {p[0], p[1], p[2], p[3], le(p + 4, 8)}; }
Error tableHeader(Bytes table, const char *signature, size_t minimum) {
    if (!table.data) return Error::BadArgument;
    if (table.size < minimum) return Error::Truncated;
    if (table.size > 65536) return Error::Bounds;
    for (size_t i = 0; i < 4; ++i)
        if (table.data[i] != uint8_t(signature[i])) return Error::Signature;
    if (le(table.data + 4, 4) != table.size) return Error::Length;
    uint8_t sum = 0;
    for (size_t i = 0; i < table.size; ++i) sum = uint8_t(sum + table.data[i]);
    return sum ? Error::Checksum : Error::Ok;
}
bool capability(uint32_t field) {
    return field <= Lowest || field == LowestFrequency || field == NominalFrequency;
}
bool control(uint32_t field) {
    return field == Desired || field == Minimum || field == Maximum ||
           field == Enable || field == AutonomousSelection || field == Epp;
}
bool used(uint32_t field) { return capability(field) || control(field) || field == PerformanceLimited; }
bool validRequest(const zencppc::Request &r) {
    return r.minPerf <= r.maxPerf && r.epp <= 255 &&
           (r.desiredPerf == 0 || (r.desiredPerf >= r.minPerf && r.desiredPerf <= r.maxPerf));
}
uint32_t requestValue(const zencppc::Request &r, uint32_t field) {
    switch (field) {
    case Desired: return r.desiredPerf;
    case Minimum: return r.minPerf;
    case Maximum: return r.maxPerf;
    case Epp: return r.epp;
    default: return 1; // enable/autonomy when used by enableAutonomous
    }
}
uint64_t minimum(uint64_t a, uint64_t b) { return a < b ? a : b; }
} // namespace

Error SubspaceClaim::acquire() {
    uint32_t expected = 0;
    return __atomic_compare_exchange_n(&state_, &expected, 1u, false,
                                       __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)
        ? Error::Ok : Error::SubspaceClaimed;
}
void SubspaceClaim::retire() {
    uint32_t expected = 1;
    __atomic_compare_exchange_n(&state_, &expected, 2u, false,
                                __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE);
}

Error validateDescriptor(const Descriptor &d) {
    if (!d.baseAddress || d.length < kHeaderBytes + 4 || d.length > kMaxSharedBytes)
        return Error::Bounds;
    if (d.baseAddress % 8 || d.length % 4) return Error::Alignment;
    uint64_t end;
    if (!add(d.baseAddress, d.length - 1, end)) return Error::Overflow;
    const Gas &g = d.doorbell;
    // This transport deliberately never splits a 64-bit doorbell into dwords.
    if (g.space != 0) return Error::Unsupported;
    if (g.width != 64 || g.bitOffset || (g.access != 0 && g.access != 4)) return Error::Gas;
    if (!g.address) return Error::Bounds;
    if (g.address % 8) return Error::Alignment;
    if (!add(g.address, 7, end)) return Error::Overflow;
    if (!d.writeMask) return Error::Gas;
    if (overlap(d.baseAddress, d.length, g.address, 8)) return Error::Aliasing;
    return Error::Ok;
}

Error parsePcct(Bytes table, Descriptor &out) {
    out = {};
    Error error = tableHeader(table, "PCCT", 48);
    if (error != Error::Ok) return error;
    if (table.data[8] != 2) return Error::Revision;
    if (le(table.data + 36, 4)) return Error::Unsupported; // no interrupt protocol
    if (!zero(table.data + 40, 8)) return Error::Reserved;
    if (table.size < 50) return Error::Truncated;
    const uint8_t *p = table.data + 48;
    if (p[0] != 0) return Error::Unsupported;
    if (p[1] != 62) return Error::Length;
    if (table.size < 110) return Error::Truncated;
    if (table.size != 110) return Error::Subspace; // only subspace 0
    if (!zero(p + 2, 6)) return Error::Reserved;
    Descriptor d{le(p + 8, 8), le(p + 16, 8), gasAt(p + 24), le(p + 36, 8),
                 le(p + 44, 8), uint32_t(le(p + 52, 4)), uint32_t(le(p + 56, 4)),
                 uint16_t(le(p + 60, 2))};
    error = validateDescriptor(d);
    if (error == Error::Ok) out = d;
    return error;
}

Error parseGas(Bytes resource, Gas &out) {
    out = {};
    if (!resource.data || resource.size != 17) return Error::Length;
    const uint8_t *p = resource.data;
    if (p[0] != 0x82 || le(p + 1, 2) != 12 || p[15] != 0x79) return Error::Gas;
    if (p[16]) {
        uint8_t sum = 0;
        for (size_t i = 0; i < resource.size; ++i) sum = uint8_t(sum + p[i]);
        if (sum) return Error::Checksum;
    }
    out = gasAt(p + 3);
    return Error::Ok;
}

Error parseCpc(const CpcEntry *entries, size_t count, const Descriptor &d, CpuRegisters &out) {
    out = {};
    Error error = validateDescriptor(d);
    if (error != Error::Ok) return error;
    if (!entries) return Error::BadArgument;
    if (count != 21 && count != 23) return Error::Length;
    if (entries[0].kind != EntryKind::Integer || entries[1].kind != EntryKind::Integer)
        return Error::Unsupported;
    if (entries[0].integer != count) return Error::Length;
    if (entries[1].integer != (count == 23 ? 3 : 2)) return Error::Revision;
    for (uint32_t f = 0; f < count - 2; ++f) {
        const CpcEntry &e = entries[f + 2];
        Field &field = out.fields[f];
        if (e.kind == EntryKind::Integer) {
            if (capability(f)) {
                if (e.integer > UINT32_MAX) return Error::Overflow;
                if (f <= Lowest && !e.integer) return Error::BadValue;
                field = {FieldKind::Integer, uint32_t(e.integer), 0};
            } else if (used(f)) return Error::Unsupported;
            else if (e.integer != 0) return Error::Unsupported;
            continue;
        }
        if (e.kind != EntryKind::Buffer) return Error::Unsupported;
        Gas g{};
        error = parseGas(e.buffer, g);
        if (error != Error::Ok) return error;
        if (g.space == 0 && !g.width && !g.bitOffset && !g.access && !g.address) {
            if (used(f) && f != LowestFrequency && f != NominalFrequency)
                return Error::Unsupported;
            continue;
        }
        if (g.space == 0x7f && (f == ReferenceCounter || f == DeliveredCounter)) {
            if (g.width != 64 || g.bitOffset || g.access != 4 ||
                g.address != (f == ReferenceCounter ? 0xe7u : 0xe8u)) return Error::Gas;
            field.kind = FieldKind::FfhCounter; // metadata; no MSR access
            continue;
        }
        if (g.space != 0x0a) return Error::Unsupported;
        if (g.access != 0) return Error::Subspace;
        if (g.width != 32 || g.bitOffset) return Error::Gas;
        uint64_t offset, end;
        if (!add(g.address, kHeaderBytes, offset) || !add(offset, 4, end)) return Error::Overflow;
        if (end > d.length) return Error::Bounds;
        if (offset % 4) return Error::Alignment;
        field = {FieldKind::Pcc, 0, uint32_t(offset)};
    }
    // Local overlap of distinct roles is always ambiguous, including controls
    // over capability registers. Cross-CPU exact same-role aliases are checked
    // by validateCpuMaps and the batch preflight below.
    for (uint32_t f = 0; f < FieldCount; ++f)
        for (uint32_t g = 0; g < f; ++g)
            if (out.fields[f].kind == FieldKind::Pcc && out.fields[g].kind == FieldKind::Pcc &&
                out.fields[f].offset == out.fields[g].offset) return Error::Aliasing;
    return Error::Ok;
}

Error validateCpuMaps(const CpuRegisters *cpus, uint32_t count) {
    if (!cpus || !count || count > kMaxCpus) return Error::BadArgument;
    for (uint32_t a = 0; a < count; ++a) {
        const CpuRegisters &cpu = cpus[a];
        if (!cpu.domainMembers || cpu.domainMembers > count) return Error::Domain;
        uint32_t members = 0;
        for (uint32_t b = 0; b < count; ++b) {
            if (cpu.domain == cpus[b].domain) {
                if (cpu.domainMembers != cpus[b].domainMembers) return Error::Domain;
                ++members;
            }
            if (b >= a) continue;
            if (cpu.apicId == cpus[b].apicId ||
                (cpu.x2Apic == cpus[b].x2Apic && cpu.acpiId == cpus[b].acpiId))
                return Error::DuplicateIdentity;
        }
        if (members != cpu.domainMembers) return Error::Domain;
        for (uint32_t f = 0; f < FieldCount; ++f) {
            const Field &field = cpu.fields[f];
            if (f <= Lowest && field.kind != FieldKind::Pcc && field.kind != FieldKind::Integer)
                return Error::Unsupported;
            if ((control(f) || f == PerformanceLimited) && field.kind != FieldKind::Pcc)
                return Error::Unsupported;
            if (field.kind != FieldKind::Pcc) continue;
            if (field.offset < kHeaderBytes || field.offset > kMaxSharedBytes - 4)
                return Error::Bounds;
            if (field.offset % 4) return Error::Alignment;
            for (uint32_t b = 0; b <= a; ++b) {
                const uint32_t end = b == a ? f : uint32_t(FieldCount);
                for (uint32_t g = 0; g < end; ++g)
                    if (cpus[b].fields[g].kind == FieldKind::Pcc &&
                        field.offset == cpus[b].fields[g].offset && f != g) return Error::Aliasing;
            }
        }
    }
    return Error::Ok;
}

Error parseMadt(Bytes table, Madt &out) {
    out.count = 0;
    Error error = tableHeader(table, "APIC", 44);
    if (error != Error::Ok) return error;
    if (table.data[8] < 3 || table.data[8] > 5) return Error::Revision;
    if (le(table.data + 40, 4) & ~uint64_t(1)) return Error::Reserved;
    uint32_t count = 0;
    for (size_t at = 44; at < table.size;) {
        if (table.size - at < 2) return Error::Truncated;
        const uint8_t *p = table.data + at;
        const uint8_t type = p[0], length = p[1];
        if (length < 2) return Error::Length;
        if (length > table.size - at) return Error::Truncated;
        size_t expected;
        switch (type) {
        case 0: case 4: expected = type == 0 ? 8 : 6; break;
        case 1: case 3: case 10: expected = 12; break;
        case 2: expected = 10; break;
        case 5: expected = 12; break;
        case 9: expected = 16; break;
        default: return Error::Unsupported;
        }
        if (length != expected) return Error::Length;
        if (type == 0 || type == 9) {
            if (count == kMaxMadtCpus) return Error::Bounds;
            if (type == 9 && !zero(p + 2, 2)) return Error::Reserved;
            const uint32_t flags = uint32_t(le(p + (type == 0 ? 4 : 8), 4));
            if (flags & ~3u) return Error::Reserved;
            MadtCpu cpu{type == 0 ? p[2] : uint32_t(le(p + 12, 4)),
                        type == 0 ? p[3] : uint32_t(le(p + 4, 4)), type == 9, bool(flags & 1)};
            for (uint32_t i = 0; i < count; ++i)
                if ((cpu.x2Apic == out.cpus[i].x2Apic && cpu.acpiId == out.cpus[i].acpiId) ||
                    (cpu.enabled && out.cpus[i].enabled && cpu.apicId == out.cpus[i].apicId))
                    return Error::DuplicateIdentity;
            out.cpus[count++] = cpu;
        }
        at += length;
    }
    if (!count) return Error::MissingIdentity;
    out.count = count;
    return Error::Ok;
}

Error joinCpu(const Madt &madt, uint32_t acpiId, bool x2Apic,
              const uint16_t *apicIds, uint32_t count, uint32_t &xnuCpu) {
    xnuCpu = UINT32_MAX;
    if (!apicIds || !count || count > kMaxCpus || madt.count > kMaxMadtCpus)
        return Error::BadArgument;
    for (uint32_t a = 0; a < count; ++a)
        for (uint32_t b = 0; b < a; ++b)
            if (apicIds[a] == apicIds[b]) return Error::DuplicateIdentity;
    const MadtCpu *match = nullptr;
    for (uint32_t i = 0; i < madt.count; ++i) {
        const MadtCpu &cpu = madt.cpus[i];
        if (cpu.acpiId != acpiId || cpu.x2Apic != x2Apic) continue;
        if (match) return Error::DuplicateIdentity;
        match = &cpu;
    }
    if (!match) return Error::MissingIdentity;
    if (!match->enabled) return Error::DisabledIdentity;
    for (uint32_t i = 0; i < count; ++i)
        if (apicIds[i] == match->apicId) { xnuCpu = i; return Error::Ok; }
    return Error::MissingIdentity;
}

Error parseProcessorSsdt(Bytes table, ProcessorNames &out) {
    out.count = 0;
    Error error = tableHeader(table, "SSDT", 44);
    if (error != Error::Ok) return error;
    if (table.data[8] != 2) return Error::Revision;
    size_t at = 36;
    if (table.data[at++] != 0x10) return Error::Unsupported; // ScopeOp
    const uint8_t lead = table.data[at];
    const size_t follow = lead >> 6;
    if (follow > 1 || table.size - at < follow + 1) return Error::Unsupported;
    uint32_t length = follow ? (lead & 15) | (uint32_t(table.data[at + 1]) << 4) : lead & 63;
    if (follow && (lead & 0x30)) return Error::Reserved;
    if (length != table.size - at) return Error::Length;
    at += follow + 1;
    if (table.size - at < 5 || table.data[at] != '\\' || table.data[at + 1] != '_' ||
        table.data[at + 2] != 'P' || table.data[at + 3] != 'R' || table.data[at + 4] != '_')
        return Error::Unsupported;
    at += 5;
    uint32_t count = 0;
    while (at < table.size) {
        if (table.size - at < 13) return Error::Truncated;
        const uint8_t *p = table.data + at;
        if (p[0] != 0x5b || p[1] != 0x83 || p[2] != 0x0b) return Error::Unsupported;
        if (count == kMaxCpus) return Error::Bounds;
        ProcessorIdentity &cpu = out.cpus[count];
        for (size_t i = 0; i < 4; ++i) {
            const uint8_t c = p[i + 3];
            if (!(c == '_' || (c >= 'A' && c <= 'Z') || (i && c >= '0' && c <= '9')))
                return Error::Unsupported;
            cpu.name[i] = char(c);
        }
        cpu.name[4] = 0;
        cpu.acpiId = p[7];
        for (uint32_t i = 0; i < count; ++i) {
            bool same = true;
            for (uint32_t c = 0; c < 4; ++c) same = same && cpu.name[c] == out.cpus[i].name[c];
            if (same || cpu.acpiId == out.cpus[i].acpiId) return Error::DuplicateIdentity;
        }
        ++count;
        at += 13; // PblkAddr/PblkLen are metadata and are never used as I/O.
    }
    if (!count) return Error::MissingIdentity;
    out.count = count;
    return Error::Ok;
}

Error Engine::initialize(const Descriptor &d, const CpuRegisters *cpus, uint32_t count,
                         Accessor &accessor) {
    if (initialized_ || failed_) { reject(Error::AlreadyInitialized); return lastError_; }
    Error error = validateDescriptor(d);
    if (error == Error::Ok) error = validateCpuMaps(cpus, count);
    if (error == Error::Ok)
        for (uint32_t cpu = 0; cpu < count; ++cpu)
            for (uint32_t f = 0; f < FieldCount; ++f)
                if (cpus[cpu].fields[f].kind == FieldKind::Pcc &&
                    uint64_t(cpus[cpu].fields[f].offset) + 4 > d.length) error = Error::Bounds;
    if (error != Error::Ok) { reject(error); return error; }
    descriptor_ = d;
    cpus_ = cpus;
    count_ = count;
    accessor_ = &accessor;
    initialized_ = true;
    lastError_ = Error::Ok;
    return Error::Ok;
}
bool Engine::reject(Error error) {
    if (!failed_) lastError_ = error;
    return false;
}
bool Engine::fault(Error error) { lastError_ = error; failed_ = true; return false; }
bool Engine::clock(uint64_t &now) {
    now = accessor_->nowUs();
    if (haveNow_ && now < lastNow_) return fault(Error::TimeBackwards);
    lastNow_ = now;
    haveNow_ = true;
    return true;
}
uint32_t Engine::completionTimeoutUs() const {
    // PCCT latency is nominal, not a maximum. Match Linux's NUM_RETRIES (500),
    // capped at two seconds. Zero/tiny nominal values retain a 1-ms floor.
    uint64_t us = uint64_t(descriptor_.nominalLatencyUs) * 500;
    if (us < 1000) us = 1000;
    return uint32_t(minimum(us, kMaxCompletionTimeoutUs));
}
bool Engine::complete(uint64_t deadline, bool command) {
    uint64_t now;
    if (!clock(now)) return false;
    if (command) lastLatencyUs_ = now - commandStart_;
    if (now >= deadline) return fault(Error::Timeout);
    // Busy poll for about one nominal interval, bounded to 10 ms even for
    // malformed/very slow nominal values, then yield with 1-ms sleeps.
    const uint64_t nominal = descriptor_.nominalLatencyUs ? descriptor_.nominalLatencyUs : 1000;
    const uint64_t busyUntil = now + minimum(minimum(nominal, 10000), deadline - now);
    for (;;) {
        if (!clock(now)) return false;
        if (command) lastLatencyUs_ = now - commandStart_;
        // Late completion never releases a quarantined channel to callers.
        if (now >= deadline) return fault(Error::Timeout);
        uint16_t status;
        if (!accessor_->read16(6, status)) return fault(Error::Access);
        if (status & kComplete) {
            accessor_->barrier(); // Linux check_pcc_chan rmb(): payload follows status
            // A preemption during the MMIO read/fence must not publish a late
            // result or measure MRTT from a time before completion was observed.
            if (!clock(now)) return false;
            if (command) lastLatencyUs_ = now - commandStart_;
            if (now >= deadline) return fault(Error::Timeout);
            if (status & kError) return fault(Error::FirmwareError);
            platformOwnsPcc_ = false;
            // Only an actual command completion establishes the MRTT base.
            haveCompletion_ = true;
            lastCompletion_ = now;
            return true;
        }
        // Error without Complete does not grant ownership (Linux semantics).
        const uint64_t wait = now < busyUntil ? minimum(50, busyUntil - now) : 1000;
        accessor_->waitUs(uint32_t(minimum(wait, deadline - now)));
    }
}
bool Engine::prepare() {
    if (failed_) return false;
    if (!initialized_) return reject(Error::NotInitialized);
    uint64_t now;
    if (!clock(now)) return false;
    // Linux initially treats PCC as OSPM-owned, regardless of the idle status
    // bit. Only a command we sent can transfer ownership to the platform.
    if (platformOwnsPcc_) {
        uint64_t deadline;
        if (!add(now, completionTimeoutUs(), deadline)) return fault(Error::Overflow);
        if (!complete(deadline, false)) return false;
    }
    if (!clock(now)) return false;
    uint64_t spacingDeadline;
    if (!add(now, kRateWaitBudgetUs, spacingDeadline)) return fault(Error::Overflow);
    if (!haveSent_) {
        uint32_t signature;
        if (!accessor_->read32(0, signature)) return fault(Error::Access);
        if (signature != kSignature) return fault(Error::Signature);
    }
    uint64_t due = 0, next;
    if (haveSent_ && descriptor_.maxCommandsPerMinute) {
        const uint64_t rate = descriptor_.maxCommandsPerMinute;
        // Uniform ceil(60s/MPAR) spacing obeys any rolling 60s window without
        // Linux's burst counter or allocating a command timestamp history.
        if (!add(lastSent_, (60000000 + rate - 1) / rate, next)) return fault(Error::Overflow);
        due = next;
    }
    if (haveCompletion_) {
        if (!add(lastCompletion_, descriptor_.minTurnaroundUs, next)) return fault(Error::Overflow);
        if (next > due) due = next;
    }
    if (!clock(now)) return false;
    if (due >= spacingDeadline) return reject(Error::RateLimited);
    while (now < due) {
        accessor_->waitUs(uint32_t(minimum(1000, due - now)));
        if (!clock(now)) return false;
    }
    if (now >= spacingDeadline) return reject(Error::RateLimited);
    lastError_ = Error::Ok;
    return true;
}
bool Engine::issue(uint16_t command) {
    uint64_t deadline;
    // Linux send_pcc_cmd: staged CPC payload, command, status=0, doorbell.
    // In particular status is cleared, rather than preserving an old Error.
    if (!accessor_->write16(4, command) || !accessor_->write16(6, 0)) return fault(Error::Access);
    accessor_->barrier();
    uint64_t old;
    if (!accessor_->read64(old)) return fault(Error::Access);
    if (!clock(commandStart_)) return false;
    if (!add(commandStart_, completionTimeoutUs(), deadline)) return fault(Error::Overflow);
    platformOwnsPcc_ = true;
    // mailbox/pcc.c: set bits win even if PreserveMask and WriteMask overlap.
    if (!accessor_->write64((old & descriptor_.preserveMask) | descriptor_.writeMask))
        return fault(Error::Access);
    haveSent_ = true;
    ++commandCount_;
    // Use the timestamp AFTER the MMIO store for MPAR, so preemption between
    // timestamping and ringing can only lengthen the next command's spacing.
    if (!clock(lastSent_)) return false;
    // A timed-out WRITE may already have changed firmware state. Quarantine
    // permanently; do not retry, restore payload, or infer that it was ignored.
    if (!complete(deadline, true)) return false;
    uint32_t signature;
    if (!accessor_->read32(0, signature)) return fault(Error::Access);
    // The platform supplies the signature. Never manufacture it in OSPM memory.
    // READ results cannot become caps/state until this ACPI signature check.
    if (signature != kSignature) return fault(Error::Signature);
    lastError_ = Error::Ok;
    return true;
}
bool Engine::sample(uint32_t cpu, FieldIndex f, uint32_t &out) {
    const Field &field = cpus_[cpu].fields[f];
    if (field.kind == FieldKind::Integer) { out = field.value; return true; }
    if (field.kind == FieldKind::Absent) { out = 0; return true; }
    if (field.kind != FieldKind::Pcc) return reject(Error::Unsupported);
    if (!accessor_->read32(field.offset, out)) return fault(Error::Access);
    return true;
}
bool Engine::readCaps(uint32_t cpu, zencppc::Caps *out) {
    if (failed_) return false;
    if (!initialized_) return reject(Error::NotInitialized);
    if (!out || cpu >= count_) return reject(Error::BadArgument);
    if (!prepare() || !issue(0)) return false;
    zencppc::Caps caps{};
    if (!sample(cpu, Highest, caps.highest) || !sample(cpu, Nominal, caps.nominal) ||
        !sample(cpu, LowestNonlinear, caps.lowestNonlinear) || !sample(cpu, Lowest, caps.lowest) ||
        !sample(cpu, NominalFrequency, caps.nominalFreqMHz) ||
        !sample(cpu, LowestFrequency, caps.lowestFreqMHz)) return false;
    if (!caps.lowest || caps.lowest > caps.lowestNonlinear || caps.lowestNonlinear > caps.nominal ||
        caps.nominal > caps.highest) return fault(Error::BadValue);
    caps_[cpu] = caps;
    haveCaps_[cpu] = true;
    *out = caps;
    return true;
}
bool Engine::readState(uint32_t cpu, uint32_t *enable, uint32_t *autonomy, zencppc::Request *out) {
    if (failed_) return false;
    if (!initialized_) return reject(Error::NotInitialized);
    if (!out || !enable || !autonomy || cpu >= count_) return reject(Error::BadArgument);
    if (!prepare() || !issue(0)) return false;
    zencppc::Request r{};
    uint32_t e, a;
    if (!sample(cpu, Enable, e) || !sample(cpu, AutonomousSelection, a) ||
        !sample(cpu, Minimum, r.minPerf) || !sample(cpu, Maximum, r.maxPerf) ||
        !sample(cpu, Desired, r.desiredPerf) || !sample(cpu, Epp, r.epp)) return false;
    if (e > 1 || a > 1 || !validRequest(r)) return fault(Error::BadValue);
    *enable = e;
    *autonomy = a;
    *out = r;
    return true;
}
bool Engine::validateRequests(const zencppc::Request *requests, const bool *changed,
                              uint32_t count, bool autonomous) {
    if (failed_) return false;
    if (!initialized_) return reject(Error::NotInitialized);
    if (!requests || count != count_) return reject(Error::BadArgument);
    for (uint32_t cpu = 0; cpu < count; ++cpu) {
        const zencppc::Request &r = requests[cpu];
        if (!validRequest(r) || (autonomous && r.desiredPerf != 0)) return reject(Error::BadRequest);
        if (!changed || changed[cpu]) {
            if (!haveCaps_[cpu]) return reject(Error::MissingCaps);
            if (r.minPerf < caps_[cpu].lowest || r.maxPerf > caps_[cpu].highest)
                return reject(Error::BadRequest);
            // A register shared with an unchanged CPU writes that CPU too.
            for (uint32_t peer = 0; peer < count; ++peer)
                for (uint32_t f = 0; f < FieldCount; ++f)
                    if ((f == Desired || f == Minimum || f == Maximum || f == Epp) &&
                        cpus_[cpu].fields[f].offset == cpus_[peer].fields[f].offset) {
                        if (!haveCaps_[peer]) return reject(Error::MissingCaps);
                        if (requests[peer].minPerf < caps_[peer].lowest ||
                            requests[peer].maxPerf > caps_[peer].highest)
                            return reject(Error::BadRequest);
                    }
        }
        // Even unchanged peers must agree with requests targeting the same
        // physical register. Validate before staging any byte of the batch.
        for (uint32_t peer = 0; peer < cpu; ++peer)
            for (uint32_t f = 0; f < FieldCount; ++f)
                if (control(f) && cpus_[cpu].fields[f].offset == cpus_[peer].fields[f].offset &&
                    requestValue(r, f) != requestValue(requests[peer], f)) return reject(Error::AliasConflict);
    }
    return true;
}
bool Engine::writeRequests(const zencppc::Request *requests, const bool *changed,
                           uint32_t count, bool enable) {
    if (!prepare()) return false;
    for (uint32_t cpu = 0; cpu < count; ++cpu) {
        if (changed && !changed[cpu]) continue;
        // cppc_set_perf stages desired/min/max; cppc_set_epp_perf stages EPP
        // and AUTO_SEL. This synchronous owner combines the same-subspace
        // payloads and cppc_set_enable into one completed CMD_WRITE batch.
        const FieldIndex fields[] = {Desired, Minimum, Maximum, Epp, AutonomousSelection, Enable};
        for (uint32_t i = 0; i < (enable ? 6u : 4u); ++i) {
            const FieldIndex f = fields[i];
            if (!accessor_->write32(cpus_[cpu].fields[f].offset, requestValue(requests[cpu], f)))
                return fault(Error::Access);
        }
    }
    return issue(1);
}
bool Engine::enableAutonomous(const zencppc::Request *requests, uint32_t count) {
    if (!validateRequests(requests, nullptr, count, true)) return false;
    return writeRequests(requests, nullptr, count, true);
}
bool Engine::apply(const zencppc::Request *requests, const bool *changed, uint32_t count) {
    if (failed_) return false;
    if (!changed) return reject(Error::BadArgument);
    if (!validateRequests(requests, changed, count, false)) return false;
    bool any = false;
    for (uint32_t cpu = 0; cpu < count; ++cpu) any = any || changed[cpu];
    if (!any) { lastError_ = Error::Ok; return true; }
    return writeRequests(requests, changed, count, false);
}
} // namespace zenpcc
