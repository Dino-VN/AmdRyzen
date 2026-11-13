// Build probe: minimal IOService + IOUserClient. Compile/link-check only; never loaded.
#ifndef AMDCPU_PROBE_HPP
#define AMDCPU_PROBE_HPP

#include <IOKit/IOService.h>
#include <IOKit/IOUserClient.h>
#include <IOKit/IOWorkLoop.h>
#include <IOKit/IOTimerEventSource.h>
#include <IOKit/IOLocks.h>
#include <IOKit/pci/IOPCIDevice.h>

#define AMDCPU_MAX_CPUS 64

class AmdCpuProbe : public IOService {
    OSDeclareDefaultStructors(AmdCpuProbe)
public:
    bool start(IOService *provider) override;
    void stop(IOService *provider) override;
    IOReturn setProperties(OSObject *properties) override;

    // SMN access through the root complex index/data pair (D0F0 0x60/0x64)
    bool smnRead(uint32_t addr, uint32_t *value);

    uint64_t lastAperf[AMDCPU_MAX_CPUS];
    uint64_t lastMperf[AMDCPU_MAX_CPUS];

private:
    IOPCIDevice *findRootComplex();
    void sample(IOTimerEventSource *src);

    IOWorkLoop         *workLoop  {nullptr};
    IOTimerEventSource *timer     {nullptr};
    IOPCIDevice        *rootPci   {nullptr};
    IOLock             *smnLock   {nullptr};
};

class AmdCpuProbeUserClient : public IOUserClient {
    OSDeclareDefaultStructors(AmdCpuProbeUserClient)
public:
    bool initWithTask(task_t owningTask, void *securityToken, UInt32 type,
                      OSDictionary *properties) override;
    bool start(IOService *provider) override;
    IOReturn clientClose() override;
    IOReturn externalMethod(uint32_t selector, IOExternalMethodArguments *args,
                            IOExternalMethodDispatch *dispatch, OSObject *target,
                            void *reference) override;
private:
    static IOReturn sReadSmn(OSObject *target, void *ref, IOExternalMethodArguments *args);
    static const IOExternalMethodDispatch sMethods[1];
    AmdCpuProbe *owner {nullptr};
};

#endif
