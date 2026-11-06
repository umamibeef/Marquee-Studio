#!/usr/bin/env python3
"""Demo: reproduce a sign sequence on the emulated 80x16 RGB555 panel and export a GIF.
  boot logo (real logo_s.flc) -> scrolling jackpot message (real font) with a flashing frame.
This is the seed of an authoring preview: author FLC + text + colors, see it as the sign would show it.
"""
import sys
sys.path.insert(0, ".")
from sign_emu import Panel, FLIC, rgb888_to_rgb555, frames_to_gif
from font_render import SignFont

EXT = "../analysis/extracted/"
AMBER = rgb888_to_rgb555(255, 150, 0)
RED = rgb888_to_rgb555(255, 30, 0)
GREEN = rgb888_to_rgb555(0, 255, 0)

W, H = 80, 16
out = []

# 1. boot logo (real animation)
fl = FLIC(open(EXT + "logo_s.flc", "rb").read())
out += fl.frames_rgb()

# 2. scrolling message
font = SignFont(EXT + "fonte01.png")   # smallest font, fits 16px panel
msg = "CONGRATULATIONS  JACKPOT WINNER  $1,000,000   "
mask = font.text_mask(msg, spacing=1)
mh, mw = mask.shape
y = max(0, (H - mh) // 2)
for shift in range(0, mw + W, 1):
    p = Panel(W, H)
    flash_on = (shift // 8) % 2 == 0
    col = AMBER if flash_on else RED
    for yy in range(mh):
        for xx in range(mw):
            if mask[yy, xx]:
                p.put(xx - shift + W, y + yy, col)
    out.append(p.to_rgb_rows())

frames_to_gif(out, "demo_sign.gif", scale=8, ms=55)
print(f"wrote demo_sign.gif: {len(out)} frames, panel {W}x{H}")
