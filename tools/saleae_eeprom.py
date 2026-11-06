#!/usr/bin/env python3
"""Capture the config EEPROM (U1, CAT24WC04) reads at power-on with a Saleae Logic Pro 16 through Logic 2 automation,
then decode the I2C bus and rebuild the EEPROM's two 256-byte blocks (I2C 0xA0 and 0xA2).

Wiring (CAT24WC04 data sheet, Catalyst doc. 1022 rev J, SOIC): Ch0 = U1 pin 5 (SDA), Ch1 = U1 pin 6 (SCL),
GND = U1 pin 4 (VSS). Needs Logic 2 running with Preferences > Automation > "Enable automation server" (port 10430).
It only listens.

  saleae_eeprom.py capture [SECONDS]   arm on the first I2C START (SDA falling while SCL high), record SECONDS after it
                                       (default 20), save .sal + raw CSV + the I2C table, then decode
  saleae_eeprom.py decode DIR          decode an existing capture directory again
  saleae_eeprom.py selftest            check the decoder on a made-up waveform
"""
import csv, os, struct, sys, time

HERE = os.path.dirname(os.path.abspath(__file__))
SDA, SCL = 0, 1


def say(s):
    print(f"[{time.strftime('%H:%M:%S')}] {s}", flush=True)


def capture(seconds):
    from saleae import automation
    out = os.path.join(HERE, "..", "captures", "saleae", "eeprom-" + time.strftime("%H%M%S"))
    os.makedirs(out, exist_ok=True)
    say("connecting to Logic 2 automation server on localhost:10430")
    with automation.Manager.connect(port=10430) as manager:
        devices = manager.get_devices()
        if not devices:
            sys.exit("no Saleae device found: is it plugged in?")
        dev = devices[0]
        say(f"using {dev.device_type} serial {dev.device_id}")
        dc = automation.LogicDeviceConfiguration(
            enabled_digital_channels=[SDA, SCL], digital_sample_rate=25_000_000, digital_threshold_volts=3.3)
        start = automation.DigitalTriggerCaptureMode(
            trigger_type=automation.DigitalTriggerType.FALLING, trigger_channel_index=SDA,
            linked_channels=[automation.DigitalTriggerLinkedChannel(
                SCL, automation.DigitalTriggerLinkedChannelState.HIGH)],
            after_trigger_seconds=seconds, trim_data_seconds=seconds + 1.0)
        cc = automation.CaptureConfiguration(capture_mode=start)
        say(f"ch0 SDA + ch1 SCL at 25 MS/s, 3.3V threshold setting; trigger = I2C START; {seconds:g} s after it")
        with manager.start_capture(device_id=dev.device_id, device_configuration=dc,
                                   capture_configuration=cc) as cap:
            say("ARMED. Power the sign on now.")
            cap.wait()
            say("triggered and recorded, saving")
            cap.save_capture(filepath=os.path.join(out, "eeprom.sal"))
            cap.export_raw_data_csv(directory=out, digital_channels=[SDA, SCL])
            try:
                i2c = cap.add_analyzer("I2C", label="EEPROM", settings={"SDA": SDA, "SCL": SCL})
                cap.export_data_table(filepath=os.path.join(out, "i2c_table.csv"), analyzers=[
                    automation.DataTableExportConfiguration(i2c, automation.RadixType.HEXADECIMAL)])
                say("Logic 2 I2C analyzer table saved (i2c_table.csv)")
            except Exception as e:
                say(f"Logic 2 I2C analyzer export failed ({e}); using our own decoder only")
    say(f"saved to {os.path.relpath(out)}")
    decode(out)


def load_edges(path):
    rows = []
    with open(path) as f:
        r = csv.reader(f)
        hdr = [h.strip() for h in next(r)]
        isda, iscl = hdr.index(f"Channel {SDA}"), hdr.index(f"Channel {SCL}")
        for row in r:
            rows.append((float(row[0]), int(row[isda]), int(row[iscl])))
    return rows


def i2c_decode(rows):
    """[(t, sda, scl)] -> transactions [{t, bytes: [(value, acked)], stop}]; START or repeated START opens one."""
    txs, cur, bits = [], None, []
    psda, pscl = rows[0][1], rows[0][2]
    for t, sda, scl in rows[1:]:
        if scl == pscl == 1 and sda != psda:
            if sda == 0:
                cur = {"t": t, "bytes": [], "stop": False}
                txs.append(cur)
                bits = []
            elif cur is not None:
                cur["stop"] = True
                cur = None
        elif scl == 1 and pscl == 0 and cur is not None:
            bits.append(sda)
            if len(bits) == 9:
                v = 0
                for b in bits[:8]:
                    v = v << 1 | b
                cur["bytes"].append((v, bits[8] == 0))
                bits = []
        psda, pscl = sda, scl
    return txs


def rebuild(txs):
    """24C04 (slave address 1010 A2 A1 a8 R/W) -> memory {addr: byte}, log lines, writes."""
    mem, log, writes, ptr = {}, [], [], None
    for tx in txs:
        if not tx["bytes"]:
            continue
        sa, sa_ack = tx["bytes"][0]
        if sa >> 4 != 0xA:
            log.append(f"{tx['t']:.6f} other device 0x{sa & 0xFE:02X}")
            continue
        blk, rd, data = (sa >> 1) & 1, sa & 1, tx["bytes"][1:]
        if not sa_ack:
            log.append(f"{tx['t']:.6f} 0x{sa & 0xFE:02X} {'R' if rd else 'W'} not acknowledged")
            continue
        if not rd:
            if data:
                ptr = blk << 8 | data[0][0]
                if len(data) > 1:
                    writes.append((tx["t"], ptr, [v for v, _ in data[1:]]))
                    log.append(f"{tx['t']:.6f} WRITE 0x{ptr:03X}: {' '.join(f'{v:02X}' for v, _ in data[1:])}")
                else:
                    log.append(f"{tx['t']:.6f} set address 0x{ptr:03X}")
        else:
            ptr = blk << 8 | ((ptr or 0) & 0xFF)
            start = ptr
            for v, _ in data:
                mem[ptr] = v
                ptr = (ptr & 0x100) | ((ptr + 1) & 0xFF)
            log.append(f"{tx['t']:.6f} read 0x{start:03X} x {len(data)}")
    return mem, log, writes


def summary(mem):
    def word(a, n):
        v = [mem.get(a + i) for i in range(n)]
        return None if None in v else int.from_bytes(bytes(v), "big")
    out = []
    for name, a, n in (("board type (+4)", 4, 1), ("board revision (+5)", 5, 1), ("CPU clock Hz (+8)", 8, 4),
                       ("geometry word (+0x40)", 0x40, 2), ("options (+0x44)", 0x44, 4),
                       ("display number (+0x48)", 0x48, 4)):
        v = word(a, n)
        out.append(f"  {name:24s} " + ("not read" if v is None else f"0x{v:0{2 * n}X} ({v})"))
    six = [mem.get(0x14 + i) for i in range(6)]
    out.append("  bytes +0x14..0x19        " + ("not read" if None in six else ":".join(f"{x:02X}" for x in six)))
    g = word(0x40, 2)
    if g is not None:
        out.append(f"  geometry 0x{g:04X}: expected 0x8240 (AD0501, 200x16) -> " + ("MATCH" if g == 0x8240 else "DIFFERENT"))
    return out


def decode(out):
    txs = i2c_decode(load_edges(os.path.join(out, "digital.csv")))
    mem, log, writes = rebuild(txs)
    with open(os.path.join(out, "transactions.txt"), "w") as f:
        f.write("\n".join(log) + "\n")
    for blk in (0, 1):
        with open(os.path.join(out, f"eeprom_a{2 * blk}.bin"), "wb") as f:
            f.write(bytes(mem.get(blk << 8 | i, 0xFF) for i in range(256)))
        have = sum(1 for i in range(256) if (blk << 8 | i) in mem)
        say(f"block 0x{0xA0 + 2 * blk:02X}: {have}/256 bytes read (unread bytes saved as FF)")
    say(f"{len(txs)} I2C transactions, {len(writes)} writes" + ("  <-- the firmware WROTE the EEPROM" if writes else ""))
    for line in summary(mem):
        print(line)
    say(f"files: transactions.txt, eeprom_a0.bin, eeprom_a2.bin in {os.path.relpath(out)}")


def selftest():
    rows, t, st = [], [0.0], {}

    def put(sda, scl):
        t[0] += 1e-6
        rows.append((t[0], sda, scl))
    put(1, 1)

    def start():
        put(1, 1); put(0, 1); put(0, 0)

    def stop():
        put(0, 0); put(0, 1); put(1, 1)

    def byte(v, ack=True):
        for i in range(7, -1, -1):
            b = v >> i & 1
            put(b, 0); put(b, 1); put(b, 0)
        a = 0 if ack else 1
        put(a, 0); put(a, 1); put(a, 0); put(1, 0)
    image = bytearray(range(0x50))
    image[4:6] = b"\x02\x51"
    image[8:12] = struct.pack(">I", 50_000_000)
    image[0x40:0x42] = b"\x82\x40"
    start(); byte(0xA0); byte(0x00); start(); byte(0xA1)
    for i, v in enumerate(image):
        byte(v, ack=i < len(image) - 1)
    stop()
    start(); byte(0xA2); byte(0x10); start(); byte(0xA3); byte(0x5A); byte(0xA5, ack=False); stop()
    mem, log, writes = rebuild(i2c_decode(rows))
    ok = all(mem[i] == image[i] for i in range(len(image))) and mem[0x110] == 0x5A and mem[0x111] == 0xA5 and not writes
    for line in summary(mem):
        print(line)
    print("selftest", "PASS" if ok else "FAIL")
    return ok


if __name__ == "__main__":
    cmd = sys.argv[1] if len(sys.argv) > 1 else "capture"
    if cmd == "capture":
        capture(float(sys.argv[2]) if len(sys.argv) > 2 else 20.0)
    elif cmd == "decode":
        decode(sys.argv[2])
    elif cmd == "selftest":
        sys.exit(0 if selftest() else 1)
    else:
        sys.exit(__doc__)
