#include "ui_style.h"

#include "IconsFontAwesome6.h"

namespace sim {

UiFonts ui_fonts;

static ImFont* add(const std::filesystem::path& file, float size, bool icons, const std::filesystem::path& fa) {
    ImGuiIO& io = ImGui::GetIO();
    if (!std::filesystem::exists(file))
        return nullptr;
    ImFont* f = io.Fonts->AddFontFromFileTTF(file.string().c_str(), size);
    if (f && icons && std::filesystem::exists(fa)) {     // Font Awesome merged in, as Tracy does
        ImFontConfig c;
        c.MergeMode = true;
        c.GlyphMinAdvanceX = size;                       // icons square, so labels line up
        c.GlyphOffset = ImVec2(0, 1);
        io.Fonts->AddFontFromFileTTF(fa.string().c_str(), size * 0.85f, &c);
    }
    return f;
}

void ui_load_fonts(const std::filesystem::path& repo) {
    auto dir = repo / "third_party" / "fonts-ui";
    auto fa = dir / "fa-solid-900.ttf";
    ui_fonts.ui = add(dir / "FiraSans-Regular.ttf", 15.0f, true, fa);
    ui_fonts.bold = add(dir / "FiraSans-SemiBold.ttf", 15.0f, true, fa);
    ui_fonts.mono = add(dir / "FiraCode-Regular.ttf", 14.0f, true, fa);
    if (!ui_fonts.mono)
        for (const char* f : {"/System/Library/Fonts/SFNSMono.ttf", "/System/Library/Fonts/Menlo.ttc",
                              "/usr/share/fonts/truetype/dejavu/DejaVuSansMono.ttf"})
            if ((ui_fonts.mono = add(f, 14.0f, false, fa)))
                break;
    if (!ui_fonts.ui)
        ui_fonts.ui = ui_fonts.mono ? ui_fonts.mono : ImGui::GetIO().Fonts->AddFontDefault();
    if (!ui_fonts.bold)
        ui_fonts.bold = ui_fonts.ui;
    if (!ui_fonts.mono)
        ui_fonts.mono = ui_fonts.ui;
    ImGui::GetIO().FontDefault = ui_fonts.ui;
}

void ui_apply_style(bool light, float scale) {
    ImGuiStyle& s = ImGui::GetStyle();
    s = ImGuiStyle();
    light ? ImGui::StyleColorsLight(&s) : ImGui::StyleColorsDark(&s);
    s.WindowPadding = ImVec2(8, 8);
    s.FramePadding = ImVec2(6, 4);
    s.ItemSpacing = ImVec2(7, 5);
    s.ItemInnerSpacing = ImVec2(5, 4);
    s.ScrollbarSize = 13;
    s.GrabMinSize = 10;
    s.WindowRounding = 5;
    s.ChildRounding = 4;
    s.FrameRounding = 4;
    s.PopupRounding = 5;
    s.ScrollbarRounding = 6;
    s.GrabRounding = 4;
    s.TabRounding = 4;
    s.WindowBorderSize = 1;
    s.FrameBorderSize = 0;
    s.TabBorderSize = 0;
    s.SeparatorTextBorderSize = 1;
    s.SeparatorTextPadding = ImVec2(10, 4);
    s.DockingSeparatorSize = 2;
    ImVec4* c = s.Colors;
    if (!light) {                                        // charcoal panels, the sign's amber as the accent
        ImVec4 bg(0.105f, 0.110f, 0.125f, 1), panel(0.135f, 0.142f, 0.160f, 1), frame(0.185f, 0.195f, 0.220f, 1);
        ImVec4 amber = col::accent;
        c[ImGuiCol_WindowBg] = panel;
        c[ImGuiCol_ChildBg] = ImVec4(0, 0, 0, 0);
        c[ImGuiCol_PopupBg] = ImVec4(0.12f, 0.125f, 0.145f, 0.98f);
        c[ImGuiCol_Border] = ImVec4(0.26f, 0.27f, 0.30f, 0.6f);
        c[ImGuiCol_FrameBg] = frame;
        c[ImGuiCol_FrameBgHovered] = ImVec4(0.24f, 0.25f, 0.29f, 1);
        c[ImGuiCol_FrameBgActive] = ImVec4(0.28f, 0.29f, 0.34f, 1);
        c[ImGuiCol_TitleBg] = bg;
        c[ImGuiCol_TitleBgActive] = bg;
        c[ImGuiCol_MenuBarBg] = bg;
        c[ImGuiCol_ScrollbarBg] = ImVec4(0, 0, 0, 0);
        c[ImGuiCol_CheckMark] = amber;
        c[ImGuiCol_SliderGrab] = ImVec4(amber.x, amber.y, amber.z, 0.8f);
        c[ImGuiCol_SliderGrabActive] = amber;
        c[ImGuiCol_Button] = ImVec4(0.22f, 0.23f, 0.27f, 1);
        c[ImGuiCol_ButtonHovered] = ImVec4(0.30f, 0.31f, 0.36f, 1);
        c[ImGuiCol_ButtonActive] = ImVec4(amber.x * 0.55f, amber.y * 0.55f, amber.z * 0.55f, 1);
        c[ImGuiCol_Header] = ImVec4(0.24f, 0.25f, 0.29f, 1);
        c[ImGuiCol_HeaderHovered] = ImVec4(0.30f, 0.31f, 0.36f, 1);
        c[ImGuiCol_HeaderActive] = ImVec4(0.34f, 0.35f, 0.40f, 1);
        c[ImGuiCol_Separator] = ImVec4(0.26f, 0.27f, 0.30f, 1);
        c[ImGuiCol_Tab] = bg;
        c[ImGuiCol_TabHovered] = ImVec4(0.30f, 0.31f, 0.36f, 1);
        c[ImGuiCol_TabSelected] = panel;
        c[ImGuiCol_TabSelectedOverline] = amber;
        c[ImGuiCol_TabDimmed] = bg;
        c[ImGuiCol_TabDimmedSelected] = panel;
        c[ImGuiCol_DockingPreview] = ImVec4(amber.x, amber.y, amber.z, 0.5f);
        c[ImGuiCol_DockingEmptyBg] = bg;
        c[ImGuiCol_TextSelectedBg] = ImVec4(amber.x, amber.y, amber.z, 0.30f);
        c[ImGuiCol_NavCursor] = amber;
        c[ImGuiCol_PlotHistogram] = amber;
        c[ImGuiCol_TableHeaderBg] = frame;
        c[ImGuiCol_TableRowBgAlt] = ImVec4(1, 1, 1, 0.025f);
        c[ImGuiCol_Text] = ImVec4(0.90f, 0.91f, 0.93f, 1);
        c[ImGuiCol_TextDisabled] = ImVec4(0.55f, 0.57f, 0.62f, 1);
    } else {
        c[ImGuiCol_TabSelectedOverline] = ImVec4(0.85f, 0.50f, 0.08f, 1);
        c[ImGuiCol_CheckMark] = ImVec4(0.80f, 0.45f, 0.05f, 1);
    }
    s.ScaleAllSizes(scale);
    s.FontScaleDpi = scale;
}

}  // namespace sim
