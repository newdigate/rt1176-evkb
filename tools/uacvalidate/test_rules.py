import unittest

from wireformat import Block
from manifest import Manifest
from verdict import PASS, FAIL, WARN, SKIP
import rules


MAN = Manifest.from_dict({
    "audio_class": 2, "speed": "HS", "channels": 8, "subslot_bytes": 4,
    "sample_rate_hz": 44100, "mode": "passive", "host_note": "test",
})
MAN_COOP = Manifest.from_dict({
    "audio_class": 2, "speed": "HS", "channels": 8, "subslot_bytes": 4,
    "sample_rate_hz": 44100, "mode": "cooperative", "host_note": "test",
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
            "sample_rate_hz": 44100, "mode": "passive", "host_note": "t"})
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

    def test_never_synced_is_distinguished_from_diverged(self):
        v = rules.r7_sample_continuity(
            series(Block(), healthy(pat_err_count=0, pat_resync_count=0,
                                    nonsilent_frames=5292000, pat_first_err_idx=0,
                                    pat_first_expected=0, pat_first_actual=0,
                                    pkt_count=120000)), MAN_COOP)
        self.assertEqual(v.level, PASS)


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


if __name__ == "__main__":
    unittest.main()
