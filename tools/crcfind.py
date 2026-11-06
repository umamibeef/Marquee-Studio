# Which standard CRC-16 / checksum over which slice of the packet gives 58 3D?
pkt = bytes.fromhex("a5bb0000583d")
target = {0x583d: "big-endian 58 3D", 0x3d58: "little-endian 3D 58"}

def crc16(data, poly, init, refin, refout, xorout):
    def rev(x, n):
        return int(f"{x:0{n}b}"[::-1], 2)
    crc = init
    for b in data:
        if refin:
            b = rev(b, 8)
        crc ^= b << 8
        for _ in range(8):
            crc = ((crc << 1) ^ poly) & 0xFFFF if crc & 0x8000 else (crc << 1) & 0xFFFF
    if refout:
        crc = rev(crc, 16)
    return crc ^ xorout

# catalogue of CRC-16 variants (name, poly, init, refin, refout, xorout)
CRCS = [("CCITT-FALSE", 0x1021, 0xFFFF, 0, 0, 0), ("XMODEM", 0x1021, 0, 0, 0, 0),
        ("KERMIT", 0x1021, 0, 1, 1, 0), ("X-25", 0x1021, 0xFFFF, 1, 1, 0xFFFF),
        ("AUG-CCITT", 0x1021, 0x1D0F, 0, 0, 0), ("GENIBUS", 0x1021, 0xFFFF, 0, 0, 0xFFFF),
        ("MCRF4XX", 0x1021, 0xFFFF, 1, 1, 0), ("ARC/IBM", 0x8005, 0, 1, 1, 0),
        ("MODBUS", 0x8005, 0xFFFF, 1, 1, 0), ("BUYPASS", 0x8005, 0, 0, 0, 0),
        ("USB", 0x8005, 0xFFFF, 1, 1, 0xFFFF), ("MAXIM", 0x8005, 0, 1, 1, 0xFFFF),
        ("DNP", 0x3D65, 0, 1, 1, 0xFFFF), ("DECT-X", 0x0589, 0, 0, 0, 0)]
hits = []
for start in range(0, 4):
    for end in range(start + 1, 5):
        d = pkt[start:end]
        for name, *p in CRCS:
            v = crc16(d, *p)
            if v in target:
                hits.append(f"CRC-16/{name} over bytes {start}..{end - 1} ({d.hex(' ')}) = {target[v]}")
        s = sum(d)
        for label, v in (("sum16", s & 0xFFFF), ("~sum16", ~s & 0xFFFF), ("-sum16", -s & 0xFFFF)):
            if v in target:
                hits.append(f"{label} over bytes {start}..{end - 1} = {target[v]}")
print("\n".join(hits) or "no standard CRC-16 / checksum matched")
