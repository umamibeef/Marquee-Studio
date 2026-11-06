// UMFRAME delivery on the full firmware, on both lines: ping, time, feed, a USC1 schedule; another sign's frame and a bad
// CRC must get no answer. Usage: apprun_umf2 IMAGE CHAN OUT.raw
#include "board.h"
#include "umframe.h"
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <string>
#include <vector>
using namespace sim;
static std::vector<uint8_t> readf(const std::string& p) { std::ifstream f(p, std::ios::binary); return std::vector<uint8_t>((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>()); }
static void put16(std::vector<uint8_t>& v, uint16_t x) { v.push_back(x >> 8); v.push_back(x & 0xFF); }
static std::vector<uint8_t> mkapp(const std::string& t, uint8_t mode, uint16_t dwell, uint8_t font, uint16_t fill, uint8_t motion, uint8_t os, uint8_t oc, uint8_t bg) {
    std::vector<uint8_t> a; a.push_back(mode); put16(a, dwell); a.push_back(font); put16(a, fill); a.push_back(motion);
    a.push_back(os); a.push_back(oc); put16(a, 0xFFFF); put16(a, 0); a.push_back(bg); put16(a, t.size()); a.insert(a.end(), t.begin(), t.end()); return a;
}
int main(int argc, char** argv) {
    int chan = atoi(argv[2]);
    Scenario sc; sc.geom_word = 0xC240; sc.geom_w = 200; sc.geom_h = 16;
    Board b(readf(argv[1]), sc, std::vector<uint8_t>());
    b.fpga_model = true;
    const std::string R = std::string(getenv("REPO") ? getenv("REPO") : ".") + "/dumps/serial/";
    b.eeproms[0xA0] = readf(R + "eeprom_a0.bin"); b.eeproms[0xA2] = readf(R + "eeprom_a2.bin");
    if (!b.boot()) { printf("boot NO\n"); return 1; }
    b.frames_kept = 3000;
    b.run_app(b.icount + 60000000);
    uint16_t seq = 1;
    auto send = [&](uint8_t dest, uint8_t type, const std::vector<uint8_t>& pl, bool bad = false) {
        uint8_t w[UMF_DECMAX + 32]; int n = umf_build(dest, 2, type, seq++, pl.empty() ? nullptr : pl.data(), (int)pl.size(), w, sizeof w);
        if (bad) w[3] ^= 0x55;
        b.serial_send(chan, std::vector<uint8_t>(w, w + n), 0, 0); b.run_app(b.icount + 20000000);
    };
    send(1, UMF_PING, {});
    send(9, UMF_PING, {});              // another sign's
    send(1, UMF_PING, {}, true);        // bad CRC
    send(0xFF, UMF_FEED, {'x',0,'y'});  // broadcast: applied, unanswered
    send(1, UMF_SET_TIME, {0x6A,0xEB,0x4A,0x14});
    send(1, UMF_FEED, {'t','e','m','p',0,'2','1','C'});
    std::vector<uint8_t> u = {'U','S','C','1'}; put16(u, 1); put16(u, 0); put16(u, 2); put16(u, 0); put16(u, 0); put16(u, 0);
    auto a1 = mkapp("TEMP {temp}", 0, 20, 0xFF, 0x7FE0, 1, 0xFF, 0, 0xFF), a2 = mkapp("SCROLLING DELIVERED TEXT", 1, 0, 0xFF, 0x03E0, 0xFF, 3, 7, 0xFF);
    u.insert(u.end(), a1.begin(), a1.end()); u.insert(u.end(), a2.begin(), a2.end());
    send(1, UMF_SCHED_BEGIN, {}); send(1, UMF_SCHED_CHUNK, u); send(1, UMF_SCHED_COMMIT, {});
    b.frames.clear();
    b.run_app(b.icount + 900000000);
    int ack = 0, pong = 0, nak = 0; auto& tx = b.serial_tx[chan];
    for (size_t i = 0; i < tx.size(); i++) { if (tx[i] != UMF_SOF) continue; size_t j = i + 1; while (j < tx.size() && tx[j]) j++; if (j >= tx.size()) break;
        struct umf f; uint8_t d[UMF_DECMAX]; if (umf_parse(&tx[i + 1], (int)(j - i - 1), &f, d, sizeof d) >= 0) { if (f.type == UMF_ACKF) ack++; else if (f.type == UMF_PONG) pong++; else if (f.type == UMF_NAK) nak++; } i = j; }
    printf("chan %d: pong %d (want 1) ack %d (want 5) nak %d (want 0) frames %zu -> %s\n", chan, pong, ack, nak, b.frames.size(), pong == 1 && ack == 5 && !nak ? "PASS" : "FAIL");
    std::ofstream o(argv[3], std::ios::binary); for (auto& f : b.frames) o.write((const char*)f.data(), f.size());
    return 0;
}
