# Publishing AMDZenPM telemetry as SMC keys (VirtualSMC plugin)

Status: research note for the AmdCpu project, written 2026-11-08. It covers how a VirtualSMC plugin works, how to build one with plain clang, which SMC keys monitoring apps read, and a recommended design for `AMDZenSMC.kext`.
Target host: Ryzen 7 5700X (family 19h model 21h), macOS 26.7.2 / Darwin 25.6.0, SMBIOS **MacPro7,1**. Lilu 1.7.3 and VirtualSMC 1.3.9 are loaded, SMCProcessor is not, and AMDZenPM 1.0.0 is loaded. [HOST]

No code was copied from any of the sources below. Lilu, VirtualSMC and SMCAMDProcessor are BSD-3-Clause, Stats is MIT, and none of them is GPL. The integer encoders in §5 are original descriptions.

Source tags:
- **[V:file:line]**: read from VirtualSMC source at `acidanthera/VirtualSMC` master `b5c62c2` (2026-09-27). This is the same commit as `deps/VirtualSMC-src/src`, which only holds `Sensors/SMCProcessor` and `Docs`. The SDK and core files were fetched from GitHub into `/tmp/smcref/vsmc` (not into the project).
- **[L:file:line]**: read from Lilu source at `acidanthera/Lilu` master `0249c3a`. GitHub has no `1.7.3` tag.
- **[VB]**: read from the shipped VirtualSMC 1.3.9 RELEASE binaries or dSYM in `deps/VirtualSMC` with `nm`/`dwarfdump`. Nothing was executed.
- **[D:model:line]**: real-Mac SMC dumps in `deps/VirtualSMC-src/src/Docs/SMCDumps/` (VERIFIED data).
- **[iS:line]**: the iStat Menus model database dump `Docs/iStat.txt`, which covers 105 models up to iMacPro1,1.
- **[S:file:line]**: read from `exelban/stats` at commit `1a4a9a6`.
- **[H:file:line]**: read from `CloverHackyColor/HWMonitorSMC2` at commit `80168fb`.
- **[A:file:line]**: read from SMCAMDProcessor in `deps/SMCAMDProcessor/src` at commit `029ef3a`.
- **[T]**: local compile-only test in `/tmp/smcbuild`. Nothing was loaded or run.
- **[HOST]**: read-only `sysctl`, `kmutil showloaded`, `ioreg`.
- **[R]**: RECALLED from memory and not verified in this session.

---

## 0. Answers in brief

1. **A plugin does not have to be a Lilu plugin.** A plain `IOService` with no `kern_start` glue works when it calls `VirtualSMCAPI::registerHandler()` and submits through `callPlatformFunction("VirtualSMCSubmitPlugin")`. The docs say so [V:Docs/SensorInfo.md:20], and SMCAMDProcessor ships that way [A:SMCAMDProcessor.cpp:9-101].
2. **Linked symbols.** A minimal plugin imports exactly 4 non-KPI symbols, all from VirtualSMC.kext: `VirtualSMCAPI::registerHandler`, `::addKey`, `::valueWithData`, and `VirtualSMCValue::update` (the vtable slot). It imports nothing from Lilu [T]. The SDK headers still `#include <Headers/kern_util.hpp>` from Lilu.
3. **`readAccess()` runs in a hostile context.** It may run inside the `kernel_trap` page-fault handler with a spinlock held [V:Docs/SensorInfo.md:16], [V:VirtualSMCSDK/kern_vsmcapi.hpp:23-28]. On the MMIO path it actually runs from the `_ioTrapHandler` trampoline that VirtualSMC splices into AppleSMC's code; that trampoline saves only the GPRs and flags, **not xmm/ymm** [V:VirtualSMC/kern_handler.S] (see Critic notes). It must not block, allocate, take an `IOLock`/command gate, use any SSE/FP instruction, or call `AMDZenPM_copySample()`, which is gated [AMDZenPM.cpp:1517-1520].
4. **No SMC key carries CPU frequency on any Mac.** VirtualSMC asks plugins not to invent keys for it [V:Docs/SensorInfo.md:61-67]. Expose frequency through AMDZenPM's IORegistry or user client instead.
5. **Recommended design.** A separate `AMDZenSMC.kext`:
   - It matches on the `AMDZenPM` service and depends on Lilu, VirtualSMC and `com.dinovn.AMDZenPM`.
   - A 500 ms timer (half of AMDZenPM's 1 s telemetry period, §6.1) on its own work loop calls `AMDZenPM_copySample()` in thread context, encodes each key with integer math, and stores the result in an `_Atomic` cell per key.
   - `readAccess()` only does an atomic load and writes 2 or 4 bytes.
   - It publishes about 20 keys. See §6.

---

## 1. VirtualSMC plugin model (VERIFIED)

### 1.1 SDK surface (`VirtualSMCSDK/`)

| Item | Fact | Source |
|---|---|---|
| `VirtualSMCAPI::ServiceName` | `"APP0001"`. This is the IORegistry name of any SMC device, including a real one. | [V:VirtualSMCSDK/kern_vsmcapi.hpp:20] |
| `VirtualSMCAPI::SubmitPlugin` | `"VirtualSMCSubmitPlugin"`, a platform-function name. The header comment says: "Virtual key functions may be invoked asynchronously from inside IOSimpleLock context, so they should be designed with the help of lock-free algorithms." | [V:kern_vsmcapi.hpp:23-28] |
| `PluginMax` | 16 plugins in total | [V:kern_vsmcapi.hpp:33] |
| `Version` | API/ABI version `1`. `loadPlugin` rejects any other value with `kIOReturnInvalid`. | [V:kern_vsmcapi.hpp:38], [V:VirtualSMC/kern_keystore.cpp:159-162] |
| `struct Plugin` | `{const char *product; size_t version; size_t apiver; KeyStorage data, dataHidden;}`. The struct must stay unchanged and never be freed after submission. Both vectors **must be sorted**. | [V:kern_vsmcapi.hpp:45-54] |
| `KeyStorage` | `evector<VirtualSMCKeyValue&, VirtualSMCKeyValue::deleter>`, a Lilu container. Its `push_back` uses `kern_os_realloc`. | [V:kern_vsmcapi.hpp:43], [L:Headers/kern_util.hpp:792-950] |
| `registerHandler(handler, ctx)` | `IOService::addMatchingNotification(gIOFirstPublishNotification, nameMatching("APP0001"), …)`. It returns an `IONotifier*`. | [V:VirtualSMC/kern_vsmcapi.cpp:12-30] |
| `addKey(key, storage, value)` | `push_back<4>` of `{key, value}`. On failure it **deletes** `value`. A null `value` is a no-op that returns false. | [V:kern_vsmcapi.cpp:45-58] |
| `valueWithData(data, size, type, thisValue, attr=SMC_KEY_ATTRIBUTE_READ, ser=None)` | Allocates a `VirtualSMCValue` when `thisValue` is null, then calls `init()`. `size` must be in `1..SMC_MAX_DATA_SIZE`. | [V:kern_vsmcapi.cpp:61-80], [V:kern_vsmcapi.hpp:112] |
| `valueWithSp/Fp/Flt/Uint8/Uint16/Sint16/Uint32/Flag` | Inline wrappers. The multi-byte integer wrappers byte-swap to **big-endian**. `valueWithFlt` stores the raw float bits in host order, which is little-endian. | [V:kern_vsmcapi.hpp:224-325] |
| `encodeSp/decodeSp/encodeFp/encodeIntSp/...` | Exported functions. `encodeSp(type, double)` takes a **double**. | [V:kern_vsmcapi.hpp:122-192] |
| `postInterrupt`, `getDeviceInfo` | Optional. | [V:kern_vsmcapi.hpp:76,86] |

### 1.2 `VirtualSMCValue` (the key value class)

- Protected members: `SMC_DATA data[SMC_MAX_DATA_SIZE]`, `SMC_DATA_SIZE size`, `SMC_KEY_TYPE type`, `SMC_KEY_ATTRIBUTES attr`, `SerializeLevel serializeLevel` [V:VirtualSMCSDK/kern_value.hpp:37-57].
- Virtual functions:
  - `readAccess()` is called before every read. The plugin overrides it to refresh `data` [kern_value.hpp:65].
  - `writeAccess()` [kern_value.hpp:75].
  - `update(const SMC_DATA*)`: memcpy `size` bytes [kern_value.hpp:120], [V:VirtualSMC/kern_value.cpp].
  - Virtual destructor.
  - `deleter` calls `delete` [kern_value.hpp:137-146].
- ABI check against the shipped 1.3.9: `sizeof(VirtualSMCValue) = 0x90` [VB: dwarfdump of `VirtualSMC.kext.dSYM`]. Compiling the master headers gives the same size (`static_assert` passed) [T]. Keep that `static_assert` in the plugin.
- VirtualSMC 1.3.9 exports these 27 symbols [VB: `nm -gU`]:
  - `VirtualSMCAPI::{registerHandler, addKey, valueWithData, encodeSp, decodeSp, encodeFp, decodeFp, encodeIntSp, decodeIntSp, encodeIntFp, decodeIntFp, postInterrupt, getDeviceInfo}`
  - `VirtualSMCValue::{init×2, update, get}`
  - `vtable for VirtualSMCValue`
  - the `VirtualSMC` class metadata
- The SDK signatures match the binary's mangled names.

### 1.3 Attributes and types

- `SMC_KEY_ATTRIBUTE_*` bits are PRIVATE_WRITE `0x01`, PRIVATE_READ `0x02`, ATOMIC `0x04`, CONST `0x08`, FUNCTION `0x10`, `0x20` unknown, WRITE `0x40`, READ `0x80` [V:VirtualSMCSDK/AppleSmc.h:325-332].
  - Use `SMC_KEY_ATTRIBUTE_READ` (the default). SMCProcessor deliberately makes its keys read-only [V:Sensors/SMCProcessor/SMCProcessor.cpp:296].
  - Real Macs show `0x81`/`0x85`/`0xC0` [D:MacPro7,1].
- Type codes come from `SMC_MAKE_KEY_TYPE`, for example `SmcKeyTypeSp78`, `SmcKeyTypeSp96`, `SmcKeyTypeSp3c`, `SmcKeyTypeFloat` (`"flt "`), `SmcKeyTypeFpe2`, `SmcKeyTypeUint8` (`"ui8 "`), `SmcKeyTypeUint16` [V:AppleSmc.h:247-291].
- `SMC_MAKE_IDENTIFIER('T','C','0','D')` packs the bytes so that the in-memory order is "TC0D" on a little-endian host [V:AppleSmc.h:19-31].
- **Sort order.** `VirtualSMCKeyValue::compare` compares `OSSwapInt32(key)`, which is plain ASCII lexicographic order of the 4 characters: digits < upper case < lower case [V:VirtualSMCSDK/kern_keyvalue.hpp:103-121]. Lookups use binary search [V:VirtualSMC/kern_keystore.cpp:674-696]. An unsorted vector makes keys silently unfindable. SMCProcessor calls `qsort()` after adding keys [V:SMCProcessor.cpp:325].

### 1.4 Submission and key-store merge

1. VirtualSMC publishes itself and the resource: `registerService(); publishResource(VirtualSMCAPI::SubmitPlugin);` [V:VirtualSMC/kern_vsmc.cpp:165-168].
2. The plugin's `registerHandler` callback fires on first publish of any service named `APP0001`. The callback calls `vsmc->callPlatformFunction(VirtualSMCAPI::SubmitPlugin, true, this, &plugin, nullptr, nullptr)`. A non-VirtualSMC SMC returns `kIOReturnUnsupported`, which the handler must tolerate [V:SMCProcessor.cpp:487-504], [A:SMCAMDProcessor.cpp:59-76].
3. `VirtualSMC::callPlatformFunction` **retains the plugin IOService** and calls `keystore->loadPlugin()` [V:kern_vsmc.cpp:729-737]. The plugin object is therefore never freed.
4. `loadPlugin` [V:kern_keystore.cpp:156-226]:
   - It checks `apiver`.
   - Plugin keys that already exist in VirtualSMC's **base** store (not in other plugins) become overrides. They are **erased from the plugin's own `data`/`dataHidden` vector** (VirtualSMC mutates the submitted vector), and the base entry's value pointer is swapped for the plugin's.
   - **Correction (critic):** the "attempt to override twice" `PANIC` is effectively unreachable. The CAS runs on the `backup` field of a *local copy* in `ovrData` (always null), not on the base store entry. Afterwards `ovrData[i].deinit()` runs `VirtualSMCKeyValue::deleter`, which **deletes the replaced value**. On the first override that is VirtualSMC's base value. On a second override of the same key by another plugin, it is the *first plugin's* `VirtualSMCValue`, which is a use-after-free if that plugin still touches it [V:kern_keystore.cpp:165-225], [V:VirtualSMCSDK/kern_keyvalue.hpp:73-93], [L:Headers/kern_util.hpp:894-941]. Never publish a key that exists in the base store.
   - The plugin pointer goes into the first free slot of 16.
5. Lookup order is base store, then plugins in slot order. The first match wins [V:kern_keystore.cpp:655-672].
   - Duplicate keys **between plugins** are not detected. The later plugin's copy is shadowed by name.
   - Index enumeration (`#KEY`, `getByIndex`) still lists both copies [V:kern_keystore.cpp:698-718], so Stats would show duplicate rows.
   - Stats discovers sensor keys **only by index enumeration** (`#KEY`, then read-by-index), then reads each one by name [S:SMC/smc.swift:321-346], [S:Modules/Sensors/readers.swift:64-104]. So publish sensor keys in `Plugin::data`, never `dataHidden` (hidden keys are not indexed).
6. The base key store on this setup has no CPU sensor keys. The Info.plist `Keystore` adds only `FNum, MSTc, MSTf, MSTm, BATP, LsNM` (+`MSTg` V1, `MSTe/MSTi/MSTj` V2) [V:VirtualSMC/Info.plist, decoded]. **Do not publish `MSTc`.** It would override a base key.

### 1.5 Read path and execution context of `readAccess()`

- The read path is `readValueByName`, then the attr READ/PRIVATE_READ check, then `currval->readAccess()`, then the protocol copies `value->get()` [V:kern_keystore.cpp:736-765]. The caller is either:
  - **PMIO** (gen-1 SMC): `SMCProtocolPMIO::loadValueInBuffer`, reached from AppleSMC's port I/O via `VirtualSMC::ioRead8/ioWrite8` [V:VirtualSMC/kern_pmio.cpp:34-48], [V:kern_vsmc.cpp:582-612].
  - **MMIO** (gen-2/3 SMC, typical for modern SMBIOS): `SMCProtocolMMIO::readValue`. This is reached from VirtualSMC's routed `_kernel_trap`. A page fault on the fake MMIO window makes the hook overwrite the instruction *after* AppleSMC's faulting access with a `Trampoline` to `_ioTrapHandler` [V:VirtualSMC/kern_prov.cpp:154,223-313]. When AppleSMC resumes, the trampoline calls `ioProcessResult`, then `handleWrite`, then `readValue`, then `readValueByName`, then `readAccess()` [V:kern_prov.cpp:342-372], [V:kern_mmio.cpp:71-140]. So `readAccess` runs in the middle of an AppleSMC function, on its stack, in whatever lock, preemption and interrupt state AppleSMC had at that instruction. `_ioTrapHandler` pushes only `rflags, rax, rdi, rsi, rdx, rcx, r8-r11`. Its own comment says "xmm/ymm registers are unused (kexts do not use it atm)" [V:VirtualSMC/kern_handler.S].
- The vendor's own contract: "neither `readAccess`, nor `writeAccess`, not even `update` are invoked in an interrupt context, yet they may or may not be called within a page fault handler (`kernel_trap`) with a spinlock acquired … not use mutexes (e.g. `IOLock`) but rely on atomics … and spinlocks" [V:Docs/SensorInfo.md:16].
- Consequences for AMDZenSMC:
  - No `IOLock`, no `IOCommandGate::runAction`, no `IOMalloc`, no `IOSleep`, no `thread_call_enter*`.
  - No call into AMDZenPM: `copySample` and `copyInfo` go through `runGated` [AMDZenPM.cpp:1512-1520], and the export is documented as "thread context only" [AMDZenShared.h, C API comment].
  - SMCProcessor takes an `IOSimpleLock` in `readAccess` [V:Sensors/SMCProcessor/KeyImplementations.cpp:11-50]. That is legal but unnecessary for us.
- Concurrency between readers is not otherwise guaranteed by VirtualSMC. Any design where `readAccess` only *loads* pre-encoded atomics is safe regardless.
- **Floating point.** `-mkernel` still emits SSE for `double`/`float` (`mulsd`, `cvttsd2si`). It adds only `-disable-red-zone -no-implicit-float` [T: `clang -mkernel -###`].
  - **Verified, stronger reason:** `_ioTrapHandler` is spliced in at an arbitrary instruction, not at a call site, and saves no xmm registers [V:kern_handler.S]. Any SSE use in `readAccess` can therefore clobber AppleSMC's live xmm values, not just caller-saved registers.
  - XNU also does not save FP/SIMD state on kernel entry. FP/SIMD is managed lazily through CR0.TS and the `#NM` handler `fpnoextflt`, which loads the *current thread's* saved state into the live registers [xnu-12377.121.6 osfmk/i386/fpu.c:1300-1352, fetched]. So kernel SSE in a user thread's context operates on that thread's register file. That user state is corrupted unless the code saves and restores it (inference from that source, not a test).
  - SMCProcessor and SMCAMDProcessor both use `double`/`float` in `readAccess` [V:KeyImplementations.cpp:32-48], [A:Keyimplementations.cpp:11-32].
  - The project already avoids FP (`ZenModel.hpp` comment). **AMDZenSMC should encode with integer math only and never call `VirtualSMCAPI::encodeSp(double)`.** See §5.

### 1.6 Unloading

- VirtualSMC keeps the `Plugin` pointer, its `VirtualSMCValue*` objects and a retain on the plugin service forever (§1.4). Plugins therefore must not unload.
- SMCProcessor's `kern_stop` returns `KERN_FAILURE` ("It is not safe to unload VirtualSMC plugins!") [V:SMCProcessor.cpp:518-521].
- Lilu's generic plugin glue does the same once started [L:Library/plugin_start.cpp:94-97].
- **Consequence:** once AMDZenSMC loads, `com.dinovn.AMDZenPM` can no longer be unloaded either, because a dependent kext stays loaded. That affects AMDZenPM development reloads (we never load or unload here; this is a process note).

### 1.7 How the reference plugins match and start

| Plugin | Personality | `kern_start` | OSBundleLibraries |
|---|---|---|---|
| SMCProcessor (Intel) | `IOProviderClass=IOACPIPlatformDevice`, `IONameMatch=processor`, `IOPropertyMatch processor-index=0`, `IOResourceMatch=ACPI` | Custom `ADDPR(kern_start)`. It only reads boot args (`-vsmcdbg`/`-scpudbg`) and does **not** call `lilu.requestAccess`/`shouldLoad` [V:SMCProcessor.cpp:510-516] | `as.vit9696.Lilu 1.2.0`, `as.vit9696.VirtualSMC 1.0.0`, `IOACPIFamily`, KPIs 11.0.0. `OSBundleRequired=Root` [V:Sensors/SMCProcessor/Info.plist] |
| SMCBatteryManager | `IOProviderClass=IOResources`, `IOResourceMatch=IOKit` | Lilu-style | Lilu 1.2.0, VirtualSMC 1.0.0 [V:Sensors/SMCBatteryManager/Info.plist] |
| SMCSuperIO | `IOProviderClass=IOPCIDevice`, `IOResourceMatch=ACPI` | Lilu-style | Lilu 1.2.0, VirtualSMC 1.0.0 |
| **SMCAMDProcessor** | `IOProviderClass=AMDRyzenCPUPowerManagement`, the class of its companion PM kext | None, so Xcode's default empty kmod start applies. Not a Lilu plugin [A:SMCAMDProcessor.xcodeproj: no MODULE_START for that target] | Lilu 1.3.9, VirtualSMC 1.0.0, `wtf.spinach.AMDRyzenCPUPowerManagement 0.6`, IOPCIFamily, Libm, KPIs 12.0.0 [A:SMCAMDProcessor/Info.plist] |

- On AMD, SMCProcessor refuses to start: `BaseDeviceInfo.cpuGeneration` is Unknown, so `start()` returns false [V:SMCProcessor.cpp:352-356]. It cannot conflict with us even if installed.
- **Lilu `PluginConfiguration` is not needed.** It exists only to drive Lilu's `plugin_start.cpp` glue: `lilu.shouldLoad(...)` with disable, debug and beta boot args and a kernel range, plus `pluginStart()` for patcher callbacks [L:Headers/plugin_start.hpp:16-29], [L:Library/plugin_start.cpp:55-92]. A sensor-only plugin that patches nothing does not need it.
- `-vsmcbeta` is VirtualSMC's **own** beta argument for its Lilu config, whose `maxKernel` is `KernelVersion::Tahoe` (macOS 26) [V:VirtualSMC/kern_start.cpp:10-37]. The host is Darwin 25, so it is not needed today. It will be needed for VirtualSMC on Darwin 26+, and that is independent of our plugin.
- VirtualSMC 1.3.9's Info.plist has `OSBundleCompatibleVersion=1.0` and depends on Lilu 1.2.0. Lilu 1.7.3 has `OSBundleCompatibleVersion=1.2.0` [VB: plutil].

---

## 2. SMCAMDProcessor (prior art, read-only summary)

- Structure:
  - A plain `IOService` child of `AMDRyzenCPUPowerManagement`.
  - `start()` casts the provider and calls `setupKeysVsmc()`, which calls `registerHandler()` and adds the keys [A:SMCAMDProcessor.cpp:9-57,86-101].
  - Its `vsmcPlugin` uses `xStringify(PRODUCT_NAME)` and `parseModuleVersion(xStringify(MODULE_VERSION))` [A:SMCAMDProcessor.hpp:30-34]. The target does not define `PRODUCT_NAME` or `MODULE_VERSION` as preprocessor macros, so those become the literal strings `"PRODUCT_NAME"` and `"MODULE_VERSION"` [A:project.pbxproj:600-650]. This is harmless.
  - `readAccess()` reads the provider's public fields directly, with no lock and in double precision [A:Keyimplementations.cpp:11-32].
- Keys actually published [A:SMCAMDProcessor.cpp:18-29]:

| Key | Type | Value |
|---|---|---|
| TC0E, TC0F, TC0P, TC0T, TC0p | sp78 | package temperature (Tctl), absolute |
| TC0J | sp78 | constant 0 (no value class) |
| PCPR, PSTR | sp96 | package power (`uniPackageEnergy`, despite the name) |

- Commented out: TC0D, TC0H, TC0G, PCPT, PCTR, PC0C, PC0R, PCAM, PCPC, PC0G, PCGC, PCGM, PCPG, and per-core TCxC/TCxc. The note says "AMD cpu dont have temperature MSR for each core" [A:SMCAMDProcessor.cpp:17-48].
- Problems not to repeat:
  - It does not `qsort` its storage, and **the keys are not added in sorted order** (correction by critic). The order is TC0E, TC0F, TC0J, TC0P, TC0T, TC0p, PCPR, PSTR [A:SMCAMDProcessor.cpp:18-29]. With VirtualSMC's binary search, PCPR and PSTR are **not findable by name**. They still appear in index enumeration, so apps list them but name reads fail (simulated with the `kern_keystore.cpp:674-696` algorithm).
  - It calls `registerHandler()` *before* `addKey()` [A:SMCAMDProcessor.cpp:11-29]. If VirtualSMC is already published, IOKit fires the first-publish handler for the existing service during `addMatchingNotification`, so an empty or partly built plugin can be submitted (inference from IOKit notification semantics [R]). AMDZenSMC must build and sort all keys before registering.
  - `PSTR` (system total) is filled with CPU package power.
  - `TC0T`/`TC0E`/`TC0F` hold absolute temperatures where real Macs keep negative Tjmax-relative values (§3.2).
  - Its `free()` does not call `super::free()`.
  - It uses FP in `readAccess`.
- AMDZenPM already treats `SMCAMDProcessor` as a conflicting class [AMDZenPM.cpp:54-59]. AMDZenSMC must check for it too, because both would publish TC0P/TC0p/PCPR (§1.4 point 5).

---

## 3. SMC key catalogue for a CPU

### 3.1 Encodings

| Type | Size | Encoding | Range / LSB |
|---|---|---|---|
| `sp78` | 2 | signed fixed point, **big-endian**, 8 fraction bits. Apple uses **two's complement**: real dumps show `TC0T=FFF5` = -0.043 °C and `TCXR=C180` = -62.5 °C [D:MacPro7,1]. VirtualSMC's `encodeSp` uses **sign-magnitude** for negatives [V:VirtualSMC/kern_vsmcapi.cpp:164-169]. That makes a difference only below 0, so clamp to ≥ 0. | ±127.996, 1/256 |
| `sp96` | 2 | signed, BE, 6 fraction bits | ±511.98 W, 1/64 |
| `spa5` | 2 | signed, BE, 5 fraction bits (iMacPro1,1 PCPT) | ±1023.97, 1/32 |
| `sp3c` | 2 | signed, BE, 12 fraction bits | ±7.9998 V, 1/4096 |
| `sp4b`/`sp5a` | 2 | signed, BE, 11 or 10 fraction bits | |
| `fp2e`/`fpe2` | 2 | unsigned, BE, 14/2 fraction bits (`fpe2` = fan RPM×4) | |
| `flt ` | 4 | IEEE-754 single, **little-endian** byte order. `PC0R` bytes `0A A0 0A 42` = 34.66 W [D:MacPro7,1:692]. VirtualSMC's `encodeFlt` stores raw bits [V:kern_vsmcapi.hpp:213-216]. | |
| `ui8 `/`ui16` | 1/2 | unsigned, BE | |

- `0x8100` (sp78 = -127.0) is the "sensor not present" pattern on real Macs, for example `TC1F` [D:MacPro7,1] and `TC8c`/`TC9c` [D:iMacPro1,1]. This is inferred from the dumps.
- How the apps decode:
  - Stats decodes by the key's reported type [S:SMC/smc.swift:224-272]. It decodes sp78/sp96 **without sign** [S:smc.swift:246-254], so a negative value shows as roughly 200 °C. It returns `nil` (key hidden) when all data bytes are zero [S:smc.swift:220].
  - HWMonitorSMC2 also uses the actual type: `getType(key) ?? fallback` [H:HWSensorScanner.swift:897].

### 3.2 What real Macs expose (VERIFIED from dumps)

**MacPro7,1** (the host SMBIOS) [D:MacPro7,1.txt]:

| Key | Type | Value in dump |
|---|---|---|
| TC0P, TC0p | sp78 | `20A0` = 32.6 °C (proximity) |
| TC1P..TC3P, TC1p..TC3p | sp78 | proximity sensors |
| TCXc | sp78 | `24B8` = 36.7 °C (hottest core/PECI) |
| TCXR, TCXr | sp78 | `C180` = -62.5, `C3B8` = -60.3 (relative to Tjmax) |
| TC0E, TC0F | sp78 | `C22F` = -61.8 (**negative**: relative to Tjmax) |
| TC0T | sp78 | small negative |
| TC0J | sp78 | 0 |
| TC0L | ui16 | 0x10 |
| TCS0..TCS3 | sp78 | |
| PC0R | flt | 34.66 W (CPU high side) |
| PC0H | flt | 41.99 |
| PC0L | flt | 36.73 |
| PC0I | flt | 6.23 |
| PC0P | spc3 | |
| PSTR | flt | 128.6 W (system total) |
| VC0C | flt | 1.793 V (Xeon VCCIN) |
| VC0R | flt | |
| IC0R | flt | 2.87 A |
| IC0I | flt | 6.23 A |

MacPro7,1 has **no** TC0D, no per-core TCxC/TCxc, and no PCPT/PCPC/PCPR.

Other desktop Macs [D:iMac19,1], [D:iMac18,3], [D:iMacPro1,1]:
- TC0c..TCnc (lowercase `c`, absolute per-core) and TCXc.
- TCGc (iGPU) and TCSc (System Agent).
- PCPC/PCPT/PSTR as sp96 (iMac19,1) or spa5 (iMacPro1,1).
- PCPR/PCTR/PC0C as flt.
- TC0E/TC0F again negative.

Key types across all 42 dump files (41 Macs plus VMware8,5; corrected from "47" by critic) (count of models with that type):

| Key | Types seen |
|---|---|
| PCPT | sp78 14, sp87 9, sp96 9, spa5 2 |
| PCPC | sp78 14, sp87 9, sp96 9, spa5 1 |
| PC0C | sp78 11, sp96 7, flt 6, fp88 3, spa5 1 |
| VC0C | flt 8, sp2d 7, sp5a 5, sp3c 4, sp1e 3, fp2e 2, fp3d 1 |
| IC0C | sp78 11, flt 4, … |
| TC0D | only 8 pre-2013 laptops (MacBook6,1/7,1, MacBookAir5,2, MacBookPro5,1/5,3/5,5/7,1/8,1) and VMware8,5 |
| TCAD, MPkC, MC0C, FRC0 | **in no dump** |

Because types vary this much, apps must decode by the reported type, and they do (§3.1).

### 3.3 Catalogue

"Stats" names come from [S:Modules/Sensors/values.swift]. "HWM2" means HWMonitorSMC2 [H:SMC_Keys.swift:25-37], [H:HWSensorScanner.swift:236-392]. "iStat" means the iStat database [iS]. "VSMC" means what VirtualSMC SMCProcessor publishes on Intel [V:SMCProcessor.cpp:268-324].

| Key | Meaning | Type to publish | Units / AMD source | Read by |
|---|---|---|---|---|
| **TC0D** | CPU die diode | sp78 | °C, Tctl (= Tdie on 19h) | **Stats CPU module 1st choice**: TC0D, then TC0E, TC0F, TC0P, TC0H, each accepted if < 110 [S:Modules/CPU/readers.swift:281-300]. Stats Sensors "CPU diode" [S:values.swift:378]. HWM2 "Diode 0" via `TC%@D` [H:SMC_Keys.swift:35]. iStat default temperature key on 46 of 105 profiles [iS: `temperature: TC0D`]. Not in VSMC. |
| TC0E | Apple: Tjmax-relative "virtual" (negative). VSMC/SMCAMDProcessor: absolute | sp78 | °C | Stats "CPU diode virtual" [S:values.swift:379] and CPU-module fallback. VSMC (absolute). Optional, see §6. |
| TC0F | Same as TC0E, "filtered" | sp78 | °C | Stats [S:values.swift:380]. VSMC. Optional. |
| TC0H | heatsink | sp78 | no AMD sensor | Stats [S:values.swift:381], VSMC. **Do not publish.** |
| **TC0P** | CPU proximity (board sensor) | sp78 | °C. No board sensor, so mirror Tctl as VSMC does | Stats "CPU proximity" [S:values.swift:382]. HWM2 "Proximity" [H:HWSensorScanner.swift:251]. iStat (default on MacBookPro14,3) [iS:4]. VSMC. |
| **TC0p** | CPU proximity (newer naming) | sp78 | °C, mirror Tctl | iStat default on iMacPro1,1 and many iMacs [iS:2994-3998]. Present on MacPro7,1. VSMC. Stats shows it under the raw name (Stats keys are case-sensitive). |
| TCAD | "CPU package" | sp78 | | Stats [S:values.swift:383]. iStat on 2 profiles. **No real dump.** Skip. |
| **TCXc** | hottest core / PECI | sp78 | °C, max valid Tccd, else Tctl | iStat default on MacPro6,1 [iS:2557]. Present on MacPro7,1 and iMacs. Stats shows it raw. Not in VSMC. |
| TCXC | Same, laptop spelling | sp78 | | iStat default on 20 profiles (laptops). Skip on a desktop SMBIOS. |
| **TCxc** (x=0..n-1) | per-core die, desktop spelling | sp78 | °C. On Zen3 there is no per-core sensor, so use Tccd of the core's CCD | Stats "CPU core %" for `TC%c` with % = 0..9 only [S:values.swift:390], [S:Modules/Sensors/readers.swift:90-103]. HWM2 picks lowercase when `TC0c` exists, index in hex then `G..` [H:HWSensorScanner.swift:12-23,347-358]. iStat (iMacPro1,1 TC0c..TC7c). VSMC publishes both cases. |
| TCxC | per-core, laptop/older spelling | sp78 | same | Stats "CPU Core %" [S:values.swift:391]. HWM2 fallback. VSMC. **Publish only one case.** Publishing both doubles the Stats rows. |
| **PCPT** | CPU package total | sp96 (VSMC) | W, RAPL package power | Stats "CPU Package total" [S:values.swift:649]. HWM2 "Package Total" [H:HWSensorScanner.swift:262]. iStat "CPU Package Total" [iS:53]. VSMC. |
| **PCPC** | CPU package, cores | sp96 | W, sum of per-core RAPL power | Stats "CPU Package" [S:values.swift:647]. HWM2 "Package Core" [H:HWSensorScanner.swift:274]. iStat [iS:51]. VSMC (PP0). |
| **PC0C** | CPU core power | sp96 | W, same as PCPC | Stats "CPU Core" [S:values.swift:641]. VSMC. |
| **PCPR** | CPU package total (SMC) | sp96 (VSMC) or flt (iMacs) | W, package | Stats [S:values.swift:650]. iStat "CPU" (iMacPro1,1) [iS:4000]. VSMC. |
| PCTR | CPU total | sp96 | W, package | Stats "CPU Total" [S:values.swift:648]. VSMC. Optional (third copy of package power). |
| PCAM | core power (IMON) | flt | W, cores | Stats [S:values.swift:646]. VSMC. Optional. |
| PC0R | CPU high side (12 V input) | flt on MacPro7,1 | W. No AMD equivalent | Stats [S:values.swift:651]. iStat. VSMC maps it to cores. **Skip by default.** |
| PSTR | **system** total | flt (MacPro7,1) | W | Stats "System Total" [S:values.swift:666]. SMCAMDProcessor misuses it. **Do not publish.** |
| PC0G/PCPG/PCGC/PCGM | iGPU power | | none on 5700X | Skip. |
| PC3C/PCEC | DRAM/eDRAM power | | AMD client RAPL has no DRAM domain | Skip. |
| **VC0C** | CPU core voltage | sp3c (VSMC); flt on MacPro7,1 | V, SVI2 Vcore, else max per-core requested VID | Stats `VC%C`, shown as "CPU Core 1" [S:values.swift:614]. HWM2 "Voltage" [H:HWSensorScanner.swift:296]. iStat "CPU Core" [iS:4020,2591]. VSMC. |
| VC1C | other CPU rail (MacBookAir "1.05 S0") | | | Stats would label it "CPU Core 2". Skip. |
| VCAC | CPU IA (core) rail | flt | V | Stats "CPU IA" [S:values.swift:612]. Laptops. Optional alias of Vcore. |
| **VCSC** | System Agent rail | flt | V, SVI2 VSoC (closest analogue: the IOD/SoC rail) | Stats "CPU System Agent" [S:values.swift:613]. iStat. Optional, only if SVI2. |
| **IC0C** | CPU core current | flt | A, SVI2 Icore estimate | iStat "CPU Core" (iMacPro1,1, MacPro6,1) [iS:4021,2558]. Stats shows it raw. Optional, only if SVI2. |
| IC0R | CPU high-side current | flt | none | Stats "CPU High side" [S:values.swift:632]. Skip. |
| TC0J/TC0T/TC0L/TCXR/TCXr/TC0G | Apple-internal, Tjmax-relative | | | Do not publish (Stats would show about 200 °C). |
| MPkC, MC%C, FRC% | FakeSMC/IntelCPUMonitor **fake** multiplier and frequency keys | fp4c/freq | | HWM2 only [H:SMC_Keys.swift:31-37]. **No real Mac has them.** VirtualSMC asks plugins not to add such keys [V:Docs/SensorInfo.md:61-67]. Skip (at most behind an opt-in property). |

Frequency:
- No real Mac SMC key reports CPU frequency (§3.2 tally).
- Stats gets Apple-Silicon frequencies from IOReport [S:Modules/CPU/readers.swift:309-540]. On Intel, apps use Intel Power Gadget's framework (HWM2 `AppSd.ipg` [H:HWSensorScanner.swift:244-249]), not SMC.
- So for AMD, frequency belongs in AMDZenPM's registry properties, user client and `zenctl`.

Macs Fan Control and iStat Menus are closed source. They are believed to show TC0P/TC0p/TCXc/TC0D and PCPT/PCPC when the key exists and names them from their own databases [R]. Fan *control* needs SuperIO fan keys from SMCSuperIO, which is out of scope here.

---

## 4. Building the plugin with plain clang (VERIFIED by compile test)

- **Headers.**
  - The local `deps/Lilu/Lilu-1.7.3-RELEASE` and `deps/VirtualSMC/VirtualSMC-1.3.9-RELEASE` bundles **do not contain** `Contents/Resources/Headers` or `VirtualSMCSDK` [HOST: path absent].
  - Vendor them from GitHub at pinned commits into the project, for example `third_party/lilu/Headers/` and `third_party/vsmc/VirtualSMCSDK/`. They are BSD-3-Clause, so keep the license files.
  - Lilu files needed: `kern_util.hpp`, `kern_config.hpp`, `kern_compat.hpp`. `kern_util.hpp` includes only those plus XNU headers [L:Headers/kern_util.hpp:11-21].
  - VirtualSMC files needed: `kern_vsmcapi.hpp`, `kern_smcinfo.hpp`, `kern_keyvalue.hpp`, `kern_value.hpp`, `AppleSmcBridge.hpp`, `AppleSmc.h`.
  - Include with `-isystem <dir>` so their `-Wsign-conversion`/`-Wshadow` warnings do not pollute `-Wall` [T].
- **Defines.**
  - `-DPRODUCT_NAME=AMDZenSMC -DMODULE_VERSION=1.0.0` are needed only if `ADDPR()`, `SYSLOG`/`DBGLOG` or `kextVersion` are used [L:kern_util.hpp:23-37,77-89].
  - `LILU_KEXTPATCH_SUPPORT` and `LILU_COMPRESSION_SUPPORT` are already `#define`d in `kern_config.hpp` [L:Headers/kern_config.hpp:14,19] and do not matter here.
  - `LILU_CUSTOM_KMOD_INIT` and `LILU_CUSTOM_IOKIT_INIT` affect only `plugin_start.hpp/.cpp` and `kern_version.hpp`'s `kextVersion` [L:Headers/plugin_start.hpp:31-51], [L:Headers/kern_version.hpp:70-83]. **Do not compile `plugin_start.cpp` and do not include `plugin_start.hpp`.**
- **kmod glue.** Reuse the project's `kmod_info.c` with `_start/_stop`, as AMDZenPM does. Do not use Lilu's `ADDPR(kern_start)`.
  - Make the stop path refuse unload. The plain kmod stop is `_antimain = 0`, and a dependent plugin is never unloaded while VirtualSMC holds its retain (§1.6). Returning `KERN_FAILURE` from a custom stop routine is the explicit form.
- **Flags.** The project's `COMMON`/`CXXFLAGS`/`LDFLAGS` work unchanged: `-mkernel -fapple-kext -fno-rtti -fno-exceptions -nostdinc`, linked with `-lkmodc++ -lkmod -lcc_kext`.
  - A test TU defines a `VirtualSMCValue` subclass with an atomic-load `readAccess`, a static `VirtualSMCAPI::Plugin`, `valueWithData`, `addKey` and `registerHandler`, plus `static_assert(sizeof(VirtualSMCValue)==0x90)`. It compiled cleanly [T].
  - Its undefined symbols were only `VirtualSMCAPI::valueWithData`, `VirtualSMCAPI::registerHandler`, `VirtualSMCAPI::addKey`, `VirtualSMCValue::update`, `operator new` and `operator delete`. **No Lilu symbols** [T: `nm -u`].
  - For comparison, SMCProcessor imports `_lilu_os_log` (SYSLOG), `BaseDeviceInfo::get`, `CPUInfo::*` and a C++-mangled `qsort` from Lilu and the kernel [VB: `nm -u SMCProcessor`].
- **Avoid Lilu symbols.**
  - Use `IOLog` rather than `SYSLOG`.
  - Insert keys in ASCII order (§1.3), or call `qsort` from libkern.
  - Never call `evector::push_back`/`deinit` yourself (they pull in `kern_os_realloc/free`). Let `addKey` do the insertion inside VirtualSMC.
- **Link check.** `tools/linkcheck.sh` resolves imports only against `/System` KCs. Extend it so that a list of extra binaries also counts: VirtualSMC 1.3.9 (`nm -gU` shows the 27 exports in §1.2) and the built AMDZenPM, which exports `_AMDZenPM_copySample`/`_AMDZenPM_copyInfo` with default visibility [AMDZenPM.cpp:1671-1681].
- **Info.plist (recommended).**

| Key | Value |
|---|---|
| `CFBundleIdentifier` | `com.dinovn.AMDZenSMC` |
| `IOKitPersonalities.AMDZenSMC` | `IOClass=AMDZenSMC`, `IOProviderClass=AMDZenPM`, `IOMatchCategory=AMDZenSMC`. AMDZenPM calls `registerService()` [AMDZenPM.cpp:466]. |
| `OSBundleLibraries` | `as.vit9696.VirtualSMC` = `1.0.0` (compatible version 1.0) |
| | `as.vit9696.Lilu` = `1.2.0`. Not strictly linked, but it documents load order and matches SMCProcessor. |
| | `com.dinovn.AMDZenPM` = `1.0.0` (its `OSBundleCompatibleVersion` is 1.0.0) |
| | `com.apple.kpi.iokit`, `com.apple.kpi.libkern`, `com.apple.kpi.mach` = `20.0.0`, matching AMDZenPM |
| `OSBundleRequired` | `Root`, like VirtualSMC and AMDZenPM |

- **Kext placement [R].**
  - With OpenCore injection, add AMDZenSMC **after** Lilu, VirtualSMC and AMDZenPM in `Kernel→Add`.
  - A kext in the Boot KC cannot depend on one in the Auxiliary KC. AMDZenSMC must live in the same collection as AMDZenPM, or a later one.

---

## 5. Integer-only encoders (original design notes)

All inputs are AMDZenPM's integer units: m°C, mW, mV, mA [AMDZenShared.h header comment].

- **spXY** (X integer bits including sign, F = hex digit Y fraction bits, X+F = 15):
  1. `raw = (milli * (1 << F) + 500) / 1000` for milli ≥ 0. Use a symmetric rounding rule for negatives.
  2. Clamp to `[-32768, 32767]`.
  3. Store `(uint16_t)raw` big-endian.
  - F = 8 for sp78, 6 for sp96, 12 for sp3c. Example: 45 250 m°C gives `0x2D40`.
- **flt** (value = milli/1000):
  1. If milli == 0, the result is `0x00000000`. Stats hides all-zero keys, which is acceptable for currents and power at 0.
  2. Otherwise let `a = |milli|` (64-bit). Find the smallest `s ≥ 0` with `(a << s) ≥ 1000 · 2^23`. Because `a < 2^31`, `s ≤ 33` and nothing overflows 64 bits.
  3. Compute `q = ((a << s) + 500) / 1000`. If `q == 2^24`, set `q >>= 1` and `s -= 1`.
  4. `bits = sign<<31 | (127 + 23 − s)<<23 | (q & 0x7FFFFF)`.
  5. Store `bits` in host (little-endian) order.
  - Check: 1000 mW → s = 23, q = 2^23, exponent 127, giving 1.0. 34 656 mW → s = 18, giving 34.656.
- Both run in the timer thread. `readAccess` only copies the finished 16- or 32-bit pattern.

---

## 6. Recommendation

### 6.1 Architecture

```
AMDZenPM.kext (existing)          AMDZenSMC.kext (new, never unloads)
  telemetry timer (1 s)  ──seq──►  IOTimerEventSource on own IOWorkLoop
  runGated copySample()  ◄──call── (thread ctx) AMDZenPM_copySample(buf)
                                    encode -> _Atomic uint32_t cell[k]
VirtualSMC keystore ──readAccess()─► atomic_load(cell[k]) -> data[0..3]
```

1. **start(provider = AMDZenPM)**:
   1. `IOMalloc` one `AMDZenSample` buffer (about 5 KB, so not on the stack).
   2. Call `AMDZenPM_copyInfo()` and the first `AMDZenPM_copySample()`.
   3. Decide the **fixed** key set from `features`, `ccdTempMask`, `numCores` and the SVI2 flags. The key set can never change after submission [V:kern_vsmcapi.hpp:46].
   4. Create the values and add them in ASCII order.
   5. Bail out without registering if an `SMCAMDProcessor` or `SMCProcessor` service already published any of the same keys. A simple check is `IOService::copyMatchingService(serviceMatching("SMCAMDProcessor"))`. This must come **before** step 6, because the handler can fire synchronously inside `registerHandler()`.
   6. Arm the timer, then call `VirtualSMCAPI::registerHandler()` **last**, once the `Plugin` is fully built and sorted. Keep the returned `IONotifier`. Note that VirtualSMC retains the plugin service *before* `loadPlugin` runs, even if the submission then fails [V:kern_vsmc.cpp:729-737].
2. **Timer callback** (every `max(250, intervalUs/2000)` ms, default 500 ms; this is thread context):
   1. `AMDZenPM_copySample(buf)`.
   2. If `buf->seq` changed, encode every key and do `atomic_store_explicit(relaxed)` per cell.
   3. If AMDZenPM's `instance()` is gone, stop the timer and keep the last values.
   - Copying the full sample is cheaper to build than a new ABI. Option B is to add a small seqlock-protected `AMDZenSmcSnapshot` export to AMDZenPM. That is only worthwhile if the 5 KB gated copy ever shows up in profiles.
3. **readAccess()**: `uint32_t v = atomic_load(cell)`, write 2 bytes (BE already) or 4 bytes into `data`, return `SmcSuccess`.
   - If the data is stale (no new seq for more than 10 s), keep returning the last value. `SmcNotReadable` would make apps drop the sensor permanently [R].
4. **stop()**: cancel the timer only. Keep every `VirtualSMCValue` and the `Plugin` alive (§1.6).
5. **Lock ordering.** AMDZenPM never calls into AMDZenSMC, so the gated call from our work loop cannot deadlock.

### 6.2 Default key set for this host (desktop SMBIOS MacPro7,1, 8 cores, 1 CCD)

Insertion order is sorted ASCII. That is 20 keys, or 22 with the SVI2 extras.

| Key | Type | Source field (AMDZenSample / AMDZenInfo) | Condition |
|---|---|---|---|
| IC0C | flt | `icore_mA` | `AMDZEN_F_SVI2` and `icore_mA` ≠ 0 |
| PC0C | sp96 | `coresPower_mW` | `AMDZEN_F_CORE_ENERGY` |
| PCPC | sp96 | `coresPower_mW` | `AMDZEN_F_CORE_ENERGY` |
| PCPR | sp96 | `pkgPower_mW` | `AMDZEN_F_RAPL` |
| PCPT | sp96 | `pkgPower_mW` | `AMDZEN_F_RAPL` |
| TC0D | sp78 | `tctl_mC` (19h: Tctl = Tdie; use `tdie_mC` where an offset exists) | `AMDZEN_F_TCTL` |
| TC0P | sp78 | `tctl_mC` | `AMDZEN_F_TCTL` |
| TC0c..TC7c | sp78 | `tccd_mC[ccd(core)]`, else `tctl_mC` | `AMDZEN_F_TCTL`, with `numCores` ≤ 36 |
| TC0p | sp78 | `tctl_mC` | `AMDZEN_F_TCTL` |
| TCXc | sp78 | max valid `tccd_mC`, else `tctl_mC` | `AMDZEN_F_TCTL` |
| VC0C | sp3c | `vcore_mV`, else max `core[].vid_mV` (requested) | SVI2 or `AMDZEN_F_HWPSTATUS` |
| VCSC | flt | `vsoc_mV` | `AMDZEN_F_SVI2` |

- **Opt-in** through Info.plist personality properties:
  - `TC0E`/`TC0F` (absolute, SMCProcessor-compatible).
  - `PCTR`/`PCAM`.
  - Upper-case `TCxC`.
  - Never `PSTR`, `PC0R`, `MSTc`, or fake frequency keys.
- **Index characters.** Use `0-9A-Z` [V:SMCProcessor.hpp:55-56], so at most 36 per-core keys. Stats only expands `0..9` [S:readers.swift:92]. A 16-core part therefore shows cores 10..15 under raw names: `TCAc` would collide with Stats' "CPU A core die" label for `TCAC` only if upper case is used.
- **ABI gap.** `AMDZenInfo` has `coreOfCpu[]` but no core→CCD map. Add `uint8_t ccdOfCore[AMDZEN_MAX_CORES]` and bump `AMDZEN_ABI_VERSION`, or derive the CCD from `apicId >> ccdShift`. On the 5700X every core maps to CCD 0.

### 6.3 Update cadence

- AMDZenPM telemetry defaults to 1000 ms. The `TelemetryPeriodMs` range is 100..10000 [AMDZenPM/Info.plist], [AMDZenPM.cpp:318]. Power is averaged over that interval.
- Poll at half the period and re-encode only when `seq` changes.
- SMCProcessor uses a 500 ms timer [V:SMCProcessor.hpp:184]. Stats and iStat poll at about 1–3 s [R]. A 1 s source is enough, and faster SMC reads just return the cached value.

---

## 7. Open risks and uncertainties

1. **FP safety (now verified, see §1.5).** `_ioTrapHandler` does not save xmm registers, so SSE in `readAccess` is unsafe [V:kern_handler.S]. The integer-only design removes the risk. Add a build check that `readAccess` contains no xmm instructions, for example `otool -tV` piped through `grep xmm` on that symbol.
2. **Unloadability.** Once AMDZenSMC is loaded, AMDZenPM cannot be unloaded or reloaded until reboot (§1.6). Development loops must keep AMDZenSMC out, for example with a boot-arg such as `-amdzensmcoff` that makes `probe()` fail.
3. **Duplicate keys** with SMCAMDProcessor or any future plugin are not detected by VirtualSMC between plugins. They shadow by name and duplicate in index enumeration (§1.4). A double override of a *base* key does **not** panic as the source comment suggests. It frees the earlier plugin's value (§1.4, critic correction).
4. **Semantics mismatch.** Real Macs keep TC0E/TC0F/TCXR as negative Tjmax-relative values. SMCProcessor and SMCAMDProcessor publish absolute values. We avoid both by default and lead with TC0D/TC0P/TCXc.
5. **Per-core temperatures are synthetic** on Zen2/3 (CCD granularity). Users may read 8 identical "core" values as 8 sensors. The alternative is to publish TCXc only.
6. **MacPro7,1 SMBIOS.** Apple's `ACPI_SMC_PlatformPlugin` is loaded [HOST]. Whether it reacts to newly present CPU keys (thermal or fan policy) is unknown [R]. Publishing only read-only sensor keys keeps writes impossible.
7. **Header versions.** The GitHub tags stop at VirtualSMC 1.3.8 and Lilu 1.7.2. The local "1.3.9" is a Dortania build-repo (nightly) build: its dSYM `DW_AT_decl_file` path is `/Users/runner/.../Dortania/_work/build-repo/...`, and the bundled Changelog stops at v1.3.8. The headers used are master (VirtualSMC `b5c62c2` = the local `VirtualSMC-src` commit). They are ABI-checked only through `sizeof(VirtualSMCValue)` and the mangled export names of the shipped 1.3.9 binary (§1.2). Pin the vendored copy and keep the `static_assert`.
8. **Gen-2 MMIO path.** `readAccess` runs inside VirtualSMC's `kernel_trap` hook [V:kern_prov.cpp:223-313]. Any fault or long operation there would be catastrophic, so the atomic-load-only rule is mandatory.
9. **`com.overlorddev.RyzenHV 0.7.8`** is also loaded on the host [HOST]. It does not link VirtualSMC (its dependencies are only Apple KPIs), so it is no SMC conflict. Its interaction with AMDZenPM is out of scope here.

## 8. Source index

- VirtualSMC master `b5c62c2`: `VirtualSMCSDK/{kern_vsmcapi.hpp, kern_value.hpp, kern_keyvalue.hpp, kern_smcinfo.hpp, AppleSmc.h, AppleSmcBridge.hpp}`, `VirtualSMC/{kern_vsmcapi.cpp, kern_vsmc.cpp, kern_keystore.cpp, kern_prov.cpp, kern_pmio.cpp, kern_mmio.cpp, kern_value.cpp, kern_start.cpp, Info.plist}`, `Sensors/{SMCProcessor/*, SMCSuperIO/Info.plist, SMCBatteryManager/Info.plist}`, `Docs/{SensorInfo.md, iStat.txt, SMCDumps/*.txt}` (https://github.com/acidanthera/VirtualSMC).
- Lilu master `0249c3a`: `Lilu/Headers/{kern_util.hpp, kern_config.hpp, kern_compat.hpp, plugin_start.hpp, kern_version.hpp}`, `Lilu/Library/plugin_start.cpp` (https://github.com/acidanthera/Lilu).
- Binaries: `deps/VirtualSMC/VirtualSMC-1.3.9-RELEASE/{Kexts/VirtualSMC.kext, Kexts/SMCProcessor.kext, dSYM/VirtualSMC.kext.dSYM}`, `deps/Lilu/Lilu-1.7.3-RELEASE/Lilu.kext` (`nm`, `dwarfdump` and `plutil` only).
- SMCAMDProcessor `029ef3a`: `deps/SMCAMDProcessor/src/SMCAMDProcessor/*`, `SMCAMDProcessor.xcodeproj/project.pbxproj`.
- Stats `1a4a9a6`: `Modules/Sensors/values.swift`, `Modules/Sensors/readers.swift`, `Modules/CPU/readers.swift`, `SMC/smc.swift` (https://github.com/exelban/stats).
- HWMonitorSMC2 `80168fb`: `HWMonitorSMC/HWMonitorSensors/{SMC_Keys.swift, HWSensorScanner.swift, SMC_DataTypes.swift}` (https://github.com/CloverHackyColor/HWMonitorSMC2).
- Project: `AMDZenPM/src/AMDZenShared.h`, `AMDZenPM/src/AMDZenPM.cpp` (lines 54-59, 318, 466, 1512-1520, 1669-1681), `AMDZenPM/Info.plist`, `Makefile`, `tools/linkcheck.sh`, `docs/research/linux-sensors.md`.

## Critic notes

Independent review on 2026-11-08. The primary sources were re-fetched into `/tmp/critic_smc` and not taken from the author's `/tmp/smcref`:
- VirtualSMC master. The GitHub API confirms HEAD = `b5c62c2311540c529c00996d0d9cec33f8e571c9`.
- Lilu master `kern_util.hpp`.
- Stats master. The API confirms HEAD = `1a4a9a6079196cf1ba87124eca2b6ba4e674024a`.
- XNU `xnu-12377.121.6` `osfmk/i386/fpu.c`.

The shipped binaries were read with `nm`, `dwarfdump` and `plutil` only. One userspace compile/run of `sizeof` on `AMDZenShared.h`. Nothing was loaded, no MSR was touched, and no sudo was used.

### Verified (matches the document)
- **ABI layout.** Checked against the 1.3.9 dSYM:
  - `VirtualSMCValue` is 0x90: vptr@0, `data[120]`@0x8, `size`@0x80, `type`@0x84, `attr`@0x88, `serializeLevel`@0x8c.
  - `VirtualSMCKeyValue` is 0x18 (`key`, `value`, `backup`).
  - `VirtualSMCAPI::Plugin` is 0x48: `product`@0, `version`@8, `apiver`@0x10, `data`@0x18, `dataHidden`@0x30.
  - These match the master headers.
- **API constants.**
  - `Version == 1`, and `loadPlugin` returns `kIOReturnInvalid` on mismatch.
  - `PluginMax == 16`, with first-free-slot CAS.
  - `ServiceName "APP0001"` and `SubmitPlugin "VirtualSMCSubmitPlugin"`.
  - `registerHandler` uses `addMatchingNotification(gIOFirstPublishNotification, nameMatching)`.
- **Exports.** The 27 `nm -gU` exports of VirtualSMC 1.3.9 and their mangled signatures match the SDK.
- **Bundle versions.**
  - VirtualSMC: CFBundleVersion 1.3.9, compat 1.0.
  - Lilu: 1.7.3, compat 1.2.0.
- **Sort order and lookup.** Order is `compare()` on `OSSwapInt32(key)` (ASCII order), lookup is a binary search, and base-store-then-plugins resolution is first-match.
- **`readAccess` contract.** The vendor contract text is at SensorInfo.md:16, and "may or may not be a Lilu plugin" is at :20.
- **Type encodings.**
  - `encodeSp` is sign-magnitude and uses `15 - integral` fraction bits, so sp78/sp96/sp3c have 8/6/12 fraction bits.
  - `valueWithFlt` stores raw host-order float bits.
  - The MacPro7,1 dump values are confirmed: TC0E/TC0F `C22F`, TCXR `C180`, TC0T `FFF5`, PC0R `0AA00A42` (34.656 W), VC0C `2181E53F` (1.793 V).
- **Stats behaviour.**
  - The CPU order is TC0D, TC0E, TC0F, TC0P, TC0H, each accepted if < 110.
  - sp78/sp96 are decoded unsigned.
  - All-zero values are hidden.
  - `%` expands to 0..9 only.
- **Base store.** Info.plist adds only BATP, FNum, LsNM and MSTc/e/f/g/i/j/m. The code-added base keys contain no T*, P*, V* or I* sensor keys.
- **`-mkernel`.** It still emits SSE (`cvtsi2sd`, `mulsd`, `cvttsd2si`), and adds only `-disable-red-zone -no-implicit-float`.
- **Integer `flt` encoder.** Checked against IEEE `struct.pack('<f')` over 300k random inputs. The only mismatches are exact round-half ties, 1 ulp apart, for values ≥ ~2.1e6 milli-units. The `sp` example 45 250 m°C gives `0x2D40`.
- **Project references.** `registerService()` @466, `runGated` copy calls @1514/1519, C exports @1671-1681, the conflict list includes `SMCAMDProcessor` @57, and `sizeof(AMDZenSample)` = 5304 / `AMDZenInfo` = 1720.

### Corrected
1. **"Second override of a base key panics" is wrong.** The CAS targets a local copy's `backup`, so the panic cannot fire. The replaced value is deleted instead, which means a use-after-free for the first plugin on a second override. VirtualSMC also erases overridden keys from the submitted plugin vector (§1.4).
2. **SMCAMDProcessor key order.** Its keys are **not** sorted. PCPR and PSTR are unreachable by name (§2). It also registers the handler before adding keys.
3. **`readAccess` execution context.** It runs from the `_ioTrapHandler` trampoline, which saves no xmm/ymm registers. That makes the integer-only rule a hard, source-backed requirement, not a recalled one (§0, §1.5, §7.1).
4. **Order in §6.1.** The conflict check must precede `registerHandler()`, which must come last. VirtualSMC retains the plugin even when submission fails.
5. **Keys must be public.** Sensor keys must go in `data`, not `dataHidden`, because Stats discovers keys only by index (§1.4).
6. **Counts.** There are 42 dump files, not 47. TC0D appears on 8 laptops plus VMware, not 9.
7. **Timer period.** §0 said 1 s and §6.1 said 500 ms. Unified to 500 ms.
8. **VirtualSMC 1.3.9 provenance.** It is a Dortania build-repo nightly, not a tagged release.

### Still unverified
- **IOKit `addMatchingNotification` timing.** It invoking the first-publish handler synchronously for already-published services is recalled, not read.
- **SMC generation on this host.** Whether VirtualSMC runs gen-1 PMIO or gen-2 MMIO for MacPro7,1 is unverified, and so is the exact lock and interrupt state AppleSMC holds at the trampoline point. The design must assume the worst case.
- **1.3.9 binary vs master.** `loadPlugin` in the 1.3.9 binary was not disassembled. The override and deletion behaviour is from master source only.
- **HWMonitorSMC2 and iStat details** (§3.3) were not re-checked.
- **`ACPI_SMC_PlatformPlugin`** reaction to new keys is unknown.
- **`AMDZenPM_copySample()` lifetime race.** It uses `AMDZenPM::instance()` without a retain. That is safe only while AMDZenPM's `stop()` cannot run concurrently. Dependency keeps the kext loaded, but not the service alive, so this needs a review in AMDZenPM.
