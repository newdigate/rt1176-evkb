#!/usr/bin/env python3
"""Timescale decomposition of OUT-FIFO fill traces: which band holds the
envelope difference between hosts?

The fill level integrates the instantaneous host-vs-device rate error, so
each timescale of its wander names a mechanism:
  raw - MA(0.1s)  : packet-size quantization ripple (frame_accum sawtooth)
  MA(0.1s) - MA(1s): fast servo response (dither feed-through, slew steps)
  MA(1s) - MA(10s) : slow servo wander (filter horizon, limit cycles)
  MA(10s)          : drift tracking (thermal, residual ppm)

Envelope (p2p) and rms per band, steady window only (30 s in, like the
judge). Uniform 10 ms resample before filtering; wrap repair comes free via
vcdfill.probe_points.
"""
import sys, os
sys.path.insert(0, os.path.expanduser('~/Development/rt1170/evkb/tools'))
sys.path.insert(0, os.path.expanduser('~/Development/rt1170/evkb/tools/uacvalidate'))
import vcdfill

DT = 0.01
STEADY_SKIP_S = 30.0


def uniform(points):
    t0 = points[0][0] + STEADY_SKIP_S
    pts = [(t, v) for t, v in points if t >= t0]
    out_t, out_v = [], []
    t = pts[0][0]
    i = 0
    while t <= pts[-1][0]:
        while i + 1 < len(pts) and pts[i + 1][0] <= t:
            i += 1
        # sample-and-hold: fill is a step signal between emissions
        out_t.append(t)
        out_v.append(pts[i][1])
        t += DT
    return out_t, out_v


def moving_avg(v, w):
    n = len(v)
    if w >= n:
        return [sum(v) / n] * n
    c = [0.0]
    for x in v:
        c.append(c[-1] + x)
    half = w // 2
    out = []
    for i in range(n):
        a, b = max(0, i - half), min(n, i + half + 1)
        out.append((c[b] - c[a]) / (b - a))
    return out


def band_stats(v):
    p2p = max(v) - min(v)
    m = sum(v) / len(v)
    rms = (sum((x - m) ** 2 for x in v) / len(v)) ** 0.5
    return p2p, rms


def dominant_period(v, dt, min_lag_s, max_lag_s):
    """Crude autocorrelation peak: the servo limit-cycle detector."""
    m = sum(v) / len(v)
    x = [a - m for a in v]
    n = len(x)
    var = sum(a * a for a in x) / n
    if var == 0:
        return None, 0.0
    best_lag, best_r = None, 0.0
    lag = int(min_lag_s / dt)
    max_lag = min(int(max_lag_s / dt), n // 2)
    step = max(1, lag // 50)
    while lag <= max_lag:
        r = sum(x[i] * x[i + lag] for i in range(0, n - lag, 4)) / ((n - lag) / 4) / var
        if r > best_r:
            best_r, best_lag = r, lag
        lag += step
    return (best_lag * dt if best_lag else None), best_r


def analyse(path, label):
    sigs = vcdfill.parse(path)
    pts = vcdfill.fill_points(sigs)
    if not pts:
        print(f"{label}: no fill trace"); return
    t, v = uniform(pts)
    ma01 = moving_avg(v, int(0.1 / DT))
    ma1 = moving_avg(v, int(1.0 / DT))
    ma10 = moving_avg(v, int(10.0 / DT))
    fast = [a - b for a, b in zip(v, ma01)]
    mid = [a - b for a, b in zip(ma01, ma1)]
    slow = [a - b for a, b in zip(ma1, ma10)]
    print(f"\n=== {label} ({t[-1]-t[0]:.0f} s steady, {len(v)} samples) ===")
    print(f"  raw            : p2p {band_stats(v)[0]:6.0f} B  rms {band_stats(v)[1]:6.1f} B")
    for name, band in (("<0.1s (quant) ", fast), ("0.1-1s (servo)", mid),
                       ("1-10s (servo) ", slow), (">10s (drift)  ", ma10)):
        p2p, rms = band_stats(band)
        print(f"  {name} : p2p {p2p:6.0f} B  rms {rms:6.1f} B")
    per, r = dominant_period(ma1, DT, 2.0, 120.0)
    if per and r > 0.3:
        print(f"  periodicity    : {per:.1f} s (autocorr {r:.2f}) in the 1s-MA trace")
    else:
        print(f"  periodicity    : none above autocorr 0.3 (best {r:.2f})")


if __name__ == "__main__":
    if len(sys.argv) < 2:
        print("usage: fillbands.py capture.vcd [label] [capture2.vcd [label2] ...]")
        sys.exit(2)
    args = sys.argv[1:]
    while args:
        path = args.pop(0)
        label = args.pop(0) if args and not args[0].endswith(".vcd") else os.path.basename(path)
        analyse(path, label)
