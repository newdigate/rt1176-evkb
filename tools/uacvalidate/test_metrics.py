"""Tests for the unjudged metrics section.

Two things need pinning here that nothing else in the package covers.

First, that metrics stay unjudged. They are the one part of the report with no
level and no citation, so the discipline that verdict.py enforces by
construction has to be enforced here by test: a metric must never move the
exit code, however alarming it reads.

Second, that defect #4 is reportable at all. `class_req_bitmap` and
`host_active` were decoded by wireformat and read by nothing, which left
"the host claimed the device and then never completed the class configuration
sequence" -- one of the six defects that motivated this tool -- with no path
to the page.
"""
import unittest

from manifest import Manifest
from verdict import Verdict, PASS
from wireformat import Block
import metrics
import report


MAN = Manifest.from_dict({
    "audio_class": 2, "speed": "HS", "channels": 8, "subslot_bytes": 4,
    "sample_rate_hz": 44100, "mode": "passive", "host_note": "test",
    "max_packet_size_bytes": 1536,
})

HEALTHY = dict(
    pkt_count=120000, fb_poll_count=7500, fb_value=0x000B0000, alt_out=1,
    alt_transitions=1, class_req_bitmap=0x5, host_active=1,
    size_hist_size=[44 * 32, 45 * 32, 0, 0, 0, 0, 0, 0],
    size_hist_count=[108000, 12000, 0, 0, 0, 0, 0, 0])


def pair(**over):
    return [(0.0, Block(alt_out=1)), (120.0, Block(**dict(HEALTHY, **over)))]


def as_dict(rows):
    return dict(rows)


def fake_probes(points_by_name):
    """Stand-in for vcdfill.probe_points, so these tests do not depend on
    which tool parses the probe section."""
    return lambda _sigs, name: points_by_name.get(name, [])


class TestShape(unittest.TestCase):
    def test_empty_capture_yields_no_metrics(self):
        self.assertEqual(metrics.collect([], MAN), [])

    def test_rows_are_label_value_string_pairs(self):
        for k, v in metrics.collect(pair(), MAN):
            self.assertIsInstance(k, str)
            self.assertIsInstance(v, str)

    def test_order_is_stable(self):
        a = [k for k, _ in metrics.collect(pair(), MAN)]
        b = [k for k, _ in metrics.collect(pair(pkt_count=99), MAN)]
        self.assertEqual(a, b)


class TestStreamMetrics(unittest.TestCase):
    def test_packet_rate_is_measured_against_nominal(self):
        d = as_dict(metrics.collect(pair(), MAN))
        self.assertIn("120000", d["packets"])
        self.assertIn("1000.0/s", d["packets"])
        self.assertIn("nominal 1000/s", d["packets"])

    def test_single_block_capture_has_no_rate_and_does_not_divide_by_zero(self):
        """One block spans no time, so there is no packet delta and no rate to
        quote. The row must say 0 packets and omit the per-second figure
        rather than raise or invent one."""
        d = as_dict(metrics.collect([(5.0, Block(**HEALTHY))], MAN))
        self.assertEqual(d["packets"], "0")
        self.assertIn("0.00 s over 1 state blocks", d["duration"])

    def test_packet_sizes_are_listed_with_counts(self):
        d = as_dict(metrics.collect(pair(), MAN))
        self.assertIn("1408 B x108000", d["packet sizes"])
        self.assertIn("1440 B x12000", d["packet sizes"])

    def test_histogram_overflow_is_reported_beside_the_sizes(self):
        """The note has to survive alongside real sizes, which is where the
        first version lost it: `join(...) or "none" + note` binds the note to
        the empty branch, so it appeared only when there was nothing to
        qualify."""
        d = as_dict(metrics.collect(pair(size_hist_overflow=9000), MAN))
        self.assertIn("1408 B", d["packet sizes"])
        self.assertIn("9000 beyond the histogram", d["packet sizes"])

    def test_feedback_poll_rate_and_raw_value(self):
        d = as_dict(metrics.collect(pair(), MAN))
        self.assertIn("62.5/s", d["feedback polls"])
        self.assertIn("0x000B0000", d["device feedback value"])

    def test_the_feedback_value_says_it_is_undecoded(self):
        """The raw hex has to announce itself as raw.

        Decoding needs the 10.14-at-FS vs 16.16-at-HS choice AND the shift
        lib_xua applies, which no capture has confirmed. A bare hex word with
        no marker invites a reader to assume it was decoded and found
        uninteresting, or invites a later contributor to "helpfully" convert
        it to Hz using a guess. The marker is the deferral, made visible."""
        d = as_dict(metrics.collect(pair(), MAN))
        self.assertIn("raw", d["device feedback value"])
        self.assertIn("undecided", d["device feedback value"])

    def test_alt_transitions_are_listed_with_timestamps(self):
        blocks = [(0.0, Block(alt_out=0)), (1.0, Block(alt_out=1)),
                  (2.0, Block(alt_out=1)), (3.0, Block(alt_out=0))]
        d = as_dict(metrics.collect(blocks, MAN))
        self.assertIn("1.00s->1", d["alt setting"])
        self.assertIn("3.00s->0", d["alt setting"])

    def test_event_lists_are_capped(self):
        blocks = [(float(i), Block(alt_out=i % 2)) for i in range(40)]
        d = as_dict(metrics.collect(blocks, MAN))
        self.assertIn("more", d["alt setting"])
        self.assertLessEqual(d["alt setting"].count("s->"),
                             metrics.MAX_EVENTS_LISTED)


class TestDefectFourIsReportable(unittest.TestCase):
    """Founding defect #4, as an observation rather than a verdict.

    No clause in hand obliges a host to issue any particular class request, so
    this cannot be a FAIL, and there is no arithmetic behind a threshold that
    would make it a WARN. Stating it and letting the reader decide is the
    honest third option -- but it has to actually be stated.
    """

    def test_active_host_with_no_class_requests_is_called_out(self):
        d = as_dict(metrics.collect(pair(class_req_bitmap=0, host_active=1), MAN))
        self.assertIn("none arrived", d["class requests"])
        self.assertIn("no class request", d["class requests"])

    def test_class_requests_that_arrived_are_decoded_to_bits(self):
        d = as_dict(metrics.collect(pair(class_req_bitmap=0x5), MAN))
        self.assertIn("[0, 2]", d["class requests"])
        self.assertNotIn("no class request", d["class requests"])

    def test_a_host_that_never_claimed_the_device_is_not_accused(self):
        """No class requests is unremarkable when the host never went active.
        Flagging it there would put a finding on every idle capture and train
        the reader to skip the line."""
        d = as_dict(metrics.collect(pair(class_req_bitmap=0, host_active=0), MAN))
        self.assertNotIn("no class request", d["class requests"])

    def test_host_active_history_is_reported(self):
        blocks = [(0.0, Block(host_active=0)), (2.0, Block(host_active=1)),
                  (9.0, Block(host_active=0))]
        d = as_dict(metrics.collect(blocks, MAN))
        self.assertIn("2.00s->1", d["host active"])
        self.assertIn("9.00s->0", d["host active"])

    def test_activity_earlier_in_the_capture_still_counts(self):
        """A host that claimed the device, issued nothing and detached again
        is exactly defect #4, and reading only the final block would miss it."""
        blocks = [(0.0, Block(host_active=1, class_req_bitmap=0)),
                  (9.0, Block(host_active=0, class_req_bitmap=0))]
        d = as_dict(metrics.collect(blocks, MAN))
        self.assertIn("no class request", d["class requests"])


class TestProbeDerivedMetrics(unittest.TestCase):
    def test_fill_envelope_from_the_probe(self):
        probes = fake_probes({"out_fifo_fill": [(0.0, 100), (1.0, 300),
                                                (2.0, 200)]})
        d = as_dict(metrics.collect(pair(), MAN, {"x": []}, 2.5,
                                    probe_reader=probes))
        self.assertIn("min 100 B", d["OUT FIFO fill"])
        self.assertIn("max 300 B", d["OUT FIFO fill"])
        self.assertIn("mean 200 B", d["OUT FIFO fill"])

    def test_correction_counters_are_read_from_probes_zero_one_and_three(self):
        probes = fake_probes({"out_fifo_fill": [(0.0, 1)],
                              "out_dryout": [(0.0, 0), (5.0, 3)],
                              "out_overflow": [(0.0, 0), (6.0, 7)],
                              "out_underflow": [(0.0, 0), (7.0, 2)]})
        d = as_dict(metrics.collect(pair(), MAN, {"x": []}, 0.0,
                                    probe_reader=probes))
        self.assertIn("dry-outs 3", d["block corrections"])
        self.assertIn("overflows 7", d["block corrections"])
        self.assertIn("underflows 2", d["block corrections"])

    def test_absent_probes_say_so_rather_than_reporting_zero(self):
        """The None-versus-zero distinction again. "overflows 0" from a
        capture with no overflow probe in it is a fabricated clean bill of
        health."""
        d = as_dict(metrics.collect(pair(), MAN))
        self.assertIn("no fill probe", d["OUT FIFO fill"])
        self.assertIn("?", d["block corrections"])
        self.assertNotIn("overflows 0", d["block corrections"])

    def test_slope_none_is_not_fitted_and_slope_zero_is_a_reading(self):
        unfitted = as_dict(metrics.collect(pair(), MAN, None, None))
        self.assertIn("not fitted", unfitted["fill slope"])
        flat = as_dict(metrics.collect(pair(), MAN, {"x": []}, 0.0,
                                       probe_reader=fake_probes({})))
        self.assertIn("+0.000 B/s", flat["fill slope"])
        self.assertNotIn("not fitted", flat["fill slope"])

    def test_slope_is_converted_with_the_manifest_byte_rate(self):
        d = as_dict(metrics.collect(pair(), MAN, {"x": []}, 6.77,
                                    probe_reader=fake_probes({})))
        self.assertIn("+6.770 B/s", d["fill slope"])
        self.assertIn("+4.80 ppm", d["fill slope"])


class TestMetricsAreUnjudged(unittest.TestCase):
    def test_exit_code_never_sees_them(self):
        """Structural, not conventional: exit_code takes verdicts only, so a
        metric cannot fail a run even by accident."""
        rows = metrics.collect(pair(class_req_bitmap=0, host_active=1), MAN)
        self.assertEqual(report.exit_code([Verdict("R1", PASS, "ok")]), 0)
        out = report.render({}, [Verdict("R1", PASS, "ok")], rows)
        self.assertIn("no class request", out)

    def test_rendered_below_the_rules_and_above_the_summary(self):
        rows = [("packets", "120000")]
        out = report.render({}, [Verdict("R1", PASS, "the rule line")], rows)
        lines = out.splitlines()
        rule_at = next(i for i, l in enumerate(lines) if "the rule line" in l)
        metric_at = next(i for i, l in enumerate(lines) if "120000" in l)
        summary_at = next(i for i, l in enumerate(lines) if "PASS," in l)
        self.assertLess(rule_at, metric_at)
        self.assertLess(metric_at, summary_at)

    def test_the_section_is_labelled_as_carrying_no_verdict(self):
        out = report.render({}, [], [("packets", "1")])
        self.assertIn("metrics", out)
        self.assertIn("no verdict", out)

    def test_both_label_and_value_reach_the_page(self):
        out = report.render({}, [], [("feedback polls", "7500 (62.5/s)")])
        self.assertIn("feedback polls", out)
        self.assertIn("7500 (62.5/s)", out)

    def test_no_metrics_section_when_there_are_none(self):
        self.assertNotIn("metrics", report.render({}, [Verdict("R1", PASS, "ok")]))


if __name__ == "__main__":
    unittest.main()
