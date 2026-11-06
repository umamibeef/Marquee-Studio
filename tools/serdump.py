#!/usr/bin/env python3
"""Read the sign's boot flash, main flash, NVRAM and config EEPROMs over the DB9 serial port.

Needs the DUMP=1 boot loader (bootloader/src/serdump.c, `make DUMP=1 image`) in the sign's
boot flash socket. The protocol is described in bootloader/include/serdump.h. Read-only:
the boot loader has no write path, and this tool never sends the app's DB9 download frames
(it never sends a 0x5A byte at all, so the app's download parser cannot even sync on us:
the magic has none, and request payloads are XORed with a tag chosen to keep it out).

  1. Start the tool, then power the sign on.
  2. The tool sends the magic until the boot loader answers (its window is open about 2 s,
     starting about half a second after power-on). Then it prints the board info.
  3. It dumps the chosen regions to OUT/<region>.bin with resume (partial files are kept as
     .part and checked against the sign before they are continued) and per-block retries,
     re-reads every region in 1 MB pieces to compare CRC-32s, and writes OUT/manifest.txt.
  4. It tells the boot loader to boot (unless --stay), and the sign starts as usual.

Usage:
  serdump.py [--port /dev/cu.PL2303G-...] [--out DIR] [--regions LIST] [--speed BAUD]
             [--block BYTES] [--wait SECONDS] [--stay] [--info]
  LIST: comma-separated names from bootflash, flash1, flash2, nvram, eeprom_a0, eeprom_a2,
        sdram (speculative, not in the default list), or "all".
"""
import argparse, glob, hashlib, os, struct, sys, time, zlib

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import dbgrecord

HERE = os.path.dirname(os.path.abspath(__file__))

# ---- protocol (bootloader/include/serdump.h; test/serdumpsim.py checks these match) ----
MAGIC = b"MQ?DUMP\r"
RECOVERY_MAGIC = b"MQ?DUMP0"           # stage 0's own window (two stages, bootloader/include/serdump.h)
BAUD = 115200
LISTEN_MS, IDLE_MS, FALLBACK_MS = 2000, 30000, 3000
BLOCK_MAX = 0x4000
SYNC = b"MQ"
HDR = 6
HELLO, READ, CRC, SPEED, PING, BOOT, WRITE, RUN, INSTALL, UNINSTALL, ERROR = 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08, 0x09, 0x0A, 0xFF
OPTION = 0x0B
OPT_SET_OPTIONS, OPT_SET_TRIES, OPT_CLEAR_CRASH, OPT_TEST_FAULT, OPT_SET_FLAGS, OPT_SET_S1_TRIES = 1, 2, 3, 4, 5, 6
INSTALL_S1, UNINSTALL_S1, INSTALL_BOOT = 0x0C, 0x0D, 0x0E
S1_MAGIC, LOADER_SIZE = b"MQS1", 0x10000
WRITE_MAX = 0x400
STAGE_SIZE = 0x80000
ERRORS = {1: "unknown request", 2: "bad length", 3: "no such region on this board",
          4: "out of range", 5: "EEPROM did not answer", 6: "speed not reachable",
          7: "staged image failed its check (CRC, or no bootable entry)", 8: "writes go to the stage region only",
          9: "the main flash did not erase, program or verify"}
REGIONS = {"bootflash": 1, "flash1": 2, "flash2": 3, "nvram": 4, "eeprom_a0": 5,
           "eeprom_a2": 6, "sdram": 7, "stage": 8, "slot": 9}
DEFAULT_REGIONS = ["eeprom_a0", "eeprom_a2", "nvram", "bootflash", "flash1", "flash2"]
HEARTBEAT = bytes.fromhex("A5BB0000583D")       # the app's DB9 status frame: the app is up
FORBIDDEN = 0x5A                                # sync byte of the app's download frames
VERIFY_CHUNK = 1 << 20


FLASH_STEPS = {1: "no main flash or image too big", 2: "erase", 3: "program", 4: "blank check",
               5: "verify (CRC over the slot)"}


class DumpError(Exception):
    pass


class Closed(DumpError):
    """The window closed (the boot loader went on booting)."""


def crc32(b, c=0):
    return zlib.crc32(b, c) & 0xFFFFFFFF


def frame(ftype, tag, payload=b"", mask=True):
    """A frame; request payloads (mask=True) are XORed with the tag, replies are not."""
    if mask:
        payload = bytes(c ^ tag for c in payload)
    body = bytes([ftype, tag]) + struct.pack(">H", len(payload)) + payload
    return SYNC + body + struct.pack(">I", crc32(body))


def safe_frame(ftype, tag, payload=b""):
    """Build a request with no 0x5A byte anywhere, by picking the tag. Returns (tag, bytes).
    At most len(payload) + 1 tags are ruled out by the payload; the CRC rules out a few more."""
    for i in range(256):
        t = (tag + i) & 0xFF
        f = frame(ftype, t, payload)
        if FORBIDDEN not in f:
            return t, f
    raise DumpError("no tag keeps 0x5A out of the request")


class SerialLink:
    """pyserial port, 8N1, no flow control. All timing goes through now()/sleep()."""

    def __init__(self, port, baud=BAUD):
        import serial
        self.s = serial.Serial(port, baud, bytesize=8, parity="N", stopbits=1, timeout=0,
                               xonxoff=False, rtscts=False, dsrdtr=False)
        self.name = port

    def write(self, b):
        self.s.write(b)
        self.s.flush()

    def read(self, timeout):
        self.s.timeout = timeout
        b = self.s.read(1)
        if b and self.s.in_waiting:
            self.s.timeout = 0
            b += self.s.read(self.s.in_waiting)
        return b

    def set_baud(self, baud):
        self.s.baudrate = baud

    def flush_input(self):
        self.s.reset_input_buffer()

    def now(self):
        return time.monotonic()

    def sleep(self, t):
        time.sleep(t)


class Session:
    def __init__(self, link, log=print, block=8192):
        self.link = link
        self.log = log
        self.block = min(block, BLOCK_MAX)
        self.baud = BAUD
        self.buf = b""
        self.recent = b""                  # last bytes received, to spot the window closing
        self.tag = 0
        self.info = ""
        self.regions = {}                  # name -> (id, base, size)
        self.stats = dict(sent=0, received=0, retries=0, crc_errors=0, timeouts=0)

    # ---- low level ----
    def _send(self, b):
        assert FORBIDDEN not in b
        self.stats["sent"] += len(b)
        self.link.write(b)

    def _fill(self, timeout):
        b = self.link.read(timeout)
        self.stats["received"] += len(b)
        self.buf += b
        self.recent = (self.recent + b)[-64:]
        return bool(b)

    def _frame(self, deadline):
        """Next frame with a good CRC as (type, tag, payload), or None at the deadline."""
        while True:
            i = self.buf.find(SYNC)
            if i < 0:
                self.buf = self.buf[-1:]
            else:
                self.buf = self.buf[i:]
                if len(self.buf) >= HDR:
                    n = struct.unpack_from(">H", self.buf, 4)[0]
                    if n > BLOCK_MAX + 16:
                        self.buf = self.buf[2:]
                        continue
                    if len(self.buf) >= HDR + n + 4:
                        body = self.buf[2:HDR + n]
                        want = struct.unpack_from(">I", self.buf, HDR + n)[0]
                        if crc32(body) == want:
                            self.buf = self.buf[HDR + n + 4:]
                            return body[0], body[1], body[4:]
                        self.stats["crc_errors"] += 1
                        self.buf = self.buf[2:]
                        continue
            left = deadline - self.link.now()
            if left <= 0:
                return None
            self._fill(min(left, 0.05))

    def request(self, ftype, payload=b"", timeout=1.0, retries=6):
        """Send a request until its reply arrives. Returns the reply payload."""
        for attempt in range(retries + 1):
            self.tag = (self.tag + 1) & 0xFF
            tag, f = safe_frame(ftype, self.tag, payload)
            self.tag = tag
            self._send(f)
            deadline = self.link.now() + timeout
            while True:
                r = self._frame(deadline)
                if r is None:
                    if HEARTBEAT in self.recent or b"MQDUMP CLOSED" in self.recent:
                        raise Closed("the window closed and the sign went on booting")
                    break
                rtype, rtag, rpl = r
                if rtag != tag:
                    continue                           # a late reply to an earlier try
                if rtype == ERROR:
                    code = rpl[0] if rpl else 0
                    detail = ""
                    if code == 9 and len(rpl) >= 12:
                        step = FLASH_STEPS.get(rpl[2], rpl[2])
                        a, got, want = struct.unpack(">IHH", rpl[4:12])
                        detail = f": {step} at {a:#010x}, read {got:#06x}, expected {want:#06x}"
                    raise DumpError(f"sign says: {ERRORS.get(code, code)}{detail} (request {ftype:#x})")
                if rtype == ftype | 0x80:
                    return rpl
            self.stats["timeouts"] += 1
            if attempt < retries:
                self.stats["retries"] += 1
                if self.baud != BAUD and attempt >= 2:
                    self._fall_back()
        raise DumpError(f"no reply to request {ftype:#x} after {retries + 1} tries")

    def _fall_back(self):
        """Lost the link at a higher speed: let the sign drop back to 115200, then follow."""
        self.log(f"  no replies at {self.baud} baud: going back to {BAUD}")
        self.link.sleep(FALLBACK_MS / 1000 + 0.5)       # we are quiet, so the sign falls back
        self.link.set_baud(BAUD)
        self.baud = BAUD
        self.link.flush_input()
        self.buf = b""

    # ---- steps ----
    def wait_window(self, wait_s=180.0, magic=None):
        """Send the magic until the boot loader answers. Returns its READY line. magic=RECOVERY_MAGIC: the chip's
        own boot loader (stage 0), skipping an installed stage 1 for this boot."""
        magics = [magic, MAGIC] if magic else [MAGIC]  # recovery: both (no stage 1 installed: no recovery listen)
        sent = 0
        t0 = last_note = self.link.now()
        warned = False
        self.link.set_baud(BAUD)
        self.link.flush_input()
        self.log("waiting for the sign: power it on now")
        while self.link.now() - t0 < wait_s:
            self._send(magics[sent % len(magics)])
            sent += 1
            b = self.link.read(0.02)
            self.stats["received"] += len(b)
            self.buf = (self.buf + b)[-256:]
            i = self.buf.find(b"MQDUMP READY")
            if i >= 0:
                j = self.buf.find(b"\n", i)
                if j < 0:
                    self.buf += self.link.read(0.1)
                    j = self.buf.find(b"\n", i)
                line = self.buf[i:j if j >= 0 else None].decode(errors="replace").strip()
                self.link.sleep(0.05)                  # let our last magic bytes arrive
                self.link.flush_input()
                self.buf = b""
                return line
            if HEARTBEAT in self.buf and not warned:
                self.log("  the app is running (its 20 s status frame came in): the window was missed,\n"
                         "  or this boot flash has no dump window. Power-cycle the sign to try again.")
                warned = True
                self.buf = b""
            if self.link.now() - last_note > 10:
                self.log(f"  still waiting ({self.link.now() - t0:.0f} s)")
                last_note = self.link.now()
        raise DumpError(f"no answer from the boot loader within {wait_s:.0f} s")

    def hello(self):
        self.info = self.request(HELLO, timeout=1.5).decode(errors="replace")
        self.regions = {}
        for line in self.info.splitlines():
            if line.startswith("region="):
                rid, name, base, size = line[7:].split()
                self.regions[name] = (int(rid), int(base, 16), int(size, 16))
        return self.info

    def ping(self, timeout=0.3, retries=0):
        self.request(PING, timeout=timeout, retries=retries)

    def set_speed(self, want):
        rep = self.request(SPEED, struct.pack(">I", want))
        actual = struct.unpack_from(">I", rep, 4)[0]
        t_reply = self.link.now()
        self.link.sleep(0.02)
        self.link.set_baud(actual)                     # match the sign's real rate
        self.baud = actual
        self.link.flush_input()
        self.buf = b""
        for _ in range(5):
            try:
                self.ping(timeout=0.3)
                self.log(f"speed: {actual} baud (asked for {want})")
                return actual
            except DumpError:
                t_reply = self.link.now()
        self.log(f"speed: {actual} baud does not work here, staying at {BAUD}")
        self.link.sleep(max(0.0, t_reply + FALLBACK_MS / 1000 + 0.5 - self.link.now()))
        self.link.set_baud(BAUD)
        self.baud = BAUD
        self.link.flush_input()
        self.buf = b""
        self.ping(timeout=0.5, retries=6)
        return BAUD

    def _req12(self, rid, off, length):
        return struct.pack(">B3xII", rid, off, length)

    def read_block(self, rid, off, length):
        timeout = 1.0 + (length + 64) * 10 / self.baud * 1.5
        if rid in (5, 6):
            timeout += 1.0                             # live I2C read: 200 ms per try
        for _ in range(4):
            rep = self.request(READ, self._req12(rid, off, length), timeout=timeout)
            if rep[:12] == self._req12(rid, off, length) and len(rep) == 12 + length:
                return rep[12:]
            self.stats["retries"] += 1
        raise DumpError(f"bad READ reply for region {rid} at {off:#x}")

    def device_crc(self, rid, off, length):
        timeout = 3.0 + length / 100e3                 # the sign does 1 MB in well under 10 s
        rep = self.request(CRC, self._req12(rid, off, length), timeout=timeout)
        if rep[:12] != self._req12(rid, off, length):
            raise DumpError("bad CRC reply")
        return struct.unpack_from(">I", rep, 12)[0]

    def verified_prefix(self, rid, data):
        """Length of the longest prefix of data (in VERIFY_CHUNK steps) that matches the sign."""
        good = 0
        while good < len(data):
            n = min(VERIFY_CHUNK, len(data) - good)
            if self.device_crc(rid, good, n) != crc32(data[good:good + n]):
                break
            good += n
        return good

    def dump_region(self, name, outdir):
        rid, base, size = self.regions[name]
        os.makedirs(outdir, exist_ok=True)
        final = os.path.join(outdir, name + ".bin")
        part = final + ".part"
        if size == 0:
            self.log(f"{name}: not present on this board")
            return None
        if os.path.exists(final) and os.path.getsize(final) == size:
            data = open(final, "rb").read()
            if self.verified_prefix(rid, data) == size:
                self.log(f"{name}: already complete and verified ({final})")
                return data
            self.log(f"{name}: existing file differs from the sign, reading it again")
            os.replace(final, part)
        data = b""
        if os.path.exists(part):
            data = open(part, "rb").read()[:size]
            keep = self.verified_prefix(rid, data)
            if keep:
                self.log(f"{name}: resuming at {keep:#x} (checked against the sign)")
            data = data[:keep]
        with open(part, "wb") as f:
            f.write(data)
            t0, start, last = self.link.now(), len(data), 0
            off = len(data)
            while off < size:
                n = min(self.block, size - off)
                blk = self.read_block(rid, off, n)
                f.write(blk)
                off += n
                if off - last >= max(size // 50, 64 << 10) or off == size:
                    f.flush()
                    last = off
                    el = max(self.link.now() - t0, 1e-6)
                    rate = (off - start) / el
                    eta = (size - off) / rate if rate else 0
                    self.log(f"  {name}: {off:#x}/{size:#x} {100 * off / size:5.1f}%  "
                             f"{rate / 1024:6.1f} KB/s  eta {eta:5.0f} s")
        data = open(part, "rb").read()
        if self.verified_prefix(rid, data) != size:
            raise DumpError(f"{name}: verification against the sign failed (kept {part})")
        os.replace(part, final)
        self.log(f"{name}: {size} bytes, CRC-32 {crc32(data):08x}, verified by a second read")
        return data

    def stage_image(self, image, progress=None, max_chunk=WRITE_MAX):
        """Write a boot-flash image into the sign's staging RAM (trailing erased 0xFF bytes are left out), each
        block checked by the CRC the sign reports for what it stored. Returns the staged length."""
        n = len(image.rstrip(b"\xff"))
        n = min((n + 3) & ~3, len(image))
        if n > STAGE_SIZE:
            raise DumpError(f"image is {n} bytes; the stage holds {STAGE_SIZE}")
        rid = REGIONS["stage"]
        off = 0
        while off < n:
            size = min(max_chunk, n - off)              # the largest chunk some tag keeps 0x5A out of
            while True:
                chunk = image[off:off + size]
                try:
                    safe_frame(WRITE, self.tag, struct.pack(">B3xI", rid, off) + chunk)
                    break
                except DumpError:
                    if size <= 32:
                        raise
                    size //= 2
            timeout = 4.0 + (len(chunk) + 64) * 10 / self.baud * 1.5   # the real board answers a 1 KB WRITE slowly
            for _ in range(4):
                rep = self.request(WRITE, struct.pack(">B3xI", rid, off) + chunk, timeout=timeout)
                if rep == struct.pack(">B3xIII", rid, off, len(chunk), crc32(chunk)):
                    break
                self.stats["retries"] += 1
            else:
                raise DumpError(f"WRITE at {off:#x} not stored correctly")
            off += len(chunk)
            if progress:
                progress(off, n)
        return n

    def run_staged(self, image, n):
        """RUN: the sign checks the CRC of the staged bytes and boots from them."""
        try:
            self.request(RUN, struct.pack(">II", n, crc32(image[:n])), timeout=3.0, retries=3)
        except Closed:
            pass

    def install_staged(self, image, n):
        """INSTALL: the sign writes the staged image into the update slot (main flash) and checks it there. It
        boots from the slot from the next start on, falling back to the boot flash if that app fails."""
        self.request(INSTALL, struct.pack(">II", n, crc32(image[:n])), timeout=180.0, retries=0)

    def uninstall(self):
        """UNINSTALL: the slot's first sector is erased; the boot flash's app boots from then on."""
        self.request(UNINSTALL, b"", timeout=60.0, retries=0)

    def install_loader(self, image):
        """INSTALL_S1: a stage-1 boot loader (make DUMP=1 STAGE1=1 stage1) into the main flash's loader sector; the
        chip's boot loader runs it from the next start on, and goes back to its own after 2 failed tries."""
        if image[:4] != S1_MAGIC or len(image) > LOADER_SIZE:
            raise DumpError("not a stage-1 image (stage1.img from make DUMP=1 STAGE1=1 stage1)")
        n = self.stage_image(image)
        self.request(INSTALL_S1, struct.pack(">II", n, crc32(image[:n])), timeout=30.0, retries=0)

    def uninstall_loader(self):
        """UNINSTALL_S1: erase the loader sector; the chip's boot loader does everything again."""
        self.request(UNINSTALL_S1, b"", timeout=30.0, retries=0)

    def install_bootflash(self, image):
        """INSTALL_BOOT: write a whole boot-flash image to the chip. The sign refuses unless it is running the boot
        loader from RAM (stage 1, or a RUN image), never in place from the chip it would erase. Interrupting the
        write (power loss) leaves the chip needing the programmer, so this is the one place that still can."""
        n = self.stage_image(image)
        self.request(INSTALL_BOOT, struct.pack(">II", n, crc32(image[:n])), timeout=120.0, retries=0)

    def option(self, op, value=0):
        """OPTION: a debugging switch (bootloader/include/serdump.h OPT_*). Returns (options, tries, flags)."""
        rep = self.request(OPTION, bytes([op, value & 0xFF]), timeout=2.0, retries=1 if op != OPT_TEST_FAULT else 0)
        return rep[0], rep[1], rep[2]

    def debug_record(self):
        """The NVRAM debug record (tools/dbgrecord.py), decoded."""
        rid = self.regions["nvram"][0]
        return dbgrecord.decode(self.read_block(rid, dbgrecord.OFFSET, dbgrecord.SIZE))

    def boot(self):
        try:
            self.request(BOOT, timeout=1.0, retries=3)
        except Closed:
            pass


def find_port():
    ports = sorted(glob.glob("/dev/cu.PL2303*") + glob.glob("/dev/cu.usbserial*"))
    if not ports:
        sys.exit("no USB-serial adapter found under /dev/cu.*; give --port")
    return ports[0]


def eta_table(sizes):
    total = sum(sizes)
    return ", ".join(f"{b}: {total * 10 / b / 60:.0f} min" for b in (115200, 230400, 460800))


def main(argv=None, link=None):
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--port")
    ap.add_argument("--out", default=os.path.join(HERE, "..", "dumps", "serial"))
    ap.add_argument("--regions", default=",".join(DEFAULT_REGIONS))
    ap.add_argument("--speed", type=int, default=0, help="try this speed after the hello (e.g. 460800)")
    ap.add_argument("--block", type=int, default=8192)
    ap.add_argument("--wait", type=float, default=180.0, help="seconds to wait for the sign")
    ap.add_argument("--stay", action="store_true", help="do not send BOOT at the end")
    ap.add_argument("--info", action="store_true", help="only print the board info")
    ap.add_argument("--run", metavar="IMAGE", help="run this boot-flash image from RAM (nothing is written to flash)")
    ap.add_argument("--install", metavar="IMAGE",
                    help="install this boot-flash image permanently (the update slot in the main flash)")
    ap.add_argument("--uninstall", action="store_true", help="remove the installed image: the boot flash boots")
    ap.add_argument("--install-bootflash", metavar="IMAGE",
                    help="write a whole boot-flash image to the chip over serial (only works when a stage 1 or a "
                         "RUN image is running; power loss mid-write needs the programmer to recover)")
    ap.add_argument("--install-loader", metavar="STAGE1_IMG",
                    help="install a stage-1 boot loader (bootloader/build-dump-s1/stage1.img) in the main flash")
    ap.add_argument("--uninstall-loader", action="store_true",
                    help="remove the stage-1 boot loader: the chip's own boot loader does everything again")
    ap.add_argument("--yes", action="store_true", help="skip the stage-1 check for --install-bootflash")
    ap.add_argument("--recovery", action="store_true",
                    help="open the chip's own boot loader window (stage 0), skipping stage 1 for this boot")
    ap.add_argument("--s1-tries", type=lambda v: int(v, 0), metavar="N",
                    help="set the stage-1 try counter (0: use stage 1 again after it failed twice)")
    ap.add_argument("--boots", action="store_true",
                    help="show the debug record: the last boots' reset causes and steps, and the last crash")
    ap.add_argument("--slot-mode", choices=("stage", "direct"),
                    help="how the update slot boots: through the RAM stage (default) or in place (debugging)")
    ap.add_argument("--tries", type=lambda v: int(v, 0), metavar="N",
                    help="set the slot's try counter (0: try the slot again, 0xFF: never)")
    ap.add_argument("--clear-crash", action="store_true", help="forget the recorded crash")
    ap.add_argument("--test-fault", action="store_true",
                    help="make the boot loader fault on purpose, to check that the crash record works")
    ap.add_argument("--reset", action="store_true",
                    help="first ask our firmware's console to restart the sign (no power cycle; NVRAM untouched)")
    a = ap.parse_args(argv)

    names = list(REGIONS) if a.regions == "all" else [r.strip() for r in a.regions.split(",") if r.strip()]
    for n in names:
        if n not in REGIONS:
            sys.exit(f"unknown region {n}; choose from {', '.join(REGIONS)}")
    if link is None:
        link = SerialLink(a.port or find_port())
    s = Session(link, block=a.block)
    os.makedirs(a.out, exist_ok=True)

    if a.reset:
        link.write(b"MQ?MON\r")
        seen = b""
        t = time.time() + 3
        while time.time() < t and b"mq> " not in seen:
            seen += link.read(0.1)
        if b"mq> " in seen:
            link.write(b"reset\r")
            print("asked the sign's console to restart it")
        else:
            print("no answer from the sign's console: power the sign on")
    print(s.wait_window(a.wait, RECOVERY_MAGIC if a.recovery else None))
    info = s.hello()
    print(info.rstrip())
    with open(os.path.join(a.out, "info.txt"), "w") as f:
        f.write(info)
    if a.install_bootflash:
        info = s.hello()
        if " s=1" not in info and not a.yes:
            sys.exit("the sign is running its own boot loader (stage 0), which cannot write the chip it runs from.\n"
                     "boot a stage 1 (or a RUN image) first, or pass --yes to try anyway (it will be refused).")
        image = open(a.install_bootflash, "rb").read()
        print("Do not power the sign off until this finishes.", flush=True)
        s.install_bootflash(image)
        print(f"WROTE the boot flash from {a.install_bootflash}: the chip now holds it")
    if a.install_loader:
        s.install_loader(open(a.install_loader, "rb").read())
        print(f"INSTALLED stage 1 {a.install_loader}: the sign's boot loader runs it from the next start on")
    if a.uninstall_loader:
        s.uninstall_loader()
        print("UNINSTALLED stage 1: the chip's boot loader does everything from the next start on")
    if a.s1_tries is not None:
        s.option(OPT_SET_S1_TRIES, a.s1_tries)
        print(f"stage-1 try counter set to {a.s1_tries}")
    debugging = (a.boots or a.slot_mode or a.tries is not None or a.clear_crash or a.test_fault or a.install_loader
                 or a.uninstall_loader or a.s1_tries is not None or a.recovery or a.install_bootflash)
    if a.slot_mode:
        opts = s.debug_record()
        cur = opts["options"] if opts else 0
        new = (cur | dbgrecord.OPT_SLOT_DIRECT) if a.slot_mode == "direct" else (cur & ~dbgrecord.OPT_SLOT_DIRECT)
        print("options %02x, tries %02x, flags %02x" % s.option(OPT_SET_OPTIONS, new))
    if a.tries is not None:
        print("options %02x, tries %02x, flags %02x" % s.option(OPT_SET_TRIES, a.tries))
    if a.clear_crash:
        s.option(OPT_CLEAR_CRASH)
        print("crash record cleared")
    if a.boots:
        print(dbgrecord.report(s.debug_record()))
    if a.test_fault:
        print("asking the boot loader to fault; it resets by itself in about 3 s (then use --boots)")
        try:
            s.option(OPT_TEST_FAULT)
        except DumpError:
            pass
        return 0
    if debugging and not (a.install or a.uninstall or a.run):
        if not a.stay:
            s.boot()
        return 0
    if a.install:
        image = open(a.install, "rb").read()
        n = s.stage_image(image, progress=lambda done, total: print(f"\r  staged {done}/{total}", end="", flush=True))
        print("\n  writing the update slot (erase, program, verify; about a minute)...", flush=True)
        s.install_staged(image, n)
        print(f"INSTALLED {a.install}: the sign boots it from now on (falls back to the boot flash if it fails)")
        s.boot()
        return 0
    if a.uninstall:
        s.uninstall()
        print("UNINSTALLED: the boot flash boots from now on")
        s.boot()
        return 0
    if a.run:
        image = open(a.run, "rb").read()
        if a.speed and a.speed != BAUD:
            s.set_speed(a.speed)
        n = s.stage_image(image, progress=lambda done, total: print(f"\r  staged {done}/{total}", end="", flush=True))
        print()
        s.run_staged(image, n)
        print(f"RUN: the sign is booting {a.run} from RAM; a power cycle brings back the chip's firmware")
        return 0
    if a.info:
        if not a.stay:
            s.boot()
        return 0
    if a.speed and a.speed != BAUD:
        s.set_speed(a.speed)
    sizes = [s.regions[n][2] for n in names if n in s.regions]
    print(f"{sum(sizes)} bytes to read; on the wire at least {eta_table(sizes)}")
    if "sdram" in names:
        print("note: sdram is speculative: after a warm reset it may hold what the app left, "
              "after power-off it is noise")
    done = {}
    for n in names:
        if n not in s.regions:
            print(f"{n}: the sign does not list this region")
            continue
        data = s.dump_region(n, a.out)
        if data is not None:
            done[n] = data
    with open(os.path.join(a.out, "manifest.txt"), "a") as f:
        f.write(f"# {time.strftime('%Y-%m-%d %H:%M:%S')}  {info.splitlines()[2] if info.count(chr(10)) > 2 else ''}\n")
        for n, d in done.items():
            rid, base, size = s.regions[n]
            f.write(f"{n}.bin  base {base:08x}  size {size:08x}  crc32 {crc32(d):08x}  "
                    f"sha256 {hashlib.sha256(d).hexdigest()}\n")
    if not a.stay:
        s.boot()
        print("sent BOOT: the sign is starting normally")
    print(f"done: {s.stats}")
    return 0


if __name__ == "__main__":
    try:
        sys.exit(main())
    except DumpError as e:
        sys.exit(f"error: {e}")
