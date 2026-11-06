#!/usr/bin/env python3
"""An emulated sign behind a pseudo-terminal, for testing host tools (the Studio's Sign window) end to end.

The boot-flash image (with our boot loader, make -C bootloader DUMP=1 fwimage) boots on the emulated board of
bootloader/test/serdumpsim.py; its DB9 UART is wired to a pty. The pty's path is printed (and written to --pty-file).
When the board reaches the app, it reports which app is in RAM (compared with --app NAME=FILE, any number) and,
for a run from RAM, where the app's boot volume is, then exits.

  simsign_pty.py IMAGE [--app NAME=FILE ...] [--pty-file PATH] [--timeout SECONDS]
"""
import argparse
import os
import select
import sys
import time
import tty

HERE = os.path.dirname(os.path.abspath(__file__))
TEST = os.path.join(HERE, "..", "bootloader", "test")
sys.path.insert(0, TEST)
sys.path.insert(0, HERE)
import runsim  # noqa: E402
import serdumpsim as sd  # noqa: E402


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("image")
    ap.add_argument("--app", action="append", default=[])
    ap.add_argument("--pty-file")
    ap.add_argument("--timeout", type=float, default=900)
    a = ap.parse_args()
    img = open(a.image, "rb").read()
    syms = sd.symbols(os.path.join(os.path.dirname(a.image), "bootstrap.elf"))
    master, slave = os.openpty()
    tty.setraw(slave)
    path = os.ttyname(slave)
    print(f"PTY {path}", flush=True)
    if a.pty_file:
        open(a.pty_file, "w").write(path + "\n")
    os.set_blocking(master, False)
    b = runsim.board(img, syms)
    t_end = time.time() + a.timeout
    while time.time() < t_end:
        r, _, _ = select.select([master], [], [], 0)
        if r:
            try:
                data = os.read(master, 65536)
            except BlockingIOError:
                data = b""
            for c in data:
                b.uart.from_host.append((c, b.uart.host_baud))
        if not b.step(200_000):
            break
        if b.uart.to_host:
            os.write(master, bytes(b.uart.to_host))
            b.uart.to_host = bytearray()
    else:
        print("RESULT: TIMEOUT (the board never reached the app)", flush=True)
        return 1
    if b.uart.to_host:
        os.write(master, bytes(b.uart.to_host))
    boot_vol = runsim.be32(b, 0x2C)
    which = [n for n, f in (x.split("=", 1) for x in a.app) if sd.netsim.check_app(b, open(f, "rb").read())]
    print(f"APP {','.join(which) or 'unknown'} BOOTVOL {boot_vol:#x} "
          f"{'(the stage: run from RAM)' if boot_vol == runsim.STAGE_VIRT else '(the boot flash)'}", flush=True)
    time.sleep(0.5)
    return 0


if __name__ == "__main__":
    sys.exit(main())
