#include "attract_lines.h"

#include "board.h"

namespace sim {

static void put32(std::vector<uint8_t>& v, size_t o, uint32_t x) {
    v[o] = (uint8_t)(x >> 24);
    v[o + 1] = (uint8_t)(x >> 16);
    v[o + 2] = (uint8_t)(x >> 8);
    v[o + 3] = (uint8_t)x;
}

bool write_attract_lines(Board& b, const std::vector<AttractLine>& lines, bool exclusive) {
    uint32_t table = b.r32(MESSAGE_TABLE_PTR);
    if (!table)
        return false;
    std::vector<uint8_t> mode(64, 0), eff(64, 0), outline(128, 0);
    b.read(ATTRACT_LINE_MODE, mode.data(), mode.size());
    b.read(ATTRACT_LINE_EFFECT, eff.data(), eff.size());
    b.read(ATTRACT_LINE_OUTLINE, outline.data(), outline.size());
    if (exclusive)
        for (int i = 0; i < 16; i++)
            put32(mode, 4 * i, 0);
    for (auto& l : lines) {
        if (l.line < 0 || l.line > 15)
            continue;
        std::vector<uint8_t> text(l.text.begin(), l.text.end());
        text.resize(256, 0);                             // the whole line: the rest NUL
        text[255] = 0;
        b.write(table + 256 * l.line, text.data(), text.size());
        put32(mode, 4 * l.line, 1);
        put32(eff, 4 * l.line, l.effect);
        put32(outline, 8 * l.line, 0);                   // no outline
        put32(outline, 8 * l.line + 4, 0);
    }
    b.write(ATTRACT_LINE_MODE, mode.data(), mode.size());
    b.write(ATTRACT_LINE_EFFECT, eff.data(), eff.size());
    b.write(ATTRACT_LINE_OUTLINE, outline.data(), outline.size());
    return true;
}

}  // namespace sim
