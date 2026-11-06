#!/usr/bin/env python3
"""Wrap a raw application binary (linked at address 0, like ELITE_RGB) as a bootable archive
entry that BOOTSTRAP V2.00 accepts: flag bit 0 set, stored uncompressed, sum/xor check word.
The bootstrap copies such an entry straight to address 0 and jumps to it.

Upload the result in the network window (`tftp> put app.img`), or add it to a flash image.

Usage: mkapp.py ELITE_RGB app.img [name]"""
import struct, sys


def entry(data, name="ELITE_RGB", flags=0x00000001):
    nm = name.encode()
    hdr_len = (len(nm) + 16) & ~3
    data = data + b"\0" * (-len(data) % 4)
    total = hdr_len + len(data) + 4
    e = bytearray(struct.pack(">III", total, len(data), flags) + bytes([len(nm)]) + nm)
    e += b"\0" * (hdr_len - len(e))
    e += data
    s = x = 0
    for i in range(0, len(e), 4):
        w, = struct.unpack_from(">I", e, i)
        s += (w >> 16) + (w & 0xFFFF)
        x ^= (w >> 16) ^ (w & 0xFFFF)
    e += struct.pack(">I", ((s << 16) | x) & 0xFFFFFFFF)
    return bytes(e)


if __name__ == "__main__":
    if len(sys.argv) not in (3, 4):
        raise SystemExit(__doc__)
    raw = open(sys.argv[1], "rb").read()
    out = entry(raw, sys.argv[3] if len(sys.argv) == 4 else "ELITE_RGB")
    open(sys.argv[2], "wb").write(out + b"\0" * 4)       # + chain terminator
    print(f"{sys.argv[2]}: bootable uncompressed entry, {len(raw)} bytes of code, {len(out)} bytes total")
