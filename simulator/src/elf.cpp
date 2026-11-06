// umami-marquee-sim-elf: an ELF file with the firmware's names for gdb and VS Code. Symbols only: every func/data name in
// analysis/symbols.tsv, in empty (NOBITS) .text and .data sections, so gdb reads the code from the target. It holds no
// firmware bytes and works with any image at the stock layout (the stock dump, the from-source build).
//
// Usage: umami-marquee-sim-elf [--repo DIR] [OUT]   (default OUT: build/sim/elite_rgb_syms.elf)
#include "board.h"
#include "repo.h"

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

using namespace sim;

namespace {

constexpr uint32_t BSS_END = 0x500000;

void be16(std::vector<uint8_t>& b, uint16_t v) { b.push_back(v >> 8), b.push_back((uint8_t)v); }
void be32(std::vector<uint8_t>& b, uint32_t v) { b.push_back(v >> 24), b.push_back(v >> 16), b.push_back(v >> 8), b.push_back((uint8_t)v); }

}  // namespace

int main(int argc, char** argv) {
    std::string repo_dir, out;
    for (int i = 1; i < argc; i++) {
        std::string a = argv[i];
        if (a == "--repo" && i + 1 < argc)
            repo_dir = argv[++i];
        else
            out = a;
    }
    Repo r(repo_dir);
    if (out.empty())
        out = r.path("build/sim/elite_rgb_syms.elf");
    static const char SHSTR[] = "\0.text\0.data\0.symtab\0.strtab\0.shstrtab";
    const std::string shstr(SHSTR, sizeof SHSTR);
    auto sh = [&](const char* name) { return (uint32_t)shstr.find(std::string(name) + '\0', 1); };
    std::string strtab(1, '\0');
    std::vector<uint8_t> symtab(16, 0);                  // the null symbol
    size_t n = 0;
    for (int kind = 0; kind < 2; kind++) {
        auto& list = kind == 0 ? r.symbols.funcs : r.symbols.data;
        uint32_t cap = kind == 0 ? CODE_END : BSS_END;
        for (size_t i = 0; i < list.size(); i++) {
            auto [a, name] = list[i];
            uint32_t next = i + 1 < list.size() ? list[i + 1].first : cap;
            uint32_t size = kind == 0 && a < cap ? std::min(next, cap) - a : 0;
            uint16_t sec = kind == 0 ? 1 : (a < CODE_END ? 1 : 2);
            uint8_t type = kind == 0 ? 2 : 1;            // STT_FUNC in .text / STT_OBJECT in .data
            uint32_t off = (uint32_t)strtab.size();
            strtab += name + '\0';                      // (also for a name it then leaves out, as before)
            n++;
            if (kind == 0 && a >= CODE_END)
                continue;
            be32(symtab, off), be32(symtab, a), be32(symtab, size);
            symtab.push_back((1 << 4) | type), symtab.push_back(0), be16(symtab, sec);   // STB_GLOBAL
        }
    }
    std::vector<uint8_t> body(52, 0);
    uint32_t o_symtab = (uint32_t)body.size();
    body.insert(body.end(), symtab.begin(), symtab.end());
    uint32_t o_strtab = (uint32_t)body.size();
    body.insert(body.end(), strtab.begin(), strtab.end());
    uint32_t o_shstr = (uint32_t)body.size();
    body.insert(body.end(), shstr.begin(), shstr.end());
    while (body.size() % 4)
        body.push_back(0);
    uint32_t o_sh = (uint32_t)body.size();
    const uint32_t shdrs[6][10] = {
        {0, 0, 0, 0, 0, 0, 0, 0, 0, 0},
        {sh(".text"), 8, 0x6, 0, o_sh, CODE_END, 0, 0, 4, 0},                       // NOBITS, AX
        {sh(".data"), 8, 0x3, CODE_END, o_sh, BSS_END - CODE_END, 0, 0, 4, 0},      // NOBITS, WA
        {sh(".symtab"), 2, 0, 0, o_symtab, (uint32_t)symtab.size(), 4, 1, 4, 16},
        {sh(".strtab"), 3, 0, 0, o_strtab, (uint32_t)strtab.size(), 0, 0, 1, 0},
        {sh(".shstrtab"), 3, 0, 0, o_shstr, (uint32_t)shstr.size(), 0, 0, 1, 0},
    };
    for (auto& s : shdrs)
        for (uint32_t v : s)
            be32(body, v);
    std::vector<uint8_t> hdr;
    const uint8_t ident[16] = {0x7f, 'E', 'L', 'F', 1, 2, 1, 0};   // ELFCLASS32, big-endian, version 1
    hdr.insert(hdr.end(), ident, ident + 16);
    be16(hdr, 2), be16(hdr, 20), be32(hdr, 1), be32(hdr, 0), be32(hdr, 0), be32(hdr, o_sh), be32(hdr, 0);
    be16(hdr, 52), be16(hdr, 0), be16(hdr, 0), be16(hdr, 40), be16(hdr, 6), be16(hdr, 5);   // ET_EXEC, EM_PPC
    std::copy(hdr.begin(), hdr.end(), body.begin());
    std::filesystem::create_directories(std::filesystem::path(out).parent_path());
    std::ofstream(out, std::ios::binary).write((const char*)body.data(), (std::streamsize)body.size());
    printf("%s: %zu symbols from analysis/symbols.tsv\n", out.c_str(), n);
    return 0;
}
