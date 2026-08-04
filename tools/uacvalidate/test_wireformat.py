import os
import tempfile
import unittest

from wireformat import (BLOCK_WORDS, Block, count_missing_marks, delta32,
                        read_blocks, synth_vcd)


def _vcd_id(word):
    """The identifier synth_vcd assigns to a word, for splicing fixtures."""
    return chr(33 + word)


def _synth_text(path, blocks, **kw):
    synth_vcd(path, blocks, **kw)
    with open(path) as f:
        return f.read()


def _rewrite(path, text):
    with open(path, "w") as f:
        f.write(text)


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

    def test_sizes_selects_on_size_not_count(self):
        # A claimed slot that has seen no packets yet is real and must survive;
        # an unclaimed slot is not a zero-byte packet class, whatever count
        # happens to sit beside it.
        b = Block(size_hist_size=[192, 0, 0, 0, 0, 0, 0, 0],
                  size_hist_count=[0, 5, 0, 0, 0, 0, 0, 0])
        self.assertEqual(b.sizes(), {192: 0})

    def test_sizes_sums_duplicate_slots(self):
        # Defence against a malformed capture: overwriting would lose ten
        # packets and leave a judge unable to reconcile sizes() with pkt_count.
        b = Block(size_hist_size=[192, 192, 160, 0, 0, 0, 0, 0],
                  size_hist_count=[10, 20, 5, 0, 0, 0, 0, 0])
        self.assertEqual(b.sizes(), {192: 30, 160: 5})

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
        # one whole block at t=0 plus one changed word per later timestamp,
        # nowhere near the 3700 lines a re-send-everything writer would produce.
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
                  size_hist_overflow=18, pat_sync_count=19,
                  size_hist_size=list(range(20, 28)),
                  size_hist_count=list(range(28, 36)))
        w = b.to_words()
        self.assertEqual(len(w), BLOCK_WORDS)
        self.assertEqual(w[2], 1)
        self.assertEqual(w[20:28], list(range(20, 28)))
        # pat_sync_count sits after the histograms, not among the other pat_*
        # fields: the dataclass groups it for readability, the wire does not.
        self.assertEqual(w[36], 19)
        self.assertEqual(Block.from_words(w), b)

    def test_from_words_rejects_wrong_length(self):
        with self.assertRaises(ValueError):
            Block.from_words([0] * (BLOCK_WORDS - 1))

    def test_to_words_masks_to_32_bits(self):
        self.assertEqual(Block(pkt_count=-1).to_words()[2], 0xFFFFFFFF)

    def test_from_words_masks_to_32_bits(self):
        w = [0] * BLOCK_WORDS
        w[2] = (1 << 32) | 5
        self.assertEqual(Block.from_words(w).pkt_count, 5)

    def test_histogram_lists_must_be_full_length(self):
        # Without this the error surfaces as a bare IndexError from inside
        # to_words, far from the caller that supplied the short list.
        with self.assertRaises(ValueError):
            Block(size_hist_size=[192])
        with self.assertRaises(ValueError):
            Block(size_hist_count=[0] * 9)


class TestDelta32(unittest.TestCase):
    def test_plain_advance(self):
        self.assertEqual(delta32(100, 140), 40)

    def test_wrap_is_a_small_positive_advance(self):
        # The whole reason this lives here: unmasked, a one-count wrap reads
        # as -4294967295.
        self.assertEqual(delta32(0xFFFFFFFF, 0), 1)
        self.assertEqual(delta32(0xFFFFFF00, 0x00000010), 0x110)


class TestStationaryBlocks(unittest.TestCase):
    def test_identical_blocks_all_survive(self):
        # Corpus case E: a stalled host moves nothing at all. Without the
        # heartbeat every one of these timestamps would emit a bare "#t" and
        # the whole stall would vanish, leaving a judge unable to tell it from
        # a dead observer or a capture gap.
        n = 10
        blocks = [(i * 0.01, Block(pkt_count=7)) for i in range(n)]
        with tempfile.TemporaryDirectory() as d:
            path = os.path.join(d, "t.vcd")
            text = _synth_text(path, blocks)
            got = read_blocks(path)
        self.assertEqual(len(got), n)
        self.assertTrue(all(b.pkt_count == 7 for _, b in got))
        self.assertAlmostEqual(got[-1][0], 0.09, places=6)
        # The heartbeat costs one line per stationary timestamp, not 36.
        b_lines = [ln for ln in text.split("\n") if ln.startswith("b")]
        self.assertEqual(len(b_lines), BLOCK_WORDS + (n - 1))

    def test_stall_inside_a_moving_capture_keeps_every_sample(self):
        blocks = ([(0.00, Block(pkt_count=1))]
                  + [(0.01 * i, Block(pkt_count=2)) for i in range(1, 5)]
                  + [(0.05, Block(pkt_count=3))])
        with tempfile.TemporaryDirectory() as d:
            path = os.path.join(d, "t.vcd")
            synth_vcd(path, blocks)
            got = read_blocks(path)
        self.assertEqual([b.pkt_count for _, b in got], [1, 2, 2, 2, 2, 3])


class TestTimescale(unittest.TestCase):
    def test_non_default_timescale_round_trips(self):
        with tempfile.TemporaryDirectory() as d:
            path = os.path.join(d, "t.vcd")
            text = _synth_text(path, [(0.0, Block(pkt_count=1)),
                                      (0.5, Block(pkt_count=2)),
                                      (2.5, Block(pkt_count=3))],
                               timescale_ps=1000000000)
            got = read_blocks(path)
        self.assertIn("1 ms", text)
        self.assertAlmostEqual(got[1][0], 0.5, places=9)
        self.assertAlmostEqual(got[2][0], 2.5, places=9)

    def test_unit_is_the_largest_that_stays_whole(self):
        # Pins the unit choice itself: a writer that always said "ns" would
        # still round-trip numerically, so times alone cannot catch it.
        cases = {1: "1 ps", 1000: "1 ns", 1000000: "1 us",
                 1000000000: "1 ms", 1000000000000: "1 s"}
        with tempfile.TemporaryDirectory() as d:
            for ps, want in cases.items():
                path = os.path.join(d, f"t{ps}.vcd")
                text = _synth_text(path, [(0.0, Block())], timescale_ps=ps)
                self.assertIn(want, text, f"timescale_ps={ps}")


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

    def test_scalar_form_markers_are_counted(self):
        # xscope may declare the marker 1 bit wide, and a 1-bit $var is emitted
        # in scalar form; ignoring that shape would report a clean capture.
        vcd = ("$timescale 1 us $end\n"
               "$var wire 1 ~ Missing_Data $end\n"
               "$enddefinitions $end\n"
               "#0\n1~\n#1000\n1~\n")
        with tempfile.TemporaryDirectory() as d:
            path = os.path.join(d, "t.vcd")
            _rewrite(path, vcd)
            self.assertEqual(count_missing_marks(path), 2)

    def test_zero_valued_markers_are_not_counted(self):
        # A zero record says nothing was missing at that instant. Counting it
        # would condemn a clean capture, and would disagree with vcdfill.py.
        vcd = ("$timescale 1 us $end\n"
               "$var wire 32 ~ Missing_Data $end\n"
               "$var wire 1 } Missing_Data $end\n"
               "$enddefinitions $end\n"
               "#0\nb0 ~\n0}\n#1000\nb0 ~\n0}\n#2000\nb1 ~\n")
        with tempfile.TemporaryDirectory() as d:
            path = os.path.join(d, "t.vcd")
            _rewrite(path, vcd)
            self.assertEqual(count_missing_marks(path), 1)


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

    def test_unknown_word_index_is_refused_and_named(self):
        # A word beyond the block means the observer is emitting a layout this
        # module does not know; silently ignoring it would read the newer
        # format as if it were this one.
        with tempfile.TemporaryDirectory() as d:
            path = os.path.join(d, "t.vcd")
            text = _synth_text(path, [(0.0, Block())])
            _rewrite(path, text.replace(
                "$enddefinitions $end",
                f"$var wire 32 ~ uacv_w{BLOCK_WORDS} $end\n"
                f"$enddefinitions $end"))
            with self.assertRaises(ValueError) as cm:
                read_blocks(path)
        msg = str(cm.exception)
        self.assertIn("unknown word indices", msg)
        self.assertIn(str(BLOCK_WORDS), msg)

    def test_incomplete_first_timestamp_is_refused(self):
        # Every word declared, but the opening sample of the last one never
        # arrived. Reporting it as 0 would hand a rule a value the device
        # never sent.
        with tempfile.TemporaryDirectory() as d:
            path = os.path.join(d, "t.vcd")
            text = _synth_text(path, [(0.0, Block(pkt_count=1)),
                                      (0.01, Block(pkt_count=2))])
            lost = [ln for ln in text.split("\n")
                    if ln.endswith(" " + _vcd_id(BLOCK_WORDS - 1))]
            self.assertEqual(len(lost), 1)  # fixture is what we think it is
            _rewrite(path, text.replace(lost[0] + "\n", ""))
            with self.assertRaises(ValueError) as cm:
                read_blocks(path)
        msg = str(cm.exception)
        self.assertIn(f"{BLOCK_WORDS - 1} of {BLOCK_WORDS}", msg)
        self.assertIn(f"[{BLOCK_WORDS - 1}]", msg)

    def test_oversized_value_in_capture_is_masked(self):
        # A 33-bit value cannot come from a conforming observer, but if one
        # arrives it must not widen a uint32 counter downstream.
        with tempfile.TemporaryDirectory() as d:
            path = os.path.join(d, "t.vcd")
            text = _synth_text(path, [(0.0, Block(pkt_count=1))])
            wide = format((1 << 32) | 5, "b")
            _rewrite(path, text.replace(f"b1 {_vcd_id(2)}\n",
                                        f"b{wide} {_vcd_id(2)}\n"))
            got = read_blocks(path)
        self.assertEqual(got[0][1].pkt_count, 5)


if __name__ == "__main__":
    unittest.main()
