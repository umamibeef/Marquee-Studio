#!/usr/bin/env python3
"""Differential test of the recreated source: is the decompiled C of a function equivalent to the firmware?

For each function: take its C from ghidra_out/elite_rgb_named.c (with the compile-check prelude, header and
artefact declarations), compile it for PowerPC (clang -O1), link it at RECOMP with every symbol it uses bound to
its firmware address (functions, globals, DAT_/UNK_/s_ labels by their address suffix), then run the original and
the recompiled code in two identical emulators on the same random inputs. Pointer arguments get their own
random 512-byte buffers. Compared: r3/r4 and every byte of RAM below the stack (image, BSS, scratch).

Usage: difftest.py [--all | NAME ... | --selftest] [--trials N] [--quiet]
  --selftest  breaks a few functions on purpose and checks that they are reported different
  --all  every function in 0x4000..0x61000 that compiles cleanly (ghidra_out/check/baseline.txt)
Writes ghidra_out/check/difftest.txt (equivalent / different / could not build or run, per function).
"""
import os, random, re, subprocess, sys, tempfile
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from fw import FW, SYM, STACK

HERE = os.path.dirname(os.path.abspath(__file__))
CHK = os.path.join(HERE, "..", "ghidra_out", "check")
LLVM, LLD = "/opt/homebrew/opt/llvm/bin", "/opt/homebrew/opt/lld/bin"
RECOMP = 0x1E00000                      # recompiled code goes here (outside fw.py's scratch area)
# -fsigned-char: Ghidra's char is signed (FLI LC counts are (char) casts); clang's PowerPC default is unsigned.
CFLAGS = ["--target=powerpc-unknown-eabi", "-mcpu=ppc", "-O1", "-fsigned-char", "-ffreestanding", "-fno-builtin", "-std=gnu89", "-w",
          "-Wno-int-conversion", "-Wno-incompatible-pointer-types", "-Wno-incompatible-function-pointer-types",
          "-fno-common", "-fno-jump-tables", "-msoft-float",
          # decompiled C reaches the same bytes through differently typed pointers (font_build(f, l, &f->img)
          # copies an image onto itself shifted by a word) and relies on wrap-around int arithmetic: forbid the
          # optimiser to assume either away, as the firmware's code does not
          "-fno-strict-aliasing", "-fwrapv"]


def parts():
    src = open(os.path.join(CHK, "elite_rgb_check.c")).read()
    i = src.index('#line 1 "elite_rgb_named.c"')
    pre = src[:i]
    body = src[i:]
    fns = {}
    for m in re.finditer(r"/\* === ([0-9a-f]{8})\s+(\S+) === \*/(.*?)(?=/\* === |\n#line 1 \"sentinel|\Z)", body, re.S):
        fns[m.group(2)] = (int(m.group(1), 16), m.group(3))
    return pre, fns


def symbol_address(name):
    """Firmware address for a symbol the recompiled code refers to."""
    if name in SYM:
        return SYM[name]
    m = re.search(r"(?:^|_)([0-9a-f]{8})(?:_\d+)?$", name)       # FUN_0001a2b0, DAT_000b3128, s_Hello_0009d340
    if m:
        return int(m.group(1), 16)
    m = re.fullmatch(r"\w*Ram([0-9a-f]{8})", name)                # uRam00000098 = *(u32 *)0x98
    if m:
        return int(m.group(1), 16)
    return None


_ACCESS = None


def access_sizes(fname):
    """{address: {sizes}} of fixed memory accesses in a function (from ExportDecomp's .access.tsv)."""
    global _ACCESS
    if _ACCESS is None:
        _ACCESS = {}
        path = os.path.join(HERE, "..", "ghidra_out", "elite_rgb_named.access.tsv")
        if os.path.exists(path):
            for line in open(path):
                f, a, s = line.split()
                _ACCESS.setdefault(f, {}).setdefault(int(a, 16), set()).add(int(s))
    return _ACCESS.get(fname, {})


CTYPE = {1: "unsigned char", 2: "unsigned short", 4: "unsigned int", 8: "unsigned long long"}


GHIDRA_SIZE = {"undefined1": 1, "byte": 1, "char": 1, "uchar": 1, "bool": 1, "undefined2": 2, "ushort": 2,
               "short": 2, "word": 2, "undefined4": 4, "uint": 4, "int": 4, "dword": 4, "ulong": 4, "long": 4,
               "undefined8": 8, "ulonglong": 8, "longlong": 8}


_POINTEE = None


def program_pointees():
    """{global: target width} from every function: the casts a pointer global is assigned through, program wide
    (spi_init assigns _DAT_0045c3e8 = (undefined2 *)..., spi_done only indexes it)."""
    global _POINTEE
    if _POINTEE is None:
        txt = open(os.path.join(HERE, "..", "ghidra_out", "elite_rgb_named.c")).read()
        seen = {}
        for m in re.finditer(r"\b(_?(?:DAT|UNK)_[0-9a-f]{8})\s*=\s*\((\w+)\s*\*\)", txt):
            seen.setdefault(m.group(1), set()).add(GHIDRA_SIZE.get(m.group(2)))
        _POINTEE = {s: next(iter(w)) for s, w in seen.items() if len(w) == 1 and None not in w}
    return _POINTEE


def pointee_width(sym, text):
    """The width of what global `sym` points to: an assignment through a cast (sym = (undefined2 *)...) or a
    dereference through one (*(ushort *)sym) in this function, else the program-wide casts (program_pointees).
    Ghidra's pointer arithmetic on sym (sym + 4, sym[1]) scales by it, so it must be declared with that target
    type. None if not shown or mixed."""
    ws = set()
    for m in re.finditer(r"\b" + re.escape(sym) + r"\s*=\s*\((\w+)\s*\*\)", text):
        ws.add(GHIDRA_SIZE.get(m.group(1)))
    for m in re.finditer(r"\*\s*\((\w+)\s*\*\)\s*" + re.escape(sym) + r"\b(?!\s*[+\-\[])", text):
        ws.add(GHIDRA_SIZE.get(m.group(1)))
    ws.discard(None)
    if not ws:
        w = program_pointees().get(sym)
        return w
    return ws.pop() if len(ws) == 1 else None


def use_width(m, text):
    """The width of one read of a global, from its own statement, when the same address is read at two widths in
    one function (glyph_desc_from_effect reads 0x182F84 with lhz into halfword fields and with lwz into a word):
    `*(undefined2 *)(...) = SYM;` or `local = SYM;` with a 2-byte local."""
    sym = m.group(0)
    ls = text.rfind("\n", 0, m.start()) + 1
    le = text.find("\n", m.end())
    line = text[ls:le if le >= 0 else None]
    d = re.match(r"\s*\*\s*\((\w+)\s*\*\)[^=]*=\s*" + re.escape(sym) + r"\s*;", line)
    if d and GHIDRA_SIZE.get(d.group(1), 4) < 4:
        return {GHIDRA_SIZE[d.group(1)]}
    d = re.match(r"\s*(\w+)\s*=\s*" + re.escape(sym) + r"\s*;", line)
    if d:
        v = re.search(r"^\s*(\w+)\s+" + re.escape(d.group(1)) + r"\s*;", text, re.M)
        if v and GHIDRA_SIZE.get(v.group(1), 4) < 4:            # only a narrower read is told apart
            return {GHIDRA_SIZE[v.group(1)]}
    return None


# Device address ranges (real hardware: registers, flash command sequences, battery RAM). Globals there are declared
# volatile: without it the compiler may read a status register once and spin on the copy (cpm_command), or drop the
# first of two writes to the same register (the watchdog's 0x556C, 0xAA39). The simulator answers such registers at
# once and does not model the watchdog, so only the real board shows it.
DEVICE_RANGES = ((0xFF000000, 0xFF010000),   # IMMR: SIU, memory controller, CPM, DPRAM
                 (0xF0000000, 0xF8000000),   # panel / FPGA window
                 (0xF8180000, 0xF8200000),   # boot flash (CS0)
                 (0xF8780000, 0xF8800000),   # NVRAM (CS1)
                 (0x50000000, 0x54000000),   # main flash (CS4, CS5)
                 (0x60000000, 0x70000000))   # CS7


def is_device(a):
    return any(lo <= a < hi for lo, hi in DEVICE_RANGES)


def exact_widths(name, text):
    """Rewrite globals the decompiler only knows as 'undefined' to their real access width in this function.
    Returns (text, extra declarations)."""
    acc = access_sizes(name)
    decls = []
    def fix(m):
        sym = m.group(0)
        a = int(m.group(1), 16)
        sizes = acc.get(a)                              # the p-code table; when it records several widths (or
        if not sizes or len(sizes) > 1:                 # none), this use's own statement decides
            sizes = use_width(m, text) or (sizes if sizes and len(sizes) == 1 else None)
        if not sizes or len(sizes) != 1:
            return sym
        size = next(iter(sizes))
        if size not in CTYPE:
            return sym
        new = f"g_{a:08x}_{size}"
        ptr = re.search(r"(?:^|[=(,;{}!&|?:<>+\-~]|\breturn)\s*\*\s*" + re.escape(sym) + r"\b|\b" + re.escape(sym) + r"\s*\[",
                        text, re.M)
        ty = CTYPE[size]
        if size == 4:
            pw = pointee_width(sym, text)                   # a pointer whose target width the text shows
            if pw:
                ty = CTYPE[pw] + " *"
            elif ptr:
                ty = "unsigned char *"
        if is_device(a):                                # hardware: every access must happen, in order
            ty = ty + " volatile" if ty.endswith("*") else "volatile " + ty
        decls.append(f"extern {ty} {new};")
        return new
    # only names not used with & (address-of keeps the declared type)
    pat = re.compile(r"(?<![&\w])_?(?:DAT|UNK)_([0-9a-f]{8})\b")
    out = pat.sub(fix, text)
    return out, "\n".join(sorted(set(decls)))


def build(name, pre, fns, tmp):
    addr, text = fns[name]
    text, extra = exact_widths(name, text)
    pre = pre + "\n" + extra + "\n"
    c = os.path.join(tmp, "f.c")
    o = os.path.join(tmp, "f.o")
    open(c, "w").write(pre + "\n" + text)
    r = subprocess.run([f"{LLVM}/clang", *CFLAGS, "-c", c, "-o", o], capture_output=True, text=True)
    if r.returncode:
        return None, "compile: " + r.stderr.strip().split("\n")[0][:120]
    nm = subprocess.run([f"{LLVM}/llvm-nm", "-u", o], capture_output=True, text=True).stdout.split()
    und = [s for s in nm if s != "U"]
    lds = [f"SECTIONS {{ . = {RECOMP:#x}; .text : {{ *(.text*) }} .rodata : {{ *(.rodata*) }} "
           f".data : {{ *(.data* .sdata* .sbss* .bss*) }} /DISCARD/ : {{ *(.comment) }} }}"]
    for s in und:
        a = symbol_address(s)
        if a is None:
            return None, f"link: no address for {s}"
        lds.append(f"{s} = {a:#x};")
    ld = os.path.join(tmp, "f.ld")
    open(ld, "w").write("\n".join(lds) + "\n")
    elf = os.path.join(tmp, "f.elf")
    r = subprocess.run([f"{LLD}/ld.lld", "-T", ld, "-e", name, o, "-o", elf], capture_output=True, text=True)
    if r.returncode:
        return None, "link: " + r.stderr.strip().split("\n")[0][:120]
    sym = subprocess.run([f"{LLVM}/llvm-nm", elf], capture_output=True, text=True).stdout
    m = re.search(r"^([0-9a-f]+) T " + re.escape(name) + "$", sym, re.M)
    binf = os.path.join(tmp, "f.bin")
    subprocess.run([f"{LLVM}/llvm-objcopy", "-O", "binary", elf, binf], check=True)
    return (int(m.group(1), 16), open(binf, "rb").read()), None


def return_kind(text, name):
    """'void', 'int' (r3) or 'long' (r3:r4) from the definition line."""
    m = re.search(r"^([^\n(]*?)\b" + re.escape(name) + r"\s*\(", text, re.M)
    ret = m.group(1).strip() if m else "int"
    if re.fullmatch(r"void", ret):
        return "void"
    if re.search(r"longlong|qword|undefined8|double|ulonglong", ret):
        return "long"
    return "int"


def param_kinds(text, name):
    m = re.search(re.escape(name) + r"\s*\(([^)]*)\)\s*\{", text)
    if not m or m.group(1).strip() in ("", "void"):
        return []
    return ["ptr" if "*" in p else "int" for p in m.group(1).split(",")]


def _sizes():
    res = {}
    for line in open(os.path.join(HERE, "..", "ghidra_out", "elite_rgb_named.functions.txt")):
        m = re.match(r"([0-9a-f]{8})\s+\S+\s+\((\d+) bytes\)", line)
        if m:
            res[int(m.group(1), 16)] = int(m.group(2))
    return res


SIZES = _sizes()


def divide_sites(image, addr):
    """Addresses of the divw/divwu instructions in the function at addr, with their divisor register."""
    res = []
    for a in range(addr, addr + SIZES.get(addr, 0), 4):
        w = int.from_bytes(image[a:a + 4], "big")
        if w >> 26 == 31 and (w >> 1) & 0x1FF in (459, 491):        # divwu, divw (with or without o/.)
            res.append((a, (w >> 11) & 31))
    return res


def run(f, entry, args):
    try:
        r = f.call(entry, *args, limit=2_000_000, r4=True)
        return ("ok", r)
    except RuntimeError as e:
        return ("err", str(e).split(":")[1].split(" at ")[0].strip())


def asm_names():
    """Names of functions tagged [asm] in symbols.tsv (hand-written assembly in the original): not compared."""
    res = set()
    for line in open(os.path.join(HERE, "..", "symbols.tsv")):
        c = line.rstrip("\n").split("\t")
        if len(c) > 4 and re.fullmatch(r"[0-9a-f]{8}", c[0]) and c[1] == "func" and "[asm]" in c[4]:
            res.add(c[2])
            if c[3] == "probable" and not c[2].endswith("_maybe"):
                res.add(c[2] + "_maybe")
    return res


ASM = asm_names()


def compare(name, pre, fns, trials, tmp):
    if name in ASM:
        return "asm", "tagged [asm] in symbols.tsv: hand-written assembly in the original, supplied as assembly"
    built, why = build(name, pre, fns, tmp)
    if not built:
        return "nobuild", why
    entry, code = built
    addr, text = fns[name]
    kinds = param_kinds(text, name)
    rk = return_kind(text, name)
    if len(kinds) > 8:
        return "nobuild", "more than 8 parameters"
    rnd = random.Random(addr)
    faulted = 0
    from unicorn import UC_HOOK_MEM_WRITE
    fa, fb = FW(), FW()
    fb.write(RECOMP, code)
    pages = set()                                       # 4 KB pages either run wrote (the rest is identical)

    code_hit = [False]

    def track(uc, access, a, size, value, _):
        pages.add(a >> 12)
        pages.add((a + size - 1) >> 12)
        if a < 0xA0000:                                 # app code (random inputs made it overwrite itself)
            code_hit[0] = True
    fa.uc.hook_add(UC_HOOK_MEM_WRITE, track)
    fb.uc.hook_add(UC_HOOK_MEM_WRITE, track)
    # a firmware division by zero (divwu leaves an undefined result on the MPC8xx) is undefined behaviour in C:
    # clang may assume the divisor is not zero and drop a later `if (h != 0)`. Such a trial says nothing.
    div0 = [False]
    from unicorn import UC_HOOK_CODE
    from unicorn.ppc_const import UC_PPC_REG_0

    def on_div(uc, a, size, rb):
        if uc.reg_read(UC_PPC_REG_0 + rb) == 0:
            div0[0] = True
    for a, rb in divide_sites(fa.image, addr):
        fa.uc.hook_add(UC_HOOK_CODE, on_div, rb, begin=a, end=a)
    image = fa.image

    def reset():
        """Undo the previous trial: restore every page it wrote (image bytes or zero), rewind the allocator."""
        for pg in pages:
            lo = pg << 12
            if lo >= 0x2000000:
                continue
            orig = image[lo:lo + 0x1000].ljust(0x1000, b"\0")
            fa.write(lo, orig)
            fb.write(lo, orig)
        fb.write(RECOMP, code)
        pages.clear()
        code_hit[0] = False
        div0[0] = False
        fa.next = fb.next = fw_scratch
    from fw import SCRATCH as fw_scratch
    for t in range(trials):
        reset()
        # a pool of buffers whose words are a mix of valid pointers into the pool, small ints and random
        # bytes, so that structures (sbuf, records) mostly hold usable pointers
        POOL, NB = 8, 512
        bases = [fa.alloc(NB, 16) for _ in range(POOL)]
        assert bases == [fb.alloc(NB, 16) for _ in range(POOL)]
        for b in bases:
            words = []
            for _ in range(NB // 4):
                r = rnd.random()
                if r < 0.3:
                    words.append(rnd.choice(bases) + 4 * rnd.randrange(NB // 8))
                elif r < 0.6:
                    words.append(rnd.randrange(64))
                elif r < 0.8:
                    words.append(int.from_bytes(bytes(rnd.choice(b"ABCxyz019 ,.") for _ in range(4)), "big"))
                else:
                    words.append(rnd.randrange(1 << 32))
            data = b"".join(w.to_bytes(4, "big") for w in words)
            fa.write(b, data)
            fb.write(b, data)
        args = []
        for k in kinds:
            if k == "ptr":
                args.append(rnd.choice(bases) + 4 * rnd.randrange(NB // 8))
            else:
                args.append(rnd.randrange(0x20, 0x7F) if rnd.random() < 0.3 else rnd.choice([0, 1, 2, 3, 7, 9, 10, 0x10, 0x20, 0x7F, 0x80, 0xFF, 0x100, 0x1234, 0xFFFF,
                                        0xFFFFFFFF, rnd.randrange(1 << 32), rnd.randrange(64),
                                        rnd.randrange(0x20, 0x7F), rnd.choice(b"azAZ09")]))
        ints = [i for i, k in enumerate(kinds) if k != "ptr"]
        if t < len(ints):
            # edge trials first: every int argument a full 32-bit value except one, which is 0 (a zero count
            # with a wide value elsewhere: crc16_kermit_update(crc, buf, 0) returns crc unmasked, which the
            # mixed draws below found only by luck; tools/rebuild.py caught it)
            for j in ints:
                args[j] = 0 if j == ints[t] else rnd.randrange(1 << 32)
        ra, rb = run(fa, addr, args), run(fb, entry, args)
        if (ra[0] == "err" and rb[0] == "err") or code_hit[0] or div0[0]:
            faulted += 1                                  # both fault, or the inputs made it overwrite code
            continue
        if ra[0] == rb[0] == "ok":                        # compare only the registers that carry the result
            keep = {"void": lambda r: None, "int": lambda r: r[0], "long": lambda r: r}[rk]
            ra, rb = ("ok", keep(ra[1])), ("ok", keep(rb[1]))
        if ra[0] != rb[0]:
            # one side faults and the other completes: usually random inputs that alias the code's own data
            # structures (a buffer pointing into its header); weaker than a real difference
            return "fault_differs", f"args {[hex(a) for a in args]}: firmware {ra} recompiled {rb}"
        if ra != rb:
            return "different", f"args {[hex(a) for a in args]}: firmware {ra} recompiled {rb}"
        for pg in sorted(pages):
            lo = pg << 12
            if lo >= STACK - 0x10000 or RECOMP <= lo < RECOMP + len(code) + 0x1000 or lo >= 0x2000000:
                continue                                # stack, the recompiled code, MMIO
            ma, mb = fa.read(lo, 0x1000), fb.read(lo, 0x1000)
            if ma != mb:
                for i in range(0, 0x1000, 4):            # word by word; a stack address on both sides is fine
                    wa, wb = ma[i:i + 4], mb[i:i + 4]
                    if wa == wb:
                        continue
                    va, vb = int.from_bytes(wa, "big"), int.from_bytes(wb, "big")
                    in_stack = lambda v: STACK - 0x100000 <= v <= STACK
                    if in_stack(va) and in_stack(vb):
                        continue
                    diff = lo + i
                    return "different", (f"args {[hex(a) for a in args]}: memory differs at {diff:#x} "
                                         f"(firmware {wa.hex()} recompiled {wb.hex()})")
    if faulted == trials:
        return "untested", f"every trial faulted in both ({ra[1]})"
    return "equivalent", f"{trials - faulted} of {trials} trials ran" + (f", {faulted} faulted in both" if faulted else "")


def selftest(pre, fns, tmp):
    """The checker must catch a wrong recreation: break a few functions' C on purpose and expect 'different'."""
    cases = [("abs", r"\^", "|"), ("toupper", r"-0x10", "-0x11"), ("crc16_modbus", r"& 0xf\)", "& 0x7)"),
             ("sbuf_to_int", r"\* 10", "* 9")]
    ok = True
    for name, pat, rep in cases:
        if name not in fns:
            continue
        addr, text = fns[name]
        bad = re.sub(pat, rep, text, count=1)
        if bad == text:
            print(f"  selftest: pattern {pat!r} not found in {name}; skipped")
            continue
        fns2 = dict(fns)
        fns2[name] = (addr, bad)
        k, why = compare(name, pre, fns2, 24, tmp)
        good = k == "different"
        ok &= good
        print(f"  {'ok  ' if good else 'FAIL'} selftest: broken {name} ({pat} -> {rep}) is reported {k}")
    return ok


_W = {}


def _init(trials):
    _W["pre"], _W["fns"] = parts()
    _W["trials"] = trials
    _W["tmp"] = tempfile.mkdtemp()


def _one(name):
    try:
        return name, compare(name, _W["pre"], _W["fns"], _W["trials"], _W["tmp"])
    except Exception as e:
        return name, ("error", f"{type(e).__name__}: {e}"[:150])


def main():
    args = sys.argv[1:]
    trials = int(args[args.index("--trials") + 1]) if "--trials" in args else 12
    quiet = "--quiet" in args
    pre, fns = parts()
    if "--all" in args:
        clean = set(open(os.path.join(CHK, "baseline.txt")).read().split())
        names = [n for n, (a, _) in sorted(fns.items(), key=lambda kv: kv[1][0])     # switch-case fragments
                 if f"{a:08x}" in clean and 0x4000 <= a < 0x61000 and not n.startswith("caseD_")]  # are not functions
    else:
        names = [a for a in args if not a.startswith("--") and not a.isdigit()]
    res = {}
    if "--selftest" in args:
        with tempfile.TemporaryDirectory() as tmp:
            return 0 if selftest(pre, fns, tmp) else 1
    jobs = int(args[args.index("--jobs") + 1]) if "--jobs" in args else (os.cpu_count() or 4)
    if len(names) > 4 and jobs > 1:
        import multiprocessing as mp
        with mp.Pool(jobs, initializer=_init, initargs=(trials,)) as pool:
            for n, r in pool.imap_unordered(_one, names, chunksize=4):
                res[n] = r
                if not quiet:
                    print(f"  {r[0]:10} {n}: {r[1]}", flush=True)
    else:
        with tempfile.TemporaryDirectory() as tmp:
            for n in names:
                try:
                    res[n] = compare(n, pre, fns, trials, tmp)
                except Exception as e:
                    res[n] = ("error", f"{type(e).__name__}: {e}"[:150])
                if not quiet:
                    print(f"  {res[n][0]:10} {n}: {res[n][1]}")
    counts = {}
    for k, _ in res.values():
        counts[k] = counts.get(k, 0) + 1
    lines = [f"{k:10} {n}: {why}" for n, (k, why) in res.items()]
    if "--all" in args:
        open(os.path.join(CHK, "difftest.txt"), "w").write("\n".join(lines) + "\n")
    print("difftest: " + ", ".join(f"{v} {k}" for k, v in sorted(counts.items())))
    return 1 if counts.get("different") else 0


if __name__ == "__main__":
    sys.exit(main())
