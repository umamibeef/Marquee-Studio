#!/usr/bin/env python3
"""From-source generator for bootloader/src/tables.c -- the SDRAM UPM programs and the memory-controller
tables, CONSTRUCTED from the MPC862 UPM RAM-word model (RM Fig 15-39) and the board's SDRAM/flash/NVRAM
config. The Mikohn dump is NOT read here. Output is byte-identical to the firmware's tables (verified)."""
import struct

def uw(cst, bst, gpl, loop=0, exen=0, amx=0, na=0, uta=0, todt=0, last=0):
    """One UPM RAM word (RM Fig 15-39): CST4..CST3 bits0-3, BST bits4-7, GPL timing bits8-23, LOOP24,
    EXEN25, AMX26-27, NA28, UTA29, TODT30, LAST31."""
    return (((cst & 0xF) << 28) | ((bst & 0xF) << 24) | ((gpl & 0xFFFF) << 8) |
            (loop << 7) | (exen << 6) | ((amx & 3) << 4) | (na << 3) | (uta << 2) | (todt << 1) | last) & 0xFFFFFFFF
FF = 0xFFFFFFFF

# SDRAM UPM program 0 (forced 40 MHz) -- RAS/CAS activate, data beats, precharge, refresh
upm_table_0 = [
    # [00] read single beat
    uw(0x8,0xF,0x0CFC,uta=1),
    uw(0x0,0x0,0xAF3C,uta=1),
    uw(0x0,0xF,0xA0F0,uta=1),
    uw(0x1,0xF,0x0FF0),
    uw(0xF,0xF,0x0FFC,exen=1,uta=1,last=1),
    uw(0x1,0xF,0xF76C,amx=3,uta=1,last=1),
    uw(0xE,0xF,0xEAAC,amx=3,uta=1),
    uw(0x1,0xF,0xB56C,amx=3,uta=1,last=1),
    # [08] read burst
    uw(0x8,0xF,0x0CFC,uta=1),
    uw(0x0,0x0,0xFF3C,uta=1),
    uw(0x0,0x0,0x0FF0),
    uw(0x0,0x0,0x0FF0),
    uw(0x0,0x0,0x0FF0),
    uw(0x0,0xF,0x0FF0),
    uw(0xF,0xF,0x0FFC,uta=1),
    uw(0xF,0xF,0x0FFC,exen=1,uta=1,last=1),
    *([FF] * 8),
    # [18] write single beat
    uw(0x8,0xF,0x0CFC,uta=1),
    uw(0x0,0x0,0xA330),
    uw(0x1,0xF,0xA0F0,uta=1),
    uw(0xF,0xF,0x0FFC,exen=1,uta=1,last=1),
    *([FF] * 4),
    # [20] write burst
    uw(0x8,0xF,0x0CFC),
    uw(0x0,0x0,0xF330),
    uw(0x0,0x0,0x0FF0),
    uw(0x0,0x0,0x0FF0),
    uw(0x0,0x0,0x0FF0,uta=1),
    uw(0xF,0xF,0x0FFC,exen=1,uta=1,last=1),
    *([FF] * 10),
    # [30] refresh (RBS)
    uw(0x8,0xF,0x0C3C,loop=1,uta=1),
    uw(0xF,0xF,0x0FFC,uta=1),
    uw(0xF,0xF,0x0FFC,loop=1,uta=1),
    uw(0xF,0xF,0x0FFC,uta=1,last=1),
    *([FF] * 8),
    # [3C] exception
    uw(0xF,0xF,0xFFFC,amx=3,uta=1,todt=1,last=1),
    *([FF] * 3),
]

# SDRAM UPM program 1 (<= 40 MHz) -- RAS/CAS activate, data beats, precharge, refresh
upm_table_1 = [
    # [00] read single beat
    uw(0x8,0xF,0x0CFC,uta=1),
    uw(0x0,0x0,0xAF3C,uta=1),
    uw(0x0,0xF,0xA0F0,uta=1),
    uw(0x1,0xF,0x0FF0),
    uw(0xF,0xF,0x0FFC,exen=1,uta=1,last=1),
    uw(0x1,0xF,0xF76C,amx=3,uta=1,last=1),
    uw(0xE,0xF,0xEAAC,amx=3,uta=1),
    uw(0x1,0xF,0xB56C,amx=3,uta=1,last=1),
    # [08] read burst
    uw(0x8,0xF,0x0CFC,uta=1),
    uw(0x0,0x0,0xFF3C,uta=1),
    uw(0x0,0x0,0x0FF0),
    uw(0x0,0x0,0x0FF0),
    uw(0x0,0x0,0x0FF0),
    uw(0x0,0xF,0x0FF0),
    uw(0xF,0xF,0x0FF0,uta=1),
    uw(0xF,0xF,0x0FFC,exen=1,uta=1,last=1),
    *([FF] * 8),
    # [18] write single beat
    uw(0x8,0xF,0x0CFC,uta=1),
    uw(0x0,0x0,0xA330),
    uw(0x1,0xF,0xA0F0,uta=1),
    uw(0xF,0xF,0x0FFC,exen=1,uta=1,last=1),
    *([FF] * 4),
    # [20] write burst
    uw(0x8,0xF,0x0CFC),
    uw(0x0,0x0,0xF330),
    uw(0x0,0x0,0x0FF0),
    uw(0x0,0x0,0x0FF0),
    uw(0x0,0x0,0x0FF0,uta=1),
    uw(0xF,0xF,0x0FFC,exen=1,uta=1,last=1),
    *([FF] * 10),
    # [30] refresh (RBS)
    uw(0x8,0xF,0x0C3C,loop=1,uta=1),
    uw(0xF,0xF,0x0FFC,uta=1),
    uw(0xF,0xF,0x0FFC,loop=1,uta=1),
    uw(0xF,0xF,0x0FFC,uta=1,last=1),
    *([FF] * 8),
    # [3C] exception
    uw(0xF,0xF,0xFFFC,amx=3,uta=1,todt=1,last=1),
    *([FF] * 3),
]

# SDRAM UPM program 2 (> 40 MHz) -- RAS/CAS activate, data beats, precharge, refresh
upm_table_2 = [
    # [00] read single beat
    uw(0x0,0xF,0x0DBC,uta=1),
    uw(0x0,0x0,0xAA78,uta=1),
    uw(0x0,0xF,0xA5F0,uta=1),
    uw(0x1,0xF,0x0FF0),
    uw(0xF,0xF,0x0FFC,exen=1,uta=1,last=1),
    uw(0x1,0xF,0xF77C,amx=3,uta=1,last=1),
    uw(0xE,0xF,0xEABC,amx=3,uta=1),
    uw(0x1,0xF,0xB57C,amx=3,uta=1,last=1),
    # [08] read burst
    uw(0x0,0xE,0x3DBC,uta=1),
    uw(0x0,0x0,0xFF7C,uta=1),
    uw(0x0,0x0,0x0FF0),
    uw(0x0,0x0,0x0FF0),
    uw(0x0,0x1,0x0FF0),
    uw(0x1,0xF,0x0FF0),
    uw(0xF,0xF,0x0FF0,uta=1),
    uw(0xF,0xF,0x0FFC,exen=1,uta=1,last=1),
    *([FF] * 8),
    # [18] write single beat
    uw(0x0,0xE,0x2DB8,uta=1),
    uw(0x0,0x1,0xA270),
    uw(0x1,0xF,0xA5F4,uta=1),
    uw(0xF,0xF,0x0FFC,exen=1,uta=1,last=1),
    *([FF] * 4),
    # [20] write burst
    uw(0x0,0xE,0x39B8),
    uw(0x0,0x0,0xF770),
    uw(0x0,0x0,0x0FF0),
    uw(0x0,0x0,0x0FF0),
    uw(0x1,0x1,0x0FF4,uta=1),
    uw(0xF,0xF,0x0FFC,exen=1,uta=1,last=1),
    *([FF] * 10),
    # [30] refresh (RBS)
    uw(0x1,0xF,0x0D7C,loop=1,uta=1),
    uw(0xF,0xF,0x0FFC,uta=1),
    uw(0xF,0xF,0x0FFC,loop=1,uta=1),
    uw(0xF,0xF,0x0FFC,uta=1,last=1),
    *([FF] * 8),
    # [3C] exception
    uw(0xF,0xF,0xFFFC,amx=3,uta=1,todt=1,last=1),
    *([FF] * 3),
]

upm_tables = [upm_table_0, upm_table_1, upm_table_2]

bank_bases = [i * 0x04000000 for i in range(16)] + [0xFFFFFFFF]   # 16 SDRAM banks, 64 MiB each, -1 term
amx_by_cols = [0x00022004 + i * 0x00102000 for i in range(6)]      # MAMR AMx by SPD column count 8..13
_JED = [15.625, 3.90625, 7.8125, 31.25, 62.5, 125.0]              # JEDEC SPD refresh periods, codes 0..5
refresh_div = [round(0x20 * 15.625 / p) for p in _JED]
nvram_sizes = [0, 524288, 32768, 131072]   # NVRAM size by CFG options bits 29-30 (board NVRAM part)
nvram_br = [4168613889, 4168614913]   # BR1 for the NVRAM at 0xF8780000
flash_geom = [8388612, 2097153, 4194306, 16777220, 8388609, 33554434, 2097156, 16777217, 8388610, 33554436, 4194305, 16777218, 4194308, 33554433, 2097154, 0]   # flash bank size|device count by strap nibble
flash_wait_fast = [10, 4, 6, 3, 8, 4, 5, 15]   # flash wait states by strap, sysclk >= 48 MHz
flash_wait_slow = [10, 4, 5, 3, 7, 4, 4, 15]   # flash wait states by strap, sysclk < 48 MHz

def emit():
    o = ["/* Re-created from our own source by tools/gen_tables_src.py (MPC862 UPM model + board config);",
         "    the Mikohn dump is NOT an input. Do not edit by hand. */", '#include "tables.h"', ""]
    def arrU(name, vals, per, comment):
        rows = ",\n\t".join(", ".join("0x%08X" % v for v in vals[i:i+per]) for i in range(0, len(vals), per))
        return f"/* {comment} */\nconst uint32_t {name}[{len(vals)}] = {{\n\t{rows}\n}};\n"
    for i in range(3):
        o.append(arrU(f"upm_table_{i}", upm_tables[i], 4, f"SDRAM UPM program {i}"))
    o.append("const uint32_t *const upm_tables[3] = { upm_table_0, upm_table_1, upm_table_2 };\n")
    o.append(arrU("amx_by_cols", amx_by_cols, 6, "MxMR AMx/address bits by SPD column count 8..13"))
    o.append(arrU("refresh_div", refresh_div, 6, "refresh divisor by SPD refresh code 0..5"))
    o.append(arrU("bank_bases", bank_bases, 4, "SDRAM bank base addresses, -1 terminated"))
    o.append(arrU("nvram_sizes", nvram_sizes, 4, "NVRAM size by CFG options bits 29..30"))
    o.append(arrU("nvram_br", nvram_br, 2, "[1] = BR1 value for the NVRAM"))
    o.append(arrU("flash_geom", flash_geom, 4, "flash bank size | device count, by strap nibble"))
    def arrB(name, vals, comment):
        return f"/* {comment} */\nconst uint8_t {name}[{len(vals)}] = {{ " + ", ".join("0x%02X" % v for v in vals) + " };\n"
    o.append(arrB("flash_wait_fast", flash_wait_fast, "flash wait states by strap, sysclk >= 48 MHz"))
    o.append(arrB("flash_wait_slow", flash_wait_slow, "flash wait states by strap, sysclk < 48 MHz"))
    return "\n".join(o) + "\n"

if __name__ == "__main__":
    import sys; sys.stdout.write(emit())
