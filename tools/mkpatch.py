#!/usr/bin/env python3
"""Build the "PATCH" archive entry the bootloader (make PATCH=1) applies to ELITE_RGB in RAM.

Entry payload (big-endian):
  'MKPT'  u32 version (1)  u32 load_addr  u32 code_len  u32 n_patches
  n x { u32 address, u32 expected_old_word, u32 new_word }
  code bytes (code_len)
The bootloader checks every expected word first and applies nothing unless all match, then copies the
code to load_addr and writes the new words. The original firmware on the flash is never modified.

Patches for the colour-emoji code (bootloader/patch):
  0x42DB0  stwu r1,-144(r1)  -> b emoji_hook        detour font_draw_char
  0x44BC0  li r5,0           -> li r5,1             font_load asks the PNG loader for alpha
  0x00010  heap-start offset -> enlarged            the C heap starts after the patch code

Usage: mkpatch.py patch.bin ELITE_RGB out.entry   (writes one archive entry, CRC-32 check word)
"""
import struct
import sys
import zlib

LOAD_ADDR = 0x00C60000
BSS_END = 0x0045CE44            # heap start = BSS_END + RAM[0x10] (ELITE_RGB FUN_000049b4)
FONT_DRAW_CHAR, FONT_LOAD_LI_R5 = 0x42DB0, 0x44BC0


def branch(src, dst):
    off = dst - src
    assert -0x2000000 <= off < 0x2000000 and not off & 3
    return 0x48000000 | (off & 0x03FFFFFC)


def word(app, addr):
    return struct.unpack_from(">I", app, addr)[0]


def build(code, app):
    heap_off_old = word(app, 0x10)
    heap_off_new = (LOAD_ADDR + len(code) + 0xFFF & ~0xFFF) - BSS_END
    assert heap_off_new > heap_off_old, "patch area must lie past the original heap gap"
    patches = [
        (FONT_DRAW_CHAR, 0x9421FF70, branch(FONT_DRAW_CHAR, LOAD_ADDR)),
        (FONT_LOAD_LI_R5, 0x38A00000, 0x38A00001),
        (0x10, heap_off_old, heap_off_new),
    ]
    for addr, old, _ in patches:
        assert word(app, addr) == old, f"ELITE_RGB at {addr:#x} is {word(app, addr):#010x}, expected {old:#010x}"
    payload = b"MKPT" + struct.pack(">IIII", 1, LOAD_ADDR, len(code), len(patches))
    payload += b"".join(struct.pack(">III", *p) for p in patches) + code
    return payload, patches, heap_off_new


def entry(name, payload, flags=0):
    nb = name.encode()
    hdr_len = (len(nb) + 16) & ~3
    payload = payload + b"\0" * (-len(payload) % 4)
    total = hdr_len + len(payload) + 4
    e = struct.pack(">III", total, len(payload), flags) + bytes([len(nb)]) + nb
    e = e.ljust(hdr_len, b"\0") + payload
    return e + struct.pack(">I", zlib.crc32(e))


if __name__ == "__main__":
    if len(sys.argv) != 4:
        raise SystemExit(__doc__)
    code = open(sys.argv[1], "rb").read()
    app = open(sys.argv[2], "rb").read()
    payload, patches, heap = build(code, app)
    open(sys.argv[3], "wb").write(entry("PATCH", payload))
    print(f"{sys.argv[3]}: PATCH entry, {len(code)} bytes of code at {LOAD_ADDR:#x}, {len(patches)} word patches; "
          f"heap now starts at {BSS_END + heap:#x}")
