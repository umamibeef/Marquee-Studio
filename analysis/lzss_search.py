import sys, struct, itertools
d = open(sys.argv[1], "rb").read()
src = d[0x26d0 + 28:0x26d0 + 312912]   # ELITE_RGB payload after header + name block
LIMIT = 60000                           # decode only the start for scoring

def decode(src, msb, lit_bit, ref, n, thr, fill, rstart, limit=LIMIT):
    ring = bytearray([fill]) * n
    r = rstart % n
    out, i = bytearray(), 0
    while i < len(src) and len(out) < limit:
        fb = src[i]; i += 1
        for k in range(8):
            bit = (fb >> (7 - k)) & 1 if msb else (fb >> k) & 1
            if i >= len(src): break
            if bit == lit_bit:
                c = src[i]; i += 1
                out.append(c); ring[r] = c; r = (r + 1) % n
            else:
                if i + 1 >= len(src): break
                b0, b1 = src[i], src[i + 1]; i += 2
                if ref == "okumura":   pos, ln, rel = b0 | ((b1 & 0xF0) << 4), (b1 & 0x0F), False
                elif ref == "be12_4":  pos, ln, rel = (b0 << 4) | (b1 >> 4), (b1 & 0x0F), True
                elif ref == "be4_12":  pos, ln, rel = ((b0 & 0x0F) << 8) | b1, (b0 >> 4), True
                elif ref == "abs12_4": pos, ln, rel = (b0 << 4) | (b1 >> 4), (b1 & 0x0F), False
                elif ref == "abs4_12": pos, ln, rel = ((b0 & 0x0F) << 8) | b1, (b0 >> 4), False
                ln += thr
                start = (r - pos - 1) % n if rel else pos
                for kk in range(ln):
                    c = ring[(start + kk) % n]
                    out.append(c); ring[r] = c; r = (r + 1) % n
    return bytes(out)

def score(o):
    w = [struct.unpack(">I", o[j:j+4])[0] for j in range(0, len(o) - 3, 4)]
    land = sum(1 for x in w if x in (0x4E800020, 0x7C0802A6, 0x7C0803A6) or (x >> 16) == 0x9421)
    return land, o.count(b"    ") 

res = []
for msb, lit, ref, thr, fill, rs in itertools.product((True, False), (1, 0), ("okumura", "be12_4", "be4_12", "abs12_4", "abs4_12"),
                                                     (2, 3), (0x00, 0x20), (4096 - 18, 0)):
    o = decode(src, msb, lit, ref, 4096, thr, fill, rs)
    land, spaces = score(o)
    res.append((land, -spaces, msb, lit, ref, thr, fill, rs, o[:48]))
res.sort(reverse=True)
for x in res[:8]:
    print(f"landmarks {x[0]:4d} spaces4 {-x[1]:5d}  msb={x[2]} lit={x[3]} ref={x[4]:8} thr={x[5]} fill={x[6]:#04x} rstart={x[7]}  {x[8].hex(' ')}")
