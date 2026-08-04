"""Tests for the constructor's discipline, and for nothing else.

The plan gave verdict.py no test file, on the grounds that it is a data type
with no behaviour exercised by every rule test in Task 3. The first half is
true; the second is not, and the difference matters. Every rule constructs its
verdicts *correctly* -- passing a citation with each FAIL, a consequence with
each WARN, a witness with each SKIP -- so the rule tests confirm the rules
behave, never that the type would stop one that did not. Deleting all four
checks from __post_init__ leaves those suites green.

That leaves the judge's single most important design decision as the only
unexercised code in it. So this file covers the raising paths, which no other
suite can reach, and deliberately not the field plumbing, which they all do.
"""
import unittest

from verdict import Verdict, PASS, FAIL, WARN, SKIP, INVALID, LEVELS


class TestVerdictEnforcesDiscipline(unittest.TestCase):
    def test_fail_without_citation_raises(self):
        with self.assertRaises(ValueError) as cm:
            Verdict("R3", FAIL, "right-justified")
        self.assertIn("citation", str(cm.exception))

    def test_warn_without_consequence_raises(self):
        with self.assertRaises(ValueError) as cm:
            Verdict("W1", WARN, "host never polled feedback")
        self.assertIn("consequence", str(cm.exception))

    def test_skip_without_missing_witness_raises(self):
        with self.assertRaises(ValueError) as cm:
            Verdict("R7", SKIP, "could not run")
        self.assertIn("witness", str(cm.exception))

    def test_unknown_level_raises(self):
        with self.assertRaises(ValueError):
            Verdict("R1", "PROBABLY", "hedging")

    def test_message_names_the_rule(self):
        """A raise from deep in a rule sweep is useless without the rule id."""
        with self.assertRaises(ValueError) as cm:
            Verdict("R4b", FAIL, "x")
        self.assertIn("R4b", str(cm.exception))


class TestVerdictAcceptsWellFormed(unittest.TestCase):
    """The enforcement must not be so eager that a conformant rule cannot
    construct -- a check that rejects everything also passes every test above."""

    def test_pass_needs_no_supporting_field(self):
        self.assertEqual(Verdict("R1", PASS, "ok").level, PASS)

    def test_invalid_needs_no_supporting_field(self):
        """INVALID describes the capture, not the host; there is no clause to
        cite and no witness that would have helped."""
        self.assertEqual(Verdict("CAPTURE", INVALID, "xscope dropped data").level,
                         INVALID)

    def test_fail_with_citation_constructs(self):
        v = Verdict("R3", FAIL, "right-justified", citation="Formats 2.0 2.3.1")
        self.assertEqual(v.citation, "Formats 2.0 2.3.1")

    def test_warn_with_consequence_constructs(self):
        v = Verdict("W1", WARN, "+4.8 ppm", consequence="one slip per 100 s")
        self.assertEqual(v.consequence, "one slip per 100 s")

    def test_skip_with_witness_constructs(self):
        v = Verdict("R3", SKIP, "silent", missing_witness="sufficient signal")
        self.assertEqual(v.missing_witness, "sufficient signal")

    def test_evidence_is_not_shared_between_verdicts(self):
        """A dict default would be shared by every verdict in the report, so
        one rule's evidence would appear under every other rule's finding."""
        a, b = Verdict("R1", PASS, "ok"), Verdict("R2", PASS, "ok")
        a.evidence["pkt_count"] = 1
        self.assertEqual(b.evidence, {})

    def test_every_declared_level_is_constructible(self):
        """LEVELS is what report.py iterates; a level listed there but rejected
        by the constructor would be a section that can never be populated."""
        extra = {FAIL: dict(citation="c"), WARN: dict(consequence="x"),
                 SKIP: dict(missing_witness="w")}
        for lvl in LEVELS:
            with self.subTest(level=lvl):
                self.assertEqual(Verdict("R0", lvl, "s", **extra.get(lvl, {})).level,
                                 lvl)


if __name__ == "__main__":
    unittest.main()
