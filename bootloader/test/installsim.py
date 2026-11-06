#!/usr/bin/env python3
"""Permanent updates over the DB9: the serial window's INSTALL / UNINSTALL and the boot from the update slot.

The main flash is an AM29LV128-style command model (unlock cycles, program clears bits, 64 KB sector erase, data
reads; any other store is a violation). On the fixed-config board, chip = IMG (make DUMP=1 fwimage):
  1. INSTALL a staged image: the slot (the last 512 KB of the main flash) holds it, the NVRAM boot count is 0, only
     the slot was erased and programmed;
  2. then BOOT: the app in RAM is the slot's app; the hand-off gives the slot as the boot volume and keeps it out
     of the app's download volume;
  3. a slot whose image fails its check: the chip's app;
  4. UNINSTALL: the chip's app again;
  5. boot count 0xFF (upgrades given up): the chip's app even with a good slot.

Usage: installsim.py build-dump/bootflash-fw.bin CHIP_APP IMAGE IMAGE_APP
"""
import os
import random
import struct
import zlib
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import runsim  # noqa: E402
import serdumpsim as sd  # noqa: E402

sys.path.insert(0, os.path.join(HERE, "..", "..", "tools"))
import serdump  # noqa: E402
import dbgrecord  # noqa: E402

MAIN, SIZE, SECTOR = sd.MAIN, 0x2000000, 0x10000
CHIPS = (0x50000000, 0x50800000)        # this config: bank 1 (8 MB) on CS4, bank 2 (4 MB) on CS5
SLOT = 0x50800000 + 0x400000 - 0x80000
NVRAM = sd.NVRAM


class FlashModel:
    def __init__(self, data):
        self.data = bytearray(data)
        self.state = 0
        self.violations, self.erased, self.programmed = [], set(), set()
        self.busy = 0            # reads left before the embedded operation ends
        self.final = 0           # bit 7 of the word being written (data polling)
        self.toggle = 0
        self.rng = random.Random(9)

    def start(self, reads, final):
        self.busy, self.final = reads, final

    def read(self, uc, off, size, _):
        if self.busy:            # status reads: DQ7 = complement of the final bit 7, DQ6 toggles, DQ5 low,
            self.busy -= 1       # the upper byte undefined (random), as on the real parts in word mode
            self.toggle ^= 0x40
            s = (~self.final & 0x80) | self.toggle | (self.rng.getrandbits(5) & 0x1F)
            s |= self.rng.getrandbits(8) << 8
            return s if size == 2 else (s << 16 | s)
        return int.from_bytes(self.data[off:off + size], "big")

    def write(self, uc, off, size, value, _):
        a = MAIN + off
        chip = max(c for c in CHIPS if c <= a)
        rel = (a - chip) // 2
        v = value & 0xFFFF
        if size != 2:
            self.violations.append(("size", hex(a), size)); return
        st = self.state
        if st != 6 and v == 0xF0:                       # reset, except as the data of a program cycle
            self.state = 0; return
        if st in (0, 3) and rel == 0x555 and v == 0xAA:
            self.state = 1 if st == 0 else 4
        elif st in (1, 4) and rel == 0x2AA and v == 0x55:
            self.state = 2 if st == 1 else 5
        elif st == 2 and rel == 0x555 and v == 0xA0:
            self.state = 6
        elif st == 2 and rel == 0x555 and v == 0x80:
            self.state = 3
        elif st == 6:                                   # program: bits only go from 1 to 0
            o = a - MAIN
            old = int.from_bytes(self.data[o:o + 2], "big")
            self.data[o:o + 2] = (old & v).to_bytes(2, "big")
            self.programmed.add(a & ~(SECTOR - 1))
            self.start(3, v)
            self.state = 0
        elif st == 5 and v == 0x30:                     # sector erase
            o = (a - MAIN) & ~(SECTOR - 1)
            self.data[o:o + SECTOR] = b"\xff" * SECTOR
            self.erased.add(MAIN + o)
            self.start(50, 0xFF)
            self.state = 0
        else:
            self.violations.append(("sequence", hex(a), hex(v), st))
            self.state = 0


def board(img, syms, main, tries):
    b = runsim.board(img, syms)
    b.uc.mem_write(NVRAM + 0x7FF0, bytes([tries, 0]))
    b.uc.mem_unmap(MAIN, SIZE)
    fm = FlashModel(main)
    b.uc.mmio_map(MAIN, SIZE, fm.read, None, fm.write, None)
    b.fm = fm
    return b


class FlashModel8:
    """The boot flash (CS0): an 8-bit AMD part, byte command addresses 0x555/0x2AA, 64 KB sectors. Reads return the
    stored byte unless an embedded operation is busy (then DQ7 complement / DQ6 toggle, as the real part)."""
    def __init__(self, data, base):
        self.data = bytearray(data)
        self.base = base
        self.state = 0
        self.violations, self.erased, self.programmed = [], set(), set()
        self.busy = 0
        self.final = 0
        self.toggle = 0
        self.rng = random.Random(8)

    def read(self, uc, off, size, _):
        if self.busy and size == 1:
            self.busy -= 1
            self.toggle ^= 0x40
            return (~self.final & 0x80) | self.toggle | (self.rng.getrandbits(5) & 0x1F)
        return int.from_bytes(self.data[off:off + size], "big")

    def write(self, uc, off, size, value, _):
        if size != 1:
            self.violations.append(("size", hex(self.base + off), size)); return
        rel, v, st = off, value & 0xFF, self.state
        if st != 6 and v == 0xF0:
            self.state = 0; return
        if st in (0, 3) and rel == 0x555 and v == 0xAA:
            self.state = 1 if st == 0 else 4
        elif st in (1, 4) and rel == 0x2AA and v == 0x55:
            self.state = 2 if st == 1 else 5
        elif st == 2 and rel == 0x555 and v == 0xA0:
            self.state = 6
        elif st == 2 and rel == 0x555 and v == 0x80:
            self.state = 3
        elif st == 2 and rel == 0x555 and v == 0x90:
            self.state = 7                              # autoselect (id)
        elif st == 6:
            self.data[off] &= v
            self.programmed.add(off & ~(SECTOR - 1))
            self.busy, self.final = 2, v
            self.state = 0
        elif st == 5 and v == 0x30:
            o = off & ~(SECTOR - 1)
            self.data[o:o + SECTOR] = b"\xff" * SECTOR
            self.erased.add(o)
            self.busy, self.final = 40, 0xFF
            self.state = 0
        else:
            self.violations.append(("sequence", hex(self.base + off), hex(v), st))
            self.state = 0


def model_bootflash(b):
    """Swap the boot flash (plain memory) for an 8-bit AMD model, seeded from what is there. For stage-1 scenarios:
    stage 0 has already run from it as real memory; stage 1 (in RAM) only writes and verifies it."""
    cur = bytes(b.uc.mem_read(sd.FLASH, 0x80000))
    b.uc.mem_unmap(sd.FLASH, 0x80000)
    bf = FlashModel8(cur, sd.FLASH)
    b.uc.mmio_map(sd.FLASH, 0x80000, bf.read, None, bf.write, None)
    b.bf = bf
    return bf


def boots(b, app):
    return b.reached_app and sd.netsim.check_app(b, app)


LOADER = SLOT - 0x10000


def s1_board(img, syms, main, s1_tries=None):
    b = board(img, syms, main, 0xFF)
    if s1_tries is not None:                                # a debug record already started, with this count
        b.uc.mem_write(NVRAM + dbgrecord.OFFSET, b"MQDB" + struct.pack(">I", 1) + bytes([0, s1_tries]))
    return b


def s1_tries(b):
    return bytes(b.uc.mem_read(NVRAM + dbgrecord.OFFSET + 9, 1))[0]


def two_stages(img, syms, chip_app, image_main, image_app, stage1, image_path):
    """Two stages (include/serdump.h): stage 1 installed over the window, then booted from the loader sector."""
    print("=== 9. two stages: INSTALL_S1 ===")
    blank = random.Random(6).randbytes(SIZE)
    b = board(img, syms, blank, 0xFF)
    s = serdump.Session(sd.SimLink(b), log=sd.quiet, block=8192)
    s.wait_window(30)
    sd.check(" s=0" in s.hello(), "the chip's boot loader answers (s=0)")
    s.install_loader(stage1)
    o = LOADER - MAIN
    sd.check(bytes(b.fm.data[o:o + len(stage1)]) == stage1, f"the loader sector holds stage 1 ({len(stage1)} bytes)")
    sd.check(b.fm.erased == {LOADER}, "only the loader sector was erased")
    with_s1 = bytes(b.fm.data)

    print("=== 10. two stages: the next boot runs stage 1 ===")
    b = s1_board(img, syms, with_s1)
    s = serdump.Session(sd.SimLink(b), log=sd.quiet, block=8192)
    s.wait_window(30)
    info = s.hello()
    sd.check(" s=1" in info, "the window is stage 1's (s=1)")
    sd.check(s1_tries(b) == 1, "stage 0 counted the try")
    s.boot()
    b.run_to_app()
    sd.check(boots(b, chip_app), "stage 1 boots the chip's app (no slot installed)")
    sd.check(s1_tries(b) == 0, "stage 1 cleared the try count at the hand-off")
    rec = dbgrecord.decode(bytes(b.uc.mem_read(NVRAM + dbgrecord.OFFSET, dbgrecord.SIZE)))
    sd.check(rec["boots"][-1]["step"] == 0x23, "the record: handed off")

    print("=== 11. two stages: stage 1 boots the update slot ===")
    both = bytearray(image_main)
    both[o:o + 0x10000] = with_s1[o:o + 0x10000]
    b = s1_board(img, syms, bytes(both))
    b.uc.mem_write(NVRAM + 0x7FF0, bytes([0, 0]))
    b.run_to_app()
    sd.check(boots(b, image_app), "the slot's app, booted by stage 1")
    top = runsim.be32(b, 0x14)
    sd.check(runsim.be32(b, 0x2C) == top, f"boot volume = the stage ({runsim.be32(b, 0x2C):#x})")
    sd.check(s1_tries(b) == 0, "  and stage 1 cleared its try count")

    print("=== 12. two stages: the recovery window (stage 0) ===")
    b = s1_board(img, syms, with_s1)
    s = serdump.Session(sd.SimLink(b), log=sd.quiet, block=8192)
    s.wait_window(30, serdump.RECOVERY_MAGIC)
    sd.check(" s=0" in s.hello(), "MQ?DUMP0 reaches the chip's boot loader (s=0) with stage 1 installed")
    s.boot()
    b.run_to_app()
    sd.check(boots(b, chip_app) and s1_tries(b) == 0, "it boots on by itself, stage 1 not tried")

    print("=== 13. two stages: tries used up, a bad image ===")
    b = s1_board(img, syms, with_s1, s1_tries=2)
    b.run_to_app()
    sd.check(boots(b, chip_app) and s1_tries(b) == 2, "after 2 failed tries the chip's boot loader boots alone")
    bad = bytearray(with_s1)
    bad[o + 0x100] ^= 0xFF
    b = s1_board(img, syms, bytes(bad))
    s = serdump.Session(sd.SimLink(b), log=sd.quiet, block=8192)
    s.wait_window(30)
    sd.check(" s=0" in s.hello() and s1_tries(b) == 0, "a stage 1 with a bad CRC is not run (s=0, no try)")
    s.boot()
    b.run_to_app()
    sd.check(boots(b, chip_app), "  the chip's boot loader boots")

    print("=== 15. two stages: a stage 1 that faults at once (resets carried over in NVRAM) ===")
    broken = bytearray(stage1)
    entry = struct.unpack_from(">I", broken, 12)[0]
    broken[entry:entry + 4] = struct.pack(">I", 0x7FE00008)                    # trap
    broken[8:12] = struct.pack(">I", zlib.crc32(bytes(broken[0x20:])))
    flash = bytearray(with_s1)
    flash[o:o + len(broken)] = broken
    nv = None
    for attempt in (1, 2, 3):
        b = s1_board(img, syms, bytes(flash))
        if nv is not None:
            b.uc.mem_write(NVRAM, nv)
        done = False
        for _ in range(60):                                   # until the app, or the watchdog wait after a fault
            if not b.step(500_000):
                done = True
                break
            if b.exceptions_taken and len(b.exceptions_taken) == 1 and b.insns > 2_000_000 + b.exceptions_taken_at:
                break
        rec = dbgrecord.decode(bytes(b.uc.mem_read(NVRAM + dbgrecord.OFFSET, dbgrecord.SIZE)))
        c = rec["crash"]
        if attempt < 3:
            sd.check(not done and c and c["seq"] == rec["seq"] and c["step"] == 0x04 and c["vector"] == 0x700,
                     f"try {attempt}: stage 1 faulted, recorded as a crash in 'stage 1 running' "
                     f"(tries now {s1_tries(b)})")
        else:
            sd.check(done and boots(b, chip_app) and s1_tries(b) == 2,
                     "try 3: stage 0 boots by itself, the tries used up")
        nv = bytes(b.uc.mem_read(NVRAM, 0x8000))

    print("=== 16. the boot-flash write is refused from stage 0 (it runs from the chip) ===")
    b = s1_board(img, syms, with_s1)
    s = serdump.Session(sd.SimLink(b), log=sd.quiet, block=8192)
    s.wait_window(30, serdump.RECOVERY_MAGIC)                 # stage 0's own window, not stage 1
    sd.check(" s=0" in s.hello(), "the recovery window is stage 0's (s=0)")
    img0 = open(image_path, "rb").read()
    n = s.stage_image(img0[:0x80000] + b"\xff" * (0x80000 - len(img0[:0x80000])))
    try:
        s.request(serdump.INSTALL_BOOT, struct.pack(">II", n, serdump.crc32(img0[:n])), timeout=10, retries=0)
        refused = False
    except serdump.DumpError as e:
        refused = "flash" in str(e)
    sd.check(refused, "INSTALL_BOOT from stage 0 is refused (would erase the code it runs from)")

    print("=== 17. the boot-flash write works from stage 1 (runs from RAM) ===")
    b = s1_board(img, syms, with_s1)
    s = serdump.Session(sd.SimLink(b), log=sd.quiet, block=8192)
    s.wait_window(30)
    sd.check(" s=1" in s.hello(), "stage 1 answers (runs from RAM, so it may write the chip)")
    bf = model_bootflash(b)
    newboot = bytearray(open(image_path, "rb").read())       # a different, valid boot-flash image: the stock dump
    newboot = newboot[:0x80000] + b"\xff" * (0x80000 - len(newboot[:0x80000]))
    s.install_bootflash(bytes(newboot))
    wrote = bytes(bf.data[:len(newboot.rstrip(b"\xff"))])
    sd.check(not bf.violations, "the boot flash saw only valid 8-bit command sequences" +
             (f" ({bf.violations[:3]})" if bf.violations else ""))
    sd.check(bf.erased == {i for i in range(0, 0x80000, SECTOR)}, f"all 8 sectors erased ({len(bf.erased)})")
    sd.check(wrote == bytes(newboot)[:len(wrote)], "the chip holds the new image (verified by the sign)")

    print("=== 14. two stages: UNINSTALL_S1 ===")
    b = s1_board(img, syms, with_s1)
    s = serdump.Session(sd.SimLink(b), log=sd.quiet, block=8192)
    s.wait_window(30)
    s.hello()
    s.uninstall_loader()
    sd.check(b.fm.erased == {LOADER}, "the loader sector was erased")
    s.boot()
    b.run_to_app()
    sd.check(boots(b, chip_app), "stage 1 boots on this time")
    after = bytes(b.fm.data)
    b = s1_board(img, syms, after)
    s = serdump.Session(sd.SimLink(b), log=sd.quiet, block=8192)
    s.wait_window(30)
    sd.check(" s=0" in s.hello(), "the next boot: the chip's boot loader alone (s=0)")


def main(img_path, chip_app_path, image_path, image_app_path, stage1_path=None):
    img = open(img_path, "rb").read()
    chip_app = open(chip_app_path, "rb").read()
    image = open(image_path, "rb").read()
    image_app = open(image_app_path, "rb").read()
    syms = sd.symbols(os.path.join(os.path.dirname(img_path), "bootstrap.elf"))
    blank = random.Random(5).randbytes(SIZE)

    print("=== 1. INSTALL ===")
    b = board(img, syms, blank, 0xFF)
    s = serdump.Session(sd.SimLink(b), log=sd.quiet, block=8192)
    s.wait_window(30)
    info = s.hello()
    sd.check("region=9 slot %08x 00080000" % SLOT in info, "HELLO lists the slot at %#x" % SLOT)
    n = s.stage_image(image)
    s.request(serdump.INSTALL, struct.pack(">II", n, serdump.crc32(image[:n])), timeout=600, retries=0)
    o = SLOT - MAIN
    sd.check(bytes(b.fm.data[o:o + n]) == image[:n], f"the slot holds the {n}-byte image")
    sd.check(bytes(b.uc.mem_read(NVRAM + 0x7FF0, 1))[0] == 0, "the NVRAM boot count is 0")
    sd.check(not b.fm.violations, "the flash saw only valid command sequences" + (f" ({b.fm.violations[:3]})" if b.fm.violations else ""))
    sd.check(b.fm.erased == {SLOT + i for i in range(0, 0x80000, SECTOR)} and b.fm.programmed <= b.fm.erased,
             f"only the slot's 8 sectors were erased and programmed ({len(b.fm.erased)} erased)")
    main_after = bytes(b.fm.data)

    print("=== 2. BOOT from the slot ===")
    s.boot()
    b.run_to_app()
    sd.check(boots(b, image_app), "the app in RAM is the slot's app")
    sd.check(not boots(b, chip_app), "  (not the chip's)")
    top = runsim.be32(b, 0x14)
    sd.check(runsim.be32(b, 0x2C) == top and runsim.be32(b, 0x30) == 0x80000,
             f"hand-off: boot volume = the slot's copy in the stage, the end of RAM ({runsim.be32(b, 0x2C):#x}, top {top:#x})")
    sd.check(runsim.be32(b, 0x48) == 0x400000 - 0x80000 - 0x10000,
             f"hand-off: bank 2 shown as {runsim.be32(b, 0x48):#x} (slot and loader sector left out)")
    sd.check(bytes(b.uc.mem_read(NVRAM + 0x7FF0, 2)) == bytes([1, 2]), "the boot counted a try (count 1, slot flag 2, not the original's bit 0)")
    rec = dbgrecord.decode(bytes(b.uc.mem_read(NVRAM + dbgrecord.OFFSET, dbgrecord.SIZE)))
    last = rec and rec["boots"][-1]
    sd.check(rec is not None and last["step"] == 0x23 and last["value"] == top,
             f"debug record: the boot got to the hand-off with the stage as boot volume ({last})")

    print("=== 3. a slot image that fails its check ===")
    bad = bytearray(main_after)
    bad[o + 0x3000] ^= 0xFF                                # inside the app entry
    b = board(img, syms, bytes(bad), 0)
    b.run_to_app()
    sd.check(boots(b, chip_app), "the chip's app boots")
    sd.check(runsim.be32(b, 0x2C) != SLOT, "  with the boot flash as its boot volume")

    print("=== 4. UNINSTALL ===")
    b = board(img, syms, main_after, 0)
    s = serdump.Session(sd.SimLink(b), log=sd.quiet, block=8192)
    s.wait_window(30)
    s.hello()
    s.request(serdump.UNINSTALL, b"", timeout=60, retries=0)
    sd.check(b.fm.erased == {SLOT}, "only the slot's first sector was erased")
    s.boot()
    b.run_to_app()
    sd.check(boots(b, chip_app), "then the chip's app boots")

    print("=== 5. boot count 0xFF ===")
    b = board(img, syms, main_after, 0xFF)
    b.run_to_app()
    sd.check(boots(b, chip_app), "the chip's app, although the slot is good")
    print("=== 6. three tries used up ===")
    b = board(img, syms, main_after, 3)
    b.run_to_app()
    sd.check(boots(b, chip_app), "the chip's app boots")
    sd.check(bytes(b.uc.mem_read(NVRAM + 0x7FF0, 2)) == bytes([0xFF, 0]), "the count parks at 0xFF, flags clear")
    print("=== 7. debugging: the slot in place (OPTION slot mode direct) ===")
    b = board(img, syms, main_after, 0)
    s = serdump.Session(sd.SimLink(b), log=sd.quiet, block=8192)
    s.wait_window(30)
    s.hello()
    opts, tries, flags = s.option(serdump.OPT_SET_OPTIONS, dbgrecord.OPT_SLOT_DIRECT)
    sd.check(opts == 1 and tries == 0, f"OPTION sets the slot mode (options {opts:#x}, tries {tries:#x})")
    s.boot()
    b.run_to_app()
    sd.check(boots(b, image_app), "the slot's app boots")
    sd.check(runsim.be32(b, 0x2C) == SLOT and runsim.be32(b, 0x30) == 0x80000,
             f"hand-off: boot volume = the slot in the main flash ({runsim.be32(b, 0x2C):#x})")
    sd.check(bytes(b.uc.mem_read(NVRAM + 0x7FF0, 2)) == bytes([1, 3]), "count 1, flags 3 (ours and the original's)")

    print("=== 8. debugging: OPTION tries, and a fault in the boot loader ===")
    b = board(img, syms, main_after, 2)
    s = serdump.Session(sd.SimLink(b), log=sd.quiet, block=8192)
    s.wait_window(30)
    info = s.hello()
    sd.check("dbg=00000001 o=00 c=0000" in info and info.rstrip().endswith("slot 50b80000 00080000"), "HELLO shows the debug record: boot 1, no options, no crash")
    _, tries, _ = s.option(serdump.OPT_SET_TRIES, 0)
    sd.check(tries == 0 and bytes(b.uc.mem_read(NVRAM + 0x7FF0, 1))[0] == 0, "OPTION sets the try counter")
    s.option(serdump.OPT_TEST_FAULT)
    for _ in range(5):
        b.step(2_000_000)
    rec = dbgrecord.decode(bytes(b.uc.mem_read(NVRAM + dbgrecord.OFFSET, dbgrecord.SIZE)))
    c = rec and rec["crash"]
    sd.check(b.exceptions_taken and b.exceptions_taken[0][2] & 0x40 == 0,
             "the trap went to the vectors at 0 (MSR[IP] clear in the window)")
    sd.check(c is not None and c["source"] == 1 and c["vector"] == 0x700 and c["seq"] == 1 and c["step"] == 0x02,
             f"the crash record: boot loader, vector 0x700, boot 1, during the window ({c and {k: hex(v) for k, v in c.items() if k != 'gpr'}})")
    sd.check(not b.reached_app, "the board waits for the watchdog instead of booting on")
    print(dbgrecord.report(rec))
    if stage1_path:
        two_stages(img, syms, chip_app, main_after, image_app, open(stage1_path, "rb").read(), image_path)
    print("RESULT:", "PASS" if not sd.FAILS else f"FAIL ({len(sd.FAILS)})")
    return 0 if not sd.FAILS else 1


if __name__ == "__main__":
    if len(sys.argv) not in (5, 6):
        raise SystemExit(__doc__)
    sys.exit(main(*sys.argv[1:]))
