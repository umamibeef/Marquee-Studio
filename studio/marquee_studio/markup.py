"""Message-line markup (docs/MESSAGE-MARKUP.md): encode, decode, warnings, and rendering as the sign does."""
from .palette import _colour
from .fonts import sign_fonts, default_font


# ---- Message line markup (docs/MESSAGE-MARKUP.md) --------------------------------------------------------
# A message line is text with ESC commands; the firmware shows it as segments that slide in, hold and slide out.
# The studio describes a line as a list of segments, each fully specified:
#   {"text", "fg", "bg" (RGB555 or a PALETTE name), "entry" (MARKUP_ENTRY), "entry_speed" (px/s, 1..255),
#    "hold" (0 = scroll right through; n = stop, hold n*4 frames = n/10 s), "exit" (MARKUP_EXIT), "exit_speed",
#    "font" (fonteNN.png), "centre" (True: proportional + centred, ESC ` FF; False: left-aligned)}
# The font only applies to left-aligned segments: for a centred one the firmware picks the font itself (the
# largest that fits the line, sign_emu.font_pick_for_text), so normalised/decoded centred segments have font None.
# or {"include": n} (show message line n here). Missing fields carry over from the segment before
# (textline_init's values for the first). encode_markup emits only what changes, modelling the firmware state;
# decode_markup reads bytes back the way message_text_next_token does.
MARKUP_ENTRY = {"right": 0x40, "left": 0x41, "bottom": 0x42, "top": 0x43, "top-left": 0x44}
MARKUP_EXIT = {"left": 0x50, "right": 0x51, "up": 0x52, "down": 0x53}
MARKUP_DEFAULT = {"fg": 0x7C00, "bg": 0, "entry": "right", "entry_speed": 40, "hold": 0, "exit": "left",
                  "exit_speed": 40, "font": None, "centre": False}
MARKUP_FIELDS = ("text", "fg", "bg", "entry", "entry_speed", "hold", "exit", "exit_speed", "font", "centre")
ESC_, CR_ = 0x1B, 0x0D
HOLD_FOREVER = -1        # decode_markup: a hold counter left at 0xFFFFFFFF by the segment before

def markup_normalise(segments, H=16, fonts=None):
    """Every field filled in: carried over from the segment before, colours as RGB555, the default font
    resolved, and exit/exit_speed None for scroll-through segments (hold 0, where the exit is never used)."""
    cur = dict(MARKUP_DEFAULT, font=default_font(H, fonts))
    out = []
    for s in segments:
        if "include" in s:
            out.append({"include": int(s["include"])})
            continue
        cur = {**cur, **{k: v for k, v in s.items() if k in MARKUP_FIELDS and v is not None}}
        seg = {k: cur.get(k) for k in MARKUP_FIELDS}
        seg.update(fg=_colour(seg["fg"]), bg=_colour(seg["bg"]), centre=bool(seg["centre"]),
                   entry_speed=int(seg["entry_speed"]), hold=int(seg["hold"]), exit_speed=int(seg["exit_speed"]))
        if seg["hold"] == 0:
            seg["exit"] = seg["exit_speed"] = None
        if seg["centre"]:
            seg["font"] = None
        out.append(seg)
    return out


def _text_bytes(text, font, native=False):
    if native:                                   # firmware with native emoji: emoji as UTF-8 (native_emoji.py)
        from . import native_emoji
        data = native_emoji.encode(text)
        if b"?" in data and "?" not in text:
            raise ValueError(f"{text!r}: a character is neither in the sign's fonts nor in its emoji set")
    else:
        try:
            data = text.encode("latin-1")
        except UnicodeEncodeError:
            raise ValueError(f"{text!r}: only characters 0..255 exist in the sign's fonts")
    if not data:
        raise ValueError("a segment needs some text")
    if set(data) & {0x00, CR_, ESC_}:
        raise ValueError(f"{text!r}: NUL, CR and ESC end a segment; split it into segments instead")
    return data


def encode_markup(segments, H=16, fonts=None, native=False):
    """Segments -> the message line bytes (no trailing NUL). Raises ValueError for what the sign cannot show."""
    fonts = sign_fonts() if fonts is None else fonts
    names = [f.name for f in fonts]
    segs = markup_normalise(segments, H, fonts)
    dflt = default_font(H, fonts)
    cur = {"fg": 0x7C00, "bg": 0, "entry_dir": 0, "entry_speed": 40, "exit_speed": 40, "hold": 0,
           "font": names.index(dflt) if dflt else None, "prop": 0}         # textline_init
    out = bytearray()
    for i, s in enumerate(segs):
        if "include" in s:
            if not 0 <= s["include"] <= 16:
                raise ValueError("include: message lines are 0..16")
            out += bytes([ESC_, 0x80, s["include"]])
            cur = {}                                     # unknown: the included line may change anything
            continue
        for k in ("entry_speed", "exit_speed"):
            if s[k] is not None and not 1 <= s[k] <= 255:
                raise ValueError(f"{k} must be 1..255 px/s")
        if not 0 <= s["hold"] <= 255:
            raise ValueError("hold must be 0..255 (x 4 frames)")
        if s["entry"] not in MARKUP_ENTRY or (s["hold"] and s["exit"] not in MARKUP_EXIT):
            raise ValueError(f"unknown entry/exit {s['entry']}/{s['exit']}")
        if not s["centre"] and s["font"] not in names:
            raise ValueError(f"font {s['font']} is not on the sign ({', '.join(names)})")
        cmd = bytearray()
        for k, c in (("fg", 0x30), ("bg", 0x31)):
            if cur.get(k) != s[k]:
                cmd += bytes([ESC_, c, s[k] >> 8, s[k] & 0xFF]); cur[k] = s[k]
        if s["centre"]:
            if cur.get("prop") != 1:
                cmd += bytes([ESC_, 0x60, 0xFF]); cur["prop"] = 1
            cur["font"] = None                           # the firmware replaces it with its own pick
        else:
            n = names.index(s["font"])
            if cur.get("font") != n or cur.get("prop") != 0:
                cmd += bytes([ESC_, 0x60, n]); cur.update(font=n, prop=0)
        code = MARKUP_ENTRY[s["entry"]]
        d = min(code - 0x40, 3)
        if code == 0x44 or cur.get("entry_dir") != d or cur.get("entry_speed") != s["entry_speed"]:
            cmd += bytes([ESC_, code, s["entry_speed"]]); cur.update(entry_dir=d, entry_speed=s["entry_speed"])
        if s["hold"]:                                    # the hold is used up by each segment: always set it
            cmd += bytes([ESC_, MARKUP_EXIT[s["exit"]], s["hold"], s["exit_speed"]])
            cur.update(hold=-1, exit_speed=s["exit_speed"])
        elif cur.get("hold") != 0:                       # back to scrolling through after a held segment
            sp = cur.get("exit_speed") or 40
            cmd += bytes([ESC_, 0x50, 0, sp]); cur.update(hold=0, exit_speed=sp)
        if i and not cmd:
            cmd = bytearray([CR_])                       # CR ends the segment before (and is drawn, blank)
        out += cmd + _text_bytes(s["text"], s["font"], native)
    if len(out) > 255:
        raise ValueError(f"{len(out)} bytes: a message line holds 255 (+ NUL)")
    return bytes(out)



def _auto():
    """the fonts our firmware's automatic picks may choose (fonts.auto_fonts)"""
    from .fonts import auto_fonts
    return auto_fonts()

def decode_markup(data, H=16, fonts=None, r24=0, W=80):
    """Line bytes -> segments, as message_text_next_token reads them (includes are listed, not followed).
    A segment's hold is HOLD_FOREVER when the segment before held and nothing set the hold again."""
    from .sign_emu import font_pick_for_text, TEXT_IMG_W
    fonts = sign_fonts() if fonts is None else fonts
    names = [f.name for f in fonts]
    st = {"fg": 0x7C00, "bg": 0, "entry_dir": 0, "entry_speed": 40, "exit_dir": 0, "exit_speed": 40, "hold": 0,
          "font": default_font(H, fonts), "prop": 0}
    out, i, flag = [], 0, r24
    data = bytes(data)
    while i < len(data) and data[i] != 0:
        b = data[i]
        if b == ESC_:
            cmd, a = data[i + 1:i + 2], data[i + 2:i + 4].ljust(2, b"\0")
            cmd = cmd[0] if cmd else 0
            if cmd in (0x30, 0x31):
                st["fg" if cmd == 0x30 else "bg"] = a[0] << 8 | a[1]; i += 4
            elif 0x40 <= cmd <= 0x44:
                st.update(entry_dir=min(cmd - 0x40, 3), entry_speed=a[0]); flag |= cmd == 0x44; i += 3
            elif 0x50 <= cmd <= 0x54:
                st.update(exit_dir=min(cmd - 0x50, 3), hold=a[0] << 2, exit_speed=a[1]); flag |= cmd == 0x54; i += 4
            elif cmd == 0x60:
                if a[0] == 0xFF:
                    st["prop"] = 1
                elif a[0] < len(names):
                    st.update(font=names[a[0]], prop=0)
                i += 3
            elif cmd == 0x80:
                out.append({"include": a[0]}); i += 3
            else:
                i += 2
            continue
        j = i + 1
        while j < len(data) and data[j] not in (0, CR_, ESC_):
            j += 1
        raw = data[i:j]
        if st["prop"]:                                   # the firmware's font pick, on the first character
            k = font_pick_for_text(fonts, raw[:1], *((TEXT_IMG_W, H) if st["entry_dir"] < 2 else (W, H)), auto=_auto())
            st["font"] = names[k] if k is not None else st["font"]
        raw = raw[1:] if raw[:1] == bytes([CR_]) else raw
        text = raw.decode("latin-1")
        hold = HOLD_FOREVER if st["hold"] == 0xFFFFFFFF else st["hold"] >> 2
        entry = ["right", "left", "bottom", "top"][st["entry_dir"]]
        if entry == "top" and flag:
            entry = "top-left"
        seg = {"text": text, "fg": st["fg"], "bg": st["bg"], "entry": entry, "entry_speed": st["entry_speed"],
               "hold": hold, "exit": ["left", "right", "up", "down"][st["exit_dir"]] if hold else None,
               "exit_speed": st["exit_speed"] if hold else None, "font": None if st["prop"] else st["font"],
               "centre": bool(st["prop"])}
        out.append(seg)
        if st["hold"]:
            st["hold"] = 0xFFFFFFFF                      # textline_step counts it down past 0
        flag, i = r24, j
    return out


def markup_warnings(data, W, H, fonts=None, r24=0):
    """What will not look the way it probably was meant to, from the firmware's own behaviour."""
    fonts = sign_fonts() if fonts is None else fonts
    by_name = {f.name: f for f in fonts}
    w = []
    segs = [s for s in decode_markup(data, H, fonts, r24, W) if "include" not in s]
    for n, s in enumerate(segs, 1):
        f = by_name.get(s["font"])
        if s["hold"] == HOLD_FOREVER:
            w.append(f"segment {n} holds for ever: the segment before held, and the hold counter is left at "
                     f"0xFFFFFFFF. Give it its own exit command (ESC P..T).")
        offpanel = (s["entry"] in ("bottom", "top") or s["hold"]) if not s["centre"] else s["entry"] == "top-left"
        if offpanel:
            w.append(f"segment {n} is not visible while it {'holds' if s['hold'] else 'slides'}: "
                     + ("left-aligned text is at the left edge of the 5140-px text image, and this slide centres "
                        "that image. Use 'centred', or entry top-left."
                        if not s["centre"] else "centred text with entry top-left puts the image at x = 0."))
        if s["entry"] == "top":
            w.append(f"segment {n}: ESC 'C' (from the top) reads an uninitialised register (r24); on the sign it "
                     f"may behave as top-left (ESC 'D'). The preview assumes r24 = {r24}.")
        if f and f.ch > H and not s["centre"]:
            hangs = any(f.prop[c] for c in s["text"].encode("latin-1", "replace"))
            w.append(f"segment {n}: {s['font']} is {f.ch} px high, taller than the panel. Left-aligned, "
                     + ("the firmware HANGS drawing it (draw_line never ends; the display freezes)."
                        if hangs else "it is not drawn."))
    if 0 in bytes(data):
        w.append("the bytes contain NUL (in command arguments). Fine in a message line; a copy that stops at "
                 "NUL on the way to the sign would cut it.")
    return w


def record_text_scrolls(data, win_w, win_h, fonts=None):
    """Does the sign run RS-485 record text through the markup tokenizer? display_main_loop picks a font for the
    WHOLE text (font_pick_for_text, ESC bytes counted as glyphs) in the winner window; only when even that is
    wider than the window does it copy the text into message line 16 and scroll it as markup (textline_init).
    Otherwise it draws the text plainly and the ESC bytes show as glyphs. Returns (scrolls, width, font name).
    The window is the valLocationWindow area; the studio takes the panel size (an assumption)."""
    from .sign_emu import font_pick_for_text
    fonts = sign_fonts() if fonts is None else fonts
    k = font_pick_for_text(fonts, bytes(data), win_w, win_h, auto=_auto())
    w = fonts[k].text_width(bytes(data))
    return win_w < w, w, fonts[k].name


def render_markup(data, W, H, max_frames=2400, r24=0):
    """Emulate a marked-up message line on the panel: (RGB555 frames, states), 40 fps."""
    from .sign_emu import textline_frames
    return textline_frames(bytes(data) + b"\0", sign_fonts(), W, H, max_frames=max_frames, r24=r24)
