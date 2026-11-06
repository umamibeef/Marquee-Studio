#!/usr/bin/env python3
"""Convert an animated GIF (or APNG/WebP, anything Pillow can animate) to an FLC the sign can play.

What the sign's FLC player accepts (ELITE_RGB V4.20, traced 2026-10-31):
  - Magic 0xAF12 (FLC). The header speed is in milliseconds; the player holds each animation frame for a
    whole number of 25 ms display frames (40 fps), chosen from a threshold table (38, 63, 88, ... ms).
  - Frames are 0xF1FA chunks; a leading 0xF100 prefix chunk is skipped.
  - depth 8:  COLOR_256 (4), BRUN (15), SS2 (7), LC (12), BLACK (13), COPY (16), COLOR_64 (11).
              Palette entries become RGB555 by >> 3 on most boards (LED curve on one SPD board option).
  - depth 16: BRUN (15), SS2 (7), BLACK (13), COPY (16). Pixels are RGB555, little-endian, used as-is.
              NOTE: this firmware's 16-bit BRUN run-length-encodes the frame's raw BYTES (2*width per row,
              a positive count repeats one byte, a negative count copies bytes), then byte-swaps every pixel.
              A standard 16-bit FLC encoder (pixel-sized runs) would not decode correctly on this sign.

Every frame is written as a full BRUN frame, so playback and the player's rewind are always correct.

Usage: gif2flc.py input.gif output.flc [--size 80x16] [--fit contain|cover|stretch] [--depth 16|8] [--lut]
"""
import argparse
import os
import struct
import sys

from PIL import Image, ImageSequence

TICK_MS = 25            # the sign's display frame period (40 fps)
MAX_HOLD = 40           # the player's speed table has 41 entries


def led_luts():
    """The firmware's 8-bit -> 5-bit LED correction tables (red, green, blue), if ELITE_RGB is available."""
    from . import paths
    app = paths.FIRMWARE_APP
    if not os.path.exists(app):
        return None
    d = open(app, "rb").read()
    return [d[a:a + 256] for a in (0xB88A8, 0xB89A8, 0xB8AA8)]


def rgb555(r, g, b, lut=None):
    if lut:
        return (lut[0][r] & 31) << 10 | (lut[1][g] & 31) << 5 | (lut[2][b] & 31)
    return (r >> 3) << 10 | (g >> 3) << 5 | (b >> 3)


def _apng_frames(data):
    """An animated PNG's frames, composed as the APNG spec says (Pillow differs on one point: it lists a default image
    that the file marks as not part of the animation, and draws the first frame over it). Each frame's chunks
    become a small PNG that Pillow decodes; blend "over" is Pillow's paste with the frame's alpha as the mask."""
    import io
    import zlib

    def chunk(t, body):
        return struct.pack(">I", len(body)) + t + body + struct.pack(">I", zlib.crc32(t + body) & 0xFFFFFFFF)

    p, ihdr, extra, seen_idat, frames = 8, None, [], False, []
    while p + 12 <= len(data):
        n, t = struct.unpack_from(">I4s", data, p)
        body = data[p + 8:p + 8 + n]
        if t == b"IHDR":
            ihdr = body
        elif t == b"fcTL":
            frames.append([struct.unpack_from(">IIIIIHHBB", body), b"", not seen_idat])
        elif t == b"IDAT":
            seen_idat = True
            if frames and frames[-1][2]:
                frames[-1][1] += body
        elif t == b"fdAT" and frames:
            frames[-1][1] += body[4:]
        elif t == b"IEND":
            break
        elif not seen_idat and t != b"acTL":
            extra.append(chunk(t, body))
        p += 12 + n
    W, H = struct.unpack_from(">II", ihdr)
    canvas = Image.new("RGBA", (W, H), (0, 0, 0, 0))
    out = []
    for i, ((_, w, h, x, y, num, den, dispose, blend), z, _) in enumerate(frames):
        png = b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">II", w, h) + ihdr[8:]) + b"".join(extra) + \
            chunk(b"IDAT", z) + chunk(b"IEND", b"")
        sub = Image.open(io.BytesIO(png)).convert("RGBA")
        if i == 0 and dispose == 2:
            dispose = 1                                  # nothing to go back to: clear instead
        prev = canvas.copy() if dispose == 2 else None
        if blend == 0 or i == 0:
            canvas.paste(sub, (x, y))
        else:
            canvas.paste(sub, (x, y), sub)
        d = int(num / (den or 100) * 1000)
        out.append((canvas.copy(), d or 100))
        if dispose == 1:
            canvas.paste((0, 0, 0, 0), (x, y, x + w, y + h))
        elif dispose == 2:
            canvas = prev
    return out


def _source_frames(path):
    """(RGBA frame, duration in ms) of every frame of the input."""
    data = open(path, "rb").read()
    if data[:8] == b"\x89PNG\r\n\x1a\n":
        p = 8
        while p + 12 <= len(data):                       # acTL before the image data: animated
            n, t = struct.unpack_from(">I4s", data, p)
            if t == b"acTL":
                return _apng_frames(data)
            if t == b"IDAT":
                break
            p += 12 + n
    im = Image.open(path)
    out = []
    for fr in ImageSequence.Iterator(im):
        rgba = fr.convert("RGBA")                        # loads the frame: its duration is known only now
        out.append((rgba, fr.info.get("duration", im.info.get("duration", 100)) or 100))
    return out


def load_frames(path, size, fit, background=(0, 0, 0)):
    """Composited RGB frames at the target size, with their durations in ms."""
    W, H = size
    frames, durations = [], []
    for rgba, d in _source_frames(path):
        if fit == "stretch":
            scaled = rgba.resize((W, H), Image.LANCZOS)
        else:
            s = (min if fit == "contain" else max)(W / rgba.width, H / rgba.height)
            nw, nh = max(1, round(rgba.width * s)), max(1, round(rgba.height * s))
            scaled = rgba.resize((nw, nh), Image.LANCZOS)
            if fit == "cover":                           # crop the centre to the panel
                left, top = (nw - W) // 2, (nh - H) // 2
                scaled = scaled.crop((left, top, left + W, top + H))
        canvas = Image.new("RGBA", (W, H), background + (255,))
        canvas.alpha_composite(scaled, ((W - scaled.width) // 2, (H - scaled.height) // 2))
        frames.append(canvas.convert("RGB"))
        durations.append(int(d))
    return frames, durations


def timing(durations):
    """Map per-frame durations onto one FLC speed plus a repeat count per frame."""
    ticks = [max(1, round(d / TICK_MS)) for d in durations]
    g = ticks[0]
    for t in ticks[1:]:
        while t:
            g, t = t, g % t
    g = min(g, MAX_HOLD)
    return g * TICK_MS, [max(1, round(t / g)) for t in ticks]


def brun_bytes(row):
    """FLI BRUN packets for one row of bytes (count byte first, which the decoder ignores)."""
    out = bytearray([0])
    i, n = 0, len(row)
    while i < n:
        j = i
        while j < n and j - i < 127 and row[j] == row[i]:
            j += 1
        if j - i >= 3:                                   # repeat run
            out += bytes([j - i, row[i]])
            i = j
            continue
        j = i                                            # literal run up to the next repeat of 3+
        while j < n and j - i < 128:
            if j + 2 < n and row[j] == row[j + 1] == row[j + 2]:
                break
            j += 1
        out += bytes([(256 - (j - i)) & 0xFF]) + bytes(row[i:j])
        i = j
    return bytes(out)


def chunk(ctype, body):
    if len(body) % 2:
        body += b"\0"
    return struct.pack("<IH", 6 + len(body), ctype) + body


def frame_chunk(subchunks):
    body = b"".join(subchunks)
    return struct.pack("<IHH8x", 16 + len(body), 0xF1FA, len(subchunks)) + body


def encode16(img, lut):
    W, H = img.size
    px = img.load()
    rows = b"".join(brun_bytes(b"".join(struct.pack("<H", rgb555(*px[x, y], lut)) for x in range(W)))
                    for y in range(H))
    return chunk(15, rows)


def build_palette(frames):
    strip = Image.new("RGB", (frames[0].width, frames[0].height * len(frames)))
    for i, f in enumerate(frames):
        strip.paste(f, (0, i * f.height))
    pal_img = strip.quantize(colors=256, method=Image.MEDIANCUT, dither=Image.NONE)
    return pal_img


def encode8(img, pal_img, first):
    q = img.quantize(palette=pal_img, dither=Image.NONE)
    W, H = q.size
    data = q.tobytes()
    subs = []
    if first:
        pal = pal_img.getpalette()[:768]
        pal += [0] * (768 - len(pal))
        subs.append(chunk(4, struct.pack("<HBB", 1, 0, 0) + bytes(pal)))   # one packet, 256 colours
    subs.append(chunk(15, b"".join(brun_bytes(data[y * W:(y + 1) * W]) for y in range(H))))
    return subs


def convert(src, dst, size=(80, 16), fit="contain", depth=16, use_lut=False):
    frames, durations = load_frames(src, size, fit)
    speed, repeats = timing(durations)
    lut = led_luts() if use_lut else None
    seq = [f for f, r in zip(frames, repeats) for _ in range(r)]
    if depth == 16:
        chunks = [frame_chunk([encode16(f, lut)]) for f in seq]
    else:
        pal = build_palette(frames)
        chunks = [frame_chunk(encode8(f, pal, i == 0)) for i, f in enumerate(seq)]
    W, H = size
    oframe1 = 128
    oframe2 = 128 + len(chunks[0]) if len(chunks) > 1 else 0
    body = b"".join(chunks)
    hdr = struct.pack("<IHHHHHHI", 128 + len(body), 0xAF12, len(chunks), W, H, depth, 3, speed)
    hdr = hdr.ljust(0x50, b"\0") + struct.pack("<II", oframe1, oframe2)
    hdr = hdr.ljust(128, b"\0")
    open(dst, "wb").write(hdr + body)
    return {"frames_in": len(frames), "frames_out": len(chunks), "speed_ms": speed,
            "bytes": 128 + len(body), "size": size, "depth": depth}


def main(argv):
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("src")
    ap.add_argument("dst")
    ap.add_argument("--size", default="80x16")
    ap.add_argument("--fit", choices=["contain", "cover", "stretch"], default="contain")
    ap.add_argument("--depth", type=int, choices=[8, 16], default=16)
    ap.add_argument("--lut", action="store_true", help="apply the firmware's LED colour-correction curve")
    a = ap.parse_args(argv[1:])
    w, h = (int(v) for v in a.size.lower().split("x"))
    info = convert(a.src, a.dst, (w, h), a.fit, a.depth, a.lut)
    print(f"{a.dst}: {info['frames_in']} GIF frames -> {info['frames_out']} FLC frames at {info['speed_ms']} ms, "
          f"{w}x{h}, {info['depth']}-bit, {info['bytes']} bytes")


if __name__ == "__main__":
    main(sys.argv)
