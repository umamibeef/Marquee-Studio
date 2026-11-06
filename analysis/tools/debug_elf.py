#!/usr/bin/env python3
"""The from-source ELITE_RGB with DWARF, for source-level debugging in gdb / VS Code against the simulator
(simulator/, umami-marquee-sim --app analysis/build/fw/ELITE_RGB).

The from-source build (fwbuild.py, FWDIR = analysis/build/fw) is linked again with every C function recompiled from
FWDIR/c/NAME.c with the same flags plus -g, the same linker script, then checked byte for byte against
FWDIR/ELITE_RGB.elf. The symbols.tsv names are added for the assembly parts. Written under build/ (git-ignored); it
holds only what you built yourself.

Usage: debug_elf.py [FWDIR] [OUT]   (default OUT: build/sim/ELITE_RGB-debug.elf)
"""
import os
import re
import struct
import subprocess
import sys
import tempfile
from multiprocessing import Pool

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(os.path.dirname(HERE))
OUTDIR = os.path.join(REPO, "build", "sim")
CODE_END = 0x988C4
BSS_END = 0x500000


def _symbols():
    """(name, address, size, kind) for every func/data line of analysis/symbols.tsv."""
    funcs, data = [], []
    for line in open(os.path.join(REPO, "analysis", "symbols.tsv")):
        f = line.rstrip("\n").split("\t")
        if len(f) < 3 or not re.fullmatch("[0-9a-f]{8}", f[0]) or f[1] not in ("func", "data"):
            continue
        (funcs if f[1] == "func" else data).append((int(f[0], 16), f[2]))
    funcs.sort()
    data.sort()
    out = []
    for lst, kind, cap in ((funcs, "func", CODE_END), (data, "data", BSS_END)):
        for i, (a, n) in enumerate(lst):
            nxt = lst[i + 1][0] if i + 1 < len(lst) else cap
            size = max(0, min(nxt, cap) - a) if kind == "func" else 0
            out.append((n, a, size, kind))
    return out


def _tools():
    """LLVM, LLD and the C flags from analysis/tools/difftest.py (read from its source: importing it needs the
    decompile), with rebuild.py's optimisation level."""
    import ast
    vals = {}
    tree = ast.parse(open(os.path.join(REPO, "analysis", "tools", "difftest.py")).read())
    for node in tree.body:
        if isinstance(node, ast.Assign):
            names = [t.id for t in node.targets if isinstance(t, ast.Name)]
            tup = [e.id for t in node.targets if isinstance(t, ast.Tuple) for e in t.elts]
            if names == ["CFLAGS"]:
                vals["CFLAGS"] = ast.literal_eval(node.value)
            elif tup == ["LLVM", "LLD"]:
                vals["LLVM"], vals["LLD"] = ast.literal_eval(node.value)
    opt = os.environ.get("REBUILD_OPT", "-Oz")                     # as rebuild.py / fwbuild.py
    return vals["LLVM"], vals["LLD"], [opt if f.startswith("-O") else f for f in vals["CFLAGS"]]


def _sections(data):
    """{name: (addr, offset, size, type, flags)} of an ELF32 big-endian file."""
    shoff, = struct.unpack_from(">I", data, 0x20)
    shentsize, shnum, shstrndx = struct.unpack_from(">HHH", data, 0x2E)
    hdr = lambda i: struct.unpack_from(">10I", data, shoff + i * shentsize)
    stro = hdr(shstrndx)[4]
    out = {}
    for i in range(shnum):
        nm, typ, flags, addr, off, size = hdr(i)[:6]
        name = data[stro + nm:data.index(b"\0", stro + nm)].decode()
        out[name] = (addr, off, size, typ, flags)
    return out


def graft(src_elf, dst_elf):
    """Copy the contents of every loaded PROGBITS section of src_elf into the same section of dst_elf (same
    address and size required). Returns the names of the .f_ sections (C functions) whose bytes differed."""
    src, dst = open(src_elf, "rb").read(), bytearray(open(dst_elf, "rb").read())
    a, b = _sections(src), _sections(dst)
    changed = []
    for name, (addr, off, size, typ, flags) in b.items():
        if typ != 1 or not flags & 2:                                # PROGBITS, SHF_ALLOC
            continue
        if name not in a or a[name][0] != addr or a[name][2] != size:
            raise SystemExit(f"{dst_elf}: section {name} is not where {src_elf} has it")
        new = src[a[name][1]:a[name][1] + size]
        if dst[off:off + size] != new:
            changed.append(name[3:] if name.startswith(".f_") else name)
            dst[off:off + size] = new
    open(dst_elf, "wb").write(bytes(dst))
    return changed


def _line_mapped(src, name, out):
    """A copy of src whose function text carries a #line back to src itself: the prelude ends in
    '#line 1 "artefacts.h"', so without this the debug info would put the function in artefacts.h."""
    lines = open(src).read().split("\n")
    at = None
    for i, l in enumerate(lines):
        if re.match(rf"\S.*\b{re.escape(name)}\s*\(", l) and not l.rstrip().endswith(";") and not l.startswith(("#", "extern")):
            at = i
    if at is None:
        return src
    lines.insert(at, f'#line {at + 1} "{src}"')
    open(out, "w").write("\n".join(lines))
    return out


def _compile(job):
    llvm, flags, src, obj, moved, name = job
    src = _line_mapped(src, name, obj[:-2] + ".c")
    r = subprocess.run([f"{llvm}/clang", *flags, "-g", "-ffunction-sections", "-c", src, "-o", obj],
                       capture_output=True, text=True)
    if r.returncode:
        return name, r.stderr[:300]
    if moved:
        subprocess.run([f"{llvm}/llvm-objcopy", f"--redefine-sym={name}={name}__moved", obj], check=True)
    return name, None


def debug_elf(fw, out):
    llvm, lld, flags = _tools()
    ld_src = open(os.path.join(fw, "fw.ld")).read()
    cdir = os.path.join(fw, "c")
    names = re.findall(r"\{ \S+/c/([^/\s]+)\.o\(", ld_src)
    moved = set(re.findall(r"^\s+\.slot_(\S+) ", ld_src, re.M))
    objdir = os.path.join(os.path.dirname(out), "debug-obj")
    os.makedirs(objdir, exist_ok=True)
    jobs = [(llvm, flags, os.path.join(cdir, n + ".c"), os.path.join(objdir, n + ".o"), n in moved, n) for n in names]
    with Pool(os.cpu_count() or 4) as pool:
        bad = [(n, e) for n, e in pool.map(_compile, jobs) if e]
    if bad:
        raise SystemExit(f"compile failures: {bad[:3]}")
    ld = re.sub(r"\{ \S+/c/([^/\s]+)\.o\(", lambda m: "{ " + os.path.join(objdir, m.group(1)) + ".o(", ld_src)
    ldp = os.path.join(objdir, "fw-debug.ld")
    open(ldp, "w").write(ld)
    tmp = out + ".tmp"
    r = subprocess.run([f"{lld}/ld.lld", "-N", "-T", ldp, "-o", tmp, os.path.join(fw, "asm", "rest.o"),
                        os.path.join(fw, "data.o")] + [j[3] for j in jobs], capture_output=True, text=True)
    if r.returncode:
        raise SystemExit("link failed: " + r.stderr[:1500])
    # clang's -g changes instruction scheduling in places (same sizes, a few instructions in another order): put
    # the from-source build's own bytes into every loaded section, then check the whole image is identical. Line
    # and variable info is then approximate in the functions reordered (listed).
    reordered = graft(os.path.join(fw, "ELITE_RGB.elf"), tmp)
    with tempfile.TemporaryDirectory() as td:
        a, b = os.path.join(td, "a"), os.path.join(td, "b")
        subprocess.run([f"{llvm}/llvm-objcopy", "-O", "binary", "--gap-fill=0xff", os.path.join(fw, "ELITE_RGB.elf"), a], check=True)
        subprocess.run([f"{llvm}/llvm-objcopy", "-O", "binary", "--gap-fill=0xff", tmp, b], check=True)
        same = open(a, "rb").read() == open(b, "rb").read()
    if not same:
        raise SystemExit(f"{tmp}: the code differs from {fw}/ELITE_RGB.elf; not used")
    # symbols.tsv names for what the C objects do not define (the assembly chunks are only L_xxxx labels)
    have = set(subprocess.run([f"{llvm}/llvm-nm", "--defined-only", "-j", tmp], capture_output=True,
                              text=True).stdout.split())
    secs = []
    for m in re.finditer(r"\]\s+(\S+)\s+(PROGBITS|NOBITS)\s+([0-9a-f]{8})\s+[0-9a-f]+\s+([0-9a-f]+)\s+\S+\s+(\S*A\S*)",
                         subprocess.run([f"{llvm}/llvm-readelf", "-S", "-W", tmp], capture_output=True, text=True).stdout):
        secs.append((int(m.group(3), 16), int(m.group(3), 16) + int(m.group(4), 16), m.group(1), "X" in m.group(5)))
    args = []
    for n, a, size, kind in _symbols():
        if n in have:
            continue
        sec = next((s for s in secs if s[0] <= a < s[1]), None)
        if sec:
            args += ["--add-symbol", f"{n}={sec[2]}:{a - sec[0]:#x},global,{'function' if kind == 'func' else 'object'}"]
    for i in range(0, len(args), 2000):
        subprocess.run([f"{llvm}/llvm-objcopy", *args[i:i + 2000], tmp], check=True)
    subprocess.run([f"{llvm}/llvm-objcopy", "--wildcard", "--strip-symbol=L_*", tmp], check=True)   # asm labels
    os.replace(tmp, out)
    print(f"{out}: {len(names)} C functions with DWARF (sources in {cdir}), {len(args) // 2} names added from "
          f"symbols.tsv; code identical to {fw}/ELITE_RGB.elf")
    if reordered:
        print(f"  -g scheduled {len(reordered)} functions differently; their line info is approximate: "
              + " ".join(sorted(reordered)))


def main():
    fw = sys.argv[1] if len(sys.argv) > 1 else os.path.join(REPO, "analysis", "build", "fw")
    debug_elf(fw, sys.argv[2] if len(sys.argv) > 2 else os.path.join(OUTDIR, "ELITE_RGB-debug.elf"))
    return 0


if __name__ == "__main__":
    sys.exit(main())
