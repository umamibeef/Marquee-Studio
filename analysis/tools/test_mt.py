#!/usr/bin/env python3
"""/dev/random is MT19937 (1998 reference, sgenrand with 69069): run the firmware's mt_sgenrand (0x60334) and
mt_genrand (0x60388) in Unicorn and compare 2000 outputs with a Python copy of the reference.
Usage: test_mt.py"""
import os, sys
from unicorn import Uc, UC_ARCH_PPC, UC_MODE_PPC32, UC_MODE_BIG_ENDIAN
from unicorn.ppc_const import UC_PPC_REG_1, UC_PPC_REG_3, UC_PPC_REG_LR

HERE = os.path.dirname(os.path.abspath(__file__))
uc = Uc(UC_ARCH_PPC, UC_MODE_PPC32 | UC_MODE_BIG_ENDIAN)
uc.mem_map(0, 0x1000000)
uc.mem_write(0, open(os.path.join(HERE, "..", "extracted", "ELITE_RGB"), "rb").read())


def call(a, *r):
    for i, v in enumerate(r):
        uc.reg_write(UC_PPC_REG_3 + i, v)
    uc.reg_write(UC_PPC_REG_1, 0xF00000)
    uc.reg_write(UC_PPC_REG_LR, 0xFFFF00)
    uc.emu_start(a, 0xFFFF00, count=10_000_000)
    return uc.reg_read(UC_PPC_REG_3)


def reference(seed, n):
    N, M = 624, 397
    mt = [seed & 0xFFFFFFFF]
    for i in range(1, N):
        mt.append(69069 * mt[i - 1] & 0xFFFFFFFF)
    out, mti = [], N
    for _ in range(n):
        if mti >= N:
            for k in range(N):
                y = (mt[k] & 0x80000000) | (mt[(k + 1) % N] & 0x7FFFFFFF)
                mt[k] = mt[(k + M) % N] ^ (y >> 1) ^ (0x9908B0DF if y & 1 else 0)
            mti = 0
        y = mt[mti]
        mti += 1
        y ^= y >> 11
        y ^= (y << 7) & 0x9D2C5680
        y ^= (y << 15) & 0xEFC60000
        out.append(y ^ (y >> 18))
    return out


ok = True
for seed in (4357, 0x12345678):
    call(0x60334, seed)
    fw = [call(0x60388) for _ in range(2000)]
    good = fw == reference(seed, 2000)
    ok &= good
    print(f"  {'ok  ' if good else 'FAIL'} seed {seed:#x}: 2000 outputs == MT19937 reference")
print("ALL MT19937 CHECKS PASS" if ok else "FAILURES")
sys.exit(0 if ok else 1)
