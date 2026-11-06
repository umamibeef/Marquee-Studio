#include "image.h"

#include <cstring>
#include <fstream>
#include <iterator>
#include <stdexcept>

#define STB_IMAGE_IMPLEMENTATION
#define STBI_NO_HDR
#define STBI_NO_LINEAR
#include "stb_image.h"
#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "stb_image_write.h"

namespace host {

Image load_image(const std::vector<uint8_t>& data) {
    int w, h, n;
    uint8_t* p = stbi_load_from_memory(data.data(), (int)data.size(), &w, &h, &n, 4);
    if (!p)
        throw std::runtime_error(std::string("not an image: ") + stbi_failure_reason());
    Image img;
    img.w = w;
    img.h = h;
    img.px.assign(p, p + (size_t)w * h * 4);
    stbi_image_free(p);
    return img;
}

Image load_image(const std::string& path) {
    std::ifstream f(path, std::ios::binary);
    if (!f)
        throw std::runtime_error("cannot read " + path);
    return load_image(std::vector<uint8_t>(std::istreambuf_iterator<char>(f), {}));
}

std::vector<Image> load_gif(const std::vector<uint8_t>& data, std::vector<int>* delays_ms) {
    int* delays = nullptr;
    int w, h, frames, n;
    uint8_t* p = stbi_load_gif_from_memory(data.data(), (int)data.size(), &delays, &w, &h, &frames, &n, 4);
    if (!p)
        throw std::runtime_error(std::string("not a GIF: ") + stbi_failure_reason());
    std::vector<Image> out;
    for (int i = 0; i < frames; i++) {
        Image img;
        img.w = w;
        img.h = h;
        const uint8_t* f = p + (size_t)i * w * h * 4;
        img.px.assign(f, f + (size_t)w * h * 4);
        out.push_back(std::move(img));
        if (delays_ms)
            delays_ms->push_back(delays ? delays[i] : 0);
    }
    stbi_image_free(p);
    free(delays);
    return out;
}

static void append(void* ctx, void* data, int size) {
    auto* v = static_cast<std::vector<uint8_t>*>(ctx);
    v->insert(v->end(), (uint8_t*)data, (uint8_t*)data + size);
}

std::vector<uint8_t> png_bytes(const Image& img) {
    std::vector<uint8_t> out;
    if (!stbi_write_png_to_func(append, &out, img.w, img.h, 4, img.px.data(), img.w * 4))
        throw std::runtime_error("PNG encoding failed");
    return out;
}

void save_png(const Image& img, const std::string& path) {
    auto b = png_bytes(img);
    std::ofstream f(path, std::ios::binary);
    if (!f.write((const char*)b.data(), (std::streamsize)b.size()))
        throw std::runtime_error("cannot write " + path);
}

}  // namespace host
