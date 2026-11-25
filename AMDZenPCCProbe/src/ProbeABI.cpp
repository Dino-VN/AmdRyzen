#include "ProbeABI.hpp"

namespace pcc_probe {
const char *resultName(Result value) {
    switch (value) {
#define R(n) case Result::n: return #n
        R(Ok); R(BadRequest); R(NotAdministrator); R(GateClosed); R(MetadataUnavailable);
        R(ProfileMismatch); R(IdentityMismatch); R(DescriptorInvalid); R(TransportUnavailable);
        R(Busy); R(AlreadyAttempted); R(ClientClosed); R(Stopped); R(PowerChanged);
        R(UnknownOwnership); R(Protocol); R(FirmwareError); R(Timeout); R(LateCompletion);
        R(TimeBackwards); R(Deadline); R(AccessFailed); R(Cancelled);
        case Result::Count: break;
#undef R
    }
    return "InvalidResult";
}
const char *valueStateName(ValueState value) {
    switch (value) {
        case ValueState::NotRequested: return "not-requested";
        case ValueState::Unknown: return "unknown";
        case ValueState::Unsupported: return "unsupported";
        case ValueState::StaticInteger: return "static-integer (not measured)";
        case ValueState::FirmwareRead: return "firmware-READ response";
        case ValueState::Count: break;
    }
    return "invalid-state";
}
const char *ownershipName(Ownership value) {
    switch (value) {
        case Ownership::Unknown: return "unknown";
        case Ownership::ObservedOs: return "Command Complete observed (not an exclusive OS lease)";
        case Ownership::PlatformMayOwn: return "platform may own; timeout does not release it";
        case Ownership::Count: break;
    }
    return "invalid-ownership";
}
Result validateRequest(uint32_t selector, const Request &r) {
    if (selector >= static_cast<uint32_t>(Selector::Count) || r.abiVersion != kAbi ||
        r.structSize != sizeof(Request) || r.cpu >= kCpuCount || r.fieldMask == 0 ||
        (r.fieldMask & ~kAllFields) != 0 || r.reserved[0] != 0 || r.reserved[1] != 0)
        return Result::BadRequest;
    if (selector == static_cast<uint32_t>(Selector::Info))
        return r.timeoutUs == 0 && r.acknowledgement == 0 ? Result::Ok : Result::BadRequest;
    if (r.acknowledgement != kReadAcknowledgement || r.timeoutUs == 0 || r.timeoutUs > kMaxTimeoutUs)
        return Result::BadRequest;
    return Result::Ok;
}
bool validateWireSizes(uint32_t selector, size_t in, size_t out, uint32_t scalarIn,
                       uint32_t scalarOut, bool indirect, bool async) {
    return selector < static_cast<uint32_t>(Selector::Count) && in == sizeof(Request) &&
           out == sizeof(Reply) && scalarIn == 0 && scalarOut == 0 && !indirect && !async;
}
void initializeReply(const Request &r, Reply &out) {
    out = {};
    out.abiVersion = kAbi;
    out.structSize = sizeof(Reply);
    out.cpu = r.cpu;
    out.fieldMask = r.fieldMask;
    // Availability is deliberately zero in this release, including successful metadata calls.
    out.physicalTransportAvailable = 0;
    for (uint32_t i = 0; i < kFields; ++i)
        out.fields[i].state = static_cast<uint32_t>((r.fieldMask & (1u << i)) != 0 ?
                                                  ValueState::Unknown : ValueState::NotRequested);
}
bool validateReply(const Request &r, Selector selector, const Reply &out, size_t size) {
    if (validateRequest(static_cast<uint32_t>(selector), r) != Result::Ok ||
        size != sizeof(Reply) || out.structSize != sizeof(Reply) || out.abiVersion != kAbi ||
        out.result >= static_cast<uint32_t>(Result::Count) || out.cpu != r.cpu ||
        out.fieldMask != r.fieldMask || (out.flags & ~kKnownFlags) != 0 ||
        out.physicalTransportAvailable > 1 || out.ownership >= static_cast<uint32_t>(Ownership::Count))
        return false;
    const auto ownership = static_cast<Ownership>(out.ownership);
    const bool mayOwn = (out.flags & PlatformMayOwn) != 0;
    if (mayOwn != (ownership == Ownership::PlatformMayOwn) ||
        ((out.flags & (Submitted | PlatformMayOwn | Quarantined)) != 0 && (out.flags & Attempted) == 0))
        return false;
    if (ownership == Ownership::ObservedOs &&
        (out.physicalTransportAvailable == 0 || (out.flags & (MetadataValid | Attempted)) != (MetadataValid | Attempted)))
        return false;
    bool terminated = false;
    for (char c : out.acpiPath) if (c == '\0') terminated = true;
    if (!terminated) return false;
    if ((out.flags & MetadataValid) != 0 && (out.logicalCpus != kCpuCount || out.domains != 8 ||
        out.subspace != 0 || out.headerBytes != 8 || out.doorbellWidth != 64)) return false;
    const bool successful = out.result == static_cast<uint32_t>(Result::Ok);
    if (successful && ((out.flags & MetadataValid) == 0 ||
                       (out.flags & (ServiceStopped | PowerInvalidated)) != 0)) return false;
    const bool successfulRead = selector == Selector::ReadOnce && successful;
    constexpr uint32_t readFlags = BootEnabled | ConfigEnabled | MetadataValid | Attempted | Submitted;
    if (successfulRead &&
        (out.physicalTransportAvailable == 0 || ownership != Ownership::ObservedOs ||
         (out.flags & readFlags) != readFlags || (out.flags & (Quarantined | PlatformMayOwn)) != 0 ||
         out.completedUs < out.startedUs || out.completedUs - out.startedUs >= r.timeoutUs)) return false;
    for (uint32_t i = 0; i < kFields; ++i) {
        const FieldView &f = out.fields[i];
        if (f.state >= static_cast<uint32_t>(ValueState::Count) ||
            f.descriptorKind > static_cast<uint32_t>(DescriptorKind::Ffh)) return false;
        const auto state = static_cast<ValueState>(f.state);
        if (((r.fieldMask & (1u << i)) == 0) != (state == ValueState::NotRequested)) return false;
        const bool pcc = f.descriptorKind == static_cast<uint32_t>(DescriptorKind::Pcc);
        if ((out.flags & MetadataValid) != 0 && pcc) {
            // Descriptor validity is independent of whether the value is known.
            // INFO must not print malformed PCC geometry as validated metadata.
            if (f.addressSpace != 0x0a || (f.bitWidth != 8 && f.bitWidth != 16 && f.bitWidth != 32 && f.bitWidth != 64) ||
                f.descriptorAddress > UINT64_MAX - out.headerBytes ||
                f.byteOffset != f.descriptorAddress + out.headerBytes || f.byteOffset % (f.bitWidth / 8) != 0 ||
                f.byteOffset > out.sharedBytes || f.bitWidth / 8 > out.sharedBytes - f.byteOffset) return false;
            if (successfulRead && (r.fieldMask & (1u << i)) != 0 && state != ValueState::FirmwareRead) return false;
        }
        if (state == ValueState::FirmwareRead &&
            (!successfulRead || !pcc || (f.bitWidth < 64 && f.value >= (UINT64_C(1) << f.bitWidth)))) return false;
        if (state == ValueState::StaticInteger &&
            ((out.flags & MetadataValid) == 0 || f.descriptorKind != static_cast<uint32_t>(DescriptorKind::Integer)))
            return false;
        if (state == ValueState::Unsupported && f.descriptorKind != static_cast<uint32_t>(DescriptorKind::Unsupported))
            return false;
        if (state != ValueState::StaticInteger && state != ValueState::FirmwareRead && f.value != 0)
            return false;
    }
    return true;
}
} // namespace pcc_probe
