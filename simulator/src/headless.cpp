// umami-marquee-sim-check: runs the simulator without a window and writes a fingerprint of the whole run (every panel
// frame, the serial bytes, NVRAM, RAM, DPRAM, registers, the shadow MSR and SPRs, counters, tasks), so that two
// simulator builds, or this simulator and the earlier Python one, can be compared field by field.
//
// Usage: umami-marquee-sim-check [--repo DIR] OUT.json SCENARIO...   (scenarios: --list)
#include "board.h"
#include "repo.h"
#include "sha256.h"
#include "symbols.h"

#include <chrono>
#include <cstdio>
#include <fstream>
#include <functional>
#include <map>
#include <sstream>
#include <string>
#include <vector>

using namespace sim;

namespace {

std::string json_str(const std::string& s) {
    std::string o = "\"";
    for (unsigned char c : s) {
        if (c == '"' || c == '\\')
            o += '\\', o += (char)c;
        else if (c < 0x20) {
            char b[8];
            snprintf(b, sizeof b, "\\u%04x", c);
            o += b;
        } else
            o += (char)c;
    }
    return o + "\"";
}

std::string hexs(const std::vector<uint8_t>& v) {
    static const char* d = "0123456789abcdef";
    std::string o;
    for (uint8_t c : v)
        o += d[c >> 4], o += d[c & 15];
    return o;
}

std::string fingerprint(Board& b, const std::map<std::string, std::string>& extra) {
    std::ostringstream o;
    auto u = [](uint64_t v) { return std::to_string(v); };
    o << "{\n";
    o << " \"icount\": " << b.icount << ",\n \"skew\": " << b.skew << ",\n \"now\": " << b.now() << ",\n";
    o << " \"pit_next\": " << (b.pit_next ? u(*b.pit_next) : "null") << ",\n \"msr\": " << b.msr << ",\n";
    o << " \"spr\": [";
    bool first = true;
    for (auto& [k, v] : b.spr)
        o << (first ? "" : ", ") << "[" << k << ", " << v << "]", first = false;
    o << "],\n \"regs\": [";
    for (int i = 0; i < 32; i++)
        o << b.reg(UC_PPC_REG_0 + i) << ", ";
    o << b.reg(UC_PPC_REG_PC) << ", " << b.reg(UC_PPC_REG_CR) << ", " << b.reg(UC_PPC_REG_LR) << ", "
      << b.reg(UC_PPC_REG_CTR) << ", " << b.reg(UC_PPC_REG_XER) << "],\n";
    Sha256 fr;
    for (auto& f : b.frames) {
        uint8_t l[4] = {(uint8_t)(f.size() >> 24), (uint8_t)(f.size() >> 16), (uint8_t)(f.size() >> 8), (uint8_t)f.size()};
        fr.update(l, 4);
        fr.update(f.data(), f.size());
    }
    o << " \"frames\": " << b.frames.size() << ",\n \"frames sha\": " << json_str(fr.hex().substr(0, 20)) << ",\n";
    o << " \"frame list\": [";
    for (size_t i = 0; i < b.frames.size(); i++)
        o << (i ? ", " : "") << json_str(sha20(b.frames[i].data(), b.frames[i].size()));
    o << "],\n \"serial tx\": {";
    first = true;
    for (auto& [k, v] : b.serial_tx)
        o << (first ? "" : ", ") << json_str(std::to_string(k)) << ": " << json_str(hexs(v)), first = false;
    o << "},\n \"rx left\": " << b.rx_queue.size() << ",\n";
    for (auto [name, m] : {std::pair<const char*, std::map<int, int>*>{"rx_idx", &b.rx_idx}, {"tx_idx", &b.tx_idx}}) {
        o << " " << json_str(name) << ": [";
        first = true;
        for (auto& [k, v] : *m)
            o << (first ? "" : ", ") << "[" << k << ", " << v << "]", first = false;
        o << "],\n";
    }
    o << " \"ir\": [" << b.ir_level << ", " << b.ir_tb << ", " << b.ir_keys_sent << ", "
      << (b.ir_last ? u(*b.ir_last) : "null") << ", " << b.ir_queue.size() << ", " << b.ir_edges.size() << "],\n";
    o << " \"events\": {";
    first = true;
    for (auto& [k, v] : b.events)
        o << (first ? "" : ", ") << json_str(k) << ": " << v, first = false;
    o << "},\n";
    std::vector<uint8_t> nv(0x80000), ram(RAM_TOTAL);
    b.read(NVRAM, nv.data(), nv.size());
    b.read(0, ram.data(), ram.size());
    o << " \"nvram\": " << json_str(sha20(nv.data(), nv.size())) << ",\n";
    o << " \"ram\": " << json_str(sha20(ram.data(), ram.size())) << ",\n";
    o << " \"dpram\": " << json_str(sha20(b.dpram.data(), b.dpram.size())) << ",\n";
    o << " \"eeproms\": {";
    first = true;
    for (auto& [k, v] : b.eeproms)
        o << (first ? "" : ", ") << json_str(std::to_string(k)) << ": " << json_str(sha20(v.data(), v.size())), first = false;
    o << "},\n \"reg_writes\": [" << b.reg_writes << ", \"\"],\n";
    o << " \"tasks\": {";
    first = true;
    for (auto& [k, v] : b.tasks)
        o << (first ? "" : ", ") << json_str(k) << ": " << v, first = false;
    o << "},\n \"switches\": [";
    for (size_t i = 0; i < b.switches.size(); i++)
        o << (i ? ", " : "") << "[" << b.switches[i].first << ", " << json_str(b.switches[i].second) << "]";
    o << "]";
    for (auto& [k, v] : extra)
        o << ",\n " << json_str(k) << ": " << v;
    o << "\n}";
    return o.str();
}

std::vector<uint8_t> unhex(const char* h) {
    std::vector<uint8_t> v;
    for (size_t i = 0; h[i] && h[i + 1]; i += 2)
        v.push_back((uint8_t)std::stoul(std::string(h + i, 2), nullptr, 16));
    return v;
}

struct Run {
    std::unique_ptr<Board> b;
    std::map<std::string, std::string> extra;
};

// The set-up of sim/flashtest.py: the whole boot-flash image boots as on the sign.
Run flashtest(const Repo& r, const std::string& flash, const std::string& app, uint64_t insns) {
    Run run;
    run.b = std::make_unique<Board>(read_file(flash), Scenario{}, read_file(app));
    Board& b = *run.b;
    if (!b.boot())
        throw std::runtime_error("the boot loader did not reach the app");
    b.hook_app_specials();
    b.hook_busy_waits(r.symbols);
    b.watch_switches(r.symbols);
    b.run_app(insns);
    return run;
}

// The set-up of sim/compare_*.py: the stock boot flash boots, then the app image replaces the unpacked stock app.
std::unique_ptr<Board> swapped(const Repo& r, const std::string& app) {
    auto b = std::make_unique<Board>(read_file(r.path("dumps/bootflash.bin")), Scenario{}, read_file(app));
    if (!b->boot())
        throw std::runtime_error("the boot loader did not reach the app");
    b->load_app_image();
    b->hook_app_specials();
    b->hook_busy_waits(r.symbols);
    b->watch_switches(r.symbols);
    return b;
}

Run db9(const Repo& r, const std::string& app, uint64_t insns) {
    Run run;
    run.b = swapped(r, app);
    run.b->serial_send(4, unhex("5a2000018b77000000"), 0, 700);   // "whoami" (an invalid name)
    run.b->serial_send(4, unhex("5a90000065a9"), 0, 900);         // a header-only 0x90 frame
    run.b->run_app(insns);
    return run;
}

Run menu(const Repo& r, const std::string& app, uint64_t insns, const std::string& keys) {
    Run run;
    run.b = swapped(r, app);
    for (size_t i = 0; i < keys.size(); i++)
        run.b->ir_press(std::string(1, keys[i]), 700 + 15 * i);
    run.b->watch_writes(0x45BFCC, 4);                              // the display state
    run.b->run_app(insns);
    std::string s = "[";
    for (size_t i = 0; i < run.b->states.size(); i++)
        s += (i ? ", " : "") + std::to_string(run.b->states[i]);
    run.extra["states"] = s + "]";
    return run;
}

Run rs485(const Repo& r, const std::string& app, uint64_t insns) {
    Run run;
    run.b = swapped(r, app);
    Board& b = *run.b;
    b.force_config_word(0xDCDE4, 8);                               // protocol: Cougar
    b.force_config_bytes(0xDCDF0, {0x01, 0x0F, 0x0F, 0x0F});       // a level map
    b.serial_send(1, unhex("03004a000100210001e2400000000048454c4c4f2046524f4d205448452053494d000000000000"
                           "00000000000000000000000000000000000000000000000000000000000000000000000045e204"), 0, 700);
    b.run_app(insns);
    std::string s = "{";
    const std::pair<const char*, uint32_t> coug[] = {{"bytes_rx", 0xB9310}, {"bytes_tx", 0xB9314},
        {"frame_errors", 0xB9318}, {"other", 0xB931C}, {"frames_ok", 0xB9320}, {"quick_polls", 0xB9324},
        {"poll_replies", 0xB9328}};
    for (size_t i = 0; i < 7; i++)
        s += (i ? ", " : "") + json_str(coug[i].first) + ": " + std::to_string(b.r32(coug[i].second));
    run.extra["coug"] = s + "}";
    return run;
}

}  // namespace

int main(int argc, char** argv) {
    std::vector<std::string> args(argv + 1, argv + argc);
    std::string repo_dir;
    if (args.size() >= 2 && args[0] == "--repo") {
        repo_dir = args[1];
        args.erase(args.begin(), args.begin() + 2);
    }
    Repo r(repo_dir);
    std::map<std::string, std::function<Run()>> sc = {
        {"flashtest", [&] { return flashtest(r, r.path("bootloader/build/bootflash-fw.bin"),
                                             r.path("analysis/build/fw/ELITE_RGB-ext"), 80000000); }},
        {"flashtest_stock", [&] { return flashtest(r, r.path("dumps/bootflash.bin"),
                                                   r.path("analysis/extracted/ELITE_RGB"), 110000000); }},
        {"db9_stock", [&] { return db9(r, r.path("analysis/extracted/ELITE_RGB"), 750000000); }},
        {"db9_fromsrc", [&] { return db9(r, r.path("analysis/build/fw/ELITE_RGB"), 600000000); }},
        {"menu_stock", [&] { return menu(r, r.path("analysis/extracted/ELITE_RGB"), 680000000,
                                         "!]]]]]]]]]]]]}]]]]{-"); }},
        {"menu_edit_stock", [&] { return menu(r, r.path("analysis/extracted/ELITE_RGB"), 680000000, "![[[}-}-"); }},
        {"rs485_stock", [&] { return rs485(r, r.path("analysis/extracted/ELITE_RGB"), 750000000); }},
    };
    if (args.empty() || args[0] == "--list") {
        fprintf(stderr, "usage: umami-marquee-sim-check [--repo DIR] OUT.json SCENARIO...\nscenarios:");
        for (auto& [k, v] : sc)
            fprintf(stderr, " %s", k.c_str());
        fprintf(stderr, "\n");
        return args.empty() ? 2 : 0;
    }
    std::string out = args[0];
    std::vector<std::string> names(args.begin() + 1, args.end());
    if (names.size() == 1 && names[0] == "all")
        for (auto& [k, v] : sc)
            names.push_back(k);
    std::string body = "{";
    bool first = true;
    for (auto& n : names) {
        if (n == "all")
            continue;
        auto it = sc.find(n);
        if (it == sc.end()) {
            fprintf(stderr, "no scenario %s\n", n.c_str());
            return 2;
        }
        auto t0 = std::chrono::steady_clock::now();
        Run run = it->second();
        double s = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
        run.extra["seconds"] = std::to_string(s);
        printf("%s: %llu instructions, %zu frames, %.1f s (%.2f M/s)\n", n.c_str(), (unsigned long long)run.b->icount,
               run.b->frames.size(), s, run.b->icount / s / 1e6);
        fflush(stdout);
        body += std::string(first ? "\n" : ",\n") + json_str(n) + ": " + fingerprint(*run.b, run.extra);
        first = false;
    }
    std::ofstream(out) << body << "\n}\n";
    return 0;
}
