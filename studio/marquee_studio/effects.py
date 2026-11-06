"""Effect words (FG<<16 | BG), effect animation slots, outline styles and colours."""
import os

from . import paths
from . import sign_emu as emu
from .palette import PALETTE


# FG/BG effect codes as the firmware stores them ("Pool,%d,... Effect FG/BG", packed as FG<<16 | BG), drawn by
# sign_emu.Effects exactly as ELITE_RGB draws them (checked by test_effects.py; docs/EFFECTS-AND-LAYERS.md):
#   bit15 clear  -> solid RGB555 colour
#   0x8000       -> transparent; 0x8001 -> red/green/yellow wipe (the two specials the sign's FG/BG lists offer)
#   0x8002, 0xBFFE, 0xBFFF -> stripes, colour cycle: drawn by the firmware but not selectable on the sign
#   0xC000 | n   -> effect animation slot n, loaded from effectNN.flc (effects_load_slots 0x406DC)
SPECIAL_EFFECTS = {"Transparent": 0x8000, "Red/green/yellow wipe": 0x8001}
OUTLINE_STYLES = emu.OUTLINE_STYLES
OUTLINE_COLOURS = emu.OUTLINE_COLOURS


def effect_code(sel):
    """Firmware FG/BG code for a colour name, a special effect name, an effectNN.flc name or a hex code
    ("0x8002"); None if not expressible."""
    if not sel:
        return 0
    if isinstance(sel, int):
        return sel & 0xFFFF
    if sel in PALETTE:
        return PALETTE[sel]
    if sel in SPECIAL_EFFECTS:
        return SPECIAL_EFFECTS[sel]
    if sel.startswith("effect") and sel.endswith(".flc") and sel[6:-4].isdigit() and int(sel[6:-4]) < 100:
        return 0xC000 | int(sel[6:-4])
    if sel.lower().startswith("0x"):
        try:
            return int(sel, 16) & 0xFFFF
        except ValueError:
            return None
    return None


_SLOTS = {}


def effect_slots():
    """{n: effectNN.flc bytes} for the content folder, as effects_load_slots finds them (cached by file time)."""
    import re
    out = {}
    for name in os.listdir(os.path.join(paths.CONTENT, "animations")):
        if re.fullmatch(r"effect\d\d\.flc", name):
            p = os.path.join(paths.CONTENT, "animations", name)
            key = (name, os.path.getmtime(p))
            if key not in _SLOTS:
                _SLOTS[key] = open(p, "rb").read()
            out[int(name[6:8])] = _SLOTS[key]
    return out

def _outline(spec):
    """(style, colour index) from the spec's `outline` colour and `outline_style`. The sign's outline colours
    are the 8 of colour_index_to_rgb555; another studio colour name takes the nearest of them."""
    col = spec.get("outline", "Off")
    if col in (None, "", "Off", "None"):
        return 0, 0
    if isinstance(col, int):
        idx = col
    elif col in OUTLINE_COLOURS:
        idx = OUTLINE_COLOURS.index(col)
    else:
        v = PALETTE.get(col, 0)
        rgb = lambda c: ((c >> 10) & 31, (c >> 5) & 31, c & 31)
        idx = min(range(8), key=lambda i: sum((p - q) ** 2 for p, q in
                                               zip(rgb(v), rgb(emu.colour_index_to_rgb555(i)))))
    st = spec.get("outline_style", "Thick")
    st = OUTLINE_STYLES.index(st) if st in OUTLINE_STYLES else int(st)
    return st, idx
