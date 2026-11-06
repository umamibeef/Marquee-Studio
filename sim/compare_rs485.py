#!/usr/bin/env python3
"""Send the same RS-485 frame to the stock ELITE_RGB and another image in the system simulator and compare how
they handle it (docs/REBUILD-PLAN.md, 4e).

The simulated sign has blank NVRAM, so nothing is configured. In both runs cfg_protocol (0xDCDE4) is forced to 8
(Cougar) and the Cougar level map (cfg_level_address 0xDCDF0, one byte per display level) to 01 0F 0F 0F: display
level 0 takes Cougar level 1, as a configured sign would have it. The record sent is the studio's text record for
level 1 with the show flag, so comms_message_dispatch matches it and stores its value and text
(jackpot_location_update). The frame is delivered on /dev/rs485 (SCC2) after the same number of
panel frames in both runs, so it lands at the same point of each image's own timeline. Compared:
  - the Cougar counters (bytes received, frames dispatched, errors, polls, replies) and the received payload;
  - every jackpot_location_update call (level, record) and the four jackpot records it keeps (in NVRAM, at the pointer in 0xBB178),
    and the whole 32 KB NVRAM;
  - the bytes each image sent back on every serial channel;
  - the distinct panel frames. Note: with the Cougar task competing for the CPU, the start-up progress bar is
    caught at slightly different lengths in a few frames (our compiled code is faster); those are listed apart.

Usage: compare_rs485.py [IMAGE] [INSNS] [STOCK_INSNS] [AT_FRAME]
"""
import os
import struct
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(HERE)
sys.path.insert(0, HERE)
sys.path.insert(0, os.path.join(REPO, "studio"))
import appsim  # noqa: E402
import compare  # noqa: E402
from marquee_studio import signproto  # noqa: E402

COUG_COUNTERS = [("bytes_rx", 0xB9310), ("bytes_tx", 0xB9314), ("frame_errors", 0xB9318), ("other", 0xB931C),
                 ("frames_ok", 0xB9320), ("quick_polls", 0xB9324), ("poll_replies", 0xB9328)]
COUG_PAYLOAD = 0x12DD0C
JACKPOT_LOCATION_UPDATE, JACKPOT_RECORDS_PTR = 0x5E6BC, 0xBB178   # -> 4 x 0x2B8-byte records in NVRAM


def run(image, insns, frame, at_frame):
    import cosim
    flash = open(os.path.join(REPO, "dumps", "bootflash.bin"), "rb").read()
    app = open(image, "rb").read()
    b = appsim.AppBoard(flash, next(iter(cosim.SCENARIOS.values())), app)
    b.run()
    ram = bytes(b.uc.mem_read(0, len(app)))
    tp, tl = struct.unpack_from(">II", ram, 0x4C)
    img = bytearray(app)
    for lo, hi in ((0x14, 0x9C), (tp, tp + tl)):
        img[lo:hi] = ram[lo:hi]
    b.uc.mem_write(0, bytes(img))
    b.hook_app_specials()
    S = appsim.sym()
    b.hook_busy_waits(S)
    appsim.watch_switches(b, S)
    b.force_config_word(0xDCDE4, 8)
    b.force_config_bytes(0xDCDF0, b"\x01\x0f\x0f\x0f")
    b.serial_send(1, frame, at_frame=at_frame)
    from unicorn import UC_HOOK_CODE
    from unicorn.ppc_const import UC_PPC_REG_3, UC_PPC_REG_4
    b.updates = []
    b.uc.hook_add(UC_HOOK_CODE, lambda uc, a, sz, _: b.updates.append(
        (uc.reg_read(UC_PPC_REG_3), bytes(uc.mem_read(uc.reg_read(UC_PPC_REG_4), 0x2B8)))),
        begin=JACKPOT_LOCATION_UPDATE, end=JACKPOT_LOCATION_UPDATE)
    b.run_app(insns, {})
    r32 = lambda a: struct.unpack(">I", bytes(b.uc.mem_read(a, 4)))[0]
    b.coug = {n: r32(a) for n, a in COUG_COUNTERS}
    b.payload = bytes(b.uc.mem_read(COUG_PAYLOAD, len(signproto.coug_receive(frame)[0][1])))
    p = r32(JACKPOT_RECORDS_PTR)
    b.records = (p, bytes(b.uc.mem_read(p, 4 * 0x2B8)) if p else b"")
    b.nvram = bytes(b.uc.mem_read(cosim.NVRAM, 0x8000))
    return b


def main():
    image = sys.argv[1] if len(sys.argv) > 1 else os.path.join(REPO, "analysis", "build", "fw", "ELITE_RGB")
    insns = int(sys.argv[2]) if len(sys.argv) > 2 else 600_000_000
    stock_insns = int(sys.argv[3]) if len(sys.argv) > 3 else 750_000_000
    at_frame = int(sys.argv[4]) if len(sys.argv) > 4 else 700
    frame = signproto.rs485_frame(1, signproto.rs485_text_record("HELLO FROM THE SIM", level=1, value_a=123456))
    a = run(os.path.join(REPO, "analysis", "extracted", "ELITE_RGB"), stock_insns, frame, at_frame)
    b = run(image, insns, frame, at_frame)
    for tag, x in (("stock", a), ("other", b)):
        print(f"{tag}: jackpot updates {[(lvl, rec[:8].hex()) for lvl, rec in x.updates]}")
        print(f"{tag}: serial rx events {x.events.get('serial rx', 0)}, Cougar {x.coug}, tx "
              f"{ {k: len(v) for k, v in x.serial_tx.items()} }, tasks {sorted(x.tasks)}")
    da, db = compare.distinct(a.frames), compare.distinct(b.frames)
    n = min(len(da), len(db))
    diff = [i for i in range(n) if da[i] != db[i]]
    print(f"distinct frames: stock {len(da)}, other {len(db)}; {n - len(diff)} of {n} identical"
          + (f"; differ at {diff} (check: the start-up progress bar)" if diff else ""))
    ok = (a.events.get("serial rx") == b.events.get("serial rx") == 1 and a.coug == b.coug and a.coug["frames_ok"]
          and a.payload == b.payload and a.serial_tx == b.serial_tx and sorted(a.tasks) == sorted(b.tasks)
          and a.updates and a.updates == b.updates and a.records == b.records and a.nvram == b.nvram)
    print(f"jackpot updates same: {a.updates == b.updates}; jackpot records same: {a.records == b.records}; "
          f"NVRAM same: {a.nvram == b.nvram}")
    print(f"jackpot records at {a.records[0]:#x} / {b.records[0]:#x}")
    print("RESULT:", "SAME HANDLING" if ok else "DIFFERENT")
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
