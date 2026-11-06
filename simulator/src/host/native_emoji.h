// Native emoji (native_emoji.py): emoji.bin, the content file for firmware with native emoji (firmware/ext/
// native_emoji.c), and the text encoding that goes with it (emoji as UTF-8, everything else Latin-1).
//   header ">IHHHHII": magic "EMJ2", cell size, count, colours, frames, map length, unpacked length (20 bytes)
//   zlib   the palette (colours x RGB555, entry 0 transparent), the map (one emoji per line, UTF-8, without
//          U+FE0F, in cell order), the cell table (count x ">HHH": first frame, frames, ms per frame) and the
//          pixels (frames of size x size palette indices). "EMJ1" files (no table, a frame a cell) still load.
// The host builds stills only (a frame each, 0 ms); imported, animated emoji are the studio's.
// The pictures come from the emoji pack when it is on this machine (paths.emoji_pack, not committed), else from
// the committed firmware/emoji/<size>/emoji.bin.
#pragma once
#include <optional>

#include "fonts.h"
#include "image.h"
#include "paths.h"
#include "text.h"

#include <map>
#include <mutex>

namespace host {

constexpr int NATIVE_EMOJI_SIZE = 16;
constexpr uint32_t EMOJI_MAGIC = 0x454D4A32;             // "EMJ2"
constexpr uint32_t EMOJI_MAGIC1 = 0x454D4A31;            // "EMJ1"

struct EmojiSheet {
    int size = 0;
    std::vector<uint16_t> palette;
    std::vector<std::u32string> emojis;                  // map keys, in cell order
    std::vector<Bytes> cells;                            // size * size palette indices each (frame 0)
};
// Throws std::runtime_error when data is not an emoji.bin.
EmojiSheet unpack_emoji_bin(const Bytes& data);

// One glyph of text bytes as the firmware reads them: a byte, or an emoji cell.
struct NativeGlyph {
    bool emoji;
    int value;                                           // the byte, or the cell number
};

class NativeEmoji {
public:
    explicit NativeEmoji(const Paths& p);
    static std::u32string key(const std::u32string& e); // without U+FE0F

    const std::vector<std::u32string>& default_set() const { return set_; }   // firmware/emoji/set.txt
    bool has_pack() const { return !pack_.empty(); }
    bool has_picture(const std::u32string& e, int size = NATIVE_EMOJI_SIZE);
    // The emoji of the list that have a picture, in order, without duplicates (keys).
    std::vector<std::u32string> available(int size = NATIVE_EMOJI_SIZE);
    std::vector<std::u32string> available(const std::vector<std::u32string>& emojis, int size);
    // An emoji drawn for a size x size cell (RGBA).
    Image made_for(const std::u32string& e, int size);
    // emoji.bin for the default set at this cell size. More than 255 colours are reduced to 255 for the whole set
    // together (Pillow's median cut, as native_emoji.py does).
    Bytes build(int size = NATIVE_EMOJI_SIZE, std::vector<std::u32string>* emojis = nullptr);
    // emoji.bin of these cells (RGBA, size x size each) for these emoji.
    static Bytes pack_sheet(const std::vector<std::u32string>& emojis, std::vector<Image> cells, int size);
    // The bytes to send: emoji of the set as UTF-8 (longest match first, U+FE0F dropped), the rest Latin-1
    // ('?' when there is no Latin-1 form).
    Bytes encode(const std::u32string& text);
    // How the firmware reads text bytes (the sheet built at this size).
    std::vector<NativeGlyph> parse(const Bytes& data, int size = NATIVE_EMOJI_SIZE);
    const EmojiSheet& sheet(int size = NATIVE_EMOJI_SIZE);
    // [(x, width)] of each glyph and the total width, as ext_text_width places them.
    static int layout(const FwFont& ft, const std::vector<NativeGlyph>& glyphs, int size,
                      std::vector<std::pair<int, int>>* xs = nullptr);
    static int top(int h, int size) { return (h - size) / 2; }

private:
    Paths paths_;
    std::map<std::u32string, int> pack_;                 // emoji key -> picture id
    std::vector<std::u32string> set_;
    std::map<int, std::map<std::u32string, Image>> committed_;
    std::map<int, EmojiSheet> sheets_;
    std::mutex mu_;
    const std::map<std::u32string, Image>& committed(int size);
};

// The patch's colour kernel: format-9 pixel p over RGB555 d; nullopt leaves the pixel alone.
std::optional<uint16_t> blend(uint16_t p, uint16_t d);

}  // namespace host
