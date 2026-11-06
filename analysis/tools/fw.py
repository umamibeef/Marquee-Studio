#!/usr/bin/env python3
"""Call ELITE_RGB firmware functions from Python (Unicorn, PPC32 big-endian, EABI calling convention).

    from fw import FW
    f = FW()
    f.call("toupper", ord("a"))                 # -> 0x41
    sb = f.sbuf("hello", cap=32)                # an sbuf {data, cap, len} in emulated RAM
    f.call("sbuf_pad_left", sb, 8, ord(" ")); f.sbuf_text(sb)

Functions are looked up by name in symbols.tsv (or pass an address). Arguments are ints (r3..r10). RTOS calls
(semaphores, events, task delays) are stubbed to return 0 so leaf code can run outside RTEMS; anything else runs
for real. A call that does not come back within `limit` instructions raises.
"""
import os, re, struct
from unicorn import Uc, UcError, UC_ARCH_PPC, UC_MODE_PPC32, UC_MODE_BIG_ENDIAN, UC_HOOK_CODE
from unicorn.ppc_const import UC_PPC_REG_1, UC_PPC_REG_3, UC_PPC_REG_4, UC_PPC_REG_LR, UC_PPC_REG_PC

HERE = os.path.dirname(os.path.abspath(__file__))
APP = os.path.join(HERE, "..", "extracted", "ELITE_RGB")
RAM = 0x2000000                       # 32 MB: image, BSS (to 0x500000), heap
SCRATCH, SCRATCH_END = 0x1000000, 0x1E00000
STACK, STOP = 0x1F00000, 0x1FFFF00
IMMR = 0xFF000000                     # MPC8xx internal registers (mapped, reads 0)
STUB_RETURN_0 = ("rtems_semaphore_obtain", "rtems_semaphore_release", "rtems_event_send",
                 "rtems_task_wake_after", "rtems_semaphore_create")


def symbols():
    names = {}
    for line in open(os.path.join(HERE, "..", "symbols.tsv")):
        c = line.rstrip("\n").split("\t")
        if len(c) > 2 and re.fullmatch(r"[0-9a-f]{8}", c[0]):
            names[c[2]] = int(c[0], 16)
            if c[3] == "probable" and not c[2].endswith("_maybe"):
                names[c[2] + "_maybe"] = int(c[0], 16)
    return names


SYM = symbols()


class FW:
    def __init__(self, image=None):
        self.image = open(APP, "rb").read() if image is None else image
        uc = self.uc = Uc(UC_ARCH_PPC, UC_MODE_PPC32 | UC_MODE_BIG_ENDIAN)
        uc.mem_map(0, RAM)
        uc.mem_write(0, self.image)
        uc.mem_map(IMMR, 0x10000)
        self.next = SCRATCH
        self.stubs = {}
        for n in STUB_RETURN_0:
            if n in SYM:
                self.stub(n, 0)

    def stub(self, fn, value=0):
        """Make fn return `value` at once (for hardware reads the emulator does not model, RTOS calls, ...)."""
        a = self.addr(fn)
        if a not in self.stubs:
            self.uc.hook_add(UC_HOOK_CODE, self._stub, begin=a, end=a)
        self.stubs[a] = value

    def _stub(self, uc, addr, size, _):
        if addr in self.stubs:
            uc.reg_write(UC_PPC_REG_3, self.stubs[addr] & 0xFFFFFFFF)
            uc.reg_write(UC_PPC_REG_PC, uc.reg_read(UC_PPC_REG_LR))

    # ---- memory helpers ----
    def alloc(self, n, align=8):
        self.next = (self.next + align - 1) & ~(align - 1)
        a = self.next
        self.next += n
        assert self.next < SCRATCH_END, "scratch exhausted"
        self.uc.mem_write(a, bytes(n))
        return a

    def write(self, addr, data):
        self.uc.mem_write(addr, bytes(data))

    def read(self, addr, n):
        return bytes(self.uc.mem_read(addr, n))

    def u32(self, addr, v=None):
        if v is None:
            return struct.unpack(">I", self.read(addr, 4))[0]
        self.write(addr, struct.pack(">I", v & 0xFFFFFFFF))

    def u16(self, addr, v=None):
        if v is None:
            return struct.unpack(">H", self.read(addr, 2))[0]
        self.write(addr, struct.pack(">H", v & 0xFFFF))

    def bytes_(self, data):
        a = self.alloc(len(data) + 1)
        self.write(a, data)
        return a

    def cstr(self, s, size=None):
        data = s.encode("latin1") if isinstance(s, str) else s
        a = self.alloc(max(size or 0, len(data) + 1))
        self.write(a, data)
        return a

    def read_cstr(self, addr, limit=4096):
        d = self.read(addr, limit)
        return d[:d.index(b"\0")].decode("latin1") if b"\0" in d else d.decode("latin1")

    def sbuf(self, text="", cap=64):
        """struct sbuf {char *data; u16 cap; u16 len;} over a fresh buffer of cap bytes."""
        data = text.encode("latin1") if isinstance(text, str) else text
        buf = self.alloc(cap + 1)
        self.write(buf, data[:cap])
        sb = self.alloc(8)
        self.write(sb, struct.pack(">IHH", buf, cap, min(len(data), cap)))
        return sb

    def sbuf_text(self, sb):
        data, cap, n = struct.unpack(">IHH", self.read(sb, 8))
        return self.read(data, n).decode("latin1")

    # ---- calls ----
    def addr(self, fn):
        return fn if isinstance(fn, int) else SYM[fn]

    def call(self, fn, *args, limit=5_000_000, r4=False):
        uc = self.uc
        assert len(args) <= 8, "only register arguments (r3..r10)"
        for i, v in enumerate(args):
            uc.reg_write(UC_PPC_REG_3 + i, v & 0xFFFFFFFF)
        uc.reg_write(UC_PPC_REG_1, STACK)
        uc.reg_write(UC_PPC_REG_LR, STOP)
        try:
            uc.emu_start(self.addr(fn), STOP, count=limit)
        except UcError as e:
            raise RuntimeError(f"{fn}: {e} at {uc.reg_read(UC_PPC_REG_PC):#x}") from None
        if uc.reg_read(UC_PPC_REG_PC) != STOP:
            raise RuntimeError(f"{fn}: did not return within {limit} instructions (pc {uc.reg_read(UC_PPC_REG_PC):#x})")
        r = uc.reg_read(UC_PPC_REG_3)
        return (r, uc.reg_read(UC_PPC_REG_4)) if r4 else r

    def call_s(self, fn, *args, **kw):
        """call() with the result as a signed 32-bit int."""
        r = self.call(fn, *args, **kw)
        return r - (1 << 32) if r & 0x80000000 else r
