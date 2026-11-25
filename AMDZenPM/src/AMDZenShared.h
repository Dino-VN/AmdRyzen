//
// AMDZenShared.h — ABI shared by AMDZenPM.kext, AMDZenSMC.kext and the zenctl CLI.
//
// Plain C, fixed-width types only, no kernel or libc dependencies beyond <stdint.h>.
// Every struct starts with {abiVersion, structSize}; bump AMDZEN_ABI_VERSION on any
// layout change. All temperatures are milli-degrees C, energies micro-joules,
// powers milli-watts, voltages milli-volts, currents milli-amps, frequencies MHz.
//
#ifndef AMDZEN_SHARED_H
#define AMDZEN_SHARED_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define AMDZEN_ABI_VERSION        1u
#define AMDZEN_SERVICE_CLASS      "AMDZenPM"
#define AMDZEN_BUNDLE_ID          "com.dinovn.AMDZenPM"

#define AMDZEN_MAX_CPUS           256u   // logical CPUs
#define AMDZEN_MAX_CORES          128u   // physical cores
#define AMDZEN_MAX_CCDS           16u
#define AMDZEN_MAX_PSTATES        8u

// ---------------------------------------------------------------------------
// IOUserClient selectors (IOConnectCall*Method). "R" = any user, "A" = admin only.
// ---------------------------------------------------------------------------
enum AMDZenMethod {
    kAMDZenMethodGetInfo        = 0, // R  structOut  AMDZenInfo
    kAMDZenMethodGetSample      = 1, // R  structOut  AMDZenSample (latest cached sample)
    kAMDZenMethodSetGovernor    = 2, // A  scalarIn[0]=AMDZenGovernor
    kAMDZenMethodSetBoost       = 3, // A  scalarIn[0]=0 off / 1 on
    kAMDZenMethodSetPerfLimits  = 4, // A  scalarIn[0]=fastest allowed P-state idx, [1]=slowest allowed idx
                                     //    (CPPC backend: [0]=minPerf, [1]=maxPerf, 0..255; 0 = default)
    kAMDZenMethodSetEPP         = 5, // A  scalarIn[0]=EPP 0..255 (CPPC backend only)
    kAMDZenMethodSetUserPState  = 6, // A  scalarIn[0]=P-state idx (legacy) or desired perf (CPPC) for kGovUserspace
    kAMDZenMethodSetTunables    = 7, // A  structIn AMDZenTunables
    kAMDZenMethodGetTunables    = 8, // R  structOut AMDZenTunables
    kAMDZenMethodWritePStateDef = 9, // A  scalarIn[0]=idx, [1]=FID, [2]=DID, [3]=VID. Requires boot-arg
                                     //    amdzen.pstatewrite=1, family 17h/19h SVI2 parts only.
    kAMDZenMethodCount
};

// ---------------------------------------------------------------------------
// Governors
// ---------------------------------------------------------------------------
enum AMDZenGovernor {
    kAMDZenGovNone          = 0, // monitor only: never write any control MSR
    kAMDZenGovPerformance   = 1, // legacy: all threads request P0; CPPC: min=nominal, max=highest, EPP 0
    kAMDZenGovPowersave     = 2, // legacy: slowest allowed P-state, boost untouched; CPPC: min=lowest, max=lowest_nonlinear, des=0, EPP 0xFF
    kAMDZenGovSchedutil     = 3, // util-driven, next = 1.25 * fmax * util (legacy and CPPC passive)
    kAMDZenGovOndemand      = 4, // load > up_threshold -> fastest; else proportional
    kAMDZenGovConservative  = 5, // step by freq_step% with up/down thresholds
    kAMDZenGovUserspace     = 6, // fixed P-state / desired perf set by kAMDZenMethodSetUserPState
    kAMDZenGovCppcActive    = 7, // CPPC autonomous (amd-pstate-epp "active"): des=0, firmware picks freq per EPP
    kAMDZenGovCount
};

enum AMDZenBackend {
    kAMDZenBackendNone    = 0, // no control possible (monitor only)
    kAMDZenBackendLegacy  = 1, // HW P-states via MSR C001_0062 + HWCR.CpbDis
    kAMDZenBackendCppcMsr = 2, // CPPC MSRs C001_02B0..B4 (CPUID 8000_0008 EBX[27])
    kAMDZenBackendCppcPcc = 3, // autonomous ACPI CPPC through a serialized PCC channel
};

// Why control is disabled (AMDZenInfo.controlBlockReason)
enum AMDZenBlockReason {
    kAMDZenBlockNone          = 0,
    kAMDZenBlockBootArgRO     = 1, // -amdzenro
    kAMDZenBlockConflict      = 2, // another AMD PM kext (AMDRyzenCPUPowerManagement, SMCAMDProcessor...) is loaded
    kAMDZenBlockUnsupported   = 3, // CPU has no usable control interface
    kAMDZenBlockHypervisor    = 4, // running under a hypervisor (CPUID 1 ECX[31])
    kAMDZenBlockPccFailed     = 5, // CPPC may be enabled; quarantine writes until reset
};

// Feature bits (AMDZenInfo.features)
#define AMDZEN_F_APERFMPERF     (1ull << 0)  // CPUID 6 ECX[0]
#define AMDZEN_F_EFFFREQ_RO     (1ull << 1)  // 8000_0007 EDX[10] (C000_00E7/E8)
#define AMDZEN_F_HWPSTATE       (1ull << 2)  // 8000_0007 EDX[7]
#define AMDZEN_F_TSC_INVARIANT  (1ull << 3)  // 8000_0007 EDX[8]
#define AMDZEN_F_CPB            (1ull << 4)  // 8000_0007 EDX[9] (forced on Zen1)
#define AMDZEN_F_RAPL           (1ull << 5)  // 8000_0007 EDX[14]
#define AMDZEN_F_CORE_ENERGY    (1ull << 6)  // C001_029A readable (probed)
#define AMDZEN_F_CPPC           (1ull << 7)  // 8000_0008 EBX[27]
#define AMDZEN_F_IRPERF         (1ull << 8)  // 8000_0008 EBX[1] and counter enabled
#define AMDZEN_F_PPIN           (1ull << 9)  // 8000_0008 EBX[23] and enabled
#define AMDZEN_F_SMN            (1ull << 10) // PCI root complex found, SMN reads OK
#define AMDZEN_F_TCTL           (1ull << 11) // Tctl readable
#define AMDZEN_F_CCD_TEMP       (1ull << 12) // >=1 CCD temp sensor valid
#define AMDZEN_F_SVI2           (1ull << 13) // SVI2 telemetry decode known for this model
#define AMDZEN_F_HWPSTATUS      (1ull << 14) // C001_0293 readable (per-core VID/FID)
#define AMDZEN_F_MWAIT          (1ull << 15) // CPUID 1 ECX[3]
#define AMDZEN_F_TOPOEXT        (1ull << 16) // 8000_0001 ECX[22]
#define AMDZEN_F_FAST_CPPC      (1ull << 17) // 8000_0007 EDX[15]
#define AMDZEN_F_PREFCORE       (1ull << 18) // CPPC highest_perf differs between cores
#define AMDZEN_F_PSTATE_WRITE   (1ull << 19) // P-state definition editing unlocked by boot-arg

typedef struct AMDZenPStateDef {
    uint32_t enabled;       // PstateEn (bit 63)
    uint32_t freqMHz;       // core COF
    uint32_t fid, did, vid; // raw fields (did = 0 on 1Ah)
    uint32_t voltage_mV;    // decoded VID (SVI2 formula on 17h/19h; 0 if unknown)
    uint32_t iddValue, iddDiv;
    uint64_t raw;           // raw MSR C001_0064+n
} AMDZenPStateDef;

typedef struct AMDZenInfo {
    uint32_t abiVersion;            // AMDZEN_ABI_VERSION
    uint32_t structSize;            // sizeof(AMDZenInfo)
    char     brand[64];             // CPUID 8000_0002..4
    char     codename[32];          // e.g. "Vermeer"
    char     kextVersion[16];
    uint32_t vendor;                // 0 = AMD, 1 = Hygon
    uint32_t family, model, stepping;
    uint32_t zenGeneration;         // 1..6, 0 unknown
    uint32_t microcode;             // MSR 0x8B low 32 bits
    uint64_t ppin;                  // 0 if unavailable
    uint64_t features;              // AMDZEN_F_*
    uint64_t tscHz;
    uint32_t logicalCpus, physicalCores, threadsPerCore, numCcx;
    uint32_t nodes;
    uint32_t ccdTempMask;           // bit i = Tccd(i) present
    uint32_t backend;               // AMDZenBackend
    uint32_t controlBlockReason;    // AMDZenBlockReason (0 = control allowed)
    uint32_t energyUnitShift;       // RAPL ESU
    uint32_t numPStates;            // entries valid in pstates[] (0..PstateMaxVal)
    uint32_t pstateMaxVal;          // C001_0061[6:4]
    uint32_t baseFreqMHz;           // P0 / nominal
    uint32_t maxBoostFreqMHz;       // CPPC highest or 0 when unknown
    AMDZenPStateDef pstates[AMDZEN_MAX_PSTATES];
    uint32_t cppcHighest, cppcNominal, cppcLowestNonlinear, cppcLowest; // raw CAP1 (0 if no CPPC)
    uint32_t cppcEffectiveHighest;  // 166 with prefcore, else raw highest
    uint32_t tjMax_mC;              // configured Tjmax (for SMC / throttle %), 0 unknown
    uint16_t apicId[AMDZEN_MAX_CPUS];   // per logical CPU (cpu_number order)
    uint16_t coreOfCpu[AMDZEN_MAX_CPUS];// physical core index per logical CPU
    uint8_t  cppcRank[AMDZEN_MAX_CORES];// raw CAP1 highest_perf per core (prefcore ranking)
} AMDZenInfo;

typedef struct AMDZenCpuSample {
    uint16_t effFreqMHz;    // TSC * dAPERF / dMPERF (busy frequency), 0 if idle whole interval
    uint16_t busyPermille;  // dMPERF / dTSC * 1000 (C0 residency)
    uint8_t  curPState;     // C001_0063[2:0]
    uint8_t  reqPState;     // last value written/read from C001_0062[2:0]
    uint16_t reserved;
} AMDZenCpuSample;

typedef struct AMDZenCoreSample {
    uint64_t energy_uJ;     // accumulated since load
    uint32_t power_mW;      // over last telemetry interval
    uint16_t effFreqMHz;    // max over SMT siblings
    uint16_t vid_mV;        // requested VID from C001_0293 (0 if unknown)
    uint16_t ipcX100;       // dIRPERF / dAPERF * 100 (0 if unavailable)
    uint16_t busyPermille;  // max over siblings
    uint32_t reserved;
} AMDZenCoreSample;

typedef struct AMDZenSample {
    uint32_t abiVersion, structSize;
    uint64_t seq;                   // increments every telemetry update
    uint64_t timestampNs;           // uptime ns at sample
    uint32_t intervalUs;            // telemetry interval that produced the deltas
    int32_t  tctl_mC;               // Tctl (clamped >= 0 per k10temp)
    int32_t  tdie_mC;               // Tctl - model offset
    int32_t  tccd_mC[AMDZEN_MAX_CCDS];
    uint64_t pkgEnergy_uJ;
    uint32_t pkgPower_mW;
    uint32_t coresPower_mW;         // sum of per-core power
    uint32_t vcore_mV, vsoc_mV;     // SVI2, 0 if unavailable
    uint32_t icore_mA, isoc_mA;     // SVI2 estimate, 0 if unavailable
    uint32_t avgFreqMHz;            // mean effective freq over busy cores
    uint32_t maxFreqMHz;            // max effective freq of any core
    uint32_t avgBusyPermille;       // mean over logical CPUs
    uint32_t pstateLimit;           // C001_0061[2:0] CurPstateLimit (>0 => HTC/PROCHOT/SMU limit)
    uint32_t governor;              // AMDZenGovernor in effect
    uint32_t boostEnabled;          // HWCR.CpbDis == 0 on CPU0
    uint32_t epp;                   // CPPC backend: current EPP
    uint32_t limitFast, limitSlow;  // legacy: P-state index range; CPPC: min/max perf
    uint32_t numCpus, numCores;
    AMDZenCpuSample  cpu[AMDZEN_MAX_CPUS];
    AMDZenCoreSample core[AMDZEN_MAX_CORES];
} AMDZenSample;

// Governor tunables (Linux defaults in comments)
typedef struct AMDZenTunables {
    uint32_t abiVersion, structSize;
    uint32_t governorPeriodMs;      // control loop period, 4..1000 (default 20)
    uint32_t telemetryPeriodMs;     // sensors / registry update, 100..10000 (default 1000)
    uint32_t upThreshold;           // ondemand/conservative % (ondemand 80, conservative 80)
    uint32_t downThreshold;         // conservative % (20)
    uint32_t samplingDownFactor;    // ticks to hold before lowering (1..100, default 1)
    uint32_t freqStepPct;           // conservative step (5)
    uint32_t utilHeadroomPct;       // schedutil headroom (125 = 1.25x)
    uint32_t utilEwmaShift;         // EWMA smoothing for util, 0 = none (default 1)
    uint32_t tjMax_mC;              // used for SMC / reporting (default per model, 0 = unknown)
    uint32_t reserved[7];
} AMDZenTunables;

// C API exported by AMDZenPM.kext for in-kernel consumers (AMDZenSMC.kext).
// Returns 0 on success. Safe to call from thread context only; each call holds a
// reference on the AMDZenPM instance, so a concurrent stop() cannot free it mid-call.
#if defined(KERNEL)
int AMDZenPM_copyInfo(AMDZenInfo *out);
int AMDZenPM_copySample(AMDZenSample *out);
#endif

#ifdef __cplusplus
}
#endif

#endif // AMDZEN_SHARED_H
