// Symbol-resolution probe: references every symbol the project might want so
// kextlibs / kmutil can report which KPI bundle resolves each (or that none does).
// Built with -Xlinker -undefined -Xlinker dynamic_lookup semantics (kexts are
// always linked with undefineds left for the kernel linker). Never loaded.
#include <IOKit/IOService.h>
#include <IOKit/IOLib.h>
#include <IOKit/IOTimerEventSource.h>
#include <IOKit/IOWorkLoop.h>
#include <IOKit/pci/IOPCIDevice.h>
#include <kern/thread_call.h>
#include <libkern/OSKextLib.h>
#include <sys/sysctl.h>
#include "xnu_private.h"

extern "C" {
// not exported -- declared here only to provoke an unresolved symbol
int wrmsr_carefully(uint32_t msr, uint64_t val);
extern uint64_t tscFreq;
void *cpuid_info(void);
int kernel_sysctlbyname(const char *, void *, size_t *, void *, size_t);
unsigned int ml_get_max_cpus(void);
}

class SymProbe : public IOService {
    OSDeclareDefaultStructors(SymProbe)
public:
    bool start(IOService *provider) override;
};
OSDefineMetaClassAndStructors(SymProbe, IOService)

static void nop(void *) {}
static void tcfn(thread_call_param_t, thread_call_param_t) {}

__attribute__((used)) static volatile uintptr_t gRefs[] = {
    (uintptr_t)&mp_rendezvous_no_intrs,
    (uintptr_t)&mp_rendezvous,
    (uintptr_t)&mp_cpus_call,
    (uintptr_t)&cpu_number,
    (uintptr_t)&rdmsr_carefully,
    (uintptr_t)&wrmsr_carefully,
    (uintptr_t)&real_ncpus,
    (uintptr_t)&tscFreq,
    (uintptr_t)&cpuid_info,
    (uintptr_t)&pmKextRegister,
    (uintptr_t)&kernel_sysctlbyname,
    (uintptr_t)&ml_get_max_cpus,
    (uintptr_t)&sysctl_register_oid,
    (uintptr_t)&OSKextGetCurrentIdentifier,
    (uintptr_t)&IOSleep,
    (uintptr_t)&thread_call_allocate,
    (uintptr_t)&thread_call_allocate_with_options,
    (uintptr_t)&thread_call_enter_delayed,
    (uintptr_t)&thread_call_cancel,
    (uintptr_t)&thread_call_free,
    (uintptr_t)&tsc_get_info,
    (uintptr_t)&mp_broadcast,
};

bool SymProbe::start(IOService *provider) {
    if (!IOService::start(provider)) return false;
    // non-virtual-call reference to IOPCIDevice::configRead32 (qualified call
    // bypasses the vtable so the symbol is really imported)
    if (IOPCIDevice *pci = OSDynamicCast(IOPCIDevice, provider)) {
        uint32_t v = pci->IOPCIDevice::configRead32((UInt8)0);
        pci->IOPCIDevice::configWrite32((UInt8)0x60, v);
        (void)pci->IOPCIDevice::extendedConfigRead32(0x100);
    }
    IOTimerEventSource *t = IOTimerEventSource::timerEventSource(this, nullptr);
    if (t) t->release();
    thread_call_t tc = thread_call_allocate(tcfn, nullptr);
    if (tc) thread_call_free(tc);
    mp_rendezvous_no_intrs(nop, nullptr);
    return gRefs[0] != 0;
}
