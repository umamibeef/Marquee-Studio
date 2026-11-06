#include "board.h"

#include "symbols.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <stdexcept>

namespace sim {

const char KEYMAP[] = "1234567890 .ABCD}{][-!EF\r:?<>,GHIJKLMNOPQRSTUVWXYZabcdefghijklmn";

static uint32_t be32(const uint8_t* p) { return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3]; }
static void put32(uint8_t* p, uint32_t v) { p[0] = v >> 24; p[1] = v >> 16; p[2] = v >> 8; p[3] = v; }
static void put16(uint8_t* p, uint16_t v) { p[0] = v >> 8; p[1] = (uint8_t)v; }

// ---- EEPROM contents (cosim.eeprom_cfg / eeprom_a2) and the display geometry (appsim.set_geometry) ----
std::vector<uint8_t> eeprom_cfg(bool spd) {
    std::vector<uint8_t> c(256, 0);
    put32(&c[0x00], 0x00C1A042);                           // board id
    if (spd) {
        c[4] = 1, c[5] = 0x10;
        put32(&c[0x10], 0x20000000);                       // NVRAM size index 1 (512 KB)
    }
    put16(&c[0x06], 0x0004);                               // SCCR bits
    put32(&c[0x08], 50000000);                             // wanted sysclk
    put32(&c[0x0C], 10000000);                             // oscillator
    put16(&c[0x1A], 0x0E00);                               // SDRAM OR low half
    put32(&c[0x1C], 0xFFF80D30);                           // OR0
    put32(&c[0x20], 0xFFFF8940);                           // OR1: 32 KB NVRAM
    put32(&c[0x24], 0xFF800D34);                           // OR4: 8 MB flash bank 1
    put32(&c[0x28], 0xFFC00D34);                           // OR5: 4 MB flash bank 2
    put32(&c[0x2C], 0x12340105);                           // MCR command words
    put16(&c[0x30], 0x0088);                               // MAR
    put16(&c[0x32], 0x0106);
    c[0x34] = 0x04, c[0x35] = 1;                           // MPTPR, 128 MB
    put16(&c[0x36], 0x1312), put16(&c[0x38], 0x4111), put16(&c[0x3A], 0x9C12), put16(&c[0x3C], 0x4111);
    unsigned s = 0, x = 0;
    for (int i = 0; i < 62; i++)
        s += c[i], x ^= c[i];
    c[0x3E] = s & 0xFF, c[0x3F] = (uint8_t)x;
    return c;
}

std::vector<uint8_t> eeprom_a2(bool spd) {
    std::vector<uint8_t> s(256, 0);
    if (!spd) {
        for (int i = 0; i < 64; i++)
            put32(&s[4 * i], 0x0F0FC000u ^ (uint32_t)(i * 0x01010101u));
        return s;
    }
    s[2] = 4, s[3] = 13, s[4] = 9, s[5] = 1;               // SDRAM, 13 rows, 9 cols, 1 bank
    s[12] = 0x80, s[17] = 4, s[31] = 0x20;                 // refresh code 0, 4 banks, 128 MB
    unsigned sum = 0;
    for (int i = 0; i < 63; i++)
        sum += s[i];
    s[63] = sum & 0xFF;
    return s;
}

void set_geometry(std::vector<uint8_t>& e, uint16_t word, int w, int h) {
    e[0x13] = (uint8_t)(((w - 20) / 20) << 4 | (h - 16) / 16);
    put16(&e[0x40], word);
    for (auto [lo, hi] : {std::pair{0x00, 0x3E}, std::pair{0x40, 0x7E}}) {
        unsigned s = 0, x = 0;
        for (int i = lo; i < hi; i++)
            s = (s + e[i]) & 0xFF, x ^= e[i];
        e[hi] = (uint8_t)s, e[hi + 1] = (uint8_t)x;
    }
}

// ---- Unicorn callbacks ----
static uint64_t cb_main_read(uc_engine*, uint64_t off, unsigned size, void* ud) {
    return static_cast<Board*>(ud)->main_read(off, size);
}
static void cb_main_write(uc_engine*, uint64_t off, unsigned size, uint64_t value, void* ud) {
    static_cast<Board*>(ud)->main_write(off, size, value);
}
static uint64_t cb_immr_read(uc_engine*, uint64_t off, unsigned size, void* ud) {
    return static_cast<Board*>(ud)->immr_read(off, size);
}
static void cb_immr_write(uc_engine*, uint64_t off, unsigned size, uint64_t value, void* ud) {
    static_cast<Board*>(ud)->immr_write(off, size, value);
}
// Code our boot loader copies into SDRAM at run time (a stage 1 at 0xF00000, bootloader/include/serdump.h): its
// special instructions are found as they run, not by scanning the boot ROM.
static void cb_special_ram(uc_engine* uc, uint64_t a, uint32_t, void* ud) {
    uint8_t q[4];
    if (uc_mem_read(uc, a, q, 4) != UC_ERR_OK)
        return;
    uint32_t w = (uint32_t)q[0] << 24 | q[1] << 16 | q[2] << 8 | q[3];
    if (w >> 26 != 31)
        return;
    uint32_t xo = (w >> 1) & 0x3FF, sp = ((w >> 16) & 0x1F) | (((w >> 11) & 0x1F) << 5);
    if ((xo == 467 || xo == 339) && (sp == 1 || sp == 8 || sp == 9))
        return;
    if (xo == 467 || xo == 339 || xo == 146 || xo == 83 || xo == 370)
        static_cast<Board*>(ud)->special_boot(a);
}
static void cb_special_app(uc_engine*, uint64_t a, uint32_t, void* ud) { static_cast<Board*>(ud)->special_app(a); }
static void cb_rfi(uc_engine*, uint64_t a, uint32_t, void* ud) { static_cast<Board*>(ud)->rfi(a); }
static void cb_block(uc_engine*, uint64_t a, uint32_t size, void* ud) { static_cast<Board*>(ud)->block(a, size); }
static void cb_done(uc_engine*, uint64_t a, uint32_t, void* ud) { static_cast<Board*>(ud)->done(a); }
static void cb_busy(uc_engine*, uint64_t a, uint32_t, void* ud) { static_cast<Board*>(ud)->busy_wait(a); }
static void cb_switch(uc_engine*, uint64_t a, uint32_t, void* ud) { static_cast<Board*>(ud)->on_switch(a); }
static void cb_fpga(uc_engine*, uint64_t a, uint32_t, void* ud) { static_cast<Board*>(ud)->on_fpga_call(a); }

static void check(uc_err e, const char* what) {
    if (e != UC_ERR_OK)
        throw std::runtime_error(std::string(what) + ": " + uc_strerror(e));
}

// ---- construction: cosim.Board, then AppBoard ----
Board::Board(const std::vector<uint8_t>& flash, Scenario scenario, std::vector<uint8_t> app_image)
    : sc(scenario), app(std::move(app_image)) {
    eeproms[0xA0] = eeprom_cfg(sc.spd);
    eeproms[0xA2] = eeprom_a2(sc.spd);
    dpram.assign(0x10000, 0);
    check(uc_open(UC_ARCH_PPC, (uc_mode)(UC_MODE_PPC32 | UC_MODE_BIG_ENDIAN), &uc), "uc_open");
    check(uc_mem_map(uc, 0, RAM_SIZE, UC_PROT_ALL), "map RAM");
    check(uc_mem_map(uc, FLASH, 0x80000, UC_PROT_ALL), "map flash");
    check(uc_mem_write(uc, FLASH, flash.data(), std::min<size_t>(flash.size(), 0x80000)), "write flash");
    check(uc_mem_map(uc, NVRAM, 0x80000, UC_PROT_ALL), "map NVRAM");
    std::vector<uint8_t> nv(0x80000, 0);
    nv[0x7FF0] = sc.tries, nv[0x7FF1] = 0;
    check(uc_mem_write(uc, NVRAM, nv.data(), nv.size()), "write NVRAM");
    check(uc_mmio_map(uc, MAIN, 0x2000000, cb_main_read, this, cb_main_write, this), "map main flash");
    check(uc_mem_map(uc, CS7, 0x200000, UC_PROT_ALL), "map CS7");
    check(uc_mmio_map(uc, IMMR, 0x10000, cb_immr_read, this, cb_immr_write, this), "map IMMR");
    // The boot ROM's special instructions (mtspr/mfspr/mtmsr/mfmsr/tlbwe): found AS THEY RUN, image-agnostically,
    // not by pre-scanning the stock flash. Pre-scanning installed a per-address hook for every match, which (a)
    // assumed the stock image's layout and (b) perturbed Unicorn's translation so an arbitrary from-source image
    // would not cold-boot. One range hook over the flash and the RAM stage handles any image the same way.
    add_code_hook((void*)cb_special_ram, FLASH, FLASH + 0x80000);
    add_code_hook((void*)cb_special_ram, 0xF00000, 0xF0FFFF);
    add_code_hook((void*)cb_done, 0, 0);
    // AppBoard
    set_geometry(eeproms[0xA0], sc.geom_word, sc.geom_w, sc.geom_h);
    // RAM above 16 MB. The real board's SDRAM banks sit at scattered physical addresses that the MMU maps back to back
    // from virtual 0 (bootloader serdump.c stage_where); this model has no MMU and maps RAM flat. Our boot loader's
    // RAM stage (a run from RAM, a slot boot) is written at its physical address, 0x0EF80000, and the firmware reads
    // it as its boot volume at the virtual one, 0x07F80000 (the last 512 KB of its RAM): both are one buffer here.
    check(uc_mem_map(uc, RAM_SIZE, STAGE_VIRT - RAM_SIZE, UC_PROT_ALL), "map RAM above 16 MB");
    stage_ram.assign(STAGE_BYTES, 0);
    check(uc_mem_map_ptr(uc, STAGE_VIRT, STAGE_BYTES, UC_PROT_ALL, stage_ram.data()), "map the RAM stage (virtual)");
    check(uc_mem_map(uc, 0x08000000, STAGE_PHYS - 0x08000000, UC_PROT_ALL), "map SDRAM's second 128 MB");
    check(uc_mem_map_ptr(uc, STAGE_PHYS, STAGE_BYTES, UC_PROT_ALL, stage_ram.data()), "map the RAM stage (physical)");
    check(uc_mem_map(uc, STAGE_PHYS + STAGE_BYTES, 0x10000000 - STAGE_PHYS - STAGE_BYTES, UC_PROT_ALL),
          "map SDRAM's top");
}

Board::~Board() {
    if (uc)
        uc_close(uc);
    for (auto* f : forces)
        delete f;
    delete write_watch;
}

void Board::add_code_hook(void* cb, uint64_t begin, uint64_t end) {
    uc_hook h;
    check(uc_hook_add(uc, &h, UC_HOOK_CODE, cb, this, begin, end), "hook");
    hooks.push_back(h);
}

// ---- registers and memory ----
uint32_t Board::reg(int r) const {
    uint64_t v = 0;
    uc_reg_read(uc, r, &v);
    return (uint32_t)v;
}
void Board::set_reg(int r, uint32_t v) {
    uint64_t x = v;
    uc_reg_write(uc, r, &x);
}
uint32_t Board::pc() const { return reg(UC_PPC_REG_PC); }
uint32_t Board::r32(uint32_t a) const {
    uint8_t b[4] = {0};
    uc_mem_read(uc, a, b, 4);
    return be32(b);
}
bool Board::read(uint32_t a, void* out, size_t n) const { return uc_mem_read(uc, a, out, n) == UC_ERR_OK; }
bool Board::write(uint32_t a, const void* in, size_t n) { return uc_mem_write(uc, a, in, n) == UC_ERR_OK; }
// Patches code: also drops any translated block over it, or a block already cached there keeps running the old code.
bool Board::write_code(uint32_t a, const void* in, size_t n) {
    bool ok = uc_mem_write(uc, a, in, n) == UC_ERR_OK;
    uc_ctl_remove_cache(uc, a, a + n);
    return ok;
}

uint32_t Board::dp32(uint32_t off) const { return be32(&dpram[off]); }
void Board::dp32(uint32_t off, uint32_t v) { put32(&dpram[off], v); }
uint16_t Board::dp16(uint32_t off) const { return (uint16_t)(dpram[off] << 8 | dpram[off + 1]); }
void Board::dp16(uint32_t off, uint16_t v) { put16(&dpram[off], v); }

std::string task_name(const Board& b, uint32_t tcb) {
    uint8_t raw[4] = {0};
    b.read(tcb + 0xC, raw, 4);
    bool printable = std::all_of(raw, raw + 4, [](uint8_t c) { return c >= 32 && c < 127; });
    if (printable)
        return std::string((const char*)raw, 4);
    char hex[9];
    snprintf(hex, sizeof hex, "%02x%02x%02x%02x", raw[0], raw[1], raw[2], raw[3]);
    return hex;
}

// ---- boot ----
void Board::done(uint64_t) {
    // the jump to the app (PC 0) ends the boot; afterwards address 0 is just app code
    if (!reached_app || !in_app) {
        reached_app = true;
        uc_emu_stop(uc);
    }
}

bool Board::boot() {
    reached_app = false;
    uc_err e = uc_emu_start(uc, FLASH + 0x100, 0xFFFFFFFF, 0, BOOT_MAX);
    if (e != UC_ERR_OK)
        fprintf(stderr, "boot: emulation error %s at pc=0x%x\n", uc_strerror(e), pc());
    return reached_app;
}

void Board::load_app_image() {
    std::vector<uint8_t> ram(app.size());
    read(0, ram.data(), ram.size());
    uint32_t tp = be32(&ram[0x4C]), tl = be32(&ram[0x50]);
    std::vector<uint8_t> img = app;
    std::copy(ram.begin() + 0x14, ram.begin() + 0x9C, img.begin() + 0x14);
    if (tp + tl <= img.size())
        std::copy(ram.begin() + tp, ram.begin() + tp + tl, img.begin() + tp);
    write(0, img.data(), img.size());
}

// ---- the IMMR model: cosim's, plus the SIU interrupt controller, the CPM's and the PIT ----
int Board::cpm_highest() const {
    uint32_t pend = dp32(0x944) & dp32(0x948) & ~dp32(0x94C);
    for (int bit = 0; bit < 32; bit++)
        if (pend & (0x80000000u >> bit))
            return bit;
    return -1;
}

int Board::sivec() const {
    uint32_t pend = dp32(0x10) & dp32(0x14);
    for (int bit = 0; bit < 16; bit++)                     // SIPEND bit 0 (MSB) = IRQ0, 1 = LVL0, 2 = IRQ1, ...
        if (pend & (0x80000000u >> bit))
            return bit * 4;
    return 0x3C;
}

uint64_t Board::immr_read(uint64_t off, unsigned size) {
    if (off == 0x99E && size == 2) {                       // TCN2: our boot loader's windows time themselves by it
        tcn2 += 4;                                         // (cosim/serdumpsim: 20 us per read at 200 ticks/ms)
        return tcn2 & 0xFFFF;
    }
    if (off == 0x1C) {                                     // SIVEC: the highest-priority pending, enabled source
        uint64_t code = (uint64_t)sivec();
        return size == 4 ? code << 24 : (size == 2 ? code << 8 : code);
    }
    if (off == 0x930) {                                    // CIVR: vector of the highest pending CPM source (IACK)
        int bit = cpm_highest();
        uint64_t vn = 0;
        if (bit >= 0) {
            vn = 31 - bit;
            dp32(0x94C, dp32(0x94C) | (0x80000000u >> bit));   // in service
            dp32(0x944, dp32(0x944) & ~(0x80000000u >> bit));  // taken
        }
        uint64_t v = vn << 11;                             // VN in the top 5 bits of the 16-bit register
        return size == 4 ? v << 16 : (size == 1 ? v >> 8 : v);
    }
    auto base = [&]() -> uint64_t {                        // cosim's model
        if (off == 0x9B0)                                  // TER1: reference reached
            return 0x0002;
        if (off == 0x9C0)                                  // CPCR: command done
            return 0;
        if (off == 0x0F0 && size == 1)                     // PIPR strap byte
            return sc.pipr;
        if (off == 0xAC4)
            return sc.pbdat;
        if (off == 0x976)
            return sc.pddat;
        uint64_t v = 0;
        for (unsigned i = 0; i < size && off + i < dpram.size(); i++)
            v = v << 8 | dpram[off + i];
        return v;
    };
    if (fpga_model) {                                      // fake_fpga(): INIT high (PB17), DONE once loaded (PIPR)
        if (off == 0xAC6 && size == 2)
            return base() | 0x4000;
        if (off == 0xAC4 && size == 4)
            return base() | 0x4000;
        if (off == 0x0F0 && size == 4)
            return (base() & ~(uint64_t)0x02000000) | (fpga_done ? 0x02000000 : 0);
    }
    if (off == 0x964 || off == 0x966) {                    // PCDAT: PC13 is the IR receiver (input)
        uint64_t v = base();
        if ((off == 0x966 || size == 4) && (size == 2 || size == 4))
            v = (v & ~(uint64_t)0x0004) | (ir_level ? 0x0004 : 0);
        return v;
    }
    return base();
}

void Board::immr_write(uint64_t off, unsigned size, uint64_t value) {
    if (off == 0x00E && size == 2) {                       // SWSR: 0x556C then 0xAA39 services the watchdog
        if (value == 0x556C)
            swsr_armed = true;
        else if (value == 0xAA39 && swsr_armed) {
            swsr_armed = false;
            wdt_last = now();
            events["watchdog services"]++;
        } else
            swsr_armed = false;
    }
    if (off == 0x284 && size == 4 && (value & 0x80) && in_app) {   // PLPRCR CSR: the firmware resets the CPU
        events["cpu reset requested"]++;
        reset_requested = true;
        uc_emu_stop(uc);                                   // the board resets now (Machine: a warm restart)
    }
    if (off == 0x94C && size == 4) {                       // CISR: writing 1 ends that source's service
        dp32(0x94C, dp32(0x94C) & ~(uint32_t)value);
        return;
    }
    if ((off == 0xA10 || off == 0xA30 || off == 0xA50 || off == 0xA70) && size == 2) {   // SCCE1..4: write 1 clears
        dp16((uint32_t)off, dp16((uint32_t)off) & ~(uint16_t)value);
        return;
    }
    if ((off == 0xA86 || off == 0xA96) && size == 1) {     // SMCE1..2
        dpram[off] &= ~(uint8_t)value;
        return;
    }
    if (off == 0x240 && size == 2 && (value & 0x80))       // PISCR: writing PS = 1 acknowledges the tick
        value &= ~(uint64_t)0x80;
    for (unsigned i = 0; i < size && off + i < dpram.size(); i++)
        dpram[off + i] = (uint8_t)(value >> (8 * (size - 1 - i)));
    if (!in_app && off < 0x2000)
        reg_writes++;
    if (off == 0x86C && value == 0x81) {                   // I2COM: start
        if (in_app)
            i2c_app();
        else
            i2c_boot();
    }
}

void Board::i2c_boot() {                                   // cosim: reads only, from fixed BD addresses
    uint8_t* d = dpram.data();
    if ((be32(d + 0x2418) >> 16) != 0xAC00)
        return;
    int dev = d[0x2420] & 0xFE;
    int n = (int)(be32(d + 0x2418) & 0xFFFF) - 1;
    uint32_t buf = be32(d + 0x240C);
    std::vector<uint8_t> src = eeproms.count(dev) ? eeproms[dev] : std::vector<uint8_t>(256, 0xFF);
    size_t len = n >= 0 ? std::min<size_t>((size_t)n, src.size()) : (size_t)std::max<int>(0, (int)src.size() + n);
    if (buf >= IMMR && buf < IMMR + 0x10000)
        std::copy(src.begin(), src.begin() + std::min<size_t>(len, 0x10000 - (buf - IMMR)), dpram.begin() + (buf - IMMR));
    else
        write(buf, src.data(), len);
    dp32(0x2408, dp32(0x2408) & ~0x80000000u);             // clear E
}

void Board::i2c_app() {
    // CPM I2C master, as the hardware does it: the BD tables come from the parameter RAM (RBASE 0x3C80, TBASE 0x3C82).
    // Write-then-read: TX BD0 = [dev W, offset], TX BD1 = [dev R, n dummy bytes] -> n EEPROM bytes into the RX BD
    // buffer. A write-only transfer stores its bytes in the (in-memory) EEPROM. Then R/E bits clear.
    uint32_t rbase = dp16(0x3C80), tbase = dp16(0x3C82);
    struct Tx { uint32_t o; uint16_t st, ln; uint32_t buf; };
    std::vector<Tx> txs;
    uint32_t o = tbase;
    for (int k = 0; k < 8; k++) {
        uint16_t st = dp16(o), ln = dp16(o + 2);
        uint32_t buf = dp32(o + 4);
        if (!(st & 0x8000))
            break;
        txs.push_back({o, st, ln, buf});
        if (st & 0x2000)                                   // W: last BD of the table
            break;
        o += 8;
    }
    if (txs.empty())
        return;
    auto rd = [&](uint32_t a, size_t n) {
        std::vector<uint8_t> v(n);
        if (a >= IMMR && a < IMMR + 0x10000)
            for (size_t i = 0; i < n && a - IMMR + i < dpram.size(); i++)
                v[i] = dpram[a - IMMR + i];
        else
            read(a, v.data(), n);
        return v;
    };
    std::vector<uint8_t> first = rd(txs[0].buf, txs[0].ln);
    int dev = first.empty() ? 0 : first[0] & 0xFE;
    auto& ee = eeproms.try_emplace(dev, std::vector<uint8_t>(256, 0xFF)).first->second;
    if (txs.size() >= 2 && (rd(txs[1].buf, 1)[0] & 1)) {   // write offset, then read
        int off = first.size() > 1 ? first[1] : 0;
        int n = txs[1].ln - 1;
        std::vector<uint8_t> data(n > 0 ? n : 0);
        for (int i = 0; i < n; i++)
            data[i] = ee[(off + i) & 0xFF];
        uint32_t r = rbase;
        uint32_t rbuf = dp32(r + 4);
        if (rbuf >= IMMR && rbuf < IMMR + 0x10000)
            std::copy(data.begin(), data.end(), dpram.begin() + (rbuf - IMMR));
        else
            write(rbuf, data.data(), data.size());
        dp16(r + 2, (uint16_t)data.size());
        dp16(r, dp16(r) & ~0x8000);
    } else if (!first.empty() && !(first[0] & 1) && first.size() > 1) {   // a write: [dev W, offset, data...]
        int off = first[1];
        for (size_t i = 2; i < first.size(); i++)
            ee[(off + i - 2) & 0xFF] = first[i];
        events["eeprom writes"]++;
    }
    for (auto& t : txs)
        dp16(t.o, t.st & ~0x8000);
}

// ---- special instructions ----
void Board::special_boot(uint64_t addr) {
    uint32_t w = r32((uint32_t)addr);
    uint32_t xo = (w >> 1) & 0x3FF, rs = (w >> 21) & 0x1F, sp = ((w >> 16) & 0x1F) | (((w >> 11) & 0x1F) << 5);
    int r = UC_PPC_REG_0 + (int)rs;
    if (xo == 467)                                         // mtspr
        spr[sp] = reg(r);
    else if (xo == 339) {                                  // mfspr
        auto it = spr.find(sp);
        set_reg(r, sp == 568 ? 0 : (it == spr.end() ? 0 : it->second));
    } else if (xo == 146)                                  // mtmsr
        msr = reg(r);
    else if (xo == 83)                                     // mfmsr
        set_reg(r, (uint32_t)msr);
    set_reg(UC_PPC_REG_PC, (uint32_t)addr + 4);
}

int Board::hook_app_specials() {
    int n = 0;
    for (size_t i = 0; i + 3 < std::min<size_t>(app.size(), CODE_END); i += 4) {
        uint32_t w = be32(&app[i]);
        uint32_t op = w >> 26, xo = (w >> 1) & 0x3FF, sp = ((w >> 16) & 0x1F) | (((w >> 11) & 0x1F) << 5);
        if (op == 19 && xo == 50) {                        // rfi
            add_code_hook((void*)cb_rfi, i, i);
            n++;
        } else if (op == 31 && (xo == 146 || xo == 83 || xo == 370 || xo == 371 ||
                                ((xo == 467 || xo == 339) && sp != 1 && sp != 8 && sp != 9))) {
            add_code_hook((void*)cb_special_app, i, i);
            n++;
        }
    }
    return n;
}

void Board::special_app(uint64_t addr) {
    uint32_t w = r32((uint32_t)addr);
    uint32_t op = w >> 26, xo = (w >> 1) & 0x3FF, rs = (w >> 21) & 0x1F;
    uint32_t sp = ((w >> 16) & 0x1F) | (((w >> 11) & 0x1F) << 5);
    int r = UC_PPC_REG_0 + (int)rs;
    if (op == 31 && xo == 83)                              // mfmsr
        set_reg(r, (uint32_t)msr);
    else if (op == 31 && xo == 146)                        // mtmsr
        msr = reg(r);
    else if (xo == 467 && sp == SPR_EIE)
        msr |= EE | RI;
    else if (xo == 467 && sp == SPR_EID)
        msr = (msr & ~EE) | RI;
    else if (xo == 467 && sp == SPR_NRI)
        msr &= ~(EE | RI);
    else if (xo == 467)                                    // mtspr
        spr[sp] = reg(r);
    else if (xo == 339 || xo == 371) {                     // mfspr / mftb
        uint64_t t = now() / CPU_MIPS;
        if (sp == SPR_TBL_R || sp == 284)
            set_reg(r, addr >= IR_ISR_LO && addr < IR_ISR_HI ? (uint32_t)ir_tb : (uint32_t)t);
        else if (sp == SPR_TBU_R || sp == 285)
            set_reg(r, (uint32_t)(t >> 32));
        else if (sp == 568)
            set_reg(r, 0);
        else {
            auto it = spr.find(sp);
            set_reg(r, it == spr.end() ? 0 : it->second);
        }
    }                                                      // tlbia: nothing
    set_reg(UC_PPC_REG_PC, (uint32_t)addr + 4);
    skip_rest(addr);
}

void Board::rfi(uint64_t addr) {
    skip_rest(addr);
    auto get = [&](int s) -> uint32_t { auto it = spr.find(s); return it == spr.end() ? 0 : it->second; };
    msr = get(SPR_SRR1);
    uint32_t ret = get(SPR_SRR0);
    // as on the CPU: an interrupt still pending when a handler returns is taken at once. Without this, only one
    // interrupt per slice gets in, and the lowest-priority CPM sources (SMC1) starve behind the PIT and IDMA.
    levels();
    if ((dp32(0x10) & dp32(0x14)) && (msr & EE)) {
        spr[SPR_SRR0] = ret;
        spr[SPR_SRR1] = (uint32_t)msr;
        msr &= ~(EE | PR | RI);
        ret = 0x500;
        events["external interrupts"]++;
        tr(TR_IRQ);
    }
    set_reg(UC_PPC_REG_PC, ret);
}

void Board::hook_busy_waits(const Symbols& S) {
    // udelay(us) / delay_us(us) only pass time: skip them and advance the clock (same observable effect)
    for (const char* n : {"udelay", "delay_us_maybe"})
        if (auto a = S.find(n))
            add_code_hook((void*)cb_busy, *a, *a);
}

// The panel FPGA, for boards whose firmware loads it (board type 2, as the real sign): analysis/tools/test_fpga.py's
// FakeFPGA. INIT (PB17) reads high, spi_transfer takes the bitstream and answers 0xFF, DONE (PIPR 0x02000000) is set
// from then on; PROG (PA4) low clears it.
void Board::fake_fpga(const Symbols& S) {
    fpga_model = true;
    for (const char* n : {"spi_lock", "spi_unlock", "spi_set_speed", "spi_transfer", "spi_done"})
        if (auto a = S.find(n)) {
            fpga_calls[*a] = n;
            add_code_hook((void*)cb_fpga, *a, *a);
        }
}

void Board::on_fpga_call(uint64_t addr) {
    const std::string& n = fpga_calls[(uint32_t)addr];
    uint32_t ret = 0;
    if (n == "spi_transfer") {
        uint32_t rx = reg(UC_PPC_REG_4), len = reg(UC_PPC_REG_5);
        std::vector<uint8_t> ff(len, 0xFF);
        if (rx && len)
            write(rx, ff.data(), len);
        fpga_len = len;
        fpga_done = true;
        events["fpga loads"]++;
    } else if (n == "spi_done") {
        uint32_t p = reg(UC_PPC_REG_3), be = __builtin_bswap32(fpga_len);
        if (p)
            write(p, &be, 4);
        ret = 1;
    }
    set_reg(UC_PPC_REG_3, ret);
    set_reg(UC_PPC_REG_PC, reg(UC_PPC_REG_LR));
    skip_rest(addr);
}

void Board::busy_wait(uint64_t addr) {
    skew += (uint64_t)reg(UC_PPC_REG_3) * CPU_MIPS;
    events["busy-waits skipped"]++;
    set_reg(UC_PPC_REG_PC, reg(UC_PPC_REG_LR));
    skip_rest(addr);
}

void Board::watch_switches(const Symbols& S) {
    if (auto a = S.find("cpu_context_switch"))
        add_code_hook((void*)cb_switch, *a, *a);
}

void Board::on_switch(uint64_t) {
    uint32_t tcb = reg(UC_PPC_REG_4) - 0xE0;               // the heir's context is TCB + 0xE0
    std::string name = task_name(*this, tcb);
    tcbs[name] = tcb;
    tasks[name]++;
    if (trace_on) {
        auto it = std::find(trace_names.begin(), trace_names.end(), name);
        if (it == trace_names.end())
            it = trace_names.insert(trace_names.end(), name);
        tr(TR_TASK, 0, (uint16_t)(it - trace_names.begin()));
    }
    if (switches.size() < 40)
        switches.emplace_back(icount, name);
}

// ---- configuration and input hooks ----
static void cb_force(uc_engine* uc, uc_mem_type, uint64_t, int, int64_t, void* ud) {
    struct F { uint32_t addr; std::vector<uint8_t> data; Board* b; };
    auto* f = static_cast<F*>(ud);
    uc_mem_write(uc, f->addr, f->data.data(), f->data.size());
}

void Board::force_config_bytes(uint32_t addr, std::vector<uint8_t> data) {
    // every app read of these bytes returns them: what a sign configured in its NVRAM would have
    auto* f = new Force{addr, std::move(data), this};
    forces.push_back(f);
    write(addr, f->data.data(), f->data.size());
    uc_hook h;
    check(uc_hook_add(uc, &h, UC_HOOK_MEM_READ, (void*)cb_force, f, addr, addr + f->data.size() - 1), "hook");
    hooks.push_back(h);
}

void Board::force_config_word(uint32_t addr, uint32_t value) {
    std::vector<uint8_t> b(4);
    put32(b.data(), value);
    force_config_bytes(addr, b);
}

static void cb_write_watch(uc_engine*, uc_mem_type, uint64_t, int, int64_t value, void* ud) {
    struct W { Board* b; };
    Board* b = static_cast<W*>(ud)->b;
    uint32_t v = (uint32_t)value;
    if (b->states.empty() || b->states.back() != v)
        b->states.push_back(v);
}

void Board::watch_writes(uint32_t addr, uint32_t len) {
    write_watch = new WriteWatch{this};
    uc_hook h;
    check(uc_hook_add(uc, &h, UC_HOOK_MEM_WRITE, (void*)cb_write_watch, write_watch, addr, addr + len - 1), "hook");
    hooks.push_back(h);
}

static void cb_exec(uc_engine*, uint64_t, uint32_t, void* ud) {
    struct E { Board* b; std::function<void(Board&)> fn; };
    auto* e = static_cast<E*>(ud);
    e->fn(*e->b);
}

void Board::on_exec(uint32_t addr, std::function<void(Board&)> fn) {
    exec_hooks.push_back(std::make_unique<ExecHook>(ExecHook{this, std::move(fn)}));
    uc_hook h;
    check(uc_hook_add(uc, &h, UC_HOOK_CODE, (void*)cb_exec, exec_hooks.back().get(), addr, addr), "hook");
    hooks.push_back(h);
    uc_ctl_remove_cache(uc, addr, addr + 4);               // a block already translated there would skip the hook
}

// ---- calling firmware functions (tests) ----
static void cb_call_return(uc_engine* uc, uint64_t, uint32_t, void*) { uc_emu_stop(uc); }

uint32_t Board::call(uint32_t addr, const std::vector<uint32_t>& args, uint32_t sp, uint64_t max_insns) {
    if (!call_hooked) {
        add_code_hook((void*)cb_call_return, CALL_RETURN, CALL_RETURN);
        call_hooked = true;
    }
    std::vector<int> regs;
    for (int i = 0; i < 32; i++)
        regs.push_back(UC_PPC_REG_0 + i);
    for (int r : {UC_PPC_REG_LR, UC_PPC_REG_CR, UC_PPC_REG_CTR, UC_PPC_REG_XER, UC_PPC_REG_PC})
        regs.push_back(r);
    std::vector<uint32_t> saved;
    for (int r : regs)
        saved.push_back(reg(r));
    Count saved_cnt = cnt;
    for (size_t i = 0; i < args.size() && i < 8; i++)
        set_reg(UC_PPC_REG_3 + (int)i, args[i]);
    for (size_t i = 8; i < args.size(); i++) {
        uint8_t w[4];
        put32(w, args[i]);
        write(sp + 8 + 4 * (uint32_t)(i - 8), w, 4);
    }
    set_reg(UC_PPC_REG_1, sp);
    set_reg(UC_PPC_REG_LR, CALL_RETURN);
    uc_err e = uc_emu_start(uc, addr, CALL_RETURN, 0, max_insns);
    uint32_t at = pc();
    if (e != UC_ERR_OK || at != CALL_RETURN) {
        char msg[160];
        snprintf(msg, sizeof msg, "call 0x%x failed at pc 0x%x (%08x): %s", addr, at, r32(at),
                 e != UC_ERR_OK ? uc_strerror(e) : "did not return");
        throw std::runtime_error(msg);
    }
    uint32_t r3 = reg(UC_PPC_REG_3);
    for (size_t i = 0; i < regs.size(); i++)
        set_reg(regs[i], saved[i]);
    cnt = saved_cnt;
    return r3;
}

void Board::serial_send(int minor, std::vector<uint8_t> data, uint64_t at, uint64_t at_frame) {
    // delivered when the app has the channel open with an empty rx BD, one BD per call (a UART closes the buffer on
    // the idle line after a frame)
    rx_queue.push_back({at, at_frame, minor, std::move(data)});
}

void Board::ir_press(const std::string& keys, uint64_t at_frame) {
    for (char k : keys)
        ir_queue.emplace_back(at_frame, k);
}

// ---- devices ----
void Board::tick_idma() {
    // IDMA channel 1 drives the LED panel (MPC862 RM ch.19, "SDMA Channels and IDMA Emulation"). The real
    // hardware locates the channel's BD table through IBASE, a halfword in the IDMA1 parameter RAM at
    // IMMR+0x3CC0 (Table 19-4); a single wrap BD (so IBPTR==IBASE) describes the memory->peripheral transfer to
    // the panel FPGA (dst 0xF0000000). When the BD's V bit (status bit 15, Table 19-7) is set the CP streams the
    // frame and clears V; here we capture that frame, flag IDSR1 done and raise IDMA1 on the CPM (CIPR bit
    // 0x00200000, vector 0x15). This reads only architected registers, so the stock image and our from-source app
    // reach the panel the same way -- by programming IBASE -- with no firmware-private address.
    uint16_t ibase = dp16(0x3CC0);                         // IDMA1 IBASE: BD table offset from IMMR
    if (!ibase || (uint32_t)ibase + 16 > dpram.size())
        return;
    uint32_t o = ibase;
    uint16_t st = dp16(o);
    if (!(st & 0x8000))                                    // V: descriptor ready for processing
        return;
    uint32_t length = dp32(o + 4), src = dp32(o + 8);
    if (src && length && (uint64_t)src + length <= 0x8000000) {
        std::vector<uint8_t> f(length);
        read(src, f.data(), length);
        frames.push_back(std::move(f));
        frame_total++;
        // Test hook (not used in normal runs): UMAMI_RAW_DUMP names a file that gets every 8th panel frame appended,
        // raw RGB555, for capturing the panel headless while the Sign-window control socket stays live.
        { static const char* dp = getenv("UMAMI_RAW_DUMP"); static uint64_t dn = 0;
          static const char* se = getenv("UMAMI_RAW_STEP"); static uint64_t step = se && atoi(se) > 0 ? (uint64_t)atoi(se) : 8;
          if (dp && dn++ % step == 0) { if (FILE* df = fopen(dp, "ab")) { fwrite(frames.back().data(), 1, frames.back().size(), df); fclose(df); } } }
        if (frames.size() > frames_kept)
            frames.erase(frames.begin(), frames.begin() + (frames.size() - frames_kept));
    }
    dp16(o, st & ~0x8000);
    dpram[0x910] |= 0x01;                                  // IDSR1: done
    dp32(0x944, dp32(0x944) | 0x00200000);                 // CIPR: IDMA1
    events["panel frames"]++;
    tr(TR_FRAME);
}

void Board::set_sipend(int bit, bool on) {
    uint32_t m = 0x80000000u >> bit;
    dp32(0x10, on ? dp32(0x10) | m : dp32(0x10) & ~m);
}

void Board::tick_cpm() {
    // the CPM interrupt controller asserts its SIU level (CICR IRL) while a source is pending and enabled
    uint32_t cicr = dp32(0x940);
    int lvl = (cicr >> 13) & 7;
    set_sipend(1 + 2 * lvl, (cicr & 0x80) && cpm_highest() >= 0);
}

// CIPR vector and event register (offset, size) per serial minor: SCC1..4, SMC1..2
static const struct { int vn; uint32_t off; int size; } SERIAL_CPM[6] = {
    {0x1E, 0xA10, 2}, {0x1D, 0xA30, 2}, {0x1C, 0xA50, 2}, {0x1B, 0xA70, 2}, {0x04, 0xA86, 1}, {0x03, 0xA96, 1}};
constexpr uint32_t SERIAL_CHANS = 0xB9024, CHAN_SIZE = 0x78;   // serial_chans[6] (struct serial_chan)
// parameter RAM (DPRAM offsets, as dp16 takes them) per minor: SCC1..4, SMC1..2; TBPTR at +0x20
static const uint32_t SERIAL_PRAM[6] = {0x3C00, 0x3D00, 0x3E00, 0x3F00, 0x3E80, 0x3F80};

void Board::serial_event(int minor, int bits) {
    auto& c = SERIAL_CPM[minor];
    if (c.size == 2)
        dp16(c.off, dp16(c.off) | (uint16_t)bits);
    else
        dpram[c.off] |= (uint8_t)bits;
    dp32(0x944, dp32(0x944) | (1u << c.vn));               // CIPR
}

void Board::tick_serial() {
    // Find each channel's BD rings through the real CPM parameter RAM (RBASE/TBASE, MPC862 RM Table 29-2 for
    // the SMCs and the matching SCC UART param RAM) rather than the stock firmware's private serial_chans struct
    // at 0xB9024. RBASE/TBASE hold the BD-table base as an offset from IMMR (stock smc_uart_init / scc_uart_init
    // write low16(bd_addr)); MRBLR is the rx buffer size. Reading only architected registers means the stock
    // image and our from-source serial driver -- which programs this same parameter RAM -- both work.
    auto chan = [&](int minor, uint32_t& open, uint32_t& rxbd, uint32_t& txbd, uint32_t& rxsize) {
        uint32_t pram = SERIAL_PRAM[minor];
        uint16_t rbase = dp16(pram + 0x00), tbase = dp16(pram + 0x02), mrblr = dp16(pram + 0x06);
        rxbd = rbase ? IMMR + rbase : 0;
        txbd = tbase ? IMMR + tbase : 0;
        rxsize = mrblr;
        open = (rbase || tbase) ? 1u : 0u;   // channel configured via its parameter RAM
    };
    // every empty receive buffer of the ring is filled each tick (not one a tick): the sign takes host data as fast
    // as its firmware empties them, far above 115200 baud, and still never overruns
    for (int filled = 0; filled < 64 && !rx_queue.empty() && icount >= rx_queue.front().at &&
                         frame_total >= rx_queue.front().at_frame; filled++) {
        auto& rx = rx_queue.front();
        bool delivered = false;
        uint32_t open, rxbd, txbd, rxsize;
        chan(rx.minor, open, rxbd, txbd, rxsize);
        if (open && rxbd) {
            auto it = rx_idx.find(rx.minor);
            int i = it == rx_idx.end() ? 0 : it->second;
            uint32_t o = rxbd + 8 * i - IMMR;
            uint16_t st = dp16(o);
            if (st & 0x8000) {                             // E: empty, ours to fill
                uint32_t buf = dp32(o + 4);
                size_t n = rxsize ? std::min<size_t>(rxsize, rx.data.size()) : rx.data.size();
                write(buf, rx.data.data(), n);
                dp16(o + 2, (uint16_t)n);
                dp16(o, st & ~0x8000 & ~0x003F);
                rx_idx[rx.minor] = (st & 0x2000) ? 0 : i + 1;   // W: last BD of the ring
                serial_event(rx.minor, 0x01);              // RX
                tr(TR_RX, (uint8_t)rx.minor, (uint16_t)std::min<size_t>(n, 0xFFFF));
                delivered = true;
                if (n < rx.data.size()) {                  // more than the buffer holds: the rest goes into the next
                    rx.data.erase(rx.data.begin(), rx.data.begin() + (ptrdiff_t)n);   // BD, as on the real UART
                    events["serial rx"]++;
                    continue;
                }
                rx_queue.pop_front();
                events["serial rx"]++;
            }
        }
        if (!delivered)
            break;                                         // no empty buffer: the rest waits for the next tick
    }
    for (int minor = 0; minor < 6; minor++) {
        uint32_t open, rxbd, txbd, rxsize;
        chan(minor, open, rxbd, txbd, rxsize);
        if (!(open && txbd))
            continue;
        auto it = tx_idx.find(minor);
        int i = it == tx_idx.end() ? 0 : it->second;
        uint32_t o = txbd + 8 * i - IMMR;
        dp16(SERIAL_PRAM[minor] + 0x20, (uint16_t)(o - 0x2000));   // TBPTR: the BD the CPM sends next
        uint16_t st = dp16(o);
        if (st & 0x8000) {                                 // R: ready to send
            uint16_t n = dp16(o + 2);
            uint32_t buf = dp32(o + 4);
            std::vector<uint8_t> data(n);
            read(buf, data.data(), n);
            auto& out = serial_tx[minor];
            out.insert(out.end(), data.begin(), data.end());
            dp16(o, st & ~0x8000);
            tx_idx[minor] = (st & 0x2000) ? 0 : i + 1;     // W: last BD of the ring
            dp16(SERIAL_PRAM[minor] + 0x20, (uint16_t)(txbd - IMMR - 0x2000 + 8 * tx_idx[minor]));   // TBPTR
            serial_event(minor, 0x02);                     // TX
            tr(TR_TX, (uint8_t)minor, n);
            events["serial tx"]++;
        }
    }
}

// The decoded code ir_edge_isr would pass on for these 12 bit times (a copy of its state machine), or -1.
static int ir_isr_model(const uint32_t* bits, uint32_t, uint32_t, uint32_t Ta0, uint32_t Ta4, uint32_t Ta8) {
    uint32_t code = 0x800;
    for (int i = 0; i < 12; i++) {
        uint32_t d = bits[i];
        if (Ta0 < d) {
            if (d < Ta4 || Ta8 < d)
                return -1;
            code |= 0x1000;
        }
        uint32_t bit = code & 1;
        code >>= 1;
        if (bit)
            return (int)code;
    }
    return -1;
}

std::vector<std::pair<int, uint64_t>> Board::ir_frame(char ch) {
    // edges (level, timebase) for one remote frame of key ch, from the thresholds the app computed in ir_init_maybe
    // (0xBB098..0xBB0A8): the start low >= T98, then rising edges T0 or T1 apart
    uint32_t T98 = r32(0xBB098), T9c = r32(0xBB09C), Ta0 = r32(0xBB0A0), Ta4 = r32(0xBB0A4), Ta8 = r32(0xBB0A8);
    uint32_t t0 = (T9c + Ta0) / 2, t1 = (Ta4 + Ta8) / 2;
    const char* p = strchr(KEYMAP, ch);
    if (!p || !ch)
        return {};
    int want = (2 << 6) | (int)(p - KEYMAP);
    uint32_t bits[12];
    bool found = false;
    for (int n = 0; n < (1 << 12) && !found; n++) {
        for (int i = 0; i < 12; i++)
            bits[i] = (n >> i & 1) ? t1 : t0;
        found = ir_isr_model(bits, T98, T9c, Ta0, Ta4, Ta8) == want;
    }
    if (!found)
        return {};
    uint64_t t = ir_tb + 100000;
    std::vector<std::pair<int, uint64_t>> edges{{0, t}};
    uint64_t rise = t + T98 + T98 / 4;
    edges.emplace_back(1, rise);
    for (uint32_t d : bits) {
        edges.emplace_back(0, rise + d / 3);
        rise += d;
        edges.emplace_back(1, rise);
    }
    return edges;
}

void Board::tick_ir() {
    if (ir_edges.empty() && !ir_queue.empty() && frame_total >= ir_queue.front().first &&
        (!ir_last || now() - *ir_last >= IR_KEY_GAP_US * CPU_MIPS)) {
        if (!r32(0xBB0AC))                                 // IR semaphore: driver ready?
            return;
        char k = ir_queue.front().second;
        ir_queue.pop_front();
        auto f = ir_frame(k);                              // sent twice, as a held remote repeats
        ir_edges.assign(f.begin(), f.end());
        ir_edges.insert(ir_edges.end(), f.begin(), f.end());
        ir_last = now();
        ir_keys_sent++;
        events["ir keys"]++;
        tr(TR_IR, (uint8_t)k);
    }
    if (!ir_edges.empty() && !((dp32(0x944) | dp32(0x94C)) & 0x01000000)) {   // previous edge handled
        auto [level, tb] = ir_edges.front();
        ir_edges.pop_front();
        ir_level = level, ir_tb = tb;
        dp32(0x944, dp32(0x944) | 0x01000000);             // CIPR: PC13 edge (vector 0x18)
    }
}

std::vector<uint8_t> g_spi_tx;   // what the firmware actually clocked out over the CPM SPI
void Board::tick_devices() {
    if (fpga_model && !(dp16(0x956) & 0x0800))
        fpga_done = false;                                 // PROG low clears the FPGA
    if (fpga_model && (dpram[0xAAD] & 0x80)) {             // SPCOM.STR: run the real descriptors
        dpram[0xAAD] = 0;
        uint16_t rb = dp16(0x3D80), tb = dp16(0x3D82), st = dp16(tb), n = dp16(tb + 2);
        if ((st & 0x8000) && n) {
            std::vector<uint8_t> d(n), ff(n, 0xFF);
            read(dp32(tb + 4), d.data(), n);
            g_spi_tx = d;
            write(dp32(rb + 4), ff.data(), n);
            dp16(tb, st & ~0x8000);
            dp16(rb + 2, n);
            dp16(rb, dp16(rb) & ~0x8000);
            fpga_len = n;
            fpga_done = d[0] == 0xFF && d[1] == 0x20;
            events["fpga loads"]++;
        }
    }
    // The software watchdog (SYPCR SWE, set by the boot loader): the app must service SWSR within the timeout
    // (SWTC 0xFFFF x 2048 system clocks = 1.34 s at 100 MHz). Not reset here: recorded, so a test can fail on it.
    if (in_app && (dp32(0x004) & 0x4)) {
        if (!wdt_last)
            wdt_last = now();
        if (now() - *wdt_last > WDT_TIMEOUT_US * CPU_MIPS) {
            events["watchdog would reset"]++;
            wdt_last = now();
        }
    }
    tick_idma();
    tick_serial();
    tick_ir();
    tick_cpm();
    uint16_t piscr = dp16(0x240);
    uint32_t pitc = dp32(0x244) >> 16;
    if ((piscr & 0x1) && pitc) {                           // PTE: the PIT runs
        // the tick period the firmware asked for: system_tick_init computes PITC from us-per-tick at 0xBDDFC
        uint64_t us = r32(0xBDDFC);
        uint64_t period = (us ? us : 10000) * CPU_MIPS;
        if (!pit_next)
            pit_next = now() + period;
        if (now() >= *pit_next) {
            *pit_next += period;
            dp16(0x240, piscr | 0x80);                     // PS
            events["pit"]++;
            tr(TR_PIT);
        }
    }
    int pirq = piscr >> 8;                                 // one-hot: 0x80 = level 0, 0x40 = level 1, ...
    for (int n = 0; n < 8; n++)
        if (pirq & (0x80 >> n)) {
            set_sipend(1 + 2 * n, (piscr & 0x80) && (piscr & 0x4));   // PS and PIE: assert LVLn
            break;
        }
}

void Board::levels() {
    // SIPEND's internal levels as the sources drive them now: the CPM's and the PIT's (PS and PIE)
    tick_cpm();
    uint16_t piscr = dp16(0x240);
    int pirq = piscr >> 8;
    for (int n = 0; n < 8; n++)
        if (pirq & (0x80 >> n)) {
            set_sipend(1 + 2 * n, (piscr & 0x80) && (piscr & 0x4));
            break;
        }
}

bool Board::take_exception(uc_err e) {
    // An illegal instruction is a program exception (0x700, SRR1 bit 0x00080000); an access to no memory is a
    // machine check (0x200, as the bus answers with TEA). SRR0 = the faulting instruction. Off with exceptions=false.
    uint32_t vec, why = 0;
    if (!exceptions)
        return false;
    if (e == UC_ERR_INSN_INVALID || e == UC_ERR_EXCEPTION)   // Unicorn PPC: an illegal opcode is UC_ERR_EXCEPTION
        vec = 0x700, why = 0x00080000;
    else if (e == UC_ERR_READ_UNMAPPED || e == UC_ERR_WRITE_UNMAPPED || e == UC_ERR_FETCH_UNMAPPED)
        vec = 0x200;
    else
        return false;
    if (++fault_count > 100)                               // a fault inside the handler: give up
        return false;
    // Unicorn leaves the PC after an illegal opcode one instruction on; SRR0 must be the instruction itself
    spr[SPR_SRR0] = (uint32_t)pc() - (e == UC_ERR_EXCEPTION ? 4 : 0);
    spr[SPR_SRR1] = ((uint32_t)msr & 0xFFFF) | why;
    msr &= ~(EE | PR | RI);
    set_reg(UC_PPC_REG_PC, vec);
    events[vec == 0x700 ? "program exceptions" : "machine checks"]++;
    return true;
}

void Board::maybe_interrupt() {
    if ((dp32(0x10) & dp32(0x14)) && (msr & EE)) {
        spr[SPR_SRR0] = pc();
        spr[SPR_SRR1] = (uint32_t)msr;
        msr &= ~(EE | PR | RI);
        set_reg(UC_PPC_REG_PC, 0x500);
        events["external interrupts"]++;
        tr(TR_IRQ);
    }
}

// ---- counting and running ----
bool Board::bp_in_block(uint64_t addr, uint64_t n) const {
    auto it = breakpoints.lower_bound((uint32_t)addr);
    return it != breakpoints.end() && *it < addr + 4 * n;
}

static uint64_t bp_skip = UINT64_MAX;                      // a breakpoint at the resume address does not stop
static uint64_t stop_k = 0;                                // instructions of the stopped block to run
static bool stop_is_bp = false;

void Board::block(uint64_t address, uint32_t size) {
    uint64_t n = size >> 2;
    uint64_t allow = n;
    bool is_bp = false;
    if (cnt.count + n > cnt.limit)
        allow = cnt.limit - cnt.count;
    if (!breakpoints.empty() && bp_in_block(address, n)) {
        for (auto it = breakpoints.lower_bound((uint32_t)address); it != breakpoints.end() && *it < address + 4 * n; ++it) {
            if (*it == bp_skip)
                continue;
            uint64_t k = (*it - address) / 4;
            if (k <= allow && k < n) {
                allow = k;
                is_bp = true;
            }
            break;
        }
    }
    bp_skip = UINT64_MAX;
    if (allow < n) {
        cnt.stopped = true;
        cnt.stop_pc = address;
        cnt.stop_n = n;
        stop_k = allow;
        stop_is_bp = is_bp;
        uc_emu_stop(uc);
        return;
    }
    cnt.count += n;
    cnt.block = address;
    cnt.block_n = n;
}

void Board::skip_rest(uint64_t addr) {
    // the instruction at addr was the last of its block to run (a hook moved the PC there)
    if (addr < cnt.block)
        return;
    uint64_t k = (addr - cnt.block) / 4 + 1;
    if (0 < k && k < cnt.block_n) {
        cnt.count -= cnt.block_n - k;
        cnt.block_n = k;
    }
}

uint64_t Board::emu(uint64_t pc0, uint64_t n) {
    // exactly the instructions emu_start(pc, count=n) would run, stopping on the same instruction
    if (!counting) {
        uc_hook h;
        check(uc_hook_add(uc, &h, UC_HOOK_BLOCK, (void*)cb_block, this, 1, 0), "block hook");
        hooks.push_back(h);
        uc_ctl_flush_tb(uc);                               // every block from now on calls the hook
        counting = true;
    }
    hit_breakpoint = false;
    uint64_t pc = pc0;
    uint64_t start = cnt.count, target = start + n;
    bp_skip = pc;
    while (cnt.count < target) {
        cnt.stopped = false;
        cnt.limit = target;
        uc_err e = uc_emu_start(uc, pc, 0xFFFFFFFF, 0, 0);
        pc = this->pc();
        if (e != UC_ERR_OK && in_app && take_exception(e)) {   // the firmware's own exception vector
            pc = this->pc();
            continue;
        }
        if (e != UC_ERR_OK) {
            fprintf(stderr, "emulation error %s at pc=0x%llx\n", uc_strerror(e), (unsigned long long)pc);
            break;
        }
        if (!cnt.stopped)                                  // someone else's emu_stop
            break;
        uint64_t r = stop_k;
        bool bp = stop_is_bp;
        if (r) {
            // The block at pc would cross the target (or holds a breakpoint): run its first r instructions alone,
            // with the emulation's end address r instructions in. Unicorn applies an end address when it translates
            // a block, so the block is dropped from the translation cache before (to cut it there) and after (so no
            // later run stops there).
            cnt.stopped = false;
            uint64_t until = pc + 4 * r;
            uc_ctl_remove_cache(uc, pc, pc + 4);
            e = uc_emu_start(uc, pc, until, 0, 0);
            uc_ctl_remove_cache(uc, pc, until + 4);
            pc = this->pc();
            if (e != UC_ERR_OK && in_app && take_exception(e)) {
                pc = this->pc();
                continue;
            }
            if (e != UC_ERR_OK) {
                fprintf(stderr, "emulation error %s at pc=0x%llx\n", uc_strerror(e), (unsigned long long)pc);
                break;
            }
            if (pc != until && !cnt.stopped)               // someone else's emu_stop
                break;
        }
        if (bp && pc == cnt.stop_pc + 4 * r) {
            hit_breakpoint = true;
            hit_pc = (uint32_t)pc;
            break;
        }
        if (!r)
            break;
    }
    cnt.limit = UINT64_MAX;
    return cnt.count - start;
}

uint64_t Board::advance(uint64_t n) {
    in_app = true;
    if (slice_done >= slice_size) {                        // a new slice
        bool ir = !ir_edges.empty() || (!ir_queue.empty() && frame_total >= ir_queue.front().first);
        slice_size = ir ? SLICE_IR : SLICE;
        slice_done = 0;
    }
    uint64_t k = std::min(n, slice_size - slice_done);
    uint64_t ran = emu(pc(), k);
    if (hit_breakpoint)
        slice_done += ran;
    else if (ran < k)                                      // stopped by something else: the slice counts whole
        slice_done = slice_size;
    else
        slice_done += ran;
    if (slice_done >= slice_size) {
        slice_done = slice_size;
        icount += slice_size;
        tick_devices();
        maybe_interrupt();
    }
    return ran;
}

void Board::run_app(uint64_t max_insns) {
    in_app = true;
    while (icount < max_insns) {
        advance(UINT64_MAX);
        if (hit_breakpoint)
            break;
    }
}

}  // namespace sim

// ---- main flash: the AMD command set, word mode (command addresses 0x555 / 0x2AA in words) ----
uint64_t sim::Board::main_read(uint64_t off, unsigned size) {
    uint64_t v = 0;
    for (unsigned k = 0; k < size && off + k < main_flash.size(); k++)
        v = v << 8 | main_flash[off + k];
    return v;
}

void sim::Board::main_write(uint64_t off, unsigned size, uint64_t value) {
    int chip = off >= 0x1000000 ? 1 : 0;
    uint64_t base = chip ? 0x1000000 : 0, rel = off - base;
    int& st = mf_state[chip];
    uint16_t v = (uint16_t)(size >= 2 ? value & 0xFFFF : value & 0xFF);
    if (v == 0xF0 && st != 3) {                            // reset to read mode
        st = 0;
        return;
    }
    switch (st) {
    case 0: st = (rel == 0xAAA && v == 0xAA) ? 1 : 0; return;
    case 1: st = (rel == 0x554 && v == 0x55) ? 2 : 0; return;
    case 2:
        if (rel == 0xAAA && v == 0xA0) st = 3;
        else if (rel == 0xAAA && v == 0x80) st = 4;
        else st = 0;
        return;
    case 3:                                                // program: bits only go from 1 to 0
        for (unsigned k = 0; k < size && off + k < main_flash.size(); k++)
            main_flash[off + k] &= (uint8_t)(value >> (8 * (size - 1 - k)));
        st = 0;
        return;
    case 4: st = (rel == 0xAAA && v == 0xAA) ? 5 : 0; return;
    case 5: st = (rel == 0x554 && v == 0x55) ? 6 : 0; return;
    case 6:
        if (v == 0x30)                                     // sector erase: the 64 KB sector holding the address
            std::fill(main_flash.begin() + (off & ~0xFFFFull), main_flash.begin() + (off & ~0xFFFFull) + 0x10000, 0xFF);
        else if (v == 0x10 && rel == 0xAAA)                // chip erase
            std::fill(main_flash.begin() + base, main_flash.begin() + base + 0x1000000, 0xFF);
        st = 0;
        return;
    }
    st = 0;
}
