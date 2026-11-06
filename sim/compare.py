#!/usr/bin/env python3
"""Run the stock ELITE_RGB and another image (default: the from-source build) in the system simulator and compare
what they put on the panel, frame by frame (docs/REBUILD-PLAN.md, milestone 6).

Timing may differ (our compiled code runs a different number of instructions), so frames are compared by content
and order: the sequence of distinct panel frames must be identical. Usage: compare.py [IMAGE] [INSNS] [STOCK_INSNS]   (STOCK_INSNS: give the stock run a longer budget when the other
image is faster)
"""
import hashlib
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(HERE)
sys.path.insert(0, HERE)
import appsim  # noqa: E402


def run(image, insns, menu_key_at=None):
    import cosim
    flash = open(os.path.join(REPO, "dumps", "bootflash.bin"), "rb").read()
    app = open(image, "rb").read()
    b = appsim.AppBoard(flash, next(iter(cosim.SCENARIOS.values())), app)
    b.run()
    import struct
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
    if menu_key_at is not None:
        b.ir_press("!", at_frame=menu_key_at)
    b.run_app(insns, {})
    return b


def distinct(frames):
    out = []
    for f in frames:
        if len(f) != 2560:
            continue
        f = bytes(x & (0x7F if i % 2 == 0 else 0xFF) for i, x in enumerate(f))   # bit 15 is the driver's marker
        if not out or out[-1] != f:
            out.append(f)
    return out


def main():
    image = sys.argv[1] if len(sys.argv) > 1 else os.path.join(REPO, "analysis", "build", "fw", "ELITE_RGB")
    insns = int(sys.argv[2]) if len(sys.argv) > 2 else 80_000_000
    a = run(os.path.join(REPO, "analysis", "extracted", "ELITE_RGB"), int(sys.argv[3]) if len(sys.argv) > 3 else insns)
    b = run(image, insns)
    da, db = distinct(a.frames), distinct(b.frames)
    n = min(len(da), len(db))
    same = sum(1 for i in range(n) if da[i] == db[i])
    print(f"stock: {len(a.frames)} frames, {len(da)} distinct; tasks {sorted(a.tasks)}")
    print(f"other: {len(b.frames)} frames, {len(db)} distinct; tasks {sorted(b.tasks)}")
    print(f"distinct frames compared in order: {same} of {n} identical")
    first = next((i for i in range(n) if da[i] != db[i]), None)
    if first is not None:
        print(f"first difference at distinct frame {first}")
    h = lambda fs: hashlib.sha256(b"".join(fs)).hexdigest()[:16]
    tasks_same = sorted(a.tasks) == sorted(b.tasks)
    if same == n and len(da) == len(db) and n > 0 and tasks_same:
        verdict = "IDENTICAL"
    elif same == n and n > 0 and tasks_same:
        # every frame matches as far as the shorter run got: the images run at different speeds (our compiled code
        # takes fewer or more instructions), so one is further along within the same instruction budget
        verdict = (f"PREFIX (all {n} frames the shorter run produced match; stock {len(da)}, other {len(db)}: "
                   f"compare a longer run of the slower one)")
    else:
        why = [] if tasks_same else ["different task set"]
        if len(da) != len(db):
            why.append(f"{len(da)} vs {len(db)} distinct frames")
        if same != n:
            why.append(f"{n - same} of the first {n} differ")
        verdict = "DIFFERENT (" + "; ".join(why) + ")"
    print("RESULT:", verdict, f"(sequence hash {h(da)} vs {h(db)})")
    return 0 if verdict == "IDENTICAL" else 1

if __name__ == "__main__":
    sys.exit(main())
