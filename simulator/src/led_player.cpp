#include "led_player.h"

#include <algorithm>
#include <cmath>

namespace sim {

LedPlayer::~LedPlayer() {
    if (tex_)
        SDL_DestroyTexture(tex_);
}

void LedPlayer::set(std::vector<std::vector<uint16_t>> frames, int w, int h, bool keep_position) {
    size_t at = keep_position ? i_ : 0;
    if (w != w_ || h != h_)
        vis_.clear();
    frames_ = std::move(frames);
    w_ = w;
    h_ = h;
    i_ = at < frames_.size() ? at : 0;
    acc_ = 0;
}

void LedPlayer::seek(size_t i) {
    if (!frames_.empty())
        i_ = std::min(i, frames_.size() - 1);
    acc_ = 0;
}

void LedPlayer::advance(double now, float persist) {
    double dt = last_ < 0 ? 0 : (now - last_) * 1000;
    last_ = now;
    size_t n = (size_t)w_ * h_ * 3;
    if (vis_.size() != n)
        vis_.assign(n, 0);
    else {
        float k = persist > 0 ? (float)std::pow(persist, dt / 25.0) : 0;
        for (float& v : vis_)
            v *= k;
    }
    if (playing && frames_.size() > 1) {
        acc_ += dt;
        while (acc_ >= 25) {
            acc_ -= 25;
            i_ = (i_ + 1) % frames_.size();
        }
    }
    const auto& f = frames_[i_];
    for (size_t p = 0; p < f.size() && p * 3 + 2 < vis_.size(); p++) {
        float r = (float)(f[p] >> 10 & 31), g = (float)(f[p] >> 5 & 31), b = (float)(f[p] & 31);
        vis_[p * 3] = std::max(vis_[p * 3], r);
        vis_[p * 3 + 1] = std::max(vis_[p * 3 + 1], g);
        vis_[p * 3 + 2] = std::max(vis_[p * 3 + 2], b);
    }
}

void LedPlayer::draw(const LedLook& look, float max_w, float max_h) {
    if (frames_.empty() || !w_ || !h_) {
        ImGui::Dummy(ImVec2(max_w, max_h > 0 ? std::min(max_h, max_w / 5) : max_w / 5));
        return;
    }
    advance(ImGui::GetTime(), look.persist);
    float pitch = max_w / w_;
    if (max_h > 0)
        pitch = std::min(pitch, max_h / h_);
    {                                                    // centred in the space given
        ImVec2 c = ImGui::GetCursorScreenPos();
        float ox = std::max(0.0f, (max_w - pitch * w_) / 2), oy = max_h > 0 ? std::max(0.0f, (max_h - pitch * h_) / 2) : 0;
        ImGui::SetCursorScreenPos(ImVec2(std::floor(c.x + ox), std::floor(c.y + oy)));
    }
    uint8_t lut[32 * 32 + 1];                            // 1/32-step levels for the smooth afterglow
    for (int i = 0; i <= 32 * 32; i++) {
        double x = std::pow(std::min(31.0, i / 32.0) / 31, look.gamma) * look.brightness;
        lut[i] = (uint8_t)std::clamp((int)std::lround(x * 255), 0, 255);
    }
    auto level = [&](float v) { return lut[std::clamp((int)std::lround(v * 32), 0, 32 * 32)]; };
    ImVec2 p0 = ImGui::GetCursorScreenPos();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->AddRectFilled(p0, ImVec2(p0.x + pitch * w_, p0.y + pitch * h_), IM_COL32(5, 5, 6, 255));
    if (look.dots && pitch >= 2.5f) {
        float rad = pitch * 0.4f;
        int seg = pitch > 8 ? 12 : 8;
        for (int y = 0; y < h_; y++)
            for (int x = 0; x < w_; x++) {
                size_t p = (size_t)y * w_ + x;
                uint8_t r = level(vis_[p * 3]), g = level(vis_[p * 3 + 1]), b = level(vis_[p * 3 + 2]);
                ImVec2 c(p0.x + pitch * (x + 0.5f), p0.y + pitch * (y + 0.5f));
                if (look.unlit) {                        // a lit LED is never darker than an unlit one
                    r = std::max<uint8_t>(r, 22), g = std::max<uint8_t>(g, 22), b = std::max<uint8_t>(b, 26);
                } else if (r + g + b < 3)
                    continue;
                dl->AddCircleFilled(c, rad, IM_COL32(r, g, b, 255), seg);
            }
    } else if (ren_) {                                   // square pixels: one texture
        if (!tex_ || tex_w_ != w_ || tex_h_ != h_) {
            if (tex_)
                SDL_DestroyTexture(tex_);
            tex_ = SDL_CreateTexture(ren_, SDL_PIXELFORMAT_ABGR8888, SDL_TEXTUREACCESS_STREAMING, w_, h_);
            SDL_SetTextureScaleMode(tex_, SDL_SCALEMODE_NEAREST);
            tex_w_ = w_;
            tex_h_ = h_;
        }
        std::vector<uint8_t> px((size_t)w_ * h_ * 4);
        for (size_t p = 0; p < (size_t)w_ * h_; p++) {
            uint8_t lo_rg = look.unlit ? 22 : 0, lo_b = look.unlit ? 26 : 0;   // never darker than unlit
            px[p * 4] = std::max(level(vis_[p * 3]), lo_rg);
            px[p * 4 + 1] = std::max(level(vis_[p * 3 + 1]), lo_rg);
            px[p * 4 + 2] = std::max(level(vis_[p * 3 + 2]), lo_b);
            px[p * 4 + 3] = 255;
        }
        SDL_UpdateTexture(tex_, nullptr, px.data(), w_ * 4);
        dl->AddImage((ImTextureID)(intptr_t)tex_, p0, ImVec2(p0.x + pitch * w_, p0.y + pitch * h_));
    }
    ImGui::Dummy(ImVec2(pitch * w_, pitch * h_));
}

void LedPlayer::controls() {
    if (ImGui::SmallButton(playing ? "Pause" : "Play"))
        playing = !playing;
    ImGui::SameLine();
    int i = (int)i_, n = std::max(1, (int)frames_.size());
    ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x - 110);
    if (ImGui::SliderInt("##seek", &i, 0, n - 1, "")) {
        seek((size_t)i);
        playing = false;
    }
    ImGui::SameLine();
    ImGui::TextDisabled("%.2f / %.2f s", i_ * 0.025, n * 0.025);
}

}  // namespace sim
