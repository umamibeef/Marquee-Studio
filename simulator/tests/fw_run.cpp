// The full from-source firmware in the simulator: boot on the real board's EEPROMs, open the console with its magic line,
// run console commands (and a real store upload: sbegin/sfile/sraw/scommit, as the Sign window), keep frames.
// Usage: apprun_full IMAGE OUT.raw INSNS [SKIP] [cmd:TEXT | file:NAME=PATH | wait:INSNS | commit] ...
#include "board.h"
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <string>
#include <vector>
#include <algorithm>
#include <zlib.h>
using namespace sim;
static std::vector<uint8_t> readf(const std::string& p) {
    std::ifstream f(p, std::ios::binary);
    return std::vector<uint8_t>((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
}
static std::string tail(Board& b, size_t& from) {
    auto& t = b.serial_tx[0]; std::string s;
    for (size_t i = from; i < t.size(); i++) s += (t[i] >= 32 && t[i] < 127) || t[i] == '\n' ? (char)t[i] : (t[i] == '\r' ? '\0' : '.');
    from = t.size(); std::string o; for (char c : s) if (c) o += c; return o;
}
int main(int argc, char** argv) {
    if (argc < 4) return 2;
    Scenario sc; sc.geom_word = 0xC240; sc.geom_w = 200; sc.geom_h = 16;
    Board b(readf(argv[1]), sc, std::vector<uint8_t>());
    b.fpga_model = true;
    const std::string R = std::string(getenv("REPO") ? getenv("REPO") : ".") + "/dumps/serial/";
    b.eeproms[0xA0] = readf(R + "eeprom_a0.bin"); b.eeproms[0xA2] = readf(R + "eeprom_a2.bin");
    if (!b.boot()) { printf("boot: NO\n"); return 1; }
    b.frames_kept = 20000;
    b.app.resize(CODE_END); b.read(0, b.app.data(), CODE_END); b.hook_app_specials();   // our panel runs on an interrupt: mtmsr/rfi must be modelled
    std::string drawn; uint64_t fc_addr = 0;
    std::vector<uint8_t> presented;
    uint64_t step = 60000000; size_t seen = 0;
    auto run = [&](uint64_t n) { b.run_app(b.icount + n); };
    auto say = [&](const std::string& s) { b.serial_send(0, std::vector<uint8_t>(s.begin(), s.end()), 0, 0); };
    run(step); printf("%s", tail(b, seen).c_str());
    say("MQ?MON\r"); run(step); printf("%s", tail(b, seen).c_str());
    for (int i = 4; i < argc; i++) {
        std::string a = argv[i];
        if (a.rfind("cmd:", 0) == 0) { say(a.substr(4) + "\r"); run(step); }
        else if (a == "mark") { b.frames.clear(); }
        else if (a.rfind("drawn:", 0) == 0) {          // drawn:FILE:FC_ADDR:PANEL_PRESENT: each frame as the firmware hands it over
            auto c = a.find(':', 6), c2 = a.find(':', c + 1);
            drawn = a.substr(6, c - 6); fc_addr = strtoull(a.c_str() + c + 1, 0, 16);
            b.on_exec((uint32_t)strtoull(a.c_str() + c2 + 1, 0, 16), [&presented, fc_addr](Board& bb) {
                uint32_t px = bb.reg(UC_PPC_REG_3), fc = bb.r32((uint32_t)fc_addr) - 1;   // composed before the count went up
                uint8_t le[4] = {(uint8_t)fc, (uint8_t)(fc >> 8), (uint8_t)(fc >> 16), (uint8_t)(fc >> 24)};
                presented.insert(presented.end(), le, le + 4);
                size_t at = presented.size();
                presented.resize(at + 6400);
                bb.read(px, presented.data() + at, 6400);
            });
        }
        else if (a.rfind("cmdfile:", 0) == 0) { std::ifstream f(a.substr(8)); std::string l;
            while (std::getline(f, l)) { if (l.rfind("sbegin",0)==0||l.rfind("sfile",0)==0||l.rfind("sraw",0)==0||l.rfind("sd ",0)==0||l.rfind("scommit",0)==0||l.empty()) continue; say(l + "\r"); run(step / 4); } }
        else if (a.rfind("ufile:", 0) == 0) { auto d = readf(a.substr(6)); char h[96];
            snprintf(h, sizeof h, "ubegin %zx %lx\r", d.size(), (unsigned long)crc32(0, d.data(), (unsigned)d.size())); say(h); run(step / 4);
            for (size_t o = 0; o < d.size(); o += 256) { size_t n = std::min<size_t>(256, d.size() - o); snprintf(h, sizeof h, "uraw %zx\r", n); std::string q = h; q.append((const char*)d.data() + o, n); say(q); run(step / 8); }
            printf("%s", tail(b, seen).c_str()); say("ucommit\r"); run(step * 4);
            auto it = std::search(b.main_flash.begin(), b.main_flash.end(), d.begin(), d.end());
            printf("%s\nslot: image %s in main flash%s, nvram tries %02x flags %02x\n", tail(b, seen).c_str(), it == b.main_flash.end() ? "NOT" : "found", it == b.main_flash.end() ? "" : (" at +" + std::to_string(it - b.main_flash.begin())).c_str(), 0, 0); }
        else if (a.rfind("wait:", 0) == 0) { run(strtoull(a.c_str() + 5, 0, 10)); }
        else if (a.rfind("file:", 0) == 0) {
            auto eq = a.find('='); std::string nm = a.substr(5, eq - 5); auto d = readf(a.substr(eq + 1));
            char h[96]; snprintf(h, sizeof h, "sfile %s %zx\r", nm.c_str(), d.size()); say(h); run(step / 4);
            for (size_t o = 0; o < d.size(); o += 256) {
                size_t n = std::min<size_t>(256, d.size() - o);
                snprintf(h, sizeof h, "sraw %zx\r", n); std::string s = h; s.append((const char*)d.data() + o, n); say(s); run(step / 8);
            }
        }
        printf("%s", tail(b, seen).c_str()); fflush(stdout);
    }
    run(strtoull(argv[3], 0, 10));
    printf("%s\n", tail(b, seen).c_str());
    if (!drawn.empty()) {
        std::ofstream df(drawn, std::ios::binary);
        df.write((const char*)presented.data(), (std::streamsize)presented.size());
    }
    std::ofstream o(argv[2], std::ios::binary);
    for (auto& f : b.frames) o.write((const char*)f.data(), f.size());
    printf("frames total=%llu kept=%zu size=%zu\n", (unsigned long long)b.frame_total, b.frames.size(), b.frames.empty() ? 0 : b.frames.back().size());
    return 0;
}
