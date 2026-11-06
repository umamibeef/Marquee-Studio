#!/usr/bin/env python3
"""Golden fixtures for src/message.ts renderMessage from the Python renderer (studio/marquee_studio/render.py
render_message555). Writes message.fixtures.json and the native emoji sheet the renderer used (emoji16.bin). Run from
the repository root:  venv/bin/python frontend/packages/sign-emu/test/make_message_fixtures.py
"""
import hashlib, json, os, sys

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", "..", "..", ".."))
sys.path.insert(0, os.path.join(ROOT, "studio"))
from marquee_studio.render import render_message555          # noqa: E402
from marquee_studio import native_emoji                       # noqa: E402

HERE = os.path.dirname(__file__)
CASES = [
    ("scroll amber", 200, 16, {"text": "UMAMI MARQUEE", "font": "fonte04.png", "colour": "Amber", "mode": "scroll", "text_rate": 4}),
    ("static centred", 200, 16, {"text": "HELLO", "font": "fonte04.png", "colour": "Green", "mode": "static"}),
    ("flash", 200, 16, {"text": "FLASH", "font": "fonte03.png", "colour": "Red", "mode": "static", "flash_on_ms": 300, "flash_off_ms": 200}),
    ("money plain", 200, 16, {"text": "JACKPOT", "value": "12500.5", "font": "fonte04.png", "mode": "static"}),
    ("money odometer", 200, 16, {"text": "", "value": 1234567, "amount": {"mode": 0, "decimals": 2}, "font": "fonte04.png", "mode": "static"}),
    ("money small cents", 200, 16, {"value": "99.99", "amount": {"mode": 2, "prefix": "$"}, "font": "fonte04.png", "mode": "static"}),
    ("wipe letters", 200, 16, {"text": "WIPE", "font": "fonte04.png", "foreground": "Red/green/yellow wipe", "mode": "static"}),
    ("transparent bg over slot fg", 200, 16, {"text": "ANIM", "font": "fonte04.png", "foreground": "effect02.flc", "background": "Transparent", "mode": "static"}),
    ("slot background", 200, 16, {"text": "BG", "font": "fonte04.png", "colour": "White", "background": "effect05.flc", "mode": "static"}),
    ("non-slot animation background", 200, 16, {"text": "LOGO", "font": "fonte04.png", "colour": "White", "background": "logo_e.flc", "mode": "static"}),
    ("outline thick", 200, 16, {"text": "OUTLINE", "font": "fonte04.png", "colour": "Yellow", "outline": "Red", "outline_style": "Thick", "mode": "static"}),
    ("outline nearest colour", 200, 16, {"text": "NEAR", "font": "fonte04.png", "colour": "Amber", "outline": "Gold", "outline_style": "Drop Shadow", "mode": "scroll", "text_rate": 6}),
    ("native emoji", 200, 16, {"text": "PIZZA \U0001F355 TIME ❤️", "font": "fonte04.png", "colour": "Amber", "mode": "scroll", "native_emoji": True, "emoji_size": 16}),
    ("tall panel", 120, 32, {"text": "BIG", "font": "fonte08.png", "colour": "Cyan", "mode": "static", "frame0": 7}),
]
anims = set()
out = {"cases": []}
for name, W, H, spec in CASES:
    frames = render_message555(spec, W, H)
    for k in ("foreground", "background"):
        if str(spec.get(k, "")).endswith(".flc"):
            anims.add(spec[k])
    out["cases"].append({"name": name, "W": W, "H": H, "spec": spec,
                         "sha256": [hashlib.sha256(f.astype("<u2").tobytes()).hexdigest() for f in frames]})
out["anims"] = sorted(anims)
json.dump(out, open(os.path.join(HERE, "message.fixtures.json"), "w"), ensure_ascii=False)
open(os.path.join(HERE, "emoji16.bin"), "wb").write(native_emoji.build(native_emoji.DEFAULT, 16)[0])
print(f"{len(CASES)} cases, {sum(len(c['sha256']) for c in out['cases'])} frames; anims {sorted(anims)}")
