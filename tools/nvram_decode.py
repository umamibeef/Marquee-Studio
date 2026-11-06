#!/usr/bin/env python3
"""Decode the sign's battery-backed NVRAM (e.g. nvram.bin from the network bootloader).

Format, from the firmware's nvram_block_get (ELITE_RGB 0x5EFEC):
  - The block chain starts at the NVRAM base and may use everything except the top 0x410 bytes
    (the boot-attempt counter lives up there, at offset 0x7FF0 on a 32 KB part).
  - Each block:  u8 H                     header length = 1 + name + NUL/pad (to 4) + 4 + 4
                 name, NUL, zero padding
                 u32 size  (big-endian)
                 u32 CRC-32 of the first H-4 header bytes (zlib CRC; the data is NOT covered)
                 size bytes of data
    The next block starts right after the data. H == 0 ends the chain.
  - The firmware finds a block by name AND size. If a header CRC fails, it wipes everything from that
    block to the end and falls back to RAM defaults.

Known blocks are decoded (MESSAGE_TABLE lines, VERSION_STRING, the machine ID tables); others get a
text preview.

Usage: nvram_decode.py nvram.bin [--dump NAME]   (prints a summary; --dump writes NAME's data to NAME.bin)
"""
import struct
import sys
import zlib

RESERVED_TOP = 0x410


def walk(nv):
    """Yield (offset, name, size, data_offset, header_ok) for each block, as the firmware walks them."""
    end = len(nv) - RESERVED_TOP
    p = 0
    while p < end:
        h = nv[p]
        if h == 0:
            return
        if p + h > len(nv):
            yield p, None, 0, p, False
            return
        crc = struct.unpack_from(">I", nv, p + h - 4)[0]
        ok = zlib.crc32(nv[p:p + h - 4]) == crc
        name = nv[p + 1:p + h - 8].split(b"\0")[0].decode("latin-1")
        size = struct.unpack_from(">I", nv, p + h - 8)[0]
        yield p, name, size, p + h, ok
        if not ok:
            return                      # the firmware would wipe from here
        p = p + h + size


def encode_block(name, data):
    """Build one block exactly as nvram_block_get appends it (used by the tests)."""
    nb = name.encode("latin-1")
    pad = (4 - (len(nb) & 3)) & 3       # firmware: 4 - (len & 3), but 0 when that is 4
    h = 1 + len(nb) + pad + 8
    hdr = bytes([h]) + nb + b"\0" * pad + struct.pack(">I", len(data))
    # NOTE: when pad == 0 the firmware writes no terminating NUL inside the header
    return hdr + struct.pack(">I", zlib.crc32(hdr)) + data


def text(b):
    return b.split(b"\0")[0].decode("latin-1", "replace")


def describe(name, data):
    out = []
    if name == "MESSAGE_TABLE":
        for i in range(len(data) // 256):
            t = text(data[i * 256:(i + 1) * 256])
            if t:
                out.append(f"    line {i:2d}: {t!r}")
        if not out:
            out.append("    (all lines empty)")
    elif name == "VERSION_STRING":
        out.append(f"    {text(data)!r}")
    elif name == "ID_TEXT_TABLE":
        for i in range(len(data) // 0x14):
            mid, t = struct.unpack_from(">I", data, i * 0x14)[0], text(data[i * 0x14 + 4:i * 0x14 + 0x14])
            if mid or t:
                out.append(f"    machine {i:2d}: id {mid} text {t!r}")
    elif name == "ALT_TEXT_TABLE":
        for i in range(len(data) // 16):
            t = text(data[i * 16:(i + 1) * 16])
            if t:
                out.append(f"    alt name {i:2d}: {t!r}")
    else:
        printable = "".join(chr(c) if 32 <= c < 127 else "." for c in data[:96])
        out.append(f"    {printable}")
    return out


def main(argv):
    if len(argv) < 2:
        raise SystemExit(__doc__)
    nv = open(argv[1], "rb").read()
    dump = argv[argv.index("--dump") + 1] if "--dump" in argv else None
    print(f"{argv[1]}: {len(nv)} bytes")
    n = 0
    for off, name, size, doff, ok in walk(nv):
        n += 1
        if name is None:
            print(f"  0x{off:05x}  header runs past the end")
            break
        print(f"  0x{off:05x}  {name:24s} {size:6d} bytes  {'header CRC ok' if ok else 'HEADER CRC BAD (firmware would wipe from here)'}")
        if ok:
            for line in describe(name, nv[doff:doff + size]):
                print(line)
        if ok and dump == name:
            open(name + ".bin", "wb").write(nv[doff:doff + size])
    print(f"  {n} blocks")
    if len(nv) >= 0x7FF2:
        print(f"  boot counter at 0x7FF0: tries 0x{nv[0x7FF0]:02x}, flags 0x{nv[0x7FF1]:02x}")


if __name__ == "__main__":
    main(sys.argv)
