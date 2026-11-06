#!/usr/bin/env python3
"""Generate all 20 effect slots procedurally (no bytes taken from effectNN.flc), and emit effect_slots.h.
Tables: diamond, rainbow, shade, wave. Glitter is computed at run time from one shared phase table + a colour ramp."""
import math, os, sys

R, G, B = 0, 1, 2
HOLD = {71: 3, 42: 2, 28: 1}     # effects.ts flcSpeedToHold(speed), plus 1: ticks per frame


def rgb(r, g, b): return (r, g, b)


def lerp3(a, b, f): return tuple(int(round(a[i] + (b[i] - a[i]) * f)) for i in range(3))


def diamond(a, b, c):
    """colour cycle a -> b -> c -> a, 16 steps each, phase = t + Manhattan distance from the tile centre"""
    def col(p):
        p %= 48
        seg, k = p // 16, p % 16
        s, e = [(a, b), (b, c), (c, a)][seg]
        return lerp3(s, e, min(k * 17, 255) / 255)
    fr = []
    for t in range(48):
        fr.append([[col(t + (7 - x if x <= 7 else x - 8) + (7 - y if y <= 7 else y - 8)) for x in range(16)] for y in range(16)])
    return 16, 16, HOLD[71], fr


def rainbow(hold_ch, rise_ch, fall_mix=None):
    """2x60: a triangle over the 60-row tile; one channel rises to full, then another falls; col 1 half a step ahead"""
    def col(p):
        p %= 60
        q = min(p, 60 - p)
        c = [0, 0, 0]
        if q <= 15:
            c[hold_ch] = 255
            c[rise_ch] = int(q * 17)
        else:
            c[hold_ch] = int(max(0, (30 - q)) * 17)
            c[rise_ch] = 255
        return tuple(c)
    fr = []
    for t in range(60):
        fr.append([[col(y + t + 0.0), col(y + t + 0.5)] for y in range(60)])
    return 2, 60, HOLD[71], fr


L = [255, 221, 187, 153, 119, 102, 68, 34]


def shade(top, bot_fall, shared):
    """two 8-row bands: 'top' stays full while 'shared' falls, then 'shared' falls while 'bot_fall' rises"""
    rows = []
    for v in L:
        c = [0, 0, 0]; c[top] = 255; c[shared] = v; rows.append(tuple(c))
    for v in L:
        c = [0, 0, 0]; c[shared] = v; c[bot_fall] = 255 - v; rows.append(tuple(c))
    return 2, 16, HOLD[71], [[[rows[y], rows[y]] for y in range(16)]]


def wave(outer, centre):
    """a sine band moving right one pixel per frame; colour by the distance from the band's centre row"""
    def col(d):
        c = [0, 0, 0]
        c[outer] = 255
        if d >= 6: return tuple(c)
        rise = {5: 0x6D, 4: 0xB6}.get(d, 255)
        fall = {2: 0xB6, 1: 0x92, 0: 0}.get(d, 255)
        c[centre] = rise
        c[outer] = fall
        return tuple(c)
    centre_row = [8, 8, 7, 7, 6, 5, 5, 4, 4, 4, 5, 5, 6, 7, 7, 8]    # one period of the band's sine, as rows
    fr = []
    for t in range(16):
        fr.append([[col(abs(y - centre_row[(x - t) % 16])) for x in range(16)] for y in range(16)])
    return 16, 16, HOLD[28], fr


def ramp(a, b, n): return [lerp3(a, b, i / (n - 1)) for i in range(n)]


GLITTER = {   # slot: (low colour, high colour, levels, hold)
    3: ((0, 255, 0), (0, 255, 255), 16, HOLD[42]),
    4: ((0, 0, 255), (255, 0, 255), 16, HOLD[42]),
    5: ((0, 0, 0x1E), (0, 0, 255), 9, HOLD[42]),
    6: ((0, 0, 0x9A), (0, 255, 255), 9, HOLD[42]),
    7: ((0, 0x1E, 0), (0, 255, 0), 9, HOLD[42]),
    8: ((0x32, 0, 0), (255, 0, 0), 14, HOLD[28]),
    9: ((0, 255, 0), (255, 255, 0), 16, HOLD[42]),
    10: ((0x9A, 0, 0), (255, 255, 0), 9, HOLD[42]),
}


def slot_ramp(n):
    a, b, lv, _hold = GLITTER[n]
    r = ramp(a, b, lv)
    if n in (6, 10):                       # these two ease out on the channel that starts lit
        ch = 2 if n == 6 else 0
        for i in range(lv):
            c = list(r[i])
            f = 1 - (1 - i / (lv - 1)) ** 2
            c[ch] = int(round(a[ch] + (b[ch] - a[ch]) * f))
            r[i] = tuple(c)
    return r


def phase_table():
    """our own 80x16 byte noise (xorshift), one table shared by every glitter slot"""
    s, out = 0x2F6E2B1, []
    for _ in range(80 * 16):
        s ^= (s << 13) & 0xFFFFFFFF; s ^= s >> 17; s ^= (s << 5) & 0xFFFFFFFF
        out.append((s >> 11) & 0xFF)
    return out


PH = phase_table()


def glitter(n):
    a, b, lv, hold = GLITTER[n]
    r = slot_ramp(n)
    nf = 2 * (lv - 1)
    def tri(p):
        p %= nf
        return p if p < lv else nf - p
    fr = [[[r[tri(t + PH[y * 80 + x])] for x in range(80)] for y in range(16)] for t in range(nf)]
    return 80, 16, hold, fr


def slot(n):
    if n in GLITTER: return glitter(n)
    return {
        0: lambda: diamond((0, 255, 0), (0, 0, 255), (0, 255, 255)),
        1: lambda: diamond((0, 0, 255), (255, 0, 0), (255, 0, 255)),
        2: lambda: diamond((0, 255, 0), (255, 0, 0), (255, 255, 0)),
        11: lambda: rainbow(B, G),
        12: lambda: rainbow(R, B),
        13: lambda: rainbow(R, G),
        14: lambda: shade(B, B, G),
        15: lambda: shade(R, R, B),
        16: lambda: shade(R, R, G),
        17: lambda: wave(B, R),
        18: lambda: wave(G, B),
        19: lambda: wave(G, R),
    }[n]()


NAMES = ["DMOND-BG", "DMOND-BR", "DMOND-RG", "GLT2-BG", "GLT2-BR", "BLUE", "GLTR-BG", "GreenCYC", "redcyc", "GLTR-YG",
         "GLTR-YR", "RBOW-BCG", "RBOW-BMR", "RBOW-RYG", "SHDE-BCR", "SHDE-BMR", "SHDE-RYG", "WAVE-BMR", "WAVE-GCB", "WAVE-GYR"]


def c555(c): return (c[0] >> 3) << 10 | (c[1] >> 3) << 5 | c[2] >> 3


def emit(path):
    o = ["/* Effect slots 0-19, generated by anim_gen2.py from formulas. Slots with px play stored frames; glitter slots",
         " * (px == 0) are computed from the shared phase table and their colour ramp. Generated: do not edit. */",
         "#ifndef UMAMI_EFFECT_SLOTS_H", "#define UMAMI_EFFECT_SLOTS_H", "#include <stdint.h>", "#define NSLOT 20",
         "static const uint8_t glitter_phase[1280] = {" + ",".join(map(str, PH)) + "};"]
    rows = []
    for n in range(20):
        if n in GLITTER:
            a, b, lv, hold = GLITTER[n]
            o.append(f"static const uint16_t slot{n}_ramp[{lv}] = {{" + ",".join("0x%04X" % c555(c) for c in slot_ramp(n)) + "};")
            rows.append(f"\t{{ 80, 16, {2 * (lv - 1)}, {hold}, 0, slot{n}_ramp, {lv} }},   /* {NAMES[n]} */")
        else:
            w, h, hold, fr = slot(n)
            flat = [c555(c) for f in fr for row in f for c in row]
            o.append(f"static const uint16_t slot{n}_px[{len(flat)}] = {{" + ",".join("0x%04X" % v for v in flat) + "};")
            rows.append(f"\t{{ {w}, {h}, {len(fr)}, {hold}, slot{n}_px, 0, 0 }},   /* {NAMES[n]} */")
    o += ["struct eslot { uint8_t w, h, nframes, hold; const uint16_t *px; const uint16_t *ramp; uint8_t levels; };",
          "static const struct eslot eslots[NSLOT] = {"] + rows + ["};", "#endif"]
    open(path, "w").write("\n".join(o) + "\n")


if __name__ == "__main__":
    emit(sys.argv[1])
    print("wrote", sys.argv[1], os.path.getsize(sys.argv[1]), "bytes")
