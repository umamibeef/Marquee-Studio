#!/usr/bin/env python3
"""UCL NRV2D compressor (8-bit bit buffer), the format BOOTSTRAP V2.00 unpacks (src/nrv2d.c).

The stock ELITE_RGB entry in the boot flash is NRV2D-compressed (flags 0x20000001). A rebuilt app must go back the
same way: uncompressed, it does not fit the 512 KB chip. The bitstream is derived from the decoder in src/nrv2d.c:
  - token bit 1: a literal byte follows;
  - token bit 0: a match. Offset: a gamma code G (2 bits per step) and, unless G == 2 (reuse the last offset), a
    raw byte; X = (G - 3) * 256 + byte, offset = X / 2 + 1, the low length bit = not (X & 1). Length: a second bit,
    or both 0 and a gamma-coded (m - 2). Copies m + 1 bytes, one more when the offset is above 0x500;
  - end: G = 0x1000002 and byte 0xFF.
Bits are written MSB first into a byte reserved in the output when the decoder would read it.

Usage: nrv2d_pack.py IN OUT   (prints the sizes; nrv2d_pack.py --test runs the round trip through src/nrv2d.c)"""
import os
import subprocess
import sys
import tempfile

MAX_CHAIN = 96          # match candidates tried per position
NICE = 256              # stop searching at this length


class Bits:
    def __init__(self):
        self.out = bytearray()
        self.pos = 0
        self.k = 0

    def bit(self, b):
        if self.k == 0:
            self.pos = len(self.out)
            self.out.append(0)
            self.k = 8
        self.k -= 1
        if b:
            self.out[self.pos] |= 1 << self.k

    def byte(self, v):
        self.out.append(v & 0xFF)


def _offset_gamma(v):
    """Bits that make the decoder's offset loop produce v (>= 2), stop bits included."""
    def after_a(v):
        u = v >> 1
        if u == 1:
            return [v & 1]
        return after_a((u >> 1) + 1) + [0, u & 1, v & 1]
    return after_a(v) + [1]


def _len_gamma(v):
    """Bits for the decoder's length loop: the digits of v (>= 2) after its leading 1, each followed by a stop bit."""
    d = bin(v)[3:]
    out = []
    for i, c in enumerate(d):
        out += [int(c), 1 if i == len(d) - 1 else 0]
    return out


def _gamma_cost(v):
    return 2 * (v.bit_length() - 1)


def match_cost(off, length, last):
    m = length - 1 - (off > 0x500)
    lc = 1 if m <= 3 else 1 + _gamma_cost(m - 2)
    if off == last:
        return 1 + 2 + 1 + lc
    x = (off - 1) * 2
    return 1 + _gamma_cost((x >> 8) + 3) + 8 + lc


def compress(data):
    n = len(data)
    b = Bits()
    head = {}
    prev = [-1] * n
    last = 1

    def insert(i):
        if i + 2 < n:
            h = data[i:i + 3]
            prev[i] = head.get(h, -1)
            head[h] = i

    def find(i):
        """(length, offset) of the cheapest-looking match at i, or (0, 0)."""
        best_len, best_off = 0, 0
        if i + 2 >= n:
            return 0, 0
        # the last offset first: it is the cheapest to code
        j = i - last
        if j >= 0:
            L = 0
            while i + L < n and data[j + L] == data[i + L] and L < NICE:
                L += 1
            if L >= 2:
                best_len, best_off = L, last
        j = head.get(data[i:i + 3], -1)
        tries = 0
        while j >= 0 and tries < MAX_CHAIN:
            tries += 1
            if data[j + best_len:j + best_len + 1] == data[i + best_len:i + best_len + 1] or best_len == 0:
                L = 0
                while i + L < n and data[j + L] == data[i + L] and L < NICE:
                    L += 1
                off = i - j
                if L >= 3 or (L == 2 and off <= 0x500):
                    if best_len == 0 or L * 9 - match_cost(off, L, last) > best_len * 9 - match_cost(best_off, best_len, last):
                        best_len, best_off = L, off
                if best_len >= NICE:
                    break
            j = prev[j]
        if best_len and best_off > 0x500 and best_len < 3:
            return 0, 0
        return best_len, best_off

    i = 0
    while i < n:
        L, off = find(i)
        if L and i + 1 < n:
            # lazy: a better match one byte later wins
            insert(i)
            L2, off2 = find(i + 1)
            if L2 > L + 1 or (L2 == L + 1 and match_cost(off2, L2, last) <= match_cost(off, L, last) + 9):
                b.bit(1)
                b.byte(data[i])
                i += 1
                L, off = L2, off2
            else:
                prev[i] = prev[i]                  # already inserted
            emit_match = True
        else:
            emit_match = bool(L)
            if not L:
                insert(i)
        if not emit_match:
            b.bit(1)
            b.byte(data[i])
            i += 1
            continue
        b.bit(0)
        m = L - 1 - (off > 0x500)
        lo, hi = m & 1, (m >> 1) & 1
        if m > 3:
            lo, hi = 0, 0
        if off == last:
            for x in _offset_gamma(2):
                b.bit(x)
            b.bit(hi)
        else:
            X = (off - 1) * 2 + (1 - hi)
            for x in _offset_gamma((X >> 8) + 3):
                b.bit(x)
            b.byte(X & 0xFF)
            last = off
        b.bit(lo)
        if m > 3:
            for x in _len_gamma(m - 2):
                b.bit(x)
        for k in range(i, i + L):
            if prev[k] == -1 and head.get(data[k:k + 3]) != k:
                insert(k)
        i += L
    # end marker
    b.bit(0)
    for x in _offset_gamma(0x1000002):
        b.bit(x)
    b.byte(0xFF)
    return bytes(b.out)


def roundtrip(packed, original):
    """Decompress with the boot loader's own src/nrv2d.c on the host; True if it gives back `original`."""
    here = os.path.dirname(os.path.abspath(__file__))
    src = os.path.join(here, "..", "src", "nrv2d.c")
    with tempfile.TemporaryDirectory() as t:
        c = os.path.join(t, "rt.c")
        open(c, "w").write(r'''#include <stdio.h>
#include <stdlib.h>
#include "nrv2d.h"
int main(int argc, char **argv) {
  FILE *f = fopen(argv[1], "rb"); fseek(f, 0, SEEK_END); long n = ftell(f); rewind(f);
  unsigned char *s = malloc(n); fread(s, 1, n, f); fclose(f);
  unsigned char *d = malloc(1 << 24); unsigned out = 0;
  int rc = nrv2d_decompress(s, n, d, d + (1 << 24), &out);
  f = fopen(argv[2], "wb"); fwrite(d, 1, out, f); fclose(f);
  return rc; }''')
        exe = os.path.join(t, "rt")
        subprocess.run(["cc", "-O2", "-I", os.path.join(here, "..", "include"), c, src, "-o", exe], check=True)
        pin, pout = os.path.join(t, "in"), os.path.join(t, "out")
        open(pin, "wb").write(packed)
        r = subprocess.run([exe, pin, pout])
        return r.returncode == 0 and open(pout, "rb").read() == original


def main():
    a = sys.argv[1:]
    if a == ["--test"]:
        import random
        rnd = random.Random(1)
        cases = [b"", b"a", b"ab" * 3, bytes(range(256)) * 9, bytes(rnd.randrange(4) for _ in range(5000)),
                 bytes(rnd.randrange(256) for _ in range(3000)) * 3, b"\0" * 70000]
        ok = all(roundtrip(compress(c), c) for c in cases)
        print("round trip:", "ok" if ok else "FAILED")
        return 0 if ok else 1
    if len(a) != 2:
        raise SystemExit(__doc__)
    data = open(a[0], "rb").read()
    packed = compress(data)
    if not roundtrip(packed, data):
        raise SystemExit("round trip through src/nrv2d.c FAILED")
    open(a[1], "wb").write(packed)
    print(f"{a[1]}: {len(data)} -> {len(packed)} bytes, round trip through src/nrv2d.c ok")
    return 0


if __name__ == "__main__":
    sys.exit(main())
