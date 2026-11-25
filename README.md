# AMDZenPM

AMDZenPM is a macOS kernel extension (x86_64, macOS 11 to 26) for AMD Zen CPUs: families 17h, 18h (Hygon), 19h and 1Ah. It manages CPU power and frequency from inside the kernel, starting at boot. It also reports temperature, energy/power, voltage and per-core frequency.

Each function is modelled on a Linux driver:

| Function | Linux reference | Registers |
|---|---|---|
| Tctl / Tdie / per-CCD temperature | `k10temp` | SMN 0x59800, CCD at +0x154 / 0x300 / 0x308 / 0x1F0 |
| Package and per-core energy and power | `rapl`, `amd_energy` | MSR C001_0299/029A/029B, 32-bit counters with wrap handling |
| Vcore / Vsoc / current (SVI2) | `zenpower` | SMN 0x5A00C / 0x5A010 / 0x5A014 / 0x6F038 |
| Effective frequency, load, IPC | `aperfmperf`, turbostat | APERF/MPERF/TSC/IRPERF |
| P-state control | `acpi-cpufreq` | C001_0061..006B, written per thread |
| CPPC + EPP | `amd-pstate` (MSR path) | C001_02B0..02B4 |
| Autonomous CPPC + native macOS signals | `amd-pstate` (ACPI PCC path) | `_CPC` / PCCT shared memory; scheduler urgency and native power/thermal hints |
| Boost (CPB) | `acpi-cpufreq` boost | HWCR[25] |
| Governors | schedutil / ondemand / conservative | `ZenModel.cpp`, integer math only |

Register details and Linux sources are in `docs/research/`.

## Build

```sh
make          # build/AMDZenPM.kext (ad-hoc signed)
make check    # offline link check against the installed kernel (does not load the kext)
make test     # unit tests for decode and governor logic (runs on the host)
```

Requirements: Xcode 26 with the MacOSX SDK. The build uses clang directly; there is no Xcode project.

## Installing (OpenCore)

1. **Disable the other AMD power-management kexts:** `AMDRyzenCPUPowerManagement.kext` and `SMCAMDProcessor.kext`.
   - Both drivers write the same MSRs and use the same SMN register pair.
   - If AMDZenPM detects one of them, it switches to monitor-only mode and stops reading SMN. Its `block_reason` then reads `conflict`.
2. Copy `build/AMDZenPM.kext` to `EFI/OC/Kexts`. Add an entry to `Kernel → Add` with `MinKernel` 20.0.0. It does not need Lilu.
3. **First boot:** add the boot-arg `-amdzenro` (read-only). Check that the sensor readings are correct, then remove it.

## Configuration

Settings are applied in this order of priority: boot-arg, then the keys in `Info.plist → IOKitPersonalities`, then the built-in defaults.

| Boot-arg | Meaning |
|---|---|
| `-amdzenoff` | Do not load |
| `-amdzenro` | Monitor only; block all control writes, including PCC |
| `-amdzennocppc` | Disable CPPC selection; retain the legacy P-state backend |
| `amdzen.gov=<name>` | `performance`, `powersave`, `schedutil`, `ondemand`, `conservative`, `userspace`, `active` (CPPC only), `none` or `auto` |
| `amdzen.boost=0\|1` | Force boost off or on (default: leave as the BIOS set it) |
| `amdzen.epp=0..255` | EPP for `active` mode (128 = balance_performance) |
| `amdzen.period=<ms>` | Governor sampling period (default 40) |
| `amdzen.force=1` | Take control even when a conflicting kext is loaded |
| `amdzen.smn=1` | Read SMN sensors even when a conflicting kext is loaded (risky) |
| `amdzen.pstatewrite=1` | Allow editing P-state definitions (FID/DID/VID) through the user client |
| `amdzen.idle=auto\|0\|1\|2\|3` | CC6 idle hook (see below). `auto` (default): CC6 when the hardware checks pass, otherwise no hook. `0`: never install the hook. `1`: HLT. `2`: MWAIT C1. `3`: CC6 for long idles, MWAIT C1 for short ones |

`auto` selects `schedutil` on the legacy backend and `active` on CPPC. On CPUs without CPPC MSRs, including the Ryzen 7 5700X, **CPPC over PCC is on by default** when ACPI discovery and capability checks succeed. No opt-in boot-arg is needed. `-amdzennocppc` retains legacy control on a fresh boot; `-amdzenro` blocks both MSR and PCC writes. Neither option permits legacy control after a PCC enable attempt in the same boot.

The `cppc-pcc` backend runs autonomous native policy adaptively (50 ms while active, 200 ms when quiet): firmware selects frequency inside the requested range (`desired=0`), guided by scheduler urgency, per-CPU idle-derived busy load, AC power, thermal warnings and low-power-mode state (`kern.amdzen.lpm`: `-1` auto, `0` off, `1` on). Unknown native signals remain unknown. `kern.amdzen.epp` is the balanced base EPP; RT/background/busy hints adjust the per-CPU requests. This backend accepts `active` or `none` (which leaves a neutral autonomous request); legacy governors and P-state editing are unavailable. Boost `-1` (keep) preserves each CPU’s discovered boot setting. If either SMT sibling had boost disabled, active policy caps their shared maximum at nominal and preparation preserves HWCR.CpbDis. Explicit boost on/off changes CpbDis and the policy ceiling; returning to `-1` restores the boot setting. The native period is independent of `amdzen.period`.

Discovery/capability failures before any observed enable or enable attempt retain legacy policy. Before attempting enable, AMDZenPM sets `AMDZenPM.CPPCEnableMayHaveBeenSent` on the persistent IORegistry root and permanently retains its module, even when the idle hook is off. This conservative latch also covers an enable preflight failure. Once set, a replacement service or module reload without a live Channel remains `cppc-pcc`, degraded and without control writes until reboot; legacy/MSR control cannot resume. Transport/protocol and prerequisite failures quarantine writes with `block_reason=pcc-failed` and a numeric error.

Sleep cancels between every PCC command and phase, drains the single admitted command (transport budget about two seconds, plus scheduling/publication), and keeps the channel mapped. Wake validates enable/autonomy, accepted requests, P0 and CpbDis on every CPU, including partial firmware enablement and governor `none`. Disabled CPUs regain P0 before enable; an already enabled CPU with a lost P0 prerequisite is quarantined. Stop drains work, clears urgency, releases signals, submits a neutral request best-effort when writes remain permitted, then unmaps. Neutral requests use each shared domain’s capability intersection, so aliased SMT registers receive identical values. CPPC is never disabled.

### Governor sampling

With an active idle hook, legacy dynamic governors use coherent per-CPU idle accounting by default (`GovIdleSampling`, `amdzen.govidle`, or `kern.amdzen.gov_idle_sampling`). Each CPU's snapshot carries its own timestamp and includes an open idle interval. No new idle exits can mean either fully busy or still idle; the accounted time distinguishes them. Stable requests need no governor IPI; telemetry still samples APERF/MPERF (default every 1 s).

The default governor period remains **40 ms**. Schedutil requests the fastest **allowed** P-state at **60% busy or above**, irrespective of its smoothed frequency estimate (`FastUpPct`, `amdzen.fastup`, or `kern.amdzen.fast_up_pct`; 0 disables fast-up). Below that threshold, idle-derived utilization estimates frequency from the last requested P-state, not measured boost or throttling.

A snapshot retries at most eight times. Invalid accounting or a source change re-primes the affected baselines; fallback uses only real APERF samples from consecutive dynamic ticks. Without a usable pair, requests and governor history are held, not lowered from an unknown sample. Sleep/wake resets both sources. `Sensors.GovSource` reports `idle`, `aperf`, or `hold` (priming/no usable decision).

## Monitoring and runtime control

```sh
sysctl kern.amdzen                         # all values
sudo sysctl kern.amdzen.governor=ondemand
sudo sysctl kern.amdzen.boost=0            # -1 = return to the BIOS setting
sudo sysctl kern.amdzen.limit_slow=1       # slowest allowed P-state index
sudo sysctl kern.amdzen.idle_mode=2        # idle hook: 0/1 HLT, 2 MWAIT C1, 3 CC6 (-1 = no hook)
sudo sysctl kern.amdzen.idle_threshold_us=500   # predicted idle length that selects CC6
ioreg -l -w0 -c AMDZenPM | grep -A40 '"Sensors"'
/usr/bin/log show --last boot --predicate 'eventMessage CONTAINS "AMDZenPM"'   # zsh has a 'log' builtin; or: sudo dmesg | grep AMDZenPM
```

The IORegistry entry `CPUInfo` holds static data: the P-state table, features, CPPC capabilities and topology. The `Sensors` entry is updated once per `TelemetryPeriodMs` (default 1 s).

PCC diagnostics are `kern.amdzen.pcc_commands`, `pcc_latency_us`, `pcc_last_error`, and `pcc_state`. `cppc_highest`, `cppc_nominal`, `cppc_lowest_nonlinear`, and `cppc_lowest` report CPU0 capabilities; `cppc_min`, `cppc_max`, `cppc_epp`, and `cppc_requests` report the last accepted requests in XNU CPU order. Native signals are `native_ac_power` (`unknown/ac/battery`), `native_thermal` (0 unknown, 1 nominal through 5 emergency), `native_low_power_mode` (`unknown/off/on`), `native_rt_active`, `native_rt_mask`, and `native_urgency_active`. Error values are defined in `ZenPccModel.hpp`; 65536/65537/65538/65539/65540 mean policy/confirmation/interrupted/prerequisite/lifetime-latch failures. A firmware WRITE timeout leaves the actual request uncertain; diagnostics retain the last confirmed request.

For tools, the `AMDZenPMUserClient` user client exposes `AMDZenInfo` and `AMDZenSample` (see `AMDZenPM/src/AMDZenShared.h`). Any user can read; only an admin can change settings. For other kernel extensions, AMDZenPM exports `AMDZenPM_copyInfo()` and `AMDZenPM_copySample()` (`OSBundleCompatibleVersion` 0.0.1).

## SMC keys (AMDZenSMC.kext)

`AMDZenSMC.kext` is a VirtualSMC plugin. It publishes AMDZenPM's temperature, power and voltage readings as SMC keys, so iStat Menus, Stats and HWMonitorSMC2 show them like on a real Mac. `make` builds it next to AMDZenPM as `build/AMDZenSMC.kext`.

| Key | Type | Value | Published when |
|---|---|---|---|
| `TC0D` | sp78 | Tdie (Stats' first choice for "CPU") | Tctl readable |
| `TC0P`, `TC0p` | sp78 | Tctl (there is no board proximity sensor) | Tctl readable |
| `TCXc` | sp78 | hottest CCD (Tccd), else Tctl | Tctl readable |
| `TC0c`..`TC7c` | sp78 | the CCD temperature of each core | single-CCD parts only (one CCD sensor, at most 8 cores) |
| `PCPT`, `PCPR` | sp96 | package power (RAPL) | RAPL |
| `PCPC`, `PC0C` | sp96 | sum of per-core power | per-core energy MSR readable |
| `VC0C` | sp3c | Vcore (SVI2), else highest requested per-core VID | SVI2 or C001_0293 |
| `VCSC` | flt | VSoC (SVI2) | SVI2 reads non-zero |
| `IC0C` | flt | core current estimate (SVI2) | SVI2 reads non-zero |

- The key set is decided once, when AMDZenPM has finished its hardware discovery, and does not change afterwards. `ioreg -l -w0 -c AMDZenSMC` shows `SMCKeys` and `Status`.
- No frequency keys: no real Mac has one, and VirtualSMC asks plugins not to invent keys. Frequencies are in AMDZenPM's IORegistry entry, sysctl and user client.
- Tjmax-relative keys (`TC0E`, `TC0F`, `TCXR`...), `PSTR` (system total) and `PC0R` (12 V input) are not published.
- Values refresh every 1000 ms (aligned with `TelemetryPeriodMs`). Encoding is integer-only, and an SMC read only copies a pre-encoded value.
- Multi-CCD parts get `TCXc` but no per-core keys: `AMDZenInfo` has no core-to-CCD map yet.

**Installing (OpenCore `Kernel → Add`, in this order):**

1. `Lilu.kext`
2. `VirtualSMC.kext`
3. `AMDZenPM.kext`
4. `AMDZenSMC.kext` (`MinKernel` 20.0.0)

- Remove `SMCAMDProcessor.kext` (and `AMDRyzenCPUPowerManagement.kext`). `SMCProcessor.kext` is for Intel only and should not be injected either. If `SMCAMDProcessor` is loaded, or `SMCProcessor` actually started (it loads on AMD but its start fails there, which is harmless), AMDZenSMC publishes nothing and its `Status` reads `conflict`: two plugins with the same keys show duplicate rows, and they can free each other's values inside VirtualSMC.
- Boot-arg `-amdzensmcoff` keeps AMDZenSMC from starting.
- Once it has handed its keys to VirtualSMC, AMDZenSMC never unloads (VirtualSMC keeps pointers into it), and while it is loaded AMDZenPM cannot be unloaded either. With `-amdzensmcoff` (or a conflict) it registers nothing and may be unloaded, so for AMDZenPM development boot with `-amdzensmcoff`.

The VirtualSMC SDK and Lilu headers that the build needs are vendored in `third_party/` (BSD-3-Clause, see `third_party/README.md`). `make check` also link-checks AMDZenSMC against the VirtualSMC 1.3.9 and Lilu 1.7.3 bundles in `deps/` and the freshly built AMDZenPM, and fails if the binary contains SSE/AVX instructions.

## CC6 idle hook

Without a power-management kext, macOS idles every CPU with `sti; hlt`. On Zen that reaches only the shallow CC1 state. AMDZenPM can install its own idle routine through the kernel's exported `pmKextRegister` interface, so that idle cores enter CC6 (core power-gated).

- **How it enters CC6:** it reads one I/O port, `CStateAddr + k`. `CStateAddr` comes from MSR C001_0073. The core intercepts that read and runs C-state action CCRk from MSR C001_0296. `k` (1 or 2) is taken from the deepest SystemIO entry of ACPI `_CST` on processor 0 that lies inside the trap window. Without `_CST`, CCR1 is used, or CCR2 if CCR1 has CC6 disabled. CCR0 is never used. The read is done with interrupts masked, like Linux `acpi_idle`, so a wake-up cannot be lost.
- **Deep or shallow:** each CPU keeps an average of its measured idle lengths. If that average is at least `kern.amdzen.idle_threshold_us` (default 50 µs; personality key `IdleThresholdUs`. Compare settings with `sudo sh tools/idlebench.sh`), the CPU enters CC6. Otherwise it uses MWAIT C1, or HLT when MWAIT is unavailable.
- **When the hook is installed:** only if all of these hold:
  - Darwin 20–25.
  - Not running under a hypervisor.
  - Control is not blocked (no `-amdzenro`, no conflicting kext).
  - No other power-management kext class is loaded (`AMDRyzenCPUPowerManagement`, `SMCAMDProcessor`, `AMDCPUSupport`, `AppleIntelCPUPowerManagement`). A second registration would panic the kernel.
  - The CPU has ARAT and an invariant TSC.
  - For `auto`: CStateAddr is non-zero and identical on every CPU, HWCR[4] is set, and CC6 is enabled for the chosen CCR. An explicit `amdzen.idle=1|2|3` installs the hook anyway, limited to what the hardware supports.
- **Cannot be unloaded once installed.** The kernel keeps a pointer to the kext's table, and no exported call removes it. AMDZenPM therefore takes a permanent reference on itself. Stopping the driver only restores the kernel's own HLT idle. **If the system hangs at boot or after wake, disable the hook with `amdzen.idle=0` from the OpenCore boot picker.** If your picker setup cannot edit boot-args, add it to `boot-args` under `NVRAM → Add → 7C436110-AB2A-4BBB-A880-FE41995C9F82` in `config.plist` from another OS (the EFI partition is FAT), or run `nvram boot-args="... amdzen.idle=0"` from Recovery. With `amdzen.idle=0` the kext still reads the C-state MSRs for `CPUInfo`, but it does not evaluate `_CST` and does not install the hook.
- **Sleep:** before sleep the hook drops to HLT. After wake, it checks HWCR and C001_0073/0296 again on every CPU before restoring MWAIT/CC6. Lost capabilities stay disabled until reboot. Governor sampling baselines are reset so no window spans sleep.
- **Status:** see `CPUInfo` in the IORegistry. `IdleHook` reads `registered` or gives the reason the hook was not installed. `IdleCC6` reads `ok` or gives the reason CC6 is unavailable. The entry also holds `CStateBaseAddr`, `CStateCfg`, `CStatePolicy`, `IdlePort`, `IdleCcr`, `IdlePortSource` and `AcpiCST` (array of `{Type, Space, Port, Latency}`).
  - `Sensors` adds `IdleMode`, `CC6Residency_permille`, `C1Residency_permille`, `IdleEntriesDeep`, `IdleEntriesShallow`, `IdleMonitorWakes` and `IdleTimerMigrates` (counts per second).

Comparing idle power (leave the machine idle for each step):

```sh
sudo sysctl kern.amdzen.idle_mode=1; sleep 30; sysctl kern.amdzen.cores_power_mw kern.amdzen.pkg_power_mw   # HLT (same as no hook)
sudo sysctl kern.amdzen.idle_mode=2; sleep 30; sysctl kern.amdzen.cores_power_mw kern.amdzen.pkg_power_mw   # MWAIT C1
sudo sysctl kern.amdzen.idle_mode=3; sleep 30; sysctl kern.amdzen.cores_power_mw kern.amdzen.pkg_power_mw   # CC6
ioreg -l -w0 -c AMDZenPM | grep -E 'CC6Residency|C1Residency|IdleEntries'
```

If CC6 works, `cores_power_mw` in mode 3 should be well below the HLT value, and `CC6Residency_permille` should be high on an idle machine. Each value is averaged over one telemetry period.

## Safety design

- Every MSR is probed with `rdmsr_carefully` on every CPU before use. A capability counts only if the read worked on all CPUs. MSRs are written only after that check, because the kernel exports no `wrmsr_carefully`.
- **HWCR:** only bit 25 (CpbDis) and bit 30 (IRPerfEn) are ever changed.
- **Delayed start:** hardware initialisation waits, for up to 30 s, until every CPU is online and PCI 00:00.0 has appeared.
- **Sleep, unload and conflicts:** legacy control returns every thread to P0 before sleep and restores boot boost on stop/conflict. MSR CPPC restores its boot request. PCC follows the drain/check/neutral lifecycle above and never returns to legacy writes after enable. A late conflict atomically revokes native admission and drains an already admitted command outside the work-loop gate before the notification handoff completes. No command is admitted afterward.
- **P-state editing:**
  - It is off unless `amdzen.pstatewrite=1` is set.
  - It is supported only on SVI2 parts in family 17h/19h.
  - Allowed range: 400 to 6000 MHz and 700 to 1450 mV, with only valid DIDs.
  - P0 can be edited only when HWCR[21] LockTscToCurrentP0 is set.

## Not done yet

- **CC6 idle hook:** booted and measured on a Ryzen 7 5700X (with MWAIT monitor-write wakeup, `rtcDeadline` timer migration, and CPPC preferred-core routing; 87.5% idle CC6 residency, 0 latency vetoes). Sleep/wake with the hook active is not yet tested. Its `pmDispatch_t` layout is checked only for Darwin 20–25.
- **PCC hardware validation:** booted and verified on a Ryzen 7 5700X (macOS 26 / Darwin 25.6.0) with live ACPI PCC discovery, autonomous CPPC/EPP policy, and preferred-core selection. Sleep/wake and native thermal/LPM transitions remain to be verified across additional Zen hardware. See `docs/research/native-policy.md` for signal limits and [native-integration-fixes.md](docs/research/native-integration-fixes.md) for offline integration notes.
- **`zenctl` command-line tool**
- **SVI3 telemetry (Zen4/Zen5):** no public description exists.
