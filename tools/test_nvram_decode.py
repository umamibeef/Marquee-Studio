#!/usr/bin/env python3
"""Check tools/nvram_decode.py against the firmware itself.

Runs the real nvram_block_get (ELITE_RGB 0x5EFEC) under Unicorn:
  1. the firmware creates named blocks in an empty NVRAM -> nvram_decode must read back the same
     names, sizes and contents;
  2. nvram_decode.encode_block builds an NVRAM image -> the firmware must find each block (same
     data pointer) instead of appending a new one.
Usage: test_nvram_decode.py ELITE_RGB
"""
import struct
import sys

from unicorn import Uc, UC_ARCH_PPC, UC_MODE_PPC32, UC_MODE_BIG_ENDIAN, UC_HOOK_CODE
from unicorn.ppc_const import UC_PPC_REG_1, UC_PPC_REG_3, UC_PPC_REG_4, UC_PPC_REG_5, UC_PPC_REG_LR, UC_PPC_REG_PC

import nvram_decode as nd

NVRAM_GET = 0x5EFEC
NV_BASE, NV_SIZE = 0xF8780000, 0x8000
RAM_SIZE = 0x800000
STR_AREA, DEF_AREA, STACK, STOP = 0x700000, 0x710000, 0x7F0000, 0x7FFF00


class Firmware:
    def __init__(self, app, nvram):
        self.uc = uc = Uc(UC_ARCH_PPC, UC_MODE_PPC32 | UC_MODE_BIG_ENDIAN)
        uc.mem_map(0, RAM_SIZE)
        uc.mem_write(0, app)
        uc.mem_map(NV_BASE, NV_SIZE)
        uc.mem_write(NV_BASE, nvram)
        uc.mem_write(0x24, struct.pack(">II", NV_BASE, NV_SIZE))   # hand-off block: NVRAM base, size
        uc.hook_add(UC_HOOK_CODE, lambda u, a, s, d: u.emu_stop(), begin=STOP, end=STOP)
        self.str_next = STR_AREA

    def block_get(self, name, size, default):
        uc = self.uc
        sp = self.str_next
        uc.mem_write(sp, name.encode() + b"\0")
        self.str_next += (len(name) + 4) & ~3
        uc.mem_write(DEF_AREA, default)
        uc.reg_write(UC_PPC_REG_3, sp)
        uc.reg_write(UC_PPC_REG_4, size)
        uc.reg_write(UC_PPC_REG_5, DEF_AREA)
        uc.reg_write(UC_PPC_REG_1, STACK)
        uc.reg_write(UC_PPC_REG_LR, STOP)
        uc.emu_start(NVRAM_GET, STOP, count=50_000_000)
        assert uc.reg_read(UC_PPC_REG_PC) == STOP, "firmware did not return"
        return uc.reg_read(UC_PPC_REG_3)

    def nvram(self):
        return bytes(self.uc.mem_read(NV_BASE, NV_SIZE))


def main(app_path):
    app = open(app_path, "rb").read()
    msg = bytearray(0x1100)
    msg[0:2] = b"C1"
    msg[256:256 + 18] = b"WELCOME TO THE BAR"
    blocks = [("VERSION_STRING", b"V4.20".ljust(16, b"\0")),     # 14 chars: pad 2
              ("MESSAGE_TABLE", bytes(msg)),                     # 13 chars: pad 3
              ("TEXT_LINE_DATABASE", bytes(range(256)) * 2),    # 18 chars: pad 2
              ("ABCD", b"\x11\x22\x33\x44"),                     # 4 chars: pad 0 (NUL overwritten)
              ("JPDB", b"\xaa" * 100)]
    ok = True

    # 1. firmware writes, decoder reads
    fw = Firmware(app, bytes(NV_SIZE))
    ptrs = {}
    for name, data in blocks:
        ptrs[name] = fw.block_get(name, len(data), data)
    nv = fw.nvram()
    found = {name: (size, nv[doff:doff + size], ok_) for _, name, size, doff, ok_ in nd.walk(nv)}
    for name, data in blocks:
        got = found.get(name)
        good = got is not None and got[0] == len(data) and got[1] == data and got[2]
        good = good and ptrs[name] == NV_BASE + nv.index(data) if data in nv else good
        ok &= bool(good)
        print(f"{'ok  ' if good else 'FAIL'} firmware wrote {name!r} ({len(data)} bytes); decoder reads it back")
    lines = nd.describe("MESSAGE_TABLE", found["MESSAGE_TABLE"][1])
    good = lines == ["    line  0: 'C1'", "    line  1: 'WELCOME TO THE BAR'"]
    ok &= good
    print(f"{'ok  ' if good else 'FAIL'} MESSAGE_TABLE lines decoded: {[l.strip() for l in lines]}")

    # 2. encoder writes, firmware finds (same pointer, nothing appended)
    image = b"".join(nd.encode_block(n, d) for n, d in blocks)
    image = image + bytes(NV_SIZE - len(image))
    fw2 = Firmware(app, image)
    for name, data in blocks:
        p = fw2.block_get(name, len(data), b"\xee" * len(data))
        off = p - NV_BASE
        good = 0 <= off < NV_SIZE and fw2.nvram()[off:off + len(data)] == data
        ok &= good
        print(f"{'ok  ' if good else 'FAIL'} firmware finds encoder-written {name!r} at NVRAM+0x{off:x}")
    good = fw2.nvram() == image
    ok &= good
    print(f"{'ok  ' if good else 'FAIL'} firmware lookups left the encoder's NVRAM unchanged")

    # 3. same name, different size (e.g. after a firmware update changes a table): the firmware writes
    #    a new header starting at the OLD block's size field. The old header's CRC then fails, so on
    #    the next boot the walk wipes that block and every block after it. (Observed behaviour of the
    #    real code, recorded here so the decoder's report of such an NVRAM is understood.)
    fw3 = Firmware(app, image)
    mt = image.index(nd.encode_block("MESSAGE_TABLE", blocks[1][1]))
    fw3.block_get("MESSAGE_TABLE", 0x1000, b"\x00" * 0x1000)
    after = list(nd.walk(fw3.nvram()))
    valid = [n for o, n, s_, d_, k in after if k]
    broken = [o for o, n, s_, d_, k in after if not k]
    later = [n for n, d in blocks if image.index(nd.encode_block(n, d)) > mt]
    good = broken[:1] == [mt] and valid == ["VERSION_STRING"] and bool(later)
    print(f"{'ok  ' if good else 'FAIL'} size change on 'MESSAGE_TABLE' breaks its header at NVRAM+0x{mt:x}; "
          f"still valid: {valid}; lost at the next boot: {later}")
    ok &= good

    print("\nALL NVRAM CHECKS PASS" if ok else "\nFAILURES")
    return 0 if ok else 1


if __name__ == "__main__":
    if len(sys.argv) != 2:
        raise SystemExit(__doc__)
    sys.exit(main(sys.argv[1]))
