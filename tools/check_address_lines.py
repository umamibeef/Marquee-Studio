#!/usr/bin/env python3
"""Spot address lines that didn't reach the chip during a read.
For each address bit, compare every byte with the byte at that bit flipped. Real data matches about
10-20% of the time on low lines; ~100% means that address line was stuck (bad contact).
  check_address_lines.py DUMP.bin [MORE.bin ...]
"""
import sys

for path in sys.argv[1:]:
    d = open(path, "rb").read()
    n, bad = len(d), []
    for k in range(n.bit_length() - 1):
        m = 1 << k
        same = sum(1 for i in range(n) if not i & m and d[i] == d[i | m]) / (n // 2)
        if same > 0.9:
            bad.append(f"A{k} ({same:.0%})")
    print(f"{path}: " + ("STUCK address lines: " + ", ".join(bad) if bad else "address lines OK"))
