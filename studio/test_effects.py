#!/usr/bin/env python3
"""Check the studio's text effects (effect words FG<<16 | BG, and the outline) against the firmware itself
(ELITE_RGB under Unicorn), pixel for pixel, frame by frame.

How the firmware is driven:
- The effect slots are loaded as at boot: the real effectNN.flc files go into an LFS volume in emulated memory
  (lfs_volume_set / lfs_volume_enable, volume 2) and the firmware's own effects_load_slots (0x406DC) opens all 100
  slots with flc_open. The fonts are built by the firmware's font_build, as in test_markup.py.
- Each panel frame sets ledpanel_frame_count (the phase source) and draws into a frame that starts from a fixed
  non-black pattern, so pixels left alone ("transparent") show.
- A. Text items: what display_main_loop does for a pool's text: the text drawn into a panel-sized mask by
  text_draw_proportional_plain; glyph_blit_effect (0x40820) of an empty mask with the whole word over the
  window (the background); glyph_outline_blit (0x3FF0C); glyph_blit_effect with the BG half forced to 0x8000.
  Against sign_emu.draw_text_item, with the mask drawn by sign_emu.FwFont.
- B. Single glyphs with font_draw_char_effect (0x43118): explicit FG/BG texture origins and a phase per
  character (the only way to get a non-zero phase), against sign_emu.Effects.draw_char.
- C. The studio's message renderer: core.render_message555 frames against the firmware drawing each frame the
  same way (A), at the text positions the studio chose (core.message_layout).

Usage: ../venv/bin/python test_effects.py        (needs analysis/extracted/ELITE_RGB and unicorn)
"""
import os, struct, sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
sys.path.insert(0, os.path.join(HERE, "..", "analysis", "tools"))
import numpy as np  # noqa: E402
import core  # noqa: E402
from marquee_studio import sign_emu as emu  # noqa: E402
import fw  # noqa: E402  (read-only use of the analysis harness)
from unicorn import UC_HOOK_CODE  # noqa: E402
from unicorn.ppc_const import UC_PPC_REG_3, UC_PPC_REG_LR, UC_PPC_REG_PC  # noqa: E402

FONT_REC = 0x718
ANIMS = os.path.join(core.CONTENT, "animations")


def lfs_volume(files):
    """An LFS volume as lfs_find_in_volume walks it: per entry u32 entry_len, u32 size, u32 flags, u8 name_len,
    name, data at +0xC + ((name_len + 4) & ~3), a 4-byte check word; a 0 word ends the chain."""
    out = bytearray()
    for name, data in files:
        nb = name.encode("latin1")
        head = struct.pack(">III", 0, len(data), 0) + bytes([len(nb)]) + nb
        head = head.ljust(0xC + ((len(nb) + 4) & ~3), b"\0")
        e = head + data + b"\0" * 4
        e += b"\0" * (-len(e) % 4)
        out += struct.pack(">I", len(e)) + e[4:]
    return bytes(out + b"\0" * 16)


class FwEffects:
    """The firmware's effect code on a W x H panel, with the studio's fonts and effect slots."""
    def __init__(self, fonts, slots, W=80, H=16):
        self.f, self.W, self.H = fw.FW(), W, H
        f = self.f
        a = fw.SYM["malloc"]                                     # RTEMS malloc needs a heap: hand out scratch
        f.uc.hook_add(UC_HOOK_CODE, self._malloc, begin=a, end=a)
        f.stub("free", 0)
        word = next(m[1] for m in core.DISPLAY_MODELS if m[3:] == (W, H))
        f.u16(fw.SYM["display_config_word"], word)
        self.plain_rgb = f.call("palette_uses_plain_rgb")
        self.fonts = []
        for i, ft in enumerate(fonts):
            pix = f.alloc(ft.w * ft.h * 2)
            f.write(pix, (ft.ink.astype(np.uint16) * 0x7FFF).astype(">u2").tobytes())
            img = f.alloc(20)
            f.write(img, struct.pack(">IIIII", 1, ft.w, ft.h, pix, 0))
            f.call("font_build", fw.SYM["font_table"] + i * FONT_REC, 2, img)
            self.fonts.append(fw.SYM["font_table"] + i * FONT_REC)
        vol = lfs_volume([(f"effect{n:02d}.flc", d) for n, d in sorted(slots.items())])
        base = f.alloc(len(vol), 16)
        f.write(base, vol)
        f.call("lfs_volume_set", 2, base, len(vol))
        f.call("lfs_volume_enable", 2, 1)
        f.call("effects_load_slots")
        self.loaded = [n for n in range(100) if f.u32(fw.SYM["effect_slots"] + 4 * n)]
        self.frame_pix, self.mask_pix = f.alloc(W * H * 2), f.alloc(W * H * 2)
        self.frame_img, self.mask_img = f.alloc(20), f.alloc(20)
        f.write(self.frame_img, struct.pack(">IIIII", 1, W, H, self.frame_pix, 0))
        f.write(self.mask_img, struct.pack(">IIIII", 1, W, H, self.mask_pix, 0))
        self.args = f.alloc(0x100)

    def _malloc(self, uc, addr, size, _):
        n = uc.reg_read(UC_PPC_REG_3)
        uc.reg_write(UC_PPC_REG_3, self.f.alloc(max(n, 4), 8))
        uc.reg_write(UC_PPC_REG_PC, uc.reg_read(UC_PPC_REG_LR))

    def set_frame(self, fc, pixels):
        self.f.u32(fw.SYM["ledpanel_frame_count"], fc)
        self.f.write(self.frame_pix, pixels.astype(">u2").tobytes())

    def frame(self):
        return np.frombuffer(self.f.read(self.frame_pix, self.W * self.H * 2), ">u2").reshape(self.H, self.W).astype(np.uint16)

    def _pt(self, off, *v):
        a = self.args + off
        self.f.write(a, struct.pack(">" + "i" * len(v), *v))
        return a

    def text_item(self, word, font, text, pos, window, style=0, colour=0):
        """One text item, as display_main_loop draws a pool's text (see the module notes). pos None: no text
        (flashed off), only the background."""
        f, W, H = self.f, self.W, self.H
        f.write(self.mask_pix, bytes(W * H * 2))
        win = self._pt(0, *window)
        f.call("glyph_blit_effect", self.mask_img, self.frame_img, word, win, win)
        if pos is not None:
            sb = f.sbuf(text, cap=256)
            f.call("text_draw_proportional_plain", self.mask_img, self._pt(0x10, *pos), self.fonts[font], sb,
                   self.args + 0x18)
        win = self._pt(0, *window)
        f.call("glyph_outline_blit", self.mask_img, win, self.frame_img, self._pt(0x20, *window[:2]),
               self._pt(0x28, style, colour << 16), self._pt(0x30, 0, 0, W, H))
        win = self._pt(0, *window)
        f.call("glyph_blit_effect", self.mask_img, self.frame_img, (word & 0xFFFF0000) | 0x8000, win, win)

    def char(self, word, font, ch, pos, fg_origin, bg_origin, phase, part):
        """font_draw_char_effect (9 arguments: the effect word goes on the stack)."""
        f = self.f
        f.u32(fw.STACK + 8, word)
        f.call("font_draw_char_effect", self.frame_img, self._pt(0x40, *pos), self._pt(0x48, *fg_origin),
               self._pt(0x50, *bg_origin), self.fonts[font], ch, phase, self._pt(0x58, *part))


ok = True


def check(name, cond, got=""):
    global ok
    ok &= bool(cond)
    print(f"  {'ok  ' if cond else 'FAIL'} {name}" + (f"  (got {got})" if not cond else ""))


def background(W, H, k=0):
    """The frame the item is drawn over: a fixed non-black pattern, different each frame."""
    y, x = np.mgrid[0:H, 0:W]
    return (((x * 7 + y * 131 + k * 17) & 0x7FFF) | 0x0421).astype(np.uint16)


def first_diff(a, b):
    ys, xs = np.nonzero(a != b)
    return f"first at x={xs[0]} y={ys[0]}: firmware {a[ys[0], xs[0]]:#06x} studio {b[ys[0], xs[0]]:#06x}, " \
           f"{len(xs)} px differ" if len(xs) else ""


class Pair:
    """The firmware and the emulator side by side, with one shared panel frame counter."""
    def __init__(self, fonts, slots, W=80, H=16, fc=1):
        self.sign = FwEffects(fonts, slots, W, H)
        self.fx = emu.Effects(slots, fc)
        self.fonts, self.W, self.H, self.fc = fonts, W, H, fc

    def item_run(self, word, font, text, frames, window=None, style=0, colour=0, move=(0, 0), start=None, step=1):
        """Draw the item for `frames` panel frames (the counter advancing by `step`), moving the text by `move`
        per frame. Returns None if every frame matched, else a description of the first difference."""
        W, H = self.W, self.H
        ft = self.fonts[font]
        data = text.encode("latin1")
        window = window or (0, 0, W, H)
        x, y = start or (emu.half(W - ft.text_width(data)), emu.half(H - ft.ch))
        for k in range(frames):
            self.sign.set_frame(self.fc, background(W, H, k))
            self.sign.text_item(word, font, data, (x, y), window, style, colour)
            a = self.sign.frame()
            self.fx.frame_count = self.fc
            b = background(W, H, k)
            mask = np.zeros((H, W), np.uint16)
            ft.draw(mask, x, y, data, 0x7FFF, 0)
            emu.draw_text_item(self.fx, b, mask, word, window, style, colour)
            if not (a == b).all():
                return f"frame {k} (count {self.fc}): {first_diff(a, b)}"
            self.fc = (self.fc + step) & 0xFFFFFFFF
            x, y = x + move[0], y + move[1]
        return None

    def char_run(self, word, font, text, frames, pos, fg_origin, bg_origin, phase=0, move=(0, 0)):
        """Each character with font_draw_char_effect, trimmed to its ink as text_draw_proportional trims it, the
        phase counting up by one per character."""
        W, H = self.W, self.H
        ft = self.fonts[font]
        for k in range(frames):
            self.sign.set_frame(self.fc, background(W, H, k))
            self.fx.frame_count = self.fc
            b = background(W, H, k)
            x = pos[0] + k * move[0]
            for i, c in enumerate(text.encode("latin1")):
                part = (ft.left[c], 0, ft.width[c], ft.ch)
                p = (x, pos[1] + k * move[1])
                self.sign.char(word, font, c, p, fg_origin, bg_origin, phase + i, part)
                self.fx.draw_char(b, p, fg_origin, bg_origin, ft, c, phase + i, part, word)
                x += ft.width[c] + 1
            a = self.sign.frame()
            if not (a == b).all():
                return f"frame {k} (count {self.fc}): {first_diff(a, b)}"
            self.fc = (self.fc + 1) & 0xFFFFFFFF
        return None


def slots_from(folder, names):
    return {int(n[6:8]): open(os.path.join(folder, n), "rb").read() for n in names}


FG = {"amber": 0x7E80, "transparent 0x8000": 0x8000, "RGY 0x8001": 0x8001, "stripes 0x8002": 0x8002,
      "cycle 0xBFFE": 0xBFFE, "cycle 0xBFFF": 0xBFFF, "slot 3 (80x16)": 0xC003, "slot 11 (2x60)": 0xC00B,
      "slot 14 (2x16, 1 frame)": 0xC00E, "slot 17 (16x16, SS2)": 0xC011, "slot 25 (not loaded)": 0xC019,
      "slot 100 (out of range)": 0xC064, "0x8003 (unknown)": 0x8003}
BG = {"black": 0x0000, "blue": 0x001F, "transparent 0x8000": 0x8000, "RGY 0x8001": 0x8001,
      "stripes 0x8002": 0x8002, "cycle 0xBFFE": 0xBFFE, "cycle 0xBFFF": 0xBFFF, "slot 3": 0xC003,
      "slot 11": 0xC00B, "slot 17": 0xC011, "0x8003 (unknown)": 0x8003}


STUDIO_SPECS = [
    {"text": "WIN", "font": "fonte04.png", "mode": "static", "colour": "Amber"},
    {"text": "JACKPOT 1000", "font": "fonte04.png", "foreground": "effect03.flc", "bg_colour": "Blue",
     "outline": "White", "text_rate": 3},
    {"text": "8888", "font": "fonte04.png", "mode": "static", "foreground": "effect17.flc",
     "background": "effect11.flc", "outline": "Red", "outline_style": "Thin"},
    {"text": "GOOD LUCK", "font": "fonte02.png", "foreground": "Red/green/yellow wipe", "background": "Transparent",
     "outline": "Amber", "outline_style": "Drop Shadow", "text_rate": 5},
    {"text": "$5", "font": "fonte06.png", "mode": "static", "colour": "Cyan", "background": "Red/green/yellow wipe",
     "flash_on": 10, "flash_off": 6},
    {"text": "BONUS", "font": "fonte04.png", "mode": "static", "foreground": "0x8002", "background": "0xBFFF",
     "frame0": 3},
    {"text": "LOGO", "font": "fonte03.png", "mode": "static", "colour": "Yellow", "background": "effect00.flc"},
    {"text": "BIG", "font": "fonte08.png", "mode": "static", "foreground": "effect08.flc", "outline": "Green"},
]


def texture(sign, fn, *args):
    """Call a texture builder (effect_texture_8001 / _8002) and read back the image it describes."""
    f = sign.f
    out = sign.args + 0x80
    f.call(fn, out, *args)
    _, w, h, pix, _ = struct.unpack(">IIIII", f.read(out + 8, 20))
    return np.frombuffer(f.read(pix, w * h * 2), ">u2").reshape(h, w).astype(np.uint16)


def main():
    # the sign's fonts, then the original content's stock fonts our image leaves out (firmware/stock_fonts.txt): this
    # checks the firmware's drawing code, which draws any of them
    fonts = core.sign_fonts()
    from marquee_studio import sign_emu as _emu
    _have = {f.name for f in fonts}
    from marquee_studio import paths as _paths
    _d = os.path.join(_paths.CONTENT, "fonts")
    fonts += [_emu.FwFont(os.path.join(_d, n), name=n) for n in sorted(os.listdir(_d))
              if n.startswith("fonte") and n.endswith(".png") and n[5:7].isdigit() and n not in _have]
    names = [f.name for f in fonts]
    F04, F01 = names.index("fonte04.png"), names.index("fonte01.png")
    slots = slots_from(ANIMS, sorted(n for n in os.listdir(ANIMS) if n[:6] == "effect" and n[6:8].isdigit()))
    pair = Pair(fonts, slots, fc=1)
    sign = pair.sign
    print(f"fonts {', '.join(names)}; slots loaded by effects_load_slots: {sign.loaded}")
    check("every effectNN.flc loaded into its slot", sign.loaded == sorted(slots), sign.loaded)
    check("palette_uses_plain_rgb = 1 (8-bit palettes convert by >> 3)", sign.plain_rgb == 1, sign.plain_rgb)

    print("1. the generated textures (effect_texture_8001 / _8002, called directly)")
    for fc in (0, 1, 7, 63, 64, 200, 1000, 0x7FFFFFF0):
        pair.sign.f.u32(fw.SYM["ledpanel_frame_count"], fc)
        pair.fx.frame_count = fc
        for h in (1, 10, 16, 32):
            check(f"0x8001, height {h}, frame count {fc:#x}",
                  (texture(sign, "effect_texture_8001", h, 2) == pair.fx.texture_8001(h)).all())
        for n in (0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 0x304):
            check(f"0x8002 builder with {n:#x} ({min(max(n & 0xFF, 1), 8)} stripes each), frame count {fc:#x}",
                  (texture(sign, "effect_texture_8002", n, 1) == pair.fx.texture_8002(n)).all())

    print("2. text items: every FG x BG pair (window (5, 1, 60, 14), text moving left 3 px a frame)")
    for fn, fg in FG.items():
        for bn, bg in BG.items():
            r = pair.item_run(fg << 16 | bg, F04, "WIN 8", 6, window=(5, 1, 60, 14), move=(-3, 0))
            check(f"FG {fn} / BG {bn}: {fg << 16 | bg:#010x}", r is None, r)

    print("3. timing: the frame counter drives the phase")
    pair.fc = 1                       # from power-up: 0x8002's unsigned index has not wrapped yet
    for name, word, n in (("stripes from power-up", 0x80028000, 60), ("RGY wipe", 0x80010000, 220),
                          ("cycle fwd/back", 0xBFFEBFFF, 120)):
        r = pair.item_run(word, F04, "8888", n)
        check(f"{name}: {word:#010x}, {n} frames", r is None, r)
    for n, frames in ((3, 200), (11, 400), (14, 20), (17, 100), (8, 120)):
        fl = emu.FLIC(slots[n])
        hold = emu.flc_speed_to_hold(fl.speed_ms)
        r = pair.item_run(0xC000 | n << 16 | 0xC000 | n, F04, "JACKPOT", frames)
        check(f"slot {n} as FG and BG: {fl.frames} frames x {hold + 1} panel frames, {frames} panel frames "
              f"(several loops)", r is None, r)
        r = pair.item_run((0xC000 | n) << 16, F04, "JACKPOT", 40, step=5)
        check(f"slot {n}, counter stepping by 5 (frames skipped)", r is None, r)
        r = pair.item_run((0xC000 | n) << 16, F04, "JACKPOT", 12, step=9)
        check(f"slot {n}, counter stepping by 9 (more than 8: the slot restarts every frame)", r is None, r)
    pair.fc = 0xFFFFFFF0
    r = pair.item_run(0xC0038001, F04, "WRAP", 40)
    check("frame counter wrapping through 0 (slot 3 FG, 0x8001 BG)", r is None, r)
    pair.fc = 5000

    print("4. texture origin: fixed to the panel, the text moving through it")
    for word in (0xC0110000, 0x80020000, 0x8001C003, 0xC00B8002):
        for move in ((-2, 0), (1, 1), (0, -1)):
            for window in ((0, 0, 80, 16), (13, 3, 41, 9)):
                r = pair.item_run(word, F01, "Abc 123", 8, window=window, move=move, start=(30, 2))
                check(f"{word:#010x} window {window} moving {move}", r is None, r)
    r = pair.item_run(0xC0030000, F04, "OFF THE EDGE", 30, move=(-5, 0), start=(70, 0))
    check("text running off the left edge", r is None, r)

    print("5. outline: every style and colour")
    for style in range(5):
        for colour in range(9):
            for font, start, window in ((F01, (2, 3), (0, 0, 80, 16)), (F04, (0, 0), (0, 0, 80, 16)),
                                        (F01, (60, 9), (4, 2, 70, 12))):
                r = pair.item_run(0xC0110000, font, "Ag|_.W", 2, window, style, colour, start=start)
                check(f"style {style} ({(emu.OUTLINE_STYLES + ('?',))[min(style, 4)]}) colour {colour} "
                      f"{names[font]} at {start} window {window}", r is None, r)
    for word in (0x7E800000, 0x8000001F, 0x80018002, 0xBFFEC003, 0xC0038000):
        r = pair.item_run(word, F04, "OUT", 4, style=1, colour=7, move=(-1, 0))
        check(f"thick white outline with {word:#010x}", r is None, r)

    print("6. single glyphs (font_draw_char_effect): phase per character, explicit texture origins")
    for word in (0xBFFEBFFF, 0xBFFF0000, 0x8001001F, 0xC0118002, 0x8002C00B, 0xC003C011, 0x7C008000,
                 0x80007C00, 0x8003001F):
        for fg_o, bg_o in (((0, 0), (0, 0)), ((10, 3), (-7, 2)), ((500, -900), (77, 33))):
            for phase in (0, 5, 0xFFFE):
                r = pair.char_run(word, F01, "AB8$", 4, (20, 3), fg_o, bg_o, phase, move=(1, 0))
                check(f"{word:#010x} origins {fg_o} {bg_o} phase {phase:#x}", r is None, r)
    pair.fc = 3
    r = pair.char_run(0xBFFFBFFE, F04, "123456789", 4, (0, 0), (0, 0), (0, 0), 0)
    check("0xBFFF / 0xBFFE with the phase above frame_count >> 4 (unsigned wrap)", r is None, r)

    print("7. the studio's renderer (core.render_message555) == the firmware drawing the same frames")
    for spec in STUDIO_SPECS:
        L = core.message_layout(spec, 80, 16)
        ours = core.render_message555(spec, 80, 16)
        sign = FwEffects(fonts, L["slots"])
        font = names.index(L["font"].name)
        bad = None
        for k, (fc, p) in enumerate(L["frames"]):
            sign.set_frame(fc, np.zeros((16, 80), np.uint16))
            sign.text_item(L["word"], font, L["data"], p, L["window"], L["style"], L["colour"])
            a = sign.frame()
            if not (a == ours[k]).all():
                bad = f"frame {k} (count {fc}): {first_diff(a, ours[k])}"
                break
        desc = ", ".join(f"{k}={v}" for k, v in spec.items() if k != "text")
        check(f"{spec['text']!r} {desc}: word {L['word']:#010x}, {len(ours)} frames", bad is None, bad)

    print("\nALL STUDIO EFFECT CHECKS PASS" if ok else "\nFAILURES")
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
