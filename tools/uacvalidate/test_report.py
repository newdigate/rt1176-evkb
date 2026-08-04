"""Tests for report rendering and the exit-code policy.

The exit-code tests carry more weight than they look like they do. The split
between 1 and 2 is the tool's whole posture: 1 accuses the host, 2 says the
capture is unusable and declines to accuse anyone. A mutation that collapses
2 into 1 turns every dropped-xscope-sample run into a false accusation, and
nothing downstream would notice -- so the tests that pin the split are here,
including the case where an INVALID and a FAIL appear together.

The rendering tests pin that the supporting field travels with the verdict.
A FAIL whose citation is dropped on the way to the page is, to the reader, an
uncited FAIL -- verdict.py's constructor discipline is only worth having if
the renderer does not undo it.
"""
import unittest

from verdict import Verdict, PASS, FAIL, WARN, SKIP, INVALID
import report


HEADER = {"vcd": "/tmp/x.vcd", "duration_s": 120.0, "missing_marks": 0,
          "observer_sha": "abc1234", "host_note": "RT1176 master"}


class TestExitCode(unittest.TestCase):
    def test_zero_when_all_pass(self):
        vs = [Verdict("R1", PASS, "ok"), Verdict("R2", PASS, "ok")]
        self.assertEqual(report.exit_code(vs), 0)

    def test_nonzero_on_any_fail(self):
        vs = [Verdict("R1", PASS, "ok"),
              Verdict("R2", FAIL, "bad", citation="clause")]
        self.assertEqual(report.exit_code(vs), 1)

    def test_zero_on_warn_only(self):
        """A WARN is spec-permitted behaviour with a cost attached, not a
        violation. Failing the run on one would make the tool unusable against
        every real host, and would blur the line the citation discipline
        exists to keep sharp."""
        vs = [Verdict("W1", WARN, "meh", consequence="every 100 s")]
        self.assertEqual(report.exit_code(vs), 0)

    def test_zero_on_skip_only(self):
        vs = [Verdict("R7", SKIP, "n/a", missing_witness="pattern")]
        self.assertEqual(report.exit_code(vs), 0)

    def test_two_on_invalid(self):
        vs = [Verdict("CAPTURE", INVALID, "xscope dropped data")]
        self.assertEqual(report.exit_code(vs), 2)

    def test_invalid_outranks_fail(self):
        """If the capture is unusable, whatever the rules said about the host
        was computed from lower-bound counters. Reporting 1 here would convert
        an unreadable capture into an accusation -- the exact false accusation
        the two-code split exists to prevent. Order of the two checks in
        exit_code is therefore load-bearing, not stylistic."""
        vs = [Verdict("R2", FAIL, "bad", citation="clause"),
              Verdict("CAPTURE", INVALID, "xscope dropped data")]
        self.assertEqual(report.exit_code(vs), 2)

    def test_warn_and_skip_alongside_a_pass_stay_clean(self):
        vs = [Verdict("R1", PASS, "ok"),
              Verdict("W1", WARN, "meh", consequence="x"),
              Verdict("R7", SKIP, "n/a", missing_witness="w")]
        self.assertEqual(report.exit_code(vs), 0)


class TestFormat(unittest.TestCase):
    def test_includes_citation_for_fail(self):
        vs = [Verdict("R3", FAIL, "right-justified", citation="Formats 2.0 2.3.1")]
        out = report.render(HEADER, vs)
        self.assertIn("R3", out)
        self.assertIn("Formats 2.0 2.3.1", out)

    def test_includes_consequence_for_warn(self):
        vs = [Verdict("W1", WARN, "+4.8 ppm",
                      consequence="block correction every 2700 s")]
        self.assertIn("every 2700 s", report.render(HEADER, vs))

    def test_includes_missing_witness_for_skip(self):
        vs = [Verdict("R3", SKIP, "silent", missing_witness="sufficient signal")]
        self.assertIn("sufficient signal", report.render(HEADER, vs))

    def test_includes_the_level_and_the_summary_text(self):
        """Checked against the per-verdict line rather than against the whole
        page, because the trailing count line also contains the word FAIL --
        a renderer that dropped every finding and printed only the tally would
        satisfy a bare `assertIn(FAIL, out)`."""
        vs = [Verdict("R3", FAIL, "right-justified", citation="c")]
        line = [l for l in report.render(HEADER, vs).splitlines()
                if "right-justified" in l]
        self.assertEqual(len(line), 1)
        self.assertIn(FAIL, line[0])
        self.assertIn("R3", line[0])

    def test_includes_evidence(self):
        """Evidence is what lets a reader re-derive the verdict. A report that
        states a conclusion and withholds the numbers behind it cannot be
        checked, which for a tool whose output is an accusation is not an
        acceptable failure mode."""
        vs = [Verdict("R4a", PASS, "ok", evidence={"ceiling_bytes": 1440})]
        out = report.render(HEADER, vs)
        self.assertIn("ceiling_bytes", out)
        self.assertIn("1440", out)

    def test_header_carries_observer_sha(self):
        """Which build of the observer produced the capture. Without it a
        report cannot be tied back to the firmware that generated it, and a
        rerun after an observer change is indistinguishable from a rerun
        against a changed host."""
        out = report.render(HEADER, [Verdict("R1", PASS, "ok")])
        self.assertIn("abc1234", out)

    def test_header_carries_host_capture_and_duration(self):
        out = report.render(HEADER, [])
        self.assertIn("RT1176 master", out)
        self.assertIn("/tmp/x.vcd", out)
        self.assertIn("120.0", out)

    def test_header_carries_the_missing_mark_count(self):
        """Matched on its own line, not anywhere on the page: `assertIn("7")`
        against the whole report passes on the "7" in "RT1176", which is how
        this test was vacuous when first written -- deleting the line entirely
        left it green. The count is the reader's only warning that every
        counter below it is a lower bound, so it has to be pinned properly."""
        out = report.render(dict(HEADER, missing_marks=7), [])
        line = [l for l in out.splitlines() if "xscope missing" in l]
        self.assertEqual(len(line), 1)
        self.assertIn("7", line[0])

    def test_header_with_missing_keys_still_renders(self):
        """render() is called on the INVALID paths too, where the header has
        only been half-filled. Raising there would replace the reason the
        capture was rejected with a traceback."""
        out = report.render({}, [Verdict("CAPTURE", INVALID, "no blocks")])
        self.assertIn("no blocks", out)

    def test_summary_counts_each_level(self):
        vs = [Verdict("R1", PASS, "ok"),
              Verdict("R2", FAIL, "bad", citation="c"),
              Verdict("W1", WARN, "meh", consequence="x"),
              Verdict("R7", SKIP, "n/a", missing_witness="w")]
        out = report.render(HEADER, vs)
        self.assertIn("1 PASS", out)
        self.assertIn("1 FAIL", out)
        self.assertIn("1 WARN", out)
        self.assertIn("1 SKIP", out)

    def test_summary_states_zero_pass_and_zero_fail_rather_than_omitting_them(self):
        """'0 FAIL' is a claim; an absent FAIL count is an ambiguity. A reader
        skimming for the word FAIL must be able to tell "none" from "the line
        does not mention it"."""
        out = report.render(HEADER, [Verdict("W1", WARN, "meh", consequence="x")])
        self.assertIn("0 PASS", out)
        self.assertIn("0 FAIL", out)

    def test_summary_reports_invalid_count(self):
        out = report.render(HEADER, [Verdict("CAPTURE", INVALID, "dropped")])
        self.assertIn("1 INVALID", out)

    def test_every_verdict_appears(self):
        vs = [Verdict(f"R{i}", PASS, f"summary {i}") for i in range(6)]
        out = report.render(HEADER, vs)
        for i in range(6):
            self.assertIn(f"summary {i}", out)


if __name__ == "__main__":
    unittest.main()
