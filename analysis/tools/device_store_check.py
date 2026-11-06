#!/usr/bin/env python3
"""Find recompiled functions whose C lost a store to a hardware register.

Ghidra treats memory as plain RAM, so a store that writes back a value just read from the same register (the way
an MPC8xx handler clears write-1-to-clear event bits: SCCE = SCCE) looks like a no-op and disappears from the C.
The simulator does not keep such events pending, so only the real board shows it (scc3_isr, digi_codec_isr: the
interrupt never ends).

For each function in the replaced list: every store in the original instructions to IMMR (base register loaded with
lis rN,0xFF00) at offset X; the function's C must write g_ff00XXXX_N. Prints the functions that do not.

Usage: device_store_check.py REPLACED.txt   (one name per line; rebuild_report.txt's replaced names)
"""
import os, re, subprocess, sys

HERE = os.path.dirname(os.path.abspath(__file__))
ANA = os.path.dirname(HERE)
LLVM = "/opt/homebrew/opt/llvm/bin"


def functions():
    rows = sorted((int(l.split("\t")[0], 16), l.split("\t")[2]) for l in open(os.path.join(ANA, "symbols.tsv"))
                  if l.count("\t") >= 2 and l.split("\t")[1] == "func")
    return {n: (a, rows[i + 1][0] if i + 1 < len(rows) else a + 0x400) for i, (a, n) in enumerate(rows)}


def disasm(img, a, e):
    hexs = " ".join("0x%02x" % b for b in img[a:e])
    r = subprocess.run([f"{LLVM}/llvm-mc", "--disassemble", "-triple=powerpc-unknown-eabi"], input=hexs,
                       capture_output=True, text=True)
    return [l.strip() for l in r.stdout.splitlines() if l.strip() and not l.strip().startswith(".")]


def immr_stores(lines):
    base = set()
    out = set()
    for l in lines:
        m = re.match(r"lis (\d+), (-?\d+)$", l)
        if m:
            r, v = int(m.group(1)), int(m.group(2)) & 0xFFFF
            (base.add if v == 0xFF00 else base.discard)(r)
            continue
        m = re.match(r"(?:mr|addi) (\d+), (\d+)(?:, 0)?$", l)   # copies of the base
        if m:
            d, sreg = int(m.group(1)), int(m.group(2))
            (base.add if sreg in base and (l.startswith("mr") or l.endswith(", 0")) else base.discard)(d)
            continue
        m = re.match(r"(stb|sth|stw)u? (\d+), (-?\d+)\((\d+)\)$", l)
        if m and int(m.group(4)) in base:
            out.add(0xFF000000 + (int(m.group(3)) & 0xFFFF))
            continue
        m = re.match(r"\w+\.? (\d+),", l)                  # anything else that writes rN ends its base role
        if m and not l.startswith(("st", "cmp", "b", "mt")):
            base.discard(int(m.group(1)))
    return out


def lost_stores(img, start, end, text):
    """Hardware-register stores of the original instructions [start, end) that the C text never makes (as g_ADDR_N
    or DAT_ADDR / _DAT_ADDR)."""
    stores = immr_stores(disasm(img, start, end))
    return [a for a in sorted(stores)
            if not re.search(r"\b(?:g_%08x_\d|_?DAT_%08x)\s*(=(?!=)|\|=|&=|\+=|\^=)" % (a, a), text)]


def main(path):
    img = open(os.path.join(ANA, "extracted", "ELITE_RGB"), "rb").read()
    fns = functions()
    bad = []
    for name in open(path).read().split():
        if name not in fns:
            continue
        a, e = fns[name]
        stores = immr_stores(disasm(img, a, e))
        if not stores:
            continue
        c = os.path.join(ANA, "build", "fw", "c", name + ".c")
        text = open(c).read() if os.path.exists(c) else ""
        body = text[text.rfind("\n" + "{") - 200:] if "{" in text else text
        missing = [s for s in sorted(stores) if not re.search(r"g_%08x_\d\s*(=(?!=)|\|=|&=|\+=|\^=)" % s, body)]
        if missing:
            bad.append((name, missing))
            print(f"{name}: original stores to {', '.join('%#x' % m for m in missing)} not in the C")
    print(f"{len(bad)} function(s) lost a hardware-register store")


if __name__ == "__main__":
    main(sys.argv[1])
