// Diagnostic-only ABI. No address, register-write, or performance-control inputs.
#ifndef AMDZEN_PCC_PROBE_ABI_HPP
#define AMDZEN_PCC_PROBE_ABI_HPP
#include <stddef.h>
#include <stdint.h>

namespace pcc_probe {
constexpr uint32_t kAbi = 1;
constexpr const char *kService = "AMDZenPCCProbe";
constexpr uint32_t kCpuCount = 16;
constexpr uint32_t kFields = 21;
constexpr uint32_t kAllFields = (1u << kFields) - 1u;
constexpr uint64_t kReadAcknowledgement = UINT64_C(0x5043435245414431);
constexpr uint64_t kMaxTimeoutUs = 1000000;
constexpr const char *kReadAckOption = "--acknowledge-read-writes-command-status-and-doorbell";

enum class Selector : uint32_t { Info = 0, ReadOnce = 1, Count };
enum class Result : uint32_t {
    Ok, BadRequest, NotAdministrator, GateClosed, MetadataUnavailable,
    ProfileMismatch, IdentityMismatch, DescriptorInvalid, TransportUnavailable,
    Busy, AlreadyAttempted, ClientClosed, Stopped, PowerChanged,
    UnknownOwnership, Protocol, FirmwareError, Timeout, LateCompletion,
    TimeBackwards, Deadline, AccessFailed, Cancelled, Count
};
enum class ValueState : uint32_t {
    NotRequested, Unknown, Unsupported, StaticInteger, FirmwareRead, Count
};
enum class DescriptorKind : uint32_t { Unsupported, Integer, Pcc, Ffh };
// An observation, never proof of an exclusive OSPM-consumer lease.
enum class Ownership : uint32_t { Unknown, ObservedOs, PlatformMayOwn, Count };
enum Flags : uint32_t {
    BootEnabled = 1u << 0, ConfigEnabled = 1u << 1, MetadataValid = 1u << 2,
    Attempted = 1u << 3, Submitted = 1u << 4, PlatformMayOwn = 1u << 5,
    Quarantined = 1u << 6, ServiceStopped = 1u << 7, PowerInvalidated = 1u << 8,
    ActiveCall = 1u << 9
};
constexpr uint32_t kKnownFlags = (1u << 10) - 1u;
constexpr uint32_t kControlFlags = Attempted | Submitted | PlatformMayOwn | Quarantined |
                                   ServiceStopped | PowerInvalidated | ActiveCall;

struct Request {
    uint32_t abiVersion, structSize;
    uint32_t cpu, fieldMask; // XNU logical CPU; field bits match cppc_lab::FieldIndex.
    uint64_t timeoutUs, acknowledgement;
    uint64_t reserved[2];
};
struct FieldView {
    uint32_t descriptorKind, state;
    uint32_t bitWidth, addressSpace;
    uint64_t descriptorAddress; // PCC payload offset or FFH identifier, NOT a value.
    uint64_t byteOffset;        // PCC offset with the 8-byte header included.
    uint64_t value;             // Valid only for StaticInteger or FirmwareRead.
};
struct Reply {
    uint32_t abiVersion, structSize, result, detail; // detail: cppc_lab::Code.
    uint32_t flags, cpu, fieldMask, physicalTransportAvailable;
    uint32_t logicalCpus, domains, acpiId, apicId;
    uint32_t domain, subspace, headerBytes, ownership;
    uint64_t startedUs, completedUs; // Zero unless a transport clock was observed.
    uint64_t sharedBase, sharedBytes, doorbellAddress;
    uint64_t doorbellPreserve, doorbellWrite;
    uint32_t doorbellWidth, nominalLatencyUs, maxCommandsPerMinute, minTurnaroundUs;
    char acpiPath[64];
    uint8_t processorTableSha256[32], cpcTableSha256[32];
    FieldView fields[kFields];
};
static_assert(sizeof(Request) == 48, "Request ABI drift");
static_assert(sizeof(FieldView) == 40, "Field ABI drift");
static_assert(sizeof(Reply) == 1104, "Reply ABI drift");
static_assert(offsetof(Reply, fields) == 264, "Reply field offset drift");
static_assert(sizeof(Reply) < 4096, "Only inline user-client buffers are supported");

const char *resultName(Result value);
const char *valueStateName(ValueState value);
const char *ownershipName(Ownership value);
Result validateRequest(uint32_t selector, const Request &request);
bool validateWireSizes(uint32_t selector, size_t inputBytes, size_t outputBytes,
                       uint32_t scalarIn, uint32_t scalarOut, bool indirect, bool async);
bool validateReply(const Request &request, Selector selector, const Reply &reply, size_t size);
void initializeReply(const Request &request, Reply &reply);
} // namespace pcc_probe
#endif
