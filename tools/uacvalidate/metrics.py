"""Unjudged observations, reported as INFO.

Everything here is a measurement the report prints and never rules on. It has
no level, no citation and no effect on the exit code, which is the point: the
spec asks for numbers a reader can interpret alongside the verdicts without
the tool pretending to a clause it does not have.

It also closes a real gap. `class_req_bitmap`, `host_active` and
`alt_transitions` are decoded by wireformat and, before this module, read by
nothing -- which left founding defect #4, "the host claimed the device and
then never completed the class configuration sequence", entirely
unreportable. There is no clause in hand obliging a host to issue any
particular class request, so it cannot be a FAIL and inventing a threshold for
it would be a WARN with no arithmetic behind it. Stating the observation and
letting the reader draw the conclusion is the honest third option.

Output is an ordered list of (label, value) pairs of plain strings. Ordered
because the reading order is deliberate -- the stream first, then the
feedback loop, then the drift, then what it cost -- and strings because a
metric that needed interpreting to render would be a verdict wearing a
disguise.
"""
from wireformat import delta32 as delta

# Beyond this many alt or host-active changes the list stops being a fact and
# starts being a log. A host flapping this hard has already told its story.
MAX_EVENTS_LISTED = 8


# The fill trace opens with a stream-start transient: the device prefills and
# the host's servo has not converged, so the first seconds swing far wider than
# steady state ever does. Reporting min/max over the whole capture makes two
# soaks of different lengths incomparable and makes a startup dip look like a
# control-loop problem -- exactly the mistake a baseline meant for A/B tuning
# must not bake in.
#
# Skipped by TIME, not by sample count, so captures at different block cadences
# (about 80 Hz at high speed, 10 Hz at full speed) drop the same physical
# settling window.
STEADY_SKIP_S = 30.0
STEADY_MIN_SAMPLES = 100


def _steady_state(points):
    """Fill points after the settling window, or None if too few remain."""
    if not points:
        return None
    t0 = points[0][0]
    kept = [(t, v) for t, v in points if t - t0 >= STEADY_SKIP_S]
    return kept if len(kept) >= STEADY_MIN_SAMPLES else None


def _fmt_events(events):
    if not events:
        return "none"
    shown = events[:MAX_EVENTS_LISTED]
    text = ", ".join(f"{t:.2f}s->{v}" for t, v in shown)
    if len(events) > len(shown):
        text += f", +{len(events) - len(shown)} more"
    return text


def _changes(blocks, attr):
    """[(time, new value)] for each change of `attr`, first sample excluded."""
    out = []
    prev = None
    for t, b in blocks:
        v = getattr(b, attr)
        if prev is not None and v != prev:
            out.append((t, v))
        prev = v
    return out


def _set_bits(mask):
    return [i for i in range(32) if mask & (1 << i)]


def collect(blocks, man, fill_sigs=None, slope_bytes_per_s=None,
            probe_reader=None):
    """Metrics for a capture, as [(label, value)].

    `probe_reader(sigs, name) -> [(t_s, v)]` is injected rather than imported
    so this module stays free of vcdfill: the metrics are a property of the
    capture, not of whichever tool happens to parse the probe section. When it
    or `fill_sigs` is absent, the probe-derived rows say so instead of
    reporting zeros -- an absent instrument is not a reading of zero, the same
    distinction W1 turns into SKIP.
    """
    out = []
    if not blocks:
        return out
    (t0, a), (t1, b) = blocks[0], blocks[-1]
    dur = t1 - t0
    per_s = dur if dur > 0 else None

    pkts = delta(a.pkt_count, b.pkt_count)
    out.append(("duration", f"{dur:.2f} s over {len(blocks)} state blocks"))
    out.append(("packets", f"{pkts}" + (f" ({pkts / per_s:.1f}/s, nominal "
                                        f"{man.packets_per_second}/s)"
                                        if per_s else "")))
    sizes = b.sizes()
    # Parenthesised deliberately: `or` binds looser than `+`, so writing this
    # as `join(...) or "none recorded" + suffix` attaches the overflow note to
    # the empty case only -- losing it in exactly the runs that have one.
    size_text = (", ".join(f"{s} B x{c}" for s, c in sorted(sizes.items()))
                 or "none recorded")
    if b.size_hist_overflow:
        size_text += f"; {b.size_hist_overflow} beyond the histogram"
    out.append(("packet sizes", size_text))

    polls = delta(a.fb_poll_count, b.fb_poll_count)
    out.append(("feedback polls",
                f"{polls}" + (f" ({polls / per_s:.1f}/s)" if per_s else "")))
    # DELIBERATELY UNDECODED, and this is a recorded decision rather than an
    # oversight. The explicit-feedback value is a fixed-point frames-per-frame
    # figure in one of two encodings:
    #
    #   FS (UAC1): 10.14, right-justified in three bytes.
    #   HS (UAC2): 16.16, in four bytes.
    #
    # `man.speed` is the field that selects between them, so this IS decidable
    # from the manifest in principle. What is not yet known is where the
    # device puts the value inside the word it publishes: the relevant path in
    # lib_xua's ep_buffer.xc shifts it differently at FS and at HS, and no
    # capture exists to check a decode against. Decoding on that basis would
    # print a confident sample rate in Hz that could be wrong by a power of
    # two, sitting beside metrics that are right.
    #
    # Metrics carry no verdict, so raw hex costs nothing and a wrong Hz figure
    # costs trust. Decode this once the first real capture confirms the
    # device's convention -- and add a test pinning both encodings when you do.
    out.append(("device feedback value", f"0x{b.fb_value:08X} (raw; encoding "
                                         f"undecided, see metrics.py)"))

    out.append(("alt setting",
                f"{b.alt_out} now, {delta(a.alt_transitions, b.alt_transitions)} "
                f"transitions; changes: {_fmt_events(_changes(blocks, 'alt_out'))}"))

    bits = _set_bits(b.class_req_bitmap)
    active = [(t, v) for t, v in _changes(blocks, "host_active")]
    ever_active = b.host_active or any(v for _, v in active) or a.host_active
    req_text = (f"0x{b.class_req_bitmap:X} (bits {bits})" if bits
                else "0x0 (none arrived)")
    if ever_active and not bits:
        # Defect #4, stated as an observation. No clause in hand obliges a
        # host to issue any class request, so this is not a FAIL, and there is
        # no arithmetic that would make it a WARN.
        req_text += " -- host was active but completed no class request"
    out.append(("class requests", req_text))
    out.append(("host active",
                f"{b.host_active} now; changes: {_fmt_events(active)}"))

    fill = probe_reader(fill_sigs, "out_fifo_fill") if (
        probe_reader and fill_sigs) else []
    if fill:
        vals = [v for _, v in fill]
        out.append(("OUT FIFO fill",
                    f"min {min(vals)} B, max {max(vals)} B, mean "
                    f"{sum(vals) / len(vals):.0f} B over {len(vals)} samples"))
        steady = _steady_state(fill)
        if steady is not None:
            sv = [v for _, v in steady]
            out.append(("OUT FIFO fill (steady)",
                        f"min {min(sv)} B, max {max(sv)} B, envelope "
                        f"{max(sv) - min(sv)} B, mean {sum(sv) / len(sv):.0f} B "
                        f"over {len(sv)} samples from "
                        f"{steady[0][0] - fill[0][0]:.0f} s in"))
        else:
            out.append(("OUT FIFO fill (steady)",
                        f"not computed: fewer than {STEADY_MIN_SAMPLES} samples "
                        f"after the {STEADY_SKIP_S:.0f} s settling window"))
    else:
        out.append(("OUT FIFO fill", "no fill probe in this capture"))

    if slope_bytes_per_s is None:
        out.append(("fill slope", "not fitted (no fill probe, or no segment "
                                  "long enough between corrections)"))
    else:
        ppm = slope_bytes_per_s / man.nominal_bytes_per_second * 1e6
        out.append(("fill slope",
                    f"{slope_bytes_per_s:+.3f} B/s ({ppm:+.2f} ppm)"))

    corrections = []
    for name, label in (("out_dryout", "dry-outs"),
                        ("out_overflow", "overflows"),
                        ("out_underflow", "underflows")):
        pts = probe_reader(fill_sigs, name) if (probe_reader and fill_sigs) else []
        corrections.append(f"{label} {pts[-1][1] if pts else '?'}")
    out.append(("block corrections", ", ".join(corrections)
                + ("" if fill else " (no probes in this capture)")))
    return out
