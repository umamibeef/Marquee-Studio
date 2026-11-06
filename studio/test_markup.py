#!/usr/bin/env python3
"""Check the studio's message markup against the firmware itself (ELITE_RGB under Unicorn).

1. encode_markup / decode_markup round trip.
2. Font metrics: sign_emu.FwFont against the firmware's font_build (0x448D4) on every sign font.
3. Meaning: each studio description, encoded, run through the real textline (textline_init, textline_step,
   message_text_next_token). At each new segment the textline fields (text, colours, entry, exit, hold,
   speeds, font, proportional) must be what the description asked for.
4. Pixels: the emulator (sign_emu.TextLine) against the firmware, frame by frame, for the descriptions above and
   for raw lines that hit the firmware's quirks (hold left at 0xFFFFFFFF, ESC 'C' with r24 set, includes,
   unknown commands, a font taller than the panel).

Usage: ../venv/bin/python test_markup.py        (needs analysis/extracted/ELITE_RGB and unicorn)
"""
import os, struct, sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
sys.path.insert(0, os.path.join(HERE, "..", "analysis", "tools"))
import numpy as np  # noqa: E402
import core  # noqa: E402
from marquee_studio import sign_emu as emu  # noqa: E402
import fw  # noqa: E402  (read-only use of the analysis harness)
from unicorn.ppc_const import UC_PPC_REG_24  # noqa: E402

# struct textline offsets (symbols.tsv; as in analysis/tools/test_message_markup.py)
O = dict(state=0, line=4, depth=5, entry_dir=0x18, exit_dir=0x1C, entry_speed=0x20, exit_speed=0x24, hold=0x28,
         text=0x2C, buf=0x34, pos=0x13C, text_rect=0x144, line_img=0x154, text_img=0x168, font=0x17C,
         proportional=0x794, fg=0x798, bg=0x79A, sl=0x79C)
FONT_REC = 0x718


class FwSign:
    """The firmware's text line on a W x H box, with the studio's fonts built by the firmware's font_build."""
    def __init__(self, fonts, W=80, H=16):
        self.f, self.W, self.H = fw.FW(), W, H
        f = self.f
        self.font_pix = []
        for i, ft in enumerate(fonts):
            pix = f.alloc(ft.w * ft.h * 2)
            f.write(pix, (ft.ink.astype(np.uint16) * 0x7FFF).astype(">u2").tobytes())
            img = f.alloc(20)
            f.write(img, struct.pack(">IIIII", 1, ft.w, ft.h, pix, 0))
            f.call("font_build", fw.SYM["font_table"] + i * FONT_REC, 2, img)
            self.font_pix.append(pix)
        f.write(fw.SYM["font_count"], bytes([len(fonts)]))
        word = next(m[1] for m in core.DISPLAY_MODELS if m[3:] == (W, H))
        f.u16(fw.SYM["display_config_word"], word)              # display_width/height: the model's size
        self.table = f.alloc(256 * 256)
        f.u32(fw.SYM["message_table_ptr"], self.table)
        self.tl = f.alloc(0x7B8)
        self.lp, self.tp = f.alloc(W * H * 2), f.alloc(emu.TEXT_IMG_W * H * 2)
        self.box = f.alloc(16)

    def font_tables(self, i):
        rec = self.f.read(fw.SYM["font_table"] + i * FONT_REC, 0x418)
        return list(rec[0x18:0x118]), list(rec[0x118:0x218]), list(rec[0x318:0x418])

    def start(self, lines, r24=0):
        f, tl, W, H = self.f, self.tl, self.W, self.H
        f.write(self.table, bytes(256 * 256))
        for n, data in lines.items():
            f.write(self.table + n * 256, data)
        f.write(0x3C2134, bytes(17 * 0x118))                    # message_line_state: read positions
        f.write(tl, bytes(0x7B8))
        f.write(tl + O["line_img"], struct.pack(">IIIII", 1, W, H, self.lp, 0))
        f.write(tl + O["text_img"], struct.pack(">IIIII", 1, emu.TEXT_IMG_W, H, self.tp, 0))
        f.write(self.box, struct.pack(">iiii", 0, 0, W, H))
        f.call("textline_init", tl, self.box, 0, 0, 0)
        f.uc.reg_write(UC_PPC_REG_24, r24)                       # what the caller leaves in r24

    def step(self):
        return self.f.call("textline_step", self.tl, 0)

    def u(self, k, fmt=">I"):
        return struct.unpack(fmt, self.f.read(self.tl + O[k], struct.calcsize(fmt)))[0]

    def fields(self):
        text = self.f.read(self.tl + O["buf"], struct.unpack(">IHH", self.f.read(self.tl + O["text"], 8))[2])
        pix = struct.unpack(">IIIIII", self.f.read(self.tl + O["font"], 24))[4]
        sl = struct.unpack(">IIiiii", self.f.read(self.tl + O["sl"], 24))
        return {"text": text, "fg": self.u("fg", ">H"), "bg": self.u("bg", ">H"), "entry_dir": self.u("entry_dir"),
                "entry_speed": self.u("entry_speed"), "exit_dir": self.u("exit_dir"),
                "exit_speed": self.u("exit_speed"), "hold": self.u("hold"),
                "font": self.font_pix.index(pix) if pix in self.font_pix else None,
                "proportional": self.u("proportional"), "slide_from": sl[2:4]}

    def pick_for_text(self, data, w, h):
        """The firmware's font_pick_for_text on the whole text in a w x h box, as display_main_loop calls it for
        record text: (font index, width the window is compared with)."""
        f = self.f
        sb, fo, box, r4, r5, pt = f.sbuf(data, cap=256), f.alloc(0x618), f.alloc(16), f.alloc(16), f.alloc(16), f.alloc(8)
        f.write(box, struct.pack(">iiii", 0, 0, w, h))
        f.call("font_pick_for_text", sb, fo, box, r4, r5, pt, 1, 0)
        pix = struct.unpack(">I", f.read(fo + 0x10, 4))[0]
        return self.font_pix.index(pix), struct.unpack(">iiii", f.read(r4, 16))[2]

    def frame(self):
        return np.frombuffer(self.f.read(self.lp, self.W * self.H * 2), ">u2").reshape(self.H, self.W)


ok = True


def check(name, cond, got=""):
    global ok
    ok &= bool(cond)
    print(f"  {'ok  ' if cond else 'FAIL'} {name}" + (f"  (got {got})" if not cond else ""))


GREEN, RED, AMBER, BLUE = 0x03E0, 0x7C00, 0x7E80, 0x001F
DESCRIPTIONS = {
    "plain text": [{"text": "HELLO"}],
    "the doc's example": [
        {"text": "JACKPOT!", "fg": GREEN, "entry": "left", "entry_speed": 80, "hold": 3, "exit": "left",
         "exit_speed": 80, "centre": True},
        {"text": "$1,000", "fg": RED}],
    "colours, fonts, every entry and exit": [
        {"text": "UP", "fg": "Amber", "bg": BLUE, "entry": "bottom", "entry_speed": 60, "hold": 2, "exit": "up",
         "exit_speed": 60, "centre": True, "font": "fonte03.png"},
        {"text": "DOWN", "fg": "Cyan", "bg": 0, "entry": "top", "hold": 1, "exit": "down", "exit_speed": 90},
        {"text": "R", "entry": "right", "entry_speed": 120, "exit": "right", "font": "fonte01.png"},
        {"text": "drop", "entry": "top-left", "entry_speed": 50, "hold": 2, "exit": "left", "centre": False},
        {"text": "scroll", "entry": "left", "entry_speed": 200, "hold": 0, "font": "fonte02.png"},
        {"text": "again", "fg": "White"}],
    "same settings twice (CR between)": [
        {"text": "ONE", "entry_speed": 120}, {"text": "TWO"}, {"text": "THREE", "centre": True, "hold": 1}],
    "back to left-aligned after centred": [
        {"text": "C", "centre": True, "hold": 1, "entry_speed": 100, "exit_speed": 100},
        {"text": "L", "centre": False, "entry_speed": 100}],
    "include": [{"text": "A", "entry_speed": 150}, {"include": 5}, {"text": "C", "fg": GREEN}],
}
LINE5 = [{"text": "B", "fg": BLUE, "entry": "left", "entry_speed": 150}]
# Raw lines for the quirks, compared pixel for pixel only.
RAW = {
    "hold left at 0xFFFFFFFF (segment 2 holds for ever)": (b"\x1b`\xff\x1bP\x01\x78AB\rCD", 0),
    "ESC 'C' with r24 = 0": (b"\x1bC\x50HI", 0),
    "ESC 'C' with r24 = 1 (behaves as ESC 'D')": (b"\x1bC\x50HI", 1),
    "unknown command skipped": (b"\x1bZ\x1b0\x7c\x1fX\x1b\x99Y", 0),
    "nested includes": (b"0\x1b\x80\x011\x1b\x80\x022", 0),
    "font taller than the panel, centred (fits)": (b"\x1b`\x07\x1b`\xff\x1b@\xffBIG", 0),
    "font taller than the panel, left-aligned digits (not drawn)": (b"\x1b`\x07\x1b@\xff123", 0),
    "font taller than the panel, left-aligned letters (firmware hangs)": (b"\x1b`\x07\x1b@\xffBIG", 0),
    "text with every code 0x01..0xFF but CR/ESC": (bytes(c for c in range(1, 256) if c not in (13, 27)), 0),
}
RAW_LINES = {"nested includes": {1: b"a\x1b\x80\x03b", 2: b"c", 3: b"x"}}


def expected_fields(seg, fonts, first, W=80, H=16):
    """The textline fields the description asks for. A centred segment's font is the firmware's own pick on
    its first character (`first`, which is the CR for a segment after a CR)."""
    names = [f.name for f in fonts]
    d = min(core.MARKUP_ENTRY[seg["entry"]] - 0x40, 3)
    font = (emu.font_pick_for_text(fonts, first, *((emu.TEXT_IMG_W, H) if d < 2 else (W, H)))
            if seg["centre"] else names.index(seg["font"]))
    return {"text": core._text_bytes(seg["text"], seg["font"] or names[font]), "fg": seg["fg"], "bg": seg["bg"],
            "entry_dir": d, "entry_speed": seg["entry_speed"], "font": font, "proportional": int(seg["centre"]),
            **({"hold": seg["hold"] * 4, "exit_dir": core.MARKUP_EXIT[seg["exit"]] - 0x50,
                "exit_speed": seg["exit_speed"]} if seg["hold"] else {"hold": 0})}


def compare_frames(sign, fonts, lines, r24=0, max_frames=4000):
    """Run firmware and emulator side by side; returns (frames, first differing frame or None, done)."""
    sign.start(lines, r24)
    table = bytearray(256 * 256)
    for n, data in lines.items():
        table[n * 256:n * 256 + len(data)] = data
    tl = emu.TextLine(table, fonts, sign.W, sign.H, r24=r24)
    for i in range(max_frames):
        try:
            a = sign.step()
        except RuntimeError:                                    # did not return: the firmware hangs
            a = -1
        try:
            b = tl.step()
        except emu.FirmwareHang:
            b = -1
        if a == b == -1:
            return i + 1, None, "hang"
        if a != b or not (sign.frame() == tl.img).all():
            return i + 1, i, False
        if a == 4:
            return i + 1, None, True
    return max_frames, None, False


def main():
    # the stock firmware's fonts (FwSign runs its own font_build on them); our MQF fonts are loaded by our firmware's
    # fonts.c, checked against the emulation frame for frame in frontend/packages/sign-emu test/appvm_firmware.test.ts
    # (all eight of the original content, whichever our image keeps: this is the stock firmware's behaviour)
    import re as _re
    from marquee_studio import paths as _paths
    _d = os.path.join(_paths.CONTENT, "fonts")
    fonts = [emu.FwFont(os.path.join(_d, n), name=n) for n in sorted(os.listdir(_d)) if _re.fullmatch(r"fonte\d\d\.png", n)]
    names = [f.name for f in fonts]
    print(f"sign fonts: {', '.join(names)}")
    # the stock firmware has only these fonts and no fonts.auto: its automatic picks choose among all of them
    import marquee_studio.fonts as _fonts
    _fonts.auto_fonts = lambda: set(names)

    print("1. round trip: decode_markup(encode_markup(d)) == markup_normalise(d)")
    for name, d in DESCRIPTIONS.items():
        data = core.encode_markup(d)
        got, want = core.decode_markup(data), core.markup_normalise(d)
        check(f"{name}: {len(data)} bytes", got == want, got)
    for bad in ([{"text": "a\rb"}], [{"text": ""}], [{"text": "x", "entry_speed": 0}], [{"text": "x", "font": "no.png"}],
                [{"text": "€"}], [{"text": "x" * 256}]):
        try:
            core.encode_markup(bad); check(f"rejects {bad!r:.40}", False, "no error")
        except ValueError:
            check(f"rejects {bad!r:.40}", True)

    print("2. font metrics: FwFont == the firmware's font_build")
    sign = FwSign(fonts)
    for i, f in enumerate(fonts):
        left, width, prop = sign.font_tables(i)
        check(f"{f.name} ({f.cw}x{f.ch}): left/width/proportional for 256 codes",
              (left, width, prop) == (f.left, f.width, f.prop))
    from marquee_studio.sign_emu import font_pick_largest
    sign.start({0: b"X"})
    check(f"default font (font_pick_largest) = {core.default_font(16)}",
          sign.fields()["font"] == font_pick_largest(fonts, 16), sign.fields()["font"])

    print("3. what the firmware parses == what the studio meant")
    for name, d in DESCRIPTIONS.items():
        lines = {0: core.encode_markup(d), 5: core.encode_markup(LINE5)}
        want = [s for s in core.markup_normalise(d) if "include" not in s]
        if "include" in name:
            want.insert(1, core.markup_normalise(LINE5)[0])
        sign.start(lines)
        seen, prev = [], 0
        for _ in range(20000):
            st = sign.step()
            if prev == 0 and st == 1:
                seen.append(sign.fields())
            if st == 4:
                break
            prev = st
        check(f"{name}: {len(want)} segments", len(seen) == len(want), len(seen))
        for k, (s, got) in enumerate(zip(want, seen)):
            exp = expected_fields(s, fonts, got["text"][:1])
            text = got["text"][1:] if got["text"][:1] == b"\r" and k else got["text"]
            g = {**{k2: got[k2] for k2 in exp}, "text": text}
            if not s["hold"]:
                g["hold"] = got["hold"]
            check(f"  segment {k + 1} {s['text']!r}: {s['entry']}/{s['exit']} hold {s['hold']} "
                  f"{'centred (' + names[exp['font']] + ')' if s['centre'] else 'left ' + s['font']}", g == exp,
                  {k2: (g[k2], exp[k2]) for k2 in exp if g[k2] != exp[k2]})
            if s["entry"] == "top-left":
                check("  top-left: the text image starts at x = 0", got["slide_from"][0] == 0, got["slide_from"])

    print("4. emulator pixels == firmware pixels, frame by frame (80x16)")
    for name, d in DESCRIPTIONS.items():
        lines = {0: core.encode_markup(d), 5: core.encode_markup(LINE5)}
        n, bad, done = compare_frames(sign, fonts, lines)
        check(f"{name}: {n} frames{' to the end' if done else ''}", bad is None and done, f"differs at frame {bad}")
    for name, (data, r24) in RAW.items():
        n, bad, done = compare_frames(sign, fonts, {0: data, **RAW_LINES.get(name, {})}, r24, max_frames=1500)
        how = {True: " to the end", "hang": ", both hang"}.get(done, "")
        check(f"{name}: {n} frames{how}", bad is None and (done == "hang") == ("hangs" in name), f"differs at frame {bad}")
    print("   other display models")
    for model, W, H in (("AD0402", 160, 32), ("ED0505", 200, 80)):
        big = FwSign(fonts, W, H)
        for name in ("the doc's example", "colours, fonts, every entry and exit"):
            n, bad, done = compare_frames(big, fonts, {0: core.encode_markup(DESCRIPTIONS[name], H)})
            check(f"{model} {W}x{H} {name}: {n} frames", bad is None and done, f"differs at frame {bad}")

    print("5. RS-485 record text: scrolled as markup only when wider than the window (record_text_scrolls)")
    texts = [core.encode_markup(d) for d in DESCRIPTIONS.values()] + [
        b"HI", core.encode_markup([{"text": "WIN", "fg": RED}]), b"\x1b0\x7c\x00" + b"CONGRATULATIONS JACKPOT",
        b"x" * 60, b"$1,000,000"]
    for data in texts:
        for (w, h) in ((80, 16), (160, 32)):
            k, fw_w = sign.pick_for_text(data, w, h)
            scrolls, width, font = core.record_text_scrolls(data, w, h, fonts=fonts)
            check(f"{data[:24]!r} in {w}x{h}: {'scrolls' if scrolls else 'fits, drawn plainly'} ({width} px, {font})",
                  (fonts[k].name, fw_w) == (font, width), (fonts[k].name, fw_w))

    print("6. warnings")
    w = core.markup_warnings(RAW["hold left at 0xFFFFFFFF (segment 2 holds for ever)"][0], 80, 16)
    check("hold left at 0xFFFFFFFF is reported", any("for ever" in x for x in w), w)
    for name, d in DESCRIPTIONS.items():
        w = core.markup_warnings(core.encode_markup(d), 80, 16)
        check(f"encoder output never holds for ever: {name}", not any("for ever" in x for x in w), w)

    print("\nALL STUDIO MARKUP CHECKS PASS" if ok else "\nFAILURES")
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
