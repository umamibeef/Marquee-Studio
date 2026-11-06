#include "fonts.h"

#include "image.h"

#include <algorithm>
#include <map>
#include <mutex>
#include <regex>

namespace host {

namespace fs = std::filesystem;

FwFont::FwFont(const std::string& png_path, const std::string& n) : name(n) {
    Image img = load_image(png_path);
    w = img.w;
    h = img.h;
    ink.resize((size_t)w * h);
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++) {
            const uint8_t* p = img.at(x, y);
            ink[(size_t)y * w + x] = (p[0] >> 3) || (p[1] >> 3) || (p[2] >> 3);
        }
    measure_all();
}

FwFont::FwFont(std::vector<uint8_t> i, int iw, int ih, const std::string& n) : name(n), w(iw), h(ih), ink(std::move(i)) {
    measure_all();
}

void FwFont::measure_all() {
    cw = w >> 4;
    ch = h >> 4;
    for (int c = 0; c < 256; c++)
        measure(c);
    int lo = 255, hi = 0;                                // codes 0..9 all get the union of their extents
    for (int c = 0; c < 10; c++) {
        lo = std::min(lo, left[c]);
        hi = std::max(hi, (left[c] + width[c]) & 0xFF);
    }
    for (int c = 0; c < 10; c++) {
        left[c] = lo;
        width[c] = (hi - lo) & 0xFF;
    }
}

void FwFont::measure(int c) {
    prop[c] = 1;
    if (c == 0x20) {
        left[c] = 0;
        width[c] = std::max(3, (w >> 6) & 0xFF);
        prop[c] = 0;
        return;
    }
    if (c == 0xFF || (c >= 0x30 && c <= 0x39) || (c >= 0x10 && c <= 0x19)) {
        left[c] = 0;
        width[c] = cw & 0xFF;
        right[c] = 0;
        prop[c] = 0;
        return;
    }
    int x0 = (c & 15) * cw, y0 = (c >> 4) * ch;
    std::vector<bool> cols(cw, false);
    for (int x = 0; x < cw; x++)
        for (int y = 0; y < ch && !cols[x]; y++)
            cols[x] = at(x0 + x, y0 + y);
    int l = 0;
    while (l < cw && !cols[l])
        l++;
    int r = 0;
    if (l < cw)
        while (r < cw && !cols[cw - 1 - r])
            r++;
    if (l + r < cw) {
        left[c] = l;
        width[c] = cw - (l + r);
        right[c] = r;
    } else {
        left[c] = cw & 0xFF;
        width[c] = 0;
        right[c] = 0;
    }
}

int FwFont::text_width(const Bytes& data) const {
    int tw = 0, prev = 1;
    for (uint8_t c : data) {
        if (prop[c])
            tw += (prev == 0) + width[c] + 1;
        else
            tw += width[c];
        prev = prop[c];
    }
    return tw;
}

std::vector<std::string> sign_font_names(const Paths& p) {
    std::vector<std::string> out;
    static const std::regex re("fonte\\d\\d\\.png");
    std::error_code ec;
    for (auto& e : fs::directory_iterator(p.content / "fonts", ec)) {
        std::string n = e.path().filename().string();
        if (std::regex_match(n, re))
            out.push_back(n);
    }
    std::sort(out.begin(), out.end());
    return out;
}

static std::mutex cache_mu;
static std::map<std::string, std::pair<fs::file_time_type, std::shared_ptr<const FwFont>>> cache;

static std::shared_ptr<const FwFont> cached(const std::string& path, const std::string& name) {
    std::error_code ec;
    auto t = fs::last_write_time(path, ec);
    std::lock_guard<std::mutex> g(cache_mu);
    auto it = cache.find(path);
    if (it != cache.end() && it->second.first == t)
        return it->second.second;
    auto f = std::make_shared<const FwFont>(path, name);
    cache[path] = {t, f};
    return f;
}

Fonts sign_fonts(const Paths& p) {
    Fonts out;
    for (auto& n : sign_font_names(p))
        out.push_back(cached(p.font(n), n));
    return out;
}

std::shared_ptr<const FwFont> fw_font(const Paths& p, const std::string& name) {
    return cached(p.font(name), name);
}

std::optional<size_t> font_pick_largest(const Fonts& fonts, int box_h) {
    int best_w = 0, best_h = 0, fb_w = 0;
    uint32_t fb_h = 0xFFFFFFFF;
    std::optional<size_t> found, fb;
    for (size_t i = 0; i < fonts.size(); i++) {
        const FwFont& f = *fonts[i];
        if (f.cw < best_w || box_h < f.ch || f.ch < best_h) {
            if (fb_w < f.cw && (uint32_t)f.ch <= fb_h) {
                fb = i;
                fb_h = f.ch;
                fb_w = f.cw;
            }
        } else {
            found = i;
            best_h = f.ch;
            best_w = f.cw;
        }
    }
    return found ? found : fb;
}

std::optional<size_t> font_pick_for_text(const Fonts& fonts, const Bytes& text, int box_w, int box_h) {
    int best_w = 0, best_h = 0;
    int64_t fb_w = 0xFFFFFFFF, fb_h = 0xFFFFFFFF;
    std::optional<size_t> found, fb;
    for (size_t i = 0; i < fonts.size(); i++) {
        int w = fonts[i]->text_width(text), h = fonts[i]->ch;
        if (box_w < w || w < best_w || box_h < h || h < best_h) {
            if (w <= fb_w && h <= fb_h) {
                fb = i;
                fb_h = h;
                fb_w = w;
            }
        } else {
            found = i;
            best_h = h;
            best_w = w;
        }
    }
    return found ? found : fb;
}

std::string default_font(int H, const Fonts& fonts) {
    auto i = font_pick_largest(fonts, H);
    return i ? fonts[*i]->name : "";
}

}  // namespace host
