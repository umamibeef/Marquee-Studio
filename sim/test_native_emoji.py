#!/usr/bin/env python3
"""Native emoji, tested inside the running firmware (firmware/ext/native_emoji.c, docs/NATIVE-EMOJI.md).

Boots a complete boot-flash image in the system simulator (the original boot loader, the extension loader, the
app), lets it run until the fonts are loaded, then calls the firmware's text routines in the live system:
  1. text without emoji: ext_text_width and the drawers take the original path (results equal a direct call of
     the original code, <name>__orig);
  2. the first emoji loads emoji.png / emoji.map from the boot flash through the firmware's own LFS and PNG code
     (the state block then holds the unpacked emoji.bin: palette, map and pixels, equal to the file);
  3. "A<emoji>B" drawn by text_draw_proportional_plain: the letters equal the original routine drawing "A" and
     "B" on their own at the same places; the emoji pixels equal the cell drawn 1:1 from the file;
     the width returned equals text_width, and both equal the width rule for the glyph sequence.
Usage: test_native_emoji.py [FLASH.bin]   (default bootloader/build/bootflash-fw.bin, from make fwimage)
"""
import os
import struct
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(HERE)
sys.path.insert(0, HERE)
sys.path.insert(0, os.path.join(REPO, "studio"))
import appsim  # noqa: E402
import flashtest  # noqa: E402
from marquee_studio import native_emoji, paths  # noqa: E402
from marquee_studio.fonts import _fwfont  # noqa: E402
sys.path.insert(0, os.path.join(REPO, "analysis", "tools"))
from difftest import symbol_address  # noqa: E402
from unicorn import UC_HOOK_CODE  # noqa: E402
from unicorn.ppc_const import (UC_PPC_REG_0, UC_PPC_REG_1, UC_PPC_REG_3, UC_PPC_REG_LR, UC_PPC_REG_CR,  # noqa: E402
                               UC_PPC_REG_CTR, UC_PPC_REG_XER, UC_PPC_REG_PC)

ALL_REGS = [UC_PPC_REG_0 + i for i in range(32)] + [UC_PPC_REG_LR, UC_PPC_REG_CR, UC_PPC_REG_CTR, UC_PPC_REG_XER,
                                                    UC_PPC_REG_PC]

DISPATCH_LEVEL = symbol_address("thread_dispatch_disable_level")
STOP = 0x3C                            # an address in the vector area that no call returns to
STACK_SIZE = 0x10000
W, H = 80, 16


def ext_syms():
    syms = {}
    elf_nm = os.popen(f"/opt/homebrew/opt/llvm/bin/llvm-nm {REPO}/analysis/build/fw/ext/ext.elf").read()
    for l in elf_nm.splitlines():
        p = l.split()
        if len(p) == 3:
            syms[p[2]] = int(p[0], 16)
    return syms


class Calls:
    """Calls into the paused firmware. Memory for the test (buffers and a 64 KB stack) comes from the firmware's
    own malloc, since the heap spans the rest of RAM."""
    def __init__(self, b):
        self.b, self.uc = b, b.uc
        self.uc.hook_add(UC_HOOK_CODE, lambda u, a, s, d: u.emu_stop(), begin=STOP, end=STOP)
        self.stack = None
        sp = self.uc.reg_read(UC_PPC_REG_1)
        self.stack = self.call(symbol_address("malloc"), STACK_SIZE, sp=(sp - 0x800) & ~15) + STACK_SIZE - 64

    def alloc(self, data):
        a = self.call(symbol_address("malloc"), len(data) + 16)
        assert a, "malloc failed"
        self.uc.mem_write(a, bytes(data))
        return a

    def call(self, addr, *args, sp=None):
        """Calls a function in the paused system. The firmware was stopped at an arbitrary instruction, so every
        register is saved and restored around the call."""
        uc = self.uc
        saved = {r: uc.reg_read(r) for r in ALL_REGS}
        # the call runs in whatever context the system paused in: keep RTEMS from switching tasks under it
        # (the firmware itself draws text from the display task)
        level = struct.unpack(">I", bytes(uc.mem_read(DISPATCH_LEVEL, 4)))[0]
        uc.mem_write(DISPATCH_LEVEL, struct.pack(">I", level + 1))
        sp = sp if sp is not None else self.stack
        for i, v in enumerate(args[:8]):                   # EABI: r3..r10, then the caller's frame from sp + 8
            uc.reg_write(UC_PPC_REG_3 + i, v)
        for i, v in enumerate(args[8:]):
            uc.mem_write(sp + 8 + 4 * i, struct.pack(">I", v))
        uc.reg_write(UC_PPC_REG_1, sp)
        uc.reg_write(UC_PPC_REG_LR, STOP)
        try:
            uc.emu_start(addr, STOP, count=20_000_000)
        except Exception as e:
            pc = uc.reg_read(UC_PPC_REG_PC)
            raise SystemExit(f"call {addr:#x} failed at pc {pc:#x} ({bytes(uc.mem_read(pc, 4)).hex()}): {e}")
        r = uc.reg_read(UC_PPC_REG_3)
        uc.mem_write(DISPATCH_LEVEL, struct.pack(">I", level))
        for k, v in saved.items():
            uc.reg_write(k, v)
        return r

    def frame(self, w=None, h=None):
        w, h = w or W, h or H
        pix = self.alloc(bytes(w * h * 2))
        return self.alloc(struct.pack(">IIIII", 1, w, h, pix, 0)), pix

    def pixels(self, pix, w=None, h=None):
        w, h = w or W, h or H
        return list(struct.unpack(f">{w * h}H", bytes(self.uc.mem_read(pix, w * h * 2))))

    def sbuf(self, data):
        d = self.alloc(data + b"\0")
        return self.alloc(struct.pack(">IHH", d, len(data) + 1, len(data)))


def blend(p, d):
    """The format-9 pixel rule, independent of the firmware code."""
    if not p & 0x8000:
        return p
    a = (p >> 12) & 7
    if not a:
        return d
    a = a * 4 + 3
    ch = lambda s, dd: (a * (s << 1 | 1) + (31 - a) * dd) // 31
    return ch((p >> 8) & 0xF, (d >> 10) & 0x1F) << 10 | ch((p >> 4) & 0xF, (d >> 5) & 0x1F) << 5 | ch(p & 0xF, d & 0x1F)


def main():
    flash = sys.argv[1] if len(sys.argv) > 1 else os.path.join(REPO, "bootloader", "build", "bootflash-fw.bin")
    app = open(os.path.join(REPO, "analysis", "build", "fw", "ELITE_RGB-ext"), "rb").read()
    b, bad = flashtest.boot(open(flash, "rb").read(), app, 400_000_000)   # all fonts loaded by then
    assert not bad, "the boot loader did not load the extended app"
    S, X = appsim.sym(), ext_syms()
    c = Calls(b)
    r32 = lambda a: struct.unpack(">I", bytes(c.uc.mem_read(a, 4)))[0]
    ok = True

    def check(cond, what):
        nonlocal ok
        print(("ok   " if cond else "FAIL ") + what)
        ok &= bool(cond)

    nfonts = c.uc.mem_read(symbol_address("font_count"), 1)[0]
    font = c.alloc(bytes(0x718))
    pick = next(n for n in range(nfonts) if (c.call(S["font_get"], font, n), r32(font) == 2)[1])
    fh = r32(font + 12) >> 4                       # 16x16 grid: cell height = image height / 16
    print(f"{nfonts} fonts loaded; using font {pick} (16x16 grid, cells {r32(font + 8) >> 4}x{fh})")

    # 1. no emoji: the original path
    plain = c.sbuf(b"JACKPOT 1,234")
    st = X["emoji_state"]
    check(c.call(S["text_width"], font, plain) == c.call(X["text_width__orig"], font, plain) and r32(st) == 0,
          "ASCII text: text_width equals the original, and nothing is loaded")
    latin = c.sbuf(b"caf\xe9 \xa3 \xb0")
    check(c.call(S["text_width"], font, latin) == c.call(X["text_width__orig"], font, latin),
          "Latin-1 text (high bytes, no emoji): text_width equals the original")

    # 2. the first emoji loads the sheet
    emojis = native_emoji.available()
    cherry = native_emoji.key("🍒")
    text = b"A" + cherry.encode("utf-8") + b"B"
    t = c.sbuf(text)
    w = c.call(S["text_width"], font, t)
    es, count, pal, pix, mp, ml = (r32(st + o) for o in (4, 8, 12, 16, 20, 24))
    with open(os.path.join(REPO, "bootloader", "build", "emoji", "emoji.bin"), "rb") as f:
        size_f, palette_f, emojis_f, cells_f = native_emoji.unpack(f.read())
    check(r32(st) == 1 and (es, count) == (size_f, len(emojis_f)) and pal and pix and mp,
          f"emoji.bin unpacked from the boot flash: {count} cells of {es}x{es} (expected {len(emojis_f)} of "
          f"{size_f}x{size_f})")
    map_bytes = "".join(e + "\n" for e in emojis_f).encode()
    check(bytes(c.uc.mem_read(mp, ml)) == map_bytes
          and bytes(c.uc.mem_read(pix, count * es * es)) == b"".join(cells_f)
          and struct.unpack(f">{len(palette_f)}H", bytes(c.uc.mem_read(pal, len(palette_f) * 2))) == palette_f,
          f"the unpacked map ({ml} bytes), pixels and palette equal the file's")

    # 3. "A<emoji>B" with every loaded 16x16-grid font and each of the three drawers
    cell = emojis_f.index(cherry)
    EFF = 0x7FE00000                                              # effect word: yellow on black
    COL = c.alloc(struct.pack(">HH", 0x7FE0, 0x0000))
    shots = []
    for n_font in range(nfonts):
        c.call(S["font_get"], font, n_font)
        if r32(font) != 2:
            continue
        fh = r32(font + 12) >> 4
        fw, fh_box = 160, max(H, fh)
        widths = c.uc.mem_read(font + 24 + 256, 256)              # font->width[] (after layout, image)
        props = c.uc.mem_read(font + 24 + 768, 256)               # font->proportional[]
        expect, prev = 0, 1
        for wd, pr in ((widths[ord("A")], props[ord("A")]), (es, 1), (widths[ord("B")], props[ord("B")])):
            expect += (prev == 0) + wd + 1 if pr else wd
            prev = pr
        w = c.call(S["text_width"], font, t)
        xa = 2
        # one separator column between "A" and the emoji either way (A's background column if A is
        # proportional, else the black column before the emoji, a proportional glyph); then the emoji's own
        xe = xa + widths[ord("A")] + 1
        xb = xe + es + 1
        top = int((fh - es) / 2)                                  # drawn 1:1, centred on the line (C division)
        model = []
        for y in range(fh_box):
            for x in range(xe, xe + es):
                dy, dx = y - top, x - xe
                v = cells_f[cell][dy * es + dx] if 0 <= dy < es else 0
                model.append(palette_f[v] if v else None)          # transparent / outside: not checked
        cols = lambda img, x0, x1: [img[y * fw + x] for y in range(fh_box) for x in range(x0, min(x1, fw))]
        for variant in ("plain", "colours", "clip"):
            def draw(fn, text, x):
                d, p = c.frame(fw, fh_box)
                at, wo = c.alloc(struct.pack(">ii", x, 0)), c.alloc(bytes(4))
                if variant == "plain":
                    c.call(fn("text_draw_proportional_plain"), d, at, font, text, wo)
                elif variant == "colours":
                    c.call(fn("text_draw_proportional"), d, at, font, text, COL, wo)
                else:
                    origin = c.alloc(struct.pack(">ii", 0, 0))
                    effects = c.alloc(struct.pack(">8I", *[EFF] * 8))
                    c.call(fn("text_draw_proportional_clip_maybe"), d, at, origin, origin, 0, font, text,
                           effects, 8, 0, wo)
                return c.pixels(p, fw, fh_box), struct.unpack(">h", bytes(c.uc.mem_read(wo, 2)))[0]
            got, width_drawn = draw(lambda name: S[name], t, xa)
            a_ref, _ = draw(lambda name: X[name + "__orig"], c.sbuf(b"A"), xa)
            b_ref, _ = draw(lambda name: X[name + "__orig"], c.sbuf(b"B"), xb)
            em = cols(got, xe, xe + es)
            good = (width_drawn == w == expect
                    and cols(got, 0, xe) == cols(a_ref, 0, xe)
                    and cols(got, xb, fw) == cols(b_ref, xb, fw)
                    and all(m is None or m == g for m, g in zip(model, em))
                    and any(em))
            check(good, f"font {n_font} ({r32(font + 8) >> 4}x{fh}), {variant}: width {width_drawn} "
                        f"(text_width {w}, rule {expect}); letters as the original; emoji as the model")
            if variant == "plain":
                shots.append(bytes(struct.pack(f">{fw * fh_box}H", *got)))
                studio_font = os.path.join(paths.CONTENT, "fonts", f"fonte{n_font:02d}.png")
                if n_font and os.path.exists(studio_font):         # the studio's preview of the same text
                    ft = _fwfont(f"fonte{n_font:02d}.png")
                    glyphs = native_emoji.parse(text)
                    _, studio_w = native_emoji.layout(ft, glyphs)
                    studio_em = [None] * (es * fh_box)
                    for dx, dy, p9 in native_emoji.pixels(cell, ft.ch):
                        if 0 <= dy < fh_box:
                            studio_em[dy * es + dx] = p9
                    same_px = all(m is None or m == g for m, g in zip(studio_em, em))
                    check(ft.ch == fh and studio_w == width_drawn and same_px,
                          f"font {n_font}: the studio preview (marquee_studio.native_emoji) matches the firmware: "
                          f"width {studio_w}, emoji pixels")
    for i, shot in enumerate(shots):
        h_ = len(shot) // 2 // 160
        appsim.save_frames([shot], os.path.join(REPO, "sim", "out", f"native-emoji-{i}"), W=160, H=h_)
    print("RESULT:", "PASS" if ok else "FAIL")
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
