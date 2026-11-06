#include "dbgrecord.h"

#include <algorithm>
#include <cstdio>
#include <map>

namespace host {

static uint32_t be32(const std::vector<uint8_t>& b, size_t o) {
    return (uint32_t)b[o] << 24 | b[o + 1] << 16 | b[o + 2] << 8 | b[o + 3];
}

DbgRecord dbg_decode(const std::vector<uint8_t>& b) {
    DbgRecord r;
    if (b.size() < DBG_SIZE || be32(b, 0) != 0x4D514442)
        return r;
    r.valid = true;
    r.seq = be32(b, 4);
    r.options = b[8];
    for (int i = 0; i < 8; i++) {
        size_t o = 0x10 + 16 * i;
        uint32_t s = be32(b, o);
        if (s)
            r.boots.push_back({s, be32(b, o + 4), (uint32_t)(b[o + 8] << 8 | b[o + 9]), be32(b, o + 12)});
    }
    std::sort(r.boots.begin(), r.boots.end(), [](auto& x, auto& y) { return x.seq < y.seq; });
    if (be32(b, 0x100) == 0x4D514352) {
        r.crashed = true;
        uint32_t* f[] = {&r.c_seq, &r.source, &r.vector, &r.srr0, &r.srr1, &r.dar, &r.dsisr, &r.lr, &r.cr,
                         &r.step, &r.value};
        for (int i = 0; i < 11; i++)
            *f[i] = be32(b, 0x104 + 4 * i);
        for (int i = 0; i < 32; i++)
            r.gpr[i] = be32(b, 0x130 + 4 * i);
    }
    return r;
}

static std::string step_name(uint32_t s) {
    static const std::map<uint32_t, const char*> N = {
        {0x01, "boot loader started"}, {0x02, "SDRAM up (exceptions caught)"}, {0x03, "serial window closed"},
        {0x10, "update slot found"}, {0x11, "trying the slot"}, {0x12, "slot copied to the RAM stage"},
        {0x13, "booting the slot in place (direct mode)"}, {0x18, "booting the boot flash's image"},
        {0x19, "booting the staged image (RUN)"}, {0x20, "loading the app"}, {0x21, "app loaded"},
        {0x22, "memory map set"}, {0x23, "handed off to the app"}, {0x40, "firmware started"},
        {0x41, "firmware exception table (crash reports on)"}, {0x42, "firmware main task started"},
        {0x43, "firmware memory layout (main flash mapped)"}, {0x4F, "firmware healthy (1200 frames)"}};
    auto i = N.find(s);
    char buf[32];
    snprintf(buf, sizeof buf, "step 0x%x", s);
    return i != N.end() ? i->second : buf;
}

static std::string rsr_text(uint32_t v) {
    static const char* names[8] = {"ext-hard", "ext-soft", "loss-of-lock", "sw-wdog", "checkstop", "debug-hard",
                                   "debug-soft", "jtag"};
    std::string s;
    for (int i = 0; i < 8; i++)
        if (v & (0x80000000u >> i))
            s += (s.empty() ? "" : " ") + std::string(names[i]);
    return s.empty() ? "-" : s;
}

std::string dbg_report(const DbgRecord& r, const std::function<std::string(uint32_t)>& sym) {
    if (!r.valid)
        return "No debug record yet: our boot loader starts it on its first boot.";
    char b[256];
    std::string out;
    snprintf(b, sizeof b, "Boot #%u. Slot boot: %s.\nLast boots (oldest first): reset cause -> last step reached\n",
             r.seq, r.options & DBG_OPT_SLOT_DIRECT ? "direct, in place (debugging)" : "through the RAM stage");
    out += b;
    for (auto& x : r.boots) {
        snprintf(b, sizeof b, "  #%-5u %-28s -> %s (0x%08x)%s\n", x.seq, rsr_text(x.rsr).c_str(),
                 step_name(x.step).c_str(), x.value, x.seq == r.seq ? "  <- this boot" : "");
        out += b;
    }
    if (!r.crashed)
        return out + "No crash recorded.\n";
    static const std::map<uint32_t, const char*> V = {
        {0x200, "machine check"}, {0x300, "DSI (data access)"}, {0x400, "ISI (instruction fetch)"},
        {0x600, "alignment"}, {0x700, "program (trap/illegal)"}, {0x800, "FP unavailable"},
        {0x1000, "software emulation"}, {0x1200, "DTLB miss"}, {0x1300, "ITLB error"}, {0x1400, "DTLB error"}};
    static const char* SRC[4] = {"?", "boot loader exception", "firmware DTLB miss with no mapping (would hang)",
                                 "firmware crash"};
    auto v = V.find(r.vector);
    snprintf(b, sizeof b, "Last crash: boot #%u, %s, vector 0x%x %s\n  during: %s (0x%08x)\n", r.c_seq,
             r.source < 4 ? SRC[r.source] : "?", r.vector, v != V.end() ? v->second : "", step_name(r.step).c_str(),
             r.value);
    out += b;
    auto at = [&](uint32_t a) { return sym && a < 0x00C00000 ? " " + sym(a) : std::string(); };
    snprintf(b, sizeof b, "  srr0 %08x%s\n  lr   %08x%s\n", r.srr0, at(r.srr0).c_str(), r.lr, at(r.lr).c_str());
    out += b;
    snprintf(b, sizeof b, "  srr1 %08x  dar %08x  dsisr %08x  cr %08x\n", r.srr1, r.dar, r.dsisr, r.cr);
    out += b;
    for (int i = 0; i < 32; i += 8) {
        out += " ";
        for (int k = 0; k < 8; k++) {
            snprintf(b, sizeof b, " r%-2d %08x", i + k, r.gpr[i + k]);
            out += b;
        }
        out += "\n";
    }
    return out;
}

}  // namespace host
