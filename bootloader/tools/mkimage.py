#!/usr/bin/env python3
"""Build a boot flash image: the rebuilt bootstrap as archive entry 0, followed by every
other entry of an existing boot flash dump, unchanged.

Usage: mkimage.py build/bootstrap.bin ../dumps/bootflash.bin build/bootflash.bin [--add ENTRY ...] [--app ELITE_RGB]
                  [--stock-boot]
       --add appends ready-made archive entries (e.g. the PATCH entry from tools/mkpatch.py)
       --app replaces the ELITE_RGB entry with this raw app binary, NRV2D-compressed as the original
             (tools/nrv2d_pack.py; the entry is decompressed again and compared before the image is written)
       --stock-boot keeps the dump's own bootstrap (entry 0) byte for byte instead of build/bootstrap.bin
       --file NAME=PATH appends PATH as a stored (uncompressed) entry named NAME, e.g. the native-emoji sheet
       --replace NAME=PATH puts PATH, stored, in place of the existing entry NAME, e.g. the start-up animation logo_s.flc
       --drop NAME leaves the entry NAME out, e.g. a stock font the image does not keep

Entry 0 gets the same header the original carries: length (including the check word),
length - 4, flags 0, name "BOOTSTRAP V2.00", and a CRC-32 check word over the entry.
The other entries are position-independent (the bootstrap and the app walk the chain by
length), so they can move when the bootstrap size changes. The chain ends with a zero
word and the rest of the 512 KB chip is filled with 0x00, as in the original."""
import os, struct, sys, zlib

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

FLASH_SIZE = 512 * 1024


def entries(img, start):
    """Yield (offset, length) for each archive entry from `start` to the zero terminator."""
    off = start
    while off + 4 <= len(img):
        n, = struct.unpack_from(">I", img, off)
        if n in (0, 0xFFFFFFFF):
            return
        if off + n > len(img):
            raise SystemExit(f"entry at {off:#x} runs past the image ({n:#x} bytes)")
        yield off, n
        off = (off + n + 3) & ~3


def check_word(entry, n):
    """CRC-32, or the bootable sum/xor word when flag bit 0 is set."""
    flags, = struct.unpack_from(">I", entry, 8)
    body = entry[:n - 4]
    if flags & 1:
        s = x = 0
        for i in range(0, len(body), 4):
            w, = struct.unpack_from(">I", body, i)
            s += (w >> 16) + (w & 0xFFFF)
            x ^= (w >> 16) ^ (w & 0xFFFF)
        return ((s << 16) | x) & 0xFFFFFFFF
    return zlib.crc32(body)


def make_zentry(name, raw):
    """A zlib-compressed entry (flag 0x40000000), as the original's content entries: the second word is the
    unpacked size, the data is a zlib stream."""
    z = zlib.compress(raw, 9)
    e = bytearray(make_entry(name, 0x40000000, z))
    struct.pack_into(">I", e, 4, len(raw))
    n, = struct.unpack_from(">I", e, 0)
    struct.pack_into(">I", e, n - 4, check_word(bytes(e[:n - 4]) + b"\0" * 4, n))
    return bytes(e)


def make_entry(name, flags, data):
    """An archive entry as the original carries them: total length (header, data padded to 4, check word), the
    unpadded data length, flags, name; check word by check_word()."""
    nm = name.encode()
    hdr_len = (len(nm) + 16) & ~3
    total = hdr_len + ((len(data) + 3) & ~3) + 4
    e = bytearray(struct.pack(">III", total, len(data), flags) + bytes([len(nm)]) + nm)
    e += b"\0" * (hdr_len - len(e))
    e += data + b"\0" * (-len(data) % 4)
    e += struct.pack(">I", check_word(bytes(e) + b"\0" * 4, total))
    return bytes(e)


def main(boot_path, dump_path, out_path, extra=(), app=None, stock_boot=False, files=(), replace=(), drop=()):
    boot = bytearray(open(boot_path, "rb").read())
    dump = open(dump_path, "rb").read()
    if stock_boot:
        n0, = struct.unpack_from(">I", dump, 0)
        boot = bytearray(dump[:n0 - 4])

    if boot[13:28] != b"BOOTSTRAP V2.00":
        raise SystemExit("bootstrap.bin does not start with the BOOTSTRAP V2.00 header")
    boot += b"\0" * (-len(boot) % 4)
    n = len(boot) + 4
    struct.pack_into(">II", boot, 0, n, n - 4)
    boot += struct.pack(">I", zlib.crc32(bytes(boot)))
    if stock_boot:
        assert bytes(boot) == dump[:n], "--stock-boot: entry 0 does not round-trip"

    # Copy every entry after the original bootstrap, verifying each one as we go.
    orig_boot_len, = struct.unpack_from(">I", dump, 0)
    first = (orig_boot_len + 3) & ~3
    out = bytearray(boot)
    names = []
    replace = dict(replace)
    drop = set(drop)
    for off, ln in entries(dump, first):
        e = dump[off:off + ln]
        stored, = struct.unpack_from(">I", e, ln - 4)
        if check_word(e, ln) != stored:
            raise SystemExit(f"entry at {off:#x} fails its check word in the dump")
        name = e[13:13 + e[12]].decode()
        if name in drop:
            drop.discard(name)
            print(f"{name} dropped ({ln} bytes)")
            continue
        if app is not None and name == "ELITE_RGB":
            flags, = struct.unpack_from(">I", e, 8)
            if flags != 0x20000001:
                raise SystemExit(f"ELITE_RGB entry has flags {flags:#x}, expected NRV2D bootable 0x20000001")
            import nrv2d_pack
            raw = open(app, "rb").read()
            packed = nrv2d_pack.compress(raw)
            if not nrv2d_pack.roundtrip(packed, raw):
                raise SystemExit("--app: NRV2D round trip through src/nrv2d.c failed")
            e = make_entry(name, flags, packed)
            print(f"ELITE_RGB replaced: {len(raw)} bytes -> {len(packed)} NRV2D (original entry {ln} bytes, now {len(e)})")
            app = None
        elif name in replace:
            raw = open(replace.pop(name), "rb").read()
            flags, = struct.unpack_from(">I", e, 8)
            zipped = flags == 0x40000000                   # keep the original's codec: zlib or stored
            e = make_zentry(name, raw) if zipped else make_entry(name, 0, raw)
            print(f"{name} replaced: entry {ln} bytes, now {len(e)} ({'zlib' if zipped else 'stored'})")
        out += b"\0" * (-len(out) % 4)
        out += e
        names.append(name)
    if app is not None:
        raise SystemExit("--app: the dump has no ELITE_RGB entry")
    if replace:
        raise SystemExit(f"--replace: the dump has no entry {', '.join(replace)}")
    if drop:
        raise SystemExit(f"--drop: the dump has no entry {', '.join(sorted(drop))}")
    for name, path in files:                 # stored content entries, e.g. emoji.png / emoji.map
        if name in names:
            raise SystemExit(f"--file {name}: the image already has an entry of that name")
        e = make_entry(name, 0, open(path, "rb").read())
        out += b"\0" * (-len(out) % 4)
        out += e
        names.append(name)
    for path in extra:                       # extra ready-made entries, e.g. PATCH
        e = open(path, "rb").read()
        elen, = struct.unpack_from(">I", e, 0)
        if check_word(e, elen) != struct.unpack_from(">I", e, elen - 4)[0]:
            raise SystemExit(f"{path}: bad check word")
        out += b"\0" * (-len(out) % 4)
        out += e[:elen]
        names.append(e[13:13 + e[12]].decode())
    out += b"\0" * (-len(out) % 4)
    out += b"\0\0\0\0"                       # chain terminator

    if len(out) > FLASH_SIZE:
        raise SystemExit(f"image is {len(out)} bytes, over the {FLASH_SIZE} byte chip")
    out += b"\0" * (FLASH_SIZE - len(out))

    # Re-walk the result as the bootstrap would.
    walked = list(entries(out, 0))
    assert len(walked) == len(names) + 1, "re-walk found a different number of entries"
    for off, ln in walked:
        e = out[off:off + ln]
        assert check_word(e, ln) == struct.unpack_from(">I", e, ln - 4)[0], f"bad entry at {off:#x}"

    open(out_path, "wb").write(out)
    used = walked[-1][0] + walked[-1][1]
    print(f"{out_path}: bootstrap {n} bytes (original {orig_boot_len}), "
          f"{len(names)} entries copied, {used} of {FLASH_SIZE} bytes used, "
          f"{FLASH_SIZE - used} free")


if __name__ == "__main__":
    args = sys.argv[1:]
    extra = []
    while "--add" in args:
        i = args.index("--add")
        extra.append(args[i + 1])
        del args[i:i + 2]
    app = None
    if "--app" in args:
        i = args.index("--app")
        app = args[i + 1]
        del args[i:i + 2]
    replace = []
    while "--replace" in args:
        i = args.index("--replace")
        name, _, path = args[i + 1].partition("=")
        replace.append((name, path))
        del args[i:i + 2]
    drop = []
    while "--drop" in args:
        i = args.index("--drop")
        drop.append(args[i + 1])
        del args[i:i + 2]
    files = []
    while "--file" in args:
        i = args.index("--file")
        name, _, path = args[i + 1].partition("=")
        files.append((name, path))
        del args[i:i + 2]
    stock_boot = "--stock-boot" in args
    if stock_boot:
        args.remove("--stock-boot")
    if len(args) != 3:
        raise SystemExit(__doc__)
    main(*args, extra=extra, app=app, stock_boot=stock_boot, files=files, replace=replace, drop=drop)
