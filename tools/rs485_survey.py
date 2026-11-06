#!/usr/bin/env python3
"""Find the RS-485 pair on an unknown connector (the sign's RJ45s) with the Saleae, then decode it.

Hook Saleae ch0..ch7 to RJ45 pins 1..8 (a cut patch cable works) and Saleae GND to the sign's
ground (DB9 pin 5). Needs Logic 2 running with automation: open -a "Saleae Logic" --args --automation

  rs485_survey.py [SECONDS] [NCHANNELS]   default 25 s, 8 channels

For each pin it reports the DC level (ground, supply, or a biased RS-485 line), counts edges,
pairs up pins that switch together in opposite directions (an A/B pair), measures the shortest
pulse (the bit time) and decodes the busiest line at nearby standard baud rates.
"""
import csv, os, statistics, sys, time

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
from saleae_capture import STD_BAUDS, decode, load_digital, say  # noqa: E402

# Wire colors for T568B, the usual patch-cable wiring
T568B = ["white/orange", "orange", "white/green", "blue", "white/blue", "green", "white/brown", "brown"]


def capture(seconds, nch):
    from saleae import automation
    out = os.path.join(HERE, "..", "captures", "saleae", "rs485-" + time.strftime("%H%M%S"))
    os.makedirs(out, exist_ok=True)
    chans = list(range(nch))
    with automation.Manager.connect(port=10430) as m:
        devices = m.get_devices()
        if not devices:
            sys.exit("no Saleae device found: is it plugged in?")
        dev = devices[0].device_id
        say(f"using {devices[0].device_type} serial {dev}")
        # Digital at full speed for edges and timing, analog heavily downsampled for DC levels.
        # Allowed combinations depend on the channel count, so try a few.
        tries = [(500_000_000, 1250), (250_000_000, 1250), (125_000_000, 1250), (6_250_000, 1250)]
        cc = automation.CaptureConfiguration(
            capture_mode=automation.TimedCaptureMode(duration_seconds=seconds))
        for drate, arate in tries:
            dc = automation.LogicDeviceConfiguration(
                enabled_digital_channels=chans, enabled_analog_channels=chans,
                digital_sample_rate=drate, analog_sample_rate=arate, digital_threshold_volts=1.8)
            try:
                cap = m.start_capture(device_id=dev, device_configuration=dc, capture_configuration=cc)
            except automation.errors.InvalidRequestError:
                say(f"digital {drate / 1e6:g} MS/s + analog {arate} S/s not allowed, trying lower")
                continue
            say(f"recording ch0-{nch - 1}: digital {drate / 1e6:g} MS/s (threshold 1.8 V), "
                f"analog {arate} S/s, for {seconds:g} s")
            with cap:
                cap.wait()
                say("done, exporting")
                cap.export_raw_data_csv(directory=out, digital_channels=chans, analog_channels=chans)
                cap.save_capture(filepath=os.path.join(out, "rs485.sal"))
            break
        else:
            sys.exit("no allowed sample-rate combination found")
    say(f"saved to {out}")
    return out


def analyze(out):
    with open(os.path.join(out, "analog.csv")) as f:
        r = csv.reader(f)
        hdr = next(r)
        cols = [[] for _ in hdr[1:]]
        for row in r:
            for i, v in enumerate(row[1:]):
                cols[i].append(float(v))
    dig = load_digital(os.path.join(out, "digital.csv"))
    names = list(dig)

    say("per pin (RJ45 pin = channel + 1, T568B colors):")
    levels = {}
    for i, (name, v) in enumerate(zip(hdr[1:], cols)):
        mean, lo, hi = statistics.mean(v), min(v), max(v)
        edges = len(dig[names[i]]) - 1 if i < len(names) else 0
        if abs(mean) < 0.15 and hi - lo < 0.3:
            kind = "ground or unconnected"
        elif hi - lo < 0.3 and abs(mean) > 4.5:
            kind = "steady supply?"
        elif 0.3 < mean < 4.5:
            kind = "biased line (RS-485 candidate)"
        else:
            kind = "?"
        levels[i] = mean
        pin = i + 1
        color = T568B[i] if i < 8 else ""
        say(f"  pin {pin} ({color:12}) mean {mean:+6.2f} V  min {lo:+6.2f}  max {hi:+6.2f}  "
            f"{edges:6d} edges  -> {kind}")

    # pins that switch at the same instants in opposite directions form a differential pair
    busy = [n for n in names if len(dig[n]) > 3]
    if not busy:
        say("no switching on any pin: the bus was idle. If a pair of pins sits a few hundred mV apart "
            "around 1-4 V, that's probably the idle RS-485 pair; the sign may only answer when polled.")
        return
    say(f"pins with activity: {', '.join(busy)}")
    pairs = []
    for a in busy:
        for b in busy:
            if a >= b:
                continue
            ta = [t for t, _ in dig[a][1:]]
            tb = [t for t, _ in dig[b][1:]]
            if not ta or not tb:
                continue
            j, match = 0, 0
            for t in ta:
                while j < len(tb) and tb[j] < t - 2e-6:
                    j += 1
                if j < len(tb) and abs(tb[j] - t) < 2e-6:
                    match += 1
            score = match / max(len(ta), len(tb))
            if score > 0.5:
                pairs.append((a, b, score))
    for a, b, sc in pairs:
        say(f"  {a} and {b} switch together ({sc:.0%} of edges): likely the A/B pair")

    line = max(busy, key=lambda n: len(dig[n]))
    tr = dig[line]
    idle, edges = tr[0][1], tr[1:]
    widths = [b[0] - a[0] for a, b in zip(edges, edges[1:])]
    shortest = min(w for w in widths if w > 0)
    say(f"decoding {line}: idle level {idle}, {len(edges)} edges, shortest pulse "
        f"{shortest * 1e6:.2f} us (~{1 / shortest:.0f} baud)")
    est = 1 / shortest
    for baud in sorted(set([b for b in STD_BAUDS if 0.6 < b / est < 2.2])):
        data, errs = decode(edges[:4000], baud, 8, "N", idle)
        say(f"  {baud:>7} 8N1: {errs} framing errors  {data[:48].hex(' ')}")


if __name__ == "__main__":
    secs = float(sys.argv[1]) if len(sys.argv) > 1 else 25
    n = int(sys.argv[2]) if len(sys.argv) > 2 else 8
    analyze(capture(secs, n))
