#include "timeline.h"

#include "ui_style.h"

#include "IconsFontAwesome6.h"
#include "imgui.h"

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace sim {

namespace {

constexpr double PER_S = CPU_MIPS * 1e6;                 // instructions per simulated second
constexpr size_t MAX_EVENTS = 4u << 20;

ImU32 task_colour(uint16_t i) {                          // a distinct, stable colour per task
    float h = std::fmod(i * 0.618034f + 0.08f, 1.0f), r, g, b;
    ImGui::ColorConvertHSVtoRGB(h, 0.55f, 0.85f, r, g, b);
    return IM_COL32((int)(r * 255), (int)(g * 255), (int)(b * 255), 255);
}

std::string fmt_time(double s) {
    char b[32];
    if (std::fabs(s) >= 1)
        snprintf(b, sizeof b, "%.3f s", s);
    else if (std::fabs(s) >= 1e-3)
        snprintf(b, sizeof b, "%.3f ms", s * 1e3);
    else
        snprintf(b, sizeof b, "%.1f us", s * 1e6);
    return b;
}

struct Lane {
    const char* label;
    uint8_t kind;
    int minor;                                           // -1: any
    ImU32 colour;
};

}  // namespace

void Timeline::take(Machine& m) {
    std::vector<std::string> names;
    auto ev = m.take_trace(&names);
    if (!names.empty()) {
        // an internal RTEMS task (the idle task) is named by a pointer to a string, which task_name shows in hex
        for (auto& n : names)
            if (n.size() == 8 && n.find_first_not_of("0123456789abcdef") == std::string::npos) {
                uint8_t s[12] = {};
                uint32_t a = (uint32_t)std::stoul(n, nullptr, 16);
                if (a && m.read_mem(a, s, 8)) {
                    std::string str;
                    for (uint8_t c : s)
                        if (c >= 32 && c < 127)
                            str += (char)c;
                        else
                            break;
                    if (str.size() >= 2)
                        n = str + " (" + n + ")";
                }
            }
        names_ = names;
    }
    ev_.insert(ev_.end(), ev.begin(), ev.end());
    if (ev_.size() > MAX_EVENTS)                         // keep the newest half
        ev_.erase(ev_.begin(), ev_.begin() + (ev_.size() - MAX_EVENTS / 2));
}

void Timeline::draw(Machine& m, const char* title, bool* open) {
    m.tracing = record_;
    take(m);
    if (!ImGui::Begin(title, open)) {
        ImGui::End();
        return;
    }
    // the toolbar
    if (ImGui::Button(record_ ? ICON_FA_CIRCLE " Recording" : ICON_FA_CIRCLE_PAUSE " Paused"))
        record_ = !record_;
    ImGui::SameLine();
    ImGui::Checkbox("Follow", &follow_);
    ImGui::SameLine();
    if (ImGui::Button(ICON_FA_TRASH " Clear"))
        ev_.clear();
    ImGui::SameLine();
    if (ImGui::Button(ICON_FA_EXPAND " Fit") && !ev_.empty()) {
        t0_ = (double)ev_.front().t;
        t1_ = (double)ev_.back().t + 1;
        follow_ = false;
    }
    ImGui::SameLine();
    ImGui::TextDisabled("%zu events, view %s", ev_.size(), fmt_time((t1_ - t0_) / PER_S).c_str());

    // the lanes: one per task, then the ticks
    std::vector<Lane> lanes;
    for (size_t i = 0; i < names_.size(); i++)
        lanes.push_back({names_[i].c_str(), Board::TR_TASK, (int)i, task_colour((uint16_t)i)});
    lanes.push_back({"interrupts", Board::TR_IRQ, -1, IM_COL32(230, 120, 90, 255)});
    lanes.push_back({"PIT ticks", Board::TR_PIT, -1, IM_COL32(150, 150, 160, 255)});
    lanes.push_back({"panel frames", Board::TR_FRAME, -1, IM_COL32(250, 160, 50, 255)});
    lanes.push_back({"RS-485 rx", Board::TR_RX, 1, IM_COL32(90, 200, 120, 255)});
    lanes.push_back({"RS-485 tx", Board::TR_TX, 1, IM_COL32(90, 170, 240, 255)});
    lanes.push_back({"DB9 rx", Board::TR_RX, 4, IM_COL32(90, 200, 120, 255)});
    lanes.push_back({"DB9 tx", Board::TR_TX, 4, IM_COL32(90, 170, 240, 255)});
    lanes.push_back({"remote keys", Board::TR_IR, -1, IM_COL32(200, 130, 240, 255)});
    lanes.push_back({"stops", Board::TR_STOP, -1, IM_COL32(240, 80, 80, 255)});

    const float label_w = 120, lane_h = ImGui::GetTextLineHeight() + 6, ruler_h = ImGui::GetTextLineHeight() + 8;
    ImVec2 p0 = ImGui::GetCursorScreenPos(), avail = ImGui::GetContentRegionAvail();
    float h = std::max(avail.y, ruler_h + lane_h * lanes.size());
    float x0 = p0.x + label_w, x1 = p0.x + avail.x, w = std::max(1.0f, x1 - x0);
    ImGui::InvisibleButton("canvas", ImVec2(avail.x, h), ImGuiButtonFlags_MouseButtonLeft | ImGuiButtonFlags_MouseButtonRight);
    bool hovered = ImGui::IsItemHovered();
    ImGuiIO& io = ImGui::GetIO();

    if (follow_ && !ev_.empty()) {                       // the newest events at the right edge
        double span = t1_ - t0_;
        t1_ = (double)ev_.back().t + span * 0.02;
        t0_ = t1_ - span;
    }
    if (hovered && io.MouseWheel != 0) {                 // zoom around the mouse
        double at = t0_ + (io.MousePos.x - x0) / w * (t1_ - t0_);
        double k = std::pow(0.85, io.MouseWheel);
        double span = std::clamp((t1_ - t0_) * k, 200.0, 3600 * PER_S);
        t0_ = at - (at - t0_) / (t1_ - t0_) * span;
        t1_ = t0_ + span;
        follow_ = follow_ && io.MouseWheel < 0;
    }
    if (ImGui::IsItemActive() && (ImGui::IsMouseDragging(0) || ImGui::IsMouseDragging(1))) {   // pan
        double dt = -io.MouseDelta.x / w * (t1_ - t0_);
        t0_ += dt;
        t1_ += dt;
        if (io.MouseDelta.x != 0)
            follow_ = false;
    }
    auto X = [&](double t) { return (float)(x0 + (t - t0_) / (t1_ - t0_) * w); };

    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->PushClipRect(ImVec2(x0, p0.y), ImVec2(x1, p0.y + h), true);
    dl->AddRectFilled(ImVec2(x0, p0.y), ImVec2(x1, p0.y + h), ImGui::GetColorU32(ImGuiCol_FrameBg, 0.5f));
    // the ruler: a step of 1, 2 or 5 x 10^n that gives about 110 px between labels
    double span_s = (t1_ - t0_) / PER_S, step = std::pow(10, std::floor(std::log10(span_s * 110 / w)));
    for (double m2 : {1.0, 2.0, 5.0, 10.0})
        if (span_s * 110 / w <= step * m2) {
            step *= m2;
            break;
        }
    ImU32 grid = ImGui::GetColorU32(ImGuiCol_Border, 0.6f), text = ImGui::GetColorU32(ImGuiCol_TextDisabled);
    ui_mono();
    for (double s = std::floor(t0_ / PER_S / step) * step; s * PER_S <= t1_; s += step) {
        float x = X(s * PER_S);
        dl->AddLine(ImVec2(x, p0.y + ruler_h - 4), ImVec2(x, p0.y + h), grid);
        dl->AddText(ImVec2(x + 3, p0.y + 2), text, fmt_time(s).c_str());
    }
    ui_pop();
    dl->PopClipRect();

    // visible events (time order): from just before the view, for the task that was running at its left edge
    auto first = std::lower_bound(ev_.begin(), ev_.end(), (uint64_t)std::max(0.0, t0_),
                                  [](const Board::TraceEvent& e, uint64_t t) { return e.t < t; });
    auto last = std::upper_bound(ev_.begin(), ev_.end(), (uint64_t)std::max(0.0, t1_),
                                 [](uint64_t t, const Board::TraceEvent& e) { return t < e.t; });
    std::string tip;
    for (size_t li = 0; li < lanes.size(); li++) {
        const Lane& L = lanes[li];
        float y = p0.y + ruler_h + lane_h * li;
        dl->AddText(ImVec2(p0.x + 4, y + 2), L.kind == Board::TR_TASK ? L.colour : ImGui::GetColorU32(ImGuiCol_Text),
                    L.label);
        dl->AddLine(ImVec2(x0, y + lane_h), ImVec2(x1, y + lane_h), ImGui::GetColorU32(ImGuiCol_Border, 0.25f));
        dl->PushClipRect(ImVec2(x0, y), ImVec2(x1, y + lane_h), true);
        if (L.kind == Board::TR_TASK) {
            // a bar from each switch to this task until the next switch
            auto it = first;
            while (it != ev_.begin()) {                  // the switch in force at the left edge
                --it;
                if (it->kind == Board::TR_TASK)
                    break;
            }
            for (; it != ev_.end() && it <= last; ++it) {
                if (it->kind != Board::TR_TASK || it->b != L.minor)
                    continue;
                auto next = it + 1;
                while (next != ev_.end() && next->kind != Board::TR_TASK)
                    ++next;
                double end = next == ev_.end() ? (double)ev_.back().t : (double)next->t;
                float a = std::max(x0, X((double)it->t)), b = std::min(x1, std::max(a + 1.0f, X(end)));
                if (b < x0 || a > x1)
                    continue;
                dl->AddRectFilled(ImVec2(a, y + 3), ImVec2(b, y + lane_h - 3), L.colour, 2);
                if (b - a > 60) {
                    std::string d = fmt_time((end - (double)it->t) / PER_S);
                    dl->AddText(ImVec2(a + 4, y + 3), IM_COL32(20, 20, 24, 255), d.c_str());
                }
                if (hovered && io.MousePos.y >= y && io.MousePos.y < y + lane_h && io.MousePos.x >= a && io.MousePos.x <= b)
                    tip = std::string(L.label) + "\nfrom " + fmt_time((double)it->t / PER_S) + " for " +
                          fmt_time((end - (double)it->t) / PER_S);
            }
        } else {
            // ticks, at most one per pixel column (Tracy merges what it cannot separate)
            float last_x = -1e9f;
            size_t merged = 0, bytes = 0;
            for (auto it = first; it != last; ++it) {
                if (it->kind != L.kind || (L.minor >= 0 && it->a != L.minor))
                    continue;
                float x = X((double)it->t);
                if (x - last_x < 1.0f) {
                    merged++;
                    bytes += it->b;
                    continue;
                }
                last_x = x;
                merged = 1;
                bytes = it->b;
                float top = y + 3;
                if (L.kind == Board::TR_RX || L.kind == Board::TR_TX)   // taller for more bytes
                    top = y + lane_h - 3 - std::min(lane_h - 6, 3.0f + std::log2(1.0f + it->b) * 2);
                dl->AddLine(ImVec2(x, top), ImVec2(x, y + lane_h - 3), L.colour, L.kind == Board::TR_STOP ? 2.0f : 1.0f);
                if (hovered && std::fabs(io.MousePos.x - x) < 3 && io.MousePos.y >= y && io.MousePos.y < y + lane_h) {
                    tip = std::string(L.label) + " at " + fmt_time((double)it->t / PER_S);
                    if (L.kind == Board::TR_RX || L.kind == Board::TR_TX)
                        tip += "\n" + std::to_string(it->b) + " bytes";
                    if (L.kind == Board::TR_IR)
                        tip += std::string("\nkey '") + (char)it->a + "'";
                }
            }
            (void)merged;
            (void)bytes;
        }
        dl->PopClipRect();
    }
    if (hovered && io.MousePos.x >= x0) {                // the cursor's time
        double t = t0_ + (io.MousePos.x - x0) / w * (t1_ - t0_);
        dl->AddLine(ImVec2(io.MousePos.x, p0.y), ImVec2(io.MousePos.x, p0.y + h), ImGui::GetColorU32(ImVec4(col::accent.x, col::accent.y, col::accent.z, 0.6f)));
        ImGui::SetTooltip("%s%s%s", fmt_time(t / PER_S).c_str(), tip.empty() ? "" : "\n", tip.c_str());
    }
    if (ev_.empty())
        dl->AddText(ImVec2(x0 + 8, p0.y + ruler_h + 4), text,
                    record_ ? "Run the sign: its task switches, interrupts, frames and serial traffic show here."
                            : "Paused: press Recording to record again.");
    ImGui::End();
}

}  // namespace sim
