# AMD Zen core C-states (CC1/CC6): register-level spec and a MachineIdle design

Status: research note for the AmdCpu kext. It covers how an OS puts AMD family 17h/19h/1Ah cores into CC6, how Linux and Xen do it, what this host's firmware exposes, and a concrete MachineIdle design for XNU.
Reference sources:
- torvalds/linux `master` at `fa4ad798f6f6` (v7.4-rc1), local copies in `deps/linux-src/`.
- AMD PPR 56214 rev 3.05 (Family 19h Model 21h B0, the host's Vermeer part) and PPR 55570 rev 3.16 (Family 17h Model 18h B1). Both were downloaded from amd.com into `/private/tmp/ppr56214` and `/private/tmp/ppr55570`, and their text was extracted with PDFKit.
- AMD BKDG 42301 rev 3.14 (Family 15h Models 00h-0Fh), at `/private/tmp/bkdg15h`. This is where the IO-based C-state interface was introduced. The public Zen PPRs keep MSRC001_0073 but do not describe the C-state actions.
- coreboot `main`, Xen `master` (`xen/arch/x86/acpi/cpu_idle.c`, last commit f9b72572c8d5, 2026-10-01), CoreFreq `master` (`x86_64/amd_reg.h`, `corefreqk.c`), ZenStates-Linux (`zenstates.py`), XNU `main` (`osfmk/i386/pmCPU.[ch]`, `rtclock.c`, `i386_timer.c`, `cpu_topology.h`). These were fetched into `/private/tmp/cstate-ref/`.
- SMCAMDProcessor / AMDRyzenCPUPowerManagement source (BSD-3, commit 029ef3a), at `deps/SMCAMDProcessor/src`.

Linux, coreboot, Xen and CoreFreq are GPL. Only facts and citations are recorded here, and no code is copied from them.

Source tags:
- **[V:source:function/line]**: VERIFIED. It was read in the fetched source or PDF named.
- **[PPR56214 p.N]**, **[PPR55570 p.N]**, **[BKDG15h §x]**: VERIFIED from the AMD document. The page number is the PDF page.
- **[R]**: RECALLED from memory or community reports and not verified against a primary source in this session.
- **[I]**: INFERRED from the facts around it.
- **[HOST]**: observed on this machine by read-only means, without root.

---

## 0. Summary for the implementer

1. **The deep C-state on Zen is CC6, reached by an IO-port read.** The read targets `CStateAddr + n`, where `CStateAddr` = MSRC001_0073[15:0]. The core traps the 8 ports `CStateAddr..+7` [PPR56214 p.167]. The read executes "C-state action" CCRn from MSRC001_0296, and CC6 happens when CCRn_CC6EN = 1. [CoreFreq amd_reg.h ZEN_CSTATE_CONFIG; ZenStates-Linux; BKDG15h §2.5.3.1–2.5.3.2]
2. **On AM4 boards the `_CST` C2 is "ACPI IOPORT 0x414"** with latency 18 µs (newer AGESA) or 400 µs (older). C3, when present, is 0x415. [R: Linux sysfs dumps on Arch/CachyOS/L1T forums] Together with the coreboot rule "C-state type n → CStateAddr + n − 1" [V:coreboot cpu_power_state.c:write_cstate_entry], this suggests AGESA programs CStateAddr = **0x413**, C2 = base+1 and C3 = base+2. [I] This is **not unique**. On 15h, offsets 2..7 all act as offset 2 [BKDG15h §2.5.3.2], so a base of 0x410 with C2 at 0x414 would also land inside the trap window, as IO C-state index 2. On this host the value must be read at runtime (§5), and the CCR index must be computed as `port − base`. Do not assume it is 1.
3. **Entry with IF=0 is correct and race-free.** An INTR wakes a core from an IO-read C-state even when EFLAGS.IF=0, and execution resumes at the instruction after the IN. With HLT and IF=0 the interrupt does **not** wake the core. [BKDG15h §2.5.3.4] Linux enters `io_idle()` with IRQs disabled. [V:linux processor_idle.c:acpi_idle_do_entry, io_idle]
4. **Do not perform the "dummy wait" PM-timer read** (Linux skips it on all non-Intel CPUs since v6.0), and **do not WBINVD**: Zen sets bm_check=1, bm_control=0, and hardware flushes the caches on CC6 entry. [V:processor_idle.c:io_idle l.521–552; cstate.c:acpi_processor_power_init_bm_check l.92–107; commit e400ad8b7e6a]
5. **MWAIT gives CC1 only.** CPUID 5 EDX = 0x11, so there is one C0 and one C1 sub-state [PPR56214 p.62–63; HOST]. Linux's FFH probe rejects any deeper hint. [V:cstate.c:acpi_processor_ffh_cstate_probe_cpu]
6. **The APIC timer and TSC keep running in CC6.** ARAT is fixed at 1, and "These timers do not vary in frequency regardless of the current P-state or C-state". TscInvariant is fixed at 1. [PPR56214 p.39, p.63, CPUID 8000_0007 EDX[8]] Linux sets ARAT for every family > 0x11 and NONSTOP_TSC from 8000_0007 EDX[8]. [V:amd.c:init_amd l.~1143, early_init_amd l.~632]
7. **Host ACPI tables could not be dumped without root.** On macOS 26 the IORegistry no longer publishes an "ACPI Tables" blob. The firmware generates the CPU `_CST` at runtime, so it is not stored as static AML in the BIOS image. See §5 for what was established and how to get the rest.

---

## 1. MSRs

### 1.1 Table

| MSR | Name | Scope | Layout | Source |
|---|---|---|---|---|
| C001_0073 | CStateBaseAddr | per thread (`_ccd[1:0]_lthree0_core[7:0]_thread[1:0]`), RW, reset 0 | `[15:0]` CstateAddr. 0 = no IO trapping. Values > FFF8h are undefined. Any other value makes the core trap IO **CstateAddr .. CstateAddr+7**. `[63:16]` reserved | **[PPR56214 p.167]**; same text in [PPR55570 p.144]; coreboot `MSR_CSTATE_ADDRESS`, mask 0xFFFF [V:coreboot src/include/cpu/amd/msr.h] |
| C001_0292 | PMGT_MISC (CoreFreq name) | package | `[32]` **PC6En** (1 = package C6 enabled). Also `[2:0]` CurPstateLimit, `[5:3]` StartupPstate, `[6]` DFPstateDis, `[14:7]` CurDFVid, `[20:15]` MaxCpuCof, `[25:21]` MaxDFCof, `[28:26]` CpbCap, `[30]` MaxCpuCofPlus50 | [V:CoreFreq amd_reg.h ZEN_PMGT_MISC]; bit 32 is also in [V:ZenStates-Linux zenstates.py l.77,107]. Not in the public PPRs |
| C001_0294 | CStatePolicy (CSTATE_POLICY) | per core, RW | `[1:0]` CC1_TMRSEL, `[6:2]` CC1_TMRLEN, `[8:7]` HYST_TMRSEL, `[13:9]` HYST_TMRLEN, `[20:14]` CFOH_TMRLEN, `[21]` CFOH_TMRSEL, `[23:22]` C1E_TMRSEL, `[28:24]` C1E_TMRLEN, `[29]` C1E_EN, `[38:32]` CFSM_DURATION, `[41:39]` CFSM_THRESHOLD, `[43:42]` CFSM_MISPREDACT, `[48:44]` IRM_DECRRATE, `[51:49]` IRM_BURSTEN, `[55:52]` IRM_THRESHOLD, `[59:56]` IRM_MAXDEPTH, `[60]` CIT_EN, `[61]` CIT_FASTSAMPLE, `[62]` CLT_EN | [V:CoreFreq amd_reg.h ZEN_CSTATE_POLICY]. Not in the public PPRs. "CFOH" = cache-flush-on-halt timer: a hardware CC1→CC6 promotion after a timeout (Andrew Cooper on xen-devel 2019-06, msg00667) |
| C001_0296 | CStateCfg (CSTATE_CONFIG) | per core, RW | **CCR0** `[5:0]` CC1DFSID, `[6]` **CC6EN**. **CCR1** `[13:8]` CC1DFSID, `[14]` **CC6EN**. **CCR2** `[21:16]` CC1DFSID, `[22]` **CC6EN**. `[38:32]` CCR0_CFOHTMR_LEN, `[39]` CCR0_CC1E_EN, `[46:40]` CCR1_CFOHTMR_LEN, `[47]` CCR1_CC1E_EN, `[54:48]` CCR2_CFOHTMR_LEN, `[55]` CCR2_CC1E_EN | [V:CoreFreq amd_reg.h ZEN_CSTATE_CONFIG]; CC6 mask `(1<<22)|(1<<14)|(1<<6)` [V:zenstates.py l.78,108]. Observed values: 0x080808 with BIOS C-states off, **0x484848** with them on (CC1DFSID = 8, CC6EN set in all three) [R: CoreFreq issue #195] |
| C001_0015 | HWCR | per thread | `[4]` **INVDWBINVD**, reset 1, "Check: 1". The PPR says it "is required to be set ... when ... CC6 is enabled". `[9]` MonMwaitDis. `[10]` MonMwaitUserEn. `[26]` EffFreqCntMwait | [PPR56214 p.~160 HWCR]. CoreFreq reports CC6 as enabled only if all three CC6EN bits **and** INVDWBINVD are set [V:corefreqk.c PerCore_Query_AMD_Zen_Features] |
| C001_0055 | IntPend | — | Listed as "Reserved" on 19h/21h | [PPR56214 p.164]. Linux reads it only for the K8/Fam10h E400 C1E bug [V:process.c:arch_post_acpi_subsys_init] |

Notes:
- **The PPR does not document 0292, 0294 or 0296.** The public 56214 and 55570 skip from C001_0074 to C001_0200 and from C001_024x to C001_0299. The layouts above come from CoreFreq, cross-checked by ZenStates-Linux (CC6 and PC6 bits) and by the 0x080808/0x484848 dumps. Treat every field other than CC6EN and PC6En as unverified.
- **Read-only access is safe for the kext.** It can use `rdmsr_carefully` (linkable via kpi.unsupported, see memory notes). Never write any of these MSRs. The BIOS options "Global C-state Control" and "Power Supply Idle Control" own them.

### 1.2 Which CCR index belongs to which port, HLT and MWAIT

- **Port to CCR (VERIFIED for 15h; Zen layout consistent with it).** An IO read of `CstateAddr + n` requests per-core IO-based C-state *n*. The index is the offset added to CstateAddr, and three such C-states (0, 1, 2) exist. On 15h, offsets 2..7 act as offset 2, and the IO read always returns 0. [BKDG15h §2.5.3.1, §2.5.3.2] The three CCR fields in C001_0296 match this 3-state model one to one. [I]
- **ACPI mapping.** The BKDG tells BIOS to publish `_CST` = {Register = CstateAddr+1, Type 2, Latency 100} and FADT P_LVL2 = CstateAddr+1, P_LVL2_LAT = 100, P_LVL3_LAT = 1001 (i.e. no FADT C3), PROC_C1 = 1, P_LVL2_UP = 1. [BKDG15h §2.5.3.6.1, §2.5.3.6.4] coreboot (Picasso/Cezanne) uses CstateAddr = 0x410 and emits C1 = FFH MWAIT (GAS class 1, bit_offset 2, hint 0), C2 = IO CstateAddr+1 and C3 = IO CstateAddr+2. [V:coreboot cpu_power_state.c:write_cstate_entry; cezanne/acpi.c cstate_cfg_table: C1 lat 1, C2 lat 0x12 (18), C3 lat 350; picasso: C1 lat 1, C2 lat 400; iomap.h ACPI_CSTATE_CONTROL = ACPI_IO_BASE(0x400)+0x10]
- **Why 0x413 on AGESA boards [I].** On AMD FCH the legacy ACPI P_BLK sits at 0x410. Linux computes FADT P_LVL2 = P_BLK+4 and P_LVL3 = P_BLK+5 [V:processor_idle.c:acpi_processor_get_power_info_fadt l.239–240], which gives 0x414 and 0x415. The BKDG requires P_BLK P_LVL2 = CstateAddr+1 [BKDG15h §2.5.3.6.4], which forces CstateAddr = P_BLK+3 = 0x413. Then base+1 = 0x414 and base+2 = 0x415, so the trapped ports overlay the legacy P_LVL2/P_LVL3 addresses. The 0x414/0x415 values seen in Linux `cpuidle/state*/desc` on AM4 fit this. [R] No primary source found in this session states 0x413 for any Zen board. A base of 0x410 would also trap 0x414/0x415, as offsets 4/5, which the 15h rule clamps to index 2. So the kext must derive k = port − base from the real values and never hard-code it.
- **The host bridge `_CRS` declares the trap window.** "BIOS must declare in the root host bridge _CRS object that the IO address range from MSRC001_0073[CstateAddr] to MSRC001_0073[CstateAddr]+7 is consumed by the host bridge." [BKDG15h §2.5.3.6.3] A DSDT dump (§5) can therefore cross-check the base without reading the MSR. The DSDT variants in the ROM contain no IO descriptor in 0x410–0x41F apart from 0x40B [HOST, critic scan], so the window, if declared, is inside a WordIO range.
- **HLT.** On 15h, "Executing the HLT instruction is equivalent to reading from the IO address specified by D18F4x128[HaltCstateIndex]". [BKDG15h §2.5.3.2] The public Zen PPRs have **no** equivalent field. Jan Beulich recalled that HLT "can be configured to do the same action as the I/O port read from one of the three ports" but could not find the section. Andrew Cooper found nothing about HLT apart from CFOH. [xen-devel 2019-06 msg00667] Xen still models fam17h as C1 = FFH MWAIT (latency 1) and C2 = **HLT** (latency 400). [V:xen cpu_idle.c:amd_cpuidle_init l.1414–1430] That table is used only for fam 17h and Hygon 18h–1Ah. AMD 19h/1Ah fall back to Dom0-supplied `_CST`. [same function, switch on family] **Which CCR (if any) HLT uses on Zen 2/3 is unknown.** [I: base+0/CCR0 is the most likely candidate, and CoreFreq's IO idle reads base+0 (corefreqk.c:CoreFreqK_IO_Handler)]
- **MWAIT.** CPUID 5 EDX = 0x0000_0011 is Fixed: MWaitC0SubStates = 1 and MWaitC1SubStates = 1, with nothing deeper. ECX = 3 (EMX, IBE). [PPR56214 p.62–63] So MWAIT hints above C1 are invalid. Linux's FFH probe derives the C-state type from hint bits [7:4]+1, rejects hints whose sub-state count is 0, and requires ECX EMX|IBE. [V:cstate.c:acpi_processor_ffh_cstate_probe_cpu l.124–167] **MWAIT does not directly request CC6.** Whether the CFOH/CC1 timers in C001_0294/0296 auto-promote an MWAIT-C1 or HLT core to CC6 is not documented publicly. [R: Cooper reads "the wording suggests mwait would also take this path"]

---

## 2. Semantics of IO-based entry

From [BKDG15h §2.5.3.4]. AMD carried the interface forward unchanged in name and MSR into 17h/19h [PPR56214 p.167]. Its behaviour on Zen is assumed to match. [I]

| Entry | IF | INTR arrives | Result |
|---|---|---|---|
| HLT | 0 | does **not** wake | stays halted (only NMI/SMI/INIT/STARTUP/STPCLK/RESET wake it) |
| HLT | 1 | wakes | vectors to the ISR |
| IN CstateAddr+n | **0** | **wakes** | resumes at the instruction **after the IN** with IF still 0 |
| IN CstateAddr+n | 1 | wakes | vectors to the ISR |

- **IF=0 behaves like MWAIT with ECX[0]=1.** The core wakes on a pending or arriving interrupt but does not take it until software sets IF. That is the property an idle loop needs to avoid lost wakeups. [I]
- **The read returns 0.** No dummy read or wait is needed on Zen.
  - Linux's `io_idle()` does a single `inb(addr)`. It then returns immediately when running as a guest (X86_FEATURE_HYPERVISOR) or when the vendor is not Intel. Only Intel still performs the follow-up `inl(PM_TMR)` "dummy wait". [V:processor_idle.c:io_idle l.521–552]
  - The restriction came from commit **e400ad8b7e6a** "ACPI: processor idle: Practically limit 'Dummy wait' workaround to old Intel systems" (author Dave Hansen, 2022-09-22, merged for v6.0-rc8). It was reported by AMD (K Prateek Nayak) because the `inl()` "can take thousands of cycles". Commit **fa583f71a99c** (2019) had already skipped it under a hypervisor.
  - Xen's `acpi_dead_idle` still does `inb(address); inl(pmtmr)` for AMD SYSIO. That path is only the offline loop. [V:xen cpu_idle.c l.~922–940]
- **CC6 entry actions** (15h description, mechanism similar on Zen [I]): optional P-state pop-down, L1/L2 flush, core state save to DRAM, and power gating. The time to enter and exit scales with core frequency. [BKDG15h §2.5.3.3.3]
- **SMT [I].** CC6 is per core, so a thread's IO request only reaches CC6 once the sibling thread is idle too. Until then the core resolves to the shallower common state. 15h has the same rule for compute units. [BKDG15h §2.5.4.1, Table 21] Requests are harmless while the sibling is busy.
- **HLT reaching CC6.** This is possible only if firmware maps HLT to a CCR with CC6EN set, or through CFOH promotion. Neither can be confirmed on this host without measurement; see §6.6.

---

## 3. Latency, residency, PC6, timers

- **Typical AM4 `_CST`** [R, community sysfs dumps]:
  - POLL.
  - C1: "ACPI FFH MWAIT 0x0", latency 1.
  - C2: "ACPI IOPORT 0x414", latency **18** (residency 36) on recent AGESA (e.g. 5700X3D on B550, 7840HS), or 400 on older firmware (2700X, 4650U).
  - C3: "ACPI IOPORT 0x415", latency 350, on some APUs and laptops only.
  - coreboot's Cezanne table uses the same 1/18/350. [V:coreboot cezanne/acpi.c]
- **Linux turns latency into residency** as `exit_latency = _CST latency` and `target_residency = latency × latency_factor`, with the default factor **2**. [V:processor_idle.c:acpi_processor_setup_cstates l.784–785, l.49] The governor picks the deepest state whose target_residency ≤ predicted idle and whose exit_latency ≤ the PM QoS limit. [R]
- **FADT limits (ACPI).**
  - P_LVL2_LAT > 100 µs means "C2 not supported".
  - P_LVL3_LAT > 1000 µs means "C3 not supported".
  - Linux enforces both with `ACPI_PROCESSOR_MAX_C2/C3_LATENCY`. [V:processor_idle.c:acpi_processor_get_power_info_fadt l.250–265]
  - `_CST` overrides the FADT values.
- **APIC timer in CC6.**
  - CPUID 6 EAX[2] ARAT is fixed at 1 [PPR56214 p.63; HOST eax=4]. §2.1.11 states that the TSC and the APIC timer "do not vary in frequency regardless of the current P-state or C-state". [PPR56214 p.39]
  - Linux unconditionally sets X86_FEATURE_ARAT for family > 0x11. [V:amd.c:init_amd l.~1141–1144]
  - With ARAT set, `lapic_timer_check_state()` returns early, so the ACPI C2/C3 states never get CPUIDLE_FLAG_TIMER_STOP and need no broadcast timer. [V:processor_idle.c l.132–153]
  - (Xen's 2019 doubt on this point predates its ARAT handling; Xen sets `local_apic_timer_c2_ok = true` for fam17h. [V:xen amd_cpuidle_init])
  - **The host has no TSC-deadline mode.** CPUID 1 ECX[24] = 0 (ecx = 7ef8320b, HOST), so XNU runs the LAPIC timer in one-shot mode, and ARAT covers that.
- **TSC across CC6.**
  - CPUID 8000_0007 EDX[8] TscInvariant is fixed at 1. [PPR56214, ApmInfoEdx] Linux maps it to CONSTANT_TSC and NONSTOP_TSC. [V:amd.c:early_init_amd l.~628–635]
  - `tsc_check_state()` therefore never marks the TSC unstable for C2/C3. [V:processor_idle.c l.193–212]
  - Also, `HWCR.TscFreqSel` = 1 means the TSC counts at P0. [PPR56214 HWCR] MPERF/APERF stop outside C0. Leave HWCR[26] EffFreqCntMwait = 0.
- **E400 / AMD_APIC_C1E** affects K8 rev ≥ 0x41 and Fam10h only. [V:amd.c:init_amd_k8 l.~744, init_amd_gh l.~788] Zen is not affected, so `prefer_mwait_c1_over_halt()` returns true on the host. [V:process.c l.877–908]
- **Package C6 (PC6)** needs PMGT_MISC[32] PC6En = 1, all cores in CC6, and the fabric/SMU agreeing (DF C-states, the "Power Supply Idle Control: Low Current Idle" BIOS option). [R] The OS has no direct control. It only maximizes simultaneous CC6 residency.
  - The FCH side has related settings: PMx07E CStateEn (C1eToC3En/C1eToC2En) and PMx0A0 MsgCState (FusionSerr/PerrEn makes FCH errors force a C-state exit; TimerTickChgMsgEn). [PPR55570 p.~500] Firmware owns all of these.

---

## 4. How Linux does it

| Piece | Behaviour | Citation |
|---|---|---|
| Default idle | `select_idle_routine()` installs `mwait_idle` (MONITOR on thread flags, then `__sti_mwait(0,0)`, i.e. STI;MWAIT EAX=0 ECX=0, then CLI) when MWAIT exists, there is no MONITOR or AMD_APIC_C1E bug, no `idle=` override, and either no EMX or C1 sub-states ≠ 0. Otherwise it uses `default_idle` (STI;HLT). The host gets mwait_idle | [V:arch/x86/kernel/process.c:prefer_mwait_c1_over_halt l.877, mwait_idle l.915, select_idle_routine l.938, default_idle l.766] |
| acpi_idle C-states | Built from `_CST` (falling back to FADT P_LVL2/3 at P_BLK+4/+5). Entry methods are FFH (MWAIT with `_CST` hint, ECX = MWAIT_ECX_INTERRUPT_BREAK), HALT (`acpi_safe_halt`: STI;HLT;CLI) and SYSTEM_IO (`io_idle`: `inb(port)`, IRQs **disabled**) | [V:processor_idle.c:acpi_idle_do_entry l.560–575; cstate.c:acpi_processor_ffh_cstate_probe l.170–200, _enter l.219] |
| AMD FFH C1 | Allowed since commit **5209654a46ee** (2017-06, "x86/ACPI/cstate: Allow ACPI C1 FFH MWAIT use on AMD systems", Yazen Ghannam, about v4.13). The message says BIOS historically had no FFH C1 and Linux used HALT, and that it needs a BIOS FFH C1 plus non-zero CPUID 5 EDX. `ffh_cstate_init()` accepts Intel, AMD and Hygon | [V:cstate.c:ffh_cstate_init l.230; GitHub commit 5209654a46ee] |
| C3 type on Zen | `bm_check=1, bm_control=0`: no WBINVD (`ACPI_FLUSH_CPU_CACHE` skipped) and no ARB_DIS. Then `acpi_idle_enter_bm()` only checks BM_STS and demotes to the safe state if bus-master activity is seen (unless `bm_sts_skip`) | [V:cstate.c:acpi_processor_power_init_bm_check l.92–107; processor_idle.c:acpi_idle_enter_bm l.616–675, acpi_idle_enter l.677–704] |
| C2 type | `acpi_idle_enter` enters directly, with no BM check and no flush. It is demoted to C1 only if there is no `_CST` and FADT lacks C2_MP_SUPPORTED on SMP | [V:processor_idle.c:acpi_idle_enter, acpi_idle_fallback_to_c1 l.600] |
| Dummy wait | Intel only (see §2) | [V:io_idle] |
| Governors | **menu**: predicted idle = next timer event × a correction factor (bucketed by expected length and iowait), capped by a detected repeating interval from the last 8 idle durations. Picks the deepest state with residency ≤ prediction and latency ≤ QoS. **teo**: per-state bins of "hits" (woke at the timer) vs "intercepts" (woken earlier by another event). If intercepts dominate, it picks a shallower state | [R: drivers/cpuidle/governors/menu.c, teo.c, not in deps] |
| CoreFreq alternative | Its own cpuidle driver uses `inb CStateBaseAddr+0` for its "I/O" states when HWCR.MonMwaitDis = 0 and CStateBaseAddr ≠ 0, falling back to HLT if the base is 0 | [V:corefreqk.c:CoreFreqK_IO_Handler l.~21549, idle route selection l.~22110–22190] |

---

## 5. Host ACPI and firmware findings (Ryzen 7 5700X, AMI Aptio "ALASKA", OpenCore boot)

**What was tried [HOST]:**
- `ioreg -lw0 -p IODeviceTree -n acpi -r` and the IOService/IOACPIPlane equivalents: no "ACPI Tables" dictionary exists on macOS 26.7.2.
  - `AppleACPIPlatformExpert` exposes only "ACPI Statistics" ({MethodCount 233, SciCount 0, GpeCount 0}).
  - The processor objects are `IOACPIPlane:/acpi/C000@0 .. C00F@F` (`AppleACPICPU`, `plugin-type`=1 from SSDT-PLUG) and carry no `_CST` data.
  - `/private/tmp/acpi-dump` was created but holds no live tables.
  - Live tables need root (or a firmware or OpenCore dump), which this session does not use.
- **OpenCore log** `/Volumes/FD14-LITE/opencore-2026-11-05-053743.txt` lists the 21 firmware tables:
  - FACP is 276 bytes (an ACPI 6.x-sized FADT) and the DSDT is 24656 bytes.
  - SSDT OEM table IDs: "AmdTable" ×4 (36073, 15542 "AOD", 16451, 191 bytes), **"CPUSSDT" 252 bytes**, "MYRTLE" ×2 (CPM), and others. No contents were logged.
- **BIOS images** on the same volume were scanned read-only with `python3 -I` (`/private/tmp/acpi-scripts/scan_rom.py`, output in `/private/tmp/acpi-dump/backup/`). The images are `BACKUP.ROM`, `B45MH4_10.60` and `ModBIOS.rom`, each 16 MiB.
  - Decompressing the LZMA GUID-defined sections yields 5 DSDT variants (23982–24931 bytes; none is 24656, so AMI patches the DSDT at runtime) and AMD ALIB/CPM/AOD SSDTs.
  - **No `_CST`, `_CSD` or AML `_PSS` NameSeg exists anywhere in the image.** The `_CST` hits are only setup strings such as `FEATURE_CSTATE_BOOST` and `FEATURE_DF_CSTATES`.
  - The 252-byte "CPUSSDT" is generated by AMI **CpuDxe** (strings `\_SB`, `PLTF`, `_CID`, `_UID`, `ALASKA`, `CPUSSDT`). It is the processor-device declaration table, not a C-state table. [I]
  - The C-state SSDT is built by AGESA **AmdAcpiDxe** (in the image as `AmdSocAm4VmrDxe`). Its debug strings include "Start to create Processor SSDT", "PcdAmdAcpiCstC3=0x%x", "Set PcdAmdAcpiCstC3 to TRUE/FALSE", "C3 disable, take C3 worst case latency" and "Processor SSDT is created". The data comes from `gAmdAcpiCpuSsdtServicesProtocol`. So the host's `_CST` is the 16451-byte "AmdTable" SSDT [I], generated at boot, with C1 FFH + C2 IO and a C3 IO **only if** `PcdAmdAcpiCstC3` is TRUE.
  - Also present is the setup knob string `PcdAmdCStateMode=%d`.

**Result:**
- No `_CST` object or FADT P_LVL values could be decoded on this host.
- **The exact IO ports, types and latencies remain unknown.**
- The expected values, from AGESA practice and AM4 Linux reports [R/I], are C1 = FFH MWAIT hint 0 with latency 1, C2 = SystemIO **0x414** byte with latency 18 (or 400), and C3 = SystemIO 0x415 with latency 350 (if present).
- The expected FADT values are P_LVL2_LAT ≤ 100 or 101, P_LVL3_LAT ≥ 1001 and PROC_C1 = 1. [BKDG15h §2.5.3.6.4, R]

**Ways to get the real values without breaking the project rules:**
1. Have the kext publish **read-only** sysctls for CStateBaseAddr (all threads), CStateCfg, CStatePolicy, PMGT_MISC and HWCR, read with `rdmsr_carefully`. This is the authoritative source for the runtime check anyway (§6.5).
2. OpenCore `Misc → Debug → SysReport = true` (DEBUG build). It writes `SysReport/ACPI/*.aml` to the EFI volume, which `FD14-LITE` already mounts as user-writable, and those files can be decoded without root.
3. From the Windows install on `/Volumes/Untitled`, or a Linux live USB, read `/sys/devices/system/cpu/cpu0/cpuidle/state*/{desc,latency,residency}` and `acpidump`.

**Side observation.** The loaded AMDRyzenCPUPowerManagement source (BSD) builds with `PMRYZEN_IDLE_IO_CSTATE`. Its MachineIdle does `cli`, then `sti; inw $0xf2,%ax; cli`. [V:deps/SMCAMDProcessor/src/AMDRyzenCPUPowerManagement/pmAMDRyzen.c:pmRyzen_machine_idle l.246–295; project.pbxproj l.828,876] Port 0xF2 is not a C-state port. If the shipped binary matches this source, that hook never reaches CC6 and likely returns at once (a busy idle loop). [I] This should be checked against the binary in `deps/AMDRyzenCPUPowerManagement-bin` before relying on it.

---

## 6. Recommendation: XNU `MachineIdle` for this host

### 6.1 XNU contract (VERIFIED from xnu `main`, close to the host's xnu-12377.161)

- `machine_idle()` handles pending soft timers, marks the CPU idle, then calls `pmDispatch->MachineIdle(0x7FFFFFFFFFFFFFFF)`. The argument is always "infinite", so the kext must compute the predicted idle itself. Without a PM kext it does `pal_hlt()` (STI;HLT) and then `pal_cli()`. At `machine_idle_exit` it calls `pal_sti()`. **MachineIdle is therefore entered with IF=0 and should return with IF=0.** [V:osfmk/i386/pmCPU.c:machine_idle l.103–215] The caller `processor_idle()` does `(void)splsched()` before `machine_idle()`, and the comment there reads "returns with interrupts enabled". [V:xnu-12377.121.6 osfmk/kern/sched_prim.c:processor_idle]
- `pmKextRegister(PM_DISPATCH_VERSION=102, …)` fills the `pmCallBacks_t`.
  - It **panics** on a version mismatch, and on re-registration ("Attempt to re-register power management interface--AICPM present in xcpm mode?").
  - So the kext must refuse to register when another PM kext (AMDRyzenCPUPowerManagement) has already registered. [V:pmCPU.c:pmKextRegister l.964–1012; pmCPU.h l.40–118]
- **Wakeups.** `pmCPUExitIdle()` sends the IPI when `exitIdle` is NULL or returns TRUE. Keep `exitIdle` NULL (always IPI): an IN-based C-state wakes on any INTR. [V:pmCPU.c l.433–446]
- **Next timer.** `setPop()` records `x86_lcpu()->rtcDeadline` (requested deadline, absolute ns = mach_absolute_time on x86) and `rtcPop` (programmed pop). [V:rtclock.c:setPop l.461–481; cpu_topology.h x86_lcpu_t]
  - The kext gets its lcpu through `callbacks->GetMyLCPU()`.
  - Callbacks for `InterruptPending` (`lapic_is_interrupt_pending`) and `GetNanotimeInfo` are also provided.
  - The `x86_lcpu_t` field offsets are private ABI. Verify them against the running kernel (SymProbe) before trusting `rtcDeadline`.
  - In xnu-12377.121.6 (identical to `main`), the layout is: 8 pointers (0–63), then `flags`, `cpu_num`, `lnum`, `pnum`, `master`, `primary`, `state` and `stopped`, each 4 bytes (64–95). After them come **`rtcPop` @ 96** and **`rtcDeadline` @ 104**, both uint64_t, then `caches[]`, `pmStats` and `pmState`. [V:cpu_topology.h x86_lcpu_t; offsets computed by the critic for LP64, not compiled]
  - `rtcDeadline` = EndOfAllTime when the timer is cleared (setPop with 0 or EndOfAllTime). `timer_intr()` also sets it to EndOfAllTime just before `timer_resync_deadlines()`. [V:rtclock.c:setPop l.461–481; i386_timer.c:timer_intr l.164]
- **Leave GetDeadline, SetDeadline and Deadline NULL.** If SetDeadline is set, the kext takes over timer arbitration. [V:i386_timer.c l.151, l.227–240]

### 6.2 Instructions

| Depth | Instruction sequence (entered with IF=0) | Wake semantics | Return |
|---|---|---|---|
| Shallow (CC1) | **Option A:** `sti; hlt; cli`. STI's one-instruction shadow makes the HLT start before any interrupt is taken, which closes the lost-wakeup window. The ISR runs inside, then CLI. **Option B (preferred):** MONITOR a per-CPU line, then `mwait` with EAX=0 (hint C1) and **ECX=1** (interrupt break with IF=0). The ISR runs after we return and XNU does STI | B wakes with IF=0, so exit timestamps are taken before the ISR. B also allows a future `exitIdle` that writes the monitored line instead of sending an IPI | return 0 with IF=0 |
| Deep (CC6) | `inb %dx,%al` with DX = CStateAddr + k, where k ∈ {0,1,2} is the CCR index whose CC6EN = 1. Prefer k = (`_CST` C2 port − CStateAddr) when the `_CST` is known, and clamp it to 2. Otherwise use k = 1. With the commonly reported 0x484848 all three CCRs have CC6EN, so the choice matters less. Never issue an IN outside base..base+2. **No** following PM-timer read and **no** WBINVD | Any INTR, NMI or SMI wakes it. With IF=0, execution continues after the IN. A pending interrupt makes it return at once, so there is no race | return with IF=0. XNU then STIs and the interrupt is serviced |

Do **not** use MWAIT hints > 0, which CPUID 5 forbids. Do not use HLT with IF=0, which never wakes.

### 6.3 Choosing deep vs shallow

The steps run in this order on every idle entry:
1. `now = mach_absolute_time()`. `next = lcpu->rtcDeadline`, which is EndOfAllTime when no timer is armed. `timer_ns = next - now`.
2. **Predicted idle** = min(`timer_ns`, a history estimate). For the history estimate, keep per-CPU the last 8 measured idle durations, menu-style. If at least 6 of the 8 are below the threshold, or their typical interval is below it, predict that typical interval. A cheaper teo-like alternative counts per CPU the "early wakes" (woken before `rtcDeadline − threshold`) against "timer wakes", and demotes to shallow while early wakes exceed 50% of the last 16 deep entries.
3. **Deep if** predicted ≥ `deep_residency_ns` **and** the urgency/real-time state allows it. Use `callbacks->ThreadGetUrgency`, or the `pmThreadTellUrgency` hook, to stay shallow while RT threads are active, and also honour `getMaxIntDelay`/`setMaxIntDelay`. **Else shallow.**
4. **Default `deep_residency_ns`.** Start conservatively at **200 µs**, as a sysctl tunable. Linux would use 2 × 18 µs = 36 µs if the BIOS `_CST` says 18. Once the real `_CST` latency is known (§5), set the residency to `max(2 × latency, 50 µs)`. The exit latency of CC6 on Zen 2/3 is tens of µs. [R]
5. **Count and export** entries, early wakes and residency per depth (TSC delta) via sysctl. This is needed to tune the threshold and to show that CC6 is reached (§6.6).

### 6.4 What to do on return

- Read the TSC and account the idle duration (for P-state load estimation, which the governor needs because MPERF stops in CC6).
- Do **not** STI. XNU's `machine_idle_exit` does it.
- Do not touch caches, ARB_DIS, BM_STS or the PM timer.
- Return 0.

### 6.5 Runtime safety gate (all must pass before deep idle is enabled; otherwise use shallow-only)

1. `CPUID 0` vendor is AuthenticAMD (or HygonGenuine), family ∈ {17h, 19h, 1Ah}, and **not** running under a hypervisor (CPUID 1 ECX[31] = 0, `kern.hv_vmm_present` = 0). Under a hypervisor the IN becomes a VM exit with undefined C-state semantics. Host: hv_vmm_present = 0. RyzenHV is a guest-hypervisor provider, not a host VMM. [HOST]
2. **CStateBaseAddr.** Read C001_0073 on **every** logical CPU (`mp_rendezvous_no_intrs` + `rdmsr_carefully`). Require no #GP, `base = v & 0xFFFF` with `0 < base ≤ 0xFFF8`, upper bits zero, and **the same value on all CPUs**. If base = 0 (BIOS "Global C-state Control" disabled), an IN to 0x414 would reach the FCH ACPI P_LVL2 register instead of the core trap. That case must never be entered. [PPR56214 p.167; I]
3. **CStateCfg.** Read C001_0296 on every CPU (once per core is enough, but all is simpler). Require `rdmsr_carefully` success, an identical value across cores, and **CC6EN set for the chosen CCR k** (bit 6/14/22 for k = 0/1/2). Prefer k = 1. If CCR1_CC6EN = 0 but CCR2_CC6EN = 1, k = 2 is allowed. If none has CC6EN, use shallow only. A deep entry with CC6EN = 0 probably gives only CC1 with a clock divider, which is safe but pointless. [I]
4. **HWCR.** Bit 4 INVDWBINVD = 1 on all CPUs (a PPR requirement when CC6 is enabled). For the MWAIT shallow option, bit 9 MonMwaitDis = 0 and CPUID 1 ECX[3] = 1.
5. **Timers.** CPUID 6 EAX[2] ARAT = 1 (no broadcast timer exists in XNU) and CPUID 8000_0007 EDX[8] TscInvariant = 1.
6. **No other PM dispatch registered.** `pmDispatch` is a file-`static` pointer in pmCPU.c with no getter, so it cannot be read directly. Detect a loaded `wtf.spinach.AMDRyzenCPUPowerManagement` (OSKext / IORegistry) or any other known PM kext, and stay monitor-only if one is found, because pmKextRegister panics on re-registration. Optionally use a SymProbe lookup of the local symbol `_pmDispatch`, if the kernel collection keeps it. Calling `pmKextRegister(102, NULL, &callbacks)` (cpuFuncs = NULL) only fills the callbacks and never panics on re-registration. It still panics on a version mismatch or callbacks = NULL. [V:pmCPU.c:pmKextRegister]
7. **Kill switches.** A boot-arg (e.g. `amdzen_idle=0`, or `=1` for shallow only) checked before registration, plus a sysctl to drop to shallow at runtime. Keep the dispatch registered and only change the policy flag, since unregistering at runtime is risky.
8. **Canary.** On first enable, do a few deep entries on one CPU with a short armed timer, and check that each returns within `timer_ns` + 1 ms and that wakeups occur. If any entry overshoots by more than 10 ms, disable deep idle and log the failure.

### 6.6 How to prove CC6 is being reached

Package power reading is already implemented, so measure it with the RAPL core energy counter (C001_029A, summed per core) at idle under three conditions:
1. Stock XNU: STI;HLT, no hook.
2. The shallow-only hook.
3. Deep (IN base+1).

The host currently reads about 6 W cores / 27 W package at idle with stock HLT. [HOST: `kern.amdzen.cores_power_mw` = 5990, `pkg_power_mw` = 27260] Working CC6 should cut the core power several-fold. [R] Comparing (1) with (3) also tells whether this firmware maps HLT to a CC6-enabled action.

---

## 7. Open risks and unknowns

1. **No live `_CST` or FADT on this host.** The port (0x414?), the latencies, and whether C3/0x415 exists are unverified. Mitigation: use the runtime MSR gate (§6.5) and the read-only sysctls or OC SysReport (§5).
2. **The CCR mapping on Zen is undocumented publicly.** The C001_0294/0296/0292 layouts come from CoreFreq and ZenStates. Which CCR HLT or MWAIT uses, and whether CFOH promotes CC1 to CC6, is unknown. CCR0's purpose is unknown, so do not use base+0 until it is measured.
3. **XNU private ABI.** The `x86_lcpu_t` layout (`rtcDeadline` offset), `PM_DISPATCH_VERSION` 102 and the `pmDispatch_t` field order were checked against the public xnu `main`, but the host kernel is 12377.161 with AMD patches applied by OpenCore. Confirm with SymProbe and size checks. A version mismatch panics.
4. **Exit latency on 5700X is unmeasured.** CC6 exit with L1/L2 refill can hurt latency-sensitive audio and USB work. Keep the threshold conservative and make it tunable, and gate on RT urgency.
5. **IO-read semantics are verified only for Fam15h** (BKDG §2.5.3.4: IF=0 wake after IN). Zen behaviour is inferred from Linux and CoreFreq practice, both of which enter with IRQs off and work.
6. **Firmware variance.** CStateAddr may differ across BIOS versions (0x413 vs 0x410). The modded BIOS may have changed C-state defaults. Always read C001_0073 and never hard-code the port.
7. **The AMDRyzenCPUPowerManagement IO path reads port 0xF2** (source). That kext must be unloaded before testing, both because of the re-registration panic and because its idle behaviour would confuse any measurement.

---

## Critic notes

Independent re-check by a second reviewer. Sources were re-fetched into `/private/tmp/critic-cstate/dl/` (GitHub raw, git.kernel.org, xnu tag `xnu-12377.121.6`), and the AMD PDFs' extracted text was re-read.

**Verified and correct as written:**
1. **C001_0073** layout `[15:0]` CstateAddr: 0 = no trapping, > FFF8h undefined, traps CstateAddr..+7. [PPR56214 3.05 p.167 text] The scope string was corrected to `_ccd[1:0]_lthree0_core[7:0]_thread[1:0]`.
2. **C001_0296 / 0294 / 0292 bit layouts.**
   - The re-fetched CoreFreq `x86_64/amd_reg.h` is byte-identical to the author's copy.
   - Every bit position in the §1.1 table matches it. CoreFreq names the CCR2 field `CCR2_CC2DFSID`, a cosmetic difference.
   - ZenStates-Linux (also identical) uses CC6 mask bits 6/14/22 (l.78/108) and PC6 bit 32 (l.77/107).
   - None of these MSRs is in the public PPR, so they remain community-sourced.
3. **IF=0 wake semantics for IO-read entry.** The four-way HLT/IO × IF table matches BKDG 42301 rev 3.14 §2.5.3.4 verbatim in substance. §2.5.3.2 confirms "processor always returns 0" and "Offsets 2 through 7 result in an offset of 2". §2.5.3.6.1 and §2.5.3.6.4 confirm `_CST` Register = base+1, Type 2, Lat 100, plus FADT P_LVL2_LAT = 100, P_LVL3_LAT = 1001 and P_LVL2 = base+1.
4. **PPR56214 facts.**
   - CPUID 5 ECX = 3 and EDX = 0x11, both Fixed (p.62–63).
   - ARAT is Fixed 1 (p.63).
   - §2.1.11: the timers "do not vary in frequency regardless of the current P-state or C-state".
   - TscInvariant is Fixed 1.
   - HWCR[4] INVDWBINVD has reset 1, "Check: 1", and is required when "CC6 is enabled". HWCR[9] is MonMwaitDis.
5. **XNU contract.**
   - `PM_DISPATCH_VERSION 102`.
   - `MachineIdle(0x7FFFFFFFFFFFFFFF)`.
   - pmKextRegister panics on a version mismatch or callbacks = NULL, and on re-registration.
   - `pmCPUExitIdle` sends the IPI when `exitIdle` is NULL.
   - `setPop` records rtcDeadline and rtcPop.
   - `processor_idle()` calls `splsched()` before `machine_idle()`.
   - `pmCPU.h` and `cpu_topology.h` are **identical** between xnu `main` and tag `xnu-12377.121.6`. pmCPU.c differs by one identifier (`boot_cpu_id` vs `master_cpu`).
6. **Linux.**
   - `io_idle()` has one `inb` and returns early for a hypervisor or non-Intel CPU (l.521–552).
   - Zen gets `bm_check = 1, bm_control = 0` (cstate.c l.92–107).
   - FADT P_LVL2/3 = P_BLK+4/+5, and the C2/C3 latency caps are as stated.
   - `latency_factor = 2`.
   - Commit e400ad8b7e6a exists as described (author Dave Hansen, Reported-by K Prateek Nayak).
   - Commit 5209654a46ee exists as described (Yazen Ghannam, 2017-06-07).
   - The re-fetched files are byte-identical to `deps/linux-src`.
7. **coreboot.** `write_cstate_entry` uses IO addr = base + ctype − 1 (byte access), with C1 as FFH MWAIT. `set_cstate_io_addr` writes ACPI_IO_BASE(0x400)+0x10 = 0x410. The Cezanne latencies are 1 / 0x12 / 350.
8. **Xen.** `amd_cpuidle_init`: the fam17h table is C1 FFH (lat 1) + C2 HALT (lat 400). Family 18h/19h/1Ah take it only for Hygon, and every other family returns with `vendor_override = -1`.
9. **AMDRyzenCPUPowerManagement.** `pmRyzen_machine_idle`, under `PMRYZEN_IDLE_IO_CSTATE`, does `sti; inw $0xf2,%ax; cli`, as stated.

**Corrected or added:**
- §0.2 and §1.2: the 0x413 base is an inference and not unique. Base 0x410 with C2 at 0x414 is also consistent, because of the offset ≥ 2 clamp. Added the BKDG P_LVL2 = base+1 argument for 0x413 and the §2.5.3.6.3 host-bridge `_CRS` rule as a cross-check.
- §6.2: k is now derived as `_CST port − base`, clamped to {0,1,2}, instead of a fixed k = 1.
- §6.1: added the verified `processor_idle` → `splsched` caller evidence, computed `x86_lcpu_t` offsets (rtcPop @ 96, rtcDeadline @ 104), and the `rtcDeadline` = EndOfAllTime semantics.
- §6.5.6: `pmDispatch` is file-static and cannot be "detected" directly. Gave indirect methods and noted that `pmKextRegister(102, NULL, cb)` is a non-panicking way to obtain callbacks.
- §6.5.3: "CC6EN = 0 gives CC1" is now tagged [I].
- §2: added the author of e400ad8b7e6a.

**Still unverified (residual risk):**
- The host's actual C001_0073 value, `_CST` ports/latencies and FADT. Nothing in this session reads them.
- CCR semantics on Zen (what CC1DFSID/CFOH do, and which CCR HLT maps to). There is no public AMD document. Community dumps 0x080808 / 0x484848 [R].
- IF=0 wake on IO-read entry is documented only for Fam15h. Zen behaviour is inferred.
- The host kernel is xnu-12377.**161**, newer than the latest public tag (.121.6). The x86_lcpu offsets were computed by hand, not compiled, and must be confirmed by SymProbe or a `offsetof` static_assert against the kext's own copy of the header.
- `pal_hlt()` = STI;HLT is recalled from the XNU `pal_routines` source and was not re-fetched.
- The PPR text was extracted from the author's `56214.zip`. The zip lists `56214-B0-PUB_3.05_PPR.pdf`, dated 2021-06-08, but the critic did not re-download it from amd.com.
