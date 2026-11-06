// umami-marquee-sim: the simulated sign with a debugger (Dear ImGui on SDL3), and a GDB server for VS Code.
//
// Usage: umami-marquee-sim [--flash FLASH.bin] [--app IMAGE] [--model ED0501] [--cougar] [--run] [--gdb PORT | --no-gdb] [--repo DIR]
//   --flash   boot flash image (default dumps/bootflash.bin; bootloader/build/bootflash-fw.bin for our firmware)
//   --app     run this app image instead of the one the flash carries (e.g. analysis/build/fw/ELITE_RGB)
//   --cougar  RS-485 set up for Cougar, as the RS-485 comparison does
//   --run     start running (default: halted at the app entry)
//   --gdb     the GDB server's port (default 1234; VS Code and gdb attach with target remote :1234)
//   --no-window          no debugger window: the simulator and the GDB server only (until stdin closes or Ctrl-C)
//   --model NAME         the display model in the config EEPROM (default ED0501, the real sign's 200x16)
//   --focus WINDOW       bring that window to the front (its ### id, e.g. ###Sign)
//   --no-pty             no serial devices; by default the emulated DB9 and RS-485 appear as pseudo-terminals,
//                        linked at build/sim/db9 and build/sim/rs485, which host tools open like the real sign's port
// Content design is the web Studio's job (frontend/apps/studio + studio/server.py), not this program's: here only the
// simulated sign, the debugger and the Sign window (the real sign's firmware over the DB9).
//   --shot FILE.bmp (headless: SDL's dummy video driver, no window appears) --shot-after SECONDS   save the window to FILE after SECONDS of running, then quit (tests, docs)
#include <fstream>
#include "disasm.h"
#include "gdbserver.h"
#include "machine.h"
#include "led_player.h"
#include "pty_bridge.h"
#include "sign_ui.h"
#include "display_models.h"
#include "timeline.h"
#include "ui_style.h"

#include "IconsFontAwesome6.h"

#include "imgui.h"
#include "imgui_impl_sdl3.h"
#include "imgui_impl_sdlrenderer3.h"
#include "imgui_internal.h"

#include <algorithm>
#include <atomic>
#include <csignal>
#include <cstring>
#include <SDL3/SDL.h>

#include <chrono>
#include <cstdio>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

using namespace sim;

// Window names: an icon and a title, with a fixed ### id so the layout survives title changes.
#define W_PANEL ICON_FA_TABLE_CELLS " LED panel###LED panel"
#define W_DISASM ICON_FA_CODE " Disassembly###Disassembly"
#define W_REGS ICON_FA_MICROCHIP " Registers###Registers"
#define W_MEMORY ICON_FA_MEMORY " Memory###Memory"
#define W_BREAKS ICON_FA_CIRCLE_STOP " Breakpoints###Breakpoints"
#define W_TASKS ICON_FA_LIST_CHECK " RTEMS tasks###RTEMS tasks"
#define W_REMOTE ICON_FA_GAMEPAD " IR remote###IR remote"
#define W_EVENTS ICON_FA_CHART_BAR " Events###Events"
#define W_DB9 ICON_FA_TERMINAL " DB9  /dev/rs232 (SCC1)###DB9"
#define W_RS485 ICON_FA_TERMINAL " RS-485  /dev/rs485 (SCC2)###RS-485"
#define W_TIMELINE ICON_FA_TIMELINE " Timeline###Timeline"

namespace {

const char* REG_NAMES[38] = {"r0",  "r1",  "r2",  "r3",  "r4",  "r5",  "r6",  "r7",  "r8",  "r9",  "r10", "r11", "r12",
                             "r13", "r14", "r15", "r16", "r17", "r18", "r19", "r20", "r21", "r22", "r23", "r24", "r25",
                             "r26", "r27", "r28", "r29", "r30", "r31", "pc",  "msr", "cr",  "lr",  "ctr", "xer"};

const ImVec4 GREEN(0.45f, 0.85f, 0.45f, 1), YELLOW(0.95f, 0.8f, 0.3f, 1), DIM(0.55f, 0.55f, 0.6f, 1),
    RED(0.95f, 0.35f, 0.35f, 1), CYAN(0.4f, 0.8f, 0.95f, 1);

std::string printable(const std::vector<uint8_t>& d) {
    std::string o;
    for (uint8_t c : d) {
        if (c == '\n')
            o += '\n';
        else if (c == '\r')
            continue;
        else if (c >= 32 && c < 127)
            o += (char)c;
        else {
            char b[8];
            snprintf(b, sizeof b, "\\x%02x", c);
            o += b;
        }
    }
    return o;
}

struct Terminal {
    const char* title;
    int minor;
    char input[256] = "";
    bool hex = false, cr = true;
    size_t shown = 0;
};

struct App {
    Machine& m;
    const Symbols& syms;
    std::unique_ptr<GdbServer> gdb;
    Disasm dis;
    Machine::View v;
    uint32_t prev_regs[38] = {};
    bool was_running = false;
    // disassembly
    uint32_t dis_addr = 0;
    bool dis_follow = true;
    char dis_goto[64] = "";
    std::vector<std::pair<uint32_t, uint32_t>> dis_lines;   // address, word
    std::vector<uint32_t> bps;
    std::vector<Watchpoint> wps;
    uint32_t dis_for_pc = 1;
    std::chrono::steady_clock::time_point dis_t;
    // memory
    char mem_goto[64] = "_Thread_Executing";
    uint32_t mem_addr = 0x45C4CC;
    std::vector<uint8_t> mem;
    bool mem_ok = false;
    char mem_write[256] = "";
    std::chrono::steady_clock::time_point mem_t;
    // breakpoints
    char bp_add[64] = "", wp_add[64] = "";
    int wp_kind = 0, wp_len = 4;
    // terminals and remote
    Terminal db9{W_DB9, 4}, rs485{W_RS485, 1};
    char ir_keys[64] = "";
    bool layout_done = false;
    std::string message;
    std::unique_ptr<SignUI> sign;                        // the real sign's firmware over the DB9
    std::unique_ptr<LedPlayer> panel_player;
    Timeline timeline;
    bool show_timeline = true;
    bool reset_layout = false;
    bool show_panel = true, show_db9 = true, show_rs485 = true, show_remote = true;
    bool show_disasm = true, show_memory = true, show_regs = true, show_breaks = true, show_tasks = true, show_events = true;
    bool sign_only = false;
    std::vector<const char*> later_focus;                // after a new layout, the tabs to show in each column
    int later_focus_frames = 0;
    std::string focus;                                   // a window to bring to the front once (--focus)
    std::string db9_dev = "off", rs485_dev = "off";      // the emulated ports' devices (status bar)
    // the status bar's values, refreshed 4 times a second so they can be read
    std::chrono::steady_clock::time_point status_t;
    std::string st_state, st_pc, st_fn, st_time, st_insns, st_frames, st_speed, st_gdb, st_sign, st_job, st_remote;
    bool st_running = false, st_gdb_on = false;
    float panel_aspect = 200.0f / 16.0f;                 // width / height of the last frame, for the layout

    App(Machine& mm, const Symbols& s) : m(mm), syms(s) {}

    void refresh_code(bool force) {
        auto now = std::chrono::steady_clock::now();
        bool halted = !v.running;
        bool stopped_now = halted && v.pc != dis_for_pc;
        if (force || stopped_now || now - dis_t > std::chrono::milliseconds(halted ? 1000 : 300)) {
            // Follow PC: keep the PC in view, re-centred when it is outside the lines read (or just stopped there)
            if (dis_follow && (stopped_now || v.pc < dis_addr || v.pc >= dis_addr + 4 * 96 - 16))
                dis_addr = v.pc >= 0x40 ? v.pc - 0x40 : 0;
            std::vector<uint8_t> raw(4 * 96);
            dis_lines.clear();
            if (m.read_mem(dis_addr, raw.data(), raw.size()))
                for (int i = 0; i < 96; i++)
                    dis_lines.emplace_back(dis_addr + 4 * i, (uint32_t)raw[4 * i] << 24 | raw[4 * i + 1] << 16 |
                                                                 raw[4 * i + 2] << 8 | raw[4 * i + 3]);
            bps = m.breakpoints();
            wps = m.watchpoints();
            dis_for_pc = v.pc;
            dis_t = now;
        }
        if (force || halted || now - mem_t > std::chrono::milliseconds(300)) {
            mem.assign(256, 0);
            mem_ok = m.read_mem(mem_addr, mem.data(), mem.size());
            mem_t = now;
        }
    }

    void toolbar() {
        bool run = v.running;
        if (ImGui::Button(run ? ICON_FA_PAUSE " Halt  F6" : ICON_FA_PLAY " Run  F5"))
            run ? m.halt() : m.run();
        ImGui::SameLine();
        ImGui::BeginDisabled(run);
        if (ImGui::Button(ICON_FA_ARROW_RIGHT_TO_BRACKET " Step  F11"))
            m.step(1);
        ImGui::SameLine();
        if (ImGui::Button(ICON_FA_ARROW_TURN_DOWN " Over  F10"))
            m.step_over();
        ImGui::EndDisabled();
        ImGui::SameLine();
        if (ImGui::Button(ICON_FA_ROTATE_LEFT " Reset"))
            m.reset();
        ImGui::SameLine();
        if (ImGui::Button(ICON_FA_BUG " Reset layout"))
            reset_layout = true;
        ImGui::SameLine();
        bool rt = m.realtime;
        if (ImGui::Checkbox("Real time", &rt))
            m.realtime = rt;
        ImGui::SameLine();
        if (ImGui::Button(ICON_FA_TABLE_CELLS " Panes"))
            ImGui::OpenPopup("panes");
        if (ImGui::BeginPopup("panes")) {
            if (ImGui::Button("Sign only")) {           // just the LED strip + the Sign window
                show_db9 = show_rs485 = show_remote = show_disasm = show_memory =
                    show_regs = show_breaks = show_tasks = show_events = show_timeline = false;
                show_panel = true, sign_only = true, reset_layout = true;
                ImGui::CloseCurrentPopup();
            }
            ImGui::SameLine();
            if (ImGui::Button("Show all")) {
                show_panel = show_db9 = show_rs485 = show_remote = show_disasm = show_memory =
                    show_regs = show_breaks = show_tasks = show_events = show_timeline = true;
                sign_only = false, reset_layout = true;
                ImGui::CloseCurrentPopup();
            }
            ImGui::Separator();
            ImGui::Checkbox("LED panel", &show_panel);
            ImGui::Checkbox("DB9 console", &show_db9);
            ImGui::Checkbox("RS-485 console", &show_rs485);
            ImGui::Checkbox("IR remote", &show_remote);
            ImGui::Checkbox("Disassembly", &show_disasm);
            ImGui::Checkbox("Memory", &show_memory);
            ImGui::Checkbox("Registers", &show_regs);
            ImGui::Checkbox("Breakpoints", &show_breaks);
            ImGui::Checkbox("RTEMS tasks", &show_tasks);
            ImGui::Checkbox("Events", &show_events);
            ImGui::Checkbox("Timeline", &show_timeline);
            ImGui::EndPopup();
        }
    }

    // The status bar: one labelled cell per value, fixed widths, values refreshed 4 times a second.
    void status_refresh() {
        auto now = std::chrono::steady_clock::now();
        if (now - status_t < std::chrono::milliseconds(250) && !st_state.empty())
            return;
        status_t = now;
        char b[96];
        st_running = v.running;
        st_state = v.running ? "running" : "halted" + (v.stop_reason.empty() ? std::string() : " (" + v.stop_reason + ")");
        snprintf(b, sizeof b, "%08x", v.pc);
        st_pc = b;
        st_fn = syms.describe(v.pc);
        snprintf(b, sizeof b, "%.1f s", v.now / (CPU_MIPS * 1e6));
        st_time = b;
        snprintf(b, sizeof b, "%.1f M", v.icount / 1e6);
        st_insns = b;
        snprintf(b, sizeof b, "%llu", (unsigned long long)v.frames);
        st_frames = b;
        snprintf(b, sizeof b, "%.0f MIPS  x%.2f", v.mips, v.mips / CPU_MIPS);
        st_speed = b;
        st_gdb_on = gdb && gdb->connected();
        st_gdb = gdb ? ":" + std::to_string(gdb->port()) + (st_gdb_on ? " attached" : "") : "off";
        st_sign = sign ? sign->port_label() : "none";
        st_job = sign ? sign->activity() : "";
        st_remote = sign ? (sign->last_remote().empty() ? (sign->remote_listening() ? "listening" : sign->remote_note())
                                                        : sign->last_remote())
                         : "off";
    }

    void footer() {
        status_refresh();
        auto clip = [](const std::string& v, size_t n) { return v.size() > n ? v.substr(0, n - 1) + "~" : v; };
        struct Cell { const char* label; std::string value; ImVec4 colour; const char* widest; };
        ImVec4 text = ImGui::GetStyleColorVec4(ImGuiCol_Text);
        Cell cells[] = {
            {"State", st_state, st_running ? GREEN : YELLOW, "halted (breakpoint)"},
            {"PC", st_pc, text, "00000000"},
            {"Function", clip(st_fn, 26), text, "rtems_task_wake_after+0x1f"},
            {"Sim time", st_time, text, "99999.9 s"},
            {"Instructions", st_insns, text, "99999.9 M"},
            {"Frames", st_frames, text, "9999999"},
            {"Speed", st_speed, text, "999 MIPS  x9.99"},
            {"DB9", db9_dev, text, "ttys000"},
            {"RS-485", rs485_dev, text, "ttys000"},
            {"GDB", st_gdb, st_gdb_on ? CYAN : text, ":1234 attached"},
            {"Sign port", clip(st_sign, 22), st_sign == "none" ? DIM : text, "cu.usbserial-XXXXXXXX"},
            {"Sign job", st_job, st_job == "idle" ? DIM : CYAN, "install 100%"},
            {"Remote", clip(st_remote, 18), DIM, "send:30 apps xxxx"},
            {"Message", message, RED, ""}};
        const int n = (int)(sizeof cells / sizeof *cells);
        if (!ImGui::BeginTable("status", n, ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_SizingFixedFit))
            return;
        for (int i = 0; i < n; i++) {
            ui_mono();
            float w = std::max(ImGui::CalcTextSize(cells[i].widest).x, 0.0f);
            ui_pop();
            w = std::max(w, ImGui::CalcTextSize(cells[i].label).x);
            ImGui::TableSetupColumn(cells[i].label, i == n - 1 ? ImGuiTableColumnFlags_WidthStretch
                                                               : ImGuiTableColumnFlags_WidthFixed, i == n - 1 ? 0 : w);
        }
        for (auto& c : cells) {                          // the label above its value
            ImGui::TableNextColumn();
            ImGui::TextColored(DIM, "%s", c.label);
            ui_mono();
            ImGui::TextColored(c.colour, "%s", c.value.c_str());
            ui_pop();
            if (ImGui::IsItemHovered() && c.value.size() > 0)
                ImGui::SetTooltip("%s: %s", c.label, c.label == std::string("Function") ? st_fn.c_str()
                                                     : c.label == std::string("Remote") ? st_remote.c_str()
                                                                                         : c.value.c_str());
        }
        ImGui::EndTable();
    }

    void panel() {
        ImGui::Begin(W_PANEL, &show_panel);
        const auto& f = v.frame;
        size_t n = f.size() / 2;
        int w, h;
        std::vector<uint16_t> px;
        if (v.panel_w > 0 && v.disp_w > 0 && n >= (size_t)v.panel_w * v.panel_h) {
            // The real sign lights the top-left display_w x display_h of a panel_w x panel_h frame (both from the
            // config EEPROM). Show exactly that, reading the frame at its true stride (panel_w).
            w = v.disp_w, h = v.disp_h;
            px.assign((size_t)w * h, 0);
            for (int y = 0; y < h; y++)
                for (int x = 0; x < w; x++) {
                    size_t i = (size_t)y * v.panel_w + x;
                    px[(size_t)y * w + x] = (uint16_t)(f[2 * i] << 8 | f[2 * i + 1]);
                }
        } else {
            w = 80;
            for (int x : {80, 120, 160, 200, 320})
                if (n && n % x == 0 && (n / x) % 16 == 0 && n / x <= 96) {
                    w = x;
                    break;
                }
            h = n ? (int)(n / w) : 16;
            px.assign(n ? n : (size_t)w * h, 0);
            for (size_t i = 0; i < n; i++)
                px[i] = (uint16_t)(f[2 * i] << 8 | f[2 * i + 1]);
        }
        if (panel_player) {                              // the LED player (afterglow etc.)
            panel_player->set({std::move(px)}, w, h);
            panel_player->playing = false;
            ImVec2 avail = ImGui::GetContentRegionAvail();
            LedLook look; look.persist = 0;              // afterglow off: each frame shows exactly what the panel got
            panel_player->draw(look, avail.x, avail.y);
        }
        panel_aspect = (float)w / (float)h;
        ImGui::End();
    }

    void disassembly() {
        ImGui::Begin(W_DISASM, &show_disasm);
        ui_mono();
        ImGui::Checkbox("Follow PC", &dis_follow);
        ImGui::SameLine();
        ImGui::SetNextItemWidth(220);
        if (ImGui::InputTextWithHint("##goto", "address or symbol", dis_goto, sizeof dis_goto,
                                     ImGuiInputTextFlags_EnterReturnsTrue)) {
            if (auto a = syms.parse(dis_goto)) {
                dis_addr = *a & ~3u, dis_follow = false;
                refresh_code(true);
            } else
                message = std::string("no such address or symbol: ") + dis_goto;
        }
        ImGui::SameLine();
        if (ImGui::Button("PC")) {
            dis_follow = true;
            dis_addr = v.pc >= 0x40 ? v.pc - 0x40 : 0;
            refresh_code(true);
        }
        ImGui::SameLine();
        if (ImGui::Button("Up"))
            dis_addr -= 0x80, dis_follow = false, refresh_code(true);
        ImGui::SameLine();
        if (ImGui::Button("Down"))
            dis_addr += 0x80, dis_follow = false, refresh_code(true);
        ImGui::SameLine();
        ImGui::TextColored(DIM, "click a line: breakpoint");
        ImGui::BeginChild("code", ImVec2(0, 0), ImGuiChildFlags_Borders);
        for (auto [a, w] : dis_lines) {
            std::string nm = syms.describe(a);
            if (!nm.empty() && nm.find('+') == std::string::npos)
                ImGui::TextColored(CYAN, "%s:", nm.c_str());
            bool bp = std::find(bps.begin(), bps.end(), a) != bps.end();
            bool cur = a == v.pc;
            char line[256];
            snprintf(line, sizeof line, "%s %08x  %08x  %s", bp ? "●" : " ", a, w, dis.one(a, w, &syms).c_str());
            ImGui::PushID((int)a);
            if (cur)
                ImGui::PushStyleColor(ImGuiCol_Text, YELLOW);
            else if (bp)
                ImGui::PushStyleColor(ImGuiCol_Text, RED);
            if (ImGui::Selectable(line, cur)) {
                m.set_breakpoint(a, !bp);
                refresh_code(true);
            }
            if (cur || bp)
                ImGui::PopStyleColor();
            ImGui::PopID();
        }
        ImGui::EndChild();
        ui_pop();
        ImGui::End();
    }

    void registers() {
        ImGui::Begin(W_REGS, &show_regs);
        ui_mono();
        bool halted = !v.running;
        if (ImGui::BeginTable("regs", 4, ImGuiTableFlags_SizingFixedFit)) {
            for (int row = 0; row < 19; row++) {
                for (int c = 0; c < 2; c++) {
                    int i = row + 19 * c;
                    ImGui::TableNextColumn();
                    ImGui::TextColored(DIM, "%s", REG_NAMES[i]);
                    ImGui::TableNextColumn();
                    bool changed = halted && v.regs[i] != prev_regs[i];
                    uint32_t val = v.regs[i];
                    ImGui::PushID(i);
                    ImGui::SetNextItemWidth(ImGui::CalcTextSize("00000000").x + 12);
                    if (changed)
                        ImGui::PushStyleColor(ImGuiCol_Text, YELLOW);
                    if (ImGui::InputScalar("##r", ImGuiDataType_U32, &val, nullptr, nullptr, "%08X",
                                           ImGuiInputTextFlags_CharsHexadecimal | ImGuiInputTextFlags_EnterReturnsTrue |
                                               (halted ? 0 : ImGuiInputTextFlags_ReadOnly)))
                        m.set_reg(i, val);
                    if (changed)
                        ImGui::PopStyleColor();
                    ImGui::PopID();
                }
            }
            ImGui::EndTable();
        }
        ImGui::TextColored(DIM, "msr is the shadow MSR; edit when halted, Enter to write");
        ui_pop();
        ImGui::End();
    }

    void memory() {
        ImGui::Begin(W_MEMORY, &show_memory);
        ui_mono();
        ImGui::SetNextItemWidth(220);
        if (ImGui::InputTextWithHint("##addr", "address or symbol", mem_goto, sizeof mem_goto,
                                     ImGuiInputTextFlags_EnterReturnsTrue)) {
            if (auto a = syms.parse(mem_goto))
                mem_addr = *a, refresh_code(true);
            else
                message = std::string("no such address or symbol: ") + mem_goto;
        }
        ImGui::SameLine();
        if (ImGui::Button("-0x100"))
            mem_addr -= 0x100, refresh_code(true);
        ImGui::SameLine();
        if (ImGui::Button("+0x100"))
            mem_addr += 0x100, refresh_code(true);
        ImGui::SetNextItemWidth(300);
        if (ImGui::InputTextWithHint("##w", "hex bytes to write at the address", mem_write, sizeof mem_write,
                                     ImGuiInputTextFlags_EnterReturnsTrue)) {
            auto d = std::string(mem_write);
            std::vector<uint8_t> b;
            for (size_t i = 0; i + 1 < d.size();) {
                if (d[i] == ' ') {
                    i++;
                    continue;
                }
                b.push_back((uint8_t)std::stoul(d.substr(i, 2), nullptr, 16));
                i += 2;
            }
            if (!m.write_mem(mem_addr, b.data(), b.size()))
                message = "write failed (unmapped)";
            refresh_code(true);
        }
        ImGui::BeginChild("hex", ImVec2(0, 0), ImGuiChildFlags_Borders);
        if (!mem_ok)
            ImGui::TextColored(RED, "%08x: unmapped", mem_addr);
        else
            for (int row = 0; row < 16; row++) {
                char line[128], *p = line;
                p += snprintf(p, 16, "%08x ", mem_addr + 16 * row);
                for (int i = 0; i < 16; i++)
                    p += snprintf(p, 8, " %02x", mem[16 * row + i]);
                p += snprintf(p, 4, "  ");
                for (int i = 0; i < 16; i++) {
                    uint8_t c = mem[16 * row + i];
                    *p++ = c >= 32 && c < 127 ? (char)c : '.';
                }
                *p = 0;
                ImGui::TextUnformatted(line);
            }
        ImGui::EndChild();
        ui_pop();
        ImGui::End();
    }

    void breakpoints() {
        ImGui::Begin(W_BREAKS, &show_breaks);
        ui_mono();
        ImGui::SetNextItemWidth(220);
        if (ImGui::InputTextWithHint("##bp", "symbol or address", bp_add, sizeof bp_add, ImGuiInputTextFlags_EnterReturnsTrue)) {
            if (auto a = syms.parse(bp_add))
                m.set_breakpoint(*a, true), bp_add[0] = 0, refresh_code(true);
            else
                message = std::string("no such address or symbol: ") + bp_add;
        }
        for (uint32_t a : bps) {
            ImGui::PushID((int)a);
            if (ImGui::SmallButton("x"))
                m.set_breakpoint(a, false), refresh_code(true);
            ImGui::SameLine();
            ImGui::Text("%08x %s", a, syms.describe(a).c_str());
            ImGui::PopID();
        }
        ImGui::SeparatorText("Watchpoints");
        ImGui::SetNextItemWidth(220);
        bool add = ImGui::InputTextWithHint("##wp", "symbol or address", wp_add, sizeof wp_add, ImGuiInputTextFlags_EnterReturnsTrue);
        ImGui::SameLine();
        ImGui::SetNextItemWidth(90);
        ImGui::Combo("##kind", &wp_kind, "write\0read\0access\0");
        ImGui::SameLine();
        ImGui::SetNextItemWidth(70);
        ImGui::InputInt("bytes", &wp_len, 0);
        if (add) {
            if (auto a = syms.parse(wp_add))
                m.set_watchpoint({*a, (uint32_t)std::max(1, wp_len), wp_kind + 1}, true), wp_add[0] = 0, refresh_code(true);
            else
                message = std::string("no such address or symbol: ") + wp_add;
        }
        for (auto& w : wps) {
            ImGui::PushID((int)(w.addr ^ (w.kind << 28)));
            if (ImGui::SmallButton("x"))
                m.set_watchpoint(w, false), refresh_code(true);
            ImGui::SameLine();
            ImGui::Text("%s %08x +%u %s", w.kind == 1 ? "write" : w.kind == 2 ? "read" : "access", w.addr, w.len,
                        syms.describe(w.addr).c_str());
            ImGui::PopID();
        }
        ui_pop();
        ImGui::End();
    }

    void tasks() {
        ImGui::Begin(W_TASKS, &show_tasks);
        ui_mono();
        if (ImGui::BeginTable("tasks", 5, ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingFixedFit)) {
            for (const char* h : {"id", "name", "prio", "state", "where"})
                ImGui::TableSetupColumn(h);
            ImGui::TableHeadersRow();
            for (auto& t : v.tasks) {
                ImGui::TableNextColumn();
                ImGui::TextColored(t.current ? GREEN : ImGui::GetStyle().Colors[ImGuiCol_Text], "%08x", t.id);
                ImGui::TableNextColumn();
                ImGui::TextUnformatted(t.name.c_str());
                ImGui::TableNextColumn();
                ImGui::Text("%u", t.prio);
                ImGui::TableNextColumn();
                ImGui::Text("%04x", t.state);
                ImGui::TableNextColumn();
                ImGui::Text("%s", t.current ? "running" : syms.describe(t.regs[32]).c_str());
            }
            ImGui::EndTable();
        }
        ui_pop();
        ImGui::End();
    }

    void terminal(Terminal& t, bool* popen) {
        ImGui::Begin(t.title, popen);
        ui_mono();
        float input_h = ImGui::GetFrameHeightWithSpacing();
        ImGui::BeginChild("out", ImVec2(0, -input_h), ImGuiChildFlags_Borders);
        std::string text = printable(v.serial[t.minor]);
        ImGui::TextUnformatted(text.c_str());
        if (v.serial[t.minor].size() != t.shown) {
            ImGui::SetScrollHereY(1.0f);
            t.shown = v.serial[t.minor].size();
        }
        ImGui::EndChild();
        ImGui::SetNextItemWidth(-200);
        bool send = ImGui::InputTextWithHint("##in", t.hex ? "hex bytes" : "text (\\xNN escapes)", t.input, sizeof t.input,
                                             ImGuiInputTextFlags_EnterReturnsTrue);
        ImGui::SameLine();
        send |= ImGui::Button("Send");
        ImGui::SameLine();
        ImGui::Checkbox("hex", &t.hex);
        ImGui::SameLine();
        ImGui::Checkbox("CR", &t.cr);
        if (send) {
            std::vector<uint8_t> d;
            if (t.hex) {
                std::string s(t.input);
                for (size_t i = 0; i + 1 < s.size();) {
                    if (s[i] == ' ') {
                        i++;
                        continue;
                    }
                    d.push_back((uint8_t)std::stoul(s.substr(i, 2), nullptr, 16));
                    i += 2;
                }
            } else {
                d = unescape(t.input);
                if (t.cr)
                    d.push_back('\r');
            }
            m.serial_send(t.minor, d);
            t.input[0] = 0;
            ImGui::SetKeyboardFocusHere(-1);
        }
        ui_pop();
        ImGui::End();
    }

    void remote() {
        ImGui::Begin(W_REMOTE, &show_remote);
        for (const char* k : {"1", "2", "3", "4", "5", "6", "7", "8", "9", "0"}) {
            if (ImGui::Button(k, ImVec2(32, 0)))
                m.ir_press(k);
            ImGui::SameLine();
        }
        ImGui::NewLine();
        const std::pair<const char*, const char*> keys[] = {{"Menu", "!"}, {"Up", "["}, {"Next", "]"},
                                                            {"In", "}"},   {"Back", "{"}, {"Exit", "-"}};
        for (auto [label, key] : keys) {
            if (ImGui::Button(label, ImVec2(60, 0)))
                m.ir_press(key);
            ImGui::SameLine();
        }
        ImGui::NewLine();
        ImGui::SetNextItemWidth(160);
        if (ImGui::InputTextWithHint("##keys", "keys, e.g. !]]]}", ir_keys, sizeof ir_keys, ImGuiInputTextFlags_EnterReturnsTrue)) {
            m.ir_press(ir_keys);
            ir_keys[0] = 0;
        }
        ImGui::TextColored(DIM, "! menu/enter  ] next  [ up  } in  { back  - exit");
        ImGui::End();
    }

    void events() {
        ImGui::Begin(W_EVENTS, &show_events);
        ui_mono();
        for (auto& [k, n] : v.events)
            ImGui::Text("%-20s %llu", k.c_str(), (unsigned long long)n);
        ui_pop();
        ImGui::End();
    }

    // The LED panel as a strip on top; below it the Sign window, the debugger, and the serial terminals.
    void default_layout(ImGuiID dock) {
        ImGui::DockBuilderRemoveNode(dock);
        ImGui::DockBuilderAddNode(dock, ImGuiDockNodeFlags_DockSpace);
        ImVec2 size = ImGui::GetMainViewport()->WorkSize;
        ImGui::DockBuilderSetNodeSize(dock, size);
        if (sign_only) {
            ImGuiID s_top, s_rest;
            ImGui::DockBuilderSplitNode(dock, ImGuiDir_Up, 0.22f, &s_top, &s_rest);
            ImGui::DockBuilderDockWindow(W_PANEL, s_top);
            ImGui::DockBuilderDockWindow(SignUI::NAME, s_rest);
            ImGui::DockBuilderFinish(dock);
            later_focus = {SignUI::NAME};
            later_focus_frames = 3;
            return;
        }
        float strip = (size.x - ImGui::GetStyle().WindowPadding.x * 2) / panel_aspect + ImGui::GetStyle().WindowPadding.y * 2 + 2;
        float top_frac = std::clamp(strip / std::max(size.y, 1.0f), 0.10f, 0.35f);
        ImGuiID top, rest, left, rest2, right, center;
        ImGui::DockBuilderSplitNode(dock, ImGuiDir_Up, top_frac, &top, &rest);
        ImGui::DockBuilderSplitNode(rest, ImGuiDir_Left, 0.32f, &left, &rest2);
        ImGui::DockBuilderSplitNode(rest2, ImGuiDir_Right, 0.36f, &right, &center);
        ImGui::DockBuilderDockWindow(W_PANEL, top);
        ImGui::DockBuilderDockWindow(SignUI::NAME, left);
        for (const char* w : {W_DISASM, W_REGS, W_MEMORY, W_BREAKS, W_TASKS, W_EVENTS, W_TIMELINE})
            ImGui::DockBuilderDockWindow(w, center);
        for (const char* w : {W_DB9, W_RS485, W_REMOTE})   // (the IR remote last: our firmware has no menu)
            ImGui::DockBuilderDockWindow(w, right);
        if (auto* node = ImGui::DockBuilderGetNode(top))
            node->LocalFlags |= ImGuiDockNodeFlags_AutoHideTabBar;
        ImGui::DockBuilderFinish(dock);
        later_focus = {SignUI::NAME, W_DB9};             // each column's first tab
        later_focus_frames = 3;                          // (the dock nodes exist only from the next frame)
    }

    void frame() {
        v = m.view();
        if (was_running && !v.running)
            ;                                            // keep prev_regs from the last halt for change highlighting
        refresh_code(false);
        ImGuiViewport* vp = ImGui::GetMainViewport();
        // the toolbar on top, the dock space below
        float bar_h = ImGui::GetFrameHeightWithSpacing() + ImGui::GetStyle().WindowPadding.y * 2;
        ImGui::SetNextWindowPos(vp->WorkPos);
        ImGui::SetNextWindowSize(ImVec2(vp->WorkSize.x, bar_h));
        ImGui::Begin("##toolbar", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
                                               ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoDocking);
        toolbar();
        ImGui::End();
        float foot_h = ImGui::GetTextLineHeightWithSpacing() * 2 + ImGui::GetStyle().WindowPadding.y * 2;
        ImGui::SetNextWindowPos(ImVec2(vp->WorkPos.x, vp->WorkPos.y + vp->WorkSize.y - foot_h));
        ImGui::SetNextWindowSize(ImVec2(vp->WorkSize.x, foot_h));
        ImGui::Begin("##status", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
                                              ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoDocking);
        footer();
        ImGui::End();
        ImGui::SetNextWindowPos(ImVec2(vp->WorkPos.x, vp->WorkPos.y + bar_h));
        ImGui::SetNextWindowSize(ImVec2(vp->WorkSize.x, vp->WorkSize.y - bar_h - foot_h));
        ImGui::Begin("##dock", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings |
                                            ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_NoDocking);
        ImGuiID dock = ImGui::GetID("dock4");           // (4: the panel-strip layout; an older saved one is ignored)
        if (!layout_done || reset_layout) {
            if (reset_layout || !ImGui::DockBuilderGetNode(dock) || !ImGui::DockBuilderGetNode(dock)->IsSplitNode())
                default_layout(dock);
            layout_done = true;
            reset_layout = false;
        }
        ImGui::DockSpace(dock);
        ImGui::End();
        if (show_panel) panel();
        if (show_db9) terminal(db9, &show_db9);           // (drawn first: the right column's first tabs)
        if (show_rs485) terminal(rs485, &show_rs485);
        if (show_remote) remote();
        if (show_disasm) disassembly();
        if (show_memory) memory();
        if (show_regs) registers();
        if (show_breaks) breakpoints();
        if (show_tasks) tasks();
        if (show_events) events();
        if (show_timeline)
            timeline.draw(m, W_TIMELINE, &show_timeline);
        else
            m.tracing = false;
        if (sign) {
            sign->draw();
            if (sign->wants_focus())                     // a command from tools/signctl.py: show it happening
                ImGui::SetWindowFocus(SignUI::NAME);
        }
        if (later_focus_frames > 0 && --later_focus_frames == 0) {   // after a new layout: each column's first tab
            for (const char* w : later_focus)
                ImGui::SetWindowFocus(w);
            later_focus.clear();
            if (focus.empty())
                focus = W_DISASM;
        }
        if (!focus.empty()) {                            // after a layout reset, or --focus
            ImGui::SetWindowFocus(focus.c_str());
            focus.clear();
        }
        // keys
        if (!ImGui::GetIO().WantTextInput) {
            if (ImGui::IsKeyPressed(ImGuiKey_F5))
                m.run();
            if (ImGui::IsKeyPressed(ImGuiKey_F6))
                m.halt();
            if (ImGui::IsKeyPressed(ImGuiKey_F11) && !v.running)
                m.step(1);
            if (ImGui::IsKeyPressed(ImGuiKey_F10) && !v.running)
                m.step_over();
        }
        if (!v.running && was_running)
            ;
        if (v.running && !was_running)
            memcpy(prev_regs, v.regs, sizeof prev_regs);
        was_running = v.running;
    }
};

}  // namespace

// The window's place, kept across runs in build/sim/window.txt: the monitor's name, then x y w h and maximized.
// Restored only when that monitor is still there and the window would land on it.
static std::atomic<bool> g_quit{false};                  // SIGTERM / SIGINT: close as the window's close button does

static void window_save(SDL_Window* win, const std::string& path) {
    SDL_DisplayID d = SDL_GetDisplayForWindow(win);
    const char* name = d ? SDL_GetDisplayName(d) : nullptr;
    bool max = SDL_GetWindowFlags(win) & SDL_WINDOW_MAXIMIZED;
    int x = 0, y = 0, w = 0, h = 0;
    SDL_GetWindowPosition(win, &x, &y);
    SDL_GetWindowSize(win, &w, &h);
    if (!name || w <= 0 || h <= 0)
        return;
    if (FILE* f = fopen(path.c_str(), "w")) {
        fprintf(f, "%s\n%d %d %d %d %d\n", name, x, y, w, h, max ? 1 : 0);
        fclose(f);
    }
}

static void window_restore(SDL_Window* win, const std::string& path) {
    FILE* f = fopen(path.c_str(), "r");
    if (!f)
        return;
    char name[256] = {};
    int x, y, w, h, max;
    bool ok = fgets(name, sizeof name, f) && fscanf(f, "%d %d %d %d %d", &x, &y, &w, &h, &max) == 5;
    fclose(f);
    if (!ok)
        return;
    name[strcspn(name, "\r\n")] = 0;
    int n = 0;
    SDL_DisplayID* ds = SDL_GetDisplays(&n);
    for (int i = 0; ds && i < n; i++) {
        const char* dn = SDL_GetDisplayName(ds[i]);
        SDL_Rect b;
        if (!dn || strcmp(dn, name) != 0 || !SDL_GetDisplayBounds(ds[i], &b))
            continue;
        if (x + 64 > b.x + b.w || y + 32 > b.y + b.h || x + w < b.x + 64 || y < b.y - 32)
            continue;                                    // it would not land on that monitor any more
        SDL_SetWindowSize(win, w, h);
        SDL_SetWindowPosition(win, x, y);
        if (max)
            SDL_MaximizeWindow(win);
        break;
    }
    SDL_free(ds);
}

int main(int argc, char** argv) {
    std::string repo_dir, flash, app_path, model_name;
    bool cougar = false, run = false, window = true;
    int gdb_port = 1234;
    std::string shot, focus_window;
    bool pty = true;
    double shot_after = 5;
    for (int i = 1; i < argc; i++) {
        std::string a = argv[i];
        auto next = [&]() -> std::string { return i + 1 < argc ? argv[++i] : ""; };
        if (a == "--flash")
            flash = next();
        else if (a == "--app")
            app_path = next();
        else if (a == "--repo")
            repo_dir = next();
        else if (a == "--cougar")
            cougar = true;
        else if (a == "--model")
            model_name = next();
        else if (a == "--run")
            run = true;
        else if (a == "--gdb")
            gdb_port = std::stoi(next());
        else if (a == "--no-gdb")
            gdb_port = -1;
        else if (a == "--no-window")
            window = false;
        else if (a == "--no-pty")
            pty = false;
        else if (a == "--shot")
            shot = next();
        else if (a == "--focus")
            focus_window = next();
        else if (a == "--shot-after")
            shot_after = std::stod(next());
        else {
            fprintf(stderr, "usage: umami-marquee-sim [--flash FLASH.bin] [--app IMAGE] [--model ED0501] [--cougar] [--run] [--gdb PORT | --no-gdb]\n");
            return 2;
        }
    }
    std::unique_ptr<Repo> repo;
    try {
        repo = std::make_unique<Repo>(repo_dir);
    } catch (const std::exception& e) {
        fprintf(stderr, "%s\n", e.what());
        return 1;
    }
    MachineConfig cfg;
    if (flash.empty()) {                                 // our firmware when it is built, else a dump of the original
        flash = repo->path("bootloader/build-dump/bootflash-fw.bin");
        if (!std::ifstream(flash).good())
            flash = repo->path("dumps/bootflash.bin");
    }
    cfg.flash = flash;
    cfg.app = app_path;
    cfg.cougar = cougar;
    if (model_name.empty())
        model_name = "ED0501";                           // the real sign: 200x16 E-series (config EEPROM 0xC240)
    {
        const host::DisplayModel* dm = nullptr;
        for (auto& d : host::display_models())
            if (model_name == d.name)
                dm = &d;
        if (!dm) {
            fprintf(stderr, "unknown display model %s (e.g. ED0501, AD0501; studio/config.cpp display_models)\n", model_name.c_str());
            return 2;
        }
        cfg.geom_word = dm->word, cfg.geom_w = dm->w, cfg.geom_h = dm->h;
    }
    Machine m(*repo, cfg);
    App app(m, repo->symbols);
    if (m.from_source())
        app.db9.minor = 0;                                // the DB9 terminal: SCC1 on our firmware
    if (gdb_port >= 0) {
        app.gdb = std::make_unique<GdbServer>(m, gdb_port);
        if (!app.gdb->error.empty()) {
            app.message = app.gdb->error;
            app.gdb.reset();
        }
    }
    if (run)
        m.run();
    printf("simulating %s%s%s\n", cfg.flash.c_str(), cfg.app.empty() ? "" : " with ", cfg.app.c_str());
    std::unique_ptr<PtyBridge> pty_db9, pty_rs485;      // the emulated ports as devices, for host tools
    if (pty) {
        pty_db9 = std::make_unique<PtyBridge>(m, m.from_source() ? 0 : 4, repo->path("build/sim/db9"));
        pty_rs485 = std::make_unique<PtyBridge>(m, 1, repo->path("build/sim/rs485"));
        for (auto* b : {pty_db9.get(), pty_rs485.get()})
            if (b->device().empty() || !b->error().empty())
                fprintf(stderr, "%s\n", b->error().c_str());
            else
            {
                printf("%s: %s (-> %s)\n", b == pty_db9.get() ? "DB9" : "RS-485", b->link().c_str(),
                       b->device().c_str());
                std::string d = b->device();
                (b == pty_db9.get() ? app.db9_dev : app.rs485_dev) = d.substr(d.rfind('/') + 1);
            }
    }
    if (app.gdb)
        printf("gdb server on localhost:%d (target remote :%d)\n", app.gdb->port(), app.gdb->port());
    fflush(stdout);
    if (!window) {
        printf("no window: stdin closed or Ctrl-C ends it\n");
        fflush(stdout);
        while (getchar() != EOF)
            ;
        pty_db9.reset();
        pty_rs485.reset();
        app.gdb.reset();
        m.quit();
        return 0;
    }

    bool headless = !shot.empty();                       // --shot: no window on screen to take clicks
    if (headless)
        SDL_SetHint(SDL_HINT_VIDEO_DRIVER, "dummy");
    if (!SDL_Init(SDL_INIT_VIDEO)) {
        fprintf(stderr, "SDL_Init: %s\n", SDL_GetError());
        return 1;
    }
    float scale = headless ? 1 : SDL_GetDisplayContentScale(SDL_GetPrimaryDisplay());
    if (scale <= 0)
        scale = 1;
    SDL_Window* win = SDL_CreateWindow("Umami Marquee Sim", (int)(1600 * scale), (int)(1000 * scale),
                                       SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIGH_PIXEL_DENSITY | SDL_WINDOW_HIDDEN);
    SDL_Surface* target = nullptr;                       // headless: draw into a plain surface
    SDL_Renderer* ren = nullptr;
    if (headless) {
        target = SDL_CreateSurface((int)(1600 * scale), (int)(1000 * scale), SDL_PIXELFORMAT_XRGB8888);
        ren = target ? SDL_CreateSoftwareRenderer(target) : nullptr;
    } else
        ren = SDL_CreateRenderer(win, nullptr);
    if (!win || !ren) {
        fprintf(stderr, "SDL: %s\n", SDL_GetError());
        return 1;
    }
    SDL_SetRenderVSync(ren, 1);
    std::string win_file = repo->path("build/sim/window.txt");
    if (!headless) {
        window_restore(win, win_file);
        SDL_ShowWindow(win);
        std::signal(SIGTERM, [](int) { g_quit = true; });   // stopped from outside: save and close properly
        std::signal(SIGINT, [](int) { g_quit = true; });
    }

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_DockingEnable | ImGuiConfigFlags_NavEnableKeyboard;
    static std::string ini = repo->path("build/sim/umami-marquee-sim.ini");
    std::filesystem::create_directories(std::filesystem::path(ini).parent_path());
    io.IniFilename = ini.c_str();
    ui_apply_style(false, scale);                        // Tracy-like: tuned style, Fira fonts, icons (ui_style.cpp)
    ui_load_fonts(repo->root);
    ImGui_ImplSDL3_InitForSDLRenderer(win, ren);
    ImGui_ImplSDLRenderer3_Init(ren);
    app.sign = std::make_unique<SignUI>(repo->root.string(), !headless);   // screenshots: no remote control
    app.panel_player = std::make_unique<LedPlayer>(ren);
    if (!focus_window.empty())
        app.focus = focus_window;

    bool done = false;
    auto t_start = std::chrono::steady_clock::now();
    while (!done) {
        SDL_Event e;
        while (SDL_PollEvent(&e)) {
            ImGui_ImplSDL3_ProcessEvent(&e);
            if (e.type == SDL_EVENT_QUIT || (e.type == SDL_EVENT_WINDOW_CLOSE_REQUESTED && e.window.windowID == SDL_GetWindowID(win)))
                done = true;
            if (!headless && (e.type == SDL_EVENT_WINDOW_MOVED || e.type == SDL_EVENT_WINDOW_RESIZED ||
                              e.type == SDL_EVENT_WINDOW_MAXIMIZED || e.type == SDL_EVENT_WINDOW_RESTORED ||
                              e.type == SDL_EVENT_WINDOW_DISPLAY_CHANGED))
                window_save(win, win_file);
        }
        if (g_quit)
            done = true;
        if (!headless && (SDL_GetWindowFlags(win) & SDL_WINDOW_MINIMIZED)) {
            if (app.sign)
                app.sign->remote_step();                 // remote commands keep running while minimized
            SDL_Delay(20);
            continue;
        }
        ImGui_ImplSDLRenderer3_NewFrame();
        ImGui_ImplSDL3_NewFrame();
        ImGui::NewFrame();
        app.frame();
        ImGui::Render();
        SDL_SetRenderScale(ren, io.DisplayFramebufferScale.x, io.DisplayFramebufferScale.y);
        SDL_SetRenderDrawColor(ren, 22, 22, 26, 255);
        SDL_RenderClear(ren);
        ImGui_ImplSDLRenderer3_RenderDrawData(ImGui::GetDrawData(), ren);
        if (!shot.empty() && std::chrono::duration<double>(std::chrono::steady_clock::now() - t_start).count() > shot_after) {
            SDL_FlushRenderer(ren);
            SDL_Surface* s = headless ? SDL_DuplicateSurface(target) : SDL_RenderReadPixels(ren, nullptr);
            if (s) {
                SDL_SaveBMP(s, shot.c_str());
                SDL_DestroySurface(s);
            } else
                fprintf(stderr, "--shot: %s\n", SDL_GetError());
            done = true;
        }
        SDL_RenderPresent(ren);
    }
    pty_db9.reset();                                     // the devices, before the machine
    pty_rs485.reset();
    app.gdb.reset();                                     // stop the GDB server before the machine
    m.quit();                                            // join the emulation thread (it closes Unicorn)
    app.sign.reset();
    app.panel_player.reset();
    ImGui_ImplSDLRenderer3_Shutdown();
    ImGui_ImplSDL3_Shutdown();
    ImGui::DestroyContext();
    SDL_DestroyRenderer(ren);
    SDL_DestroyWindow(win);
    SDL_Quit();
    return 0;
}
