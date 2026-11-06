// The sign's fonts (content/fonts/fonteNN.png) as the firmware builds and measures them (fonts.py, sign_emu.FwFont):
// a 16 x 16 grid of all 256 codes, cell = (w/16, h/16), ink where the RGB555 value is not 0.
#pragma once

#include "paths.h"
#include "text.h"

#include <memory>
#include <optional>
#include <string>

namespace host {

struct FwFont {
    std::string name;
    int w = 0, h = 0, cw = 0, ch = 0;
    std::vector<uint8_t> ink;                            // h x w, 1 = ink
    int left[256] = {}, width[256] = {}, right[256] = {}, prop[256] = {};

    FwFont() = default;
    FwFont(const std::string& png_path, const std::string& name);
    FwFont(std::vector<uint8_t> ink, int w, int h, const std::string& name);
    bool at(int x, int y) const { return ink[(size_t)y * w + x] != 0; }
    // text_width (0x43BC0): inked widths, a 1-px gap after each proportional glyph and one more before a
    // proportional glyph that follows a fixed one.
    int text_width(const Bytes& data) const;

private:
    void measure_all();
    void measure(int c);
};

using Fonts = std::vector<std::shared_ptr<const FwFont>>;

// fonte00..fonte99.png in the content folder, as fonts_load numbers them.
std::vector<std::string> sign_font_names(const Paths& p);
// The FwFont of each (cached by file time).
Fonts sign_fonts(const Paths& p);
// A content font by name (from sign_fonts when it is one of them).
std::shared_ptr<const FwFont> fw_font(const Paths& p, const std::string& name);

// font_pick_largest (0x44750), mode 0: the widest font whose cell height fits; ties go to the later one.
std::optional<size_t> font_pick_largest(const Fonts& fonts, int box_h);
// text_draw_justified_maybe (0x43F20) for a centred segment: the tallest font that fits the box for the text.
std::optional<size_t> font_pick_for_text(const Fonts& fonts, const Bytes& text, int box_w, int box_h);
// The font textline_init picks for an H-high line ("" when there are no fonts).
std::string default_font(int H, const Fonts& fonts);

}  // namespace host
