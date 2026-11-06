"""Glow: light that falls off with distance around a shape (text, a logo), optionally radiating outward in rings.

    field = glow_field(mask, W, H, radius)          # per-pixel distance to the shape, once per shape
    img = render_glow(field, mask, text_rgb, glow_rgb, t, ...)   # one frame

The glow is additive light: brightness = strength * exp(-d / radius) for a pixel at distance d (in LEDs) from the
shape, times a travelling ring term when `rings` > 0 (rings move outward as t grows). The shape itself is drawn
on top in its own colour. Values are kept as RGB888 and become RGB555 when encoded for the sign.
"""
import math


def glow_field(mask, W, H, reach=6):
    """Euclidean distance from each pixel to the nearest shape pixel, up to `reach` (beyond: None)."""
    pts = list(mask)
    field = {}
    for y in range(H):
        for x in range(W):
            if (x, y) in mask:
                field[(x, y)] = 0.0
                continue
            best = reach + 1
            for px, py in pts:
                dx, dy = abs(px - x), abs(py - y)
                if dx <= reach and dy <= reach:
                    d = math.hypot(dx, dy)
                    if d < best:
                        best = d
            if best <= reach:
                field[(x, y)] = best
    return field


def glow_level(d, t=0.0, radius=2.2, rings=0.0, wavelength=3.0, period=1.0):
    """Brightness (0..1) of the glow at distance d > 0 and time t (seconds)."""
    base = math.exp(-d / radius)
    if rings:
        phase = (d / wavelength - t / period) * 2 * math.pi
        base *= 1 - rings + rings * (0.5 + 0.5 * math.cos(phase))
    return base


def render_glow(img, field, mask, text_rgb, glow_rgb, t=0.0, strength=1.0, radius=2.2, rings=0.0,
                wavelength=3.0, period=1.0, add=True):
    """Draw the glow into img (a PIL RGB image), then the shape on top. With add=True the glow adds to what is
    already there (light), else it replaces it."""
    px = img.load()
    for (x, y), d in field.items():
        if d == 0:
            continue
        k = strength * glow_level(d, t, radius, rings, wavelength, period)
        if k <= 0.01:
            continue
        g = tuple(int(c * k) for c in glow_rgb)
        if add:
            o = px[x, y]
            px[x, y] = tuple(min(255, a + b) for a, b in zip(o, g))
        else:
            px[x, y] = g
    for x, y in mask:
        px[x, y] = text_rgb
    return img
