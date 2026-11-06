#!/usr/bin/env python3
"""Boot a complete boot-flash image in the system simulator, exactly as the sign would, and check the app it carries.

Unlike compare.py, nothing is swapped in: the boot loader in the image finds the ELITE_RGB entry, unpacks it (NRV2D)
and hands over. Checks:
  1. after the hand-off, RAM holds APP byte for byte, except what the boot loader writes on purpose (the hand-off
     block 0x14..0x9C and the MMU table at the pointer/length in 0x4C/0x50);
  2. the app's panel output equals the stock dump's (the same sequence of distinct frames; the stock run may be
     given a longer budget, as our compiled code is faster).

Usage: flashtest.py FLASH.bin APP [INSNS] [STOCK_INSNS]   (FLASH.bin from bootloader/tools/mkimage.py --app APP)
"""
import os
import struct
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(HERE)
sys.path.insert(0, HERE)
import appsim  # noqa: E402
import compare  # noqa: E402


def boot(flash, app, insns):
    import cosim
    b = appsim.AppBoard(flash, next(iter(cosim.SCENARIOS.values())), app)
    b.run()
    if not b.reached_app:
        raise SystemExit("the boot loader did not reach the app")
    ram = bytes(b.uc.mem_read(0, len(app)))
    tp, tl = struct.unpack_from(">II", ram, 0x4C)
    skip = [(0x14, 0x9C), (tp, tp + tl)]
    bad = [i for i in range(0, len(app), 4)
           if not any(lo <= i < hi for lo, hi in skip) and ram[i:i + 4] != app[i:i + 4]]
    b.hook_app_specials()
    S = appsim.sym()
    b.hook_busy_waits(S)
    appsim.watch_switches(b, S)
    b.run_app(insns, {})
    return b, bad


def main():
    if len(sys.argv) < 3:
        raise SystemExit(__doc__)
    flash = open(sys.argv[1], "rb").read()
    app = open(sys.argv[2], "rb").read()
    insns = int(sys.argv[3]) if len(sys.argv) > 3 else 80_000_000
    stock_insns = int(sys.argv[4]) if len(sys.argv) > 4 else insns
    b, bad = boot(flash, app, insns)
    print(f"1. RAM after the boot loader = {os.path.basename(sys.argv[2])}: "
          + ("yes (except the hand-off block and MMU table)" if not bad else f"NO, {len(bad)} words differ, first {bad[0]:#x}"))
    stock_flash = open(os.path.join(REPO, "dumps", "bootflash.bin"), "rb").read()
    stock_app = open(os.path.join(REPO, "analysis", "extracted", "ELITE_RGB"), "rb").read()
    a, _ = boot(stock_flash, stock_app, stock_insns)
    da, db = compare.distinct(a.frames), compare.distinct(b.frames)
    n = min(len(da), len(db))
    same = sum(1 for i in range(n) if da[i] == db[i])
    tasks = sorted(a.tasks) == sorted(b.tasks)
    print(f"2. stock dump: {len(da)} distinct frames, tasks {sorted(a.tasks)}")
    print(f"   this image: {len(db)} distinct frames, tasks {sorted(b.tasks)}")
    print(f"   {same} of the first {n} distinct frames identical")
    ok = not bad and tasks and n > 0 and same == n
    print("RESULT:", "PASS" if ok else "FAIL")
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
