// umami-marquee-sim-test native-emoji (the port of sim/test_native_emoji.py): native emoji, tested inside the running
// firmware (firmware/ext/native_emoji.c, docs/NATIVE-EMOJI.md).
//
// A complete boot-flash image boots (the original boot loader, the extension loader, the app) and runs until the
// fonts are loaded; then the firmware's text routines are called in the live system:
//   1. text without emoji: text_width takes the original path (equal to a direct call of text_width__orig);
//   2. the first emoji loads emoji.bin from the boot flash (the state block then holds its palette, map and pixels,
//      equal to the file);
//   3. "A<emoji>B" with every 16x16-grid font and each of the three drawers: the letters equal the original routine
//      drawing "A" and "B" alone at the same places, the emoji pixels equal the cell drawn 1:1, the widths equal
//      text_width and the width rule; and the studio's preview (studio/native_emoji.h) gives the same.
#include "simtest.h"

#include "fonts.h"
#include "native_emoji.h"

#include <cstdio>
#include <cstring>
#include <filesystem>
#include <optional>
#include <stdexcept>

namespace sim {

namespace {

constexpr uint32_t STACK_SIZE = 0x10000;
constexpr int H = 16;

std::vector<uint8_t> be(std::initializer_list<std::pair<uint32_t, int>> fields) {   // (value, bytes) big-endian
    std::vector<uint8_t> v;
    for (auto [x, n] : fields)
        for (int i = n - 1; i >= 0; i--)
            v.push_back((uint8_t)(x >> (8 * i)));
    return v;
}

// The defined symbols of a big-endian ELF32 file (the extensions' ext.elf), as llvm-nm lists them.
std::map<std::string, uint32_t> elf_symbols(const std::string& path) {
    auto f = read_file(path);
    auto u16 = [&](size_t o) { return (uint32_t)(f.at(o) << 8 | f.at(o + 1)); };
    auto u32 = [&](size_t o) { return u16(o) << 16 | u16(o + 2); };
    if (f.size() < 52 || memcmp(f.data(), "\x7f" "ELF", 4) || f[4] != 1 || f[5] != 2)
        throw std::runtime_error(path + ": not a big-endian ELF32 file");
    uint32_t shoff = u32(0x20), shentsize = u16(0x2E), shnum = u16(0x30);
    std::map<std::string, uint32_t> out;
    for (uint32_t i = 0; i < shnum; i++) {
        size_t sh = shoff + (size_t)i * shentsize;
        if (u32(sh + 4) != 2)                              // SHT_SYMTAB
            continue;
        uint32_t off = u32(sh + 0x10), size = u32(sh + 0x14), link = u32(sh + 0x18), ent = u32(sh + 0x24);
        size_t str = u32(shoff + (size_t)link * shentsize + 0x10);
        for (uint32_t s = off + ent; s + ent <= off + size; s += ent) {   // entry 0 is the null symbol
            uint32_t name = u32(s), value = u32(s + 4);
            uint8_t info = f.at(s + 12);
            uint32_t shndx = u16(s + 14);
            if (!name || !shndx || (info & 0xF) == 4)      // undefined, or a file symbol
                continue;
            out[std::string((const char*)&f.at(str + name))] = value;
        }
    }
    return out;
}

// Calls into the paused firmware. Memory for the test (buffers and a 64 KB stack) comes from the firmware's own
// malloc, since the heap spans the rest of RAM.
struct Calls {
    Board& b;
    uint32_t malloc_at, dispatch_level, stack = 0;

    explicit Calls(Board& board) : b(board) {
        malloc_at = repo().symbols.at("malloc");
        dispatch_level = repo().symbols.at("thread_dispatch_disable_level");
        if (!malloc_at || !dispatch_level)
            throw std::runtime_error("symbols.tsv has no malloc / thread_dispatch_disable_level");
        uint32_t sp = b.reg(UC_PPC_REG_1);
        stack = call(malloc_at, {STACK_SIZE}, (sp - 0x800) & ~15u) + STACK_SIZE - 64;
    }

    uint32_t call(uint32_t addr, const std::vector<uint32_t>& args, std::optional<uint32_t> sp = std::nullopt) {
        // the call runs in whatever context the system paused in: keep RTEMS from switching tasks under it (the
        // firmware itself draws text from the display task)
        uint32_t level = b.r32(dispatch_level);
        put(dispatch_level, level + 1);
        uint32_t r = b.call(addr, args, sp ? *sp : stack);
        put(dispatch_level, level);
        return r;
    }

    void put(uint32_t a, uint32_t v) {
        auto w = be({{v, 4}});
        b.write(a, w.data(), 4);
    }

    uint32_t alloc(const std::vector<uint8_t>& data) {
        uint32_t a = call(malloc_at, {(uint32_t)data.size() + 16});
        if (!a)
            throw std::runtime_error("malloc failed");
        b.write(a, data.data(), data.size());
        return a;
    }

    std::pair<uint32_t, uint32_t> frame(int w, int h) {    // (struct frame *, its pixels)
        uint32_t pix = alloc(std::vector<uint8_t>((size_t)w * h * 2));
        return {alloc(be({{1, 4}, {(uint32_t)w, 4}, {(uint32_t)h, 4}, {pix, 4}, {0, 4}})), pix};
    }

    std::vector<uint16_t> pixels(uint32_t pix, int w, int h) {
        std::vector<uint8_t> raw((size_t)w * h * 2);
        b.read(pix, raw.data(), raw.size());
        std::vector<uint16_t> out(raw.size() / 2);
        for (size_t i = 0; i < out.size(); i++)
            out[i] = (uint16_t)(raw[2 * i] << 8 | raw[2 * i + 1]);
        return out;
    }

    uint32_t sbuf(const std::vector<uint8_t>& data) {       // a struct sbuf over a NUL-terminated copy
        std::vector<uint8_t> z = data;
        z.push_back(0);
        uint32_t d = alloc(z);
        return alloc(be({{d, 4}, {(uint32_t)data.size() + 1, 2}, {(uint32_t)data.size(), 2}}));
    }
};

std::vector<uint8_t> bytes_of(const std::string& s) { return std::vector<uint8_t>(s.begin(), s.end()); }

}  // namespace

int cmd_native_emoji(const std::vector<std::string>& args) {
    std::string flash = args.size() > 0 ? args[0] : repo().path("bootloader/build/bootflash-fw.bin");
    auto app = read_file(repo().path("analysis/build/fw/ELITE_RGB-ext"));
    std::vector<uint32_t> bad;
    auto board = booted(read_file(flash), app, &bad);
    board->run_app(400000000);                             // all fonts loaded by then
    if (!bad.empty())
        throw std::runtime_error("the boot loader did not load the extended app");
    const Symbols& S = repo().symbols;
    auto X = elf_symbols(repo().path("analysis/build/fw/ext/ext.elf"));
    auto sym = [&](const std::string& n) {
        auto a = S.find(n);
        if (!a)
            throw std::runtime_error("symbols.tsv has no " + n);
        return *a;
    };
    auto ext = [&](const std::string& n) {
        auto it = X.find(n);
        if (it == X.end())
            throw std::runtime_error("ext.elf has no " + n);
        return it->second;
    };
    Calls c(*board);
    auto r32 = [&](uint32_t a) { return board->r32(a); };
    bool ok = true;
    auto check = [&](bool cond, const std::string& what) {
        printf("%s%s\n", cond ? "ok   " : "FAIL ", what.c_str());
        ok &= cond;
    };

    uint8_t nfonts = 0;
    board->read(sym("font_count"), &nfonts, 1);
    uint32_t font = c.alloc(std::vector<uint8_t>(0x718));
    int pick = -1;
    for (int n = 0; n < nfonts && pick < 0; n++) {
        c.call(sym("font_get"), {font, (uint32_t)n});
        if (r32(font) == 2)                                // a 16x16 grid
            pick = n;
    }
    if (pick < 0)
        throw std::runtime_error("no 16x16-grid font loaded");
    uint32_t fh = r32(font + 12) >> 4;                     // cell height = image height / 16
    printf("%d fonts loaded; using font %d (16x16 grid, cells %ux%u)\n", nfonts, pick, r32(font + 8) >> 4, fh);

    // 1. no emoji: the original path
    uint32_t st = ext("emoji_state");
    uint32_t plain = c.sbuf(bytes_of("JACKPOT 1,234"));
    uint32_t w_new = c.call(sym("text_width"), {font, plain});
    uint32_t w_orig = c.call(ext("text_width__orig"), {font, plain});
    check(w_new == w_orig && r32(st) == 0, "ASCII text: text_width equals the original, and nothing is loaded");
    uint32_t latin = c.sbuf({'c', 'a', 'f', 0xE9, ' ', 0xA3, ' ', 0xB0});
    w_new = c.call(sym("text_width"), {font, latin});
    w_orig = c.call(ext("text_width__orig"), {font, latin});
    check(w_new == w_orig, "Latin-1 text (high bytes, no emoji): text_width equals the original");

    // 2. the first emoji loads the sheet
    std::u32string cherry = host::NativeEmoji::key(U"\U0001F352");
    std::vector<uint8_t> text = bytes_of("A" + host::to_utf8(cherry) + "B");
    uint32_t t = c.sbuf(text);
    c.call(sym("text_width"), {font, t});                  // the first emoji: loads the sheet
    uint32_t es = r32(st + 4), count = r32(st + 8), pal = r32(st + 12), pix = r32(st + 16), mp = r32(st + 20),
             ml = r32(st + 24);
    host::EmojiSheet file = host::unpack_emoji_bin(read_file(repo().path("bootloader/build/emoji/emoji.bin")));
    check(r32(st) == 1 && es == (uint32_t)file.size && count == file.emojis.size() && pal && pix && mp,
          "emoji.bin unpacked from the boot flash: " + std::to_string(count) + " cells of " + std::to_string(es) + "x" +
              std::to_string(es) + " (expected " + std::to_string(file.emojis.size()) + " of " +
              std::to_string(file.size) + "x" + std::to_string(file.size) + ")");
    std::string map_bytes;
    for (auto& e : file.emojis)
        map_bytes += host::to_utf8(e) + "\n";
    std::vector<uint8_t> got_map(ml), got_pix((size_t)count * es * es), want_pix, got_pal(file.palette.size() * 2);
    board->read(mp, got_map.data(), ml);
    board->read(pix, got_pix.data(), got_pix.size());
    board->read(pal, got_pal.data(), got_pal.size());
    for (auto& cell : file.cells)
        want_pix.insert(want_pix.end(), cell.begin(), cell.end());
    bool pal_same = true;
    for (size_t i = 0; i < file.palette.size(); i++)
        pal_same &= (uint16_t)(got_pal[2 * i] << 8 | got_pal[2 * i + 1]) == file.palette[i];
    check(got_map == bytes_of(map_bytes) && got_pix == want_pix && pal_same,
          "the unpacked map (" + std::to_string(ml) + " bytes), pixels and palette equal the file's");

    // 3. "A<emoji>B" with every loaded 16x16-grid font and each of the three drawers
    int cell = (int)(std::find(file.emojis.begin(), file.emojis.end(), cherry) - file.emojis.begin());
    if (cell == (int)file.emojis.size())
        throw std::runtime_error("the cherry is not in emoji.bin");
    const uint32_t EFF = 0x7FE00000;                       // effect word: yellow on black
    uint32_t COL = c.alloc(be({{0x7FE0, 2}, {0x0000, 2}}));
    host::Paths paths(repo().root);
    host::NativeEmoji studio_emoji(paths);
    std::vector<std::vector<uint16_t>> shots;
    for (int n_font = 0; n_font < nfonts; n_font++) {
        c.call(sym("font_get"), {font, (uint32_t)n_font});
        if (r32(font) != 2)
            continue;
        int fhh = (int)(r32(font + 12) >> 4);
        int fw = 160, fh_box = std::max(H, fhh);
        uint8_t widths[256], props[256];
        board->read(font + 24 + 256, widths, 256);         // font->width[] (after layout, image)
        board->read(font + 24 + 768, props, 256);          // font->proportional[]
        int expect = 0, prev = 1;
        for (auto [wd, pr] : {std::pair<int, int>{widths['A'], props['A']}, {(int)es, 1}, {widths['B'], props['B']}}) {
            expect += pr ? (prev == 0) + wd + 1 : wd;
            prev = pr;
        }
        int w_font = (int)c.call(sym("text_width"), {font, t});
        int xa = 2;
        // one separator column between "A" and the emoji either way (A's background column if A is proportional,
        // else the black column before the emoji, a proportional glyph); then the emoji's own
        int xe = xa + widths['A'] + 1, xb = xe + (int)es + 1;
        int top = (fhh - (int)es) / 2;                     // drawn 1:1, centred on the line (C division)
        std::vector<std::optional<uint16_t>> model;        // empty: transparent or outside, not checked
        for (int y = 0; y < fh_box; y++)
            for (int x = xe; x < xe + (int)es; x++) {
                int dy = y - top, dx = x - xe;
                int v = dy >= 0 && dy < (int)es ? file.cells[cell][dy * es + dx] : 0;
                model.push_back(v ? std::optional<uint16_t>(file.palette[v]) : std::nullopt);
            }
        auto cols = [&](const std::vector<uint16_t>& img, int x0, int x1) {
            std::vector<uint16_t> out;
            for (int y = 0; y < fh_box; y++)
                for (int x = x0; x < std::min(x1, fw); x++)
                    out.push_back(img[(size_t)y * fw + x]);
            return out;
        };
        for (std::string variant : {"plain", "colours", "clip"}) {
            auto draw = [&](bool orig, uint32_t txt, int x) {
                auto fn = [&](const std::string& name) { return orig ? ext(name + "__orig") : sym(name); };
                auto [d, p] = c.frame(fw, fh_box);
                uint32_t at = c.alloc(be({{(uint32_t)x, 4}, {0, 4}})), wo = c.alloc(std::vector<uint8_t>(4));
                if (variant == "plain")
                    c.call(fn("text_draw_proportional_plain"), {d, at, font, txt, wo});
                else if (variant == "colours")
                    c.call(fn("text_draw_proportional"), {d, at, font, txt, COL, wo});
                else {
                    uint32_t origin = c.alloc(be({{0, 4}, {0, 4}}));
                    std::vector<uint8_t> e8;
                    for (int i = 0; i < 8; i++) {
                        auto e = be({{EFF, 4}});
                        e8.insert(e8.end(), e.begin(), e.end());
                    }
                    uint32_t effects = c.alloc(e8);
                    c.call(fn("text_draw_proportional_clip_maybe"), {d, at, origin, origin, 0, font, txt, effects, 8, 0, wo});
                }
                uint8_t wb[2];
                board->read(wo, wb, 2);
                return std::make_pair(c.pixels(p, fw, fh_box), (int)(int16_t)(wb[0] << 8 | wb[1]));
            };
            auto [got, width_drawn] = draw(false, t, xa);
            uint32_t sa = c.sbuf(bytes_of("A"));
            auto a_ref = draw(true, sa, xa).first;
            uint32_t sb = c.sbuf(bytes_of("B"));
            auto b_ref = draw(true, sb, xb).first;
            auto em = cols(got, xe, xe + (int)es);
            bool model_ok = true, any = false;
            for (size_t i = 0; i < std::min(model.size(), em.size()); i++)
                model_ok &= !model[i] || *model[i] == em[i];
            for (uint16_t v : em)
                any |= v != 0;
            bool good = width_drawn == w_font && w_font == expect && cols(got, 0, xe) == cols(a_ref, 0, xe) &&
                        cols(got, xb, fw) == cols(b_ref, xb, fw) && model_ok && any;
            char msg[256];
            snprintf(msg, sizeof msg, "font %d (%ux%d), %s: width %d (text_width %d, rule %d); letters as the original; "
                     "emoji as the model", n_font, r32(font + 8) >> 4, fhh, variant.c_str(), width_drawn, w_font, expect);
            check(good, msg);
            if (variant != "plain")
                continue;
            shots.push_back(got);
            char name[32];
            // font n is fonte<n+1>.png for the stock fonts our image keeps (firmware/stock_fonts.txt: fonte01..04):
            // fonts_load takes them in order, with no built-in menu font before them (nomenu.c); the MQF fonts after
            // them are checked by the app tests
            snprintf(name, sizeof name, "fonte%02d.png", n_font + 1);
            static const int kept_stock = [] {
                std::ifstream k(repo().path("firmware/stock_fonts.txt"));
                int n = 0;
                for (std::string l; std::getline(k, l);)
                    n += l.rfind("fonte", 0) == 0;
                return n;
            }();
            if (n_font < kept_stock && std::filesystem::exists(paths.font(name))) {   // the studio's preview of it
                auto ft = host::fw_font(paths, name);
                auto glyphs = studio_emoji.parse(text);
                int studio_w = host::NativeEmoji::layout(*ft, glyphs, host::NATIVE_EMOJI_SIZE);
                const host::EmojiSheet& sh = studio_emoji.sheet(host::NATIVE_EMOJI_SIZE);
                int size = sh.size, t0 = host::NativeEmoji::top(ft->ch, size);
                std::vector<std::optional<uint16_t>> studio_em((size_t)es * fh_box);
                for (int k = 0; k < size * size; k++) {
                    int v = sh.cells.at(cell)[k];
                    if (!v)
                        continue;
                    int dx = k % size, dy = t0 + k / size;
                    if (dy >= 0 && dy < fh_box) {
                        auto bl = host::blend(sh.palette[v], 0);
                        studio_em[(size_t)dy * es + dx] = bl ? *bl : 0;
                    }
                }
                bool same_px = true;
                for (size_t i = 0; i < std::min(studio_em.size(), em.size()); i++)
                    same_px &= !studio_em[i] || *studio_em[i] == em[i];
                snprintf(msg, sizeof msg, "font %d: the studio preview (studio/native_emoji.h) matches the firmware: "
                         "width %d, emoji pixels", n_font, studio_w);
                check(ft->ch == fhh && studio_w == width_drawn && same_px, msg);
            }
        }
    }
    for (size_t i = 0; i < shots.size(); i++) {
        Frame f;
        for (uint16_t v : shots[i])
            f.push_back((uint8_t)(v >> 8)), f.push_back((uint8_t)v);
        save_frames({f}, repo().path("build/sim-test/native-emoji-" + std::to_string(i)), 160,
                    (int)(shots[i].size() / 160));
    }
    printf("RESULT: %s\n", ok ? "PASS" : "FAIL");
    return ok ? 0 : 1;
}

}  // namespace sim
