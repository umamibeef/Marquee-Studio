// Put lines on the simulated sign the way its own attract configuration does: the text into MESSAGE_TABLE (256 bytes
// a line), the line's enable into cfg_attract_line_mode[], its FG<<16|BG effect into cfg_attract_line_effect[]. The
// stock attract loop (display_main_loop, state 1 substate 0xc / state 2) then cycles the enabled lines by itself.
// Used by umami-marquee-sim-test attracttest (a firmware check; content design is the web Studio's job).
#pragma once
#include <cstdint>
#include <string>
#include <vector>

namespace sim {

class Board;

constexpr uint32_t MESSAGE_TABLE_PTR = 0x45C374;          // message_table_ptr (message_table_init)
constexpr uint32_t ATTRACT_LINE_MODE = 0xDD2B4;           // cfg_attract_line_mode[16], u32 each
constexpr uint32_t ATTRACT_LINE_EFFECT = 0xDD2F4;         // cfg_attract_line_effect[16], FG<<16 | BG
constexpr uint32_t ATTRACT_LINE_OUTLINE = 0xDD334;        // cfg_attract_line_outline[16], {style, colour<<16}

struct AttractLine {
    int line;                                            // 0..15
    std::string text;                                    // the line's bytes (plain text or encoded markup)
    uint32_t effect;
};

// Writes the lines; with exclusive, every other attract line is switched off. False before the firmware has set up
// its message table (early in its boot).
bool write_attract_lines(Board& b, const std::vector<AttractLine>& lines, bool exclusive = true);

}  // namespace sim
