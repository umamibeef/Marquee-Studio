"""The message renderer (firmware-exact text items: fonts, effects, outlines, native emoji) and frame packing."""
import base64
import io
import os

from PIL import Image

from . import paths
from . import sign_emu as emu
from . import native_emoji
from . import diag_patterns
from .sign_emu import FLIC, rgb888_to_rgb555, rgb555_to_rgb888
from .amount import amount_text
from .flash import flash_frames
from .effects import effect_code, effect_slots, _outline
from .fonts import _font, _fwfont


def panel_png(rows, scale):
    h = len(rows); w = len(rows[0])
    im = Image.new("RGB", (w, h)); im.putdata([px for r in rows for px in r])
    im = im.resize((w * scale, h * scale), Image.NEAREST)
    buf = io.BytesIO(); im.save(buf, "PNG")
    return "data:image/png;base64," + base64.b64encode(buf.getvalue()).decode()

def message_layout(spec, W, H):
    """What render_message draws: the effect word, outline, font, text bytes, emoji, slot contents and, per
    panel frame, the panel frame count and the text position (None while flashed off)."""
    import math
    text = spec.get("text", "")
    if spec.get("value") not in (None, ""):
        text = amount_text(spec["value"], spec.get("amount") or {}) + " " + text
    font_name = spec.get("font", "fonte01.png")
    ft = _fwfont(font_name)
    native = bool(spec.get("native_emoji"))       # the sign runs firmware with native emoji (native_emoji.py)
    esize = int(spec.get("emoji_size") or native_emoji.SIZE)   # the emoji sheet's cell: the panel height
    if native:
        data = native_emoji.encode(text)
        glyphs = native_emoji.parse(data, size=esize)
    else:
        data = bytes(ord(c) for c in text if ord(c) < 256)
        glyphs = None
    slots = effect_slots()
    codes = []
    for key, solid, default in (("foreground", "colour", "Amber"), ("background", "bg_colour", "Off")):
        sel = spec.get(key) or spec.get(solid, default)
        c = effect_code(sel)
        if c is None and isinstance(sel, str) and sel.endswith(".flc"):
            # not an effect slot (e.g. a plain .flc as the background): previewed as if loaded in a free slot
            n = next(n for n in range(99, -1, -1) if n not in slots)
            slots[n] = open(os.path.join(paths.CONTENT, "animations", os.path.basename(sel)), "rb").read()
            c = 0xC000 | n
        codes.append(0x7FE0 if c is None else c)
    word = codes[0] << 16 | codes[1]
    style, colour = _outline(spec)
    tw = native_emoji.layout(ft, glyphs, esize)[1] if native else ft.text_width(data)
    ty = emu.half(H - ft.ch)
    frame0 = int(spec.get("frame0", 1000)) & 0xFFFFFFFF
    flash_on, flash_off = flash_frames(spec)

    def flashed(step):
        if flash_on <= 0:
            return True
        return (step % (flash_on + max(1, flash_off))) < flash_on

    if spec.get("mode", "scroll") == "static":
        periods = [1]
        for c in codes:
            if c == 0x8001:
                periods.append(12 * H)
            elif c == 0x8002:
                periods.append(24)
            elif c in (0xBFFE, 0xBFFF):
                periods.append(48)
            elif c >= 0xC000 and (c & 0x3FFF) in slots:
                fl = FLIC(slots[c & 0x3FFF])
                periods.append(max(1, fl.frames) * (emu.flc_speed_to_hold(fl.speed_ms) + 1))
        if flash_on > 0:
            periods.append(flash_on + max(1, flash_off))
        n = 1
        for p in periods:
            n = n * p // math.gcd(n, p)
        n = n if n <= 480 else max(periods)
        x = emu.half(W - tw)
        pos = [(x, ty) for _ in range(n)]
    else:
        rate = max(1, int(spec.get("text_rate", 4)))
        pos = [(W - shift, ty) for shift in range(0, tw + W, rate)]
    pos = [p if flashed(k) else None for k, p in enumerate(pos)]
    return {"word": word, "style": style, "colour": colour, "font": ft, "data": data, "glyphs": glyphs, "emoji_size": esize,
            "slots": slots, "window": (0, 0, W, H),
            "frames": [((frame0 + k) & 0xFFFFFFFF, p) for k, p in enumerate(pos)]}


def _glyph_xs(ft, data):
    """x offset of each glyph as text_draw_proportional places it."""
    out, x, prev = [], 0, 1
    for c in data:
        p = ft.prop[c]
        if prev == 0 and p:
            x += 1
        out.append((c, x))
        x += ft.width[c] + (1 if p else 0)
        prev = p
    return out


def render_message555(spec, W, H, background=None):
    """Render a message spec to RGB555 frames (H x W arrays), one per 40 fps panel frame, the way the sign draws
    a pool's text item (sign_emu.draw_text_item):
      background - Effect BG over the whole panel: solid `bg_colour`, or `background` (an effectNN.flc slot,
                   "Transparent", "Red/green/yellow wipe", or a hex code)
      outline    - `outline` colour (Black, Red, Green, Yellow, Blue, Cyan, Magenta, White) in `outline_style`
                   (Thick, Thin, Drop Shadow)
      foreground - Effect FG in the letters: solid `colour`, or `foreground` as for the background
    Other keys: text, font, mode (scroll|static), text_rate (px per frame), flash_on, flash_off, value (a money
    amount, formatted as the sign does by amount_text with `amount`: {decimals, mode, dec, thou, prefix, suffix}),
    frame0 (the panel frame count at the first frame: the phase of 0x8001/0x8002/0xBFFE/0xBFFF).
    `background` (an array) is what the item is drawn over (default black); transparent pixels show it."""
    import numpy as np
    L = message_layout(spec, W, H)
    ft, data = L["font"], L["data"]
    fx = emu.Effects(L["slots"])
    if L["glyphs"] is not None:
        return _render_native(L, ft, fx, H, W, background)
    out = []
    for fc, p in L["frames"]:
        fx.frame_count = fc
        frame = np.zeros((H, W), np.uint16) if background is None else background.copy()
        mask = np.zeros((H, W), np.uint16)
        if p is not None:
            ft.draw(mask, p[0], p[1], data, 0x7FFF, 0)
        emu.draw_text_item(fx, frame, mask, L["word"], L["window"], L["style"], L["colour"])
        out.append(frame)
    return out


def _render_native(L, ft, fx, H, W, background):
    """render_message555 for firmware with native emoji: letters through the normal mask and effects, each emoji
    drawn afterwards in its own opaque RGB555 colours (native_emoji.pixels), as the firmware does."""
    import numpy as np
    placed, _ = native_emoji.layout(ft, L["glyphs"], L["emoji_size"])
    out = []
    for fc, p in L["frames"]:
        fx.frame_count = fc
        frame = np.zeros((H, W), np.uint16) if background is None else background.copy()
        mask = np.zeros((H, W), np.uint16)
        if p is not None:
            for g, gx, _ in placed:
                if g[0] == "c":
                    ft.draw(mask, p[0] + gx, p[1], bytes([g[1]]), 0x7FFF, 0)
        emu.draw_text_item(fx, frame, mask, L["word"], L["window"], L["style"], L["colour"])
        if p is not None:
            for g, gx, _ in placed:
                if g[0] != "e":
                    continue
                for dx, dy, px in native_emoji.pixels(g[1], ft.ch, size=L["emoji_size"]):
                    x, y = p[0] + gx + dx, p[1] + dy
                    if 0 <= x < W and 0 <= y < H:
                        frame[y, x] = px
        out.append(frame)
    return out


def render_message(spec, W, H):
    """render_message555 as RGB888 frame-rows (for pack_frames, GIF export)."""
    return [[[rgb555_to_rgb888(int(v)) for v in row] for row in fr] for fr in render_message555(spec, W, H)]


def _blit_text(frame, mask, ox, oy, col, out, W, H, effect=None, phase=0, fill=None):
    mh, mw = mask.shape
    if out != (0, 0, 0):   # outline
        for yy in range(mh):
            for xx in range(mw):
                if mask[yy, xx]:
                    for dx, dy in ((-1, 0), (1, 0), (0, -1), (0, 1)):
                        x, y = ox + xx + dx, oy + yy + dy
                        if 0 <= x < W and 0 <= y < H:
                            frame[y][x] = out
    for yy in range(mh):
        for xx in range(mw):
            if mask[yy, xx]:
                x, y = ox + xx, oy + yy
                if 0 <= x < W and 0 <= y < H:
                    if fill is not None:
                        frame[y][x] = fill(x, y, xx, yy)
                    elif effect is not None:
                        frame[y][x] = rgb555_to_rgb888(
                            diag_patterns.effect_color(int(effect), xx, yy, mw, mh, phase % 32))
                    else:
                        frame[y][x] = col


def effect_gallery(text, font_name, W, H, cols=1):
    """Render every effect (0..51) applied to `text`, one representative frame each. Returns list of
    dicts {idx, name, rows}."""
    font = _font(font_name)
    mask = font.text_mask(text, spacing=1)
    mh, mw = mask.shape
    pw = max(W, mw + 2)
    out = []
    for idx in range(diag_patterns.NUM_EFFECTS):
        frame = [[(0, 0, 0)] * pw for _ in range(H)]
        _blit_text(frame, mask, 1, max(0, (H - mh) // 2), (255, 180, 0), (0, 0, 0), pw, H, idx, 7)
        out.append({"idx": idx, "name": diag_patterns.EFFECT_NAMES.get(idx, f"#{idx}"), "rows": frame})
    return out


def pack_frames(frames_rgb):
    """Pack frames (list of rows of (r,g,b)) into base64 RGB555 (2 bytes/pixel, little-endian).
    This is the true panel colour; the browser expands each pixel to draw it on a canvas."""
    import base64, struct
    H = len(frames_rgb[0]); W = len(frames_rgb[0][0])
    buf = bytearray()
    for fr in frames_rgb:
        for row in fr:
            for (r, g, b) in row:
                buf += struct.pack("<H", rgb888_to_rgb555(r, g, b))
    return {"w": W, "h": H, "nframes": len(frames_rgb),
            "data": base64.b64encode(bytes(buf)).decode()}


def frames_to_gif_bytes(frames, scale=8, ms=60):
    imgs = []
    for fr in frames:
        h = len(fr); w = len(fr[0])
        im = Image.new("RGB", (w, h)); im.putdata([px for r in fr for px in r])
        imgs.append(im.resize((w * scale, h * scale), Image.NEAREST))
    buf = io.BytesIO()
    imgs[0].save(buf, "GIF", save_all=True, append_images=imgs[1:], duration=ms, loop=0)
    return buf.getvalue()

def pack_frames555(frames, ms=25):
    """pack_frames for RGB555 arrays (the emulator's own frames)."""
    import numpy as np
    H, W = frames[0].shape
    data = np.stack(frames).astype("<u2").tobytes()
    return {"w": W, "h": H, "nframes": len(frames), "ms": ms, "data": base64.b64encode(data).decode()}
