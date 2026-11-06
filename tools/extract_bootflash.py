#!/usr/bin/env python3
"""Walk the file archive that follows the BOOTSTRAP code in the boot flash and extract every entry.

Layout (worked out from the dump):
  0x00  u32 BE  ?            0x00002684
  0x04  u32 BE  archive offset (end of bootstrap code), 0x00002680
  0x0C  pascal string "BOOTSTRAP V2.00"
  0x100 PowerPC code (MPC8xx system-reset vector)
  archive entries from the offset above, each:
    u32 checksum, u32 length (whole entry incl. this 16-byte header), u32 size, u32 flags,
    u8 name length, name, zero padding, data
  flags 0x40000000 = zlib (size = inflated size); 0x20000001 = UCL NRV2D, 8-bit bit buffer (the bootstrap's
  decompressor at 0xC90, called by the loader at 0x1E74); 0 = stored

  extract_bootflash.py DUMP OUTDIR
"""
import os, struct, sys, zlib


def walk(d, start):
    off, entries = start, []
    while off + 17 <= len(d):
        ck, ln, size, flags = struct.unpack(">IIII", d[off:off + 16])
        nlen = d[off + 16]
        if flags not in (0, 0x40000000, 0x20000000, 0x20000001) or not 0 < nlen < 64 or ln < 17 or off + ln > len(d):
            break
        name = d[off + 17:off + 17 + nlen].decode("latin-1")
        hdr_end = off + 17 + nlen
        entries.append(dict(off=off, ck=ck, ln=ln, size=size, flags=flags, name=name, hdr_end=hdr_end))
        off += ln
    return entries, off


def lzss(src, n=4096, f=18, threshold=2, fill=0x20):
    """Okumura LZSS: flag byte LSB first, 1 = literal; refs are 12-bit ring position + 4-bit length."""
    ring = bytearray([fill]) * n
    r, out, i, flags = n - f, bytearray(), 0, 0
    while i < len(src):
        flags >>= 1
        if not flags & 0x100:
            flags = src[i] | 0xFF00
            i += 1
            if i >= len(src):
                break
        if flags & 1:
            c = src[i]; i += 1
            out.append(c); ring[r] = c; r = (r + 1) & (n - 1)
        else:
            if i + 1 >= len(src):
                break
            pos = src[i] | ((src[i + 1] & 0xF0) << 4)
            length = (src[i + 1] & 0x0F) + threshold + 1
            i += 2
            for k in range(length):
                c = ring[(pos + k) & (n - 1)]
                out.append(c); ring[r] = c; r = (r + 1) & (n - 1)
    return bytes(out)


def nrv2d_8(src):
    """UCL NRV2D decompressor with an 8-bit bit buffer, mirroring the bootstrap routine at 0xC90."""
    out = bytearray()
    st = {"bb": 0, "i": 0}

    def getbit():
        st["bb"] = (st["bb"] << 1) & 0xFFFFFFFF
        if not st["bb"] & 0xFF:
            st["bb"] = src[st["i"]] * 2 + 1
            st["i"] += 1
        return (st["bb"] >> 8) & 1

    last_off = 1
    while True:
        while getbit():
            out.append(src[st["i"]]); st["i"] += 1
        m_off = 1
        while True:
            m_off = m_off * 2 + getbit()
            if getbit():
                break
            m_off = (m_off - 1) * 2 + getbit()
        if m_off == 2:
            m_off = last_off
            m_len = getbit()
        else:
            m_off = ((m_off - 3) << 8) + src[st["i"]]; st["i"] += 1
            if m_off == 0xFFFFFFFF or m_off < 0 or m_off == -1:
                break
            if (m_off & 0xFFFFFFFF) == 0xFFFFFFFF:
                break
            m_len = (~m_off) & 1
            m_off = (m_off >> 1) + 1
            last_off = m_off
        m_len = m_len * 2 + getbit()
        if m_len == 0:
            m_len = 1
            while True:
                m_len = m_len * 2 + getbit()
                if getbit():
                    break
            m_len += 2
        if m_off > 0x500:
            m_len += 1
        pos = len(out) - m_off
        if pos < 0:
            raise ValueError(f"match before start at out {len(out)}")
        for _ in range(m_len + 1):
            out.append(out[pos]); pos += 1
    return bytes(out), st["i"]


def payload(d, e):
    """Return (data, how) for an entry: zlib-inflate if flagged, else the stored bytes."""
    raw = d[e["hdr_end"]:e["off"] + e["ln"]]
    if e["flags"] == 0x40000000:
        start = raw.find(b"\x78\x9c")
        if start < 0:
            start = raw.find(b"\x78\xda")
        out = zlib.decompressobj().decompress(raw[start:])
        return out, f"zlib at +{start}"
    if e["flags"] & 0x20000000:  # the loader only tests this bit (andis. 0x2000 at 0x1E14)
        start = 0
        while start < 4 and raw[start] == 0:
            start += 1
        out, used = nrv2d_8(raw[start:])
        return out, f"nrv2d +{start} used {used}"
    # stored: skip the zero padding after the name
    start = 0
    while start < len(raw) and raw[start] == 0 and start < 4:
        start += 1
    return raw[start:start + e["size"]], f"stored at +{start}"


def main():
    dump, outdir = sys.argv[1], sys.argv[2]
    d = open(dump, "rb").read()
    start = struct.unpack(">I", d[4:8])[0]
    entries, end = walk(d, start)
    print(f"archive at {start:#x}: {len(entries)} entries, ends at {end:#x} (dump is {len(d):#x})")
    os.makedirs(outdir, exist_ok=True)
    for e in entries:
        try:
            data, how = payload(d, e)
        except zlib.error as ex:
            data, how = b"", f"zlib error {ex}"
        crc_ok = "crc32 ok" if zlib.crc32(data) == e["ck"] else ("adler ok" if zlib.adler32(data) == e["ck"] else "chk ?")
        if e["flags"] == 0x20000001:   # NRV2D, size field = compressed length (0x20000000: unpacked length)
            used = int(how.split("used ")[1]) if "used " in how else -1
            size_ok = "packed size ok" if used == e["size"] else f"packed {used} != {e['size']}"
        else:
            size_ok = "size ok" if len(data) == e["size"] else f"size {len(data)} != {e['size']}"
        print(f"  {e['off']:#07x} {e['name']:24} len {e['ln']:6d} size {e['size']:7d} "
              f"{ {0x40000000: 'zlib', 0x20000001: 'nrv2d', 0x20000000: 'nrv2d', 0: 'raw'}[e['flags']]:5}  {how:22} {size_ok:20} {crc_ok}")
        safe = e["name"].replace("/", "_").replace("\\", "_") or f"entry_{e['off']:x}"
        with open(os.path.join(outdir, safe), "wb") as f:
            f.write(data)
    tail = d[end:]
    nz = len(tail) - tail.count(0xFF) - tail.count(0)
    print(f"after the archive: {len(tail)} bytes, {nz} not 0x00/0xFF")
    with open(os.path.join(outdir, "_bootstrap_code.bin"), "wb") as f:
        f.write(d[:start])


if __name__ == "__main__":
    main()
