// Where the inputs are: the repository root (found from the working directory upwards, or given), the symbols.
#pragma once

#include "symbols.h"

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <stdexcept>
#include <string>
#include <vector>

namespace sim {

inline std::vector<uint8_t> read_file(const std::string& p) {
    std::ifstream f(p, std::ios::binary);
    if (!f)
        throw std::runtime_error("cannot read " + p);
    return std::vector<uint8_t>(std::istreambuf_iterator<char>(f), {});
}

struct Repo {
    std::filesystem::path root;
    Symbols symbols;

    explicit Repo(const std::string& given = "") {
        namespace fs = std::filesystem;
        if (!given.empty())
            root = given;
        else
            for (fs::path p = fs::current_path(); !p.empty(); p = p.parent_path()) {
                if (fs::exists(p / "analysis" / "symbols.tsv")) {
                    root = p;
                    break;
                }
                if (p == p.parent_path())
                    break;
            }
        if (root.empty() || !symbols.load((root / "analysis" / "symbols.tsv").string()))
            throw std::runtime_error("run inside the marquee-sign repository (or pass --repo DIR)");
    }
    std::string path(const std::string& rel) const { return (root / rel).string(); }
};

}  // namespace sim
