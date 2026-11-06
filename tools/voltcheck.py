import os, sys, statistics
from saleae import automation
out = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "captures", "saleae", "voltcheck")
os.makedirs(out, exist_ok=True)
with automation.Manager.connect(port=10430) as m:
    dev = m.get_devices()[0].device_id
    dc = automation.LogicDeviceConfiguration(enabled_digital_channels=[0, 1], enabled_analog_channels=[0, 1],
        digital_sample_rate=6_250_000, analog_sample_rate=781_250, digital_threshold_volts=1.8)
    cc = automation.CaptureConfiguration(capture_mode=automation.TimedCaptureMode(duration_seconds=1.0))
    with m.start_capture(device_id=dev, device_configuration=dc, capture_configuration=cc) as cap:
        cap.wait()
        cap.export_raw_data_csv(directory=out, analog_channels=[0, 1])
with open(os.path.join(out, "analog.csv")) as f:
    hdr = next(f).strip().split(",")
    cols = [[] for _ in hdr[1:]]
    for line in f:
        p = line.split(",")
        for i, v in enumerate(p[1:]):
            cols[i].append(float(v))
for name, v in zip(hdr[1:], cols):
    print(f"{name}: mean {statistics.mean(v):+.3f} V  min {min(v):+.3f}  max {max(v):+.3f}")
