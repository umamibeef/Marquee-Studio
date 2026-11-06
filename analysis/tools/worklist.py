#!/usr/bin/env python3
"""Worklist for hand-naming: every still-unnamed function in an address range, with the strings it
uses, how many functions call it, and the named functions it calls.
Usage: worklist.py START END [--strings-only]   (hex addresses; reads ghidra_out/)"""
import collections, re, sys

lo, hi = int(sys.argv[1], 16), int(sys.argv[2], 16)
only_str = "--strings-only" in sys.argv
t = open("ghidra_out/elite_rgb_named.c").read()
fns = re.findall(r"/\* === ([0-9a-f]{8})\s+(\S+) === \*/(.*?)(?=/\* === |\Z)", t, re.S)
addr_of = {n: a for a, n, _ in fns}
strs = collections.defaultdict(list)
for line in open("ghidra_out/string_xref.txt"):
    m = re.match(r'[0-9a-f]{8}\s+"(.*)"\s+<-\s+(.*)$', line.rstrip("\n"))
    if m:
        for fn in m.group(2).split(","):
            fn = fn.strip()
            a = fn[4:] if fn.startswith("FUN_") else addr_of.get(fn)
            if a:
                strs[a].append(m.group(1))
callers = collections.Counter()
calls = {}
for a, n, b in fns:
    seen = []
    for c in re.finditer(r"\b([A-Za-z_]\w*)\s*\(", b[b.find("{"):]):
        x = c.group(1)
        ca = x[4:] if x.startswith("FUN_") else addr_of.get(x)
        if ca and ca != a and ca not in seen:
            seen.append(ca)
    calls[a] = seen
    for ca in seen:
        callers[ca] += 1
names = {a: n for a, n, _ in fns}
for a, n, b in fns:
    if not (lo <= int(a, 16) < hi) or not n.startswith("FUN_"):
        continue
    if only_str and not strs[a]:
        continue
    named = [names[c] for c in calls[a] if not names[c].startswith(("FUN_", "os_", "libc_"))]
    s = " | ".join(x[:40] for x in strs[a][:5])
    print(f"{a} {len(b.splitlines()):5d}L callers={callers[a]:<3d} str[{len(strs[a])}] {s}"
          + (f"  calls: {', '.join(named[:5])}" if named else ""))
