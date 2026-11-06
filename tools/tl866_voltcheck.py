#!/usr/bin/env python3
"""Measure what the TL866A puts on a PLCC32 adapter while minipro reads an AM29LV040B.
Run it with the adapter EMPTY (no chip) at the handle end of the ZIF.

Wiring (adapter DIP32 pads): GND = pad 16, ch0 = pad 32 (VCC), ch1 = pad 26 (A9),
ch2 = pad 1 (A18 / where the TL866A routes VPP), ch3 = pad 24 (OE#).
Needs Logic 2 running with automation: open -a "Saleae Logic" --args --automation

  tl866_voltcheck.py
"""
import csv, os, subprocess, sys, threading, time

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
from saleae_capture import say  # noqa: E402

LABELS = ["pad 32 VCC", "pad 26 A9", "pad 1 A18/VPP", "pad 24 OE#"]
CHIP = "AM29LV040B@PLCC32"
# Patched minipro (build/): stock 0.7.4 drives 3.3 V chips at 5 V on a TL866A (upstream #387).
MINIPRO = os.environ.get("MINIPRO", os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "build", "install", "bin", "minipro"))


def run_minipro(out):
    time.sleep(2)
    say("minipro: ID check")
    subprocess.run([MINIPRO, "-p", CHIP, "-r", os.path.join(out, "x.bin")],
                   capture_output=True)
    time.sleep(1)
    say("minipro: forced read (powers the socket for about 6 s)")
    subprocess.run([MINIPRO, "-p", CHIP, "-y", "-r", os.path.join(out, "x.bin")],
                   capture_output=True)
    say("minipro: done")


def main():
    from saleae import automation
    out = os.path.join(HERE, "..", "captures", "saleae", "tl866-volt-" + time.strftime("%H%M%S"))
    os.makedirs(out, exist_ok=True)
    with automation.Manager.connect(port=10430) as m:
        dev = m.get_devices()[0].device_id
        dc = automation.LogicDeviceConfiguration(
            enabled_analog_channels=[0, 1, 2, 3], enabled_digital_channels=[0, 1, 2, 3],
            digital_sample_rate=6_250_000, analog_sample_rate=1_562_500)
        cc = automation.CaptureConfiguration(
            capture_mode=automation.TimedCaptureMode(duration_seconds=15.0))
        with m.start_capture(device_id=dev, device_configuration=dc, capture_configuration=cc) as cap:
            say("recording 15 s on pads 32, 26, 1, 24")
            t = threading.Thread(target=run_minipro, args=(out,))
            t.start()
            cap.wait()
            t.join()
            cap.export_raw_data_csv(directory=out, analog_channels=[0, 1, 2, 3])
            cap.save_capture(filepath=os.path.join(out, "tl866-volt.sal"))
    try:
        os.remove(os.path.join(out, "x.bin"))
    except FileNotFoundError:
        pass
    peaks = [[-99.0, 99.0] for _ in LABELS]
    vcc_on = []
    with open(os.path.join(out, "analog.csv")) as f:
        r = csv.reader(f)
        next(r)
        for row in r:
            for i in range(len(LABELS)):
                v = float(row[i + 1])
                peaks[i][0] = max(peaks[i][0], v)
                peaks[i][1] = min(peaks[i][1], v)
            if float(row[1]) > 1.0:
                vcc_on.append(float(row[1]))
    say(f"saved to {out}")
    for lab, (hi, lo) in zip(LABELS, peaks):
        say(f"  {lab:15} max {hi:+6.2f} V   min {lo:+6.2f} V")
    if vcc_on:
        vcc_on.sort()
        say(f"VCC while powered: median {vcc_on[len(vcc_on) // 2]:.2f} V over {len(vcc_on)} samples")
    else:
        say("VCC never rose above 1 V: the socket was never powered on pad 32")


if __name__ == "__main__":
    main()
