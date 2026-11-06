// umami-marquee-sim-test: the simulator's verification runs, entirely in C++ (the port of the Python scripts in sim/).
// Each subcommand takes the arguments of its script, prints the same report and ends with the same verdict line
// and exit status.
//
// Usage: umami-marquee-sim-test [--repo DIR] SUBCOMMAND [ARGS...]   (umami-marquee-sim-test --help lists them)
#include "simtest.h"
#include "dbgrecord.h"
#include "signlink.h"

#include "attract_lines.h"
#include "image.h"
#include "sha256.h"
#include "signproto.h"

#include <fstream>
#include <algorithm>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <functional>
#include <map>
#include <regex>
#include <set>
#include <sstream>
#include <stdexcept>
#include <tuple>

namespace sim {

namespace {

std::string g_repo_dir;
const std::string STOCK = "analysis/extracted/ELITE_RGB", FROMSRC = "analysis/build/fw/ELITE_RGB",
                  DUMP = "dumps/bootflash.bin";

uint32_t be32(const uint8_t* p) { return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3]; }

std::string fmt(const char* f, ...) __attribute__((format(printf, 1, 2)));
std::string fmt(const char* f, ...) {
    char buf[1024];
    va_list ap;
    va_start(ap, f);
    vsnprintf(buf, sizeof buf, f, ap);
    va_end(ap);
    return buf;
}

std::string arg(const std::vector<std::string>& a, size_t i, const std::string& def) { return i < a.size() ? a[i] : def; }
uint64_t count(const std::vector<std::string>& a, size_t i, uint64_t def) { return i < a.size() ? count_arg(a[i]) : def; }

std::string sha16(const std::vector<Frame>& fs) {           // sha256 of the frames joined, first 16 hex digits
    Sha256 s;
    for (auto& f : fs)
        s.update(f.data(), f.size());
    return s.hex().substr(0, 16);
}

}  // namespace

// ---- shared set-ups ----
const Repo& repo() {
    static Repo r(g_repo_dir);
    return r;
}

uint64_t count_arg(const std::string& s) {
    std::string t;
    for (char c : s)
        if (c != '_')
            t += c;
    size_t used = 0;
    uint64_t v = std::stoull(t, &used, 10);
    if (used != t.size())
        throw std::invalid_argument("not a number: " + s);
    return v;
}

std::unique_ptr<Board> swapped(const std::string& app_path) {
    auto b = std::make_unique<Board>(read_file(repo().path(DUMP)), Scenario{}, read_file(app_path));
    if (!b->boot())
        throw std::runtime_error("the boot loader did not reach the app");
    b->load_app_image();
    b->hook_app_specials();
    b->hook_busy_waits(repo().symbols);
    b->watch_switches(repo().symbols);
    return b;
}

std::unique_ptr<Board> booted(const std::vector<uint8_t>& flash, const std::vector<uint8_t>& app,
                              std::vector<uint32_t>* bad) {
    auto b = std::make_unique<Board>(flash, Scenario{}, app);
    // EEPROM_A0 / EEPROM_A2: a real board's config EEPROM (e.g. a Saleae capture, tools/saleae_eeprom.py) instead of
    // the synthetic one; its firmware may then load the panel FPGA, so the fake FPGA is on
    const char* ea0 = getenv("EEPROM_A0");
    const char* ea2 = getenv("EEPROM_A2");
    if (ea0)
        b->eeproms[0xA0] = read_file(ea0);
    if (ea2)
        b->eeproms[0xA2] = read_file(ea2);
    if (!b->boot())
        throw std::runtime_error("the boot loader did not reach the app");
    if (bad) {
        std::vector<uint8_t> ram(app.size());
        b->read(0, ram.data(), ram.size());
        uint32_t tp = be32(&ram[0x4C]), tl = be32(&ram[0x50]);
        for (uint32_t i = 0; i < app.size(); i += 4) {
            if ((i >= 0x14 && i < 0x9C) || (i >= tp && i < tp + tl))
                continue;
            size_t n = std::min<size_t>(4, app.size() - i);
            if (!std::equal(ram.begin() + i, ram.begin() + i + n, app.begin() + i))
                bad->push_back(i);
        }
    }
    b->hook_app_specials();
    b->hook_busy_waits(repo().symbols);
    b->watch_switches(repo().symbols);
    if (ea0)
        b->fake_fpga(repo().symbols);
    return b;
}

Frame masked(const Frame& f) {
    Frame m = f;
    for (size_t i = 0; i < m.size(); i += 2)
        m[i] &= 0x7F;                                      // bit 15 is the driver's marker
    return m;
}

std::vector<Frame> distinct(const std::vector<Frame>& frames) {
    std::vector<Frame> out;
    static const size_t want = getenv("EEPROM_A0") ? 0 : 2560;   // a real EEPROM: its own driver frame (+0x13)
    // MASK_ROWS=A-B (with EEPROM_A0): rows A..B left out of the comparison, e.g. the stock app's loading progress
    // bars, which move with the image's file list; the row width comes from the EEPROM's driver frame (+0x13)
    static int mask_lo = -1, mask_hi = -1, row_w = 0;
    static bool parsed = false;
    if (!parsed) {
        parsed = true;
        const char* mr = getenv("MASK_ROWS");
        const char* ea0 = getenv("EEPROM_A0");
        if (mr && ea0 && sscanf(mr, "%d-%d", &mask_lo, &mask_hi) == 2) {
            auto e = read_file(ea0);
            row_w = ((e[0x13] >> 4) * 20 + 20);
        }
    }
    for (auto& f : frames) {
        if (want && f.size() != want)
            continue;
        Frame m = masked(f);
        if (row_w)
            for (int y = mask_lo; y <= mask_hi; y++)
                for (int x = 0; x < row_w && (size_t)(2 * (y * row_w + x) + 1) < m.size(); x++)
                    m[2 * (y * row_w + x)] = m[2 * (y * row_w + x) + 1] = 0;
        if (out.empty() || out.back() != m)
            out.push_back(std::move(m));
    }
    return out;
}

// ---- report text ----
std::string py_tasks(const Board& b) {
    std::string s = "[";
    bool first = true;
    for (auto& [name, n] : b.tasks)                        // std::map: sorted, as sorted(b.tasks)
        s += (first ? "'" : ", '") + name + "'", first = false;
    return s + "]";
}

std::string py_bytes_repr(const std::vector<uint8_t>& v) {
    bool sq = std::find(v.begin(), v.end(), '\'') != v.end(), dq = std::find(v.begin(), v.end(), '"') != v.end();
    char q = sq && !dq ? '"' : '\'';
    std::string s(1, q);
    for (uint8_t c : v) {
        if (c == '\\' || c == (uint8_t)q)
            s += '\\', s += (char)c;
        else if (c == '\n')
            s += "\\n";
        else if (c == '\r')
            s += "\\r";
        else if (c == '\t')
            s += "\\t";
        else if (c < 0x20 || (c >= 0x7F && c <= 0xA0) || c == 0xAD)
            s += fmt("\\x%02x", c);
        else
            s += latin1_to_utf8({c});
    }
    return s + q;
}

std::string hex(const std::vector<uint8_t>& v, const char* sep) {
    std::string s;
    for (size_t i = 0; i < v.size(); i++)
        s += (i ? sep : "") + fmt("%02x", v[i]);
    return s;
}

std::string latin1_to_utf8(const std::vector<uint8_t>& v) {
    std::string s;
    for (uint8_t c : v)
        if (c < 0x80)
            s += (char)c;
        else
            s += (char)(0xC0 | c >> 6), s += (char)(0x80 | (c & 0x3F));
    return s;
}

uint64_t event(const Board& b, const std::string& name) {
    auto it = b.events.find(name);
    return it == b.events.end() ? 0 : it->second;
}

// ---- the DB9 protocol ----
static const std::set<uint8_t> DATA_TYPES = {0x10, 0x20, 0x21};
static const std::map<uint8_t, const char*> FRAME_NAMES = {{0x10, "finish"}, {0x20, "start/name"}, {0x21, "block"},
    {0x80, "ack"}, {0x81, "nak"}, {0x90, "0x90"}, {0xBB, "status/stop"}};

std::vector<uint8_t> db9_frame(uint8_t type, const std::vector<uint8_t>& data, uint16_t param, uint8_t sync) {
    uint16_t v = DATA_TYPES.count(type) ? (uint16_t)data.size() : param;
    std::vector<uint8_t> f = {sync, type, (uint8_t)(v >> 8), (uint8_t)v};
    uint16_t c = host::crc16_xmodem(f.data(), 4);
    f.push_back((uint8_t)(c >> 8)), f.push_back((uint8_t)c);
    if (DATA_TYPES.count(type)) {
        f.insert(f.end(), data.begin(), data.end());
        c = host::crc16_xmodem(data.data(), data.size());
        f.push_back((uint8_t)(c >> 8)), f.push_back((uint8_t)c);
    }
    return f;
}

std::vector<SignFrame> parse_sign(const std::vector<uint8_t>& in) {
    // complete A5 frames out of a byte buffer (signproto.parse_sign; the leftover is dropped)
    std::vector<SignFrame> out;
    std::vector<uint8_t> buf = in;
    for (;;) {
        auto it = std::find(buf.begin(), buf.end(), 0xA5);
        if (it == buf.end())
            return out;
        buf.erase(buf.begin(), it);
        if (buf.size() < 6)
            return out;
        uint8_t type = buf[1];
        uint16_t val = (uint16_t)(buf[2] << 8 | buf[3]);
        if (host::crc16_xmodem(buf.data(), 4) != (buf[4] << 8 | buf[5])) {
            buf.erase(buf.begin());
            continue;
        }
        if (DATA_TYPES.count(type)) {
            if (buf.size() < 6 + (size_t)val + 2)
                return out;
            std::vector<uint8_t> data(buf.begin() + 6, buf.begin() + 6 + val);
            bool ok = host::crc16_xmodem(data.data(), data.size()) == (buf[6 + val] << 8 | buf[7 + val]);
            out.push_back({type, val, data, ok});
            buf.erase(buf.begin(), buf.begin() + 8 + val);
        } else {
            out.push_back({type, val, {}, true});
            buf.erase(buf.begin(), buf.begin() + 6);
        }
    }
}

std::string describe(const SignFrame& f) {
    auto nm = FRAME_NAMES.find(f.type);
    std::string s = fmt("A5 %02X (%s) %s=0x%04x", f.type, nm == FRAME_NAMES.end() ? "?" : nm->second,
                        DATA_TYPES.count(f.type) ? "len" : "param", f.val);
    if (!f.data.empty()) {
        s += " data=" + hex(f.data, " ");
        if (f.type == 0x20 && f.data[0] < f.data.size())
            s += "  name=" + py_bytes_repr(std::vector<uint8_t>(f.data.begin() + 1, f.data.begin() + 1 + f.data[0]));
    }
    return s + (f.ok ? "" : "  DATA CRC BAD");
}

// ---- frames as pictures ----
void save_frames(const std::vector<Frame>& frames, const std::string& out, int W, int H) {
    std::filesystem::create_directories(out);
    size_t n = frames[0].size() / 2;
    if (!W) {
        W = 80, H = (int)(n / 80);
        for (int w : {80, 120, 160, 200, 320}) {
            size_t h = n / w;
            if (n % w == 0 && (h == 16 || h == 32 || h == 48 || h == 64 || h == 80 || h == 96)) {
                W = w, H = (int)h;
                break;
            }
        }
    }
    auto rgb = [&](const Frame& f, int x, int y, uint8_t* p) {
        uint16_t v = (uint16_t)((f[2 * (y * W + x)] << 8 | f[2 * (y * W + x) + 1]) & 0x7FFF);
        p[0] = (uint8_t)(((v >> 10) & 31) << 3), p[1] = (uint8_t)(((v >> 5) & 31) << 3), p[2] = (uint8_t)((v & 31) << 3);
        p[3] = 255;
    };
    size_t step = std::max<size_t>(1, frames.size() / 24);
    std::vector<const Frame*> pick;
    for (size_t i = 0; i < frames.size() && pick.size() < 24; i += step)
        pick.push_back(&frames[i]);
    int sh = (int)pick.size() * (H + 2);
    host::Image sheet = host::Image::blank(W * 4, sh * 4);
    for (int y = 0; y < sh; y++)
        for (int x = 0; x < W; x++) {
            uint8_t p[4] = {40, 40, 40, 255};             // one row of grey between frames
            int k = y / (H + 2), fy = y % (H + 2) - 1;
            if (fy >= 0 && fy < H)
                rgb(*pick[k], x, fy, p);
            for (int dy = 0; dy < 4; dy++)
                for (int dx = 0; dx < 4; dx++)
                    std::copy(p, p + 4, sheet.at(x * 4 + dx, y * 4 + dy));
        }
    host::save_png(sheet, out + "/sheet.png");
    host::Image last = host::Image::blank(W * 8, H * 8);
    for (int y = 0; y < H; y++)
        for (int x = 0; x < W; x++) {
            uint8_t p[4];
            rgb(frames.back(), x, y, p);
            for (int dy = 0; dy < 8; dy++)
                for (int dx = 0; dx < 8; dx++)
                    std::copy(p, p + 4, last.at(x * 8 + dx, y * 8 + dy));
        }
    host::save_png(last, out + "/last.png");
    printf("  saved %zu frames (%dx%d) -> %s/sheet.png, last.png\n", frames.size(), W, H, out.c_str());
}

// ---- compare (sim/compare.py): stock vs another image, frame by frame ----
static std::unique_ptr<Board> compare_run(const std::string& image, uint64_t insns, int64_t menu_key_at = -1) {
    auto b = swapped(image);
    if (menu_key_at >= 0)
        b->ir_press("!", (uint64_t)menu_key_at);
    b->run_app(insns);
    return b;
}

int cmd_compare(const std::vector<std::string>& args) {
    std::string image = arg(args, 0, repo().path(FROMSRC));
    uint64_t insns = count(args, 1, 80000000);
    auto a = compare_run(repo().path(STOCK), count(args, 2, insns));
    auto b = compare_run(image, insns);
    auto da = distinct(a->frames), db = distinct(b->frames);
    size_t n = std::min(da.size(), db.size()), same = 0;
    for (size_t i = 0; i < n; i++)
        same += da[i] == db[i];
    printf("stock: %zu frames, %zu distinct; tasks %s\n", a->frames.size(), da.size(), py_tasks(*a).c_str());
    printf("other: %zu frames, %zu distinct; tasks %s\n", b->frames.size(), db.size(), py_tasks(*b).c_str());
    printf("distinct frames compared in order: %zu of %zu identical\n", same, n);
    for (size_t i = 0; i < n; i++)
        if (da[i] != db[i]) {
            printf("first difference at distinct frame %zu\n", i);
            break;
        }
    bool tasks_same = py_tasks(*a) == py_tasks(*b);
    std::string verdict;
    if (same == n && da.size() == db.size() && n > 0 && tasks_same)
        verdict = "IDENTICAL";
    else if (same == n && n > 0 && tasks_same)
        // every frame matches as far as the shorter run got: the images run at different speeds, so one is
        // further along within the same instruction budget
        verdict = fmt("PREFIX (all %zu frames the shorter run produced match; stock %zu, other %zu: compare a longer "
                      "run of the slower one)", n, da.size(), db.size());
    else {
        std::vector<std::string> why;
        if (!tasks_same)
            why.push_back("different task set");
        if (da.size() != db.size())
            why.push_back(fmt("%zu vs %zu distinct frames", da.size(), db.size()));
        if (same != n)
            why.push_back(fmt("%zu of the first %zu differ", n - same, n));
        verdict = "DIFFERENT (";
        for (size_t i = 0; i < why.size(); i++)
            verdict += (i ? "; " : "") + why[i];
        verdict += ")";
    }
    printf("RESULT: %s (sequence hash %s vs %s)\n", verdict.c_str(), sha16(da).c_str(), sha16(db).c_str());
    return verdict == "IDENTICAL" ? 0 : 1;
}

// ---- compare-rs485 (sim/compare_rs485.py): the same RS-485 record to both images ----
namespace {
const std::pair<const char*, uint32_t> COUG_COUNTERS[] = {{"bytes_rx", 0xB9310}, {"bytes_tx", 0xB9314},
    {"frame_errors", 0xB9318}, {"other", 0xB931C}, {"frames_ok", 0xB9320}, {"quick_polls", 0xB9324},
    {"poll_replies", 0xB9328}};
constexpr uint32_t COUG_PAYLOAD = 0x12DD0C;
constexpr uint32_t JACKPOT_LOCATION_UPDATE = 0x5E6BC, JACKPOT_RECORDS_PTR = 0xBB178;   // -> 4 x 0x2B8 in NVRAM

struct Rs485Run {
    std::unique_ptr<Board> b;
    std::vector<std::pair<uint32_t, std::vector<uint8_t>>> updates;   // jackpot_location_update (level, record)
    std::vector<uint32_t> coug;
    std::vector<uint8_t> payload, records, nvram;
    uint32_t records_at = 0;
};

Rs485Run rs485_run(const std::string& image, uint64_t insns, const std::vector<uint8_t>& frame, uint64_t at_frame,
                   size_t payload_len) {
    Rs485Run r;
    r.b = swapped(image);
    Board& b = *r.b;
    b.force_config_word(0xDCDE4, 8);                       // protocol: Cougar
    b.force_config_bytes(0xDCDF0, {0x01, 0x0F, 0x0F, 0x0F});   // display level 0 takes Cougar level 1
    b.serial_send(1, frame, 0, at_frame);
    auto* rp = &r;
    b.on_exec(JACKPOT_LOCATION_UPDATE, [rp](Board& bb) {
        std::vector<uint8_t> rec(0x2B8);
        bb.read(bb.reg(UC_PPC_REG_4), rec.data(), rec.size());
        rp->updates.emplace_back(bb.reg(UC_PPC_REG_3), std::move(rec));
    });
    b.run_app(insns);
    for (auto& c : COUG_COUNTERS)
        r.coug.push_back(b.r32(c.second));
    r.payload.resize(payload_len);
    b.read(COUG_PAYLOAD, r.payload.data(), payload_len);
    r.records_at = b.r32(JACKPOT_RECORDS_PTR);
    if (r.records_at) {
        r.records.resize(4 * 0x2B8);
        b.read(r.records_at, r.records.data(), r.records.size());
    }
    r.nvram.resize(0x8000);
    b.read(NVRAM, r.nvram.data(), r.nvram.size());
    return r;
}
}  // namespace

int cmd_compare_rs485(const std::vector<std::string>& args) {
    std::string image = arg(args, 0, repo().path(FROMSRC));
    uint64_t insns = count(args, 1, 600000000), stock_insns = count(args, 2, 750000000), at_frame = count(args, 3, 700);
    auto frame = host::rs485_frame(1, host::rs485_text_record(U"HELLO FROM THE SIM", 1, true, 123456));
    auto rx = host::coug_receive(frame);
    size_t plen = rx.empty() ? 0 : rx[0].second.size();
    Rs485Run a = rs485_run(repo().path(STOCK), stock_insns, frame, at_frame, plen);
    Rs485Run b = rs485_run(image, insns, frame, at_frame, plen);
    for (auto [tag, x] : {std::pair<const char*, Rs485Run*>{"stock", &a}, {"other", &b}}) {
        std::string up = "[";
        for (size_t i = 0; i < x->updates.size(); i++)
            up += fmt("%s(%u, '%s')", i ? ", " : "", x->updates[i].first,
                      hex(std::vector<uint8_t>(x->updates[i].second.begin(), x->updates[i].second.begin() + 8)).c_str());
        printf("%s: jackpot updates %s]\n", tag, up.c_str());
        std::string coug = "{", tx = "{";
        for (size_t i = 0; i < 7; i++)
            coug += fmt("%s'%s': %u", i ? ", " : "", COUG_COUNTERS[i].first, x->coug[i]);
        bool first = true;
        for (auto& [k, v] : x->b->serial_tx)
            tx += fmt("%s%d: %zu", first ? "" : ", ", k, v.size()), first = false;
        printf("%s: serial rx events %llu, Cougar %s}, tx %s}, tasks %s\n", tag,
               (unsigned long long)event(*x->b, "serial rx"), coug.c_str(), tx.c_str(), py_tasks(*x->b).c_str());
    }
    auto da = distinct(a.b->frames), db = distinct(b.b->frames);
    size_t n = std::min(da.size(), db.size());
    std::vector<size_t> diff;
    for (size_t i = 0; i < n; i++)
        if (da[i] != db[i])
            diff.push_back(i);
    std::string at;
    for (size_t i = 0; i < diff.size(); i++)
        at += (i ? ", " : "") + std::to_string(diff[i]);
    printf("distinct frames: stock %zu, other %zu; %zu of %zu identical%s\n", da.size(), db.size(), n - diff.size(), n,
           diff.empty() ? "" : ("; differ at [" + at + "] (check: the start-up progress bar)").c_str());
    bool rx_one = a.b->events.count("serial rx") && event(*a.b, "serial rx") == 1 && b.b->events.count("serial rx") &&
                  event(*b.b, "serial rx") == 1;
    bool ok = rx_one && a.coug == b.coug && a.coug[4] && a.payload == b.payload && a.b->serial_tx == b.b->serial_tx &&
              py_tasks(*a.b) == py_tasks(*b.b) && !a.updates.empty() && a.updates == b.updates &&
              a.records_at == b.records_at && a.records == b.records && a.nvram == b.nvram;
    auto tf = [](bool v) { return v ? "True" : "False"; };
    printf("jackpot updates same: %s; jackpot records same: %s; NVRAM same: %s\n", tf(a.updates == b.updates),
           tf(a.records_at == b.records_at && a.records == b.records), tf(a.nvram == b.nvram));
    printf("jackpot records at 0x%x / 0x%x\n", a.records_at, b.records_at);
    printf("RESULT: %s\n", ok ? "SAME HANDLING" : "DIFFERENT");
    return ok ? 0 : 1;
}

// ---- compare-db9 (sim/compare_db9.py): the same read-only DB9 probes to both images ----
int cmd_compare_db9(const std::vector<std::string>& args) {
    std::string image = arg(args, 0, repo().path(FROMSRC));
    uint64_t insns = count(args, 1, 600000000), stock_insns = count(args, 2, 750000000);
    const std::vector<std::pair<uint64_t, std::vector<uint8_t>>> probes = {{700, db9_frame(0x20, {0x00})},
                                                                           {900, db9_frame(0x90)}};
    auto run = [&](const std::string& img, uint64_t n) {
        auto b = swapped(img);
        for (auto& [at_frame, data] : probes)
            b->serial_send(4, data, 0, at_frame);
        b->run_app(n);
        return b;
    };
    auto a = run(repo().path(STOCK), stock_insns);
    auto b = run(image, insns);
    for (auto [tag, x] : {std::pair<const char*, Board*>{"stock", a.get()}, {"other", b.get()}}) {
        printf("%s: probes delivered %llu of %zu, tasks %s\n", tag, (unsigned long long)event(*x, "serial rx"),
               probes.size(), py_tasks(*x).c_str());
        for (auto& [minor, data] : x->serial_tx) {
            printf("  sent on minor %d: %s\n", minor, hex(data, " ").c_str());
            for (auto& f : parse_sign(data))
                printf("    %s\n", describe(f).c_str());
        }
    }
    auto tx4 = a->serial_tx.find(4);
    bool ok = a->events.count("serial rx") && b->events.count("serial rx") &&
              event(*a, "serial rx") == probes.size() && event(*b, "serial rx") == probes.size() &&
              a->serial_tx == b->serial_tx && tx4 != a->serial_tx.end() && !tx4->second.empty() &&
              py_tasks(*a) == py_tasks(*b);
    printf("RESULT: %s\n", ok ? "SAME REPLIES" : "DIFFERENT");
    return ok ? 0 : 1;
}

// ---- compare-menu (sim/compare_menu.py): walk the on-screen menu with the IR remote in both images ----
namespace {
const std::string MENU_KEYS = "!" + std::string(12, ']') + "}" + std::string(4, ']') + "{-";
const std::string MENU_EDIT_KEYS = "![[[}-}-";
constexpr uint64_t MENU_FIRST = 700, MENU_GAP = 15;       // panel frame of the first key, frames between keys
constexpr int SETTLE = 10;                                 // panel frames a screen must stay to count as settled
constexpr uint32_t DISPLAY_STATE = 0x45BFCC;

std::set<Frame> settled(const std::vector<Frame>& frames, size_t from) {
    // the distinct frames shown for SETTLE or more panel frames in a row (masked as distinct does)
    std::set<Frame> out;
    int run = 0;
    Frame prev;
    bool have = false;
    for (size_t i = from; i < frames.size(); i++) {
        if (frames[i].size() != 2560)
            continue;
        Frame f = masked(frames[i]);
        run = have && f == prev ? run + 1 : 1;
        prev = std::move(f), have = true;
        if (run == SETTLE)
            out.insert(prev);
    }
    return out;
}
}  // namespace

int cmd_compare_menu(const std::vector<std::string>& in) {
    std::vector<std::string> args;
    bool edit = false;
    for (auto& a : in)
        if (a == "--edit" && !edit)
            edit = true;
        else
            args.push_back(a);
    const std::string& keys = edit ? MENU_EDIT_KEYS : MENU_KEYS;
    std::string image = arg(args, 0, repo().path(FROMSRC));
    uint64_t insns = count(args, 1, 600000000), stock_insns = count(args, 2, 680000000);
    auto run = [&](const std::string& img, uint64_t n) {
        auto b = swapped(img);
        for (size_t i = 0; i < keys.size(); i++)
            b->ir_press(std::string(1, keys[i]), MENU_FIRST + MENU_GAP * i);
        b->watch_writes(DISPLAY_STATE, 4);
        b->run_app(n);
        return b;
    };
    auto a = run(repo().path(STOCK), stock_insns);
    auto b = run(image, insns);
    auto sa = settled(a->frames, MENU_FIRST), sb = settled(b->frames, MENU_FIRST);
    for (auto [tag, x, d] : {std::tuple<const char*, Board*, std::set<Frame>*>{"stock", a.get(), &sa},
                             {"other", b.get(), &sb}}) {
        std::string st = "[";
        for (size_t i = 0; i < x->states.size(); i++)
            st += (i ? ", " : "") + std::to_string(x->states[i]);
        printf("%s: keys %llu of %zu, display states %s], %zu settled screens\n", tag,
               (unsigned long long)event(*x, "ir keys"), keys.size(), st.c_str(), d->size());
    }
    size_t both = 0;
    for (auto& f : sa)
        both += sb.count(f);
    printf("settled screens: %zu in both, %zu only in stock, %zu only in the other\n", both, sa.size() - both,
           sb.size() - both);
    std::vector<uint8_t> na(0x8000), nb(0x8000);
    a->read(NVRAM, na.data(), na.size());
    b->read(NVRAM, nb.data(), nb.size());
    size_t nv = 0, first = 0;
    for (size_t i = 0; i < na.size(); i++)
        if (na[i] != nb[i] && !nv++)
            first = i;
    printf("NVRAM: %s\n", nv ? fmt("%zu bytes differ, first at 0x%zx", nv, first).c_str() : "identical");
    bool ok = a->events.count("ir keys") && b->events.count("ir keys") && event(*a, "ir keys") == keys.size() &&
              event(*b, "ir keys") == keys.size() &&
              std::find(a->states.begin(), a->states.end(), 4u) != a->states.end() && a->states == b->states &&
              sa == sb && !nv;
    printf("RESULT: %s\n", ok ? "IDENTICAL" : "DIFFERENT");
    return ok ? 0 : 1;
}

// ---- flashtest (sim/flashtest.py): a complete boot-flash image boots, its app is checked ----
int cmd_flashtest(const std::vector<std::string>& args) {
    if (args.size() < 2) {
        fprintf(stderr, "usage: umami-marquee-sim-test flashtest FLASH.bin APP [INSNS] [STOCK_INSNS]\n"
                        "  (FLASH.bin from bootloader/tools/mkimage.py --app APP)\n");
        return 1;
    }
    auto flash = read_file(args[0]), app = read_file(args[1]);
    uint64_t insns = count(args, 2, 80000000), stock_insns = count(args, 3, insns);
    std::vector<uint32_t> bad;
    auto b = booted(flash, app, &bad);
    b->run_app(insns);
    std::string name = std::filesystem::path(args[1]).filename().string();
    printf("1. RAM after the boot loader = %s: %s\n", name.c_str(),
           bad.empty() ? "yes (except the hand-off block and MMU table)"
                       : fmt("NO, %zu words differ, first 0x%x", bad.size(), bad[0]).c_str());
    // FW_VERSION: the image shows and reports its own version (analysis/tools/fwext.py); the stock reference gets the
    // same text in version_string (0xBDA90, 5 characters) so every frame can still be compared
    auto a = booted(read_file(repo().path(DUMP)), read_file(repo().path(STOCK)));
    if (const char* v = getenv("FW_VERSION")) {
        std::string s = v;
        if (s.size() != 5)
            throw std::runtime_error("FW_VERSION must be 5 characters, as \"V4.20\"");
        a->write(0xBDA90, reinterpret_cast<const uint8_t*>(s.data()), s.size());   // in RAM, before the app runs
        printf("   (stock reference with version_string \"%s\", as this image)\n", v);
    }
    // FW_REF_RETURN=ADDR[,ADDR...] (hex): functions this image silences (bootscreen.c: boot_scroll_version) return at
    // once in the stock reference too ("blr"), so the frames stay comparable
    if (const char* rr = getenv("FW_REF_RETURN")) {
        std::stringstream ss(rr);
        for (std::string tok; std::getline(ss, tok, ',');) {
            uint32_t at = (uint32_t)std::stoul(tok, nullptr, 16);
            const uint8_t blr[4] = {0x4E, 0x80, 0x00, 0x20};
            a->write_code(at, blr, 4);
            printf("   (stock reference: %s returns at once, as in this image)\n", tok.c_str());
        }
    }
    // FW_VERSION_FULL + FW_VERSION_SITES (fwext.py: "lis-address addi-address" lines): the version the image shows,
    // longer than version_string; the reference gets it in spare RAM and the same instructions load it from there
    if (const char* full = getenv("FW_VERSION_FULL"); full && getenv("FW_VERSION_SITES")) {
        const uint32_t AT = 0x00FFF000;                      // the top of bank 0: untouched by the boot frames compared
        std::string s = full;
        a->write(AT, reinterpret_cast<const uint8_t*>(s.c_str()), s.size() + 1);
        std::ifstream f(getenv("FW_VERSION_SITES"));
        unsigned hi_at, lo_at, n = 0;
        while (f >> std::hex >> hi_at >> lo_at) {
            uint32_t hi = a->r32(hi_at), lo = a->r32(lo_at);
            hi = (hi & 0xFFFF0000u) | (((AT + 0x8000) >> 16) & 0xFFFF);
            lo = (lo & 0xFFFF0000u) | (AT & 0xFFFF);
            uint8_t b[4] = {(uint8_t)(hi >> 24), (uint8_t)(hi >> 16), (uint8_t)(hi >> 8), (uint8_t)hi};
            a->write_code(hi_at, b, 4);
            uint8_t c[4] = {(uint8_t)(lo >> 24), (uint8_t)(lo >> 16), (uint8_t)(lo >> 8), (uint8_t)lo};
            a->write_code(lo_at, c, 4);
            n++;
        }
        printf("   (stock reference showing \"%s\" through %u load(s), as this image)\n", full, n);
    }
    a->run_app(stock_insns);
    auto da = distinct(a->frames), db = distinct(b->frames);
    size_t n = std::min(da.size(), db.size()), same = 0;
    for (size_t i = 0; i < n; i++)
        same += da[i] == db[i];
    bool tasks = py_tasks(*a) == py_tasks(*b);
    printf("2. stock dump: %zu distinct frames, tasks %s\n", da.size(), py_tasks(*a).c_str());
    printf("   this image: %zu distinct frames, tasks %s\n", db.size(), py_tasks(*b).c_str());
    printf("   %zu of the first %zu distinct frames identical%s\n", same, n,
           getenv("MASK_ROWS") && getenv("EEPROM_A0") ? fmt(" (rows %s left out)", getenv("MASK_ROWS")).c_str() : "");
    auto wd = [](const Board& x) { auto it = x.events.find("watchdog would reset"); return it == x.events.end() ? 0ull : (unsigned long long)it->second; };
    printf("3. software watchdog: stock %llu expiries, this image %llu (each one is a reset on the real board)\n",
           wd(*a), wd(*b));
    bool ok = bad.empty() && tasks && n > 0 && same == n && wd(*b) == 0;
    printf("RESULT: %s\n", ok ? "PASS" : "FAIL");
    return ok ? 0 : 1;
}

// ---- resettest: the console's reset runs the firmware's own restart, and never touches NVRAM ----
// ---- apprun: boot a from-OUR-source image (bootloader + our app, no dump) and check the app ran ----
int cmd_apprun(const std::vector<std::string>& args) {
    auto flash = read_file(arg(args, 0, repo().path("build/app/umami.bin")));
    uint32_t addr = (uint32_t)strtoul(arg(args, 1, "2f0000").c_str(), nullptr, 16);
    uint64_t insns = count(args, 2, 5000000);
    Scenario sc;
    sc.geom_word = 0xC240, sc.geom_w = 200, sc.geom_h = 16;
    auto b = std::make_unique<Board>(flash, sc, std::vector<uint8_t>());
    bool booted = b->boot();
    if (booted) {
        b->app.resize(CODE_END);                         // our panel runs on an interrupt: mtmsr/rfi must be modelled
        b->read(0, b->app.data(), CODE_END);
        b->hook_app_specials();
        b->frames_kept = 400;                            // keep a scroll sequence for inspection
        b->run_app(b->icount + insns);
    }
    uint8_t q[4] = {0, 0, 0, 0};
    b->read(addr, q, 4);
    uint32_t v = (uint32_t)q[0] << 24 | q[1] << 16 | q[2] << 8 | q[3];
    printf("boot reached our app: %s (pc %08x); [%08x] = %08x %s\n", booted ? "yes" : "NO",
           (uint32_t)b->pc(), addr, v, v == 0x554D414Du ? "('UMAM')" : "");
    { std::string dir = repo().path("build/app/frames"); std::filesystem::create_directories(dir);
      int n = 0; for (auto& f : b->frames) { char nm[300]; snprintf(nm, sizeof nm, "%s/%03d.rgb555", dir.c_str(), n++);
        FILE* ff = fopen(nm, "wb"); if (ff) { fwrite(f.data(), 1, f.size(), ff); fclose(ff); } } }
    printf("panel frames our app produced: %llu (sequence dumped to build/app/frames/)\n",
           (unsigned long long)b->frame_total);
    bool ok = booted && v == 0x554D414Du;
    printf("RESULT: %s\n", ok ? "PASS" : "FAIL");
    return ok ? 0 : 1;
}

// ---- slotboot: a board whose main flash holds an image in the update slot (as INSTALL / ucommit leave it, try
// counter and flags 0) boots it: our boot loader copies the slot into the RAM stage and the app starts from it ----
int cmd_slotboot(const std::vector<std::string>& args) {
    auto flash = read_file(arg(args, 0, repo().path("bootloader/build-dump/bootflash-fw.bin")));
    auto image = read_file(arg(args, 1, repo().path("bootloader/build-dump/bootflash-fw.bin")));
    Scenario sc;
    sc.geom_word = 0xC240, sc.geom_w = 200, sc.geom_h = 16;
    sc.tries = 0;
    auto b = std::make_unique<Board>(flash, sc, std::vector<uint8_t>());
    std::copy(image.begin(), image.end(), b->main_flash.begin() + 0x1F80000);
    uint8_t zero[2] = {0, 0};
    b->write(NVRAM + 0x7FF0, zero, 2);
    bool ok = b->boot();
    printf("reached the app: %s, pc %08x, %llu instructions\n", ok ? "yes" : "NO", b->pc(),
           (unsigned long long)b->icount);
    std::vector<uint8_t> nv(0x100);
    b->read(NVRAM + 0x7C00, nv.data(), nv.size());
    printf("debug record:");
    for (int i = 0; i < 64; i++)
        printf("%s%02x", i % 16 ? " " : "\n  ", nv[i]);
    printf("\nRESULT: %s\n", ok ? "PASS" : "FAIL");
    return ok ? 0 : 1;
}

// ---- updatetest: a firmware update through our running firmware's console (firmware/ext/update.c: ubegin, uraw
// pieces, ucommit), as the Sign window sends it; the firmware writes its update slot and resets the CPU; a new board
// with the same main flash and NVRAM (a reset keeps both) boots the slot, and the firmware in it comes up healthy ----
// updatescreen: a tiny update (ubegin + 16 bytes, nothing written), then what reaches the panel: the frame handed to
// frame_submit (0x45178) and the panel driver's output (the IDMA frames the GUI shows), each counted
int cmd_updatescreen(const std::vector<std::string>& args) {
    auto flash = read_file(arg(args, 0, repo().path("bootloader/build-dump/bootflash-fw.bin")));
    const int DB9 = 0;              // minor 0 = SCC1 = the DB9 on our from-source firmware
    Scenario sc;
    sc.geom_word = 0xC240, sc.geom_w = 200, sc.geom_h = 16;
    auto b = std::make_unique<Board>(flash, sc, std::vector<uint8_t>());
    if (!b->boot())
        throw std::runtime_error("the boot loader did not reach the app");
    b->app.resize(CODE_END);
    b->read(0, b->app.data(), CODE_END);
    b->hook_app_specials();
    b->hook_busy_waits(repo().symbols);
    b->watch_switches(repo().symbols);
    b->in_app = true;
    std::vector<uint16_t> submitted;
    uint64_t submits = 0;
    b->on_exec(0x45178, [&](Board& bb) {
        uint32_t im = bb.reg(UC_PPC_REG_3), w = bb.r32(im + 4), h = bb.r32(im + 8), px = bb.r32(im + 12);
        submits++;
        if (w * h != 3200)
            return;
        std::vector<uint8_t> raw(6400);
        bb.read(px, raw.data(), 6400);
        submitted.assign(3200, 0);
        for (int q = 0; q < 3200; q++)
            submitted[q] = (uint16_t)(raw[2 * q] << 8 | raw[2 * q + 1]);
    });
    b->run_app(400000000);
    auto& tx = b->serial_tx[DB9];
    auto send = [&](const std::string& l) {
        size_t before = tx.size();
        b->serial_send(DB9, std::vector<uint8_t>(l.begin(), l.end()), 0, b->frame_total + 1);
        uint64_t limit = b->frame_total + 4000;
        while (b->frame_total < limit) {
            b->run_app(b->icount + 2000000);
            std::string t(tx.begin() + before, tx.end());
            if (t.find("mq> ") != std::string::npos)
                break;
        }
    };
    send("MQ?MON\r");
    send("ubegin 10 0\r");
    send("uraw 10\r");
    send("AAAAAAAAAAAAAAAA");
    submits = 0;
    uint64_t f0 = b->frame_total;
    b->run_app(b->icount + 100000000);
    auto count = [](const std::vector<uint16_t>& px, size_t w, const char* what) {
        size_t lit = 0, yellow = 0, green = 0, n = px.size();
        for (size_t q = 0; q < n; q++) {
            uint16_t c = px[q];
            lit += c != 0;
            yellow += c == 0x7FE0;
            green += q >= n - w && c && !(c & 0x7C1F);
        }
        printf("%s: %zu pixels, %zu lit, %zu yellow (text), %zu green in the bottom row (bar)\n", what, n, lit, yellow, green);
    };
    printf("after ubegin + 16 bytes: %llu frame_submit calls, %llu panel frames out\n", (unsigned long long)submits,
           (unsigned long long)(b->frame_total - f0));
    count(submitted, 200, "handed to frame_submit");
    std::vector<uint16_t> out;
    if (!b->frames.empty()) {
        auto& f = b->frames.back();
        for (size_t q = 0; q + 1 < f.size(); q += 2)
            out.push_back((uint16_t)(f[q] << 8 | f[q + 1]));
    }
    count(out, out.size() >= 3200 ? out.size() / 16 : 200, "panel driver output");
    std::string t(tx.begin(), tx.end());
    printf("console: %s\n", t.find("ok") != std::string::npos ? "ubegin/uraw answered ok" : "NO ok");
    return 0;
}

int cmd_updatetest(const std::vector<std::string>& args) {
    auto flash = read_file(arg(args, 0, repo().path("bootloader/build-dump/bootflash-fw.bin")));
    auto image = read_file(arg(args, 1, repo().path("bootloader/build-dump/bootflash-fw.bin")));
    const int DB9 = 0;              // minor 0 = SCC1 = the DB9 on our from-source firmware
    Scenario sc;
    sc.geom_word = 0xC240, sc.geom_w = 200, sc.geom_h = 16;
    auto b = std::make_unique<Board>(flash, sc, std::vector<uint8_t>());
    if (!b->boot())
        throw std::runtime_error("the boot loader did not reach the app");
    b->app.resize(CODE_END);
    b->read(0, b->app.data(), CODE_END);
    b->hook_app_specials();
    b->hook_busy_waits(repo().symbols);
    b->watch_switches(repo().symbols);
    b->in_app = true;
    // the panel during the update: every frame submitted, and those showing the update's progress bar (bottom row)
    uint64_t upd_frames = 0, upd_bar = 0;
    bool upd_watch = false;
    b->on_exec(0x45178, [&](Board& bb) {
        if (!upd_watch)
            return;
        uint32_t im = bb.reg(UC_PPC_REG_3), w = bb.r32(im + 4), h = bb.r32(im + 8), px = bb.r32(im + 12);
        if (w != 200 || h != 16)
            return;
        upd_frames++;
        uint8_t q[2];
        bb.read(px + ((h - 1) * w) * 2, q, 2);
        uint16_t c = (uint16_t)(q[0] << 8 | q[1]);
        upd_bar += c && !(c & 0x7C1F);                      // green only
    });
    b->run_app(400000000);
    // IDLE_DISPLAY=1: as the real sign with nothing playing, the display loop's frames never reach frame_submit (they
    // return at its first instruction; calls from our extension block, past 0xC00000, still go through)
    uint64_t idle_dropped = 0;
    if (getenv("IDLE_DISPLAY"))
        b->on_exec(0x45174, [&](Board& bb) {
            uint32_t lr = bb.reg(UC_PPC_REG_LR);
            if (lr < 0xC00000) {
                idle_dropped++;
                bb.set_reg(UC_PPC_REG_PC, lr);
            }
        });
    if (getenv("LINES_OFF")) {                              // as a sign with an empty store: nothing plays
        uint32_t zero[16] = {};
        b->write(0xE61B8, zero, sizeof zero);
        b->run_app(b->icount + 200000000);
        upd_watch = true;
        b->run_app(b->icount + 200000000);
        upd_watch = false;
        printf("lines off: %llu frames drawn in 200M instructions before the update\n", (unsigned long long)upd_frames);
        upd_frames = upd_bar = 0;
    }
    auto& tx = b->serial_tx[DB9];
    auto prompts = [&] {
        size_t n = 0;
        for (size_t i = 0; i + 4 <= tx.size(); i++)
            n += tx[i] == 'm' && tx[i + 1] == 'q' && tx[i + 2] == '>' && tx[i + 3] == ' ';
        return n;
    };
    auto send = [&](const std::string& l, bool wait_prompt) {
        size_t before = prompts();
        b->serial_send(DB9, std::vector<uint8_t>(l.begin(), l.end()), 0, b->frame_total + 1);
        uint64_t limit = b->frame_total + 8000;
        while (wait_prompt && prompts() == before && b->frame_total < limit && !b->reset_requested)
            b->run_app(b->icount + 2000000);
        return prompts() > before;
    };
    uint32_t crc = host::crc32_bytes(image.data(), image.size());
    bool ok = send("MQ?MON\r", true) && send(fmt("ubegin %zx %x\r", image.size(), crc), true);
    printf("console and ubegin: %s\n", ok ? "ok" : "NO");
    upd_watch = true;
    uint64_t t_send = b->icount;
    size_t piece = getenv("PIECE") ? std::stoul(getenv("PIECE")) : 256;
    for (size_t k = 0; ok && k < image.size(); k += piece) {
        size_t n = std::min<size_t>(piece, image.size() - k);
        std::string l = fmt("uraw %zx\r", n);
        l.append(image.begin() + k, image.begin() + k + n);
        ok = send(l, true);
        if (k / piece == image.size() / piece / 2) {           // halfway: what the panel driver actually sends out
            size_t lit = 0, green = 0, n = 0;
            if (!b->frames.empty()) {
                auto& f = b->frames.back();
                n = f.size() / 2;
                for (size_t q = 0; q < n; q++) {
                    uint16_t c = (uint16_t)(f[2 * q] << 8 | f[2 * q + 1]);
                    lit += c != 0;
                    green += q >= n - 200 && c && !(c & 0x7C1F);
                }
            }
            printf("the panel's output halfway: %zu pixels, %zu lit, %zu green in the bottom row\n", n, lit, green);
        }
    }
    printf("image sent: %s in %.1f emulated seconds\n", ok ? "ok" : "NO", (b->icount - t_send) / (CPU_MIPS * 1e6));
    upd_watch = false;
    printf("the panel while receiving: %llu frames, %llu with the progress bar (display loop frames dropped: %llu)\n",
           (unsigned long long)upd_frames, (unsigned long long)upd_bar, (unsigned long long)idle_dropped);
    send("ucommit\r", true);
    for (int k = 0; k < 40 && !b->reset_requested; k++)    // the restart task: blank the panel, stop, reset
        b->run_app(b->icount + 20000000);
    printf("after ucommit: pc %08x, events:", b->pc());
    for (auto& [k, n] : b->events)
        if (k.find("reset") != std::string::npos || k.find("machine") != std::string::npos)
            printf(" %s=%llu", k.c_str(), (unsigned long long)n);
    printf("\n");
    std::string out(tx.begin(), tx.end());
    bool updated = out.find("updated: restarting") != std::string::npos;
    printf("ucommit: %s; CPU reset requested: %s\n", updated ? "updated" : "NOT updated", b->reset_requested ? "yes" : "no");
    // the reset: a new board, the same main flash and NVRAM
    std::vector<uint8_t> nv(0x8000);
    b->read(NVRAM, nv.data(), nv.size());
    auto mf = b->main_flash;
    // the slot as the firmware and the boot loader place it: just past the firmware's volume 1, on 512 KB
    uint32_t vend = b->r32(0x4508fc + 4) + b->r32(0x450908 + 4), slot = (vend + 0x7FFFF) & ~0x7FFFFu;
    bool slot_same = slot >= 0x50000000 && slot - 0x50000000 + image.size() <= mf.size() &&
                     std::equal(image.begin(), image.end(), mf.begin() + (slot - 0x50000000));
    printf("update slot at %08x\n", slot);
    if (!slot_same && slot >= 0x50000000) {
        size_t diff = 0, first = SIZE_MAX, base = slot - 0x50000000;
        for (size_t i = 0; i < image.size() && base + i < mf.size(); i++)
            if (mf[base + i] != image[i]) {
                diff++;
                if (first == SIZE_MAX)
                    first = i;
            }
        printf("  %zu bytes differ, the first at +%#zx (slot %02x, image %02x)\n", diff, first,
               first == SIZE_MAX ? 0 : mf[base + first], first == SIZE_MAX ? 0 : image[first]);
    }
    for (size_t o = 0; o + image.size() <= mf.size(); o += 0x10000)
        if (std::equal(image.begin(), image.begin() + 4096, mf.begin() + o)) {
            printf("the image's first 4 KB are at main flash +%#zx (%#zx)%s\n", o, 0x50000000 + o,
                   std::equal(image.begin(), image.end(), mf.begin() + o) ? ", all of it" : "");
        }
    printf("the firmware's volume 1: base %08x size %08x\n", b->r32(0x4508fc + 4), b->r32(0x450908 + 4));
    printf("update slot holds the image: %s; NVRAM tries %02x flags %02x\n", slot_same ? "yes" : "NO", nv[0x7FF0], nv[0x7FF1]);
    auto c = std::make_unique<Board>(flash, sc, std::vector<uint8_t>());
    c->main_flash = mf;
    c->write(NVRAM, nv.data(), nv.size());
    bool booted = c->boot();
    printf("after the reset: the boot reached the app: %s (pc %08x)\n", booted ? "yes" : "NO", c->pc());
    bool healthy = false;
    if (booted) {
        c->app.resize(CODE_END);
        c->read(0, c->app.data(), CODE_END);
        c->hook_app_specials();
        c->hook_busy_waits(repo().symbols);
        c->watch_switches(repo().symbols);
        c->in_app = true;
        c->run_app(c->icount + 3000000000ull);
        std::vector<uint8_t> rec(0x100);
        c->read(NVRAM + 0x7C00, rec.data(), rec.size());
        uint8_t flags = 0;
        c->read(NVRAM + 0x7FF1, &flags, 1);
        // booted from the slot: the firmware's boot volume is the RAM stage (the boot loader's hand-over at 0x2C);
        // healthy: past its 1200 frames, the firmware cleared the slot flag (firmware/ext/slotboot.c)
        bool from_slot = c->r32(0x2C) == STAGE_VIRT, cleared = !(flags & 0x02);
        printf("booted from the update slot: %s (boot volume %08x); slot flag cleared after 1200 frames: %s; "
               "frames drawn: %zu\n", from_slot ? "yes" : "NO", c->r32(0x2C), cleared ? "yes" : "NO", c->frames.size());
        healthy = from_slot && cleared && c->frames.size() > 1200;
        // its console answers
        std::string hello = "MQ?MON\r";
        c->serial_send(DB9, std::vector<uint8_t>(hello.begin(), hello.end()), 0, c->frame_total + 1);
        for (int k = 0; k < 40; k++)
            c->run_app(c->icount + 20000000);
        std::string out(c->serial_tx[DB9].begin(), c->serial_tx[DB9].end());
        bool console = out.find("mq> ") != std::string::npos;
        printf("its console answers: %s (%zu bytes out on the DB9: %s)\n", console ? "yes" : "NO", out.size(),
               out.substr(0, 60).c_str());
        healthy = healthy && console;
    }
    bool pass = ok && updated && b->reset_requested && slot_same && booted && healthy;
    printf("RESULT: %s\n", pass ? "PASS" : "FAIL");
    return pass ? 0 : 1;
}

int cmd_resettest(const std::vector<std::string>& args) {
    const int DB9 = 0;                                     // minor 0 = SCC1 = the DB9 on our from-source board
    auto flash = read_file(arg(args, 0, repo().path("bootloader/build/bootflash-fw.bin")));
    auto app = read_file(repo().path("analysis/build/fw/ELITE_RGB-ext"));
    auto b = booted(flash, app);
    b->load_app_image();
    std::vector<uint8_t> nv0(0x80000), nv1(0x80000);
    b->run_app(400000000);                                 // the app is up and has written its NVRAM
    b->read(0xF8780000, nv0.data(), nv0.size());
    for (const char* l : {"MQ?MON\r", "reset\r"})
        b->serial_send(DB9, std::vector<uint8_t>(l, l + strlen(l)), 0, b->frame_total + 10);
    for (uint64_t at = 400000000; at < 1000000000 && !b->reset_requested; at += 10000000)
        b->run_app(at);
    b->read(0xF8780000, nv1.data(), nv1.size());
    auto& tx = b->serial_tx[DB9];
    std::string text(tx.begin(), tx.end());
    uint8_t q[4];
    b->read(0x45C00C, q, 4);
    uint32_t flags = (uint32_t)q[0] << 24 | q[1] << 16 | q[2] << 8 | q[3];
    size_t diff = 0;
    for (size_t i = 0; i < nv0.size(); i++)
        diff += nv0[i] != nv1[i];
    bool said = text.find("resetting") != std::string::npos;
    printf("console said 'resetting': %s; CPU reset requested (PLPRCR CSR): %s; restart flags %08x; "
           "NVRAM bytes changed after the reset command: %zu\n", said ? "yes" : "no",
           b->reset_requested ? "yes" : "no", flags, diff);
    bool ok = said && b->reset_requested && flags == 0 && diff == 0;
    printf("RESULT: %s\n", ok ? "PASS" : "FAIL");
    return ok ? 0 : 1;
}

// ---- slotflagtest: after 1200 panel frames the firmware clears the update slot's try count (firmware/ext/slotboot.c)
// when the boot loader's slot flag (bit 1) is set, and leaves it alone otherwise ----
int cmd_slotflagtest(const std::vector<std::string>& args) {
    auto flash = read_file(arg(args, 0, repo().path("bootloader/build/bootflash-fw.bin")));
    auto app = read_file(repo().path("analysis/build/fw/ELITE_RGB-ext"));
    const uint32_t TRIES = 0xF8787FF0;                     // NVRAM end - 0x10 / - 0xF (bootloader/include/board.h)
    bool ok = true;
    for (uint8_t flag : {uint8_t(2), uint8_t(0)}) {
        auto b = booted(flash, app);
        b->load_app_image();
        uint8_t set[2] = {2, flag}, got[2] = {0, 0};
        b->write(TRIES, set, 2);
        uint64_t at = 0;
        auto fw_frames = [&] {                             // ledpanel_frame_count: the firmware's own count
            uint8_t c[4];
            b->read(0xBDB28, c, 4);
            return (uint32_t)c[0] << 24 | c[1] << 16 | c[2] << 8 | c[3];
        };
        while (fw_frames() < 1250 && at < 12000000000ull)
            b->run_app(at += 50000000);
        b->read(TRIES, got, 2);
        uint8_t w[12];
        b->read(0xBDB28, w, 4);                            // ledpanel_frame_count
        b->read(0x24, w + 4, 8);                           // hand-off: NVRAM base, size
        auto be = [&](int o) { return (uint32_t)w[o] << 24 | w[o + 1] << 16 | w[o + 2] << 8 | w[o + 3]; };
        printf("  firmware frame count %u, NVRAM %08x + %x\n", be(0), be(4), be(8));
        bool want = flag ? (got[0] == 0 && got[1] == 0) : (got[0] == 2 && got[1] == 0);
        printf("flag %u: after %llu frames count %u flags %u -> %s\n", flag, (unsigned long long)b->frame_total, got[0],
               got[1], want ? "as expected" : "WRONG");
        ok = ok && want && fw_frames() >= 1200;
    }
    printf("RESULT: %s\n", ok ? "PASS" : "FAIL");
    return ok ? 0 : 1;
}


// ---- nvram: the firmware's NVRAM block chain (nvram_block_get, 0x5EFEC) once the app is up: each block's name and
// size, and the room left for new blocks (NVRAM base at *0x24, size at *0x28, the last 0x410 bytes reserved) ----
int cmd_nvram(const std::vector<std::string>& args) {
    auto flash = read_file(arg(args, 0, repo().path("bootloader/build/bootflash-fw.bin")));
    auto app = read_file(repo().path("analysis/build/fw/ELITE_RGB-ext"));
    Scenario sc;
    sc.geom_word = 0xC240, sc.geom_w = 200, sc.geom_h = 16;
    auto b = std::make_unique<Board>(flash, sc, app);
    if (!b->boot())
        throw std::runtime_error("the boot loader did not reach the app");
    b->hook_app_specials();
    b->hook_busy_waits(repo().symbols);
    b->watch_switches(repo().symbols);
    b->load_app_image();
    b->run_app(600000000);
    uint32_t base = b->r32(0x24), size = b->r32(0x28), p = base, used = 0;
    printf("NVRAM %08x, %u bytes (%u KB)\n", base, size, size / 1024);
    for (int k = 0; k < 200; k++) {
        uint8_t h = 0;
        b->read(p, &h, 1);
        if (!h || p + h + 8 > base + size)
            break;
        char name[64] = {};
        b->read(p + 1, (uint8_t*)name, std::min<uint32_t>(h - 9, 63));
        uint32_t len = b->r32(p + h - 8);
        printf("  %08x  %-28s %7u bytes\n", p, name, len);
        p += h + len;
        used = p - base;
        if (len > size)
            break;
    }
    printf("used %u bytes; room for new blocks %d bytes (%d KB)\n", used, (int)(size - 0x410 - used),
           (int)(size - 0x410 - used) / 1024);
    return 0;
}


int cmd_consoledump(const std::vector<std::string>& args) {
    if (args.size() < 3) {
        fprintf(stderr, "usage: umami-marquee-sim-test consoledump OUTDIR CMDS.txt FRAMES [FLASH.bin]\n");
        return 1;
    }
    std::string dir = args[0];
    std::vector<std::string> lines;
    {
        auto raw = read_file(args[1]);
        std::string cur;
        for (uint8_t c : raw) {
            if (c == '\n') {
                if (!cur.empty())
                    lines.push_back(cur);
                cur.clear();
            } else if (c != '\r')
                cur += (char)c;
        }
        if (!cur.empty())
            lines.push_back(cur);
    }
    size_t nframes = std::stoul(args[2]);
    uint32_t T = 0;
    auto flash = read_file(arg(args, 3, repo().path("bootloader/build/bootflash-fw.bin")));
    auto app = read_file(repo().path("analysis/build/fw/ELITE_RGB-ext"));
    const int DB9 = 0;              // minor 0 = SCC1 = the DB9 on our from-source firmware
    Scenario sc;
    sc.geom_word = 0xC240, sc.geom_w = 200, sc.geom_h = 16;
    auto b = std::make_unique<Board>(flash, sc, app);
    const uint32_t LEDPANEL_FRAME_COUNT = 0xBDB28;
    std::vector<uint8_t> drawn;
    bool capture = false;
    // every frame the display loop draws (frame_submit, at 0x45178: past its first instruction, which our firmware
    // patches into its take-over, so the take-over has done its work; r3 = the image), with the frame counter then: the
    // panel repeats a frame when drawing one takes longer than a frame time (outlines do on this panel)
    b->on_exec(0x45178, [&drawn, &capture, LEDPANEL_FRAME_COUNT](Board& bb) {
        if (!capture)
            return;
        uint32_t im = bb.reg(UC_PPC_REG_3), w = bb.r32(im + 4), h = bb.r32(im + 8), px = bb.r32(im + 12);
        if (w != 200 || h != 16)
            return;
        uint32_t c = bb.r32(LEDPANEL_FRAME_COUNT);
        uint8_t le[4] = {(uint8_t)c, (uint8_t)(c >> 8), (uint8_t)(c >> 16), (uint8_t)(c >> 24)};
        drawn.insert(drawn.end(), le, le + 4);
        size_t at = drawn.size();
        drawn.resize(at + 6400);
        bb.read(px, drawn.data() + at, 6400);
    });
    // CONSOLEDUMP_TRACE=ADDR,ADDR,... (hex): how often each ran while capturing, printed at the end (debugging aid)
    std::vector<std::pair<uint32_t, uint64_t>> traced;
    if (const char* tr = getenv("CONSOLEDUMP_TRACE")) {
        std::string t = tr;
        for (size_t i = 0; i < t.size();) {
            size_t j = t.find(',', i);
            traced.push_back({(uint32_t)std::stoul(t.substr(i, j - i), nullptr, 16), 0});
            i = j == std::string::npos ? t.size() : j + 1;
        }
    }
    traced.reserve(traced.size());
    for (size_t k = 0; k < traced.size(); k++)
        b->on_exec(traced[k].first, [&traced, &capture, k](Board&) { if (capture) traced[k].second++; });
    if (!b->boot())
        throw std::runtime_error("the boot loader did not reach the app");
    b->hook_app_specials();
    b->hook_busy_waits(repo().symbols);
    b->watch_switches(repo().symbols);
    b->load_app_image();
    b->run_app(400000000);
    std::vector<std::string> cmds = {"MQ?MON"};
    for (auto& l : lines) {
        cmds.push_back(l);
        if (l.rfind("time ", 0) == 0)
            T = (uint32_t)std::stoul(l.substr(5), nullptr, 16);
    }
    cmds.push_back("exit");
    auto& tx = b->serial_tx[DB9];
    auto prompts = [&] {
        size_t n = 0;
        for (size_t i = 0; i + 4 <= tx.size(); i++)
            n += tx[i] == 'm' && tx[i + 1] == 'q' && tx[i + 2] == '>' && tx[i + 3] == ' ';
        return n;
    };
    for (auto& c : cmds) {
        size_t before = prompts();
        std::string l = c + "\r";
        if (c.rfind("sraw ", 0) == 0) {                    // "sraw HEX": "sraw N" and the bytes, raw
            std::vector<uint8_t> raw;
            for (size_t i = 5; i + 1 < c.size(); i += 2)
                raw.push_back((uint8_t)std::stoul(c.substr(i, 2), nullptr, 16));
            l = fmt("sraw %zx\r", raw.size());
            l.append(raw.begin(), raw.end());
        }
        b->serial_send(DB9, std::vector<uint8_t>(l.begin(), l.end()), 0, b->frame_total + 1);
        uint64_t limit = b->frame_total + 4000;
        while (prompts() == before && b->frame_total < limit && c != "exit")
            b->run_app(b->icount + 2000000);
    }
    size_t first = b->frames.size();
    std::vector<uint32_t> fc;
    capture = true;
    size_t want = first + nframes;
    while (b->frames.size() < want && b->icount < 60000000000ull) {
        size_t n = b->frames.size();
        b->run_app(b->icount + 100000);
        for (size_t i = n; i < b->frames.size(); i++)
            fc.push_back(b->r32(LEDPANEL_FRAME_COUNT));
    }
    for (auto& [addr, n] : traced)
        printf("trace %08x: %llu\n", addr, (unsigned long long)n);
    if (const char* st = getenv("CONSOLEDUMP_WORDS")) {         // ADDR (hex): 16 words there at the end
        uint32_t a0 = (uint32_t)std::stoul(st, nullptr, 16);
        printf("words %08x:", a0);
        for (int i = 0; i < 16; i++)
            printf(" %08x", b->r32(a0 + 4 * i));
        printf("\n");
    }
    std::filesystem::create_directories(dir);
    FILE* f = fopen((dir + "/frames.bin").c_str(), "wb");
    for (size_t k = first; k < b->frames.size(); k++) {
        auto& fr = b->frames[k];
        uint32_t n = (uint32_t)fr.size();
        uint8_t le[4] = {(uint8_t)n, (uint8_t)(n >> 8), (uint8_t)(n >> 16), (uint8_t)(n >> 24)};
        fwrite(le, 1, 4, f);
        fwrite(fr.data(), 1, fr.size(), f);
    }
    fclose(f);
    f = fopen((dir + "/drawn.bin").c_str(), "wb");      // each: u32 frame counter (little-endian), 200 x 16 RGB555 BE
    fwrite(drawn.data(), 1, drawn.size(), f);
    fclose(f);
    f = fopen((dir + "/meta.json").c_str(), "w");
    fprintf(f, "{\"w\": 200, \"h\": 16, \"time\": %u, \"fc\": [", T);
    for (size_t i = 0; i < fc.size(); i++)
        fprintf(f, "%s%u", i ? ", " : "", fc[i]);
    fprintf(f, "], \"lines\": [");
    for (int i = 0; i < 16; i++) {
        uint8_t t[256] = {};
        b->read(b->r32(MESSAGE_TABLE_PTR) + 256 * i, t, 256);
        fprintf(f, "%s\"", i ? ", " : "");
        for (int j = 0; j < 256 && t[j]; j++)
            fprintf(f, "%02x", t[j]);
        fprintf(f, "\"");
    }
    fprintf(f, "]}\n");
    fclose(f);
    std::string out(tx.begin(), tx.end());
    {
        uint8_t nfont = 0;
        b->read(0x1BF020, &nfont, 1);
        printf("font_count %u:", nfont);
        for (unsigned i = 0; i < nfont && i < 100; i++) {
            char nm[33] = {};
            b->read(0x192AC0 + i * 0x718 + 0x618, (uint8_t*)nm, 32);
            printf(" %u=%s", i, nm);
        }
        printf("\n");
    }
    {
        FILE* lf = fopen((dir + "/console.txt").c_str(), "wb");
        fwrite(out.data(), 1, out.size(), lf);
        fclose(lf);
    }
    printf("%s\n%zu frames after the commands -> %s\n", out.find("committed") != std::string::npos ? "committed" : "(no commit)",
           b->frames.size() - first, dir.c_str());
    return 0;
}

// ---- attractdump OUTDIR: the real firmware's attract loop with three lines, every panel frame and the firmware's
// frame counter (ledpanel_frame_count) at it, for the web Studio's emulation to be checked against
// (frontend/packages/sign-emu/test/firmware.test.ts) ----
int cmd_attractdump(const std::vector<std::string>& args) {
    if (args.empty()) {
        fprintf(stderr, "usage: umami-marquee-sim-test attractdump OUTDIR [FLASH.bin]\n");
        return 1;
    }
    std::string dir = args[0];
    auto flash = read_file(arg(args, 1, repo().path("bootloader/build/bootflash-fw.bin")));
    auto app = read_file(repo().path("analysis/build/fw/ELITE_RGB-ext"));
    const std::string E = "\x1b";
    std::vector<AttractLine> lines = {
        {0, "ONE", 0x7E800000u},                                                      // amber, a plain scroll
        {1, E + "`\xff" + E + "@\xff" + E + "P\x06\xff" + "TWO", 0x03E00000u},       // green, centred and held
        {2, E + "`\xff" + E + "@\xff" + E + "P\x04\xff" + "THREE", 0x80020000u},     // diagonal stripes (phase)
        // per-character effects (in-text commands, 0x1F): motions, a fill colour, a fill animation, on/off times
        // (0x1F as "\x1F" "...": a C hex escape would swallow the hex digits after it)
        {3, std::string("\x1F" "m1WAVY \x1F" "m2HOPS \x1F" "m3SHAKE \x1F" "o0302\x1F" "m4BLINK\x1F" "o0000 \x1F" "m0") +
                "\x1F" "f7C00RED \x1F" "fC003FIRE\x1F" "f- \x1F" "o0804\x1F" "m1\x1F" "f03E0PULSE\x1F" "o0000\x1F" "m0" +
                "\x1F" "f- STILL",
         0x7FE00000u},
    };
    Scenario sc;                                         // this sign's model: ED0501, 200 x 16 (config EEPROM 0xC240)
    sc.geom_word = 0xC240, sc.geom_w = 200, sc.geom_h = 16;
    auto b = std::make_unique<Board>(flash, sc, app);
    if (!b->boot())
        throw std::runtime_error("the boot loader did not reach the app");
    b->hook_app_specials();
    b->hook_busy_waits(repo().symbols);
    b->watch_switches(repo().symbols);
    b->load_app_image();
    while (!b->r32(MESSAGE_TABLE_PTR) && b->icount < 2000000000ull)
        b->run_app(b->icount + 5000000);
    write_attract_lines(*b, lines);
    std::vector<uint32_t> fc;
    const uint32_t LEDPANEL_FRAME_COUNT = 0xBDB28;
    // every frame the display loop draws, with the frame counter at that moment (frame_submit, at 0x45178: past the
    // instruction our firmware patches): the effects' phase, exactly, whatever the panel's queue holds at the time
    std::vector<uint8_t> drawn;
    b->on_exec(0x45178, [&drawn, LEDPANEL_FRAME_COUNT](Board& bb) {
        uint32_t im = bb.reg(UC_PPC_REG_3), w = bb.r32(im + 4), h = bb.r32(im + 8), px = bb.r32(im + 12);
        if (w != 200 || h != 16)
            return;
        uint32_t c = bb.r32(LEDPANEL_FRAME_COUNT);
        uint8_t le[4] = {(uint8_t)c, (uint8_t)(c >> 8), (uint8_t)(c >> 16), (uint8_t)(c >> 24)};
        drawn.insert(drawn.end(), le, le + 4);
        size_t at = drawn.size();
        drawn.resize(at + 6400);
        bb.read(px, drawn.data() + at, 6400);
    });
    size_t want = 1400;
    while (b->frames.size() < want && b->icount < 40000000000ull) {
        size_t n = b->frames.size();
        b->run_app(b->icount + 100000);
        for (size_t i = n; i < b->frames.size(); i++)
            fc.push_back(b->r32(LEDPANEL_FRAME_COUNT));
    }
    std::filesystem::create_directories(dir);
    FILE* f = fopen((dir + "/frames.bin").c_str(), "wb");     // each: u32 length (little-endian), then the bytes
    for (auto& fr : b->frames) {
        uint32_t n = (uint32_t)fr.size();
        uint8_t le[4] = {(uint8_t)n, (uint8_t)(n >> 8), (uint8_t)(n >> 16), (uint8_t)(n >> 24)};
        fwrite(le, 1, 4, f);
        fwrite(fr.data(), 1, fr.size(), f);
    }
    fclose(f);
    f = fopen((dir + "/drawn.bin").c_str(), "wb");      // each: u32 frame counter (little-endian), 200 x 16 RGB555 BE
    fwrite(drawn.data(), 1, drawn.size(), f);
    fclose(f);
    f = fopen((dir + "/meta.json").c_str(), "w");
    fprintf(f, "{\"w\": 200, \"h\": 16, \"frames\": %zu, \"fc\": [", b->frames.size());
    for (size_t i = 0; i < fc.size(); i++)
        fprintf(f, "%s%u", i ? ", " : "", fc[i]);
    fprintf(f, "], \"lines\": [");
    for (size_t i = 0; i < lines.size(); i++) {
        fprintf(f, "%s{\"line\": %d, \"effect\": %u, \"data\": \"", i ? ", " : "", lines[i].line, lines[i].effect);
        for (unsigned char c : lines[i].text)
            fprintf(f, "%02x", c);
        fprintf(f, "\"}");
    }
    fprintf(f, "]}\n");
    fclose(f);
    printf("%zu frames (RGB555 big-endian) and the frame counter -> %s\n", b->frames.size(), dir.c_str());
    return 0;
}

// ---- attracttest: lines put into the firmware's own attract configuration (attract_lines.h) are shown by the stock
// attract loop, each in its own colour, and a rewritten line shows its new text ----
int cmd_attracttest(const std::vector<std::string>& args) {
    auto flash = read_file(arg(args, 0, repo().path("bootloader/build/bootflash-fw.bin")));
    auto app = read_file(repo().path("analysis/build/fw/ELITE_RGB-ext"));
    const uint32_t AMBER = 0x7E80u << 16, GREEN = 0x03E0u << 16, RED = 0x7C00u << 16;
    std::vector<AttractLine> first = {{0, "03:07", AMBER}, {1, "UMAMI", GREEN}, {2, "42", RED}};
    std::vector<AttractLine> later = {{0, "03:08", AMBER}, {1, "UMAMI", GREEN}, {2, "43", RED}};
    auto b = booted(flash, app);
    b->load_app_image();
    while (!b->r32(MESSAGE_TABLE_PTR) && b->icount < 2000000000ull)   // the firmware sets up its message table
        b->run_app(b->icount + 5000000);
    bool ok = write_attract_lines(*b, first);
    printf("lines written: %s\n", ok ? "yes" : "NO (no message table)");
    auto line_text = [&](int i) {
        char t[257] = {};
        b->read(b->r32(MESSAGE_TABLE_PTR) + 256 * i, t, 256);
        return std::string(t);
    };
    bool texts = line_text(0) == "03:07" && line_text(1) == "UMAMI" && line_text(2) == "42";
    printf("message table: [%s] [%s] [%s]: %s\n", line_text(0).c_str(), line_text(1).c_str(), line_text(2).c_str(),
           texts ? "as written" : "WRONG");
    // the same board without the lines: deterministic, so its frames match until the lines show; only frames from
    // where the two differ count (the intro and logo loop light colours of their own)
    auto base = booted(flash, app);
    base->load_app_image();
    while (!base->r32(MESSAGE_TABLE_PTR) && base->icount < 2000000000ull)
        base->run_app(base->icount + 5000000);
    write_attract_lines(*base, {});
    bool amber = false, green = false, red = false;
    size_t diverged = SIZE_MAX, scanned = 0;
    auto run_to = [](Board& x, size_t frames) {          // by frame count: both boards stay frame-aligned
        for (int guard = 0; x.frames.size() < frames && guard < 20000; guard++)
            x.run_app(x.icount + 20000000);
    };
    for (size_t want = 100; want <= 6000 && !(amber && green && red); want += 100) {
        run_to(*b, want);
        run_to(*base, want);
        size_t n = std::min(b->frames.size(), base->frames.size());
        if (diverged == SIZE_MAX)
            for (size_t f = scanned; f < n; f++)
                if (b->frames[f] != base->frames[f]) {
                    diverged = f;
                    break;
                }
        if (diverged != SIZE_MAX)
            for (size_t f = std::max(diverged, scanned); f < n; f++) {
                auto& fr = b->frames[f];
                for (size_t i = 0; i + 1 < fr.size(); i += 2) {
                    uint16_t px = (uint16_t)(fr[i] << 8 | fr[i + 1]) & 0x7FFF;
                    int r = px >> 10 & 31, g = px >> 5 & 31, bl = px & 31;
                    if (!px || bl)
                        continue;
                    if (r && g && r > g)
                        amber = true;
                    else if (g && !r)
                        green = true;
                    else if (r && !g)
                        red = true;
                }
            }
        scanned = n;
    }
    printf("the lines show from frame %zu (the board without them differs there)\n", diverged);
    printf("lit on the panel by the lines: amber %s, green %s, red %s (%zu frames)\n", amber ? "yes" : "no",
           green ? "yes" : "no", red ? "yes" : "no", b->frames.size());
    write_attract_lines(*b, later);
    bool tick = line_text(0) == "03:08" && line_text(2) == "43";
    printf("rewritten: [%s] [%s]: %s\n", line_text(0).c_str(), line_text(2).c_str(), tick ? "updated" : "NOT");
    bool pass = ok && texts && amber && green && red && tick && diverged != SIZE_MAX;
    printf("RESULT: %s\n", pass ? "PASS" : "FAIL");
    return pass ? 0 : 1;
}

// ---- dbgtest: with our boot loader, the NVRAM debug record (bootloader/include/dbg.h) shows the firmware's steps,
// the DTLB dead end is redirected, and a crash lands in the record as well as on the DB9 ----
int cmd_dbgtest(const std::vector<std::string>& args) {
    auto flash = read_file(arg(args, 0, repo().path("bootloader/build-dump/bootflash-fw.bin")));
    auto app = read_file(repo().path("analysis/build/fw/ELITE_RGB-ext"));
    const uint32_t REC = 0xF8780000 + 0x7C00;
    auto rd32 = [](Board& b, uint32_t a) {
        uint8_t q[4];
        b.read(a, q, 4);
        return (uint32_t)q[0] << 24 | q[1] << 16 | q[2] << 8 | q[3];
    };
    auto cur_step = [&](Board& b) {
        uint32_t seq = rd32(b, REC + 4), e = REC + 0x10 + 16 * (seq & 7);
        uint8_t q[2];
        b.read(e + 8, q, 2);
        return std::make_pair(seq, (uint32_t)(q[0] << 8 | q[1]));
    };
    bool ok = true;
    {
        auto b = booted(flash, app);
        b->load_app_image();
        uint64_t at = 0;
        std::set<uint32_t> seen;
        while (rd32(*b, 0xBDB28) < 1250 && at < 12000000000ull) {
            b->run_app(at += 20000000);
            seen.insert(cur_step(*b).second);
        }
        auto [seq, step] = cur_step(*b);
        uint32_t hang = rd32(*b, 0x1284);
        printf("record magic %08x, boot %u, steps seen:", rd32(*b, REC), seq);
        for (auto v : seen)
            printf(" %02x", v);
        printf("; last %02x; 0x1284 = %08x\n", step, hang);
        bool good = rd32(*b, REC) == 0x4D514442 && step == 0x4F && seen.count(0x43) && hang != 0x48000000 &&
                    (hang >> 26) == 18;
        printf("  steps up to 'healthy', the DTLB dead end redirected: %s\n", good ? "yes" : "NO");
        ok = ok && good;
    }
    {
        auto b = booted(flash, app);
        b->load_app_image();
        b->run_app(100000000);
        while (cur_step(*b).second < 0x41 && b->icount < 4000000000ull)   // crash reports on (firmware vectors)
            b->run_app(b->icount + 10000000);
        uint32_t pc = 0, c = REC + 0x100;
        for (int attempt = 0; attempt < 5 && rd32(*b, c) != 0x4D514352; attempt++) {
            // a task's code that really runs: not an exception vector, and not a loop the simulator fast-forwards
            do
                b->run_app(b->icount + 1000 + 7919 * attempt);
            while (b->pc() < 0x4000);
            pc = (uint32_t)b->pc();
            uint32_t orig = rd32(*b, pc), illegal = 0;
            b->write(pc, &illegal, 4);
            uc_ctl_remove_cache(b->uc, pc, pc + 4);        // Unicorn's translated copy of that code, if any
            b->run_app(b->icount + 30000000);              // run_app takes an absolute count (busy-waits skip ahead)
            if (rd32(*b, c) != 0x4D514352) {               // it did not run: put it back and try elsewhere
                uint8_t q[4] = {(uint8_t)(orig >> 24), (uint8_t)(orig >> 16), (uint8_t)(orig >> 8), (uint8_t)orig};
                b->write(pc, q, 4);
                uc_ctl_remove_cache(b->uc, pc, pc + 4);
            }
            printf("  fault planted at %08x (step %02x): %s\n", pc, cur_step(*b).second,
                   rd32(*b, c) == 0x4D514352 ? "taken" : "not run, next");
        }
        bool good = rd32(*b, c) == 0x4D514352 && rd32(*b, c + 8) == 3 && rd32(*b, c + 12) == 0x700 &&
                    rd32(*b, c + 0x10) == pc;
        printf("crash at %08x: record magic %08x source %u vector %x srr0 %08x step %02x: %s\n", pc, rd32(*b, c),
               rd32(*b, c + 8), rd32(*b, c + 12), rd32(*b, c + 0x10), rd32(*b, c + 0x28), good ? "recorded" : "NOT");
        ok = ok && good;
    }
    {   // two stages: a stage 1 in the loader sector (bootloader/include/serdump.h) boots the firmware to healthy
        const char* s1p = getenv("STAGE1");
        auto s1 = read_file(s1p ? s1p : repo().path("bootloader/build-dump-s1/stage1.img"));
        uint32_t loader = 0;
        {
            auto b = booted(flash, app);                   // where the boot loader puts the loader sector
            uint32_t base = rd32(*b, 0x44), shown = rd32(*b, 0x48);
            loader = shown ? base + shown : rd32(*b, 0x3C) + rd32(*b, 0x40);
        }
        auto b = std::make_unique<Board>(flash, Scenario{}, app);
        if (const char* e = getenv("EEPROM_A0"))           // the real sign's EEPROMs, as booted() takes them
            b->eeproms[0xA0] = read_file(e);
        if (const char* e = getenv("EEPROM_A2"))
            b->eeproms[0xA2] = read_file(e);
        b->write(loader, s1.data(), s1.size());
        if (!b->boot())
            throw std::runtime_error("the boot loader did not reach the app with stage 1");
        b->hook_app_specials();
        b->hook_busy_waits(repo().symbols);
        b->watch_switches(repo().symbols);
        if (getenv("EEPROM_A0"))
            b->fake_fpga(repo().symbols);
        b->load_app_image();
        uint64_t at = 0;
        std::set<uint32_t> seen;
        while (rd32(*b, 0xBDB28) < 1250 && at < 12000000000ull) {
            b->run_app(at += 20000000);
            seen.insert(cur_step(*b).second);
        }
        uint8_t t[1];
        b->read(REC + 9, t, 1);
        auto [seq, step] = cur_step(*b);
        bool good = step == 0x4F && t[0] == 0;
        printf("stage 1 at %08x (%zu bytes): last step %02x, stage-1 tries %u: %s\n", loader, s1.size(), step, t[0],
               good ? "booted to healthy through stage 1" : "NOT");
        ok = ok && good;
    }
    printf("RESULT: %s\n", ok ? "PASS" : "FAIL");
    return ok ? 0 : 1;
}

// ---- crashtest: a fault in a running task gives a crash report on the DB9 (firmware/ext/crash.c) ----
int cmd_crashtest(const std::vector<std::string>& args) {
    if (args.size() < 2) {
        fprintf(stderr, "usage: umami-marquee-sim-test crashtest FLASH.bin APP [INSNS]\n");
        return 1;
    }
    auto flash = read_file(args[0]), app = read_file(args[1]);
    uint64_t at = count(args, 2, 100000000);
    auto b = booted(flash, app);
    b->load_app_image();
    b->run_app(at);
    while (b->pc() < 0x4000) {                             // a task's code, not an exception vector
        at += 1000;
        b->run_app(at);
    }
    uint32_t pc = (uint32_t)b->pc(), illegal = 0;
    b->write_code(pc, &illegal, 4);
    b->run_app(at + 30000000);
    uint32_t minor = 0;
    {
        uint8_t q[4];
        b->read(0x14A14C, q, 4);
        minor = (uint32_t)q[0] << 24 | q[1] << 16 | q[2] << 8 | q[3];
    }
    auto& out = b->serial_tx[(int)minor];
    std::string s(out.begin(), out.end());
    size_t k = s.find("MQCRASH 1"), e = s.find("MQCRASH END");
    printf("illegal instruction planted at %08x; the DB9 (minor %u) sent %zu bytes\n", pc, minor, out.size());
    if (k != std::string::npos)
        printf("%s\n", s.substr(k, e == std::string::npos ? std::string::npos : e + 11 - k).c_str());
    bool ok = k != std::string::npos && e != std::string::npos &&
              s.find(fmt("vector 06 program pc %08x", pc)) != std::string::npos && s.find("\ntask ") != std::string::npos &&
              s.find("\nsp ") != std::string::npos;
    printf("RESULT: %s\n", ok ? "PASS" : "FAIL");
    return ok ? 0 : 1;
}

// ---- find-culprit (sim/find_culprit.py): bisect the rebuild set with the simulator as the test ----
int cmd_find_culprit(const std::vector<std::string>& args) {
    // the boot animation must play: at least MIN_DISTINCT different 80x16 frames in INSNS instructions
    uint64_t insns = count(args, 0, 60000000);
    const size_t MIN_DISTINCT = 20;
    // MODE=alive: good if only the stock tasks ever ran and the panel kept being fed (MIN_FRAMES frames);
    // MODE=menu: "!" on the remote after 700 frames, good if the panel then shows TARGET (a frame file)
    const char* m = getenv("MODE");
    std::string mode = m ? m : "boot";
    const std::set<std::string> stock_tasks = {"INIT", "LEDP", "REST", "SMOD", "000afe18"};
    const char* mf = getenv("MIN_FRAMES");
    size_t min_frames = mf ? (size_t)count_arg(mf) : 440;
    const char* target = getenv("TARGET");
    std::string tools = repo().path("analysis/tools"), rebuilt = repo().path("analysis/build/ELITE_RGB.rebuilt");

    auto good = [&](const std::vector<std::string>& names) {
        // build an image replacing only these functions (rebuild.py --only) and run it
        std::string cmd = "cd '" + tools + "' && '" + repo().path("venv/bin/python") + "' rebuild.py --trials 4 --only";
        for (auto& n : names)
            cmd += " '" + n + "'";
        cmd += " >/dev/null 2>&1";
        (void)std::system(cmd.c_str());
        if (mode == "menu") {
            if (!target)
                throw std::runtime_error("MODE=menu needs TARGET=FRAME_FILE");
            auto b = compare_run(rebuilt, insns, 700);
            auto want = read_file(target);
            bool ok = false;
            for (auto& f : b->frames)
                if (f.size() == 2560 && masked(f) == want) {
                    ok = true;
                    break;
                }
            printf("  %4zu functions replaced -> %zu frames, menu frame %s\n", names.size(), b->frames.size(),
                   ok ? "ok" : "BAD");
            fflush(stdout);
            return ok;
        }
        auto b = compare_run(rebuilt, insns);
        if (mode == "alive") {
            std::string extra = "[";
            for (auto& [t, k] : b->tasks)
                if (!stock_tasks.count(t))
                    extra += (extra.size() > 1 ? ", '" : "'") + t + "'";
            extra += "]";
            bool ok = extra == "[]" && b->frames.size() >= min_frames;
            printf("  %4zu functions replaced -> %zu frames, unexpected tasks %s %s\n", names.size(), b->frames.size(),
                   extra.c_str(), ok ? "ok" : "BAD");
            fflush(stdout);
            return ok;
        }
        size_t n = distinct(b->frames).size();
        bool ok = n >= MIN_DISTINCT;
        printf("  %4zu functions replaced -> %zu distinct frames %s\n", names.size(), n, ok ? "ok" : "BAD");
        fflush(stdout);
        return ok;
    };

    auto text = read_file(repo().path("analysis/build/rebuild_report.txt"));
    std::string rep(text.begin(), text.end());
    size_t cut = rep.find("\n\n");
    if (cut == std::string::npos)
        throw std::runtime_error("rebuild_report.txt: no function list");
    rep = rep.substr(cut + 2);
    size_t a = rep.find_first_not_of(" \t\r\n\f\v"), z = rep.find_last_not_of(" \t\r\n\f\v");
    rep = a == std::string::npos ? "" : rep.substr(a, z - a + 1);   // rep.strip()
    std::vector<std::string> names;
    std::stringstream ss(rep);
    for (std::string l; std::getline(ss, l);)
        if (!l.empty())
            names.push_back(l.substr(0, l.find('\t')));
    printf("bisecting %zu replaced functions\n", names.size());
    fflush(stdout);
    std::vector<std::string> bad = names;
    while (bad.size() > 1) {
        std::vector<std::string> lo(bad.begin(), bad.begin() + bad.size() / 2), hi(bad.begin() + bad.size() / 2, bad.end());
        if (!good(lo))
            bad = lo;
        else if (!good(hi))
            bad = hi;
        else {
            printf("  both halves are fine alone: an interaction; stopping with %zu\n", bad.size());
            break;
        }
    }
    std::string list = "[";
    for (size_t i = 0; i < bad.size(); i++)
        list += (i ? ", '" : "'") + bad[i] + "'";
    printf("culprit(s): %s]\n", list.c_str());
    return 0;
}

// ---- console (sim/test_console.py): the DB9 serial console of our firmware (firmware/ext/console.c) ----
int cmd_console(const std::vector<std::string>& args) {
    const int DB9 = 0, DB9_STOCK = 4;                      // from-source DB9 = SCC1 (minor 0); stock DB9 = SMC1 (minor 4)
    std::vector<std::string> fails;
    auto check = [&](bool ok, const std::string& what) {
        printf("%s%s\n", ok ? "ok   " : "FAIL ", what.c_str());
        if (!ok)
            fails.push_back(what);
    };
    auto board = [&](const std::vector<uint8_t>& flash, const std::vector<uint8_t>& app,
                     const std::vector<std::pair<uint64_t, std::vector<uint8_t>>>& sends, uint64_t insns, int minor) {
        auto b = booted(flash, app);
        for (auto& [at_frame, data] : sends)
            b->serial_send(minor, data, 0, at_frame);
        b->run_app(insns);
        return b;
    };
    auto flash = read_file(arg(args, 0, repo().path("bootloader/build/bootflash-fw.bin")));
    auto app = read_file(repo().path("analysis/build/fw/ELITE_RGB-ext"));
    auto probe = db9_frame(0x20, {0x00});                  // "whoami": an invalid name, the sign answers with its own
    const char* lines[] = {"MQ?MON\r", "help\r", "info\r", "md f8180000 20\r", "md 4000 10\r", "md 70000000 4\r",
                           "mw c6cc00 12345678\r", "md c6cc00 4\r", "mw f8780000 1\r", "bogus\r", "exit\r"};
    const uint64_t nl = sizeof lines / sizeof *lines;
    std::vector<std::pair<uint64_t, std::vector<uint8_t>>> sends;
    for (uint64_t i = 0; i < nl; i++)
        sends.emplace_back(700 + 10 * i, std::vector<uint8_t>(lines[i], lines[i] + strlen(lines[i])));
    sends.emplace_back(700 + 10 * nl + 40, probe);
    auto b = board(flash, app, sends, 1000000000, DB9);
    std::vector<uint8_t> tx = b->serial_tx.count(DB9) ? b->serial_tx[DB9] : std::vector<uint8_t>{};
    std::string text(tx.begin(), tx.end());                // the bytes as Latin-1 characters
    std::string ev = "{";
    for (auto& [k, v] : b->events)
        ev += fmt("%s'%s': %llu", ev.size() > 1 ? ", " : "", k.c_str(), (unsigned long long)v);
    printf("run: %zu frames, %llu instructions, events %s}, rx left %zu\n", b->frames.size(),
           (unsigned long long)b->icount, ev.c_str(), b->rx_queue.size());
    std::vector<uint8_t> shown;
    for (uint8_t c : tx)
        if (c != '\r')
            shown.push_back(c);
    std::string out = "---- DB9 output ----\n" + latin1_to_utf8(shown) + "\n--------------------\n";
    fwrite(out.data(), 1, out.size(), stdout);             // the output holds NUL bytes
    auto has = [&](const std::string& s) { return text.find(s) != std::string::npos; };
    check(has("MQMON 1 marquee-sign console") && has("mq> "), "the magic line opens the console");
    check(has("md ADDR [LEN]") && has("exit"), "help lists the commands");
    check(std::regex_search(text, std::regex("ram top [0-9a-f]{8}, heap at [0-9a-f]{8}")), "info answers");
    std::vector<uint8_t> boot16(flash.begin(), flash.begin() + 16), ram(16);
    check(has("f8180000: " + hex(boot16, " ")), "md of the boot flash shows its bytes");
    b->read(0x4000, ram.data(), 16);
    check(has("00004000: " + hex(ram, " ")), "md of RAM shows its bytes");
    size_t ranges = 0;
    for (size_t p = text.find("? range"); p != std::string::npos; p = text.find("? range", p + 1))
        ranges++;
    check(ranges == 1, "md outside RAM / boot flash / NVRAM is refused");
    check(has("00c6cc00: 12 34 56 78") && b->r32(0xC6CC00) == 0x12345678, "mw writes a RAM word");
    check(has("? mw ADDR WORD"), "mw to the NVRAM is refused");
    check(has("? unknown command") && has("bye"), "unknown commands answered; exit says bye");
    check(b->r32(0xC5CE50 + 0x80) == 0, "the console is closed at the end");
    size_t bye = text.rfind("bye\r\n");
    std::vector<uint8_t> after = bye == std::string::npos ? std::vector<uint8_t>{}
                                                          : std::vector<uint8_t>(tx.begin() + bye + 5, tx.end());

    printf("---- the stock firmware, the same probe ----\n");
    auto stock = board(read_file(repo().path(DUMP)), read_file(repo().path(STOCK)), {{700 + 10 * nl + 40, probe}},
                       750000000, DB9_STOCK);
    std::vector<uint8_t> stx = stock->serial_tx.count(DB9_STOCK) ? stock->serial_tx[DB9_STOCK] : std::vector<uint8_t>{};
    size_t k = std::min(after.size(), stx.size());
    bool same = !stx.empty() && std::equal(after.begin(), after.begin() + k, stx.begin()) &&
                after.size() >= std::min<size_t>(stx.size(), 8);
    auto head = [](const std::vector<uint8_t>& v) {
        return hex(std::vector<uint8_t>(v.begin(), v.begin() + std::min<size_t>(v.size(), 24)));
    };
    check(same, "after exit the download server answers the probe as on the stock firmware (" + head(after) + " vs " +
                    head(stx) + ")");
    printf("RESULT: %s\n", fails.empty() ? "PASS" : fmt("FAIL (%zu)", fails.size()).c_str());
    return fails.empty() ? 0 : 1;
}

}  // namespace sim

using namespace sim;

int main(int argc, char** argv) {
    std::vector<std::string> args(argv + 1, argv + argc);
    if (args.size() >= 2 && args[0] == "--repo") {
        g_repo_dir = args[1];
        args.erase(args.begin(), args.begin() + 2);
    }
    const std::vector<std::tuple<std::string, std::function<int(const std::vector<std::string>&)>, const char*>> cmds = {
        {"compare", cmd_compare, "[IMAGE] [INSNS] [STOCK_INSNS]   stock vs IMAGE, distinct panel frames in order"},
        {"compare-rs485", cmd_compare_rs485, "[IMAGE] [INSNS] [STOCK_INSNS] [AT_FRAME]   one RS-485 text record"},
        {"compare-db9", cmd_compare_db9, "[IMAGE] [INSNS] [STOCK_INSNS]   read-only DB9 probes, every reply byte"},
        {"compare-menu", cmd_compare_menu, "[--edit] [IMAGE] [INSNS] [STOCK_INSNS]   the menu walked with the IR remote"},
        {"flashtest", cmd_flashtest, "FLASH.bin APP [INSNS] [STOCK_INSNS]   a boot-flash image boots; its app vs stock"},
        {"crashtest", cmd_crashtest, "FLASH.bin APP [INSNS]   a fault in a task: crash report on the DB9"},
        {"nvram", cmd_nvram, "[FLASH.bin]   the firmware's NVRAM blocks and the room left"},
        {"consoledump", cmd_consoledump, "OUTDIR CMDS.txt FRAMES [FLASH.bin]   console lines in, the drawn frames out"},
        {"attractdump", cmd_attractdump, "OUTDIR [FLASH.bin]   the firmware's attract frames, for the web emulation"},
        {"attracttest", cmd_attracttest, "[FLASH.bin]   lines in the firmware's attract configuration are shown"},
        {"dbgtest", cmd_dbgtest, "[FLASH.bin]   the NVRAM debug record with our boot loader (steps, crash)"},
        {"slotflagtest", cmd_slotflagtest, "[FLASH.bin]   the slot's try count is cleared after 1200 frames"},
        {"updatescreen", cmd_updatescreen, "[FLASH.bin]   a tiny update: what reaches the panel"},
        {"apprun", cmd_apprun, "[FLASH.bin [ADDR [INSNS]]]   boot our from-source app (no dump); check its marker"},
        {"updatetest", cmd_updatetest, "[FLASH.bin [IMAGE.bin]]   an update through the firmware's console boots after its reset"},
        {"slotboot", cmd_slotboot, "[FLASH.bin [SLOT.bin]]   an image in the update slot boots (RAM stage)"},
        {"resettest", cmd_resettest, "[FLASH.bin]   the console's reset: the firmware's restart, NVRAM untouched"},
        {"find-culprit", cmd_find_culprit, "[INSNS]   bisect the rebuild set (MODE=boot|alive|menu, MIN_FRAMES, TARGET)"},
        {"console", cmd_console, "[FLASH.bin]   the DB9 serial console of our firmware"},
        {"native-emoji", cmd_native_emoji, "[FLASH.bin]   native emoji inside the running firmware vs its reference"},
    };
    if (args.empty() || args[0] == "--help" || args[0] == "-h") {
        fprintf(stderr, "usage: umami-marquee-sim-test [--repo DIR] SUBCOMMAND [ARGS...]\n");
        for (auto& [name, fn, help] : cmds)
            fprintf(stderr, "  %-14s %s\n", name.c_str(), help);
        return args.empty() ? 2 : 0;
    }
    for (auto& [name, fn, help] : cmds)
        if (name == args[0]) {
            try {
                return fn(std::vector<std::string>(args.begin() + 1, args.end()));
            } catch (const std::exception& e) {
                fprintf(stderr, "umami-marquee-sim-test %s: %s\n", name.c_str(), e.what());
                return 1;
            }
        }
    fprintf(stderr, "no subcommand %s (--help lists them)\n", args[0].c_str());
    return 2;
}
