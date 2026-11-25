#ifndef AMDZEN_PCC_KERNEL_HPP
#define AMDZEN_PCC_KERNEL_HPP
#include <IOKit/IOService.h>
#include <IOKit/IOUserClient.h>
#include <IOKit/IOLocks.h>
#include "ReadEngine.hpp"

class AMDZenPCCProbe : public IOService {
    OSDeclareDefaultStructors(AMDZenPCCProbe)
public:
    bool start(IOService *provider) override;
    void stop(IOService *provider) override;
    void free() override;
    IOReturn setProperties(OSObject *) override;
    IOReturn newUserClient(task_t task, void *security, UInt32 type, OSDictionary *properties,
                          IOUserClient **handler) override;
    // Serialized with the service's final reply-commit, stop and power boundaries.
    void closeClient(pcc_probe::ClientState &client);
    void perform(pcc_probe::Selector selector, const pcc_probe::Request &request, bool admin,
                 const pcc_probe::ClientState &client, pcc_probe::Reply &reply);
private:
    static IOReturn powerMessage(void *, void *, UInt32, IOService *, void *, vm_size_t);
    void drain();
    IOLock *drainLock_ = nullptr;
    IONotifier *powerNotifier_ = nullptr;
    pcc_probe::Control control_;
    bool bootGate_ = false, configGate_ = false;
};
class AMDZenPCCProbeUserClient : public IOUserClient {
    OSDeclareDefaultStructors(AMDZenPCCProbeUserClient)
public:
    bool initWithTask(task_t task, void *security, UInt32 type, OSDictionary *properties) override;
    bool start(IOService *provider) override;
    void stop(IOService *provider) override;
    void free() override;
    IOReturn clientClose() override;
    IOReturn clientDied() override;
    IOReturn externalMethod(uint32_t selector, IOExternalMethodArguments *arguments,
                           IOExternalMethodDispatch *dispatch = nullptr,
                           OSObject *target = nullptr, void *reference = nullptr) override;
private:
    AMDZenPCCProbe *owner_ = nullptr; // Retained until free, never cleared by a racing close.
    pcc_probe::ClientState state_;
    bool admin_ = false;
};
#endif
