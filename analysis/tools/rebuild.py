#!/usr/bin/env python3
"""Rebuild ELITE_RGB at the stock layout, replacing proven functions with our recompiled C (docs/REBUILD-PLAN.md).

  1. Scan the stock image for every direct branch target and every word that points into code. A function that is
     entered anywhere but its start (jump-table fragments, shared tails), or that jumps through a register (bctr:
     switch tables we cannot see), is not replaced.
  2. Compile each candidate (by default: the functions the last difftest sweep found equivalent) with the exact
     flags difftest proves equivalence with, linked with every callee and global at its original address. Place it
     in its own slot when it fits; otherwise into a hole left by a function that shrank, with a branch at the
     original entry. Freed bytes are filled with trap instructions.
  3. Verify every replaced function from the IMAGE itself: call it at its original address in the stock and in the
     rebuilt image, on difftest's random inputs, and compare the result and all memory it wrote.
  4. Write build/ELITE_RGB.rebuilt (git-ignored: it still holds the stock data and code) and a report.

The rebuilt image contains Mikohn's data and the not-yet-replaced code, read from your own extracted image; it is
never committed. Usage: rebuild.py [--trials N] [--only NAME ...] [--identity] [--jobs N]
"""
import os
import random
import re
import struct
import subprocess
import sys
import tempfile
from multiprocessing import Pool

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import difftest  # noqa: E402
import device_store_check  # noqa: E402
from difftest import LLVM, LLD, CFLAGS, exact_widths, symbol_address, param_kinds, return_kind, run  # noqa: E402

ANA = os.path.dirname(HERE)
APP = os.path.join(ANA, "extracted", "ELITE_RGB")
OUT = os.path.join(ANA, "build")
CODE_END = 0x988C4                      # the data/text section starts here (ANALYSIS.md)
TRAP = struct.pack(">I", 0x7FE00008)    # tw 31,0,0: unconditional trap


def functions():
    """[(start, end, name)] from Ghidra's function list, by address."""
    out = []
    for line in open(os.path.join(ANA, "ghidra_out", "elite_rgb_named.functions.txt")):
        m = re.match(r"([0-9a-f]{8})\s+(\S+)\s+\((\d+) bytes\)", line)
        if m:
            a = int(m.group(1), 16)
            out.append((a, a + int(m.group(3)), m.group(2)))
    return sorted(out)


def scan(image, funcs):
    """(entered_inside, computed_jump): names of functions entered other than at their start, and of functions
    containing a bctr (an indirect jump, not a call)."""
    import capstone
    md = capstone.Cs(capstone.CS_ARCH_PPC, capstone.CS_MODE_32 | capstone.CS_MODE_BIG_ENDIAN)
    md.skipdata = True
    md.detail = True
    refs = []                                             # (from, to): a function's own branches don't count
    bctr_at = set()
    for ins in md.disasm(image[:CODE_END], 0):
        mn = ins.mnemonic
        if mn.startswith("b") and not mn.startswith(("bctr", "blr", "bdnz", "bdz")) or mn.startswith(("bdnz", "bdz")):
            for op in ins.operands:
                if op.type == capstone.ppc.PPC_OP_IMM:
                    refs.append((ins.address, op.imm))
        if mn in ("bctr", "bctr+", "bctr-") or (mn.startswith("b") and mn.endswith("ctr") and "l" not in mn[1:]):
            bctr_at.add(ins.address)
    for i in range(0, len(image) - 3, 4):                 # pointers anywhere in the image
        v = struct.unpack_from(">I", image, i)[0]
        if 0x4000 <= v < CODE_END and v % 4 == 0:
            refs.append((i, v))
    entered, computed = set(), set()
    starts = {s for s, _, _ in funcs}
    for s, e, n in funcs:
        for src, t in refs:
            if s < t < e and t not in starts and not s <= src < e:
                entered.add(n)
                break
        if any(s <= a < e for a in bctr_at):
            computed.add(n)
    return entered, computed


OPT = os.environ.get("REBUILD_OPT", "-Oz")               # every placed function is verified in the image itself


def build_at(name, pre, fns, base, tmp):
    """difftest.build, linked at `base` instead of its scratch address. Returns (entry, bytes) or (None, why)."""
    addr, text = fns[name]
    text, extra = exact_widths(name, text)
    c, o = os.path.join(tmp, "f.c"), os.path.join(tmp, "f.o")
    open(c, "w").write(pre + "\n" + extra + "\n\n" + text)
    flags = [OPT if f.startswith("-O") else f for f in CFLAGS]
    r = subprocess.run([f"{LLVM}/clang", *flags, "-c", c, "-o", o], capture_output=True, text=True)
    if r.returncode:
        return None, "compile: " + r.stderr.strip().split("\n")[0][:120]
    sec = subprocess.run([f"{LLVM}/llvm-size", "-A", o], capture_output=True, text=True).stdout
    if re.search(r"^\.(s?data|s?bss)\S*\s+[1-9]", sec, re.M):
        return None, "has writable data (statics)"
    nm = subprocess.run([f"{LLVM}/llvm-nm", "-u", o], capture_output=True, text=True).stdout.split()
    lds = [f"SECTIONS {{ . = {base:#x}; .text : {{ *(.text*) }} .rodata : {{ *(.rodata*) }} "
           f"/DISCARD/ : {{ *(.comment) *(.data*) *(.bss*) }} }}"]
    for s in [s for s in nm if s != "U"]:
        a = symbol_address(s)
        if a is None:
            return None, f"link: no address for {s}"
        lds.append(f"{s} = {a:#x};")
    ld, elf, binf = (os.path.join(tmp, x) for x in ("f.ld", "f.elf", "f.bin"))
    open(ld, "w").write("\n".join(lds) + "\n")
    r = subprocess.run([f"{LLD}/ld.lld", "-T", ld, "-e", name, o, "-o", elf], capture_output=True, text=True)
    if r.returncode:
        return None, "link: " + r.stderr.strip().split("\n")[0][:120]
    sym = subprocess.run([f"{LLVM}/llvm-nm", elf], capture_output=True, text=True).stdout
    m = re.search(r"^([0-9a-f]+) T " + re.escape(name) + "$", sym, re.M)
    subprocess.run([f"{LLVM}/llvm-objcopy", "-O", "binary", elf, binf], check=True)
    entry = int(m.group(1), 16)
    if entry != base:
        return None, f"entry not at the start of the section ({entry:#x})"
    return (entry, open(binf, "rb").read()), None


_G = {}


def _init():
    _G["pre"], _G["fns"] = difftest.parts()


def _size(name):
    with tempfile.TemporaryDirectory() as tmp:
        r, why = build_at(name, _G["pre"], _G["fns"], 0x1E00000, tmp)
    return name, (len(r[1]) if r else None), why


def _place(job):
    name, base = job
    with tempfile.TemporaryDirectory() as tmp:
        r, why = build_at(name, _G["pre"], _G["fns"], base, tmp)
    return name, base, (r[1] if r else None), why


def compare_images(name, start, stock, rebuilt, trials):
    """Call `name` at its original address in both images on difftest's random inputs; compare results and
    every page either run wrote. Returns (verdict, detail)."""
    from fw import FW, SCRATCH, STACK
    from unicorn import UC_HOOK_MEM_WRITE
    _, text = _G["fns"][name]
    kinds, rk = param_kinds(text, name), return_kind(text, name)
    rnd = random.Random(start)
    fa, fb = FW(stock), FW(rebuilt)
    pages = set()

    def track(uc, access, a, size, value, _):
        pages.add(a >> 12)
        pages.add((a + size - 1) >> 12)
    fa.uc.hook_add(UC_HOOK_MEM_WRITE, track)
    fb.uc.hook_add(UC_HOOK_MEM_WRITE, track)
    ran = 0
    for _ in range(trials):
        for pg in pages:
            lo = pg << 12
            if lo < 0x2000000:
                fa.write(lo, stock[lo:lo + 0x1000].ljust(0x1000, b"\0"))
                fb.write(lo, rebuilt[lo:lo + 0x1000].ljust(0x1000, b"\0"))
        pages.clear()
        fa.next = fb.next = SCRATCH
        POOL, NB = 8, 512
        bases = [fa.alloc(NB, 16) for _ in range(POOL)]
        assert bases == [fb.alloc(NB, 16) for _ in range(POOL)]
        for b in bases:
            words = [rnd.choice(bases) + 4 * rnd.randrange(NB // 8) if rnd.random() < 0.3 else
                     rnd.randrange(64) if rnd.random() < 0.5 else rnd.randrange(1 << 32) for _ in range(NB // 4)]
            data = b"".join(w.to_bytes(4, "big") for w in words)
            fa.write(b, data)
            fb.write(b, data)
        args = [rnd.choice(bases) + 4 * rnd.randrange(NB // 8) if k == "ptr" else
                rnd.choice([0, 1, 2, 3, 7, 10, 0x20, 0x41, 0x7F, 0xFF, 0x100, 0xFFFF, 0xFFFFFFFF, rnd.randrange(1 << 32)])
                for k in kinds]
        ra, rb = run(fa, start, args), run(fb, start, args)
        if ra[0] == rb[0] == "err":
            continue
        if ra[0] == rb[0] == "ok":
            keep = {"void": lambda r: None, "int": lambda r: r[0], "long": lambda r: r}[rk]
            ra, rb = ("ok", keep(ra[1])), ("ok", keep(rb[1]))
        if ra != rb:
            return "different", f"args {[hex(a) for a in args]}: stock {ra} rebuilt {rb}"
        ran += 1
        for pg in sorted(pages):
            lo = pg << 12
            if lo >= STACK - 0x10000 or lo >= 0x2000000 or lo < CODE_END:
                continue                                # stack, MMIO, and the code itself (it differs by design)
            if fa.read(lo, 0x1000) != fb.read(lo, 0x1000):
                ma, mb = fa.read(lo, 0x1000), fb.read(lo, 0x1000)
                for i in range(0, 0x1000, 4):
                    va, vb = int.from_bytes(ma[i:i + 4], "big"), int.from_bytes(mb[i:i + 4], "big")
                    if va != vb and not (STACK - 0x100000 <= va <= STACK and STACK - 0x100000 <= vb <= STACK):
                        return "different", f"args {[hex(a) for a in args]}: memory at {lo + i:#x} {va:08x} vs {vb:08x}"
    return ("equivalent" if ran else "untested"), f"{ran} of {trials} trials ran"


def _verify(job):
    name, start, trials = job
    stock = _G.setdefault("stock", open(APP, "rb").read())
    rebuilt = _G.setdefault("rebuilt", open(os.path.join(OUT, "ELITE_RGB.rebuilt"), "rb").read())
    try:
        return name, *compare_images(name, start, stock, rebuilt, trials)
    except Exception as e:
        return name, "error", f"{type(e).__name__}: {e}"[:150]


def make_image(stock, byname, ok, skip, jobs, trials):
    """Compile, place (in place or into holes), write and verify. Returns (done, placed, image, verdicts)."""
    with Pool(jobs, initializer=_init) as pool:
        sizes = pool.map(_size, ok)
    fit, big = [], []
    for n, sz, why in sizes:
        if sz is None:
            skip[n] = why
        elif sz <= byname[n][1] - byname[n][0]:
            fit.append((n, sz))
        else:
            big.append((n, sz))
    # holes: the unused tail of every function replaced in place, first fit by decreasing size
    holes = sorted(((byname[n][0] + ((sz + 3) & ~3), byname[n][1] - byname[n][0] - ((sz + 3) & ~3)) for n, sz in fit),
                   key=lambda h: -h[1])
    holes = [list(h) for h in holes if h[1] >= 8]
    moved = []
    for n, sz in sorted(big, key=lambda b: -b[1]):
        need = (sz + 3) & ~3
        h = next((h for h in holes if h[1] >= need), None)
        if h is None:
            skip[n] = f"larger than its slot ({sz} > {byname[n][1] - byname[n][0]}) and no hole fits"
            continue
        skip.pop(n, None)
        moved.append((n, h[0]))
        h[0] += need
        h[1] -= need
    with Pool(jobs, initializer=_init) as pool:
        placed = pool.map(_place, [(n, byname[n][0]) for n, _ in fit] + moved)
    img = bytearray(stock)
    done = []
    for n, base, code, why in placed:
        if code is None:
            skip[n] = "placement build: " + why
            continue
        s, e = byname[n]
        img[s:e] = TRAP * ((e - s) // 4) + TRAP[:(e - s) % 4]
        done.append((n, base))
    for n, base in done:                                   # code after all slots are trapped (holes reuse them)
        code = next(c for m, b, c, w in placed if m == n)
        img[base:base + len(code)] = code
        s, e = byname[n]
        if base != s:
            img[s:s + 4] = struct.pack(">I", 0x48000000 | ((base - s) & 0x03FFFFFC))
    open(os.path.join(OUT, "ELITE_RGB.rebuilt"), "wb").write(bytes(img))
    _G.pop("rebuilt", None)
    with Pool(jobs, initializer=_init) as pool:
        ver = pool.map(_verify, [(n, byname[n][0], trials) for n, _ in done])
    return done, placed, img, ver


def main():
    args = sys.argv[1:]
    trials = int(args[args.index("--trials") + 1]) if "--trials" in args else 12
    jobs = int(args[args.index("--jobs") + 1]) if "--jobs" in args else (os.cpu_count() or 4)
    only = args[args.index("--only") + 1:] if "--only" in args else None
    stock = open(APP, "rb").read()
    os.makedirs(OUT, exist_ok=True)
    funcs = functions()
    byname = {n: (s, e) for s, e, n in funcs}
    if "--identity" in args:
        open(os.path.join(OUT, "ELITE_RGB.rebuilt"), "wb").write(stock)
        print("identity image written; byte-identical:", open(os.path.join(OUT, "ELITE_RGB.rebuilt"), "rb").read() == stock)
        return 0
    entered, computed = scan(stock, funcs)
    if only:
        cands = only
    else:
        cands = [l.split()[1].rstrip(":") for l in open(os.path.join(ANA, "ghidra_out", "check", "difftest.txt"))
                 if l.startswith("equivalent")]
    skip = {}
    ok = []
    hw_exclude = {l.split(None, 1)[0]: (l.split(None, 1)[1].strip() if len(l.split(None, 1)) > 1 else "")
                  for l in open(os.path.join(HERE, "hw_exclude.tsv")) if l.strip() and not l.startswith("#")}
    import compile_check
    asm = compile_check.asm_addresses()
    _, fns = difftest.parts()
    for n in cands:
        if n not in byname:
            skip[n] = "not in the function list (renamed?)"
        elif n in hw_exclude:
            skip[n] = "real hardware: " + hw_exclude[n]
        elif n in fns and device_store_check.lost_stores(stock, byname[n][0], byname[n][1], fns[n][1]):
            # Ghidra dropped a store to a hardware register (a write-1-to-clear event register written back: SCCE =
            # SCCE looks like a no-op); the real board then never clears the event (scc3_isr, digi_codec_isr)
            skip[n] = "the C lost a store to a hardware register"
        elif byname[n][0] in asm:
            skip[n] = "tagged [asm]: kept as the original instructions"
        elif n in fns and "Removing unreachable block" in fns[n][1]:
            # Ghidra assumed some memory constant (a hardware register, a tick counter) and dropped code; random
            # inputs cannot show it (ac97_link_send: the watchdog and port writes gone, its tick wait endless)
            skip[n] = "Ghidra removed 'unreachable' blocks: the C is missing code"
        elif n in entered:
            skip[n] = "entered other than at its start"
        elif n in computed:
            skip[n] = "jumps through a register (bctr)"
        elif byname[n][0] < 0x4000 or byname[n][1] > CODE_END:
            skip[n] = "outside the app code"
        else:
            ok.append(n)
    rejected = {}
    for round_ in range(1, 6):
        done, placed, img, ver = make_image(stock, byname, [n for n in ok if n not in rejected], skip, jobs, trials)
        bad = [v for v in ver if v[1] not in ("equivalent", "untested")]
        print(f"round {round_}: {len(done)} replaced, {len(bad)} failed verification in the image")
        if not bad:
            break
        for v in bad:
            rejected[v[0]] = v[2]
    for n, why in rejected.items():
        skip[n] = "different when run from the image: " + why
    code_ours = sum(byname[n][1] - byname[n][0] for n, _ in done)
    app_code = sum(e - s for s, e, n in funcs if 0x4000 <= s < CODE_END)
    lines = [f"rebuild: {len(done)} functions replaced ({len([1 for n, b in done if b == byname[n][0]])} in place, "
             f"{len([1 for n, b in done if b != byname[n][0]])} moved to holes), {code_ours} of {app_code} code bytes "
             f"({100 * code_ours / app_code:.1f}%) now our compiled C",
             f"verified from the image: {sum(1 for v in ver if v[1] == 'equivalent')} equivalent, "
             f"{sum(1 for v in ver if v[1] == 'untested')} untested, {len(bad)} different",
             f"not replaced: {len(skip)}"]
    from collections import Counter
    for why, k in Counter(re.sub(r"\(.*\)|\d+", "N", w)[:60] for w in skip.values()).most_common(8):
        lines.append(f"  {k:4} {why}")
    for n, why in rejected.items():
        lines.append(f"  excluded (different in the image) {n}: {why}")
    report = "\n".join(lines)
    open(os.path.join(OUT, "rebuild_report.txt"), "w").write(report + "\n\n" + "\n".join(
        f"{n}\t{b:#x}\t{'in place' if b == byname[n][0] else 'moved'}" for n, b in sorted(done, key=lambda x: x[1])) + "\n")
    print(report)
    return 0


if __name__ == "__main__":
    sys.exit(main())
