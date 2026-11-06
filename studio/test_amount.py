#!/usr/bin/env python3
"""Check core.amount_format against the firmware's own money formatter (amount_format 0x479B4, under Unicorn),
and that imported fonts (font_import.py) carry every glyph its four modes use.

Usage: ../venv/bin/python test_amount.py     (needs analysis/extracted/ELITE_RGB and unicorn)
"""
import os, random, struct, sys, tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
sys.path.insert(0, os.path.join(HERE, "..", "analysis", "tools"))
import core  # noqa: E402
from marquee_studio import font_import  # noqa: E402
from marquee_studio import sign_emu as emu  # noqa: E402
import fw  # noqa: E402

ok = True


def check(name, cond, got=""):
    global ok
    ok &= bool(cond)
    print(f"  {'ok  ' if cond else 'FAIL'} {name}" + ("" if cond else f"  (got {got})"))


def firmware(f, value, decimals, mode, dec, thou, prefix, suffix):
    cfg = f.alloc(0x50)
    f.write(cfg, bytes(0x50))
    f.write(cfg + 0x3A, bytes([decimals]))
    f.write(cfg + 0x48, dec.encode("latin1") + (thou.encode("latin1") if thou else b"\0"))
    f.u32(cfg + 0x4C, mode)
    out = f.sbuf("", cap=64)
    f.call("amount_format", out, value, f.sbuf(prefix), f.sbuf(suffix), cfg)
    return bytes(f.read(f.u32(out), f.u16(out + 6)))


def main():
    f = fw.FW()
    rnd = random.Random(1)
    cases = [(v, d, m, ".", ",", "$", "") for v in (0, 5, 99, 100, 1000, 123456789, 4294967295)
             for d in (0, 1, 2, 3) for m in range(4)]
    for _ in range(300):
        cases.append((rnd.choice([rnd.randrange(1000), rnd.randrange(10 ** 7), rnd.randrange(2 ** 32)]),
                      rnd.randrange(4), rnd.randrange(4), rnd.choice(".,"), rnd.choice([",", ".", " ", "", "'"]),
                      rnd.choice(["$", "", "EUR ", "£"]), rnd.choice(["", " pts", "!"])))
    print("1. core.amount_format == the firmware's amount_format")
    bad = []
    for c in cases:
        want = firmware(f, *c)
        if core.amount_format(*c) != want:
            bad.append((c, core.amount_format(*c), want))
    check(f"{len(cases)} cases (values, 0-3 decimals, 4 modes, separators, prefixes, suffixes)", not bad, bad[:3])

    print("2. imported fonts carry the glyphs of every mode")
    out = tempfile.mkdtemp(prefix="amount-")
    for rel in ("spleen/spleen-8x16.bdf", "spleen/spleen-5x8.bdf", "cozette/cozette.bdf",
                "monogram/monogram-extended.ttf", "jersey10/Jersey10-Regular.ttf", "tamzen/Tamzen8x16b.bdf"):
        png = os.path.join(out, rel.replace("/", "_") + ".png")
        font_import.import_font(rel, png)
        ft = emu.FwFont(png)
        ink = lambda c: ft.ink[(c >> 4) * ft.ch:(c >> 4) * ft.ch + ft.ch, (c & 15) * ft.cw:(c & 15) * ft.cw + ft.cw].any()
        need = list(range(0x00, 0x0A)) + list(range(0x10, 0x1A)) + [0x1C]
        missing = [hex(c) for c in need if not ink(c)]
        text = core.amount_format(123456789, 2, 2)
        check(f"{rel}: 0x00-0x09, 0x10-0x19, 0x1C inked; '$1,234,567.89' in mode 2 is {ft.text_width(text)} px wide",
              not missing, missing)
    print("\nALL AMOUNT CHECKS PASS" if ok else "\nFAILURES")
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
