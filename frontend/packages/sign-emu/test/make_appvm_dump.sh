#!/bin/sh
# The firmware's drawn frames of the app scenario in appvm_scenario.json, for appvm_firmware.test.ts: the bundles and
# console commands as the Studio makes them (studio/marquee_studio/store_upload.py), replayed into our firmware in
# the C++ simulator (simulator/tests/fw_run.cpp). The bundles go next to the dump, so the test runs the same bytes.
set -e
ROOT=$(cd "$(dirname "$0")/../../../.." && pwd)
OUT=$ROOT/build/sim-test/appvmdump
rm -rf "$OUT" && mkdir -p "$OUT"
(cd "$ROOT/studio" && "$ROOT/venv/bin/python" - "$ROOT/frontend/packages/sign-emu/test/appvm_scenario.json" "$OUT" <<'PY'
import json, sys, calendar, re
from marquee_studio import store_upload as su
spec, out = json.load(open(sys.argv[1])), sys.argv[2]
import os, shutil
from PIL import Image
here = os.path.dirname(os.path.abspath(sys.argv[1]))
for p in spec["placements"]:                      # test apps (test/apps/<name>): built from a copy, assets made here
    src = os.path.join(here, "apps", p["app"])
    if os.path.isdir(src):
        d = os.path.join(out, "src", p["app"])
        shutil.copytree(src, d, dirs_exist_ok=True)
        if p["app"] == "drawtest":                # an 8 x 8 arrow, transparent around it, two frames (GIF)
            os.makedirs(os.path.join(d, "assets"), exist_ok=True)
            fr = []
            for k in range(2):
                im = Image.new("RGBA", (8, 8), (0, 0, 0, 0))
                for y in range(8):
                    for x in range(8):
                        if abs(y - 3.5) <= x / 2 - 0.5:
                            im.putpixel((x, y), (255, 128 * k, 0, 255))
                fr.append(im)
            fr[0].save(os.path.join(d, "assets", "arrow.gif"), save_all=True, append_images=fr[1:], duration=100,
                       transparency=0, disposal=2)
        p["app"] = d
from marquee_studio import native_emoji as ne   # imported emoji: into a folder of the dump's own
ne.CUSTOM_DIR = os.path.join(out, "emoji")
for c in spec.get("custom_emoji", []):            # a 6 x 6 block walking across a 16 x 16 cell, colour by frame
    fr = []
    for k, _ in enumerate(c["durations"]):
        im = Image.new("RGBA", (16, 16), (0, 0, 0, 0))
        for y in range(5, 11):
            for x in range(6):
                im.putpixel((2 + 2 * k + x, y), (255 * (k % 2), 255, 64 * k, 255))
        fr.append(im)
    import io
    b = io.BytesIO()
    fr[0].save(b, "GIF", save_all=True, append_images=fr[1:], duration=c["durations"], loop=0, disposal=2,
               )
    ne.add_custom(b.getvalue(), c["name"])
files = su.apps_files(spec["placements"])
for n, b in files.items():
    open(f"{out}/{n}", "wb").write(b)
y, mo, d, h, mi, se = map(int, re.split(r"[- :]", spec["time"]))
T = calendar.timegm((y, mo, d, h, mi, se, 0, 0, 0))
cmds = su.store_commands(files) + [f"time {T:x}"]
for line, vals in spec.get("data", {}).items():
    cmds += su.data_commands(int(line), vals)
cmds.append("apps")
open(f"{out}/cmds.txt", "w").write("\n".join(su.as_text(cmds)) + "\n")
open(f"{out}/time", "w").write(str(T))
print(len(cmds), "console lines,", sum(len(b) for b in files.values()), "bytes of apps")
PY
)
# the bundles go into the store as the Sign window sends them; then the drawn frames of the running firmware
FILES=""
for f in "$OUT"/app*.mqap "$OUT"/emoji.bin; do FILES="$FILES file:$(basename "$f")=$f"; done
FC=$(/opt/homebrew/opt/llvm/bin/llvm-nm "$ROOT/bootloader/build-dump/fw/app.elf" | awk '$3=="effect_frame_count"{print $1}')
PP=$(/opt/homebrew/opt/llvm/bin/llvm-nm "$ROOT/bootloader/build-dump/fw/app.elf" | awk '$3=="panel_present"{print $1}')
REPO="$ROOT" "$ROOT/build/simulator/fw_run" "$ROOT/bootloader/build-dump/bootflash-fw.bin" /dev/null 900000000 \
    cmd:sbegin $FILES cmd:scommit "cmdfile:$OUT/cmds.txt" cmd:apps mark "drawn:$OUT/drawn.bin:$FC:$PP" | tail -1
