#!/usr/bin/env python3
"""Identify the libgcc soft-float / 64-bit helpers by running them (Unicorn, PPC32 big-endian).

Each candidate in the libgcc part of the image is called with known arguments in the EABI registers
(a double in r3:r4, a second in r5:r6; or ints in r3, r4) and its result is matched against what
__adddf3, __muldf3, __floatsidf, __fixdfsi, __udivdi3, ... would return.
Usage: softfloat_id.py [START END]   (hex; default the 0x93000-0x98000 helper area)
"""
import math, os, re, struct, sys
from unicorn import Uc, UC_ARCH_PPC, UC_MODE_PPC32, UC_MODE_BIG_ENDIAN, UC_HOOK_CODE, UcError
from unicorn.ppc_const import *

HERE = os.path.dirname(os.path.abspath(__file__))
APP = open(os.path.join(HERE, "..", "extracted", "ELITE_RGB"), "rb").read()
RAM, STACK, STOP = 0x1000000, 0xF00000, 0xFFFF00


def d2w(x):
    return struct.unpack(">II", struct.pack(">d", x))


def w2d(hi, lo):
    return struct.unpack(">d", struct.pack(">II", hi, lo))[0]


def f2w(x):
    return struct.unpack(">I", struct.pack(">f", x))[0]


def w2f(w):
    return struct.unpack(">f", struct.pack(">I", w))[0]


def call(addr, regs):
    uc = Uc(UC_ARCH_PPC, UC_MODE_PPC32 | UC_MODE_BIG_ENDIAN)
    uc.mem_map(0, RAM)
    uc.mem_write(0, APP)
    for i, v in enumerate(regs):
        uc.reg_write(UC_PPC_REG_3 + i, v & 0xFFFFFFFF)
    uc.reg_write(UC_PPC_REG_1, STACK)
    uc.reg_write(UC_PPC_REG_LR, STOP)
    try:
        uc.emu_start(addr, STOP, count=200000)
    except UcError:
        return None
    if uc.reg_read(UC_PPC_REG_PC) != STOP:
        return None
    return uc.reg_read(UC_PPC_REG_3), uc.reg_read(UC_PPC_REG_4)


# (name, args, check(r3, r4) -> bool)
A, B = d2w(3.0), d2w(2.0)
TESTS = [
    ("__adddf3", [*A, *B], lambda h, l: w2d(h, l) == 5.0),
    ("__subdf3", [*A, *B], lambda h, l: w2d(h, l) == 1.0),
    ("__muldf3", [*A, *B], lambda h, l: w2d(h, l) == 6.0),
    ("__divdf3", [*A, *B], lambda h, l: w2d(h, l) == 1.5),
    ("__negdf2", [*A], lambda h, l: w2d(h, l) == -3.0),
    ("__floatsidf", [7], lambda h, l: w2d(h, l) == 7.0),
    ("__floatunsidf", [0x80000000], lambda h, l: w2d(h, l) == 2147483648.0),
    ("__fixdfsi", [*d2w(-7.75)], lambda h, l: h == (-7) & 0xFFFFFFFF),
    ("__fixunsdfsi", [*d2w(3e9)], lambda h, l: h == 3000000000),
    ("__extendsfdf2", [f2w(1.5)], lambda h, l: w2d(h, l) == 1.5),
    ("__truncdfsf2", [*d2w(2.5)], lambda h, l: w2f(h) == 2.5),
    ("__addsf3", [f2w(3.0), f2w(2.0)], lambda h, l: w2f(h) == 5.0),
    ("__subsf3", [f2w(3.0), f2w(2.0)], lambda h, l: w2f(h) == 1.0),
    ("__mulsf3", [f2w(3.0), f2w(2.0)], lambda h, l: w2f(h) == 6.0),
    ("__divsf3", [f2w(3.0), f2w(2.0)], lambda h, l: w2f(h) == 1.5),
    ("__floatsisf", [7], lambda h, l: w2f(h) == 7.0),
    ("__fixsfsi", [f2w(-7.75)], lambda h, l: h == (-7) & 0xFFFFFFFF),
    ("__udivdi3", [0, 100, 0, 7], lambda h, l: (h, l) == (0, 14)),
    ("__umoddi3", [0, 100, 0, 7], lambda h, l: (h, l) == (0, 2)),
    ("__divdi3", [0xFFFFFFFF, (-100) & 0xFFFFFFFF, 0, 7], lambda h, l: (h, l) == (0xFFFFFFFF, (-14) & 0xFFFFFFFF)),
    ("__moddi3", [0xFFFFFFFF, (-100) & 0xFFFFFFFF, 0, 7], lambda h, l: (h, l) == (0xFFFFFFFF, (-2) & 0xFFFFFFFF)),
    ("sqrt", [*d2w(16.0)], lambda h, l: w2d(h, l) == 4.0),
]
# comparisons: (a, b) -> <0 / 0 / >0
CMP = [("__cmpdf2/__eqdf2/__nedf2-like", "d"), ("__cmpsf2-like", "f")]


def cmp_kind(addr):
    out = {}
    for kind, pack in (("d", lambda x: list(d2w(x))), ("f", lambda x: [f2w(x)])):
        rs = []
        for a, b in ((1.0, 2.0), (2.0, 2.0), (3.0, 2.0)):
            r = call(addr, pack(a) + pack(b))
            if r is None:
                break
            v = r[0] - (1 << 32) if r[0] & 0x80000000 else r[0]
            rs.append(v)
        if len(rs) == 3:
            out[kind] = rs
    for kind, rs in out.items():
        lt, eq, gt = rs
        t = "df" if kind == "d" else "sf"
        if lt < 0 and eq == 0 and gt > 0:
            return f"__cmp{t}2 / __lt{t}2 / __gt{t}2 family (<0, 0, >0)"
        if lt == 1 and eq == 0 and gt == 1:
            return f"__ne{t}2 (!=)"
        if lt != 0 and eq == 0 and gt != 0:
            return f"__eq{t}2/__ne{t}2 family (0 when equal)"
    return None


def main():
    lo, hi = (int(sys.argv[1], 16), int(sys.argv[2], 16)) if len(sys.argv) > 2 else (0x93000, 0x98000)
    fns = []
    for line in open(os.path.join(HERE, "..", "ghidra_out", "elite_rgb_named.functions.txt")):
        a, n = line.split()[:2]
        a = int(a, 16)
        if lo <= a < hi:
            fns.append((a, n))
    for a, n in fns:
        hits = [name for name, args, ok in TESTS if (lambda r: r is not None and ok(*r))(call(a, args))]
        if not hits:
            k = cmp_kind(a)
            hits = [k] if k else []
        if hits:
            print(f"{a:08x} {n:24} {' / '.join(hits)}")


if __name__ == "__main__":
    main()
