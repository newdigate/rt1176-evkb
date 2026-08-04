import os
import tempfile
import unittest

from trace import Block, synth_vcd, read_blocks, count_missing_marks


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
        blocks = [(i * 0.01, Block(pkt_count=i * 10)) for i in range(100)]
        with tempfile.TemporaryDirectory() as d:
            path = os.path.join(d, "t.vcd")
            synth_vcd(path, blocks)
            got = read_blocks(path)
        self.assertEqual(len(got), 100)
        self.assertEqual(got[50][1].pkt_count, 500)
        self.assertAlmostEqual(got[50][0], 0.5, places=6)


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
        # Hand-built because synth_vcd models a healthy device; a capture with
        # xscope lost-sample markers is not something the generator emits.
        vcd = (
            "$timescale 1 us $end\n"
            "$var wire 32 ! uacv_w02 $end\n"
            "$var wire 32 \" Missing_Data $end\n"
            "$enddefinitions $end\n"
            "#0\n"
            "b0 !\n"
            "#1000\n"
            "b1 !\n"
            "b1 \"\n"
            "#2000\n"
            "b10 !\n"
            "b1 \"\n"
            "#3000\n"
            "b1 \"\n"
        )
        with tempfile.TemporaryDirectory() as d:
            path = os.path.join(d, "t.vcd")
            with open(path, "w") as f:
                f.write(vcd)
            self.assertEqual(count_missing_marks(path), 3)
            # The uacv words in the same file still read normally, and the
            # marker-only timestamp #3000 yields no block: no state word
            # changed there, so there is no new block to report.
            self.assertEqual([b.pkt_count for _, b in read_blocks(path)],
                             [0, 1, 2])


if __name__ == "__main__":
    unittest.main()
