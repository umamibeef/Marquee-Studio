#!/usr/bin/env python3
"""How many register arguments does a function really take? Reads the machine code (capstone) and finds which of
r3..r10 are read before they are written on some path from the entry (EABI: arguments in r3..r10). A call or tail
call reads the argument registers its callee takes (computed the same way, recursively). Compares the result with
the prototype Ghidra exported (ghidra_out/elite_rgb_named.h), which is what the compile check sees.

Usage: argregs.py ADDR|NAME ...        one line per function: real register args vs prototype
       argregs.py --mismatch [LO HI]   every function whose prototype has a different count (hex range)
Limits: indirect calls (bctrl) are taken to read nothing; a parameter only passed on to a callee whose own
count is wrong inherits the error. Treat the output as evidence to check, not a verdict.
"""
import os, re, sys
from capstone import Cs, CS_ARCH_PPC, CS_MODE_32, CS_MODE_BIG_ENDIAN

HERE = os.path.dirname(os.path.abspath(__file__))
OUT = os.path.join(HERE, "..", "ghidra_out")
IMG = open(os.path.join(HERE, "..", "extracted", "ELITE_RGB"), "rb").read()
md = Cs(CS_ARCH_PPC, CS_MODE_32 | CS_MODE_BIG_ENDIAN)

FUNCS = {}                                    # addr -> (name, size)
for l in open(os.path.join(OUT, "elite_rgb_named.functions.txt")):
    m = re.match(r"([0-9a-f]{8})\s+(\S+)\s+\((\d+) bytes\)", l)
    if m:
        FUNCS[int(m.group(1), 16)] = (m.group(2), int(m.group(3)))
BY_NAME = {n: a for a, (n, _) in FUNCS.items()}

ARGS = [f"r{i}" for i in range(3, 11)]
READ_ALL = ("st", "cmp", "tw", "mt", "dcb", "icb", "tlb")
NO_GPR = ("cr", "mcrf", "sync", "isync", "eieio", "rfi", "sc", "nop", "blr", "bctr", "mfcr")


def regs(op):
    return re.findall(r"\br(\d+)\b", op)


def effects(ins):
    """(reads, writes) as sets of 'rN' (gprs only)."""
    m, op = ins.mnemonic, ins.op_str
    rs = ["r" + r for r in regs(op)]
    if m.startswith(NO_GPR) or not rs:
        return set(), set()
    if m == "lmw":
        return set(rs[1:]), {f"r{i}" for i in range(int(rs[0][1:]), 32)}
    if m == "stmw":
        return {f"r{i}" for i in range(int(rs[0][1:]), 32)} | set(rs[1:]), set()
    if m.startswith(READ_ALL):
        w = {rs[-1]} if m.startswith("st") and m.rstrip(".").endswith(("u", "ux")) else set()
        return set(rs), w
    if m.startswith(("mf", "li", "lis")):
        return set(rs[1:]), {rs[0]}
    w = {rs[0]}
    if m.startswith("l") and m.rstrip(".").endswith(("u", "ux")):
        w.add(rs[-1])
    return set(rs[1:]), w


_memo, _busy = {}, set()
DEBUG = "-v" in sys.argv


def params(addr):
    """Sorted list of r3..r10 read before written (the register arguments), or None if not decodable."""
    if addr in _memo:
        return _memo[addr]
    if addr in _busy or addr not in FUNCS:
        return []
    _busy.add(addr)
    name, size = FUNCS[addr]
    code = {i.address: i for i in md.disasm(IMG[addr:addr + size], addr)}
    used, seen = set(), {}
    work = [(addr, frozenset())]
    while work:
        pc, written = work.pop()
        while pc in code:
            if pc in seen and seen[pc] <= written:          # reached before with fewer writes: nothing new
                break
            seen[pc] = written if pc not in seen else seen[pc] & written
            written = seen[pc]
            ins = code[pc]
            m = ins.mnemonic
            r, w = effects(ins)
            if m in ("bl", "bla"):
                t = int(ins.op_str, 16)
                if t == pc + 4:                                 # PIC idiom: bl to the next instruction (gets LR)
                    pc += 4
                    continue
                new = {x for x in (params(t) or []) if x not in written}
                if new and DEBUG: print(f"  {name} {pc:#x}: call {t:#x} reads {sorted(new)}")
                used |= new
                written = written | {f"r{i}" for i in range(3, 13)}
                pc += 4
                continue
            new = {x for x in r if x in ARGS and x not in written}
            if new and DEBUG: print(f"  {name} {pc:#x}: {m} {ins.op_str} reads {sorted(new)}")
            used |= new
            written = written | w
            if m == "b":
                t = int(ins.op_str, 16)
                if addr <= t < addr + size:
                    pc = t
                    continue
                used |= {x for x in (params(t) or []) if x not in written}   # tail call
                break
            if m.startswith(("blr", "bctr", "rfi")) and not m.startswith(("blrl", "bctrl")):
                if m in ("blr", "bctr", "rfi"):
                    break
                pc += 4                                     # conditional return: fall through too
                continue
            if m.startswith("b") and not m.startswith(("bl", "bctrl")):
                t = re.search(r"0x([0-9a-f]+)$", ins.op_str)
                if t:
                    t = int(t.group(1), 16)
                    if addr <= t < addr + size:
                        work.append((t, written))
                pc += 4
                continue
            if m in ("bctrl", "blrl"):
                written = written | {f"r{i}" for i in range(3, 13)}
            pc += 4
    _busy.discard(addr)
    res = sorted(used, key=lambda x: int(x[1:]))
    _memo[addr] = res
    return res


def proto_counts():
    h = open(os.path.join(OUT, "elite_rgb_named.h")).read()
    res = {}
    for m in re.finditer(r"^[^\n;(]*?\b(\w+)\s*\(([^;]*?)\);", h, re.M):
        args = m.group(2).strip()
        n = 0 if args in ("", "void") else len([a for a in args.split(",") if a.strip() != "..."])
        res[m.group(1)] = (n, "..." in args)
    return res


def count(p):
    """Argument count: the highest register of the run from r3 that has no gap of two or more registers
    (r3, r5 -> 3: r4 is then an unused parameter). Registers read further up are usually a variable read
    uninitialised on an impossible path (e.g. a switch with no default), not a parameter."""
    have = {int(x[1:]) for x in p}
    n, miss = 0, 0
    for r in range(3, 11):
        if r in have:
            n, miss = r - 2, 0
        else:
            miss += 1
            if miss == 2:
                break
    return n


def main():
    a = [x for x in sys.argv[1:] if x != "-v"]
    pc = proto_counts()
    if a and a[0] == "--mismatch":
        lo, hi = (int(a[1], 16), int(a[2], 16)) if len(a) > 2 else (0, 1 << 32)
        for addr in sorted(FUNCS):
            if lo <= addr < hi:
                name = FUNCS[addr][0]
                p = params(addr)
                n, va = pc.get(name, (None, False))
                if n is not None and not va and count(p) != n:
                    print(f"{addr:08x} {name:40} regs {count(p)} ({','.join(p)})  prototype {n}")
        return
    for x in a:
        addr = BY_NAME[x] if x in BY_NAME else int(x, 16)
        name = FUNCS[addr][0]
        p = params(addr)
        print(f"{addr:08x} {name:40} regs {count(p)} ({','.join(p)})  prototype {pc.get(name)}")


if __name__ == "__main__":
    main()
