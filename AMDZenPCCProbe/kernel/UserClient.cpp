#include "AMDZenPCCProbe.hpp"
#include <IOKit/IOLib.h>
#include <string.h>

OSDefineMetaClassAndStructors(AMDZenPCCProbeUserClient, IOUserClient)
bool AMDZenPCCProbeUserClient::initWithTask(task_t task, void *security, UInt32 type,
                                           OSDictionary *properties) {
    if (type != 0 || !IOUserClient::initWithTask(task, security, type, properties)) return false;
    // Kernel authorization is independent of argv, euid checks, and the acknowledgement token.
    admin_ = clientHasPrivilege(security, kIOClientPrivilegeAdministrator) == kIOReturnSuccess;
    return true;
}
bool AMDZenPCCProbeUserClient::start(IOService *provider) {
    owner_ = OSDynamicCast(AMDZenPCCProbe, provider);
    if (!owner_) return false;
    owner_->retain();
    return IOUserClient::start(provider);
}
void AMDZenPCCProbeUserClient::stop(IOService *provider) {
    if (owner_) owner_->closeClient(state_); else state_.close();
    IOUserClient::stop(provider);
}
void AMDZenPCCProbeUserClient::free() {
    if (owner_) { owner_->release(); owner_ = nullptr; }
    IOUserClient::free();
}
IOReturn AMDZenPCCProbeUserClient::clientClose() {
    // Share the reply-commit mutex: closure cannot race between cancellation
    // validation and commitment of a successful metadata response.
    if (owner_) owner_->closeClient(state_); else state_.close();
    if (!isInactive()) terminate();
    return kIOReturnSuccess;
}
IOReturn AMDZenPCCProbeUserClient::clientDied() { return clientClose(); }
IOReturn AMDZenPCCProbeUserClient::externalMethod(uint32_t selector, IOExternalMethodArguments *args,
                                                 IOExternalMethodDispatch *, OSObject *, void *) {
    using namespace pcc_probe;
    if (!args || !args->structureInput || !args->structureOutput ||
        !validateWireSizes(selector, args->structureInputSize, args->structureOutputSize,
                           args->scalarInputCount, args->scalarOutputCount,
                           args->structureInputDescriptor != nullptr || args->structureOutputDescriptor != nullptr,
                           args->asyncReferenceCount != 0 || args->asyncWakePort != MACH_PORT_NULL))
        return kIOReturnBadArgument;
    Request request{};
    memcpy(&request, args->structureInput, sizeof(request));
    if (validateRequest(selector, request) != Result::Ok) return kIOReturnBadArgument;
    if (selector == static_cast<uint32_t>(Selector::ReadOnce) && !admin_) return kIOReturnNotPrivileged;
    if (!owner_ || isInactive() || state_.closed()) return kIOReturnNotAttached;
    retain(); // Together with owner's lifetime retain, protects concurrent close/stop paths.
    auto *reply = static_cast<Reply *>(IOMallocZero(sizeof(Reply)));
    if (!reply) { release(); return kIOReturnNoMemory; }
    owner_->perform(static_cast<Selector>(selector), request, admin_, state_, *reply);
    memcpy(args->structureOutput, reply, sizeof(*reply));
    args->structureOutputSize = sizeof(*reply);
    IOFree(reply, sizeof(*reply));
    release();
    return kIOReturnSuccess;
}
