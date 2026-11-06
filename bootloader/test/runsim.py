#!/usr/bin/env python3
"""Run from RAM: the serial window stages a whole boot-flash image in SDRAM and the boot goes on from it.

On an emulated board with the DB9 UART, fixed-config path (the real sign's), chip = IMG (make DUMP=1 fwimage):
  1. stage STAGED (another image) and RUN: the app in RAM is STAGED's app, not the chip's; the app's hand-off says
     its boot flash is the staged copy (RAM 0x2C/0x30) and its RAM ends below it (RAM 0x14 cut by the stage);
     the staged bytes are still there; no store to flash, NVRAM or CS7, and in the window SDRAM stores only to
     the stage and the receive buffer;
  2. RUN with a wrong CRC: refused (E_CHECK), the window stays open, and BOOT boots the chip's app;
  3. WRITE to a region other than the stage: refused (E_WRITE);
  4. no magic: the chip's app, as before.

Usage: runsim.py build-dump/bootflash-fw.bin CHIP_APP STAGED_IMAGE STAGED_APP
"""
import ctypes
import os
import struct
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import serdumpsim as sd  # noqa: E402

sys.path.insert(0, os.path.join(HERE, "..", "..", "tools"))
import serdump  # noqa: E402

RAM_TOTAL = 0x08000000            # what the config EEPROM declares (128 MB)


STAGE_PHYS, STAGE_VIRT = 0x0EF80000, 0x07F80000   # this config: 8 banks of 16 MB, the last at 0x0E000000
_keep = []


def board(img, syms):
    """The emulator has no MMU (virtual = physical), so the stage's physical address, used by the boot loader
    with the MMU off, is mapped onto the same memory as its virtual address, where the app sees it."""
    fixed = sd.cosim.SCENARIOS["fixed config, no upgrade (tries=0xFF)"]
    b = sd.DumpBoard(img, fixed, sd.SMC1, None, syms["dump_window"])
    b.uc.mem_map(sd.cosim.RAM_SIZE, STAGE_VIRT - sd.cosim.RAM_SIZE)
    buf = ctypes.create_string_buffer(serdump.STAGE_SIZE)
    _keep.append(buf)
    b.uc.mem_map_ptr(STAGE_VIRT, serdump.STAGE_SIZE, 7, ctypes.addressof(buf))
    b.uc.mem_map_ptr(STAGE_PHYS, serdump.STAGE_SIZE, 7, ctypes.addressof(buf))
    return b


def be32(b, a):
    return struct.unpack(">I", bytes(b.uc.mem_read(a, 4)))[0]


def stage_bounds(b):
    """The stage from the board's bank list and bank size (DPRAM), as stage_where() works it out."""
    dp = bytes(b.uc.mem_read(sd.IMMR + 0x2000, 0x1000))
    bank = struct.unpack_from(">I", dp, 0x240 + 0x08)[0]
    banks = []
    for i in range(16):
        v = struct.unpack_from(">I", dp, 0x398 + 4 * i)[0]
        if v == 0xFFFFFFFF:
            break
        banks.append(v)
    return banks[-1] + bank - serdump.STAGE_SIZE, len(banks) * bank - serdump.STAGE_SIZE, bank * len(banks)


def store_checks(b, label, allowed):
    bad = [w for w in b.writes if not (w[0] == "SDRAM" and any(lo <= w[2] < hi for lo, hi in allowed))]
    sd.check(not bad, f"{label}: no store to flash, NVRAM or CS7; window SDRAM stores only to the stage"
             + (f" (first: {bad[0][0]} {bad[0][2]:#x} from pc {bad[0][1]:#x})" if bad else ""))


def main(img_path, chip_app_path, staged_path, staged_app_path):
    img = open(img_path, "rb").read()
    chip_app = open(chip_app_path, "rb").read()
    staged = open(staged_path, "rb").read()
    staged_app = open(staged_app_path, "rb").read()
    syms = sd.symbols(os.path.join(os.path.dirname(img_path), "bootstrap.elf"))

    print("=== 1. stage another image and RUN ===")
    b = board(img, syms)
    s = serdump.Session(sd.SimLink(b), log=sd.quiet, block=8192)
    sd.check(s.wait_window(30) == "MQDUMP READY 1 SMC1 115200", "window answers")
    info = s.hello()
    sd.check("region=8 stage" in info, "HELLO lists the stage region")
    n = s.stage_image(staged)
    phys, virt, mapped = stage_bounds(b)
    sd.check((phys, virt) == (STAGE_PHYS, STAGE_VIRT), f"stage at physical {phys:#x}, virtual {virt:#x}")
    sd.check(bytes(b.uc.mem_read(phys, n)) == staged[:n], f"{n} bytes staged, equal to the image")
    s.run_staged(staged, n)
    b.run_to_app()
    sd.check(b.reached_app and sd.netsim.check_app(b, staged_app), "RUN: the app in RAM is the staged image's app")
    sd.check(not sd.netsim.check_app(b, chip_app), "  (and not the chip's app)")
    sd.check(be32(b, 0x2C) == virt and be32(b, 0x30) == serdump.STAGE_SIZE,
             f"hand-off: boot flash = the stage (RAM 0x2C {be32(b, 0x2C):#x}, 0x30 {be32(b, 0x30):#x})")
    sd.check(be32(b, 0x14) == mapped - serdump.STAGE_SIZE,
             f"hand-off: the app's RAM ends below the stage (RAM 0x14 {be32(b, 0x14):#x})")
    sd.check(bytes(b.uc.mem_read(phys, n)) == staged[:n], "the staged image is intact after the load")
    store_checks(b, "RUN", [(phys, phys + serdump.STAGE_SIZE)])

    print("=== 2. RUN with a wrong CRC ===")
    b = board(img, syms)
    s = serdump.Session(sd.SimLink(b), log=sd.quiet, block=8192)
    s.wait_window(30)
    s.hello()
    n = s.stage_image(staged)
    try:
        s.request(serdump.RUN, struct.pack(">II", n, serdump.crc32(staged[:n]) ^ 1), timeout=3.0, retries=1)
        sd.check(False, "a wrong CRC is refused")
    except serdump.DumpError as e:
        sd.check("failed its check" in str(e), f"a wrong CRC is refused ({e})")
    try:
        s.ping(timeout=0.5, retries=2)
        sd.check(True, "the window stays open (PING answered)")
    except serdump.DumpError as e:
        sd.check(False, f"the window stays open ({e})")
    s.boot()
    b.run_to_app()
    sd.check(b.reached_app and sd.netsim.check_app(b, chip_app), "BOOT afterwards: the chip's app")
    sd.check(be32(b, 0x2C) != virt, "  with the boot flash as its boot volume")

    print("=== 3. WRITE outside the stage ===")
    b = board(img, syms)
    s = serdump.Session(sd.SimLink(b), log=sd.quiet, block=8192)
    s.wait_window(30)
    s.hello()
    for rid in (1, 4, 7):
        try:
            s.request(serdump.WRITE, struct.pack(">B3xI", rid, 0) + b"\0" * 16, timeout=1.0, retries=1)
            sd.check(False, f"WRITE to region {rid} refused")
        except serdump.DumpError as e:
            sd.check("stage region only" in str(e), f"WRITE to region {rid} refused ({e})")
    s.boot()
    b.run_to_app()
    store_checks(b, "WRITE refused", [])

    print("=== 4. no magic ===")
    b = board(img, syms)
    b.run_to_app()
    sd.check(b.reached_app and sd.netsim.check_app(b, chip_app), "the chip's app, as before")
    print("RESULT:", "PASS" if not sd.FAILS else f"FAIL ({len(sd.FAILS)})")
    return 0 if not sd.FAILS else 1


if __name__ == "__main__":
    if len(sys.argv) != 5:
        raise SystemExit(__doc__)
    sys.exit(main(*sys.argv[1:]))
