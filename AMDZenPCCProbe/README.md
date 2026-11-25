# AMDZenPCCProbe

**Separate, fail-closed CPPC/PCC metadata diagnostic. Not a working physical READ driver or CPU controller.** The production kext unconditionally supplies a null transport. No flag, request, privilege or configuration can enable firmware access in this release.

`pccprobe info` can validate the current OS-exposed ACPI profile and existing AMDZenPM topology **without loading the probe**. CPPC register values remain `unknown`; integer declarations are labelled static, not measured. This does not replace schedutil, activate XCPM, translate native DVFS requests, enable CPPC autonomy, or choose CPU performance. A usable native policy-output source and verified firmware transport remain separate prerequisites.

## Build and offline checks

Requires an existing Xcode/macOS SDK with Kernel.framework headers, Python 3, and the already-built `../build/AMDZenPM.kext`. Nothing downloads dependencies or rebuilds the main AMDZenPM/AMDZenSMC bundles.

From the repository root:

```sh
make -C AMDZenPCCProbe all
make -C AMDZenPCCProbe check CAPTURE=/absolute/path/to/saved-metadata-directory
```

`CAPTURE` must explicitly identify an existing private directory containing `PCCT.bin`, `APIC.bin`, `SSDT-2.bin` and `SSDT-3.bin`. These are saved original-profile table bytes, not a hardware transport. No machine-local temporary path is assumed and no capture happens automatically. The sanitized PCCT/MADT and synthetic XNU map in `tools/cppc-lab/fixtures` are intentionally insufficient for this profile-specific test. Original firmware binaries are not copied into this tree.

Build products, all under this directory's ignored `build/`:

- `AMDZenPCCProbe.kext` — x86_64, ad-hoc signed, configuration gate closed.
- `pccprobe` — x86_64 diagnostic CLI, ad-hoc signed.
- `tests`, `tests-san` — host tests using saved metadata and a fake transport.

`check` runs optimized tests and ASan/UBSan with warnings as errors; builds and verifies signatures; resolves imports offline against installed kernel collections plus the existing declared AMDZenPM dependency; and audits the no-physical-access boundary. The SDK's `libkmod` can warn that its objects target a newer OS than the 12.0 deployment target. Offline linkage does not prove compatibility on every macOS release.

Verified on 2026-11-14: **22 groups, 2062 checks, zero failures per optimized/sanitized variant**; all 351 imports resolved, both signatures verified, and the structural audit passed. The unchanged CPPC lab also passed **19 groups, 5211 checks per variant** plus importer/freestanding checks. Metadata-only CLI inspection succeeded against current OS tables and existing AMDZenPM on XNU CPU 15 without a probe load. This is not a loaded-probe or firmware test.

## Metadata-only inspection

```sh
AMDZenPCCProbe/build/pccprobe --help
AMDZenPCCProbe/build/pccprobe info --cpu 0 --fields all
```

Default invocation is `info` for XNU logical CPU 0. `--help`, including `read --help`, returns before service lookup or method invocation. Lookup allocation/API/iterator failures are errors, not evidence that the probe is absent. Direct INFO fallback is used only after confirmed empty enumeration: a valid empty iterator **or the documented IOKit success-with-NULL result**. A nonzero API error with a NULL iterator still refuses fallback.

INFO uses only the existing AMDZenPM GetInfo/copyInfo ABI and the `AppleACPIPlatformExpert` **ACPI Tables** property. It does not call GetSample, evaluate AML, inspect PCC shared memory, sample FFH registers, send IPIs, or write firmware. The output distinguishes:

- Validated descriptor metadata and addresses, never dereferenced by this release.
- `unknown` PCC/FFH values and unsupported fields.
- Static integer declarations, explicitly not current measurements.
- Transport availability, gate/attempt state, and ownership observations; a Command Complete observation is **not** an exclusive OS lease.

Support is deliberately limited to the reviewed Ryzen 7 5700X family 19h/model 21h profile: 16 logical CPUs, eight HW_ALL domains and one generic polling PCC subspace. Both static processor/CPC SSDTs must match the locked content hashes in `src/Profile.hpp`; hashes are a profile check, not authentication. PCCT addresses are derived from current OS tables, not embedded addresses or saved captures. ACPI/MADT/APIC identities join to AMDZenPM's independently indexed XNU topology; names, table order and the lab's synthetic XNU map are not runtime numbering sources. Changed, missing, duplicated, unsupported or inconsistent data fails closed.

## READ refusal and safety boundary

A real PCC READ **writes command/status and rings a firmware doorbell**, even if no performance payload is changed. The explicit `read` selector requires kernel administrator authorization, an opt-in boot argument (`amdzenpccprobe=1`), the boot-time `EnablePccReadProbe` property, a bounded timeout and the unmistakable CLI acknowledgement. These gates are diagnostic request checks, **not** authorization to install or test hardware here.

Even with every gate satisfied, the current build returns `TransportUnavailable` before mapping, clock/header access or device writes. A qualifying attempt is latched; subsequent attempts are refused. There is no arbitrary address/MSR/port/payload-write selector, runtime re-arm, force-unlock, install/load target, automatic retry, timer, scheduler hook or PM-dispatch registration.

The missing binding must establish an exclusive PCC-consumer lease, validated physical mappings/cache attributes, exact-width device operations, ordering barriers, and a teardown/power/commit fence. Inspecting Command Complete, adding a process-local mutex, or changing a flag is not a substitute. **Do not replace the null transport with raw mappings as a workaround.**

The common fake-tested engine models exact READ-only header/doorbell ordering, mask preservation, bounded deadlines/rate spacing, requested-field collection, error/cancellation handling, and one attempt per service lifetime. Timeout does not release possible firmware ownership; late/error data is discarded and uncertain ownership is quarantined without reuse.

## Lifecycle and verification limits

The diagnostic adapter balances superclass startup on failure and does not consume admission before its lock is allocated. Stop, power invalidation, client closure and final reply commitment share a sleep mutex. Cancellation checks and descriptor/value clearing occur inside that commitment boundary; a successful reply is a snapshot as of commitment, not a promise that the system cannot change afterwards. Stop drains admitted calls before cleanup. Priority CPU-capability transitions are handled synchronously with zero wait; wake never re-arms an invalidated service.

Host regression tests exercise the production-used profile, ABI, lookup, cancellation/commit, capability and READ-engine helpers with fake handles, clocks and transport. They are not real IOKit loading/startup/teardown tests, firmware responses, physical mapping tests, cache/barrier validation or sleep/wake hardware tests. The structural audit rejects direct mapping-class imports and device instructions; inherited IOKit vtable parameter types alone do not indicate mapping execution. The audit is not a proof of hardware or lifecycle safety.

Before any future physical test: inspect metadata without a probe load, establish and review the missing lease/transport/lifecycle binding, and obtain separate authorization for installation and the command/status/doorbell writes. This release cannot perform that test.
