#!/usr/bin/env python3
"""Recursive-descent control-flow map of BOOTSTRAP V2.00 (linked at 0xF8180000, reset entry at +0x100).
Follows b/bl/bc, and lis+ori/addi -> mtlr/mtctr computed targets. Prints functions (bl targets) and code ranges."""
import sys, struct
from capstone import Cs, CS_ARCH_PPC, CS_MODE_32, CS_MODE_BIG_ENDIAN
BASE = 0xF8180000
img = open(sys.argv[1], "rb").read()
END = struct.unpack(">I", img[4:8])[0]          # archive offset = end of code (0x2680)
md = Cs(CS_ARCH_PPC, CS_MODE_32 | CS_MODE_BIG_ENDIAN); md.detail = False
def ins_at(off):
    l = list(md.disasm(img[off:off+4], BASE + off)); return l[0] if l else None
code, funcs, work = set(), {0x100}, [0x100]
calls = {}
while work:
    off = work.pop()
    regs = {}
    while 0x100 <= off < END and off not in code:
        i = ins_at(off)
        if i is None: break
        code.add(off); w = struct.unpack(">I", img[off:off+4])[0]; op = w >> 26
        mn, ops = i.mnemonic, i.op_str
        # track lis/ori/addi constants for computed jumps
        if mn == "lis": r, v = ops.split(", "); regs[r] = (int(v, 0) << 16) & 0xffffffff
        elif mn in ("ori", "addi") and ops.count(",") == 2:
            d, s, v = ops.split(", ")
            if s in regs:
                v = int(v, 0); regs[d] = (regs[s] | (v & 0xffff)) if mn == "ori" else (regs[s] + v) & 0xffffffff
        elif mn in ("mtlr", "mtctr") and ops in regs:
            t = regs[ops] - BASE
            if 0x100 <= t < END and t not in code: work.append(t); funcs.add(t)
        if op == 18:                                   # b / bl
            li = w & 0x03FFFFFC
            if li & 0x02000000: li -= 0x04000000
            t = (li if w & 2 else off + li)
            if w & 1:
                funcs.add(t); calls.setdefault(off, t); work.append(t)
            else:
                work.append(t); break
        elif op == 16:                                 # bc
            bd = w & 0xFFFC
            if bd & 0x8000: bd -= 0x10000
            work.append(off + bd if not (w & 2) else bd)
            if w & 1: funcs.add(off + bd)
        if mn in ("blr", "bctr", "rfi") or (op == 19 and ((w >> 1) & 0x3ff) in (16, 528) and ((w >> 21) & 0x14) == 0x14):
            break
        off += 4
code_bytes = len(code) * 4
print(f"code region 0x100..{END:#x} ({END-0x100} bytes); reachable code: {code_bytes} bytes ({100*code_bytes/(END-0x100):.0f}%)")
print(f"functions (call/jump targets): {len(funcs)}")
fl = sorted(funcs)
for f in fl: print(f"  {BASE+f:#010x}  (+{f:#06x})")
# data gaps
gaps, prev = [], None
for o in range(0x100, END, 4):
    if o not in code:
        if prev is None: prev = o
    elif prev is not None: gaps.append((prev, o)); prev = None
if prev is not None: gaps.append((prev, END))
print("non-code (data) ranges:", ", ".join(f"+{a:#x}..+{b:#x}({b-a})" for a, b in gaps if b - a >= 8))
