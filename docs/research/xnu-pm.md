# XNU x86 CPU power-management dispatch (`pmDispatch` / `pmKextRegister`)

Status: research note, written 2026-11-08, for the AMDZenPM CC6 idle hook. Target host: macOS 26.7.2, Darwin 25.6.0, `xnu-12377.161.15.701.48~2/RELEASE_X86_64`, Ryzen 7 5700X (19h/21h), `machdep.xcpm.mode=0`.

**Source used.** Apple has not published `xnu-12377.161.15`. The newest tag on `github.com/apple-oss-distributions/xnu` is **`xnu-12377.121.6`**, and `main` is `xnu-12377.1.9`. Every source line number below is from `xnu-12377.121.6` unless a tag is named. The fetched files are kept as reference data in `deps/xnu-src/<tag>/`. XNU is APSL-2.0, not GPL. Even so, the structs here are written out as field tables, not pasted code.

**What the running kernel shows.** `/System/Library/Kernels/kernel` is the same build as the running kernel (`uname -v` gives the same xnu string). I disassembled it with `otool -tV`, read-only, and checked the facts that matter against it. Its `pmCPU.c` is about 51 lines longer than the 12377.121.6 file (the `__LINE__` in the panic strings is 1047/1052, against 996/1001 in the OSS file). Every offset and branch I checked still matches the OSS code.

Tags:
- **[X:file:func:Lnn]**: VERIFIED. Read in fetched XNU source.
- **[BIN]**: VERIFIED. Read in the disassembly of this host's kernel or kernel collections (KCs).
- **[HOST]**: VERIFIED. Read-only observation on this machine (sysctl, kmutil, ls, plutil).
- **[RECALLED]**: not checked in this session.

---

## 1. Interface definition (`osfmk/i386/pmCPU.h`)

`PM_DISPATCH_VERSION` = **102** [X:pmCPU.h:L40]. [BIN] confirms it: `pmKextRegister` starts with `cmpl $0x66, %edi`.

### 1.1 `pmDispatch_t` (kext → kernel table; 29 pointers, 0xE8 bytes) [X:pmCPU.h:L52-83]

The offsets marked ✓ were confirmed in the host kernel binary, where the C wrappers load `off(%reg)` from `pmDispatch` [BIN].

| # | Off | Field | Prototype |
|---|---|---|---|
| 0 | 0x00 ✓ | `pmCPUStateInit` | `kern_return_t (*)(void)` |
| 1 | 0x08 ✓ | `cstateInit` | `void (*)(void)` |
| 2 | 0x10 ✓ | `MachineIdle` | `uint64_t (*)(uint64_t maxIdleDuration)` |
| 3 | 0x18 ✓ | `GetDeadline` | `uint64_t (*)(x86_lcpu_t *lcpu)` |
| 4 | 0x20 ✓ | `SetDeadline` | `uint64_t (*)(x86_lcpu_t *lcpu, uint64_t)` |
| 5 | 0x28 ✓ | `Deadline` | `void (*)(x86_lcpu_t *lcpu)` |
| 6 | 0x30 ✓ | `exitIdle` | `boolean_t (*)(x86_lcpu_t *lcpu)` |
| 7 | 0x38 ✓ | `markCPURunning` | `void (*)(x86_lcpu_t *lcpu)` |
| 8 | 0x40 ✓ | `pmCPUControl` | `int (*)(uint32_t cmd, void *datap)` |
| 9 | 0x48 ✓ | `pmCPUHalt` | `void (*)(void)` |
| 10 | 0x50 | `getMaxSnoop` | `uint64_t (*)(void)` |
| 11 | 0x58 ✓ | `setMaxBusDelay` | `void (*)(uint64_t time)` |
| 12 | 0x60 | `getMaxBusDelay` | `uint64_t (*)(void)` |
| 13 | 0x68 ✓ | `setMaxIntDelay` | `void (*)(uint64_t time)` |
| 14 | 0x70 | `getMaxIntDelay` | `uint64_t (*)(void)` |
| 15 | 0x78 ✓ | `pmCPUSafeMode` | `void (*)(x86_lcpu_t *lcpu, uint32_t flags)` |
| 16 | 0x80 ✓ | `pmTimerStateSave` | `void (*)(void)` |
| 17 | 0x88 ✓ | `pmTimerStateRestore` | `void (*)(void)` |
| 18 | 0x90 ✓ | `exitHalt` | `kern_return_t (*)(x86_lcpu_t *lcpu)` |
| 19 | 0x98 ✓ | `exitHaltToOff` | `kern_return_t (*)(x86_lcpu_t *lcpu)` |
| 20 | 0xA0 ✓ | `markAllCPUsOff` | `void (*)(void)` |
| 21 | 0xA8 ✓ | `pmSetRunCount` | `void (*)(uint32_t count)` |
| 22 | 0xB0 | `pmIsCPUUnAvailable` | `boolean_t (*)(x86_lcpu_t *lcpu)` |
| 23 | 0xB8 ✓ | `pmChooseCPU` | `int (*)(int startCPU, int endCPU, int preferredCPU)` |
| 24 | 0xC0 ✓ | `pmIPIHandler` | `int (*)(void *state)` |
| 25 | 0xC8 ✓ | `pmThreadTellUrgency` | `void (*)(int urgency, uint64_t rt_period, uint64_t rt_deadline)` |
| 26 | 0xD0 ✓ | `pmActiveRTThreads` | `void (*)(boolean_t active)` |
| 27 | 0xD8 ✓ | `pmInterruptPrewakeApplicable` | `boolean_t (*)(void)` |
| 28 | 0xE0 ✓ | `pmThreadGoingOffCore` | `void (*)(thread_t old_thread, boolean_t transfer_load, uint64_t last_dispatch, boolean_t thread_runnable)` |

### 1.2 `pmCallBacks_t` (kernel → kext; 27 slots, 0xD8 bytes) [X:pmCPU.h:L96-124]

`pmKextRegister` fills every slot. The stores to `0x00..0xD0(%rdx)` were confirmed in [BIN]. The "kernel implementation" column is from [X:pmCPU.c:pmKextRegister:L968-994].

| # | Off | Field | Type | Kernel implementation |
|---|---|---|---|---|
| 0 | 0x00 | `setRTCPop` | `uint64_t (*)(uint64_t time)` | `setPop`. Programs the LAPIC deadline and records `lcpu.rtcDeadline/rtcPop` [X:rtclock.c:setPop:L461-481] |
| 1 | 0x08 | `resyncDeadlines` | `void (*)(int cpu)` | `pmReSyncDeadlines`. On first use it registers `timer_resync_deadlines` as `mp_PM_func`. It then runs locally or sends `MP_CALL_PM` [L908-923] |
| 2 | 0x10 | `initComplete` | `void (*)(void)` | `pmInitComplete`. **Sets `pmInitDone = 1`** [L301-311] |
| 3 | 0x18 | `GetLCPU` | `x86_lcpu_t *(*)(int cpu)` | `pmGetLogicalCPU` |
| 4 | 0x20 | `GetCore` | `x86_core_t *(*)(int cpu)` | |
| 5 | 0x28 | `GetDie` | `x86_die_t *(*)(int cpu)` | |
| 6 | 0x30 | `GetPackage` | `x86_pkg_t *(*)(int cpu)` | |
| 7 | 0x38 | `GetMyLCPU` | `x86_lcpu_t *(*)(void)` | `&current_cpu_datap()->lcpu` |
| 8 | 0x40 | `GetMyCore` | `x86_core_t *(*)(void)` | |
| 9 | 0x48 | `GetMyDie` | `x86_die_t *(*)(void)` | |
| 10 | 0x50 | `GetMyPackage` | `x86_pkg_t *(*)(void)` | |
| 11 | 0x58 | `GetPkgRoot` | `x86_pkg_t *(*)(void)` | `x86_pkgs` |
| 12 | 0x60 | `LockCPUTopology` | `void (*)(int lock)` | `x86_topo_lock` spin lock |
| 13 | 0x68 | `GetHibernate` | `boolean_t (*)(int cpu)` | `cpu_datap(cpu)->cpu_hibernate` |
| 14 | 0x70 | `LCPUtoProcessor` | `processor_t (*)(int lcpu)` | |
| 15 | 0x78 | `ThreadBind` | `processor_t (*)(processor_t proc)` | `thread_bind` |
| 16 | 0x80 | `GetSavedRunCount` | `uint32_t (*)(void)` | |
| 17 | 0x88 | `pmSendIPI` | `void (*)(int cpu)` | `lapic_send_ipi(cpu, LAPIC_PM_INTERRUPT)` |
| 18 | 0x90 | `GetNanotimeInfo` | `void (*)(pm_rtc_nanotime_t *)` | Copies the TSC→ns parameters using a generation-count retry loop |
| 19 | 0x98 | `ThreadGetUrgency` | `int (*)(uint64_t *rt_period, uint64_t *rt_deadline)` | **Returns `THREAD_URGENCY_NONE` from xnu-8019 onwards.** xnu-7195 returned the real urgency |
| 20 | 0xA0 | `timerQueueMigrate` | `uint32_t (*)(int cpu)` | |
| 21 | 0xA8 | `RTCClockAdjust` | `void (*)(uint64_t adjustment)` | `rtc_clock_adjust` |
| 22 | 0xB0 | `topoParms` | `x86_topology_parameters_t *` (data pointer) | `&topoParms` |
| 23 | 0xB8 | `InterruptPending` | `boolean_t (*)(void)` | `lapic_is_interrupt_pending` |
| 24 | 0xC0 | `IsInterrupting` | `boolean_t (*)(uint8_t vector)` | `lapic_is_interrupting` |
| 25 | 0xC8 | `InterruptStats` | `void (*)(uint64_t intrs[256])` | `lapic_interrupt_counts` |
| 26 | 0xD0 | `DisableApicTimer` | `void (*)(void)` | `lapic_disable_timer` |

`pm_rtc_nanotime_t` is `{uint64_t tsc_base; uint64_t ns_base; uint32_t scale; uint32_t shift; uint32_t generation;}` [X:pmCPU.h:L88-94].

Other constants [X:pmCPU.h:L147-157]:
- `PM_HALT_NORMAL` 0, `_DEBUG` 1, `_PANIC` 2, `_SLEEP` 3.
- `PM_SAFE_FL_NORMAL` 0x1, `_SAFE` 0x2, `_PAUSE` 0x10, `_RESUME` 0x20.

### 1.3 Topology structs passed to callbacks (`osfmk/i386/cpu_topology.h`)

`x86_lcpu_t` fields, in order [X:cpu_topology.h:L122-145]:
- `next_in_core`, `next_in_die`, `next_in_pkg`, `lcpu` (self), `core`, `die`, `package` (pointers)
- `struct cpu_data *cpu`
- `uint32_t flags, cpu_num, lnum, pnum`
- `boolean_t master, primary`
- `volatile lcpu_state_t state`
- `volatile boolean_t stopped`
- `uint64_t rtcPop` (next LAPIC pop programmed)
- `uint64_t rtcDeadline` (next etimer deadline requested)
- `x86_cpu_cache_t *caches[3]`
- `void *pmStats, *pmState` (owned by the PM kext)

`lcpu_state_t` values: `LCPU_OFF`=0, `HALT`=1, `NONSCHED`=2, `PAUSE`=3, `IDLE`=4, `RUN`=5 [L94-101].

`x86_core_t`, `x86_die_t`, `x86_pkg_t` and `x86_topology_parameters_t` are at [L153-232]. `x86_pkg_t` ends with `package_idle_exits` and `num_idle`. `machine_track_platform_idle` keeps `num_idle` up to date around every `machine_idle()`, so `num_idle == topoParms.nLThreadsPerPackage` means the whole package is idle [X:pmCPU.c:L1029-1042].

`cpu_data_t` embeds `struct x86_lcpu lcpu` [X:cpu_data.h:L317]. On this build the offset is 0x1590 [BIN: `pmCPUExitIdle` does `addq $0x1590,%rdi`]. Do not hard-code it. Use the callbacks.

The normalized struct bodies of all four topology structs plus `x86_topology_parameters_t` are **identical in every tag checked, from xnu-2422 to xnu-12377.121.6**.

---

## 2. Registration semantics

### 2.1 `pmKextRegister(uint32_t version, pmDispatch_t *cpuFuncs, pmCallBacks_t *callbacks)` [X:pmCPU.c:L963-1016; BIN]

1. **Version check.** If `callbacks == NULL` **or** `version != 102`, the kernel calls `panic("Version mis-match between Kernel and CPU PM")`. So `callbacks` must always be a writable buffer of 0xD8 bytes, even when you only want to probe.
2. It fills all 27 callback slots (§1.2).
3. If `cpuFuncs == NULL`, it **returns here**. `pmDispatch` is not touched and nothing panics. This is a safe way to get the callbacks without registering, but it tells you nothing about whether `pmDispatch` is set (the function returns `void`).
4. If `cpuFuncs != NULL` and `pmDispatch != NULL`, the kernel calls **`panic("Attempt to re-register power management interface--AICPM present in xcpm mode? %p->%p")`**. There is no lock and no compare-and-swap. Two registrants racing on different CPUs are not guarded.
5. Otherwise it stores `pmDispatch = cpuFuncs`. The kernel keeps **your pointer**, not a copy. The table must stay mapped for the rest of the boot.
6. If `earlyTopology` is set and `cpuFuncs->pmCPUStateInit != NULL`, it calls `pmCPUStateInit()` synchronously and clears `earlyTopology`. `earlyTopology` is set when `cpu_topology_sort()` runs `pmCPUStateInit()` while `pmDispatch` is still NULL [X:pmCPU.c:L479-487; X:cpu_topology.c:L167-168]. That happens on the first `ml_processor_register(start=TRUE)`, at boot, from the ACPI CPU driver [X:machine_routines.c:L617-621]. So for a kext loaded after boot, `earlyTopology` is TRUE and `pmCPUStateInit` runs inside `pmKextRegister`.
   - *(Critic)* AMDZenPM is OpenCore-injected and its personality matches `IOResources` with `IOResourceMatch=IOKit` (AMDZenPM/Info.plist), so it can start **before** AppleACPICPU calls `ml_processor_register` → `cpu_topology_sort`. In that case `earlyTopology` is still FALSE at register time, `pmCPUStateInit` is **not** called by `pmKextRegister`, and is instead called later, directly through our table, from `cpu_topology_sort()` (because `pmCPUStateInit()` calls the dispatch whenever `pmDispatch` is set) [X:cpu_topology.c:L167-168; X:pmCPU.c:pmCPUStateInit L479-487]. Non-boot CPUs may not exist yet, and `GetLCPU(n)` may return NULL for them. `pmCPUStateInit` must therefore tolerate both call sites and must not assume a complete topology at register time. Whether our start actually precedes CPU registration on this host is not verified.
7. If `cpuFuncs->pmIPIHandler != NULL`, it calls `lapic_set_pm_func(handler)`. That installs the handler for the LAPIC **PM vector** (`LAPIC_PM_INTERRUPT` = 7, base + 7) [X:lapic.h:L230,L330; X:lapic_native.c:L873,L909]. **It cannot be undone**, because `lapic_set_pm_func` is a static inline over `lapic_set_intr_func`, and neither is exported.
8. It does **not** set `pmInitDone` and does **not** call `initComplete`.

### 2.2 `pmInitDone` and `initComplete`

- `pmInitDone` is a global `uint32_t`, 0 at boot [X:pmCPU.c:L67].
- The only OSS writer is `pmInitComplete()`, reached through `callbacks->initComplete`. It runs `pmCPUStateInit` if `earlyTopology` is still set, then sets `pmInitDone = 1` [L301-311]. **The kernel never calls `initComplete` itself.** The kext must call `cb.initComplete()` after `pmKextRegister`, or `MachineIdle` and every other `pmInitDone`-gated hook is never used.
- In the shipping kernel, XCPM also writes `pmDispatch = &xcpm_dispatch; pmInitDone = 1` directly, inside `xcpm_init_complete` and `xcpm_register_dispatch_table`. See §6.2 [BIN].
- Nothing ever clears `pmInitDone`, and `pmUnRegister` does not reset it.

### 2.3 `pmUnRegister(pmDispatch_t *cpuFuncs)` [X:pmCPU.c:L1021-1027; BIN]

If `cpuFuncs != NULL && pmDispatch == cpuFuncs`, it sets `pmDispatch = NULL`. Nothing else happens: `pmInitDone` stays 1, the LAPIC PM handler stays installed, and no CPU is synchronized.

**Export status** [X:config/Unsupported.x86_64.exports:L19-20; HOST kmutil]:
- `com.apple.kpi.unsupported` exports only `_pmCPUControl` and `_pmKextRegister`.
- `_pmUnRegister`, `_pmDispatch`, `_pmInitDone`, `_machine_idle` and `_xcpm_enabled` are global `T`/`S` symbols in the kernel [BIN nm], but they are **not** in any KPI. The same holds for `lapic_set_pm_func`, which is an inline.

---

## 3. `machine_idle()` [X:pmCPU.c:L102-230; BIN]

Caller: `processor_idle()` in `kern/sched_prim.c`. It runs `(void)splsched()` (interrupts **off**) at L6442. Then, in a loop, it calls `machine_track_platform_idle(TRUE)`, `machine_idle()` and `machine_track_platform_idle(FALSE)`, with the comment "returns with interrupts enabled" (L6502-6506). It then runs `splsched()` again (L6515).

Control flow:
1. `ctime = mach_absolute_time()`. Read this CPU's `rtclock_timer.queue.earliest_soft_deadline` (esd) and `rtclock_timer.deadline` (ehd).
2. **Pending-timer shortcut.** If `ctime >= esd && ctime < ehd && (ehd - ctime) < idle_entry_timer_processing_hdeadline_threshold` (5,000,000 abs units, which is 5 ms on x86 where abs time is ns), it skips idle entirely. It jumps to the exit, enables interrupts, and fires `lapic_timer_swi()` (with a `num_idle` adjust). **`MachineIdle` is not called.**
3. Otherwise:
   - It sets `lcpu.state = LCPU_IDLE` and runs `MARK_CPU_IDLE`.
   - It adds the run time since `cpu_ixtime` into `cpu_rtime_total` and `cpu_rtimes[]` bins.
4. If `pmInitDone`, it replays any early `ml_set_maxbusdelay` / `ml_set_maxintdelay` values. These are stored while `pmDispatch->set*` is NULL.
5. **Dispatch.** If `pmInitDone && pmDispatch && pmDispatch->MachineIdle`, it calls `MachineIdle(0x7FFFFFFFFFFFFFFF)`. Otherwise it runs `pal_hlt()` then `pal_cli()`. On bare metal `pal_hlt()` is `sti; hlt`, so the instruction sequence is `sti; hlt; cli` [X:pal_native.h:L40-42; BIN machine_idle: `sti; hlt; cli` inline].
6. It runs `MARK_CPU_ACTIVE` and sets `lcpu.state = LCPU_RUN`. It updates `cpu_ixtime`, `cpu_idle_exits`, `cpu_itime_total` and the `cpu_itimes[]` bins.
7. `pal_sti()`. Interrupts are enabled here, and any interrupt still pending is delivered now.

**Contract for `MachineIdle`** (derived from the flow above):
- **Entry:** IF=0, at splsched, on the idle thread with preemption effectively disabled. `maxIdleDuration` is **always** the constant `0x7FFFFFFFFFFFFFFF`. The kernel passes no deadline.
  - To size the idle period, read `GetMyLCPU()->rtcPop` / `rtcDeadline` (set by `setPop`) or `cpu_data.rtclock_timer.deadline`.
  - **The return value is ignored.**
- **Exit:** return with **IF=0**, like the fallback, which does `cli` after `hlt`. The kernel's post-idle accounting assumes interrupts are still masked, and step 7 re-enables them. If you return with IF=1, nothing visibly breaks, but the accounting can be interrupted.
  - Pattern A: `sti; hlt; cli`. The `sti` interrupt shadow makes `sti; hlt` atomic. The interrupt handler runs inside `MachineIdle`.
  - Pattern B: MWAIT with ECX[0]=1 (break on masked interrupt), with IF kept 0. The interrupt is then serviced at step 7. The host supports this: CPUID 5 ECX=3, so EMX and IBE are set (linux-pstate.md §0). *(Critic)* On Zen, MWAIT is expected to reach only C1 regardless of the EAX hint; CC6 is reached through the I/O C-state (Pattern C) [RECALLED: AMD PPR / Linux AMD `_CST` usage, not checked here]. Do not expect MWAIT alone to produce CC6 residency.
  - Pattern C: an I/O C-state read (`inb CstateAddr+n`). *(Critic correction)* Linux does this with **IF=0**, not IF=1: `acpi_idle_do_entry()` is documented "Caller disables interrupt before call and enables interrupt after return", and `io_idle()` is a bare `inb(addr)` (no PM-timer dummy read on non-Intel) [VERIFIED in local copy deps/linux-src/drivers/acpi/processor_idle.c: `io_idle` L521-551, `acpi_idle_do_entry` L555-575]. So the core wakes on a pending-but-masked interrupt and the interrupt is serviced after IF is re-enabled, which fits the `MachineIdle` IF=0 contract directly (return with IF=0; `machine_idle` step 7 does `sti`). See linux-pstate.md §6.
- No blocking, no mutexes, no allocation, no `IOLog`. This runs on every idle entry on every CPU.
- **Wake-ups and `exitIdle`:** scheduler wake-ups go through `cpu_interrupt()`. That function sends an IPI only if `pmCPUExitIdle()` returns TRUE [X:mp.c:cpu_interrupt:L680-692]. With HLT you must leave `exitIdle` NULL (which defaults to TRUE) or return TRUE. TLB shootdowns and other `i386_signal_cpu` IPIs do not consult `exitIdle`.
- `callbacks->pmSendIPI(cpu)` sends the LAPIC PM vector. If `pmIPIHandler` is NULL, `lapic_interrupt` only EOIs it [X:lapic_native.c:L905-914]. That makes it a side-effect-free "kick a CPU out of hlt/mwait" primitive.

Maximum duration and deadlines: XNU itself never limits the idle duration. Wake-up depends on the LAPIC timer (programmed by `timer_resync_deadlines` → `setPop`) firing during the C-state.
- The host has ARAT (CPUID 6 EAX[2]=1), so the LAPIC timer keeps running in deep C-states. No timer save/restore or broadcast is needed. `pmTimerStateSave/Restore` exist for the Intel HPET/deep-C-state era.

---

## 4. Every other hook: gating, call sites, and what NULL does

"Gated" means the wrapper checks `pmInitDone && pmDispatch && fn`. Hooks marked "no" are live as soon as `pmKextRegister` returns, before `initComplete` runs.

| Field | Gated | Wrapper / call site | NULL behaviour (= current behaviour with no PM kext) | Safe NULL? |
|---|---|---|---|---|
| `pmCPUStateInit` | no | Called from `pmKextRegister` / `pmInitComplete` when `earlyTopology` is set. Return value ignored | Nothing. `earlyTopology` stays TRUE (harmless) | yes |
| `cstateInit` | no | Only `power_management_init()` in `i386_init` (L1010), before any kext exists | Never called for a kext | yes |
| `MachineIdle` | yes | `machine_idle` | `sti;hlt;cli` | yes (baseline) |
| `GetDeadline` | yes | `timer_intr` on **every** LAPIC timer interrupt, and `timer_resync_deadlines` [X:i386_timer.c:L151,L227] | Returns 0, meaning "no PM deadline". The default timer path is unchanged | yes |
| `Deadline` | yes | `timer_intr`, only if `GetDeadline() != 0 && <= now` | Not reached | yes |
| `SetDeadline` | yes | `pmCPUSetDeadline` has **no callers** in the OSS tree and no direct `call` in [BIN] | Dead | yes |
| `exitIdle` | yes | `cpu_interrupt` (scheduler wake-up IPIs) | Returns TRUE, so an IPI is always sent | yes. Required with HLT |
| `markCPURunning` | yes | `cpu_thread_init()` on every CPU start/restart [X:cpu_threads.c:L978], and `acpi_sleep_kernel` wake on the boot CPU [X:acpi.c:L367] | Kernel sets `current lcpu.state = LCPU_RUN`. **A non-NULL hook replaces this. It must set `lcpu->state = LCPU_RUN` itself**, or `cpu_exit_wait`/state tracking breaks | yes (recommended) |
| `pmCPUControl` | no | Exported `pmCPUControl()`. Called by `IOPMrootDomain::informCPUStateChange` with `PMIOCSETVARINFO`/`PMIOCGETVARNAMEINFO` (`_IOW('P',27/26,uint64_t)`) on AC/lid changes [X:IOPMrootDomain.cpp:L5349-5400]. AppleHPET references it [BIN] | Returns −1 | yes. If implemented, return non-zero for unknown commands |
| `pmCPUHalt` | yes | `pmCPUHalt(reason)` for NORMAL/SLEEP: `cpu_thread_halt` (CPU offline), `acpi_idle_kernel` | `wbinvd`, `state=LCPU_HALT`, `wbinvd;hlt`, then **`panic("back from Halt")`** if it ever resumes. The CPU is restarted later by INIT/SIPI | yes (see §8) |
| `getMaxSnoop` / `getMaxBusDelay` | yes | `ml_get_maxsnoop` / `ml_get_maxbusdelay` | 0 | yes |
| `setMaxBusDelay` / `setMaxIntDelay` | no | `ml_set_maxbusdelay` / `ml_set_maxintdelay` | Value parked in `earlyMax*`. `machine_idle` replays it on every idle while it stays parked, which costs a few instructions | yes. Optionally provide store-only stubs |
| `getMaxIntDelay` | no | `ml_get_maxintdelay` | 0 | yes |
| `pmCPUSafeMode` | no | `pmSafeMode`: panic path (`PM_SAFE_FL_SAFE`) [X:kern/debug.c:L1335], NMI/KDP in `mp.c` | Handles only PAUSE (spin while `LCPU_PAUSE`) and RESUME. SAFE/NORMAL are ignored | yes (recommended) |
| `pmTimerStateSave` / `Restore` | no | `acpi_sleep_kernel` before firmware sleep and after wake [X:acpi.c:L228,L370] | no-op | yes |
| `exitHalt` | yes | `intel_startCPU_fast` [X:mp.c:L281-292] | Returns `KERN_INVALID_ARGUMENT`, so `cpu_start` falls back to `intel_startCPU` (full INIT/SIPI) [X:cpu.c:L118-145] | yes. Must stay NULL while `pmCPUHalt` is NULL |
| `exitHaltToOff` | yes | `acpi_sleep_kernel`, for each other CPU (a failure panics) [X:acpi.c:L205] | Returns `KERN_SUCCESS` | yes |
| `markAllCPUsOff` | yes | `acpi_sleep_kernel` wake [X:acpi.c:L325] | no-op | yes |
| `pmSetRunCount` | no | `machine_run_count()`. No callers in the 12377 OSS tree | Value saved for `GetSavedRunCount` | yes |
| `pmIsCPUUnAvailable` | — | Not referenced anywhere | — | yes |
| `pmChooseCPU` | yes | `machine_choose_processor` ← `choose_processor` [X:kern/sched_prim.c:L4714] (scheduler hot path) | Returns `preferred` | yes |
| `pmIPIHandler` | at register | Installed permanently into the LAPIC PM vector | Vector only EOIs | **keep NULL** (it cannot be removed) |
| `pmThreadTellUrgency` | yes | `thread_tell_urgency` from `thread_dispatch` and friends. Runs with preemption off. **Panics if it takes longer than `urgency_notification_assert_abstime_threshold`** when that is non-zero [X:pmCPU.c:L797-799] | return | yes |
| `pmActiveRTThreads` | yes | `active_rt_threads()`. No callers in OSS | — | yes |
| `pmInterruptPrewakeApplicable` | yes | `ml_interrupt_prewarm` [X:mp.c:L2114] | FALSE, so prewarm is a no-op | yes |
| `pmThreadGoingOffCore` | yes | `machine_thread_going_off_core` from `thread_dispatch` (2 sites) | return | yes |

Main conclusion: **a table with only `MachineIdle` set** (plus optional `pmCPUStateInit` and store-only delay stubs) leaves every other kernel path exactly as it is today on this machine with no PM kext. Once `pmInitDone` is 1, every gated wrapper whose field is NULL takes the same default branch.

---

## 5. ABI stability across versions

I fetched `pmCPU.h` and `pmCPU.c` for 18 tags and compared normalized struct bodies (comments and whitespace removed):

| Tags | Darwin / macOS [RECALLED mapping; Darwin 25 = xnu-12377 is HOST-verified] | `PM_DISPATCH_VERSION` | `pmDispatch_t` | `pmCallBacks_t` |
|---|---|---|---|---|
| 2422.115.4, 2782.40.9, 3248.60.10, 3789.70.16, 4570.71.2 | 13–17 / 10.9–10.13 | 102 | 28 fields (no `pmThreadGoingOffCore`) | same 27 slots |
| **4903.270.47, 6153.141.1, 7195.50.7.100.1, 7195.141.2, 8019.80.24, 8020.140.41, 8792.81.2, 8796.141.3, 10002.81.5, 10063.141.1, 11215.81.4, 11417.140.69, 12377.121.6** | **18–25 / 10.14–26** | 102 | **identical, 29 fields** | **identical** |

- `pmCPU.h` is **byte-identical** from xnu-6153 to xnu-12377.121.6, so the layout is the same for macOS 11–26 (Darwin 20–25). It also matches 10.14 and 10.15 (Darwin 18–19).
- `pmThreadGoingOffCore` was appended in xnu-4903 **without** a version bump. A kext built with the 29-field table is still prefix-compatible with Darwin 13–17, because the kernel never reads index 28 there.
- The `pmCPU.c` diffs from 7195 to 12377 cover only the following: `master_cpu` became `boot_cpu_id`; `ThreadGetUrgency` became a stub returning NONE (from 8019); `disableConsoleOutput` was removed; and empty `machine_perfcontrol_*` stubs were added. The registration, idle and halt logic is unchanged.
- The running 12377.161.15 binary matches every offset in §1.1 and §1.2 [BIN].
- Runtime guard: the kernel only checks `version == 102`. It has no size check, so a layout change would show up as silent memory corruption, not as a panic. Before registering, gate on `version_major` (Darwin) being in 18..25 and refuse anything else.

---

## 6. Is `pmDispatch` already registered? Who registers it on this host?

### 6.1 No exported query exists

- `pmDispatch` and `pmInitDone` are not exported (§2.3).
- `pmKextRegister(102, NULL, &cb)` is safe but returns `void` and reveals nothing.
- No slot in `pmCallBacks_t` reports registration state.
- `pmCPUControl(cmd, p)` returns −1 when `pmDispatch == NULL`, but also when the registered table has `pmCPUControl == NULL`. AMDRyzenCPUPowerManagement's table sets `.pmCPUControl = 0` [deps/SMCAMDProcessor/src/AMDRyzenCPUPowerManagement/pmAMDRyzen.c, `pmRyzen_cpuFuncs`]. Calling a foreign `pmCPUControl` with a made-up command is also unsafe. **The result is ambiguous, so do not use it.**
- `machdep.xcpm.mode` (readable in-kernel with `sysctlbyname`) shows only whether XCPM is active. `xcpm_enabled()` exists but is not exported [BIN nm].
- The only non-KPI route is to resolve `_pmDispatch` from the on-disk kernel symtab plus the KASLR slide and read it. That is what Lilu-based kexts do. SMCAMDProcessor resolves `_pmDispatch` and `_pmUnRegister` through `lookup_symbol` [deps/.../AMDRyzenCPUPowerManagement.cpp:L59-60]. It needs kernel file I/O and slide computation, which goes against the project's no-Lilu stance.

**Practical guard (all exported or IOKit):**
1. Darwin 18..25 and `machdep.xcpm.mode == 0`.
2. No `AppleIntelCPUPowerManagement` IOService and no `AMDRyzenCPUPowerManagement` IOService (`IOService::copyMatchingService(nameMatching(...))`).
3. A process-wide `static bool registered` in our kext, plus a published resource property (for example `AMDZenPM-pmDispatch`) so a second instance or a reload sees it.
4. Make the kext non-unloadable (§7), so a reload, which would panic on re-register, cannot happen.

Residual race: AMDRyzenCPUPowerManagement itself calls `pmUnRegister(*pmDispatch)` and then `pmKextRegister` [pmAMDRyzen.c `pmRyzen_init`]. If it loads **after** us, it silently replaces our table, with no panic. If it loads **before** us and we skip check 2, we panic.

### 6.2 XCPM on AMD [BIN]

The OSS `xcpm_bootstrap` in `osfmk/x86_64/loose_ends.c:L924` is a lock-init stub. The shipping kernel contains the real XCPM (137 `xcpm` symbols).
- `xcpm_bootstrap` (called from `i386_init` right after `power_management_init`) sets `xcpm_mode = 0` if `cpuid_features` bit 63 (the hypervisor bit) is set, or if `cpuid_info()->cpuid_model` (byte at +0x4D) minus 0x3C is above 0x69 unsigned. That leaves only models 0x3C–0xA5 eligible, each through a jump table.
  - On the 5700X, `cpuid_model` = 0x01 + (0x2 << 4) = 0x21, because family 0xF folds `extmodel` [X:cpuid.c:L677-678]. So `xcpm_mode = 0`. That matches [HOST] `machdep.xcpm.mode: 0`.
- `xcpm_early_init` and `xcpm_init_complete` both start with `cmpl $0, xcpm_mode; je <return>`. `xcpm_init_complete` is the only place in the kernel that installs XCPM's dispatch (`pmDispatch = &xcpm_dispatch; pmInitDone = 1`), so it does nothing on this host.
- `xcpm_register_dispatch_table` performs the same store with no mode check, but **no direct call site exists** in the kernel disassembly.
- **The kernel itself contains no `call _pmKextRegister`.**

### 6.3 Apple kexts [BIN, HOST]

- **AppleIntelCPUPowerManagement**: not present in `/System/Library/Extensions` [HOST]. [RECALLED] It was removed in macOS 13.
- **Boot KC** (203 kexts, AppleACPIPlatform among them): I scanned for rel32 `call`/`jmp` instructions and KC chained pointers that target `_pmKextRegister` and `_pmUnRegister`. **There were zero hits.** A positive control worked: AppleACPIPlatform does reference `_IOLog` and `_ml_set_interrupts_enabled`. The only PM symbol referenced was `_pmCPUControl`, by **AppleHPET**.
- **System KC** (X86PlatformPlugin, X86PlatformShim, ACPI_SMC_PlatformPlugin, IOPlatformPluginLegacy, ApplePlatformEnabler): its import string pool contains `_pmCPUControl` and `_IOLog`, but **not `_pmKextRegister`**. The **Aux KC** contains neither.
- **X86PlatformPlugin does load on AMD.** It is loaded on this host (kmutil showloaded, index 130). Its personality matches `AppleACPICPU` with `IOCPUNumber=0` and `IOResourceMatch=ACPI`, and has no vendor check [HOST plutil]. It does not register `pmDispatch`.
- Third-party kexts on this host: AMDRyzenCPUPowerManagement is **not loaded right now** (kmutil). **RyzenHV (`com.overlorddev.RyzenHV` 0.7.8) is loaded and injected by OpenCore.** ~~Its binary is not on disk where I could check it, so whether it registers `pmDispatch` is unknown.~~
  - *(Critic correction)* The binary is on the mounted EFI volume: `/Volumes/FD14-LITE/EFI/OC/Kexts/RyzenHV.kext/Contents/MacOS/RyzenHV`, CFBundleVersion 0.7.8, LC_UUID `A5D42052-BB36-38B1-A6E7-6E408FE044BC`, which equals the loaded instance's UUID in `kmutil showloaded` [HOST]. It links `com.apple.kpi.unsupported` and `kpi.private`, but its undefined-symbol table has **no** `_pmKextRegister`, `_pmCPUControl`, `_pmDispatch` or `_pmUnRegister`, and its strings contain no `pmKextRegister`/`pmDispatch`/`/System/Library/Kernels/kernel`/`symtab` (read-only `nm -u` / `strings`) [HOST]. So RyzenHV does not register a PM table by any linked route; only an obfuscated runtime symbol resolver could, which nothing in the binary suggests.
  - *(Critic)* `EFI/OC/Kexts/` also contains `AMDRyzenCPUPowerManagement.kext` and `SMCAMDProcessor.kext`, but both have `Enabled=false` in `EFI/OC/config.plist` `Kernel→Add` [HOST, plistlib read]. Re-enabling either one reintroduces risk §10.3.
  - *(Critic)* `Kernel→Emulate→DummyPowerManagement=true` in config.plist. [RECALLED] That quirk only neuters AppleIntelCPUPowerManagement (absent here); it does not touch `pmDispatch`.

---

## 7. Unload safety

Facts:
- `pmUnRegister` is not exported. `pmKextRegister` refuses to replace a non-NULL table and panics.
- The kernel holds our table pointer, and `machine_idle`/wrappers reload `pmDispatch->field` on every call. [BIN] shows `movq 0x10(%rax),%rax` on every idle.
- A CPU can be executing inside `MachineIdle` (halted in it) at any moment.
- If `pmIPIHandler` was set, the LAPIC vector points into our text permanently.

**Therefore a kext that has registered must never be unmapped.** Minimum safe recipe:
1. Statically allocate the table in `__DATA` and fill it completely **before** `pmKextRegister`. Leave `pmIPIHandler` NULL. Then call `cb.initComplete()`.
2. Right after a successful register, call `OSKextRetainKextWithLoadTag(kmod_info.id)` and never release it.
   - `OSKextRetainKextWithLoadTag` is exported [X:config/Libkern.exports:L30].
   - `OSKextGetCurrentLoadTag()` is only libkmod's `return kmod_info.id` [X:libkern/kmod/c_start.c:L83-86]. The project has its own `kmod_info.c`, so read `kmod_info.id` directly.
   - `OSKext::removeKext` first terminates IOServices, which runs our `stop()`. It then calls `unload()`, which fails with `kOSKextReturnInUse` because the retain count is above 2 [X:OSKext.cpp:L4629-4645,L8671-8678].
   - Optional second guard: make the kmod stop routine (`_antimain` in `kmod_info.c`) return `KERN_FAILURE` while registered. `OSKext::stop` aborts the unload when `stopfunc` fails [X:OSKext.cpp:stop].
3. In `IOService::stop()` (which will still run), atomically store NULL into `table.MachineIdle` (and `exitIdle`, `markCPURunning`, if they were used). The next idle on each CPU takes the `pal_hlt` path, and CPUs already halted inside our function return through our still-mapped code.
   - An equivalent design: keep the pointer and check an `enabled` flag inside `MachineIdle` that falls back to `sti;hlt;cli`.
   - Restore any per-CPU hardware state the idle path changed. Restoring the boot P-state is already handled.
   - *(Critic)* For the **OpenCore-injected** deployment, the kext sits in the primary (boot) KC. `OSKext::initWithPrelinkedInfoDict` sets `flags.unloadUnsupported = 1` for a `KCKindPrimary` kext unless it declares `OSBundleAllowUserLoad` or `OSMutableSegmentCopy` [X:OSKext.cpp:L2268-2279], and `OSKext::removeKext` checks that flag **before** `terminateDriversForModule` [X:OSKext.cpp:L4618-4626]. AMDZenPM/Info.plist declares neither key, so in that deployment an unload request fails with `kOSKextReturnInUse` and **`stop()` does not run at all**. The retain in step 2 and the `stop()` logic in step 3 matter for the `kmutil load` dev workflow (Aux KC / non-KC), not for the OC-injected build. Whether OC-injected kexts are really classified `KCKindPrimary` is [RECALLED], not verified.
4. Do not re-register in a later `start()`. Detect our own prior registration with the static flag. Because of step 2, the kext text, and so the flag, survives.

This is the model SMCAMDProcessor follows, except that it resolves the private `pmUnRegister` and spins IPIs until CPUs leave idle [pmAMDRyzen.c `pmRyzen_stop`]. Without the private symbol, the only safe option is never to unload.

---

## 8. Sleep/wake and CPU offline

Path for taking a non-boot CPU offline (S3 and `cpu_quiesce`/offline):
1. `processor_doshutdown` → `processor_offline_intstack` → `cpu_sleep()` [X:kern/machine.c:L492-506].
2. `cpu_sleep()` → `PE_cpu_machine_quiesce` → `cpu_thread_halt()` [X:cpu.c:L93-101].
3. `cpu_thread_halt()` decrements the counts, disables interrupts, and loops on `pmCPUHalt(PM_HALT_NORMAL)` [X:cpu_threads.c:L988-1010].

**This path calls `pmCPUHalt`, not `machine_idle`/`MachineIdle`.** With the `pmCPUHalt` hook NULL, the CPU does `wbinvd`, `state=LCPU_HALT`, `wbinvd;hlt`, and panics only if it resumes. The requesting CPU waits in `cpu_exit_wait` for `LCPU_HALT`/`LCPU_OFF`/`stopped`, with a 10 s TSC timeout that panics [X:cpu.c:L148-178]. A custom `pmCPUHalt` would have to set the state itself and would also need `exitHalt`. Leave both NULL.

Boot CPU, `acpi_sleep_kernel` [X:acpi.c:L182-]:
- Before firmware: `pmCPUExitHaltToOff` for each other CPU (NULL → success), then `lapic_shutdown`, then `pmTimerSave`.
- On wake: `pmMarkAllCPUsOff`, `lapic_configure`, `pmCPUMarkRunning(boot)`, `pmTimerRestore`, `rtc_timer_start`.
- Other CPUs come back through `cpu_start` → `intel_startCPU_fast` (fails with NULL `exitHalt`) → `intel_startCPU` (INIT/SIPI) → `cpu_thread_init` → `pmCPUMarkRunning` → idle thread → `machine_idle` → **`MachineIdle`**.

Requirements this places on `MachineIdle`:
- It runs again on each CPU immediately after a wake, with no notification. `markCPURunning` is the only per-CPU wake hook, and using it means taking over `LCPU_RUN`.
- Any per-CPU setup it depends on must therefore either be firmware-restored or be done lazily inside `MachineIdle`, for example a per-CPU generation counter compared with a wake counter that a sleep/wake notifier bumps. CStateBaseAddr and CC6 enables are programmed by firmware on resume [RECALLED/PPR, unverified].
- It must tolerate CPUs leaving and joining at any time, because other CPUs keep idling while one is going offline.

`acpi_idle_kernel` (S0 "deep idle", requested by the ACPI platform kext) calls `pmCPUHalt(PM_HALT_SLEEP)` on the boot CPU [X:acpi.c:L428-490]. With no `pmCPUHalt` hook, that is the `back from Halt` panic path. **That is already true today without a PM kext**, and registering a table with `pmCPUHalt = NULL` does not change it. A hackintosh using S3 does not take this path.

---

## 9. Recommended minimal registration for AMDZenPM (phase 1)

- Gate: Darwin 18..25 (*critic note:* AMDZenPM/Info.plist requires `com.apple.kpi.*` 20.0.0, so the kext cannot link below Darwin 20; the effective range is 20..25), x86_64, `machdep.xcpm.mode == 0`, no AICPM or AMDRyzenCPUPowerManagement service present, and not already registered (static flag). Add a boot-arg kill switch.
- Table: set `MachineIdle` only, optionally `pmCPUStateInit` returning `KERN_SUCCESS`, and store-only `setMaxBusDelay`/`setMaxIntDelay`. Leave every other field NULL, especially `pmIPIHandler`, `pmCPUHalt`, `exitHalt`, `markCPURunning` and `exitIdle`.
- Sequence: fill the table → `pmKextRegister(102, &table, &cb)` → `OSKextRetainKextWithLoadTag(kmod_info.id)` → `cb.initComplete()`.
- `MachineIdle`: enter with IF=0 and return with IF=0. Use `sti;hlt;cli` first (behaviour identical to the kernel fallback, as a smoke test), then MWAIT or the I/O C-state for CC6.
- `stop()`: NULL the `MachineIdle` pointer and never unload.

---

## 10. Open risks

1. **No OSS source for 12377.161.15.** The binary agrees on every checked offset and branch, but its `pmCPU.c` is about 51 lines longer than the published one. Unseen code could exist outside the functions I disassembled.
2. ~~**RyzenHV** (OC-injected, loaded) was not inspected.~~ *(Critic: largely resolved.)* The loaded RyzenHV 0.7.8 binary (UUID-matched, on the EFI volume) imports no PM symbols (§6.3). Residual risk: any future OC-injected kext that registers a table makes our `pmKextRegister` panic, and no exported check can see it.
3. **Load-order race with AMDRyzenCPUPowerManagement**: it unregisters whatever is present, which silently steals our table. If it is already present and we do not detect it, we panic.
4. `pmKextRegister` has no locking. Two registrants racing on different CPUs are undefined.
5. There is no size check in the ABI. A future layout change would corrupt memory silently, so the Darwin-range gate is mandatory.
6. Once registered, the kext is permanent for the boot. Testing iterations need a reboot.
7. The `acpi_idle_kernel` panic path exists independently of us. Calling `pmCPUHalt` hooks would be needed if S0 deep idle is ever triggered.
8. Wake-time state of the CC6 I/O port and C001_0073/0296 has not been verified on this board.

---

## Critic notes

Independent review, 2026-11-08. I re-fetched primary sources myself into a scratch dir (`/tmp/critic-xnu`, from `raw.githubusercontent.com/apple-oss-distributions/xnu/xnu-12377.121.6/...`) instead of trusting `deps/xnu-src/`. Read-only work only: no kext load/unload, no MSR access, no sudo.

### Verified

1. **Source provenance.** `git ls-remote` shows the newest 12377 tag is `xnu-12377.121.6` (others: .1.9, .41.6, .61.12, .81.4, .101.15); no 12377.161.x tag is published. My fresh copies of `pmCPU.h`, `pmCPU.c`, `lapic.h`, `acpi.c`, `mp.c`, `cpu_topology.c`, `cpu_threads.c`, `cpu.c`, `machine_routines.c`, and the cpuid model-fold lines in `cpuid.c` are byte-identical to `deps/xnu-src/xnu-12377.121.6/`.
2. **Running kernel = on-disk kernel.** The `com.apple.kpi.*` pseudo-kexts in `kmutil showloaded` carry UUID `28267FD5-3E1E-3686-8A1C-E73E2466FCFB`, the same as `dwarfdump --uuid /System/Library/Kernels/kernel`. So the [BIN] evidence comes from the build that is running, apart from OpenCore's in-memory patches (item 8).
3. **ABI.** `PM_DISPATCH_VERSION 102`, the 29-field `pmDispatch_t`, and the 27-slot `pmCallBacks_t`, in the order and with the offsets in §1.1/§1.2 [X:pmCPU.h:L40-124]. In the host binary, `pmKextRegister` starts `cmpl $0x66,%edi; jne panic; testq %rdx,%rdx; je panic`. Its callback stores go to 0x00..0xD0(%rdx) in the order the table gives. It reads `0xc0(%rsi)` for `pmIPIHandler`. The panic `__LINE__`s are 0x417/0x41C (1047/1052) [BIN].
4. **Registration semantics.** NULL `callbacks` or a wrong version panics. NULL `cpuFuncs` returns after the callbacks are filled. A non-NULL `pmDispatch` causes the re-register panic. There is no lock. The pointer is stored, not copied. `pmCPUStateInit` is called if `earlyTopology` is set. `pmIPIHandler` is written straight into the LAPIC function table. `pmInitComplete` sets `pmInitDone=1` [X + BIN]. Whole-kernel disassembly shows only five stores to `_pmDispatch`/`_pmInitDone`:
   - `pmKextRegister` and `pmInitComplete` (static)
   - `pmUnRegister`
   - `xcpm_register_dispatch_table`
   - `xcpm_init_complete`, which is gated by `cmpl $0, _xcpm_mode`

   The kernel itself never calls `_pmKextRegister`. `xcpm_register_dispatch_table` has no caller in the kernel, and it is in **no** KPI symbol set, so no kext can link to it [BIN].
5. **Exports.** The kernel's `__LINKINFO,__symbolsets` plist shows:
   - `com.apple.kpi.unsupported` exports `_pmKextRegister` and `_pmCPUControl`.
   - `com.apple.kpi.libkern` exports `_OSKextRetainKextWithLoadTag`.
   - `_pmUnRegister`, `_pmDispatch`, `_pmInitDone` and `_machine_idle` are in **no** set.
   - `kpi.private` exports only `xcpm_bios_mbox_*`, `xcpm_is_hwp_enabled` and `xcpm_mbox_*`.
6. **`machine_idle` contract.**
   - `processor_idle` calls `(void)splsched()` before the loop and comments "returns with interrupts enabled" [X:sched_prim.c:L6442,L6502-6506].
   - The pending-timer shortcut uses a threshold of 5000000 [X:pmCPU.c:L96,L118-123].
   - The gate is `pmInitDone && pmDispatch && MachineIdle`. The argument is `movabsq $0x7fffffffffffffff,%rdi` and the field is read with `movq 0x10(%rax)` on every idle. The fallback is inline `sti; hlt; cli` [X:pmCPU.c:L162-177; BIN].
   - `pal_hlt` is `sti; hlt` [X:pal_native.h].
7. **Other contracts.**
   - With NULL hooks, `pmCPUExitIdle` returns TRUE [X:pmCPU.c:L433-446], `pmCPUExitHalt` returns `KERN_INVALID_ARGUMENT` [L449-460] and `pmCPUControl` returns -1 [L510-519].
   - `cpu_interrupt` sends an IPI only when `pmCPUExitIdle` returns TRUE [X:mp.c:L682-691].
   - `LAPIC_PM_INTERRUPT=7`. `lapic_set_pm_func` is a static inline, and `lapic_interrupt` EOIs the PM vector even with a NULL handler [X:lapic.h:L230,L330; lapic_native.c:L866-876,L905-914].
   - `cpu_thread_halt` loops on `pmCPUHalt(PM_HALT_NORMAL)` with IF=0, and with a NULL hook `pmCPUHalt` falls into `wbinvd; hlt` and then panics "back from Halt" [X].
   - `cpu_data.lcpu` is at 0x1590: the `GetCore` callback loads `0x15b0` = lcpu+0x20 = `core` [BIN].
   - Unload: `removeKext` runs `terminateDriversForModule(unload=false)` before `unload()`, and `unload()` refuses with `kOSKextReturnInUse` when the retain count is above the minimum [X:OSKext.cpp:L4633-4646,L8671-8680].
8. **OpenCore patches.** Every enabled `kernel` patch in `EFI/OC/config.plist` (AMD Vanilla cpuid/commpage/i386_init/lapic_init/mtrr, plus the non-monotonic-time removals) was scanned with its Find/Mask against the on-disk kernel. **None matches inside the pmCPU.c code window** (0xffffff80003efa30–0x3f0c00: `machine_idle` … `pmUnRegister`). So the [BIN] facts about the dispatch path should still hold at runtime. One patch does touch `lapic_init` (it removes the LAPIC version check), which is not on the PM-vector path.
9. **XCPM off.** [HOST] `machdep.xcpm.mode: 0`. The cpuid model fold (`model += extmodel<<4` for family 0x6/0xF) [X:cpuid.c:L676-681] gives 0x21 for the 5700X.

### Corrected

- **§3 Pattern C.** Linux executes the I/O C-state read with **IF=0** (`acpi_idle_do_entry`: "Caller disables interrupt before call…"), not IF=1. The section has been fixed, and it now says this fits the `MachineIdle` IF=0 contract.
- **§3 Pattern B.** Added a caveat [RECALLED] that MWAIT on Zen reaches only C1, so CC6 needs the I/O path.
- **§2.1(6).** The claim "`pmCPUStateInit` runs inside `pmKextRegister`" holds only when the kext registers after `cpu_topology_sort`. AMDZenPM is OC-injected and matches `IOResources`/`IOKit`, so it may register first. In that case `pmCPUStateInit` is called later from `cpu_topology_sort`, and other CPUs may not exist yet.
- **§6.3 / §10.2 RyzenHV.** The binary *is* on disk (on the EFI volume), and its UUID matches the loaded instance. It imports no PM symbols. The risk is downgraded.
- **§6.3.** I recorded that AMDRyzenCPUPowerManagement and SMCAMDProcessor are present on the ESP but `Enabled=false`, and that `DummyPowerManagement=true`.
- **§7.** For an OC-injected kext in the primary KC, `unloadUnsupported` is set unless `OSBundleAllowUserLoad` is declared. `removeKext` then rejects the unload **before** terminating services, so `stop()` never runs. The retain and `stop()` recipe only matters for `kmutil load` testing.
- **§9.** The effective Darwin floor is 20, from the Info.plist `OSBundleLibraries` versions, not 18.

### Still unverified / residual risk

- Apple has not published 12377.161.15 source. I checked the binary only in `pmKextRegister`, `pmInitComplete`, `machine_idle` and the global stores to `pmDispatch`/`pmInitDone`. The other wrappers (`pmCPUHalt`, `pmCPUMarkRunning`, `timer_intr` gating) were not re-disassembled by me.
- My OC patch scan only checked byte patterns against the on-disk kernel. It did not inspect OC's `ProvideCurrentCpuInfo` quirk, which patches code itself instead of using Find/Replace.
- Whether OC-injected kexts are `KCKindPrimary` is [RECALLED]. So is whether AMDZenPM's `start()` runs before AppleACPICPU registers CPUs.
- The doc's statement that `pmCPU.h` is the same across Darwin 18–25 was not re-diffed by me across all 18 tags. I checked only the 12377.121.6 copy.
- `IOPMrootDomain::informCPUStateChange`, the `acpi_idle_kernel` S0 path, and the AppleHPET/X86PlatformPlugin KC scans were not re-checked.
- The Zen behaviour of the I/O C-state read with IF=0 (whether it wakes on a masked pending interrupt), MWAIT reaching only C1, and the CC6 state restored on wake all remain [RECALLED] or need on-hardware tests.
