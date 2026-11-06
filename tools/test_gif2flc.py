#!/usr/bin/env python3
"""Check gif2flc output with the sign's own FLC decoders, run under Unicorn.

For every FLC frame the frame's sub-chunks are placed in emulated RAM, a player state block is set up
the way ELITE_RGB's player lays it out, and the firmware's chunk dispatcher is called:
  0x420FC  8-bit frames  (COLOR_256 -> palette, BRUN -> indices)
  0x42220  16-bit frames (byte-wise BRUN, then per-pixel byte swap)
The decoded RGB555 pixels must equal what gif2flc meant to encode. The FLC speed is also run through
the firmware's speed-to-hold-count routine (0x41C58).

Usage: test_gif2flc.py ELITE_RGB
"""
import os
import struct
import sys
import tempfile

from PIL import Image
from unicorn import Uc, UC_ARCH_PPC, UC_MODE_PPC32, UC_MODE_BIG_ENDIAN, UC_HOOK_CODE
from unicorn.ppc_const import UC_PPC_REG_1, UC_PPC_REG_3, UC_PPC_REG_LR, UC_PPC_REG_PC

import gif2flc as g

DISPATCH8, DISPATCH16, SPEED_HOLD = 0x420FC, 0x42220, 0x41C58
STATE, CHUNKS, PIXELS, PALETTE = 0x600000, 0x610000, 0x620000, 0x630000
STACK, STOP = 0x7F0000, 0x7FFF00


def make_test_gif(path):
    """6 frames, 40x20 (not the panel's aspect), its own palette per frame, a transparent hole, uneven delays."""
    frames = []
    for i in range(6):
        im = Image.new("RGBA", (40, 20), (20 * i, 255 - 40 * i, 60 + 30 * i, 255))
        for x in range(40):
            for y in range(20):
                if (x + y + i * 3) % 7 == 0:
                    im.putpixel((x, y), (255, (37 * i) % 256, (x * 6) % 256, 255))
        for x in range(15, 25):
            for y in range(6, 14):
                im.putpixel((x, y), (0, 0, 0, 0))          # transparent hole
        frames.append(im)
    frames[0].save(path, save_all=True, append_images=frames[1:], duration=[100, 50, 50, 200, 75, 25],
                   loop=0, disposal=2, transparency=0)


class FirmwareFLC:
    def __init__(self, app):
        self.uc = uc = Uc(UC_ARCH_PPC, UC_MODE_PPC32 | UC_MODE_BIG_ENDIAN)
        uc.mem_map(0, 0x800000)
        uc.mem_write(0, app)
        uc.hook_add(UC_HOOK_CODE, lambda u, a, s, d: u.emu_stop(), begin=STOP, end=STOP)

    def call(self, addr, *args):
        uc = self.uc
        for i, v in enumerate(args):
            uc.reg_write(UC_PPC_REG_3 + i, v)
        uc.reg_write(UC_PPC_REG_1, STACK)
        uc.reg_write(UC_PPC_REG_LR, STOP)
        uc.emu_start(addr, STOP, count=20_000_000)
        assert uc.reg_read(UC_PPC_REG_PC) == STOP, f"call to {addr:#x} did not return"
        return uc.reg_read(UC_PPC_REG_3)

    def setup(self, w, h, depth):
        uc = self.uc
        uc.mem_write(STATE, bytes(0x130))
        uc.mem_write(STATE + 0x48, struct.pack(">HHH", w, h, depth))            # header copy: w, h, depth
        fmt = 1 if depth == 16 else 2
        uc.mem_write(STATE + 0xD0, struct.pack(">IIIII", fmt, w, h, PIXELS, PALETTE))  # image
        uc.mem_write(STATE + 0x0C, struct.pack(">HH", w, h))
        uc.mem_write(PIXELS, bytes(w * h * 2))
        uc.mem_write(PALETTE, bytes(512))

    def decode_frame(self, subchunks_bytes, n, depth):
        uc = self.uc
        uc.mem_write(CHUNKS, subchunks_bytes)
        uc.mem_write(STATE + 0xC8, struct.pack(">H", n))
        uc.mem_write(STATE + 0xCC, struct.pack(">I", CHUNKS))
        self.call(DISPATCH16 if depth == 16 else DISPATCH8, STATE)

    def pixels555(self, w, h, depth):
        if depth == 16:
            raw = bytes(self.uc.mem_read(PIXELS, w * h * 2))
            return list(struct.unpack(f">{w * h}H", raw))
        idx = bytes(self.uc.mem_read(PIXELS, w * h))
        pal = struct.unpack(">256H", bytes(self.uc.mem_read(PALETTE, 512)))
        return [pal[i] for i in idx]


def flc_frames(data):
    """Yield (subchunk bytes, subchunk count) for each 0xF1FA frame of an FLC file."""
    off = struct.unpack_from("<I", data, 0x50)[0]
    while off + 16 <= len(data):
        size, ctype, n = struct.unpack_from("<IHH", data, off)
        if ctype == 0xF1FA:
            yield data[off + 16:off + size], n
        off += size


def main(app_path):
    app = open(app_path, "rb").read()
    fw = FirmwareFLC(app)
    ok = True
    with tempfile.TemporaryDirectory() as td:
        gif = os.path.join(td, "test.gif")
        make_test_gif(gif)
        frames, durations = g.load_frames(gif, (80, 16), "contain")
        speed, repeats = g.timing(durations)
        print(f"test GIF: {len(frames)} frames, delays {durations} ms -> FLC speed {speed} ms, repeats {repeats}")
        hold = fw.call(SPEED_HOLD, speed) & 0xFF
        good = (hold + 1) * g.TICK_MS == speed
        ok &= good
        print(f"{'ok  ' if good else 'FAIL'} firmware holds each frame {hold + 1} display frame(s) = "
              f"{(hold + 1) * g.TICK_MS} ms (intended {speed} ms)")
        expect = [f for f, r in zip(frames, repeats) for _ in range(r)]

        for depth in (16, 8):
            flc = os.path.join(td, f"test{depth}.flc")
            info = g.convert(gif, flc, (80, 16), "contain", depth)
            data = open(flc, "rb").read()
            fw.setup(80, 16, depth)
            bad = 0
            pal_img = g.build_palette(frames) if depth == 8 else None
            for i, ((sub, n), img) in enumerate(zip(flc_frames(data), expect)):
                fw.decode_frame(sub, n, depth)
                got = fw.pixels555(80, 16, depth)
                if depth == 16:
                    want = [g.rgb555(*img.getpixel((x, y))) for y in range(16) for x in range(80)]
                else:
                    q = img.quantize(palette=pal_img, dither=Image.NONE).convert("RGB")
                    want = [g.rgb555(*q.getpixel((x, y))) for y in range(16) for x in range(80)]
                if got != want:
                    bad += 1
                    diff = sum(a != b for a, b in zip(got, want))
                    print(f"     frame {i}: {diff} pixels differ")
            good = bad == 0 and info["frames_out"] == len(expect)
            ok &= good
            print(f"{'ok  ' if good else 'FAIL'} {depth}-bit FLC ({info['bytes']} bytes, {info['frames_out']} frames): "
                  f"firmware decoder output matches every pixel of every frame")
    print("\nALL GIF->FLC CHECKS PASS" if ok else "\nFAILURES")
    return 0 if ok else 1


if __name__ == "__main__":
    if len(sys.argv) != 2:
        raise SystemExit(__doc__)
    sys.exit(main(sys.argv[1]))
