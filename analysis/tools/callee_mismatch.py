#!/usr/bin/env python3
"""From the last compile check, rank the callees whose prototype disagrees with their call sites
(too many / too few arguments, or a void result that is used). Fixing one signature in symbols.tsv
fixes every caller. Usage: callee_mismatch.py [N]"""
import collections, os, re, subprocess, sys
HERE = os.path.dirname(os.path.abspath(__file__))
CHK = os.path.join(HERE, "..", "ghidra_out", "check")
r = subprocess.run(["clang", "-fsyntax-only", "-target", "powerpc-unknown-none-elf", "-std=gnu89", "-w",
                    "-ferror-limit=0", "-fno-caret-diagnostics", "-Wno-int-conversion",
                    "-Wno-incompatible-pointer-types", "-Wno-incompatible-function-pointer-types",
                    os.path.join(CHK, "elite_rgb_check.c")], capture_output=True, text=True).stderr
c = open(os.path.join(HERE, "..", "ghidra_out", "elite_rgb_named.c")).read().split("\n")
cnt, ex = collections.Counter(), {}
for m in re.finditer(r"^elite_rgb_named\.c:(\d+):(\d+): error: ((?:too (?:many|few) arguments|assigning to .* from "
                     r"incompatible type 'void')[^\n]*)", r, re.M):
    line, col = c[int(m.group(1)) - 1], int(m.group(2))
    callee = None
    for mm in re.finditer(r"(\w+)\s*\(", line):
        if mm.start() < col or "void" in m.group(3):
            callee = mm.group(1)
    cnt[callee] += 1
    ex.setdefault(callee, m.group(3))
for k, n in cnt.most_common(int(sys.argv[1]) if len(sys.argv) > 1 else 40):
    print(f"{n:4} {str(k):32} {ex[k][:100]}")
