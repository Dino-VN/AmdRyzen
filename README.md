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
| `-amdzenro` | Monitor only; never write control MSRs |
| `amdzen.gov=<name>` | `performance`, `powersave`, `schedutil`, `ondemand`, `conservative`, `userspace`, `active` (CPPC only), `none` or `auto` |
| `amdzen.boost=0\|1` | Force boost off or on (default: leave as the BIOS set it) |
| `amdzen.epp=0..255` | EPP for `active` mode (128 = balance_performance) |
| `amdzen.period=<ms>` | Governor sampling period (default 40) |
| `amdzen.force=1` | Take control even when a conflicting kext is loaded |
| `amdzen.smn=1` | Read SMN sensors even when a conflicting kext is loaded (risky) |
| `amdzen.pstatewrite=1` | Allow editing P-state definitions (FID/DID/VID) through the user client |
| `amdzen.idle=auto\|0\|1\|2\|3` | CC6 idle hook (see below). `auto` (default): CC6 when the hardware checks pass, otherwise no hook. `0`: never install the hook. `1`: HLT. `2`: MWAIT C1. `3`: CC6 for long idles, MWAIT C1 for short ones |

`auto` selects `schedutil` on the legacy P-state backend and `active` with EPP on the CPPC backend. The Ryzen 5000 desktop parts, including the 5700X, have no CPPC MSRs, so they use the legacy backend.

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

For tools, the `AMDZenPMUserClient` user client exposes `AMDZenInfo` and `AMDZenSample` (see `AMDZenPM/src/AMDZenShared.h`). Any user can read; only an admin can change settings. For other kernel extensions, AMDZenPM exports `AMDZenPM_copyInfo()` and `AMDZenPM_copySample()` (`OSBundleCompatibleVersion` 1.0.0).

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
- Values refresh every 500 ms (half of `TelemetryPeriodMs`). Encoding is integer-only, and an SMC read only copies a pre-encoded value.
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
- **Sleep:** before sleep the hook drops to MWAIT. After wake, it reads C001_0073/0296 again on every CPU. CC6 is re-enabled only if the values are unchanged. If they changed, the hook stays at MWAIT until the next reboot.
- **Status:** see `CPUInfo` in the IORegistry. `IdleHook` reads `registered` or gives the reason the hook was not installed. `IdleCC6` reads `ok` or gives the reason CC6 is unavailable. The entry also holds `CStateBaseAddr`, `CStateCfg`, `CStatePolicy`, `IdlePort`, `IdleCcr`, `IdlePortSource` and `AcpiCST` (array of `{Type, Space, Port, Latency}`).
  - `Sensors` adds `IdleMode`, `CC6Residency_permille`, `C1Residency_permille`, `IdleEntriesDeep` and `IdleEntriesShallow` (entries per second).

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
- **Sleep, unload and conflicts:** before sleep, every thread is returned to P0. On wake, CPPC, boost and the active policy are applied again. On unload, P0, the boot CPPC request and the boot boost state are restored. If a conflicting kext appears later, AMDZenPM does the same restore and switches to monitor-only.
- **P-state editing:**
  - It is off unless `amdzen.pstatewrite=1` is set.
  - It is supported only on SVI2 parts in family 17h/19h.
  - Allowed range: 400 to 6000 MHz and 700 to 1450 mV, with only valid DIDs.
  - P0 can be edited only when HWCR[21] LockTscToCurrentP0 is set.

## Not done yet

- **CC6 idle hook:** booted and measured on a 5700X (idle cores 4.2 W with MWAIT C1, 2.1 W with CC6 at 50 µs). Sleep/wake with the hook active is not yet tested. Its `pmDispatch_t` layout is checked only for Darwin 20–25.
- **ACPI `_CPC` (shared-memory CPPC):** needed for Zen3 desktop with CPPC enabled in the BIOS.
- **`zenctl` command-line tool**
- **SVI3 telemetry (Zen4/Zen5):** no public description exists.
