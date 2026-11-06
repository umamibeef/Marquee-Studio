#!/usr/bin/env python3
"""Does the firmware ever talk to the Ethernet PHY? Record the LXT972A's management bus and
MII transmit-enable through a power-on, then decode any MDIO frames.

Wiring (LXT972A, LQFP-64, pin 1 top-left by the Y2 crystal; datasheet in ../docs):
  ch0 = pin 43 MDC, ch1 = pin 42 MDIO, ch2 = pin 56 TX_EN, GND = pin 41 (or any board ground)
Needs Logic 2 running with automation: open -a "Saleae Logic" --args --automation

  mdio_probe.py [SECONDS]      record for SECONDS (default 90); power the sign on once it says so
  mdio_probe.py analyze DIR    re-decode an earlier capture
"""
import bisect, os, sys, time

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
from saleae_capture import load_digital, say  # noqa: E402

# LXT972A / IEEE 802.3 clause 22 register names
REGS = {0: "control", 1: "status", 2: "PHY ID 1", 3: "PHY ID 2", 4: "autoneg advertise",
        5: "link partner ability", 6: "autoneg expansion", 16: "port config", 17: "status #2",
        18: "interrupt enable", 19: "interrupt status", 20: "LED config"}


def capture(seconds):
    from saleae import automation
    out = os.path.join(HERE, "..", "captures", "saleae", "mdio-" + time.strftime("%H%M%S"))
    os.makedirs(out, exist_ok=True)
    with automation.Manager.connect(port=10430) as m:
        devices = m.get_devices()
        if not devices:
            sys.exit("no Saleae device found: is it plugged in?")
        dev = devices[0].device_id
        cc = automation.CaptureConfiguration(
            capture_mode=automation.TimedCaptureMode(duration_seconds=seconds))
        for rate in (500_000_000, 250_000_000, 100_000_000, 50_000_000):
            dc = automation.LogicDeviceConfiguration(
                enabled_digital_channels=[0, 1, 2], digital_sample_rate=rate,
                digital_threshold_volts=1.8)
            try:
                cap = m.start_capture(device_id=dev, device_configuration=dc, capture_configuration=cc)
            except automation.errors.InvalidRequestError:
                continue
            say(f"recording MDC/MDIO/TX_EN at {rate / 1e6:g} MS/s for {seconds:g} s: POWER THE SIGN ON NOW")
            with cap:
                cap.wait()
                say("done, exporting")
                cap.export_raw_data_csv(directory=out, digital_channels=[0, 1, 2])
                cap.save_capture(filepath=os.path.join(out, "mdio.sal"))
            break
        else:
            sys.exit("no allowed sample rate found")
    say(f"saved to {out}")
    return out


def level_at(trans, t):
    """Level of a channel at time t, from its (time, level) transition list."""
    i = bisect.bisect_right([x for x, _ in trans], t) - 1
    return trans[max(i, 0)][1]


def analyze(out):
    ch = load_digital(os.path.join(out, "digital.csv"))
    names = list(ch)
    mdc, mdio, txen = (ch[n] for n in names[:3])
    for label, tr in (("MDC (pin 43)", mdc), ("MDIO (pin 42)", mdio), ("TX_EN (pin 56)", txen)):
        say(f"{label}: starts at level {tr[0][1]}, {len(tr) - 1} transitions")

    rises = [t for (t, lv), (_, prev) in zip(mdc[1:], mdc) if lv == 1 and prev == 0]
    if not rises:
        say("RESULT: MDC never clocked. The firmware never talked to the PHY during this capture.")
    else:
        say(f"MDC clocked {len(rises)} times, first at {rises[0]:.6f} s")
        tlist = [x for x, _ in mdio]
        bits = []
        for t in rises:
            i = bisect.bisect_right(tlist, t) - 1
            bits.append(mdio[max(i, 0)][1])
        decode_frames(bits, rises)
    if len(txen) > 1:
        highs = sum(1 for t, lv in txen[1:] if lv == 1)
        say(f"RESULT: TX_EN went high {highs} times: the MAC tried to transmit "
            f"(first at {txen[1][0]:.6f} s)")
    else:
        say("RESULT: TX_EN never went high: the MAC never transmitted a frame.")


def decode_frames(bits, times):
    """IEEE 802.3 clause 22: >=32 ones preamble, ST=01, OP (10 read / 01 write), PHYAD[5], REGAD[5], TA[2], DATA[16]."""
    i, n, found = 0, len(bits), 0
    while i < n - 32:
        # find start-of-frame 0,1 after at least a few preamble ones
        if bits[i] == 0 and bits[i + 1] == 1 and i >= 8 and all(bits[i - 8:i]):
            op = bits[i + 2] * 2 + bits[i + 3]
            phy = int("".join(map(str, bits[i + 4:i + 9])), 2)
            reg = int("".join(map(str, bits[i + 9:i + 14])), 2)
            data = int("".join(map(str, bits[i + 16:i + 32])), 2)
            kind = {2: "READ ", 1: "WRITE"}.get(op, f"op{op}")
            say(f"  {times[i]:10.6f} s  {kind} phy {phy:2d} reg {reg:2d} ({REGS.get(reg, '?')}) "
                f"= 0x{data:04x}")
            found += 1
            i += 32
        else:
            i += 1
    say(f"decoded {found} MDIO frames" + ("" if found else " (clocking seen but no valid frames)"))


if __name__ == "__main__":
    a = sys.argv[1:]
    if a and a[0] == "analyze":
        analyze(a[1])
    else:
        analyze(capture(float(a[0]) if a else 90))
