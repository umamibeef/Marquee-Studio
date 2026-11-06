#include "symbols.h"

#include <algorithm>
#include <cstdio>
#include <fstream>
#include <sstream>

namespace sim {

bool Symbols::load(const std::string& tsv_path) {
    std::ifstream f(tsv_path);
    if (!f)
        return false;
    std::string line;
    while (std::getline(f, line)) {
        if (line.empty() || line[0] == '#')
            continue;
        std::vector<std::string> col;
        std::stringstream ss(line);
        std::string c;
        while (std::getline(ss, c, '\t'))
            col.push_back(c);
        if (col.size() < 3 || (col[1] != "func" && col[1] != "data") || col[0].size() != 8)
            continue;
        uint32_t a = (uint32_t)std::stoul(col[0], nullptr, 16);
        by_name.emplace(col[2], a);
        (col[1] == "func" ? funcs : data).emplace_back(a, col[2]);
    }
    std::sort(funcs.begin(), funcs.end());
    std::sort(data.begin(), data.end());
    return !by_name.empty();
}

std::optional<uint32_t> Symbols::find(const std::string& name) const {
    auto it = by_name.find(name);
    if (it == by_name.end())
        return std::nullopt;
    return it->second;
}

uint32_t Symbols::at(const std::string& name) const { return find(name).value_or(0); }

std::string Symbols::describe(uint32_t addr) const {
    auto it = std::upper_bound(funcs.begin(), funcs.end(), std::make_pair(addr, std::string("\x7f")));
    if (it == funcs.begin())
        return "";
    --it;
    uint32_t off = addr - it->first;
    if (off > 0x4000)
        return "";
    if (!off)
        return it->second;
    char buf[32];
    snprintf(buf, sizeof buf, "+0x%x", off);
    return it->second + buf;
}

std::optional<uint32_t> Symbols::parse(const std::string& text) const {
    std::string t = text;
    t.erase(0, t.find_first_not_of(" \t"));
    t.erase(t.find_last_not_of(" \t") + 1);
    if (t.empty())
        return std::nullopt;
    uint32_t add = 0;
    auto plus = t.find('+');
    if (plus != std::string::npos) {
        try {
            add = (uint32_t)std::stoul(t.substr(plus + 1), nullptr, 16);
        } catch (...) {
            return std::nullopt;
        }
        t = t.substr(0, plus);
    }
    if (auto a = find(t))
        return *a + add;
    try {
        size_t used = 0;
        uint32_t v = (uint32_t)std::stoul(t, &used, 16);
        if (used == t.size())
            return v + add;
    } catch (...) {
    }
    return std::nullopt;
}

}  // namespace sim
