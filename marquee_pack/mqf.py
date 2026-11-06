"""MQF1: our bitmap font format, read by our firmware (firmware/ext/fonts.c) and by the web Studio's emulation
(frontend/packages/sign-emu src/mqf.ts). Each glyph is its designer's advance box (advance x line height), 1 bit per
pixel, rows padded to whole bytes: the bearings and the spacing are exactly the font's own, with one byte of metrics
per glyph. Code ranges let a font hold any characters. The body is zlib-compressed (the firmware has zlib for the
emoji sheet). Measured on the converted fonts: 23% smaller than the same fonts as 16 x 16 PNG sheets. Big-endian.

    header  16 bytes: "MQF1", line height u8, ascent u8, flags u8 (1: body zlib-compressed; 2: proportional grid,
            see below), 0, ranges u16, glyphs u16, unpacked body length u32
    body    ranges   8 bytes each: first code u32, count u16, first glyph u16 (codes first .. first + count - 1)
            advances 1 byte per glyph
            cells    per glyph: line height rows of ceil(advance / 8) bytes, MSB first (the glyph's advance box,
                     top row = the line's top; ascent rows above the baseline)

Flag 2 (proportional grid): the glyphs are the full 16 x 16 cell grid (every stored advance is the cell width, each
glyph is its whole cell with the bearings left in place), and the loader keeps the stock measuring (font.ts /
firmware font_build layout 2: trimmed inked columns, left bearing, 1 px gaps, the space and digit rules) instead of
the flat advance = width, left 0 of a plain font. This holds the sign's own proportional fonts (fonte01..04) with
their exact spacing. Without the flag the glyphs are the designer's trimmed advance boxes and each code's width is
its advance, left 0, not proportional.
"""
import struct
import zlib

import numpy as np

MAGIC = b"MQF1"


def cell(adv, xo, yo, b, ascent, line_h):
    """The glyph's advance box as bool[line_h][adv]: BDF-style glyph (bitmap b, x offset xo, bottom offset yo)."""
    box = np.zeros((line_h, max(adv, 0)), bool)
    b = np.asarray(b, bool)
    top = ascent - (yo + b.shape[0])
    for y in range(b.shape[0]):
        for x in range(b.shape[1]):
            if b[y, x] and 0 <= top + y < line_h and 0 <= xo + x < adv:
                box[top + y, xo + x] = True
    return box


def write(glyphs, ascent, descent, codes, out, prop=False):
    """glyphs: {code: (advance, x offset, bottom y offset (BDF), bool bitmap rows)}; the codes to store. Ink outside
    a glyph's advance box (a negative bearing) is clipped; the converter reports it. prop=True sets the
    proportional-grid flag (the glyphs are full cells and the loader keeps the measured metrics)."""
    lh = ascent + descent
    cs = sorted(c for c in set(codes) if c in glyphs)
    ranges, run = [], None
    for k, c in enumerate(cs):
        if run and c == run[0] + run[1]:
            run[1] += 1
        else:
            run = [c, 1, k]
            ranges.append(run)
    advs, cells = bytearray(), bytearray()
    for c in cs:
        adv, xo, yo, b = glyphs[c]
        if not 0 <= adv <= 255:
            raise ValueError(f"glyph {c:#x}: advance {adv}")
        advs.append(adv)
        box = cell(adv, xo, yo, b, ascent, lh)
        nbytes = (adv + 7) // 8
        for row in box:                                   # MSB first: pixel 0 is the top bit of the row's first byte
            v = 0
            for k, bit in enumerate(row):
                v |= int(bit) << (nbytes * 8 - 1 - k)
            cells += v.to_bytes(nbytes, "big")
    body = b"".join(struct.pack(">IHH", *r) for r in ranges) + bytes(advs) + bytes(cells)
    flags = 1 | (2 if prop else 0)
    data = MAGIC + struct.pack(">BBBBHHI", lh, ascent, flags, 0, len(ranges), len(cs), len(body)) + zlib.compress(body, 9)
    with open(out, "wb") as f:
        f.write(data)
    return data


def read(data):
    """(line height, ascent, {code: (advance, bool[line height][advance])})"""
    if data[:4] != MAGIC:
        raise ValueError("not an MQF1 font")
    lh, asc, flags, _, nr, ng, blen = struct.unpack_from(">BBBBHHI", data, 4)
    body = zlib.decompress(data[16:]) if flags & 1 else data[16:16 + blen]
    ranges = [struct.unpack_from(">IHH", body, 8 * k) for k in range(nr)]
    advs = body[8 * nr:8 * nr + ng]
    off = 8 * nr + ng
    cells = []
    for g in range(ng):
        a, nbytes = advs[g], (advs[g] + 7) // 8
        rows = np.zeros((lh, a), bool)
        for y in range(lh):
            v = int.from_bytes(body[off:off + nbytes], "big")
            off += nbytes
            for x in range(a):
                rows[y, x] = (v >> (nbytes * 8 - 1 - x)) & 1
        cells.append((a, rows))
    out = {}
    for first, count, g0 in ranges:
        for k in range(count):
            out[first + k] = cells[g0 + k]
    return lh, asc, out
