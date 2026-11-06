#!/usr/bin/env python3
"""The Umami Marquee start-up animation: logo_s.flc for a 200 x 16 panel.

A circuit board lights up: current chases along its traces from the left edge to the right, and "UMAMI MARQUEE"
is powered up column by column as the current passes. The traces fade, the text flashes, and it settles into a
glow that radiates outward (marquee_pack/glow.py).

  make_intro.py [OUT_DIR]   -> OUT_DIR/logo_s.flc (start-up), logo_e.flc (the logo loop after it; stock: MIKOHN),
                               and previews (intro.gif, preview.gif, sheet.png, logo_loop_preview.gif)
Default OUT_DIR: build/intro. Font: Jersey 10 (third_party/fonts/jersey10, as the sign font Jersey10.mqf).
"""
import os
import random
import sys
import tempfile

from PIL import BdfFontFile, Image, ImageDraw, ImageFont

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(HERE)
sys.path.insert(0, REPO)
from marquee_pack import gif2flc, glow  # noqa: E402

W, H = 200, 16
TEXT = "UMAMI MARQUEE"
FONT = os.path.join(REPO, "third_party", "fonts", "jersey10", "Jersey10-Regular.ttf")
SPACING = 1
FRAME_MS = 50

BLACK = (0, 0, 0)
TRACE = (0, 90, 60)          # saturated: pure hues, little white mixed in
TRACE_LIT = (0, 200, 120)    # just after the current passed
HEAD = (40, 255, 220)
SPARK = (120, 255, 255)
VIA = (0, 255, 150)
HOT = (255, 230, 0)          # the text as the current reaches it
GOLD = (255, 140, 0)
GLOW = (255, 30, 0)
WHITE = (255, 255, 160)      # the flash: warm, not grey


def mix(a, b, t):
    t = max(0.0, min(1.0, t))
    return tuple(int(round(x + (y - x) * t)) for x, y in zip(a, b))


def text_mask():
    """The text's pixels: Jersey 10 as the sign draws it (its sign font, bootloader/build/fonts/Jersey10.mqf, from
    the same import), centred on its ink."""
    from marquee_pack import mqf
    built = os.environ.get("INTRO_FONT") or os.path.join(REPO, "bootloader", "build", "fonts", "Jersey10.mqf")
    lh, asc, g = mqf.read(open(built, "rb").read())
    cells = [g[ord(c)] for c in TEXT]
    width = sum(a for a, _ in cells) + SPACING * (len(TEXT) - 1)
    rows = [y for a, r in cells for y in range(lh) if r[y].any()]
    top, bottom = min(rows), max(rows)
    x, y0 = (W - width) // 2, (H - (bottom - top + 1)) // 2 - top
    out = set()
    for a, r in cells:
        for y in range(lh):
            for xx in range(a):
                if r[y, xx] and 0 <= y + y0 < H:
                    out.add((x + xx, y + y0))
        x += a + SPACING
    return out


class Board:
    """Traces routed on the pixel grid with one pixel of clearance between different traces."""

    def __init__(self, rnd):
        self.rnd = rnd
        self.owner = {}                     # pixel -> trace id (including its clearance halo)
        self.traces = []                    # (points, vias, pad)

    def free(self, x, y, tid):
        if not (0 <= x < W and 0 <= y < H):
            return False
        for dx in (-1, 0, 1):
            for dy in (-1, 0, 1):
                o = self.owner.get((x + dx, y + dy))
                if o is not None and o != tid:
                    return False
        return True

    def take(self, x, y, tid):
        self.owner[(x, y)] = tid

    def route(self, y, tid, end_x):
        """From the left edge on row y to end_x: straight runs on lanes three rows apart, changing lane with a
        45-degree bend now and then, or when the way ahead is taken."""
        pts, vias, x = [(0, y)], [], 0
        if not self.free(0, y, tid):
            return None
        self.take(0, y, tid)
        run = self.rnd.randint(10, 40)
        while x < end_x:
            blocked = not all(self.free(x + k, y, tid) for k in (1, 2))
            if run <= 0 or blocked:
                run = self.rnd.randint(14, 46)
                moved = False
                for dy in self.rnd.sample((3, -3), 2):
                    ny = y + dy
                    step = 1 if dy > 0 else -1
                    path = [(x + i + 1, y + step * (i + 1)) for i in range(3)]
                    ahead = [(ny_x, ny) for ny_x in range(x + 4, min(W, x + 10))]
                    if 1 <= ny <= H - 2 and x + 3 < W and all(self.free(px, py, tid) for px, py in path + ahead):
                        if self.rnd.random() < 0.6:
                            vias.append((x, y))
                        for pnt in path:
                            pts.append(pnt)
                            self.take(*pnt, tid)
                        x, y = path[-1]
                        moved = True
                        break
                if moved:
                    continue
                if blocked:
                    break
            x += 1
            run -= 1
            pts.append((x, y))
            self.take(x, y, tid)
        if len(pts) < 30:
            return None
        return pts, vias, pts[-1] if pts[-1][0] < W - 1 else None


def build():
    rnd = random.Random(11)
    mask = text_mask()
    xs = [x for x, _ in mask]
    left, right = min(xs), max(xs)
    board = Board(rnd)
    for tid, y in enumerate((1, 7, 13, 4, 10, 2, 12, 5, 9, 14, 0, 8, 3, 11, 6, 15)):   # as many as fit, staggered
        end = W - 1 if tid < 5 else rnd.randint(110, 196)
        r = board.route(y, tid, end)
        if r:
            board.traces.append((r, rnd.randint(0, 9)))

    speed = 6                               # trace pixels per frame
    lead = 3                                # dark frames first
    chase_end = lead + max(t0 + len(p) // speed for (p, _, _), t0 in board.traces) + 1
    fade_end = chase_end + 6
    flash = fade_end
    total = flash + 34
    field = glow.glow_field(mask, W, H, reach=7)

    def front(f):
        """The rightmost column the current has reached on any trace."""
        best = -1
        for (pts, _, _), t0 in board.traces:
            head = (f - lead - t0) * speed
            if head >= 0:
                best = max(best, pts[min(head, len(pts) - 1)][0])
        return best

    reveal = {}                             # column -> frame the current reached it
    for f in range(total):
        fr = front(f)
        for x in range(fr + 1):
            reveal.setdefault(x, f)

    frames = []
    for f in range(total):
        img = Image.new("RGB", (W, H), BLACK)
        px = img.load()
        level = 1.0 if f < chase_end else max(0.0, 1 - (f - chase_end) / (fade_end - chase_end))
        if level > 0:
            for (pts, vias, pad), t0 in board.traces:
                head = (f - lead - t0) * speed
                if head < 0:
                    continue
                for i, (x, y) in enumerate(pts[:head + 1]):
                    behind = head - i
                    c = mix(HEAD, TRACE_LIT, behind / 10) if behind < 10 else mix(TRACE_LIT, TRACE, (behind - 10) / 40)
                    px[x, y] = mix(BLACK, c, level)
                if f < chase_end and head < len(pts):     # sparks: a few bright pixels jumping off the head
                    hx, hy = pts[head]
                    srnd = random.Random(f * 131 + t0 * 7 + hx)
                    for _ in range(2):
                        sx, sy = hx + srnd.randint(-2, 1), hy + srnd.choice((-1, 1)) * srnd.randint(1, 2)
                        if 0 <= sx < W and 0 <= sy < H:
                            px[sx, sy] = SPARK
                for vx, vy in vias:                       # a via: a small ring that flashes as the current passes
                    i = next((k for k, p in enumerate(pts) if p == (vx, vy)), None)
                    if i is None or head < i:
                        continue
                    c = mix(HEAD, VIA, (head - i) / 12)
                    for dx, dy in ((-1, 0), (1, 0), (0, -1), (0, 1)):
                        if 0 <= vx + dx < W and 0 <= vy + dy < H:
                            px[vx + dx, vy + dy] = mix(BLACK, c, level)
                if pad and head >= len(pts) - 1:
                    x, y = pad
                    for dy in (0, -1 if y > 0 else 1):
                        for dx in (0, 1):
                            if x + dx < W:
                                px[x + dx, y + dy] = mix(BLACK, VIA, level)
        # the text: powered up as the current passes its columns
        if f < flash:
            lit = {(x, y) for x, y in mask if x in reveal and reveal[x] <= f}
            for x, y in lit:
                px[x, y] = mix(HOT, GOLD, (f - reveal[x]) / 5)
            if f >= chase_end:                            # the glow comes up as the traces go
                k = (f - chase_end) / (fade_end - chase_end)
                glow.render_glow(img, field, mask, GOLD, GLOW, strength=0.6 * k, radius=1.8)
        else:
            t = (f - flash) * FRAME_MS / 1000
            if f - flash < 2:                             # the flash: the text alone, white hot, a wide glow
                glow.render_glow(img, field, mask, WHITE, (255, 200, 120), strength=1.0, radius=3.0)
            else:                                         # settles into a glow radiating outward
                k = min(1.0, (f - flash - 2) / 6)
                text = mix(WHITE, GOLD, k)
                glow.render_glow(img, field, mask, text, GLOW, t=t, strength=0.95 - 0.25 * k, radius=2.4 - 0.4 * k,
                                 rings=0.55 * k, wavelength=3.0, period=0.7)
                import math                               # a sheen sweeping across the letters once
                sweep = (f - flash - 2) / max(1, total - flash - 4)
                sx = left - 10 + (right - left + 20) * sweep
                for x, y in mask:
                    kk = math.exp(-((x - sx - (H - y) * 0.5) ** 2) / 10.0)
                    if kk > 0.05:
                        px[x, y] = mix(px[x, y], WHITE, 0.9 * kk)
        frames.append(img)
    return frames


LOOP_FRAMES = 44                                     # as the stock logo_e.flc: 44 frames at 50 ms


def build_logo_loop():
    """logo_e.flc, the logo the display loop shows after start-up (the stock one is MIKOHN): the text in gold with a
    glow radiating outward and a sheen sweeping across once per loop. Every term repeats over LOOP_FRAMES, so the
    loop has no seam."""
    import math
    mask = text_mask()
    field = glow.glow_field(mask, W, H, reach=7)
    xs = [x for x, _ in mask]
    left, right = min(xs), max(xs)
    period = LOOP_FRAMES * FRAME_MS / 1000
    frames = []
    for f in range(LOOP_FRAMES):
        t = f * FRAME_MS / 1000
        img = Image.new("RGB", (W, H), BLACK)
        glow.render_glow(img, field, mask, GOLD, GLOW, t=t, strength=0.75, radius=2.0, rings=0.55, wavelength=3.0,
                         period=period / 4)
        px = img.load()
        sx = left - 12 + (right - left + 24) * f / LOOP_FRAMES   # the sheen: a soft bright band moving right
        for x, y in mask:
            k = math.exp(-((x - sx - (H - y) * 0.6) ** 2) / 18.0)
            px[x, y] = mix(GOLD, WHITE, 0.85 * k)
        frames.append(img)
    return frames


def main(argv):
    out = argv[1] if len(argv) > 1 else os.path.join(REPO, "build", "intro")
    os.makedirs(out, exist_ok=True)
    frames = build()
    gif = os.path.join(out, "intro.gif")
    frames[0].save(gif, save_all=True, append_images=frames[1:], duration=FRAME_MS, loop=0)
    big = []
    for fr in frames:                                     # preview: round-ish LEDs with gaps, 5x
        b = Image.new("RGB", (W * 5, H * 5), (10, 10, 12))
        src, o = fr.load(), b.load()
        for y in range(H):
            for x in range(W):
                c = src[x, y]
                for dy in range(1, 5):
                    for dx in range(1, 5):
                        if not ((dx in (1, 4)) and (dy in (1, 4))):
                            o[x * 5 + dx, y * 5 + dy] = c
        big.append(b)
    big[0].save(os.path.join(out, "preview.gif"), save_all=True, append_images=big[1:], duration=FRAME_MS, loop=0)
    pick = frames[::3] + [frames[-1]]
    sheet = Image.new("RGB", (W * 3, (H + 2) * len(pick) * 3), (30, 30, 30))
    for i, fr in enumerate(pick):
        sheet.paste(fr.resize((W * 3, H * 3), Image.NEAREST), (0, i * (H + 2) * 3))
    sheet.save(os.path.join(out, "sheet.png"))
    info = gif2flc.convert(gif, os.path.join(out, "logo_s.flc"), size=(W, H), fit="stretch", depth=8)
    print(f"{len(frames)} frames, {len(frames) * FRAME_MS / 1000:.2f} s -> {out}/logo_s.flc: {info}")
    loop = build_logo_loop()
    lgif = os.path.join(out, "logo_loop.gif")
    loop[0].save(lgif, save_all=True, append_images=loop[1:], duration=FRAME_MS, loop=0)
    loop[0].resize((W * 5, H * 5), Image.NEAREST).save(os.path.join(out, "logo_loop_frame0.png"))
    big = [fr.resize((W * 5, H * 5), Image.NEAREST) for fr in loop]
    big[0].save(os.path.join(out, "logo_loop_preview.gif"), save_all=True, append_images=big[1:], duration=FRAME_MS, loop=0)
    info = gif2flc.convert(lgif, os.path.join(out, "logo_e.flc"), size=(W, H), fit="stretch", depth=8)
    print(f"{len(loop)} frames, {len(loop) * FRAME_MS / 1000:.2f} s loop -> {out}/logo_e.flc: {info}")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
