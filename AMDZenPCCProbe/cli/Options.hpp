#ifndef AMDZEN_PCC_OPTIONS_HPP
#define AMDZEN_PCC_OPTIONS_HPP
#include "ProbeABI.hpp"
namespace pcc_probe {
constexpr uint32_t kDefaultFields = (1u<<0)|(1u<<1)|(1u<<2)|(1u<<3)|(1u<<5)|(1u<<6)|(1u<<7)|
                                    (1u<<13)|(1u<<14)|(1u<<15)|(1u<<17)|(1u<<19)|(1u<<20);
struct Options { bool help; Selector selector; Request request; };
bool parseOptions(int argc, const char *const *argv, Options &options);
const char *fieldOption(uint32_t index);
void printHelp();
} // namespace pcc_probe
#endif
