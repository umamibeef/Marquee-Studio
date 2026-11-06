#!/usr/bin/env python3
"""Capture the sign's 20-second burst with a Saleae Logic Pro 16 through Logic 2 automation,
then measure the bit timing and decode it.

Wiring: Ch0 = sign pin 3 (sign TX), Ch1 = pin 2 (sign RX, i.e. what our adapter sends), GND = pin 5.
Needs Logic 2 running with Preferences > Automation > "Enable automation server" on (port 10430).

  saleae_capture.py capture      wait for the next burst, save .sal + CSV, then analyze
  saleae_capture.py scan [N]     record ch0..N-1 (default 10) for 25 s when the TX wire is unknown
  saleae_capture.py analyze DIR  re-analyze an existing export directory
"""
import csv, os, sys, time

HERE = os.path.dirname(os.path.abspath(__file__))
STD_BAUDS = [300, 600, 1200, 2400, 4800, 9600, 14400, 19200, 28800, 31250, 38400, 57600,
             76800, 115200, 128000, 153600, 230400, 250000, 460800, 500000, 921600, 1000000]


def say(s):
    print(f"[{time.strftime('%H:%M:%S')}] {s}", flush=True)


def capture():
    from saleae import automation
    out = os.path.join(HERE, "..", "captures", "saleae", time.strftime("%H%M%S"))
    os.makedirs(out, exist_ok=True)
    say("connecting to Logic 2 automation server on localhost:10430")
    with automation.Manager.connect(port=10430) as manager:
        devices = manager.get_devices()
        for d in devices:
            say(f"found device {d.device_type} serial {d.device_id}")
        if not devices:
            sys.exit("no Saleae device found: is it plugged in?")
        dev = devices[0].device_id
        dc = automation.LogicDeviceConfiguration(
            enabled_digital_channels=[0, 1],
            enabled_analog_channels=[0],
            digital_sample_rate=500_000_000,
            analog_sample_rate=3_125_000,
            digital_threshold_volts=1.8,
        )
        say("digital ch0 (sign TX) + ch1 (sign RX) at 500 MS/s, threshold 1.8 V; analog ch0 at 3.125 MS/s")
        cc = automation.CaptureConfiguration(
            capture_mode=automation.DigitalTriggerCaptureMode(
                trigger_type=automation.DigitalTriggerType.RISING,
                trigger_channel_index=0,
                after_trigger_seconds=0.05,
                trim_data_seconds=0.1,
            )
        )
        say("armed: trigger on rising edge of ch0 (RS-232 start bit). Bursts come every ~20 s")
        with manager.start_capture(device_id=dev, device_configuration=dc,
                                   capture_configuration=cc) as cap:
            cap.wait()
            say("triggered, exporting")
            cap.export_raw_data_csv(directory=out, digital_channels=[0, 1], analog_channels=[0])
            cap.save_capture(filepath=os.path.join(out, "burst.sal"))
    say(f"saved to {out}")
    analyze(out)


def scan(nch):
    """Timed capture of channels 0..nch-1 for 25 s, for when we don't know which wire is TX."""
    from saleae import automation
    out = os.path.join(HERE, "..", "captures", "saleae", "scan-" + time.strftime("%H%M%S"))
    os.makedirs(out, exist_ok=True)
    chans = list(range(nch))
    say("connecting to Logic 2 automation server on localhost:10430")
    with automation.Manager.connect(port=10430) as manager:
        devices = manager.get_devices()
        if not devices:
            sys.exit("no Saleae device found: is it plugged in?")
        say(f"using {devices[0].device_type} serial {devices[0].device_id}")
        dc = automation.LogicDeviceConfiguration(
            enabled_digital_channels=chans, digital_sample_rate=500_000_000,
            digital_threshold_volts=1.8)
        cc = automation.CaptureConfiguration(
            capture_mode=automation.TimedCaptureMode(duration_seconds=25.0))
        say(f"recording digital ch0-{nch - 1} at 500 MS/s for 25 s (catches at least one burst)")
        with manager.start_capture(device_id=devices[0].device_id, device_configuration=dc,
                                   capture_configuration=cc) as cap:
            cap.wait()
            say("done, exporting")
            cap.export_raw_data_csv(directory=out, digital_channels=chans)
            cap.save_capture(filepath=os.path.join(out, "scan.sal"))
    say(f"saved to {out}")
    analyze(out)


def load_digital(path):
    """Logic 2 digital CSV: one row per transition. Returns {channel: [(t, level), ...]}."""
    with open(path) as f:
        r = csv.reader(f)
        hdr = next(r)
        ch = {name: [] for name in hdr[1:]}
        for row in r:
            t = float(row[0])
            for name, v in zip(hdr[1:], row[1:]):
                lv = int(v)
                if not ch[name] or ch[name][-1][1] != lv:
                    ch[name].append((t, lv))
    return ch


def analyze(out):
    dig = os.path.join(out, "digital.csv")
    ch = load_digital(dig)
    for name, tr in ch.items():
        say(f"{name}: {len(tr) - 1} transitions, starts at level {tr[0][1]}")
    busy = [n for n in ch if len(ch[n]) >= 3]
    if not busy:
        say("no channel shows a burst: check the probes and ground")
        return
    name = max(busy, key=lambda n: len(ch[n]))
    tx = ch[name]
    idle = tx[0][1]
    say(f"analyzing {name}: idle level {idle} "
        f"({'RS-232 side, inverted' if idle == 0 else 'TTL side, not inverted'})")
    edges = tx[1:]
    t0 = edges[0][0]
    widths = [(b[0] - a[0], a[1]) for a, b in zip(edges, edges[1:])]
    say(f"activity on {name}: {len(edges)} edges over {(edges[-1][0] - t0) * 1e6:.1f} us")
    say("pulse list (level as captured; idle level = mark/logic 1 on the wire):")
    for w, lv in widths:
        if w > 0.01:
            print(f"    ... {w:.3f} s gap ...")
            continue
        print(f"    level {lv}  {w * 1e6:9.2f} us")
    shortest = min(w for w, _ in widths)
    say(f"shortest pulse {shortest * 1e6:.2f} us -> about {1 / shortest:.0f} baud if that's one bit")
    analog = os.path.join(out, "analog.csv")
    if os.path.exists(analog):
        vals = []
        with open(analog) as f:
            next(f)
            vals = [float(line.split(",")[1]) for line in f if "," in line]
        if vals:
            say(f"analog ch0: min {min(vals):.2f} V, max {max(vals):.2f} V")
    # try decoding inverted async at standard bauds near the estimate, and at the estimate itself
    est = 1 / shortest
    cands = sorted(set([b for b in STD_BAUDS if 0.6 < b / est < 2.2] + [round(est)]))
    for baud in cands:
        for bits, par in ((8, "N"), (7, "E"), (7, "O"), (8, "E")):
            data, errs = decode(edges, baud, bits, par, idle)
            txt = "".join(chr(c) if 32 <= c < 127 else f"\\x{c:02x}" for c in data)
            say(f"{baud:>7} {bits}{par}1: framing/parity errors {errs}  {data.hex(' ')}  '{txt}'")


def decode(edges, baud, bits, par, idle):
    """Async-decode a channel whose idle (mark) level is `idle`. Returns (bytes, error count)."""
    T = 1 / baud
    def mark(t):  # True = mark (logic 1, the idle level)
        lv = idle
        for te, l in edges:
            if te > t:
                break
            lv = l
        return lv == idle
    out, errs, i = [], 0, 0
    t_end = edges[-1][0] + 12 * T
    t = edges[0][0]
    while t < t_end:
        v = 0
        for k in range(bits):
            v |= (1 if mark(t + T * (1.5 + k)) else 0) << k
        n = 1 + bits
        if par != "N":
            p = 1 if mark(t + T * (1.5 + bits)) else 0
            if (bin(v).count("1") + p) % 2 != (0 if par == "E" else 1):
                errs += 1
            n += 1
        if not mark(t + T * (n + 0.5)):
            errs += 1
        out.append(v)
        nxt = [te for te, l in edges if te > t + T * (n + 0.5) and l != idle]
        if not nxt:
            break
        t = nxt[0]
    return bytes(out), errs


if __name__ == "__main__":
    if len(sys.argv) >= 2 and sys.argv[1] == "capture":
        capture()
    elif len(sys.argv) >= 2 and sys.argv[1] == "scan":
        scan(int(sys.argv[2]) if len(sys.argv) > 2 else 10)
    elif len(sys.argv) >= 3 and sys.argv[1] == "analyze":
        analyze(sys.argv[2])
    else:
        sys.exit(__doc__)
