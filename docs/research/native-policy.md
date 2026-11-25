# Native macOS signals and autonomous CPPC policy

Implementation report, 2026-11-15. Checked read-only on this Ryzen 7 5700X / MacPro7,1 host: Darwin 25.6.0, 16 logical / 8 physical CPUs, `machdep.xcpm.mode = 0`. The implementation observes scheduler urgency and registry state; the integrator must connect its thread call and the separate PCC transport. The policy always requests `desiredPerf = 0`.

## Files

Created `AMDZenPM/src/ZenNativeSnapshot.hpp`, `ZenNativeSignals.hpp/.cpp`, `ZenNativePolicy.hpp/.cpp`, `tests/test_native_policy.cpp`, and this report. Edited `AMDZenPM/src/ZenIdle.cpp/.hpp` to populate, rearm, and clear the urgency callout alongside the existing idle hook. Existing uncommitted idle-accounting work was preserved. No other source files, makefiles, build directories, probe files, or PCC files were edited.

## Scheduler evidence and lifetime

Apple's [xnu-12377.121.6 sched_urgency.h](https://github.com/apple-oss-distributions/xnu/blob/xnu-12377.121.6/osfmk/kern/sched_urgency.h) defines:

| Class | Value | Treatment |
| --- | ---: | --- |
| `THREAD_URGENCY_NONE` | 0 | Idle; energy bias once foreground hold expires |
| `THREAD_URGENCY_BACKGROUND` | 1 | Background/min-power hint |
| `THREAD_URGENCY_NORMAL` | 2 | Balanced |
| `THREAD_URGENCY_REAL_TIME` | 3 | Performance EPP and raised minimum floor |
| `THREAD_URGENCY_LOWPRI` | 4 | Balanced; explicitly distinguished from background by XNU |

Local source evidence: `deps/xnu-src/xnu-12377.121.6/sched_prim.c:3788` notifies the new current thread in `thread_dispatch`; line 3831 notifies idle, and lines 5902/6084 notify urgency changes without a context switch. `thread_get_urgency`, at line 6303, explains the LOWPRI distinction and supplies non-RT QoS/priority arguments in the same argument positions as RT period/deadline. These arguments are therefore ignored, rather than mistaken for RT timestamps.

`pmCPU.c:760` asserts preemption disabled **or** interrupts disabled, checks `pmInitDone` and the slot, and measures the callout when `urgency_notification_assert_abstime_threshold` is enabled. It can panic for an excessive callout duration. The observer does not measure time, allocate, log, lock, schedule work, or run policy/transport code. All publication operations inline; the sole function call is the exported current-CPU accessor `cpu_number()`. This is a narrow exception to the requested literal “no calls”: obtaining the OS logical CPU through the KPI avoids embedding private CPU-data offsets. Read-only disassembly of this running kernel confirmed the accessor is five instructions: frame prologue, one `%gs` CPU-index read, frame epilogue, return. The callback's generated code has no lock-prefixed instructions or other calls.

The publication is one aligned atomic 64-bit word per CPU: current class/observed flag and two 28-bit wrapping counters for RT and foreground notifications. Single-word loads cannot mix classes with counters from another publication. Only the owning CPU writes; consumers never clear live words. RT/foreground counter changes preserve bursts that begin and end between samples. The first sample primes counters; current urgency takes effect immediately, but historical events are not replayed after reset. An exact full counter wrap between two samples is indistinguishable from no events; 268,435,456 notifications per sampling interval would be required.

`pmActiveRTThreads` is left NULL: `pmCPU.c:870` implements its wrapper but current OSS XNU has no callers. The per-CPU RT event counters supply useful activity without an unverified global call-context assumption. `pmThreadGoingOffCore` remains NULL.

The table's 0xC8 offset is compile-time asserted. Its urgency pointer is set before the existing once-per-boot `pmKextRegister`, and XNU's existing `initComplete` gating applies. Rearm atomically restores both urgency and MachineIdle; unhook atomically clears both. In-flight calls only touch resident code/BSS; the existing kext self-retain and one-registration guard are preserved. The MachineIdle implementation, idle configuration, latency callouts, and registration eligibility were not changed by this work. Native signal availability follows registration eligibility; if the existing idle gate declines registration, CPU snapshots are unobserved.

## Signals observed on this machine

| Signal | Read-only evidence | Kernel observer |
| --- | --- | --- |
| Urgency | XNU source/dispatch ABI above | `pmThreadTellUrgency`; available after existing hook registration/initComplete |
| Power source | `ioreg -l -w0 -d1 -r -c X86PlatformPlugin`: `CPUOnACPower = Yes`; `pmset -g batt`: AC Power | Prefer root-domain `getAggressiveness(kPMPowerSource)` on successful documented values; otherwise `IOPMPowerSource.ExternalConnected`; otherwise the observed X86PlatformPlugin OSBoolean property. Record evidence source. |
| Low Power Mode | No low-power-mode/energy-mode property found in root domain or registry; `pmset -g custom` contains no `lowpowermode` setting | Unknown. No documented LPM kernel setting/notification was found in this SDK/XNU source. `Low Power Sleep`, device `kIOPMLowPower`, and `kern.lowpowerwake` describe other states. No speculative key or private XCPM symbol is used. |
| Thermal pressure | Root domain has no `Power Status` / `Thermal_Level_Warning`; `pmset -g therm` reports no recorded thermal/performance/CPU-power warning | Unknown until a producer publishes `Power Status["Thermal_Level_Warning"]`. Retained property reads decode only documented common/x86 values. |

`ioreg -r -c IOPMPowerSource` returned no service. The root-domain aggressiveness getter cannot be invoked from a userspace read-only shell, so its current value was not asserted; the fallback property is demonstrably published. `machdep.xcpm.power_source`, `.cpu_thermal_level`, `.gpu_thermal_level`, and `.io_thermal_level` all exist with value 0, but inactive XCPM makes them unsuitable as live policy evidence. ThermalConfiguration's Nominal/Moderate/Heavy/Trapping/Sleeping dictionaries are static thresholds, not current pressure measurements.

The observer uses normal, retained IORegistry/KPI reads from preemptible thread context. It polls at 250 ms instead of installing additional notification lifetimes. XNU `IOPMrootDomain.cpp:8732` copies/replaces the `Power Status` dictionary and broadcasts `kIOPMMessageSystemPowerEventOccurred`; polling that published dictionary observes the same state without calling private setters. Thermal values map as follows: 0 nominal; 5 moderate; 100 heavy; 10 critical; 110 emergency/trap; 255 and other platform-specific values unknown. These are a conservative policy mapping, not a claim that IOPM warnings reproduce XCPM's internal pressure scale.

MacPro7,1's `IOPlatformPowerProfile.FrequencyVectors` is an array containing one 7,668-byte OSData blob. No documented format or verified native EPP field was established, so decoding was deliberately skipped. Balanced EPP defaults to 0x80; the integrator can assign the existing EPP setting to `Tunables::baseEpp`.

## Integration API

All signatures below are in namespace `zennative`, except the existing hook API. Types are declared in the corresponding headers.

```cpp
void initSignals(SignalState &state);
void stopSignals(SignalState &state);
void snapshotCpus(CpuSnapshot *out, uint32_t ncpu);
void snapshotSignals(SignalState &state, uint64_t nowMs, uint32_t ncpu);
bool tickSignals(SignalState &signals, PolicyState &policy,
                 const Topology &topology, const zencppc::Caps *caps,
                 const Tunables &tunables, uint64_t nowMs, TickResult &result);

Tunables defaultTunables();
bool validCaps(const zencppc::Caps &caps);
ThermalLevel decodeThermalWarning(uint32_t raw);
zencppc::Request decide(const CpuSnapshot &cpu, bool recentRT,
                       bool recentForeground, const SystemSignals &system,
                       const zencppc::Caps &caps, const Tunables &tunables);
bool tick(PolicyState &state, const Topology &topology,
          const CpuSnapshot *cpus, const SystemSignals &system,
          const zencppc::Caps *caps, const Tunables &tunables,
          uint64_t nowMs, TickResult &result);
void reset(PolicyState &state);
void invalidateRequests(PolicyState &state);
bool cpuChanged(const TickResult &result, uint32_t cpu);
bool cpuValid(const TickResult &result, uint32_t cpu);

// Pure publication primitives:
void recordUrgency(CpuPublication &publication, int value); // always inline
CpuSnapshot readCpuSnapshot(const CpuPublication &publication);

// Existing hook API; only the availability query is new:
bool zen::idle::registerHook(const zen::idle::HookConfig &cfg);
void zen::idle::rearm(const zen::idle::HookConfig &cfg);
void zen::idle::unhook();
bool zen::idle::nativeSignalsActive();
```

`initSignals`, `stopSignals`, `snapshotSignals`, and `tickSignals` require preemptible thread context with no spinlocks held, serialized on the owning work loop/thread call. Zero-initialize `SignalState` before first use. Keep `SignalState`, `PolicyState`, and `TickResult` in long-lived allocated/member storage; they should not be large kernel stack temporaries. Call `reset(policy)` on initial activation, wake, or topology changes. Use monotonic elapsed **milliseconds**; convert `mach_absolute_time` through the normal timebase rather than passing raw ticks. A backwards clock resets policy history; normal suspend/wake still needs an explicit reset.

Call `tickSignals` every 50 ms (25 ms is also supported). It snapshots urgency each call and polls global properties at most once per 250 ms after initialization. `Topology::domainOfCpu` must contain normalized probed `coreOfCpu` or `_PSD` domain indices below 256, with one or two CPUs per domain. Do not infer sibling pairs from consecutive OS CPU numbers. Copy the PCC reader's per-CPU `zencppc::Caps` into the input array. On this 5700X, provide 16 CPUs in eight HW_ALL domains. Invalid/missing caps suppress both siblings; disjoint ranges also suppress the domain.

On a true return, submit only `cpuChanged(result,cpu)` requests with valid bits set. Both siblings are marked and receive identical requests: highest sibling minimum floor, lowest sibling EPP, and the intersection of upper limits/capabilities. Domains need a common valid range with both siblings' lowestNonlinear at or below both nominal ceilings; conflicting capabilities suppress that domain. A false return indicates invalid input/topology and clears the output count/masks. A true return with zero changes requires no transport command. Requests remain autonomous throughout.

The output is a **batch**, not permission to issue one command per CPU. Coalesce all dirty fields in each PCC shared-memory subspace and issue one command for that subspace, respecting the transport's own serialization/firmware latency. The 50 ms hard minimum limits policy output to at most 20 batches/s; if the actual PCC layout needs multiple commands per batch, transport budgeting must additionally bound commands system-wide. Returned requests are treated as accepted; the transport must retain/retry its newest queued batch, or call `invalidateRequests(policy)` on a failed/dropped write. Invalidation retries at the next allowed batch without resetting the rate limit.

`snapshotCpus` may run in ordinary thread context on any CPU and atomically reads each publication; hook-off results are unobserved. `recordUrgency` requires the owning CPU's non-reentrant scheduler context with preemption disabled/interrupts off. Only the PM callback uses it in the kernel. `readCpuSnapshot` is a single atomic load and may race that writer. Pure `decide`/decode/cap helpers have no mutable global state. Pure `tick`, `reset`, and invalidation require serialized access to the caller's state and belong in the thread call, never the scheduler callout.

Hook registration/rearm/unhook retain their existing AMDZenPM work-loop context and eligibility rules. Do not call `pmKextRegister` from the observer. Before stop, cancel and drain the integrator's thread call, clear the hook through the existing unhook path, and release observer references with `stopSignals`.

## Policy defaults

Normal/LOWPRI: EPP 128, min lowest, max highest. Background/idle with no recent foreground: EPP at least 192. Realtime/current or recent: EPP at most 32/base, min at least lowestNonlinear and 80% nominal. Internal power raises balanced EPP to at least 160. LPM raises EPP to at least 224 and caps maximum at nominal, including for realtime. Thermal moderate/heavy/critical/emergency progressively cap the range above lowestNonlinear to 75%/50%/25%/0%. Floors yield to safety caps, and all requests clamp `lowest <= min <= max <= highest`.

RT activity holds 250 ms after the last observation. Foreground activity holds 200 ms before a CPU is treated as background-only. Less-demanding changes must be stable for another 200 ms; performance demand increases and tighter maximum caps bypass that relaxation hold, but still obey the global 50 ms output interval. Thus background relaxation after a short RT burst can take approximately 450 ms, plus tick quantization.

## Validation

Exact host command, also at the top of the test file, from repository root:

```sh
clang++ -std=c++17 -O1 -g -Wall -Wextra -Werror -Wno-unused-function -fsanitize=address,undefined -fno-sanitize-recover=all -IAMDZenPM/src tests/test_native_policy.cpp AMDZenPM/src/ZenNativePolicy.cpp -o /private/tmp/test_native_policy && /private/tmp/test_native_policy
```

Result: **26 tests, 633,146 checks, 0 failures**, with ASan/UBSan enabled. Coverage includes single-writer concurrent publication (200,000 reader snapshots), counter wrap, background/LOWPRI distinction, RT bursts/holds, EPP and cap clamping, every documented thermal mapping, nonadjacent sibling domains, incompatible/missing caps and conflicting nominal/nonlinear limits, output masks through CPU 255, transport retry, clock/reset handling, stable relaxation, the 20-batches/s limit, and 5,000 deterministic randomized request invariants. Existing `test_idle`: **14 tests, 269 checks, 0 failures**. Existing `test_idle_sampling`: **14 tests, 1,522 checks, 0 failures**. Both were independently built/run with the same sanitizer flags in the temporary directory.

Kernel compile checks used the tests/Makefile `kcheck` flags plus `-Werror`, debug symbols, and the root Makefile's warning suppressions. Reproduction without touching any repository build directory:

```sh
native_sdk=$(xcrun --sdk macosx --show-sdk-path)
native_kfw="$native_sdk/System/Library/Frameworks/Kernel.framework"
native_check=$(mktemp -d /private/tmp/amdzen-native.XXXXXX)
for native_source in ZenNativePolicy ZenNativeSignals ZenIdle ZenIdleModel; do
  xcrun --sdk macosx clang++ -arch x86_64 -isysroot "$native_sdk" \
    -mmacosx-version-min=11.0 -mkernel -fno-builtin -fno-common -nostdinc \
    -I"$native_kfw/PrivateHeaders" -I"$native_kfw/Headers" \
    -DKERNEL -DKERNEL_PRIVATE -DDRIVER_PRIVATE -DAPPLE -DNeXT \
    -O2 -g -Wall -Wextra -Werror -Wno-unused-function -Wno-deprecated-declarations \
    -Wno-inconsistent-missing-override -Wno-overloaded-virtual \
    -std=gnu++17 -fapple-kext -fno-exceptions -fno-rtti -fcheck-new \
    -c "AMDZenPM/src/$native_source.cpp" -o "$native_check/$native_source.o"
done
nm -u "$native_check/ZenNativePolicy.o" "$native_check/ZenNativeSignals.o" \
  "$native_check/ZenIdle.o" "$native_check/ZenIdleModel.o"
```

All four compiled successfully with **zero compiler warnings/errors** against Xcode's macOS 26.5 Kernel.framework. The three owned translation units have **zero `mod_init_func` sections and zero SSE/AVX/x87/MMX instruction matches**. ZenNativePolicy's compiler support imports are `___bzero`, `___stack_chk_fail`, and `___stack_chk_guard`; its source uses no kernel APIs or floating point. New signals import only normal IOKit/libkern KPIs plus this module's policy/availability functions. Their virtual property/getAggressiveness/retain/release calls use exported class ABIs.

An isolated temporary kext linked the four objects plus the **existing** module metadata, without AMDZenPM.cpp or PCC objects. `tools/linkcheck.sh` against installed kernel collections found **22 imports, 22 resolved, zero unresolved**; all providers are already declared by AMDZenPM/Info.plist. The SDK's libkmod startup/shutdown objects emitted their existing minimum-version warning (built for 26.5, repository link minimum 11.0); compilation and symbol resolution passed.

| Export provider | Remaining external symbols after isolated link |
| --- | --- |
| `com.apple.kpi.unsupported` | `_cpu_number`, `_pmKextRegister` |
| `com.apple.kpi.iokit` | `_IOLog`, `__ZN9IOService15getPMRootDomainEv`, `__ZN9IOService15serviceMatchingEPKcP12OSDictionary`, `__ZN9IOService19getMatchingServicesEP12OSDictionary`, `__ZN9IOService9metaClassE`, `__ZNK9IOService10isInactiveEv` |
| `com.apple.kpi.libkern` | `_OSKextRetainKextWithLoadTag`, `__ZN12OSDictionary9metaClassE`, `__ZN15OSMetaClassBase12safeMetaCastEPKS_PK11OSMetaClass`, `__ZN6OSData9metaClassE`, `__ZN7OSArray9metaClassE`, `__ZN8OSNumber9metaClassE`, `__ZN8OSString11withCStringEPKc`, `__ZN9OSBoolean9metaClassE`, `___bzero`, `___stack_chk_fail`, `___stack_chk_guard`, `_strcmp`, `_strlcpy` |
| `com.apple.iokit.IOACPIFamily` (existing idle `_CST` dependency) | `__ZN20IOACPIPlatformDevice9metaClassE` |

## Open integration risks

No live kext load/unload, sudo, pmset toggle, EFI/NVRAM change, hardware write, root make, git commit, REA tool, or network installation was performed. Actual urgency callout execution, firmware response, thermal transitions, and power-source transitions remain untested at runtime. Registration remains governed by the existing idle eligibility gate and permanently pins the kext once successful.

`CPUOnACPower` is a published property with observed OSBoolean format, but its cross-version freshness/transition behavior is not a documented contract; the observer prefers documented root/source KPIs and fails to unknown on a missing/wrong type. LPM and live thermal pressure are unknown on this machine, so they currently cannot constrain policy. IOPM warning categories and the chosen EPP/floor/thermal percentages are local policy decisions guided by native signals, not an exact reproduction of private XCPM algorithms. FrequencyVectors do not supply a justified EPP baseline.

A short RT burst entirely before the first priming tick can be missed; later bursts are retained by counters. Scheduler collection intentionally has no per-event timestamps. The tick may add up to a policy period of RT response delay, and 250 ms property polling adds thermal observation delay. Wake/topology changes need explicit state reset; firmware write failures need retry/invalidation. Domain identification, CPPC enable/autonomous/EPP support, PCC batching, and coexistence with the existing software desired-performance governor belong to the integrator/transport. The old governor must not simultaneously overwrite these autonomous requests. The literal no-call requirement retains the single `cpu_number` KPI exception described above.
