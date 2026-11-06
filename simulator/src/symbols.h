// Names for addresses, from analysis/symbols.tsv (the firmware's address map).
#pragma once

#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace sim {

struct Symbols {
    std::map<std::string, uint32_t> by_name;
    std::vector<std::pair<uint32_t, std::string>> funcs;   // sorted by address
    std::vector<std::pair<uint32_t, std::string>> data;    // sorted by address
    bool load(const std::string& tsv_path);
    std::optional<uint32_t> find(const std::string& name) const;
    uint32_t at(const std::string& name) const;            // 0 if unknown
    // "name+0x1c" for an address inside a function, or "" when none is near.
    std::string describe(uint32_t addr) const;
    // An address from "name", "name+0x10", "0x1234" or "1234" (hex).
    std::optional<uint32_t> parse(const std::string& text) const;
};

}  // namespace sim
