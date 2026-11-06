#!/usr/bin/env python3
"""Pack/unpack the sign's LFS content archive (the downloadable content image).
Entry: u32 chk, u32 entry_len(incl 16B hdr), u32 size, u32 flags, u8 namelen, name, pad-to-4, data.
flags (codec bits 0x70000000, as lfs_volume_append_unpacked 0x5D324 reads them): 0x40000000 zlib, 0x20000000 NRV2D,
0x10000000 bzip2 (bzip2 1.0.2 is linked in), else stored; size = unpacked size. Header: u32 ?, u32 archive_off(=0x10? we use 8), "BOOTSTRAP"
Note: this writes a *content* LFS (no bootstrap code); the sign extracts entries into RTEMS IMFS by name.
The per-entry chk is a non-standard checksum (unverified on extract); we store 0 and rely on the download
file-level CRC-32 for integrity. If the sign rejects chk, it can be filled once the algorithm is confirmed.
"""
import struct, zlib, os, sys

ZLIB, NRV2D, BZIP2 = 0x40000000, 0x20000000, 0x10000000

def pack(files, compress=True):
    """files: list of (name, bytes). Returns the LFS image bytes."""
    body = bytearray()
    for name, data in files:
        nb = name.encode("latin-1")
        if compress:
            comp = zlib.compress(data, 9)
            payload = comp; flags = ZLIB; size = len(data)
        else:
            payload = data; flags = 0; size = len(data)
        hdr_fixed = 17 + len(nb)
        pad = (-hdr_fixed) % 4
        entry_len = hdr_fixed + pad + len(payload)
        chk = zlib.crc32(data) & 0xFFFFFFFF   # placeholder; see note
        e = struct.pack(">IIII", chk, entry_len, size, flags) + bytes([len(nb)]) + nb + b"\x00" * pad + payload
        body += e
    header = struct.pack(">II", len(body) + 0x10, 0x10) + b"\x00" * 8
    return bytes(header) + bytes(body)

def unpack(img):
    out = []
    off = struct.unpack(">I", img[4:8])[0]
    while off + 17 <= len(img):
        chk, ln, size, flags = struct.unpack(">IIII", img[off:off+16])
        if ln < 17 or off + ln > len(img): break
        nl = img[off+16]; name = img[off+17:off+17+nl].decode("latin-1")
        raw = img[off + 17 + nl + (-(17 + nl)) % 4:off + ln]          # the data starts after the name's padding
        codec = flags & 0x70000000
        if codec == ZLIB:
            data = zlib.decompressobj().decompress(raw)[:size]     # any compression level; stops at the stream end
        elif codec == BZIP2:
            import bz2
            data = bz2.BZ2Decompressor().decompress(raw)[:size]
        elif codec == NRV2D:
            data = raw                       # no NRV2D decoder here (the boot loader's nrv2d.c has one): left packed
            name += " [NRV2D, packed]"
        else:
            data = raw[:size]
        out.append((name, data)); off += ln
    return out

if __name__ == "__main__":
    if sys.argv[1] == "pack":
        files = [(os.path.basename(p), open(p,"rb").read()) for p in sys.argv[3:]]
        open(sys.argv[2], "wb").write(pack(files)); print("wrote", sys.argv[2], len(files), "files")
    elif sys.argv[1] == "unpack":
        for n, d in unpack(open(sys.argv[2],"rb").read()):
            open(os.path.join(sys.argv[3], n.replace("/","_")), "wb").write(d); print(n, len(d))
