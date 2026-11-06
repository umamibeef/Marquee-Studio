// The LED panel player of the Studio previews (frontend/packages/sign-player/src/engine.ts): RGB555 frames drawn as
// round LEDs on a dark stage, with the afterglow (a pixel turns on at once but only fades towards a darker value:
// after dt ms a level keeps persist^(dt / 25 ms)), gamma and brightness per 5-bit channel, and faint unlit LEDs.
// Frames advance on their own clock (25 ms, the sign's 40 fps). Small pitches draw square pixels from a texture.
#pragma once

#include "imgui.h"

#include <SDL3/SDL.h>

#include <cstdint>
#include <vector>

namespace sim {

struct LedLook {
    float gamma = 1, brightness = 1, persist = 0.3f;
    bool dots = true, unlit = true;
};

class LedPlayer {
public:
    explicit LedPlayer(SDL_Renderer* ren = nullptr) : ren_(ren) {}
    ~LedPlayer();
    LedPlayer(const LedPlayer&) = delete;
    LedPlayer& operator=(const LedPlayer&) = delete;
    // New frames (w * h RGB555 each). keep_position: stay at the same frame when the new set is as long or longer.
    void set(std::vector<std::vector<uint16_t>> frames, int w, int h, bool keep_position = false);
    void clear() { frames_.clear(); }
    bool empty() const { return frames_.empty(); }
    int w() const { return w_; }
    int h() const { return h_; }
    size_t size() const { return frames_.size(); }
    size_t index() const { return i_; }
    void seek(size_t i);
    const std::vector<uint16_t>* current() const { return frames_.empty() ? nullptr : &frames_[i_]; }
    bool playing = true;
    // Draws the panel max_w wide (and at most max_h high, 0 = no limit), advancing the clock when playing.
    void draw(const LedLook& look, float max_w, float max_h = 0);
    // The play/pause button, the seek bar and the time.
    void controls();

private:
    SDL_Renderer* ren_;
    SDL_Texture* tex_ = nullptr;
    int tex_w_ = 0, tex_h_ = 0;
    std::vector<std::vector<uint16_t>> frames_;
    int w_ = 0, h_ = 0;
    size_t i_ = 0;
    double acc_ = 0, last_ = -1;
    std::vector<float> vis_;                             // shown level per channel, 0..31, fading
    void advance(double now, float persist);
};

}  // namespace sim
