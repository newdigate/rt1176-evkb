"""Tests for the drift instrument in tools/vcdfill.py.

vcdfill.py lives a directory up because tools/driftrun.sh has used it
standalone since before the validator existed. Its tests live here because
this is the suite that runs, and because the validator is now its second
consumer: W1 and W3 are arithmetic on the number fit_fill_slope() returns, so
a silent change to that number is a silent change to two verdicts about a
host.

Two properties get most of the attention:

  * None is not zero. An absent fill probe and a perfectly stable one are
    opposite findings -- SKIP versus PASS at the noise floor -- and a fitter
    that folded them together would report the best possible result as an
    un-run check, or worse, report an un-run check as the best possible
    result.
  * The fit spans one correction-free segment. A block correction moves the
    FIFO level by hundreds of bytes in a single sample; a fit spanning one
    measures the correction, not the drift, and returns a confident number
    about something nobody asked.
"""
import json
import os
import sys
import tempfile
import unittest

sys.path.append(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))

import vcdfill


def write_vcd(path, fill, fill_id="2", fill_name="out_fifo_fill",
              extra_vars=(), tick_s=1e-3):
    """A minimal VCD carrying one fill trace. `fill` is [(t_s, value)]."""
    head = ["$timescale", " 1 ms", "$end",
            f"$var wire 32 {fill_id} {fill_name} $end"]
    head.extend(extra_vars)
    head.append("$enddefinitions $end")
    body = []
    for t, v in fill:
        body.append(f"#{int(round(t / tick_s))}")
        body.append(f"b{int(v):b} {fill_id}")
    with open(path, "w") as f:
        f.write("\n".join(head + body) + "\n")
    return path


def ramp(t0, t1, slope, base=2000, hz=100.0):
    n = int(round((t1 - t0) * hz))
    return [(t0 + i / hz, round(base + slope * (i / hz))) for i in range(n + 1)]


class VcdCase(unittest.TestCase):
    def setUp(self):
        self.dir = tempfile.TemporaryDirectory()
        self.addCleanup(self.dir.cleanup)

    def path(self, name="t.vcd"):
        return os.path.join(self.dir.name, name)

    def parsed(self, fill, **kw):
        return vcdfill.parse(write_vcd(self.path(), fill, **kw))


class TestSegmentation(VcdCase):
    def test_a_clean_ramp_is_one_segment(self):
        segs = vcdfill.segments(ramp(0, 60, 2.5))
        self.assertEqual(len(segs), 1)

    def test_splits_at_a_block_correction(self):
        """A step larger than SEGMENT_STEP_BYTES is a dry-out refill or an
        overflow drain, not drift."""
        pts = ramp(0, 30, 2.5) + ramp(30, 60, 2.5, base=2000 - 900)
        segs = vcdfill.segments(pts)
        self.assertEqual(len(segs), 2)

    def test_does_not_split_on_a_step_at_the_threshold(self):
        """The comparison is strict: 300 B exactly is not a correction. Pinned
        so the boundary cannot drift into or out of the fit unnoticed."""
        pts = ramp(0, 30, 0) + ramp(30, 60, 0,
                                    base=2000 + vcdfill.SEGMENT_STEP_BYTES)
        self.assertEqual(len(vcdfill.segments(pts)), 1)

    def test_splits_at_an_emission_gap(self):
        """Overflow gaps the emissions entirely, because emission rides on
        packet arrival, so a hole in the trace is itself evidence."""
        pts = ramp(0, 30, 2.5) + ramp(35, 60, 2.5, base=2075)
        self.assertEqual(len(vcdfill.segments(pts)), 2)

    def test_drops_fragments_with_too_few_points(self):
        pts = ramp(0, 30, 2.5) + [(40.0, 9000), (41.0, 9001)]
        self.assertEqual(len(vcdfill.segments(pts)), 1)

    def test_drops_a_long_but_sparsely_sampled_fragment(self):
        """Six points spread over 2.25 s clear the duration floor and still
        fail the point-count floor. The two filters are not redundant: this is
        the shape a stretch of dropped emissions leaves behind, and fitting six
        points across it produces a slope with no support under it. Written
        because the earlier too-few-points fixture was also too SHORT, so
        deleting the point-count filter left the suite green."""
        sparse = [(40.0 + i * 0.45, 9000 + i) for i in range(6)]
        self.assertGreaterEqual(sparse[-1][0] - sparse[0][0],
                                vcdfill.MIN_SEGMENT_SECONDS)
        self.assertLess(len(sparse), vcdfill.MIN_SEGMENT_POINTS)
        self.assertEqual(len(vcdfill.segments(ramp(0, 30, 2.5) + sparse)), 1)

    def test_drops_fragments_shorter_than_the_minimum_duration(self):
        """Twenty points spanning 0.2 s clear the point-count floor and still
        cannot carry a slope: over that window the drift is smaller than the
        sample-to-sample jitter."""
        pts = ramp(0, 30, 2.5) + ramp(40, 40.2, 2.5, base=9000)
        self.assertEqual(len(vcdfill.segments(pts)), 1)

    def test_empty_input_is_no_segments_not_a_crash(self):
        self.assertEqual(vcdfill.segments([]), [])


class TestLongestSegment(VcdCase):
    def test_picks_by_duration_not_by_point_count(self):
        """A sparse 30 s stretch beats a dense 10 s one. The fit's confidence
        comes from the time base it spans, and emission is not periodic --
        choosing by len() would prefer whichever segment happened to be
        sampled hardest."""
        sparse = [(t, round(2000 + 2.5 * t)) for t in
                  [i * 0.3 for i in range(101)]]          # 30 s, 101 points
        dense = ramp(40, 50, 9.0, base=5000)              # 10 s, 1001 points
        sigs = self.parsed(sparse + dense)
        seg = vcdfill.longest_segment(sigs)
        self.assertAlmostEqual(seg[-1][0] - seg[0][0], 30.0, places=1)

    def test_the_longest_segment_is_not_assumed_to_be_the_first(self):
        """A soak that corrects early and then runs clean for an hour is the
        normal shape: the useful stretch is the last one, not the first.
        Returning segs[0] would answer with the pre-correction fragment, which
        is both short and, being adjacent to a correction, the least
        representative part of the trace."""
        pts = ramp(0, 5, 10.0) + ramp(6, 46, 2.5, base=9000)
        sigs = self.parsed(pts)
        seg = vcdfill.longest_segment(sigs)
        self.assertAlmostEqual(seg[-1][0] - seg[0][0], 40.0, places=1)
        self.assertAlmostEqual(vcdfill.fit_fill_slope(sigs), 2.5, places=2)

    def test_none_when_there_is_no_fill_trace(self):
        self.assertIsNone(vcdfill.longest_segment(self.parsed([])))


class TestFitFillSlope(VcdCase):
    def test_recovers_a_known_slope(self):
        self.assertAlmostEqual(vcdfill.fit_fill_slope(self.parsed(ramp(0, 120, 2.5))),
                               2.5, places=2)

    def test_recovers_a_negative_slope(self):
        self.assertAlmostEqual(vcdfill.fit_fill_slope(self.parsed(ramp(0, 120, -1.75,
                                                                      base=8000))),
                               -1.75, places=2)

    def test_fits_the_longest_segment_not_across_the_corrections(self):
        """The fixture is built so the two answers cannot be confused: each
        segment drifts at +2.5 B/s, but the corrections between them are large
        and one-directional, so a fit across the whole trace reads about
        +9 B/s. Off by a factor of three, and confidently."""
        pts = (ramp(0, 60, 2.5)
               + ramp(60, 90, 2.5, base=2500)
               + ramp(90, 120, 2.5, base=3000))
        sigs = self.parsed(pts)
        self.assertAlmostEqual(vcdfill.fit_fill_slope(sigs), 2.5, places=2)
        whole = vcdfill.fit_slope([(t, v) for t, v in pts])[0]
        self.assertGreater(whole, 5.0)

    def test_none_when_the_capture_has_no_fill_probe(self):
        """None, not 0.0. The validator turns None into SKIP and 0.0 into a
        PASS at the noise floor; collapsing them would let a capture with no
        instrument in it be reported as a host with no drift in it."""
        self.assertIsNone(vcdfill.fit_fill_slope(self.parsed([])))

    def test_none_when_every_segment_is_too_short(self):
        self.assertIsNone(vcdfill.fit_fill_slope(self.parsed(ramp(0, 0.5, 2.5))))

    def test_zero_for_a_genuinely_flat_trace(self):
        """The other half of the same distinction: a real reading of no drift
        must come back as 0.0 and must not be None."""
        slope = vcdfill.fit_fill_slope(self.parsed(ramp(0, 120, 0.0)))
        self.assertIsNotNone(slope)
        self.assertAlmostEqual(slope, 0.0, places=6)


class TestPhantomWrapRepairInFillTrace(VcdCase):
    def test_fit_spans_a_phantom_wrap(self):
        """The capture chain's spurious 32-bit wrap (wireformat.XSCOPE_WRAP_S)
        pushes every later timestamp 42.9497 s late. Unrepaired, the jump
        splits the trace at SEGMENT_GAP_S and caps every fitted window at the
        inter-wrap spacing -- the short-window trap that once produced a
        confident -3.39 ppm a 21-minute soak read as +0.24. Repaired, the two
        halves are one continuous segment with the true slope."""
        from wireformat import XSCOPE_WRAP_S
        first = ramp(0, 60, 2.5)
        second = [(t + 60 + XSCOPE_WRAP_S, 150 + (v - 0))
                  for t, v in ramp(0, 60, 2.5)]
        sigs = self.parsed(first + second)
        seg = vcdfill.longest_segment(sigs)
        self.assertGreater(seg[-1][0] - seg[0][0], 100)   # spans both halves
        self.assertAlmostEqual(vcdfill.fit_fill_slope(sigs), 2.5, places=2)

    def test_a_real_forty_second_gap_still_splits(self):
        """Only the exact quantum is repaired: a genuine 40 s silence is a
        stream stop, and fitting across one measures the stop."""
        first = ramp(0, 60, 2.5)
        second = [(t + 100, 150 + v) for t, v in ramp(0, 60, 2.5)]
        seg = vcdfill.longest_segment(self.parsed(first + second))
        self.assertLess(seg[-1][0] - seg[0][0], 61)


class TestFillSignalResolution(VcdCase):
    def test_resolved_by_name_at_any_identifier(self):
        """xscope assigns ids in xscope_register order, so "2" is the fill
        probe only until somebody prepends one. The name is the contract."""
        sigs = self.parsed(ramp(0, 60, 2.5), fill_id="QQ")
        self.assertAlmostEqual(vcdfill.fit_fill_slope(sigs), 2.5, places=2)

    def test_falls_back_to_the_historical_id_for_an_unfamiliar_name(self):
        """out_fifo_fill replaced out_fifo_free partway through this bench's
        life. Captures from before that rename still have the trace at id 2,
        and analyse() has always read them; the fallback keeps them readable
        rather than turning every archived capture into "no fill probe"."""
        sigs = self.parsed(ramp(0, 60, 2.5), fill_name="out_fifo_free")
        self.assertAlmostEqual(vcdfill.fit_fill_slope(sigs), 2.5, places=2)

    def test_refuses_the_id_fallback_in_a_wireformat_capture(self):
        """In a capture written by uacvalidate/wireformat.py's synth_vcd the
        VCD identifiers are printable characters from '!', so identifier "2"
        is chr(50) -- state word 17, pat_first_expected. Fitting a drift slope
        to a state counter is the 0.222-slope failure that started this whole
        design. A capture declaring uacv_w* words and no out_fifo_fill has no
        fill probe, full stop."""
        sigs = self.parsed(ramp(0, 60, 2.5), fill_name="out_fifo_free",
                           extra_vars=["$var wire 32 ! uacv_w00 $end"])
        self.assertIsNone(vcdfill.fit_fill_slope(sigs))


class TestAnalyseStillWorksStandalone(VcdCase):
    """tools/driftrun.sh consumes analyse()'s JSON. Extracting the segmenter
    out of it must not have changed a single key it reads."""

    def _analyse(self, path):
        import io
        from contextlib import redirect_stdout
        buf = io.StringIO()
        with redirect_stdout(buf):
            rc = vcdfill.analyse(path, want_json=True)
        return rc, json.loads(buf.getvalue())

    def test_reports_the_keys_driftrun_reads(self):
        p = write_vcd(self.path(), ramp(0, 120, 2.5))
        rc, out = self._analyse(p)
        self.assertEqual(rc, 0)
        for k in ("duration_s", "weighted_ppm_equiv", "overflow_final",
                  "dryout_final", "xscope_missing_marks"):
            self.assertIn(k, out)

    def test_segment_table_matches_the_shared_segmenter(self):
        """The point of the extraction: vcdfill's own per-segment table and
        the single slope handed to the validator are cut the same way. Two
        segmenters would let the printed report and the W1 verdict disagree
        while both calling it "the drift"."""
        pts = ramp(0, 60, 2.5) + ramp(60, 120, 2.5, base=2500)
        p = write_vcd(self.path(), pts)
        _, out = self._analyse(p)
        shared = vcdfill.segments(vcdfill.fill_points(vcdfill.parse(p)))
        self.assertEqual(len(out["segments"]), len(shared))
        for row, seg in zip(out["segments"], shared):
            self.assertAlmostEqual(row["t_start"], seg[0][0], places=2)
            self.assertAlmostEqual(row["t_end"], seg[-1][0], places=2)

    def test_a_capture_with_no_fill_still_exits_one(self):
        p = write_vcd(self.path(), [])
        rc, _ = self._analyse(p)
        self.assertEqual(rc, 1)


if __name__ == "__main__":
    unittest.main()
