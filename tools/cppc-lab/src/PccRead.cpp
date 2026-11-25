#include "CppcLab.hpp"

namespace cppc_lab {
namespace {

uint64_t load(const uint8_t *bytes, size_t count) {
    uint64_t value = 0;
    for (size_t i = 0; i < count; ++i) value |= uint64_t(bytes[i]) << (i * 8);
    return value;
}
void store(uint8_t *bytes, size_t count, uint64_t value) {
    for (size_t i = 0; i < count; ++i) bytes[i] = static_cast<uint8_t>(value >> (i * 8));
}
uint64_t mask(uint8_t width) {
    return width == 64 ? UINT64_MAX : (uint64_t(1) << width) - 1;
}

} // namespace

Code SimulatedReadChannel::initialize(const ChannelDescriptor &descriptor, uint8_t subspace,
                                      uint64_t initialDoorbell, uint64_t nowUs) {
    if (state_ != ChannelState::Uninitialized) return Code::AlreadyInitialized;
    if (subspace >= kMaxSubspaces) return Code::Subspace;
    const Code code = validateChannel(descriptor);
    if (code != Code::Ok) return code;
    if ((initialDoorbell & ~mask(descriptor.doorbell.width)) != 0) return Code::Bounds;
    descriptor_ = descriptor;
    subspace_ = subspace;
    size_ = static_cast<size_t>(descriptor.length);
    for (size_t i = 0; i < size_; ++i) memory_[i] = 0;
    // Synthetic firmware initialization, NOT an observed signature/ownership.
    store(memory_, 4, (kPccSignature | subspace_));
    store(memory_ + kCommandOffset, 2, kReadCommand);
    store(memory_ + kStatusOffset, 2, kComplete);
    doorbell_ = initialDoorbell;
    lastNow_ = nowUs;
    state_ = ChannelState::OsOwned;
    return Code::Ok;
}

Code SimulatedReadChannel::time(uint64_t nowUs) {
    if (nowUs < lastNow_) return Code::TimeBackwards;
    lastNow_ = nowUs; // Including a rejected busy/rate/deadline attempt.
    return Code::Ok;
}

Code SimulatedReadChannel::protocolFault() {
    snapshotValid_ = false;
    state_ = ChannelState::Fault; // Unknown ownership; no automatic reuse/reset.
    return Code::Protocol;
}

Code SimulatedReadChannel::beginRead(uint64_t nowUs, uint64_t timeoutUs) {
    if (state_ == ChannelState::Uninitialized) return Code::NotInitialized;
    Code code = time(nowUs);
    if (code != Code::Ok) return code;
    if (state_ == ChannelState::Quarantine) return Code::Quarantined;
    if (state_ == ChannelState::Fault) return Code::Faulted;
    if (state_ != ChannelState::OsOwned) return Code::Busy;
    if (timeoutUs == 0 || timeoutUs > kMaxDeadlineUs) return Code::Deadline;
    uint64_t deadline = 0;
    if (!checkedAdd(nowUs, timeoutUs, deadline) || generation_ == UINT64_MAX)
        return Code::Overflow;
    if (haveCompletion_ && nowUs - lastCompletion_ < descriptor_.minTurnaroundUs)
        return Code::Turnaround;
    if (haveSent_ && descriptor_.maxCommandsPerMinute != 0) {
        const uint64_t rate = descriptor_.maxCommandsPerMinute;
        // Conservative non-bursting limiter. This meets the maximum in ANY
        // half-open rolling minute, unlike a resettable fixed-window counter.
        const uint64_t spacing = kMinuteUs / rate + (kMinuteUs % rate != 0 ? 1u : 0u);
        if (nowUs - lastSent_ < spacing) return Code::RateLimited;
    }
    const uint64_t status = load(memory_ + kStatusOffset, 2);
    if (load(memory_, 4) != (kPccSignature | subspace_) ||
        load(memory_ + kCommandOffset, 2) != kReadCommand ||
        (status & kComplete) == 0 || (status & ~uint64_t(kComplete | kError)) != 0)
        return protocolFault();

    // No payload writes by the controller, and there is no command selector.
    // Linux cppc_acpi.c send_pcc_cmd: command, status=0, platform owns, doorbell.
    store(memory_ + kCommandOffset, 2, kReadCommand);
    store(memory_ + kStatusOffset, 2, 0);
    snapshotValid_ = false;
    for (size_t i = 0; i < sizeof(supplied_); ++i) supplied_[i] = 0;
    deadline_ = deadline;
    lastSent_ = nowUs;
    haveSent_ = true;
    ++generation_;
    state_ = ChannelState::InFlight;
    // Linux drivers/mailbox/pcc.c pcc_chan_reg_read_modify_write.
    // Set mask wins if masks overlap; this is not OR-ing the entire old value.
    doorbell_ = (doorbell_ & descriptor_.preserveMask) | descriptor_.writeMask;
    return Code::Ok;
}

Code SimulatedReadChannel::poll(uint64_t nowUs) {
    if (state_ == ChannelState::Uninitialized) return Code::NotInitialized;
    Code code = time(nowUs);
    if (code != Code::Ok) return code;
    if (state_ == ChannelState::Fault) return Code::Faulted;
    if (state_ == ChannelState::OsOwned) return Code::NoOutstanding;
    if (load(memory_, 4) != (kPccSignature | subspace_) || load(memory_ + kCommandOffset, 2) != kReadCommand)
        return protocolFault();
    const uint64_t status = load(memory_ + kStatusOffset, 2);
    // SCI/notification/unknown bits require protocols this polling model lacks.
    if ((status & ~uint64_t(kComplete | kError)) != 0) return protocolFault();
    if ((status & kComplete) != 0) {
        // A completion first observed AT or after the deadline is late even if
        // no earlier poll noticed expiry. Never expose that timed-out payload.
        const bool late = state_ == ChannelState::Quarantine || nowUs >= deadline_;
        const bool error = (status & kError) != 0;
        lastCompletion_ = nowUs; // Conservative: observation, not firmware time.
        haveCompletion_ = true;
        snapshotValid_ = !late && !error;
        state_ = ChannelState::OsOwned;
        if (late) return error ? Code::LateError : Code::LateComplete;
        return error ? Code::FirmwareError : Code::Ok;
    }
    // Error without Command Complete does NOT give OSPM ownership.
    if (state_ == ChannelState::Quarantine) return Code::Quarantined;
    if (nowUs >= deadline_) {
        snapshotValid_ = false;
        state_ = ChannelState::Quarantine;
        return Code::Timeout;
    }
    return Code::Pending;
}

Code SimulatedReadChannel::sample(const Field &field, Value &out) const {
    out = initialValue(field);
    if (out.kind == ValueKind::StaticInteger || out.kind == ValueKind::Unsupported)
        return Code::Ok;
    if (field.kind != FieldKind::PccRegister || state_ != ChannelState::OsOwned || !snapshotValid_)
        return Code::UnknownValue;
    uint64_t offset = 0;
    uint8_t width = 0;
    const Code code = pccLocation(field.gas, descriptor_, subspace_, offset, width);
    if (code != Code::Ok) return code;
    if (offset != field.byteOffset || offset > size_ || width > size_ - offset)
        return Code::Bounds;
    for (uint64_t i = offset; i < offset + width; ++i)
        if ((supplied_[i / 8] & (uint32_t(1) << (i % 8))) == 0) return Code::UnknownValue;
    out = {ValueKind::Simulated, load(memory_ + static_cast<size_t>(offset), width)};
    return Code::Ok;
}

Code SimulatedReadChannel::simulateFirmwarePayload(const Gas &gas, uint64_t value) {
    if (state_ != ChannelState::InFlight && state_ != ChannelState::Quarantine)
        return Code::NoOutstanding;
    uint64_t offset = 0;
    uint8_t width = 0;
    const Code code = pccLocation(gas, descriptor_, subspace_, offset, width);
    if (code != Code::Ok) return code;
    if (offset > size_ || width > size_ - offset || (value & ~mask(gas.width)) != 0)
        return Code::Bounds;
    store(memory_ + static_cast<size_t>(offset), width, value);
    for (uint64_t i = offset; i < offset + width; ++i)
        supplied_[i / 8] = static_cast<uint8_t>(supplied_[i / 8] | (uint32_t(1) << (i % 8)));
    return Code::Ok;
}

Code SimulatedReadChannel::simulateFirmwareStatus(uint16_t status) {
    if (state_ != ChannelState::InFlight && state_ != ChannelState::Quarantine)
        return Code::NoOutstanding;
    store(memory_ + kStatusOffset, 2, status);
    return Code::Ok;
}

Code SimulatedReadChannel::simulateHeaderCorruption(size_t offset, uint8_t value) {
    if (state_ != ChannelState::InFlight && state_ != ChannelState::Quarantine)
        return Code::NoOutstanding;
    if (offset >= kPccHeaderBytes) return Code::Bounds;
    memory_[offset] = value;
    return Code::Ok;
}

} // namespace cppc_lab
