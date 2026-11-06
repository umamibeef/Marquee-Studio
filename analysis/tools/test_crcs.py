#!/usr/bin/env python3
"""Run the firmware's CRC routines (Unicorn) and check them against reference implementations,
including tools/signproto.py's Cougar frame CRC.
  crc16_ccitt_refl (0xB5B0)    (crc, byte) step, reflected CCITT: == signproto.crc16_ccitt_refl
  crc16_kermit_update (0xB5D4) (crc, buf, len): "123456789" -> 0x2189
  crc16_modbus (0xB614)        (buf, len): "123456789" -> 0x4B37
Usage: test_crcs.py
"""
import os, random, sys
from unicorn import Uc, UC_ARCH_PPC, UC_MODE_PPC32, UC_MODE_BIG_ENDIAN
from unicorn.ppc_const import UC_PPC_REG_1, UC_PPC_REG_3, UC_PPC_REG_LR

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, "..", "..", "tools"))
from signproto import crc16_ccitt_refl as signproto_crc

APP = open(os.path.join(HERE, "..", "extracted", "ELITE_RGB"), "rb").read()
BUF, STOP = 0x800000, 0xFFFF00
uc = Uc(UC_ARCH_PPC, UC_MODE_PPC32 | UC_MODE_BIG_ENDIAN)
uc.mem_map(0, 0x1000000)
uc.mem_write(0, APP)


def call(addr, *args):
    for i, v in enumerate(args):
        uc.reg_write(UC_PPC_REG_3 + i, v)
    uc.reg_write(UC_PPC_REG_1, 0xF00000)
    uc.reg_write(UC_PPC_REG_LR, STOP)
    uc.emu_start(addr, STOP, count=1_000_000)
    return uc.reg_read(UC_PPC_REG_3) & 0xFFFF


def refl(poly, init, data):
    c = init
    for b in data:
        c ^= b
        for _ in range(8):
            c = (c >> 1) ^ poly if c & 1 else c >> 1
    return c


ok = True
uc.mem_write(BUF, b"123456789")
for name, got, want in (("crc16_kermit_update", call(0xB5D4, 0, BUF, 9), 0x2189),
                        ("crc16_modbus", call(0xB614, BUF, 9), 0x4B37),
                        ("crc16_ccitt_refl_nibble (CCOM)", call(0x3A9F8, BUF, 9, 0), 0x2189),
                        ("crc16_ccitt_refl_nibble from 0xFFFF", call(0x3A9F8, BUF, 9, 0xFFFF), 0x6F91)):
    ok &= got == want
    print(f"  {'ok  ' if got == want else 'FAIL'} {name}('123456789') = {got:#06x} (want {want:#06x})")
rnd = random.Random(1)
same = True
for _ in range(200):
    data = bytes(rnd.randrange(256) for _ in range(rnd.randrange(1, 40)))
    c = 0
    for b in data:
        c = call(0xB5B0, c, b)
    same &= c == signproto_crc(data) == refl(0x8408, 0, data)
    uc.mem_write(BUF, data)
    same &= call(0xB614, BUF, len(data)) == refl(0xA001, 0xFFFF, data)
ok &= same
print(f"  {'ok  ' if same else 'FAIL'} 200 random frames: firmware == signproto (Cougar CRC) and MODBUS == reference")
# JWAM byte-stuffing (jwam_escape 0x317F0) on an sbuf {data, cap, len}
import struct
frame = bytes([0x03, 0x01, 0x00, 0x05, 0x10, 0x11, 0xFF, 0x04])
uc.mem_write(BUF, frame + bytes(32))
uc.mem_write(0x810000, struct.pack(">IHH", BUF, 0x400, len(frame)))
n_esc = call(0x317F0, 0x810000)
out = bytes(uc.mem_read(BUF, struct.unpack(">H", uc.mem_read(0x810006, 2))[0]))
want = bytes.fromhex("03 01 10 10 10 15 10 20 11 ff 04")
good = out == want and n_esc == 3
ok &= good
print(f"  {'ok  ' if good else 'FAIL'} jwam_escape: bytes < 0x11 -> 10, b+0x10 (got {out.hex(' ')})")
print("ALL CRC CHECKS PASS" if ok else "FAILURES")
sys.exit(0 if ok else 1)
