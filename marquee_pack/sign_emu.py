#!/usr/bin/env python3
"""Software emulator for the Mikohn/Neurizon RGB LED sign (reconstructed from the firmware).

Panel model (from ELITE_RGB):
  - Modular LED panel, this unit 80x16. Framebuffer = W*H 16-bit pixels, RGB555:
    bit14..10 = Red(5), bit9..5 = Green(5), bit4..0 = Blue(5).  (render fn 0x0ef48; red mask 0x7c00)
  - Content animations are Autodesk FLIC (.flc), 8-bit palette; palette RGB(0..255) -> RGB555 by >>3.
This module: panel + FLIC decoder + scale-up export (PNG/GIF). Text rendering is in font_render.py.
"""
import struct, sys


def rgb555_to_rgb888(v):
    r = (v >> 10) & 0x1F; g = (v >> 5) & 0x1F; b = v & 0x1F
    return (r << 3 | r >> 2, g << 3 | g >> 2, b << 3 | b >> 2)


def rgb888_to_rgb555(r, g, b):
    return ((r >> 3) & 0x1F) << 10 | ((g >> 3) & 0x1F) << 5 | ((b >> 3) & 0x1F)


class Panel:
    def __init__(self, w=80, h=16):
        self.w, self.h = w, h
        self.fb = [0] * (w * h)          # RGB555

    def clear(self, c=0):
        self.fb = [c] * (self.w * self.h)

    def put(self, x, y, c555):
        if 0 <= x < self.w and 0 <= y < self.h:
            self.fb[y * self.w + x] = c555

    def to_rgb_rows(self):
        return [[rgb555_to_rgb888(self.fb[y * self.w + x]) for x in range(self.w)] for y in range(self.h)]


class FLIC:
    """Minimal Autodesk FLIC (.flc/.fli) decoder: FLI_COLOR256, DELTA_FLC, BRUN, COPY, BLACK."""
    def __init__(self, data):
        self.d = data
        size, magic, self.frames, self.w, self.h, self.depth = struct.unpack_from("<IHHHHH", data, 0)
        assert magic in (0xAF11, 0xAF12), f"not FLIC: {magic:#x}"
        speed = struct.unpack_from("<I", data, 0x10)[0]
        self.speed_ms = speed * 100 // 7 if magic == 0xAF11 else speed   # FLI: 1/70 s units
        self.palette = [(0, 0, 0)] * 256
        self.pixels = [0] * (self.w * self.h)   # palette indices (8-bit) or RGB555 values (16-bit)

    def _chunks(self, off, end):
        while off + 6 <= end:
            sz, typ = struct.unpack_from("<IH", self.d, off)
            yield typ, off + 6, sz
            off += sz if sz else 6

    def frame_offsets(self):
        """File offsets of the frame chunks (0xF1FA), in order. FLC stores the offset to frame 1 at header +0x50
        (oframe1); the region before it may hold an FLI_PREFIX chunk (0xF100) with the thumbnail/authoring
        metadata, which must NOT be parsed as a frame. Other chunk types are skipped."""
        d = self.d
        oframe1 = struct.unpack_from("<I", d, 0x50)[0] if len(d) >= 0x54 else 0
        off = oframe1 if 128 <= oframe1 < len(d) else 128
        out = []
        while off + 16 <= len(d):
            fsize, ftype = struct.unpack_from("<IH", d, off)
            if ftype == 0xF1FA:
                out.append(off)
            off += fsize if fsize else 16
        return out

    def decode(self, off):
        """Apply the frame chunk at `off` to the current pixels and palette (frames are deltas)."""
        d = self.d
        fsize = struct.unpack_from("<I", d, off)[0]
        nsub = struct.unpack_from("<H", d, off + 6)[0]
        p = off + 16
        for _ in range(nsub):
            if p + 6 > off + fsize: break
            csz, ctyp = struct.unpack_from("<IH", d, p)
            body = p + 6
            if self.depth == 16:     # the sign's 16-bit variant (see tools/gif2flc.py)
                if ctyp == 15:
                    self._brun16(body)
                elif ctyp == 16:
                    self.pixels = list(struct.unpack_from(f"<{self.w * self.h}H", d, body))
                elif ctyp == 13:
                    self.pixels = [0] * (self.w * self.h)
                p += csz if csz else 6
                continue
            if ctyp in (4, 11):      # COLOR256 / COLOR64
                self._palette(body, ctyp == 11)
            elif ctyp == 15:          # BYTE_RUN (BRUN)
                self._brun(body)
            elif ctyp == 12:          # LC (delta FLI)
                self._lc(body)
            elif ctyp == 7:           # SS2 (delta FLC)
                self._ss2(body)
            elif ctyp == 13:          # BLACK
                self.pixels = [0] * (self.w * self.h)
            elif ctyp == 16:          # COPY
                self.pixels = list(d[body:body + self.w * self.h])
            p += csz if csz else 6

    def frames_rgb(self):
        """Every frame (and the ring frame after the last) as rows of RGB888."""
        out = []
        for off in self.frame_offsets()[:self.frames + 1]:
            self.decode(off)
            out.append(self._render())
        return out

    def _palette(self, off, div64):
        d = self.d
        npk = struct.unpack_from("<H", d, off)[0]; off += 2
        idx = 0
        for _ in range(npk):
            skip = d[off]; cnt = d[off + 1]; off += 2
            idx += skip
            n = cnt if cnt else 256
            for _ in range(n):
                r, g, b = d[off], d[off + 1], d[off + 2]; off += 3
                if div64: r, g, b = r << 2, g << 2, b << 2
                if idx < 256: self.palette[idx] = (r, g, b)
                idx += 1

    def _brun(self, off):
        d = self.d; px = self.pixels; W, H = self.w, self.h
        for y in range(H):
            off += 1  # per-row packet-count byte (legacy; decode by width instead)
            line = []
            while len(line) < W:
                c = d[off]; off += 1
                if c & 0x80:              # negative: literal run of (256-c) bytes
                    n = 256 - c
                    line += list(d[off:off + n]); off += n
                else:                     # positive: repeat next byte c times
                    n = c
                    line += [d[off]] * n; off += 1
            for x in range(W): px[y * W + x] = line[x]

    def _brun16(self, off):
        """16-bit BRUN as ELITE_RGB decodes it: byte runs over 2*width bytes per row, little-endian pixels."""
        d = self.d; W, H = self.w, self.h
        raw = bytearray()
        for y in range(H):
            off += 1
            row = bytearray()
            while len(row) < 2 * W:
                c = d[off]; off += 1
                if c & 0x80:
                    n = 256 - c; row += d[off:off + n]; off += n
                else:
                    row += bytes([d[off]]) * c; off += 1
            raw += row[:2 * W]
        self.pixels = list(struct.unpack(f"<{W * H}H", bytes(raw)))

    def _skip_rle_line(self, off):  # helper for deltas (SS2/LC handled inline)
        return off

    def _lc(self, off):
        d = self.d; px = self.pixels; W = self.w
        y0, ny = struct.unpack_from("<HH", d, off); off += 4
        y = y0
        for _ in range(ny):
            npk = d[off]; off += 1; x = 0
            for _ in range(npk):
                skip = d[off]; sz = struct.unpack_from("b", d, off + 1)[0]; off += 2
                x += skip
                if sz >= 0:
                    for i in range(sz): px[y * W + x + i] = d[off + i]
                    off += sz; x += sz
                else:
                    n = -sz; c = d[off]; off += 1
                    for i in range(n): px[y * W + x + i] = c
                    x += n
            y += 1

    def _ss2(self, off):
        d = self.d; px = self.pixels; W = self.w
        ny = struct.unpack_from("<H", d, off)[0]; off += 2
        y = 0
        lines_done = 0
        while lines_done < ny:
            word = struct.unpack_from("<H", d, off)[0]; off += 2
            if word & 0x8000:
                if word & 0x4000:        # skip lines
                    y += (0x10000 - word)
                    continue
                else:                     # last byte of line
                    px[y * W + (W - 1)] = word & 0xFF
                    # still expect packets? fallthrough: treat as line with last pixel set
                    lines_done += 1; y += 1
                    continue
            npk = word; x = 0
            for _ in range(npk):
                skip = d[off]; sz = struct.unpack_from("b", d, off + 1)[0]; off += 2
                x += skip
                if sz >= 0:
                    for i in range(sz):
                        px[y * W + x + i * 2] = d[off + i * 2]
                        px[y * W + x + i * 2 + 1] = d[off + i * 2 + 1]
                    off += sz * 2; x += sz * 2
                else:
                    n = -sz; a, b = d[off], d[off + 1]; off += 2
                    for i in range(n):
                        px[y * W + x + i * 2] = a
                        px[y * W + x + i * 2 + 1] = b
                    x += n * 2
            lines_done += 1; y += 1

    def image555(self):
        """The current frame as the firmware holds it, as an H x W RGB555 array: a 16-bit frame as stored, an
        8-bit one through the palette converted by >>3 (flc_chunk_color256 with palette_uses_plain_rgb)."""
        import numpy as np
        px = np.array(self.pixels, np.uint16).reshape(self.h, self.w)
        if self.depth == 16:
            return px
        pal = np.array([rgb888_to_rgb555(*c) for c in self.palette], np.uint16)
        return pal[px]

    def _render(self):
        if self.depth == 16:
            return [[rgb555_to_rgb888(self.pixels[y * self.w + x]) for x in range(self.w)] for y in range(self.h)]
        return [[self.palette[self.pixels[y * self.w + x]] for x in range(self.w)] for y in range(self.h)]


def frames_to_gif(frames_rgb, path, scale=8, ms=60):
    from PIL import Image
    imgs = []
    for fr in frames_rgb:
        h = len(fr); w = len(fr[0])
        im = Image.new("RGB", (w, h))
        im.putdata([px for row in fr for px in row])
        imgs.append(im.resize((w * scale, h * scale), Image.NEAREST))
    imgs[0].save(path, save_all=True, append_images=imgs[1:], duration=ms, loop=0)
    return path



# ---------------------------------------------------------------------------
# Message lines: the firmware's textline (ELITE_RGB 0x466C8 textline_init, 0x46C78 message_text_next_token,
# 0x469D0 textline_step, 0x4D24C image_slide_init, 0x4D354 image_slide_step), ported statement by statement.
# Markup: docs/MESSAGE-MARKUP.md. Checked frame by frame against the firmware by studio/test_markup.py.
#   - Each segment is drawn once into a 0x1414-wide text image, then slid over the line image (= the panel).
#   - Fonts are fonteNN.png in load order (fonts_load: fonte00..fonte99), measured like font_build (0x448D4).
# Signed/unsigned casts follow the binary (s16 = the C "(short)" casts).

TEXT_IMG_W = 0x1414
ESC, CR = 0x1B, 0x0D


def s16(v):
    v &= 0xFFFF
    return v - 0x10000 if v & 0x8000 else v


def s32(v):
    v &= 0xFFFFFFFF
    return v - (1 << 32) if v & 0x80000000 else v


def cdiv(a, b):
    """C integer division (truncates towards zero). PowerPC divw by 0 is undefined; taken as 0 here."""
    if b == 0:
        return 0
    q = abs(a) // abs(b)
    return q if (a < 0) == (b < 0) else -q


def half(u):
    """(int)u >> 1, rounded towards zero (the idiom in text_centre_in_rect / font_pick_largest)."""
    return cdiv(s32(u), 2)


def fwfont_mqf(path, name=""):
    """An MQF1 font (marquee_studio/mqf.py) as our firmware loads it (firmware/ext/fonts.c): each glyph in a 16 x 16
    cell grid (cell: the widest advance x the line height), font_build's measuring, then the font's own metrics: every
    code's width = its advance (0 if the font lacks it), left 0, not proportional."""
    import numpy as np
    from . import mqf
    lh, asc, glyphs = mqf.read(open(path, "rb").read())
    cw = max([1] + [a for a, _ in glyphs.values()])
    ink = np.zeros((16 * lh, 16 * cw), bool)
    for c, (a, rows) in glyphs.items():
        if c < 256 and a:
            ink[(c >> 4) * lh:(c >> 4) * lh + lh, (c & 15) * cw:(c & 15) * cw + a] = rows
    f = FwFont(ink=ink, name=name)
    for c in range(256):
        a = glyphs[c][0] if c in glyphs else 0
        f.left[c], f.width[c], f.right[c], f.prop[c] = 0, a, (cw - a) & 0xFF, 0
    return f


class FwFont:
    """A fonteNN.png as font_build makes it: layout 2, a 16 x 16 grid of all 256 codes, cell = (w/16, h/16).
    A pixel is ink when its RGB555 value is not 0. Per-code tables left/width/right/proportional
    (font_measure_glyph_maybe 0x427FC):
      - space: width = img_w / 64 (at least 3), fixed;
      - '0'..'9', 0x10..0x19 (the large digits) and 0xFF: the whole cell, fixed;
      - anything else: trimmed to its inked columns, proportional (a glyph with no ink: width 0).
    Then codes 0..9 all get the union of their extents."""
    def __init__(self, png_path=None, ink=None, name=""):
        import numpy as np
        if ink is None:
            from PIL import Image
            rgb = np.array(Image.open(png_path).convert("RGB"))
            ink = ((rgb >> 3) != 0).any(axis=2)
        self.ink, self.name = ink, name
        self.h, self.w = ink.shape
        self.cw, self.ch = self.w >> 4, self.h >> 4
        self.left, self.width, self.right, self.prop = [0] * 256, [0] * 256, [0] * 256, [0] * 256
        for c in range(256):
            self._measure(c)
        lo = min(self.left[c] for c in range(10))
        hi = max((self.left[c] + self.width[c]) & 0xFF for c in range(10))
        for c in range(10):
            self.left[c], self.width[c] = lo, (hi - lo) & 0xFF

    def _measure(self, c):
        cw, ch = self.cw, self.ch
        self.prop[c] = 1
        if c == 0x20:
            self.left[c] = 0
            self.width[c] = max(3, (self.w >> 6) & 0xFF)
            self.prop[c] = 0
            return
        if c == 0xFF or 0x30 <= c <= 0x39 or 0x10 <= c <= 0x19:
            self.left[c], self.width[c], self.right[c], self.prop[c] = 0, cw & 0xFF, 0, 0
            return
        x0, y0 = (c & 15) * cw, (c >> 4) * ch
        cols = self.ink[y0:y0 + ch, x0:x0 + cw].any(axis=0)
        left = 0
        while left < cw and not cols[left]:
            left += 1
        right = 0
        if left < cw:
            while right < cw and not cols[cw - 1 - right]:
                right += 1
        if left + right < cw:
            self.left[c], self.width[c], self.right[c] = left, cw - (left + right), right
        else:
            self.left[c], self.width[c], self.right[c] = cw & 0xFF, 0, 0

    def text_width(self, data):
        """text_width (0x43BC0): inked widths, plus a 1-px gap after each proportional glyph and one more
        before a proportional glyph that follows a fixed one."""
        w, prev = 0, 1
        for c in data:
            if self.prop[c]:
                w += (prev == 0) + self.width[c] + 1
            else:
                w += self.width[c]
            prev = self.prop[c]
        return w

    def draw(self, img, x, y, data, fg, bg):
        """text_draw_proportional (0x42478): each glyph trimmed to its ink, drawn as a two-colour mask
        (ink = fg, rest of the cell = bg); a bg column after a proportional glyph, a black one before a
        proportional glyph that follows a fixed one. Returns the drawn width (u16)."""
        x0, prev = x, 1
        for c in data:
            p = self.prop[c]
            if prev == 0 and p:
                _vline(img, x, y, self.ch, 0); x += 1
            gw = self.width[c]
            sx, sy = (c & 15) * self.cw + self.left[c], (c >> 4) * self.ch
            _blit_mask(img, self.ink[sy:sy + self.ch, sx:sx + gw], x, y, fg, bg)
            x += gw
            if p:
                _vline(img, x, y, self.ch, bg); x += 1
            prev = p
        return (x - x0) & 0xFFFF


class FirmwareHang(Exception):
    """The firmware would loop for ever here (the display task stops; the panel freezes)."""


def _vline(img, x, y, n, colour):
    """draw_line from (x, y) to (x, y + n): n + 1 pixels, each clipped (unsigned compare). draw_line steps a
    16-bit y until it equals the 32-bit end, so an end outside -32768..32767 never comes: that happens for
    left-aligned text in a font taller than the line (its y is (h - cell_h) >> 1, unsigned)."""
    if not -0x8000 <= y + n <= 0x7FFF:
        raise FirmwareHang(f"draw_line to y = {y + n} never ends (font taller than the line)")
    h, w = img.shape
    if 0 <= x < w:
        for yy in range(max(0, y), min(h, y + n + 1)):
            img[yy, x] = colour


def _blit_mask(img, mask, x, y, fg, bg):
    """glyph_blit mode 0 -> blit_mask_2colour, clipped to the destination."""
    h, w = img.shape
    mh, mw = mask.shape
    x1, y1 = max(0, x), max(0, y)
    x2, y2 = min(w, x + mw), min(h, y + mh)
    if x2 <= x1 or y2 <= y1:
        return
    m = mask[y1 - y:y2 - y, x1 - x:x2 - x]
    img[y1:y2, x1:x2] = m * fg + (~m) * bg


def _auto(fonts, auto):
    """The indices of fonts an automatic pick may choose (our firmware's fonts.auto, firmware/ext/fonts.c): those
    named in auto, in order; None (all of them, the stock rule) when auto is empty or names none of them."""
    if not auto:
        return None
    idx = [i for i, f in enumerate(fonts) if f.name in auto]
    return idx or None


def font_pick_largest(fonts, box_h, auto=None):
    """font_pick_largest (0x44750), mode 0: the widest font whose cell height fits; ties go to the later one.
    Returns an index into fonts (None if there are none). auto: the names it may pick (_auto)."""
    sub = _auto(fonts, auto)
    if sub is not None:
        k = font_pick_largest([fonts[i] for i in sub], box_h)
        return None if k is None else sub[k]
    best_w = best_h = fb_w = 0
    fb_h, found, fb = 0xFFFFFFFF, None, None
    for i, f in enumerate(fonts):
        if f.cw < best_w or box_h < f.ch or f.ch < best_h:
            if fb_w < f.cw and f.ch <= fb_h:
                fb, fb_h, fb_w = i, f.ch, f.cw
        else:
            found, best_h, best_w = i, f.ch, f.cw
    return found if found is not None else fb


def font_pick_for_text(fonts, text, box_w, box_h, auto=None):
    """text_draw_justified_maybe (0x43F20) as message_text_next_token calls it for a centred segment (ESC ` FF):
    on the segment's FIRST character, the font with the largest cell height that fits the box (and whose width
    for that character is not smaller); ties go to the later font. If none fits, the smallest. The box is the
    text image (5140 x H) for entries from the right/left, the display (W x H) for entries from the bottom/top.
    (Effect words 1..3 would shrink the box first; not modelled.) auto: the names it may pick (_auto)."""
    sub = _auto(fonts, auto)
    if sub is not None:
        k = font_pick_for_text([fonts[i] for i in sub], text, box_w, box_h)
        return None if k is None else sub[k]
    best_w = best_h = 0
    fb_w = fb_h = 0xFFFFFFFF
    found = fb = None
    for i, f in enumerate(fonts):
        w, h = f.text_width(text), f.ch
        if box_w < w or w < best_w or box_h < h or h < best_h:
            if w <= fb_w and h <= fb_h:
                fb, fb_h, fb_w = i, h, w
        else:
            found, best_h, best_w = i, h, w
    return found if found is not None else fb


class Slide:
    """image_slide_init / image_slide_step: a straight line from `from` to `to` in frames = |d| / speed * 40
    (40 fps), a per-axis step in 1/100 px, positions rounded per frame."""
    def __init__(self, x0, y0, x1, y1, speed):
        import numpy as np
        self.fx, self.fy, self.tx, self.ty = x0, y0, x1, y1
        dx, dy = s16(s16(x1) - s16(x0)), s16(s16(y1) - s16(y0))
        self.step = 0
        d = float(np.float32(dx * dx + dy * dy))            # __floatsisf, then double
        f = (d ** 0.5) / speed * 40.0 if speed else float("inf")
        self.frames = 0xFFFFFFFF if f >= 2 ** 32 else int(f)  # __fixunsdfsi
        n = s16(self.frames)
        self.dx100, self.dy100 = s16(cdiv(dx * 100, n)), s16(cdiv(dy * 100, n))

    def pos(self):
        st = s16(self.step)
        return cdiv(st * self.dx100 + 50, 100) + self.fx, cdiv(st * self.dy100 + 50, 100) + self.fy

    def run(self, src, dst):
        """One frame. Returns True when done (that frame blits at `to` without clearing first)."""
        if self.frames < self.step:
            _blit(src, dst, self.tx, self.ty)
            return True
        x, y = self.pos()
        dst[:] = 0
        _blit(src, dst, x, y)
        self.step += 1
        return False


def _blit(src, dst, x, y):
    """image_blit, RGB555 to RGB555: a plain copy, clipped."""
    h, w = dst.shape
    sh, sw = src.shape
    x1, y1, x2, y2 = max(0, x), max(0, y), min(w, x + sw), min(h, y + sh)
    if x2 > x1 and y2 > y1:
        dst[y1:y2, x1:x2] = src[y1 - y:y2 - y, x1 - x:x2 - x]


class TextLine:
    """One text line on a W x H box (the whole panel), reading `table`: the message lines, 256 bytes each.
    step() runs one display frame (40 fps) and returns the state: 0 next segment, 1 slide in, 2 slide out,
    3 hold, 4 done. `img` is the line image after the frame.
    r24: message_text_next_token never sets r24 before it tests it, so ESC 'C' (entry from the top) takes the
    ESC 'D' path whenever the caller left r24 non-zero. 0 here; the real value depends on the display task."""
    def __init__(self, table, fonts, W, H, line=0, r24=0, box_x=0, auto=None):
        import numpy as np
        self.table = bytes(table).ljust(256 * 256, b"\0")
        self.fonts, self.W, self.H, self.r24 = fonts, W, H, r24
        self.img = np.zeros((H, W), np.uint16)
        self.text_img = np.zeros((H, TEXT_IMG_W), np.uint16)
        self.lpos = [0] * 256                   # message_line_state[n] + 0x114: read position per line
        self.stack = [0] * 256
        # textline_init
        self.state, self.line, self.depth = 0, line, 0
        self.entry_dir = self.exit_dir = 0
        self.entry_speed = self.exit_speed = 40
        self.hold, self.fg, self.bg, self.proportional = 0, 0x7C00, 0, 0
        self.box_x = box_x
        self.auto = auto
        self.font = font_pick_largest(fonts, H, auto)
        self.text, self.pos, self.text_rect, self.sl = b"", (0, 0), [0, 0, 0, 0], None

    @property
    def f(self):
        return self.fonts[self.font]

    def next_token(self):
        """message_text_next_token."""
        T, r24 = self.table, self.r24
        p = self.line * 256 + self.lpos[self.line]
        self.state = 0
        while True:
            b = T[p]; p += 1
            self.lpos[self.line] = (self.lpos[self.line] + 1) & 0xFFFF
            if b == 0:                                       # end of line: pop an include, or done
                self.lpos[self.line] = 0
                self.depth = (self.depth - 1) & 0xFF
                if self.depth & 0x80:
                    self.state = 4
                    return
                self.line = self.stack[self.depth]
                p = self.line * 256 + self.lpos[self.line]
                continue
            if b != ESC:
                text = bytearray([b])
                while T[p] not in (CR, ESC, 0) and len(text) < 256:
                    text.append(T[p]); p += 1
                self.lpos[self.line] = (p - self.line * 256) & 0xFFFF
                self.text = bytes(text)
                self._layout_and_slide(r24)
                self.state = 1
                return
            cmd, a = T[p], p + 1
            self.lpos[self.line] = (self.lpos[self.line] + 1) & 0xFFFF
            if cmd in (0x30, 0x31):                          # colours, big-endian RGB555
                v = T[a] << 8 | T[a + 1]
                if cmd == 0x30:
                    self.fg = v
                else:
                    self.bg = v
                p += 3; self.lpos[self.line] += 2
            elif 0x40 <= cmd <= 0x44:                        # entry direction + speed
                self.entry_dir = min(cmd - 0x40, 3)
                self.entry_speed = T[a]
                if cmd == 0x44:
                    r24 = 1
                p += 2; self.lpos[self.line] += 1
            elif 0x50 <= cmd <= 0x54:                        # hold, exit direction + speed
                self.exit_dir = min(cmd - 0x50, 3)
                self.hold = T[a] << 2
                self.exit_speed = T[a + 1]
                if cmd == 0x54:
                    r24 = 1
                p += 3; self.lpos[self.line] += 2
            elif cmd == 0x60:                                # font n, or 0xFF: proportional (centred)
                n = T[a]
                if n == 0xFF:
                    self.proportional = 1
                elif n < len(self.fonts):
                    self.font, self.proportional = n, 0
                p += 2; self.lpos[self.line] += 1
            elif cmd == 0x80:                                # include line n
                self.stack[self.depth] = self.line
                self.depth = (self.depth + 1) & 0xFF
                self.lpos[self.line] += 1
                self.line = T[a]
                p = self.line * 256 + self.lpos[self.line]
            else:                                            # unknown: skip ESC + command byte
                p += 1

    def _layout_and_slide(self, r24):
        f, ti = self.f, self.text_img
        th, tiw, lw, lh = self.H, TEXT_IMG_W, self.W, self.H
        ti[:] = 0
        if self.proportional == 0:
            self.pos = (self.box_x, s32(((th - f.ch) & 0xFFFFFFFF) >> 1))
            self.text_rect = [0, 0, len(self.text) * f.cw, f.ch]
        else:                                                # pick a font, then text_centre_in_rect
            box = (tiw, th) if self.entry_dir < 2 else (self.W, self.H)
            n = font_pick_for_text(self.fonts, self.text[:1], *box, auto=self.auto)
            if n is not None:
                self.font = n
            f = self.f
            w, h = min(f.text_width(self.text), tiw), min(f.ch, th)
            self.text_rect = [0, 0, w, h]
            self.pos = (half(tiw - w), half(th - h))
        self.text_rect[2] = f.draw(ti, self.pos[0], self.pos[1], self.text, self.fg, self.bg)
        px, py = s16(self.pos[0]), s16(self.pos[1])
        tw, trh = s16(self.text_rect[2]), s16(self.text_rect[3])
        cy = s16(((th - lh) & 0xFFFFFFFF) >> 1)              # vertical centring of the text image (0)
        cx = s16(((tiw - lw) & 0xFFFFFFFF) >> 1)             # horizontal centring of the text image
        hold, d = self.hold, self.entry_dir
        if d == 0:                                           # from the right
            sl = (s16(lw - px), -cy, -s16(px + tw) if hold == 0 else -cx, -cy)
        elif d == 1:                                         # from the left
            sl = (-s16(px + tw), -cy, s16(lw - px) if hold == 0 else -cx, -cy)
        elif d == 2:                                         # from the bottom
            sl = (-cx, s16(lh - py), -cx, -s16(py + trh) if hold == 0 else -cy)
        elif d == 3 and not r24:                             # from the top
            sl = (-cx, -s16(lh), -cx, s16(lh - py) if hold == 0 else -cy)
        elif d == 3:                                         # from the top, text image at x = 0 (ESC 'D'/'T')
            sl = (0, -s16(lh), 0, s16(lh - py) if hold == 0 else -cy)
        else:
            sl = None                                        # (not reachable: entry_dir is 0..3)
        if sl:
            self.sl = Slide(*sl, self.entry_speed)

    def step(self):
        """textline_step: one frame."""
        st = self.state
        if st == 0:
            self.next_token()
        elif st == 1:
            if self.sl.run(self.text_img, self.img):
                self.state = 3 if self.hold else 0
        elif st == 2:
            if self.sl.run(self.text_img, self.img):
                self.state = 0
        elif st == 3:
            d = self.hold
            self.hold = (d - 1) & 0xFFFFFFFF
            if d == 0:
                tx, ty = s16(self.sl.tx), s16(self.sl.ty)
                px, py = s16(self.pos[0]), s16(self.pos[1])
                tw, trh = s16(self.text_rect[2]), s16(self.text_rect[3])
                e = self.exit_dir
                if e == 0:
                    self.sl = Slide(tx, ty, -s16(px + tw), ty, self.exit_speed)
                elif e == 1:
                    self.sl = Slide(tx, ty, s16(self.W - px), ty, self.exit_speed)
                elif e == 2:
                    self.sl = Slide(tx, ty, tx, -s16(py + trh), self.exit_speed)
                elif e == 3:
                    self.sl = Slide(tx, ty, tx, s16(self.H - py), self.exit_speed)
                self.state = 2
        return self.state


def textline_frames(table, fonts, W, H, line=0, max_frames=2400, r24=0, auto=None):
    """Run a text line to the end (state 4) or max_frames. Returns (frames, states): RGB555 arrays, one per
    40 fps frame, and the state after each frame (-1 last: the firmware hangs, see FirmwareHang)."""
    tl = TextLine(table, fonts, W, H, line=line, r24=r24, auto=auto)
    frames, states = [], []
    while len(frames) < max_frames:
        try:
            st = tl.step()
        except FirmwareHang:
            states.append(-1)                                # -1: the firmware hangs here
            frames.append(tl.img.copy())
            break
        frames.append(tl.img.copy()); states.append(st)
        if st == 4:
            break
    return frames, states



# ---- Text effects: effect words FG<<16 | BG, the outline (docs/EFFECTS-AND-LAYERS.md) ----
# Checked pixel for pixel against the firmware's own code under Unicorn by studio/test_effects.py.
FLC_SPEED_THRESHOLDS = tuple(38 + 25 * i for i in range(40)) + (0,)      # flc_speed_thresholds 0xB8CB0
CYCLE_COLOURS = (0x7C00, 0x03E0, 0x7FE0)    # red, green, yellow: 0xA1A30 (palette cycle), 0xA1A0A (stripes)
OUTLINE_STYLES = ("None", "Thick", "Thin", "Drop Shadow")                 # theme enum 0xB5510
OUTLINE_COLOURS = ("Black", "Red", "Green", "Yellow", "Blue", "Cyan", "Magenta", "White")   # 0xB5528
TRANSPARENT = 0xFFFF                        # what effect_resolve makes of 0x8000: "leave the pixel"
U32 = 0xFFFFFFFF


def flc_speed_to_hold(speed):
    """flc_speed_to_hold (0x41C58): extra 25 ms display frames to hold each animation frame."""
    if speed <= FLC_SPEED_THRESHOLDS[0]:
        return 0
    i = 1
    while FLC_SPEED_THRESHOLDS[i] < speed and i < 40:
        i += 1
    return i


def colour_index_to_rgb555(i):
    """colour_index_to_rgb555 (0x41040): the outline colours; anything else is black."""
    return (0x0000, 0x7C00, 0x03E0, 0x7FE0, 0x001F, 0x03FF, 0x7C1F, 0x7FFF)[i] if 0 <= i < 8 else 0


class EffectSlot:
    """One effect animation slot (code 0xC000|n): the FLC player effects_load_slots (0x406DC) opens on
    effectNN.flc, as effect_resolve drives it. Per panel frame (ledpanel_frame_count) the slot ticks at most once,
    however many glyphs use it; flc_tick (0x42314) shows each frame for 1 + flc_speed_to_hold(speed) panel
    frames, and after the header's last frame effect_resolve rewinds it, so the ring frame is never shown.
    A slot not ticked for more than 8 panel frames starts again from frame 0. A file that did not load draws
    solid yellow (flc_open leaves a panel-sized indexed image with every palette entry 0x7FE0)."""
    def __init__(self, data=None):
        import numpy as np
        self.stamp = 0          # 0x192930[n]: frame count effect_resolve last ticked this slot at
        self.failed = data is None
        if self.failed:
            self.image = np.full((1, 1), 0x7FE0, np.uint16)
            return
        self.fl = FLIC(data)
        self.offsets = self.fl.frame_offsets()
        self.n = self.fl.frames
        self.hold = flc_speed_to_hold(self.fl.speed_ms)
        self.image = np.zeros((self.fl.h, self.fl.w), np.uint16)       # image_alloc: pixels and palette 0
        self.index = 0          # player +0x10: frame number (and file position: the next frame chunk)
        self.count = 0          # +0x126: hold count down
        self.last = 0           # +0x128: ledpanel_frame_count at the last tick

    def rewind(self):
        if not self.failed:
            self.index = 0

    def tick(self, fc):
        """flc_tick: 1 playing, 2 decoded the last frame, 0 failed."""
        import numpy as np
        if self.failed:
            return 0
        if (self.last + 8) & U32 < fc:
            self.rewind()
        self.last = fc
        if self.count:
            self.count -= 1
            return 1
        self.count = self.hold
        if self.index >= len(self.offsets):          # no frame chunk left: blank (palette all yellow), stop
            self.failed = True
            self.image = np.full(self.image.shape, 0x7FE0 if self.fl.depth != 16 else 0, np.uint16)
            return 0
        self.fl.decode(self.offsets[self.index])     # decoded over the current frame, as the player does
        self.image = self.fl.image555()
        if self.index < self.n - 1:
            self.index += 1
            return 1
        return 2


class Layer:
    """One resolved half of an effect word (effect_resolve's output): a solid colour (tex None; TRANSPARENT
    leaves the pixel), or a texture repeated across the destination. anchored = 0 puts the texture origin at the
    top-left of the clipped blit instead of the given origin (0x8001 only)."""
    def __init__(self, colour=0, tex=None, anchored=1):
        self.colour, self.tex, self.anchored = colour, tex, anchored


class Effects:
    """The firmware's effect state: the panel frame counter (ledpanel_frame_count, the phase source) and the
    100 effect slots. frame_count must be set before each panel frame is drawn."""
    def __init__(self, slots=None, frame_count=0):
        self.frame_count = frame_count
        self.slots = {n: EffectSlot(slots.get(n)) for n in range(100)} if slots is not None else {}

    def slot(self, n):
        if n not in self.slots:
            self.slots[n] = EffectSlot(None)
        return self.slots[n]

    def texture_8001(self, h, shift=2):
        """effect_texture_8001 (0x408FC): a 1-pixel-wide column, h high (h = the cell or rect height), wiping
        downwards one row per 4 panel frames: rows 0..t%h in the new colour, the rest in the old one; the
        colours step red -> yellow -> green -> red every h rows. t = frame_count >> 2."""
        import numpy as np
        t = self.frame_count >> shift
        a, b = ((0x7C00, 0x03E0), (0x7FE0, 0x7C00), (0x03E0, 0x7FE0))[(t // h) % 3]
        return np.where(np.arange(h) > t % h, b, a).astype(np.uint16).reshape(h, 1)

    def texture_8002(self, n=0x304, shift=1):
        """effect_texture_8002 (0x40AB8): diagonal stripes of red, green and yellow, each n wide; a (3n x 3n)
        tile moving one pixel per 2 panel frames. effect_resolve always passes 0x304, so n is 4 (a 12 x 12
        tile). The index (x + y - t) is unsigned: until t > x + y it has not wrapped, and 2^32 is not a multiple
        of 3n, so the first 2(x + y) frames after power-up are out of step with the rest."""
        import numpy as np
        n = min(max(n & 0xFF, 1), 8)
        w = 3 * n
        t = self.frame_count >> shift
        stripes = np.repeat(np.array(CYCLE_COLOURS, np.uint16), n)
        k = (np.arange(w)[None, :] + np.arange(w)[:, None] - t) & U32
        return stripes[k % w]

    def resolve(self, code, phase=0, cell_h=16):
        """effect_resolve (0x40D10): (ok, Layer)."""
        fc = self.frame_count
        if code < 0x8000:
            return True, Layer(code)
        if code & 0xC000 == 0x8000:
            if code == 0x8002:
                return True, Layer(tex=self.texture_8002())
            if code in (0xBFFE, 0xBFFF):           # effect_palette_cycle: table[((fc >> 4) +- phase) % 3]
                v = ((fc >> 4) + (phase if code == 0xBFFE else -phase)) & U32
                return True, Layer(CYCLE_COLOURS[v % 3])
            if code == 0x8000:
                return True, Layer(TRANSPARENT)
            if code == 0x8001:
                return True, Layer(tex=self.texture_8001(cell_h), anchored=0)
            return False, Layer(0x7FE0)            # 0x8003..0xBFFD: nothing is drawn at all
        n = code & 0x3FFF
        if n >= 100:
            return True, Layer(0x7FE0)
        s = self.slot(n)
        if s.stamp != fc:
            s.stamp = fc
            if s.tick(fc) != 1:
                s.rewind()
        return True, Layer(tex=s.image)

    def desc(self, word, phase=0, cell_h=16):
        """glyph_desc_from_effect (0x3EA28): (fg, bg) Layers, or None (then nothing is drawn). The BG half is
        not resolved (its slot does not tick) when the FG half fails."""
        ok, fg = self.resolve(word >> 16, phase, cell_h)
        if not ok:
            return None
        ok, bg = self.resolve(word & 0xFFFF, phase, cell_h)
        return (fg, bg) if ok else None

    def blit(self, src, dst, desc, r, pos, fg_origin, bg_origin):
        """glyph_blit (0x3EBF0) mode 8. src is the glyph image, used only as a mask (pixel != 0); r is the source
        rect (clipped in place, as the firmware does); pos the destination. A texture pixel at destination
        (X, Y) is tex[(Y - origin.y) mod h][(X - origin.x) mod w], the subtraction unsigned 32-bit."""
        import numpy as np
        sh, sw = src.shape
        dh, dw = dst.shape
        if not rect_clip((0, 0, sw, sh), r):
            return
        d = [pos[0], pos[1], r[2], r[3]]
        if not rect_clip((0, 0, dw, dh), d):
            return
        r[0] -= pos[0] - d[0]
        r[1] -= pos[1] - d[1]
        fo = [d[0] - pos[0] + fg_origin[0], d[1] - pos[1] + fg_origin[1]]
        bo = [d[0] - pos[0] + bg_origin[0], d[1] - pos[1] + bg_origin[1]]
        fg, bg = desc
        x, y, w, h = d
        if w <= 0 or h <= 0:
            return
        m = src[r[1]:r[1] + h, r[0]:r[0] + w] != 0
        out = dst[y:y + h, x:x + w]
        if not fg.anchored:
            fo = [0, 0]
        if fg.tex is not None:
            t = _tile(fg.tex, fo, w, h)
            if bg.tex is None and bg.colour == TRANSPARENT:
                out[m] = t[m]                      # blit_mask_texture_*_transparent
            else:
                out[:] = np.where(m, t, 0)         # blit_mask_texture_*: clear pixels black (the BG pass follows)
        else:
            behind = bg.colour if bg.tex is None else 0
            if behind == TRANSPARENT:
                out[m] = fg.colour                 # blit_mask_fg_only
            else:
                out[:] = np.where(m, fg.colour, behind)    # blit_mask_2colour
        if not bg.anchored:
            bo = [0, 0]
        if bg.tex is not None:
            out[~m] = _tile(bg.tex, bo, w, h)[~m]  # blit_bg_texture_*
        elif fg.tex is not None and bg.colour != TRANSPARENT:
            out[~m] = bg.colour                    # blit_bg_solid

    def blit_effect(self, src, dst, word, r, pos):
        """glyph_blit_effect (0x40820): phase 0, the rect's height as the cell height, and pos as both texture
        origins, so textures are fixed to the panel (destination pixel (X, Y) shows tex[Y mod h][X mod w])."""
        r = list(r)
        h = r[3] & 0xFFFF
        dsc = self.desc(word, 0, h - 0x10000 if h & 0x8000 else h)
        if dsc:
            self.blit(src, dst, dsc, r, pos, pos, pos)

    def draw_char(self, dst, pos, fg_origin, bg_origin, font, ch, phase, part, word):
        """font_draw_char_effect (0x43118): `part` of glyph ch (a rect inside its cell) at pos, the textures
        anchored at fg_origin / bg_origin, the font's cell height for 0x8001."""
        o1 = (pos[0] - fg_origin[0], pos[1] - fg_origin[1])
        o2 = (pos[0] - bg_origin[0], pos[1] - bg_origin[1])
        r = [(ch & 15) * font.cw + part[0], (ch >> 4) * font.ch + part[1], part[2], part[3]]
        dsc = self.desc(word, phase & 0xFFFF, font.ch & 0xFFFF)
        if dsc:
            self.blit(font.ink, dst, dsc, r, pos, o1, o2)


def rect_clip(bounds, r):
    """rect_clip (0x3E6E0): clip r (a list) to bounds in place. Edges that only touch still count."""
    bx, by, bw, bh = bounds
    if not (r[1] <= by + bh and by <= r[1] + r[3] and r[0] <= bx + bw and bx <= r[0] + r[2]):
        return False
    if r[0] < bx:
        r[2], r[0] = r[0] + r[2] - bx, bx
    if bx + bw < r[0] + r[2]:
        r[2] = bx + bw - r[0]
    if r[1] < by:
        r[3], r[1] = r[1] + r[3] - by, by
    if by + bh < r[1] + r[3]:
        r[3] = by + bh - r[1]
    return True


def _tile(tex, origin, w, h):
    """The w x h block of a repeated texture starting at origin (each coordinate taken as unsigned 32-bit)."""
    import numpy as np
    th, tw = tex.shape
    rows = ((origin[1] & U32) % th + np.arange(h)) % th
    cols = ((origin[0] & U32) % tw + np.arange(w)) % tw
    return tex[rows][:, cols]


def outline_blit(src, sr, dst, pos, style, colour, clip):
    """glyph_outline_blit (0x3FF0C): an outline around the mask pixels (!= 0) of rect sr of src, drawn into dst
    with sr's corner at pos, in colour_index_to_rgb555(colour). Rows outside clip are not written; columns are
    limited only at clip's left and right edge. Styles: 1 Thick (8 neighbours), 2 Thin (4 neighbours),
    3 Drop Shadow (one pixel right and down); 0 and others draw nothing. Run by run, as the firmware does:
    a pixel is outlined where a row's run of mask pixels starts and ends, and above and below each of them."""
    c = colour_index_to_rgb555(colour)
    sf, df = src.reshape(-1), dst.reshape(-1)
    dw = dst.shape[1]
    sw = src.shape[1]
    px, py = pos
    xr, yb = clip[0] + clip[2] - 1, clip[1] + clip[3] - 1

    def put(x, y):
        i = y * dw + x
        if 0 <= i < len(df):                       # (the firmware would write outside the image)
            df[i] = c

    def ink(u, v):
        i = (sr[1] + v) * sw + sr[0] + u
        return 0 <= i < len(sf) and sf[i] != 0

    inr = lambda y: clip[1] <= y <= yb
    for v in range(max(sr[3], 0)):
        Y = v + py
        up, mid, dn = inr(Y - 1), inr(Y), inr(Y + 1)
        last = sr[2] - 1
        if style == 1 and 0 <= Y <= yb:
            inside = False
            for u in range(sr[2]):
                X = u + px
                notright, atleft = X < xr, X <= clip[0]
                if inside:
                    if not ink(u, v):
                        inside = False
                        if notright:
                            if up: put(X, Y - 1)
                            if mid: put(X, Y)
                            if dn: put(X, Y + 1)
                    else:
                        if up: put(X, Y - 1)
                        if dn: put(X, Y + 1)
                elif ink(u, v):
                    inside = True
                    if up:
                        put(X, Y - 1)
                        if not atleft: put(X - 1, Y - 1)
                    if mid and not atleft: put(X - 1, Y)
                    if dn:
                        if not atleft: put(X - 1, Y + 1)
                        put(X, Y + 1)
                if u == last and inside and notright:
                    if up: put(X + 1, Y - 1)
                    if mid: put(X + 1, Y)
                    if dn: put(X + 1, Y + 1)
        elif style == 2 and 0 <= Y <= yb:
            inside = False
            for u in range(sr[2]):
                X = u + px
                atright = xr <= X
                if inside:
                    if not ink(u, v):
                        inside = False
                        if mid and not atright: put(X, Y)
                    else:
                        if up: put(X, Y - 1)
                        if dn: put(X, Y + 1)
                elif ink(u, v):
                    inside = True
                    if up: put(X, Y - 1)
                    if mid and not X <= clip[0]: put(X - 1, Y)
                    if dn: put(X, Y + 1)
                if u == last and inside and mid and not atright:
                    put(X + 1, Y)
        elif style == 3 and Y <= yb:
            state = 0
            for u in range(sr[2]):
                X = u + px
                atright = xr <= X
                if state == 0:
                    state = int(ink(u, v))
                else:
                    state = int(ink(u, v))
                    if dn and not atright: put(X, Y + 1)
                if u == last and state and dn and not atright:
                    put(X + 1, Y + 1)


def draw_text_item(fx, frame, mask, word, window, style=0, colour=0):
    """A text item as display_main_loop draws a pool's text (e.g. 0x7484..0x7500): the background over the
    window (glyph_blit_effect of an empty mask with the whole word), the outline around the text mask, then the
    letters (glyph_blit_effect with the BG half forced to 0x8000). `mask` is the panel-sized text image (the
    text drawn by text_draw_proportional_plain), `window` the item's rect (x, y, w, h) on the panel."""
    import numpy as np
    W, H = frame.shape[1], frame.shape[0]
    fx.blit_effect(np.zeros_like(mask), frame, word, window, window[:2])
    outline_blit(mask, window, frame, window[:2], style, colour, (0, 0, W, H))
    fx.blit_effect(mask, frame, (word & 0xFFFF0000) | 0x8000, window, window[:2])

if __name__ == "__main__":
    data = open(sys.argv[1], "rb").read()
    fl = FLIC(data)
    frames = fl.frames_rgb()
    print(f"{sys.argv[1]}: {fl.w}x{fl.h}, {len(frames)} frames decoded")
    out = sys.argv[2] if len(sys.argv) > 2 else "/tmp/out.gif"
    frames_to_gif(frames, out, scale=10)
    print("wrote", out)

def sign_frame_ms(speed_ms):
    """How long the sign really shows each animation frame: the player holds frames for a whole number of
    25 ms display frames (40 fps), picked from its threshold table (38, 63, 88, ... ms; ELITE_RGB 0x41C58)."""
    if speed_ms <= 38:
        return 25
    return 25 * min(41, 1 + -(-(speed_ms - 38) // 25))
