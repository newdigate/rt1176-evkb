"""Conformance rules.

Each rule takes (blocks, manifest) and returns exactly one Verdict. Rules are
independent: adding one appends to ALL_RULES and edits nothing.

Citation discipline: a FAIL cites a clause. Citations carrying [UNVERIFIED] or
[DOC MISSING] have not been read against the primary document -- usb_20.pdf
and frmts20.pdf are not on this machine. A validator that mis-cites is worse
than one that does not cite, so the marker travels with the text into the
report rather than being quietly dropped.
"""
from verdict import Verdict, PASS, FAIL, WARN, SKIP
# delta32 lives in wireformat because MASK32 does: computing a difference in
# the wire format's own arithmetic belongs with the format. Deciding what a
# wrap MEANS is this module's job; doing the subtraction is not.
from wireformat import delta32 as delta

# Below this many non-silent frames the byte-lane accumulators have not seen
# enough signal to distinguish justification from silence. 10,000 frames is
# 0.23 s at 44.1 kHz -- deliberately conservative, because the cost of a false
# SKIP is one more run and the cost of a false PASS is a wrong verdict.
MIN_NONSILENT_FRAMES = 10000

# R1 has NO citation, deliberately. USB 2.0 5.12.4.2 reads: "An asynchronous
# sink must provide explicit feedback to the host [...] This allows the host to
# continuously adjust the number of samples sent to the sink". Every must/shall
# in 5.12.4 involving the host places the obligation on the DEVICE, and the one
# host-facing "must" concerns an adaptive source -- the IN direction. Nothing
# requires a host to read an asynchronous sink's feedback endpoint. A host that
# ignores it is conformant and ruinous, which is what WARN is for.
CITE_R2 = ("UAC 2.0 sections 3.16.2 and 4.9.1 [partial]; USB 2.0 section "
           "9.4.10 [UNVERIFIED]")
CITE_R3 = "USB Audio Data Formats 2.0 section 2.3.1 [DOC MISSING]"
CITE_R4A = "USB 2.0 section 5.6.3 [UNVERIFIED]"
CITE_R4B = "UAC 2.0 audio frame structure [UNVERIFIED]"
CITE_R7 = ("no clause in hand -- see the design spec; this citation is the "
           "weakest in the set and may demote R7 to WARN [UNVERIFIED]")


def _span(blocks):
    return blocks[0][1], blocks[-1][1]


def _duration(blocks):
    return blocks[-1][0] - blocks[0][0]


def _packets(blocks):
    a, b = _span(blocks)
    return delta(a.pkt_count, b.pkt_count)


def _streamed(blocks):
    """Did any block observe an active output stream?"""
    return any(b.alt_out > 0 for _, b in blocks)


def r1_feedback_polled(blocks, man, correction_quantum_bytes=1024,
                       fill_slope_bytes_per_s=None):
    a, b = _span(blocks)
    polls = delta(a.fb_poll_count, b.fb_poll_count)
    ev = {"feedback_polls": polls, "duration_s": round(_duration(blocks), 2)}
    if not _streamed(blocks):
        return Verdict("R1", SKIP, "no output stream was ever active",
                       missing_witness="an active stream (alt > 0)", evidence=ev)
    if polls == 0:
        if fill_slope_bytes_per_s:
            ppm = slope_to_ppm(fill_slope_bytes_per_s, man)
            seconds = correction_quantum_bytes / abs(fill_slope_bytes_per_s)
            consequence = (
                f"the device is reporting its rate and no one is listening. "
                f"Open loop, the measured drift is {ppm:+.2f} ppm, so this "
                f"host forces a block correction -- silence insertion or "
                f"dropped packets, audible either way -- every "
                f"{seconds:.0f} s. The device is entitled to do this: the "
                f"spec obliges it to publish its rate, not the host to read "
                f"it.")
        else:
            consequence = (
                f"the device published its rate for {_duration(blocks):.0f} s "
                f"and the host never read it. Drift is unbounded and the "
                f"device will block-correct at whatever cadence its own "
                f"crystal offset dictates. No fill slope was available in "
                f"this capture to quantify the cadence.")
        return Verdict(
            "R1", WARN,
            f"host never polled the feedback endpoint in "
            f"{_duration(blocks):.1f} s of streaming",
            consequence=consequence, evidence=ev)
    return Verdict("R1", PASS,
                   f"feedback polled {polls} times "
                   f"({polls / max(_duration(blocks), 1e-9):.1f}/s)", evidence=ev)


def r2_no_streaming_in_alt0(blocks, man):
    """Packets arriving while the OUT interface is in alt 0.

    An interval counts only when BOTH its endpoints are in alt 0.

    The packet delta between two blocks covers the whole interval between
    them, but the alt reading is a point sample at each end. Attributing the
    delta to the alt at the END of the interval books every packet that
    legitimately arrived before an alt 1 -> alt 0 transition as an alt-0
    packet -- so a host that merely stops, pauses, or changes rate, which is
    precisely what alt_transitions exists to record, collects a cited FAIL for
    conformant behaviour.

    Requiring both endpoints discards at most one sample period of real
    evidence at each edge, 10 ms at the observer's 100 Hz. The defect this
    rule is looking for -- a host streaming to an interface that reserves no
    bandwidth and exposes no endpoint -- runs for seconds, so it survives that
    loss intact. A false FAIL would not survive contact with a user.
    """
    in_alt0 = 0
    prev = None
    for _, b in blocks:
        if prev is not None and prev.alt_out == 0 and b.alt_out == 0:
            in_alt0 += delta(prev.pkt_count, b.pkt_count)
        prev = b
    ev = {"packets_while_alt0": in_alt0, "total_packets": _packets(blocks)}
    if _packets(blocks) == 0 and in_alt0 == 0:
        return Verdict("R2", SKIP, "no packets arrived at all",
                       missing_witness="any OUT packet", evidence=ev)
    if in_alt0 > 0:
        return Verdict(
            "R2", FAIL,
            f"{in_alt0} packets arrived while the OUT interface was in alt 0, "
            f"which reserves no bandwidth and exposes no endpoint",
            citation=CITE_R2, evidence=ev)
    return Verdict("R2", PASS, "no packets arrived during alt 0", evidence=ev)


def r3_left_justified(blocks, man):
    a, b = _span(blocks)
    frames = delta(a.nonsilent_frames, b.nonsilent_frames)
    ev = {"or_acc": f"0x{b.or_acc:08X}", "and_acc": f"0x{b.and_acc:08X}",
          "nonsilent_frames": frames}
    if man.subslot_bytes * 8 <= 16:
        return Verdict("R3", SKIP,
                       f"{man.subslot_bytes}-byte subslot has no spare lane",
                       missing_witness="a subslot wider than the sample",
                       evidence=ev)
    if frames < MIN_NONSILENT_FRAMES:
        return Verdict(
            "R3", SKIP,
            f"only {frames} non-silent frames "
            f"(need {MIN_NONSILENT_FRAMES}); a silent stream would pass this "
            f"check for the wrong reason",
            missing_witness="sufficient signal in the stream", evidence=ev)
    if (b.or_acc & 0xFF) == 0:
        return Verdict("R3", PASS,
                       "low byte never non-zero: samples are left-justified",
                       evidence=ev)
    if (b.or_acc >> 24) == (b.and_acc >> 24):
        return Verdict(
            "R3", FAIL,
            "top byte constant while the low byte carries data: samples are "
            "right-justified in the subslot",
            citation=CITE_R3, evidence=ev)
    return Verdict(
        "R3", FAIL,
        f"low byte of the subslot carries data (or_acc=0x{b.or_acc:08X}), so "
        f"samples are not left-justified",
        citation=CITE_R3, evidence=ev)


def r4a_max_packet_size(blocks, man):
    """No packet exceeds the endpoint's declared wMaxPacketSize.

    The bound is the manifest's `max_packet_size_bytes`, read off the
    descriptor -- NOT max(legal_packet_sizes), which is the fractional-sample
    ceiling. Those are different numbers with different meanings and an
    earlier version of this rule used the wrong one. With an asynchronous
    feedback loop a host is entitled to vary packet size, so comparing against
    the fractional ceiling issued a cited FAIL for legal behaviour, and at an
    integer sample rate -- where legal_packet_sizes has exactly one element --
    it fired on any variation whatsoever, against an endpoint whose
    wMaxPacketSize exists to declare precisely that headroom. Lumpy sizing is
    W2's business, and W2 is a WARN.

    A FAIL survives histogram overflow but a PASS does not. An oversized
    packet in a visible slot is positive evidence of a violation; overflow
    does not weaken it. A clean sweep of the visible slots, with packets in
    sizes the histogram could not record, is the absence of evidence, and
    reporting that as PASS would be the vacuity this package exists to refuse
    -- W2 already treats the same field as load-bearing.
    """
    _, b = _span(blocks)
    sizes = b.sizes()
    ceiling = man.max_packet_size_bytes
    ev = {"sizes": sizes, "wMaxPacketSize_bytes": ceiling,
          "hist_overflow": b.size_hist_overflow}
    over = {s: c for s, c in sizes.items() if s > ceiling}
    if over:
        return Verdict(
            "R4a", FAIL,
            f"packet sizes exceed the endpoint's declared wMaxPacketSize of "
            f"{ceiling} B: {over}",
            citation=CITE_R4A, evidence=ev)
    if b.size_hist_overflow:
        return Verdict(
            "R4a", SKIP,
            f"{b.size_hist_overflow} packets arrived in sizes beyond the "
            f"{len(b.size_hist_size)} histogram slots, so their sizes are "
            f"unknown; the {len(sizes)} recorded sizes are all within "
            f"{ceiling} B, which is not evidence that the unrecorded ones were",
            missing_witness=f"the sizes of {b.size_hist_overflow} packets that "
                            f"fell outside the histogram",
            evidence=ev)
    if not sizes:
        return Verdict("R4a", SKIP, "no packets observed",
                       missing_witness="any OUT packet", evidence=ev)
    return Verdict("R4a", PASS,
                   f"all packets within the declared {ceiling} B", evidence=ev)


def r4b_whole_frames(blocks, man):
    a, b = _span(blocks)
    not_mult = delta(a.pkt_not_multiple, b.pkt_not_multiple)
    short = delta(a.pkt_short_discarded, b.pkt_short_discarded)
    ev = {"not_multiple": not_mult, "short_discarded": short,
          "frame_bytes": man.frame_bytes, "total_packets": _packets(blocks)}
    if _packets(blocks) == 0 and not_mult == 0 and short == 0:
        return Verdict("R4b", SKIP, "no packets observed",
                       missing_witness="any OUT packet", evidence=ev)
    if not_mult or short:
        return Verdict(
            "R4b", FAIL,
            f"{not_mult} packets were not a whole number of "
            f"{man.frame_bytes} B audio frames, and {short} were shorter than "
            f"one frame and silently discarded by the device",
            citation=CITE_R4B, evidence=ev)
    return Verdict("R4b", PASS,
                   f"all packets are whole multiples of {man.frame_bytes} B",
                   evidence=ev)


def r7_sample_continuity(blocks, man):
    """Sample continuity, gated on the pattern actually having held lock.

    The lock count is the witness that stops this rule making a false
    accusation. A host whose playback path is not bit-exact -- OS volume,
    mixing, resampling -- plays full-scale audio and never holds lock: it
    matches by chance, mismatches, relocks, forever. Its pattern-error count
    is enormous and looks exactly like a host dropping packets. Reporting that
    as "N sample discontinuities" would blame a conformant host for something
    it did not do, which is the worst output this tool can produce; the design
    spec keeps an unusable capture at exit 2 rather than 1 for the same reason.

    So: no lock, or lock lost and regained, means continuity cannot be judged,
    and the rule says so instead of guessing. Both stay SKIP rather than WARN
    -- repeated relock does not tell us whether the host is ALSO dropping
    packets, and a SKIP that names its missing witness is honest where a WARN
    would be arithmetic over an unknown.

    An earlier draft keyed the never-locked case on a low non-silent frame
    count. That detected silence, not failure to lock -- the non-bit-exact
    host it was meant to catch is loud -- so it left the false accusation in
    place. Word 36 exists because a proxy could not do this job.
    """
    a, b = _span(blocks)
    errs = delta(a.pat_err_count, b.pat_err_count)
    resyncs = delta(a.pat_resync_count, b.pat_resync_count)
    # Absolute, not a delta, and deliberately unlike every other counter here:
    # "how many times has the pattern held lock in this run" is a property of
    # the run, not of the window between two blocks. A capture that opens
    # mid-stream, after lock was achieved, has a lock delta of zero across
    # every block in it -- taking the difference would report a healthy soak as
    # never having locked.
    syncs = b.pat_sync_count
    ev = {"pattern_errors": errs, "resyncs": resyncs, "pattern_syncs": syncs,
          "first_error_index": b.pat_first_err_idx,
          "first_expected": f"0x{b.pat_first_expected:08X}",
          "first_actual": f"0x{b.pat_first_actual:08X}"}
    if man.mode != "cooperative":
        return Verdict("R7", SKIP, "continuity needs a known test pattern",
                       missing_witness="passive mode: no test pattern sent",
                       evidence=ev)
    if _packets(blocks) == 0:
        return Verdict("R7", SKIP, "no packets observed",
                       missing_witness="any OUT packet", evidence=ev)
    if syncs == 0:
        return Verdict(
            "R7", SKIP,
            "the pattern never locked: the host's playback path is not "
            "bit-exact (volume control, mixing or resampling), so continuity "
            "cannot be judged",
            missing_witness="a bit-exact playback path: the pattern never "
                            "locked",
            evidence=ev)
    if syncs > 1:
        return Verdict(
            "R7", SKIP,
            f"the pattern lost and regained lock {syncs} times, which is "
            f"consistent with a playback path that is not bit-exact rather "
            f"than with discrete packet loss; the {errs} pattern errors "
            f"cannot be attributed to the host",
            missing_witness=f"a playback path that holds lock: the pattern "
                            f"relocked {syncs} times",
            evidence=ev)
    if errs:
        return Verdict(
            "R7", FAIL,
            f"{errs} sample discontinuities, first at sample index "
            f"{b.pat_first_err_idx} (expected 0x{b.pat_first_expected:08X}, "
            f"got 0x{b.pat_first_actual:08X}); {resyncs} resyncs",
            citation=CITE_R7, evidence=ev)
    return Verdict("R7", PASS, "sample stream continuous", evidence=ev)


def w2_packet_size_shape(blocks, man):
    _, b = _span(blocks)
    sizes = b.sizes()
    legal = man.legal_packet_sizes
    ev = {"sizes": sizes, "expected": sorted(legal),
          "hist_overflow": b.size_hist_overflow}
    if not sizes:
        return Verdict("W2", SKIP, "no packets observed",
                       missing_witness="any OUT packet", evidence=ev)
    unexpected = {s: c for s, c in sizes.items() if s not in legal}
    if unexpected or b.size_hist_overflow:
        worst = max(abs(s - man.nominal_frames_per_packet * man.frame_bytes)
                    for s in sizes)
        excursion_frames = worst / man.frame_bytes
        return Verdict(
            "W2", WARN,
            f"packet sizes outside the fractional-sample floor/ceiling "
            f"{sorted(legal)}: {unexpected}"
            + (f" plus {b.size_hist_overflow} packets in sizes beyond the "
               f"8 histogram slots" if b.size_hist_overflow else ""),
            consequence=(
                f"worst deviation is {excursion_frames:.1f} audio frames from "
                f"nominal, so the device's OUT FIFO must carry that much extra "
                f"headroom on top of drift wander. Permitted by the spec with "
                f"an asynchronous feedback loop, but it consumes margin that "
                f"exists to absorb rate error."),
            evidence=ev)
    return Verdict("W2", PASS,
                   f"packet sizes are the expected floor/ceiling pair "
                   f"{sorted(legal)}", evidence=ev)


# A residual this small is indistinguishable from measurement noise over any
# practical soak: at 1.4 MB/s it is under 2 B/s, inside one packet of jitter.
# A capture's timestamps and its counters are two independent clocks. If they
# disagree, any rule that divides by elapsed time is reporting a number it
# cannot support -- and a wrong ppm figure is worse than none, because it looks
# like a measurement.
#
# The cross-check uses packets rather than frames: pkt_count / packets_per_sec
# is the streaming duration regardless of how much of the audio was silent,
# whereas nonsilent_frames is only a lower bound. It doubles as a second net
# under a mis-declared packet rate, which silicon has already caught once.
TIME_BASE_TOLERANCE = 0.25


def time_base_consistent(blocks, man):
    """(ok, vcd_seconds, implied_seconds).

    ok is False when the capture's own timestamps disagree with what its
    packet count implies, or when there are too few packets to judge.
    """
    vcd_s = _duration(blocks)
    packets = _packets(blocks)
    implied_s = packets / float(man.packets_per_second)
    if packets < man.packets_per_second:      # under a second of stream
        return False, vcd_s, implied_s
    if implied_s <= 0 or vcd_s <= 0:
        return False, vcd_s, implied_s
    ratio = vcd_s / implied_s
    return (abs(ratio - 1.0) <= TIME_BASE_TOLERANCE), vcd_s, implied_s


def _time_base_skip(rule_id, blocks, man, ev):
    """Shared SKIP for the two rate-derived rules. None when the base is sound."""
    ok, vcd_s, implied_s = time_base_consistent(blocks, man)
    if ok:
        return None
    ev = dict(ev, vcd_seconds=round(vcd_s, 3), implied_seconds=round(implied_s, 3))
    return Verdict(
        rule_id, SKIP,
        f"capture timestamps span {vcd_s:.1f} s but its {_packets(blocks)} packets "
        f"at {man.packets_per_second}/s imply {implied_s:.1f} s",
        missing_witness=("a trustworthy time base: this rule divides by elapsed "
                         "time, and the capture's two clocks disagree"),
        evidence=ev)


DRIFT_NOISE_FLOOR_PPM = 1.0


def slope_to_ppm(slope_bytes_per_s, man):
    """Convert an OUT-FIFO fill slope to a rate error in ppm.

    vcdfill.py's printed ppm column assumes 4 bytes per frame. This does not:
    it uses the manifest's actual byte rate, which is why an 8ch 24-in-4
    stream does not need the 1.4112 correction factor applied by hand.
    """
    return slope_bytes_per_s / man.nominal_bytes_per_second * 1e6


def w1_residual_drift(blocks, man, fill_slope_bytes_per_s,
                      correction_quantum_bytes):
    ev = {"fill_slope_bytes_per_s": fill_slope_bytes_per_s,
          "correction_quantum_bytes": correction_quantum_bytes}
    if fill_slope_bytes_per_s is None:
        return Verdict("W1", SKIP, "no fill-probe slope available",
                       missing_witness="the out_fifo_fill trace in the capture",
                       evidence=ev)
    stale = _time_base_skip("W1", blocks, man, ev)
    if stale is not None:
        return stale
    ppm = slope_to_ppm(fill_slope_bytes_per_s, man)
    ev["residual_ppm"] = round(ppm, 3)
    if abs(ppm) <= DRIFT_NOISE_FLOOR_PPM:
        return Verdict("W1", PASS,
                       f"residual rate error {ppm:+.3f} ppm, at the noise floor",
                       evidence=ev)
    seconds = correction_quantum_bytes / abs(fill_slope_bytes_per_s)
    ev["seconds_between_corrections"] = round(seconds, 1)
    return Verdict(
        "W1", WARN,
        f"residual rate error {ppm:+.2f} ppm after the feedback loop",
        consequence=(
            # The ppm is restated here rather than left to the summary: the
            # consequence has to stand on its own. report.render prints it on
            # its own line, --json exposes the field on its own, and a WARN
            # whose arithmetic does not name the quantity it was derived from
            # cannot be checked by the reader. The plan's draft omitted it and
            # its own test caught that.
            f"a {ppm:+.2f} ppm residual against the device's "
            f"{correction_quantum_bytes} B correction quantum: at "
            f"{fill_slope_bytes_per_s:+.3f} B/s this host will force a "
            f"block correction -- silence insertion or dropped packets, "
            f"audible either way -- every {seconds:.0f} s "
            f"({seconds / 60:.1f} min)."),
        evidence=ev)


def w3_feedback_tracked(blocks, man, fill_slope_bytes_per_s):
    a, b = _span(blocks)
    polls = delta(a.fb_poll_count, b.fb_poll_count)
    ev = {"feedback_polls": polls, "device_fb_value": f"0x{b.fb_value:08X}",
          "fill_slope_bytes_per_s": fill_slope_bytes_per_s}
    if polls == 0:
        return Verdict(
            "W3", SKIP,
            "host never polled feedback, so there is no tracking to assess "
            "(R1 covers this)",
            missing_witness="any feedback poll", evidence=ev)
    if fill_slope_bytes_per_s is None:
        return Verdict("W3", SKIP, "no fill-probe slope available",
                       missing_witness="the out_fifo_fill trace in the capture",
                       evidence=ev)
    stale = _time_base_skip("W3", blocks, man, ev)
    if stale is not None:
        return stale
    ppm = slope_to_ppm(fill_slope_bytes_per_s, man)
    ev["residual_ppm"] = round(ppm, 3)
    if abs(ppm) <= DRIFT_NOISE_FLOOR_PPM:
        return Verdict("W3", PASS,
                       f"host tracks the device's feedback: residual "
                       f"{ppm:+.3f} ppm over {polls} polls", evidence=ev)
    return Verdict(
        "W3", WARN,
        f"host polled feedback {polls} times but still runs {ppm:+.2f} ppm off",
        consequence=(
            f"the device is reporting its rate and the host is reading it, yet "
            f"a {ppm:+.2f} ppm residual remains -- the servo is receiving the "
            f"information and not acting on it correctly. A servo that chases "
            f"the raw dithered report rather than its mean produces exactly "
            f"this signature."),
        evidence=ev)


# W1 and W3 are NOT in ALL_RULES: like r1_feedback_polled they take extra
# arguments, and the CLI invokes them directly.
# r1_feedback_polled is NOT here: it takes the fill slope so its consequence
# can quantify the glitch cadence, so the CLI calls it directly alongside W1
# and W3. Everything in ALL_RULES has the uniform (blocks, manifest) signature.
ALL_RULES = [
    r2_no_streaming_in_alt0,
    r3_left_justified,
    r4a_max_packet_size,
    r4b_whole_frames,
    r7_sample_continuity,
    w2_packet_size_shape,
]
