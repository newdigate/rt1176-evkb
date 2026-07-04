#!/usr/bin/env python3
"""Capture a short window with the Saleae and report, per digital channel, the
measured PWM frequency and duty cycle. Auto-detects which channel carries the
square wave (the one wired to header D9). Requires Logic 2 with the automation
server enabled (port 10430)."""
import sys, os, glob, tempfile, csv
from saleae import automation

DURATION = float(sys.argv[1]) if len(sys.argv) > 1 else 0.2
SAMPLE_HZ = 10_000_000

def analyze(times, vals):
    # times: list of transition timestamps (s); vals: level AFTER each transition.
    # Reconstruct high-time and period from edges.
    rises = [t for t, v in zip(times, vals) if v == 1]
    if len(rises) < 2:
        # maybe steady level
        steady = vals[-1] if vals else None
        return None, None, steady
    periods = [rises[i+1] - rises[i] for i in range(len(rises)-1)]
    period = sum(periods) / len(periods)
    freq = 1.0 / period if period > 0 else 0
    # high time: for each rise, find next fall
    highs = []
    for i in range(len(times)-1):
        if vals[i] == 1:
            highs.append(times[i+1] - times[i])
    duty = (sum(highs)/len(highs)) / period * 100.0 if highs and period else 0
    return freq, duty, None

with automation.Manager.connect(port=10430) as mgr:
    devices = mgr.get_devices()
    if not devices:
        print("no Saleae device found"); sys.exit(2)
    dev_id = devices[0].device_id
    cfg = automation.LogicDeviceConfiguration(
        enabled_digital_channels=[0,1,2,3,4,5,6,7],
        digital_sample_rate=SAMPLE_HZ,
    )
    cap_cfg = automation.CaptureConfiguration(
        capture_mode=automation.TimedCaptureMode(duration_seconds=DURATION))
    with mgr.start_capture(device_id=dev_id, device_configuration=cfg,
                           capture_configuration=cap_cfg) as cap:
        cap.wait()
        outdir = tempfile.mkdtemp()
        cap.export_raw_data_csv(directory=outdir, digital_channels=[0,1,2,3,4,5,6,7])
        # find the csv
        files = glob.glob(os.path.join(outdir, "*.csv"))
        print("csv files:", files)
        path = files[0]
        # Saleae raw digital CSV: header "Time [s], Channel 0, Channel 1, ..."; one row per SAMPLE.
        with open(path) as f:
            r = csv.reader(f); header = next(r)
            nch = len(header) - 1
            last = [None]*nch
            trans = [([],[]) for _ in range(nch)]   # per channel: (times, vals)
            for row in r:
                if not row: continue
                t = float(row[0])
                for c in range(nch):
                    v = int(float(row[c+1]))
                    if last[c] is None or v != last[c]:
                        trans[c][0].append(t); trans[c][1].append(v)
                    last[c] = v
        print(f"channels={nch}")
        for c in range(nch):
            times, vals = trans[c]
            freq, duty, steady = analyze(times, vals)
            if freq:
                print(f"CH{c}: freq={freq:8.1f} Hz  duty={duty:5.1f} %   (edges={len(times)})")
            elif steady is not None:
                print(f"CH{c}: steady {'HIGH' if steady else 'LOW'}")
            else:
                print(f"CH{c}: (no data)")
