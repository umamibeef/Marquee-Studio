#!/usr/bin/env python3
"""Name the library functions linked into ELITE_RGB (libpng 1.2.5, zlib 1.1.4) by matching them
against the real library sources.

1. clang parses each library source file (as PowerPC Linux, with a parse-only stub libc in
   libstub/stubinc) and dumps its AST. For every function we keep the string literals it uses and
   the functions it calls, in order.
2. String vote: a firmware function gets a library name when every string it references that is
   unique to one library function points at the same library function.
3. Link order: between two identified functions from the same source file, the unnamed firmware
   functions in between are that file's functions in between, in order, when the counts match
   exactly (GCC emits a file's functions in source order and the linker keeps a file together).
   These names are "probable".
4. Call alignment: for each identified pair, the firmware function's distinct callees (by first
   call) are aligned with the library function's, but only when both lists have the same length.
   A callee name is "known" when two or more identified callers agree on it, "probable" with one.
   Steps 3 and 4 repeat until nothing new is found.

Output: symbol lines for analysis/symbols.tsv (stdout), and a report on stderr.
Usage: libmatch.py  (run from analysis/; reads ghidra_out/, ../third_party/ and libstub/)
"""
import collections, json, os, re, subprocess, sys

HERE = os.path.dirname(os.path.abspath(__file__))
ANA = os.path.dirname(HERE)
SRC = os.path.join(os.path.dirname(ANA), "third_party")      # vendored upstream sources (THIRD-PARTY.md)
LIBSTUB = os.path.join(ANA, "libstub")                            # our stub headers
CLANG = "/opt/homebrew/opt/llvm/bin/clang"
STUB = ["-isystem", os.path.join(LIBSTUB, "stubinc")]
PNGZ = ["-I" + os.path.join(SRC, "zlib-1.1.4"), "-I" + os.path.join(SRC, "libpng-1.2.5")]
NEWLIB = os.path.join(SRC, "newlib-1.11.0", "newlib", "libc")
NEWLIB_DIRS = ["ctype", "errno", "locale", "misc", "reent", "search", "signal", "stdio", "stdlib",
               "string", "time", "machine/powerpc"]
RTEMS_VER = os.environ.get("RTEMS_VER", "4.6.0")
RTEMS = os.path.join(SRC, "rtems-" + RTEMS_VER)
RTEMS_DIRS = ["cpukit/score/src", "cpukit/rtems/src", "cpukit/sapi/src", "cpukit/libcsupport/src",
              "cpukit/libfs/src/imfs", "cpukit/score/cpu/powerpc", "c/src/lib/libcpu/powerpc/mpc8xx",
              "c/src/lib/libbsp/powerpc/mbx8xx", "c/src/lib/libbsp/powerpc/shared"]
RTEMS_INC = ["cpukit/include", "cpukit/score/include", "cpukit/score/inline", "cpukit/score/cpu/powerpc",
             "cpukit/rtems/include", "cpukit/rtems/inline", "cpukit/sapi/include", "cpukit/sapi/inline",
             "cpukit/libcsupport/include", "cpukit/libfs/src/imfs", "c/src/lib/libcpu/powerpc/mpc8xx/include",
             "c/src/lib/libcpu/powerpc/shared/include", "c/src/lib/libbsp/powerpc/mbx8xx/include",
             "cpukit/posix/include", "cpukit/posix/inline", "cpukit/itron/include", "cpukit/itron/inline",
             "c/src/lib/libbsp/powerpc/mbx8xx", "c/src/lib/libbsp/powerpc/shared"]
# name -> (source root, [files relative to it], extra clang args)
LIBS = {
    "zlib 1.1.4": ("zlib-1.1.4", [f + ".c" for f in
                   ["adler32", "compress", "crc32", "deflate", "gzio", "infblock", "infcodes", "inffast",
                    "inflate", "inftrees", "infutil", "trees", "uncompr", "zutil"]], STUB + PNGZ),
    "libpng 1.2.5": ("libpng-1.2.5", [f + ".c" for f in
                     ["png", "pngerror", "pngget", "pngmem", "pngpread", "pngread", "pngrio", "pngrtran",
                      "pngrutil", "pngset", "pngtrans", "pngwio", "pngwrite", "pngwtran", "pngwutil"]],
                     STUB + PNGZ),
    # newlib as RTEMS 4.6 used it: reentrant syscalls provided by the OS
    "newlib 1.11.0": ("newlib-1.11.0/newlib/libc",
                      sorted(os.path.relpath(os.path.join(r, f), NEWLIB)
                             for d in NEWLIB_DIRS if os.path.isdir(os.path.join(NEWLIB, d))
                             for r, _, fs in os.walk(os.path.join(NEWLIB, d)) for f in fs if f.endswith(".c")),
                      ["-nostdinc", "-isystem", "/opt/homebrew/opt/llvm/lib/clang/" +
                       sorted(os.listdir("/opt/homebrew/opt/llvm/lib/clang"))[-1] + "/include",
                       "-I" + os.path.join(NEWLIB, "include"), "-D__rtems__", "-D_COMPILING_NEWLIB",
                       "-DREENTRANT_SYSCALLS_PROVIDED", "-DMISSING_SYSCALL_NAMES"]),
}
if os.path.isdir(RTEMS):
    LIBS["rtems " + RTEMS_VER] = (
        "rtems-" + RTEMS_VER,
        sorted(os.path.relpath(os.path.join(r, f), RTEMS)
               for d in RTEMS_DIRS if os.path.isdir(os.path.join(RTEMS, d))
               for r, _, fs in os.walk(os.path.join(RTEMS, d)) for f in fs if f.endswith(".c")),
        ["-nostdinc", "-isystem", "/opt/homebrew/opt/llvm/lib/clang/" +
         sorted(os.listdir("/opt/homebrew/opt/llvm/lib/clang"))[-1] + "/include",
         "-I" + os.path.join(LIBSTUB, "rtemsstub"), "-I" + os.path.join(NEWLIB, "sys", "rtems"),
         "-I" + os.path.join(NEWLIB, "sys", "rtems", "include"), "-I" + os.path.join(NEWLIB, "include"),
         "-D__rtems__", "-D__RTEMS_INSIDE__", "-DUSE_INLINES", "-Dppc", "-Dmpc860", "-D__PPC__",
         "-D_ARCH_PPC", "-DPPC_ABI=PPC_ABI_EABI", "-D__CLANG_STDINT_H", "-D__STDC_HOSTED__=0"] + ["-I" + os.path.join(RTEMS, d) for d in RTEMS_INC])


def c_unescape(s):
    """Decode a C string literal body (as printed by clang or by our xref dump)."""
    out, i = [], 0
    esc = {"n": "\n", "r": "\r", "t": "\t", "0": "\0", "\\": "\\", '"': '"', "'": "'", "a": "\a",
           "b": "\b", "f": "\f", "v": "\v"}
    while i < len(s):
        c = s[i]
        if c == "\\" and i + 1 < len(s):
            n = s[i + 1]
            if n == "x":
                j = i + 2
                while j < len(s) and s[j] in "0123456789abcdefABCDEF":
                    j += 1
                out.append(chr(int(s[i + 2:j], 16)))
                i = j
                continue
            if n in "01234567" and i + 2 < len(s) and s[i + 2] in "01234567":
                j = i + 1
                while j < i + 4 and j < len(s) and s[j] in "01234567":
                    j += 1
                out.append(chr(int(s[i + 1:j], 8)))
                i = j
                continue
            out.append(esc.get(n, n))
            i += 2
            continue
        out.append(c)
        i += 1
    return "".join(out)


def source_functions():
    """{name: {"lib", "file", "strings": set, "calls": [names in first-call order], "index"}}"""
    funcs = {}
    for lib, (root, files, args) in LIBS.items():
        for rel in files:
            path = os.path.join(SRC, root, rel)
            cmd = [CLANG, "--target=powerpc-unknown-linux-gnu", "-std=gnu89", "-w", "-fsyntax-only",
                   "-ferror-limit=0"] + args + ["-Xclang", "-ast-dump=json", path]
            out = subprocess.run(cmd, capture_output=True, text=True).stdout
            if not out.strip():
                continue
            try:
                ast = json.loads(out)
            except json.JSONDecodeError:
                continue
            fname = root + "/" + rel
            cur_file, index = None, 0
            for node in ast.get("inner", []):
                f = node.get("loc", {}).get("file") or node.get("range", {}).get("begin", {}).get("file")
                if f:
                    cur_file = f
                if node.get("kind") != "FunctionDecl" or not cur_file or not cur_file.endswith(rel):
                    continue
                body = [n for n in node.get("inner", []) if n.get("kind") == "CompoundStmt"]
                if not body or node["name"] in funcs:
                    continue
                strings, calls = set(), []
                walk(body[0], strings, calls)
                funcs[node["name"]] = {"lib": lib, "file": fname, "strings": strings, "calls": calls,
                                       "index": index}
                index += 1
    return funcs


def walk(n, strings, calls):
    k = n.get("kind")
    if k == "StringLiteral":
        v = n.get("value", "")
        if v.startswith('"') and v.endswith('"'):
            strings.add(c_unescape(v[1:-1]))
    elif k == "CallExpr" and n.get("inner"):
        callee = find_ref(n["inner"][0])
        if callee and callee not in calls:
            calls.append(callee)
    for ch in n.get("inner", []):
        walk(ch, strings, calls)


def find_ref(n):
    if n.get("kind") == "DeclRefExpr":
        return n.get("referencedDecl", {}).get("name")
    for ch in n.get("inner", []):
        r = find_ref(ch)
        if r:
            return r
    return None


def binary_side():
    """String refs per firmware function, call order per function, and the current names."""
    gout = os.path.join(ANA, "ghidra_out")
    name_of, addr_of = {}, {}
    for line in open(os.path.join(gout, "elite_rgb_named.functions.txt")):
        m = re.match(r"([0-9a-f]{8})\s+(\S+)", line)
        if m:
            name_of[m.group(1)] = m.group(2)
            addr_of[m.group(2)] = m.group(1)
    strings = collections.defaultdict(set)
    for line in open(os.path.join(gout, "string_xref.txt")):
        m = re.match(r'[0-9a-f]{8}\s+"(.*)"\s+<-\s+(.*)$', line.rstrip("\n"))
        if not m:
            continue
        s = c_unescape(m.group(1))
        for fn in m.group(2).split(","):
            fn = fn.strip()
            a = fn[4:] if fn.startswith("FUN_") else addr_of.get(fn)
            if a:
                strings[a].add(s)
    calls = {}
    text = open(os.path.join(gout, "elite_rgb_named.c")).read()
    for m in re.finditer(r"/\* === ([0-9a-f]{8})\s+(\S+) === \*/(.*?)(?=/\* === |\Z)", text, re.S):
        a, body = m.group(1), m.group(3)
        brace = body.find("{")
        seq = []
        for c in re.finditer(r"\b([A-Za-z_]\w*)\s*\(", body[brace:]):
            nm = c.group(1)
            ca = nm[4:] if nm.startswith("FUN_") else addr_of.get(nm)
            if ca and ca != a and ca not in seq:
                seq.append(ca)
        calls[a] = seq
    return name_of, strings, calls


GENERATED = "# ---- libraries ("


def hand_names():
    """{address: name} for the hand-made function lines of symbols.tsv."""
    out = {}
    for line in open(os.path.join(ANA, "symbols.tsv")):
        if line.startswith(GENERATED) or line.startswith("# ---- libpng 1.2.5 / zlib"):
            break
        c = line.rstrip("\n").split("\t")
        if len(c) >= 5 and re.match(r"^[0-9a-f]{8}$", c[0]) and c[1] == "func" and c[3] == "known":
            out[c[0]] = c[2]
    return out


def open_name(n):
    """Still unnamed, or only a guess from an earlier run."""
    return n.startswith("FUN_") or n.endswith("_maybe")


def main():
    src = source_functions()
    name_of, bstrings, bcalls = binary_side()
    owners = collections.defaultdict(set)
    for name, f in src.items():
        for s in f["strings"]:
            if len(s) >= 6:
                owners[s].add(name)
    unique = {s: next(iter(o)) for s, o in owners.items() if len(o) == 1}

    found, evidence, conf = {}, {}, {}
    taken = {}
    # Anchors: only hand-made names (symbols.tsv above the generated section). Everything this
    # tool found before is re-derived from scratch, so its output is complete and reproducible.
    hand = hand_names()
    for a0, n0 in hand.items():
        if n0 in src:
            found[a0], taken[n0], conf[a0] = n0, a0, "known"
            evidence[a0] = "hand-made name"
    seeded = set(found)
    for a0 in name_of:                      # previous generated names count as open again
        if a0 not in hand and not name_of[a0].startswith("FUN_"):
            name_of[a0] = "FUN_" + a0
    # 1. string votes: a string is evidence only if exactly one library function and exactly one
    #    firmware function use it (e.g. "0123456789ABCDEF" is in _vfprintf_r but also in five
    #    of the sign's own formatters, so it proves nothing)
    users = collections.Counter(s for ss in bstrings.values() for s in ss)
    unique = {s: g for s, g in unique.items() if users[s] == 1}
    for a, ss in bstrings.items():
        votes = {unique[s] for s in ss if s in unique}
        if len(votes) == 1:
            g = votes.pop()
            hits = [s for s in ss if s in unique]
            if g in taken and taken[g] != a:
                print(f"conflict: {g} matches both {taken[g]} and {a}", file=sys.stderr)
                continue
            found[a], taken[g], conf[a] = g, a, "known"
            evidence[a] = f'string "{hits[0][:40]}"' + (f" (+{len(hits) - 1} more)" if len(hits) > 1 else "")
    n_str = len(found)

    # Each library's object files are linked together, so a call-aligned name must fall inside
    # its library's address window: around the string-matched functions, split at the midpoint
    # between neighbouring libraries. (Catches inlining: e.g. png_destroy_struct_2 is inlined in
    # this build, so position 1 of png_destroy_struct is really free() in the C library.)
    spans = {}
    for a, g in found.items():
        lib = src[g]["lib"]
        lo, hi = spans.get(lib, (a, a))
        spans[lib] = (min(lo, a), max(hi, a))
    order = sorted(spans.items(), key=lambda kv: kv[1][0])
    window = {}
    for i, (lib, (lo, hi)) in enumerate(order):
        wlo = int(lo, 16) - 0x2000
        whi = int(hi, 16) + 0x2000
        if i > 0:
            wlo = max(wlo, (int(order[i - 1][1][1], 16) + int(lo, 16)) // 2)
        if i + 1 < len(order):
            whi = min(whi, (int(hi, 16) + int(order[i + 1][1][0], 16)) // 2)
        window[lib] = (wlo, whi)
        print(f"{lib}: functions 0x{lo}..0x{hi}, window 0x{wlo:x}..0x{whi:x}", file=sys.stderr)

    all_addrs = sorted(name_of)

    def fill_by_order():
        by_file = collections.defaultdict(list)
        for a, g in found.items():
            by_file[src[g]["file"]].append((a, src[g]["index"]))
        order_src = collections.defaultdict(list)
        for n, f in src.items():
            order_src[f["file"]].append((f["index"], n))
        added = 0
        for file, pts in by_file.items():
            pts.sort()
            names = [n for _, n in sorted(order_src[file])]
            for (a1, i1), (a2, i2) in zip(pts, pts[1:]):
                if i2 <= i1:
                    continue                                   # out of order: not a clean span
                between_b = [x for x in all_addrs if a1 < x < a2]
                between_s = names[i1 + 1:i2]
                if not between_b or len(between_b) != len(between_s):
                    continue
                if any(x in found or not open_name(name_of[x]) for x in between_b):
                    continue
                if any(n in taken for n in between_s):
                    continue
                for x, n in zip(between_b, between_s):
                    found[x], taken[n], conf[x] = n, x, "probable"
                    evidence[x] = f"link order: between {src[found[a1]]['file'].split('/')[1]} " \
                                  f"{found[a1]} and {found[a2]}, counts match"
                    added += 1
        return added

    # 2. link order and call alignment, to a fixpoint
    while True:
        n_order = fill_by_order()
        support = collections.defaultdict(lambda: collections.defaultdict(list))
        for a, g in list(found.items()):
            bseq, sseq = bcalls.get(a, []), src[g]["calls"]
            if len(bseq) != len(sseq) or not bseq:
                continue
            for ba, sn in zip(bseq, sseq):
                if sn not in src:                     # libc or something outside the libraries
                    continue
                support[ba][sn].append(g)
        new = 0
        for ba, cands in support.items():
            if ba in found or not open_name(name_of.get(ba, "")):
                continue
            if len(cands) != 1:
                continue
            sn, callers = next(iter(cands.items()))
            if sn in taken:
                continue
            wlo, whi = window.get(src[sn]["lib"], (0, 0))
            if not wlo <= int(ba, 16) < whi:
                print(f"rejected: {sn} at {ba} is outside the {src[sn]['lib']} window", file=sys.stderr)
                continue
            found[ba], taken[sn] = sn, ba
            conf[ba] = "known" if len(set(callers)) >= 2 else "probable"
            evidence[ba] = f"called by {', '.join(sorted(set(callers))[:3])} at the matching position"
            new += 1
        if not new and not n_order:
            break

    clashes = [(a, name_of[a], g) for a, g in found.items() if not name_of[a].startswith("FUN_")]
    for a, cur, g in clashes:
        print(f"note: {a} already named {cur}; library says {g} (kept ours)", file=sys.stderr)
    by_lib = collections.Counter(src[g]["lib"] for a, g in found.items()
                                 if name_of[a].startswith("FUN_") or name_of[a].endswith("_maybe"))
    print(f"string matches: {n_str}, after call alignment: {len(found)}; new names: {dict(by_lib)}",
          file=sys.stderr)

    print(f"\n# ---- libraries ({', '.join(LIBS)}), matched against the sources by tools/libmatch.py ----")
    for a in sorted(found):
        if a in seeded or not (name_of[a].startswith("FUN_") or name_of[a].endswith("_maybe")):
            continue
        g = found[a]
        print(f"{a}\tfunc\t{g}\t{conf[a]}\t{src[g]['lib']} {src[g]['file'].split('/')[1]}: {evidence[a]}")


if __name__ == "__main__":
    main()
