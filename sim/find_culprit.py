#!/usr/bin/env python3
"""Find the replaced function(s) that break whole-system behaviour: bisect the rebuild set (rebuild.py --only) with
the simulator as the test: the boot animation must play, at least MIN_DISTINCT different 80x16 frames in INSNS
instructions. (Counting all frames is not enough: a stuck image repeats one blank frame.)
Usage: find_culprit.py [INSNS]"""
import os
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(HERE)
sys.path.insert(0, HERE)
TOOLS = os.path.join(REPO, "analysis", "tools")
INSNS = int(sys.argv[1]) if len(sys.argv) > 1 else 60_000_000
MIN_DISTINCT = 20
# MODE=alive: for failures later in the run. Good if only the stock tasks ever ran (a corrupted ready queue switches
# to a "task" that is not one) and the panel kept being fed: at least MIN_FRAMES frames in INSNS instructions.
MODE = os.environ.get("MODE", "boot")
STOCK_TASKS = {"INIT", "LEDP", "REST", "SMOD", "000afe18"}
MIN_FRAMES = int(os.environ.get("MIN_FRAMES", "440"))
# MODE=menu: press "!" on the remote after 700 frames; good if the panel then shows TARGET (a frame file: the stock
# image's first menu frame, from sim/compare_menu.py)
TARGET = os.environ.get("TARGET")


def good(names):
    """Build an image replacing only `names` and run it; True if the boot animation appears."""
    r = subprocess.run([os.path.join(REPO, "venv", "bin", "python"), "rebuild.py", "--trials", "4", "--only", *names],
                       cwd=TOOLS, capture_output=True, text=True)
    import compare
    if MODE == "menu":
        b = compare.run(os.path.join(REPO, "analysis", "build", "ELITE_RGB.rebuilt"), INSNS, menu_key_at=700)
        want = open(TARGET, "rb").read()
        ok = any(compare.distinct([f])[0] == want for f in b.frames if len(f) == 2560)
        print(f"  {len(names):4} functions replaced -> {len(b.frames)} frames, menu frame {'ok' if ok else 'BAD'}",
              flush=True)
        return ok
    b = compare.run(os.path.join(REPO, "analysis", "build", "ELITE_RGB.rebuilt"), INSNS)
    if MODE == "alive":
        extra = sorted(set(b.tasks) - STOCK_TASKS)
        ok = not extra and len(b.frames) >= MIN_FRAMES
        print(f"  {len(names):4} functions replaced -> {len(b.frames)} frames, unexpected tasks {extra} "
              f"{'ok' if ok else 'BAD'}", flush=True)
        return ok
    n = len(compare.distinct(b.frames))
    ok = n >= MIN_DISTINCT
    print(f"  {len(names):4} functions replaced -> {n} distinct frames {'ok' if ok else 'BAD'}", flush=True)
    return ok


def main():
    rep = open(os.path.join(REPO, "analysis", "build", "rebuild_report.txt")).read().split("\n\n", 1)[1]
    names = [l.split("\t")[0] for l in rep.strip().split("\n") if l]
    print(f"bisecting {len(names)} replaced functions")
    bad = names
    while len(bad) > 1:
        half = bad[:len(bad) // 2]
        if not good(half):
            bad = half
        elif not good(bad[len(bad) // 2:]):
            bad = bad[len(bad) // 2:]
        else:
            print("  both halves are fine alone: an interaction; stopping with", len(bad))
            break
    print("culprit(s):", bad)


if __name__ == "__main__":
    main()
