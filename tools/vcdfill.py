#!/usr/bin/env python3
"""Analyse an xscope VCD from the instrumented lib_xua decoupler.

Signals (xscope_register order in lib_xua/src/core/main.xc):
  id 0 out_underflow  -- stream-reset underflow count (event emissions)
  id 1 out_overflow   -- OUT FIFO overflow count (event emissions)
  id 2 out_fifo_fill  -- OUT FIFO occupancy in bytes, ~100 Hz
  id 3 out_dryout     -- runtime dry-out count, emitted at the fill cadence
  id 255 Missing_Data -- xscope's own lost-sample marker

The fill signal is the instrument: its slope is the host-vs-device rate error.
  drift[bytes/s] = 4 bytes/frame * 44100 frame/s * (ppm error)*1e-6
                 = 0.17640 * ppm
A block correction (dry-out silence insertion, or overflow packet-dropping)
shows as a discontinuity in fill and a step in the matching counter. Overflow
additionally gaps the emissions entirely while packets are being dropped,
because emission rides on packet arrival.

Usage: vcdfill.py file.vcd [--plot out.png] [--json]
"""
import sys, json

TICK = None  # seconds per VCD tick, read from $timescale


def parse(path):
    global TICK
    sigs = {}      # id -> list[(t_ticks, value)]
    t = 0
    with open(path) as f:
        lines = f.read().split("\n")
    i = 0
    # header
    while i < len(lines):
        ln = lines[i].strip()
        if ln == "$timescale":
            ts = lines[i + 1].strip().split()
            mag = float(ts[0])
            unit = {"s": 1, "ms": 1e-3, "us": 1e-6, "ns": 1e-9, "ps": 1e-12}[ts[1]]
            TICK = mag * unit
        if ln.startswith("$var"):
            parts = ln.split()
            sid = parts[3]
            sigs[sid] = []
        if ln == "$enddefinitions $end":
            i += 1
            break
        i += 1
    for ln in lines[i:]:
        if not ln:
            continue
        c = ln[0]
        if c == "#":
            t = int(ln[1:])
        elif c == "b":
            val_s, sid = ln[1:].split()
            sigs[sid].append((t, int(val_s, 2)))
    return sigs


def fit_slope(pts):
    """OLS slope over [(t_s, v)] -> (slope per s, r2, n)."""
    n = len(pts)
    if n < 8:
        return None
    sx = sum(p[0] for p in pts) / n
    sy = sum(p[1] for p in pts) / n
    sxx = sum((p[0] - sx) ** 2 for p in pts)
    sxy = sum((p[0] - sx) * (p[1] - sy) for p in pts)
    if sxx == 0:
        return None
    b = sxy / sxx
    syy = sum((p[1] - sy) ** 2 for p in pts)
    r2 = (sxy * sxy) / (sxx * syy) if syy > 0 else 1.0
    return (b, r2, n)


def analyse(path, want_json=False, plot=None):
    sigs = parse(path)
    fill = [(t * TICK, v) for t, v in sigs.get("2", [])]
    out = {"file": path}
    if not fill:
        print(json.dumps({"error": "no fill samples"}) if want_json
              else "no fill samples")
        return 1
    t0, t1 = fill[0][0], fill[-1][0]
    out["duration_s"] = round(t1 - t0, 2)
    out["fill_n"] = len(fill)
    vals = [v for _, v in fill]
    out["fill_min"] = min(vals)
    out["fill_max"] = max(vals)
    out["fill_mean"] = round(sum(vals) / len(vals), 1)

    # counters: absolute value emissions -> step times
    for sid, name in (("0", "underflow"), ("1", "overflow"), ("3", "dryout")):
        pts = [(t * TICK, v) for t, v in sigs.get(sid, [])]
        steps = []
        last = None
        for ts, v in pts:
            if last is not None and v != last:
                steps.append((round(ts, 3), v))
            last = v
        # event-style probes (0,1) emit only on the event, so every
        # emission after the first of a value is a step; capture first too
        if pts and sid in ("0", "1"):
            steps = [(round(ts, 3), v) for ts, v in pts]
        out[name + "_events"] = steps
        out[name + "_final"] = pts[-1][1] if pts else 0

    # missing data marker
    md = sigs.get("255", [])
    out["xscope_missing_marks"] = len([1 for _, v in md if v])

    # segment the fill trace at discontinuities and emission gaps
    segs = []
    cur = [fill[0]]
    for prev, nxt in zip(fill, fill[1:]):
        dv = nxt[1] - prev[1]
        dt = nxt[0] - prev[0]
        if abs(dv) > 300 or dt > 0.5:
            segs.append(cur)
            cur = []
        cur.append(nxt)
    segs.append(cur)
    seg_rows = []
    for s in segs:
        if len(s) < 8:
            continue
        dur = s[-1][0] - s[0][0]
        if dur < 2.0:
            continue
        r = fit_slope(s)
        if not r:
            continue
        b, r2, n = r
        seg_rows.append({
            "t_start": round(s[0][0], 2), "t_end": round(s[-1][0], 2),
            "dur_s": round(dur, 1), "slope_Bps": round(b, 3),
            "ppm_equiv": round(b / 0.17640, 1), "r2": round(r2, 4), "n": n,
        })
    out["segments"] = seg_rows
    if seg_rows:
        tot = sum(r["dur_s"] for r in seg_rows)
        wm = sum(r["slope_Bps"] * r["dur_s"] for r in seg_rows) / tot
        out["weighted_slope_Bps"] = round(wm, 3)
        out["weighted_ppm_equiv"] = round(wm / 0.17640, 1)

    if plot:
        try:
            import matplotlib
            matplotlib.use("Agg")
            import matplotlib.pyplot as plt
            fig, ax = plt.subplots(figsize=(12, 4))
            ax.plot([p[0] for p in fill], [p[1] for p in fill],
                    lw=0.5, color="#2563eb")
            for ts, _ in out["dryout_events"]:
                ax.axvline(ts, color="#dc2626", lw=0.8, alpha=0.7)
            for ts, _ in out["overflow_events"]:
                ax.axvline(ts, color="#d97706", lw=0.8, alpha=0.7)
            ax.set_xlabel("time (s)")
            ax.set_ylabel("OUT FIFO fill (bytes)")
            ax.set_title(f"{path}  red=dryout amber=overflow")
            fig.tight_layout()
            fig.savefig(plot, dpi=110)
            out["plot"] = plot
        except ImportError:
            out["plot"] = "matplotlib unavailable"

    if want_json:
        print(json.dumps(out, indent=1))
    else:
        for k, v in out.items():
            if k == "segments":
                print("segments:")
                for r in v:
                    print(f"  {r['t_start']:8.1f}..{r['t_end']:8.1f}s "
                          f"slope={r['slope_Bps']:+9.3f} B/s "
                          f"({r['ppm_equiv']:+7.1f} ppm) r2={r['r2']}")
            else:
                print(f"{k}: {v}")
    return 0


if __name__ == "__main__":
    args = [a for a in sys.argv[1:]]
    want_json = "--json" in args
    plot = None
    if "--plot" in args:
        plot = args[args.index("--plot") + 1]
        args.remove("--plot")
        args.remove(plot)
    if want_json:
        args.remove("--json")
    sys.exit(analyse(args[0], want_json, plot))
