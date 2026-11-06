#!/usr/bin/env python3
"""Name library functions from their assert() calls: newlib's __assert(file, line, expr) gives the source file
and the asserted expression. The expression is looked up in the vendored source (third_party/), which locates the
function it belongs to; the line number only breaks ties, because the firmware was built from a slightly different
RTEMS 4.6 snapshot (free's assert is at line 323 there, 383 in 4.6.0).
Usage: assert_match.py      (run from analysis/; prints candidate symbols.tsv lines and a report on stderr)
"""
import os, re, sys
sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__))))
from fw import FW  # noqa: E402

ANA = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
TP = os.path.join(os.path.dirname(ANA), "third_party")
t = open(os.path.join(ANA, "ghidra_out", "elite_rgb_named.c")).read()
fw = FW()


def string_at(name):
    m = re.search(r"_([0-9a-f]{8})$", name)
    return fw.read_cstr(int(m.group(1), 16)) if m else None


def norm(s):
    return re.sub(r"\s+", "", s)


def locate(src, expr, fw_line):
    """The source line of `assert( expr )` in src: matched on the expression text (line numbers differ between
    RTEMS 4.6 snapshots); the line nearest to the firmware's breaks ties. None if the expression is not there."""
    lines = open(src, encoding="latin-1").read().split("\n")
    want = norm(expr)
    at = [i + 1 for i, l in enumerate(lines) if "assert" in l and norm(l.split("assert", 1)[1]).startswith("(" + want + ")")]
    return min(at, key=lambda i: abs(i - fw_line)) if at else None


EXPRS = {}


def expr_of(addr, fn):
    return EXPRS.get((addr, fn), "")


def find_source(path):
    """The vendored file for an assert path (absolute paths from the build machine)."""
    parts = path.replace("\\", "/").split("/")
    for i in range(len(parts)):
        rel = "/".join(parts[i:])
        for root in ("rtems-4.6.0", "newlib-1.11.0", "zlib-1.1.4", "libpng-1.2.5"):
            p = os.path.join(TP, root, rel)
            if os.path.isfile(p):
                return p
    base = os.path.basename(path)
    hits = [os.path.join(r, base) for r, _, fs in os.walk(TP) if base in fs]
    return hits[0] if len(hits) == 1 else None


FN_RE = re.compile(r"^(?:static\s+)?[A-Za-z_][\w\s\*]*?\b([A-Za-z_]\w*)\s*\([^;]*$")


def enclosing(src, line):
    """Name of the function whose body contains `line` (1-based): the last top-level definition above it."""
    lines = open(src, encoding="latin-1").read().split("\n")
    name, depth = None, 0
    cand = None
    for i, l in enumerate(lines[:line], 1):
        s = l.split("//")[0]
        if depth == 0:
            m = FN_RE.match(s)
            if m and m.group(1) not in ("if", "while", "for", "switch", "return", "sizeof"):
                cand = m.group(1)
        opens, closes = s.count("{"), s.count("}")
        if depth == 0 and opens:
            name = cand
        depth += opens - closes
    return name if depth > 0 else None


fns = re.findall(r"/\* === ([0-9a-f]{8})\s+(\S+) === \*/(.*?)(?=/\* === |\Z)", t, re.S)
hits = {}                                                     # firmware function -> {source function: (file, line)}
for addr, name, body in fns:
    for fsym, ln, esym in re.findall(r"__assert\((s_\w+),(0x[0-9a-f]+|\d+),\s*&?((?:s_|DAT_)\w+)\)", body):
        path, expr = string_at(fsym), string_at(esym)
        src = find_source(path) if path else None
        line = locate(src, expr, int(ln, 0)) if src and expr is not None else None
        fn = enclosing(src, line) if line else None
        if fn:
            hits.setdefault((addr, name), {}).setdefault(fn, (os.path.relpath(src, TP), line, int(ln, 0)))
            EXPRS.setdefault((addr, fn), expr)
spread = {}                                                   # source function -> how many firmware functions
for found in hits.values():
    for fn in found:
        spread[fn] = spread.get(fn, 0) + 1
# Inlined helpers leave their asserts in every caller, so the function a body belongs to is its least widespread
# source function; a source function seen in several bodies with no better candidate is reported, not named.
out = []
for (addr, name), found in sorted(hits.items()):
    best = min(found, key=lambda fn: (spread[fn], fn))
    if spread[best] > 1:
        print(f"inlined-only {addr} {name}: {sorted(found)} (seen in {spread[best]} functions)", file=sys.stderr)
        continue
    rel, ln, fw_ln = found[best]
    tag = "" if re.match(r"(os|libc|FUN)_", name) else f" (was {name})" if name != best else ""
    if name == best:
        continue
    out.append(f"{addr}\tfunc\t{best}\tknown\t{rel.split('/')[0]} {'/'.join(rel.split('/')[1:])}: its own assert({expr_of(addr, best)}) at line {ln} here, {fw_ln} in the firmware's build"
               f"{'; also inlines ' + ', '.join(sorted(set(found) - {best})) if len(found) > 1 else ''} (assert_match.py){tag}")
print("\n".join(out))
print(f"{len(out)} functions named or corrected from their asserts", file=sys.stderr)
