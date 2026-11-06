#!/usr/bin/env python3
"""Rank unnamed functions in an address range by how many named functions they call or are called by.
Usage: neighbours.py START END [N]   (hex; reads ghidra_out/elite_rgb_named.c)"""
import collections, re, sys
lo, hi = int(sys.argv[1], 16), int(sys.argv[2], 16)
N = int(sys.argv[3]) if len(sys.argv) > 3 else 30
t = open("ghidra_out/elite_rgb_named.c").read()
fns = re.findall(r"/\* === ([0-9a-f]{8})\s+(\S+) === \*/(.*?)(?=/\* === |\Z)", t, re.S)
addr_of = {n: a for a, n, _ in fns}; name = {a: n for a, n, _ in fns}; body = {a: b for a, _, b in fns}
calls, callers = {}, collections.defaultdict(set)
for a, n, b in fns:
    s = set()
    for c in re.finditer(r"\b([A-Za-z_]\w*)\s*\(", b[b.find("{"):]):
        x = c.group(1); ca = x[4:] if x.startswith("FUN_") else addr_of.get(x)
        if ca and ca != a: s.add(ca)
    calls[a] = s
    for ca in s: callers[ca].add(a)
named = lambda a: not name[a].startswith(("FUN_", "os_", "libc_", "caseD", "switchD"))
rows = []
for a in name:
    if not (lo <= int(a, 16) < hi) or named(a): continue
    out = sorted(name[x] for x in calls[a] if named(x)); inn = sorted(name[x] for x in callers[a] if named(x))
    rows.append((len(out) + len(inn), a, len(body[a].splitlines()), inn[:3], out[:6], len(callers[a])))
rows.sort(reverse=True)
for s, a, L, inn, out, ncall in rows[:N]:
    print(f"{a} {L:4d}L  {ncall} callers, named: by[{', '.join(inn)}]  calls[{', '.join(out)}]")
