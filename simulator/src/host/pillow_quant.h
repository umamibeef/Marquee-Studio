// Pillow's colour quantizer and palette mapping (Pillow 11.3.0 libImaging/Quant.c, QuantHash.c, QuantHeap.c,
// Palette.c, Convert.c topalette), ported so the studio's 8-bit FLCs and GIFs come out byte-identical to the
// Python studio's. Only the paths the studio's Python uses: Image.quantize(method=MEDIANCUT, kmeans=0) of an RGB
// image, and Image.quantize(palette=..., dither=NONE) (closest colour through the palette cache).
// Pillow is under the MIT-CMU licence (Copyright 1997-2011 Secret Labs AB, 1995-2011 Fredrik Lundh and
// contributors; quantizer by Toby J Sargeant).
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace host::pillow {

using Rgb = std::array<uint8_t, 3>;

struct Quantized {
    std::vector<Rgb> palette;                            // the palette's entries (Pillow's palette->size of them)
    std::vector<uint8_t> index;                          // per pixel (empty when not asked for)
};

// ImagingQuantize(im, colors, 0 /* median cut */, 0 /* no k-means */) of npixels RGB pixels, each `stride` bytes
// apart (3 for RGB888, 4 for RGBA; alpha is ignored as for a Pillow "RGB" image). Throws std::runtime_error where
// Pillow raises "quantization error".
Quantized quantize_mediancut(const uint8_t* px, size_t npixels, size_t stride, int colors, bool want_index = true);

// Convert.c topalette without dither: each pixel's palette index as Pillow's palette cache picks it (the closest
// entry, squared RGB distance, to the colour with its two low bits cleared; the lowest index on ties).
class PaletteMapper {
public:
    explicit PaletteMapper(const std::vector<Rgb>& palette);
    uint8_t index(int r, int g, int b);

private:
    std::vector<Rgb> pal_;
    std::vector<int16_t> cache_;                         // 64 x 64 x 64 slots, 0x100 = not computed
    void update(int r, int g, int b);
};

}  // namespace host::pillow
