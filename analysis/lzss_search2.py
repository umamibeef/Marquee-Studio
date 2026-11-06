import sys, struct, itertools, re
sys.path.insert(0, ".")
from lzss_search import decode, src
best = []
for ref, thr, fill, rs in itertools.product(("okumura", "be12_4", "be4_12", "abs12_4", "abs4_12"), (2, 3), (0x00, 0x20), (4096 - 18, 0)):
    o = decode(src, True, 1, ref, 4096, thr, fill, rs, limit=10**8)
    w = [struct.unpack(">I", o[j:j+4])[0] for j in range(0, len(o) - 3, 4)]
    land = sum(1 for x in w if x in (0x4E800020, 0x7C0802A6, 0x7C0803A6) or (x >> 16) == 0x9421)
    words = len(re.findall(rb"[A-Za-z]{5,}", o))
    best.append((land, words, len(o), ref, thr, fill, rs))
best.sort(reverse=True)
for b in best[:8]: print(f"landmarks {b[0]:6d}  words {b[1]:6d}  out {b[2]:8d}  ref={b[3]:8} thr={b[4]} fill={b[5]:#04x} rstart={b[6]}")
