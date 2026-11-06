#!/usr/bin/env python3
"""Turn a vendored pixel font (third_party/fonts, see THIRD-PARTY.md) into a sign font, fonteNN.png.

A sign font (font_build 0x448D4, layout 2; sign_emu.FwFont) is a 16 x 16 grid of cells, one per code 0..255,
cell = (png width / 16, png height / 16). A pixel is ink when it is not black. The firmware measures each glyph
itself: letters are trimmed to their inked columns (proportional), while '0'..'9', 0x10..0x19 and 0xFF use the
whole cell and the space is fixed at png width / 64 (at least 3). So the importer places every glyph on a
common baseline, sizes the cell for the widest ink and the digit advance, and centres the digits (layout()).

Codes 0x00..0x09, 0x10..0x19 and 0x1C get the extra digit glyphs the money formatter uses
(add_amount_glyphs). Other codes are Unicode code points: 0x20..0x7E always, and 0xA0..0xFF (Latin-1) unless upper="none", which leaves
0x80..0xFF empty. 0x80..0x9F stays empty either way.

Sources:
  - BDF (Spleen, Tamzen, X11 misc-fixed, Cozette, ...) and Unifont-style .hex (unscii): read exactly, bit for bit.
  - TTF/OTF drawn on a pixel grid (monogram, m5x7, Jersey 10, ...): the design pixel is read from the outlines
    (the gcd of their coordinates, fontTools) and FreeType renders the font at that design size, one design
    pixel = one output pixel, no scaling. Each glyph is rendered with and without anti-aliasing; the two must
    agree and the anti-aliased one must have no partial-coverage pixel, or the import fails (read_ttf).
    The cell height is trimmed to the ink of the imported glyphs.
Each output gets a sidecar fonteNN.font.json with the source, licence and credit line (studio only).
"""
import json
import os
import re

import numpy as np
from PIL import Image, ImageFont

from . import paths

ROOT = paths.REPO
VENDORED = paths.FONTS

# folder -> (licence, credit line); see THIRD-PARTY.md
LICENCES = {
    "spleen": ("BSD 2-Clause", "Spleen by Frederic Cambus (https://github.com/fcambus/spleen), BSD 2-Clause licence."),
    "tamzen": ("Tamzen/Tamsyn free licence", "Tamzen by Suraj N. Kurapati, based on Tamsyn by Scott Fial "
               "(https://github.com/sunaku/tamzen-font)."),
    "misc-fixed": ("public domain", "X11 misc-fixed fonts by Markus Kuhn and contributors (public domain)."),
    "cozette": ("MIT", "Cozette by Samhain and contributors (https://github.com/the-moonwitch/Cozette), MIT licence."),
    "monogram": ("CC0 1.0", "monogram by Vinícius Menézio (https://datagoblin.itch.io/monogram), CC0."),
    "m5x7": ("CC0 1.0", "m5x7 by Daniel Linssen (https://managore.itch.io/m5x7), CC0."),
    "jersey10": ("SIL OFL 1.1", "Jersey 10, Copyright 2023 The Soft Type Project Authors "
                 "(https://github.com/scfried/soft-type-jersey), SIL Open Font License 1.1."),
    "tiny5": ("SIL OFL 1.1", "Tiny5, Copyright 2026 The Tiny5 Project Authors (https://github.com/Gissio/font_Tiny5), "
              "SIL Open Font License 1.1."),
    "micro5": ("SIL OFL 1.1", "Micro 5, Copyright 2023 The Soft Type Project Authors "
               "(https://github.com/scfried/soft-type-micro), SIL Open Font License 1.1."),
    "kenney": ("CC0 1.0", "Kenney Fonts by Kenney (https://kenney.nl/assets/kenney-fonts), CC0."),
    "pressstart2p": ("SIL OFL 1.1", "Sign Start 8: converted from Press Start 2P, Copyright 2012 The Press Start 2P "
                     "Project Authors (cody@zone38.net), with Reserved Font Name \"Press Start 2P\", "
                     "SIL Open Font License 1.1."),
    "scientifica": ("SIL OFL 1.1", "scientifica, Copyright (c) 2020 Akshay Oppiliappan "
                    "(https://github.com/oppiliappan/scientifica), SIL Open Font License 1.1."),
    "creep2": ("MIT", "creep2 by Raymond W. Ko (https://github.com/raymond-w-ko/creep2), from creep, Copyright (c) 2014 "
               "Romeo Van Snick (https://github.com/romeovs/creep), MIT licence."),
    "departure-mono": ("SIL OFL 1.1", "Departure Mono, Copyright 2022-2024 Helena Zhang (helenazhang.com) "
                       "(https://github.com/rektdeckard/departure-mono), SIL Open Font License 1.1."),
    "unscii": ("public domain", "unscii by Viznut (http://viznut.fi/unscii/), public domain."),
    "jacquardabastarda9": ("SIL OFL 1.1", "Jacquarda Bastarda 9, Copyright 2023 The Soft Type Project Authors "
                           "(https://github.com/scfried/soft-type-jacquarda-bastarda), SIL Open Font License 1.1."),
}
# OFL fonts with a Reserved Font Name: a converted font is a Modified Version and must not carry that name.
RENAMED = {"pressstart2p": ("Sign Start 8", "Press Start 2P")}


def available():
    """The importable vendored fonts: [{"path" (relative to third_party/fonts), "family", "licence"}]."""
    out = []
    for fam in sorted(os.listdir(VENDORED)) if os.path.isdir(VENDORED) else []:
        if fam not in LICENCES:
            continue
        for f in sorted(os.listdir(os.path.join(VENDORED, fam))):
            if f.lower().endswith((".bdf", ".hex", ".ttf", ".otf")):
                out.append({"path": f"{fam}/{f}", "family": fam, "licence": LICENCES[fam][0]})
    return out


def codes(upper="latin1"):
    cs = list(range(0x20, 0x7F))
    if upper == "latin1":
        cs += list(range(0xA0, 0x100))
    elif upper != "none":
        raise ValueError('upper must be "latin1" or "none"')
    return cs


# ---- BDF ----------------------------------------------------------------------------------------------------
def read_bdf(path):
    """{code: (advance, xoff, yoff, bitmap bool[h][w])}, ascent, descent."""
    glyphs, ascent, descent = {}, None, None
    fbb = None
    lines = open(path, encoding="latin-1").read().splitlines()
    i = 0
    while i < len(lines):
        ln = lines[i]
        if ln.startswith("FONT_ASCENT "):
            ascent = int(ln.split()[1])
        elif ln.startswith("FONT_DESCENT "):
            descent = int(ln.split()[1])
        elif ln.startswith("FONTBOUNDINGBOX "):
            fbb = [int(v) for v in ln.split()[1:5]]
        elif ln.startswith("STARTCHAR"):
            enc, adv, bbx, rows = None, None, None, []
            i += 1
            while not lines[i].startswith("ENDCHAR"):
                p = lines[i].split()
                if p[0] == "ENCODING":
                    enc = int(p[1])
                elif p[0] == "DWIDTH":
                    adv = int(p[1])
                elif p[0] == "BBX":
                    bbx = [int(v) for v in p[1:5]]
                elif p[0] == "BITMAP":
                    i += 1
                    while not lines[i].startswith("ENDCHAR"):
                        rows.append(lines[i].strip())
                        i += 1
                    break
                i += 1
            if enc is not None and enc >= 0 and bbx:
                w, h, xo, yo = bbx
                bits = np.zeros((h, w), bool)
                for y, r in enumerate(rows[:h]):
                    v, n = int(r, 16), len(r) * 4
                    bits[y] = [(v >> (n - 1 - x)) & 1 for x in range(w)]
                glyphs[enc] = (adv if adv is not None else w, xo, yo, bits)
        i += 1
    if ascent is None or descent is None:
        ascent, descent = fbb[1] + fbb[3], -fbb[3]
    return glyphs, ascent, descent


# ---- .hex (GNU Unifont format, unscii) --------------------------------------------------------------------
def read_hex(path):
    """A Unifont-style .hex file, "CODE:BITS" per line, rows top first, read exactly. The glyph height is the
    font's: 8 when any glyph is 16 hex digits long (unscii-8: 8 x 8), else 16 (Unifont, unscii-16). A glyph is
    as wide as its digits allow (8 or 16 px). The baseline is put under the bottom row of 'H'.
    Returns the same as read_bdf."""
    rows = {}
    for ln in open(path, encoding="ascii"):
        ln = ln.strip()
        if ":" in ln:
            c, bits = ln.split(":", 1)
            rows[int(c, 16)] = bits
    h = 8 if any(len(b) == 16 for b in rows.values()) else 16
    glyphs = {}
    for c, bits in rows.items():
        w = len(bits) * 4 // h
        if w * h != len(bits) * 4:
            raise ValueError(f"{os.path.basename(path)}: U+{c:04X} is not {h} rows high")
        v, n = int(bits, 16), len(bits) * 4
        b = np.array([(v >> (n - 1 - i)) & 1 for i in range(n)], bool).reshape(h, w)
        glyphs[c] = [w, 0, 0, b]
    hb = glyphs[0x48][3]
    descent = h - 1 - int(np.nonzero(hb.any(axis=1))[0][-1])
    for g in glyphs.values():
        g[2] = -descent
    return {c: tuple(g) for c, g in glyphs.items()}, h - descent, descent


# ---- TTF/OTF on a pixel grid -------------------------------------------------------------------------------
def _outline_points(font, gname):
    """Every point (on- and off-curve) of a glyph's outline in font units, components resolved (glyf or CFF)."""
    from fontTools.pens.recordingPen import DecomposingRecordingPen
    gs = font.getGlyphSet()
    pen = DecomposingRecordingPen(gs)
    gs[gname].draw(pen)
    return [pt for op, args in pen.value for pt in args]


MAX_PX_PER_EM = 48                                             # a design taller than this is no pixel font for a 16 px sign
CORE = [c for c in range(0x30, 0x7B) if chr(c).isalnum()]       # 0-9 A-Z a-z: they set the grid and must be exact


def pixel_unit(path):
    """(units per em, font units per design pixel): the gcd of every outline coordinate of 0-9, A-Z, a-z.
    Other glyphs off that grid fail the exactness check in read_ttf and are left out."""
    import math
    from fontTools.ttLib import TTFont
    f = TTFont(path)
    cmap, g = f.getBestCmap(), 0
    for c in CORE:
        if c in cmap:
            for x, y in _outline_points(f, cmap[c]):
                g = math.gcd(g, int(round(x)), int(round(y)))
    if not g:
        raise ValueError(f"{os.path.basename(path)}: no outlines")
    if f["head"].unitsPerEm / g > MAX_PX_PER_EM:     # e.g. gcd 1: the outlines are not on a coarse grid
        raise ValueError(f"{os.path.basename(path)}: its outlines are on no pixel grid (gcd {g} of "
                         f"{f['head'].unitsPerEm} units per em); not a pixel font")
    return f["head"].unitsPerEm, g


def design_size_font(path, unit, cs):
    """(PIL font at its design pixel size, px per em): one design pixel (unit font units) = one output pixel.

    FreeType does not get the file itself but a plain copy built in memory (fontTools) with the same outlines,
    advances and cmap for the codes cs, and nothing else:
      - TrueType (glyf) outlines, whatever the source: a CFF/OTF font's outlines are copied point for point
        (curves, if any, become quadratic), so one rasteriser path serves both;
      - no hinting (no instructions, no CFF hints). A pixel font's points are already on the grid, and hinting
        only moves them: Jersey 10's own bytecode shifts some points by 1/64 px at this size;
      - FreeType sets sizes in 1/64 px, so a design size that is not a whole number of px per em (Jersey 10:
        1400 / 75 = 18.67) cannot be set exactly. The copy's unitsPerEm is the next multiple of the unit
        (1425 = 19 x 75) and it is set at that whole size (19 px per em). Only the em square changes, not a
        coordinate: every outline point lands on the same whole pixel."""
    import io
    from fontTools.fontBuilder import FontBuilder
    from fontTools.pens.cu2quPen import Cu2QuPen
    from fontTools.pens.boundsPen import ControlBoundsPen
    from fontTools.pens.recordingPen import DecomposingRecordingPen
    from fontTools.pens.ttGlyphPen import TTGlyphPen
    from fontTools.ttLib import TTFont
    src = TTFont(path)
    upm = src["head"].unitsPerEm
    cmap, gs, hmtx = src.getBestCmap(), src.getGlyphSet(), src["hmtx"]
    names, glyphs, metrics, cm = [".notdef"], {}, {".notdef": (0, 0)}, {}
    glyphs[".notdef"] = TTGlyphPen(None).glyph()
    for c in cs:
        if c not in cmap:
            continue
        n = f"u{c:04X}"
        rec, pen, box = DecomposingRecordingPen(gs), TTGlyphPen(None), ControlBoundsPen(None)
        gs[cmap[c]].draw(rec)                              # components resolved into plain contours
        rec.replay(Cu2QuPen(pen, max_err=1, reverse_direction=False))
        rec.replay(box)
        # the left side bearing must be the outline's xMin: FreeType puts the origin at xMin - lsb
        lsb = int(box.bounds[0]) if box.bounds else 0
        glyphs[n], metrics[n], cm[c] = pen.glyph(), (hmtx[cmap[c]][0], lsb), n
        names.append(n)
    px = -(-upm // unit)                                   # whole px per em
    fb = FontBuilder(px * unit, isTTF=True)
    fb.setupGlyphOrder(names)
    fb.setupCharacterMap(cm)
    fb.setupGlyf(glyphs)
    fb.setupHorizontalMetrics(metrics)
    asc = src["hhea"].ascent
    fb.setupHorizontalHeader(ascent=asc, descent=src["hhea"].descent)
    fb.setupOS2(sTypoAscender=asc, usWinAscent=max(asc, 1), usWinDescent=abs(src["hhea"].descent))
    fb.setupNameTable({"familyName": "pixel", "styleName": "Regular"})
    fb.setupPost()
    buf = io.BytesIO()
    fb.save(buf)
    return ImageFont.truetype(io.BytesIO(buf.getvalue()), px), upm / unit


def render_glyph(font, ch, aa, pad=8):
    """A glyph drawn by FreeType with its origin on a whole pixel: (uint8 image, baseline row, origin column).
    aa=False: FreeType's monochrome renderer; aa=True: 8-bit coverage (anti-aliased)."""
    from PIL import ImageDraw
    l, t, r, b = font.getbbox(ch, anchor="ls")
    asc, w = max(0, -t) + pad, max(0, r) + 2 * pad
    im = Image.new("L", (w, asc + max(0, b) + pad), 0)
    d = ImageDraw.Draw(im)
    d.fontmode = "L" if aa else "1"
    d.text((pad, asc), ch, font=font, fill=255, anchor="ls")
    return np.array(im), asc, pad


def check_exact(font, ch):
    """None when the glyph is pixel-exact at this size: the anti-aliased rendering has no partial-coverage pixel
    and its ink equals the monochrome rendering's. Otherwise a short reason."""
    mono, _, _ = render_glyph(font, ch, False)
    aa, _, _ = render_glyph(font, ch, True)
    part = int(((aa > 0) & (aa < 255)).sum())
    diff = int(((mono > 0) != (aa > 0)).sum())
    return None if not part and not diff else f"{part} partial-coverage px, {diff} px differ from mono"


def read_ttf(path, cs):
    """Rasterise a TTF/OTF drawn on a pixel grid at its design pixel size (one design pixel = pixel_unit font
    units = one output pixel), with FreeType, no scaling. Every glyph is checked: anti-aliased and monochrome
    renderings must agree and the anti-aliased one must have no partial-coverage pixel. A glyph that fails is
    left out (and listed); if one of 0-9, A-Z, a-z fails, or more than a quarter of the glyphs, the font is not
    a pixel font at this grid and ValueError is raised. So every glyph imported is exact.
    Returns glyphs (BDF-style), ascent, descent, design px per em, the glyphs left out ["U+XXXX reason"]."""
    from fontTools.ttLib import TTFont
    tt = TTFont(path)
    cmap = tt.getBestCmap()
    cs = [c for c in cs if c in cmap]
    upm, unit = pixel_unit(path)
    font, px_em = design_size_font(path, unit, cs)
    hmtx = tt["hmtx"]
    raw, bad = {}, []
    top_all = bot_all = 0
    for c in cs:
        ch = chr(c)
        why = check_exact(font, ch)
        if why:
            bad.append(f"U+{c:04X} {why}")
            continue
        a, base, x0 = render_glyph(font, ch, False)
        ink = a > 0
        if c != 0x20 and not ink.any():
            continue
        adv = hmtx[cmap[c]][0] / unit
        rows = np.nonzero(ink.any(axis=1))[0]
        cols = np.nonzero(ink.any(axis=0))[0]
        if len(rows):
            above, below = base - rows[0], rows[-1] + 1 - base          # ink rows above / below the baseline
            top_all, bot_all = max(top_all, above), max(bot_all, below)
            bits, xoff, ybot = ink[rows[0]:rows[-1] + 1, cols[0]:cols[-1] + 1], cols[0] - x0, -below
        else:
            bits, xoff, ybot = np.zeros((0, 0), bool), 0, 0
        raw[c] = (int(round(adv)), int(xoff), int(ybot), bits)
    core = [b for b in bad if int(b[2:6], 16) in CORE]
    if core or len(bad) > len(cs) // 4:
        raise ValueError(f"{os.path.basename(path)} is not pixel-exact at {px_em:g} px/em: " + "; ".join(bad[:6]))
    return raw, int(top_all), int(bot_all), px_em, bad


def fit_ink(glyphs, ascent, descent, cs):
    """FONT_ASCENT / FONT_DESCENT grown, if needed, to hold the ink of every glyph of cs (Tiny5's accented
    capitals and scientifica's U+00A6 reach above / below them); a glyph is never clipped."""
    for c in cs:
        if c in glyphs:
            adv, xo, yo, b = glyphs[c]
            rows = np.nonzero(b.any(axis=1))[0]
            if len(rows):
                ascent = max(ascent, yo + b.shape[0] - int(rows[0]))
                descent = max(descent, -(yo + b.shape[0] - int(rows[-1]) - 1))
    return ascent, descent


# ---- the sign font ------------------------------------------------------------------------------------------
DIGITS = set(range(0x30, 0x3A)) | set(range(0x10, 0x1A))          # the whole-cell codes (font_build)
SMALL_DIGIT_FONTS = ["misc-fixed/4x6.bdf", "misc-fixed/5x7.bdf", "misc-fixed/5x8.bdf", "misc-fixed/6x9.bdf",
                     "misc-fixed/6x10.bdf", "misc-fixed/6x12.bdf", "misc-fixed/7x13.bdf"]


def bold_rel(rel):
    """The same family's bold BDF of the same size, if vendored: Tamzen ...r -> ...b, misc-fixed WxH -> WxHB."""
    fam, name = rel.split("/")
    cands = []
    if name.endswith("r.bdf"):
        cands.append(name[:-5] + "b.bdf")
    if fam == "misc-fixed" and name[:-4].replace("x", "").isdigit():
        cands.append(name[:-4] + "B.bdf")
    for c in cands:
        if os.path.exists(os.path.join(VENDORED, fam, c)):
            return f"{fam}/{c}"
    return None


def bold_glyphs(rel):
    b = bold_rel(rel) if rel.endswith(".bdf") else None
    return read_bdf(os.path.join(VENDORED, b))[0] if b else None


def add_amount_glyphs(glyphs, ascent, line_h, rel=""):
    """The extra codes the sign's money formatter uses (amount_format, 0x479B4), as in the sign's own fonts:
    0x10..0x19 and 0x1C = bold odometer digits and '$' (here a synthetic bold: each glyph OR its 1-px right
    shift), 0x00..0x09 = small digits for the cents, top-aligned with the normal digits (here from the
    public-domain X11 misc-fixed fonts, the largest whose digits are at most half the line height, or the font's own
    digits when none fits).
    Returns a note for the sidecar."""
    note = []
    bold = bold_glyphs(rel)
    for src, dst in [(0x30 + i, 0x10 + i) for i in range(10)] + [(0x24, 0x1C)]:
        if src in glyphs and dst not in glyphs:
            glyphs[dst] = bold[src] if bold and src in bold else glyphs[src]
    note.append(f"0x10-0x19, 0x1C: digits and '$' from {bold_rel(rel)} (the family's bold)" if bold else
                "0x10-0x19, 0x1C: the font's own digits and '$' (the family has no bold of this size)")
    if 0x30 not in glyphs:
        return note
    adv, xo, yo, b = glyphs[0x30]
    rows = np.nonzero(b.any(axis=1))[0]
    digit_top = yo + b.shape[0] - rows[0] if len(rows) else ascent       # top of '0' above the baseline
    digit_h = rows[-1] - rows[0] + 1 if len(rows) else ascent
    best = None
    for rel in SMALL_DIGIT_FONTS:
        g, _, _ = read_bdf(os.path.join(VENDORED, rel))
        r = np.nonzero(g[0x30][3].any(axis=1))[0]
        h = r[-1] - r[0] + 1
        if h * 2 <= line_h and h < digit_h:                                # about half the line, like the sign's fonts
            best = (rel, g)
    if best:
        rel, g = best
        for i in range(10):
            sadv, sxo, syo, sb = g[0x30 + i]
            r = np.nonzero(sb.any(axis=1))[0]
            sb = sb[r[0]:r[-1] + 1]
            glyphs[i] = (sadv, max(sxo, 0), digit_top - sb.shape[0], sb)  # its top on the top of the normal digits
        note.append(f"0x00-0x09: small digits from {rel} (public domain)")
    else:                                                                 # nothing small enough: the normal digits
        for i in range(10):
            if 0x30 + i in glyphs:
                glyphs[i] = glyphs[0x30 + i]
        note.append("0x00-0x09: the font's own digits (no smaller digits fit this height)")
    return note


def layout(glyphs, ascent, descent, cs):
    """The 16 x 16 ink grid. Returns (ink array, cell w, cell h, codes placed).

    The firmware trims letters to their ink but draws digits as whole cells, so the cell is as wide as the
    widest inked glyph or the digit advance, whichever is more; letters start at the cell's left edge and
    digits are centred in it, keeping the gaps between digits close to the font's own (a digit whose ink would
    then sit more than a pixel off-centre is centred by its ink instead)."""
    used = {c: g for c, g in glyphs.items() if c in cs}
    if 0x41 not in used:
        raise ValueError("the font has no 'A'")
    ch = ascent + descent

    def ink_cols(b):
        cols = np.nonzero(b.any(axis=0))[0]
        return (int(cols[0]), int(cols[-1]) + 1) if len(cols) else (0, 0)
    cw = max([1] + [ink_cols(b)[1] - ink_cols(b)[0] for c, (adv, xo, yo, b) in used.items() if c not in DIGITS]
             + [adv for c, (adv, xo, yo, b) in used.items() if c in DIGITS])
    ink = np.zeros((16 * ch, 16 * cw), bool)
    placed = []
    for c, (adv, xo, yo, b) in used.items():
        x0, y0 = (c % 16) * cw, (c // 16) * ch
        l, r = ink_cols(b)
        if c in DIGITS:
            shift = (cw - adv) // 2 + xo                  # the digit's own advance box, centred in the cell
            own = 0x30 <= c <= 0x39 or used.get(c + 0x20) is used[c]     # a digit, or a copy of one
            if own and r > l and abs((shift + l) - (cw - (shift + r))) > 2:
                shift = (cw - (r - l)) // 2 - l           # ink far off-centre in its box (Tiny5's '1'): centre the ink
        else:
            shift = -l                                    # ink from the left edge (the firmware trims anyway)
        top = ascent - (yo + b.shape[0])                  # bbox relative to the origin on the baseline
        for y in range(b.shape[0]):
            yy = top + y
            if not 0 <= yy < ch:
                continue
            for x in range(b.shape[1]):
                xx = shift + x
                if b[y, x] and 0 <= xx < cw:
                    ink[y0 + yy, x0 + xx] = True
        placed.append(c)
    return ink, int(cw), int(ch), sorted(int(c) for c in placed)


def save_sign_png(ink, out_png):
    """A 1-bit palette PNG like the sign's own fonts: index 0 black, 1 white."""
    im = Image.frombytes("P", (ink.shape[1], ink.shape[0]), ink.astype(np.uint8).tobytes())
    im.putpalette([0, 0, 0, 255, 255, 255])
    im.save(out_png, bits=1, optimize=True)


def import_font(rel, out_png, upper="latin1"):
    """Convert third_party/fonts/<rel> into out_png. Returns the sidecar dict (also written next to it)."""
    path = os.path.join(VENDORED, rel)
    fam = rel.split("/")[0]
    if fam not in LICENCES or not os.path.exists(path):
        raise ValueError(f"not a vendored font: {rel}")
    cs, size, dropped = codes(upper), None, []
    if path.lower().endswith(".bdf"):
        glyphs, ascent, descent = read_bdf(path)
    elif path.lower().endswith(".hex"):
        glyphs, ascent, descent = read_hex(path)
    if not path.lower().endswith((".ttf", ".otf")):
        ascent, descent = fit_ink(glyphs, ascent, descent, cs)
    else:
        glyphs, ascent, descent, size, dropped = read_ttf(path, cs)
    extras = add_amount_glyphs(glyphs, ascent, ascent + descent, rel)
    cs = sorted(set(cs) | set(range(0x00, 0x0A)) | set(range(0x10, 0x1A)) | {0x1C})
    if out_png.endswith(".mqf"):                         # our own format: the designer's metrics, cropped bitmaps
        from . import mqf
        data = mqf.write(glyphs, ascent, descent, cs, out_png)
        placed = sorted(c for c in cs if c in glyphs)
        cw, ch = max([glyphs[c][0] for c in placed] + [1]), ascent + descent
    else:
        ink, cw, ch, placed = layout(glyphs, ascent, descent, cs)
        save_sign_png(ink, out_png)
    info = {"source": f"third_party/fonts/{rel}", "licence": LICENCES[fam][0], "credits": [LICENCES[fam][1]],
            "cell": [cw, ch], "codes": len(placed), "upper": upper, "amount_glyphs": extras}
    if size:
        info["design_px_per_em"] = round(size, 3)
    if dropped:
        info["left_out"] = dropped                        # glyphs off the pixel grid (not exact), not imported
    if fam in RENAMED:
        info["name"] = RENAMED[fam][0]
        info["note"] = (f"SIL OFL 1.1 with Reserved Font Name \"{RENAMED[fam][1]}\": this converted font is a "
                        f"Modified Version, named \"{RENAMED[fam][0]}\", and stays under the OFL.")
    elif LICENCES[fam][0] == "SIL OFL 1.1":
        info["note"] = "SIL OFL 1.1: this converted font stays under the OFL (no Reserved Font Name)."
    json.dump(info, open(sidecar(out_png), "w"), ensure_ascii=False, indent=1)
    return info


def sidecar(png):
    return os.path.splitext(png)[0] + ".font.json"


def font_info(png):
    p = sidecar(png)
    return json.load(open(p)) if os.path.exists(p) else {}


def read_set(path):
    """[(sign font name, vendored path)] of a font list such as firmware/fonts.txt"""
    out = []
    for ln in open(path, encoding="utf-8"):
        ln = ln.split("#", 1)[0].strip().split(None, 1)       # the path is the rest of the line (it may have spaces)
        if len(ln) == 2:
            out.append((ln[0], ln[1].strip()))
    return out


def install_set(path, out_dir, keep=True):
    """Convert every font of a list into out_dir (fonteNN.png and its sidecar); keep: leave existing ones alone."""
    os.makedirs(out_dir, exist_ok=True)
    done = []
    for name, rel in read_set(path):
        dst = os.path.join(out_dir, name)
        if keep and os.path.exists(dst) and font_info(dst).get("source") == f"third_party/fonts/{rel}":
            continue
        import_font(rel, dst)
        done.append(name)
    return done


if __name__ == "__main__":
    import sys
    if sys.argv[1:2] == ["--set"] and len(sys.argv) >= 4:      # font_import.py --set LIST OUT_DIR
        print("converted:", " ".join(install_set(sys.argv[2], sys.argv[3])) or "(all there)")
        sys.exit(0)
    if len(sys.argv) < 3:
        print("usage: font_import.py FAMILY/FILE OUT.png [latin1|none]\nvendored fonts:")
        for a in available():
            print("  ", a["path"], f'({a["licence"]})')
        sys.exit(1)
    i = import_font(sys.argv[1], sys.argv[2], *(sys.argv[3:4] or ["latin1"]))
    print(sys.argv[2], i)
