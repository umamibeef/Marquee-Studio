#!/usr/bin/env python3
"""Check docs/MESSAGE-MARKUP.md against the firmware: run the real message_text_next_token
(ELITE_RGB 0x46C78) under Unicorn on hand-built message lines and read back the textline fields.

Usage: test_message_markup.py      (reads extracted/ELITE_RGB and symbols.tsv)
"""
import os, re, struct, sys
from unicorn import Uc, UC_ARCH_PPC, UC_MODE_PPC32, UC_MODE_BIG_ENDIAN, UcError
from unicorn.ppc_const import UC_PPC_REG_1, UC_PPC_REG_3, UC_PPC_REG_4, UC_PPC_REG_LR, UC_PPC_REG_PC

HERE = os.path.dirname(os.path.abspath(__file__))
APP = open(os.path.join(HERE, "..", "extracted", "ELITE_RGB"), "rb").read()
SYM = {}
for line in open(os.path.join(HERE, "..", "symbols.tsv")):
    c = line.split("\t")
    if len(c) > 2 and re.fullmatch(r"[0-9a-f]{8}", c[0]):
        SYM[c[2]] = int(c[0], 16)

RAM = 0x2000000
STACK, STOP = 0x1F00000, 0x1FFFF00
TL, MSGS, TEXT_PIX, LINE_PIX, FONT_PIX = 0x1000000, 0x1100000, 0x1200000, 0x1400000, 0x1500000
LINE_STATE, LINE_REC = 0x3C2134, 0x118
TEXT_W, TEXT_H, LINE_W, LINE_H = 0x1414, 16, 80, 16

# textline offsets (struct textline in symbols.tsv)
O = dict(state=0, line=4, depth=5, entry_dir=0x18, exit_dir=0x1C, entry_speed=0x20, exit_speed=0x24, hold=0x28,
         text=0x2C, buf=0x34, box_pos=0x134, pos=0x13C, text_rect=0x144, line_img=0x154, text_img=0x168,
         font=0x17C, proportional=0x794, fg=0x798, bg=0x79A, sl=0x79C)


class Sign:
    def __init__(self, lines):
        uc = self.uc = Uc(UC_ARCH_PPC, UC_MODE_PPC32 | UC_MODE_BIG_ENDIAN)
        uc.mem_map(0, RAM)
        uc.mem_write(0, APP)
        table = bytearray(17 * 256)
        for n, data in lines.items():
            table[n * 256:n * 256 + len(data)] = data
        uc.mem_write(MSGS, bytes(table))
        uc.mem_write(SYM["message_table_ptr"], struct.pack(">I", MSGS))
        w = lambda off, fmt, *v: uc.mem_write(TL + off, struct.pack(">" + fmt, *v))
        w(O["text"], "IHH", TL + O["buf"], 0x100, 0)                         # sbuf over buf
        w(O["line_img"], "IIIII", 1, LINE_W, LINE_H, LINE_PIX, 0)
        w(O["text_img"], "IIIII", 1, TEXT_W, TEXT_H, TEXT_PIX, 0)
        # a blank 16x16-grid font, 8x8 cells (layout 2, image 128 x 128 RGB555)
        w(O["font"], "IIIIII", 2, 1, 128, 128, FONT_PIX, 0)
        w(O["fg"], "HH", 0x7C00, 0)

    def u(self, off, fmt=">I"):
        return struct.unpack(fmt, self.uc.mem_read(TL + off, struct.calcsize(fmt)))[0]

    def next_token(self, line):
        uc = self.uc
        uc.mem_write(TL + O["line"], bytes([line]))
        uc.reg_write(UC_PPC_REG_3, TL)
        uc.reg_write(UC_PPC_REG_4, 0)
        uc.reg_write(UC_PPC_REG_1, STACK)
        uc.reg_write(UC_PPC_REG_LR, STOP)
        uc.emu_start(SYM["message_text_next_token"], STOP, count=20_000_000)
        assert uc.reg_read(UC_PPC_REG_PC) == STOP, "did not return"

    def text(self):
        n = self.u(O["text"] + 6, ">H")
        return bytes(self.uc.mem_read(TL + O["buf"], n))


ESC = b"\x1b"
ok = True


def check(name, cond, got=""):
    global ok
    ok &= bool(cond)
    print(f"  {'ok  ' if cond else 'FAIL'} {name}" + (f"  (got {got})" if not cond else ""))


def main():
    print("1. plain text: one segment, scrolls in from the right, state 1 (sliding in)")
    s = Sign({0: b"HELLO"})
    s.next_token(0)
    check("text = HELLO", s.text() == b"HELLO", s.text())
    check("state 1", s.u(O["state"]) == 1, s.u(O["state"]))
    check("entry_dir 0 (from the right, the default)", s.u(O["entry_dir"]) == 0, s.u(O["entry_dir"]))

    print("2. colours: ESC '0' hi lo = fg, ESC '1' hi lo = bg (RGB555)")
    s = Sign({0: ESC + b"0\x03\xe0" + ESC + b"1\x00\x1f" + b"GREEN"})
    s.next_token(0)
    check("fg = 0x03E0", s.u(O["fg"], ">H") == 0x03E0, hex(s.u(O["fg"], ">H")))
    check("bg = 0x001F", s.u(O["bg"], ">H") == 0x001F, hex(s.u(O["bg"], ">H")))
    check("text = GREEN", s.text() == b"GREEN", s.text())

    print("3. entry: ESC '@'..'C' speed = from right / left / bottom / top")
    for cmd, d in ((b"@", 0), (b"A", 1), (b"B", 2), (b"C", 3)):
        s = Sign({0: ESC + cmd + b"\x07" + b"X"})
        s.next_token(0)
        check(f"ESC {cmd.decode()} -> entry_dir {d}, entry_speed 7",
              (s.u(O["entry_dir"]), s.u(O["entry_speed"])) == (d, 7), (s.u(O["entry_dir"]), s.u(O["entry_speed"])))

    print("4. exit: ESC 'P'..'S' hold speed = hold*4 frames, then exit left / right / up / down")
    for cmd, d in ((b"P", 0), (b"Q", 1), (b"R", 2), (b"S", 3)):
        s = Sign({0: ESC + cmd + b"\x0a\x05" + b"X"})
        s.next_token(0)
        got = (s.u(O["exit_dir"]), s.u(O["hold"]), s.u(O["exit_speed"]))
        check(f"ESC {cmd.decode()} 10 5 -> exit_dir {d}, hold 40, exit_speed 5", got == (d, 40, 5), got)

    print("5. segments: CR ends one; the next call returns the next")
    s = Sign({0: b"ONE\rTWO"})
    s.next_token(0)
    check("first = ONE", s.text() == b"ONE", s.text())
    s.next_token(s.u(O["line"]) >> 24)
    t2 = s.text()        # the CR ends the first run and is kept as the first byte of the next
    check("second = \\rTWO (the CR starts the next segment)", t2 == b"\rTWO", t2)

    print("6. include: ESC 0x80 n shows line n, then comes back")
    s = Sign({0: b"A" + ESC + b"\x80\x05" + b"C", 5: b"B"})
    seen = []
    for _ in range(4):
        s.next_token(s.u(O["line"]) >> 24)
        if s.u(O["state"]) == 4:
            break
        seen.append(s.text())
    check("segments A, B, C", seen == [b"A", b"B", b"C"], seen)

    print("7. end: NUL at the end of the outermost line -> state 4 (done)")
    s = Sign({0: b"Z"})
    s.next_token(0)
    s.next_token(0)
    check("state 4", s.u(O["state"]) == 4, s.u(O["state"]))

    print("\nALL MARKUP CHECKS PASS" if ok else "\nFAILURES")
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
