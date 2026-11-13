//
// AMDZenPMUserClient.cpp — classic IOExternalMethodDispatch user client.
//
// Read selectors (GetInfo / GetSample / GetTunables) are open to any user; setters
// require an administrator client (decided once in initWithTask). Struct outputs
// larger than the inline limit (4096 bytes, e.g. AMDZenSample) arrive as an
// out-of-line memory descriptor, so both delivery paths are handled.
//
#include "AMDZenPM.hpp"
#include <IOKit/IOLib.h>
#include <IOKit/IOMemoryDescriptor.h>
#include <string.h>

OSDefineMetaClassAndStructors(AMDZenPMUserClient, IOUserClient)

namespace {

IOReturn writeStructOut(IOExternalMethodArguments *args, const void *buf, uint32_t size) {
    if (IOMemoryDescriptor *md = args->structureOutputDescriptor) {
        if (args->structureOutputDescriptorSize < size || md->getLength() < size)
            return kIOReturnBadArgument;
        IOReturn r = md->prepare();
        if (r != kIOReturnSuccess) return r;
        IOByteCount n = md->writeBytes(0, buf, size);
        md->complete();
        if (n != size) return kIOReturnIOError;
        args->structureOutputDescriptorSize = size;
        return kIOReturnSuccess;
    }
    if (!args->structureOutput || args->structureOutputSize < size) return kIOReturnBadArgument;
    memcpy(args->structureOutput, buf, size);
    args->structureOutputSize = size;
    return kIOReturnSuccess;
}

IOReturn readStructIn(IOExternalMethodArguments *args, void *buf, uint32_t size) {
    if (IOMemoryDescriptor *md = args->structureInputDescriptor) {
        if (md->getLength() != size) return kIOReturnBadArgument;
        IOReturn r = md->prepare();
        if (r != kIOReturnSuccess) return r;
        IOByteCount n = md->readBytes(0, buf, size);
        md->complete();
        return n == size ? kIOReturnSuccess : kIOReturnIOError;
    }
    if (!args->structureInput || args->structureInputSize != size) return kIOReturnBadArgument;
    memcpy(buf, args->structureInput, size);
    return kIOReturnSuccess;
}

// Scalars arrive as uint64_t; reject anything that would be truncated.
inline bool scalar32(const IOExternalMethodArguments *args, uint32_t i, uint32_t *out) {
    if (i >= args->scalarInputCount) return false;
    uint64_t v = args->scalarInput[i];
    if (v > 0xFFFFFFFFull) return false;
    *out = (uint32_t)v;
    return true;
}

inline AMDZenPM *ownerOf(OSObject *target) { return OSDynamicCast(AMDZenPM, target); }

template <typename T, IOReturn (AMDZenPM::*Copy)(T *)>
IOReturn copyOut(OSObject *target, IOExternalMethodArguments *args) {
    AMDZenPM *pm = ownerOf(target);
    if (!pm) return kIOReturnNotAttached;
    T *buf = static_cast<T *>(IOMallocZero(sizeof(T)));
    if (!buf) return kIOReturnNoMemory;
    IOReturn r = (pm->*Copy)(buf);
    if (r == kIOReturnSuccess) r = writeStructOut(args, buf, (uint32_t)sizeof(T));
    IOFree(buf, sizeof(T));
    return r;
}

IOReturn sGetInfo(OSObject *target, void *, IOExternalMethodArguments *args) {
    return copyOut<AMDZenInfo, &AMDZenPM::copyInfo>(target, args);
}
IOReturn sGetSample(OSObject *target, void *, IOExternalMethodArguments *args) {
    return copyOut<AMDZenSample, &AMDZenPM::copySample>(target, args);
}
IOReturn sGetTunables(OSObject *target, void *, IOExternalMethodArguments *args) {
    return copyOut<AMDZenTunables, &AMDZenPM::copyTunables>(target, args);
}

IOReturn sSetGovernor(OSObject *target, void *, IOExternalMethodArguments *args) {
    AMDZenPM *pm = ownerOf(target);
    uint32_t v;
    if (!pm) return kIOReturnNotAttached;
    if (!scalar32(args, 0, &v)) return kIOReturnBadArgument;
    return pm->setGovernor(v);
}
IOReturn sSetBoost(OSObject *target, void *, IOExternalMethodArguments *args) {
    AMDZenPM *pm = ownerOf(target);
    uint32_t v;
    if (!pm) return kIOReturnNotAttached;
    if (!scalar32(args, 0, &v) || v > 1) return kIOReturnBadArgument;
    return pm->setBoost(v != 0);
}
IOReturn sSetPerfLimits(OSObject *target, void *, IOExternalMethodArguments *args) {
    AMDZenPM *pm = ownerOf(target);
    uint32_t a, b;
    if (!pm) return kIOReturnNotAttached;
    if (!scalar32(args, 0, &a) || !scalar32(args, 1, &b)) return kIOReturnBadArgument;
    return pm->setPerfLimits(a, b);
}
IOReturn sSetEPP(OSObject *target, void *, IOExternalMethodArguments *args) {
    AMDZenPM *pm = ownerOf(target);
    uint32_t v;
    if (!pm) return kIOReturnNotAttached;
    if (!scalar32(args, 0, &v)) return kIOReturnBadArgument;
    return pm->setEPP(v);
}
IOReturn sSetUserPState(OSObject *target, void *, IOExternalMethodArguments *args) {
    AMDZenPM *pm = ownerOf(target);
    uint32_t v;
    if (!pm) return kIOReturnNotAttached;
    if (!scalar32(args, 0, &v)) return kIOReturnBadArgument;
    return pm->setUserPState(v);
}
IOReturn sSetTunables(OSObject *target, void *, IOExternalMethodArguments *args) {
    AMDZenPM *pm = ownerOf(target);
    if (!pm) return kIOReturnNotAttached;
    AMDZenTunables t;
    IOReturn r = readStructIn(args, &t, (uint32_t)sizeof(t));
    if (r != kIOReturnSuccess) return r;
    return pm->setTunables(&t);
}
IOReturn sWritePStateDef(OSObject *target, void *, IOExternalMethodArguments *args) {
    AMDZenPM *pm = ownerOf(target);
    uint32_t idx, fid, did, vid;
    if (!pm) return kIOReturnNotAttached;
    if (!scalar32(args, 0, &idx) || !scalar32(args, 1, &fid) || !scalar32(args, 2, &did) ||
        !scalar32(args, 3, &vid))
        return kIOReturnBadArgument;
    return pm->writePStateDef(idx, fid, did, vid);
}

// {function, scalarIn, structIn, scalarOut, structOut}
const IOExternalMethodDispatch sMethods[kAMDZenMethodCount] = {
    /* GetInfo */ { sGetInfo,        0, 0, 0, kIOUCVariableStructureSize },
    /* GetSample */ { sGetSample,      0, 0, 0, kIOUCVariableStructureSize },
    /* SetGovernor */ { sSetGovernor,    1, 0, 0, 0 },
    /* SetBoost */ { sSetBoost,       1, 0, 0, 0 },
    /* SetPerfLimits */ { sSetPerfLimits,  2, 0, 0, 0 },
    /* SetEPP */ { sSetEPP,         1, 0, 0, 0 },
    /* SetUserPState */ { sSetUserPState,  1, 0, 0, 0 },
    /* SetTunables */ { sSetTunables,    0, (uint32_t)sizeof(AMDZenTunables), 0, 0 },
    /* GetTunables */ { sGetTunables,    0, 0, 0, kIOUCVariableStructureSize },
    /* WritePStateDef */ { sWritePStateDef, 4, 0, 0, 0 },
};

const bool sAdminOnly[kAMDZenMethodCount] = {
    /* GetInfo */ false,
    /* GetSample */ false,
    /* SetGovernor */ true,
    /* SetBoost */ true,
    /* SetPerfLimits */ true,
    /* SetEPP */ true,
    /* SetUserPState */ true,
    /* SetTunables */ true,
    /* GetTunables */ false,
    /* WritePStateDef */ true,
};

} // namespace

bool AMDZenPMUserClient::initWithTask(task_t owningTask, void *securityToken, UInt32 type,
                                      OSDictionary *properties) {
    if (!IOUserClient::initWithTask(owningTask, securityToken, type, properties)) return false;
    admin = clientHasPrivilege(securityToken, kIOClientPrivilegeAdministrator) == kIOReturnSuccess;
    return true;
}

bool AMDZenPMUserClient::start(IOService *provider) {
    owner = OSDynamicCast(AMDZenPM, provider);
    if (!owner) return false;
    return IOUserClient::start(provider);
}

IOReturn AMDZenPMUserClient::clientClose() {
    owner = nullptr;
    if (!isInactive()) terminate();
    return kIOReturnSuccess;
}

IOReturn AMDZenPMUserClient::externalMethod(uint32_t selector, IOExternalMethodArguments *args,
                                            IOExternalMethodDispatch *dispatch, OSObject *target,
                                            void *reference) {
    (void)dispatch; (void)target;
    if (selector >= kAMDZenMethodCount) return kIOReturnBadArgument;
    if (sAdminOnly[selector] && !admin) return kIOReturnNotPrivileged;
    AMDZenPM *pm = owner;
    if (!pm || isInactive()) return kIOReturnNotAttached;
    return IOUserClient::externalMethod(selector, args,
                                        const_cast<IOExternalMethodDispatch *>(&sMethods[selector]),
                                        pm, reference);
}
