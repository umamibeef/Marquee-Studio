#!/usr/bin/env python3
"""The DB9 serial console (firmware/ext/console.c), tested in the running firmware in the system simulator.

The full boot-flash image boots as on the sign (the loader finds the EXT entry). Over the emulated DB9 (SMC1):
  1. the magic line opens the console: banner and prompt;
  2. help, info, md (RAM, the boot flash; refused outside the regions), mw (RAM only; refused for flash);
  3. exit hands the port back: a read-only download-server probe then gets the same reply as on the stock firmware.
The console's state (+0x80 in the extension block) shows it closed at the end.

Usage: test_console.py [FLASH.bin]   (default bootloader/build/bootflash-fw.bin, from make fwimage)
"""
import os
import re
import struct
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(HERE)
sys.path.insert(0, HERE)
sys.path.insert(0, os.path.join(REPO, "studio"))
sys.path.insert(0, os.path.join(REPO, "bootloader", "test"))
import appsim  # noqa: E402
import cosim  # noqa: E402
from marquee_studio import signproto  # noqa: E402

DB9 = 4                                     # minor 4 = SMC1 = /dev/rs232 on the default board
FAILS = []


def check(ok, what):
    print(("ok   " if ok else "FAIL ") + what)
    if not ok:
        FAILS.append(what)


def board(flash, app, sends, insns):
    b = appsim.AppBoard(flash, next(iter(cosim.SCENARIOS.values())), app)
    b.run()
    if not b.reached_app:
        raise SystemExit("the boot loader did not reach the app")
    b.hook_app_specials()
    S = appsim.sym()
    b.hook_busy_waits(S)
    appsim.watch_switches(b, S)
    for at_frame, data in sends:
        b.serial_send(DB9, data, at_frame=at_frame)
    b.run_app(insns, {})
    return b


def main():
    flash_path = sys.argv[1] if len(sys.argv) > 1 else os.path.join(REPO, "bootloader", "build", "bootflash-fw.bin")
    flash = open(flash_path, "rb").read()
    app = open(os.path.join(REPO, "analysis", "build", "fw", "ELITE_RGB-ext"), "rb").read()
    probe = signproto.frame(0x20, b"\x00")              # "whoami": an invalid name, the sign answers with its own
    lines = [b"MQ?MON\r", b"help\r", b"info\r", b"md f8180000 20\r", b"md 4000 10\r", b"md 70000000 4\r",
             b"mw c6cc00 12345678\r", b"md c6cc00 4\r", b"mw f8780000 1\r", b"bogus\r", b"exit\r"]
    sends = [(700 + 10 * i, l) for i, l in enumerate(lines)] + [(700 + 10 * len(lines) + 40, probe)]
    b = board(flash, app, sends, 1_000_000_000)
    tx = b.serial_tx.get(DB9, b"")
    text = tx.decode("latin-1")
    print(f"run: {len(b.frames)} frames, {b.icount} instructions, events {b.events}, rx left {len(b.rx_queue)}")
    print("---- DB9 output ----\n" + text.replace("\r", "") + "\n--------------------")
    check("MQMON 1 marquee-sign console" in text and "mq> " in text, "the magic line opens the console")
    check("md ADDR [LEN]" in text and "exit" in text, "help lists the commands")
    check(re.search(r"ram top [0-9a-f]{8}, heap at [0-9a-f]{8}", text) is not None, "info answers")
    boot = flash[:0x20]
    want = " ".join(f"{c:02x}" for c in boot[:16])
    check(f"f8180000: {want}" in text, "md of the boot flash shows its bytes")
    ram = bytes(b.uc.mem_read(0x4000, 16))
    check("00004000: " + " ".join(f"{c:02x}" for c in ram) in text, "md of RAM shows its bytes")
    check(text.count("? range") == 1, "md outside RAM / boot flash / NVRAM is refused")
    check("00c6cc00: 12 34 56 78" in text and struct.unpack(">I", bytes(b.uc.mem_read(0xC6CC00, 4)))[0] == 0x12345678,
          "mw writes a RAM word")
    check("? mw ADDR WORD" in text, "mw to the NVRAM is refused")
    check("? unknown command" in text and "bye" in text, "unknown commands answered; exit says bye")
    st = 0xC5CE50 + 0x80
    check(struct.unpack(">I", bytes(b.uc.mem_read(st, 4)))[0] == 0, "the console is closed at the end")
    after = tx[tx.rfind(b"bye\r\n") + 5:] if b"bye\r\n" in tx else b""

    print("---- the stock firmware, the same probe ----")
    stock = board(open(os.path.join(REPO, "dumps", "bootflash.bin"), "rb").read(),
                  open(os.path.join(REPO, "analysis", "extracted", "ELITE_RGB"), "rb").read(),
                  [(700 + 10 * len(lines) + 40, probe)], 750_000_000)
    stock_tx = stock.serial_tx.get(DB9, b"")
    check(stock_tx and after.startswith(stock_tx[:len(after)]) and after[:len(stock_tx)] == stock_tx[:len(after)]
          and len(after) >= min(len(stock_tx), 8),
          f"after exit the download server answers the probe as on the stock firmware ({after[:24].hex()} vs "
          f"{stock_tx[:24].hex()})")
    print("RESULT:", "PASS" if not FAILS else f"FAIL ({len(FAILS)})")
    return 0 if not FAILS else 1


if __name__ == "__main__":
    sys.exit(main())
