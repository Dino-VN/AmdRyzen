# AMD Zen sensor register spec (from Linux and related drivers)

Status: research notes, written 2026-11-08. Target host: Ryzen 7 5700X, family 19h, model 21h (Vermeer), stepping 2.

Sources were fetched from `raw.githubusercontent.com/torvalds/linux/<tag>/...` (master and older tags) and from the out-of-tree drivers listed in section 9. All prose and pseudocode here are original. Only register facts, constants and formulas were carried over. **Do not paste GPL code into the kext.**

Notation:
- `SMN[x]`: a 32-bit read of System Management Network address `x`.
- `MSR[x]`: an RDMSR.
- `[hi:lo]`: a bit field.
- Temperatures are in millidegrees C (m°C) unless stated otherwise.

---

## 0. Host facts verified on this machine (read-only probes)

| Item | Value | How it was checked |
|---|---|---|
| CPUID 1 EAX | `0x00A20F12`: family 19h, model 21h, stepping 2 | user-space `cpuid` |
| CPUID 8000_0007 EDX | `0x00006799`: bit14 RAPL=1, bit9 CPB=1, bit8 TscInvariant=1, bit7 HwPstate=1, bit12 AccPower=0 | user-space `cpuid` |
| CPUID 6 ECX bit0 (EffFreq, APERF/MPERF) | 1 | user-space `cpuid` |
| CPUID 8000_001E ECX[10:8] (NodesPerProcessor-1) | 0, so 1 node | user-space `cpuid` |
| CPUID 8000_0001 EBX[31:28] PkgType | 2 (AM4) | user-space `cpuid` |
| Root complex 00:00.0 | 1022:**1480**. This is Linux `PCI_DEVICE_ID_AMD_17H_M30H_ROOT`, the same root ID as Matisse and Rome | task brief and ioreg |
| Data Fabric 00:18.0-7 | 1022:1440..**1447**. F3 = **0x1443** (`PCI_DEVICE_ID_AMD_17H_M70H_DF_F3`), *not* 0x1653 | `ioreg` |

The DF F3 ID matters. Vermeer reuses the Matisse IO die, so Linux k10temp matches this CPU through the 17h M70h F3 ID. It then uses `boot_cpu_data` (family 19h, model 21h) to choose CCD offsets.

Other kexts are loaded that probably also touch SMN: `wtf.spinach.AMDRyzenCPUPowerManagement` 0.7.2 and `com.overlorddev.RyzenHV` 0.7.8. See the concurrency notes in section 1.6.

---

## 1. SMN access

Sources:
- `arch/x86/kernel/amd_node.c` (master): `__amd_smn_rw()`, `amd_smn_read()`, `amd_smn_write()`, `amd_smn_hsmp_rdwr()`, `get_next_root()`, `amd_smn_init()`.
- `arch/x86/include/asm/amd/node.h` (master): `MAX_AMD_NUM_NODES`, `AMD_NODE0_PCI_SLOT`, `amd_num_nodes()`.
- `arch/x86/kernel/amd_nb.c` (v6.12 and v5.8): the older ID-table implementation.

### 1.1 Index/data pairs in the root complex (bus 0, dev 0, fn 0) config space

| Pair | Index offset | Data offset | Linux user |
|---|---|---|---|
| SMN (general) | **0x60** | **0x64** | `amd_smn_read`/`amd_smn_write` (k10temp, EDAC, ...), and all versions since 4.x |
| HSMP / SMU | **0xC4** | **0xC8** | `amd_smn_hsmp_rdwr()` (amd_hsmp driver only). The out-of-tree `ryzen_smu` uses this pair for **all** of its SMN accesses (`SMU_PCI_ADDR_REG 0xC4` / `SMU_PCI_DATA_REG 0xC8`) |
| "NB index" | 0xB8 | 0xBC | Linux uses this **only on family 15h models 60h/70h** (k10temp `read_htcreg_nb_f15`/`read_tempreg_nb_f15`, which read D0F0xBC_xD820_0C64/0CA4). No Linux code uses it on Zen. Treat it as unused/undocumented on Zen. |

A comment in `amd_smn_init()` says "There are a few SMN index/data pairs and other registers that shouldn't be accessed by user space". For that reason Linux reserves the **entire 256-byte config space** of each root exclusively, using `pci_request_config_region_exclusive(root, 0, PCI_CFG_SPACE_SIZE)`.

### 1.2 Access sequence (`__amd_smn_rw`)

```
lock(smn_mutex)                       // one global mutex covers both pairs
cfg_write32(root, INDEX_OFF, smn_addr)
if error: unlock, return error
read:  value = cfg_read32(root, DATA_OFF)
write: cfg_write32(root, DATA_OFF, value)
unlock
```

- In `amd_smn_read()`, a value of `0xFFFFFFFF` (`PCI_POSSIBLE_ERROR`) produces `-ENODEV` and the value is forced to 0. Callers must still handle Read-As-Zero registers themselves.
- Writes are not verified. Callers do their own read-back.
- The lock is a sleeping mutex. Accesses are not made from interrupt context.
- The debugfs write path taints the kernel with `TAINT_CPU_OUT_OF_SPEC`, which shows how risky Linux considers arbitrary SMN writes.

### 1.3 Root discovery and node mapping (master, `amd_smn_init` + `get_next_root`)

1. Enumerate PCI devices with class `PCI_CLASS_BRIDGE_HOST << 8` (0x060000), `devfn == 0`, and vendor 0x1022 (AMD) or 0x1D94 (Hygon), in enumeration order. Count them as `num_roots`.
2. Compute `num_nodes = amd_num_nodes() = topology_amd_nodes_per_pkg() * topology_max_packages()`. The nodes-per-package value comes from CPUID 8000_001E ECX[10:8] + 1.
3. Compute `roots_per_node = num_roots / num_nodes`. If that is 0, warn (unless running on a hypervisor) and use 1.
4. Assign `amd_roots[node] = the (node * roots_per_node)-th root`. The others are skipped, because multi-root parts such as Rome or Milan have several IOHC roots per node.
5. The DF "node" PCI devices are at bus 0, slot `0x18 + node` (`AMD_NODE0_PCI_SLOT = 0x18`), functions 0-7, with at most 8 nodes (`MAX_AMD_NUM_NODES`). k10temp derives the node ID from its F3 device as `PCI_SLOT(devfn) - 0x18`.
6. This only runs if `X86_FEATURE_ZEN` is set. Hygon 18h also sets that flag in `hygon.c`.

Older kernels (v5.x to v6.12, `amd_nb.c`) matched roots by device ID, and required `root_count % (root_count / misc_count) == 0`. The root IDs were:

| Define | ID |
|---|---|
| 17H_ROOT (Zeppelin: Naples/Summit/Pinnacle/TR1/TR2) | 0x1450 |
| 17H_M10H_ROOT (Raven/Picasso) | 0x15D0 |
| 17H_M30H_ROOT (Rome/Castle Peak/Matisse, **also Vermeer and Milan in practice**) | **0x1480** |
| 17H_M60H_ROOT (Renoir/Lucienne/Cezanne) | 0x1630 |
| 17H_MA0H_ROOT / 19H_M40H_ROOT | 0x14B5 |
| 19H_M10H_ROOT (Genoa) | 0x14A4 |
| 19H_M60H_ROOT (Raphael) | 0x14D8 |
| 19H_M70H_ROOT (Phoenix) | 0x14E8 |
| 1AH_M00H_ROOT (Turin) | 0x153A |
| 1AH_M20H_ROOT | 0x1507 |
| 1AH_M60H_ROOT | 0x1122 |
| MI200 / MI300 | 0x14BB / 0x14F8 |
| Hygon root | vendor 0x1D94, device 0x1450 |

DF F3 ("misc") IDs, from `include/linux/pci_ids.h` and k10temp's local defines:

- 17h: 0x1463 (M00), 0x15EB (M10), 0x1493 (M30), 0x13F3 (M40), 0x144B (M60), **0x1443 (M70, also the 5700X)**, 0x1663 (M90), 0x1727 (MA0)
- 19h: 0x1653 (M00), 0x14B0 (M10), 0x167C (M40), 0x166D (M50), 0x14E3 (M60), 0x14F3 (M70), 0x12FB (M78)
- 1Ah: 0x12C3 (M00), 0x16FB (M20), 0x12CB (M50), 0x124B (M60), 0x12BB (M70), 0x1243 (M80), 0x127B (M90)

The DF F4 ("link") ID is F3 + 1.

### 1.4 Hygon family 18h

- Up to v6.17, k10temp matched `PCI_VDEVICE(HYGON, 0x1463)` and treated 18h exactly like 17h.
- **Master has removed it.** Upstream series "x86/hygon: Add Family 0x18 DF node enumeration and SMN access" (v5, 2026-09), patch "hwmon: (k10temp) Stop matching Hygon devices", says the AMD accessors do not give usable results on Hygon:
  - Hygon DF slots carry no node identity.
  - DF nodes share one SMN root per socket.
  - CPUID 8000_001E ECX[7:0] node IDs are sparse (`(socket << 4) | cdd_index`).
- The new Hygon accessor still uses index/data **0x60/0x64**. It finds roots by host-bridge class, function 0 and vendor 0x1D94, and assigns `roots[node] = reserved_roots[socket_id * roots_per_socket]`.
- Hygon DF F3 IDs: 0x1463 (generic), 0x1493 (M04h), 0x14B3 (M05h).
- **The spec gives no Hygon temperature registers.** Recommendation: support Hygon for RAPL only, and leave Hygon temperatures disabled.

### 1.5 Single-node desktop (this host)

`num_nodes = 1`, and node 0 uses root 00:00.0 (1022:1480). Every SMN address in this document is read through `cfg[0:0.0] 0x60/0x64`.

### 1.6 XNU implementation notes (design guidance, not from Linux)

- Use `IOPCIDevice::configWrite32(0x60, addr)` and then `configRead32(0x64)` on the 00:00.0 nub. `IOPCIDevice::extendedConfigRead32` is not needed, because all offsets are below 0x100.
- **Concurrency hazard.** The index/data pair is global hardware state. Linux protects it with one kernel-wide mutex. On macOS there is no shared lock with the other kexts that are already loaded (AMDRyzenCPUPowerManagement, RyzenHV, and possibly VirtualSMC plugins). If two drivers interleave index and data accesses, either can read the wrong register or write to the wrong address. Mitigations, in order of preference:
  1. Ship as the sole SMN user (disable the other kexts).
  2. Serialize inside our kext with an `IOLock`, and sanity-check values (for example, Tctl within 0-130 °C; CCD valid bit set).
  3. Avoid all SMN writes.

  The 0xC4/0xC8 pair has fewer users, but `ryzen_smu`-style tools and the SMU mailbox use it, so it is not safe either.

---

## 2. Temperature (k10temp)

Source: `drivers/hwmon/k10temp.c` (master). Functions: `get_raw_temp()`, `read_tempreg_nb_zen()`, `read_ccd_temp_reg()`, `k10temp_read_temp()`, `k10temp_get_ccd_support()`, `k10temp_probe()`, `k10temp_is_visible()`. History was checked against tags v5.8, v5.10, v5.11, v6.1, v6.5, v6.6, v6.12-v6.17.

### 2.1 Tctl register (all Zen: 17h, 19h, 1Ah)

`ZEN_REPORTED_TEMP_CTRL_BASE = SMN 0x0005_9800` (THM_TCON_CUR_TMP)

| Bits | Name (Linux define) | Meaning |
|---|---|---|
| [31:21] | CUR_TEMP (`ZEN_CUR_TEMP_SHIFT = 21`) | 11-bit temperature, 0.125 °C/LSB |
| [19] | `ZEN_CUR_TEMP_RANGE_SEL_MASK` = BIT(19) = 0x80000 | When set, the range is -49..+206.875 °C |
| [17:16] | `ZEN_CUR_TEMP_TJ_SEL_MASK` = GENMASK(17,16) = 0x30000 | When **both** bits are set (`== 0x30000`), apply the same -49 °C offset. Added around v6.5. |

Decoding (`get_raw_temp`):

```
r   = SMN[0x59800]            // read on the node's root; 0 on read error
t   = (r >> 21) * 125         // m°C
if (r & BIT(19)) || ((r & 0x30000) == 0x30000):  t -= 49000
Tctl = t
```

- The -49 °C rule applies only on Zen. k10temp sets `temp_adjust_mask = ZEN_CUR_TEMP_RANGE_SEL_MASK` when `X86_FEATURE_ZEN` is set. The TJ_SEL test is unconditional in master. It used to be k10temp's only check.
- **Negative clamp.** If the result is negative, Tctl and Tdie are reported as 0. The exception is family 17h when the brand string contains `"3255"` (AMD industrial part rated -40..105 °C, flag `disp_negative`).
- **Tdie.** It is shown only when the brand string matches the offset table, and it equals `Tctl - offset`.

Tctl offset table (`tctl_offset_table`), family 17h only, matched with `strstr()` on the CPUID brand string:

| Family | Brand substring | Offset (m°C) |
|---|---|---|
| 0x17 | "AMD Ryzen 5 1600X" | 20000 |
| 0x17 | "AMD Ryzen 7 1700X" | 20000 |
| 0x17 | "AMD Ryzen 7 1800X" | 20000 |
| 0x17 | "AMD Ryzen 7 2700X" | 10000 |
| 0x17 | "AMD Ryzen Threadripper 19" (1900X/1920X/1950X) | 27000 |
| 0x17 | "AMD Ryzen Threadripper 29" (2920X/2950X/2970WX/2990WX) | 27000 |

There are no entries for 19h or 1Ah, so on those parts Tctl == Tdie. Up to v5.11, k10temp always showed Tdie on Zen (17h/18h/19h), with offset 0 when there was no table match. By v6.1, Tdie is shown only on a table match.

**On the host (19h/21h, "AMD Ryzen 7 5700X 8-Core Processor"):** there is no table match, so report a single "Tctl", which also serves as die temperature. Apply the bit19/TJ_SEL -49 °C rule and clamp at 0.

### 2.2 CCD (chiplet) temperatures

```
addr(ccd i) = 0x59800 + ccd_offset + 4*i
ZEN_CCD_TEMP_VALID = BIT(11)
ZEN_CCD_TEMP_MASK  = GENMASK(10,0) = 0x7FF
Tccd(i) = (r & 0x7FF) * 125 - 49000          // m°C, signed; k10temp does NOT clamp
```

Probe (`k10temp_get_ccd_support`): for `i in 0..limit-1`, read the register. **Skip the CCD if the SMN read returns an error**, because a read of 0xFFFFFFFF would otherwise pass the valid-bit test falsely. Enable channel `Tccd(i+1)` only if bit 11 is set. The probe runs once, and later reads do not re-check the valid bit.

The equivalent formula in zenpower and LibreHardwareMonitor is `(r & 0xFFF)*125 - 305000`. It includes bit 11, which adds 256000, and 256000 + 49000 = 305000. It gives the same result whenever the valid bit is set.

CCD map by family and model (`k10temp_probe`, master):

| Family | Models | Codename (comment) | ccd_offset | Base address | Max CCDs probed |
|---|---|---|---|---|---|
| 17h | 0x01, 0x08, 0x11, 0x18 | Zen, Zen+, Zen APU, Zen+ APU | 0x154 | 0x59954 | 4 |
| 17h | 0x31, 0x47, 0x60, 0x68, 0x71 | Zen2 TR/Rome, Cyan Skillfish, Renoir, Lucienne, Matisse | 0x154 | 0x59954 | 8 |
| 17h | 0xA0-0xAF | (Mendocino) | 0x300 | 0x59B00 | 8 |
| 19h | 0x00-0x01, 0x08, **0x21**, 0x50-0x5F | Milan/Chagall, Chagall TR, **Vermeer**, Cezanne (Green Sardine) | **0x154** | **0x59954** | **8** |
| 19h | 0x40-0x4F | Rembrandt (Yellow Carp) | 0x300 | 0x59B00 | 8 |
| 19h | 0x60-0x6F, 0x70-0x7F | Raphael, Phoenix/Hawk Point | 0x308 | 0x59B08 | 8 |
| 19h | 0x10-0x1F, 0xA0-0xAF | Genoa/Storm Peak, Bergamo/Siena | 0x300 | 0x59B00 | 12 |
| 1Ah | 0x00-0x1F | Zen5 Turin (also TR 9000 "Shimada Peak") | 0x1F0 | 0x599F0 | 16 |
| 1Ah | 0x40-0x4F | Zen5 Ryzen desktop (Granite Ridge) | 0x308 | 0x59B08 | 8 |
| any other Zen model | | (e.g. 17h M20 Dali, M90 Van Gogh, 1Ah M20/M60/M70 Strix) | — | — | 0, so Tctl only |

Version history:
- v5.8: 17h only, CCDs at a fixed 0x59954, models 01/08/11/18 with 4 CCDs and 31/71 with 8.
- v5.10: added 19h models 00-01.
- The per-model `ccd_offset` field came later.
- v6.16: added 1Ah 40-4F.
- After v6.17: added 1Ah 00-1F with offset 0x1F0 and 16 CCDs, and 17h 0x47.

**On the host (19h/21h):** probe SMN 0x59954, 0x59958, ..., 0x59970 (8 slots). The 5700X has 1 CCD, so expect only `0x59954` to have bit 11 set. Physical CCD-to-slot mapping can be sparse on parts with disabled CCDs, so keep the slot index as the label (Tccd1..8).

### 2.3 Tctl max, critical and HTC

- `hwmon_temp_max` = 70 °C, a fixed constant shown **only on non-Zen** parts (hidden when `is_zen`).
- crit and crit_hyst come from F3x64 (`REG_HARDWARE_THERMAL_CONTROL`). Zen sets no `read_htcreg`, so these are **not available on Zen** in Linux. For reference (pre-Zen, from `k10temp_read_temp`):
  - The attribute is visible only if F3xE8 (`REG_NORTHBRIDGE_CAPABILITIES`) bit 10 (`NB_CAP_HTC`) is set and F3x64 bit 0 (`HTC_ENABLE`) is set.
  - `crit = ((r >> 16) & 0x7F) * 500 + 52000`
  - `crit_hyst = (((r >> 16) & 0x7F) - ((r >> 24) & 0xF)) * 500 + 52000`
- Pre-Zen temperature is in F3xA4 (`REG_REPORTED_TEMPERATURE`), using the same `>>21 * 125` decode. Family 15h models 60h/70h use D0F0xBC_xD820_0CA4 through the 0xB8/0xBC pair.
- **Zen Tjmax / HTC limit:** Linux has no register for it. Tjmax per SKU is a datasheet value (5700X: 90 °C). The ryzen_smu PM table carries the THM limit and value (see section 6). For the kext, either use a per-SKU constant or omit it.

---

## 3. Energy and power (RAPL)

### 3.1 MSRs

From `arch/x86/include/asm/msr-index.h`:

| MSR | Linux name | Scope | Content |
|---|---|---|---|
| 0xC0010299 | `MSR_AMD_RAPL_POWER_UNIT` | package (identical on all CPUs) | [3:0] PU: power unit = 1/2^PU W. [12:8] **ESU**: energy unit = 1/2^ESU J. [19:16] TU: time unit = 1/2^TU s. |
| 0xC001029A | `MSR_AMD_CORE_ENERGY_STATUS` | **per physical core** (SMT siblings read the same counter) | [31:0] energy counter; the upper bits are not part of the counter |
| 0xC001029B | `MSR_AMD_PKG_ENERGY_STATUS` | per package/socket | [31:0] energy counter |

There is no power-limit MSR on AMD in Linux. The interface is read-only.

Field extraction:
- `perf/rapl.c` (`rapl_check_hw_unit`): `(msr >> 8) & 0x1F`.
- `amd_energy` (`get_energy_units`): `(msr & 0x1F00) >> 8`.
- powercap `rapl_default_check_unit`: ESU mask GENMASK(12,8), PU mask GENMASK(3,0), TU mask GENMASK(19,16).

A typical value is ESU = 16, giving 15.2588 µJ per LSB. The 5700X is expected to report ESU = 16, but **read it at runtime and never hardcode it**.

### 3.2 Feature gating

- `arch/x86/kernel/cpu/amd.c` and `hygon.c` set `X86_FEATURE_RAPL` when **CPUID 8000_0007 EDX bit 14** is set (`c->x86_power & BIT(14)`). The host has it set.
- For comparison, EDX bit 12 means "accumulated power mechanism" (`X86_FEATURE_ACC_POWER`, fam15h). The host has it cleared, and it is irrelevant here.
- `arch/x86/events/rapl.c` (master): `X86_MATCH_FEATURE(X86_FEATURE_RAPL, &model_amd_hygon)` covers AMD and Hygon with RAPL. It sets up:
  - the package PMU ("power", PKG scope, event `energy-pkg`, MSR 0xC001029B);
  - since about v6.14, a **per-core PMU "power_core"** (CORE scope, event `energy-core`, MSR 0xC001029A).
  - Each MSR is validated with a `test_msr` probe.
  - Before about v5.11 the match was by family (AMD 17h, Hygon 18h, then AMD 19h) instead of the feature bit.
- `drivers/powercap/intel_rapl_msr.c` (master, `rapl_ids`): matches AMD 17h, 19h and 1Ah, plus Hygon 18h, with `rapl_defaults_amd` (`check_unit = rapl_default_check_unit` only). `rapl_msr_priv_amd` defines:
  - package STATUS = 0xC001029B;
  - PP0 STATUS = 0xC001029A;
  - unit = 0xC0010299.

  The powercap PP0 domain is read on the package lead CPU. **It is therefore one core's energy, not the sum of all cores.** That conclusion comes from reading the code; Linux does not document it.
- `drivers/hwmon/amd_energy.c` (v5.8-v5.12, removed in 5.13) matched only server parts: AMD 17h model 0x31, 19h model 0x01, and 19h model 0x30. It did **not** match desktop 19h/21h. The core MSR exists on desktop Zen 2 and Zen 3 anyway (perf `power_core` probes it with `test_msr`).
- zenpower5 sets a "NO_RAPL_CORE" flag for 1Ah models 0x44 and 0x70, with the note "RAPL Core power unavailable/meaningless". Treat per-core energy on Zen5 client parts as unverified.

### 3.3 Wraparound and accumulation

Counter width is 32 bits (`RAPL_CNTR_WIDTH = 32`, `RAPL_MSR_MASK = 0xFFFFFFFF`, powercap `ENERGY_STATUS_MASK = GENMASK(31,0)`).

perf/rapl `rapl_event_update`: the delta is computed modulo 2^32 by shifting both raw values left 32, subtracting, then arithmetic-shifting right 32. The delta is then scaled to 2^-32 J units by `delta << (32 - ESU)`.

Overflow-poll intervals used by Linux:

| Driver | Formula | Result at ESU = 16 |
|---|---|---|
| perf/rapl `rapl_check_hw_unit` | `timer_ms = 5 * 2^(31-ESU)` (200 W reference, halved; 2 ms if ESU = 32) | 163,840 ms |
| powercap PMU | `timer_ms = (2^32 * energy_unit_pJ) / (1e6 * 200 * 2)`, with `energy_unit_pJ = 1e9 >> ESU` | about 163,800 ms |
| amd_energy | `timeout_ms = 1000 * 2^min(28, 31-ESU) / 250` (250 W reference) | 131,072 ms |

Sanity check: at ESU = 16 a full counter is 2^32 * 15.2588 µJ = 65,536 J. That lasts 262 s at 250 W and about 400 s at 165 W.

amd_energy accumulator design (`accumulate_delta`, `read_accumulate`, `energy_accumulator`, `amd_add_delta`):
- A kthread wakes every `timeout_ms`. On each wake it:
  - accumulates the package counter of every socket (read on the first online CPU of the node);
  - accumulates **one** core, round-robin (`core_id++`). Each core is therefore sampled only every `nr_cores * timeout_ms`, which is fine at less than 1 W per core but marginal for heavy loads.
- Wrap handling is `if new >= prev: acc += new - prev; else acc += UINT_MAX - prev + new`. This is **off by one**; the correct value is `2^32 - prev + new`. Use the modular formula.
- Output: `µJ = (acc + current partial) * 1e6 / 2^ESU`.
- Number of cores: `num_present_cpus / threads_per_core`, where threads_per_core = CPUID 8000_001E EBX[15:8] + 1. On the host EBX = 0x107, so 2 threads/core.
- Per-core channel *i* is read on logical CPU *i*. This assumes logical CPUs 0..N-1 are distinct cores, which is **wrong under Linux SMT enumeration and also under XNU's**. Use the APIC/core-ID mapping instead.

Security note:
- After the PLATYPUS power side channel (CVE-2020-12912), Linux restricted amd_energy reads to root (mode 0440).
- The perf RAPL PMU is system-wide-only and requires privilege.
- The kext should expose per-core energy only to privileged clients, or at reduced resolution and rate (for example, at most 1 Hz, rounded).

### 3.4 Recommended kext algorithm (original)

1. At start, read ESU from 0xC0010299 once (any CPU).
2. Keep a 64-bit accumulator per package and per core. Poll at an interval of at most 60 s. Use a much shorter interval if power is computed from deltas (1 s is typical).
3. `delta = (uint32_t)(new - prev)`, which is unsigned 32-bit wrap arithmetic. Then `energy_uJ += delta * 1e6 / 2^ESU`. Keep a 128-bit or fractional remainder to avoid drift.
4. Read the core MSR on a thread of that core, using `mp_cpus_call` or a per-CPU timer. Read the package MSR on any CPU of the package.
5. `power_W = Δenergy / Δt`. Use `mach_absolute_time`, or TSC with `tscFreq`.

**Host (19h/21h):**
- RAPL is present (CPUID bit 14 = 1).
- Package MSR 0xC001029B: yes.
- Core MSR 0xC001029A: yes. Zen 2 and Zen 3 desktop are known to implement it (LibreHardwareMonitor reads it per core on all Zen), but Linux does not explicitly confirm this model.
- Number of cores: 8.

---

## 4. SVI2 / SVI3 voltage and current telemetry

Sources:
- Linux k10temp v5.8-v5.10. SVI support was added in v5.8 and **removed in v5.11**; the unused defines remain in v5.11.
- zenpower by ocerman, v0.1.12, `zenpower.c`.
- zenpower3, AliEmreSenel fork (the one packaged by nixpkgs), `zenpower.c`.
- zenpower5 (mattkeenan), `zenpower_core.c`, `zenpower_svi2.c`.
- LibreHardwareMonitor `Amd17Cpu.cs`, used for cross-checking. LibreHardwareMonitor is MPL, and only facts were used.

None of these registers are documented by AMD for public use. k10temp's header says: "Register addresses to read chip voltage and current are also from zenpower, and not confirmed from chip datasheets. Current calibration is board specific."

### 4.1 Registers

| Address | Name | Fields |
|---|---|---|
| SMN 0x5A000 | `ZEN_SVI_BASE` / `F17H_M01H_SVI` | — |
| SMN 0x5A008 | SVI0_TFN (LibreHardwareMonitor only) | bit0 = plane0 telemetry **disabled** when 1; bit1 = plane1 disabled when 1 |
| SMN 0x5A00C | TEL_PLANE "A" (base + 0xC) | [23:16] VDDCOR (VID code), [7:0] IDDCOR (current code). LibreHardwareMonitor's comment says [24:16], but every implementation masks with 0xFF. |
| SMN 0x5A010 | TEL_PLANE "B" (base + 0x10) | same fields |
| SMN 0x5A014 | TEL_PLANE "C" (base + 0x14) | same fields |
| SMN 0x6F038 / 0x6F03C | `F17H_M02H_SVI + 0x38/0x3C` (zenpower3 only) | APU core / SoC planes on Renoir and Cezanne |
| SMN 0x73010 / 0x73014 | `F1AH_M70H_SVI_TEL_PLANE0/1` (zenpower5 only) | Zen5. **Unverified**, and contradicted by zenpower5's own README ("Zen 5 uses SVI3 ... not supported yet") |

### 4.2 Formulas

Voltage (all sources agree; SVI2 VID encoding):

```
VID = (plane >> 16) & 0xFF
V_mV = 1550 - 6.25 * VID        // k10temp: DIV_ROUND_CLOSEST(155000 - VID*625, 100)
```

zenpower5 clamps the result to 0..2000 mV.

Current, where `IDD = plane & 0xFF`:

| Source | Core | SoC |
|---|---|---|
| k10temp v5.8-v5.10 (normalized; board-specific scaling left to `sensors.conf`) | 1 A/LSB (`CFACTOR_ICORE` 1000000) | 17h M01: 0.25 A/LSB (250000). 17h M31/M71 and 19h M01: 0.31 A/LSB (310000). |
| zenpower "Zen1" formula | mA = IDD × 1039.211 | mA = IDD × 360.772 |
| zenpower "Zen2" formula (also used for Zen3; zenpower3 `zen1_calc` param overrides) | mA = IDD × 658.823 | mA = IDD × 294.3 |

Power is `P = I × V`. zenpower outputs µW as `mA × mV`.

### 4.3 Plane mapping by family and model

| Family/model | Part | Core plane | SoC plane | Current formula | Sources |
|---|---|---|---|---|---|
| 17h 01h, 08h, single node | Summit / Pinnacle Ridge | 0x5A00C | 0x5A010 | Zen1 | k10temp 5.8-5.10, zenpower, LHM |
| 17h 01h, 08h, multi-node (TR1/TR2/Naples) | | node0: SoC = 0x5A00C. node1: core = 0x5A00C. | | Zen1 | zenpower. k10temp hid current on TR/EPYC (strings "Threadripper"/"EPYC"). |
| 17h 11h, 18h | Raven / Picasso APU | 0x5A00C | 0x5A010 | Zen1 | k10temp, zenpower |
| 17h 31h | Castle Peak / Rome | 0x5A014 | 0x5A010 | Zen2 | k10temp v5.10, zenpower, LHM |
| 17h 60h | Renoir | 0x6F038 | 0x6F03C | Zen2 | zenpower3 and zenpower5 (original zenpower: no SVI) |
| 17h 71h | Matisse | 0x5A010 | 0x5A00C | Zen2 | zenpower, zenpower3, LHM. **Conflict:** k10temp v5.10 grouped 0x71 with 0x31 (core 0x5A014, SoC 0x5A010). |
| 19h 00h-01h | Milan / Chagall | 0x5A014 | 0x5A010 | Zen2 | k10temp v5.10 (SVI), zenpower3 |
| **19h 21h** | **Vermeer (host)** | **0x5A010** | **0x5A00C** | **Zen2** (core 658.823 mA/LSB, SoC 294.3 mA/LSB) | zenpower3, zenpower5, LHM (all agree). Never in mainline k10temp. |
| 19h 50h | Cezanne | 0x6F038 | 0x6F03C | Zen2 | zenpower3 |
| 19h 40h, 44h | Rembrandt | none | none | — | (LHM falls back to 0x5A00C/0x5A010; unverified) |
| 19h 61h, 1Ah 44h | Raphael, Granite Ridge | (LHM reads 0x5A010/0x5A00C but **forces both disabled**: "Readout not working for Ryzen 7000/9000") | | — | LHM. These parts use SVI3. |
| 19h 10h-1Fh, 70h-7Fh, A0h-AFh; 1Ah all | Genoa, Phoenix, Bergamo, Turin, Strix | unsupported (SVI3) | | — | zenpower5 README |

**SVI3:** no public source decodes SVI3 telemetry. For Zen4 and Zen5, voltage and current are only available from the SMU PM table (section 6) or the per-core requested VID (section 5.2).

**Host recommendation:**
- Core voltage = 1550 − 6.25 × ((SMN[0x5A010] >> 16) & 0xFF) mV.
- SoC voltage = 1550 − 6.25 × ((SMN[0x5A00C] >> 16) & 0xFF) mV.
- Optionally gate on SMN[0x5A008] bits 0/1 being clear.
- Label currents as "estimated"; scaling is board-specific.

Sanity checks:
- VID 0 is 1.55 V; VID 0xFF is −43.75 mV, which is invalid.
- Reject results below 200 mV or above 1600 mV.

---

## 5. Other useful CPU sensor registers

### 5.1 Effective frequency

- `MSR_IA32_MPERF` = 0xE7 and `MSR_IA32_APERF` = 0xE8 (msr-index.h). Gate on CPUID 6 ECX bit 0 (host: 1).
- AMD also provides read-only aliases MPERF_RO = 0xC00000E7 and APERF_RO = 0xC00000E8, which LibreHardwareMonitor uses.
- The common formula is `f_eff = (ΔAPERF / ΔMPERF) × f_P0`.
  - On Zen, MPERF counts at the P0 (base) frequency, which is also the TSC frequency, because the TSC is invariant (CPUID 8000_0007 EDX bit 8 = 1).
  - So `f_eff ≈ ΔAPERF / Δt_TSC × tscFreq`.
- Read both counters on the target CPU, back to back with interrupts off.
- `MSR_F17H_IRPERF` = 0xC00000E9 counts instructions retired. Enable it with HWCR (0xC0010015) bit 30 `IRPERF_EN`. **Writing that bit is a state change.**

### 5.2 P-state, boost and per-core VID (read-only use)

- `MSR_AMD_PERF_CTL` 0xC0010062, `MSR_AMD_PERF_STATUS` 0xC0010063, and `MSR_AMD_PSTATE_DEF_BASE` 0xC0010064 (+0..7) are in msr-index.h.
- HW P-state status is 0xC0010293 (LibreHardwareMonitor). Fields for 17h and 19h:
  - CpuFid = [7:0]
  - CpuDfsId = [13:8]
  - CpuVid = [21:14]
  - COF = FID / DfsId × 200 MHz
  - **Per-core requested VID: V = 1.55 − 0.00625 × CpuVid**. This is a useful voltage fallback that does not depend on SVI2.
  - For 1Ah, COF = Fid[11:0] × 5 MHz (LHM, citing PPR 57896).
  - These decodes are not from Linux; confirm them against the PPR in the P-state spec.
- `MSR_K7_HWCR` 0xC0010015 bit 25 `CPB_DIS` (Linux `MSR_K7_HWCR_CPB_DIS_BIT`) is 1 when boost is disabled. Read it to report boost state. Writing it is a power-management action outside this spec.
- CPUID 8000_0007 EDX bit 9 is `X86_FEATURE_CPB`. Linux forces it on for family 17h because of erratum 1076 (`amd.c`).
- The CPPC MSRs 0xC00102B0-B5 (CAP1, ENABLE, CAP2, REQ, STATUS, REQ2) belong to the P-state spec. They exist on 19h/21h if the BIOS enables CPPC.

### 5.3 Thermal throttle and PROCHOT

- **Linux has no reader for Zen HTC, PROCHOT or thermal-throttle status on client parts.** AMD does not implement `IA32_THERM_STATUS`/`IA32_PACKAGE_THERM_STATUS`, and Linux's `therm_throt` is Intel-only.
- For EPYC only, HSMP (`drivers/platform/x86/amd/hsmp/`) offers:
  - message 0x0B `HSMP_GET_PROC_HOT`;
  - message 0x04 `HSMP_GET_SOCKET_POWER`;
  - message 0x10 `HSMP_GET_CCLK_THROTTLE_LIMIT`.

  Mailbox (`plat.c`): SMN base 0x3B00000 + MSG_ID 0x10534 (1Ah M00-0F: 0x10934), RESP 0x10980, DATA 0x109E0, accessed through the **0xC4/0xC8** pair. Legacy HSMP is enabled only on 19h models 00-1F, 30-3F, 90-9F and A0-AF, and 1Ah models 00-0F. **It is not present on 19h/21h.**
- zenpower's debug dump also reads SMN 0x598BC and 0x5994C (unlabeled). Their meaning is unknown, so do not use them.
- On the host, the practical throttle indicators are:
  - effective frequency (5.1) compared with the requested frequency;
  - THM and PROCHOT fields in the SMU PM table (section 6, risky).

---

## 6. ryzen_smu: SMU mailbox and PM table (optional, risky)

Sources: the out-of-tree `ryzen_smu` (gitlab.com/leogx9r/ryzen_smu, and the maintained fork github.com/amkillam/ryzen_smu), `smu.c`/`smu.h`. Facts only; GPL-2.0.

- **Transport:** root 00:00.0 config pair **0xC4/0xC8**, with its own mutex. The driver binds to the root IDs listed in 1.3 (including 0x1480).
- **Protocol** (`smu_send_command`, 6 argument registers, `SMU_REQ_MAX_ARGS = 6`):
  1. Poll RSP until it is non-zero, meaning the mailbox is idle.
  2. Write 0 to RSP.
  3. Write ARG0..5 at ARGS + 4·i.
  4. Write the message ID to CMD.
  5. Poll RSP until it is non-zero.
  6. Read ARG0..5.
- **Response codes:** 0x01 OK, 0xFF Failed, 0xFE UnknownCmd, 0xFD RejectedPrereq, 0xFC Busy.
- **Mailbox addresses (SMN):**

| Codename | RSMU cmd / rsp / args | MP1 cmd / rsp / args | HSMP cmd / rsp / args |
|---|---|---|---|
| Summit, Pinnacle, TR1/TR2, Naples, Colfax | 0x3B1051C / 0x3B10568 / 0x3B10590 | 0x3B10528 / 0x3B10564 / 0x3B10598 | — |
| Raven, Picasso, Dali | 0x3B10A20 / 0x3B10A80 / 0x3B10A88 | 0x3B10528 / 0x3B10564 / 0x3B10998 | — |
| **Matisse, Castle Peak, Vermeer (19h 20h/21h)**, Milan, Chagall, Raphael, Granite Ridge, Storm Peak | **0x3B10524 / 0x3B10570 / 0x3B10A40** | **0x3B10530 / 0x3B1057C / 0x3B109C4** | 0x3B10534 / 0x3B10980 / 0x3B109E0 |
| Renoir, Lucienne, Cezanne | 0x3B10A20 / 0x3B10A80 / 0x3B10A88 | 0x3B10528 / 0x3B10564 / 0x3B10998 | — |
| Rembrandt, Phoenix, Hawk Point | 0x3B10A20 / 0x3B10A80 / 0x3B10A88 | 0x3B10528 / 0x3B10578 / 0x3B10998 | — |
| Strix Point, Strix Halo | 0x3B10A20 / 0x3B10A80 / 0x3B10A88 | 0x3B10928 / 0x3B10978 / 0x3B10998 | — |

- **PM table on Vermeer** (all RSMU messages):
  - 0x02 is "get SMU version" (valid on all mailboxes).
  - 0x08 gets the PM table version in ARG0.
  - 0x06 with ARG0 = ARG1 = 1 gets the DRAM base: physical address `ARG0 | ARG1 << 32`.
  - 0x05 transfers the table to DRAM. Each refresh requires a call; this is a command, not a passive read.
  - Table sizes by version: 0x2D0803 → 0x894, 0x2D0903 → 0x594, 0x380005 → 0x1BB0, 0x380505 → 0xF30, 0x380605 → 0xC10, 0x380705 → 0x8F0, 0x380804 → 0x8A4, 0x380805 → 0x8F0, 0x380904 → 0x5A4, 0x380905 → 0x5D0.
  - The table is an array of IEEE-754 floats. Field offsets are version-specific and community-reverse-engineered (ZenStates, LHM `RyzenSMU.cs`).
- **Risk:**
  - Sending SMU messages changes SMU state, and can conflict with BIOS, AGESA or other tools (Ryzen Master, other kexts).
  - Wrong message IDs can change limits or voltages.
  - Mapping the DRAM table requires `IOMemoryDescriptor::withPhysicalAddress`.
  - Recommended scope: phase 2 at most, read-only messages only (0x02, 0x08, 0x06, 0x05), behind an opt-in boot argument.

---

## 7. Host summary: family 19h, model 21h (Ryzen 7 5700X)

| Sensor | Access | Decode |
|---|---|---|
| Tctl (= Tdie) | SMN 0x59800 via cfg 0x60/0x64 on 00:00.0 | `((r>>21)*125) − (bit19 or TJ_SEL==3 ? 49000 : 0)` m°C. No Tdie offset. Clamp below 0. |
| Tccd1..8 | SMN 0x59954 + 4i (offset 0x154), i = 0..7. Expect only i = 0 valid (1 CCD). | Present if bit 11 set at probe and the read did not fail. `(r & 0x7FF)*125 − 49000`. |
| Package energy | MSR 0xC001029B (any CPU) | 32-bit counter, units of 2^−ESU J. ESU = MSR 0xC0010299[12:8]. |
| Core energy ×8 | MSR 0xC001029A on one thread of each core | same decode |
| Vcore (SVI2) | SMN 0x5A010 | 1550 − 6.25·[23:16] mV |
| VSoC (SVI2) | SMN 0x5A00C | 1550 − 6.25·[23:16] mV |
| Icore / ISoC (estimated) | same registers, [7:0] | 0.658823 A/LSB core, 0.2943 A/LSB SoC (zenpower Zen2 factors) |
| SVI telemetry enable | SMN 0x5A008 bits 0/1 (1 = disabled) | LHM only |
| Per-core requested VID | MSR 0xC0010293 [21:14] | 1.55 − 0.00625·VID V |
| Effective clock | MSR 0xE8/0xE7 (or 0xC00000E8/E7) | ΔAPERF/ΔMPERF × base frequency |
| Boost state | MSR 0xC0010015 bit 25 | 1 = boost disabled |
| DF F3 PCI ID | 1022:1443 | — |
| Root complex PCI ID | 1022:1480 | — |

---

## 8. Uncertainties and conflicts

1. **SVI2 on 17h M71:**
   - k10temp v5.10 used core 0x5A014 / SoC 0x5A010.
   - zenpower, zenpower3 and LHM use core 0x5A010 / SoC 0x5A00C.

   For 19h/21h every source that supports it agrees on core 0x5A010 / SoC 0x5A00C. **Verify on the host by comparing the result with the MSR 0xC0010293 VID under load.**
2. **SVI2 current scaling** is empirical and board-dependent. k10temp dropped all SVI telemetry in v5.11, citing undocumented and unreliable data (reason recalled from the commit history; the commit text was not re-fetched).
3. **The SVI3 register layout (Zen4/Zen5) is unknown.** zenpower5 lists 0x73010/0x73014 for 1Ah but its README says SVI3 is unsupported. Also, zenpower5's `PCI_DEVICE_ID_AMD_1AH_M40H_DF_F3 = 0x14E3` collides with Linux's 19H_M60H_DF_F3 (0x14E3). Treat zenpower5's Zen5 constants as unreliable.
4. **Per-core energy MSR on 19h/21h** has no explicit Linux statement. The perf `power_core` PMU probes it at runtime, so do the same: an RDMSR that faults must be handled with `rdmsr_carefully`, if that symbol is exported, or with a `cpuid`-gated plain read.
5. **amd_energy removal reason (v5.13):** recalled as a maintenance and side-channel concern, with no driver left after the RAPL PMU covered the same data. This was not re-verified.
6. **Zen Tjmax / HTC limit** has no Linux source. Use a per-SKU constant or the SMU PM table.
7. **Hygon 18h temperatures** are not supported in Linux master. Support Hygon for RAPL only.
8. **SMN concurrency with the other loaded kexts** (AMDRyzenCPUPowerManagement, RyzenHV) cannot be solved by locking alone. See 1.6.
9. **The 0xB8/0xBC pair is not an SMN pair on Zen** as far as Linux shows. Do not use it.

---

## 9. Source index

| Topic | File (tag) | Functions / symbols |
|---|---|---|
| SMN access | `arch/x86/kernel/amd_node.c` (master) | `__amd_smn_rw`, `amd_smn_read`, `amd_smn_write`, `amd_smn_hsmp_rdwr`, `get_next_root`, `amd_smn_init`, `SMN_INDEX_OFFSET`, `HSMP_INDEX_OFFSET` |
| Node count and slot | `arch/x86/include/asm/amd/node.h` (master) | `amd_num_nodes`, `AMD_NODE0_PCI_SLOT`, `MAX_AMD_NUM_NODES` |
| Legacy root IDs | `arch/x86/kernel/amd_nb.c` (v6.12, v5.8) | `amd_root_ids`, `amd_nb_misc_ids`, `hygon_root_ids`, `amd_cache_northbridges` |
| PCI IDs | `include/linux/pci_ids.h` (master) | `PCI_DEVICE_ID_AMD_1{7,9}H_*_DF_F3`, `PCI_VENDOR_ID_HYGON` |
| Temperatures | `drivers/hwmon/k10temp.c` (master; v5.8-v6.17 for history) | `get_raw_temp`, `read_ccd_temp_reg`, `k10temp_get_ccd_support`, `k10temp_probe`, `tctl_offset_table`, `ZEN_*` defines |
| SVI2 in k10temp | `drivers/hwmon/k10temp.c` (v5.8, v5.10) | `k10temp_read_in`, `k10temp_read_curr`, `ZEN_SVI_BASE`, `F17H_M01H/M31H_SVI_TEL_PLANE*`, `F19H_M01_SVI_TEL_PLANE*`, `*_CFACTOR_*` |
| RAPL perf | `arch/x86/events/rapl.c` (master, v5.8, v5.11, v6.12-v6.15) | `rapl_check_hw_unit`, `rapl_event_update`, `rapl_scale`, `amd_rapl_pkg_msrs`, `amd_rapl_core_msrs`, `model_amd_hygon` |
| RAPL powercap | `drivers/powercap/intel_rapl_msr.c`, `intel_rapl_common.c` (master) | `rapl_msr_priv_amd`, `rapl_defaults_amd`, `rapl_ids`, `rapl_default_check_unit` |
| amd_energy | `drivers/hwmon/amd_energy.c` (v5.12) | `get_energy_units`, `accumulate_delta`, `read_accumulate`, `amd_add_delta`, `amd_energy_probe`, `cpu_ids` |
| CPUID features | `arch/x86/kernel/cpu/amd.c`, `hygon.c`, `scattered.c` (master) | `X86_FEATURE_RAPL` (8000_0007 EDX[14]), CPB (EDX[9]), HW_PSTATE (EDX[7]), Zen generation model ranges |
| MSR numbers | `arch/x86/include/asm/msr-index.h` (master) | `MSR_AMD_RAPL_POWER_UNIT`, `MSR_AMD_{CORE,PKG}_ENERGY_STATUS`, `MSR_K7_HWCR_CPB_DIS_BIT`, `MSR_AMD_CPPC_*`, `MSR_F17H_IRPERF` |
| HSMP | `drivers/platform/x86/amd/hsmp/plat.c`, `hsmp.c`; `arch/x86/include/uapi/asm/amd_hsmp.h` (master) | `SMN_HSMP_*`, `legacy_hsmp_support`, `HSMP_GET_PROC_HOT` |
| zenpower | github.com/ocerman/zenpower `zenpower.c` (v0.1.12) | `plane_to_vcc`, `get_core_current`, `get_soc_current`, `zenpower_probe` |
| zenpower3 | github.com/AliEmreSenel/zenpower3 `zenpower.c` | adds 17h M60, 19h M00/01/21/50, `zen1_calc` |
| zenpower5 | github.com/mattkeenan/zenpower5 (`zenpower_core.c`, `_svi2.c`, `_rapl.c`, `_temp.c`) | `model_configs[]`, `F1AH_M70H_*` |
| ryzen_smu | github.com/amkillam/ryzen_smu, gitlab.com/leogx9r/ryzen_smu (`smu.c`, `smu.h`) | `smu_send_command`, mailbox tables, `smu_update_pmtable_size` |
| Cross-check | github.com/LibreHardwareMonitor `Amd17Cpu.cs` | SVI0_TFN, SVI plane map for 0x21/0x61/0x44, 0xC0010293 VID |
| Hygon SMN (upstream, 2026-09) | LKML "[PATCH v5 0/8] x86/hygon: Add Family 0x18 DF node enumeration and SMN access" | HYGON_SMN_INDEX/DATA 0x60/0x64, k10temp Hygon removal |

Downloaded copies, treated as data: `/tmp/lnxsrc` (Linux), `/tmp/zp_ocerman`, `/tmp/zp3`, `/tmp/zp5`, `/tmp/rsmu`, `/tmp/lhm`.
