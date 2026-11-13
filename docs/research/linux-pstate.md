# AMD Zen performance / power control: register-level spec (from Linux)

Status: research note for the AmdCpu kext. It covers performance control, frequency reporting, boost, topology and idle on AMD family 17h/18h/19h/1Ah.
Reference source: torvalds/linux `master` at commit `fa4ad798f6f6` (2026-11-07, v7.4-rc1). Local copies of the source files are in `deps/linux-src/`. Treat them as reference data and do not copy them into the project, because they are GPL.
The constants and formulas below are restated as facts. Algorithms are paraphrased and are not quoted code.

Source tags used below:
- **[L:file:function]**: read directly from that Linux file.
- **[PPR]**: from AMD PPR or APM knowledge. Linux does not define the layout, so verify it against the PPR for the exact family and model before writing.
- **[HOST]**: observed on this machine (Ryzen 7 5700X) with a user-space CPUID probe and sysctl. These are read-only checks.

---

## 0. Host facts (Ryzen 7 5700X, family 19h model 21h stepping 2, "Vermeer", Zen3)

CPUID was read in user space (`/tmp/amdcpu-probe/cpuid`). [HOST]

| Leaf | Raw | Decoded |
|---|---|---|
| 1 | eax=00a20f12 ebx=04100800 ecx=7ef8320b | fam 0xF+0xA=0x19, model 0x21, step 2; MONITOR (ecx[3])=1 |
| 5 | eax=40 ebx=40 ecx=3 edx=0x11 | MWAIT line 64 B; EMX+IBE; C0 sub-states 1, C1 sub-states 1, nothing deeper via MWAIT |
| 6 | eax=4 ecx=1 | ARAT=1; ecx[0] EffFreq (APERF/MPERF) = 1 |
| 0xB.0 / 0xB.1 | eax=1 ebx=2 / eax=4 ebx=0x10 | SMT shift 1 (2 threads), core-level shift 4 (16 logical) |
| 8000_0001 | ecx=75c237ff | TOPOEXT (ecx[22])=1, MWAITX (ecx[29])=1 |
| 8000_0007 | ebx=0x3b edx=0x6799 | edx: TS0, TTP3, TM4, **HwPstate7=1**, **TscInvariant8=1**, **CPB9=1**, **EffFreqRO10=1**, ProcFeedback11=0, ProcPowerReporting12=0, ConnectedStandby13=1, **RAPL14=1**, FastCPPC15=0, CppcPerfPrio16=0 |
| 8000_0008 | ebx=111ef657 ecx=400f | ebx: CLZERO0, **IRPERF1=1**, XSaveErPtr2, **RDPRU4=1**, ..., **PPIN23=0**, SSBD24, **CPPC27=0**, PSFD28. ecx: NC=0xF (16 threads), ApicIdSize=4 |
| 8000_001D.3 (L3) | eax=0003c163 | NumSharingCache = ((eax>>14)&0xFFF)+1 = 16 threads, so 1 CCX of 8 cores |
| 8000_001E | eax=4 ebx=0x102 ecx=0 | ExtApicId=4 (current thread), CoreId=2, ThreadsPerCore=2, NodeId=0, NodesPerSocket=1 |
| 8000_0000 | eax=80000023 | **No leaf 0x80000026** (that leaf is Zen4c/Zen5+ only) |

sysctl: `machdep.tsc.frequency=3393617160` (TSC is about P0 = 3400 MHz base). `machdep.cpu.microcode_version=186 (0xBA)`. See the uncertainty list for why that value is suspicious on AMD.

**Main consequence for 19h/21h:** `X86_FEATURE_CPPC` (CPUID 8000_0008 EBX[27]) is **clear** on this CPU. Linux therefore runs amd-pstate on it **only through the ACPI `_CPC` "shared memory" path** (`cppc_acpi`). It never uses MSRs C001_02B0..B4. This matches Linux's own tables:
- `cpc_supported_by_cpu()` lists family 19h models 0x00–0x0F and 0x20–0x2F as `_CPC`-capable **without** the CPUID bit. [L:arch/x86/kernel/acpi/cppc.c:cpc_supported_by_cpu]
- `amd_cppc_supported()` warns "CPPC supported but disabled by BIOS" for Zen3/Zen4 only when the model is in 0x10–0x1F or 0x40–0xAF. Model 0x21 is not in that list, so Linux does not expect the MSR interface on Vermeer. [L:drivers/cpufreq/amd-pstate.c:amd_cppc_supported]

The practical OS-driven control paths on this host are therefore:
1. Legacy HW P-states: write `PStateCtl` (C001_0062) per thread, choosing among PStateDef P0..PstateMaxVal, and toggle boost with HWCR.CpbDis. This is the acpi-cpufreq equivalent.
2. Optionally, ACPI `_CPC` shared-memory CPPC. That needs parsing `_CPC` and possibly a PCC mailbox. See §3.9.

---

## 1. MSR reference

### 1.1 Table

| MSR | Linux name [L:arch/x86/include/asm/msr-index.h] | Scope / families | Use |
|---|---|---|---|
| 0x0000_008B | `MSR_AMD64_PATCH_LEVEL` (= `MSR_IA32_UCODE_REV`) | all | Microcode patch level. On AMD it is the **low 32 bits** (Linux reads EAX). [L:arch/x86/kernel/cpu/microcode/amd.c] |
| 0x0000_00E7 | `MSR_IA32_MPERF` | per thread, RW | Counts at a fixed reference rate (P0/TSC rate) **only in C0** |
| 0x0000_00E8 | `MSR_IA32_APERF` | per thread, RW | Counts at the actual core clock, only in C0 |
| 0xC000_00E7 | (not in Linux) MPerfReadOnly | 17h+, if CPUID 8000_0007 EDX[10] EffFreqRO | Read-only alias of MPERF [PPR] |
| 0xC000_00E8 | (not in Linux) APerfReadOnly | as above | Read-only alias of APERF [PPR] |
| 0xC000_00E9 | `MSR_F17H_IRPERF` | 17h+, if CPUID 8000_0008 EBX[1] | Instructions-retired free-running counter. Enabled by HWCR[30] |
| 0xC001_0015 | `MSR_K7_HWCR` | per thread | See §1.2 |
| 0xC001_0061 | (cpupower `MSR_AMD_PSTATE_LIMIT`) | per thread, RO | `[2:0]` CurPstateLimit, `[6:4]` PstateMaxVal [L:tools/power/cpupower/utils/helpers/amd.c:decode_pstates uses (val>>4)&7] |
| 0xC001_0062 | `MSR_AMD_PERF_CTL` | per thread, RW | `[2:0]` PstateCmd: requested HW P-state number (0 = fastest) |
| 0xC001_0063 | `MSR_AMD_PERF_STATUS` | per thread, RO | `[2:0]` CurPstate. acpi-cpufreq masks with `AMD_MSR_RANGE = 0x7` [L:drivers/cpufreq/acpi-cpufreq.c] |
| 0xC001_0064 + n (n=0..7) | `MSR_AMD_PSTATE_DEF_BASE` | per core (identical across cores) | PStateDef[n]. See §2 |
| 0xC001_0070 / 0071 | COFVID_CTL / COFVID_STATUS | **families 10h–16h only, absent on Zen** | Not in current msr-index.h. Do not use on 17h+ |
| 0xC001_0073 | (not in Linux) CStateBaseAddr | 17h+ [PPR] | `[15:0]` CstateAddr: I/O base trapped for C-state entry. See §6 |
| 0xC001_0292 | (not in Linux) PMGT_MISC / "PC6" | 17h/19h [PPR, ZenStates] | bit 32 = Package C6 enable (as used by ZenStates tools). Informational |
| 0xC001_0293 | (not in Linux) HwPstateStatus | 17h/19h [PPR] | `[7:0]` CurCpuFid, `[13:8]` CurCpuDfsId, `[21:14]` CurCpuVid, `[24:22]` CurHwPstate. Unverified for 1Ah |
| 0xC001_0294 | (not in Linux) CSTATE_POLICY | 17h+ [PPR] | CC1/CC6 timers and hysteresis. Informational |
| 0xC001_0296 | (not in Linux) CSTATE_CONFIG | 17h+ [PPR] | Per C-state action CCR0/1/2 in bytes 0/1/2. CCRn_CC6EN = bits 6/14/22. See §6 |
| 0xC001_0299 | `MSR_AMD_RAPL_POWER_UNIT` | 17h+ (8000_0007 EDX[14]) | RAPL units (covered in a separate note) |
| 0xC001_029A | `MSR_AMD_CORE_ENERGY_STATUS` | per core | RAPL core energy |
| 0xC001_029B | `MSR_AMD_PKG_ENERGY_STATUS` | per package | RAPL package energy |
| 0xC001_02B0 | `MSR_AMD_CPPC_CAP1` | per thread, RO; **only if 8000_0008 EBX[27]** | See §3.2 |
| 0xC001_02B1 | `MSR_AMD_CPPC_ENABLE` | per thread | bit 0 CppcEnable. Linux writes 1. [PPR] It cannot be cleared without a reset |
| 0xC001_02B2 | `MSR_AMD_CPPC_CAP2` | RO | Linux does not use it. [PPR] `[7:0]` ConstrainedPerf (unverified) |
| 0xC001_02B3 | `MSR_AMD_CPPC_REQ` | per thread, RW | See §3.2 |
| 0xC001_02B4 | `MSR_AMD_CPPC_STATUS` | | Linux does not use it. [PPR] bit 0 "excursion to minimum" or similar (unverified) |
| 0xC001_02B5 | `MSR_AMD_CPPC_REQ2` | only if 8000_0007 EDX[16] (`X86_FEATURE_CPPC_PERF_PRIO`) | `[7:0]` FloorPerf |
| 0xC001_02F0 | `MSR_AMD_PPIN_CTL` | if 8000_0008 EBX[23] | bit 0 LockOut, bit 1 Enable. See §7 |
| 0xC001_02F1 | `MSR_AMD_PPIN` | as above | 64-bit PPIN |
| 0xC000_0500 / 0501 / 0502 | `MSR_AMD_WORKLOAD_CLASS_CONFIG` / `_CLASS_ID` / `_HRST` | 1Ah with 8000_0021 EAX[22] | HW feedback / workload class (amd-hfi). See §7 |
| 0xC001_100C | `MSR_FAM10H_NODE_ID` | pre-Zen only | Node ID fallback in topology code |

The host has no PPIN (EBX[23]=0) and no CPPC MSRs (EBX[27]=0). Probing C001_02B0 or C001_02F1 with `rdmsr_carefully` will most likely #GP or return garbage. Gate every access on the CPUID bits.

### 1.2 HWCR (0xC001_0015) bit layout

| Bit | Name | Source | Meaning / use |
|---|---|---|---|
| 0 | SmmLock | L: `MSR_K7_HWCR_SMMLOCK` | Read only after lock |
| 9 | MonMwaitDis | PPR | 1 disables MONITOR/MWAIT |
| 10 | MonMwaitUserEn | PPR | Allows MWAIT at CPL3 |
| 21 | LockTscToCurrentP0 | PPR | 1 keeps the TSC (and the MPERF reference) at the boot P0 rate even if P0 is redefined |
| 24 | TscFreqSel | L: amd.c `bsp_init_amd` checks `BIT(24)` | Must be 1. Linux logs "TSC doesn't count with P0 frequency!" if it is 0 (family > 0x10) |
| **25** | **CpbDis** | L: `MSR_K7_HWCR_CPB_DIS` | 1 disables Core Performance Boost **on this thread**. 0 means boost allowed |
| 26 | EffFreqCntMwait | PPR | 1 makes APERF/MPERF also count during MWAIT C1. Leave it 0 if MPERF/TSC is used as C0 residency |
| 27 | EffFreqReadOnlyLock | PPR | Locks APERF/MPERF/IRPERF as read-only |
| **30** | **IRPerfEn** | L: `MSR_K7_HWCR_IRPERF_EN` | Enables the C000_00E9 instructions-retired counter |
| 35 | CpuidUserDis | L: `MSR_K7_HWCR_CPUID_USER_DIS` | CPUID faulting at CPL3 (Linux `set_cpuid_faulting`) |

Linux's own uses of HWCR:
- **IRPerfEn:** `init_amd()` sets bit 30 when `X86_FEATURE_IRPERF` is present. Zen1 (17h model < 0x30) clears it and the feature because of erratum 1054. [L:arch/x86/kernel/cpu/amd.c:init_amd, init_amd_zen1]
- **Boost read:** CPB is active when bit 25 is 0. [L:drivers/cpufreq/acpi-cpufreq.c:boost_state; tools/power/cpupower/utils/helpers/misc.c uses mask 0x02000000]
- **Boost write:** read-modify-write bit 25 on **every CPU in the policy** (`on_each_cpu_mask`). Clear to enable, set to disable. [L:acpi-cpufreq.c:boost_set_msr, set_boost]
- **CPU going offline:** clear CpbDis so an offline CPU cannot block boost for the others. [L:acpi-cpufreq.c:cpufreq_boost_down_prep]
- **amd-pstate:** reads bit 25 once at init. If it is set, `boost_supported=false` and max_perf is clamped to nominal_perf. amd-pstate never writes HWCR. Its "boost" knob only moves `policy->cpuinfo.max_freq` between `max_freq` and `nominal_freq`. [L:amd-pstate.c:amd_pstate_init_boost_support, amd_pstate_cpu_boost_update, amd_pstate_update]

### 1.3 Legacy P-state control MSRs (family 10h+, still the OS-visible path on Zen1–3 desktop)

- `PStateCurLim` C001_0061: `PstateMaxVal = (v >> 4) & 7` is the highest P-state **index** (the slowest state) software may request. `CurPstateLimit = v & 7` is the current fastest-allowed index (raised by HTC/PROCHOT/SMU).
- `PStateCtl` C001_0062: write the index to `[2:0]`. acpi-cpufreq writes the whole MSR with the `_PSS` Control value. [L:acpi-cpufreq.c:cpu_freq_write_amd]
  - amd-pstate writes **0** here before enabling CPPC ("resetting PERF_CTL_MSR will put the CPU in P0"). [L:amd-pstate.c:amd_perf_ctl_reset]
- `PStateStat` C001_0063: `[2:0]` current P-state index.
- Coordination [PPR]: on Zen the request is per thread. The core runs at the fastest P-state requested by its threads (the SMT siblings share one clock domain).
  - For family < 0x19, acpi-cpufreq overrides `_PSD` and uses one policy per thread with `CPUFREQ_SHARED_TYPE_HW`.
  - For family ≥ 0x19 it keeps the BIOS `_PSD`. [L:acpi-cpufreq.c:acpi_cpufreq_cpu_init, the `boot_cpu_data.x86 < 0x19` test]
  - The transition latency from `_PSS` is capped at 20 µs for FFH (MSR) control. [same function]

---

## 2. P-state definition decoding (PStateDef C001_0064..C001_006B)

The exact field layouts are those of cpupower's `union core_pstate`. [L:tools/power/cpupower/utils/helpers/amd.c] Capability gating is in [L:tools/power/cpupower/utils/helpers/cpuid.c]:
- `HW_PSTATE` = CPUID 8000_0007 EDX[7] && family != 0x14.
- `PSTATEDEF` (the Zen layout) = `HW_PSTATE` && family ≥ 0x17. Family 18h Hygon counts as ≥ 0x17.
- `CPB` = 8000_0007 EDX[9]. `CPB_MSR` (boost lives in HWCR, with no discrete boost P-states) = `CPB` && family ≥ 0x17.

### 2.1 Layouts

**Family 17h / 18h / 19h ("pstatedef"):**
| Bits | Field |
|---|---|
| 7:0 | CpuFid |
| 13:8 | CpuDfsId (DID) |
| 21:14 | CpuVid[7:0] |
| 29:22 | IddValue |
| 31:30 | IddDiv |
| 62:32 | reserved in cpupower's view (see the Zen4 VID note) |
| 63 | PstateEn |

**Family 1Ah ("pstatedef2", Zen5):**
| Bits | Field |
|---|---|
| 11:0 | CpuFid (12 bits) |
| 13:12 | reserved |
| 21:14 | CpuVid[7:0] |
| 29:22 | IddValue |
| 31:30 | IddDiv |
| 63 | PstateEn |

**Pre-17h (for completeness):** fid[5:0], did[8:6], vid[15:9], nbdid[22], nbvid[31:25], iddval[39:32], idddiv[41:40], en[63]. Family 12h uses did = `val & 0xF`.

### 2.2 Frequency (core COF) in MHz, from cpupower `get_cof()` / `get_did()`

- Family ≥ 0x1A: `COF = FID * 5` **only if FID > 0x0F**, otherwise 0 (invalid). DID is ignored.
- Family 0x17–0x19: `COF = 200 * FID / DID`. Integer math in cpupower. DID=0 means a disabled clock, so guard against division by zero.
  - Equivalent PPR form: COF = 25 MHz × FID / (DID/8). DID=8 means ÷1, DID=0x0A means ÷1.25, DID=0x10 means ÷2, and so on.
- Pre-17h: `COF = (100 * (FID + 0x10)) >> DID`. Family 11h uses +0x08 instead of +0x10.
- `decode_pstates()` loop:
  - Read C001_0061 and take `psmax = (v>>4)&7`.
  - Add `boost_states`. That is 0 on 17h+, because there are no discrete boost P-states. Pre-17h it is read from PCI D18F4 offset 0x15C bits [4:2].
  - Iterate i = 0..psmax and skip entries with bit 63 = 0.
- On Zen, P0 is the **base / nominal** frequency. Boost above P0 is decided by firmware (Precision Boost) when CpbDis=0 and the thread requests P0.

Example for this host: P0 is expected at 3400 MHz (for example FID=0x88, DID=8 gives 200·136/8 = 3400). TSC is 3393.6 MHz. Read the actual P0–P2 at runtime.

### 2.3 Voltage and current (not decoded anywhere in Linux) [PPR]

- **SVI2** (Zen1/Zen+/Zen2/Zen3, families 17h and 19h AM4/SP3): `V = 1.55 V − 0.00625 V × VID` for VID 0x00..0xF7. VID ≥ 0xF8 means off.
- **Idd:** `I = IddValue / 10^IddDiv` A. IddDiv 0 → ×1 A, 1 → ×0.1 A, 2 → ×0.01 A, 3 reserved. This is the per-P-state current *specification*, not telemetry.
- **Zen4 (19h models 0x10–0x1F and 0x60–0xAF, SVI3)** and **Zen5 (1Ah):** the VID width and scale differ.
  - Community tools and some PPRs use a 9th VID bit (often quoted at PStateDef bit 32) and the SVI3 scale `V ≈ 0.245 V + 0.005 V × VID`.
  - **Unverified:** do not write VIDs on Zen4/Zen5 until checked against the PPR for the exact model.
  - cpupower ignores everything above bit 31 except bit 63.

### 2.4 Editing P-states (informational only; this project must not write MSRs during research)

Rewriting PStateDef (FID/DID/VID) is what ZenStates-style tools do. Linux never does it.

---

## 3. amd-pstate (CPPC). MSR path in detail, plus what 19h/21h actually needs

Files: `drivers/cpufreq/amd-pstate.c`, `amd-pstate.h`, `amd-pstate-trace.h`, `arch/x86/kernel/acpi/cppc.c`, `Documentation/admin-guide/pm/amd-pstate.rst`.

### 3.1 Detection and gating (`amd_pstate_init`, `amd_cppc_supported`)

1. The vendor must be AMD. Hygon is **not** handled by amd-pstate.
2. `X86_FEATURE_HW_PSTATE` (8000_0007 EDX[7]) is required.
3. Family 17h with model < 0x30 (Zen1/Zen+) is rejected: no CPPC.
4. If `X86_FEATURE_CPPC` (**CPUID 8000_0008 EBX[27]**) is missing, the driver warns that the BIOS disabled CPPC for these cases:
   - Zen2 models 0x60–0x6F and 0x80–0xAF
   - Zen3/Zen4 models 0x10–0x1F and 0x40–0xAF
   - any Zen5

   It then continues with the shared-memory path.
5. The ACPI `_CPC` object must exist (`acpi_cpc_valid()`) **even on the MSR path**. The driver takes `nominal_freq` and `lowest_freq` (MHz) from `_CPC`, not from MSRs.
6. Default mode is `CONFIG_X86_AMD_PSTATE_DEFAULT_MODE` (1 disable, 2 passive, **3 active, the default**, 4 guided). [L:drivers/cpufreq/Kconfig.x86] The driver refuses to auto-load when the FADT preferred PM profile is unspecified, or is a server profile on family < 0x1A.
7. Path selection:
   - If CPPC is present, MSR callbacks are used (`msr_*`) and `fast_switch_possible=true`.
   - Otherwise shmem callbacks are used (`cppc_set_perf`, `cppc_set_enable`, `cppc_get_epp_perf`, and so on).

Zen generation by model [L:arch/x86/kernel/cpu/amd.c:bsp_init_amd]:
| Family | Zen1 | Zen2 | Zen3 | Zen4 | Zen5 | Zen6 |
|---|---|---|---|---|---|---|
| 17h | models 00–2F, 50–5F | models 30–4F, 60–7F, 90–91, A0–AF | | | | |
| 19h | | | models 00–0F, **20–5F** | models 10–1F, 60–AF | | |
| 1Ah | | | | | models 00–2F, 40–4F, 60–7F, D0–D7 | models 50–5F, 80–AF, C0–CF, D8–EF |

Zen1 also force-sets `X86_FEATURE_CPB` because of erratum 1076 (CPB bit missing from CPUID).

### 3.2 MSR field layouts [L:msr-index.h]

**CPPC_CAP1 (C001_02B0, RO):**
| Bits | Field |
|---|---|
| 7:0 | LowestPerf |
| 15:8 | LowestNonlinearPerf |
| 23:16 | NominalPerf |
| 31:24 | HighestPerf (with preferred core this is the *ranking*) |
| 39:32 | FloorPerfCount (number of distinct floor levels; 0 = unlimited). New field |

**CPPC_REQ (C001_02B3, RW):**
| Bits | Field |
|---|---|
| 7:0 | MaxPerf |
| 15:8 | MinPerf |
| 23:16 | DesiredPerf (0 = autonomous / no desired target) |
| 31:24 | EPP (0 = performance … 0xFF = energy) |

**CPPC_REQ2 (C001_02B5):** `[7:0]` FloorPerf. It is used only with `X86_FEATURE_CPPC_PERF_PRIO` (8000_0007 EDX[16]).

**CPPC_ENABLE (C001_02B1):** write 1. Linux uses `wrmsrq_safe_on_cpu(cpu, 0xC00102B1, 1)` on every policy init and on every CPU online. [L:msr_cppc_enable] Once enabled, CPPC takes priority over the legacy P-state MSRs (amd-pstate.rst).

### 3.3 Init sequence per CPU (`amd_pstate_cpu_init` / `amd_pstate_epp_cpu_init`)

1. Write `PStateCtl` C001_0062 = 0 (request P0).
2. `msr_init_perf`:
   - Read CAP1.
   - Compute `highest_perf = amd_get_effective_highest_perf(cpu)` (§3.4).
   - Read REQ into `cppc_req_cached`.
   - Set:
     - `perf.highest_perf = perf.max_limit_perf = effective_highest`
     - `lowest_perf = min_limit_perf = CAP1[7:0]`
     - `nominal_perf = CAP1[23:16]`
     - `lowest_nonlinear_perf = CAP1[15:8]`
     - `bios_min_perf = REQ[15:8]` (the BIOS "Requested CPU Min Frequency")
     - `prefcore_ranking = CAP1[31:24]` (raw)
     - `floor_perf_cnt = CAP1[39:32]`
3. `amd_pstate_init_prefcore`: if preferred cores are detected and amd-hfi is not active, publish `prefcore_ranking` as the scheduler priority (ITMT).
4. `amd_pstate_init_freq`:
   - `nominal_freq = _CPC.NominalFrequency × 1000` (kHz). The DMI quirk for EPYC 7K62 BIOS 5.14 (12/12/2019) on Zen2 replaces it with nominal 2600 MHz and lowest 550 MHz.
   - `min_freq = _CPC.LowestFrequency × 1000`.
   - `max_freq = amd_get_max_frequency()`. That returns a table value only for Zen6 hybrid (5025 / 3524 / 2399 MHz by core type), else 0. When it is 0, the fallback is `perf_to_freq(highest_perf)`.
   - `lowest_nonlinear_freq = perf_to_freq(lowest_nonlinear_perf)`.
   - Sanity checks:
     - `min, max, nominal > 0` and `min ≤ max`
     - `min ≤ lowest_nonlinear_freq ≤ nominal_freq`
5. `amd_pstate_init_boost_support`: if `X86_FEATURE_CPB` and HWCR[25]==0, then `boost_supported=true`.
6. Policy limits:
   - `cpuinfo.min_freq = perf_to_freq(lowest_perf)`
   - `cpuinfo.max_freq = max_freq`
   - `transition_latency` = `_CPC` latency, or 20000 ns
   - `transition_delay_us` = `_CPC` latency in µs; otherwise 600 µs if FastCPPC (8000_0007 EDX[15]), else 1000 µs
7. Write CPPC_ENABLE = 1.
8. `amd_pstate_init_floor_perf`, only with PERF_PRIO: read REQ2. If FloorPerf < lowest_perf, write nominal_perf there.
9. EPP driver (active) only:
   - Read the current EPP from REQ[31:24] and cache it.
   - If the FADT profile is server or undefined: policy=PERFORMANCE and EPP defaults stay at the firmware value. Otherwise: policy=POWERSAVE, `epp_default_ac = values[performance]`, `epp_default_dc = values[balance_performance]`.
   - Write `epp_default_dc`. Note that this is the DC value even on AC, at init. The power-supply notifier changes it later only in dynamic mode.

Exit, offline and suspend write REQ back to `{min=bios_min_perf, des=…, max=…, epp=…}`. Exit uses `{bios_min_perf, 0, 0, 0}`. They also restore REQ2 floor to the BIOS value.

### 3.4 highest_perf, preferred core and the 166/196 numerators [L:arch/x86/kernel/acpi/cppc.c]

Constants: `CPPC_HIGHEST_PERF_PREFCORE = 166`, `CPPC_HIGHEST_PERF_PERFORMANCE = 196`.

- `amd_get_highest_perf(cpu)`: CAP1[31:24] if `X86_FEATURE_CPPC`, else `_CPC.HighestPerformance`.
- `amd_detect_prefcore()`:
  - Walk the online CPUs and collect up to two **distinct** highest_perf values.
  - Preferred core is "supported" if two distinct values exist.
  - `boost_numerator` = the first CPU's value.
  - The result is cached.
- `amd_get_effective_highest_perf(cpu)`:
  1. No prefcore: return `boost_numerator` (the shared raw HighestPerf).
  2. Zen4 with model 0x70–0x7F (Phoenix): return **196**.
  3. Heterogeneous (`X86_FEATURE_AMD_HTR_CORES`, 8000_0026 EAX[30]):
     - Zen5 performance-type core: return **196**.
     - Zen5 efficiency core, and all Zen6: return that CPU's raw highest_perf.
  4. Otherwise (any other prefcore system, **including Zen3 desktop when CPPC is enabled in BIOS**): return **166**.

  The reason: with preferred cores, firmware encodes a per-core *ranking* in HighestPerf, anywhere in 166–255. The real "max perf" numerator is therefore fixed and the ranking is kept separately.
- Boost ratio for scheduler frequency invariance and for acpi-cpufreq max_freq (`amd_get_boost_ratio`): numerator = effective_highest (or the max_frequency table), denominator = nominal_perf (or nominal_freq).
- `amd_set_max_freq_ratio`: `ratio = ((num·1024/den) + 1024) / 2`. That is the **midpoint** between nominal and max boost, used as the "max freq ratio" for frequency invariance.
- Ranking updates: the platform raises a `_CPC` / Notify event and `amd_pstate_update_limits` re-reads highest_perf. If it changed and is < 255, the ITMT priority is updated.

### 3.5 perf ↔ freq conversion (`amd-pstate.h`)

All frequencies are in kHz and perf is an 8-bit value.
- `freq_to_perf(f) = clamp( ceil(f × nominal_perf / nominal_freq), lowest_perf, highest_perf )`
- `perf_to_freq(p) = ceil(nominal_freq × p / nominal_perf)`

The scale is **linear and anchored at nominal**. `max_freq = nominal_freq × highest_perf / nominal_perf`. For prefcore Zen3 that becomes `nominal_freq × 166 / nominal_perf`.

### 3.6 Modes

| Mode | Driver | Behaviour |
|---|---|---|
| passive (2) | `amd-pstate` (`.target`, `.fast_switch`, `.adjust_perf`) | The OS sets DesiredPerf. Min and max come from policy limits |
| active (3) | `amd-pstate-epp` (`.setpolicy`) | Writes min/max limits and EPP with **DesiredPerf = 0**. Firmware picks the frequency |
| guided (4) | `amd-pstate` | Like passive, but with a dynamic governor (schedutil/ondemand flag `CPUFREQ_GOV_DYNAMIC_SWITCHING`) the request becomes **MinPerf = computed desired, DesiredPerf = 0**. The platform then picks autonomously in [min, max]. In shmem mode it also flips `_CPC` AutoSelectEnable |

Mode transitions use a state machine.
- passive ↔ guided only toggles state (and `auto_sel` on shmem).
- Any transition involving active re-registers the driver.

**Passive update (`amd_pstate_update`):**
1. If `!boost_supported`: `max_perf = min(nominal_perf, max_perf)`.
2. `des = clamp(des, min, max)`.
3. `policy->cur = perf_to_freq(des)`.
4. Apply the guided transform if in guided mode.
5. Write REQ `{max, min, des, epp=0}`, only if it differs from the cache.

**Fast path (schedutil `adjust_perf`, `amd_pstate_adjust_perf`).** Inputs are min_util, target_util and capacity (1024), all in capacity units:
- `cap = highest_perf`
- `des = target_util < capacity ? ceil(cap × target_util / capacity) : cap`
- `min = min_util < capacity ? ceil(cap × min_util / capacity) : cap`, then raised to at least `min_limit_perf`
- `max = max(max_limit_perf, min)`
- Then call the passive update.

**Policy limits (`amd_pstate_update_min_max_limit`):**
- `max_limit_perf = highest_perf` if `policy.max ≥ max_freq`, else `freq_to_perf(policy.max)`.
- If the policy is PERFORMANCE (active mode): `min_limit_perf = min(bios_min_perf ?: nominal_perf, max_limit_perf)`. It deliberately does **not** use highest_perf, to avoid throttling on power-limited parts. Otherwise: `min_limit_perf = freq_to_perf(policy.min)`.
- `verify()`: if `policy.min` is still the QoS default, the default min is `perf_to_freq(bios_min_perf)` if set, else `lowest_nonlinear_freq`.

**Active-mode update (`amd_pstate_epp_update_limit`):** write REQ `{min_limit_perf, des=0, max_limit_perf, epp}`. EPP is 0 for the PERFORMANCE policy, else the cached EPP.

### 3.7 EPP presets (`amd-pstate.c`)

| Index | String | Legacy value (all non-hybrid parts, including Zen3) |
|---|---|---|
| 0 | default | (firmware value) |
| 1 | performance | **0x00** |
| 2 | balance_performance | **0x80** |
| 3 | balance_power | **0xBF** |
| 4 | power | **0xFF** |
| 5 | custom (raw numeric write) | user value |
| 6 | dynamic (platform profile driven) | |

Zen6 hybrid client overrides (family 1Ah models 0x80/0x81/0x84/0x85/0xE0), as performance / balance_performance / balance_power / power:
- P and E cores: 25 / 51 / 64 / 115
- Low-power cores: 25 / 64 / 64 / 64

With the PERFORMANCE policy only "performance" is accepted, and EPP is forced to 0.

### 3.8 Boost under amd-pstate

- `boost_supported` = CPB in CPUID && HWCR.CpbDis==0 at init time.
- Turning boost off sets `cpuinfo.max_freq = nominal_freq`. In passive mode it also updates the max QoS request, so MaxPerf becomes `freq_to_perf(nominal)`. The SMU then cannot exceed nominal.
- HWCR is not touched. cpupower notes that with amd-pstate active, firmware gives CPPC priority, and it hides the CPB/HWCR knobs.

### 3.9 What 19h model 21h (this host) gets in Linux

- `X86_FEATURE_CPPC` = 0, so amd-pstate uses **shared memory**:
  - `cppc_set_enable(cpu, 1)` writes the `_CPC` Enable register.
  - Perf requests go through `cppc_set_perf` into the `_CPC` DesiredPerf, MinPerf and MaxPerf registers.
  - EPP goes through the `_CPC` EnergyPerformancePreference register.
  - There is no fast_switch, so schedutil requests are deferred to a kthread / irq_work.
- `_CPC` registers can be SystemMemory, SystemIO, FFH or **PCC** (Platform Communication Channel, address space 0x0A, with a doorbell and a command-complete handshake through the ACPI PCCT table). [L:drivers/acpi/cppc_acpi.c `CPC_IN_PCC`, `send_pcc_cmd`]
- `_CPC` package register order [L:include/acpi/cppc_acpi.h enum cppc_regs], 0-based after the NumEntries and Revision header:
  - HighestPerf, NominalPerf, LowestNonlinearPerf, LowestPerf, GuaranteedPerf
  - DesiredPerf, MinPerf, MaxPerf, PerfReductionTolerance, TimeWindow
  - CounterWrapTime, ReferenceCounter, DeliveredCounter, PerfLimited, Enable
  - AutoSelectEnable, AutoActivityWindow, EnergyPerfPreference, ReferencePerf
  - LowestFreq, NominalFreq (rev 3)
  - plus newer OSPM entries
- Prefcore: on Ryzen 5000 desktop with BIOS CPPC and "CPPC Preferred Cores" enabled, `_CPC.HighestPerf` differs per core. The effective highest is then **166** for every core and the raw value is the ranking.
- If BIOS CPPC is disabled or `_CPC` is absent, Linux falls back to **acpi-cpufreq** using `_PSS`. That gives the 3 HW P-states via C001_0062, plus CPB through HWCR[25]. `cpuinfo.max_freq` is boosted using `amd_get_boost_ratio` when `_CPC` exists, else by the DMI "Max Speed" if it is plausible.
- **For the kext:** on 19h/21h the realistic first implementation is the **acpi-cpufreq-equivalent path**:
  - Get the P-states from PStateDef. They should match `_PSS`.
  - Control them through PStateCtl per thread.
  - Toggle boost through HWCR[25] on all threads.

  `_CPC`/PCC support is a possible later stage. It needs `IOACPIPlatformDevice::evaluateObject("_CPC")` on the processor objects plus PCCT parsing, and the host's ACPI tables have **not been inspected yet**. IORegistry does not expose "ACPI Tables" on this system, so the tables have to be dumped another way, for example with OpenCore's SysReport.

### 3.10 Trace fields (useful as a debug schema) [L:amd-pstate-trace.h]

- `amd_pstate_perf`: min_perf, target_perf, capacity, freq, mperf, aperf, tsc, cpu_id, fast_switch.
- `amd_pstate_epp_perf`: cpu_id, highest_perf, epp, min_perf, max_perf, boost, changed.
- `amd_pstate_cppc_req2`: cpu_id, floor_perf, changed, err_code.

`amd_pstate_sample()`:
- Disable IRQs, read APERF, MPERF and TSC.
- Skip the sample if MPERF or TSC is unchanged.
- Compute the deltas and `freq = Δaperf × cpu_khz / Δmperf`.

---

## 4. Governors to port (exact defaults)

### 4.1 schedutil [L:kernel/sched/cpufreq_schedutil.c, include/linux/sched/cpufreq.h]

- **Headroom:** `map_util_perf(u) = u + u/4`, which is ×1.25 and gives the tipping point at 80 % util. It is applied in `sugov_effective_cpu_perf`: `perf = min(1.25·util, max_cap)`, then `perf = max(perf, bw_min)`.
- **Frequency:** `next_raw = ref_freq × perf / max_cap` (`map_util_freq(u, f, cap) = f·u/cap`), where `max_cap = 1024` on non-hybrid parts. The reference frequency (`get_capacity_ref_freq`) is:
  - `arch_scale_freq_ref` if non-zero (an arm64 concept; 0 on x86), else
  - `policy->cpuinfo.max_freq` if the scheduler is frequency invariant (x86 with APERF/MPERF ratio set up), else
  - `cur + cur/4` (×1.25 of the current frequency). In this non-invariant case the 1.25 headroom effectively applies twice.
- **Resolve:** `cpufreq_driver_resolve_freq(policy, next_raw)`:
  - Clamp to `[policy.min, policy.max]`.
  - For table drivers pick the **lowest table frequency ≥ target** (`CPUFREQ_RELATION_LE`: L = lowest at or above target, E = prefer efficient entries).
- **Rate limit:** `rate_limit_us` defaults to `cpufreq_policy_transition_delay_us()`:
  - `transition_delay_us` if the driver set it (amd-pstate: 1000 µs, 600 µs with FastCPPC, or the `_CPC` value), else
  - `latency_ns/1000 × 1.5`, else
  - 1000 µs.

  For acpi-cpufreq on Zen the `_PSS` latency is capped at 20 µs, so the rate limit is 30 µs.
- **Hold:** if the CPU has not entered idle since the last update (NO_HZ idle-call counter unchanged), do not lower the frequency.
- **iowait boost:**
  - Starts at `IOWAIT_BOOST_MIN = 1024/8 = 128` and doubles on each consecutive iowait wakeup, up to 1024.
  - Halves on each update without a new request and drops to 0 below 128.
  - Resets if more than one tick passed since the last update.
  - `boost_util = iowait_boost × max_cap >> 10`, and `util = max(util, boost_util)`.
- **Shared policies:** use the max util over the CPUs in the policy (`sugov_next_freq_shared`).
- **Perf path:** with `adjust_perf` (amd-pstate MSR passive) and frequency invariance, schedutil calls `adjust_perf(min=bw_min, target=util, max=bw_max, cap)` directly. See §3.6.

### 4.2 ondemand [L:drivers/cpufreq/cpufreq_ondemand.c, cpufreq_governor.c]

Defaults:
- `up_threshold` = **95** (when NO_HZ idle micro-accounting is active, the normal case) or **80** otherwise. Range 1..100.
- `sampling_down_factor` = **1**, max **100000**.
- `powersave_bias` = 0 (in 0.1 % units, max 1000).
- `ignore_nice_load` = 0.
- `io_is_busy` = platform dependent (true on modern x86).
- `sampling_rate` = `max(2 × TICK_USEC, transition_delay_us)`. The minimum settable value is `2 × TICK_NSEC/1000` µs.

Load (`dbs_update`), computed per CPU in the policy over the window `Δt`:
- `idle = Δidle_time`. Iowait counts as busy if `io_is_busy`.
- `load = 100 × (Δt − idle) / Δt`.
- If `idle > 2 × sampling_rate` and `prev_load ≠ 0`: use `prev_load` once and then clear it. This handles a task that wakes up after a long idle.
- Policy load = max over its CPUs.

Decision (`od_update`):
- If `load > up_threshold`: go to `policy.max` (`RELATION_HE`). If cur < max, also set `rate_mult = sampling_down_factor` so the next samples are delayed.
- Else: `f = cpuinfo.min + load × (cpuinfo.max − cpuinfo.min) / 100` with `RELATION_CE` (closest, efficient), and `rate_mult = 1`.
- Next sample after `sampling_rate × rate_mult`.

`powersave_bias`:
- Target `f_req × (1 − bias/1000)`. If that falls between two table entries, alternate between f_hi and f_lo within one sampling period.
- Time at f_hi = `round((f_avg − f_lo) × rate / (f_hi − f_lo))`.
- `amd_freq_sensitivity.c` sets this only on family 15h/16h. It is not relevant to Zen.

### 4.3 conservative [L:drivers/cpufreq/cpufreq_conservative.c]

Defaults:
- `up_threshold` = **80**
- `down_threshold` = **20**
- `freq_step` = **5** % of `policy.max`. If the computed step is 0, 5 kHz is used.
- `sampling_down_factor` = **1**, max **10**

Each sample:
1. If `freq_step` is 0, do nothing.
2. If the remembered request is outside [min, max], reset it to cur.
3. If the policy sample load is below `down_threshold` and some CPU was idle for N > 2 periods, apply N deferred down-steps.
4. If `load > up`: `req += step`, capped at max, applied with `RELATION_HE`.
5. Else, once every `sampling_down_factor` samples: if `load < down`, `req −= step`, floored at min, applied with `RELATION_LE`.

### 4.4 Utilization without a scheduler hook: APERF/MPERF/TSC

[L:arch/x86/kernel/cpu/aperfmperf.c, turbostat]

Per-CPU counter deltas over a window are `ΔA`, `ΔM` and `ΔT` (TSC). Recommended derived metrics:
- **C0 residency (busy fraction)** `busy = ΔM / ΔT`. This is turbostat's `Busy% = 100·mperf/tsc`. It holds because MPERF ticks at the TSC rate (the P0 reference) only in C0. HWCR[26] EffFreqCntMwait must be 0, or MWAIT time is counted as busy.
- **Effective frequency while busy** `f_busy = f_tsc × ΔA / ΔM`. This is turbostat Bzy_MHz, and Linux `arch_freq_get_on_cpu`: `cpu_khz × acnt / mcnt`. That result is valid only if the sample is younger than `HZ/50` (20 ms), otherwise it falls back to the cpufreq cached frequency.
- **Average frequency over wall time** `f_avg = f_tsc × ΔA / ΔT` (turbostat Avg_MHz).
- **Frequency-invariant utilization** on the 0..1024 scale, which is what schedutil consumes after PELT averaging:
  - `freq_scale = (ΔA << 20) / (ΔM × max_freq_ratio)`, capped at 1024 [L:scale_freq_tick]
  - `max_freq_ratio = 1024 × f_maxboost / f_P0`, or the AMD midpoint ratio from §3.4
  - `util_inv ≈ busy × freq_scale` = `1024 × ΔA / (ΔT × max_freq_ratio/1024)`

  The kext can use `util_inv` (optionally EWMA-smoothed with a PELT-like half-life of about 32 ms) as the `util` input to the schedutil formula. Then: `next_freq = 1.25 × f_max × util_inv / 1024`, clamped and resolved to a P-state.
- Overflow and wobble guard: Linux disables invariance if a shift or multiply would overflow, or if the result is 0.

---

## 5. CPUID features, boost and topology

### 5.1 Feature bits

| Feature | CPUID | Linux flag |
|---|---|---|
| HW P-state | 8000_0007 EDX[7] | `X86_FEATURE_HW_PSTATE` (scattered.c) |
| TSC invariant | 8000_0007 EDX[8] | sets `CONSTANT_TSC` + `NONSTOP_TSC` (amd.c early_init) |
| CPB | 8000_0007 EDX[9] | `X86_FEATURE_CPB`. Zen1 force-sets it (erratum 1076) |
| EffFreqRO (C000_00E7/E8) | 8000_0007 EDX[10] | (none) |
| ProcFeedbackInterface | 8000_0007 EDX[11] | `X86_FEATURE_PROC_FEEDBACK` |
| Accumulated power | 8000_0007 EDX[12] | `X86_FEATURE_ACC_POWER` (family 15h) |
| RAPL | 8000_0007 EDX[14] | `X86_FEATURE_RAPL` |
| Fast CPPC | 8000_0007 EDX[15] | `X86_FEATURE_AMD_FAST_CPPC` |
| CPPC floor perf (REQ2) | 8000_0007 EDX[16] | `X86_FEATURE_CPPC_PERF_PRIO` |
| APERF/MPERF | 6 ECX[0] | `X86_FEATURE_APERFMPERF` |
| IRPERF | 8000_0008 EBX[1] | `X86_FEATURE_IRPERF` |
| RDPRU | 8000_0008 EBX[4] | `X86_FEATURE_RDPRU` (user-mode read of MPERF/APERF: ECX=0 MPERF, ECX=1 APERF) |
| PPIN | 8000_0008 EBX[23] | `X86_FEATURE_AMD_PPIN` |
| **CPPC MSRs** | **8000_0008 EBX[27]** | `X86_FEATURE_CPPC` |
| Workload class (HFI) | 8000_0021 EAX[22] | `X86_FEATURE_AMD_WORKLOAD_CLASS` |
| Heterogeneous cores | 8000_0026 EAX[30] | `X86_FEATURE_AMD_HTR_CORES` |
| MONITOR/MWAIT | 1 ECX[3] | `X86_FEATURE_MWAIT` |
| MWAITX | 8000_0001 ECX[29] | `X86_FEATURE_MWAITX` |
| TOPOEXT | 8000_0001 ECX[22] | `X86_FEATURE_TOPOEXT` |

### 5.2 Topology (`topology_amd.c`, `topology_ext.c`, `cacheinfo.c`)

Linux parsing order on AMD:
1. Leaf **8000_0026** (if max ext leaf ≥ 0x80000026), else leaf **0xB**. Either sets the SMT and CORE shifts from the x2APIC ID.
2. Else **8000_0008** ECX: `[7:0]` NC = threads−1, `[15:12]` ApicIdCoreIdSize. If that is 0, shift = order(NC+1).
3. Then **8000_001E** (requires TOPOEXT):
   - EAX = extended APIC ID
   - EBX`[7:0]` CoreId (unique per socket), EBX`[15:8]` ThreadsPerCore−1
   - ECX`[7:0]` NodeId, ECX`[10:8]` NodesPerSocket−1

   If no x2topology leaf was used, the APIC ID comes from EAX and the SMT shift is `order(ThreadsPerCore)` (family ≥ 17h).

**LLC / CCX id** (`cacheinfo_amd_init_llc_id`):
- Pre-Zen: `llc_id = node_id`.
- **17h models ≤ 0x1F (Zen1/Zen+):** `llc_id = apicid >> 3`. Each CCX has 4 cores / 8 threads.
- All newer parts: `llc_id = apicid >> order(L3 NumSharingCache)`. NumSharingCache = `CPUID 8000_001D(subleaf = L3 index) EAX[25:14] + 1`.

**Logical CPU → core / CCX / CCD recipe** (for XNU, run CPUID on each CPU or use XNU's per-cpu APIC ID):
- `smt_shift`, `core_shift` from leaf 0xB (or 8000_0026).
- `thread = apicid & ((1<<smt_shift)−1)`
- `core = apicid >> smt_shift`
- `ccx = apicid >> order(L3 sharing)`
- **CCD:**
  - Zen3/Zen4/Zen5 desktop: CCD = CCX (one CCX per CCD).
  - Zen2: 2 CCX per CCD, so `ccd = apicid >> (order(L3 sharing)+1)`. This is an inference, not stated by Linux.
  - Zen4/Zen5 with leaf 8000_0026: use level type 3 (Linux calls it `AMD_CCD_TYPE` and maps it to TILE) for CCD and type 4 for socket.
- APIC IDs can be **sparse** (for example 6-core CCDs keep 8-core slots), so never assume contiguity.
- Host result: SMT shift 1, 16 threads share L3, so all 16 threads are in CCX 0 / CCD 0 and core = apicid>>1.

**Leaf 8000_0026** per subleaf (Zen4c / Zen5+):
- EAX`[4:0]` x2APIC shift to the next level, EAX`[30]` heterogeneous cores.
- EBX`[15:0]` processors at this level, `[23:16]` power-efficiency ranking, `[27:24]` native model id, `[31:28]` core type (0 = performance, 1 = efficiency, 2 = low power). [L:arch/x86/include/asm/processor.h]
- ECX`[7:0]` level, `[15:8]` type: 1 SMT, 2 Core, 3 CCD/complex, 4 socket.
- EDX = x2APIC id.

Hygon (18h) note: package ID is ApicId[6..] on models ≤ 3, and the node ID sits in bits [5:4].

---

## 6. Idle / C-states on AMD (informational)

- **Default idle routine** [L:arch/x86/kernel/process.c:prefer_mwait_c1_over_halt, select_idle_routine]:
  - Use MWAIT for C1 if MONITOR/MWAIT is present, no `X86_BUG_MONITOR` or `X86_BUG_AMD_APIC_C1E`, no idle= override, and either CPUID 5 lacks EMX, or EMX is present and EDX[7:4] (C1 sub-states) is non-zero.
  - Host: EDX=0x11, so C1 sub-states = 1 and Linux uses `mwait_idle` (MWAIT EAX=0, ECX=0) for C1.
  - Otherwise use HLT.
- **E400 (AMD C1E erratum):** this is K8 rev ≥ 0x41.2 and family 10h model ≥ 2. After ACPI init, if `MSR_K8_INT_PENDING_MSG` (C001_0055) & `0x18000000` (C1E active), Linux sets `X86_BUG_AMD_APIC_C1E`, forces tick broadcast and avoids MWAIT. **Not applicable to Zen.**
- **ACPI `_CST` entry** [L:drivers/acpi/processor_idle.c]:
  - FFH entries go through `acpi_processor_ffh_cstate_enter`, which issues MWAIT with the `_CST` address as hint. The hint is validated against the CPUID 5 EDX sub-state count and needs ECX[1] break-on-interrupt for C2+.
  - SystemIO entries (`io_idle`) do a single `inb(port)`. AMD skips the dummy PM-timer read (that is Intel-only).
  - Zen sets `bm_check=1, bm_control=0`: no WBINVD and no ARB_DIS needed for C3-type states. [L:arch/x86/kernel/acpi/cstate.c:acpi_processor_power_init_bm_check]
  - TSC stays reliable in deep C-states when NONSTOP_TSC is set (8000_0007 EDX[8]).
- **How CC6 is reached on Zen** [PPR, unverified]:
  - The BIOS programs `CStateBaseAddr` C001_0073[15:0] (commonly 0x413) and exposes `_CST` entries.
  - C1 is FFH/MWAIT or HLT. C2 is typically SystemIO at `CstateAddr + 1` (sometimes C3 at +2).
  - An I/O **read** of `CstateAddr + n` executes C-state action CCRn from `CSTATE_CONFIG` C001_0296. Byte n holds the CC1 DFS divider in `[5:0]` and CCn_CC6EN in bit 6, so the CC6 enables are bits 6/14/22.
  - Package C6 is enabled by C001_0292 bit 32 (ZenStates naming).
  - `CSTATE_POLICY` C001_0294 holds the CC1→CC6 promotion timers and hysteresis.
  - CPUID 5 reports only C0 and C1 MWAIT sub-states, so **MWAIT alone gives CC1 (clock-gated); CC6 needs the I/O-port read** (or firmware auto-promotion from CC1 per CSTATE_POLICY).
- **XNU implications:**
  - Stock XNU without a PM kext idles with STI;HLT, which is CC1. HLT does not count MPERF.
  - To get CC6 the kext would need an idle hook that performs `inb(CstateAddr+1)` with interrupts handled like `io_idle`. XNU's `pmKextRegister` dispatch table (`MachineIdle`) is a private interface whose export status is unknown.
  - Also required: an APIC timer that survives deep C-states. ARAT = CPUID 6 EAX[2] = 1 on the host, and Linux sets ARAT for family > 0x11.

---

## 7. Other controls

- **Core Performance Boost:**
  - Per thread through HWCR[25]. The effect is per core, because a core boosts only if no sibling disables it.
  - Linux applies it to every CPU in the policy and clears it on CPU offline.
  - It needs CPUID 8000_0007 EDX[9]. Family ≥ 17h has no discrete boost P-states, and PCI D18F4x15C applies only before 17h.
- **Precision Boost Overdrive / Curve Optimizer / PPT, TDC and EDC limits:** these are SMU mailbox commands only (SMN mailbox through the root complex 00:00.0 index/data registers 0x60/0x64 on Zen). Linux mainline has no PBO control. amd-pstate only hints via CPPC. They are out of scope for the MSR interface.
- **Preferred-core ranking:**
  - MSR systems: CAP1[31:24].
  - `_CPC` systems (this host): `_CPC.HighestPerf`.
  - Zen5/Zen6 hybrid with 8000_0021 EAX[22]: **amd-hfi** takes over. It enables workload classification with `MSR_AMD_WORKLOAD_CLASS_CONFIG` C000_0500 = 1 and resets history with `MSR_AMD_WORKLOAD_HRST` C000_0502 = 1. Per-class perf/efficiency scores are read from a PCC shared-memory ranking table (version 2..3). CPUID **8000_0027 EAX** gives the number of classes. [L:drivers/platform/x86/amd/hfi/hfi.c]
  - 8000_0026 EBX[23:16] gives a static power-efficiency ranking.
- **PPIN** [L:arch/x86/kernel/cpu/common.c:ppin_init]:
  - Requires CPUID 8000_0008 EBX[23].
  - Read PPIN_CTL C001_02F0. If `(ctl & 3) == 1` it is locked-disabled: give up.
  - If bit 1 (enable) is clear, Linux tries to set it. **The kext should not write it.** It should only read it if it is already enabled.
  - Then read C001_02F1.
  - Absent on this host.
- **Microcode patch level:** MSR 0x8B, low 32 bits (`rdmsr` EAX) on AMD. Family 19h model 21h values look like 0x0A20120x.
- **Instructions-retired counter:** C000_00E9, enabled by HWCR[30] (Linux enables it at boot when `X86_FEATURE_IRPERF` is set, except on Zen1, where erratum 1054 applies). Use ΔIRPERF / ΔAPERF as an IPC metric. The kext must decide whether it may set HWCR[30], since that is a state change.
- **RDPRU** (8000_0008 EBX[4]): user-space read of MPERF (ECX=0) and APERF (ECX=1) with no kext. It is gated by CR4/HWCR configuration that XNU does not touch. Unverified on XNU.

---

## 8. Family 19h model 21h quick sheet (what to implement first)

| Item | Value / behaviour |
|---|---|
| Generation | Zen3, `X86_FEATURE_ZEN3` (19h models 00–0F and 20–5F) |
| CPPC MSRs | **No** (CPUID 8000_0008 EBX[27]=0). Linux uses `_CPC` shared memory if the BIOS enables CPPC, else acpi-cpufreq |
| amd-pstate highest_perf | 166 if the `_CPC` HighestPerf values differ per core (prefcore). Otherwise the raw `_CPC.HighestPerf` |
| No Zen3-specific quirk in amd-pstate | The 196 value applies only to Zen4 models 0x70–0x7F and Zen5 heterogeneous parts. The DMI quirk is EPYC 7K62 (Zen2) only |
| P-states | PStateDef C001_0064.. with 17h layout. COF = 200·FID/DID MHz. PstateMaxVal from C001_0061[6:4] (typically 2, so P0..P2) |
| Control | C001_0062 per thread. The core takes the fastest request of its 2 threads. Latency ≤ 20 µs |
| Boost | CPB present. HWCR[25] per thread; set the same value on all 16 threads |
| Freq reporting | APERF/MPERF (or C000_00E7/E8 read-only). `f = TSC_Hz × ΔA/ΔM`. TSC = 3393.6 MHz on the host (about the P0 nominal) |
| IRPERF | Present (C000_00E9) |
| PPIN | Not present |
| RAPL | Present (C001_0299/029A/029B) |
| Topology | 8 cores / 16 threads, SMT shift 1, one CCX = one CCD, APIC IDs 0..15 |
| Idle | MWAIT C1 supported (1 sub-state). CC6 through the `_CST` I/O-port entry |
| Governors | acpi-cpufreq style: ondemand (up 95/80, sdf 1) or schedutil-like (1.25 headroom on frequency-invariant util from APERF/MPERF/TSC), with a rate limit of about 30 µs. A practical XNU timer period is 4–20 ms |

---

## 9. Uncertainties and items to verify

1. **No `_CPC`/PCCT/`_PSS`/`_CST` dump has been made for the host.** IORegistry does not expose "ACPI Tables" here. A dump is needed to learn whether CPPC is enabled in the BIOS, which address space `_CPC` uses (PCC vs SystemMemory), the `_PSS` values and latencies, and the `_CST` I/O ports.
2. **MSR CPPC on 19h/21h:** the CPUID bit is 0. Whether C001_02B0..B4 still respond (undocumented) is unknown. If it is ever tested, use `rdmsr_carefully` only.
3. **Fields marked PPR** were not verified against the 19h/21h PPR (document 56214 for Vermeer). These include HWCR bits 9/10/21/26/27, C001_0293 layout, C001_0073/0294/0296/0292 semantics, CPPC_CAP2 and CPPC_STATUS contents, and the "fastest request among SMT threads" coordination rule.
4. **Zen4/Zen5 VID:** the 9-bit VID position (bit 32?) and the SVI3 scale (0.245 V + 5 mV·VID) are unverified. cpupower only shows an 8-bit VID at [21:14] for 1Ah.
5. **The Zen5 FID rule** "COF = 5·FID, valid only if FID > 0x0F" comes from cpupower. The PPR may define more constraints.
6. **XNU microcode value 0xBA:** AMD's patch level is MSR 0x8B[31:0] (Linux reads EAX). XNU's `machdep.cpu.microcode_version` probably reads the high dword (the Intel convention), so 0xBA may not be the real AMD patch level. Check with a read-only `rdmsr` in the future kext.
7. **The Zen2 CCD = 2 CCX shift** is inferred, not stated by Linux.
8. **CC6 entry details:** whether the BIOS on this board uses `CstateAddr+1` for CC6, and whether the SMU auto-promotes MWAIT-C1 to CC6, are unknown.
9. **Kernel export status** of `rdmsr_carefully`, `mp_cpus_call`, `pmKextRegister` and similar remains open. That is covered by a separate task.
