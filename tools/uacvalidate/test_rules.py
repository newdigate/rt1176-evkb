import unittest

from wireformat import Block
from manifest import Manifest
from verdict import PASS, FAIL, WARN, SKIP
import rules


# max_packet_size_bytes deliberately exceeds max(legal_packet_sizes) = 1440.
# That headroom is what separates R4a from W2: a host sizing at 48 frames
# (1536 B) is inside the endpoint's declared limit -- no FAIL -- while still
# being lumpier than the 44/45 nominal, which is a W2 WARN.
MAN = Manifest.from_dict({
    "audio_class": 2, "speed": "HS", "channels": 8, "subslot_bytes": 4,
    "sample_rate_hz": 44100, "mode": "passive", "host_note": "test",
    "max_packet_size_bytes": 1536,
})
MAN_COOP = Manifest.from_dict({
    "audio_class": 2, "speed": "HS", "channels": 8, "subslot_bytes": 4,
    "sample_rate_hz": 44100, "mode": "cooperative", "host_note": "test",
    "max_packet_size_bytes": 1536,
})


def series(first, last, duration=120.0):
    """Two blocks, start and end, which is all any counter rule needs."""
    return [(0.0, first), (duration, last)]


def healthy(**over):
    """A block from a conformant host after 120 s of 8ch 24-in-4 at 44.1 kHz."""
    base = dict(
        pkt_count=120000, pkt_short_discarded=0, pkt_not_multiple=0,
        or_acc=0xFFFFFF00, and_acc=0x00000000, nonsilent_frames=5292000,
        fb_poll_count=7500, fb_value=0x000B0000, alt_out=1, alt_transitions=1,
        class_req_bitmap=0x5, host_active=1,
        # Locked once and held: what a bit-exact playback path looks like.
        # Ignored outside cooperative mode, but a conformant host in
        # cooperative mode has exactly this, so it belongs in the baseline.
        pat_sync_count=1,
        size_hist_size=[44 * 32, 45 * 32, 0, 0, 0, 0, 0, 0],
        size_hist_count=[108000, 12000, 0, 0, 0, 0, 0, 0],
    )
    base.update(over)
    return Block(**base)


class TestR1FeedbackPolled(unittest.TestCase):
    def test_pass_when_polled(self):
        v = rules.r1_feedback_polled(series(Block(alt_out=1), healthy()), MAN)
        self.assertEqual(v.level, PASS)

    def test_warn_not_fail_when_never_polled(self):
        """USB 2.0 5.12.4.2 obliges the SINK to provide feedback; it only says
        this 'allows' the host to adjust. No clause requires a host to read an
        asynchronous sink's feedback endpoint, so ignoring it is conformant
        and ruinous -- a WARN carrying consequence arithmetic, not a FAIL."""
        v = rules.r1_feedback_polled(
            series(Block(alt_out=1), healthy(fb_poll_count=0)), MAN)
        self.assertEqual(v.level, WARN)
        self.assertFalse(v.citation)
        self.assertTrue(v.consequence)

    def test_skip_when_never_streamed(self):
        v = rules.r1_feedback_polled(
            series(Block(), Block(alt_out=0, fb_poll_count=0)), MAN)
        self.assertEqual(v.level, SKIP)
        self.assertIn("stream", v.missing_witness.lower())

    def test_unpolled_warning_quantifies_the_glitch_cadence_when_a_slope_exists(self):
        """The whole point of handing R1 the fill slope: with one, the WARN
        names a glitch cadence instead of saying 'unbounded'. Without this the
        quantified branch is never entered by any test and could be deleted
        without a red line -- 1024 B / 6.77 B/s is a correction every 151 s."""
        v = rules.r1_feedback_polled(
            series(Block(alt_out=1), healthy(fb_poll_count=0)), MAN,
            correction_quantum_bytes=1024, fill_slope_bytes_per_s=6.77)
        self.assertEqual(v.level, WARN)
        self.assertFalse(v.citation)
        self.assertIn("+4.80 ppm", v.consequence)
        self.assertIn("every 151 s", v.consequence)


class TestR2NoStreamingInAlt0(unittest.TestCase):
    def test_pass_when_packets_only_in_alt_1(self):
        v = rules.r2_no_streaming_in_alt0(series(Block(alt_out=1), healthy()), MAN)
        self.assertEqual(v.level, PASS)

    def test_fail_when_packets_arrive_in_alt_0(self):
        blocks = [(0.0, Block(alt_out=0, pkt_count=0)),
                  (10.0, Block(alt_out=0, pkt_count=10000))]
        v = rules.r2_no_streaming_in_alt0(blocks, MAN)
        self.assertEqual(v.level, FAIL)
        self.assertTrue(v.citation)

    def test_skip_when_no_packets_at_all(self):
        v = rules.r2_no_streaming_in_alt0(
            [(0.0, Block(alt_out=0)), (10.0, Block(alt_out=0))], MAN)
        self.assertEqual(v.level, SKIP)

    # The transition cases. No fixture above changes alt_out at all, so
    # attributing an interval's packets to the alt at either end gives the
    # same answer for every one of them -- which is why the end-attributing
    # version survived the suite while issuing cited FAILs against hosts that
    # merely stopped streaming.

    def test_pass_when_a_stream_stops_cleanly(self):
        """alt 1 -> alt 0 with packets in the final interval, which is what a
        host stopping, pausing or changing rate looks like: the packets
        arrived while the interface was still in alt 1 and the transition
        happened afterwards. Attributing them to the interval's END alt books
        conformant traffic as alt-0 traffic and FAILs a host for stopping."""
        blocks = [(0.0, Block(alt_out=1, pkt_count=0)),
                  (10.0, Block(alt_out=1, pkt_count=10000)),
                  (10.01, Block(alt_out=0, pkt_count=10010))]
        v = rules.r2_no_streaming_in_alt0(blocks, MAN)
        self.assertEqual(v.level, PASS)
        self.assertEqual(v.evidence["packets_while_alt0"], 0)

    def test_pass_when_a_stream_starts(self):
        """The mirror image: alt 0 -> alt 1. Nothing arrived before the
        interface came up, and the first interval must not be charged for the
        packets that arrived after it did."""
        blocks = [(0.0, Block(alt_out=0, pkt_count=0)),
                  (0.01, Block(alt_out=1, pkt_count=5)),
                  (10.0, Block(alt_out=1, pkt_count=10000))]
        v = rules.r2_no_streaming_in_alt0(blocks, MAN)
        self.assertEqual(v.level, PASS)

    def test_fail_survives_the_transition_windows(self):
        """The real defect runs for seconds, so discarding one 10 ms sample
        period at each edge cannot hide it. If tightening the attribution had
        cost the rule its teeth, this is where it would show."""
        blocks = ([(0.0, Block(alt_out=1, pkt_count=0)),
                   (10.0, Block(alt_out=1, pkt_count=10000))]
                  + [(10.0 + i, Block(alt_out=0, pkt_count=10000 + i * 1000))
                     for i in range(1, 6)]
                  + [(16.0, Block(alt_out=1, pkt_count=15500))])
        v = rules.r2_no_streaming_in_alt0(blocks, MAN)
        self.assertEqual(v.level, FAIL)
        self.assertTrue(v.citation)
        # Four fully-enclosed alt-0 intervals of 1000 packets each; the two
        # transition intervals are excluded by design.
        self.assertEqual(v.evidence["packets_while_alt0"], 4000)


class TestR3Justification(unittest.TestCase):
    def test_pass_when_left_justified(self):
        v = rules.r3_left_justified(series(Block(), healthy()), MAN)
        self.assertEqual(v.level, PASS)

    def test_fail_when_right_justified(self):
        v = rules.r3_left_justified(
            series(Block(), healthy(or_acc=0x00FFFFFF, and_acc=0x00000000)), MAN)
        self.assertEqual(v.level, FAIL)
        self.assertTrue(v.citation)

    def test_skip_on_silence_rather_than_pass(self):
        """The lie this tool is most likely to tell: a silent stream makes the
        justification test pass for the wrong reason."""
        v = rules.r3_left_justified(
            series(Block(), healthy(or_acc=0, and_acc=0, nonsilent_frames=0)), MAN)
        self.assertEqual(v.level, SKIP)
        self.assertIn("signal", v.missing_witness.lower())

    def test_skip_just_below_threshold(self):
        v = rules.r3_left_justified(
            series(Block(), healthy(nonsilent_frames=9999)), MAN)
        self.assertEqual(v.level, SKIP)

    def test_pass_at_threshold(self):
        v = rules.r3_left_justified(
            series(Block(), healthy(nonsilent_frames=10000)), MAN)
        self.assertEqual(v.level, PASS)

    def test_skip_when_subslot_is_two_bytes(self):
        """16-bit in a 2-byte subslot has no spare lane, so there is nothing
        to justify."""
        m = Manifest.from_dict({
            "audio_class": 1, "speed": "FS", "channels": 2, "subslot_bytes": 2,
            "sample_rate_hz": 44100, "mode": "passive", "host_note": "t",
            "max_packet_size_bytes": 192})
        v = rules.r3_left_justified(series(Block(), healthy()), m)
        self.assertEqual(v.level, SKIP)


class TestR4aMaxPacketSize(unittest.TestCase):
    def test_pass_within_ceiling(self):
        v = rules.r4a_max_packet_size(series(Block(), healthy()), MAN)
        self.assertEqual(v.level, PASS)

    def test_fail_when_a_packet_is_oversized(self):
        big = healthy(size_hist_size=[44 * 32, 45 * 32, 9999, 0, 0, 0, 0, 0],
                      size_hist_count=[100000, 12000, 3, 0, 0, 0, 0, 0])
        v = rules.r4a_max_packet_size(series(Block(), big), MAN)
        self.assertEqual(v.level, FAIL)
        self.assertTrue(v.citation)

    def test_skip_when_no_packets(self):
        v = rules.r4a_max_packet_size(series(Block(), Block()), MAN)
        self.assertEqual(v.level, SKIP)

    def test_bound_is_the_descriptor_not_the_fractional_ceiling(self):
        """A host sizing at 40/48 frames is lumpy, and lumpiness is not
        forbidden by anything citable -- with an async feedback loop the host
        is entitled to vary packet size. 1536 B is inside the endpoint's
        declared wMaxPacketSize, so R4a must PASS and leave the complaint to
        W2. Against max(legal_packet_sizes) = 1440 this same fixture produced
        a cited FAIL against a conformant host."""
        lumpy = healthy(size_hist_size=[40 * 32, 48 * 32, 0, 0, 0, 0, 0, 0],
                        size_hist_count=[60000, 60000, 0, 0, 0, 0, 0, 0])
        self.assertGreater(48 * 32, max(MAN.legal_packet_sizes))
        self.assertEqual(rules.r4a_max_packet_size(series(Block(), lumpy),
                                                   MAN).level, PASS)
        self.assertEqual(rules.w2_packet_size_shape(series(Block(), lumpy),
                                                    MAN).level, WARN)

    def test_no_fail_on_variation_at_an_integer_sample_rate(self):
        """48 kHz makes legal_packet_sizes a one-element set, so the old bound
        fired on ANY second size -- against an endpoint whose wMaxPacketSize
        exists to declare exactly that headroom. The sizes here straddle the
        single nominal size and stay inside the descriptor's limit."""
        m = Manifest.from_dict({
            "audio_class": 2, "speed": "HS", "channels": 8, "subslot_bytes": 4,
            "sample_rate_hz": 48000, "mode": "passive", "host_note": "t",
            "max_packet_size_bytes": 1600})
        self.assertEqual(len(m.legal_packet_sizes), 1)
        b = healthy(size_hist_size=[47 * 32, 48 * 32, 49 * 32, 0, 0, 0, 0, 0],
                    size_hist_count=[40000, 40000, 40000, 0, 0, 0, 0, 0])
        self.assertEqual(rules.r4a_max_packet_size(series(Block(), b), m).level,
                         PASS)

    def test_fail_at_one_byte_over_the_declared_maximum(self):
        b = healthy(size_hist_size=[MAN.max_packet_size_bytes + 1, 0, 0, 0,
                                    0, 0, 0, 0],
                    size_hist_count=[1, 0, 0, 0, 0, 0, 0, 0])
        self.assertEqual(rules.r4a_max_packet_size(series(Block(), b), MAN).level,
                         FAIL)

    def test_pass_at_exactly_the_declared_maximum(self):
        b = healthy(size_hist_size=[MAN.max_packet_size_bytes, 0, 0, 0,
                                    0, 0, 0, 0],
                    size_hist_count=[1, 0, 0, 0, 0, 0, 0, 0])
        self.assertEqual(rules.r4a_max_packet_size(series(Block(), b), MAN).level,
                         PASS)

    def test_skip_rather_than_pass_when_the_histogram_overflowed(self):
        """9,000 packets of unknown size and a clean sweep of the eight
        recorded ones. Reading that as PASS is the absence of evidence
        rendered as evidence of absence -- the exact lie the SKIP level
        exists for, and W2 already treats this same field as load-bearing."""
        v = rules.r4a_max_packet_size(
            series(Block(), healthy(size_hist_overflow=9000)), MAN)
        self.assertEqual(v.level, SKIP)
        self.assertIn("9000", v.missing_witness)

    def test_fail_outranks_histogram_overflow(self):
        """Overflow undermines a PASS, not a FAIL. An oversized packet in a
        visible slot is positive evidence of a violation, and the packets the
        histogram could not record cannot un-observe it. Degrading this to
        SKIP would let a host hide a real violation by producing enough
        distinct sizes to overflow the table."""
        big = healthy(size_hist_size=[44 * 32, 9999, 0, 0, 0, 0, 0, 0],
                      size_hist_count=[100000, 3, 0, 0, 0, 0, 0, 0],
                      size_hist_overflow=9000)
        v = rules.r4a_max_packet_size(series(Block(), big), MAN)
        self.assertEqual(v.level, FAIL)
        self.assertTrue(v.citation)


class TestR4bWholeFrames(unittest.TestCase):
    def test_pass_when_all_whole(self):
        v = rules.r4b_whole_frames(series(Block(), healthy()), MAN)
        self.assertEqual(v.level, PASS)

    def test_fail_on_non_multiple(self):
        v = rules.r4b_whole_frames(
            series(Block(), healthy(pkt_not_multiple=17)), MAN)
        self.assertEqual(v.level, FAIL)

    def test_fail_on_short_discarded(self):
        v = rules.r4b_whole_frames(
            series(Block(), healthy(pkt_short_discarded=4)), MAN)
        self.assertEqual(v.level, FAIL)

    def test_skip_when_no_packets(self):
        v = rules.r4b_whole_frames(series(Block(), Block()), MAN)
        self.assertEqual(v.level, SKIP)


class TestR7Continuity(unittest.TestCase):
    def test_skip_in_passive_mode(self):
        v = rules.r7_sample_continuity(series(Block(), healthy()), MAN)
        self.assertEqual(v.level, SKIP)
        self.assertIn("passive", v.missing_witness.lower())

    def test_pass_in_cooperative_with_no_errors(self):
        v = rules.r7_sample_continuity(series(Block(), healthy()), MAN_COOP)
        self.assertEqual(v.level, PASS)

    def test_fail_on_pattern_errors(self):
        v = rules.r7_sample_continuity(
            series(Block(), healthy(pat_err_count=9, pat_first_err_idx=41207)), MAN_COOP)
        self.assertEqual(v.level, FAIL)
        self.assertIn("41207", v.summary)

    def test_skip_when_cooperative_but_no_packets(self):
        """Distinct from the never-synchronised SKIP below, and the assertion
        is on the witness rather than the level: both branches report SKIP, so
        a test checking only the level would pass with either one deleted."""
        v = rules.r7_sample_continuity(series(Block(), Block()), MAN_COOP)
        self.assertEqual(v.level, SKIP)
        self.assertIn("OUT packet", v.missing_witness)

    def test_skip_not_fail_when_the_pattern_never_locked(self):
        """The false-accusation case. A host with OS volume or resampling in
        the path plays loud audio and never holds lock, so it piles up pattern
        errors that look exactly like dropped packets. Reporting those as
        discontinuities would blame a conformant host. The error count here is
        large on purpose: nothing but the lock count distinguishes this from a
        genuinely broken host."""
        v = rules.r7_sample_continuity(
            series(Block(), healthy(pat_sync_count=0, pat_err_count=8000,
                                    pat_resync_count=1000,
                                    pat_first_err_idx=32)), MAN_COOP)
        self.assertEqual(v.level, SKIP)
        self.assertIn("never locked", v.missing_witness)

    def test_skip_not_fail_when_lock_was_lost_and_regained(self):
        """Same accusation, softer symptom: lock achieved, then lost and
        retaken repeatedly. Consistent with a playback path that is not
        bit-exact rather than with discrete packet loss -- and we cannot tell
        from relock alone whether the host is ALSO dropping packets, which is
        why this is a SKIP naming its witness and not a WARN doing arithmetic
        over an unknown."""
        v = rules.r7_sample_continuity(
            series(Block(), healthy(pat_sync_count=7, pat_err_count=4000,
                                    pat_resync_count=6)), MAN_COOP)
        self.assertEqual(v.level, SKIP)
        self.assertIn("relocked", v.missing_witness)

    def test_lock_count_is_absolute_not_a_window_delta(self):
        """A capture opened mid-stream, after lock was already achieved: the
        lock count is 1 in every block, so its delta is 0 across the whole
        capture. Reading it as a delta -- consistent with every other counter
        in this module, and wrong -- would report a healthy soak as never
        having locked. 'Has the pattern held lock in this run' is a property
        of the run, not of the window."""
        blocks = [(0.0, healthy(pat_sync_count=1, pkt_count=0)),
                  (120.0, healthy(pat_sync_count=1))]
        v = rules.r7_sample_continuity(blocks, MAN_COOP)
        self.assertEqual(v.level, PASS)

    def test_never_locked_is_distinguished_from_locked_then_diverged(self):
        """The distinction the spec requires and the old proxy could not draw.
        Identical error counts; only the lock count differs, and it decides
        whether the host is accused or excused."""
        never = rules.r7_sample_continuity(
            series(Block(), healthy(pat_sync_count=0, pat_err_count=9,
                                    pat_first_err_idx=41207)), MAN_COOP)
        diverged = rules.r7_sample_continuity(
            series(Block(), healthy(pat_sync_count=1, pat_err_count=9,
                                    pat_first_err_idx=41207)), MAN_COOP)
        self.assertEqual(never.level, SKIP)
        self.assertEqual(diverged.level, FAIL)
        self.assertEqual(never.evidence["pattern_syncs"], 0)
        self.assertEqual(diverged.evidence["pattern_syncs"], 1)


class TestW2PacketSizeLumpiness(unittest.TestCase):
    def test_pass_on_floor_and_ceiling_only(self):
        v = rules.w2_packet_size_shape(series(Block(), healthy()), MAN)
        self.assertEqual(v.level, PASS)

    def test_warn_on_lumpy_sizes(self):
        lumpy = healthy(size_hist_size=[40 * 32, 48 * 32, 0, 0, 0, 0, 0, 0],
                        size_hist_count=[60000, 60000, 0, 0, 0, 0, 0, 0])
        v = rules.w2_packet_size_shape(series(Block(), lumpy), MAN)
        self.assertEqual(v.level, WARN)
        self.assertTrue(v.consequence)

    def test_skip_when_no_packets(self):
        v = rules.w2_packet_size_shape(series(Block(), Block()), MAN)
        self.assertEqual(v.level, SKIP)


class TestCounterWrap(unittest.TestCase):
    """delta32 itself is tested in test_wireformat; these pin that the rules
    module actually routes counter arithmetic through it. A sample counter at
    8ch x 44.1 kHz wraps in about 3.4 hours, inside the soak durations this
    bench already runs, so a rule doing bare subtraction would report a
    catastrophic negative rather than a small positive."""

    def test_delta_handles_uint32_wrap(self):
        self.assertEqual(rules.delta(0xFFFFFFF0, 0x10), 0x20)

    def test_rule_counter_survives_a_wrap(self):
        blocks = series(Block(alt_out=1, fb_poll_count=0xFFFFFFF0),
                        healthy(fb_poll_count=0x10))
        v = rules.r1_feedback_polled(blocks, MAN)
        self.assertEqual(v.level, PASS)
        self.assertEqual(v.evidence["feedback_polls"], 0x20)


class TestW1ResidualDrift(unittest.TestCase):
    def test_pass_on_a_closed_loop(self):
        v = rules.w1_residual_drift(series(Block(), healthy()), MAN,
                                    fill_slope_bytes_per_s=0.220,
                                    correction_quantum_bytes=1024)
        self.assertEqual(v.level, PASS)

    def test_warn_on_a_biased_servo(self):
        """+4.8 ppm: the dither-chasing servo. 1411200 B/s * 4.8e-6 = 6.77 B/s."""
        v = rules.w1_residual_drift(series(Block(), healthy()), MAN,
                                    fill_slope_bytes_per_s=6.77,
                                    correction_quantum_bytes=1024)
        self.assertEqual(v.level, WARN)
        self.assertIn("ppm", v.consequence)
        self.assertIn("s", v.consequence)

    def test_skip_without_a_slope(self):
        v = rules.w1_residual_drift(series(Block(), healthy()), MAN,
                                    fill_slope_bytes_per_s=None,
                                    correction_quantum_bytes=1024)
        self.assertEqual(v.level, SKIP)

    def test_ppm_uses_the_stream_byte_rate_not_four_bytes_per_frame(self):
        ppm = rules.slope_to_ppm(6.77, MAN)
        self.assertAlmostEqual(ppm, 4.80, places=1)

    def test_seconds_to_glitch_is_quantum_divided_by_slope(self):
        """The consequence arithmetic itself, not just its shape. 1024 B of
        correction quantum drained at 6.77 B/s is a glitch every 151 s;
        multiplying instead of dividing gives 6932 s, which is equally
        plausible-looking prose and off by a factor of 45."""
        v = rules.w1_residual_drift(series(Block(), healthy()), MAN,
                                    fill_slope_bytes_per_s=6.77,
                                    correction_quantum_bytes=1024)
        self.assertAlmostEqual(v.evidence["seconds_between_corrections"],
                               151.3, places=1)
        self.assertIn("every 151 s", v.consequence)


class TestW3FeedbackTracking(unittest.TestCase):
    def test_pass_when_sizing_tracks_feedback(self):
        v = rules.w3_feedback_tracked(series(Block(), healthy()), MAN,
                                      fill_slope_bytes_per_s=0.220)
        self.assertEqual(v.level, PASS)

    def test_warn_when_host_polls_but_ignores(self):
        v = rules.w3_feedback_tracked(series(Block(), healthy()), MAN,
                                      fill_slope_bytes_per_s=118.3)
        self.assertEqual(v.level, WARN)

    def test_skip_when_feedback_never_polled(self):
        v = rules.w3_feedback_tracked(
            series(Block(), healthy(fb_poll_count=0)), MAN,
            fill_slope_bytes_per_s=118.3)
        self.assertEqual(v.level, SKIP)
        self.assertIn("feedback poll", v.missing_witness)

    def test_skip_when_polled_but_no_slope_to_compare_against(self):
        """The second SKIP: the host did poll, so R1's ground is covered, but
        with no fill probe there is no drift to hold the polling against.
        Without this the branch falls through to slope_to_ppm(None) -- and an
        absent instrument must be a SKIP, never a reading of zero."""
        v = rules.w3_feedback_tracked(series(Block(), healthy()), MAN,
                                      fill_slope_bytes_per_s=None)
        self.assertEqual(v.level, SKIP)
        self.assertIn("fill", v.missing_witness.lower())


if __name__ == "__main__":
    unittest.main()
