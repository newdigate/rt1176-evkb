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
    in_alt0 = 0
    prev = None
    for _, b in blocks:
        if prev is not None and b.alt_out == 0:
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
    _, b = _span(blocks)
    sizes = b.sizes()
    ceiling = max(man.legal_packet_sizes)
    ev = {"sizes": sizes, "ceiling_bytes": ceiling,
          "hist_overflow": b.size_hist_overflow}
    if not sizes:
        return Verdict("R4a", SKIP, "no packets observed",
                       missing_witness="any OUT packet", evidence=ev)
    over = {s: c for s, c in sizes.items() if s > ceiling}
    if over:
        return Verdict(
            "R4a", FAIL,
            f"packet sizes exceed the endpoint's maximum of {ceiling} B: {over}",
            citation=CITE_R4A, evidence=ev)
    return Verdict("R4a", PASS,
                   f"all packets within {ceiling} B", evidence=ev)


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
    a, b = _span(blocks)
    errs = delta(a.pat_err_count, b.pat_err_count)
    resyncs = delta(a.pat_resync_count, b.pat_resync_count)
    ev = {"pattern_errors": errs, "resyncs": resyncs,
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
    if errs == 0 and resyncs == 0 and b.pat_first_err_idx == 0 \
            and delta(a.nonsilent_frames, b.nonsilent_frames) < MIN_NONSILENT_FRAMES:
        return Verdict(
            "R7", SKIP,
            "the pattern never synchronised: the host's playback path is not "
            "bit-exact (volume control, mixing or resampling), so continuity "
            "cannot be judged",
            missing_witness="a bit-exact playback path", evidence=ev)
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
