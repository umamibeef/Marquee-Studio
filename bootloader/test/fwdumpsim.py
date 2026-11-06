#!/usr/bin/env python3
"""The firmware image with the serial dump window (make DUMP=1 fwimage) on an emulated board with the DB9 UART.

Checks, on the default board (SMC1):
  1. with the magic: the window answers; the boot flash read over serial equals the image; NVRAM and both EEPROMs
     read back as emulated; BOOT goes on to the app, and RAM holds FW_APP (the from-source app with extensions);
  2. without the magic: the window closes on its own and the board boots the same app;
  3. read-only: no store to flash, NVRAM or CS7, every I2C transaction a read.

Usage: fwdumpsim.py build-dump/bootflash-fw.bin FW_APP
"""
import os
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import serdumpsim as sd  # noqa: E402

sys.path.insert(0, os.path.join(HERE, "..", "..", "tools"))
import serdump  # noqa: E402


def main(img_path, app_path):
    img = open(img_path, "rb").read()
    app = open(app_path, "rb").read()
    syms = sd.symbols(os.path.join(os.path.dirname(img_path), "bootstrap.elf"))
    fixed = sd.cosim.SCENARIOS["fixed config, no upgrade (tries=0xFF)"]
    with tempfile.TemporaryDirectory() as out:
        print("=== with the magic ===")
        b = sd.DumpBoard(img, fixed, sd.SMC1, None, syms["dump_window"])
        s = serdump.Session(sd.SimLink(b), log=sd.quiet, block=8192)
        line = s.wait_window(30)
        sd.check(line == "MQDUMP READY 1 SMC1 115200", f"magic answered: {line!r}")
        s.hello()
        exp = sd.expected(b, img, fixed["spd"])
        for name in ("eeprom_a0", "eeprom_a2", "nvram", "bootflash"):
            data = s.dump_region(name, out)
            sd.check(data == exp[name], f"{name}: {len(exp[name])} bytes over serial, equal to the board's")
        sd.check(open(os.path.join(out, "bootflash.bin"), "rb").read() == img[:len(exp["bootflash"])],
                 "bootflash.bin equals the image (bootstrap, app, emoji.bin)")
        s.boot()
        b.run_to_app()
        sd.check(b.reached_app and sd.netsim.check_app(b, app), "BOOT: the app in RAM is FW_APP")
        sd.readonly_checks(b, "with the magic")
    print("=== without the magic ===")
    b = sd.DumpBoard(img, fixed, sd.SMC1, None, syms["dump_window"])
    b.run_to_app()
    sd.check(b.reached_app and sd.netsim.check_app(b, app), "window closes by itself; the app in RAM is FW_APP")
    sd.readonly_checks(b, "without the magic")
    print("RESULT:", "PASS" if not sd.FAILS else f"FAIL ({len(sd.FAILS)})")
    return 0 if not sd.FAILS else 1


if __name__ == "__main__":
    if len(sys.argv) != 3:
        raise SystemExit(__doc__)
    sys.exit(main(*sys.argv[1:]))
