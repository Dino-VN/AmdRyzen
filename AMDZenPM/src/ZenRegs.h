//
// ZenRegs.h — AMD Zen MSR / SMN / CPUID constants.
// Facts restated from Linux (msr-index.h, k10temp.c, amd_node.c, cpupower amd.c),
// AMD PPRs and community drivers; see docs/research/*.md for per-item sources.
// No code, only constants: usable from kernel and user space.
//
#ifndef ZEN_REGS_H
#define ZEN_REGS_H

#include <stdint.h>

// ---------------- MSRs ----------------
#define MSR_PATCH_LEVEL              0x0000008Bu  // microcode, low 32 bits on AMD
#define MSR_MPERF                    0x000000E7u
#define MSR_APERF                    0x000000E8u
#define MSR_MPERF_RO                 0xC00000E7u  // needs 8000_0007 EDX[10]
#define MSR_APERF_RO                 0xC00000E8u
#define MSR_IRPERF                   0xC00000E9u  // needs 8000_0008 EBX[1], HWCR[30]
#define MSR_HWCR                     0xC0010015u
#define   HWCR_INVDWBINVD            (1ull << 4)  // must be 1 when CC6 is enabled (PPR)
#define   HWCR_MON_MWAIT_DIS         (1ull << 9)
#define   HWCR_CPB_DIS               (1ull << 25)
#define   HWCR_LOCK_TSC_TO_P0        (1ull << 21) // TSC/MPERF stay at boot P0 rate if P0 is redefined
#define   HWCR_TSC_FREQ_SEL          (1ull << 24)
#define   HWCR_EFF_FREQ_CNT_MWAIT    (1ull << 26)
#define   HWCR_IRPERF_EN             (1ull << 30)
#define MSR_PSTATE_CUR_LIMIT         0xC0010061u  // [2:0] CurPstateLimit, [6:4] PstateMaxVal
#define MSR_PSTATE_CTL               0xC0010062u  // [2:0] PstateCmd
#define MSR_PSTATE_STATUS            0xC0010063u  // [2:0] CurPstate
#define MSR_PSTATE_DEF_BASE          0xC0010064u  // +0..7
#define MSR_CSTATE_BASE_ADDR         0xC0010073u  // [15:0] CstateAddr; IO reads of base..base+7 trap to C-states
#define MSR_PMGT_MISC                0xC0010292u  // [32] PC6En (CoreFreq, ZenStates); package scope, read-only use
#define MSR_CSTATE_POLICY            0xC0010294u  // undocumented in PPR (CoreFreq layout); informational
#define MSR_CSTATE_CONFIG            0xC0010296u  // CCR0/1/2: CC6EN at bits 6/14/22 (CoreFreq, ZenStates)
#define MSR_HW_PSTATE_STATUS         0xC0010293u  // [7:0] FID [13:8] DFS [21:14] VID [24:22] HwPstate (17h/19h)
#define MSR_RAPL_PWR_UNIT            0xC0010299u  // [12:8] ESU
#define MSR_CORE_ENERGY_STAT         0xC001029Au  // per core, 32-bit
#define MSR_PKG_ENERGY_STAT          0xC001029Bu  // per package, 32-bit
#define MSR_CPPC_CAP1                0xC00102B0u  // [7:0] lowest [15:8] lowest_nonlinear [23:16] nominal [31:24] highest
#define MSR_CPPC_ENABLE              0xC00102B1u  // bit0, write-once until reset
#define MSR_CPPC_CAP2                0xC00102B2u
#define MSR_CPPC_REQ                 0xC00102B3u  // [7:0] max [15:8] min [23:16] des [31:24] EPP
#define MSR_CPPC_STATUS              0xC00102B4u
#define MSR_PPIN_CTL                 0xC00102F0u  // bit0 lockout, bit1 enable
#define MSR_PPIN                     0xC00102F1u

// ---------------- CPUID bits ----------------
#define CPUID1_ECX_MWAIT             (1u << 3)
#define CPUID1_ECX_HYPERVISOR        (1u << 31)
#define CPUID5_ECX_EMX               (1u << 0)    // MWAIT extensions enumerated
#define CPUID5_ECX_IBE               (1u << 1)    // MWAIT ECX[0]: break on masked interrupt
#define CPUID6_EAX_ARAT              (1u << 2)    // APIC timer keeps running in deep C-states
#define CPUID6_ECX_EFFFREQ           (1u << 0)
#define CPUID81_ECX_TOPOEXT          (1u << 22)
#define CPUID87_EDX_HWPSTATE         (1u << 7)
#define CPUID87_EDX_TSC_INVARIANT    (1u << 8)
#define CPUID87_EDX_CPB              (1u << 9)
#define CPUID87_EDX_EFFFREQ_RO       (1u << 10)
#define CPUID87_EDX_RAPL             (1u << 14)
#define CPUID87_EDX_FAST_CPPC        (1u << 15)
#define CPUID88_EBX_IRPERF           (1u << 1)
#define CPUID88_EBX_PPIN             (1u << 23)
#define CPUID88_EBX_CPPC             (1u << 27)

// ---------------- SMN (via root complex 00:00.0 config 0x60/0x64) ----------------
#define PCI_ROOT_SMN_INDEX           0x60u
#define PCI_ROOT_SMN_DATA            0x64u
#define SMN_THM_TCON_CUR_TMP         0x00059800u
#define   TCTL_CUR_TEMP_SHIFT        21
#define   TCTL_RANGE_SEL             (1u << 19)
#define   TCTL_TJ_SEL_MASK           0x00030000u
#define   CCD_TEMP_VALID             (1u << 11)
#define   CCD_TEMP_MASK              0x7FFu
#define SMN_SVI_BASE                 0x0005A000u
#define SMN_SVI_TFN                  0x0005A008u  // bit0/1 = plane0/1 telemetry disabled (LHM)
#define SMN_SVI_PLANE_A              0x0005A00Cu
#define SMN_SVI_PLANE_B              0x0005A010u
#define SMN_SVI_PLANE_C              0x0005A014u
#define SMN_SVI_APU_CORE             0x0006F038u
#define SMN_SVI_APU_SOC              0x0006F03Cu

#define AMD_VENDOR_ID                0x1022u
#define HYGON_VENDOR_ID              0x1D94u

// CPPC EPP presets (amd-pstate legacy values)
#define EPP_PERFORMANCE              0x00u
#define EPP_BALANCE_PERFORMANCE      0x80u
#define EPP_BALANCE_POWER            0xBFu
#define EPP_POWER                    0xFFu
#define CPPC_HIGHEST_PERF_PREFCORE   166u

#endif // ZEN_REGS_H
