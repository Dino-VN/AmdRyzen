#ifndef CPPC_LAB_HOST_INPUT_HPP
#define CPPC_LAB_HOST_INPUT_HPP
#include "CppcLab.hpp"

namespace cppc_lab_host {
struct Inputs {
    cppc_lab::Pcct pcct{};
    cppc_lab::Madt madt{};
    cppc_lab::StaticCpc cpc{};
    cppc_lab::CpuMap map{};
    uint8_t pcctBytes[cppc_lab::kMaxTableBytes]{};
    uint8_t madtBytes[cppc_lab::kMaxTableBytes]{};
    size_t pcctSize = 0, madtSize = 0;
};
bool readFile(const char *path, uint8_t *out, size_t capacity, size_t &size);
bool loadInputs(bool hexTables, const char *pcctPath, const char *madtPath,
                const char *cpcPath, const char *mapPath, Inputs &out);
} // namespace cppc_lab_host
#endif
