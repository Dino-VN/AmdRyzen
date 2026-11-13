//
// ZenHW.cpp — hardware access layer for AMDZenPM.
//
//   zen::SMN  serialized System Management Network reads through the index/data
//             pair (0x60/0x64) of the bus0/dev0/fn0 root complex.
//   zen::CPU  CPUID identification, topology and MSR capability probing
//             (discover), and per-CPU MSR sampling / control (run) executed with
//             mp_rendezvous_no_intrs.
//
// Rendezvous actions in this file run with interrupts disabled on every logical
// CPU at once: they only touch their own slot of pre-allocated arrays, never
// lock, allocate or log. Writes are only issued to MSRs that discover() has
// validated with rdmsr_carefully on every CPU (there is no exported
// wrmsr_carefully; an invalid WRMSR would #GP and panic).
//
// Register facts: see ZenRegs.h and docs/research/linux-{sensors,pstate}.md.
//

#include "ZenHW.hpp"
#include "ZenRegs.h"
#include "XnuPrivate.h"

#include <IOKit/IOService.h>
#include <IOKit/pci/IOPCIDevice.h>
#include <libkern/OSTypes.h>
#include <string.h>

#define ZLOG(fmt, ...) IOLog("AMDZenPM: " fmt "\n", ##__VA_ARGS__)

namespace zen {

namespace {

// ---------------------------------------------------------------- okMsr bits
// One bit per MSR (group) that read successfully on every logical CPU.
enum : uint32_t {
    kOkPerfArch     = 1u << 0,  // MPERF/APERF 0xE7/0xE8
    kOkPerfRo       = 1u << 1,  // MPERF/APERF read-only aliases C000_00E7/E8
    kOkIrperf       = 1u << 2,  // C000_00E9
    kOkHwcr         = 1u << 3,  // C001_0015
    kOkPatch        = 1u << 4,  // 0x8B
    kOkCurLimit     = 1u << 5,  // C001_0061
    kOkPstateCtl    = 1u << 6,  // C001_0062
    kOkPstateStatus = 1u << 7,  // C001_0063
    kOkHwPstStatus  = 1u << 8,  // C001_0293
    kOkRaplUnit     = 1u << 9,  // C001_0299
    kOkPkgEnergy    = 1u << 10, // C001_029B
    kOkCoreEnergy   = 1u << 11, // C001_029A
    kOkCppcCap1     = 1u << 12, // C001_02B0
    kOkCppcReq      = 1u << 13, // C001_02B3
    kOkCppcEnable   = 1u << 14, // C001_02B1
    kOkPpin         = 1u << 15, // C001_02F0 enabled + C001_02F1 read (CPU 0 only)
    kOkDefShift     = 24,       // bits 24..31: PStateDef[n] readable
    kOkDefMask      = 0xFFu << kOkDefShift,
};

static inline uint32_t okDef(uint32_t n) { return 1u << (kOkDefShift + n); }

static const uint32_t kLegacyCore = kOkCurLimit | kOkPstateCtl | kOkPstateStatus;
static const uint32_t kCppcCore   = kOkCppcCap1 | kOkCppcReq | kOkCppcEnable;

// ---------------------------------------------------------------- helpers
static inline uint32_t orderOf(uint32_t n) {   // smallest k with (1 << k) >= n
    uint32_t k = 0;
    while (k < 31 && (1u << k) < n) k++;
    return k;
}

static inline bool rdOk(uint32_t msr, uint64_t *v) {
    return zen_rdmsr_safe(msr, v) == 0;
}

static void copyStr(char *dst, size_t cap, const char *src) {
    if (!dst || cap == 0) return;
    size_t i = 0;
    if (src) {
        for (; i + 1 < cap && src[i]; i++) dst[i] = src[i];
    }
    dst[i] = '\0';
}

static bool regsEqual(const uint32_t r[3], const char *s12) {
    // r = {EBX, EDX, ECX} of CPUID 0, compared with a 12 char vendor string
    for (uint32_t i = 0; i < 12; i++) {
        uint8_t c = (uint8_t)(r[i / 4] >> (8 * (i % 4)));
        if (c != (uint8_t)s12[i]) return false;
    }
    return true;
}

// Sorted-unique insertion into a small array; returns the index of `key`.
static uint32_t uniqInsert(uint32_t *arr, uint32_t *count, uint32_t key) {
    uint32_t n = *count, pos = 0;
    while (pos < n && arr[pos] < key) pos++;
    if (pos < n && arr[pos] == key) return pos;
    for (uint32_t j = n; j > pos; j--) arr[j] = arr[j - 1];
    arr[pos] = key;
    *count = n + 1;
    return pos;
}

static uint32_t uniqFind(const uint32_t *arr, uint32_t count, uint32_t key) {
    uint32_t lo = 0, hi = count;
    while (lo < hi) {
        uint32_t mid = (lo + hi) / 2;
        if (arr[mid] < key) lo = mid + 1; else hi = mid;
    }
    return lo;
}

// ---------------------------------------------------------------- discover
struct DiscCpu {
    uint32_t seen;
    uint32_t ok;          // kOk* bits that read OK on this CPU
    uint32_t apic;        // x2APIC id (leaf 0xB) or extended APIC id
    uint32_t smtShift;
    uint32_t llcShift;    // APIC-id shift that identifies the L3 domain (CCX)
    uint32_t threads;     // threads per core as reported by CPUID
    uint32_t nodes;       // 8000_001E ECX[10:8] + 1
    uint32_t pad;
    uint64_t hwcr;
    uint64_t pstateCtl;
    uint64_t cppcCap1;
    uint64_t cppcReq;
};

struct DiscScratch {
    DiscCpu  cpu[AMDZEN_MAX_CPUS];
    uint32_t uniq[AMDZEN_MAX_CPUS];
    uint32_t ranks[AMDZEN_MAX_CORES];
};

struct DiscCtx {
    DiscCpu *cpu;
    uint32_t limit;       // slots in cpu[]
    uint32_t want;        // kOk* bits to probe (HWCR and PATCH always)
    bool     fixedLlc3;   // Zen1 / Hygon: llc id = apic >> 3
    // package-scope values captured on logical CPU 0
    uint64_t defs[AMDZEN_MAX_PSTATES];
    uint64_t curLimit, raplUnit, patch, ppin;
    bool     ppinOk;
};

static void readTopology(DiscCpu *d, bool fixedLlc3) {
    uint32_t r[4];
    zen_cpuid(0, 0, r);
    uint32_t maxStd = r[0];
    zen_cpuid(0x80000000u, 0, r);
    uint32_t maxExt = r[0];
    zen_cpuid(0x80000001u, 0, r);
    bool topoext = (r[2] & CPUID81_ECX_TOPOEXT) != 0;

    zen_cpuid(1, 0, r);
    uint32_t apic = r[1] >> 24;
    uint32_t smt = 0, threads = 1, nodes = 1;
    bool haveSmt = false;

    if (maxStd >= 0xB) {
        zen_cpuid(0xB, 0, r);
        // valid SMT level: non-zero processor count and level type 1 (SMT)
        if ((r[1] & 0xFFFFu) != 0 && ((r[2] >> 8) & 0xFFu) == 1) {
            apic = r[3];
            smt = r[0] & 0x1Fu;
            threads = r[1] & 0xFFFFu;
            haveSmt = true;
        }
    }
    if (topoext && maxExt >= 0x8000001Eu) {
        zen_cpuid(0x8000001Eu, 0, r);
        threads = ((r[1] >> 8) & 0xFFu) + 1;
        nodes = ((r[2] >> 8) & 0x7u) + 1;
        if (!haveSmt) {
            apic = r[0];
            smt = orderOf(threads);
            haveSmt = true;
        }
    }

    uint32_t llc = 31;    // unknown: everything in one domain
    if (fixedLlc3) {
        llc = 3;
    } else if (topoext && maxExt >= 0x8000001Du) {
        for (uint32_t sub = 0; sub < 16; sub++) {
            zen_cpuid(0x8000001Du, sub, r);
            uint32_t type = r[0] & 0x1Fu;
            if (type == 0) break;
            if (((r[0] >> 5) & 0x7u) == 3) {
                llc = orderOf(((r[0] >> 14) & 0xFFFu) + 1);
                break;
            }
        }
    }

    d->apic = apic;
    d->smtShift = smt;
    d->llcShift = llc;
    d->threads = threads ? threads : 1;
    d->nodes = nodes;
}

// Runs on every CPU with interrupts off. Read-only: CPUID + rdmsr_carefully.
static void discoverAction(void *arg) {
    DiscCtx *ctx = static_cast<DiscCtx *>(arg);
    int c = cpu_number();
    if (c < 0 || (uint32_t)c >= ctx->limit) return;
    DiscCpu *d = &ctx->cpu[c];
    const bool cpu0 = (c == 0);
    const uint32_t want = ctx->want;
    uint32_t ok = 0;
    uint64_t v = 0, v2 = 0;

    readTopology(d, ctx->fixedLlc3);

    if ((want & kOkPerfRo) && rdOk(MSR_MPERF_RO, &v) && rdOk(MSR_APERF_RO, &v2)) ok |= kOkPerfRo;
    if ((want & kOkPerfArch) && rdOk(MSR_MPERF, &v) && rdOk(MSR_APERF, &v2)) ok |= kOkPerfArch;
    if ((want & kOkIrperf) && rdOk(MSR_IRPERF, &v)) ok |= kOkIrperf;

    if (rdOk(MSR_HWCR, &v)) { ok |= kOkHwcr; d->hwcr = v; }
    if (rdOk(MSR_PATCH_LEVEL, &v)) { ok |= kOkPatch; if (cpu0) ctx->patch = v; }

    if (want & kOkCurLimit) {
        if (rdOk(MSR_PSTATE_CUR_LIMIT, &v)) { ok |= kOkCurLimit; if (cpu0) ctx->curLimit = v; }
        if (rdOk(MSR_PSTATE_CTL, &v)) { ok |= kOkPstateCtl; d->pstateCtl = v; }
        if (rdOk(MSR_PSTATE_STATUS, &v)) ok |= kOkPstateStatus;
        for (uint32_t n = 0; n < AMDZEN_MAX_PSTATES; n++) {
            if (rdOk(MSR_PSTATE_DEF_BASE + n, &v)) {
                ok |= okDef(n);
                if (cpu0) ctx->defs[n] = v;
            }
        }
    }
    if ((want & kOkHwPstStatus) && rdOk(MSR_HW_PSTATE_STATUS, &v)) ok |= kOkHwPstStatus;

    if (want & kOkRaplUnit) {
        if (rdOk(MSR_RAPL_PWR_UNIT, &v)) { ok |= kOkRaplUnit; if (cpu0) ctx->raplUnit = v; }
        if (rdOk(MSR_PKG_ENERGY_STAT, &v)) ok |= kOkPkgEnergy;
        if (rdOk(MSR_CORE_ENERGY_STAT, &v)) ok |= kOkCoreEnergy;
    }

    if (want & kOkCppcCap1) {
        if (rdOk(MSR_CPPC_CAP1, &v)) { ok |= kOkCppcCap1; d->cppcCap1 = v; }
        if (rdOk(MSR_CPPC_REQ, &v)) { ok |= kOkCppcReq; d->cppcReq = v; }
        if (rdOk(MSR_CPPC_ENABLE, &v)) ok |= kOkCppcEnable;
    }

    // PPIN: package scope, read on CPU 0 only. Never write PPIN_CTL: use it only
    // when firmware already enabled it (bit1) and it is not locked-disabled.
    if (cpu0 && (want & kOkPpin) && rdOk(MSR_PPIN_CTL, &v)) {
        if ((v & 3u) != 1u && (v & 2u) && rdOk(MSR_PPIN, &v2)) {
            ctx->ppin = v2;
            ctx->ppinOk = true;
        }
    }

    d->ok = ok;
    d->seen = 1;
}

// ---------------------------------------------------------------- run
struct RunCtx {
    uint32_t ops;
    uint32_t ncpu;
    uint32_t ok;
    uint32_t pstateMax;
    uint32_t msrA, msrM;
    bool     perf, irperf, legacy, cppc, coreEnergy, hwPst, defWrite;
    PerCpuRaw      *out;
    const uint8_t  *pstateReq;
    const uint64_t *cppcReq;
    uint64_t hwcrSet, hwcrClear;
    uint32_t defMsr;
    uint64_t defValue;
    uint64_t *ctlCache;
    uint64_t *reqCache;
    const Topology *topo;
};

// Runs on every CPU with interrupts off. Only validated MSRs are accessed.
static void runAction(void *arg) {
    RunCtx *ctx = static_cast<RunCtx *>(arg);
    int c = cpu_number();
    if (c < 0 || (uint32_t)c >= ctx->ncpu) return;
    const uint32_t i = (uint32_t)c;
    const uint32_t ops = ctx->ops;
    bool ctlPreValid = false;
    uint64_t ctlPre = 0;

    // 1. HWCR read-modify-write (bits pre-filtered to an allow-list by run()).
    if (ops & kOpHwcrModify) {
        uint64_t v = zen_rdmsr_raw(MSR_HWCR);
        uint64_t nv = (v & ~ctx->hwcrClear) | ctx->hwcrSet;
        if (nv != v) zen_wrmsr_raw(MSR_HWCR, nv);
    }

    // 2. P-state definition (validated index/value by run()).
    if (ops & kOpWritePStateDef) {
        zen_wrmsr_raw(ctx->defMsr, ctx->defValue);
    }

    // 3. P-state request: RMW of PstateCmd[2:0], only when it differs from cache.
    if ((ops & kOpWritePState) && ctx->pstateReq) {
        uint32_t req = ctx->pstateReq[i];
        if (req <= ctx->pstateMax) {
            uint64_t cached = ctx->ctlCache[i];
            if (cached == UINT64_MAX || (uint32_t)(cached & 7u) != req) {
                uint64_t cur = zen_rdmsr_raw(MSR_PSTATE_CTL);
                ctlPre = cur;
                ctlPreValid = true;
                uint64_t nv = (cur & ~7ull) | req;
                if (nv != cur) zen_wrmsr_raw(MSR_PSTATE_CTL, nv);
                ctx->ctlCache[i] = nv;
            }
        }
    }

    // 4. CPPC enable (write-once until reset), after the P0 request above as in
    //    amd-pstate's per-CPU init (PSTATE_CTL = 0, then CPPC_ENABLE = 1).
    if (ops & kOpCppcEnable) {
        if ((zen_rdmsr_raw(MSR_CPPC_ENABLE) & 1u) == 0) zen_wrmsr_raw(MSR_CPPC_ENABLE, 1);
    }

    // 5. CPPC request, only when changed (UINT64_MAX = leave this CPU alone).
    if ((ops & kOpWriteCppcReq) && ctx->cppcReq) {
        uint64_t v = ctx->cppcReq[i];
        if (v != UINT64_MAX && v != ctx->reqCache[i]) {
            zen_wrmsr_raw(MSR_CPPC_REQ, v);
            ctx->reqCache[i] = v;
        }
    }

    if (!ctx->out) return;
    PerCpuRaw *o = &ctx->out[i];

    // 6. Fast sample: MPERF then APERF back to back, then TSC.
    if (ops & kOpSamplePerf) {
        uint64_t m = 0, a = 0;
        if (ctx->perf) {
            m = zen_rdmsr_raw(ctx->msrM);
            a = zen_rdmsr_raw(ctx->msrA);
        }
        uint64_t t = zen_rdtsc();
        o->perf.mperf = m;
        o->perf.aperf = a;
        o->perf.tsc = t;
        o->perf.irperf = ctx->irperf ? zen_rdmsr_raw(MSR_IRPERF) : 0;
        o->pstateStatus = (ctx->ok & kOkPstateStatus) ? zen_rdmsr_raw(MSR_PSTATE_STATUS) : 0;
        if (ctlPreValid) {
            o->pstateCtl = ctlPre;
        } else if (ctx->ok & kOkPstateCtl) {
            uint64_t v = zen_rdmsr_raw(MSR_PSTATE_CTL);
            o->pstateCtl = v;
            ctx->ctlCache[i] = v;        // keep the cache in sync with hardware
        } else {
            o->pstateCtl = 0;
        }
    }

    // 7. Slow sample: per-core counters on the primary thread only, HWCR on all.
    if (ops & kOpSampleSlow) {
        uint32_t core = ctx->topo->coreOfCpu[i];
        bool primary = core < AMDZEN_MAX_CORES && ctx->topo->primaryCpuOfCore[core] == i;
        o->coreEnergy = (primary && ctx->coreEnergy)
                        ? (zen_rdmsr_raw(MSR_CORE_ENERGY_STAT) & 0xFFFFFFFFull) : 0;
        o->hwPstateStatus = (primary && ctx->hwPst) ? zen_rdmsr_raw(MSR_HW_PSTATE_STATUS) : 0;
        o->hwcr = (ctx->ok & kOkHwcr) ? zen_rdmsr_raw(MSR_HWCR) : 0;
    }
}

// ---------------------------------------------------------------- idle MSRs
struct IdleCtx {
    IdleMsrSample *out;
    uint32_t limit;
};

// Runs on every CPU with interrupts off. Read-only.
static void idleMsrAction(void *arg) {
    IdleCtx *ctx = static_cast<IdleCtx *>(arg);
    int c = cpu_number();
    if (c < 0 || (uint32_t)c >= ctx->limit) return;
    IdleMsrSample *o = &ctx->out[c];
    uint64_t v = 0;
    bool ok = true;
    if (rdOk(MSR_CSTATE_BASE_ADDR, &v)) o->base = v; else ok = false;
    if (rdOk(MSR_CSTATE_CONFIG, &v)) o->cfg = v; else ok = false;
    if (rdOk(MSR_HWCR, &v)) { o->hwcr = v; o->hwcrOk = 1; }
    if (rdOk(MSR_CSTATE_POLICY, &v)) { o->policy = v; o->policyOk = 1; }
    o->ok = ok ? 1 : 0;
}

} // anonymous namespace

// ==================================================================== SMN

static IOPCIDevice *findRootComplex() {
    OSDictionary *match = IOService::serviceMatching("IOPCIDevice");
    if (!match) return nullptr;
    IOPCIDevice *found = nullptr;
    OSIterator *it = IOService::getMatchingServices(match);
    match->release();
    if (!it) return nullptr;
    while (OSObject *o = it->getNextObject()) {
        IOPCIDevice *d = OSDynamicCast(IOPCIDevice, o);
        if (!d) continue;
        if (d->getBusNumber() != 0 || d->getDeviceNumber() != 0 || d->getFunctionNumber() != 0)
            continue;
        uint16_t ven = d->configRead16(kIOPCIConfigVendorID);
        if (ven != AMD_VENDOR_ID && ven != HYGON_VENDOR_ID) continue;
        uint32_t classRev = d->configRead32(kIOPCIConfigRevisionID);
        if ((classRev >> 16) != 0x0600u) continue;         // host bridge
        d->retain();
        found = d;
        break;
    }
    it->release();
    return found;
}

bool SMN::init(uint32_t timeoutMs) {
    if (root) return true;
    if (!lock) {
        lock = IOLockAlloc();
        if (!lock) return false;
    }
    uint32_t waited = 0;
    for (;;) {
        IOPCIDevice *dev = findRootComplex();
        if (dev) {
            rootDevId = dev->configRead16(kIOPCIConfigDeviceID);
            root = dev;
            ZLOG("SMN root complex %04x:%04x", dev->configRead16(kIOPCIConfigVendorID), rootDevId);
            return true;
        }
        if (waited >= timeoutMs) break;
        uint32_t step = timeoutMs - waited;
        if (step > 100) step = 100;
        IOSleep(step);
        waited += step;
    }
    ZLOG("SMN root complex 00:00.0 not found after %u ms", timeoutMs);
    return false;
}

void SMN::free() {
    if (lock) IOLockLock(lock);
    IOPCIDevice *r = root;
    root = nullptr;
    rootDevId = 0;
    if (lock) IOLockUnlock(lock);
    OSSafeReleaseNULL(r);
    if (lock) {
        IOLockFree(lock);
        lock = nullptr;
    }
}

bool SMN::read(uint32_t addr, uint32_t *val) {
    if (!val) return false;
    *val = 0;
    if (!root || !lock) return false;
    IOLockLock(lock);
    root->configWrite32(PCI_ROOT_SMN_INDEX, addr);
    uint32_t v = root->configRead32(PCI_ROOT_SMN_DATA);
    IOLockUnlock(lock);
    if (v == 0xFFFFFFFFu) return false;
    *val = v;
    return true;
}

// ==================================================================== CPU

bool CPU::discover(AMDZenInfo *info, CpuModelInfo *model) {
    if (!info || !model) return false;

    // ---- reset state
    bzero(&topology, sizeof(topology));
    topology.threadsPerCore = 1;
    topology.numCcx = 1;
    feat = 0;
    tsc = 0;
    haveCoreEnergy = haveHwPstateStatus = haveIrperf = false;
    haveCppc = haveLegacy = useRoPerf = false;
    okMsr = 0;
    pstateMax = 0;
    missing = 0;
    hwcrBoot = 0;
    for (uint32_t i = 0; i < AMDZEN_MAX_CPUS; i++) {
        cppcReqBoot[i] = 0;
        pstateCtlCache[i] = UINT64_MAX;
        cppcReqCache[i] = UINT64_MAX;
    }
    bzero(model, sizeof(*model));
    model->codename = "Unknown";

    info->abiVersion = AMDZEN_ABI_VERSION;
    info->structSize = sizeof(AMDZenInfo);
    bzero(info->brand, sizeof(info->brand));
    bzero(info->codename, sizeof(info->codename));
    bzero(info->pstates, sizeof(info->pstates));
    bzero(info->apicId, sizeof(info->apicId));
    bzero(info->coreOfCpu, sizeof(info->coreOfCpu));
    bzero(info->cppcRank, sizeof(info->cppcRank));
    info->numPStates = info->pstateMaxVal = 0;
    info->baseFreqMHz = info->maxBoostFreqMHz = 0;
    info->cppcHighest = info->cppcNominal = info->cppcLowestNonlinear = info->cppcLowest = 0;
    info->cppcEffectiveHighest = 0;
    info->microcode = 0;
    info->ppin = 0;
    info->energyUnitShift = 0;

    // ---- identity (current CPU)
    uint32_t r[4];
    zen_cpuid(0, 0, r);
    const uint32_t maxStd = r[0];
    const uint32_t vend[3] = { r[1], r[3], r[2] };
    Vendor vendor = kVendorOther;
    if (regsEqual(vend, "AuthenticAMD")) vendor = kVendorAMD;
    else if (regsEqual(vend, "HygonGenuine")) vendor = kVendorHygon;

    zen_cpuid(0x80000000u, 0, r);
    const uint32_t maxExt = (r[0] >= 0x80000000u) ? r[0] : 0;

    char brand[52];
    bzero(brand, sizeof(brand));
    if (maxExt >= 0x80000004u) {
        for (uint32_t l = 0; l < 3; l++) {
            zen_cpuid(0x80000002u + l, 0, r);
            for (uint32_t k = 0; k < 16; k++)
                brand[l * 16 + k] = (char)(r[k / 4] >> (8 * (k % 4)));
        }
    }
    const char *b = brand;
    while (*b == ' ') b++;
    size_t blen = 0;
    while (b[blen]) blen++;
    while (blen > 0 && (b[blen - 1] == ' ' || b[blen - 1] == '\0')) blen--;
    copyStr(info->brand, (blen + 1 < sizeof(info->brand)) ? blen + 1 : sizeof(info->brand), b);

    zen_cpuid(1, 0, r);
    uint32_t fam = 0, mdl = 0, step = 0;
    decodeFms(r[0], &fam, &mdl, &step);
    const uint32_t ecx1 = r[2];
    info->vendor = (uint32_t)vendor;
    info->family = fam;
    info->model = mdl;
    info->stepping = step;

    bool supported = (vendor != kVendorOther) && identify(vendor, fam, mdl, info->brand, model);
    if (!model->codename) model->codename = "Unknown";
    copyStr(info->codename, sizeof(info->codename), model->codename);
    info->zenGeneration = model->zenGeneration;
    if (!supported) {
        ZLOG("unsupported CPU: vendor %u family %xh model %xh \"%s\"", (uint32_t)vendor, fam, mdl,
             info->brand);
        return false;
    }

    // ---- CPUID features
    if (ecx1 & CPUID1_ECX_MWAIT) feat |= AMDZEN_F_MWAIT;
    if (maxStd >= 6) {
        zen_cpuid(6, 0, r);
        if (r[2] & CPUID6_ECX_EFFFREQ) feat |= AMDZEN_F_APERFMPERF;
    }
    if (maxExt >= 0x80000001u) {
        zen_cpuid(0x80000001u, 0, r);
        if (r[2] & CPUID81_ECX_TOPOEXT) feat |= AMDZEN_F_TOPOEXT;
    }
    if (maxExt >= 0x80000007u) {
        zen_cpuid(0x80000007u, 0, r);
        const uint32_t edx = r[3];
        if (edx & CPUID87_EDX_HWPSTATE)      feat |= AMDZEN_F_HWPSTATE;
        if (edx & CPUID87_EDX_TSC_INVARIANT) feat |= AMDZEN_F_TSC_INVARIANT;
        if (edx & CPUID87_EDX_CPB)           feat |= AMDZEN_F_CPB;
        if (edx & CPUID87_EDX_EFFFREQ_RO)    feat |= AMDZEN_F_EFFFREQ_RO;
        if (edx & CPUID87_EDX_RAPL)          feat |= AMDZEN_F_RAPL;
        if (edx & CPUID87_EDX_FAST_CPPC)     feat |= AMDZEN_F_FAST_CPPC;
    }
    bool cpuidIrperf = false, cpuidPpin = false;
    if (maxExt >= 0x80000008u) {
        zen_cpuid(0x80000008u, 0, r);
        cpuidIrperf = (r[1] & CPUID88_EBX_IRPERF) != 0;
        cpuidPpin = (r[1] & CPUID88_EBX_PPIN) != 0;
        if (r[1] & CPUID88_EBX_CPPC) feat |= AMDZEN_F_CPPC;
    }
    const bool zen1 = (vendor == kVendorAMD && fam == 0x17 && mdl < 0x30);
    if (zen1) {
        feat |= AMDZEN_F_CPB;      // erratum 1076: CPB missing from CPUID
        cpuidIrperf = false;       // erratum 1054: IRPERF unreliable
    }

    zen_tscInfo ti;
    bzero(&ti, sizeof(ti));
    tsc_get_info(&ti);
    tsc = ti.tscFreq;

    // ---- logical CPU count
    uint32_t ncpu = real_ncpus;
    if (ncpu == 0) ncpu = 1;
    if (ncpu > AMDZEN_MAX_CPUS) {
        ZLOG("%u logical CPUs, only the first %u are managed", ncpu, AMDZEN_MAX_CPUS);
        ncpu = AMDZEN_MAX_CPUS;
    }

    DiscScratch *s = static_cast<DiscScratch *>(IOMallocZero(sizeof(DiscScratch)));
    if (!s) {
        ZLOG("discover: out of memory");
        return false;
    }

    // ---- one rendezvous: topology + MSR probes on every CPU
    DiscCtx ctx;
    bzero(&ctx, sizeof(ctx));
    ctx.cpu = s->cpu;
    ctx.limit = ncpu;
    ctx.fixedLlc3 = (vendor == kVendorAMD && fam == 0x17 && mdl <= 0x1F) || vendor == kVendorHygon;
    uint32_t want = kOkHwcr | kOkPatch;
    if (feat & AMDZEN_F_EFFFREQ_RO) want |= kOkPerfRo;
    if (feat & AMDZEN_F_APERFMPERF) want |= kOkPerfArch;
    if (cpuidIrperf)                want |= kOkIrperf;
    if (feat & AMDZEN_F_HWPSTATE)   want |= kLegacyCore | kOkDefMask;
    if (fam == 0x17 || fam == 0x18 || fam == 0x19) want |= kOkHwPstStatus;
    if (feat & AMDZEN_F_RAPL)       want |= kOkRaplUnit | kOkPkgEnergy | kOkCoreEnergy;
    if (feat & AMDZEN_F_CPPC)       want |= kCppcCore;
    if (cpuidPpin)                  want |= kOkPpin;
    ctx.want = want;

    mp_rendezvous_no_intrs(discoverAction, &ctx);

    uint32_t all = want & ~kOkPpin;
    uint32_t nMissing = 0;
    for (uint32_t i = 0; i < ncpu; i++) {
        if (!s->cpu[i].seen) { nMissing++; all = 0; continue; }
        all &= s->cpu[i].ok;
    }
    if (nMissing) ZLOG("discover: %u CPUs did not answer the rendezvous", nMissing);
    missing = nMissing;
    if (ctx.ppinOk) all |= kOkPpin;
    okMsr = all;
    const DiscCpu &c0 = s->cpu[0];

    // ---- topology: dense cores sorted by (apic >> smtShift)
    {
        uint32_t smt = c0.smtShift > 8 ? 8 : c0.smtShift;
        uint32_t llc = c0.llcShift > 31 ? 31 : c0.llcShift;
        uint32_t ncoreKeys = 0;
        for (uint32_t i = 0; i < ncpu; i++)
            uniqInsert(s->uniq, &ncoreKeys, s->cpu[i].apic >> smt);
        uint32_t ncores = ncoreKeys;
        if (ncores > AMDZEN_MAX_CORES) {
            ZLOG("%u physical cores, folding the excess into core %u", ncores, AMDZEN_MAX_CORES - 1);
            ncores = AMDZEN_MAX_CORES;
        }
        uint16_t perCore[AMDZEN_MAX_CORES];
        bzero(perCore, sizeof(perCore));
        for (uint32_t c = 0; c < AMDZEN_MAX_CORES; c++) topology.primaryCpuOfCore[c] = 0xFFFF;
        for (uint32_t i = 0; i < ncpu; i++) {
            uint32_t core = uniqFind(s->uniq, ncoreKeys, s->cpu[i].apic >> smt);
            if (core >= ncores) core = ncores - 1;
            topology.apicId[i] = (uint16_t)s->cpu[i].apic;
            topology.coreOfCpu[i] = (uint16_t)core;
            if (topology.primaryCpuOfCore[core] == 0xFFFF) topology.primaryCpuOfCore[core] = (uint16_t)i;
            perCore[core]++;
        }
        uint32_t tpc = 1;
        for (uint32_t c = 0; c < ncores; c++) if (perCore[c] > tpc) tpc = perCore[c];
        for (uint32_t c = ncores; c < AMDZEN_MAX_CORES; c++) topology.primaryCpuOfCore[c] = 0;

        // CCX = L3 sharing domain, dense ids sorted by (apic >> llcShift)
        uint32_t nccx = 0;
        for (uint32_t c = 0; c < ncores; c++)
            uniqInsert(s->uniq, &nccx, s->cpu[topology.primaryCpuOfCore[c]].apic >> llc);
        for (uint32_t c = 0; c < ncores; c++)
            topology.ccxOfCore[c] = (uint16_t)uniqFind(s->uniq, nccx,
                                        s->cpu[topology.primaryCpuOfCore[c]].apic >> llc);

        topology.ncpu = ncpu;
        topology.ncores = ncores;
        topology.threadsPerCore = tpc;
        topology.smtShift = smt;
        topology.numCcx = nccx ? nccx : 1;
    }

    // ---- capabilities (a capability = MSR readable on all CPUs)
    useRoPerf = (all & kOkPerfRo) != 0;
    bool havePerf = useRoPerf || (all & kOkPerfArch);
    if (havePerf) feat |= AMDZEN_F_APERFMPERF; else feat &= ~AMDZEN_F_APERFMPERF;

    haveIrperf = cpuidIrperf && (all & kOkIrperf) && (all & kOkHwcr);
    if (haveIrperf) {
        bool enabledEverywhere = true;
        for (uint32_t i = 0; i < ncpu; i++)
            if (!(s->cpu[i].hwcr & HWCR_IRPERF_EN)) enabledEverywhere = false;
        if (enabledEverywhere) feat |= AMDZEN_F_IRPERF;
    }

    haveLegacy = (feat & AMDZEN_F_HWPSTATE) && (all & kLegacyCore) == kLegacyCore && (all & okDef(0));
    if (!haveLegacy) feat &= ~AMDZEN_F_HWPSTATE;

    haveHwPstateStatus = (all & kOkHwPstStatus) != 0;
    if (haveHwPstateStatus) feat |= AMDZEN_F_HWPSTATUS;

    if ((feat & AMDZEN_F_RAPL) && (all & (kOkRaplUnit | kOkPkgEnergy)) != (kOkRaplUnit | kOkPkgEnergy))
        feat &= ~AMDZEN_F_RAPL;
    haveCoreEnergy = (feat & AMDZEN_F_RAPL) && (all & kOkCoreEnergy);
    if (haveCoreEnergy) feat |= AMDZEN_F_CORE_ENERGY;

    haveCppc = (feat & AMDZEN_F_CPPC) && (all & kCppcCore) == kCppcCore;
    if (!haveCppc) feat &= ~AMDZEN_F_CPPC;

    if (all & kOkPpin) { feat |= AMDZEN_F_PPIN; info->ppin = ctx.ppin; }

    // ---- info: identity / counts
    if (all & kOkPatch) info->microcode = (uint32_t)ctx.patch;
    info->tscHz = tsc;
    info->logicalCpus = topology.ncpu;
    info->physicalCores = topology.ncores;
    info->threadsPerCore = topology.threadsPerCore;
    info->numCcx = topology.numCcx;
    info->nodes = c0.nodes ? c0.nodes : 1;
    info->tjMax_mC = model->defaultTjMax_mC;
    for (uint32_t i = 0; i < topology.ncpu; i++) {
        info->apicId[i] = topology.apicId[i];
        info->coreOfCpu[i] = topology.coreOfCpu[i];
    }

    // ---- P-states
    if (haveLegacy) {
        pstateMax = pstateMaxVal(ctx.curLimit);
        info->pstateMaxVal = pstateMax;
        info->numPStates = pstateMax + 1;
        for (uint32_t n = 0; n <= pstateMax && n < AMDZEN_MAX_PSTATES; n++) {
            if (!(all & okDef(n))) continue;
            PStateDecoded d = decodePStateDef(ctx.defs[n], model->pstateDef1Ah, model->sviVidFormula);
            AMDZenPStateDef &p = info->pstates[n];
            p.enabled = d.enabled ? 1 : 0;
            p.freqMHz = d.freqMHz;
            p.fid = d.fid;
            p.did = d.did;
            p.vid = d.vid;
            p.voltage_mV = d.voltage_mV;
            p.iddValue = d.iddValue;
            p.iddDiv = d.iddDiv;
            p.raw = ctx.defs[n];
        }
        info->baseFreqMHz = info->pstates[0].freqMHz;
    }
    if (info->baseFreqMHz == 0 && tsc) info->baseFreqMHz = (uint32_t)((tsc + 500000) / 1000000);

    // ---- RAPL
    if (feat & AMDZEN_F_RAPL) info->energyUnitShift = (uint32_t)((ctx.raplUnit >> 8) & 0x1F);

    // ---- CPPC
    if (haveCppc) {
        CppcCaps caps = decodeCppcCap1(c0.cppcCap1);
        info->cppcHighest = caps.highest;
        info->cppcNominal = caps.nominal;
        info->cppcLowestNonlinear = caps.lowestNonlinear;
        info->cppcLowest = caps.lowest;
        for (uint32_t c = 0; c < topology.ncores; c++) {
            uint32_t h = decodeCppcCap1(s->cpu[topology.primaryCpuOfCore[c]].cppcCap1).highest;
            s->ranks[c] = h;
            info->cppcRank[c] = (uint8_t)h;
        }
        bool prefcore = false;
        info->cppcEffectiveHighest = cppcEffectiveHighest(s->ranks, topology.ncores, &prefcore);
        if (prefcore) feat |= AMDZEN_F_PREFCORE;
        if (caps.nominal && info->baseFreqMHz)
            info->maxBoostFreqMHz = cppcPerfToFreq(info->cppcEffectiveHighest, caps.nominal,
                                                   info->baseFreqMHz);
    }

    // ---- boot values and write caches
    if (all & kOkHwcr) {
        hwcrBoot = c0.hwcr;
        for (uint32_t i = 1; i < ncpu; i++) {
            if ((s->cpu[i].hwcr ^ hwcrBoot) & (HWCR_CPB_DIS | HWCR_IRPERF_EN)) {
                ZLOG("HWCR differs between CPU0 (0x%llx) and CPU%u (0x%llx)", hwcrBoot, i, s->cpu[i].hwcr);
                break;
            }
        }
    }
    for (uint32_t i = 0; i < ncpu; i++) {
        if (haveCppc) {
            cppcReqBoot[i] = s->cpu[i].cppcReq;
            cppcReqCache[i] = s->cpu[i].cppcReq;
        }
        if (all & kOkPstateCtl) pstateCtlCache[i] = s->cpu[i].pstateCtl;
    }

    info->features = feat;

    ZLOG("%s (%s, fam %xh model %xh step %u, Zen%u) ucode 0x%x",
         info->brand, info->codename, fam, mdl, step, info->zenGeneration, info->microcode);
    ZLOG("%u CPUs / %u cores / %u CCX, SMT %u, TSC %llu Hz, features 0x%llx, msr 0x%08x",
         topology.ncpu, topology.ncores, topology.numCcx, topology.threadsPerCore, tsc, feat, okMsr);
    if (haveLegacy)
        ZLOG("P-states P0..P%u, P0 %u MHz, HWCR 0x%llx", pstateMax, info->baseFreqMHz, hwcrBoot);
    if (haveCppc)
        ZLOG("CPPC highest %u (eff %u) nominal %u lnl %u lowest %u, max boost %u MHz",
             info->cppcHighest, info->cppcEffectiveHighest, info->cppcNominal,
             info->cppcLowestNonlinear, info->cppcLowest, info->maxBoostFreqMHz);

    IOFree(s, sizeof(DiscScratch));
    return true;
}

void CPU::run(uint32_t ops, PerCpuRaw *out, const uint8_t *pstateReq, const uint64_t *cppcReq,
              uint64_t hwcrSet, uint64_t hwcrClear, uint32_t defIndex, uint64_t defValue) {
    if (topology.ncpu == 0 || ops == 0) return;

    RunCtx ctx;
    bzero(&ctx, sizeof(ctx));
    uint32_t eff = 0;

    if (ops & kOpHwcrModify) {
        // Only boost-disable and IRPERF-enable may ever be changed.
        uint64_t allowed = 0;
        if (feat & AMDZEN_F_CPB) allowed |= HWCR_CPB_DIS;
        if (haveIrperf) allowed |= HWCR_IRPERF_EN;
        hwcrSet &= allowed;
        hwcrClear &= allowed & ~hwcrSet;
        if ((okMsr & kOkHwcr) && (hwcrSet | hwcrClear)) {
            eff |= kOpHwcrModify;
            ctx.hwcrSet = hwcrSet;
            ctx.hwcrClear = hwcrClear;
        }
    }
    if ((ops & kOpCppcEnable) && haveCppc) eff |= kOpCppcEnable;
    if ((ops & kOpWritePStateDef) && haveLegacy && defIndex < AMDZEN_MAX_PSTATES &&
        defIndex <= pstateMax && (okMsr & okDef(defIndex)) && (defValue >> 63)) {
        eff |= kOpWritePStateDef;
        ctx.defMsr = MSR_PSTATE_DEF_BASE + defIndex;
        ctx.defValue = defValue;
    }
    if ((ops & kOpWritePState) && haveLegacy && pstateReq) eff |= kOpWritePState;
    if ((ops & kOpWriteCppcReq) && haveCppc && cppcReq) eff |= kOpWriteCppcReq;
    if (out) eff |= ops & (kOpSamplePerf | kOpSampleSlow);
    if (eff == 0) return;

    ctx.ops = eff;
    ctx.ncpu = topology.ncpu;
    ctx.ok = okMsr;
    ctx.pstateMax = pstateMax;
    ctx.perf = (feat & AMDZEN_F_APERFMPERF) != 0;
    ctx.msrA = useRoPerf ? MSR_APERF_RO : MSR_APERF;
    ctx.msrM = useRoPerf ? MSR_MPERF_RO : MSR_MPERF;
    ctx.irperf = haveIrperf;
    ctx.legacy = haveLegacy;
    ctx.cppc = haveCppc;
    ctx.coreEnergy = haveCoreEnergy;
    ctx.hwPst = haveHwPstateStatus;
    ctx.out = out;
    ctx.pstateReq = pstateReq;
    ctx.cppcReq = cppcReq;
    ctx.ctlCache = pstateCtlCache;
    ctx.reqCache = cppcReqCache;
    ctx.topo = &topology;

    mp_rendezvous_no_intrs(runAction, &ctx);

    if ((eff & kOpHwcrModify) && haveIrperf) {
        if (ctx.hwcrSet & HWCR_IRPERF_EN) feat |= AMDZEN_F_IRPERF;
        if (ctx.hwcrClear & HWCR_IRPERF_EN) feat &= ~AMDZEN_F_IRPERF;
    }
}

bool CPU::readPkgEnergy(uint32_t *raw) {
    if (!raw || !(okMsr & kOkPkgEnergy)) return false;
    uint64_t v;
    if (zen_rdmsr_safe(MSR_PKG_ENERGY_STAT, &v) != 0) return false;
    *raw = (uint32_t)v;
    return true;
}

bool CPU::readCurLimit(uint64_t *raw) {
    if (!raw || !(okMsr & kOkCurLimit)) return false;
    return zen_rdmsr_safe(MSR_PSTATE_CUR_LIMIT, raw) == 0;
}

bool CPU::readPStateDef(uint32_t idx, uint64_t *raw) {
    if (!raw || idx >= AMDZEN_MAX_PSTATES || !(okMsr & okDef(idx))) return false;
    return zen_rdmsr_safe(MSR_PSTATE_DEF_BASE + idx, raw) == 0;
}

uint32_t CPU::readIdleMsrs(IdleMsrSample *out) {
    if (!out || topology.ncpu == 0) return 0;
    bzero(out, sizeof(IdleMsrSample) * AMDZEN_MAX_CPUS);
    IdleCtx ctx { out, topology.ncpu };
    mp_rendezvous_no_intrs(idleMsrAction, &ctx);
    return topology.ncpu;
}

void CPU::invalidateWriteCache() {
    for (uint32_t i = 0; i < AMDZEN_MAX_CPUS; i++) {
        pstateCtlCache[i] = UINT64_MAX;
        cppcReqCache[i] = UINT64_MAX;
    }
}

} // namespace zen
