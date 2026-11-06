#!/usr/bin/env python3
"""Boot the PATCH=1 flash image on the emulated board (test/cosim.py) and check what the app starts with.

For both memory paths, at the moment the bootloader jumps to the application:
  - the PATCH entry's word patches are in place and its code sits at its load address;
  - every other byte of RAM equals a boot of the faithful build (build/bootflash.bin).
And all-or-nothing: a PATCH entry with one wrong "expected" word leaves RAM exactly as the faithful build.

Usage: patchsim.py build/bootflash.bin build-patch/bootflash.bin build-patch/PATCH.entry
"""
import struct
import sys
import os

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import cosim

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "tools"))


def parse_patch(entry):
    n, = struct.unpack_from(">I", entry, 0)
    hdr = (entry[12] + 16) & ~3
    p = entry[hdr:n - 4]
    assert p[:4] == b"MKPT"
    _, load, clen, npatch = struct.unpack_from(">IIII", p, 4)
    patches = [struct.unpack_from(">III", p, 20 + i * 12) for i in range(npatch)]
    code = p[20 + npatch * 12:20 + npatch * 12 + clen]
    return load, code, patches


def boot(flash, sc):
    b = cosim.Board(flash, sc, None)
    b.run()
    return b, bytes(b.uc.mem_read(0, cosim.RAM_SIZE))


def corrupt(image, entry):
    """Same flash image, but the PATCH entry's first expected word is wrong (valid CRC kept)."""
    import zlib
    off = image.index(entry[:entry.index(b"PATCH") + 5])
    e = bytearray(entry)
    n, = struct.unpack_from(">I", e, 0)
    hdr = (e[12] + 16) & ~3
    struct.pack_into(">I", e, hdr + 24, struct.unpack_from(">I", e, hdr + 24)[0] ^ 1)   # patch 0 'expected'
    struct.pack_into(">I", e, n - 4, zlib.crc32(bytes(e[:n - 4])))
    return image[:off] + bytes(e) + image[off + len(e):]


def main(plain_path, patched_path, entry_path):
    plain, patched = open(plain_path, "rb").read(), open(patched_path, "rb").read()
    entry = open(entry_path, "rb").read()
    load, code, patches = parse_patch(entry)
    ok = True
    for name in ("fixed config, no upgrade (tries=0xFF)", "SPD SDRAM, one flash bank + CS7 (tries=0xFF)"):
        sc = cosim.SCENARIOS[name]
        print(f"\n=== {name} ===")
        _, ref = boot(plain, sc)
        b, got = boot(patched, sc)
        good = b.reached_app
        for addr, old, new in patches:
            w = struct.unpack_from(">I", got, addr)[0]
            g = w == new and struct.unpack_from(">I", ref, addr)[0] == old
            good &= g
            print(f"  {'ok  ' if g else 'FAIL'} word {addr:#08x}: {old:#010x} -> {w:#010x} (want {new:#010x})")
        g = got[load:load + len(code)] == code
        good &= g
        print(f"  {'ok  ' if g else 'FAIL'} patch code ({len(code)} bytes) at {load:#x}")
        touched = {a + k for a, _, _ in patches for k in range(4)} | set(range(load, load + len(code)))
        diff = [i for i in range(len(ref)) if ref[i] != got[i] and i not in touched]
        g = not diff
        good &= g
        print(f"  {'ok  ' if g else 'FAIL'} every other byte of RAM equals the faithful build's boot"
              + ("" if g else f" ({len(diff)} differ, first {diff[0]:#x})"))
        _, neg = boot(corrupt(patched, entry), sc)
        g = neg == ref
        good &= g
        print(f"  {'ok  ' if g else 'FAIL'} PATCH with one wrong expected word: nothing applied (RAM == faithful build)")
        ok &= good
    print("\nALL PATCH CHECKS PASS" if ok else "\nFAILURES")
    return 0 if ok else 1


if __name__ == "__main__":
    if len(sys.argv) != 4:
        raise SystemExit(__doc__)
    sys.exit(main(*sys.argv[1:]))
