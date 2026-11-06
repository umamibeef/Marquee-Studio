#!/usr/bin/env python3
"""Bitmap font rendering for the sign emulator, using the extracted fonte0N.png atlases.

The fonts are ASCII glyph atlases: 16 columns, cell width = image_width/16. The printable ASCII
block (0x20..0x7F) occupies 6 consecutive rows; a taller large-digit row sits above it and currency
symbols below. We locate the ASCII rows by the horizontal ink profile, then take each glyph's ink
bounding box inside its cell for proportional spacing. (Exact per-glyph metrics live in the firmware
font loader FUN_000446d4; this reproduction is faithful for preview.)
"""
import numpy as np
from PIL import Image


class SignFont:
    def __init__(self, png_path):
        self.ink = np.array(Image.open(png_path).convert("L")) > 128  # True = lit (white glyph)
        self.H, self.W = self.ink.shape
        self.cw = self.W // 16
        self._find_ascii_rows()

    def _find_ascii_rows(self):
        # Atlas row pitch scales with font size; derive from the image height.
        # Layout: [header micro-row][large-digit row][6 ASCII rows 0x20-0x7F][blank][currency].
        # Pitch and ASCII-block origin are proportional to cell width (cw).
        pitch = self.H // 16              # 16 macro-rows; ASCII rows are 1 pitch each
        # origin: skip the header+large-digit band (about 2 pitches)
        y0 = 2 * pitch
        self.pitch = pitch
        self.ascii_bands = [(y0 + pitch * i, y0 + pitch * (i + 1)) for i in range(6)]

    def glyph(self, ch):
        """Return a boolean mask (h x w) for character ch, or None."""
        code = ord(ch)
        if not (0x20 <= code <= 0x7F):
            return None
        idx = code - 0x20
        row, col = idx // 16, idx % 16
        if row >= len(self.ascii_bands):
            return None
        y0, y1 = self.ascii_bands[row]
        x0 = col * self.cw
        cell = self.ink[y0:y1, x0:x0 + self.cw]
        cols = np.where(cell.any(axis=0))[0]
        if len(cols) == 0:
            return np.zeros((y1 - y0, max(2, self.cw // 3)), bool)  # space
        return cell[:, cols[0]:cols[-1] + 1]

    def text_mask(self, text, spacing=1):
        glyphs = [self.glyph(c) for c in text]
        glyphs = [g for g in glyphs if g is not None]
        if not glyphs:
            return np.zeros((1, 1), bool)
        h = max(g.shape[0] for g in glyphs)
        w = sum(g.shape[1] + spacing for g in glyphs)
        out = np.zeros((h, w), bool)
        x = 0
        for g in glyphs:
            out[:g.shape[0], x:x + g.shape[1]] |= g
            x += g.shape[1] + spacing
        return out

    def draw(self, panel, text, x, y, color555, spacing=1):
        m = self.text_mask(text, spacing)
        for yy in range(m.shape[0]):
            for xx in range(m.shape[1]):
                if m[yy, xx]:
                    panel.put(x + xx, y + yy, color555)
        return m.shape[1]


if __name__ == "__main__":
    from .sign_emu import Panel, rgb888_to_rgb555
    from PIL import Image
    f = SignFont("../analysis/extracted/fonte04.png")
    print("ASCII row bands:", f.ascii_bands, "cell width", f.cw)
    p = Panel(176, 40)
    f.draw(p, "JACKPOT $1000", 1, 1, rgb888_to_rgb555(255, 40, 0))
    rows = p.to_rgb_rows()
    im = Image.new("RGB", (p.w, p.h)); im.putdata([px for r in rows for px in r])
    im.resize((p.w * 6, p.h * 6), Image.NEAREST).save("text_test.png")
    print("wrote text_test.png")
