"""The content folder (paths.CONTENT): assets, animations, imported fonts, the emoji catalogue, and what the UI lists."""
import glob
import os

from . import paths
from . import font_import
from .sign_emu import FLIC
from .palette import PALETTE
from .config import PROTOCOLS, DISPLAY_MODELS
from .effects import SPECIAL_EFFECTS, OUTLINE_STYLES, OUTLINE_COLOURS
from .amount import AMOUNT_MODES
from .fonts import sign_font_names
from .markup import MARKUP_ENTRY, MARKUP_EXIT


def _files(sub, ext):
    """Every file in content/<sub> with the extension (any case): nothing in the folder is hidden."""
    d = os.path.join(paths.CONTENT, sub)
    return sorted(f for f in (os.listdir(d) if os.path.isdir(d) else [])
                  if f.lower().endswith(ext) and not f.startswith(".") and os.path.isfile(os.path.join(d, f)))


def list_animations():
    return _files("animations", ".flc")


def list_fonts():
    """The sign's fonts in our firmware's order (fonts.sign_font_names)."""
    return sign_font_names()


def list_assets():
    anims = list_animations()
    fonts = list_fonts()
    return {"animations": anims, "fonts": fonts, "palette": PALETTE, "protocols": PROTOCOLS,
            "font_info": {f: font_credits(f) for f in fonts},
            "importable_fonts": font_import.available(),
            "effect_names": {a: n for a in anims if (n := _flc_author_name(a))},
            "animation_info": {a: animation_summary(a) for a in anims},
            "special_effects": SPECIAL_EFFECTS, "outline_styles": list(OUTLINE_STYLES[1:]),
            "outline_colours": list(OUTLINE_COLOURS), "amount_modes": AMOUNT_MODES,
            "sign_fonts": sign_font_names(), "markup_entry": list(MARKUP_ENTRY), "markup_exit": list(MARKUP_EXIT),
            "display_models": [{"type": t, "word": w, "name": n, "w": x, "h": y} for t, w, n, x, y in DISPLAY_MODELS]}


_ANIM_SUMMARY = {}


def animation_summary(name):
    """Header facts of one animation (cached by file time): size, depth, frames, speed, the effect slot it
    fills (0xC000 | n for effectNN.flc, else None: still usable as a fill or background, previewed in a free
    slot) and its authoring name."""
    from .effects import effect_code
    p = os.path.join(paths.CONTENT, "animations", name)
    key = (name, os.path.getmtime(p))
    if key not in _ANIM_SUMMARY:
        try:
            fl = FLIC(open(p, "rb").read())
            info = {"w": fl.w, "h": fl.h, "depth": fl.depth, "frames": fl.frames, "speed_ms": fl.speed_ms,
                    "sign_ms": sign_frame_ms(fl.speed_ms)}
        except Exception as e:                                   # a broken file is still listed
            info = {"error": str(e)}
        code = effect_code(name)
        info.update(author=_flc_author_name(name), slot=code if code is not None and code >= 0xC000 else None,
                    bytes=os.path.getsize(p))
        _ANIM_SUMMARY[key] = info
    return _ANIM_SUMMARY[key]


def _flc_author_name(name):
    """Original authoring file name from the FLC prefix chunk, e.g. 'GLT2-BG' (glitter, blue-green)."""
    import re
    try:
        head = open(os.path.join(paths.CONTENT, "animations", name), "rb").read(0x400)
    except OSError:
        return None
    m = re.search(rb"([A-Za-z0-9_-]+)\.FLC", head, re.I)
    return m.group(1).decode() if m else None


from marquee_pack.sign_emu import sign_frame_ms  # (moved to marquee_pack)


def anim_info(name):
    """Decoded frames plus the timing and format details the studio shows."""
    fl = FLIC(open(os.path.join(paths.CONTENT, "animations", name), "rb").read())
    frames = fl.frames_rgb()
    return {"frames": frames, "w": fl.w, "h": fl.h, "depth": fl.depth, "speed_ms": fl.speed_ms,
            "sign_ms": sign_frame_ms(fl.speed_ms)}


def safe_asset_name(name, ext):
    """A plain file name for the content folder: no directories, only [A-Za-z0-9_-.], forced extension."""
    import re
    base = os.path.basename(name or "")
    stem = re.sub(r"[^A-Za-z0-9_-]", "_", os.path.splitext(base)[0])[:24] or "anim"
    return stem + ext


def import_animation(src_path, name, W, H, depth=16, fit="contain"):
    """Convert a GIF/APNG/WebP to an FLC the sign can play (tools/gif2flc.py) into content/animations."""
    from . import gif2flc
    out_name = safe_asset_name(name, ".flc")
    info = gif2flc.convert(src_path, os.path.join(paths.CONTENT, "animations", out_name), (W, H), fit, depth)
    info["name"] = out_name
    return info


def font_credits(font):
    """Where a content font came from: {"origin": "sign" | "import", "shareable", "credits", ...}.
    Fonts without a sidecar are the sign's own (fonte01..08), which stay local and are never shared."""
    png = os.path.join(paths.CONTENT, "fonts", font)
    out = _font_origin(png)
    try:
        if font.endswith(".mqf"):                        # (widest advance, line height)
            from . import mqf
            lh, _, g = mqf.read(open(png, "rb").read())
            out["cell"] = [max([1] + [a for a, _ in g.values()]), lh]
        else:
            from PIL import Image
            w, h = Image.open(png).size
            out["cell"] = [w // 16, h // 16]
    except Exception as e:
        out["error"] = str(e)
    return out


def _font_origin(png):
    imp = font_import.font_info(png)
    if imp:
        return {"origin": "import", "source": imp["source"], "licence": imp["licence"], "shareable": True,
                "credits": imp["credits"]}
    return {"origin": "sign", "shareable": False, "credits": []}


def _next_font_name():
    taken = set(os.listdir(os.path.join(paths.CONTENT, "fonts")))
    for n in range(20, 100):
        if f"fonte{n:02d}.png" not in taken:
            return f"fonte{n:02d}.png"
    raise ValueError("no free font number")


def upload_font_name(filename):
    """The content-folder name for a dropped font PNG: kept when it is a free fonteNN.png (a sign font), else
    the next free fonteNN.png (the sign only loads fonte00..fonte99.png)."""
    import re
    base = os.path.basename(filename or "").lower()
    if re.fullmatch(r"fonte\d\d\.png", base) and not os.path.exists(os.path.join(paths.CONTENT, "fonts", base)):
        return base
    return _next_font_name()


def import_font(rel, upper="latin1"):
    """Import a vendored pixel font (third_party/fonts) as <its name>.mqf (our firmware's own font format)."""
    name = os.path.splitext(os.path.basename(rel))[0] + ".mqf"
    info = font_import.import_font(rel, os.path.join(paths.CONTENT, "fonts", name), upper)
    return name, info


# ---- The emoji picker: the emoji pack's pictures, or the committed native emoji sheet -------------------------
# Groups by code point range (an approximation of the Unicode emoji groups; no Unicode data file is vendored).
EMOJI_GROUPS = [
    ("Smileys", [(0x1F600, 0x1F64F), (0x1F910, 0x1F92F), (0x1F970, 0x1F97A), (0x1F9D0, 0x1F9D0), (0x263A, 0x263A),
                 (0x2639, 0x2639), (0x1FAE0, 0x1FAEF)]),
    ("People", [(0x1F466, 0x1F487), (0x1F44A, 0x1F450), (0x1F930, 0x1F93F), (0x1F9D1, 0x1F9DF), (0x1F574, 0x1F57A),
                (0x1F590, 0x1F596), (0x270A, 0x270D), (0x261D, 0x261D), (0x1F440, 0x1F449), (0x1F9B0, 0x1F9BF),
                (0x1F6B4, 0x1F6B6), (0x1F90C, 0x1F90F), (0x1FAC0, 0x1FAC6), (0x1F3C2, 0x1F3CC), (0x1FAF0, 0x1FAFF), (0x1F918, 0x1F91F)]),
    ("Animals & nature", [(0x1F400, 0x1F43F), (0x1F330, 0x1F343), (0x1F98A, 0x1F9AF), (0x1F980, 0x1F989),
                          (0x1F331, 0x1F33F), (0x1FAB0, 0x1FABF), (0x1FACD, 0x1FACF), (0x2618, 0x2618), (0x1F577, 0x1F578)]),
    ("Food & drink", [(0x1F344, 0x1F37F), (0x1F950, 0x1F96F), (0x1F9C0, 0x1F9CF), (0x1FAD0, 0x1FADF), (0x2615, 0x2615)]),
    ("Activities", [(0x1F380, 0x1F3C1), (0x1F3CD, 0x1F3FA), (0x1F93A, 0x1F94F), (0x26BD, 0x26BE), (0x26F3, 0x26F3),
                    (0x1F3B0, 0x1F3B0), (0x2660, 0x2667), (0x1F0CF, 0x1F0CF), (0x1F004, 0x1F004), (0x1FA80, 0x1FA8F)]),
    ("Travel & places", [(0x1F680, 0x1F6FF), (0x1F300, 0x1F32F), (0x1F5FA, 0x1F5FF), (0x26F0, 0x26FF), (0x2600, 0x2604)]),
    ("Objects", [(0x1F451, 0x1F465), (0x1F97B, 0x1F97F), (0x1F488, 0x1F4FF), (0x1F500, 0x1F573), (0x1F57B, 0x1F5F9), (0x231A, 0x23FF), (0x1F9E0, 0x1F9FF),
                 (0x1FA70, 0x1FA7F), (0x1FA90, 0x1FAAF), (0x2702, 0x2709)]),
    ("Symbols", [(0x2190, 0x21FF), (0x2700, 0x27BF), (0x2B00, 0x2BFF), (0x2600, 0x26FF), (0x3030, 0x303D),
                 (0x00A9, 0x00AE), (0x1F7E0, 0x1F7F0), (0x2122, 0x2139), (0x1F170, 0x1F251)]),
    ("Flags", [(0x1F1E6, 0x1F1FF), (0x1F3F4, 0x1F3F4), (0x1F6A9, 0x1F6A9), (0x1F3C1, 0x1F3C1)]),
]
# emoji newer than this Python's Unicode data
_NEWER_NAMES = {0x1FACE: "MOOSE", 0x1FACF: "DONKEY"}
_EMOJI = None


def _emoji_group(cps):
    if len(cps) == 2 and all(0x1F1E6 <= c <= 0x1F1FF for c in cps) or cps[0] == 0x1F3F4:
        return "Flags"
    if any(0x1F3FB <= c <= 0x1F3FF for c in cps) or 0x200D in cps and cps[0] in range(0x1F466, 0x1F9FF):
        return "People"
    c = cps[0]
    for g, ranges in EMOJI_GROUPS:
        if any(a <= c <= b for a, b in ranges):
            return g
    return "Symbols" if c < 0x1F000 else "Other"


def _emoji_name(cps):
    import unicodedata
    if len(cps) == 2 and all(0x1F1E6 <= c <= 0x1F1FF for c in cps):
        return "flag " + "".join(chr(c - 0x1F1E6 + ord("A")) for c in cps)
    names = []
    for c in cps:
        if c in (0x200D, 0xFE0F, 0x20E3):
            continue
        if 0x1F3FB <= c <= 0x1F3FF:
            names.append(["light", "medium-light", "medium", "medium-dark", "dark"][c - 0x1F3FB] + " skin tone")
            continue
        n = unicodedata.name(chr(c), "")
        if n.startswith("TAG "):
            continue
        n = n or _NEWER_NAMES.get(c, "")
        names.append(n.lower() if n else f"U+{c:04X}")
    return ", ".join(names)


def emoji_catalog():
    """[{"e": the emoji string, "cps": "1F600", "name", "group", "file"}]. With the emoji pack on this machine
    (paths.EMOJI_PACK: JOI3's Flat Pixel Art Emojis, bought and not redistributed; the art the sign's native emoji
    are made from), every picture of the pack, file "pack/<id>.png". Without it, the emoji of the committed native
    emoji sheet (firmware/emoji/<size>/emoji.bin), file "sheet/<index>.png"."""
    global _EMOJI
    if _EMOJI is None:
        from . import native_emoji
        if native_emoji.PACK:
            pictures = [(e, f"pack/{i}.png") for e, i in native_emoji.PACK.items()]
        else:
            pictures = [(e, f"sheet/{n}.png") for n, e in enumerate(native_emoji._committed(native_emoji.SIZE))]
        out = []
        for e, f in pictures:
            cps = [ord(c) for c in e]
            out.append({"e": e, "cps": " ".join(f"{c:X}" for c in cps), "name": _emoji_name(cps),
                        "group": _emoji_group(cps), "file": f})
        _EMOJI = out
    return _EMOJI


def sheet_emoji_png(n, size=None):
    """PNG bytes of picture n of the committed native emoji sheet (emoji_catalog without the pack), or None."""
    import io
    from . import native_emoji
    cells = list(native_emoji._committed(size or native_emoji.SIZE).values())
    if not 0 <= n < len(cells):
        return None
    buf = io.BytesIO()
    cells[n].save(buf, "PNG")
    return buf.getvalue()
