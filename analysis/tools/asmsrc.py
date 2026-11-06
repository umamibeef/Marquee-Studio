#!/usr/bin/env python3
"""Assembly source for ELITE_RGB's code: disassemble, reassemble with LLVM, and prove it byte-identical.

The parts of the firmware we do not (yet) build from C are built from this assembly source (docs/REBUILD-PLAN.md):
the hand-written assembly ([asm] functions: vectors, context switch, alignment emulator, ...) and, as a stopgap,
functions not yet proven in C. Every branch target becomes a label (`L_<addr>`), so the source is relocatable text
rather than raw words; an instruction LLVM cannot reproduce exactly is kept as `.long` with its disassembly as a
comment. The result is assembled, linked at the stock addresses and compared byte for byte.

The output (build/asm/) is disassembly of Mikohn's code: generated locally from your own extracted image, never
committed. Usage: asmsrc.py [START END]   (hex; default: the whole code region 0..0x988C4)
"""
import os
import re
import struct
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ANA = os.path.dirname(HERE)
APP = os.path.join(ANA, "extracted", "ELITE_RGB")
OUT = os.path.join(ANA, "build", "asm")
LLVM = "/opt/homebrew/opt/llvm/bin"
LLD = "/opt/homebrew/opt/lld/bin"
CODE_END = 0x988C4

BRANCH = re.compile(r"^(b|bl|ba|bla|bc|bcl|bdnz|bdz|bdnzl|bdzl|bdnz[tf]|bdz[tf]|b(lt|le|eq|ge|gt|ne|un|nu|so|ns)l?)[+-]?$")


def disassemble(img, start, end):
    """[(addr, word, mnemonic, ops, target)]: capstone, with undecodable words as (addr, word, None, '', None)."""
    import capstone
    md = capstone.Cs(capstone.CS_ARCH_PPC, capstone.CS_MODE_32 | capstone.CS_MODE_BIG_ENDIAN)
    md.detail = True
    out = []
    a = start
    while a < end:
        w = struct.unpack_from(">I", img, a)[0]
        ins = next(md.disasm(img[a:a + 4], a), None)
        if ins is None:
            out.append((a, w, None, "", None))
        else:
            tgt = None
            if BRANCH.match(ins.mnemonic):
                imms = [op.imm for op in ins.operands if op.type == capstone.ppc.PPC_OP_IMM]
                if imms:
                    tgt = imms[-1]
            out.append((a, w, ins.mnemonic, ins.op_str, tgt))
        a += 4
    return out


def raw_branch(a, w, tgt):
    """A conditional branch whose BO carries the old-style prediction bit (y, the BO low bit) cannot be written with
    a mnemonic: capstone drops the bit, and LLVM's '+'/'-' encode the newer 'at' bits instead. Such branches are
    written in the raw form (bc / bclr / bcctr with BO, BI), which encodes every bit as given. None otherwise."""
    op, bo, bi = w >> 26, (w >> 21) & 0x1F, (w >> 16) & 0x1F
    if not bo & 1 or bo & 0x14 == 0x14:                            # no y bit, or branch always
        return None
    lk = "l" if w & 1 else ""
    if op == 16 and tgt is not None:
        return f"bc{lk}{'a' if w & 2 else ''} {bo}, {bi}, L_{tgt & 0xFFFFFFFF:x}"
    if op == 19 and ((w >> 1) & 0x3FF) in (16, 528):
        return f"{'bclr' if (w >> 1) & 0x3FF == 16 else 'bcctr'}{lk} {bo}, {bi}, {(w >> 11) & 3}"
    return None


CR_OPS = {33: "crnor", 129: "crandc", 193: "crxor", 225: "crnand", 257: "crand", 289: "creqv", 417: "crorc", 449: "cror"}


def raw_cr(w):
    """Condition-register logic with explicit bit numbers: capstone prints aliases (crnot, cror with field names)
    that LLVM encodes differently from the original."""
    if w >> 26 == 19 and (w >> 1) & 0x3FF in CR_OPS:
        return f"{CR_OPS[(w >> 1) & 0x3FF]} {(w >> 21) & 31}, {(w >> 16) & 31}, {(w >> 11) & 31}"
    return None


def to_llvm(ops):
    return re.sub(r"\b(r|f|cr)(\d+)\b", r"%\1\2", ops)


def source(insns, forced, start, end):
    """The .S text. `forced`: addresses emitted as .long. Branch targets inside the range get labels; targets
    outside it become absolute symbols resolved by the linker script."""
    labels = {t for *_, t in insns if t is not None}
    lines = ["  .section .text.code, \"ax\"", "  .globl _code_start", "_code_start:"]
    for a, w, mn, ops, tgt in insns:
        if a in labels:
            lines.append(f"L_{a:x}:")
        if mn is None or a in forced:
            lines.append(f"  .long 0x{w:08x}" + (f"   # {mn} {ops}" if mn else ""))
            continue
        if tgt is not None:
            # replace the numeric target (the last operand) by its label
            ops = re.sub(r"(0x[0-9a-f]+|-?\d+)\s*$", f"L_{tgt & 0xFFFFFFFF:x}", ops)
        raw = raw_branch(a, w, tgt) or raw_cr(w)
        lines.append(f"  {raw}" if raw else f"  {mn} {to_llvm(ops)}")
    return "\n".join(lines) + "\n", {t for *_, t in insns if t is not None and not start <= t < end}


def build(src, extern, start, out):
    s, o, ld, elf, binf = (os.path.join(out, x) for x in ("code.S", "code.o", "code.ld", "code.elf", "code.bin"))
    open(s, "w").write(src)
    r = subprocess.run([f"{LLVM}/llvm-mc", "-triple=powerpc-unknown-eabi", "-filetype=obj", s, "-o", o],
                       capture_output=True, text=True)
    if r.returncode:
        return None, r.stderr
    lines = [f"SECTIONS {{ . = {start:#x}; .text : {{ *(.text.code) }} }}"]
    lines += [f"L_{t:x} = {t:#x};" for t in sorted(extern)]
    open(ld, "w").write("\n".join(lines) + "\n")
    r = subprocess.run([f"{LLD}/ld.lld", "-T", ld, o, "-o", elf, "--no-check-sections"], capture_output=True, text=True)
    if r.returncode:
        return None, r.stderr
    subprocess.run([f"{LLVM}/llvm-objcopy", "-O", "binary", "--only-section=.text", elf, binf], check=True)
    return open(binf, "rb").read(), None


def main():
    a = sys.argv[1:]
    start, end = (int(a[0], 16), int(a[1], 16)) if len(a) == 2 else (0, CODE_END)
    img = open(APP, "rb").read()
    os.makedirs(OUT, exist_ok=True)
    insns = disassemble(img, start, end)
    forced = set()
    errors_line = re.compile(r":(\d+):\d+: error")
    for round_ in range(1, 20):
        src, extern = source(insns, forced, start, end)
        got, err = build(src, extern, start, OUT)
        if got is None:
            # an instruction the assembler rejects: force every reported line to .long
            bad_lines = {int(m.group(1)) for m in errors_line.finditer(err)}
            if not bad_lines:
                print(err[:2000])
                return 1
            src_lines = src.split("\n")
            addr_of_line = {}
            idx = 0
            for i, l in enumerate(src_lines, 1):
                if l.startswith("  ") and not l.startswith("  .section") and not l.startswith("  .globl"):
                    addr_of_line[i] = insns[idx][0]
                    idx += 1
            forced |= {addr_of_line[n] for n in bad_lines if n in addr_of_line}
            print(f"round {round_}: {len(bad_lines)} lines rejected by the assembler -> .long")
            continue
        want = img[start:end]
        diff = [start + i for i in range(0, min(len(got), len(want)), 4) if got[i:i + 4] != want[i:i + 4]]
        if len(got) != len(want):
            print(f"length differs: {len(got)} vs {len(want)}")
            return 1
        if not diff:
            n_long = sum(1 for x in insns if x[2] is None or x[0] in forced)
            print(f"byte-identical: {(end - start) // 4} words, {(end - start) // 4 - n_long} as instructions, "
                  f"{n_long} as .long ({len({t for *_, t in insns if t is not None})} branch labels) -> {OUT}/code.S")
            return 0
        forced |= set(diff)
        print(f"round {round_}: {len(diff)} words assemble differently -> .long")
    return 1


if __name__ == "__main__":
    sys.exit(main())
