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
NAMES = {}   # VCD id -> signal name, read from $var; both set by parse()

# The fill probe, by the two ways it can be identified. The name is the
# contract and the id is the historical accident: xscope assigns ids in
# xscope_register order, so "2" is out_fifo_fill only for as long as nobody
# prepends a probe. Callers added for the validator resolve by name and fall
# back to the id; analyse() keeps using the id alone so its behaviour on
# captures predating this change is bit-identical.
FILL_SIGNAL_NAME = "out_fifo_fill"
FILL_SIGNAL_ID = "2"

# The four decoupler probes, in xscope_register order. Probes 0, 1 and 3 are
# the block-correction counters: a report that shows drift without showing how
# many corrections it already cost is only half the picture.
PROBE_IDS = {"out_underflow": "0", "out_overflow": "1",
             FILL_SIGNAL_NAME: FILL_SIGNAL_ID, "out_dryout": "3"}

# Segmentation thresholds. A block correction (dry-out silence insertion or
# overflow packet-dropping) moves the fill level by hundreds of bytes in one
# sample; an overflow additionally gaps the emissions, because emission rides
# on packet arrival. Either is a discontinuity, and a fit spanning one measures
# the correction rather than the drift it is supposed to reveal.
SEGMENT_STEP_BYTES = 300
SEGMENT_GAP_S = 0.5
# A segment shorter than this cannot carry a meaningful slope: 8 points is
# fit_slope's own minimum, and 2 s of a ~100 Hz probe is the shortest window in
# which the drift exceeds the sample-to-sample jitter.
MIN_SEGMENT_POINTS = 8
MIN_SEGMENT_SECONDS = 2.0


def parse(path):
    global TICK, NAMES
    sigs = {}      # id -> list[(t_ticks, value)]
    names = {}     # id -> signal name
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
            if len(parts) > 4:
                names[sid] = parts[4]
        if ln == "$enddefinitions $end":
            i += 1
            break
        i += 1
    NAMES = names
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


def segments(points):
    """Split [(t_s, v)] at block corrections; return the fittable pieces.

    THE segmenter for this module -- extracted from analyse(), which used to
    inline it, so that the per-segment table it prints and the single slope
    fit_fill_slope() hands the validator are cut the same way. Two segmenters
    could disagree, and then vcdfill's own report and the validator's W1
    verdict would be measuring different things while both calling it "the
    drift".

    Pieces too short to fit are dropped here rather than by the caller: a
    two-sample fragment between two corrections has a slope, and it is noise.
    """
    if not points:
        return []
    segs = []
    cur = [points[0]]
    for prev, nxt in zip(points, points[1:]):
        dv = nxt[1] - prev[1]
        dt = nxt[0] - prev[0]
        if abs(dv) > SEGMENT_STEP_BYTES or dt > SEGMENT_GAP_S:
            segs.append(cur)
            cur = []
        cur.append(nxt)
    segs.append(cur)
    return [s for s in segs
            if len(s) >= MIN_SEGMENT_POINTS
            and s[-1][0] - s[0][0] >= MIN_SEGMENT_SECONDS]


def probe_points(sigs, name):
    """One decoupler probe's samples as [(t_s, v)], or [] if absent.

    Resolved by signal NAME first. The id fallback is refused for a capture
    that declares uacv_w* state words, because those come from
    uacvalidate/wireformat.py's synth_vcd, whose VCD identifiers are printable
    characters starting at '!' -- identifier "2" there is chr(50), which is
    state word 17 (pat_first_expected), not FIFO occupancy. Fitting a drift
    slope to a state counter would produce a confident, meaningless number:
    exactly the 0.222-slope failure the validator's manifest rule exists to
    prevent. Returning nothing makes the validator report SKIP, which is the
    honest answer for a capture that has no probe of that name in it.
    """
    if TICK is None:
        return []
    sid = None
    for s, n in NAMES.items():
        if n == name:
            sid = s
            break
    if sid is None:
        if any(n.startswith("uacv_w") for n in NAMES.values()):
            return []
        sid = PROBE_IDS.get(name)
        if sid is None:
            return []
    # Phantom-wrap repair, per signal: the capture chain sometimes double-
    # counts its own 32-bit timestamp wrap, jumping the clock forward by an
    # exact multiple of 42.94967296 s between samples that were milliseconds
    # apart (see uacvalidate/wireformat.py, XSCOPE_WRAP_S, for the silicon
    # evidence). Left in place, each jump splits the fill trace at
    # SEGMENT_GAP_S and caps every fitted window at ~43 s -- the same
    # short-window trap that once produced a confident -3.39 ppm that a
    # 21-minute soak read as +0.24. This signal's own cadence is
    # milliseconds, far inside the repair tolerance, so filtering this
    # signal's stream alone is sound.
    try:
        from uacvalidate.wireformat import PhantomWrapFilter
    except ImportError:                       # run from tools/ directly
        from wireformat import PhantomWrapFilter
    fix = PhantomWrapFilter()
    return [(fix(t * TICK), v) for t, v in sigs.get(sid, [])]


def fill_points(sigs):
    """The OUT-FIFO fill trace as [(t_s, v)], or [] if the capture has none."""
    return probe_points(sigs, FILL_SIGNAL_NAME)


def longest_segment(sigs):
    """The longest correction-free stretch of the fill trace, or None.

    Longest by duration, not by point count: emission is not perfectly
    periodic, and the fit's confidence comes from the time base it spans.
    """
    segs = segments(fill_points(sigs))
    if not segs:
        return None
    return max(segs, key=lambda s: s[-1][0] - s[0][0])


def fit_fill_slope(sigs):
    """Least-squares slope of the OUT-FIFO fill trace, in bytes/second.

    Fitted over the longest segment between block corrections: a correction is
    a discontinuity, and fitting across one measures the correction rather than
    the drift.

    Returns None when there is nothing fittable -- no fill probe in the
    capture, or every segment too short. The caller reports that as SKIP, not
    as zero drift: an absent instrument is not a reading of zero, and the
    difference is the whole point of the validator's SKIP level. A genuine
    0.0 from a real fit is a PASS at the noise floor and must stay
    distinguishable from it.

    Named apart from fit_slope() above, which is the generic OLS primitive over
    a point list and returns (slope, r2, n). Both are public and both are used;
    this one takes parse()'s signal dict and answers the one question the
    validator asks.
    """
    seg = longest_segment(sigs)
    if seg is None:
        return None
    r = fit_slope(seg)
    return None if r is None else r[0]


# Bytes per second of device OUT stream that ONE ppm of rate error represents.
# The fill probe counts BYTES, so converting its slope to a ppm equivalent needs
# the stream's byte rate -- which depends on the negotiated format, not just the
# sample rate:
#
#     1AMi2o2 (UAC1, 2ch x 16-bit)   44100 x  4 B =  176400 B/s -> 0.17640
#     2AMi8o8 (UAC2, 8ch x 24-in-4)  44100 x 32 B = 1411200 B/s -> 1.41120
#
# 0.17640 was hardcoded. This is NOT a newly discovered defect: the UAC2 P1
# gate hit it on 2026-08-02 and transcript_hw_evkb.txt says so in as many
# words -- "vcdfill's printed ppm column assumes 4 B frames; divide its B/s by
# 1.4112 for this stream". It was recorded, worked around by hand, and left.
#
# What forced the parameter was driftrun.sh's FIT rather than a single point.
# A manual division is fine when you read one number off one run; it is not
# fine when a five-point sweep feeds slope/intercept arithmetic, because the
# slope comes out 7.85 against a gate that demands 1.000 and the natural
# reading of that is "the probe or the analysis is broken" -- which is exactly
# what the gate's own comment tells you to conclude.
#
# Note what the wrong constant did NOT break: the zero crossing (-icept/slope)
# is invariant under a uniform scale error on drift, so -85.8 ppm for the
# device's converter was right either way. Only the slope gate could see it.
PPM_BPS_UAC1 = 0.17640


def analyse(path, want_json=False, plot=None, ppm_Bps=PPM_BPS_UAC1):
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
    seg_rows = []
    for s in segments(fill):
        dur = s[-1][0] - s[0][0]
        r = fit_slope(s)
        if not r:
            continue
        b, r2, n = r
        seg_rows.append({
            "t_start": round(s[0][0], 2), "t_end": round(s[-1][0], 2),
            "dur_s": round(dur, 1), "slope_Bps": round(b, 3),
            "ppm_equiv": round(b / ppm_Bps, 1), "r2": round(r2, 4), "n": n,
        })
    out["segments"] = seg_rows
    out["ppm_Bps"] = ppm_Bps
    if seg_rows:
        tot = sum(r["dur_s"] for r in seg_rows)
        wm = sum(r["slope_Bps"] * r["dur_s"] for r in seg_rows) / tot
        out["weighted_slope_Bps"] = round(wm, 3)
        out["weighted_ppm_equiv"] = round(wm / ppm_Bps, 1)

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
    # --frame-bytes N: bytes per audio frame on the wire (channels x subslot).
    # Default 4 = UAC1 stereo 16-bit, which is what every pre-2026-08-06 caller
    # meant. Pass 32 for 2AMi8o8 (8ch x 24-in-4).
    fb = 4
    if "--frame-bytes" in args:
        i = args.index("--frame-bytes")
        fb = int(args[i + 1])
        del args[i:i + 2]
    ppm_Bps = 44100.0 * fb / 1e6
    sys.exit(analyse(args[0], want_json, plot, ppm_Bps))
