// Images for the studio: RGBA8 pictures read and written with stb_image / stb_image_write.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace host {

struct Image {
    int w = 0, h = 0;
    std::vector<uint8_t> px;                             // RGBA, row by row
    uint8_t* at(int x, int y) { return &px[((size_t)y * w + x) * 4]; }
    const uint8_t* at(int x, int y) const { return &px[((size_t)y * w + x) * 4]; }
    bool empty() const { return w == 0 || h == 0; }
    static Image blank(int w, int h) {
        Image i;
        i.w = w;
        i.h = h;
        i.px.assign((size_t)w * h * 4, 0);
        return i;
    }
};

// Throws std::runtime_error when the file cannot be read or decoded.
Image load_image(const std::string& path);
Image load_image(const std::vector<uint8_t>& data);
// Every frame of an animated GIF, composed to full frames, with each frame's delay in milliseconds.
std::vector<Image> load_gif(const std::vector<uint8_t>& data, std::vector<int>* delays_ms);
void save_png(const Image& img, const std::string& path);
std::vector<uint8_t> png_bytes(const Image& img);

// RGB555 (bit 14..10 red) <-> RGB888, as the sign's tools convert (sign_emu.rgb555_to_rgb888 / rgb888_to_rgb555).
inline void rgb555_to_rgb888(uint16_t v, uint8_t* rgb) {
    int r = v >> 10 & 31, g = v >> 5 & 31, b = v & 31;
    rgb[0] = (uint8_t)(r << 3 | r >> 2);
    rgb[1] = (uint8_t)(g << 3 | g >> 2);
    rgb[2] = (uint8_t)(b << 3 | b >> 2);
}
inline uint16_t rgb888_to_rgb555(int r, int g, int b) {
    return (uint16_t)(((r >> 3) & 31) << 10 | ((g >> 3) & 31) << 5 | ((b >> 3) & 31));
}

}  // namespace host
