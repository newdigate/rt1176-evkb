#!/usr/bin/env python3
"""Does the >10 s fill drift track the device's own feedback report?

The capture carries fb_value (the device's raw report) in every state
block. If integrating (EMA_horizon(fb) - long_mean(fb)) reproduces the
observed slow fill drift, the drift is fb-measurement noise leaking through
a too-short filter horizon -- and the fix is a longer horizon, not slew or
packet-size work. If it does not correlate, the drift is elsewhere
(delivery integration, device-side consumption wander).
"""
import sys, os
sys.path.insert(0, os.path.expanduser('~/Development/rt1170/evkb/tools'))
sys.path.insert(0, os.path.expanduser('~/Development/rt1170/evkb/tools/uacvalidate'))
from wireformat import read_blocks
import vcdfill

STEADY_SKIP_S = 30.0
BYTES_PER_FRAME = 32          # 8ch x 4B at UAC2/HS
MHZ_PER_RAW = 8e6 / 65536     # Q16.16 samples/uframe -> mHz


def series(path):
    blocks = read_blocks(path)
    t0 = blocks[0][0] + STEADY_SKIP_S
    out = [(t, b.fb_value * MHZ_PER_RAW) for t, b in blocks
           if t >= t0 and b.fb_value]
    return out


def ema(pts, tau_s):
    """Continuous-time EMA over irregular samples."""
    import math
    out = []
    avg = pts[0][1]
    tprev = pts[0][0]
    for t, v in pts:
        dt = t - tprev
        a = 1.0 - math.exp(-dt / tau_s) if tau_s > 0 else 1.0
        avg += a * (v - avg)
        out.append((t, avg))
        tprev = t
    return out


def integrate_error(ema_pts, mean_mhz):
    """Predicted fill trajectory in bytes from a sizing-rate error."""
    out = []
    acc = 0.0
    tprev = ema_pts[0][0]
    for t, v in ema_pts:
        acc += (v - mean_mhz) * 1e-3 * BYTES_PER_FRAME * (t - tprev)
        out.append((t, acc))
        tprev = t
    return out


def resample(pts, grid):
    out = []
    i = 0
    for t in grid:
        while i + 1 < len(pts) and pts[i + 1][0] <= t:
            i += 1
        out.append(pts[i][1])
    return out


def corr(a, b):
    n = len(a)
    ma, mb = sum(a) / n, sum(b) / n
    sa = (sum((x - ma) ** 2 for x in a)) ** 0.5
    sb = (sum((x - mb) ** 2 for x in b)) ** 0.5
    if sa == 0 or sb == 0:
        return 0.0
    return sum((x - ma) * (y - mb) for x, y in zip(a, b)) / (sa * sb)


def moving_avg_ts(t, v, w_s):
    # same shape as fillbands: centered MA on a uniform grid
    dt = t[1] - t[0]
    w = int(w_s / dt)
    c = [0.0]
    for x in v:
        c.append(c[-1] + x)
    half = w // 2
    out = []
    for i in range(len(v)):
        a, b = max(0, i - half), min(len(v), i + half + 1)
        out.append((c[b] - c[a]) / (b - a))
    return out


def analyse(path, label):
    fb = series(path)
    if not fb:
        print(f"{label}: no fb_value"); return
    vals = [v for _, v in fb]
    mean = sum(vals) / len(vals)
    uniq = sorted(set(round(v / MHZ_PER_RAW) for v in vals))
    print(f"\n=== {label} ===")
    print(f"  fb_value: {len(fb)} samples, mean {mean/1000:.3f} Hz, "
          f"{len(uniq)} distinct raw values "
          f"(quantum {MHZ_PER_RAW:.1f} mHz = {MHZ_PER_RAW/44100:.2f} ppm)")
    lo, hi = uniq[0], uniq[-1]
    print(f"  raw span {lo}..{hi} ({(hi-lo)} steps)")
    # 10 s means of the report: does the DEVICE's report itself wander?
    grid = [fb[0][0] + i for i in range(int(fb[-1][0] - fb[0][0]))]
    fb_grid = resample(fb, grid)
    fb_ma10 = moving_avg_ts(grid, fb_grid, 10.0)
    span = max(fb_ma10) - min(fb_ma10)
    print(f"  10s-mean report wander: {span:.1f} mHz p2p ({span/44100:.3f} ppm)")

    # Observed slow fill drift (>10 s band), same grid.
    sigs = vcdfill.parse(path)
    fill = [(t, v) for t, v in vcdfill.fill_points(sigs) if t >= fb[0][0]]
    fill_grid = resample(fill, grid)
    fill_ma10 = moving_avg_ts(grid, fill_grid, 10.0)

    # Predicted drift for candidate horizons.
    for tau, note in ((0.128, "shipped 128 ms"), (0.5, "500 ms"),
                      (1.0, "1 s"), (4.0, "4 s")):
        pred = integrate_error(ema(fb, tau), mean)
        pred_grid = resample(pred, grid)
        pred_ma10 = moving_avg_ts(grid, pred_grid, 10.0)
        r = corr(pred_ma10, fill_ma10)
        p2p = max(pred_ma10) - min(pred_ma10)
        print(f"  horizon {note:>14}: predicted slow-band p2p {p2p:6.0f} B, "
              f"corr with observed {r:+.2f}")
    print(f"  observed slow-band p2p: {max(fill_ma10)-min(fill_ma10):.0f} B")


if __name__ == "__main__":
    if len(sys.argv) < 2:
        print("usage: fbdrift.py [--fs] capture.vcd [label] ...")
        sys.exit(2)
    args = sys.argv[1:]
    if args and args[0] == "--fs":
        # UAC1: 10.14 samples/frame, and set BYTES_PER_FRAME to the FS
        # geometry (2ch x 2B). CAVEAT: at FS the block cadence subsamples
        # the 62.5/s reports ~6:1, so the reconstructed view of what the
        # host integrated is itself aliased -- treat FS correlations as
        # inconclusive either way (measured: they were).
        args.pop(0)
        globals()["MHZ_PER_RAW"] = 1e6 / 16384
        globals()["BYTES_PER_FRAME"] = 4
    while args:
        path = args.pop(0)
        label = args.pop(0) if args and not args[0].endswith(".vcd") else os.path.basename(path)
        analyse(path, label)
