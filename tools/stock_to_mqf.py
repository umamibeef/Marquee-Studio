#!/usr/bin/env python3
"""Convert the sign's own stock fonts (fonteNN.png, a 16 x 16 cell grid) into our MQF1 format, proportional-grid
mode (mqf.py flag 2): the firmware then loads them like any other .mqf and draws them with their exact stock
measuring (left bearing, 1 px gaps, the space and digit rules), matching the web Studio pixel for pixel. The glyphs
are the sign's own; this only changes the container, so no dump is read at build time (the pngs are checked in).

usage: stock_to_mqf.py SRC_DIR OUT_DIR NAME [NAME ...]   (NAME e.g. fonte03.png; output keeps the same name)
"""
import sys
import os

sys.path.insert(0, os.path.abspath(os.path.join(os.path.dirname(__file__), "..")))
from marquee_pack import mqf
from marquee_pack.sign_emu import FwFont


def convert(png_path, out_path):
    fe = FwFont(png_path=png_path)
    cw, ch = fe.cw, fe.ch
    glyphs = {}
    for c in range(256):
        x0, y0 = (c & 15) * cw, (c >> 4) * ch
        glyphs[c] = (cw, 0, 0, fe.ink[y0:y0 + ch, x0:x0 + cw].astype(bool))
    mqf.write(glyphs, ch, 0, list(range(256)), out_path, prop=True)
    return cw, ch


def main(argv):
    if len(argv) < 4:
        print(__doc__)
        return 1
    src, out = argv[1], argv[2]
    os.makedirs(out, exist_ok=True)
    for name in argv[3:]:
        cw, ch = convert(os.path.join(src, name), os.path.join(out, name))
        print(f"stock_to_mqf: {name}  cell {cw}x{ch}  -> {os.path.join(out, name)}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv))
