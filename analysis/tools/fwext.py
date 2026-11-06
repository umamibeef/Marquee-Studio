#!/usr/bin/env python3
"""Add the firmware extensions (firmware/ext/*.c) to the from-source ELITE_RGB: build/fw/ELITE_RGB-ext + build/fw/ext/EXT.

fwbuild.py links ELITE_RGB from source at the stock layout. This step adds our own code without moving anything:
  - the extensions are linked as one block for RAM just past the C heap's old start, which moves up by RESERVE
    (the word at 0x10: heap start = BSS_END + that word). The block starts with the extension state (STATE_SIZE
    bytes, symbol ext_state / emoji_state), then the code;
  - the code travels as its own boot-flash archive entry, "EXT" (build/fw/ext/EXT; bootloader/tools/mkimage.py
    --file EXT=...), so its size is not limited by the image's free space. It is NRV2D-packed
    (bootloader/tools/nrv2d_pack.py); the code's last word, and the entry's, is a tag (a hash of the code) that ties
    it to this app;
  - a small loader, in the free space our compiled C left (gaps), takes over boot_card (the app's first C
    function, run once per start). It walks the boot flash archive (base from the hand-off word 0x2C) for the EXT
    entry; when the length and the tag match, it unpacks the code into the RAM block (ext_unpack,
    firmware/loader/unpack.c, in another gap), checks the unpacked length and tag, writes the data cache back,
    invalidates the instruction cache over it and continues at ext_boot_card with boot_card's arguments. When the
    entry is missing or does not match, it puts back the original first instruction of every taken-over function
    (the restore table, also in a gap) and continues at boot_card: the app then runs exactly as the stock-equivalent
    build;
  - a function named ext_<name> takes over the firmware function <name>: the first instruction at <name> becomes
    "b ext_<name>", and <name>__orig (a trampoline in the block) runs the displaced instruction and continues at
    <name> + 4, so the extension can call the original. Only functions starting with "stwu r1,-N(r1)" or "mflr r0"
    (position independent) can be taken over. firmware/ext must define ext_boot_card (it clears the state).
  - the version is ours: version_string itself (0xBDA90, "V4.20" -> VERSION, same length; jackpot_controller_init
    still loads it) and, in a gap, VERSION_FULL (VERSION plus the build time, "UM0.1-YYMMDDHHMM"), which
    boot_scroll_version and menu_show_version_maybe load, and the console and crash reports print (FW_VERSION). version_string is also the key of
    the NVRAM settings: version_string_check erases NVRAM to the defaults when the stored version differs, so the
    first boot of this firmware clears whatever the sign held (the casino's jackpot records, messages and settings;
    the sign's NVRAM was dumped first: tools/serdump.py, region nvram).
Every byte outside the gaps, the taken-over entries, the version loads and the heap word is checked to equal ELITE_RGB.

Usage: fwext.py   (after fwbuild.py)
"""
import glob
import os
import re
import struct
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import rebuild  # noqa: E402
from difftest import LLVM, LLD, CFLAGS, symbol_address  # noqa: E402

ANA = os.path.dirname(HERE)
REPO = os.path.dirname(ANA)
EXT_DIR = os.path.join(REPO, "firmware", "ext")
OUT = os.path.join(ANA, "build", "fw")
EXT_OUT = os.path.join(OUT, "ext")
UNPACK_SRC = os.path.join(REPO, "firmware", "loader", "unpack.c")
BSS_END = 0x0045CE44                        # heap start = BSS_END + word at 0x10 (FUN_000049b4, tools/mkpatch.py)
RESERVE = 0x10000                           # RAM block: state, then code (64 KB)
DMA_SCRATCH = 0x1100                        # the block's last 4.25 KB (console 4 KB, then the crash report's 128 B): buffers a DMA reads (ext_dma_scratch). The MMU maps
                                            # the SDRAM banks back to back virtually but 32 MB apart physically, so a
                                            # DMA buffer must lie in bank 0 (the first 16 MB, virtual = physical)
STATE_SIZE = 0x100
APPS_RAM = 0x900                            # just below DMA_SCRATCH: the apps' state (time base) and the RAM default of
                                            # their NVRAM block (firmware/ext/apps.c, ext_apps_ram)
STWU_R1 = 0x94210000                        # stwu r1,-N(r1)
MFLR_R0 = 0x7C0802A6                        # mflr r0
LINE = 16                                   # MPC8xx cache line


def run(*cmd):
    r = subprocess.run(cmd, capture_output=True, text=True)
    if r.returncode:
        raise SystemExit(f"{os.path.basename(cmd[0])} failed:\n{r.stderr[:2000]}")
    return r.stdout


def section_list(obj):
    """[(name, size)] of an object's allocated sections with contents or size."""
    out = []
    for m in re.finditer(r"\]\s+(\S+)\s+\S+\s+[0-9a-f]+\s+[0-9a-f]+\s+([0-9a-f]+)\s+\S+\s+(\S*)",
                         run(f"{LLVM}/llvm-readelf", "-S", "-W", obj)):
        if "A" in m.group(3) and int(m.group(2), 16):
            out.append((m.group(1), int(m.group(2), 16)))
    return out


def free_gaps(elf, image):
    """Free space in the replaced functions' slots: slot ranges minus the sections fwbuild placed there."""
    used = []
    for m in re.finditer(r"\]\s+(\S+)\s+\S+\s+([0-9a-f]{8})\s+[0-9a-f]+\s+([0-9a-f]+)",
                         run(f"{LLVM}/llvm-readelf", "-S", "-W", elf)):
        if m.group(1).startswith((".f_", ".slot_")):
            a = int(m.group(2), 16)
            used.append((a, a + int(m.group(3), 16)))
    rep = open(os.path.join(ANA, "build", "rebuild_report.txt")).read().split("\n\n", 1)[1]
    placed = [l.split("\t")[0] for l in rep.strip().split("\n") if l]
    byname = {n: (s, e) for s, e, n in rebuild.functions()}
    gaps = []
    for n in placed:
        s, e = byname[n]
        a = s
        for u0, u1 in sorted(used):
            if u1 <= s or u0 >= e:
                continue
            if u0 > a:
                gaps.append([a, u0])
            a = max(a, u1)
        if a < e:
            gaps.append([a, e])
    gaps = [[(a + 3) & ~3, b & ~3] for a, b in gaps if (b & ~3) - ((a + 3) & ~3) >= 8]
    for a, b in gaps:                       # only space fwbuild filled with traps
        assert all(image[x:x + 4] == rebuild.TRAP for x in range(a, b, 4)), f"gap {a:#x} is not free"
    return sorted(gaps, key=lambda g: g[1] - g[0], reverse=True)


def take(gaps, size):
    """The smallest gap that holds size bytes; consumes it."""
    best = min((g for g in gaps if g[1] - g[0] >= size), key=lambda g: g[1] - g[0], default=None)
    if best is None:
        raise SystemExit(f"no free gap of {size} bytes")
    a = best[0]
    best[0] += size
    return a


def loader_asm(restore, code_start, length, unpacked, tag, entry, boot_card):
    """Finds the EXT entry in the boot flash archive (base at RAM 0x2C, at most 512 KB), checks its data length and
    last word (tag), unpacks it to code_start (ext_unpack, firmware/loader/unpack.c: NRV2D), checks the unpacked
    length and the code's own last word (the same tag), syncs the caches and jumps to entry. Otherwise writes back
    the words of the restore table (address, word pairs, 0), syncs them and jumps to boot_card. The caller's
    r3..r5 and return address are left untouched (kept on the stack around the call)."""
    def li32(r, v):
        return f"  lis   %r{r}, {v >> 16 & 0xFFFF:#x}\n  ori   %r{r}, %r{r}, {v & 0xFFFF:#x}\n"
    return f"""  .section .text.ext_loader, "ax"
  .globl ext_loader
ext_loader:
  lwz   %r6, 0x2c(0)
  addis %r11, %r6, 8
1:
  cmplw %r6, %r11
  bge   7f
  lwz   %r7, 0(%r6)
  cmpwi %r7, 0
  beq   7f
  cmpwi %r7, -1
  beq   7f
  lbz   %r8, 12(%r6)
  cmpwi %r8, 3
  bne   2f
  lbz   %r8, 13(%r6)
  cmpwi %r8, 0x45
  bne   2f
  lbz   %r8, 14(%r6)
  cmpwi %r8, 0x58
  bne   2f
  lbz   %r8, 15(%r6)
  cmpwi %r8, 0x54
  beq   3f
2:
  add   %r6, %r6, %r7
  addi  %r6, %r6, 3
  rlwinm %r6, %r6, 0, 0, 29
  b     1b
3:
  lwz   %r8, 4(%r6)
{li32(9, length)}  cmplw %r8, %r9
  bne   7f
  addi  %r8, %r6, 16
  add   %r10, %r8, %r9
  lwz   %r10, -4(%r10)
{li32(12, tag)}  cmplw %r10, %r12
  bne   7f
  stwu  %r1, -32(%r1)
  mflr  %r0
  stw   %r0, 36(%r1)
  stw   %r3, 8(%r1)
  stw   %r4, 12(%r1)
  stw   %r5, 16(%r1)
  mr    %r3, %r8
{li32(4, code_start)}  bl    ext_unpack
{li32(9, unpacked)}  cmplw %r3, %r9
{li32(7, code_start)}  add   %r9, %r7, %r9
  bne   6f
  lwz   %r10, -4(%r9)
{li32(12, tag)}  cmplw %r10, %r12
6:
  lwz   %r0, 36(%r1)
  mtlr  %r0
  lwz   %r3, 8(%r1)
  lwz   %r4, 12(%r1)
  lwz   %r5, 16(%r1)
  addi  %r1, %r1, 32
  bne   7f
5:
  dcbst 0, %r7
  sync
  icbi  0, %r7
  addi  %r7, %r7, {LINE}
  cmplw %r7, %r9
  ble   5b
  sync
  isync
{li32(8, entry)}  mtctr %r8
  bctr
7:
{li32(6, restore)}8:
  lwz   %r7, 0(%r6)
  cmpwi %r7, 0
  beq   9f
  lwz   %r8, 4(%r6)
  stw   %r8, 0(%r7)
  dcbst 0, %r7
  sync
  icbi  0, %r7
  addi  %r6, %r6, 8
  b     8b
9:
  sync
  isync
{li32(8, boot_card)}  mtctr %r8
  bctr
"""


VERSION = "UM0.1"                             # the NVRAM key (5 characters, as "V4.20") and the controllers' version
import time
VERSION_FULL = VERSION + "-" + time.strftime("%y%m%d%H%M")   # what the sign shows and reports: with the build time
VERSION_STRING = 0xBDA90                     # "V4.20": the NVRAM key, left as it is
VERSION_USERS = ("boot_scroll_version", "menu_show_version_maybe")   # these show VERSION_FULL; the controllers
                                                                     # (jackpot_controller_init) keep VERSION
DEFINES_RT = {14, 15, 32, 33, 34, 35, 40, 41, 42, 43}   # addi addis lwz lwzu lbz lbzu lhz lhzu lha lhau: rT = bits 6-10


def function_end(start):
    """The next function's start in symbols.tsv (functions are laid out back to back)."""
    starts = sorted(int(l.split("\t")[0], 16) for l in open(os.path.join(ANA, "symbols.tsv"))
                    if l.count("\t") >= 2 and l.split("\t")[1] == "func")
    return next(a for a in starts if a > start)


def retarget(image, start, end, old, new):
    """Every 'lis rD,hi ; addi rX,rD,lo' in [start, end) that makes `old` is changed to make `new`. A site is only
    patched if rD is not read again before something redefines it (so the lis served this addi alone); otherwise,
    or on a branch first, the build stops. Returns the patched addresses."""
    word = lambda a: struct.unpack_from(">I", image, a)[0]
    sext = lambda v: v - 0x10000 if v & 0x8000 else v
    hit = []
    for a in range(start, end, 4):
        w = word(a)
        if w >> 26 != 15 or (w >> 16) & 31:            # lis rD, hi  (addis rD, 0, hi)
            continue
        rd, hi = (w >> 21) & 31, w & 0xFFFF
        for b in range(a + 4, min(a + 32, end), 4):
            v = word(b)
            if v >> 26 == 14 and (v >> 16) & 31 == rd and ((hi << 16) + sext(v & 0xFFFF)) & 0xFFFFFFFF == old:
                rx = (v >> 21) & 31
                # rD must not be read after this addi before it is redefined
                if rx != rd:
                    for c in range(b + 4, end, 4):
                        u = word(c)
                        op = u >> 26
                        if op in (16, 18, 19):
                            raise SystemExit(f"version retarget at {a:#x}: branch at {c:#x} before r{rd} is redefined")
                        if op in DEFINES_RT and (u >> 21) & 31 == rd and (u >> 16) & 31 != rd:
                            break
                        if rd in ((u >> 21) & 31, (u >> 16) & 31, (u >> 11) & 31):
                            raise SystemExit(f"version retarget at {a:#x}: r{rd} used again at {c:#x}")
                nhi, nlo = ((new + 0x8000) >> 16) & 0xFFFF, new & 0xFFFF
                struct.pack_into(">I", image, a, (w & 0xFFFF0000) | nhi)
                struct.pack_into(">I", image, b, (v & 0xFFFF0000) | nlo)
                hit += [a, b]
                break
            if (v >> 21) & 31 == rd or (v >> 16) & 31 == rd or (v >> 11) & 31 == rd:
                break                                   # rD used otherwise first: not this pattern
    return hit


def main():
    srcs = sorted(glob.glob(os.path.join(EXT_DIR, "*.c")))
    image = bytearray(open(os.path.join(OUT, "ELITE_RGB"), "rb").read())
    base_image = bytes(image)
    os.makedirs(EXT_OUT, exist_ok=True)
    flags = [rebuild.OPT if f.startswith("-O") else f for f in CFLAGS]
    flags += ["-std=gnu99",                              # our own code: C99 scoping (the rebuild's own is gnu89)
              "-ffunction-sections", "-fdata-sections", "-fno-common", "-I", EXT_DIR, f'-DFW_VERSION="{VERSION_FULL}"']
    objs = []
    for src in srcs:
        o = os.path.join(EXT_OUT, os.path.basename(src)[:-2] + ".o")
        run(f"{LLVM}/clang", *flags, "-c", src, "-o", o)
        bad = [n for n, _ in section_list(o) if not n.startswith((".text", ".rodata"))]
        if bad:
            raise SystemExit(f"{src}: writable data sections {bad}: keep state in ext_state")
        objs.append(o)

    defined = set()
    for o in objs:
        defined |= {l.split()[-1] for l in run(f"{LLVM}/llvm-nm", "--defined-only", o).splitlines()
                    if l.split()[1] in "Tt"}
    if "ext_boot_card" not in defined:
        raise SystemExit("firmware/ext must define ext_boot_card (it clears ext_state and calls boot_card__orig)")
    targets = {}
    for sym in sorted(defined):
        if sym.startswith("ext_"):
            name = sym[4:]
            addr = symbol_address(name)
            if addr is None:
                raise SystemExit(f"{sym}: no firmware function {name}")
            first, = struct.unpack_from(">I", image, addr)
            for _ in range(4):                   # a function the from-source build moved: its old entry is a plain
                if first >> 26 != 18 or first & 3:   # "b" (no link) to the code, so take over the code itself
                    break
                off = first & 0x03FFFFFC
                addr += off - (1 << 26) if off & 0x02000000 else off
                first, = struct.unpack_from(">I", image, addr)
            if first & 0xFFFF0000 != STWU_R1 and first != MFLR_R0:
                raise SystemExit(f"{sym}: {name} at {addr:#x} starts with {first:#010x}, not stwu r1,-N(r1) or "
                                 "mflr r0")
            targets[name] = (addr, first)
    thunk_s = os.path.join(EXT_OUT, "thunks.S")
    with open(thunk_s, "w") as f:
        for name, (addr, first) in targets.items():
            f.write(f"  .section .text.thunk_{name}, \"ax\"\n  .globl {name}__orig\n{name}__orig:\n"
                    f"  .long {first:#010x}\n  b {name}__cont\n")
    thunk_o = os.path.join(EXT_OUT, "thunks.o")
    run(f"{LLVM}/llvm-mc", "-triple=powerpc-unknown-eabi", "-filetype=obj", thunk_s, "-o", thunk_o)
    objs.append(thunk_o)

    # 1. link the block for RAM
    heap_off, = struct.unpack_from(">I", image, 0x10)
    block = (BSS_END + heap_off + 15) & ~15
    code_start = block + STATE_SIZE
    ls = ["SECTIONS {", f"  . = {code_start:#x};", "  .ext : { *(.text*) *(.rodata*) }",
          "  /DISCARD/ : { *(.comment) *(.note*) *(.eh_frame*) }", "}",
          f"ext_state = {block:#x};", f"emoji_state = {block:#x};", f"fonts_state = {block + 0x60:#x};",
          f"console_state = {block + 0x80:#x};",
          f"ext_dma_scratch = {block + RESERVE - DMA_SCRATCH:#x};",
          f"ext_apps_ram = {block + RESERVE - DMA_SCRATCH - APPS_RAM:#x};"]
    for name, (addr, _) in targets.items():
        ls.append(f"{name}__cont = {addr + 4:#x};")
    undefined = set()
    for o in objs:
        undefined |= {l.split()[-1] for l in run(f"{LLVM}/llvm-nm", "-u", o).splitlines()}
    for sym in sorted(undefined - defined - {f"{n}__orig" for n in targets} - {f"{n}__cont" for n in targets}
                      - {"ext_state", "emoji_state", "fonts_state", "console_state", "ext_dma_scratch",
                         "ext_apps_ram"}):
        a = symbol_address(sym)
        if a is None:
            raise SystemExit(f"extension needs {sym}: no such firmware symbol")
        ls.append(f"{sym} = {a:#x};")
    open(os.path.join(EXT_OUT, "ext.ld"), "w").write("\n".join(ls) + "\n")
    elf = os.path.join(EXT_OUT, "ext.elf")
    run(f"{LLD}/ld.lld", "-N", "-T", os.path.join(EXT_OUT, "ext.ld"), "-o", elf, *objs)
    blob_path = os.path.join(EXT_OUT, "ext.bin")
    run(f"{LLVM}/llvm-objcopy", "-O", "binary", "--only-section=.ext", elf, blob_path)
    blob = open(blob_path, "rb").read()
    blob += b"\0" * (-len(blob) % 4)
    if STATE_SIZE + len(blob) > RESERVE - DMA_SCRATCH - APPS_RAM:
        raise SystemExit(f"extensions are {len(blob)} bytes; the RAM block holds "
                         f"{RESERVE - STATE_SIZE - DMA_SCRATCH - APPS_RAM}")
    if block + RESERVE > 0x01000000:
        raise SystemExit("the RAM block must lie in the first 16 MB (bank 0, where virtual = physical for DMA)")
    syms = {}
    for l in run(f"{LLVM}/llvm-nm", elf).splitlines():
        p = l.split()
        if len(p) == 3:
            syms[p[2]] = int(p[0], 16)

    # 2. the code goes out as the EXT entry, tagged; the loader and the restore table go in gaps
    import zlib
    tag = zlib.crc32(blob) | 1                               # never 0
    blob += struct.pack(">I", tag)
    if STATE_SIZE + len(blob) > RESERVE:
        raise SystemExit(f"extensions are {len(blob)} bytes; the RAM block holds {RESERVE - STATE_SIZE}")
    sys.path.insert(0, os.path.join(REPO, "bootloader", "tools"))
    import nrv2d_pack
    packed = nrv2d_pack.compress(blob)
    packed += b"\0" * (-len(packed) % 4) + struct.pack(">I", tag)   # the loader checks this before unpacking
    ext_entry = os.path.join(EXT_OUT, "EXT")
    open(ext_entry, "wb").write(packed)
    gaps = free_gaps(os.path.join(OUT, "ELITE_RGB.elf"), image)
    uo = os.path.join(EXT_OUT, "unpack.o")                  # the decoder: its own gap
    run(f"{LLVM}/clang", "--target=powerpc-unknown-eabi", "-Os", "-ffreestanding", "-fno-builtin", "-msoft-float",
        "-fno-pic", "-ffunction-sections", "-c", UNPACK_SRC, "-o", uo)
    if run(f"{LLVM}/llvm-nm", "-u", uo).strip() or [n for n, _ in section_list(uo) if n != ".text.ext_unpack"]:
        raise SystemExit(f"{UNPACK_SRC}: must be one function with no data and no calls")
    unpack_size = (dict(section_list(uo))[".text.ext_unpack"] + 3) & ~3
    unpack_at = take(gaps, unpack_size)
    restore = struct.pack(">" + "II" * len(targets), *[v for addr, first in targets.values() for v in (addr, first)])
    restore += struct.pack(">I", 0)
    probe = loader_asm(0, 0, 0, 0, 0, 0, 0)
    loader_size = 4 * len([l for l in probe.splitlines()
                           if l.startswith("  ") and not l.startswith(("  .section", "  .globl"))])
    loader_at = take(gaps, loader_size)
    restore_at = take(gaps, len(restore))
    image[restore_at:restore_at + len(restore)] = restore
    changed = [(restore_at, restore_at + len(restore))]
    asm = loader_asm(restore_at, code_start, len(packed), len(blob), tag, syms["ext_boot_card"],
                     targets["boot_card"][0])
    ls_ = os.path.join(EXT_OUT, "loader.S")
    open(ls_, "w").write(asm)
    lo = os.path.join(EXT_OUT, "loader.o")
    run(f"{LLVM}/llvm-mc", "-triple=powerpc-unknown-eabi", "-filetype=obj", ls_, "-o", lo)
    lelf = os.path.join(EXT_OUT, "loader.elf")
    open(os.path.join(EXT_OUT, "loader.ld"), "w").write(
        f"SECTIONS {{ .loader {loader_at:#x} : {{ *(.text.ext_loader) }}\n"
        f"           .unpack {unpack_at:#x} : {{ *(.text.ext_unpack) }} }}\n")
    run(f"{LLD}/ld.lld", "-N", "-T", os.path.join(EXT_OUT, "loader.ld"), "-o", lelf, lo, uo)
    for sec, at, size in ((".loader", loader_at, loader_size), (".unpack", unpack_at, unpack_size)):
        lbin = os.path.join(EXT_OUT, sec[1:] + ".bin")
        run(f"{LLVM}/llvm-objcopy", "-O", "binary", f"--only-section={sec}", lelf, lbin)
        code = open(lbin, "rb").read()
        code += b"\0" * (-len(code) % 4)
        assert len(code) == size, (sec, len(code), size)
        image[at:at + len(code)] = code
        changed.append((at, at + len(code)))

    # 3. our version for the screen and the controllers (the NVRAM key stays)
    key = VERSION.encode() + b"\0"
    ver = VERSION_FULL.encode() + b"\0"
    if len(VERSION) != 5 or image[VERSION_STRING:VERSION_STRING + 6] != b"V4.20\0":
        raise SystemExit("version_string is not V4.20 or VERSION is not 5 characters")
    image[VERSION_STRING:VERSION_STRING + 6] = key                       # the NVRAM key too: a clean NVRAM
    changed.append((VERSION_STRING, VERSION_STRING + 6))
    ver_at = take(gaps, (len(ver) + 3) & ~3)
    image[ver_at:ver_at + len(ver)] = ver
    changed.append((ver_at, ver_at + len(ver)))
    version_sites = []
    for fn in VERSION_USERS:
        a = symbol_address(fn)
        sites = retarget(image, a, function_end(a), VERSION_STRING, ver_at)
        version_sites += sites
        if not sites:
            raise SystemExit(f"{fn}: no load of version_string found")
        changed += [(x, x + 4) for x in sites]
        print(f"version: {fn} now loads {VERSION_FULL!r} at {ver_at:#x} ({len(sites) // 2} site(s))")
    open(os.path.join(EXT_OUT, "VERSION"), "w").write(VERSION + "\n")   # for make fwtest (FW_VERSION: the key)
    open(os.path.join(EXT_OUT, "VERSION_FULL"), "w").write(VERSION_FULL + "\n")
    # the (lis, addi) pairs now loading VERSION_FULL: make fwtest gives the stock reference the same text through them
    open(os.path.join(EXT_OUT, "VERSION_SITES"), "w").write(
        "".join(f"{version_sites[i]:x} {version_sites[i + 1]:x}\n" for i in range(0, len(version_sites), 2)))

    # 4. the take-overs (boot_card goes to the loader) and the heap word
    def branch(src, dst):
        off = dst - src
        assert -0x2000000 <= off < 0x2000000 and not off & 3
        return 0x48000000 | (off & 0x03FFFFFC)
    for name, (addr, _) in targets.items():
        dst = loader_at if name == "boot_card" else syms[f"ext_{name}"]
        struct.pack_into(">I", image, addr, branch(addr, dst))
        changed.append((addr, addr + 4))
    struct.pack_into(">I", image, 0x10, heap_off + RESERVE)
    changed.append((0x10, 0x14))
    for i in range(0, len(image), 4):                  # nothing else may differ
        if image[i:i + 4] != base_image[i:i + 4] and not any(a <= i < b for a, b in changed):
            raise SystemExit(f"unexpected change at {i:#x}")
    out = os.path.join(OUT, "ELITE_RGB-ext")
    open(out, "wb").write(image)
    print(f"extensions: {len(srcs)} source(s), take-overs: {', '.join(targets)}")
    print(f"RAM block {block:#x}: state {STATE_SIZE:#x} bytes, code {len(blob)} bytes at {code_start:#x} "
          f"(entry EXT: {len(packed)} bytes packed, tag {tag:#010x}); loader {loader_size} bytes at "
          f"{loader_at:#x}, decoder {unpack_size} bytes at {unpack_at:#x}, restore table at "
          f"{restore_at:#x}; heap moved up {RESERVE:#x}")
    print(f"-> {out}, {ext_entry}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
