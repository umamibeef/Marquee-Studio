#!/usr/bin/env python3
"""Run the firmware's fpga_configure (0x5A764) against a fake FPGA in Unicorn and check the decoded sequence:
  PROG (PA4) pulsed low, wait for INIT (PB17) high, PB19 low during the download, bitstream sent over SPI
  (spi_set_speed(0x670, 1 MHz), spi_transfer(tx = bitstream, rx, len)), DONE read on PIPR bit 0x02000000,
  success only when DONE is high and the SPI receive data is all 0xFF; otherwise it retries.
  The argument (eeprom_display_bit14, the A/E series bit) picks the design; EEPROM options bit 1 picks between
  copies that are byte-identical.
Usage: test_fpga.py
"""
import struct, sys
from unicorn import UC_HOOK_MEM_WRITE, UC_HOOK_MEM_READ
from unicorn.ppc_const import UC_PPC_REG_3, UC_PPC_REG_4, UC_PPC_REG_5, UC_PPC_REG_LR, UC_PPC_REG_PC
from fw import FW, SYM

IMMR = 0xFF000000
PADAT, PBDAT, PIPR = IMMR + 0x956, IMMR + 0xAC6, IMMR + 0xF0
PA4, PB17, PB19, DONE = 0x0800, 0x4000, 0x1000, 0x02000000


class FakeFPGA:
    def __init__(self, f, bad_readback=0):
        self.f, self.uc = f, f.uc
        self.events, self.sent, self.speed = [], [], None
        self.done = False
        self.bad_readback = bad_readback         # how many downloads come back with a non-0xFF byte
        uc = self.uc
        f.stub("delay_us_maybe", 0)
        f.stub("spi_lock", 0)
        f.stub("spi_unlock", 0)
        f.u16(PBDAT, PB17)                        # INIT high once PROG is released
        uc.hook_add(UC_HOOK_MEM_WRITE, self.on_write, begin=PADAT, end=PADAT + 1)
        uc.hook_add(UC_HOOK_MEM_WRITE, self.on_pb, begin=PBDAT, end=PBDAT + 1)
        uc.hook_add(UC_HOOK_MEM_READ, self.on_pipr, begin=PIPR, end=PIPR + 3)
        for name, h in (("spi_set_speed", self.on_speed), ("spi_transfer", self.on_transfer), ("spi_done", self.on_done)):
            a = SYM[name]
            uc.hook_add(1 << 2, h, begin=a, end=a)            # UC_HOOK_CODE

    def ret(self, v=0):
        self.uc.reg_write(UC_PPC_REG_3, v)
        self.uc.reg_write(UC_PPC_REG_PC, self.uc.reg_read(UC_PPC_REG_LR))

    def on_write(self, uc, access, addr, size, value, _):
        v = value if size == 2 else (value >> 8 if addr == PADAT else value)
        prog = bool(v & PA4)
        if not self.events or self.events[-1] != ("PROG", prog):
            self.events.append(("PROG", prog))
            if not prog:
                self.done = False                 # PROG low clears the FPGA

    def on_pb(self, uc, access, addr, size, value, _):
        cs = bool(value & PB19)
        if not self.events or self.events[-1] != ("PB19", cs):
            self.events.append(("PB19", cs))

    def on_pipr(self, uc, access, addr, size, value, _):
        uc.mem_write(PIPR, struct.pack(">I", DONE if self.done else 0))

    def on_speed(self, uc, addr, size, _):
        self.speed = (uc.reg_read(UC_PPC_REG_3) & 0xFFFF, uc.reg_read(UC_PPC_REG_4))
        self.ret()

    def on_transfer(self, uc, addr, size, _):
        tx, rx, n = (uc.reg_read(r) for r in (UC_PPC_REG_3, UC_PPC_REG_4, UC_PPC_REG_5))
        self.sent.append((tx, n))
        self.events.append(("SPI", tx, n))
        self.rx, self.n = rx, n
        reply = bytearray(b"\xff" * n)
        if self.bad_readback:
            self.bad_readback -= 1
            reply[n // 2] = 0x00                  # INIT pulled low during the download (CRC error)
        uc.mem_write(rx, bytes(reply))
        self.done = True
        self.ret()

    def on_done(self, uc, addr, size, _):
        self.f.u32(uc.reg_read(UC_PPC_REG_3), self.n)
        self.ret(1)


def run(arg, option_bit1, bad_readback=0):
    f = FW()
    f.stub("eeprom_option_bit1", option_bit1)
    fpga = FakeFPGA(f, bad_readback)
    f.call("fpga_configure", arg, limit=20_000_000)
    return fpga


ok = True


def check(name, cond, got=""):
    global ok
    ok &= bool(cond)
    print(f"  {'ok  ' if cond else 'FAIL'} {name}" + ("" if cond else f"  (got {got})"))


BS = {SYM[f"fpga_bitstream_{c}"]: c for c in "abcd"}
for arg, bit1, want in ((1, 0, "a"), (1, 1, "c"), (0, 0, "b"), (0, 1, "d")):
    fp = run(arg, bit1)
    tx, n = fp.sent[-1]
    check(f"arg {arg} (E series = 1), options bit1 {bit1} -> fpga_bitstream_{want}, 0x3541 bytes",
          (BS.get(tx), n) == (want, 0x3541), (BS.get(tx, hex(tx)), hex(n)))
fp = run(1, 0)
check("SPI at mode 0x670 (master, 8-bit, MSB first), 1 MHz", fp.speed == (0x670, 1_000_000), fp.speed)
seq = [e[0] if e[0] == "SPI" else f"{e[0]}={'hi' if e[1] else 'lo'}" for e in fp.events]
want = ["PB19=hi", "PROG=hi", "PROG=lo", "PROG=hi", "PB19=lo", "SPI", "PB19=hi"]
check("pin sequence: PROG pulse, then PB19 low around the SPI download", seq[-len(want):] == want, seq)
fp = run(1, 0, bad_readback=2)
check("a non-0xFF byte in the SPI readback makes it retry (3 downloads for 2 bad ones)", len(fp.sent) == 3, len(fp.sent))
a, c = (bytes(FW().read(SYM[f"fpga_bitstream_{x}"], 0x3544)) for x in "ac")
b, d = (bytes(FW().read(SYM[f"fpga_bitstream_{x}"], 0x3544)) for x in "bd")
check("bitstreams a == c and b == d (options bit 1 makes no difference in V4.20)", a == c and b == d)
check("the two designs differ (606 bytes)", sum(x != y for x, y in zip(a, b)) == 606)
print("ALL FPGA CHECKS PASS" if ok else "FAILURES")
sys.exit(0 if ok else 1)
