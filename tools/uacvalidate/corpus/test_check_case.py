import unittest

from check_case import check


CASE = {"id": "X", "expect": {"R2": "FAIL", "R3": "PASS"},
        "expect_evidence": {"R2": {"packets_while_alt0": 10000}}}


def rpt(verdicts, evidence=None):
    return {"verdicts": [
        {"rule_id": rid, "level": lvl,
         "evidence": (evidence or {}).get(rid, {})}
        for rid, lvl in verdicts.items()]}


class TestCheck(unittest.TestCase):
    def test_passes_when_levels_match(self):
        ok, msgs = check(CASE, rpt({"R2": "FAIL", "R3": "PASS"},
                                   {"R2": {"packets_while_alt0": 10000}}))
        self.assertTrue(ok, msgs)

    def test_fails_when_a_level_differs(self):
        ok, msgs = check(CASE, rpt({"R2": "PASS", "R3": "PASS"}))
        self.assertFalse(ok)
        self.assertIn("R2", " ".join(msgs))

    def test_fails_when_expected_rule_absent(self):
        ok, msgs = check(CASE, rpt({"R3": "PASS"}))
        self.assertFalse(ok)
        self.assertIn("R2", " ".join(msgs))

    def test_fails_when_evidence_differs(self):
        ok, msgs = check(CASE, rpt({"R2": "FAIL", "R3": "PASS"},
                                   {"R2": {"packets_while_alt0": 0}}))
        self.assertFalse(ok)
        self.assertIn("packets_while_alt0", " ".join(msgs))

    def test_ignores_rules_not_mentioned(self):
        ok, _ = check(CASE, rpt({"R2": "FAIL", "R3": "PASS", "R7": "SKIP"},
                                {"R2": {"packets_while_alt0": 10000}}))
        self.assertTrue(ok)

    # --- the checker's own failure modes -------------------------------
    #
    # A checker that reports OK for the wrong reason is worse than no
    # checker: the corpus's entire job is to be the thing that notices.

    def test_a_capture_level_invalidation_is_not_a_pass(self):
        """A report can be INVALID (exit 2) with no rule verdicts at all --
        an unreadable capture, a magic mismatch. Every expected rule is then
        absent, so the case must MISMATCH rather than silently agree."""
        ok, msgs = check(CASE, {"verdicts": [
            {"rule_id": "CAPTURE", "level": "INVALID", "evidence": {}}]})
        self.assertFalse(ok)
        self.assertIn("did not run", " ".join(msgs))

    def test_empty_expectations_do_not_vacuously_pass_a_broken_report(self):
        """A case with no expectations passes anything -- that is by design
        for `expect_evidence`-only cases, but it means a MALFORMED case
        (expect accidentally emptied) would go green against any report.
        Pin the behaviour so the danger is visible rather than surprising."""
        ok, _ = check({"id": "empty", "expect": {}}, rpt({"R2": "PASS"}))
        self.assertTrue(ok)

    def test_evidence_zero_is_compared_not_treated_as_absent(self):
        """feedback_polls == 0 is the WHOLE POINT of cases A and B: the host
        never polled. A truthiness test instead of a presence test would
        make the corpus's most important evidence check unfireable."""
        case = {"id": "Z", "expect": {},
                "expect_evidence": {"R1": {"feedback_polls": 0}}}
        ok, _ = check(case, rpt({"R1": "WARN"}, {"R1": {"feedback_polls": 0}}))
        self.assertTrue(ok)
        ok, msgs = check(case, rpt({"R1": "PASS"}, {"R1": {"feedback_polls": 3}}))
        self.assertFalse(ok)
        self.assertIn("feedback_polls", " ".join(msgs))

    def test_all_mismatches_are_reported_not_just_the_first(self):
        """Bench iterations are expensive: a checker that stops at the first
        mismatch costs a whole re-run to discover the second."""
        ok, msgs = check(CASE, rpt({"R2": "PASS", "R3": "FAIL"},
                                   {"R2": {"packets_while_alt0": 0}}))
        self.assertFalse(ok)
        joined = " ".join(msgs)
        self.assertIn("R2:", joined)
        self.assertIn("R3:", joined)
        self.assertIn("packets_while_alt0", joined)


if __name__ == "__main__":
    unittest.main()
