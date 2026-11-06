#!/usr/bin/env python3
"""Co-simulate the original BOOTSTRAP V2.00 and the C rebuild on one emulated board, and
compare everything they do to the hardware.

Both images run from reset (0xF8180100) to the jump into the application (PC = 0) under
Unicorn (PowerPC 32, big-endian). MPC8xx-specific instructions (mtspr/mfspr/mtmsr/mfmsr/
tlbia) are intercepted and logged instead of executed. The board model provides:
  - IMMR/DPRAM at 0xFF000000 with a CPM timer whose reference event is always due, a CPM
    command register that completes at once, strap inputs (PIPR, PBDAT, PDDAT), and an I2C
    controller that serves reads from simulated EEPROMs when I2COM is written;
  - SDRAM at 0, NVRAM at 0xF8780000, main flash at 0x50000000 and CS7 at 0x60000000.

Compared per scenario:
  1. the ordered sequence of writes to IMMR registers (0xFF000000..0xFF001FFF);
  2. the ordered sequence of SPR/MSR writes and tlbia;
  3. final DPRAM state the app or hardware can see (boot info, bank list, TLB counters, BDs);
  4. final SDRAM (hash of 16 MB: the app image + hand-off block + MMU table), NVRAM, PC/MSR.

Usage: cosim.py ORIGINAL_FLASH NEW_FLASH [ELITE_RGB]
"""
import hashlib, struct, sys
from unicorn import Uc, UcError, UC_ARCH_PPC, UC_MODE_PPC32, UC_MODE_BIG_ENDIAN, UC_HOOK_CODE
from unicorn.ppc_const import UC_PPC_REG_PC, UC_PPC_REG_0

FLASH, IMMR, NVRAM, MAIN, CS7 = 0xF8180000, 0xFF000000, 0xF8780000, 0x50000000, 0x60000000
RAM_SIZE = 0x01000000
MAX_INSNS = 400_000_000


def be32(b, o):
    return struct.unpack_from(">I", b, o)[0]


def eeprom_cfg(spd_path):
    """Board config EEPROM (I2C 0xA0) for one of the two memory paths, with valid sum/xor."""
    c = bytearray(256)
    struct.pack_into(">I", c, 0x00, 0x00C1A042)            # board id
    if spd_path:
        c[4], c[5] = 1, 0x10
        struct.pack_into(">I", c, 0x10, 0x20000000)        # NVRAM size index 1 (512 KB)
    else:
        c[4], c[5] = 0, 0
        struct.pack_into(">I", c, 0x10, 0)
    struct.pack_into(">H", c, 0x06, 0x0004)                # SCCR bits
    struct.pack_into(">I", c, 0x08, 50_000_000)            # wanted sysclk
    struct.pack_into(">I", c, 0x0C, 10_000_000)            # oscillator
    struct.pack_into(">H", c, 0x1A, 0x0E00)                # SDRAM OR low half
    struct.pack_into(">I", c, 0x1C, 0xFFF80D30)            # OR0
    struct.pack_into(">I", c, 0x20, 0xFFFF8940)            # OR1: 32 KB NVRAM
    struct.pack_into(">I", c, 0x24, 0xFF800D34)            # OR4: 8 MB flash bank 1
    struct.pack_into(">I", c, 0x28, 0xFFC00D34)            # OR5: 4 MB flash bank 2
    struct.pack_into(">I", c, 0x2C, 0x12340105)            # MCR command words
    struct.pack_into(">H", c, 0x30, 0x0088)                # MAR
    struct.pack_into(">H", c, 0x32, 0x0106)
    c[0x34], c[0x35] = 0x04, 1                             # MPTPR, 128 MB
    struct.pack_into(">HHHH", c, 0x36, 0x1312, 0x4111, 0x9C12, 0x4111)
    c[0x3E] = sum(c[:62]) & 0xFF
    x = 0
    for v in c[:62]:
        x ^= v
    c[0x3F] = x
    return bytes(c)


def eeprom_a2(spd_path):
    """Second EEPROM (I2C 0xA2): the UPM program (fixed path) or an SDRAM SPD block."""
    if not spd_path:
        return b"".join(struct.pack(">I", 0x0F0FC000 ^ (i * 0x01010101)) for i in range(64))
    s = bytearray(256)
    s[2], s[3], s[4], s[5] = 4, 13, 9, 1                   # SDRAM, 13 rows, 9 cols, 1 bank
    s[12], s[17], s[31] = 0x80, 4, 0x20                    # refresh code 0, 4 banks, 128 MB
    s[63] = sum(s[:63]) & 0xFF
    return bytes(s)


class Board:
    def __init__(self, flash, scenario, upgrade):
        self.sc = scenario
        self.spd = scenario["spd"]
        self.eeproms = {0xA0: eeprom_cfg(self.spd), 0xA2: eeprom_a2(self.spd)}
        self.dpram = bytearray(0x10000)                    # IMMR + DPRAM backing store
        self.reg_writes = []
        self.spr_log = []
        self.spr = {}
        self.msr = 0
        self.uc = uc = Uc(UC_ARCH_PPC, UC_MODE_PPC32 | UC_MODE_BIG_ENDIAN)
        uc.mem_map(0, RAM_SIZE)
        uc.mem_map(FLASH, 0x80000)
        uc.mem_write(FLASH, flash)
        uc.mem_map(NVRAM, 0x80000)
        nv = bytearray(0x80000)
        nv[0x7FF0], nv[0x7FF1] = scenario["tries"], 0
        uc.mem_write(NVRAM, bytes(nv))
        uc.mem_map(MAIN, 0x2000000)
        if upgrade:
            uc.mem_write(MAIN, upgrade)
        uc.mem_map(CS7, 0x200000)
        uc.mmio_map(IMMR, 0x10000, self._rd, None, self._wr, None)
        self._hook_specials(flash)
        uc.hook_add(UC_HOOK_CODE, self._done, begin=0, end=0)

    # ---- IMMR model ----
    def _rd(self, uc, off, size, _):
        if off == 0x9B0:                                   # TER1: reference reached
            return 0x0002
        if off == 0x9C0:                                   # CPCR: command done
            return 0
        if off == 0x0F0 and size == 1:                     # PIPR strap byte
            return self.sc["pipr"]
        if off == 0xAC4:
            return self.sc["pbdat"]
        if off == 0x976:
            return self.sc["pddat"]
        return int.from_bytes(self.dpram[off:off + size], "big")

    def _wr(self, uc, off, size, value, _):
        self.dpram[off:off + size] = value.to_bytes(size, "big")
        if off < 0x2000:
            self.reg_writes.append((off, size, value))
        if off == 0x86C and value == 0x81:                 # I2COM: start
            self._i2c()

    def _i2c(self):
        d = self.dpram
        txbd1_st = be32(d, 0x2418) >> 16
        if txbd1_st != 0xAC00:                             # only reads are modelled
            return
        dev = d[0x2420] & 0xFE
        n = (be32(d, 0x2418) & 0xFFFF) - 1
        buf = be32(d, 0x240C)
        data = self.eeproms.get(dev, b"\xff" * 256)[:n]
        if IMMR <= buf < IMMR + 0x10000:
            o = buf - IMMR
            self.dpram[o:o + len(data)] = data
        else:
            self.uc.mem_write(buf, data)
        st = be32(d, 0x2408)
        struct.pack_into(">I", self.dpram, 0x2408, st & ~0x80000000)   # clear E

    # ---- special instructions ----
    def _hook_specials(self, flash):
        for i in range(0, len(flash) - 3, 4):
            w = be32(flash, i)
            if w >> 26 != 31:
                continue
            xo = (w >> 1) & 0x3FF
            spr = ((w >> 16) & 0x1F) | (((w >> 11) & 0x1F) << 5)
            if xo in (467, 339) and spr in (1, 8, 9):      # XER/LR/CTR: let the CPU do these
                continue
            if xo in (467, 339, 146, 83, 370):
                self.uc.hook_add(UC_HOOK_CODE, self._special, begin=FLASH + i, end=FLASH + i)

    def _special(self, uc, addr, size, _):
        w = be32(uc.mem_read(addr, 4), 0)
        xo = (w >> 1) & 0x3FF
        rs = (w >> 21) & 0x1F
        spr = ((w >> 16) & 0x1F) | (((w >> 11) & 0x1F) << 5)
        reg = lambda r: uc.reg_read(UC_PPC_REG_0 + r)
        if xo == 467:                                      # mtspr
            v = reg(rs)
            self.spr[spr] = v
            self.spr_log.append(("mtspr", spr, v))
        elif xo == 339:                                    # mfspr
            uc.reg_write(UC_PPC_REG_0 + rs, 0 if spr == 568 else self.spr.get(spr, 0))
        elif xo == 146:                                    # mtmsr
            self.msr = reg(rs)
            self.spr_log.append(("mtmsr", 0, self.msr))
        elif xo == 83:                                     # mfmsr
            uc.reg_write(UC_PPC_REG_0 + rs, self.msr)
        elif xo == 370:                                    # tlbia
            self.spr_log.append(("tlbia", 0, 0))
        uc.reg_write(UC_PPC_REG_PC, addr + 4)

    def _done(self, uc, addr, size, _):
        self.reached_app = True
        uc.emu_stop()

    def run(self):
        self.reached_app = False
        try:
            self.uc.emu_start(FLASH + 0x100, 0xFFFFFFFF, count=MAX_INSNS)
        except UcError as e:
            pc = self.uc.reg_read(UC_PPC_REG_PC)
            raise SystemExit(f"emulation error {e} at pc={pc:#x}")
        return self


def summarise(b):
    d = b.dpram
    ram = bytes(b.uc.mem_read(0, RAM_SIZE))
    return {
        "reached app": b.reached_app,
        "final MSR": hex(b.msr),
        "boot info 0x2240": d[0x2240:0x2298].hex(),
        "bank list 0x2398": d[0x2398:0x23DC].hex(),
        "TLB counters": (d[0x23E0:0x2408] + d[0x2428:0x243C]).hex(),
        "I2C BDs+tx": d[0x2408:0x2424].hex(),
        "RAM handoff 0x14..0x98": ram[0x14:0x9C].hex(),
        "RAM sha256": hashlib.sha256(ram).hexdigest()[:16],
        "NVRAM counter": bytes(b.uc.mem_read(NVRAM + 0x7FF0, 2)).hex(),
    }


def first_diff(a, b):
    for i, (x, y) in enumerate(zip(a, b)):
        if x != y:
            return i
    return min(len(a), len(b)) if len(a) != len(b) else None


def fmt(e):
    k, a, v = e if len(e) == 3 else (None, *e)
    return f"{k} {a} = {v:#x}" if k else f"IMMR+{a:#05x} ({v and ''}) <- {e[2]:#x} [{e[1]}B]"


def compare(name, orig, new):
    ok = True
    print(f"\n=== {name} ===")
    so, sn = summarise(orig), summarise(new)
    for k in so:
        same = so[k] == sn[k]
        ok &= same
        print(f"  {'ok  ' if same else 'DIFF'} {k}" + ("" if same else f"\n       orig {so[k]}\n       new  {sn[k]}"))
    for label, a, b in (("register writes", orig.reg_writes, new.reg_writes),
                        ("SPR/MSR ops", orig.spr_log, new.spr_log)):
        i = first_diff(a, b)
        if i is None:
            print(f"  ok   {label}: {len(a)} identical")
        else:
            ok = False
            print(f"  DIFF {label}: {len(a)} vs {len(b)}, first difference at #{i}")
            for j in range(max(0, i - 2), min(i + 4, max(len(a), len(b)))):
                ea = a[j] if j < len(a) else None
                eb = b[j] if j < len(b) else None
                show = lambda e: "-" if e is None else (
                    f"{e[0]} {e[1]} {e[2]:#x}" if isinstance(e[0], str) else f"+{e[0]:#05x}/{e[1]} <- {e[2]:#x}")
                print(f"       #{j}: orig {show(ea):32} new {show(eb)}")
    return ok


SCENARIOS = {
    "fixed config, no upgrade (tries=0xFF)":
        dict(spd=False, tries=0xFF, pipr=0x02, pbdat=0x3C000, pddat=0x0000, upgrade=False),
    "fixed config, upgrade image in main flash (tries=0)":
        dict(spd=False, tries=0x00, pipr=0x02, pbdat=0x3C000, pddat=0x0000, upgrade=True),
    "fixed config, 4th upgrade attempt falls back (tries=3)":
        dict(spd=False, tries=0x03, pipr=0x02, pbdat=0x3C000, pddat=0x0000, upgrade=True),
    "SPD SDRAM, one flash bank + CS7 (tries=0xFF)":
        dict(spd=True, tries=0xFF, pipr=0x02, pbdat=0x3C000, pddat=0x0000, upgrade=False),
    "SPD SDRAM, two flash banks, upgrade (tries=1)":
        dict(spd=True, tries=0x01, pipr=0x02, pbdat=0x00000, pddat=0x0400, upgrade=True),
    "fixed config, truncated upgrade falls back to boot flash (tries=0)":
        dict(spd=False, tries=0x00, pipr=0x02, pbdat=0x3C000, pddat=0x0000, upgrade="corrupt"),
}


def corrupt_upgrade(chain):
    """Truncate the bootable entry's NRV2D stream to half, with a valid sum/xor check word,
    so the bootstrap accepts the image but the decoder runs out of input and fails.
    (A stream of bogus matches is not a usable test: the original's back-reference check
    compares against output base 0 and can never fire, so it reads wrapped addresses.)"""
    off = 0
    while True:
        n = be32(chain, off)
        if be32(chain, off + 8) & 1:
            break
        off = (off + n + 3) & ~3
    hdr = (chain[off + 12] + 16) & ~3
    data_len = ((n - hdr - 4) // 2) & ~3
    e = bytearray(chain[off:off + hdr + data_len])
    struct.pack_into(">I", e, 0, len(e) + 4)
    s = x = 0
    for i in range(0, len(e), 4):
        w = be32(e, i)
        s += (w >> 16) + (w & 0xFFFF)
        x ^= (w >> 16) ^ (w & 0xFFFF)
    e += struct.pack(">I", ((s << 16) | x) & 0xFFFFFFFF)
    return bytes(e) + b"\0" * 4


def main(orig_path, new_path, app_path=None):
    orig_flash = open(orig_path, "rb").read()
    new_flash = open(new_path, "rb").read()
    # Upgrade image for the main flash: the archive chain of the original, minus the bootstrap.
    first = (be32(orig_flash, 0) + 3) & ~3
    upgrade = orig_flash[first:]
    app = open(app_path, "rb").read() if app_path else None

    all_ok = True
    for name, sc in SCENARIOS.items():
        up = corrupt_upgrade(upgrade) if sc["upgrade"] == "corrupt" else (upgrade if sc["upgrade"] else None)
        o = Board(orig_flash, sc, up).run()
        n = Board(new_flash, sc, up).run()
        ok = compare(name, o, n)
        print(f"  info NVRAM tries/flags: before {sc['tries']:#04x}/00, after "
              f"{bytes(n.uc.mem_read(NVRAM + 0x7FF0, 2)).hex(' ')}")
        if app:
            # The bootstrap legitimately rewrites two parts of the image: the hand-off block
            # (0x14..0x9C) and the app's MMU descriptor table (pointer/length at 0x4C/0x50).
            ram = bytes(n.uc.mem_read(0, len(app)))
            tp, tl = struct.unpack_from(">II", app, 0x4C)
            skip = [(0x14, 0x9C), (tp, tp + tl)]
            bad = [i for i in range(0, len(app), 4)
                   if not any(a <= i < b for a, b in skip) and ram[i:i + 4] != app[i:i + 4]]
            got = not bad
            print(f"  {'ok  ' if got else 'DIFF'} app image = ELITE_RGB except hand-off block and "
                  f"MMU table {tp:#x}..{tp + tl:#x}" + ("" if got else f" ({len(bad)} words differ, first {bad[0]:#x})"))
            ok &= got
        all_ok &= ok
    print("\nALL SCENARIOS MATCH" if all_ok else "\nDIFFERENCES FOUND")
    return 0 if all_ok else 1


if __name__ == "__main__":
    if len(sys.argv) not in (3, 4):
        raise SystemExit(__doc__)
    sys.exit(main(*sys.argv[1:]))
