# Offline CPPC lab

**Discovery/protocol groundwork, not a working CPPC driver.** Nothing here is connected to AMDZenPM, XNU callbacks, ACPI method evaluation or hardware. There is no native-mode-ready flag, governor, target-frequency decision, enable/autonomy command, or fallback policy. The separate, paused schedutil work is not an input to this build.

The prospective direction remains macOS constraints plus verified AMD autonomy, **not native XCPM**. Neither usable changing native performance inputs nor firmware responsiveness/autonomous operation has been established by this lab. Even a future hardware **PCC READ writes command/status and rings the doorbell**; it requires separate hardware authorization.

## Build and test without hardware

Requires an existing C++17 compiler and make. No dependencies are installed or downloaded by these targets. Run from any directory:

```sh
make -C /Volumes/Data/Projects/AmdCpu/tools/cppc-lab check
```

`check` builds/runs optimized tests and the importer, repeats both under ASan/UBSan, and compiles the core freestanding. All compilations use `-Werror`, conversion/sign/shadow/format warnings, `-fno-exceptions`, and `-fno-rtti`. Sanitizers stop on an error. `make ... sanitize` runs only the sanitizer variant. Only this directory's ignored `build/` receives build products; no kext is built or installed.

Verified with Apple clang 21.0.0, x86_64 Darwin: **19 groups, 5211 checks, 0 failures**, both optimized and ASan/UBSan; both importer runs and freestanding compilation passed. During the initial independent verification, the original saved PCCT/MADT binaries also imported under sanitizers with 16 CPUs/8 domains/one channel, using the explicitly synthetic test XNU map. The reproducible check above uses the retained derived fixtures. This is not a hardware test.

## Inputs and implementation

`src/CppcLab.hpp`, `Descriptors.cpp`, and `PccRead.cpp` are bounded, integer-only C++ with no allocation, floating point, OS calls, hardware accessors, or external-buffer transport binding. Compiler-generated memory-copy/stack-protector helpers can still be emitted. `host/` provides ordinary bounded regular-file input; tests call the same implementation.

```sh
/Volumes/Data/Projects/AmdCpu/tools/cppc-lab/build/cppc-lab --hex \
  /Volumes/Data/Projects/AmdCpu/tools/cppc-lab/fixtures/pcct.hex \
  /Volumes/Data/Projects/AmdCpu/tools/cppc-lab/fixtures/madt.hex \
  /Volumes/Data/Projects/AmdCpu/tools/cppc-lab/fixtures/static.cpc \
  /Volumes/Data/Projects/AmdCpu/tools/cppc-lab/fixtures/xnu.map
```

`--binary` instead accepts raw saved PCCT/MADT bytes. The CLI only imports; the simulated protocol is exercised by the tests. No physical address becomes a pointer.

See `FORMAT.txt` for the intentionally small static normalization grammar. It accepts integers and Generic Register resource buffers, **not AML**, methods, expressions or namespace references. A producer must establish static provenance independently; calling a file “static” cannot prove that provenance. Raw table signatures, lengths, checksums and supported layouts are checked again in C++.

Joins are explicit: canonical ACPI path + independently supplied ProcessorID/numeric UID → MADT APIC ID → caller-provided XNU number. All active identities must join one-to-one, regardless of order. Disabled placeholders with repeated unused APIC IDs are retained but cannot satisfy a join; duplicate ACPI identities or active APIC IDs fail.

Minimum/Maximum Performance must both be supported or both absent, including Integer-zero and null-resource absence forms. Within each PCC subspace, conflicting overlaps involving control/status-acknowledgement fields are rejected across all CPUs. Identical same-role/same-geometry aliases and read-only sharing remain permitted; equal offsets in different subspaces do not conflict. Regression tests cover these cases and valid/mismatched signature tags for every supported subspace.

## Simulated READ contract

One noncopyable `SimulatedReadChannel` owns one private simulated region, shared by all CPUs referencing that subspace. Calls must be serialized; this is not a concurrent transport implementation.

- Little-endian header: signature `0x50434300 | subspace` at 0, READ command at 4, status at 6; payload begins at 8. Initialization, submission and completion validate the same channel-specific signature. PCC GAS AccessSize means **subspace ordinal**, unlike doorbell GAS access width.
- Submission clears status, transfers modeled ownership, then calculates `(old & preserveMask) | writeMask`. It never writes payload controls. There is no WRITE command selector.
- Caller timeout is 1–60,000,000 microseconds; 4000-us nominal latency is not a deadline. Completion first observed **at** the deadline is late.
- Timeout quarantines the channel indefinitely while the platform may own it. No reinitialization/force-unlock is offered. Only observed completion drains it; late data is discarded. Errors without completion do not release ownership.
- Turnaround starts at observed completion, including errors/late completions. Nonzero MPAR uses conservative `ceil(60,000,000 / rate)` spacing, preventing rolling-minute burst loopholes. Zero means unlimited. Backwards time and deadline overflow are rejected without ringing.
- Notifications, reserved status bits or corrupted headers enter a terminal fault. Real interrupt acknowledgement, ownership acquisition, cache/barrier behavior, cancellation and sleep/resume recovery are unimplemented.
- Values distinguish unknown, unsupported, static integer and **simulated**. Unfilled/stale simulated bytes remain unknown even after completion. Only explicitly supplied fake-firmware bytes can become simulated values; none is live evidence.

## Fixture and limits

Five fixture files total **25,309 bytes**. PCCT/MADT retain captured layouts and CPU ordering but replace physical addresses/OEM metadata and recompute ACPI checksums. Static CPC resource bytes are retained; the XNU map is deliberately synthetic. `provenance.json` distinguishes original hashes from derived hashes: ACPI checksums detect corruption, **not authenticity or digital signatures**. No full firmware bundle is included.

The strict profile supports revision-3/23-entry CPC, HW_ALL PSD, revision-2 generic polling PCCT, selected MADT revision-3–5 records, aligned whole-byte 8/16/32/64-bit PCC fields (performance fields ≤32 bits), and FFH E7/E8 **metadata only**. Limits: 64 packages, 128 MADT CPU records, 8 nonaliasing subspaces, 64-KiB tables/regions, 128-KiB text. Other forms fail explicitly; these are implementation limits, not ACPI maxima. The simulator's large owned buffer belongs in host/caller-managed storage, not a future kernel stack.

`references.json` records protocol source symbols and hashes. Supplementary pinned Linux headers/mailbox source were read as inert references in ignored `build/reference/`; they are not build dependencies.
