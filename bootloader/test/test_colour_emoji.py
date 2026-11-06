#!/usr/bin/env python3
"""Run the colour-emoji patch inside the real firmware (ELITE_RGB V4.20) under Unicorn.

Fonts and text go through the firmware's own code:
  png_rows_to_rgb555  (0x46288)  RGB rows  -> format-1 font image (the sign's normal fonts)
  png_rows_to_rgb555a (0x46360)  RGBA rows -> format-9 font image (a colour font)
  font_build          (0x448D4)  image -> font record
  font_draw_text      (0x43424)  text  -> frame, through font_draw_char (0x42DB0)

Checks:
  1. With the patch, a normal (mono) font draws exactly as without it.
  2. With the patch and a colour font, letters (< 0x80) draw exactly as without the patch.
  3. Emoji cells (>= 0x80) are drawn in their own colours: they match an independent pixel model of
     the format-9 pixels (opaque copied, alpha 0 left alone, partial alpha blended).
  4. Without the patch the same emoji come out as one-colour silhouettes (why the patch is needed).
Writes test/colour_emoji_*.png (8x scale) so the result can be looked at.

Usage: test_colour_emoji.py ELITE_RGB build-patch/emoji.bin ../analysis/extracted/fonte04.png
"""
import os
import struct
import sys

from PIL import Image, ImageDraw
from unicorn import Uc, UC_ARCH_PPC, UC_MODE_PPC32, UC_MODE_BIG_ENDIAN, UC_HOOK_CODE
from unicorn.ppc_const import UC_PPC_REG_1, UC_PPC_REG_3, UC_PPC_REG_LR, UC_PPC_REG_PC

ROWS_TO_RGB555, ROWS_TO_RGB555A, FONT_BUILD, DRAW_TEXT = 0x46288, 0x46360, 0x448D4, 0x43424
FONT_DRAW_CHAR, PATCH_BASE = 0x42DB0, 0xC60000
RAM = 0x1000000
ROWPTRS, ROWBUF, FONTPIX, FONTIMG, FONTREC = 0x500000, 0x510000, 0x580000, 0x5F0000, 0x5F1000
FRAME_IMG, FRAMEPIX, POS, SBUF, TEXT, EFFECT = 0x600000, 0x601000, 0x608000, 0x608010, 0x608020, 0x608100
STACK, STOP = 0x7F0000, 0x7FFF00
W, H = 80, 16
FG, BG = 0x7FE0, 0x0000                      # yellow letters on black cells (the firmware's mode 0)


def branch(src, dst):
    off = dst - src
    assert -0x2000000 <= off < 0x2000000 and not off & 3
    return 0x48000000 | (off & 0x03FFFFFC)


class Fw:
    def __init__(self, app, patch=None):
        self.uc = uc = Uc(UC_ARCH_PPC, UC_MODE_PPC32 | UC_MODE_BIG_ENDIAN)
        uc.mem_map(0, RAM)
        uc.mem_write(0, app)
        if patch:
            uc.mem_write(PATCH_BASE, patch)
            assert struct.unpack(">I", bytes(uc.mem_read(FONT_DRAW_CHAR, 4)))[0] == 0x9421FF70
            uc.mem_write(FONT_DRAW_CHAR, struct.pack(">I", branch(FONT_DRAW_CHAR, PATCH_BASE)))
        uc.hook_add(UC_HOOK_CODE, lambda u, a, s, d: u.emu_stop(), begin=STOP, end=STOP)

    def call(self, addr, *args):
        uc = self.uc
        for i, v in enumerate(args):
            uc.reg_write(UC_PPC_REG_3 + i, v)
        uc.reg_write(UC_PPC_REG_1, STACK)
        uc.reg_write(UC_PPC_REG_LR, STOP)
        uc.emu_start(addr, STOP, count=50_000_000)
        assert uc.reg_read(UC_PPC_REG_PC) == STOP, f"call {addr:#x} did not return"

    def load_font(self, img, alpha):
        """Font image -> firmware image (via its own converter) -> font record (via font_build)."""
        uc = self.uc
        w, h = img.size
        bpp = 4 if alpha else 3
        rows = img.convert("RGBA" if alpha else "RGB").tobytes()
        for y in range(h):
            uc.mem_write(ROWBUF + y * w * bpp, rows[y * w * bpp:(y + 1) * w * bpp])
            uc.mem_write(ROWPTRS + y * 4, struct.pack(">I", ROWBUF + y * w * bpp))
        uc.mem_write(FONTIMG, struct.pack(">IIIII", 9 if alpha else 1, w, h, FONTPIX, 0))
        self.call(ROWS_TO_RGB555A if alpha else ROWS_TO_RGB555, ROWPTRS, FONTIMG)
        uc.mem_write(FONTREC, bytes(0x718))
        self.call(FONT_BUILD, FONTREC, 2, FONTIMG)
        return FONTREC

    def font_pixels(self, w, h):
        return struct.unpack(f">{w * h}H", bytes(self.uc.mem_read(FONTPIX, w * h * 2)))

    def draw(self, font, text, background):
        uc = self.uc
        uc.mem_write(FRAME_IMG, struct.pack(">IIIII", 1, W, H, FRAMEPIX, 0))
        uc.mem_write(FRAMEPIX, struct.pack(f">{W * H}H", *background))
        uc.mem_write(POS, struct.pack(">ii", 1, 0))
        uc.mem_write(TEXT, text)
        uc.mem_write(SBUF, struct.pack(">IHH", TEXT, len(text), len(text)))
        uc.mem_write(EFFECT, struct.pack(">I", FG << 16 | BG))
        self.call(DRAW_TEXT, FRAME_IMG, POS, font, SBUF, EFFECT)
        return list(struct.unpack(f">{W * H}H", bytes(uc.mem_read(FRAMEPIX, W * H * 2))))


def test_emoji_cells(cw, chh):
    """Three test emoji: opaque shapes, a fully transparent surround, and half-transparent edges."""
    cells = []
    # 0x80: red heart-ish blob with a soft (alpha 128) rim
    im = Image.new("RGBA", (cw, chh), (0, 0, 0, 0))
    d = ImageDraw.Draw(im)
    d.ellipse((0, 2, cw - 1, chh - 2), fill=(255, 40, 60, 128))
    d.ellipse((2, 4, cw - 3, chh - 4), fill=(255, 30, 50, 255))
    cells.append(im)
    # 0x81: yellow smiley with black eyes
    im = Image.new("RGBA", (cw, chh), (0, 0, 0, 0))
    d = ImageDraw.Draw(im)
    d.ellipse((0, 2, cw - 1, chh - 3), fill=(255, 220, 0, 255))
    d.point([(cw // 3, chh // 3 + 1), (2 * cw // 3, chh // 3 + 1)], fill=(0, 0, 0, 255))
    cells.append(im)
    # 0x82: green / blue stripes, alternating opacity (exercises the blend for every alpha level)
    im = Image.new("RGBA", (cw, chh), (0, 0, 0, 0))
    for y in range(chh):
        for x in range(cw):
            if (x + y) % 3:
                im.putpixel((x, y), (0, 200, 255, 255) if y % 2 else (60, 255, 60, 32 * (x % 8)))
    cells.append(im)
    return cells


def colour_font(mono_png):
    """fonte04's letters (white on OPAQUE black) plus emoji in cells 0x80.. (transparent background)."""
    base = Image.open(mono_png).convert("RGB")
    fw_, fh = base.size
    cw, chh = fw_ // 16, fh // 16
    img = base.convert("RGBA")                      # every pixel alpha 255: letters keep the mask path
    for i, cell in enumerate(test_emoji_cells(cw, chh)):
        code = 0x80 + i
        x0, y0 = (code % 16) * cw, (code // 16) * chh
        img.paste(Image.new("RGBA", (cw, chh), (0, 0, 0, 0)), (x0, y0))
        img.paste(cell, (x0, y0))
    return img, cw, chh


def model_cell(frame, font_px, fw_, cw, chh, code, pos):
    """Independent model of what a colour glyph must look like (format-9 pixels over the frame)."""
    out = list(frame)
    col, row = code % 16, code // 16
    for y in range(chh):
        for x in range(cw):
            dx, dy = pos[0] + x, pos[1] + y
            if not (0 <= dx < W and 0 <= dy < H):
                continue
            p = font_px[(row * chh + y) * fw_ + col * cw + x]
            if not p & 0x8000:
                out[dy * W + dx] = p
                continue
            a3 = (p >> 12) & 7
            if a3 == 0:
                continue
            a = a3 * 4 + 3
            d = out[dy * W + dx]
            sr, sg, sb = ((p >> 8) & 15) << 1 | 1, ((p >> 4) & 15) << 1 | 1, (p & 15) << 1 | 1
            dr, dg, db = (d >> 10) & 31, (d >> 5) & 31, d & 31
            out[dy * W + dx] = ((a * sr + (31 - a) * dr) // 31) << 10 | ((a * sg + (31 - a) * dg) // 31) << 5 | \
                               ((a * sb + (31 - a) * db) // 31)
    return out


def save(frame, path):
    im = Image.new("RGB", (W, H))
    im.putdata([((v >> 10 & 31) << 3, (v >> 5 & 31) << 3, (v & 31) << 3) for v in frame])
    im.resize((W * 8, H * 8), Image.NEAREST).save(path)


def main(app_path, patch_path, mono_png):
    app, patch = open(app_path, "rb").read(), open(patch_path, "rb").read()
    background = [((x * 3) & 31) << 5 | ((y * 2) & 31) for y in range(H) for x in range(W)]   # a gradient
    mono = Image.open(mono_png)
    cimg, cw, chh = colour_font(mono_png)
    text = b"Hi\x80\x81\x82!"
    here = os.path.dirname(os.path.abspath(__file__))
    ok = True

    # 1. mono font: patched == unpatched
    plain = Fw(app)
    f = plain.load_font(mono, alpha=False)
    ref_mono = plain.draw(f, text, background)
    patched = Fw(app, patch)
    f = patched.load_font(mono, alpha=False)
    got_mono = patched.draw(f, text, background)
    good = got_mono == ref_mono
    ok &= good
    print(f"{'ok  ' if good else 'FAIL'} mono font (fonte04): patched firmware draws exactly as the original")

    # 2-4. colour font
    plain = Fw(app)
    f = plain.load_font(cimg, alpha=True)
    ref = plain.draw(f, text, background)
    patched = Fw(app, patch)
    f = patched.load_font(cimg, alpha=True)
    fpx = patched.font_pixels(*cimg.size)
    got = patched.draw(f, text, background)

    letters = [i for i in range(W * H) if (i % W) - 1 < 2 * cw or (i % W) - 1 >= 5 * cw]   # 'H','i','!' columns
    good = all(got[i] == ref[i] for i in letters)
    ok &= good
    print(f"{'ok  ' if good else 'FAIL'} colour font: letters (< 0x80) draw exactly as the original firmware")

    want = list(ref)
    # emoji cells: start from what the original drew there, but the model draws over the BACKGROUND
    for i in range(W * H):
        if not (i in set(letters)):
            want[i] = background[i]
    for k, code in enumerate((0x80, 0x81, 0x82)):
        want = model_cell(want, fpx, cimg.size[0], cw, chh, code, (1 + (2 + k) * cw, 0))
    good = got == want
    ok &= good
    diff = sum(a != b for a, b in zip(got, want))
    print(f"{'ok  ' if good else 'FAIL'} colour font: emoji (0x80..0x82) match the pixel model "
          f"(opaque copied, alpha 0 left alone, partial alpha blended){'' if good else f' ({diff} px differ)'}")

    emoji_cols = [i for i in range(W * H) if i not in set(letters)]
    silhouettes = len({ref[i] for i in emoji_cols}) <= 2
    coloured = len({got[i] for i in emoji_cols}) > 10
    good = silhouettes and coloured
    ok &= good
    print(f"{'ok  ' if good else 'FAIL'} without the patch the emoji cells use {len({ref[i] for i in emoji_cols})} colours "
          f"(mask silhouettes); with it {len({got[i] for i in emoji_cols})} colours")

    save(ref, os.path.join(here, "colour_emoji_original.png"))
    save(got, os.path.join(here, "colour_emoji_patched.png"))
    print("\nALL COLOUR-EMOJI CHECKS PASS" if ok else "\nFAILURES")
    return 0 if ok else 1


if __name__ == "__main__":
    if len(sys.argv) != 4:
        raise SystemExit(__doc__)
    sys.exit(main(*sys.argv[1:]))
