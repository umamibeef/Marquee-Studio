#!/usr/bin/env python3
"""Send the same read-only DB9 probes to the stock ELITE_RGB and another image in the system simulator and compare
every byte each one sends back (docs/REBUILD-PLAN.md, 4f).

The DB9 is /dev/rs232 (SMC1, 115200 8N1): the .lfs download server. Only probes that cannot start a transfer
are sent, as on the real sign (never a valid 0x20/0x21/0x10 sequence):
  - "whoami": a 0x20 start header with a deliberately invalid name; the sign replies with the name it expects;
  - a header-only 0x90 frame.
They arrive after the same numbers of panel frames in both runs. Everything sent on any serial channel is captured,
including the A5 BB status frame the sign sends after 20 s of silence.

Usage: compare_db9.py [IMAGE] [INSNS] [STOCK_INSNS]
"""
import os
import struct
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(HERE)
sys.path.insert(0, HERE)
sys.path.insert(0, os.path.join(REPO, "studio"))
import appsim  # noqa: E402
from marquee_studio import signproto  # noqa: E402

PROBES = [(700, signproto.frame(0x20, b"\x00")), (900, signproto.frame(0x90))]


def run(image, insns):
    import cosim
    flash = open(os.path.join(REPO, "dumps", "bootflash.bin"), "rb").read()
    app = open(image, "rb").read()
    b = appsim.AppBoard(flash, next(iter(cosim.SCENARIOS.values())), app)
    b.run()
    ram = bytes(b.uc.mem_read(0, len(app)))
    tp, tl = struct.unpack_from(">II", ram, 0x4C)
    img = bytearray(app)
    for lo, hi in ((0x14, 0x9C), (tp, tp + tl)):
        img[lo:hi] = ram[lo:hi]
    b.uc.mem_write(0, bytes(img))
    b.hook_app_specials()
    S = appsim.sym()
    b.hook_busy_waits(S)
    appsim.watch_switches(b, S)
    for at_frame, data in PROBES:
        b.serial_send(4, data, at_frame=at_frame)
    b.run_app(insns, {})
    return b


def main():
    image = sys.argv[1] if len(sys.argv) > 1 else os.path.join(REPO, "analysis", "build", "fw", "ELITE_RGB")
    insns = int(sys.argv[2]) if len(sys.argv) > 2 else 600_000_000
    stock_insns = int(sys.argv[3]) if len(sys.argv) > 3 else 750_000_000
    a = run(os.path.join(REPO, "analysis", "extracted", "ELITE_RGB"), stock_insns)
    b = run(image, insns)
    for tag, x in (("stock", a), ("other", b)):
        print(f"{tag}: probes delivered {x.events.get('serial rx', 0)} of {len(PROBES)}, tasks {sorted(x.tasks)}")
        for minor, data in sorted(x.serial_tx.items()):
            frames, rest = signproto.parse_sign(data)
            print(f"  sent on minor {minor}: {data.hex(' ')}")
            for f in frames:
                print("    " + signproto.describe(f))
    ok = (a.events.get("serial rx") == b.events.get("serial rx") == len(PROBES) and a.serial_tx == b.serial_tx
          and a.serial_tx.get(4) and sorted(a.tasks) == sorted(b.tasks))
    print("RESULT:", "SAME REPLIES" if ok else "DIFFERENT")
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
