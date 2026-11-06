#!/usr/bin/env python3
"""Compile check for the named decompile: does ghidra_out/elite_rgb_named.c parse and type-check as C?

Builds ghidra_out/check/elite_rgb_check.c = a prelude for Ghidra's built-in types and macros
(CONCAT44, SUB42, ZEXT14, ...), the generated header ghidra_out/elite_rgb_named.h (data types,
prototypes, globals as the decompiler typed them) and the decompile itself, then runs
clang -fsyntax-only for a 32-bit big-endian PowerPC target. Errors are mapped back to functions.

Warnings are off: decompiled code is full of int/pointer mixing. Only errors count.

Usage: compile_check.py [--update-baseline | --ratchet] [--list]
  --ratchet: when there is no regression, raise the baseline to what compiles now (apply_symbols.sh uses it)
  Writes ghidra_out/check/report.txt. Exit 1 if a function that compiled in
  ghidra_out/check/baseline.txt no longer does (a regression), else 0.
"""
import os, re, subprocess, sys, collections

HERE = os.path.dirname(os.path.abspath(__file__))
OUT = os.path.join(HERE, "..", "ghidra_out")
CHK = os.path.join(OUT, "check")

PRELUDE = r"""
/* Ghidra built-ins not emitted by DataTypeWriter */
typedef void *pointer;
typedef char string[];
typedef char TerminatedCString[];
typedef unsigned short unicode[];
typedef unsigned char PNG[];
typedef unsigned char PNG_Image[];
typedef unsigned char GIF_Image[];
typedef unsigned char JPEG_Image[];
typedef unsigned char GIF[];
typedef unsigned char JPEG[];
typedef void *pointer32;
typedef int code();
typedef void void_fn(void);       /* symbols.tsv type "fn" (ApplySymbols): a callback */
typedef unsigned char bool;
typedef unsigned char uchar;
typedef unsigned short ushort;
typedef unsigned long ulong;
typedef long long longlong;
typedef unsigned long long ulonglong;
typedef unsigned int undefined3;
typedef unsigned long long undefined5, undefined6, undefined7;
typedef int sdword;
typedef short sword;
typedef signed char sbyte;
typedef unsigned long long qword;
typedef long double float10;
typedef unsigned int uint3;
typedef int int3;
typedef unsigned long long uint5, uint6, uint7;
typedef long long int5, int6, int7;
#define true 1
#define false 0
#define CONCAT11(a,b) ((unsigned short)(((unsigned)(unsigned char)(a) << 8) | (unsigned char)(b)))
#define CONCAT12(a,b) (((unsigned)(unsigned char)(a) << 16) | (unsigned short)(b))
#define CONCAT13(a,b) (((unsigned)(unsigned char)(a) << 24) | ((unsigned)(b) & 0xffffff))
#define CONCAT21(a,b) (((unsigned)(unsigned short)(a) << 8) | (unsigned char)(b))
#define CONCAT22(a,b) (((unsigned)(unsigned short)(a) << 16) | (unsigned short)(b))
#define CONCAT31(a,b) (((unsigned)(a) << 8) | (unsigned char)(b))
#define CONCAT44(a,b) (((unsigned long long)(unsigned)(a) << 32) | (unsigned)(b))
#define CONCAT14(a,b) (((unsigned long long)(unsigned char)(a) << 32) | (unsigned)(b))
#define CONCAT24(a,b) (((unsigned long long)(unsigned short)(a) << 32) | (unsigned)(b))
#define CONCAT34(a,b) (((unsigned long long)(a) << 32) | (unsigned)(b))
#define CONCAT41(a,b) (((unsigned long long)(unsigned)(a) << 8) | (unsigned char)(b))
#define CONCAT42(a,b) (((unsigned long long)(unsigned)(a) << 16) | (unsigned short)(b))
#define CONCAT43(a,b) (((unsigned long long)(unsigned)(a) << 24) | ((unsigned)(b) & 0xffffff))
#define SUB41(x,n) ((unsigned char)((unsigned)(x) >> ((n)*8)))
#define SUB42(x,n) ((unsigned short)((unsigned)(x) >> ((n)*8)))
#define SUB43(x,n) ((unsigned)(x) >> ((n)*8) & 0xffffff)
#define SUB21(x,n) ((unsigned char)((unsigned)(x) >> ((n)*8)))
#define SUB31(x,n) ((unsigned char)((unsigned)(x) >> ((n)*8)))
#define SUB32(x,n) ((unsigned short)((unsigned)(x) >> ((n)*8)))
#define SUB81(x,n) ((unsigned char)((unsigned long long)(x) >> ((n)*8)))
#define SUB82(x,n) ((unsigned short)((unsigned long long)(x) >> ((n)*8)))
#define SUB84(x,n) ((unsigned)((unsigned long long)(x) >> ((n)*8)))
#define SUB164(x,n) ((unsigned)(x))
#define ZEXT12(x) ((unsigned short)(unsigned char)(x))
#define ZEXT14(x) ((unsigned)(unsigned char)(x))
#define ZEXT24(x) ((unsigned)(unsigned short)(x))
#define ZEXT34(x) ((unsigned)(x))
#define ZEXT48(x) ((unsigned long long)(unsigned)(x))
#define ZEXT18(x) ((unsigned long long)(unsigned char)(x))
#define ZEXT28(x) ((unsigned long long)(unsigned short)(x))
#define SEXT14(x) ((int)(signed char)(x))
#define SEXT24(x) ((int)(short)(x))
#define SEXT48(x) ((long long)(int)(x))
#define SEXT816(x) ((long long)(x))
#define CARRY1(a,b) ((unsigned char)((a)+(b)) < (unsigned char)(a))
#define CARRY2(a,b) ((unsigned short)((a)+(b)) < (unsigned short)(a))
#define CARRY4(a,b) ((unsigned)((a)+(b)) < (unsigned)(a))
#define SCARRY1(a,b) 0
#define SCARRY2(a,b) 0
#define SCARRY4(a,b) (((int)(a) > 0 && (int)(b) > 0 && (int)((a)+(b)) < 0) || ((int)(a) < 0 && (int)(b) < 0 && (int)((a)+(b)) >= 0))
#define SBORROW1(a,b) 0
#define SBORROW2(a,b) 0
#define SBORROW4(a,b) (((int)(a) >= 0 && (int)(b) < 0 && (int)((a)-(b)) < 0) || ((int)(a) < 0 && (int)(b) > 0 && (int)((a)-(b)) >= 0))
#define POPCOUNT(x) __builtin_popcount(x)
#define LZCOUNT(x) __builtin_clz(x)
#define NAN __builtin_nan("")
void halt_baddata();
void halt_unimplemented();
void trap();
void sync();
void eieio();
void isync();
void dataCacheBlockFlush();
void dataCacheBlockStore();
void dataCacheBlockInvalidate();
void dataCacheBlockSetToZero();
void instructionCacheBlockInvalidate();
void enforceInOrderExecutionIO();
void synchronize();
void instructionSynchronize();
int  countLeadingZeros();
unsigned moveFromSpecialPurposeRegister();
void moveToSpecialPurposeRegister();
unsigned moveFromMachineStateRegister();
void moveToMachineStateRegister();
void returnFromInterrupt();
void systemCall();
void TLBInvalidateEntry();
unsigned moveFromTimeBaseRegister();
"""


def dedupe_types(h):
    """DataTypeWriter repeats some struct bodies; keep the first definition of each."""
    out, seen = [], set()
    for chunk in re.split(r"\n(?=(?:struct|union|enum) \w+ \{)", h):
        m = re.match(r"(struct|union|enum) (\w+) \{", chunk)
        if m:
            key = m.group(0)
            body_end = chunk.find("};")
            if key in seen:
                chunk = chunk[body_end + 2:] if body_end >= 0 else ""
            seen.add(key)
        out.append(chunk)
    return "\n".join(out)


def sanitize_header(h):
    """extern names from raw labels (older exports): make them identifiers as the decompiler prints them."""
    def fix(line):
        if not line.startswith("extern ") or "(*" in line:      # function pointers are well-formed already
            return line
        decl = line.split(";", 1)[0]               # drops the "/* also: ... */" note (raw names may hold */)
        m = re.match(r"(extern .*?[\s*])([^\s*\[]+)((?:\[\d*\])*)\s*$", decl)
        if not m:
            return decl + ";"
        return m.group(1) + re.sub(r"[^A-Za-z0-9_]", "_", m.group(2)) + m.group(3) + ";"
    return "\n".join(fix(l) for l in h.split("\n"))


def retype_by_use(h, c):
    """Globals Ghidra has no data type for ('undefined', 1 byte) are subscripted by the decompiler as
    byte arrays: declare those used as NAME[...] as arrays."""
    sub = set(re.findall(r"(?<![&\w])(\w+)\[", c))
    def fix(m):
        ty, name = m.group(1), m.group(2)
        if name in sub and ty in ("undefined", "undefined1", "undefined2", "undefined4", "byte", "char"):
            return f"extern {ty} {name}[];"
        return m.group(0)
    return re.sub(r"^extern (\w+) (\w+);", fix, h, flags=re.M)


def sanitize(c):
    """Ghidra-isms that are not C identifiers."""
    c = re.sub(r"(\w)::(\w)", r"\1__\2", c)          # switchD_0001::caseD_1
    c = re.sub(r"(\w)::~?(\w)", r"\1__\2", c)
    # Ghidra prints unsigned compares of a promoted byte minus a constant as `b - 0x30 < 10`; in C that is a
    # signed int compare (characters below '0' would pass). The machine code compares unsigned (cmplwi):
    # make it explicit. Found by tools/difftest.py on sbuf_to_int and FUN_0000c6b8.
    c = re.sub(r"((?:\(byte\)\s*)?(?:\*\s*)?\b(?:bVar\d+|pbVar\d+|[a-z]\w*(?:->\w+)?(?:\[[^\]\[]*\])*))"
               r"\s*-\s*(0x[0-9a-f]+|\d+)\s*<\s*(0x[0-9a-f]+|\d+)\b",
               lambda m: f"(uint)({m.group(1)} - {m.group(2)}) < {m.group(3)}"
               if re.search(r"bVar|pbVar|\(byte\)", m.group(1)) else m.group(0), c)
    # the same for a byte read through a cast pointer: *(byte *)(p + i) - 0x30 < 10 (decimal_field_parse)
    c = re.sub(r"(\*\(byte \*\)\((?:[^()]|\([^()]*\))*\))\s*-\s*(0x[0-9a-f]+|\d+)\s*<\s*(0x[0-9a-f]+|\d+)\b",
               lambda m: f"(uint)({m.group(1)} - {m.group(2)}) < {m.group(3)}", c)
    # partial access x._4_2_ = 2 bytes at offset 4 of x
    sz = {"1": "unsigned char", "2": "unsigned short", "4": "unsigned int", "8": "unsigned long long"}
    part = re.compile(r"((?:\w+|\))(?:\[[^\]\[]*\])?(?:(?:->|\.)\w+(?:\[[^\]\[]*\])?)*)\._(\d+)_(\d+)_")
    for _ in range(3):
        c = part.sub(lambda m: f"(*({sz.get(m.group(3), 'unsigned char')} *)((char *)&{m.group(1)} + {m.group(2)}))"
                     if not m.group(1).endswith(")") else m.group(0), c)
    # a string label with characters C does not allow (Ghidra keeps '<', '>', '.', '-' in s_..._ADDR names); the
    # header declares it with those replaced by '_' (sanitize_header), so the body has to match
    c = re.sub(r"\bs_[^\s;,()\[\]]*?_[0-9a-f]{8}\b",
               lambda m: re.sub(r"[^A-Za-z0-9_]", "_", m.group(0)), c)
    # a call through a word-sized global that has no function type (RTEMS configuration hooks): `(*_DAT_x)(..)`
    c = re.sub(r"\(\*(_DAT_[0-9a-f]{8})\)\(", r"(*(code *)\1)(", c)
    # array casts: `local_28 = (char  [4])EXPR;` copies N bytes into a char[N] local, and
    # `arr == (byte  [4])0x0` tests its first N bytes. C has no array casts: use an N-byte word.
    word = {"1": "unsigned char", "2": "unsigned short", "4": "unsigned int", "8": "unsigned long long"}
    c = re.sub(r"^(\s*)(\w+) = \((?:char|byte|undefined1?)\s+\[(1|2|4|8)\]\)(.+);$",
               lambda m: f"{m.group(1)}*({word[m.group(3)]} *){m.group(2)} = {m.group(4)};", c, flags=re.M)
    c = re.sub(r"\b(\w+) (==|!=) \((?:char|byte|undefined1?)\s+\[(1|2|4|8)\]\)(0x[0-9a-f]+|\d+)",
               lambda m: f"*({word[m.group(3)]} *){m.group(1)} {m.group(2)} {m.group(4)}", c)
    return c


# Ghidra artefacts that are never declared: direct memory (uRam00000098), stack/register stand-ins
ARTEFACT_TYPES = [(r"(?:u|ui)Ram[0-9a-f]+", "unsigned int"), (r"iRam[0-9a-f]+", "int"), (r"cRam[0-9a-f]+", "char"),
                  (r"bRam[0-9a-f]+", "unsigned char"), (r"sRam[0-9a-f]+", "short"), (r"usRam[0-9a-f]+", "unsigned short"),
                  (r"pb\w*Ram[0-9a-f]+", "unsigned char *"), (r"pu\w*Ram[0-9a-f]+", "unsigned int *"),
                  (r"pi\w*Ram[0-9a-f]+", "int *"), (r"pc\w*Ram[0-9a-f]+", "char *"), (r"p\w*Ram[0-9a-f]+", "void *"),
                  (r"\w*Ram[0-9a-f]+", "int"), (r"register0x[0-9a-f]+", "int"), (r"ram0x[0-9a-f]+", "int")]


def artefact_decls(names, header_decls, pointer_use=frozenset()):
    """Declarations for undeclared identifiers: _X as X's type, artefacts by prefix, the rest int.
    Returns (text, counts by kind)."""
    out, kinds = [], collections.Counter()
    for n in sorted(names):
        base = n.lstrip("_")
        if n.startswith("_") and base in header_decls:
            if n in pointer_use:                 # _DAT_x[i] or *_DAT_x: the value is a pointer
                out.append(f"extern unsigned char *{n};")
            else:                                # '_' = accessed at another size than the data: a word
                out.append(f"extern unsigned int {n};")
            kinds["_X spelling of a declared global"] += 1
            continue
        for pat, ty in ARTEFACT_TYPES:
            if re.fullmatch(pat, n):
                out.append(f"extern {ty} {n};")
                kinds["direct memory / register stand-in"] += 1
                break
        else:
            out.append(f"extern {'unsigned char *' if n in pointer_use else 'int'} {n};")
            kinds["other (label names the header spells differently)"] += 1
    return "\n".join(out), kinds


def functions(c):
    """[(start_line, address, name)] from the /* === addr  name === */ markers (1-based lines)."""
    res = []
    for i, line in enumerate(c.split("\n"), 1):
        m = re.match(r"/\* === ([0-9a-f]{8})\s+(\S+) === \*/", line)
        if m:
            res.append((i, m.group(1), m.group(2)))
    return res


def asm_addresses():
    """Functions whose symbols.tsv comment carries the tag [asm]: hand-written assembly in the original (exception
    vectors, context switch, the NRV2D bit reader with its register return, ...). They are kept in the decompile
    but are expected to be supplied as assembly in a rebuild, so they are counted apart."""
    res = set()
    for line in open(os.path.join(HERE, "..", "symbols.tsv")):
        c = line.rstrip("\n").split("\t")
        if len(c) > 4 and re.fullmatch(r"[0-9a-f]{8}", c[0]) and c[1] == "func" and "[asm]" in c[4]:
            res.add(int(c[0], 16))
    return res


def main():
    os.makedirs(CHK, exist_ok=True)
    c = sanitize(open(os.path.join(OUT, "elite_rgb_named.c")).read())
    h = retype_by_use(sanitize(sanitize_header(dedupe_types(open(os.path.join(OUT, "elite_rgb_named.h")).read()))), c)
    header_decls = {m.group(2): m.group(1) for m in re.finditer(r"^(extern [^;]*?\b(\w+)(?:\[\d+\])*)\s*;", h, re.M)}
    path = os.path.join(CHK, "elite_rgb_check.c")

    def run(extra):
        src = (PRELUDE + "\n#line 1 \"elite_rgb_named.h\"\n" + h + "\n#line 1 \"artefacts.h\"\n" + extra
               + "\n#line 1 \"elite_rgb_named.c\"\n" + c
               + "\n#line 1 \"sentinel.c\"\nint compile_check_sentinel = compile_check_sentinel_undeclared;\n")
        open(path, "w").write(src)
        # int/pointer mixing is everywhere in decompiled C and harmless here: warnings, not errors
        return subprocess.run(["clang", "-fsyntax-only", "-fsigned-char", "-target", "powerpc-unknown-none-elf", "-std=gnu89",
                               "-w", "-ferror-limit=0", "-fno-color-diagnostics", "-fno-caret-diagnostics",
                               "-Wno-int-conversion", "-Wno-incompatible-pointer-types",
                               "-Wno-incompatible-function-pointer-types", path], capture_output=True, text=True)
    r = run("")
    undeclared = set(re.findall(r"use of undeclared identifier '(\w+)'", r.stderr)) - {"compile_check_sentinel_undeclared"}
    # NAME[...] or a unary *NAME (after an operator, bracket or return, not a multiply)
    pointer_use = set(re.findall(r"(?<![&\w])(\w+)\[", c)) | \
        set(re.findall(r"(?:^|[=(,;{}!&|?:<>+\-~]|\breturn)\s*\*\s*(_\w+)", c, re.M))
    extra, art_kinds = artefact_decls(undeclared, header_decls, pointer_use)
    r = run(extra)
    if "sentinel.c:1" not in r.stderr:
        sys.exit("compile check BROKEN: clang never reached the end of the file (an unterminated comment or "
                 "string in the header?); see ghidra_out/check/elite_rgb_check.c")
    errs = [m for m in re.finditer(r"^(elite_rgb_named\.[ch]):(\d+):\d+: error: (.*)$", r.stderr, re.M)]
    fns = functions(c)
    starts = [f[0] for f in fns]
    import bisect
    per_fn = collections.defaultdict(list)
    header_errs = []
    for m in errs:
        if m.group(1) == "elite_rgb_named.h":
            header_errs.append(f"h:{m.group(2)}: {m.group(3)}")
            continue
        i = bisect.bisect_right(starts, int(m.group(2))) - 1
        key = f"{fns[i][1]} {fns[i][2]}" if i >= 0 else "(top)"
        per_fn[key].append(f"{m.group(2)}: {m.group(3)}")
    total = len(fns)
    bad = set(per_fn)
    # app functions only, for the headline number
    app = [f for f in fns if 0x4000 <= int(f[1], 16) < 0x61000]
    app_bad = [f for f in app if f"{f[1]} {f[2]}" in bad]
    asm = asm_addresses()
    asm_fns = [f for f in fns if int(f[1], 16) in asm]
    c_fns = [f for f in fns if int(f[1], 16) not in asm]
    c_bad = [f for f in c_fns if f"{f[1]} {f[2]}" in bad]
    kinds = collections.Counter(re.sub(r"'[^']*'", "'X'", e.split(": ", 1)[1]) for v in per_fn.values() for e in v)
    rep = [f"compile check: {total - len(bad)}/{total} functions compile cleanly "
           f"(app 0x4000-0x61000: {len(app) - len(app_bad)}/{len(app)}); "
           f"{sum(len(v) for v in per_fn.values())} errors in functions, {len(header_errs)} in the header",
           f"C functions (not tagged [asm]): {len(c_fns) - len(c_bad)}/{len(c_fns)} compile; "
           f"{len(asm_fns)} tagged [asm] (hand-written assembly in the original, "
           f"{len([f for f in asm_fns if f'{f[1]} {f[2]}' in bad])} of them do not compile as C)",
           "auto-declared Ghidra artefacts: " + ", ".join(f"{n} {k}" for k, n in art_kinds.items()),
           "", "most common errors:"]
    rep += [f"  {n:5}  {k}" for k, n in kinds.most_common(25)]
    rep += ["", "header errors:"] + ["  " + e for e in header_errs[:50]]
    rep += ["", "functions with errors:"]
    for k in sorted(per_fn):
        rep.append(f"  {k}  ({len(per_fn[k])})")
        rep += [f"      {e}" for e in per_fn[k][:5]]
    open(os.path.join(CHK, "report.txt"), "w").write("\n".join(rep) + "\n")
    print(rep[0])
    print(rep[1])
    print(rep[2])

    # regressions against the baseline: functions (by address) that compiled and now do not
    base_path = os.path.join(CHK, "baseline.txt")
    good_now = {f[1] for f in fns if f"{f[1]} {f[2]}" not in bad}
    if "--update-baseline" in sys.argv or not os.path.exists(base_path):
        open(base_path, "w").write("\n".join(sorted(good_now)) + "\n")
        print(f"baseline written: {len(good_now)} clean functions")
        return 0
    base = set(open(base_path).read().split())
    lost = sorted(base - good_now)
    gained = sorted(good_now - base)
    names = {f[1]: f[2] for f in fns}
    if gained and not lost and "--ratchet" in sys.argv:
        open(base_path, "w").write("\n".join(sorted(good_now)) + "\n")
        print(f"  {len(gained)} newly clean; baseline raised to {len(good_now)}")
    elif gained:
        print(f"  {len(gained)} newly clean (run with --update-baseline to keep them)")
    if lost:
        print(f"REGRESSION: {len(lost)} functions compiled before and no longer do:")
        for a in lost[:20]:
            print(f"    {a} {names.get(a, '?')}: {per_fn.get(f'{a} {names.get(a)}', ['?'])[0]}")
        return 1
    if "--list" in sys.argv:
        print("\n".join(rep[1:40]))
    return 0


if __name__ == "__main__":
    sys.exit(main())
