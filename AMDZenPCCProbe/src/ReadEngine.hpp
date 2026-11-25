#ifndef AMDZEN_PCC_READ_ENGINE_HPP
#define AMDZEN_PCC_READ_ENGINE_HPP
#include "Profile.hpp"

namespace pcc_probe {
class ClientState {
public:
    void close() { __atomic_store_n(&closed_, 1u, __ATOMIC_RELEASE); }
    bool closed() const { return __atomic_load_n(&closed_, __ATOMIC_ACQUIRE) != 0; }
private:
    uint32_t closed_ = 0;
};

// One per service, NOT one per user client. Only tryEnter/leave admit operations.
// Atomics coordinate cancellation; they never protect a blocking transport call.
// The kernel adapter uses a sleep mutex/condition to drain ActiveCall on stop.
class Control {
public:
    Result tryEnter(const ClientState &client);
    void leave();
    void stop();
    void powerChanged(); // Terminal for this loaded instance; wake NEVER re-arms it.
    void capabilitiesChanged(uint32_t from, uint32_t to, uint32_t cpuCapability);
    Result cancellation(const ClientState &client) const;
    uint32_t flags() const { return __atomic_load_n(&flags_, __ATOMIC_ACQUIRE); }
    bool reserveAttempt();
    void mayOwn();
    void submitted();
    void completed();
    void quarantine();
    Control() = default;
    Control(const Control &) = delete;
    Control &operator=(const Control &) = delete;
private:
    uint32_t flags_ = 0;
};
struct Header { uint32_t signature; uint16_t command, status; };

// Transport contract, not an arbitrary physical-access API. NOT exported to
// userspace. This release constructs NO kernel implementation of this interface.
// A future binding needs an independently established exclusive OS PCC lease,
// UC mappings, exact-width device accesses, barriers, and a power/teardown fence.
// Firmware's Complete bit alone is not exclusion against another OSPM consumer.
class ReadTransport {
public:
    virtual ~ReadTransport() = default;
    virtual bool claim(const cppc_lab::ChannelDescriptor &) = 0;
    virtual void releaseMapping() = 0; // Never reset/release FIRMWARE ownership.
    virtual uint64_t nowUs() = 0;
    virtual void pause() = 0; // Bounded thread-context wait; no spinlocked callback.
    virtual bool readHeader(Header &) = 0; // 32/16/16-bit accesses at 0/4/6.
    virtual bool readDoorbell(uint64_t &) = 0; // Validated GAS width only.
    // beginCommit must exclude power/stop/close transitions until endCommit.
    virtual bool beginCommit(const Control &, const ClientState &) = 0;
    virtual void endCommit() = 0;
    virtual bool writeReadCommand() = 0; // One 16-bit zero at offset 4.
    virtual bool clearStatus() = 0;      // One 16-bit zero at offset 6.
    virtual void deviceBarrier() = 0;
    virtual bool writeDoorbell(uint64_t maskedValue) = 0;
    virtual bool readField(uint64_t offset, uint8_t bytes, uint64_t &) = 0;
};

// Caller owns ActiveCall and holds the SAME sleep mutex used by stop, power
// invalidation and client closure. Cancellation checking and reply commitment
// are one linearization point. Returns a snapshot as of that point, then leaves.
Result completeCall(const Request &request, Result result, Control &control,
                    const ClientState &client, Reply &reply);

// Caller holds Control's ActiveCall throughout metadata acquisition and this call.
// Null transport is the production safety boundary: gates and diagnostics work,
// but the ownership/power-management blocker cannot be bypassed via configuration.
Result readOnce(const Request &request, bool admin, bool bootGate, bool configGate,
                const Workspace &workspace, Control &control, const ClientState &client,
                ReadTransport *transport, Reply &reply);
} // namespace pcc_probe
#endif
