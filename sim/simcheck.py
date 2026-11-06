#!/usr/bin/env python3
"""Fingerprints of simulator runs, for checking that a change to the simulator itself (speed work, refactoring)
does not change what it computes. Each scenario runs one of the sim/compare*.py / flashtest.py set-ups and records
everything a run produces: every panel frame (hash of the whole sequence and of each frame), the bytes sent on each
serial channel, the NVRAM, the whole RAM, DPRAM/IMMR, CPU registers and shadow MSR, the instruction count and
simulated time, the event counters, the PIT, the context switches and the IR state. Two runs are the same run only
if every field matches.

Usage: simcheck.py OUT.json SCENARIO...      (scenarios: see SCENARIOS; 'all' runs them all)
       simcheck.py --diff A.json B.json      (field by field)
"""
import hashlib
import json
import os
import struct
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(HERE)
sys.path.insert(0, HERE)
sys.path.insert(0, os.path.join(REPO, "studio"))
import appsim  # noqa: E402

STOCK = os.path.join(REPO, "analysis", "extracted", "ELITE_RGB")
FROMSRC = os.path.join(REPO, "analysis", "build", "fw", "ELITE_RGB")
EXT = os.path.join(REPO, "analysis", "build", "fw", "ELITE_RGB-ext")
FWIMAGE = os.path.join(REPO, "bootloader", "build", "bootflash-fw.bin")
DUMP = os.path.join(REPO, "dumps", "bootflash.bin")


def h(data):
    return hashlib.sha256(data).hexdigest()[:20]


def fingerprint(b):
    import cosim
    from unicorn import ppc_const as P
    uc = b.uc
    regs = [uc.reg_read(P.UC_PPC_REG_0 + i) for i in range(32)] + [
        uc.reg_read(r) for r in (P.UC_PPC_REG_PC, P.UC_PPC_REG_CR, P.UC_PPC_REG_LR, P.UC_PPC_REG_CTR,
                                 P.UC_PPC_REG_XER)]
    fr = hashlib.sha256()
    for f in b.frames:
        fr.update(len(f).to_bytes(4, "big") + f)
    out = {
        "icount": b.icount, "skew": b.skew, "now": b.now(), "pit_next": b.pit_next, "msr": b.msr,
        "spr": sorted(b.spr.items()), "regs": regs,
        "frames": len(b.frames), "frames sha": fr.hexdigest()[:20],
        "frame list": [h(f) for f in b.frames],
        "serial tx": {str(k): v.hex() for k, v in sorted(b.serial_tx.items())},
        "rx left": len(b.rx_queue), "rx_idx": sorted(b.rx_idx.items()), "tx_idx": sorted(b.tx_idx.items()),
        "ir": [b.ir_level, b.ir_tb, b.ir_keys_sent, b.ir_last, len(b.ir_queue), len(b.ir_edges)],
        "events": dict(sorted(b.events.items())),
        "nvram": h(bytes(uc.mem_read(cosim.NVRAM, 0x80000))),
        "ram": h(bytes(uc.mem_read(0, 0x8000000))),
        "dpram": h(bytes(b.dpram)), "eeproms": {str(k): h(bytes(v)) for k, v in sorted(b.eeproms.items())},
        "reg_writes": [len(b.reg_writes), h(repr(b.reg_writes).encode())],
        "tasks": dict(sorted(getattr(b, "tasks", {}).items())), "switches": getattr(b, "switches", []),
    }
    for k in ("states", "updates", "records", "coug", "payload"):
        if hasattr(b, k):
            v = getattr(b, k)
            out[k] = h(repr(v).encode()) if k in ("updates", "records", "payload") else v
    return out


def sc_flashtest():
    import flashtest
    b, bad = flashtest.boot(open(FWIMAGE, "rb").read(), open(EXT, "rb").read(), 80_000_000)
    return b


def sc_flashtest_stock():
    import flashtest
    b, bad = flashtest.boot(open(DUMP, "rb").read(), open(STOCK, "rb").read(), 110_000_000)
    return b


def sc_db9_stock():
    import compare_db9
    return compare_db9.run(STOCK, 750_000_000)


def sc_db9_fromsrc():
    import compare_db9
    return compare_db9.run(FROMSRC, 600_000_000)


def sc_menu_stock():
    import compare_menu
    return compare_menu.run(STOCK, 680_000_000)


def sc_menu_edit_stock():
    import compare_menu
    return compare_menu.run(STOCK, 680_000_000, compare_menu.EDIT_KEYS)


def sc_rs485_stock():
    import compare_rs485
    from marquee_studio import signproto
    frame = signproto.rs485_frame(1, signproto.rs485_text_record("HELLO FROM THE SIM", level=1, value_a=123456))
    return compare_rs485.run(STOCK, 750_000_000, frame, 700)


SCENARIOS = {n[3:]: f for n, f in globals().items() if n.startswith("sc_")}


# seconds: wall time; reg_writes: cosim's log of IMMR register writes, kept for the boot only since the speed work
IGNORED = ("seconds", "reg_writes")


def diff(a, b):
    same = True
    for k in sorted(set(a) | set(b)):
        for sc in [k]:
            x, y = a.get(sc), b.get(sc)
            if x is None or y is None:
                print(f"{sc}: only in one file")
                continue
            bad = [f for f in sorted(set(x) | set(y)) if f not in IGNORED and x.get(f) != y.get(f)]
            same &= not bad
            print(f"{sc}: {'identical' if not bad else 'DIFFERENT: ' + ', '.join(bad)}"
                  f"  ({x.get('seconds', 0):.0f} s -> {y.get('seconds', 0):.0f} s)")
    return same


def main():
    if sys.argv[1] == "--diff":
        a, b = (json.load(open(p)) for p in sys.argv[2:4])
        return 0 if diff(a, b) else 1
    out, names = sys.argv[1], sys.argv[2:]
    if names == ["all"]:
        names = list(SCENARIOS)
    res = json.load(open(out)) if os.path.exists(out) else {}
    for n in names:
        t = time.time()
        r = SCENARIOS[n]()
        fp = r if isinstance(r, dict) else fingerprint(r)
        fp["seconds"] = time.time() - t
        print(f"{n}: {fp['icount']:,} instructions, {fp['frames']} frames, {fp['seconds']:.1f} s "
              f"({fp['icount'] / fp['seconds'] / 1e6:.2f} M/s)", flush=True)
        res[n] = fp
        json.dump(res, open(out, "w"), indent=1)
    return 0


if __name__ == "__main__":
    sys.exit(main())
