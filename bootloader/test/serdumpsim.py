#!/usr/bin/env python3
"""Run the DUMP=1 bootloader on the emulated board (test/cosim.py, test/netsim.py) with an
emulated DB9 UART, and drive it with the real host tool (tools/serdump.py) over a simulated
serial link.

The UART model works at buffer-descriptor level, like the FEC model in netsim.py: it reads the
channel's parameter RAM (RBASE, TBASE, MRBLR), its mode registers, pins, clock routing and BRG,
moves received characters into empty RX BDs and takes ready TX BDs. It only passes data when
the channel the board wires to the DB9 is a working 8N1 UART at the host's speed (within 2.5 %);
otherwise the host sees garbage, as on a real line. SMC1 is the DB9 on the default board, SCC1
on boards with the panel lines off Port D (config byte 4 == 2, or 1 with byte 5 >= 0x50).
The wire itself is not timed: bytes move at the end of each emulation slice, so the dump
times in the summary come from the byte counts, not from the emulation.

Checks:
  1. full sessions on three boards (fixed memory/SMC1, SPD memory/SMC1, fixed memory/SCC1):
     serdump.py dumps every region, and each file equals the emulated contents (boot flash =
     the emulated image, whose archive part equals dumps/bootflash.bin); then BOOT boots the app;
  2. read-only: no store to boot flash, main flash, CS7 at any time; none to NVRAM or SDRAM
     while the window runs; every I2C transaction is a read; the CPM only touches DPRAM;
  3. robustness: corrupted and dropped bytes, bad requests, a bad CRC, speed change (and a
     host that cannot follow), resume of an interrupted dump, idle timeout;
  4. no magic (and line noise without the magic): normal boot, window closes after 2 s of its
     own clock, everything outside the window identical to the default build (register writes,
     SPR writes, SDRAM, NVRAM, boot info), all registers put back;
  5. NET=1 DUMP=1: the serial window runs first, then the network window; both ways boot.

Usage: serdumpsim.py DUMP_IMAGE DEFAULT_IMAGE NET_DUMP_IMAGE ORIGINAL_DUMP ELITE_RGB
"""
import collections, hashlib, os, random, re, struct, subprocess, sys, tempfile, time
HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
sys.path.insert(0, os.path.join(HERE, "..", "..", "tools"))
import cosim, netsim, serdump
from cosim import be32, FLASH, IMMR, NVRAM, MAIN, CS7, RAM_SIZE
from unicorn import UC_HOOK_CODE, UC_HOOK_MEM_WRITE, UcError, UC_ERR_EXCEPTION
from unicorn.ppc_const import UC_PPC_REG_LR, UC_PPC_REG_PC, UC_PPC_REG_MSR

NM = "/opt/homebrew/opt/llvm/bin/llvm-nm"
TICK = 4                      # TCN2 advance per read: 20 us at 200 ticks/ms (50 MHz / 250)
SLICE = 200_000               # instructions per emulation slice
BUDGET = 6_000_000_000        # instructions per board, as a hang guard
SMC1, SCC1 = "SMC1", "SCC1"

FAILS = []


def check(ok, what):
    print(("ok   " if ok else "FAIL ") + what)
    if not ok:
        FAILS.append(what)
    return ok


def cfg_variant(spd, byte4=None, byte5=None):
    c = bytearray(cosim.eeprom_cfg(spd))
    if byte4 is not None:
        c[4] = byte4
    if byte5 is not None:
        c[5] = byte5
    c[0x3E] = sum(c[:62]) & 0xFF
    x = 0
    for v in c[:62]:
        x ^= v
    c[0x3F] = x
    return bytes(c)


class Uart:
    """The DB9 UART at BD level (see the module docstring)."""

    def __init__(self, b, wired):
        self.b, self.wired = b, wired
        self.from_host = collections.deque()       # (byte, host baud)
        self.to_host = bytearray()
        self.host_baud = serdump.BAUD
        self.rx_idx = self.tx_idx = 0
        self.rx_count = self.tx_count = 0
        self.corrupt = None                        # f(tx_count) -> True: flip that byte
        self.drop = None                           # f(rx_count) -> True: lose that byte
        self.garbled = 0
        self.other_enabled = False
        self.violations = []
        self.bauds = set()

    def r(self, off, n):
        return int.from_bytes(self.b.dpram[off:off + n], "big")

    def brgclk(self):
        osc = be32(self.b.eeproms[0xA0], 0x0C)
        mf = (self.r(0x284, 4) >> 20) & 0xFFF
        return (osc * (mf + 1)) >> (2 * ((self.r(0x280, 4) >> 11) & 3))

    def state(self):
        """(baud, pram offset) if the wired channel is a working 8N1 UART, else None."""
        r = self.r
        if self.wired == SMC1:
            m = r(0xA82, 2)
            if m & 3 != 3 or m & 0x0030 != 0x0020 or (m >> 11) & 0xF != 9 or m & 0x0700:
                return None
            if r(0xABC, 4) & 0xC0 != 0xC0 or r(0xAB8, 4) & 0xC0:
                return None
            sim = r(0xAE0, 4)
            if sim & 0x8000:
                return None
            brg, pram = (sim >> 12) & 7, 0x3E80
        else:
            g = r(0xA00, 4)
            if g & 0x30 != 0x30 or g & 0xF != 4 or (g >> 16) & 3 != 2 or (g >> 14) & 3 != 2:
                return None
            if r(0xA08, 2) & 0xF010 != 0x3000:
                return None
            if r(0x952, 2) & 3 != 3 or r(0x950, 2) & 3:
                return None
            si = r(0xAEC, 4)
            if si & 0x40 or (si >> 3) & 7 != si & 7:
                return None
            brg, pram = si & 7, 0x3C00
        if brg > 3:
            return None
        v = r(0x9F0 + 4 * brg, 4)
        if not v & 0x10000:
            return None
        div = (((v >> 1) & 0xFFF) + 1) * (16 if v & 1 else 1)
        return self.brgclk() / 16 / div, pram

    def bd_table(self, pram, which):
        base = self.r(pram + 2 * which, 2)
        if not 0x2000 <= base < 0x3000:
            self.violations.append(f"{'TBASE' if which else 'RBASE'} {base:#x} outside DPRAM")
            return None
        return IMMR + base

    def cpcr(self, v):
        """INIT RX AND TX on the wired channel restarts the BD pointers."""
        ch = (v >> 4) & 0xF
        if (v >> 8) & 0xF == 0 and ch == (9 if self.wired == SMC1 else 0):
            self.rx_idx = self.tx_idx = 0

    def mode_write(self, off, v):
        if self.wired == SMC1 and off == 0xA00 and v & 0x30:
            self.other_enabled = True
        if self.wired == SCC1 and off == 0xA82 and v & 3:
            self.other_enabled = True

    def brg_write(self):
        st = self.state()
        if not st:
            return
        t = self.bd_table(st[1], 1)
        for i in range(8):
            if t and bytes(self.b.uc.mem_read(t + 8 * i, 2))[0] & 0x80:
                self.violations.append("BRG changed while a TX BD was still ready")

    def service(self):
        st = self.state()
        if st is None:
            self.from_host.clear()                 # nobody listening: the bytes are lost
            return
        baud, pram = st
        self.bauds.add(round(baud))
        uc = self.b.uc
        if self.r(pram + 6, 2) != 1:
            self.violations.append(f"MRBLR {self.r(pram + 6, 2)}")
        rt, tt = self.bd_table(pram, 0), self.bd_table(pram, 1)
        if rt is None or tt is None:
            return
        while self.from_host:
            bd = rt + 8 * self.rx_idx
            s = struct.unpack(">H", bytes(uc.mem_read(bd, 2)))[0]
            if not s & 0x8000:
                break                              # ring full: the rest waits on the wire
            c, hb = self.from_host.popleft()
            self.rx_count += 1
            if self.drop and self.drop(self.rx_count):
                continue
            flags = 0
            if abs(baud - hb) > 0.025 * hb:
                c, flags = random.randrange(256), 0x0010   # framing error, garbage
                self.garbled += 1
            buf = be32(bytes(uc.mem_read(bd + 4, 4)), 0)
            if not IMMR + 0x2000 <= buf < IMMR + 0x3000:
                self.violations.append(f"RX buffer {buf:#x} outside DPRAM")
            uc.mem_write(buf, bytes([c]))
            uc.mem_write(bd, struct.pack(">HH", (s & 0x3000) | flags, 1))
            self.rx_idx = 0 if s & 0x2000 else self.rx_idx + 1
        while True:
            bd = tt + 8 * self.tx_idx
            s, n = struct.unpack(">HH", bytes(uc.mem_read(bd, 4)))
            if not s & 0x8000:
                break
            buf = be32(bytes(uc.mem_read(bd + 4, 4)), 0)
            if not (IMMR + 0x2000 <= buf and buf + n <= IMMR + 0x3000):
                self.violations.append(f"TX buffer {buf:#x}+{n} outside DPRAM")
                data = b"\0" * n
            else:
                data = bytearray(uc.mem_read(buf, n))
            for i in range(n):
                self.tx_count += 1
                if abs(baud - self.host_baud) > 0.025 * self.host_baud:
                    data[i] = random.randrange(256)
                    self.garbled += 1
                elif self.corrupt and self.corrupt(self.tx_count):
                    data[i] ^= 0x10
            self.to_host += data
            uc.mem_write(bd, struct.pack(">H", s & ~0x8000))
            self.tx_idx = 0 if s & 0x2000 else self.tx_idx + 1


def symbols(elf):
    out = subprocess.run([NM, elf], capture_output=True, text=True, check=True).stdout
    return {l.split()[2]: int(l.split()[0], 16) for l in out.splitlines() if len(l.split()) == 3}


def call_sites(flash, target):
    """Addresses right after each `bl target` in the image."""
    sites = []
    for i in range(0, len(flash) - 3, 4):
        w = be32(flash, i)
        if w >> 26 == 18 and w & 3 == 1:
            li = w & 0x03FFFFFC
            if li & 0x02000000:
                li -= 0x04000000
            if FLASH + i + li == target:
                sites.append(FLASH + i + 4)
    return sites


class DumpBoard(netsim.NetBoard):
    """cosim/netsim board + the DB9 UART + write monitors. DPRAM page 0xFF002000 is plain
    memory (it holds the stack, the boot info and the window's BDs), the rest of IMMR stays
    MMIO with the cosim/netsim models."""

    def __init__(self, flash, sc, wired=SMC1, cfg=None, window=None, fill=True, seed=1):
        super().__init__(flash, sc, None)
        self.cable = False                         # no PHY: the NET window gives up at once
        if cfg:
            self.eeproms[0xA0] = cfg
        uc = self.uc
        uc.mem_unmap(IMMR, 0x10000)
        uc.mmio_map(IMMR, 0x2000, self._rd, None, self._wr, None)
        uc.mem_map(IMMR + 0x2000, 0x1000)
        uc.mmio_map(IMMR + 0x3000, 0xD000, lambda u, o, s, d: self._rd(u, o + 0x3000, s, d), None,
                    lambda u, o, s, v, d: self._wr(u, o + 0x3000, s, v, d), None)
        self.ticks = self.serviced = 0
        self.insns = 0
        self.uart = Uart(self, wired)
        self.in_window = False
        self.window_log = []                       # (event, ticks, reg log len, spr log len)
        self.writes = []                           # forbidden stores: (what, pc, addr, size)
        self.dpram_writes = set()
        self.i2c_log = []
        self.cpm_dma = []
        rng = random.Random(seed)
        if fill:                                   # flash, NVRAM and "warm" SDRAM contents
            self.main_img = rng.randbytes(0x2000000)
            uc.mem_write(MAIN, self.main_img)
            nv = bytearray(rng.randbytes(0x80000))
            nv[0x7FF0], nv[0x7FF1] = sc["tries"], 0
            uc.mem_write(NVRAM, bytes(nv))
            uc.mem_write(CS7, rng.randbytes(0x200000))
            uc.mem_write(0, rng.randbytes(RAM_SIZE))
        self.store_hooks = []
        self.exceptions_taken = []
        self.exceptions_taken_at = 0
        self.vector_hooks = None
        self.hook_ram_specials()
        for lo, n, what in ((FLASH, 0x80000, "boot flash"), (MAIN, 0x2000000, "main flash"),
                            (CS7, 0x200000, "CS7"), (NVRAM, 0x80000, "NVRAM"), (0, RAM_SIZE, "SDRAM")):
            self.store_hooks.append(uc.hook_add(UC_HOOK_MEM_WRITE, self._store, what,
                                                begin=lo, end=lo + n - 1))
        uc.hook_add(UC_HOOK_MEM_WRITE, self._dpstore, begin=IMMR + 0x2000, end=IMMR + 0x2FFF)
        if window:
            uc.hook_add(UC_HOOK_CODE, self._enter, begin=window, end=window)
            for a in call_sites(flash, window):
                uc.hook_add(UC_HOOK_CODE, self._leave, begin=a, end=a)

    # ---- monitors ----
    def _store(self, uc, access, addr, size, value, what):
        if what in ("NVRAM", "SDRAM") and not self.in_window:
            return                                 # the normal boot writes these
        self.writes.append((what, uc.reg_read(UC_PPC_REG_PC), addr, size))

    def _dpstore(self, uc, access, addr, size, value, _):
        if self.in_window:
            self.dpram_writes.add(addr - IMMR)

    def snapshot(self):
        return dict(ticks=self.ticks, regs=len(self.reg_writes), sprs=len(self.spr_log),
                    imm=bytes(self.dpram[:0x2000]),
                    dp=bytes(self.uc.mem_read(IMMR + 0x2000, 0x440)),
                    nv=bytes(self.uc.mem_read(NVRAM, 0x80000)),
                    ram=hashlib.sha256(bytes(self.uc.mem_read(0, RAM_SIZE))).hexdigest(),
                    sdram=bytes(self.uc.mem_read(0, RAM_SIZE)))

    def _enter(self, uc, addr, size, _):
        self.in_window = True
        self.window_log.append(("enter", self.snapshot()))

    def _leave(self, uc, addr, size, _):
        if self.in_window:
            self.in_window = False
            self.window_log.append(("leave", self.snapshot()))

    # ---- IMMR model additions ----
    def _rd(self, uc, off, size, _):
        if off == 0x99E:                           # TCN2: the clock of the windows
            self.ticks += TICK
            if self.ticks - self.serviced >= 200:   # the UART runs alongside: every 1 ms
                self.serviced = self.ticks
                self.uart.service()
            return self.ticks & 0xFFFF
        return super()._rd(uc, off, size, _)

    def _wr(self, uc, off, size, value, _):
        super()._wr(uc, off, size, value, _)
        if off == 0x9C0:
            self.uart.cpcr(value)
        elif off in (0xA00, 0xA82):
            self.uart.mode_write(off, value)
        elif off == 0x9F4:
            self.uart.brg_write()

    def _i2c(self):
        """cosim's I2C model on the DPRAM page (reads only; anything else is recorded)."""
        rd = lambda a, n: bytes(self.uc.mem_read(IMMR + a, n))
        t1 = be32(rd(0x2418, 4), 0)
        tx = rd(0x2420, 3)
        is_read = t1 >> 16 == 0xAC00 and tx[2] == (tx[0] | 1) and be32(rd(0x2410, 4), 0) >> 16 == 0x8400
        self.i2c_log.append((self.in_window, is_read, tx[0]))
        if not is_read:
            return
        n = (t1 & 0xFFFF) - 1
        buf = be32(rd(0x240C, 4), 0)
        self.uc.mem_write(buf, self.eeproms.get(tx[0] & 0xFE, b"\xff" * 256)[:n])
        st = be32(rd(0x2408, 4), 0)
        self.uc.mem_write(IMMR + 0x2408, struct.pack(">I", st & ~0x80000000))

    def hook_ram_specials(self):
        """Code our boot loader runs from SDRAM uses mtspr/mfspr/mtmsr too: its exception stubs (0x100..0x1400) and
        a stage 1 (0xF00000, two stages). Emulated as in the flash (cosim._special)."""
        if not self.vector_hooks:
            self.vector_hooks = [self.uc.hook_add(UC_HOOK_CODE, self._vector_special, begin=0x100, end=0x14FF),
                                 self.uc.hook_add(UC_HOOK_CODE, self._vector_special, begin=0xF00000, end=0xF0FFFF)]

    def _vector_special(self, uc, addr, size, _):
        w = struct.unpack(">I", bytes(uc.mem_read(addr, 4)))[0]
        if w >> 26 == 31 and ((w >> 1) & 0x3FF) in (467, 339, 146, 83, 370):
            spr = ((w >> 16) & 0x1F) | (((w >> 11) & 0x1F) << 5)
            if spr not in (1, 8, 9):
                self._special(uc, addr, size, None)

    # ---- running ----
    def step(self, n=SLICE):
        if self.reached_app:
            return False
        self.deliver()
        try:
            self.uc.emu_start(self.uc.reg_read(UC_PPC_REG_PC), 0xFFFFFFFF, count=n)
        except UcError as e:
            # a program exception (trap, illegal opcode): to the vector, as the CPU would. MSR[IP] picks the base:
            # 0xFFF00000 (nothing there once BR0 has moved: as on the real board, a dead end) or 0 (dbg.c's stubs).
            if e.errno != UC_ERR_EXCEPTION:
                raise
            msr = self.msr                              # cosim emulates mtmsr/mfmsr/mtspr/mfspr itself
            pc = self.uc.reg_read(UC_PPC_REG_PC) - 4    # Unicorn stops one instruction on
            self.exceptions_taken.append((0x700, pc, msr))
            self.exceptions_taken_at = self.insns
            if len(self.exceptions_taken) > 4:
                raise
            self.hook_ram_specials()
            self.spr[26], self.spr[27] = pc, msr & 0xFFFF | 0x00020000   # SRR0, SRR1 (trap)
            self.msr = msr & ~0x8032                    # EE, IR, DR off
            self.uc.reg_write(UC_PPC_REG_PC, (0xFFF00000 if msr & 0x40 else 0) + 0x700)
        self.insns += n
        self.uart.service()
        if self.insns > BUDGET:
            raise SystemExit("FAIL: instruction budget exhausted (hang?)")
        return not self.reached_app

    def run_to_app(self):
        while self.step(2_000_000):
            pass
        return self

    def ms(self):
        return self.ticks / 200.0

    def window_ms(self):
        e = [s for k, s in self.window_log if k == "enter"]
        l = [s for k, s in self.window_log if k == "leave"]
        return (l[0]["ticks"] - e[0]["ticks"]) / 200.0 if e and l else None


def only_ip_toggles_removed(log):
    """An SPR/MSR log without the mtmsr writes that only change MSR[IP] (dbg.c: vectors at 0 while SDRAM works)."""
    out, msr = [], None
    for op in log:
        if op[0] == "mtmsr" and msr is not None and (op[2] ^ msr) == 0x40:
            msr = op[2]
            continue
        if op[0] == "mtmsr":
            msr = op[2]
            op = (op[0], op[1], op[2] | 0x40)           # the other MSR writes, as they would be with IP set
        out.append(op)
    return out


class SimLink:
    """serdump.SerialLink over the emulated UART. Time is the board's timer 2 clock."""

    def __init__(self, board, can=None):
        self.b = board
        self.baud = serdump.BAUD
        self.can = can or (lambda baud: True)      # can the "adapter" do this speed?
        self.fake = 0.0                            # time after the app has started
        self.app_announced = False

    def write(self, data):
        for c in data:
            if c == serdump.FORBIDDEN:
                FAILS.append("host sent a 0x5A byte")
            self.b.uart.from_host.append((c, self.baud))

    def now(self):
        return self.b.ms() / 1000 + self.fake

    def _run(self, until, want_data):
        while self.now() < until:
            if want_data and self.b.uart.to_host:
                return
            if not self.b.step():
                if not self.app_announced:         # the app's 20 s status frame
                    self.app_announced = True
                    self.b.uart.to_host += serdump.HEARTBEAT
                    return
                self.fake += until - self.now()

    def read(self, timeout):
        self._run(self.now() + timeout, True)
        d, self.b.uart.to_host = bytes(self.b.uart.to_host), bytearray()
        return d

    def set_baud(self, baud):
        if self.can(baud):
            self.baud = baud
            self.b.uart.host_baud = baud

    def flush_input(self):
        self.b.uart.to_host = bytearray()

    def sleep(self, t):
        self._run(self.now() + t, False)


def quiet(*a, **k):
    pass


def expected(b, img, sc_spd):
    """What each region must hold, from the emulated board."""
    e = b.window_log[0][1]
    bi = e["dp"][0x240:0x298]
    f1b, f1s = be32(bi, 0x14), be32(bi, 0x50)
    f2b, f2s = be32(bi, 0x20), be32(bi, 0x54)
    nvs, bank = be32(bi, 0x40), be32(bi, 0x08)
    mem = lambda base, n: b.main_img[base - MAIN:base - MAIN + n]
    return {"bootflash": img, "flash1": mem(f1b, f1s), "flash2": mem(f2b, f2s) if f2s else None,
            "nvram": e["nv"][:nvs], "eeprom_a0": b.eeproms[0xA0], "eeprom_a2": b.eeproms[0xA2],
            "sdram": e["sdram"][:bank]}


def readonly_checks(b, label):
    check(not b.writes, f"{label}: no store to flash or CS7, none to NVRAM or SDRAM in the window"
          + (f" (first: {b.writes[0][0]} {b.writes[0][2]:#x} from pc {b.writes[0][1]:#x})" if b.writes else ""))
    win = [x for x in b.i2c_log if x[0]]
    check(all(x[1] for x in b.i2c_log), f"{label}: all {len(b.i2c_log)} I2C transactions are reads "
          f"({len(win)} in the window, to {sorted({hex(x[2]) for x in win})})")
    bad = sorted(a for a in b.dpram_writes if a < 0x2440 and not 0x2408 <= a < 0x2428)
    check(not bad, f"{label}: the window wrote DPRAM only in its scratch, the stack and the I2C BDs"
          + (f" (also {bad[:4]})" if bad else ""))
    check(not b.uart.violations, f"{label}: CPM BDs and buffers all in DPRAM, MRBLR 1, BRG never "
          f"changed mid-transmission" + (f" ({b.uart.violations[:3]})" if b.uart.violations else ""))
    check(not b.uart.other_enabled, f"{label}: only the board's DB9 channel ({b.uart.wired}) was enabled")


def full_session(title, img, sc, wired, cfg, syms, app, outdir, extra=None):
    print(f"\n=== {title} ===")
    b = DumpBoard(img, sc, wired, cfg, syms["dump_window"])
    link = SimLink(b)
    s = serdump.Session(link, log=quiet, block=8192)
    line = s.wait_window(30)
    check(line == f"MQDUMP READY 1 {wired} 115200", f"magic answered on {wired}: {line!r}")
    info = s.hello()
    print("     " + info.strip().replace("\n", "\n     "))
    check(f"uart={wired}" in info and "region=1 bootflash f8180000 00080000" in info,
          "HELLO info names the UART and the memory map")
    if extra:
        extra(b, s, link)
    exp = expected(b, img, sc["spd"])
    t_wire = 0
    for name in ("eeprom_a0", "eeprom_a2", "nvram", "bootflash", "flash1", "flash2", "sdram"):
        before = s.stats["received"]
        data = s.dump_region(name, outdir)
        t_wire += (s.stats["received"] - before) * 10 / s.baud
        if exp[name] is None:
            check(data is None, f"{name}: absent on this board, reported absent")
            continue
        f = os.path.join(outdir, name + ".bin")
        good = data == exp[name] and open(f, "rb").read() == exp[name]
        check(good, f"{name}: {len(exp[name])} bytes, file identical to the emulated contents")
    archive_orig = ORIG[(be32(ORIG, 0) + 3) & ~3:]
    first = (be32(img, 0) + 3) & ~3
    bf = open(os.path.join(outdir, "bootflash.bin"), "rb").read()
    check(bf[first:first + len(archive_orig)].rstrip(b"\0") == archive_orig.rstrip(b"\0"),
          "bootflash.bin: archive entries 1..33 equal dumps/bootflash.bin's")
    check(b.window_log[0][1]["sdram"] == b.snapshot()["sdram"],
          "SDRAM unchanged by the window so far (warm-reset dump is meaningful)")
    s.boot()
    b.run_to_app()
    check(b.reached_app and netsim.check_app(b, app), "BOOT: normal boot, app in RAM = ELITE_RGB")
    readonly_checks(b, title.split(":")[0])
    print(f"     session: {s.stats}, wire time at {s.baud} baud: {t_wire:.0f} s")
    return b, s


def robustness(img, sc, syms, outdir):
    print("\n=== robustness (fixed memory board, SMC1) ===")
    b = DumpBoard(img, sc, SMC1, None, syms["dump_window"])
    link = SimLink(b)
    s = serdump.Session(link, log=quiet, block=4096)
    s.wait_window(30)
    s.hello()

    # garbage and a bad CRC: no reply, then the sign still answers
    bad = serdump.safe_frame(serdump.PING, 7)[1]
    link.write(b"\x00\x11garbage\xff" + bad[:-1] + bytes([bad[-1] ^ 1]))
    try:
        s.request(serdump.PING, timeout=0.3, retries=0)
        check(True, "garbage and a request with a bad CRC are ignored; the next PING is answered")
    except serdump.DumpError as e:
        check(False, f"PING after garbage: {e}")

    def err(ftype, payload):
        try:
            s.request(ftype, payload, timeout=0.5, retries=0)
            return None
        except serdump.DumpError as e:
            return str(e)
    r12 = lambda rid, off, n: struct.pack(">B3xII", rid, off, n)
    cases = [("region 0", serdump.READ, r12(0, 0, 16), "no such region"),
             ("region 10", serdump.READ, r12(10, 0, 16), "no such region"),
             ("region 99 CRC", serdump.CRC, r12(99, 0, 16), "no such region"),
             ("offset past the end", serdump.READ, r12(4, 0x8000, 16), "out of range"),
             ("offset + length wraps", serdump.READ, r12(1, 0xFFFFF000, 0x2000), "out of range"),
             ("CRC length past the end", serdump.CRC, r12(1, 0x1000, 0x80000), "out of range"),
             ("length 0", serdump.READ, r12(1, 0, 0), "out of range"),
             ("length > 16 KB", serdump.READ, r12(1, 0, 0x4001), "out of range"),
             ("short payload", serdump.READ, b"\x01\x00\x00", "bad length"),
             ("unknown type 0x42", 0x42, b"", "unknown request"),
             ("speed 921600 (13 % off at 50 MHz)", serdump.SPEED, struct.pack(">I", 921600), "speed not")]
    for what, t, p, want in cases:
        e = err(t, p)
        check(e is not None and want in e, f"bad request ({what}) -> error: {e}")
    s.ping(timeout=0.5)

    # line errors both ways: every 20000th byte to the host flipped, every 500th from it lost
    b.uart.corrupt = lambda i: i % 20000 == 0
    b.uart.drop = lambda i: i % 500 == 0
    data = s.dump_region("bootflash", outdir)
    check(data == img, f"bootflash read through flipped and lost bytes: identical "
          f"({s.stats['crc_errors']} bad CRCs, {s.stats['retries']} retries)")
    b.uart.corrupt = b.uart.drop = None

    # speed change the host can follow, then one it cannot
    got = s.set_speed(460800)
    check(got == 446428 and any(abs(x - 446428) < 2 for x in b.uart.bauds)
          and any(abs(x - 115741) < 2 for x in b.uart.bauds),
          f"SPEED 460800: sign runs at {got} (BRGCLK 50 MHz / 16 / 7), host follows, PING works")
    d2 = s.dump_region("nvram", outdir)
    check(d2 == b.window_log[0][1]["nv"][:0x8000], "nvram read at the higher speed: identical")
    s2 = s.set_speed(serdump.BAUD)
    check(s2 == 115740 or s2 == serdump.BAUD, f"SPEED back to 115200: {s2}")
    link.set_baud(serdump.BAUD)
    s.baud = serdump.BAUD
    link.can = lambda baud: baud == serdump.BAUD   # an adapter stuck at 115200
    t0 = link.now()
    got = s.set_speed(230400)
    check(got == serdump.BAUD and link.now() - t0 > serdump.FALLBACK_MS / 1000,
          f"SPEED 230400 the host cannot follow: both back at 115200 after "
          f"{link.now() - t0:.1f} s, session goes on")
    link.can = lambda baud: True

    # resume: stop flash2 after 300 blocks, then a new boot of the board continues it
    calls = {"n": 0}
    orig_read = s.read_block

    def flaky(*a):
        calls["n"] += 1
        if calls["n"] > 300:
            raise serdump.DumpError("cable pulled")
        return orig_read(*a)
    s.read_block = flaky
    try:
        s.dump_region("flash2", outdir)
    except serdump.DumpError:
        pass
    part = os.path.join(outdir, "flash2.bin.part")
    have = os.path.getsize(part)
    check(have == 300 * 4096, f"interrupted dump left {have:#x} bytes in flash2.bin.part")
    # spoil one byte in the second MB of the partial file: resume must restart from there
    with open(part, "r+b") as f:
        f.seek(0x100010)
        f.write(b"\xEE")
    readonly_checks(b, "robustness")

    print("\n=== resume after a power cycle ===")
    b2 = DumpBoard(img, sc, SMC1, None, syms["dump_window"])
    s2 = serdump.Session(SimLink(b2), log=quiet, block=4096)
    s2.wait_window(30)
    s2.hello()
    reads = {"n": 0}
    orig2 = s2.read_block

    def counting(*a):
        reads["n"] += 1
        return orig2(*a)
    s2.read_block = counting
    data = s2.dump_region("flash2", outdir)
    exp = b2.main_img[0x800000:0xC00000]
    check(data == exp and reads["n"] == (0x400000 - 0x100000) // 4096,
          f"resume: verified the first MB, re-read from 0x100000 ({reads['n']} blocks), file identical")

    # idle timeout: the host goes quiet, the window closes after 30 s and the app boots
    t0 = b2.ms()
    while b2.step():
        pass
    t = (b2.ms() - t0) / 1000
    check(b2.reached_app and 29.5 < t < 31.5, f"idle timeout: window closed after {t:.1f} s, normal boot")
    check(b"MQDUMP CLOSED" in bytes(b2.uart.to_host), "the sign said MQDUMP CLOSED before booting")
    readonly_checks(b2, "resume/idle")


def no_magic(title, img, ref_img, sc, wired, cfg, syms, app, noise=False):
    print(f"\n=== no magic: {title} ===")
    ref = DumpBoard(ref_img, sc, wired, cfg, None).run_to_app()
    b = DumpBoard(img, sc, wired, cfg, syms["dump_window"])
    link = SimLink(b)
    junk = serdump.HEARTBEAT + b"MQ?DUMQ\rMQ" + bytes(range(0x30, 0x50))
    while b.step(SLICE if noise else 2_000_000):
        if noise and b.in_window and len(b.uart.from_host) < 64:
            link.write(junk)                        # the app's status frame and junk, no magic
    if noise:
        check(b.uart.rx_count > 100, f"{b.uart.rx_count} junk bytes reached the UART in the window")
    e = dict(b.window_log)["enter"]
    l = dict(b.window_log)["leave"]
    ms = b.window_ms()
    check(b.reached_app and netsim.check_app(b, app), "normal boot from flash, app in RAM = ELITE_RGB")
    check(2000 <= ms <= 2010, f"window open {ms:.1f} ms of its own clock (DUMP_LISTEN_MS = 2000), "
          f"no other delay")
    check(not b.uart.to_host, "the sign sent nothing on the DB9")
    regs = b.reg_writes[:e["regs"]] + b.reg_writes[l["regs"]:]
    sprs = b.spr_log[:e["sprs"]] + b.spr_log[l["sprs"]:]
    # the debug record's own (bootloader/src/dbg.c), and only those: RSR cleared once, MSR[IP] off (stubs) and on
    rsr = [w for w in regs if w[0] == 0x288]
    check(len(rsr) == 1 and not [w for w in ref.reg_writes if w[0] == 0x288],
          "the debug record clears RSR once (write 1 to clear)")
    regs = [w for w in regs if w[0] != 0x288]
    sprs = only_ip_toggles_removed(sprs)
    check(regs == ref.reg_writes, f"register writes outside the window identical to the default "
          f"build ({len(ref.reg_writes)})")
    want = only_ip_toggles_removed(ref.spr_log)
    first = next((i for i, (x, y) in enumerate(zip(sprs, want)) if x != y), min(len(sprs), len(want)))
    check(sprs == want, f"SPR/MSR writes outside the window identical ({len(ref.spr_log)}; MSR[IP] aside)" +
          ("" if sprs == want else f" first difference at {first}: {sprs[first:first + 3]} vs {want[first:first + 3]}"
                                   f" (lengths {len(sprs)}/{len(want)})"))
    skip = {0x00E, 0x86C, 0x870, 0x99E, 0x9B0, 0x9B2, 0x9C0, 0xA10, 0xA11, 0xA86}  # SWSR, events, cmds
    diff = [o for o in range(0, 0x2000) if e["imm"][o] != l["imm"][o] and o not in skip
            and o - 1 not in skip]
    check(not diff, "every register the window touched is put back" +
          (f" (differs: {[hex(o) for o in diff[:8]]})" if diff else ""))
    rs = ref.snapshot()
    bs = b.snapshot()
    # the hand-off's flash sizes (RAM 0x40 bank 1, 0x48 bank 2) leave out the update slot and the loader sector (576 KB, the last bank's
    # end) with our boot loader; everything else must be identical
    ra, ba = bytearray(rs["sdram"]), bytearray(bs["sdram"])
    slot_bank = 0x48 if be32(ra, 0x48) else 0x40
    check(be32(ba, slot_bank) == be32(ra, slot_bank) - 0x80000 - 0x10000,
          f"hand-off: the bank with the update slot is shown 576 KB smaller: the slot and the loader sector "
          f"({be32(ba, slot_bank):#x})")
    ra[0x40:0x44] = ba[0x40:0x44] = ra[0x48:0x4C] = ba[0x48:0x4C] = b"\0" * 4
    check(ra == ba, "SDRAM after boot identical to the default build (but for the slot)")
    rec = bytes(bs["nv"][0x7C00:0x7C00 + 0x200])
    check(rec[:4] == b"MQDB", "the NVRAM debug record is started (bootloader/include/dbg.h)")
    check(rs["nv"][:0x7C00] == bs["nv"][:0x7C00] and rs["nv"][0x7E00:] == bs["nv"][0x7E00:],
          "NVRAM after boot identical to the default build (but for the debug record at 0x7C00)")
    check(rs["dp"][0x240:0x298] == bs["dp"][0x240:0x298] and rs["dp"][0x398:0x3DC] == bs["dp"][0x398:0x3DC],
          "boot info and bank list identical")
    check(ref.uc.reg_read(UC_PPC_REG_PC) == b.uc.reg_read(UC_PPC_REG_PC) == 0 and ref.msr == b.msr,
          "same hand-off (pc 0, MSR)")
    readonly_checks(b, "no magic")
    return b


def net_dump(img, sc, syms, app):
    print("\n=== NET=1 DUMP=1: serial window, then the network window (no PHY) ===")
    for magic in (False, True):
        b = DumpBoard(img, sc, SMC1, None, syms["dump_window"])
        link = SimLink(b)
        fec_before = None
        if magic:
            s = serdump.Session(link, log=quiet)
            s.wait_window(30)
            s.hello()
            bf = b""
            for off in range(0, 0x80000, 0x4000):
                bf += s.read_block(1, off, 0x4000)
            check(bf == img, "with magic: bootflash read over serial, identical")
            check(s.read_block(5, 0, 256) == b.eeproms[0xA0], "with magic: eeprom_a0 read live over I2C")
            fec_before = [w for w in b.reg_writes if w[0] == 0xE40]
            s.boot()
        b.run_to_app()
        l = dict(b.window_log)["leave"]
        fec = [i for i, w in enumerate(b.reg_writes) if w[0] == 0xE40]
        check(b.reached_app and netsim.check_app(b, app) and fec and fec[0] >= l["regs"]
              and (fec_before is None or not fec_before),
              f"{'magic' if magic else 'no magic'}: serial window first, then the FEC is tried "
              f"(no PHY), normal boot, app = ELITE_RGB")
        readonly_checks(b, "net+dump")


def header_matches():
    h = open(os.path.join(HERE, "..", "include", "serdump.h")).read()
    d = dict(re.findall(r"#define\s+DUMP_(\w+)\s+(\S+)", h))
    num = lambda k: int(d[k], 0)
    ok = (d["MAGIC"].strip('"').encode().replace(b"\\r", b"\r") == serdump.MAGIC
          and num("BAUD") == serdump.BAUD and num("LISTEN_MS") == serdump.LISTEN_MS
          and num("IDLE_MS") == serdump.IDLE_MS and num("FALLBACK_MS") == serdump.FALLBACK_MS
          and num("BLOCK_MAX") == serdump.BLOCK_MAX and num("HDR") == serdump.HDR
          and all(num(k) == getattr(serdump, k) for k in ("HELLO", "READ", "CRC", "SPEED", "PING", "BOOT", "ERROR"))
          and all(num("R_" + k) == v for k, v in (("BOOT", 1), ("FLASH1", 2), ("FLASH2", 3), ("NVRAM", 4),
                                                   ("EE_A0", 5), ("EE_A2", 6), ("SDRAM", 7))))
    check(ok, "tools/serdump.py constants match include/serdump.h")


ORIG = b""


def main(dump_img, default_img, netdump_img, orig_path, app_path):
    global ORIG
    t0 = time.time()
    img = open(dump_img, "rb").read()
    ref = open(default_img, "rb").read()
    netimg = open(netdump_img, "rb").read()
    ORIG = open(orig_path, "rb").read()
    app = open(app_path, "rb").read()
    syms = symbols(os.path.join(os.path.dirname(dump_img), "bootstrap.elf"))
    nsyms = symbols(os.path.join(os.path.dirname(netdump_img), "bootstrap.elf"))
    fixed = cosim.SCENARIOS["fixed config, no upgrade (tries=0xFF)"]
    spd = cosim.SCENARIOS["SPD SDRAM, one flash bank + CS7 (tries=0xFF)"]
    portcb = cfg_variant(False, byte4=2)
    portcb2 = cfg_variant(False, byte4=1, byte5=0x50)
    header_matches()

    with tempfile.TemporaryDirectory() as out:
        full_session("fixed memory, default board: DB9 on SMC1", img, fixed, SMC1, None, syms, app,
                     os.path.join(out, "a"))
        full_session("SPD memory, default board: DB9 on SMC1", img, spd, SMC1, None, syms, app,
                     os.path.join(out, "b"))
        full_session("fixed memory, Port C/B board (cfg byte 4 = 2): DB9 on SCC1", img, fixed, SCC1,
                     portcb, syms, app, os.path.join(out, "c"))
        robustness(img, fixed, syms, os.path.join(out, "d"))

        print("\n=== Port C/B board via byte 4 = 1, byte 5 = 0x50 ===")
        b = DumpBoard(img, fixed, SCC1, portcb2, syms["dump_window"])
        s = serdump.Session(SimLink(b), log=quiet)
        check(s.wait_window(30).endswith("SCC1 115200"), "DB9 on SCC1, as the app's rule says")
        s.boot()
        b.run_to_app()
        check(b.reached_app, "boots")
        print("\n=== the wrong channel hears nothing ===")
        b = DumpBoard(img, fixed, SCC1, None, syms["dump_window"])   # default board, host on SCC1 pins
        s = serdump.Session(SimLink(b), log=quiet)
        try:
            s.wait_window(5)
            check(False, "a default board answered on SCC1")
        except serdump.DumpError:
            check(b.reached_app, "default board: nothing on SCC1, window closed, normal boot")

    no_magic("fixed memory, default board (SMC1)", img, ref, fixed, SMC1, None, syms, app)
    no_magic("SPD memory, default board (SMC1)", img, ref, spd, SMC1, None, syms, app)
    no_magic("fixed memory, Port C/B board (SCC1)", img, ref, fixed, SCC1, portcb, syms, app)
    no_magic("line noise but no magic (SMC1)", img, ref, fixed, SMC1, None, syms, app, noise=True)
    net_dump(netimg, fixed, nsyms, app)

    print(f"\n{'ALL SERIAL DUMP CHECKS PASS' if not FAILS else f'{len(FAILS)} FAILURES'}  "
          f"({time.time() - t0:.0f} s wall)")
    for f in FAILS:
        print("  FAIL", f)
    return 0 if not FAILS else 1


if __name__ == "__main__":
    if len(sys.argv) != 6:
        raise SystemExit(__doc__)
    sys.exit(main(*sys.argv[1:]))
