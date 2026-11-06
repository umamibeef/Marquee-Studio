#!/usr/bin/env python3
"""Build a boot-flash image ENTIRELY from our own source — the Mikohn dump is never read. Archive: BOOTSTRAP V2.00
(our bootloader) as entry 0, our app (NRV2D-compressed, bootable), optional stored content files, zero terminator.
Usage: mkimg.py BOOTSTRAP.bin APP.bin OUT.bin [NAME=PATH ...]"""
import os, sys, struct, zlib
sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..", "bootloader", "tools"))
import nrv2d_pack  # our NRV2D compressor

ARC_BOOTABLE = 0x00000001
ARC_NRV2D = 0x20000000


def check_word(body, flags):
    if flags & ARC_BOOTABLE:                       # bootable: (sum16 << 16) | xor16 over 16-bit halves
        s = x = 0
        for i in range(0, len(body), 2):
            h = (body[i] << 8) | (body[i + 1] if i + 1 < len(body) else 0)
            s = (s + h) & 0xFFFF
            x ^= h
        return ((s << 16) | x) & 0xFFFFFFFF
    return zlib.crc32(body) & 0xFFFFFFFF            # content: CRC-32


def make_entry(name, flags, data, unpacked):
    nm = name.encode()
    hdr = (len(nm) + 16) & ~3
    total = hdr + ((len(data) + 3) & ~3) + 4
    e = bytearray(struct.pack(">III", total, unpacked, flags) + bytes([len(nm)]) + nm)
    e += b"\0" * (hdr - len(e))
    e += data + b"\0" * (-len(data) % 4)
    e += struct.pack(">I", check_word(bytes(e), flags))
    return bytes(e)


def main(bootstrap, appbin, out, files):
    boot = bytearray(open(bootstrap, "rb").read())
    if boot[13:28] != b"BOOTSTRAP V2.00":
        raise SystemExit("bootstrap does not start with BOOTSTRAP V2.00")
    boot += b"\0" * (-len(boot) % 4)
    n = len(boot) + 4
    struct.pack_into(">II", boot, 0, n, n - 4)     # entry 0 header: total len, len-4 (as the bootstrap entry)
    boot += struct.pack(">I", zlib.crc32(bytes(boot)))
    img = bytearray(boot)

    app = open(appbin, "rb").read()
    packed = nrv2d_pack.compress(app)
    if not nrv2d_pack.roundtrip(packed, app):
        raise SystemExit("NRV2D round-trip failed")
    img += make_entry("UMAMI_APP", ARC_BOOTABLE | ARC_NRV2D, packed, len(app))
    for f in files:
        name, path = f.split("=", 1)
        img += make_entry(name, 0, open(path, "rb").read(), os.path.getsize(path))
    img += struct.pack(">I", 0)                     # zero terminator
    open(out, "wb").write(img)
    print(f"{out}: {len(img)} bytes — dump-free (bootstrap {len(boot)} + app {len(app)}->{len(packed)} packed"
          f"{', ' + str(len(files)) + ' files' if files else ''})")


if __name__ == "__main__":
    main(sys.argv[1], sys.argv[2], sys.argv[3], sys.argv[4:])
