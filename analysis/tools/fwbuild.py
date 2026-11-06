#!/usr/bin/env python3
"""Build ELITE_RGB from source: C objects + assembly source + your dump's data, linked at the stock layout.

  - C: every function rebuild.py replaced (build/rebuild_report.txt: name, address), compiled from the decompiled C
    exactly as rebuild.py compiles it, linked at that address. A moved function's original entry gets a branch to
    it; the rest of every replaced slot is trap instructions.
  - Assembly: every other stretch of the code region, from asmsrc.py (labels for every branch target, .long for what
    LLVM cannot reproduce), proven byte-identical on its own.
  - Data: the data region (0x988C4 to the end) included from your extracted image at build time.

The link must reproduce build/ELITE_RGB.rebuilt byte for byte (the image rebuild.py verified function by function).
Everything is generated locally under build/fw/ from your own dump; nothing here is committed but this script.
Usage: fwbuild.py     (run rebuild.py first)
"""
import os
import re
import struct
import subprocess
import sys
import tempfile
from multiprocessing import Pool

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import asmsrc  # noqa: E402
import rebuild  # noqa: E402
import difftest  # noqa: E402
from difftest import LLVM, LLD, CFLAGS, exact_widths, symbol_address  # noqa: E402

ANA = os.path.dirname(HERE)
OUT = os.path.join(ANA, "build", "fw")
CODE_END = asmsrc.CODE_END


def compile_c(job):
    """One replaced function -> (name, object path, undefined symbols) or (name, None, why)."""
    name, moved = job
    pre, fns = rebuild._G["pre"], rebuild._G["fns"]
    addr, text = fns[name]
    text, extra = exact_widths(name, text)
    c, o = os.path.join(OUT, "c", name + ".c"), os.path.join(OUT, "c", name + ".o")
    open(c, "w").write(pre + "\n" + extra + "\n\n" + text)
    flags = [rebuild.OPT if f.startswith("-O") else f for f in CFLAGS]
    r = subprocess.run([f"{LLVM}/clang", *flags, "-ffunction-sections", "-c", c, "-o", o], capture_output=True, text=True)
    if r.returncode:
        return name, None, r.stderr[:200]
    if moved:                                       # callers keep the original address (its branch), as rebuild.py
        subprocess.run([f"{LLVM}/llvm-objcopy", f"--redefine-sym={name}={name}__moved", o], check=True)
    und = [s for s in subprocess.run([f"{LLVM}/llvm-nm", "-u", o], capture_output=True, text=True).stdout.split() if s != "U"]
    return name, o, und


def main():
    stock = open(rebuild.APP, "rb").read()
    rebuilt = open(os.path.join(ANA, "build", "ELITE_RGB.rebuilt"), "rb").read()
    rep = open(os.path.join(ANA, "build", "rebuild_report.txt")).read().split("\n\n", 1)[1]
    placed = [(l.split("\t")[0], int(l.split("\t")[1], 16)) for l in rep.strip().split("\n") if l]
    byname = {n: (s, e) for s, e, n in rebuild.functions()}
    os.makedirs(os.path.join(OUT, "c"), exist_ok=True)
    os.makedirs(os.path.join(OUT, "asm"), exist_ok=True)

    with Pool(os.cpu_count() or 4, initializer=rebuild._init) as pool:
        objs = pool.map(compile_c, [(n, base != byname[n][0]) for n, base in placed])
    bad = [(n, w) for n, o, w in objs if o is None]
    if bad:
        print("C compile failures:", bad[:5])
        return 1
    defined = {n for n, base in placed if base == byname[n][0]} | {n + "__moved" for n, base in placed if base != byname[n][0]}

    # the code region minus the replaced slots, as assembly chunks; slots become traps (+ a branch if moved)
    slots = sorted(byname[n] for n, _ in placed)
    chunks, a = [], 0
    for s, e in slots:
        if a < s:
            chunks.append((a, s))
        a = max(a, e)
    if a < CODE_END:
        chunks.append((a, CODE_END))
    insns = asmsrc.disassemble(stock, 0, CODE_END)
    byaddr = {x[0]: x for x in insns}
    # .long for every word asmsrc found LLVM cannot reproduce: rerun its fix-up on the whole region once
    forced = set()
    for _ in range(5):
        src, ext = asmsrc.source(insns, forced, 0, CODE_END)
        got, err = asmsrc.build(src, ext, 0, os.path.join(OUT, "asm"))
        if got is None:
            lines = {int(m.group(1)) for m in re.finditer(r":(\d+):\d+: error", err)}
            idx, amap = 0, {}
            for i, l in enumerate(src.split("\n"), 1):
                if l.startswith("  ") and not l.startswith(("  .section", "  .globl")):
                    amap[i] = insns[idx][0]
                    idx += 1
            forced |= {amap[n] for n in lines if n in amap}
            continue
        d = {i for i in range(0, CODE_END, 4) if got[i:i + 4] != stock[i:i + 4]}
        if not d:
            break
        forced |= d
    asm, labels_needed = [], set()
    for k, (s, e) in enumerate(chunks):
        part = [x for x in insns if s <= x[0] < e]
        text, ext = asmsrc.source(part, forced, s, e)
        text = text.replace(".section .text.code,", f".section .text.a{k},").replace("_code_start", f"_a{k}")
        asm.append(text)
        labels_needed |= ext
    # labels defined inside chunks must not be redefined by the script
    defined_labels = set(re.findall(r"^(L_[0-9a-f]+):", "".join(asm), re.M))
    trap_text = []
    for n, base in placed:
        s, e = byname[n]
        body = []
        if base != s:
            body.append(f"  b {n}__moved")                       # the original entry jumps to the moved copy
            s2 = s + 4
        else:
            s2 = s
        trap_text.append((s, f"  .section .text.slot_{n}, \"ax\"\n" + "\n".join(body) + "\n"))
    src = "".join(asm) + "".join(t for _, t in trap_text)
    open(os.path.join(OUT, "asm", "rest.S"), "w").write(src)
    r = subprocess.run([f"{asmsrc.LLVM}/llvm-mc", "-triple=powerpc-unknown-eabi", "-filetype=obj",
                        os.path.join(OUT, "asm", "rest.S"), "-o", os.path.join(OUT, "asm", "rest.o")],
                       capture_output=True, text=True)
    if r.returncode:
        print("assembly failed:", r.stderr[:800])
        return 1
    data = os.path.join(OUT, "data.bin")                           # your dump's data region, at build time
    open(data, "wb").write(stock[CODE_END:])
    open(os.path.join(OUT, "data.S"), "w").write(f'  .section .data.stock, "a"\n  .incbin "{data}"\n')
    subprocess.run([f"{asmsrc.LLVM}/llvm-mc", "-triple=powerpc-unknown-eabi", "-filetype=obj",
                    os.path.join(OUT, "data.S"), "-o", os.path.join(OUT, "data.o")], check=True)

    # linker script: every section at its stock address; absolute symbols for what nothing defines
    ls = ["SECTIONS {", "  /DISCARD/ : { *(.comment) *(.note*) *(.eh_frame*) }"]
    for k, (s, e) in enumerate(chunks):
        ls.append(f"  .a{k} {s:#x} : {{ *(.text.a{k}) }}")
    for n, base in placed:
        s, e = byname[n]
        if base == s:
            ls.append(f"  .f_{n} {s:#x} : {{ {OUT}/c/{n}.o(.text* .rodata*) }}")
        else:
            ls.append(f"  .slot_{n} {s:#x} : {{ *(.text.slot_{n}) }}")
            ls.append(f"  .f_{n} {base:#x} : {{ {OUT}/c/{n}.o(.text* .rodata*) }}")
    ls.append(f"  .data_stock {CODE_END:#x} : {{ *(.data.stock) }}")
    ls.append("}")
    und = set()
    for n, o, u in objs:
        und |= set(u)
    for n, base in placed:
        if base != byname[n][0]:
            ls.append(f"{n} = {byname[n][0]:#x};")
            und.discard(n)
    for s in sorted((und - defined) | {f"L_{t:x}" for t in labels_needed} - defined_labels):
        if s.startswith("L_"):
            ls.append(f"{s} = 0x{s[2:]};")
        else:
            a = symbol_address(s)
            if a is None:
                print("no address for", s)
                return 1
            ls.append(f"{s} = {a:#x};")
    ld = os.path.join(OUT, "fw.ld")
    open(ld, "w").write("\n".join(ls) + "\n")
    elf, binf = os.path.join(OUT, "ELITE_RGB.elf"), os.path.join(OUT, "ELITE_RGB")
    r = subprocess.run([f"{LLD}/ld.lld", "-N", "-T", ld, "-o", elf,
                        os.path.join(OUT, "asm", "rest.o"), os.path.join(OUT, "data.o")] + [o for _, o, _ in objs],
                       capture_output=True, text=True)
    if r.returncode:
        print("link failed:", r.stderr[:1500])
        return 1
    subprocess.run([f"{LLVM}/llvm-objcopy", "-O", "binary", "--gap-fill=0xff", elf, binf], check=True)
    img = bytearray(open(binf, "rb").read())
    # slack of replaced slots: traps, as rebuild.py writes them (objcopy fills gaps with 0xFF)
    secs = subprocess.run([f"{LLVM}/llvm-readelf", "-S", "-W", elf], capture_output=True, text=True).stdout
    used = []
    for m in re.finditer(r"\]\s+(\S+)\s+\S+\s+([0-9a-f]{8})\s+[0-9a-f]+\s+([0-9a-f]+)", secs):
        if m.group(1).startswith((".f_", ".slot_")):
            used.append((int(m.group(2), 16), int(m.group(2), 16) + int(m.group(3), 16)))
    for n, base in placed:
        s, e = byname[n]
        for a in range(s, e - 3, 4):
            if not any(u0 <= a < u1 for u0, u1 in used):
                img[a:a + 4] = rebuild.TRAP
    out = bytes(img)
    open(binf, "wb").write(out)                                     # the verified bytes, traps included
    n_diff = [i for i in range(0, min(len(out), len(rebuilt)), 4) if out[i:i + 4] != rebuilt[i:i + 4]]
    print(f"from source: {len(placed)} C functions, {len(chunks)} assembly chunks, data {len(stock) - CODE_END} bytes "
          f"-> {binf} ({len(out)} bytes)")
    print(f"vs the verified rebuilt image: {'IDENTICAL' if not n_diff and len(out) == len(rebuilt) else f'{len(n_diff)} words differ, first {n_diff[:4] and hex(n_diff[0])}, lengths {len(out)} / {len(rebuilt)}'}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
