#!/usr/bin/env python3
"""The sign's 52 text colour effects (indices 0x00..0x33), reversed from the compositor FUN_0000ef48.

Each effect paints an RGB555 colour PATTERN over the text area; the glyphs are shown through it as a stencil.
Effects animate: a phase `p` (0..31) advances each frame (the firmware's uVar17), wrapping to drive the motion.
`effect_color(idx, x, y, W, H, p)` returns the RGB555 colour at pixel (x,y) for frame phase p.
Families (from the decompilation):
  0x00-0x0e solid colours        — Red/Green/Yellow at 5 brightness levels (31,23,15,7,1)
  0x0f-0x12 horizontal gradients — Red / Green / Yellow ramp, and a 3-colour R|G|Y ramp
  0x13-0x14 split / long rainbow ramp
  0x15-0x17 vertical bar sweeping horizontally (R/G/Y)
  0x18-0x1a 5px comet sweeping horizontally (R/G/Y)
  0x1b-0x1d horizontal bar sweeping vertically (R/G/Y)
  0x1e-0x23 diagonal sweep (R/G/Y, two directions)
  0x24-0x29 box / cross wipe (R/G/Y)
  0x2a-0x2f interlaced columns / venetian-blind shimmer (R/G/Y, 2 phases)
  0x30-0x33 dashed / spaced line patterns
"""

R, G, Y = 0, 1, 2  # channel kinds


def _c(kind, level):
    """RGB555 for a channel kind at a 5-bit level (level 0..31)."""
    lv = max(0, min(31, level))
    if kind == R:
        return lv << 10
    if kind == G:
        return lv << 5
    return (lv << 10) | (lv << 5)  # Y = R+G


SOLID = [  # 0x00..0x0e
    (R, 31), (G, 31), (Y, 31), (R, 23), (G, 23), (Y, 23),
    (R, 15), (G, 15), (Y, 15), (R, 7), (G, 7), (Y, 7),
    (R, 1), (G, 1), (Y, 1),
]
BAR_KIND = {0x15: R, 0x16: G, 0x17: Y, 0x18: R, 0x19: G, 0x1a: Y,
            0x1b: R, 0x1c: G, 0x1d: Y, 0x1e: R, 0x1f: G, 0x20: Y,
            0x21: R, 0x22: G, 0x23: Y, 0x24: R, 0x25: R, 0x26: G,
            0x27: G, 0x28: Y, 0x29: Y, 0x2a: R, 0x2b: R, 0x2c: G,
            0x2d: G, 0x2e: Y, 0x2f: Y, 0x30: R, 0x31: G, 0x32: Y, 0x33: R}

NUM_EFFECTS = 0x34


def effect_color(idx, x, y, W, H, p):
    if idx < 0x0f:                                  # solid
        k, lv = SOLID[idx]
        return _c(k, lv)
    if idx == 0x0f:                                 # red ramp L->R
        return _c(R, (x & 0x1f))
    if idx == 0x10:
        return _c(G, (x & 0x1f))
    if idx == 0x11:
        return _c(Y, (x & 0x1f))
    if idx == 0x12:                                 # 3-colour ramp across thirds
        third = max(1, W // 3)
        seg = min(2, x // third)
        return _c((R, G, Y)[seg], ((x % third) * 31 // third))
    if idx == 0x13:                                 # bottom half solid, top gradient
        if y >= H // 2:
            return _c(Y, 31)
        return _c(Y, (x & 0x1f))
    if idx == 0x14:                                 # long rainbow ramp (brightness waves)
        return _c((R, G, Y)[(x // max(1, W // 6)) % 3], (x * 31 // max(1, W)) & 0x1f)
    k = BAR_KIND.get(idx, R)
    pos = p * W // 32                               # animated sweep position
    if 0x15 <= idx <= 0x17:                         # 1px vertical bar sweeping right
        return _c(k, 31) if x == pos else 0
    if 0x18 <= idx <= 0x1a:                         # 5px comet (fading trail) sweeping right
        d = pos - x
        return _c(k, 31 - d * 6) if 0 <= d < 5 else 0
    if 0x1b <= idx <= 0x1d:                         # horizontal bar sweeping down
        ypos = p * H // 32
        return _c(k, 31) if y == ypos else 0
    if 0x1e <= idx <= 0x23:                         # diagonal sweep
        diag = (x + y) % 32
        return _c(k, 31) if diag == (p % 32) else 0
    if 0x24 <= idx <= 0x29:                         # box / cross wipe (expanding frame)
        r = pos
        on = (x == r or x == W - 1 - r or y == (p * H // 32) or y == H - 1 - (p * H // 32))
        return _c(k, 31) if on else 0
    if 0x2a <= idx <= 0x2f:                         # interlaced columns shimmer (2 phases)
        phase = p & 1 if idx % 2 == 0 else (p + 1) & 1
        return _c(k, 31) if (x & 1) == phase else 0
    if 0x30 <= idx <= 0x33:                         # dashed / spaced lines
        spacing = 3 + (idx - 0x30)
        return _c(k, 31) if ((x + p) % spacing == 0 or (y + p) % spacing == 0) else 0
    return _c(R, 31)


EFFECT_NAMES = {}
for i in range(NUM_EFFECTS):
    if i < 0x0f:
        k, lv = SOLID[i]; EFFECT_NAMES[i] = f"Solid {'RGY'[k].replace('Y','Yellow').replace('R','Red').replace('G','Green')} {lv}"
    elif i <= 0x12: EFFECT_NAMES[i] = ["Red ramp", "Green ramp", "Yellow ramp", "R|G|Y ramp"][i - 0x0f]
    elif i <= 0x14: EFFECT_NAMES[i] = ["Split fill", "Rainbow ramp"][i - 0x13]
    elif i <= 0x17: EFFECT_NAMES[i] = f"Vertical bar {'RGY'[BAR_KIND[i]]}"
    elif i <= 0x1a: EFFECT_NAMES[i] = f"Comet {'RGY'[BAR_KIND[i]]}"
    elif i <= 0x1d: EFFECT_NAMES[i] = f"Horizontal bar {'RGY'[BAR_KIND[i]]}"
    elif i <= 0x23: EFFECT_NAMES[i] = f"Diagonal {'RGY'[BAR_KIND[i]]}"
    elif i <= 0x29: EFFECT_NAMES[i] = f"Box wipe {'RGY'[BAR_KIND[i]]}"
    elif i <= 0x2f: EFFECT_NAMES[i] = f"Shimmer {'RGY'[BAR_KIND[i]]}"
    else: EFFECT_NAMES[i] = f"Dashed {'RGY'[BAR_KIND.get(i, 0)]}"
