// The look of umami-marquee-sim, after Tracy's use of Dear ImGui: a proportional UI font (Fira Sans) with Font Awesome
// icons merged in, a monospace font (Fira Code) for code, registers, hex and serial text, and a tuned style where
// colour carries meaning. The fonts are vendored in third_party/fonts-ui (THIRD-PARTY.md).
#pragma once

#include "imgui.h"

#include <filesystem>

namespace sim {

struct UiFonts {
    ImFont* ui = nullptr;                                // the default (Fira Sans + icons)
    ImFont* bold = nullptr;                              // headings (Fira Sans SemiBold + icons)
    ImFont* mono = nullptr;                              // code, hex, registers, terminals (Fira Code + icons)
};
extern UiFonts ui_fonts;

// Loads the fonts (falling back to the system's monospace font when the vendored ones are missing).
void ui_load_fonts(const std::filesystem::path& repo);
// The dark or light style with the semantic colours below; scale is the display's content scale.
void ui_apply_style(bool light, float scale);

// Semantic colours (the same in both themes, picked to read on either background).
namespace col {
inline const ImVec4 running(0.36f, 0.80f, 0.45f, 1);     // running, OK, done
inline const ImVec4 halted(0.98f, 0.72f, 0.25f, 1);      // halted, warnings
inline const ImVec4 error(0.95f, 0.36f, 0.36f, 1);       // errors, hang
inline const ImVec4 info(0.40f, 0.76f, 0.98f, 1);        // gdb, links, status
inline const ImVec4 accent(0.98f, 0.62f, 0.18f, 1);      // the sign's amber
}  // namespace col

// A monospace span: ui_mono() ... ui_pop().
inline void ui_mono() { ImGui::PushFont(ui_fonts.mono, 0.0f); }
inline void ui_bold() { ImGui::PushFont(ui_fonts.bold, 0.0f); }
inline void ui_pop() { ImGui::PopFont(); }

}  // namespace sim
