// Offline discovery/protocol groundwork. No hardware transport exists here.
// Pure bounded integer logic: no heap, floating point, OS, or I/O dependencies.
#ifndef CPPC_LAB_HPP
#define CPPC_LAB_HPP

#include <stddef.h>
#include <stdint.h>

namespace cppc_lab {

constexpr size_t kMaxCpus = 64;
constexpr size_t kMaxMadtCpus = 128; // Includes disabled placeholders.
constexpr size_t kMaxSubspaces = 8; // Implementation bound, NOT an ACPI limit.
constexpr size_t kMaxTableBytes = 65536;
constexpr size_t kMaxTextBytes = 131072;
constexpr size_t kMaxSharedBytes = 65536;
constexpr size_t kCpcEntries = 23;
constexpr size_t kFieldCount = kCpcEntries - 2;
constexpr size_t kPathBytes = 64;
constexpr uint64_t kMinuteUs = 60000000;
constexpr uint64_t kMaxDeadlineUs = kMinuteUs; // Caller policy bound, not latency.

struct Bytes { const uint8_t *data; size_t size; };

enum class Code : uint8_t {
    Ok, Syntax, Unsupported, Capacity, Truncated, Length, Signature, Checksum,
    Revision, Reserved, Gas, Overflow, Bounds, Alignment, Subspace, Aliasing,
    MissingIdentity, DuplicateIdentity, DisabledIdentity, Domain,
    NotInitialized, AlreadyInitialized, Busy, Pending, FirmwareError, Timeout,
    Quarantined, LateComplete, LateError, Protocol, Faulted, RateLimited,
    Turnaround, TimeBackwards, Deadline, NoOutstanding, UnknownValue
};
const char *codeName(Code code);
bool checkedAdd(uint64_t a, uint64_t b, uint64_t &result);

struct Gas {
    uint8_t space, width, bitOffset, access;
    uint64_t address; // PCC: payload offset; FFH: identifier; never dereferenced.
};
constexpr uint8_t kSystemMemory = 0x00;
constexpr uint8_t kPcc = 0x0a;
constexpr uint8_t kFfh = 0x7f;

// Full Generic Register Resource + EndTag (17 bytes), not an AML evaluator.
Code parseGasResource(Bytes bytes, Gas &out);
// Host-friendly, bounded hex byte tokens; # comments permitted.
Code decodeHex(Bytes text, uint8_t *out, size_t capacity, size_t &size);

struct ChannelDescriptor {
    uint64_t baseAddress; // Opaque evidence only. No address -> pointer conversion.
    uint64_t length;
    Gas doorbell;
    uint64_t preserveMask, writeMask;
    uint32_t nominalLatencyUs, maxCommandsPerMinute;
    uint16_t minTurnaroundUs;
};
struct Pcct {
    size_t count;
    ChannelDescriptor channels[kMaxSubspaces];
};
Code validateChannel(const ChannelDescriptor &channel);
Code parsePcct(Bytes table, Pcct &out);
// GAS AccessSize is a subspace ordinal here, never a width encoding.
// The returned byteOffset includes the eight-byte shared-memory header.
Code pccLocation(const Gas &gas, const ChannelDescriptor &channel,
                 uint8_t subspace, uint64_t &byteOffset, uint8_t &byteWidth);

enum class IdentityKind : uint8_t { Lapic, X2Apic };
struct MadtCpu {
    IdentityKind kind;
    uint32_t acpiId, apicId;
    bool enabled, onlineCapable;
};
struct Madt {
    size_t count;
    MadtCpu cpus[kMaxMadtCpus];
};
Code parseMadt(Bytes table, Madt &out);

// The normalizer supplies both an absolute, canonical namespace path and an
// independently verified ProcessorID / numeric _UID. Never infer ID from name.
enum class EntryKind : uint8_t { Integer, Buffer, Dynamic };
struct StaticEntry {
    EntryKind kind;
    uint64_t integer;
    size_t size;
    uint8_t buffer[17];
};
struct StaticCpu {
    char path[kPathBytes];
    IdentityKind identityKind;
    uint32_t acpiId;
    uint64_t psdEntries, psdRevision, psdDomain, psdCoordination, psdProcessors;
    size_t count;
    StaticEntry entries[kCpcEntries];
};
struct StaticCpc {
    size_t count;
    StaticCpu cpus[kMaxCpus];
};
struct CpuMapEntry { uint32_t apicId, xnuCpu; };
struct CpuMap { size_t count; CpuMapEntry entries[kMaxCpus]; };
Code parseStaticCpc(Bytes text, StaticCpc &out);
Code parseCpuMap(Bytes text, CpuMap &out);

enum FieldIndex : size_t {
    Highest, Nominal, LowestNonlinear, Lowest, Guaranteed, Desired, Minimum,
    Maximum, ReductionTolerance, TimeWindow, WraparoundTime, ReferenceCounter,
    DeliveredCounter, PerformanceLimited, Enable, AutonomousSelection,
    AutonomousWindow, Epp, ReferencePerformance, LowestFrequency, NominalFrequency
};
const char *fieldName(size_t index);
enum class FieldKind : uint8_t { Unsupported, Integer, PccRegister, FfhCounter };
struct Field {
    FieldKind kind;
    uint64_t integer;
    Gas gas;
    uint64_t byteOffset; // Validated PCC offset, header included; not a value.
};
struct CpuDescriptor {
    char path[kPathBytes];
    uint32_t acpiId, apicId, xnuCpu, domain;
    IdentityKind identityKind;
    Field fields[kFieldCount];
};
struct DomainDescriptor {
    uint32_t id, expectedMembers, members; // Only HW_ALL is supported.
};
struct Discovery {
    size_t cpuCount, domainCount;
    Pcct pcct;
    CpuDescriptor cpus[kMaxCpus];
    DomainDescriptor domains[kMaxCpus];
};
// All enabled MADT CPUs, CPC packages and caller mappings must join one-to-one.
// Outputs of any parsing/import function are usable ONLY on Code::Ok.
Code importDescriptors(const StaticCpc &cpc, const Pcct &pcct, const Madt &madt,
                       const CpuMap &map, Discovery &out);

enum class ValueKind : uint8_t { Unknown, Unsupported, StaticInteger, Simulated };
struct Value { ValueKind kind; uint64_t number; };
Value initialValue(const Field &field);

// Local Linux references: include/acpi/{cppc_acpi,pcc}.h and actbl2.h.
constexpr size_t kPccHeaderBytes = 8;
constexpr size_t kCommandOffset = 4;
constexpr size_t kStatusOffset = 6;
constexpr uint32_t kPccSignature = 0x50434300;
constexpr uint16_t kReadCommand = 0;
constexpr uint16_t kCommandInterrupt = 1u << 15; // Not requested by this model.
constexpr uint16_t kComplete = 1u << 0;
constexpr uint16_t kSciDoorbell = 1u << 1;
constexpr uint16_t kError = 1u << 2;
constexpr uint16_t kPlatformNotify = 1u << 3;

enum class ChannelState : uint8_t {
    Uninitialized, OsOwned, InFlight, Quarantine, Fault
};

// One noncopyable owner of one entirely simulated shared channel, not per CPU.
// Serial calls only; an eventual real transport would need separate locking,
// barriers, lifecycle/ownership acquisition and explicit hardware authorization.
// No externally supplied pointer, MMIO callback or real transport can be bound.
class SimulatedReadChannel {
public:
    SimulatedReadChannel() = default;
    SimulatedReadChannel(const SimulatedReadChannel &) = delete;
    SimulatedReadChannel &operator=(const SimulatedReadChannel &) = delete;
    SimulatedReadChannel(SimulatedReadChannel &&) = delete;
    SimulatedReadChannel &operator=(SimulatedReadChannel &&) = delete;

    Code initialize(const ChannelDescriptor &descriptor, uint8_t subspace,
                    uint64_t initialDoorbell, uint64_t nowUs);
    // timeoutUs is a bounded caller choice; nominalLatencyUs never sets it.
    Code beginRead(uint64_t nowUs, uint64_t timeoutUs);
    Code poll(uint64_t nowUs);
    Code sample(const Field &field, Value &out) const;

    // Explicit simulated firmware/test actions, NOT controller write commands.
    Code simulateFirmwarePayload(const Gas &gas, uint64_t value);
    Code simulateFirmwareStatus(uint16_t status);
    Code simulateHeaderCorruption(size_t offset, uint8_t value);

    ChannelState state() const { return state_; }
    bool platformMayOwn() const {
        return state_ == ChannelState::InFlight || state_ == ChannelState::Quarantine ||
               state_ == ChannelState::Fault;
    }
    Bytes simulatedMemory() const { return {memory_, size_}; }
    uint64_t simulatedDoorbell() const { return doorbell_; }
    uint64_t generation() const { return generation_; }

private:
    Code time(uint64_t nowUs);
    Code protocolFault();
    ChannelState state_ = ChannelState::Uninitialized;
    ChannelDescriptor descriptor_{};
    uint8_t subspace_ = 0;
    size_t size_ = 0;
    uint8_t memory_[kMaxSharedBytes]{};
    // Simulator provenance: untouched/stale zero bytes never become settings.
    uint8_t supplied_[kMaxSharedBytes / 8]{};
    uint64_t doorbell_ = 0, lastNow_ = 0, deadline_ = 0, lastSent_ = 0;
    uint64_t lastCompletion_ = 0, generation_ = 0;
    bool haveSent_ = false, haveCompletion_ = false, snapshotValid_ = false;
};

} // namespace cppc_lab
#endif
