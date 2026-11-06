// Across a reboot, with the boot loader on the sign (its update slot): 1. boot image A, upload apps, power-cycle: the store is
// still there. 2. update over the console to image B, restart: the boot loader boots the slot; 1200 frames clear its tries.
// Usage: apprun_reboot IMAGE UPDATE.bin APP.mqap OUT.raw
#include "board.h"
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <memory>
#include <string>
#include <vector>
#include <algorithm>
#include <zlib.h>
using namespace sim;
static std::vector<uint8_t> readf(const std::string& p) { std::ifstream f(p, std::ios::binary); return std::vector<uint8_t>((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>()); }
static const std::string E = std::string(getenv("REPO") ? getenv("REPO") : ".") + "/dumps/serial/";
static std::unique_ptr<Board> mk(const std::vector<uint8_t>& img, const std::vector<uint8_t>* mf, const std::vector<uint8_t>* nv) {
    Scenario sc; sc.geom_word = 0xC240; sc.geom_w = 200; sc.geom_h = 16;
    auto b = std::make_unique<Board>(img, sc, std::vector<uint8_t>());
    if (mf) b->main_flash = *mf;
    if (nv) b->write(NVRAM, nv->data(), nv->size());
    b->fpga_model = true; b->frames_kept = 200;
    b->eeproms[0xA0] = readf(E + "eeprom_a0.bin"); b->eeproms[0xA2] = readf(E + "eeprom_a2.bin");
    if (!b->boot()) { printf("boot NO\n"); exit(1); }
    return b;
}
static size_t seen;
static void show(Board& b) { auto& t = b.serial_tx[0]; std::string o; for (size_t i = seen; i < t.size(); i++) if (t[i] == '\n' || (t[i] >= 32 && t[i] < 127)) o += (char)t[i]; seen = t.size(); printf("%s", o.c_str()); fflush(stdout); }
static void say(Board& b, const std::string& s) { b.serial_send(0, std::vector<uint8_t>(s.begin(), s.end()), 0, 0); }
static int lit(Board& b) { if (b.frames.empty()) return -1; int n = 0; auto& f = b.frames.back(); for (int i = 0; i < 16 * 480; i += 2) if ((f[i] & 0x7F) || f[i + 1]) n++; return n; }
int main(int, char** v) {
    auto img = readf(v[1]), upd = readf(v[2]), app = readf(v[3]);
    auto b = mk(img, 0, 0);
    uint64_t S = 60000000;
    auto run = [&](Board& x, uint64_t n) { x.run_app(x.icount + n); };
    run(*b, S); say(*b, "MQ?MON\r"); run(*b, S); say(*b, "time 6aeb4a14\r"); run(*b, S); say(*b, "sbegin\r"); run(*b, S / 4);
    char h[96]; snprintf(h, sizeof h, "sfile app0.mqap %zx\r", app.size()); say(*b, h); run(*b, S / 4);
    for (size_t o = 0; o < app.size(); o += 256) { size_t n = std::min<size_t>(256, app.size() - o); snprintf(h, sizeof h, "sraw %zx\r", n); std::string q = h; q.append((const char*)app.data() + o, n); say(*b, q); run(*b, S / 8); }
    say(*b, "scommit\r"); run(*b, S); say(*b, "boots\r"); run(*b, S); show(*b);
    printf("\n--- power cycle 1 ---\n");
    std::vector<uint8_t> nv(0x8000); b->read(NVRAM, nv.data(), nv.size());
    auto c = mk(img, &b->main_flash, &nv); seen = 0;
    run(*c, S * 12); say(*c, "MQ?MON\r"); run(*c, S); say(*c, "sls\r"); run(*c, S); say(*c, "apps\r"); run(*c, S); show(*c); printf("\nlit pixels in the last frame: %d\n", lit(*c));
    printf("\n--- update over the console ---\n");
    snprintf(h, sizeof h, "ubegin %zx %lx\r", upd.size(), (unsigned long)crc32(0, upd.data(), (unsigned)upd.size())); say(*c, h); run(*c, S / 4);
    for (size_t o = 0; o < upd.size(); o += 256) { size_t n = std::min<size_t>(256, upd.size() - o); snprintf(h, sizeof h, "uraw %zx\r", n); std::string q = h; q.append((const char*)upd.data() + o, n); say(*c, q); run(*c, S / 8); }
    say(*c, "ucommit\r"); run(*c, S * 4); { auto& t = c->serial_tx[0]; std::string o(t.end() - std::min<size_t>(t.size(), 40), t.end()); printf("tail: %s\n", o.find("restarting") != std::string::npos ? "updated: restarting" : "NO restart message"); }
    auto it = std::search(c->main_flash.begin(), c->main_flash.end(), upd.begin(), upd.end());
    printf("slot: %s\n", it == c->main_flash.end() ? "NOT written" : ("written at main flash +" + std::to_string(it - c->main_flash.begin())).c_str());
    printf("\n--- restart: the boot loader boots the slot ---\n");
    c->read(NVRAM, nv.data(), nv.size());
    auto d = mk(img, &c->main_flash, &nv); seen = 0; printf("before: NVRAM tries %02x flags %02x\n", nv[0x7FF0], nv[0x7FF1]); { uint8_t x=0,y=0; d->read(NVRAM+0x7FF0,&x,1); d->read(NVRAM+0x7FF1,&y,1); printf("right after the boot loader: tries %02x flags %02x\n",x,y); }
    run(*d, S * 12); say(*d, "MQ?MON\r"); run(*d, S); say(*d, "info\r"); run(*d, S); say(*d, "boots\r"); run(*d, S); say(*d, "sls\r"); run(*d, S); show(*d);
    printf("\nfirst-boot NVRAM tries %02x flags %02x\n", [&]{ uint8_t x=0; d->read(NVRAM+0x7FF0,&x,1); return x; }(), [&]{ uint8_t x=0; d->read(NVRAM+0x7FF1,&x,1); return x; }());
    run(*d, S * 30); uint8_t x = 0, y = 0; d->read(NVRAM + 0x7FF0, &x, 1); d->read(NVRAM + 0x7FF1, &y, 1);
    printf("after %llu frames: NVRAM tries %02x flags %02x, lit %d\n", (unsigned long long)d->frame_total, x, y, lit(*d));
    std::ofstream o(v[4], std::ios::binary); for (auto& f : d->frames) o.write((const char*)f.data(), f.size());
    return 0;
}
