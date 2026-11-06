#!/usr/bin/env python3
"""Whole-system simulation of the sign: the real boot ROM and ELITE_RGB (RTEMS included) on an emulated MPC862 board.

Builds on bootloader/test/cosim.py: its Board boots the boot ROM from your dump to the application at 0, with the
memory controller, the I2C config EEPROMs and the hand-off block. From there this module keeps running the app:
  - MPC8xx special instructions are intercepted over the app's code too (mtspr/mfspr/mtmsr/mfmsr/tlbia, rfi, mftb),
    with a shadow MSR and SRR0/SRR1, so Unicorn never turns translation on (the app's MMU map is 1:1);
  - interrupts are delivered between execution slices, as the MPC862 does (SRR0/SRR1, MSR[EE] cleared, vector):
    the SIU controller (SIPEND 0x10, SIMASK 0x14, SIVEC 0x1C) and the PIT (PISCR 0x240, PITC 0x244), which is
    RTEMS's clock tick (system_tick_init / system_tick_isr);
  - time: a fixed number of instructions per microsecond (CPU_MIPS).

Milestones (docs/REBUILD-PLAN.md, item E): 1. the first RTEMS context switch; 2. all tasks running; 3. panel frames;
4. attract mode; 5. an RS-485 record on the panel; 6. stock vs rebuilt.

Usage: appsim.py [APP_IMAGE]   (default: analysis/extracted/ELITE_RGB; the boot ROM is dumps/bootflash.bin)
"""
import os
import struct
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(HERE)
sys.path.insert(0, os.path.join(REPO, "bootloader", "test"))
sys.path.insert(0, os.path.join(REPO, "analysis", "tools"))
import cosim  # noqa: E402
from unicorn import UcError, UC_HOOK_CODE  # noqa: E402
from unicorn.ppc_const import UC_PPC_REG_PC, UC_PPC_REG_0, UC_PPC_REG_1  # noqa: E402

IMMR = cosim.IMMR
CODE_END = 0x988C4
CPU_MIPS = 40                                   # instructions per microsecond, nominal
SLICE = 200000                                 # instructions between interrupt checks
IR_ISR = (0x59608, 0x59738)                    # ir_edge_isr: its mftb reads get the edge times
SLICE_IR = 10000                               # ... while remote-control edges are being delivered

# MSR bits
EE, PR, IR, DR, RI = 0x8000, 0x4000, 0x20, 0x10, 0x2
SPR_SRR0, SPR_SRR1, SPR_DEC, SPR_TBL_R, SPR_TBU_R = 26, 27, 22, 268, 269
SPR_EIE, SPR_EID, SPR_NRI = 80, 81, 82


COUNT_MODE = os.environ.get("SIM_COUNT", "block")   # 'block' (sim/fastcount.c) or 'unicorn' (emu_start count=N)


class SimCore:
    """The C half of the simulator (sim/fastcount.c, built on first use into build/sim/ with cc): exact instruction
    counting without Unicorn's per-instruction count hook, and the interrupt enable/disable instructions against
    the shadow MSR, which lives here (s.msr). Without a C compiler, or with SIM_COUNT=unicorn, the simulator falls
    back to emu_start(count=N) and Python hooks, with the same results.

    run(pc, n) executes exactly the instructions emu_start(pc, count=n) would, and stops on the same instruction.
    A Python code hook that moves the PC (rfi, the busy-wait skip, the other special instructions) must call
    skip_rest(addr): the rest of its block does not run."""
    _lib = False

    @classmethod
    def library(cls):
        """The built library, or None (no compiler; the reason is printed once)."""
        if cls._lib is False:
            import ctypes
            import hashlib
            import subprocess
            src = os.path.join(HERE, "fastcount.c")
            code = open(src, "rb").read()
            out = os.path.join(REPO, "build", "sim", f"fastcount-{hashlib.sha256(code).hexdigest()[:12]}.so")
            try:
                if not os.path.exists(out):
                    os.makedirs(os.path.dirname(out), exist_ok=True)
                    tmp = f"{out}.{os.getpid()}"
                    subprocess.run([os.environ.get("CC", "cc"), "-O2", "-shared", "-fPIC", "-o", tmp, src],
                                   check=True, capture_output=True)
                    os.replace(tmp, out)
                cls._lib = ctypes.CDLL(out)
            except (OSError, subprocess.CalledProcessError) as e:
                print(f"appsim: no fast path ({e}); counting with Unicorn (slower, same results)", file=sys.stderr)
                cls._lib = None
        return cls._lib

    def __init__(self):
        import ctypes
        self.lib = self.library() if COUNT_MODE != "unicorn" else None

        class Sim(ctypes.Structure):
            _fields_ = [(n, ctypes.c_uint64) for n in ("count", "limit", "block", "block_n", "stop_pc", "stop_n",
                                                         "stopped", "msr")] + \
                       [("reg_pc", ctypes.c_int64), ("reg_0", ctypes.c_int64)] + \
                       [(n, ctypes.c_void_p) for n in ("emu_stop", "reg_read", "reg_write", "mem_read", "python")]
        self.s = Sim()
        self.s.limit = 1 << 63
        self.uc = None
        self.counting = False

    def attach(self, uc, python_special):
        """Point the C hooks at uc; python_special(addr) handles the special instructions the C code does not."""
        import ctypes
        from unicorn.unicorn_py3 import unicorn as ucmod
        self.uc, s, lib = uc, self.s, ucmod.uclib
        s.reg_pc, s.reg_0 = UC_PPC_REG_PC, UC_PPC_REG_0
        for n in ("emu_stop", "reg_read", "reg_write", "mem_read"):
            setattr(s, n, ctypes.cast(getattr(lib, "uc_" + n), ctypes.c_void_p))
        self._py = python_special
        s.python = ctypes.cast(python_special, ctypes.c_void_p)
        self._ud = ctypes.cast(ctypes.addressof(s), ctypes.c_void_p)

    def _hook(self, kind, fn, begin, end):
        import ctypes
        from unicorn.unicorn_py3 import unicorn as ucmod
        handle = ucmod.uc_hook_h()
        err = ucmod.uclib.uc_hook_add(self.uc._uch, ctypes.byref(handle), kind,
                                      ctypes.cast(getattr(self.lib, fn), ctypes.c_void_p), self._ud,
                                      ctypes.c_uint64(begin), ctypes.c_uint64(end))
        if err:
            raise UcError(err)
        return handle.value

    def special_hook(self, addr):
        self._hook(UC_HOOK_CODE, "fastcount_special", addr, addr)

    def start_counting(self):
        from unicorn import UC_HOOK_BLOCK
        self._hook(UC_HOOK_BLOCK, "fastcount_block", 1, 0)
        self.uc.ctl_flush_tb()                             # every block from now on calls the hook
        self.counting = True

    def skip_rest(self, addr):
        """The instruction at addr was the last of its block to run (a hook moved the PC there)."""
        s = self.s
        k = (addr - s.block) // 4 + 1
        if 0 < k < s.block_n:
            s.count -= s.block_n - k
            s.block_n = k

    def run(self, pc, n):
        """Run n instructions from pc. Returns the number run: n, or fewer if something else stopped the
        emulation (a hook's emu_stop; then the block that hook was in counts whole, as a block hook counts)."""
        if not self.counting:
            self.start_counting()
        uc, c = self.uc, self.s
        start = c.count
        target = start + n
        try:
            while c.count < target:
                c.stopped = 0
                c.limit = target
                uc.emu_start(pc, 0xFFFFFFFF)
                pc = uc.reg_read(UC_PPC_REG_PC)
                if not c.stopped:                          # someone else's emu_stop
                    break
                r = target - c.count
                if not r:
                    break
                # The next block (at pc) would cross the target: run its first r instructions alone, with the
                # emulation's end address r instructions in. Unicorn applies an end address when it translates a
                # block, so the block is dropped from the translation cache before (to cut it there) and after
                # (so no later run stops there).
                c.stopped = 0
                until = pc + 4 * r
                uc.ctl_remove_cache(pc, pc + 4)
                try:
                    uc.emu_start(pc, until)
                finally:
                    uc.ctl_remove_cache(pc, until + 4)
                pc = uc.reg_read(UC_PPC_REG_PC)
                if pc != until and not c.stopped:          # someone else's emu_stop
                    break
        finally:
            c.limit = 1 << 63
        return c.count - start


class RawUc:
    """Direct ctypes calls into Unicorn for the hooks that run hundreds of thousands of times a second (the special
    instructions, the IMMR callbacks): the same calls the unicorn package makes, without its per-call wrapping.
    A Python exception in a hook stops the emulation and is raised by emu_start, as the package does."""

    def __init__(self, uc):
        import ctypes
        from unicorn.unicorn_py3 import unicorn as ucmod
        self.uc, self.uch, self.lib, self.m = uc, uc._uch, ucmod.uclib, ucmod
        self._v = ctypes.c_uint64()
        self._ref = ctypes.byref(self._v)
        self._buf = ctypes.create_string_buffer(4)
        self._keep = []

    def reg(self, r):
        self._v.value = 0
        self.lib.uc_reg_read(self.uch, r, self._ref)
        return self._v.value

    def set_reg(self, r, v):
        self._v.value = v
        self.lib.uc_reg_write(self.uch, r, self._ref)

    def word(self, a):
        """The big-endian 32-bit word at a."""
        if self.lib.uc_mem_read(self.uch, a, self._buf, 4):
            raise UcError(self.uc.errno())
        return int.from_bytes(self._buf.raw, "big")

    def _guard(self, fn):
        uc = self.uc

        def guarded(*args):
            try:
                return fn(*args)
            except BaseException as e:                       # noqa: BLE001 - raised again by emu_start
                if uc._hook_exception is None:
                    uc._hook_exception = e
                uc.emu_stop()
                return 0
        return guarded

    def code_callback(self, fn):
        """A C code-hook callback (kept alive here) that calls fn(addr)."""
        f = self._guard(fn)

        def hook(h, addr, size, ud):
            f(addr)
        cb = self.m.HOOK_CODE_CFUNC(hook)
        self._keep.append(cb)
        return cb

    def code_hook(self, fn, begin, end):
        """fn(addr) before the instruction at each address in begin..end."""
        import ctypes
        f = self._guard(fn)
        def hook(h, addr, size, ud):
            f(addr)
        cb = self.m.HOOK_CODE_CFUNC(hook)
        handle = self.m.uc_hook_h()
        err = self.lib.uc_hook_add(self.uch, ctypes.byref(handle), UC_HOOK_CODE, cb, None, ctypes.c_uint64(begin),
                                   ctypes.c_uint64(end))
        if err:
            raise UcError(err)
        self._keep.append(cb)
        return handle.value

    def mmio_map(self, address, size, rd, wr):
        """uc.mmio_map(address, size, rd, None, wr, None), with rd(uc, off, size, None) and
        wr(uc, off, size, value, None) as the unicorn package calls them."""
        uc = self.uc
        r, w = self._guard(rd), self._guard(wr)
        rcb = self.m.MMIO_READ_CFUNC(lambda h, off, sz, ud: r(uc, off, sz, None))

        def write(h, off, sz, v, ud):
            w(uc, off, sz, v, None)
        wcb = self.m.MMIO_WRITE_CFUNC(write)
        err = self.lib.uc_mmio_map(self.uch, address, size, rcb, None, wcb, None)
        if err:
            raise UcError(err)
        self._keep += [rcb, wcb]


def sym():
    """{name: address} from the named decompile, for readable reports."""
    import re
    out = {}
    p = os.path.join(REPO, "analysis", "ghidra_out", "elite_rgb_named.functions.txt")
    for line in open(p):
        m = re.match(r"([0-9a-f]{8})\s+(\S+)", line)
        if m:
            out[m.group(2)] = int(m.group(1), 16)
    return out


def set_geometry(eeprom, word=0x80C0, w=80, h=16):
    """Config EEPROM display geometry (non-SPD board): +0x10 low byte = width ((w - 20) / 20 in bits 4..7) and
    height ((h - 16) / 16 in bits 0..3), which panel_width / panel_height read (the LED driver's frame size); +0x40
    = the display word (menu item 1; display_width / display_height for drawing). Section checksums (sum/xor at
    0x3E/0x3F and 0x7E/0x7F, eeprom_section_checksum) are updated."""
    eeprom[0x13] = ((w - 20) // 20) << 4 | (h - 16) // 16
    struct.pack_into(">H", eeprom, 0x40, word)
    for lo, hi in ((0x00, 0x3E), (0x40, 0x7E)):
        s = x = 0
        for v in eeprom[lo:hi]:
            s, x = (s + v) & 0xFF, x ^ v
        eeprom[hi], eeprom[hi + 1] = s, x


class AppBoard(cosim.Board):
    def __init__(self, flash, scenario, app, geometry=(0x80C0, 80, 16)):
        self.core = SimCore()                           # holds the shadow MSR, so before cosim.Board sets it
        super().__init__(flash, scenario, None)
        self.eeproms[0xA0] = bytearray(self.eeproms[0xA0])
        set_geometry(self.eeproms[0xA0], *geometry)
        self.uc.mem_map(cosim.RAM_SIZE, 0x8000000 - cosim.RAM_SIZE)   # all 128 MB the config EEPROM declares
        self.app = app
        self.icount = 0
        self.skew = 0                                   # instructions' worth of time skipped by busy-wait shortcuts
        self.pit_next = None
        self.frames = []
        self.rx_queue = []          # (at instruction count, minor, bytes): serial input, delivered in order
        self.rx_idx = {}            # minor -> next rx BD the CPM fills
        self.tx_idx = {}            # minor -> next tx BD the CPM sends
        self.serial_tx = {}         # minor -> bytes the app sent
        self.ir_queue = []          # (at_frame, key char): remote-control key presses
        self.ir_edges = []          # (level, virtual timebase) edges of the frame being sent
        self.ir_level = 1           # the IR receiver output on PC13 (PCDAT 0x0004), idle high
        self.ir_tb = 0              # what mftb returns inside ir_edge_isr
        self.ir_keys_sent = 0
        self.ir_last = None         # simulated time (instructions) the last key was sent
        self.log = []
        self.events = {}
        self.in_app = False         # set when the app runs (run_app)
        self.raw = RawUc(self.uc)
        self._specials = {}         # instruction word -> what it does (_special_fn)
        self.uc.mem_unmap(IMMR, 0x10000)                # the IMMR model again, through the direct callbacks
        self.raw.mmio_map(IMMR, 0x10000, self._rd, self._wr)
        self.core.attach(self.uc, self.raw.code_callback(self._special_app))

    @property
    def msr(self):
        """The shadow MSR (the MPC8xx MSR as the app sets it; Unicorn's own MSR keeps translation off)."""
        return self.core.s.msr

    @msr.setter
    def msr(self, v):
        self.core.s.msr = v

    def emu(self, pc, n):
        """Run n instructions from pc, exactly as emu_start(pc, count=n). Returns the number run (fewer only if a
        hook stopped the emulation)."""
        if self.core.lib is None:
            self.uc.emu_start(pc, 0xFFFFFFFF, count=n)
            return n
        return self.core.run(pc, n)

    def _done(self, uc, addr, size, _):
        """cosim stops at the jump to the app (PC 0); after that, address 0 is just app code."""
        if not getattr(self, "reached_app", False) or not getattr(self, "in_app", False):
            self.reached_app = True
            uc.emu_stop()

    def _i2c(self):
        """CPM I2C master, as the hardware does it: the BD tables come from the parameter RAM (RBASE 0x3C80, TBASE
        0x3C82). Write-then-read: TX BD0 = [dev W, offset], TX BD1 = [dev R, n dummy bytes] -> n EEPROM bytes into
        the RX BD buffer. A write-only transfer stores its bytes in the (in-memory) EEPROM. Then R/E bits clear."""
        if not getattr(self, "in_app", False):
            return super()._i2c()
        d = self.dpram
        rbase, tbase = int.from_bytes(d[0x3C80:0x3C82], "big"), int.from_bytes(d[0x3C82:0x3C84], "big")
        txs = []
        o = tbase
        for _ in range(8):
            st, ln, buf = int.from_bytes(d[o:o + 2], "big"), int.from_bytes(d[o + 2:o + 4], "big"), int.from_bytes(d[o + 4:o + 8], "big")
            if not st & 0x8000:
                break
            txs.append((o, st, ln, buf))
            if st & 0x2000:                              # W: last BD of the table
                break
            o += 8
        if not txs:
            return
        rd = lambda a, n: (bytes(self.dpram[a - IMMR:a - IMMR + n]) if IMMR <= a < IMMR + 0x10000
                           else bytes(self.uc.mem_read(a, n)))
        first = rd(txs[0][3], txs[0][2])
        dev = first[0] & 0xFE
        ee = self.eeproms.setdefault(dev, bytearray(b"\xff" * 256))
        if len(txs) >= 2 and rd(txs[1][3], 1)[0] & 1:     # write offset, then read
            off = first[1] if len(first) > 1 else 0
            n = txs[1][2] - 1
            data = bytes(ee[(off + i) & 0xFF] for i in range(n))
            r = rbase
            rbuf = int.from_bytes(d[r + 4:r + 8], "big")
            if IMMR <= rbuf < IMMR + 0x10000:
                self.dpram[rbuf - IMMR:rbuf - IMMR + n] = data
            else:
                self.uc.mem_write(rbuf, data)
            d[r + 2:r + 4] = n.to_bytes(2, "big")
            d[r:r + 2] = (int.from_bytes(d[r:r + 2], "big") & ~0x8000).to_bytes(2, "big")
        elif not first[0] & 1 and len(first) > 1:          # a write: [dev W, offset, data...]
            off = first[1]
            for i, v in enumerate(first[2:]):
                ee[(off + i) & 0xFF] = v
            self.events["eeprom writes"] = self.events.get("eeprom writes", 0) + 1
        for o, st, ln, buf in txs:
            d[o:o + 2] = (st & ~0x8000).to_bytes(2, "big")

    # ---- IMMR: the SIU interrupt controller and the PIT on top of cosim's model ----
    def _rd(self, uc, off, size, _):
        if off == 0x1C:                                  # SIVEC: the highest-priority pending, enabled source
            code = self._sivec()
            return code << 24 if size == 4 else (code << 8 if size == 2 else code)
        if off == 0x930:                                 # CIVR: vector of the highest pending CPM source (IACK)
            bit = self._cpm_highest()
            if bit is None:
                vn = 0
            else:
                vn = 31 - bit
                self._w32(0x94C, self._r32(0x94C) | (0x80000000 >> bit))      # in service
                self._w32(0x944, self._r32(0x944) & ~(0x80000000 >> bit))     # taken
            v = vn << 11                                 # VN in the top 5 bits of the 16-bit register
            return v << 16 if size == 4 else (v >> 8 if size == 1 else v)   # exc_external reads it with lbz
        if off in (0x964, 0x966) and self.ir_level is not None:   # PCDAT: PC13 is the IR receiver (input)
            v = super()._rd(uc, off, size, _)
            bit = 0x0004 if (off == 0x966 or size == 4) else None
            if bit is not None and size in (2, 4):
                v = (v & ~bit) | (bit if self.ir_level else 0)
            return v
        return super()._rd(uc, off, size, _)

    def _r32(self, off):
        return int.from_bytes(self.dpram[off:off + 4], "big")

    def _w32(self, off, v):
        self.dpram[off:off + 4] = (v & 0xFFFFFFFF).to_bytes(4, "big")

    def _cpm_highest(self):
        pend = self._r32(0x944) & self._r32(0x948) & ~self._r32(0x94C)
        for bit in range(32):
            if pend & (0x80000000 >> bit):
                return bit
        return None

    def _wr(self, uc, off, size, value, _):
        if off == 0x94C and size == 4:                    # CISR: writing 1 ends that source's service
            self._w32(0x94C, self._r32(0x94C) & ~value)
            return
        if off in (0xA10, 0xA30, 0xA50, 0xA70) and size == 2:   # SCCE1..4: writing 1 clears the event
            cur = int.from_bytes(self.dpram[off:off + 2], "big")
            self.dpram[off:off + 2] = (cur & ~value & 0xFFFF).to_bytes(2, "big")
            return
        if off in (0xA86, 0xA96) and size == 1:           # SMCE1..2
            self.dpram[off] &= ~value & 0xFF
            return
        if off == 0x240 and size == 2 and value & 0x80:   # PISCR: writing PS = 1 acknowledges the tick
            cur = int.from_bytes(self.dpram[0x240:0x242], "big")
            value = (value & ~0x80) | (cur & 0x80 & ~0x80)
        if self.in_app:              # cosim's model, without its log of register writes (that one is for the boot)
            self.dpram[off:off + size] = value.to_bytes(size, "big")
            if off == 0x86C and value == 0x81:
                self._i2c()
            return
        super()._wr(uc, off, size, value, _)

    def _sivec(self):
        pend = int.from_bytes(self.dpram[0x10:0x14], "big") & int.from_bytes(self.dpram[0x14:0x18], "big")
        for bit in range(16):                            # SIPEND bit 0 (MSB) = IRQ0, 1 = LVL0, 2 = IRQ1, ...
            if pend & (0x80000000 >> bit):
                return bit * 4
        return 0x3C

    # ---- special instructions over the app too ----
    def hook_app_specials(self):
        img = self.app
        n = 0
        raw = self.raw
        for i in range(0, min(len(img), CODE_END) - 3, 4):
            w = struct.unpack_from(">I", img, i)[0]
            op, xo = w >> 26, (w >> 1) & 0x3FF
            spr = ((w >> 16) & 0x1F) | (((w >> 11) & 0x1F) << 5)
            if op == 19 and xo == 50:                        # rfi
                raw.code_hook(self._rfi, i, i)
                n += 1
            elif op == 31 and (xo in (146, 83, 370, 371) or (xo in (467, 339) and spr not in (1, 8, 9))):
                if self.core.lib:
                    self.core.special_hook(i)                # mfmsr/mtmsr/EIE/EID/NRI in C, the rest here
                else:
                    raw.code_hook(self._special_app, i, i)
                n += 1
        return n

    def _special_app(self, addr):
        w = self.raw.word(addr)
        fn = self._specials.get(w)
        if fn is None:
            fn = self._specials[w] = self._special_fn(w)
        fn(addr)
        self.raw.set_reg(UC_PPC_REG_PC, addr + 4)
        self.core.skip_rest(addr)

    def _special_fn(self, w):
        """What the special instruction w does, as a function of its address (decoded once per instruction word)."""
        raw = self.raw
        xo, rs = (w >> 1) & 0x3FF, (w >> 21) & 0x1F
        spr = ((w >> 16) & 0x1F) | (((w >> 11) & 0x1F) << 5)
        r = UC_PPC_REG_0 + rs
        if xo == 467:                                        # mtspr
            if spr == SPR_EIE:
                def fn(addr):
                    self.msr |= EE | RI
            elif spr == SPR_EID:
                def fn(addr):
                    self.msr = (self.msr & ~EE) | RI
            elif spr == SPR_NRI:
                def fn(addr):
                    self.msr &= ~(EE | RI)
            else:
                def fn(addr):
                    self.spr[spr] = raw.reg(r)
        elif xo in (339, 371):                               # mfspr / mftb
            if spr in (SPR_TBL_R, 284):
                def fn(addr):
                    if IR_ISR[0] <= addr < IR_ISR[1]:
                        raw.set_reg(r, self.ir_tb)           # the remote's own edge timing (see ir_press)
                    else:
                        raw.set_reg(r, (self.now() // CPU_MIPS) & 0xFFFFFFFF)
            elif spr in (SPR_TBU_R, 285):
                def fn(addr):
                    raw.set_reg(r, (self.now() // CPU_MIPS) >> 32)
            elif spr == 568:
                def fn(addr):
                    raw.set_reg(r, 0)
            else:
                def fn(addr):
                    raw.set_reg(r, self.spr.get(spr, 0))
        elif xo == 146:                                      # mtmsr
            def fn(addr):
                self.msr = raw.reg(r)
        elif xo == 83:                                       # mfmsr
            def fn(addr):
                raw.set_reg(r, self.msr)
        else:                                                # tlbia
            def fn(addr):
                pass
        return fn

    def _rfi(self, addr):
        self.core.skip_rest(addr)
        self.msr = self.spr.get(SPR_SRR1, 0)
        ret = self.spr.get(SPR_SRR0, 0)
        # as on the CPU: an interrupt still pending when a handler returns is taken at once. Without this, only one
        # interrupt per slice gets in, and the lowest-priority CPM sources (SMC1) starve behind the PIT and IDMA.
        self.levels()
        pend = int.from_bytes(self.dpram[0x10:0x14], "big") & int.from_bytes(self.dpram[0x14:0x18], "big")
        if pend and self.msr & EE:
            self.spr[SPR_SRR0] = ret
            self.spr[SPR_SRR1] = self.msr
            self.msr &= ~(EE | PR | RI)
            ret = 0x500
            self.events["external interrupts"] = self.events.get("external interrupts", 0) + 1
        self.raw.set_reg(UC_PPC_REG_PC, ret)

    # ---- interrupts and time ----
    def now(self):
        """Simulated time, in instructions (CPU_MIPS per microsecond): executed plus skipped busy-waits."""
        return self.icount + self.skew

    def hook_busy_waits(self, S):
        """udelay(us) / delay_us(us) only pass time: skip them and advance the clock (same observable effect)."""
        from unicorn.ppc_const import UC_PPC_REG_3, UC_PPC_REG_LR

        def skip(uc, addr, size, _):
            self.skew += uc.reg_read(UC_PPC_REG_3) * CPU_MIPS
            self.events["busy-waits skipped"] = self.events.get("busy-waits skipped", 0) + 1
            uc.reg_write(UC_PPC_REG_PC, uc.reg_read(UC_PPC_REG_LR))
            self.core.skip_rest(addr)
        for n in ("udelay", "delay_us_maybe"):
            if n in S:
                self.uc.hook_add(UC_HOOK_CODE, skip, begin=S[n], end=S[n])

    def tick_idma(self):
        """IDMA channel 1 as the panel output: a ready descriptor completes by the next slice. The frame it sent
        (src, len) is captured; IDSR1 gets 'done', the CPM raises IDMA1 (CIPR bit 0x00200000, vector 0x15)."""
        bdp = struct.unpack(">I", bytes(self.uc.mem_read(0xBB038, 4)))[0]       # idma_bd_ptr
        if not bdp or not IMMR <= bdp < IMMR + 0x10000:
            return
        o = bdp - IMMR
        st = int.from_bytes(self.dpram[o:o + 2], "big")
        if not st & 0x8000:
            return
        length, src = int.from_bytes(self.dpram[o + 4:o + 8], "big"), int.from_bytes(self.dpram[o + 8:o + 12], "big")
        if src and length and src + length <= 0x8000000:
            self.frames.append(bytes(self.uc.mem_read(src, length)))
        self.dpram[o:o + 2] = (st & ~0x8000).to_bytes(2, "big")
        self.dpram[0x910] |= 0x01                                                   # IDSR1: done
        self._w32(0x944, self._r32(0x944) | 0x00200000)                             # CIPR: IDMA1
        self.events["panel frames"] = self.events.get("panel frames", 0) + 1

    def tick_cpm(self):
        """The CPM interrupt controller asserts its SIU level (CICR IRL) while a source is pending and enabled."""
        cicr = self._r32(0x940)
        lvl = (cicr >> 13) & 7
        on = bool(cicr & 0x80) and self._cpm_highest() is not None
        self._set_sipend(1 + 2 * lvl, on)

    # ---- serial channels (the CPM UARTs behind /dev/rs485, /dev/rs232) ----
    SERIAL_CHANS, CHAN_SIZE = 0xB9024, 0x78          # serial_chans[6] (symbols.tsv: struct serial_chan)
    # CIPR bit per minor: SCC1..4 = vectors 0x1E..0x1B, SMC1..2 = 0x04, 0x03; event register offset in IMMR
    SERIAL_CPM = {0: (0x1E, 0xA10, 2), 1: (0x1D, 0xA30, 2), 2: (0x1C, 0xA50, 2), 3: (0x1B, 0xA70, 2),
                  4: (0x04, 0xA86, 1), 5: (0x03, 0xA96, 1)}

    def _chan(self, minor):
        base = self.SERIAL_CHANS + minor * self.CHAN_SIZE
        w = struct.unpack(">8I", bytes(self.uc.mem_read(base, 32)))
        return {"open": w[0], "rx_bd": w[3], "tx_bd": w[4], "rx_buf_size": w[7]}

    def _serial_event(self, minor, bits):
        vn, off, size = self.SERIAL_CPM[minor]
        cur = int.from_bytes(self.dpram[off:off + size], "big")
        self.dpram[off:off + size] = (cur | bits).to_bytes(size, "big")
        self._w32(0x944, self._r32(0x944) | (1 << vn))                     # CIPR

    def serial_send(self, minor, data, at=0, at_frame=0):
        """Queue `data` to arrive on serial minor (1 = /dev/rs485) at instruction count `at` or later, and after
        `at_frame` panel frames (the same moment of the app's own timeline in two images that run at different
        speeds). It is delivered when the app has the channel open with an empty rx BD, one BD per call (a UART
        closes the buffer on the idle line after a frame)."""
        self.rx_queue.append(((at, at_frame), minor, bytes(data)))

    def tick_serial(self):
        if self.rx_queue and self.icount >= self.rx_queue[0][0][0] and len(self.frames) >= self.rx_queue[0][0][1]:
            at, minor, data = self.rx_queue[0]
            ch = self._chan(minor)
            if ch["open"] and ch["rx_bd"]:
                i = self.rx_idx.get(minor, 0)
                o = ch["rx_bd"] + 8 * i - IMMR
                st = int.from_bytes(self.dpram[o:o + 2], "big")
                if st & 0x8000:                                              # E: empty, ours to fill
                    buf = int.from_bytes(self.dpram[o + 4:o + 8], "big")
                    data = data[:ch["rx_buf_size"] or len(data)]
                    self.uc.mem_write(buf, data)
                    self.dpram[o + 2:o + 4] = len(data).to_bytes(2, "big")
                    self.dpram[o:o + 2] = (st & ~0x8000 & ~0x003F).to_bytes(2, "big")
                    self.rx_idx[minor] = 0 if st & 0x2000 else i + 1        # W: last BD of the ring
                    self._serial_event(minor, 0x01)                          # RX
                    self.rx_queue.pop(0)
                    self.events["serial rx"] = self.events.get("serial rx", 0) + 1
        for minor in range(6):
            ch = self._chan(minor)
            if not (ch["open"] and ch["tx_bd"]):
                continue
            i = self.tx_idx.get(minor, 0)
            o = ch["tx_bd"] + 8 * i - IMMR
            st = int.from_bytes(self.dpram[o:o + 2], "big")
            if st & 0x8000:                                                  # R: ready to send
                n = int.from_bytes(self.dpram[o + 2:o + 4], "big")
                buf = int.from_bytes(self.dpram[o + 4:o + 8], "big")
                self.serial_tx[minor] = self.serial_tx.get(minor, b"") + bytes(self.uc.mem_read(buf, n))
                self.dpram[o:o + 2] = (st & ~0x8000).to_bytes(2, "big")
                self.tx_idx[minor] = 0 if st & 0x2000 else i + 1                 # W: last BD of the ring
                self._serial_event(minor, 0x02)                              # TX
                self.events["serial tx"] = self.events.get("serial tx", 0) + 1

    def force_config_bytes(self, addr, data):
        """force_config_word for any run of bytes (e.g. a jackpot level entry)."""
        from unicorn import UC_HOOK_MEM_READ
        data = bytes(data)

        def on_read(uc, access, a, size, v, _):
            uc.mem_write(addr, data)
        self.uc.mem_write(addr, data)
        self.uc.hook_add(UC_HOOK_MEM_READ, on_read, begin=addr, end=addr + len(data) - 1)

    def force_config_word(self, addr, value):
        """Make every app read of the 32-bit config word at addr return value (e.g. cfg_protocol 0xDCDE4 = 8,
        Cougar): what a sign configured in its NVRAM would have. The same for both images in a comparison."""
        from unicorn import UC_HOOK_MEM_READ

        def on_read(uc, access, a, size, v, _):
            uc.mem_write(addr, value.to_bytes(4, "big"))
        self.uc.mem_write(addr, value.to_bytes(4, "big"))
        self.uc.hook_add(UC_HOOK_MEM_READ, on_read, begin=addr, end=addr + 3)

    # ---- the IR remote: pulse-distance frames on PC13, decoded by the app's own ir_edge_isr ----
    KEYMAP = b"1234567890 .ABCD}{][-!EF\r:?<>,GHIJKLMNOPQRSTUVWXYZabcdefghijklmn"   # 0xBB048

    @staticmethod
    def _ir_isr_model(bits, T98, T9c, Ta0, Ta4, Ta8):
        """The decoded code ir_edge_isr would pass on for these 12 bit times (Python copy of its state machine)."""
        code = 0x800
        for d in bits:
            if Ta0 < d:
                if d < Ta4 or Ta8 < d:
                    return None
                code |= 0x1000
            bit, code = code & 1, code >> 1
            if bit:
                return code
        return None

    def _ir_frame(self, ch):
        """Edges (level, timebase) for one remote frame of key ch, from the thresholds the app computed in
        ir_init_maybe (0xBB098..0xBB0A8): the start low >= T98, then rising edges T0 or T1 apart."""
        r = lambda a: struct.unpack(">I", bytes(self.uc.mem_read(a, 4)))[0]
        T98, T9c, Ta0, Ta4, Ta8 = (r(a) for a in (0xBB098, 0xBB09C, 0xBB0A0, 0xBB0A4, 0xBB0A8))
        t0, t1 = (T9c + Ta0) // 2, (Ta4 + Ta8) // 2
        want = (2 << 6) | self.KEYMAP.index(ch.encode() if isinstance(ch, str) else ch)
        for n in range(1 << 12):
            bits = [t1 if n >> i & 1 else t0 for i in range(12)]
            if self._ir_isr_model(bits, T98, T9c, Ta0, Ta4, Ta8) == want:
                break
        else:
            raise ValueError(f"no frame for key {ch!r}")
        t = self.ir_tb + 100000
        edges = [(0, t)]
        rise = t + T98 + T98 // 4
        edges.append((1, rise))
        for d in bits:
            edges.append((0, rise + d // 3))
            rise += d
            edges.append((1, rise))
        return edges

    IR_KEY_GAP_US = 300_000     # at least this long between key presses, as a person presses keys

    def ir_press(self, keys, at_frame=0):
        """Queue remote key presses (characters of KEYMAP, e.g. "!" Enter, "]" next) after at_frame panel frames,
        and at least IR_KEY_GAP_US of simulated time after the previous key. Each key is sent as two identical
        frames, as a held remote repeats (the app needs the repeat). The gap matters: the app treats the same code
        again within 19 ticks (190 ms) as the same held key, so presses timed by frames alone would merge or not
        depending on how fast the image draws."""
        for k in keys:
            self.ir_queue.append((at_frame, k))

    def tick_ir(self):
        if (not self.ir_edges and self.ir_queue and len(self.frames) >= self.ir_queue[0][0]
                and (self.ir_last is None or self.now() - self.ir_last >= self.IR_KEY_GAP_US * CPU_MIPS)):
            if not struct.unpack(">I", bytes(self.uc.mem_read(0xBB0AC, 4)))[0]:      # IR semaphore: driver ready?
                return
            k = self.ir_queue.pop(0)[1]
            self.ir_edges = self._ir_frame(k) + self._ir_frame(k)
            self.ir_last = self.now()
            self.ir_keys_sent += 1
            self.events["ir keys"] = self.events.get("ir keys", 0) + 1
        if self.ir_edges and not (self._r32(0x944) | self._r32(0x94C)) & 0x01000000:   # previous edge handled
            level, tb = self.ir_edges.pop(0)
            self.ir_level, self.ir_tb = level, tb
            self._w32(0x944, self._r32(0x944) | 0x01000000)                    # CIPR: PC13 edge (vector 0x18)

    def tick_devices(self):
        self.tick_idma()
        self.tick_serial()
        self.tick_ir()
        self.tick_cpm()
        piscr = int.from_bytes(self.dpram[0x240:0x242], "big")
        pitc = int.from_bytes(self.dpram[0x244:0x248], "big") >> 16
        if piscr & 0x1 and pitc:                             # PTE: the PIT runs
            # the tick period the firmware asked for: system_tick_init computes PITC from us-per-tick at 0xBDDFC
            us = struct.unpack(">I", bytes(self.uc.mem_read(0xBDDFC, 4)))[0] or 10000
            period = us * CPU_MIPS
            if self.pit_next is None:
                self.pit_next = self.now() + period
            if self.now() >= self.pit_next:
                self.pit_next += period
                self.dpram[0x240:0x242] = (piscr | 0x80).to_bytes(2, "big")   # PS
                self.events["pit"] = self.events.get("pit", 0) + 1
        pirq = piscr >> 8                                    # one-hot: 0x80 = level 0, 0x40 = level 1, ...
        lvl = next((n for n in range(8) if pirq & (0x80 >> n)), None)
        if lvl is not None:
            self._set_sipend(1 + 2 * lvl, bool(piscr & 0x80 and piscr & 0x4))   # PS and PIE: assert LVLn

    def levels(self):
        """SIPEND's internal levels as the sources drive them now: the CPM's (tick_cpm) and the PIT's (PS and PIE)."""
        self.tick_cpm()
        piscr = int.from_bytes(self.dpram[0x240:0x242], "big")
        pirq = piscr >> 8
        lvl = next((n for n in range(8) if pirq & (0x80 >> n)), None)
        if lvl is not None:
            self._set_sipend(1 + 2 * lvl, bool(piscr & 0x80 and piscr & 0x4))

    def _set_sipend(self, bit, on):
        v = int.from_bytes(self.dpram[0x10:0x14], "big")
        m = 0x80000000 >> bit
        v = v | m if on else v & ~m
        self.dpram[0x10:0x14] = v.to_bytes(4, "big")

    def maybe_interrupt(self):
        pend = int.from_bytes(self.dpram[0x10:0x14], "big") & int.from_bytes(self.dpram[0x14:0x18], "big")
        if pend and self.msr & EE:
            uc = self.uc
            self.spr[SPR_SRR0] = uc.reg_read(UC_PPC_REG_PC)
            self.spr[SPR_SRR1] = self.msr
            self.msr &= ~(EE | PR | RI)
            uc.reg_write(UC_PPC_REG_PC, 0x500)
            self.events["external interrupts"] = self.events.get("external interrupts", 0) + 1

    def run_app(self, max_insns, watch):
        """Run the app from PC 0 until a watched address is reached or max_insns pass."""
        uc = self.uc
        self.in_app = True
        hit = {}

        def on_watch(uc, addr, size, _):
            name = watch[addr]
            hit.setdefault(name, self.icount)
        for a in watch:
            uc.hook_add(UC_HOOK_CODE, on_watch, begin=a, end=a)
        pc = uc.reg_read(UC_PPC_REG_PC)
        while self.icount < max_insns:
            try:
                slice_ = SLICE_IR if self.ir_edges or (self.ir_queue and len(self.frames) >= self.ir_queue[0][0]) else SLICE
                self.emu(pc, slice_)
            except UcError as e:
                pc = uc.reg_read(UC_PPC_REG_PC)
                regs = " ".join(f"r{i}={uc.reg_read(UC_PPC_REG_0 + i):#x}" for i in (1, 3, 4, 5, 9, 10, 11))
                return f"emulation error {e} at pc={pc:#x} ({regs})", hit
            self.icount += slice_
            pc = uc.reg_read(UC_PPC_REG_PC)
            if hasattr(self, "profile"):
                self.profile[pc] = self.profile.get(pc, 0) + 1
                chain, sp = [pc], uc.reg_read(UC_PPC_REG_1)
                for _ in range(6):                               # PowerPC EABI back chain: [sp] -> caller frame, LR at +4
                    try:
                        nsp, lr = struct.unpack(">II", bytes(uc.mem_read(sp, 4)) + bytes(uc.mem_read(sp + 4, 4)))
                        nsp2 = nsp
                        lr = struct.unpack(">I", bytes(uc.mem_read(nsp2 + 4, 4)))[0] if nsp2 else 0
                    except Exception:
                        break
                    if not nsp or not 0 < lr < CODE_END:
                        break
                    chain.append(lr)
                    sp = nsp
                ex = struct.unpack(">I", bytes(uc.mem_read(0x45C4CC, 4)))[0]     # _Thread_Executing
                who = task_name(uc, ex) if ex else "-"
                self.stacks[(who,) + tuple(chain)] = self.stacks.get((who,) + tuple(chain), 0) + 1
            self.tick_devices()
            self.maybe_interrupt()
            pc = uc.reg_read(UC_PPC_REG_PC)
            if "stop" in hit:
                return "reached the stop point", hit
        return f"ran {max_insns} instructions; pc={pc:#x}", hit


def save_frames(frames, out, W=None, H=None):
    """The captured panel frames as PNGs (RGB555 big-endian; bit 15 is the driver's frame marker) and a contact sheet."""
    import numpy as np
    from PIL import Image
    os.makedirs(out, exist_ok=True)
    n = len(frames[0]) // 2
    if W is None:
        W, H = next(((w, n // w) for w in (80, 120, 160, 200, 320) if n % w == 0 and n // w in (16, 32, 48, 64, 80, 96)), (80, n // 80))
    imgs = []
    for k, f in enumerate(frames):
        a = np.frombuffer(f[:W * H * 2], ">u2").reshape(H, W) & 0x7FFF
        rgb = np.stack([((a >> 10) & 31) << 3, ((a >> 5) & 31) << 3, (a & 31) << 3], -1).astype(np.uint8)
        imgs.append(rgb)
    step = max(1, len(imgs) // 24)
    pick = imgs[::step][:24]
    sheet = np.concatenate([np.pad(i, ((1, 1), (0, 0), (0, 0)), constant_values=40) for i in pick])
    Image.fromarray(sheet).resize((sheet.shape[1] * 4, sheet.shape[0] * 4), Image.NEAREST).save(os.path.join(out, "sheet.png"))
    Image.fromarray(imgs[-1]).resize((W * 8, H * 8), Image.NEAREST).save(os.path.join(out, "last.png"))
    print(f"  saved {len(imgs)} frames ({W}x{H}) -> {out}/sheet.png, last.png")


def task_name(uc, tcb):
    """The 4-character RTEMS name of a thread (Objects_Control.name at TCB + 0xC)."""
    raw = bytes(uc.mem_read(tcb + 0xC, 4))
    return raw.decode("latin-1") if all(32 <= c < 127 for c in raw) else raw.hex()


def watch_switches(b, S):
    """Record every context switch: the heir's name (its context is TCB + 0xE0)."""
    from unicorn.ppc_const import UC_PPC_REG_4
    b.switches = []
    b.tasks = {}
    b.tcbs = {}

    def on_switch(uc, addr, size, _):
        tcb = uc.reg_read(UC_PPC_REG_4) - 0xE0
        name = task_name(uc, tcb)
        b.tcbs[name] = tcb
        b.tasks.setdefault(name, 0)
        b.tasks[name] += 1
        if len(b.switches) < 40:
            b.switches.append((b.icount, name))
    b.uc.hook_add(UC_HOOK_CODE, on_switch, begin=S["cpu_context_switch"], end=S["cpu_context_switch"])


def main():
    app_path = sys.argv[1] if len(sys.argv) > 1 else os.path.join(REPO, "analysis", "extracted", "ELITE_RGB")
    flash = open(os.path.join(REPO, "dumps", "bootflash.bin"), "rb").read()
    app = open(app_path, "rb").read()
    sc = dict(cosim.SCENARIOS["fixed config, tries ff (normal boot)"] if "fixed config, tries ff (normal boot)" in cosim.SCENARIOS
              else next(iter(cosim.SCENARIOS.values())))
    b = AppBoard(flash, sc, app)
    b.run()
    if not b.reached_app:
        raise SystemExit("the boot ROM did not reach the app")
    # the boot ROM loaded the stock app from the dump's archive; run the chosen image instead, keeping what the boot
    # ROM legitimately wrote into it: the hand-off block (0x14..0x9C) and the MMU table (pointer/length at 0x4C/0x50)
    ram = bytes(b.uc.mem_read(0, len(app)))
    tp, tl = struct.unpack_from(">II", ram, 0x4C)
    img = bytearray(app)
    for lo, hi in ((0x14, 0x9C), (tp, tp + tl)):
        img[lo:hi] = ram[lo:hi]
    b.uc.mem_write(0, bytes(img))
    print(f"boot ROM -> app at 0 (as cosim); running {os.path.relpath(app_path, REPO)}")
    print("app special-instruction sites hooked:", b.hook_app_specials())
    S = sym()
    b.hook_busy_waits(S)
    watch = {S[n]: n for n in ("boot_card", "rtems_initialize_executive_early", "boot_card_start_multitasking",
                               "thread_start_multitasking", "cpu_context_switch", "system_tick_isr_maybe",
                               "rtems_clock_tick") if n in S}
    budget = int(os.environ.get("SIM_INSNS", "0"))
    if budget:                                        # milestone 2+: keep running, watch the tasks
        watch_switches(b, S)
        b.profile = {}
        b.stacks = {}
    else:
        watch[S["cpu_context_switch"]] = "stop"
    why, hit = b.run_app(budget or 400_000_000, watch)
    print(why, f"after {b.icount:,} instructions")
    for name, ic in sorted(hit.items(), key=lambda kv: kv[1]):
        print(f"  reached {name:34s} at instruction {ic:,}")
    if b.frames:
        from collections import Counter
        print("  frame lengths (bytes):", dict(Counter(len(f) for f in b.frames)))
        common = Counter(len(f) for f in b.frames).most_common(1)[0][0]
        save_frames([f for f in b.frames if len(f) == common], os.path.join(REPO, "sim", "out"))
    print("  events:", b.events, f"; simulated time {b.now() / CPU_MIPS / 1e6:.3f} s, tick "
          f"{struct.unpack('>I', bytes(b.uc.mem_read(0xBDDFC, 4)))[0]} us")
    if budget:
        d = b.dpram
        print(f"  MSR {b.msr:#x} (EE {'on' if b.msr & EE else 'off'}); PISCR {int.from_bytes(d[0x240:0x242], 'big'):#06x} "
              f"PITC {int.from_bytes(d[0x244:0x248], 'big'):#010x}; SIPEND {int.from_bytes(d[0x10:0x14], 'big'):#010x} "
              f"SIMASK {int.from_bytes(d[0x14:0x18], 'big'):#010x}; pc {b.uc.reg_read(UC_PPC_REG_PC):#x}")
        print(f"  CPM: CICR {b._r32(0x940):#010x} CIPR {b._r32(0x944):#010x} CIMR {b._r32(0x948):#010x} "
              f"CISR {b._r32(0x94C):#010x}; IDSR1 {b.dpram[0x910]:#04x}; PCINT {int.from_bytes(b.dpram[0x968:0x96A], 'big'):#06x} "
              f"PCSO {int.from_bytes(b.dpram[0x964:0x966], 'big'):#06x}")
        print("  context switches per task:", b.tasks)
        starts = sorted((a, n) for n, a in S.items())
        import bisect
        fn = {}
        for pc, k in b.profile.items():
            i = bisect.bisect_right(starts, (pc, "~")) - 1
            nm = starts[i][1] if i >= 0 else "?"
            fn[nm] = fn.get(nm, 0) + k
        print("  where each task is parked (from its saved context):")
        for name, tcb in b.tcbs.items():
            ctx = tcb + 0xE0
            sp = struct.unpack(">I", bytes(b.uc.mem_read(ctx, 4)))[0]
            lr = struct.unpack(">I", bytes(b.uc.mem_read(ctx + 0x58, 4)))[0]
            chain = [lr]
            for _ in range(8):
                try:
                    nsp = struct.unpack(">I", bytes(b.uc.mem_read(sp, 4)))[0]
                    lr2 = struct.unpack(">I", bytes(b.uc.mem_read(nsp + 4, 4)))[0] if nsp else 0
                except Exception:
                    break
                if not nsp or not 0 < lr2 < CODE_END:
                    break
                chain.append(lr2)
                sp = nsp
            names = []
            for a in chain:
                i = bisect.bisect_right(starts, (a, "~")) - 1
                n = starts[i][1] if i >= 0 else "?"
                if not names or names[-1] != n:
                    names.append(n)
            print(f"    {name:9s} " + " < ".join(names))
        agg = {}
        for ch, k in b.stacks.items():
            names = [ch[0] + ":"]
            for a in ch[1:]:
                i = bisect.bisect_right(starts, (a, "~")) - 1
                n = starts[i][1] if i >= 0 else "?"
                if not names or names[-1] != n:
                    names.append(n)
            key = " < ".join(names[:5])
            agg[key] = agg.get(key, 0) + k
        for key, k in sorted(agg.items(), key=lambda kv: -kv[1])[:8]:
            print(f"  {k:5}  {key}")
        print("  where the time went:", ", ".join(f"{n} {k}" for n, k in sorted(fn.items(), key=lambda kv: -kv[1])[:10]))
        print("  first switches:", " ".join(f"{n}" for _, n in b.switches[:30]))
    return 0 if ("stop" in hit or budget) else 1


if __name__ == "__main__":
    sys.exit(main())
