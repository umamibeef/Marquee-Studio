"""The sign's fonts (content/fonts/fonteNN.png) as the firmware builds and measures them (sign_emu.FwFont)."""
import os

from . import paths
from .font_render import SignFont


def _font(name):
    return SignFont(os.path.join(paths.CONTENT, "fonts", name))

def _fwfont(name):
    from .sign_emu import FwFont
    for f in sign_fonts():
        if f.name == name:
            return f
    return FwFont(os.path.join(paths.CONTENT, "fonts", name), name=name)

_FONTS = {}


def font_path(name):
    return os.path.join(paths.CONTENT, "fonts", name)


def sign_font_names():
    """The fonts as our firmware numbers them: the sign's own fonte00..99.png (fonts_load), then our MQF fonts by
    name (firmware/ext/fonts.c loads fonts.lst, which the image build writes in this order)."""
    import re
    d = os.listdir(os.path.join(paths.CONTENT, "fonts"))
    keep = stock_kept()
    dropped = {f"fonte{n:02d}.png" for n in range(1, 9)} - keep   # the original content's fonts the image leaves out
    return sorted(f for f in d if re.fullmatch(r"fonte\d\d\.png", f) and f not in dropped) + \
        sorted(f for f in d if f.endswith(".mqf"))


def stock_kept():
    """The sign's own fonteNN.png our image keeps (firmware/stock_fonts.txt)."""
    with open(os.path.join(paths.REPO, "firmware", "stock_fonts.txt"), encoding="utf-8") as f:
        return {ln.split("#", 1)[0].strip() for ln in f} - {""}


def sign_fonts():
    """FwFont for each sign font (cached by file time)."""
    from .sign_emu import FwFont
    out = []
    for n in sign_font_names():
        p = font_path(n)
        key = (n, os.path.getmtime(p))
        if key not in _FONTS:
            if n.endswith(".mqf"):
                from .sign_emu import fwfont_mqf
                _FONTS[key] = fwfont_mqf(p, name=n)
            else:
                _FONTS[key] = FwFont(p, name=n)
        out.append(_FONTS[key])
    return out


def auto_fonts():
    """The fonts our firmware may pick by itself (firmware/auto_fonts.txt, fonts.auto in the image), as a set."""
    p = os.path.join(paths.REPO, "firmware", "auto_fonts.txt")
    if not os.path.exists(p):
        return set()
    with open(p, encoding="utf-8") as f:
        return {ln.strip() for ln in f if ln.strip() and not ln.startswith("#")}


def default_font(H, fonts=None):
    """The font textline_init picks for an H-high line (font_pick_largest, from the auto fonts)."""
    from .sign_emu import font_pick_largest
    fonts = sign_fonts() if fonts is None else fonts
    i = font_pick_largest(fonts, H, auto_fonts())
    return None if i is None else fonts[i].name
