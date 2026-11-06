#!/usr/bin/env python3
"""Assemble PNG frames into an animated WebP (used by readme-demo.mjs).

Usage: frames-to-webp.py FRAMES_DIR DURATIONS_JSON OUT.webp   (durations in ms, one per frame)
Frames are scaled to WIDTH and the file is kept under the repository's 1 MB limit by lowering the quality.
"""
import json
import os
import sys

from PIL import Image

WIDTH, LIMIT = 800, 1_000_000


def main(frames_dir, durations_json, out):
    names = sorted(f for f in os.listdir(frames_dir) if f.endswith(".png"))
    durations = [max(20, int(d)) for d in json.load(open(durations_json))][:len(names)]
    frames = []
    for n in names:
        im = Image.open(os.path.join(frames_dir, n)).convert("RGB")
        frames.append(im.resize((WIDTH, round(im.height * WIDTH / im.width)), Image.LANCZOS))
    for quality in (65, 55, 45, 35, 25):
        frames[0].save(out, save_all=True, append_images=frames[1:], duration=durations, loop=0,
                       quality=quality, method=4)
        size = os.path.getsize(out)
        if size <= LIMIT:
            break
    print(f"{out}: {len(frames)} frames, {sum(durations) / 1000:.1f} s, quality {quality}, {size // 1024} KB")
    if size > LIMIT:
        raise SystemExit("still over 1 MB: shorten the recording or lower WIDTH")


if __name__ == "__main__":
    if len(sys.argv) != 4:
        raise SystemExit(__doc__)
    main(*sys.argv[1:])
