#include "native_emoji.h"

#include "pillow_quant.h"

#include <zlib.h>

#include <algorithm>
#include <cstring>
#include <fstream>
#include <regex>
#include <sstream>
#include <stdexcept>

namespace host {

namespace fs = std::filesystem;

static std::string slurp(const fs::path& p) {
    std::ifstream f(p, std::ios::binary);
    std::stringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

EmojiSheet unpack_emoji_bin(const Bytes& data) {
    uint32_t magic = data.size() < 20 ? 0 : rd32(data.data());
    if (magic != EMOJI_MAGIC && magic != EMOJI_MAGIC1)
        throw std::runtime_error("not an emoji.bin");
    EmojiSheet s;
    s.size = (int)rd16(&data[4]);
    int count = (int)rd16(&data[6]), colours = (int)rd16(&data[8]);
    uint32_t map_len = rd32(&data[12]), raw_len = rd32(&data[16]);
    Bytes raw(raw_len);
    uLongf n = raw_len;
    if (uncompress(raw.data(), &n, data.data() + 20, (uLong)(data.size() - 20)) != Z_OK || n != raw_len)
        throw std::runtime_error("emoji.bin: bad zlib data");
    for (int i = 0; i < colours; i++)
        s.palette.push_back((uint16_t)rd16(&raw[i * 2]));
    std::string m(raw.begin() + colours * 2, raw.begin() + colours * 2 + map_len);
    std::u32string cur;
    for (char32_t c : from_utf8(m))
        if (c == '\n') {
            s.emojis.push_back(cur);
            cur.clear();
        } else
            cur += c;
    s.emojis.resize(std::min<size_t>(s.emojis.size(), count));
    size_t at = colours * 2 + map_len, cell = (size_t)s.size * s.size, px = at + (magic == EMOJI_MAGIC ? 6 * count : 0);
    for (int i = 0; i < count; i++) {
        size_t first = magic == EMOJI_MAGIC ? rd16(&raw[at + 6 * i]) : (size_t)i;
        s.cells.emplace_back(raw.begin() + px + first * cell, raw.begin() + px + (first + 1) * cell);
    }
    return s;
}

std::u32string NativeEmoji::key(const std::u32string& e) {
    std::u32string out;
    for (char32_t c : e)
        if (c != 0xFE0F)
            out += c;
    return out;
}

NativeEmoji::NativeEmoji(const Paths& p) : paths_(p) {
    // The pack's Unicode_Map.json, read leniently (the file as shipped misses a comma); a picture shared by many
    // codes (the generic flag), and ids with no picture, are left out.
    fs::path map = p.emoji_pack / "Unicode_Map.json";
    if (fs::exists(map)) {
        std::string text = slurp(map);
        static const std::regex re("\"unicode\":\\s*\"([^\"]+)\",\\s*\"pixel_emoji_id\":\\s*\"(\\d+)\"");
        std::vector<std::pair<std::u32string, int>> pairs;
        std::map<int, int> shared;
        for (std::sregex_iterator it(text.begin(), text.end(), re), end; it != end; ++it) {
            std::u32string e;
            std::stringstream ss((*it)[1].str());
            std::string cp;
            while (ss >> cp)
                e += (char32_t)std::stoul(cp.substr(2), nullptr, 16);
            int id = std::stoi((*it)[2].str());
            pairs.emplace_back(key(e), id);
            shared[id]++;
        }
        for (auto& [e, id] : pairs) {
            fs::path drawn = p.emoji_pack / "Emojis_Flat_16x16" / ("Emojis_Flat_16x16_" + std::to_string(id) + ".png");
            if (shared[id] <= 3 && !pack_.count(e) && fs::exists(drawn))
                pack_[e] = id;
        }
    }
    std::stringstream ss(slurp(p.emoji_dir / "set.txt"));
    std::string line;
    while (std::getline(ss, line)) {
        if (!line.empty() && line[0] == '#')
            continue;
        std::stringstream ls(line);
        std::string w;
        while (ls >> w)
            set_.push_back(from_utf8(w));
    }
}

const std::map<std::u32string, Image>& NativeEmoji::committed(int size) {
    auto it = committed_.find(size);
    if (it != committed_.end())
        return it->second;
    auto& cells = committed_[size];
    fs::path bin = paths_.emoji_dir / std::to_string(size) / "emoji.bin";
    if (fs::exists(bin)) {
        std::string d = slurp(bin);
        EmojiSheet s = unpack_emoji_bin(Bytes(d.begin(), d.end()));
        for (size_t n = 0; n < s.emojis.size() && n < s.cells.size(); n++) {
            Image img = Image::blank(s.size, s.size);
            for (int i = 0; i < s.size * s.size; i++) {
                uint8_t v = s.cells[n][i];
                if (!v)
                    continue;
                uint16_t c = s.palette[v];
                uint8_t* q = &img.px[(size_t)i * 4];
                q[0] = (uint8_t)((c >> 10 & 31) << 3);
                q[1] = (uint8_t)((c >> 5 & 31) << 3);
                q[2] = (uint8_t)((c & 31) << 3);
                q[3] = 255;
            }
            cells[s.emojis[n]] = std::move(img);
        }
    }
    return cells;
}

bool NativeEmoji::has_picture(const std::u32string& e, int size) {
    auto k = key(e);
    return has_pack() ? pack_.count(k) > 0 : committed(size).count(k) > 0;
}

std::vector<std::u32string> NativeEmoji::available(const std::vector<std::u32string>& emojis, int size) {
    std::vector<std::u32string> out;
    for (auto& e : emojis) {
        auto k = key(e);
        if (std::find(out.begin(), out.end(), k) == out.end() && has_picture(e, size))
            out.push_back(k);
    }
    return out;
}

std::vector<std::u32string> NativeEmoji::available(int size) { return available(set_, size); }

Image NativeEmoji::made_for(const std::u32string& e, int size) {
    if (!has_pack())
        return committed(size).at(key(e));
    // pixel art drawn at (or below) the size, at the largest whole scale that fits, centred
    std::vector<int> drawn;
    static const std::regex re("Emojis_Flat_(\\d+)x(\\d+)");
    for (auto& d : fs::directory_iterator(paths_.emoji_pack)) {
        std::smatch m;
        std::string n = d.path().filename().string();
        if (std::regex_match(n, m, re) && m[1] == m[2])
            drawn.push_back(std::stoi(m[1]));
    }
    std::sort(drawn.begin(), drawn.end());
    int n = drawn.empty() ? size : drawn[0];
    for (int d : drawn)
        if (d <= size)
            n = d;
    std::string dir = "Emojis_Flat_" + std::to_string(n) + "x" + std::to_string(n);
    Image art = load_image((paths_.emoji_pack / dir / (dir + "_" + std::to_string(pack_.at(key(e))) + ".png")).string());
    int k = std::max(1, size / art.w);
    Image big = Image::blank(art.w * k, art.h * k);
    for (int y = 0; y < big.h; y++)
        for (int x = 0; x < big.w; x++)
            memcpy(big.at(x, y), art.at(x / k, y / k), 4);
    int ox = (big.w - size) / 2, oy = (big.h - size) / 2;   // Python floor division
    if (big.w - size < 0 && (big.w - size) % 2)
        ox--;
    if (big.h - size < 0 && (big.h - size) % 2)
        oy--;
    int sx = 0, sy = 0, sw = big.w, sh = big.h;
    if (ox >= 0) {
        sx = ox;
        sy = std::max(oy, 0);
        sw = size;
        sh = std::min(size, big.h);
    }
    Image cell = Image::blank(size, size);
    int dx = std::max(0, -ox), dy = std::max(0, -oy);
    for (int y = 0; y < sh; y++)
        for (int x = 0; x < sw; x++) {
            int tx = dx + x, ty = dy + y, fx = sx + x, fy = sy + y;
            if (tx >= size || ty >= size || fx >= big.w || fy >= big.h)
                continue;
            const uint8_t* s = big.at(fx, fy);          // alpha_composite onto a transparent cell
            uint8_t* d = cell.at(tx, ty);
            if (s[3] == 255 || d[3] == 0)
                memcpy(d, s, 4);
            else if (s[3]) {
                float sa = s[3] / 255.f, da = d[3] / 255.f, oa = sa + da * (1 - sa);
                for (int c = 0; c < 3; c++)
                    d[c] = (uint8_t)((s[c] * sa + d[c] * da * (1 - sa)) / oa + 0.5f);
                d[3] = (uint8_t)(oa * 255 + 0.5f);
            }
        }
    return cell;
}

static uint16_t px555(const uint8_t* p) {
    return p[3] < 128 ? 0 : (uint16_t)((p[0] >> 3) << 10 | (p[1] >> 3) << 5 | (p[2] >> 3));
}

Bytes NativeEmoji::build(int size, std::vector<std::u32string>* list) {
    auto emojis = available(size);
    std::vector<Image> cells;
    for (auto& e : emojis)
        cells.push_back(made_for(e, size));
    if (list)
        *list = emojis;
    return pack_sheet(emojis, std::move(cells), size);
}

Bytes NativeEmoji::pack_sheet(const std::vector<std::u32string>& emojis, std::vector<Image> cells, int size) {
    auto count_colours = [&](std::map<uint16_t, std::pair<int, int>>& count) {   // colour -> (count, first seen)
        count.clear();
        int order = 0;
        for (auto& c : cells)
            for (int i = 0; i < size * size; i++)
                if (uint16_t v = px555(&c.px[(size_t)i * 4])) {
                    auto it = count.find(v);
                    if (it == count.end())
                        count[v] = {1, order++};
                    else
                        it->second.first++;
                }
    };
    std::map<uint16_t, std::pair<int, int>> count;
    count_colours(count);
    if (count.size() > 255 && !cells.empty()) {
        // reduce: the whole set quantized together, as one strip of cells (Pillow's median cut to 255 colours of the
        // RGB channels; the alpha channel kept)
        std::vector<uint8_t> strip;
        strip.reserve(cells.size() * size * size * 4);
        for (auto& c : cells)
            strip.insert(strip.end(), c.px.begin(), c.px.end());
        pillow::Quantized q = pillow::quantize_mediancut(strip.data(), strip.size() / 4, 4, 255);
        size_t k = 0;
        for (auto& c : cells)
            for (size_t i = 0; i < (size_t)size * size; i++, k++) {
                const pillow::Rgb& rgb = q.palette[q.index[k]];
                c.px[i * 4] = rgb[0];
                c.px[i * 4 + 1] = rgb[1];
                c.px[i * 4 + 2] = rgb[2];
            }
        count_colours(count);
    }
    std::vector<std::pair<uint16_t, std::pair<int, int>>> sorted(count.begin(), count.end());
    std::sort(sorted.begin(), sorted.end(), [](auto& a, auto& b) {   // Counter.most_common: ties by first seen
        return a.second.first != b.second.first ? a.second.first > b.second.first : a.second.second < b.second.second;
    });
    std::vector<uint16_t> palette{0};
    std::map<uint16_t, uint8_t> index;
    for (auto& [c, _] : sorted) {
        index[c] = (uint8_t)palette.size();
        palette.push_back(c);
    }
    std::string m;
    for (auto& e : emojis)
        m += to_utf8(e) + "\n";
    Bytes raw;
    for (uint16_t c : palette)
        be16(raw, c);
    raw.insert(raw.end(), m.begin(), m.end());
    for (size_t i = 0; i < cells.size(); i++) {               // the cell table: stills, a frame each
        be16(raw, (uint32_t)i);
        be16(raw, 1);
        be16(raw, 0);
    }
    for (auto& c : cells)
        for (int i = 0; i < size * size; i++) {
            uint16_t v = px555(&c.px[(size_t)i * 4]);
            raw.push_back(v ? index[v] : 0);
        }
    uLongf zn = compressBound((uLong)raw.size());
    Bytes z(zn);
    if (compress2(z.data(), &zn, raw.data(), (uLong)raw.size(), 9) != Z_OK)
        throw std::runtime_error("zlib failed");
    z.resize(zn);
    Bytes out;
    be32(out, EMOJI_MAGIC);
    be16(out, size);
    be16(out, (uint32_t)emojis.size());
    be16(out, (uint32_t)palette.size());
    be16(out, (uint32_t)cells.size());
    be32(out, (uint32_t)m.size());
    be32(out, (uint32_t)raw.size());
    out.insert(out.end(), z.begin(), z.end());
    return out;
}

Bytes NativeEmoji::encode(const std::u32string& text) {
    auto keys = available();
    std::stable_sort(keys.begin(), keys.end(), [](auto& a, auto& b) { return a.size() > b.size(); });
    Bytes out;
    size_t i = 0;
    while (i < text.size()) {
        if (text[i] == 0xFE0F) {
            i++;
            continue;
        }
        std::u32string rest = key(text.substr(i, 12));
        const std::u32string* hit = nullptr;
        for (auto& k : keys)
            if (rest.compare(0, k.size(), k) == 0) {
                hit = &k;
                break;
            }
        if (hit) {
            std::string u = to_utf8(*hit);
            out.insert(out.end(), u.begin(), u.end());
            size_t n = 0, j = i;                         // past the emoji's code points in the text
            while (n < hit->size()) {
                if (text[j] != 0xFE0F)
                    n++;
                j++;
            }
            i = j;
            continue;
        }
        out.push_back(text[i] > 0xFF ? '?' : (uint8_t)text[i]);
        i++;
    }
    return out;
}

const EmojiSheet& NativeEmoji::sheet(int size) {
    auto it = sheets_.find(size);
    if (it != sheets_.end())
        return it->second;
    return sheets_[size] = unpack_emoji_bin(build(size));
}

std::vector<NativeGlyph> NativeEmoji::parse(const Bytes& data, int size) {
    const EmojiSheet& s = sheet(size);
    std::vector<std::string> keys;
    for (auto& e : s.emojis)
        keys.push_back(to_utf8(e));
    std::vector<NativeGlyph> out;
    size_t i = 0;
    while (i < data.size()) {
        size_t best = 0;
        int cell = 0;
        if (data[i] >= 0xC2) {
            for (size_t n = 0; n < keys.size(); n++) {
                auto& k = keys[n];
                if (k.size() > best && i + k.size() <= data.size() && std::equal(k.begin(), k.end(), data.begin() + i,
                                                                                  [](char a, uint8_t b) { return (uint8_t)a == b; })) {
                    best = k.size();
                    cell = (int)n;
                }
            }
            if (best && i + best + 3 <= data.size() && data[i + best] == 0xEF && data[i + best + 1] == 0xB8 &&
                data[i + best + 2] == 0x8F)
                best += 3;
        }
        if (best) {
            out.push_back({true, cell});
            i += best;
        } else
            out.push_back({false, data[i++]});
    }
    return out;
}

int NativeEmoji::layout(const FwFont& ft, const std::vector<NativeGlyph>& glyphs, int size,
                        std::vector<std::pair<int, int>>* xs) {
    int x = 0, prev = 1;
    for (auto& g : glyphs) {
        int w = g.emoji ? size : ft.width[g.value], p = g.emoji ? 1 : ft.prop[g.value];
        if (prev == 0 && p)
            x++;
        if (xs)
            xs->emplace_back(x, w);
        x += w + (p ? 1 : 0);
        prev = p;
    }
    return x;
}

std::optional<uint16_t> blend(uint16_t p, uint16_t d) {
    if (!(p & 0x8000))
        return p;
    int a3 = (p >> 12) & 7;
    if (a3 == 0)
        return std::nullopt;
    int a = a3 * 4 + 3;
    int sr = ((p >> 8) & 15) << 1 | 1, sg = ((p >> 4) & 15) << 1 | 1, sb = (p & 15) << 1 | 1;
    int dr = (d >> 10) & 31, dg = (d >> 5) & 31, db = d & 31;
    return (uint16_t)(((a * sr + (31 - a) * dr) / 31) << 10 | ((a * sg + (31 - a) * dg) / 31) << 5 |
                      ((a * sb + (31 - a) * db) / 31));
}

}  // namespace host
