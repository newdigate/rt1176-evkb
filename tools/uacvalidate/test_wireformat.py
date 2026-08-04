import os
import tempfile
import unittest

from trace import (BLOCK_WORDS, Block, count_missing_marks, read_blocks,
                   synth_vcd)


class TestSynthRoundTrip(unittest.TestCase):
    def test_single_block_round_trips(self):
        b = Block(pkt_count=8000, or_acc=0xFFFFFF00, and_acc=0x00000000,
                  nonsilent_frames=44100, fb_poll_count=63, fb_value=0x000B0000,
                  alt_out=1)
        with tempfile.TemporaryDirectory() as d:
            path = os.path.join(d, "t.vcd")
            synth_vcd(path, [(0.0, b)])
            got = read_blocks(path)
        self.assertEqual(len(got), 1)
        t, blk = got[0]
        self.assertEqual(t, 0.0)
        self.assertEqual(blk.pkt_count, 8000)
        self.assertEqual(blk.or_acc, 0xFFFFFF00)
        self.assertEqual(blk.nonsilent_frames, 44100)
        self.assertEqual(blk.fb_poll_count, 63)
        self.assertEqual(blk.alt_out, 1)

    def test_defaults_are_zero(self):
        b = Block()
        self.assertEqual(b.pkt_count, 0)
        self.assertEqual(b.size_hist_size, [0] * 8)
        self.assertEqual(b.size_hist_count, [0] * 8)

    def test_sizes_helper_pairs_nonzero_slots(self):
        b = Block(size_hist_size=[192, 160, 0, 0, 0, 0, 0, 0],
                  size_hist_count=[7000, 1000, 0, 0, 0, 0, 0, 0])
        self.assertEqual(b.sizes(), {192: 7000, 160: 1000})

    def test_many_blocks_keep_order_and_time(self):
        # and_acc is stationary and non-default, so it is written once at t=0
        # and never again: reading it back at t=0.5 can only succeed if the
        # reader carries unchanged words forward.
        blocks = [(i * 0.01, Block(pkt_count=i * 10, and_acc=0xDEADBEEF))
                  for i in range(100)]
        with tempfile.TemporaryDirectory() as d:
            path = os.path.join(d, "t.vcd")
            synth_vcd(path, blocks)
            got = read_blocks(path)
            with open(path) as f:
                body = f.read()
        self.assertEqual(len(got), 100)
        self.assertEqual(got[50][1].pkt_count, 500)
        self.assertEqual(got[50][1].and_acc, 0xDEADBEEF)
        self.assertEqual(got[-1][1].and_acc, 0xDEADBEEF)
        self.assertAlmostEqual(got[50][0], 0.5, places=6)
        # And the writer must be emitting deltas, not a full block each time:
        # 36 words at t=0 plus one changed word per later timestamp, nowhere
        # near the 3600 lines a re-send-everything writer would produce.
        b_lines = [ln for ln in body.split("\n") if ln.startswith("b")]
        self.assertEqual(len(b_lines), BLOCK_WORDS + 99)


class TestBlockWords(unittest.TestCase):
    def test_and_acc_defaults_to_all_ones(self):
        # AND-accumulator identity: a fresh block must not read as "every bit
        # of every sample was clear".
        self.assertEqual(Block().and_acc, 0xFFFFFFFF)

    def test_words_round_trip(self):
        b = Block(magic=0x55414356, version=1, pkt_count=1, pkt_short_discarded=2,
                  pkt_not_multiple=3, or_acc=4, and_acc=5, nonsilent_frames=6,
                  fb_poll_count=7, fb_value=8, alt_out=9, alt_transitions=10,
                  class_req_bitmap=11, host_active=12, pat_err_count=13,
                  pat_resync_count=14, pat_first_err_idx=15,
                  pat_first_expected=16, pat_first_actual=17,
                  size_hist_overflow=18,
                  size_hist_size=list(range(20, 28)),
                  size_hist_count=list(range(28, 36)))
        w = b.to_words()
        self.assertEqual(len(w), 36)
        self.assertEqual(w[2], 1)
        self.assertEqual(w[20:28], list(range(20, 28)))
        self.assertEqual(Block.from_words(w), b)

    def test_from_words_rejects_wrong_length(self):
        with self.assertRaises(ValueError):
            Block.from_words([0] * 35)

    def test_to_words_masks_to_32_bits(self):
        self.assertEqual(Block(pkt_count=-1).to_words()[2], 0xFFFFFFFF)


class TestMissingMarks(unittest.TestCase):
    def test_absent_signal_counts_zero(self):
        with tempfile.TemporaryDirectory() as d:
            path = os.path.join(d, "t.vcd")
            synth_vcd(path, [(0.0, Block()), (0.01, Block(pkt_count=1))])
            self.assertEqual(count_missing_marks(path), 0)

    def test_counts_value_change_records(self):
        # Markers are spliced in rather than generated: synth_vcd models a
        # healthy device, and teaching it to fake an xscope failure would be
        # inventing a second, unverifiable emitter.
        with tempfile.TemporaryDirectory() as d:
            path = os.path.join(d, "t.vcd")
            synth_vcd(path, [(0.0, Block(pkt_count=1)),
                             (0.01, Block(pkt_count=2))])
            with open(path) as f:
                text = f.read()
            text = text.replace(
                "$enddefinitions $end",
                "$var wire 32 ~ Missing_Data $end\n$enddefinitions $end")
            # Two markers alongside real changes, one at a trailing timestamp
            # that carries nothing else.
            text = text.replace("#10000\n", "#10000\nb1 ~\n")
            text += "b1 ~\n#20000\nb1 ~\n"
            with open(path, "w") as f:
                f.write(text)
            self.assertEqual(count_missing_marks(path), 3)
            # The state words in the same file still read normally, and the
            # marker-only timestamp #20000 yields no block: no state word
            # changed there, so there is no new block to report.
            self.assertEqual([b.pkt_count for _, b in read_blocks(path)],
                             [1, 2])


class TestTruncatedCapture(unittest.TestCase):
    def test_partial_var_set_is_refused(self):
        vcd = ("$timescale 1 us $end\n"
               "$var wire 32 ! uacv_w00 $end\n"
               "$var wire 32 \" uacv_w01 $end\n"
               "$var wire 32 # uacv_w02 $end\n"
               "$enddefinitions $end\n"
               "#0\n"
               "b1010101010000010100001101010110 !\n"
               "b1 \"\n"
               "b1 #\n")
        with tempfile.TemporaryDirectory() as d:
            path = os.path.join(d, "t.vcd")
            with open(path, "w") as f:
                f.write(vcd)
            with self.assertRaises(ValueError) as cm:
                read_blocks(path)
        # The message has to name what was found and exactly what was absent:
        # this error is how a wire-format change first shows itself.
        msg = str(cm.exception)
        self.assertIn(f"3 of {BLOCK_WORDS}", msg)
        self.assertIn(str(list(range(3, BLOCK_WORDS))), msg)


if __name__ == "__main__":
    unittest.main()
