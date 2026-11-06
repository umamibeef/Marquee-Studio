#!/usr/bin/env python3
"""Walk the on-screen menu with the IR remote in the stock ELITE_RGB and another image, and compare the panel
(docs/REBUILD-PLAN.md, 4f).

The remote is modelled at the pin: pulse-distance frames on PC13, decoded by the app's own ir_edge_isr
(sim/appsim.py, ir_press). The simulated NVRAM is blank, so no PIN is set and "!" (Enter) opens the menu. The keys
only navigate ("]" next, "}" into a submenu, "{" back, "-" exit); nothing is edited. They arrive after the same
numbers of panel frames in both runs, at least 300 ms of simulated time apart (appsim.ir_press: the app treats the
same code within 190 ms as one held key). Compared from the first key press on:
  - the display states, in order, and (with --edit) the whole NVRAM: exact;
  - the screens the menu settles on: frames shown for SETTLE panel frames or more, as a set. The menu's slide
    between items and the blinking cursor run on the tick clock, so builds that draw at different speeds catch
    different in-between frames; every settled screen must appear in both.

With --edit, a second walk changes a setting and saves it: "!" open, "[" x3 down to the RNG item, "}" changes it,
"-" leave, "}" turns the "Confirm Chg." prompt to accept, "-" confirms. Then the whole 32 KB NVRAM must
also be identical (the simulated sign's NVRAM; nothing real is written).

Usage: compare_menu.py [--edit] [IMAGE] [INSNS] [STOCK_INSNS]
"""
import os
import struct
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(HERE)
sys.path.insert(0, HERE)
import appsim  # noqa: E402
import compare  # noqa: E402

KEYS = "!" + "]" * 12 + "}" + "]" * 4 + "{-"
EDIT_KEYS = "![[[}-}-"
FIRST, GAP = 700, 15                     # panel frame of the first key, frames between keys
SETTLE = 10                              # panel frames a screen must stay to count as settled (a key gap is ~12)
DISPLAY_STATE = 0x45BFCC


def run(image, insns, keys=KEYS):
    import cosim
    from unicorn import UC_HOOK_MEM_WRITE
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
    for i, k in enumerate(keys):
        b.ir_press(k, at_frame=FIRST + GAP * i)
    b.states = []

    def on_state(uc, access, addr, size, value, _):
        if not b.states or b.states[-1] != value:
            b.states.append(value)
    b.uc.hook_add(UC_HOOK_MEM_WRITE, on_state, begin=DISPLAY_STATE, end=DISPLAY_STATE + 3)
    b.run_app(insns, {})
    b.nvram = bytes(b.uc.mem_read(cosim.NVRAM, 0x8000))
    return b


def settled(frames):
    """The distinct frames shown for SETTLE or more panel frames in a row (masked as compare.distinct does)."""
    out, run, prev = set(), 0, None
    for f in (compare.distinct([f])[0] for f in frames if len(f) == 2560):
        run = run + 1 if f == prev else 1
        prev = f
        if run == SETTLE:
            out.add(f)
    return out


def main():
    edit = "--edit" in sys.argv
    if edit:
        sys.argv.remove("--edit")
    keys = EDIT_KEYS if edit else KEYS
    image = sys.argv[1] if len(sys.argv) > 1 else os.path.join(REPO, "analysis", "build", "fw", "ELITE_RGB")
    insns = int(sys.argv[2]) if len(sys.argv) > 2 else 600_000_000
    stock_insns = int(sys.argv[3]) if len(sys.argv) > 3 else 680_000_000
    a = run(os.path.join(REPO, "analysis", "extracted", "ELITE_RGB"), stock_insns, keys)
    b = run(image, insns, keys)
    sa, sb = settled(a.frames[FIRST:]), settled(b.frames[FIRST:])
    for tag, x, d in (("stock", a, sa), ("other", b, sb)):
        print(f"{tag}: keys {x.events.get('ir keys', 0)} of {len(keys)}, display states {x.states}, "
              f"{len(d)} settled screens")
    print(f"settled screens: {len(sa & sb)} in both, {len(sa - sb)} only in stock, {len(sb - sa)} only in the other")
    nv = [i for i in range(len(a.nvram)) if a.nvram[i] != b.nvram[i]]
    print(f"NVRAM: {'identical' if not nv else f'{len(nv)} bytes differ, first at {nv[0]:#x}'}")
    ok = (a.events.get("ir keys") == b.events.get("ir keys") == len(keys) and 4 in a.states
          and a.states == b.states and sa == sb and not nv)
    print("RESULT:", "IDENTICAL" if ok else "DIFFERENT")
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
