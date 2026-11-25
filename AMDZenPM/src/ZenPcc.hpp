// Kernel CPPC transport, one polled generic PCC subspace (ordinal 0).
//
// discover(apicIdByXnuCpu,ncpu,failReason): once per Channel lifetime; evaluates
// static _CPC/_PSD, joins ACPI IDs -> MADT APIC IDs -> XNU indexes, maps UC MMIO.
// It does not read MMIO or issue firmware commands. failReason/lastError use
// zenpcc::Error numeric values. readCaps/readState each issue one synchronous
// READ. enableAutonomous requires desiredPerf=0 and stages requests + AUTO_SEL=1
// + ENABLE=1 for all CPUs, then one WRITE. apply stages changed CPUs' four request
// fields, then one WRITE (no changes = no command). Arrays are XNU-CPU indexed;
// their count must exactly match discovery and callers keep them stable during
// the call. Every CPU written requires a successful readCaps first; enable needs
// all CPUs' caps. Engine rejects invalid requests before any command-path I/O.
//
// ALL methods except diagnostic getters require thread context with interrupts
// and preemption enabled. NEVER call from pmDispatch hooks. Commands block on an
// IOLock and poll/sleep; no allocation occurs in command paths. Firmware timeout,
// error, access/protocol failure permanently disables commands on this object.
// Completion waits up to min(500 * nominal latency, 2 s), with a 1-ms floor.
// A timed-out WRITE may have been applied; never retry or assume firmware state.
// Teardown serializes with commands and only unmaps; it never disables CPPC or
// rings a doorbell. Stop/drain the policy before teardown, especially for sleep;
// a Channel cannot be rediscovered/rearmed after teardown. Destroy only after
// callers have drained. Only one Channel may claim subspace 0 per kext lifetime;
// teardown retires that global claim after unmapping, without rearming it.
// Getters publish atomic diagnostic snapshots.
#ifndef ZEN_PCC_HPP
#define ZEN_PCC_HPP
#include "ZenPccModel.hpp"
#include <IOKit/IOLocks.h>
#include <IOKit/IOMemoryDescriptor.h>

namespace zenpcc {
class Channel {
public:
    Channel() = default;
    ~Channel();
    Channel(const Channel &) = delete;
    Channel &operator=(const Channel &) = delete;
    bool discover(const uint16_t *apicIdByXnuCpu, uint32_t ncpu, uint32_t *failReason);
    bool readCaps(uint32_t cpu, zencppc::Caps *out);
    bool readState(uint32_t cpu, uint32_t *enable, uint32_t *autonomy, zencppc::Request *cur);
    bool enableAutonomous(const zencppc::Request *perCpu, uint32_t ncpu);
    bool apply(const zencppc::Request *perCpu, const bool *changed, uint32_t ncpu);
    void teardown();
    uint32_t lastError() const;
    uint64_t commandCount() const;
    uint64_t lastLatencyUs() const;
private:
    class MappedAccessor final : public Accessor {
    public:
        bool read16(uint32_t offset, uint16_t &value) override;
        bool write16(uint32_t offset, uint16_t value) override;
        bool read32(uint32_t offset, uint32_t &value) override;
        bool write32(uint32_t offset, uint32_t value) override;
        bool read64(uint64_t &value) override;
        bool write64(uint64_t value) override;
        void barrier() override;
        uint64_t nowUs() override;
        void waitUs(uint32_t us) override;
        volatile uint8_t *shared = nullptr;
        volatile uint64_t *doorbell = nullptr;
        uint32_t bytes = 0;
    };
    bool discoverLocked(const uint16_t *apicIds, uint32_t ncpu);
    bool map(const Descriptor &d);
    bool enter();
    void publish();
    bool reject(Error error);
    void unmapLocked();
    IOLock *lock_ = nullptr;
    IOMemoryDescriptor *sharedMemory_ = nullptr, *doorbellMemory_ = nullptr;
    IOMemoryMap *sharedMap_ = nullptr, *doorbellMap_ = nullptr;
    bool attempted_ = false, discovered_ = false, claimed_ = false;
    CpuRegisters cpus_[kMaxCpus]{};
    MappedAccessor accessor_;
    Engine engine_;
    uint32_t error_ = uint32_t(Error::Ok);
    uint64_t commands_ = 0, latency_ = 0;
};
} // namespace zenpcc
#endif
