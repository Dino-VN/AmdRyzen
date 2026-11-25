// Freestanding CPPC/PCC model. Parsers are ported from cppc-lab/Descriptors.cpp;
// command ordering follows Linux cppc_acpi.c send_pcc_cmd and mailbox/pcc.c.
#ifndef ZEN_PCC_MODEL_HPP
#define ZEN_PCC_MODEL_HPP
#include <stddef.h>
#include <stdint.h>
#include "ZenCppcTypes.hpp"

namespace zenpcc {
constexpr uint32_t kMaxCpus = 64;
constexpr uint32_t kMaxMadtCpus = 128;
constexpr size_t kCpcEntries = 23;
constexpr uint32_t kMaxSharedBytes = 65536;
constexpr uint32_t kHeaderBytes = 8;
constexpr uint32_t kSignature = 0x50434300; // OR with subspace ordinal (only 0).
constexpr uint16_t kComplete = 1, kError = 4;
constexpr uint32_t kMaxCompletionTimeoutUs = 2000000;
constexpr uint32_t kRateWaitBudgetUs = 50000; // spacing only; completion has its own deadline

enum class Error : uint32_t {
    Ok, BadArgument, Truncated, Length, Signature, Checksum, Revision, Reserved,
    Unsupported, Gas, Overflow, Bounds, Alignment, Subspace, Aliasing,
    MissingIdentity, DuplicateIdentity, DisabledIdentity, Domain, NoMemory,
    AcpiTables, AcpiEvaluation, Mapping, WrongContext, NotInitialized,
    AlreadyInitialized, Access, Timeout, FirmwareError, TimeBackwards,
    RateLimited, BadValue, BadRequest, AliasConflict, MissingCaps, SubspaceClaimed
};
// One claim domain per kext, shared by every Channel. Retiring after all mappings
// are released keeps the claim consumed for the entire kext lifetime, including
// discovery failure and timeout quarantine. No reset/rearm is supported.
class SubspaceClaim {
public:
    SubspaceClaim() = default;
    SubspaceClaim(const SubspaceClaim &) = delete;
    SubspaceClaim &operator=(const SubspaceClaim &) = delete;
    Error acquire();
    void retire(); // owner only, after teardown finishes; safe to repeat
private:
    uint32_t state_ = 0; // never claimed, active, retired
};
struct Bytes { const uint8_t *data; size_t size; };
struct Gas { uint8_t space, width, bitOffset, access; uint64_t address; };
struct Descriptor {
    uint64_t baseAddress, length;
    Gas doorbell;
    uint64_t preserveMask, writeMask;
    uint32_t nominalLatencyUs, maxCommandsPerMinute;
    uint16_t minTurnaroundUs;
};
// Only the generic, polled subspace 0 is supported. Additional subspaces fail
// closed. PCC GAS AccessSize is the ordinal, NOT the transfer width.
Error validateDescriptor(const Descriptor &d);
Error parsePcct(Bytes table, Descriptor &out);
Error parseGas(Bytes resource, Gas &out);

enum class EntryKind : uint8_t { Integer, Buffer, Invalid };
struct CpcEntry { EntryKind kind; uint64_t integer; Bytes buffer; };
enum FieldIndex : uint32_t {
    Highest, Nominal, LowestNonlinear, Lowest, Guaranteed, Desired, Minimum,
    Maximum, ReductionTolerance, TimeWindow, WraparoundTime, ReferenceCounter,
    DeliveredCounter, PerformanceLimited, Enable, AutonomousSelection,
    AutonomousWindow, Epp, ReferencePerformance, LowestFrequency, NominalFrequency,
    FieldCount
};
enum class FieldKind : uint8_t { Absent, Integer, Pcc, FfhCounter };
struct Field { FieldKind kind; uint32_t value, offset; }; // offset includes header
struct CpuRegisters {
    uint32_t acpiId, apicId, domain, domainMembers;
    bool x2Apic;
    Field fields[FieldCount];
};
// Accept revisions 2/3 (21/23 entries); every used control must be PCC/32/0/0.
// Caps may instead be integers. Other fields are validated metadata, never I/O.
Error parseCpc(const CpcEntry *entries, size_t count, const Descriptor &d,
               CpuRegisters &out);
Error validateCpuMaps(const CpuRegisters *cpus, uint32_t count);
struct MadtCpu { uint32_t acpiId, apicId; bool x2Apic, enabled; };
struct Madt { uint32_t count; MadtCpu cpus[kMaxMadtCpus]; };
Error parseMadt(Bytes table, Madt &out);
// Bounded identity decoder for the AMI CPUSSDT used by this machine: a single
// Scope (\\_PR_) containing empty Processor declarations. This is not an AML
// interpreter. IDs come from declaration bytes, never from C00n name digits.
struct ProcessorIdentity { char name[5]; uint32_t acpiId; };
struct ProcessorNames { uint32_t count; ProcessorIdentity cpus[kMaxCpus]; };
Error parseProcessorSsdt(Bytes table, ProcessorNames &out);
// Independent ACPI identity -> MADT APIC ID -> caller's XNU-indexed APIC map.
Error joinCpu(const Madt &madt, uint32_t acpiId, bool x2Apic,
              const uint16_t *apicIdByXnuCpu, uint32_t count, uint32_t &xnuCpu);

// All calls are serialized by the owner. Binding, including initialize(), does
// no I/O. Exact-width header accesses avoid replaying adjacent firmware status.
// Implementations must provide monotonic microseconds and a blocking wait.
class Accessor {
public:
    virtual ~Accessor() = default;
    virtual bool read16(uint32_t offset, uint16_t &value) = 0;
    virtual bool write16(uint32_t offset, uint16_t value) = 0;
    virtual bool read32(uint32_t offset, uint32_t &value) = 0;
    virtual bool write32(uint32_t offset, uint32_t value) = 0;
    virtual bool read64(uint64_t &doorbell) = 0;
    virtual bool write64(uint64_t doorbell) = 0;
    virtual void barrier() = 0; // full MMIO fence, including completion -> payload
    virtual uint64_t nowUs() = 0;
    virtual void waitUs(uint32_t us) = 0;
};

class Engine {
public:
    Engine() = default;
    Engine(const Engine &) = delete;
    Engine &operator=(const Engine &) = delete;
    // Maps/accessor are borrowed, immutable, and must outlive the Engine.
    Error initialize(const Descriptor &d, const CpuRegisters *cpus, uint32_t count,
                     Accessor &accessor);
    bool readCaps(uint32_t cpu, zencppc::Caps *out);
    bool readState(uint32_t cpu, uint32_t *enable, uint32_t *autonomy,
                   zencppc::Request *out);
    bool enableAutonomous(const zencppc::Request *requests, uint32_t count);
    bool apply(const zencppc::Request *requests, const bool *changed, uint32_t count);
    Error lastError() const { return lastError_; }
    bool failed() const { return failed_; }
    uint64_t commandCount() const { return commandCount_; }
    uint64_t lastLatencyUs() const { return lastLatencyUs_; }
    uint32_t completionTimeoutUs() const;
private:
    bool reject(Error error);
    bool fault(Error error);
    bool clock(uint64_t &now);
    bool prepare(); // wait for platform ownership/spacing BEFORE payload writes
    bool complete(uint64_t deadline, bool command);
    bool issue(uint16_t command);
    bool sample(uint32_t cpu, FieldIndex field, uint32_t &out);
    bool validateRequests(const zencppc::Request *requests, const bool *changed,
                          uint32_t count, bool autonomous);
    bool writeRequests(const zencppc::Request *requests, const bool *changed,
                       uint32_t count, bool enable);
    Descriptor descriptor_{};
    const CpuRegisters *cpus_ = nullptr;
    Accessor *accessor_ = nullptr;
    uint32_t count_ = 0;
    Error lastError_ = Error::Ok;
    bool initialized_ = false, failed_ = false, platformOwnsPcc_ = false;
    bool haveNow_ = false, haveSent_ = false, haveCompletion_ = false;
    uint64_t lastNow_ = 0, lastSent_ = 0, lastCompletion_ = 0;
    uint64_t commandStart_ = 0, commandCount_ = 0, lastLatencyUs_ = 0;
    zencppc::Caps caps_[kMaxCpus]{};
    bool haveCaps_[kMaxCpus]{};
};
} // namespace zenpcc
#endif
