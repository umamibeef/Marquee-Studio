#!/usr/bin/env python3
"""Check font_import.py: vendored pixel fonts -> sign fonts (fonteNN.png).

1. Every vendored font imports, into a 16 x 16 grid with codes 0x20..0x7E (and Latin-1 where the font has it).
2. BDF and .hex fonts are copied bit for bit: each glyph's inked pixels in its cell equal the source bitmap's.
   TTF/OTF fonts are exact on their design grid: each glyph equals its outline filled (nonzero rule) at the
   pixel centres of the design grid, by a rasteriser independent of FreeType.
3. The firmware's own font_build (0x448D4, under Unicorn) measures every imported font exactly as the studio's
   sign_emu.FwFont does (left / width / proportional for all 256 codes). FwFont's drawing is checked against
   the firmware in test_markup.py, so the studio previews these fonts as the sign will draw them.
4. Digits are centred in their cells; the sidecar carries the licence and credit line.

Usage: ../venv/bin/python test_font_import.py     (needs analysis/extracted/ELITE_RGB, unicorn, fonttools)
"""
import os, sys, tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import numpy as np  # noqa: E402
from marquee_studio import font_import as fi  # noqa: E402
from marquee_studio import sign_emu as emu  # noqa: E402
from test_markup import FwSign  # noqa: E402

ok = True


def check(name, cond, got=""):
    global ok
    ok &= bool(cond)
    print(f"  {'ok  ' if cond else 'FAIL'} {name}" + ("" if cond else f"  (got {got})"))


def trim(b):
    """Inked pixels of a bool image, cropped to their bounding box."""
    if not b.any():
        return b[:0, :0]
    ys, xs = np.nonzero(b.any(axis=1))[0], np.nonzero(b.any(axis=0))[0]
    return b[ys[0]:ys[-1] + 1, xs[0]:xs[-1] + 1]


def outline_check(rel, ft, info):
    """A TTF/OTF import against an independent rasteriser: each glyph's outline (fontTools, in design pixels)
    filled with the nonzero rule at every pixel centre, compared with its cell. Only straight segments are
    allowed (a curve cannot be pixel-exact). Returns (codes that differ, glyphs checked)."""
    from fontTools.ttLib import TTFont
    from fontTools.pens.recordingPen import DecomposingRecordingPen
    path = os.path.join(fi.VENDORED, rel)
    tt = TTFont(path)
    cmap, gs = tt.getBestCmap(), tt.getGlyphSet()
    upm, unit = fi.pixel_unit(path)
    cw, ch = ft.cw, ft.ch
    bad, n = [], 0
    for c in fi.codes(info["upper"]):
        if c not in cmap or c == 0x20 or c in set(info.get("left_out", [])):
            continue
        rec = DecomposingRecordingPen(gs)
        gs[cmap[c]].draw(rec)
        contours, cur = [], []
        for op, args in rec.value:
            if op == "moveTo":
                cur = [args[0]]
            elif op == "lineTo":
                cur.append(args[0])
            elif op in ("closePath", "endPath"):
                contours.append([(x / unit, y / unit) for x, y in cur])
            else:
                bad.append(f"{hex(c)} has curves")
        pts = [p for k in contours for p in k]
        if not pts:
            continue
        x0, x1 = int(np.floor(min(p[0] for p in pts))), int(np.ceil(max(p[0] for p in pts)))
        y0, y1 = int(np.floor(min(p[1] for p in pts))), int(np.ceil(max(p[1] for p in pts)))
        X, Y = np.meshgrid(np.arange(x0, x1) + 0.5, np.arange(y1 - 1, y0 - 1, -1) + 0.5)
        wind = np.zeros(X.shape, int)
        for k in contours:
            for (ax, ay), (bx, by) in zip(k, k[1:] + k[:1]):
                if ay != by:
                    xi = ax + (Y - ay) * (bx - ax) / (by - ay)
                    wind += ((ay <= Y) & (by > Y) & (xi > X)).astype(int) - ((by <= Y) & (ay > Y) & (xi > X))
        cell = ft.ink[(c >> 4) * ch:(c >> 4) * ch + ch, (c & 15) * cw:(c & 15) * cw + cw]
        n += 1
        if not np.array_equal(trim(cell), trim(wind != 0)):
            bad.append(hex(c))
    return bad, n


def main():
    out = tempfile.mkdtemp(prefix="fontimport-")
    fonts = []
    print("1-2. import every vendored font")
    for a in fi.available():
        rel = a["path"]
        png = os.path.join(out, rel.replace("/", "_") + ".png")
        try:
            info = fi.import_font(rel, png)
        except Exception as e:
            check(f"{rel} imports", False, e)
            continue
        ft = emu.FwFont(png, name=rel)
        fonts.append(ft)
        cw, ch = info["cell"]
        good = (ft.cw, ft.ch) == (cw, ch) and all(ft.width[c] for c in range(0x21, 0x7F) if c != 0x7F)
        check(f"{rel}: {cw}x{ch} cells, {info['codes']} codes, every printable ASCII glyph inked", good,
              [chr(c) for c in range(0x21, 0x7F) if not ft.width[c]])
        if rel.endswith((".bdf", ".hex")):
            src = os.path.join(fi.VENDORED, rel)
            glyphs = (fi.read_bdf if rel.endswith(".bdf") else fi.read_hex)(src)[0]
            bad = []
            for c in fi.codes("latin1"):
                if c not in glyphs:
                    continue
                cell = ft.ink[(c >> 4) * ch:(c >> 4) * ch + ch, (c & 15) * cw:(c & 15) * cw + cw]
                if not np.array_equal(trim(cell), trim(glyphs[c][3])):
                    bad.append(hex(c))
            check(f"{rel}: every glyph bit for bit as in the {rel[-3:].upper()} file", not bad, bad[:8])
        else:
            bad, n = outline_check(rel, ft, info)
            check(f"{rel}: every glyph ({n}) equals its outline filled on the design grid "
                  f"({info['design_px_per_em']} px/em)", not bad, bad[:8])
        d = [ft.ink[(0x30 + i >> 4) * ch:(0x30 + i >> 4) * ch + ch,
                    ((0x30 + i) & 15) * cw:((0x30 + i) & 15) * cw + cw].any(axis=0) for i in range(10)]
        off = [abs(int(np.nonzero(x)[0][0]) - (cw - 1 - int(np.nonzero(x)[0][-1]))) for x in d if x.any()]
        check(f"{rel}: digits centred (left/right margins differ by at most 2 px)", max(off) <= 2, off)
        check(f"{rel}: sidecar has licence and credits", info["licence"] and info["credits"], info)

    print("3. the firmware's font_build measures them as FwFont does")
    for i in range(0, len(fonts), 8):
        batch = fonts[i:i + 8]
        sign = FwSign(batch)
        for k, ft in enumerate(batch):
            left, width, prop = sign.font_tables(k)
            check(f"{ft.name}: left/width/proportional for 256 codes",
                  (left, width, prop) == (ft.left, ft.width, ft.prop))

    print("4. Latin-1 and the emoji range")
    png = os.path.join(out, "ascii.png")
    fi.import_font("spleen/spleen-8x16.bdf", png, upper="none")
    ft = emu.FwFont(png)
    check("upper='none' leaves 0x80..0xFF without ink", not ft.ink[8 * ft.ch:].any())
    png = os.path.join(out, "latin1.png")
    fi.import_font("spleen/spleen-8x16.bdf", png)
    ft = emu.FwFont(png)
    check("Latin-1: 0xE9 (é) is inked, 0x80..0x9F stays empty",
          ft.width[0xE9] and not any(ft.width[c] for c in range(0x80, 0xA0)))

    print("\nALL FONT IMPORT CHECKS PASS" if ok else "\nFAILURES")
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
