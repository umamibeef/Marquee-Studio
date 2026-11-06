#!/usr/bin/env python3
"""Build a content folder for demos and screenshots, with nothing taken from the sign.

Fonts are converted from the vendored free fonts (third_party/fonts, see THIRD-PARTY.md), and the animations are
drawn here in code and converted to FLC. Emoji need no font: they are native emoji (marquee_studio.native_emoji).
Point the studio at it with MARQUEE_STUDIO_CONTENT (frontend/scripts/readme-demo.mjs does this).

Usage: demo_content.py OUT_DIR
"""
import colorsys
import math
import os
import random
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
from PIL import Image  # noqa: E402
from marquee_studio import font_import, gif2flc  # noqa: E402

W, H, FRAMES = 80, 16, 24
FONTS = ["m5x7/m5x7.ttf", "spleen/spleen-6x12.bdf", "monogram/monogram.ttf", "jersey10/Jersey10-Regular.ttf"]


def rainbow(t):
    return [Image.frombytes("RGB", (W, H), bytes(
        c for y in range(H) for x in range(W)
        for c in (int(v * 255) for v in colorsys.hsv_to_rgb(((x + 2 * y) / W + t / FRAMES) % 1, 1, 1))))]


def sparkle(t, rnd=random.Random(7)):
    img = Image.new("RGB", (W, H), (10, 0, 30))
    px = img.load()
    for _ in range(90):
        x, y = rnd.randrange(W), rnd.randrange(H)
        b = rnd.choice((120, 200, 255))
        px[x, y] = (b, b, rnd.choice((60, 255)))
    return [img]


def plasma(t):
    img = Image.new("RGB", (W, H))
    px = img.load()
    for y in range(H):
        for x in range(W):
            v = math.sin(x / 6 + t / 3) + math.sin(y / 3 + t / 4) + math.sin((x + y) / 8 + t / 5)
            r, g, b = colorsys.hsv_to_rgb((v / 6 + 0.5 + t / FRAMES) % 1, 0.9, 1)
            px[x, y] = (int(r * 255), int(g * 255), int(b * 255))
    return [img]


def bars(t):
    img = Image.new("RGB", (W, H))
    px = img.load()
    for y in range(H):
        for x in range(W):
            on = ((x + t * 2) // 5) % 2
            px[x, y] = (255, 170 if on else 40, 0) if on else (60, 0, 0)
    return [img]


ANIMATIONS = {"effect00.flc": rainbow, "effect01.flc": sparkle, "effect02.flc": plasma, "effect03.flc": bars}


def build(out):
    fonts = os.path.join(out, "fonts")
    anims = os.path.join(out, "animations")
    os.makedirs(fonts, exist_ok=True)
    os.makedirs(anims, exist_ok=True)
    for i, rel in enumerate(FONTS, 1):
        font_import.import_font(rel, os.path.join(fonts, f"fonte{i:02d}.png"))
    with tempfile.TemporaryDirectory() as tmp:
        for name, draw in ANIMATIONS.items():
            frames = [f for t in range(FRAMES) for f in draw(t)]
            gif = os.path.join(tmp, name + ".gif")
            frames[0].save(gif, save_all=True, append_images=frames[1:], duration=80, loop=0)
            gif2flc.convert(gif, os.path.join(anims, name))
    return out


if __name__ == "__main__":
    if len(sys.argv) != 2:
        raise SystemExit(__doc__)
    print("demo content ->", build(sys.argv[1]))
