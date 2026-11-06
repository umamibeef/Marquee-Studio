#!/usr/bin/env python3
"""Golden fixtures for the attract-line composition (src/attract.ts composeAttract): a line through an effect word
and outline, each frame as display_main_loop draws it (decompile 4498-4540), with sign_emu.py's Effects and
outline_blit (checked against the firmware) and 8 frame buffers that are never cleared. Run from the repo root:
    venv/bin/python frontend/packages/sign-emu/test/make_effect_fixtures.py
"""
import glob, hashlib, json, os, sys
import numpy as np

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", "..", "..", ".."))
sys.path.insert(0, os.path.join(ROOT, "studio"))
from marquee_studio.fonts import sign_fonts, sign_font_names                       # noqa: E402
from marquee_studio.sign_emu import TextLine, Effects, outline_blit, s32, U32     # noqa: E402

E = b"\x1b"
def font(n): return E + bytes([0x60, n])
def entry(d, s): return E + bytes([0x40 + d, s])
def hold(e, f4, s): return E + bytes([0x50 + e, f4, s])

ANIMS = os.path.join(ROOT, "studio", "content", "animations")
SLOTS = {int(os.path.basename(p)[6:8]): open(p, "rb").read() for p in sorted(glob.glob(os.path.join(ANIMS, "effect[0-9][0-9].flc")))}

CENTRED = font(0xFF) + entry(0, 255) + hold(0, 6, 255)
CASES = [
    ("amber on off", 200, 16, b"UMAMI", 0x7E800000, 0, 0),
    ("amber on transparent", 200, 16, b"UMAMI", 0x7E808000, 0, 0),
    ("wipe 8001", 200, 16, CENTRED + b"WIPE", 0x80010000, 0, 0),
    ("stripes 8002", 200, 16, CENTRED + b"STRIPES", 0x80020000, 0, 0),
    ("palette cycle BFFE", 200, 16, CENTRED + b"CYCLE", 0xBFFE0000, 0, 0),
    ("effect00 letters", 200, 16, CENTRED + b"FLC", 0xC0000000, 0, 0),
    ("red on effect01 background", 200, 16, CENTRED + b"BACK", 0x7C00C001, 0, 0),
    ("outline thick red", 200, 16, CENTRED + b"THICK", 0x7E800000, 1, 1),
    ("outline thin white", 200, 16, CENTRED + b"THIN", 0x03E00000, 2, 7),
    ("drop shadow blue", 200, 16, CENTRED + b"SHADOW", 0x7FE00000, 3, 4),
    ("small font band, stale buffers", 200, 16, font(0) + b"small font", 0x7E800000, 0, 0),
    ("effect slot on a tall panel", 120, 32, CENTRED + b"TALL", 0xC0020000, 1, 3),
]

def compose(fx, frame, line_img, f, word, style, colour):
    H, W = frame.shape
    cell = f.h >> 4
    y = s32(((H - cell) & U32) >> 1)
    band = [0, y, W, cell]
    if style:
        fx.blit_effect(np.zeros((H, W), np.uint16), frame, word, [0, 0, W, H], (0, 0))
        word = (word & 0xFFFF0000) | 0x8000
        outline_blit(line_img, list(band), frame, (0, y), style, colour, (0, 0, W, H))
    fx.blit_effect(line_img, frame, word, band, (0, y))

fonts = sign_fonts()
out = {"fonts": sign_font_names(), "slots": sorted(SLOTS), "cases": []}
for name, W, H, data, word, style, colour in CASES:
    fx = Effects(slots=SLOTS, frame_count=0)
    table = (data + b"\0").ljust(256, b"\0")
    tl = TextLine(table, fonts, W, H, line=0)
    ring = [np.zeros((H, W), np.uint16) for _ in range(8)]
    shas, states = [], []
    for k in range(400):
        st = tl.step()
        frame = ring[k % 8]
        compose(fx, frame, tl.img, tl.f, word, style, colour)
        fx.frame_count += 1
        shas.append(hashlib.sha256(frame.astype("<u2").tobytes()).hexdigest())
        states.append(st)
        if st == 4:
            break
    out["cases"].append({"name": name, "W": W, "H": H, "data": data.hex(), "effect": word,
                         "style": style, "colour": colour, "states": states, "sha256": shas})
path = os.path.join(os.path.dirname(__file__), "attract.fixtures.json")
json.dump(out, open(path, "w"))
print(f"{path}: {len(CASES)} cases, {sum(len(c['states']) for c in out['cases'])} frames, slots {sorted(SLOTS)}")
