#!/usr/bin/env python3
"""Print the meaningful lines of functions (no declarations, blank lines or Ghidra warnings).
Usage: peek.py ADDR [ADDR...] [-n LINES]"""
import re, sys
args = sys.argv[1:]
n = 30
if "-n" in args:
    i = args.index("-n"); n = int(args[i + 1]); del args[i:i + 2]
t = open("ghidra_out/elite_rgb_named.c").read()
for a in args:
    m = re.search(r"/\* === %s\s+(\S+) === \*/(.*?)(?=/\* === |\Z)" % a, t, re.S)
    if not m:
        print("== not found", a); continue
    lines = [l for l in m.group(2).splitlines() if l.strip() and "WARNING" not in l
             and not re.match(r"^\s+(undefined|u?int|uint|char|byte|ushort|short|bool|dword|float|double)[0-9]* \**\w+( \[\d+\])?;", l)
             and l.strip() not in ("{", "}")]
    print(f"######## {a} {m.group(1)} ({len(m.group(2).splitlines())} lines)")
    print("\n".join(l[:150] for l in lines[:n]))
