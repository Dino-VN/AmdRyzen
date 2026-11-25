#ifndef AMDZEN_PCC_PROFILE_HPP
#define AMDZEN_PCC_PROFILE_HPP
#include "ProbeABI.hpp"
#include "CppcLab.hpp"
#include "AMDZenShared.h"

namespace pcc_probe {
using cppc_lab::Bytes;
// These are OS-exposed post-boot-patch table hashes, not signatures/authentication.
constexpr const char *kProcessorHash = "bdfb454245935efe3ef7174a9efa5324c742764fcf40a8632221fd27fcf189f3";
constexpr const char *kCpcHash = "f22c5c2615bcd436bdd5601ee16d3e6269187018220ddf1e21b26efd73fe6013";
constexpr const char *kNormalizedHash = "d5fdb6087f9f0665835fa41ad2964608e7a1cb7befa738e4bb73477f8f0edcc9";
void sha256(Bytes bytes, uint8_t digest[32]);
bool hashMatches(Bytes bytes, const char *hex);

// No owning buffers. The acquisition adapter retains/copies the OS property for
// the entire validation. Never populate this from a user-client input buffer.
struct TableSet {
    Bytes pcct{}, madt{}, processors{}, cpc{};
    uint32_t pcctCount = 0, madtCount = 0, processorCount = 0, cpcCount = 0;
};
// Scan TABLE CONTENTS rather than inferring an identity from an SSDT's ordinal.
// Only signature-matching PCCT/APIC and both exact-profile SSDTs are selected.
void considerTable(Bytes bytes, TableSet &tables);

// About 150 KiB: MUST be heap/caller-owned, never a kernel automatic variable.
struct Workspace {
    cppc_lab::StaticCpc normalized;
    cppc_lab::Pcct pcct;
    cppc_lab::Madt madt;
    cppc_lab::CpuMap runtimeMap;
    cppc_lab::Discovery discovery;
    AMDZenInfo info;
    uint32_t detail;
};
Bytes normalizedProfile();
Result validateProfile(const TableSet &tables, Workspace &workspace);
void describe(const Request &request, const TableSet &tables, const Workspace &workspace, Reply &reply);
} // namespace pcc_probe
#endif
