#include "ReadEngine.hpp"

namespace pcc_probe {
Result Control::cancellation(const ClientState &client) const {
    const uint32_t f = flags();
    if ((f & ServiceStopped) != 0) return Result::Stopped;
    if ((f & PowerInvalidated) != 0) return Result::PowerChanged;
    if (client.closed()) return Result::ClientClosed;
    return Result::Ok;
}
Result Control::tryEnter(const ClientState &client) {
    Result result = cancellation(client);
    if (result != Result::Ok) return result;
    uint32_t expected = flags();
    if ((expected & ActiveCall) != 0) return Result::Busy;
    if ((expected & (ServiceStopped | PowerInvalidated)) != 0) return cancellation(client);
    if (!__atomic_compare_exchange_n(&flags_, &expected, expected | ActiveCall, false,
                                     __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) {
        result = cancellation(client);
        return result == Result::Ok ? Result::Busy : result;
    }
    result = cancellation(client);
    if (result != Result::Ok) leave();
    return result;
}
void Control::leave() { __atomic_fetch_and(&flags_, ~uint32_t(ActiveCall), __ATOMIC_RELEASE); }
void Control::stop() { __atomic_fetch_or(&flags_, uint32_t(ServiceStopped), __ATOMIC_ACQ_REL); }
void Control::powerChanged() { __atomic_fetch_or(&flags_, uint32_t(PowerInvalidated), __ATOMIC_ACQ_REL); }
void Control::capabilitiesChanged(uint32_t from, uint32_t to, uint32_t cpuCapability) {
    if (((from ^ to) & cpuCapability) != 0) powerChanged();
}
bool Control::reserveAttempt() {
    return (__atomic_fetch_or(&flags_, uint32_t(Attempted), __ATOMIC_ACQ_REL) & Attempted) == 0;
}
void Control::mayOwn() { __atomic_fetch_or(&flags_, uint32_t(PlatformMayOwn), __ATOMIC_ACQ_REL); }
void Control::submitted() { __atomic_fetch_or(&flags_, uint32_t(Submitted), __ATOMIC_ACQ_REL); }
void Control::completed() { __atomic_fetch_and(&flags_, ~uint32_t(PlatformMayOwn), __ATOMIC_ACQ_REL); }
void Control::quarantine() { __atomic_fetch_or(&flags_, uint32_t(Quarantined), __ATOMIC_ACQ_REL); }

namespace {
constexpr size_t kPollLimit = 20000; // Bounds broken/nonadvancing clocks as well.
struct MappingGuard {
    ReadTransport &transport;
    ~MappingGuard() { transport.releaseMapping(); }
};
bool equal(const Header &a, const Header &b) {
    return a.signature == b.signature && a.command == b.command && a.status == b.status;
}
bool protocol(const Header &h) {
    return h.signature == (cppc_lab::kPccSignature | 0u) && h.command == cppc_lab::kReadCommand &&
           (h.status & ~uint16_t(cppc_lab::kComplete | cppc_lab::kError)) == 0;
}
void discardValues(Reply &out) {
    for (FieldView &f : out.fields)
        if (f.state == static_cast<uint32_t>(ValueState::FirmwareRead)) {
            f.state = static_cast<uint32_t>(ValueState::Unknown); f.value = 0;
        }
    out.completedUs = 0;
}
Result finish(Result result, Control &control, Reply &reply) {
    if (result != Result::Ok) {
        discardValues(reply);
        if ((control.flags() & PlatformMayOwn) != 0) control.quarantine();
    }
    const uint32_t flags = control.flags();
    reply.flags = (reply.flags & ~kControlFlags) | flags;
    if ((flags & PlatformMayOwn) != 0)
        reply.ownership = static_cast<uint32_t>(Ownership::PlatformMayOwn);
    else if (result == Result::UnknownOwnership || result == Result::Protocol ||
             result == Result::AccessFailed || result == Result::TimeBackwards ||
             result == Result::Stopped || result == Result::PowerChanged ||
             result == Result::ClientClosed || result == Result::Cancelled)
        reply.ownership = static_cast<uint32_t>(Ownership::Unknown);
    reply.result = static_cast<uint32_t>(result);
    return result;
}
struct Clock {
    uint64_t last, deadline;
    bool read(ReadTransport &t, uint64_t &now) {
        now = t.nowUs();
        if (now < last) return false;
        last = now;
        return true;
    }
};
}
Result completeCall(const Request &r, Result result, Control &control,
                    const ClientState &client, Reply &out) {
    const Result cancelled = control.cancellation(client);
    if (cancelled != Result::Ok) {
        result = cancelled;
        const uint32_t gates = out.flags & (BootEnabled | ConfigEnabled);
        initializeReply(r, out); // No stale descriptors or static/READ values.
        out.flags = gates;
    }
    control.leave();
    return finish(result, control, out);
}
Result readOnce(const Request &r, bool admin, bool bootGate, bool configGate,
                const Workspace &w, Control &control, const ClientState &client,
                ReadTransport *t, Reply &out) {
    using namespace cppc_lab;
    out.flags = (out.flags & ~uint32_t(BootEnabled | ConfigEnabled)) |
                (bootGate ? uint32_t(BootEnabled) : 0u) | (configGate ? uint32_t(ConfigEnabled) : 0u);
    auto end = [&](Result result) { return finish(result, control, out); };
    if (validateRequest(static_cast<uint32_t>(Selector::ReadOnce), r) != Result::Ok)
        return end(Result::BadRequest);
    if (!admin) return end(Result::NotAdministrator);
    if (!bootGate || !configGate) return end(Result::GateClosed);
    Result cancelled = control.cancellation(client);
    if (cancelled != Result::Ok) return end(cancelled);
    if ((control.flags() & ActiveCall) == 0) return end(Result::Busy);
    if ((out.flags & MetadataValid) == 0 || w.discovery.cpuCount != kCpuCount || w.discovery.pcct.count != 1)
        return end(Result::MetadataUnavailable);
    if (!control.reserveAttempt()) return end(Result::AlreadyAttempted);
    // There is no user-controllable transport/lease/config override. The shipped
    // kext passes nullptr, before any clock, mapping, header, or device access.
    if (!t) return end(Result::TransportUnavailable);
    // An injected transport exists only in host tests in this release. This bit
    // describes a binding, not a successful lease or a responsive firmware.
    out.physicalTransportAvailable = 1;
    const ChannelDescriptor &channel = w.discovery.pcct.channels[0];
    const CpuDescriptor *cpu = nullptr;
    for (size_t i = 0; i < w.discovery.cpuCount; ++i)
        if (w.discovery.cpus[i].xnuCpu == r.cpu) cpu = &w.discovery.cpus[i];
    if (!cpu || validateChannel(channel) != Code::Ok) return end(Result::DescriptorInvalid);
    const uint64_t start = t->nowUs();
    uint64_t deadline = 0;
    if (!checkedAdd(start, r.timeoutUs, deadline)) return end(Result::Deadline);
    out.startedUs = start;
    Clock clock{start, deadline};
    if (!t->claim(channel)) return end(Result::TransportUnavailable);
    MappingGuard mapping{*t};
    cancelled = control.cancellation(client);
    if (cancelled != Result::Ok) return end(cancelled);
    Header initial{};
    if (!t->readHeader(initial)) return end(Result::AccessFailed);
    // Inspect the actual initial state; NEVER seed signature or Command Complete.
    // A previous error, notification, WRITE command or incomplete command refuses.
    if (!protocol(initial) || initial.status != kComplete) return end(Result::UnknownOwnership);
    out.ownership = static_cast<uint32_t>(Ownership::ObservedOs);
    uint64_t now = 0;
    if (!clock.read(*t, now)) return end(Result::TimeBackwards);
    const uint64_t observed = now;
    // Initial last-command time is unknown. Wait conservatively after observing
    // completion, then recheck ownership. Nominal latency is NOT a timeout.
    const uint64_t rate = channel.maxCommandsPerMinute;
    uint64_t quiet = rate == 0 ? 0 : kMinuteUs / rate + (kMinuteUs % rate != 0 ? 1u : 0u);
    if (quiet < channel.minTurnaroundUs) quiet = channel.minTurnaroundUs;
    size_t polls = 0;
    while (now - observed < quiet) {
        cancelled = control.cancellation(client);
        if (cancelled != Result::Ok) return end(cancelled);
        if (now >= clock.deadline || ++polls >= kPollLimit) return end(Result::Timeout);
        t->pause();
        if (!clock.read(*t, now)) return end(Result::TimeBackwards);
    }
    if (now >= clock.deadline) return end(Result::Timeout);
    // The transport's commit fence must also serialize against teardown/power and
    // client cancellation, not merely check a racy boolean before three writes.
    if (!t->beginCommit(control, client)) {
        cancelled = control.cancellation(client);
        return end(cancelled == Result::Ok ? Result::Cancelled : cancelled);
    }
    Result committed = Result::Ok;
    Header before{};
    uint64_t oldBell = 0;
    if (!t->readHeader(before) || !t->readDoorbell(oldBell)) committed = Result::AccessFailed;
    else if (!equal(initial, before)) committed = Result::UnknownOwnership;
    else if (!clock.read(*t, now)) committed = Result::TimeBackwards;
    else if (now >= clock.deadline) committed = Result::Timeout;
    else {
        // All possible write targets are fixed by this interface. No payload
        // controls, signature initialization, status acknowledgement, or retries.
        control.mayOwn(); // Conservative even on a partially failed submission.
        if (!t->writeReadCommand() || !t->clearStatus()) committed = Result::AccessFailed;
        else {
            t->deviceBarrier();
            if (!t->writeDoorbell((oldBell & channel.preserveMask) | channel.writeMask))
                committed = Result::AccessFailed;
            else {
                control.submitted();
                t->deviceBarrier();
            }
        }
    }
    t->endCommit();
    if (committed != Result::Ok) return end(committed);
    polls = 0;
    for (;;) {
        cancelled = control.cancellation(client);
        if (cancelled != Result::Ok) return end(cancelled);
        Header header{};
        if (!t->readHeader(header)) return end(Result::AccessFailed);
        if (!clock.read(*t, now)) return end(Result::TimeBackwards);
        if (!protocol(header)) return end(Result::Protocol);
        if ((header.status & kComplete) != 0) {
            control.completed(); // Only an observed protocol-valid completion.
            out.ownership = static_cast<uint32_t>(Ownership::ObservedOs);
            if (now >= clock.deadline) { control.quarantine(); return end(Result::LateCompletion); }
            if ((header.status & kError) != 0) return end(Result::FirmwareError);
            t->deviceBarrier();
            for (uint32_t i = 0; i < kFields; ++i) {
                cancelled = control.cancellation(client);
                if (cancelled != Result::Ok) return end(cancelled);
                if ((r.fieldMask & (1u << i)) == 0) continue;
                const Field &field = cpu->fields[i];
                if (field.kind != FieldKind::PccRegister) continue; // FFH never read.
                uint64_t offset = 0, value = 0;
                uint8_t width = 0;
                if (pccLocation(field.gas, channel, 0, offset, width) != Code::Ok || offset != field.byteOffset)
                    return end(Result::DescriptorInvalid);
                if (!t->readField(offset, width, value)) return end(Result::AccessFailed);
                if (width < 8 && value >= (UINT64_C(1) << (width * 8))) return end(Result::Protocol);
                out.fields[i].state = static_cast<uint32_t>(ValueState::FirmwareRead);
                out.fields[i].value = value;
            }
            Header after{};
            if (!t->readHeader(after)) return end(Result::AccessFailed);
            if (!equal(header, after)) {
                control.mayOwn();
                return end(Result::Protocol);
            }
            if (!clock.read(*t, now)) return end(Result::TimeBackwards);
            if (now >= clock.deadline) { control.quarantine(); return end(Result::LateCompletion); }
            cancelled = control.cancellation(client);
            if (cancelled != Result::Ok) return end(cancelled);
            out.completedUs = now;
            return end(Result::Ok);
        }
        if ((header.status & kError) != 0) return end(Result::FirmwareError); // Still may own.
        if (now >= clock.deadline || ++polls >= kPollLimit) return end(Result::Timeout);
        t->pause();
    }
}
} // namespace pcc_probe
