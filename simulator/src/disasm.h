// PowerPC disassembly (Capstone), with the firmware's names for branch targets.
#pragma once

#include "symbols.h"

#include <cstdint>
#include <string>

namespace sim {

class Disasm {
public:
    Disasm();
    ~Disasm();
    // "mnemonic operands" of the big-endian instruction word w at addr; a branch target gets its name.
    std::string one(uint32_t addr, uint32_t w, const Symbols* names = nullptr) const;

private:
    size_t handle_ = 0;
};

}  // namespace sim
