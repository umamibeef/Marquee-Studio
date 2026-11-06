#!/usr/bin/env python3
"""Golden fixtures for @marquee/sign-emu from the Python sign emulation (studio/marquee_studio/sign_emu.py), which is
checked against the firmware itself. Each case: a message line's bytes on a W x H panel -> the state and a SHA-256 of
every 40 fps frame (RGB555, little-endian). Run from the repository root:
    venv/bin/python frontend/packages/sign-emu/test/make_fixtures.py
"""
import hashlib, json, os, sys

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", "..", "..", ".."))
sys.path.insert(0, os.path.join(ROOT, "studio"))
from marquee_studio.fonts import sign_fonts, sign_font_names          # noqa: E402
from marquee_studio.sign_emu import textline_frames                   # noqa: E402

E = b"\x1b"
def entry(d, speed): return E + bytes([0x40 + d, speed])
def hold(e, frames4, speed): return E + bytes([0x50 + e, frames4, speed])
def font(n): return E + bytes([0x60, n])
def fg(v): return E + b"\x30" + v.to_bytes(2, "big")
def bg(v): return E + b"\x31" + v.to_bytes(2, "big")

CASES = [
    ("plain scroll", 200, 16, b"HELLO"),
    ("plain wider than the panel", 200, 16, b"59d 3h 22m and a lot more text that runs on"),
    ("countdown text", 200, 16, b"59d 3h 22m"),
    ("centred held", 200, 16, font(0xFF) + entry(0, 255) + hold(0, 10, 255) + b"UMAMI"),
    ("centred, entry bottom, exit up", 200, 16, font(0xFF) + entry(2, 255) + hold(2, 8, 255) + b"MARQUEE"),
    ("entry left, exit right", 200, 16, entry(1, 80) + hold(1, 4, 120) + b"LEFT"),
    ("entry top (r24 0)", 200, 16, font(0xFF) + entry(3, 200) + hold(3, 4, 200) + b"TOP"),
    ("entry top-left (ESC D)", 200, 16, entry(4, 200) + hold(4, 4, 200) + b"TOPLEFT"),
    ("colours", 200, 16, fg(0x03E0) + bg(0x0010) + b"GREEN ON BLUE"),
    ("font switch", 200, 16, font(0) + b"SMALL" + font(1) + b"BIGGER"),
    ("two segments with CR", 200, 16, font(0xFF) + entry(0, 255) + hold(0, 3, 255) + b"ONE\rTWO"),
    ("slow scroll", 80, 16, entry(0, 20) + b"SLOW"),
    ("narrow panel", 80, 16, b"80 PX"),
    ("tall panel", 120, 32, font(0xFF) + entry(0, 255) + hold(0, 5, 255) + b"TALL"),
    ("digits and space", 200, 16, b"0123 456 789"),
    ("speed 0 never moves", 200, 16, entry(0, 0) + b"STUCK"),
    ("font taller than the line (hang)", 200, 16, font(7) + b"Hg"),
]

out = {"fonts": sign_font_names(), "cases": []}
fonts = sign_fonts()
for name, W, H, data in CASES:
    frames, states = textline_frames(data + b"\0", fonts, W, H, max_frames=800)
    out["cases"].append({
        "name": name, "W": W, "H": H, "data": data.hex(), "states": states,
        "sha256": [hashlib.sha256(f.astype("<u2").tobytes()).hexdigest() for f in frames],
    })
path = os.path.join(os.path.dirname(__file__), "textline.fixtures.json")
json.dump(out, open(path, "w"))
print(f"{path}: {len(CASES)} cases, {sum(len(c['states']) for c in out['cases'])} frames")
