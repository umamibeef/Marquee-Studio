#!/usr/bin/env python3
"""Record all 16 pins of the CPU board's 2x8 DEBUG PORT through a power-on and characterize each one.

Wiring (header as in photos/debug-port-jp2.jpg, "16" printed at the top-right corner):
  top row, left to right    -> ch0..ch7   (probably pins 2, 4, ..., 16)
  bottom row, left to right -> ch8..ch15  (probably pins 1, 3, ..., 15)
  Saleae GND -> board ground (DB9 pin 5, or any header pin that turns out to be ground)
Needs Logic 2 running with automation: open -a "Saleae Logic" --args --automation

  debugport_probe.py [SECONDS]   default 60; power the sign on after it says so
  debugport_probe.py analyze DIR
"""
import csv, os, statistics, sys, time

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
from saleae_capture import STD_BAUDS, decode, load_digital, say  # noqa: E402

CH = list(range(16))


def where(ch):
    row, col = ("top", ch) if ch < 8 else ("bottom", ch - 8)
    pin = 2 * (col + 1) if row == "top" else 2 * col + 1
    return f"{row} #{col + 1} (pin {pin}?)"


def capture(seconds):
    from saleae import automation
    out = os.path.join(HERE, "..", "captures", "saleae", "debugport-" + time.strftime("%H%M%S"))
    os.makedirs(out, exist_ok=True)
    with automation.Manager.connect(port=10430) as m:
        devices = m.get_devices()
        if not devices:
            sys.exit("no Saleae device found: is it plugged in?")
        dev = devices[0].device_id
        cc = automation.CaptureConfiguration(
            capture_mode=automation.TimedCaptureMode(duration_seconds=seconds))
        for drate in (500_000_000, 250_000_000, 125_000_000, 100_000_000, 50_000_000, 25_000_000):
            dc = automation.LogicDeviceConfiguration(
                enabled_digital_channels=CH, enabled_analog_channels=CH,
                digital_sample_rate=drate, analog_sample_rate=1250, digital_threshold_volts=1.8)
            try:
                cap = m.start_capture(device_id=dev, device_configuration=dc, capture_configuration=cc)
            except automation.errors.InvalidRequestError:
                continue
            say(f"recording all 16 pins: digital {drate / 1e6:g} MS/s, analog 1250 S/s, {seconds:g} s")
            say(">>> POWER THE SIGN ON NOW <<<")
            with cap:
                cap.wait()
                say("done, exporting")
                cap.export_raw_data_csv(directory=out, digital_channels=CH, analog_channels=CH)
                cap.save_capture(filepath=os.path.join(out, "debugport.sal"))
            break
        else:
            sys.exit("no allowed sample-rate combination found")
    say(f"saved to {out}")
    return out


def analyze(out):
    with open(os.path.join(out, "analog.csv")) as f:
        r = csv.reader(f)
        next(r)
        rows = [[float(v) for v in row] for row in r]
    tail = rows[int(len(rows) * 0.8):]  # last 20 %: the sign has booted by then
    dig = load_digital(os.path.join(out, "digital.csv"))
    names = list(dig)

    say("per pin, after boot (last 20 % of the capture):")
    active = []
    for ch in CH:
        v = [row[ch + 1] for row in tail]
        allv = [row[ch + 1] for row in rows]
        mean = statistics.mean(v)
        edges = len(dig[names[ch]]) - 1
        if abs(mean) < 0.2 and max(v) - min(v) < 0.3:
            kind = "ground (or floating)"
        elif max(v) - min(v) < 0.3 and 2.9 < mean < 3.6:
            kind = "steady 3.3 V (supply or pulled-up)"
        elif max(v) - min(v) < 0.3 and 4.5 < mean < 5.4:
            kind = "steady 5 V (supply or pulled-up)"
        else:
            kind = "in between / changing"
        say(f"  ch{ch:<2} {where(ch):24} {mean:+5.2f} V (whole run {min(allv):+5.2f}..{max(allv):+5.2f})"
            f"  {edges:7d} edges  {kind}")
        if edges > 2:
            active.append(ch)

    if not active:
        say("RESULT: no pin switched during the whole power-on. Nothing on this header talks by itself;"
            " it's probably JTAG/BDM, which only moves when a debugger drives it.")
        return
    for ch in active:
        tr = dig[names[ch]]
        idle, edges = tr[0][1], tr[1:]
        widths = [b[0] - a[0] for a, b in zip(edges, edges[1:]) if b[0] > a[0]]
        say(f"--- ch{ch} {where(ch)}: {len(edges)} edges, first at {edges[0][0]:.4f} s, idle level {idle}")
        if not widths:
            continue
        shortest = min(widths)
        spread = statistics.pstdev(widths[:2000]) / statistics.mean(widths[:2000])
        if len(edges) > 1000 and spread < 0.05:
            say(f"    looks like a steady clock: about {1 / (2 * statistics.mean(widths[:2000])) / 1e6:.3f} MHz")
            continue
        say(f"    shortest pulse {shortest * 1e6:.2f} us (~{1 / shortest:.0f} baud if serial)")
        est = 1 / shortest
        for baud in sorted(set([b for b in STD_BAUDS if 0.7 < b / est < 1.6])):
            data, errs = decode(edges[:20000], baud, 8, "N", idle)
            txt = "".join(chr(c) if 32 <= c < 127 or c in (10, 13) else "." for c in data[:400])
            say(f"    {baud} 8N1: {errs} framing errors in {len(data)} bytes: {txt!r}")


if __name__ == "__main__":
    a = sys.argv[1:]
    if a and a[0] == "analyze":
        analyze(a[1])
    else:
        analyze(capture(float(a[0]) if a else 60))
