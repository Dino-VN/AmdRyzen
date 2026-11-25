#include "Options.hpp"
#include <stdio.h>
#include <string.h>

namespace pcc_probe {
const char *fieldOption(uint32_t index) {
    static const char *const names[kFields] = {
        "highest", "nominal", "lowest-nonlinear", "lowest", "guaranteed", "desired", "min", "max",
        "reduction-tolerance", "time-window", "wraparound", "reference-counter", "delivered-counter",
        "limited", "enable", "autonomy", "autonomous-window", "epp", "reference-performance",
        "lowest-frequency", "nominal-frequency"
    };
    return index < kFields ? names[index] : "invalid";
}
namespace {
bool decimal(const char *text, uint64_t &value) {
    value = 0;
    if (!text || *text == '\0') return false;
    for (size_t i = 0; text[i] != '\0'; ++i) {
        if (text[i] < '0' || text[i] > '9') return false;
        const uint64_t digit = static_cast<uint64_t>(text[i] - '0');
        if (value > (UINT64_MAX - digit) / 10) return false;
        value = value * 10 + digit;
    }
    return true;
}
bool fields(const char *text, uint32_t &mask) {
    if (strcmp(text, "all") == 0) { mask = kAllFields; return true; }
    mask = 0;
    const char *at = text;
    for (;;) {
        const char *end = strchr(at, ',');
        const size_t size = end ? static_cast<size_t>(end-at) : strlen(at);
        bool found = false;
        for (uint32_t i = 0; i < kFields; ++i) {
            const char *name = fieldOption(i);
            if (strlen(name) != size || strncmp(at, name, size) != 0) continue;
            if ((mask & (1u << i)) != 0) return false;
            mask |= 1u << i; found = true; break;
        }
        if (!found) return false;
        if (!end) return true;
        at = end+1;
    }
}
}
bool parseOptions(int argc, const char *const *argv, Options &o) {
    o = {};
    o.selector = Selector::Info;
    o.request = {kAbi, sizeof(Request), 0, kDefaultFields, 0, 0, {0,0}};
    if (argc < 1 || !argv) return false;
    for (int i = 1; i < argc; ++i) {
        if (!argv[i]) return false;
        // Help is unconditionally non-actuating, including `read --help`.
        if (strcmp(argv[i], "--help") == 0) { o.help = true; return true; }
    }
    int at = 1;
    if (at < argc) {
        if (strcmp(argv[at], "read") == 0) { o.selector = Selector::ReadOnce; ++at; }
        else if (strcmp(argv[at], "info") == 0) ++at;
    }
    bool cpu = false, selected = false, timeout = false, ack = false;
    for (; at < argc; ++at) {
        const char *arg = argv[at];
        if (strcmp(arg, kReadAckOption) == 0) {
            if (ack) return false;
            ack = true; o.request.acknowledgement = kReadAcknowledgement;
        } else {
            if (at+1 >= argc) return false;
            const char *value = argv[++at];
            uint64_t number = 0;
            if (strcmp(arg, "--cpu") == 0) {
                if (cpu || !decimal(value, number) || number >= kCpuCount) return false;
                cpu = true; o.request.cpu = static_cast<uint32_t>(number);
            } else if (strcmp(arg, "--fields") == 0) {
                if (selected || !fields(value, o.request.fieldMask)) return false;
                selected = true;
            } else if (strcmp(arg, "--timeout-us") == 0) {
                if (timeout || !decimal(value, number)) return false;
                timeout = true; o.request.timeoutUs = number;
            } else return false;
        }
    }
    if (o.selector == Selector::ReadOnce && (!ack || !timeout)) return false;
    return validateRequest(static_cast<uint32_t>(o.selector), o.request) == Result::Ok;
}
void printHelp() {
    puts("pccprobe [info] [--cpu 0..15] [--fields name,name|all]\n"
         "pccprobe read --cpu N --fields names --timeout-us 1..1000000\n"
         "    --acknowledge-read-writes-command-status-and-doorbell\n"
         "pccprobe --help\n\n"
         "Default: metadata only for XNU CPU 0; descriptors for capabilities,\n"
         "desired/min/max, limited, enable/autonomy/EPP, plus static frequencies.\n"
         "Register values remain unknown. No physical-memory access.\n"
         "Info works without the probe when existing AMDZenPM GetInfo and OS tables\n"
         "are available. It never calls GetSample, evaluates AML, or issues READ.\n\n"
         "This release has NO physical transport. Even an administrator with both\n"
         "boot/config gates enabled and the exact acknowledgement receives a refusal.\n"
         "A PCC ownership/power-quiescence binding remains unestablished. The gates\n"
         "are not installation or hardware-test permission. No automatic loading,\n"
         "boot-setting changes, retries, uploads, or performance controls exist.\n\n"
         "Field names:");
    for (uint32_t i = 0; i < kFields; ++i) printf("  %s\n", fieldOption(i));
}
} // namespace pcc_probe
