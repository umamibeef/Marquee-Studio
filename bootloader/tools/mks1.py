#!/usr/bin/env python3
"""Wrap a stage-1 boot loader binary (make DUMP=1 STAGE1=1) in its header (include/serdump.h struct s1_header).

The header: magic "MQS1", the code's length, its CRC-32 (zlib), the entry offset (0x20, right after the header) and a
version string. Stage 0 checks magic, length and CRC before it runs it.

Usage: mks1.py bootstrap.bin stage1.img [--version TEXT]
"""
import argparse
import struct
import zlib

MAGIC, HDR, LOADER_SIZE = 0x4D515331, 0x20, 0x10000

ap = argparse.ArgumentParser()
ap.add_argument("code")
ap.add_argument("out")
ap.add_argument("--version", default="")
a = ap.parse_args()
code = open(a.code, "rb").read()
code += b"\0" * (-len(code) % 4)
if HDR + len(code) > LOADER_SIZE:
    raise SystemExit(f"stage 1 is {len(code)} bytes; the loader sector holds {LOADER_SIZE - HDR}")
ver = a.version.encode()[:15]
img = struct.pack(">IIII16s", MAGIC, len(code), zlib.crc32(code), HDR, ver) + code
open(a.out, "wb").write(img)
print(f"{a.out}: stage 1, {len(code)} bytes of code, crc {zlib.crc32(code):08x}, version {ver.decode() or '-'}")
