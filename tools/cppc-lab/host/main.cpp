#include "Input.hpp"
#include <inttypes.h>
#include <stdio.h>
#include <string.h>

int main(int argc, char **argv) {
    using namespace cppc_lab;
    if (argc != 6 || (strcmp(argv[1], "--hex") != 0 && strcmp(argv[1], "--binary") != 0)) {
        fprintf(stderr, "Usage: cppc-lab --hex|--binary PCCT MADT STATIC_CPC XNU_MAP\n"
                        "Offline regular-file input only. No hardware transport.\n");
        return 2;
    }
    static cppc_lab_host::Inputs input;
    static Discovery discovery;
    if (!cppc_lab_host::loadInputs(strcmp(argv[1], "--hex") == 0,
                                  argv[2], argv[3], argv[4], argv[5], input)) return 1;
    const Code code = importDescriptors(input.cpc, input.pcct, input.madt, input.map, discovery);
    if (code != Code::Ok) { fprintf(stderr, "Import rejected: %s\n", codeName(code)); return 1; }
    printf("OFFLINE DISCOVERY ONLY; not a working CPPC driver or native performance mode.\n");
    printf("Imported %zu CPUs, %zu HW_ALL domains, %zu shared PCC channel(s).\n",
           discovery.cpuCount, discovery.domainCount, discovery.pcct.count);
    printf("XNU identities are caller-supplied, not measured by this program.\n");
    for (size_t i = 0; i < discovery.cpuCount; ++i) {
        const CpuDescriptor &cpu = discovery.cpus[i];
        printf("%s %s=%" PRIu32 " -> APIC=%" PRIu32 " -> caller XNU=%" PRIu32
               " domain=%" PRIu32 "\n", cpu.path,
               cpu.identityKind == IdentityKind::Lapic ? "ProcessorID" : "UID",
               cpu.acpiId, cpu.apicId, cpu.xnuCpu, cpu.domain);
    }
    for (size_t i = 0; i < discovery.pcct.count; ++i) {
        const ChannelDescriptor &channel = discovery.pcct.channels[i];
        printf("Subspace %zu: %" PRIu64 " bytes; nominal %" PRIu32
               " us (NOT a deadline); maximum %" PRIu32 "/minute; turnaround %u us.\n",
               i, channel.length, channel.nominalLatencyUs, channel.maxCommandsPerMinute,
               static_cast<unsigned>(channel.minTurnaroundUs));
    }
    const CpuDescriptor &first = discovery.cpus[0];
    for (size_t i = 0; i < kFieldCount; ++i) {
        const Value value = initialValue(first.fields[i]);
        printf("%s %s: ", first.path, fieldName(i));
        if (value.kind == ValueKind::StaticInteger) printf("static integer %" PRIu64, value.number);
        else if (value.kind == ValueKind::Unsupported) printf("unsupported/absent");
        else printf("UNKNOWN (descriptor is not a register value)");
        printf("\n");
    }
    printf("No simulated or real PCC command was issued by this importer.\n"
           "Even a future hardware PCC READ writes command/status and rings a doorbell;\n"
           "it requires separate hardware authorization.\n");
    return 0;
}
