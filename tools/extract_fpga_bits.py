#!/usr/bin/env python3
"""Make firmware/app/fpga_bits.h, the panel FPGA's bitstream, on a machine that has the sign's firmware.

The bitstream is the configuration of a separate chip, kept with the hardware, and it comes from the sign, so it is
never committed (.gitignore). Run this once; the firmware's build then reads the header and never the firmware image.
Usage: extract_fpga_bits.py APP_IMAGE [OUT]   (APP_IMAGE: the unpacked app, E-series design at 0xA216C)"""
import sys

OFFSET, SIZE, FNV = 0xA216C, 13633, 0x92CE6125


def main(argv):
    data = open(argv[1], "rb").read()[OFFSET:OFFSET + SIZE]
    h = 2166136261
    for c in data:
        h = ((h ^ c) * 16777619) & 0xFFFFFFFF
    if h != FNV:
        raise SystemExit(f"not the expected bitstream: fnv {h:08X}")
    out = argv[2] if len(argv) > 2 else "firmware/app/fpga_bits.h"
    with open(out, "w") as f:
        f.write("#include <stdint.h>\n#define FPGA_BITS_LEN %d\nstatic const uint8_t fpga_bits[FPGA_BITS_LEN] = {%s};\n"
                % (SIZE, ",".join(map(str, data))))
    print("wrote", out)


if __name__ == "__main__":
    main(sys.argv)
