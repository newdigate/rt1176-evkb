"""End-to-end tests for the CLI: wiring, ordering, and the two invalidations.

The plan gave cli.py no test file. It is the only module where the judge's
central policy is actually enforced rather than merely available -- report.py
knows an INVALID means 2, but it is cli.py that decides an INVALID is reached
*before* any rule runs. Deleting either invalidation leaves every other suite
in this package green while the tool starts issuing host verdicts computed
from counters it knows are lower bounds.

Fixtures are built with wireformat.synth_vcd, so nothing here needs hardware.
Captures that also carry a fill probe are assembled by merging an
`out_fifo_fill` signal into a synthesised capture, which is the shape a real
xrun capture has: the four decoupler probes alongside the 37 state words.
"""
import io
import json
import os
import tempfile
import unittest
from contextlib import redirect_stdout, redirect_stderr

import cli
from wireformat import Block, synth_vcd, MAGIC


MANIFEST = {"audio_class": 2, "speed": "HS", "channels": 8, "subslot_bytes": 4,
            "sample_rate_hz": 44100, "mode": "passive", "host_note": "synthetic"}

HEALTHY = dict(
    pkt_count=120000, or_acc=0xFFFFFF00, and_acc=0, nonsilent_frames=5292000,
    fb_poll_count=7500, alt_out=1, class_req_bitmap=5,
    size_hist_size=[44 * 32, 45 * 32, 0, 0, 0, 0, 0, 0],
    size_hist_count=[108000, 12000, 0, 0, 0, 0, 0, 0])

TICK_S = 1e-6   # synth_vcd's default timescale_ps=1000000 -> 1 us per tick


def healthy_pair(**over):
    """A 120 s capture: an idle opening block and a healthy closing one."""
    return [(0.0, Block(alt_out=1)), (120.0, Block(**dict(HEALTHY, **over)))]


def _split(text):
    """VCD text -> (header lines, {tick: [data lines]}) preserving order."""
    lines = text.split("\n")
    cut = lines.index("$enddefinitions $end") + 1
    head, body, t = lines[:cut], {}, None
    for ln in lines[cut:]:
        if not ln:
            continue
        if ln[0] == "#":
            t = int(ln[1:])
            body.setdefault(t, [])
        else:
            body[t].append(ln)
    return head, body


class CliCase(unittest.TestCase):
    def setUp(self):
        self.dir = tempfile.TemporaryDirectory()
        self.addCleanup(self.dir.cleanup)
        self.man = os.path.join(self.dir.name, "man.json")
        self.write_manifest()

    def write_manifest(self, **over):
        with open(self.man, "w") as f:
            json.dump(dict(MANIFEST, **over), f)

    def vcd(self, timed_blocks, name="cap.vcd"):
        p = os.path.join(self.dir.name, name)
        synth_vcd(p, timed_blocks)
        return p

    def vcd_with_fill(self, timed_blocks, slope_bytes_per_s,
                      t0=10.0, t1=70.0, hz=100.0, base=2000, name="fill.vcd"):
        """A capture carrying both the state block and an out_fifo_fill probe.

        The fill signal is declared by NAME, which is how vcdfill resolves it.
        Its VCD identifier is deliberately not "2": in a synthesised capture
        "2" is chr(50), state word 17, and a resolver keying on the id would
        fit a drift slope to pat_first_expected.
        """
        p = self.vcd(timed_blocks, name)
        with open(p) as f:
            head, body = _split(f.read())
        head.insert(-1, "$var wire 32 ZZ out_fifo_fill $end")
        n = int((t1 - t0) * hz)
        for i in range(n + 1):
            t = t0 + i / hz
            v = int(round(base + slope_bytes_per_s * (t - t0)))
            body.setdefault(int(round(t / TICK_S)), []).append(f"b{v:b} ZZ")
        out = list(head)
        for t in sorted(body):
            out.append(f"#{t}")
            out.extend(body[t])
        with open(p, "w") as f:
            f.write("\n".join(out) + "\n")
        return p

    def run_cli(self, vcd, *extra):
        buf, err = io.StringIO(), io.StringIO()
        with redirect_stdout(buf), redirect_stderr(err):
            rc = cli.main([vcd, self.man] + list(extra))
        return rc, buf.getvalue(), err.getvalue()

    def run_json(self, vcd, *extra):
        rc, out, _ = self.run_cli(vcd, "--json", *extra)
        return rc, json.loads(out)


class TestHealthyCapture(CliCase):
    def test_exit_zero_and_every_rule_reported(self):
        rc, doc = self.run_json(self.vcd(healthy_pair()))
        self.assertEqual(rc, 0)
        ids = [v["rule_id"] for v in doc["verdicts"]]
        self.assertEqual(sorted(ids),
                         sorted(["R1", "R2", "R3", "R4a", "R4b", "R7", "W1",
                                 "W2", "W3"]))

    def test_r1_is_first(self):
        """R1 leads because whether the host reads feedback at all frames how
        W1 and W3 should be read: a +5 ppm residual means one thing when the
        servo is running and another when nobody is polling. Appending it
        instead of inserting it would put that context after the verdicts it
        conditions."""
        _, doc = self.run_json(self.vcd(healthy_pair()))
        self.assertEqual(doc["verdicts"][0]["rule_id"], "R1")

    def test_r1_appears_exactly_once(self):
        """It is invoked directly, not via ALL_RULES. Adding it to ALL_RULES
        as well would run it twice with different arguments and report two
        verdicts for one rule."""
        _, doc = self.run_json(self.vcd(healthy_pair()))
        self.assertEqual([v["rule_id"] for v in doc["verdicts"]].count("R1"), 1)

    def test_drift_rules_come_last(self):
        _, doc = self.run_json(self.vcd(healthy_pair()))
        self.assertEqual([v["rule_id"] for v in doc["verdicts"]][-2:],
                         ["W1", "W3"])

    def test_header_reports_the_capture_span_and_observer(self):
        _, doc = self.run_json(self.vcd(healthy_pair()), "--observer-sha", "deadbee")
        self.assertAlmostEqual(doc["header"]["duration_s"], 120.0, places=3)
        self.assertEqual(doc["header"]["observer_sha"], "deadbee")
        self.assertEqual(doc["header"]["missing_marks"], 0)

    def test_text_report_is_the_default(self):
        rc, out, _ = self.run_cli(self.vcd(healthy_pair()))
        self.assertEqual(rc, 0)
        self.assertIn("UAC host conformance report", out)
        self.assertIn("6 PASS, 0 FAIL, 3 SKIP", out)


class TestExitCodes(CliCase):
    def test_one_when_the_host_fails_a_rule(self):
        vcd = self.vcd(healthy_pair(pkt_not_multiple=17))
        rc, doc = self.run_json(vcd)
        self.assertEqual(rc, 1)
        levels = {v["rule_id"]: v["level"] for v in doc["verdicts"]}
        self.assertEqual(levels["R4b"], "FAIL")

    def test_zero_when_only_warnings(self):
        """A host that never polls feedback earns a WARN, not a FAIL: no
        clause obliges it to. The run must still exit 0."""
        vcd = self.vcd(healthy_pair(fb_poll_count=0))
        rc, doc = self.run_json(vcd)
        self.assertEqual(rc, 0)
        levels = {v["rule_id"]: v["level"] for v in doc["verdicts"]}
        self.assertEqual(levels["R1"], "WARN")


class TestCaptureInvalidations(CliCase):
    """Both invalidations must pre-empt the rules entirely.

    Not "the rules ran but we exited 2 anyway": a report that lists R2 PASS
    beside a note that xscope dropped data invites the reader to believe the
    PASS. The rule verdicts must be absent.
    """

    RULE_IDS = ("R1", "R2", "R3", "R4a", "R4b", "R7", "W1", "W2", "W3")

    def assert_only_capture_verdict(self, doc):
        ids = [v["rule_id"] for v in doc["verdicts"]]
        self.assertEqual(ids, ["CAPTURE"])
        self.assertEqual(doc["verdicts"][0]["level"], "INVALID")

    def with_missing_marks(self, n=3):
        p = self.vcd(healthy_pair())
        with open(p) as f:
            head, body = _split(f.read())
        head.insert(-1, "$var wire 1 MD Missing_Data $end")
        last = max(body)
        for i in range(n):
            body.setdefault(last + 1 + i, []).append("1MD")
        out = list(head)
        for t in sorted(body):
            out.append(f"#{t}")
            out.extend(body[t])
        with open(p, "w") as f:
            f.write("\n".join(out) + "\n")
        return p

    def test_missing_marks_invalidate_before_any_rule(self):
        rc, doc = self.run_json(self.with_missing_marks(3))
        self.assertEqual(rc, 2)
        self.assert_only_capture_verdict(doc)
        self.assertIn("3 missing marks", doc["verdicts"][0]["summary"])
        self.assertEqual(doc["header"]["missing_marks"], 3)

    def test_missing_marks_text_report_names_no_rule(self):
        rc, out, _ = self.run_cli(self.with_missing_marks(1))
        self.assertEqual(rc, 2)
        for rid in self.RULE_IDS:
            self.assertNotIn(f"] {rid} ", out)

    def test_wrong_magic_invalidates_before_any_rule(self):
        vcd = self.vcd(healthy_pair(magic=0xDEADBEEF))
        rc, doc = self.run_json(vcd)
        self.assertEqual(rc, 2)
        self.assert_only_capture_verdict(doc)
        self.assertIn("DEADBEEF", doc["verdicts"][0]["summary"])
        self.assertIn(f"{MAGIC:08X}", doc["verdicts"][0]["summary"])

    def test_wrong_magic_text_report_names_no_rule(self):
        rc, out, _ = self.run_cli(self.vcd(healthy_pair(magic=0xDEADBEEF)))
        self.assertEqual(rc, 2)
        for rid in self.RULE_IDS:
            self.assertNotIn(f"] {rid} ", out)

    def test_empty_capture_is_invalid_not_a_crash(self):
        p = os.path.join(self.dir.name, "empty.vcd")
        synth_vcd(p, healthy_pair())
        with open(p) as f:
            head, _ = _split(f.read())
        with open(p, "w") as f:
            f.write("\n".join(head) + "\n")
        rc, doc = self.run_json(p)
        self.assertEqual(rc, 2)
        self.assert_only_capture_verdict(doc)

    def test_unreadable_capture_is_invalid_not_a_traceback(self):
        """A truncated capture makes read_blocks raise. Letting that escape
        would exit 1 through Python's own error path, and a gate reading exit
        1 records "this host FAILED" for a capture nobody could read."""
        p = os.path.join(self.dir.name, "trunc.vcd")
        synth_vcd(p, healthy_pair())
        with open(p) as f:
            text = f.read()
        # Drop one state word's declaration and data: the capture no longer
        # describes this wire format at all.
        keep = [l for l in text.split("\n") if "uacv_w05" not in l]
        with open(p, "w") as f:
            f.write("\n".join(keep) + "\n")
        rc, doc = self.run_json(p)
        self.assertEqual(rc, 2)
        self.assert_only_capture_verdict(doc)

    def test_missing_file_is_invalid_not_a_traceback(self):
        rc, doc = self.run_json(os.path.join(self.dir.name, "nope.vcd"))
        self.assertEqual(rc, 2)
        self.assert_only_capture_verdict(doc)

    def test_unusable_manifest_exits_two(self):
        self.write_manifest(mode="magic")
        rc, out, err = self.run_cli(self.vcd(healthy_pair()))
        self.assertEqual(rc, 2)
        self.assertIn("manifest", err)


class TestFillProbeWiring(CliCase):
    """The slope has to reach three separate rules, with the right arguments.

    Every one of these fails if cli.py hands the drift rules None -- which is
    what a synthetic capture without a fill probe produces, and therefore what
    a broken wiring looks like indistinguishably in the smoke test.
    """

    # 8ch x 4 B x 44100 = 1411200 B/s, so 6.77 B/s is +4.80 ppm and the 1024 B
    # correction quantum is consumed every 1024/6.77 = 151 s.
    SLOPE = 6.77

    def test_w1_receives_the_fitted_slope(self):
        vcd = self.vcd_with_fill(healthy_pair(), self.SLOPE)
        rc, doc = self.run_json(vcd)
        self.assertEqual(rc, 0)
        w1 = [v for v in doc["verdicts"] if v["rule_id"] == "W1"][0]
        self.assertEqual(w1["level"], "WARN")
        self.assertAlmostEqual(w1["evidence"]["residual_ppm"], 4.80, places=1)
        self.assertIn("151 s", w1["consequence"])

    def test_w3_receives_the_fitted_slope(self):
        vcd = self.vcd_with_fill(healthy_pair(), self.SLOPE)
        _, doc = self.run_json(vcd)
        w3 = [v for v in doc["verdicts"] if v["rule_id"] == "W3"][0]
        self.assertEqual(w3["level"], "WARN")
        self.assertAlmostEqual(w3["evidence"]["residual_ppm"], 4.80, places=1)

    def test_r1_receives_slope_and_quantum_in_that_order(self):
        """r1_feedback_polled(blocks, man, correction_quantum, slope). Swapping
        the last two arguments still produces a plausible WARN sentence, so the
        test pins both derived numbers: +4.80 ppm from the slope and 151 s from
        quantum/slope. Transposed, they come out as +0.73 ppm and 0 s."""
        vcd = self.vcd_with_fill(healthy_pair(fb_poll_count=0), self.SLOPE)
        _, doc = self.run_json(vcd)
        r1 = doc["verdicts"][0]
        self.assertEqual(r1["rule_id"], "R1")
        self.assertEqual(r1["level"], "WARN")
        self.assertIn("+4.80 ppm", r1["consequence"])
        self.assertIn("151 s", r1["consequence"])

    def test_correction_quantum_flag_changes_the_cadence(self):
        vcd = self.vcd_with_fill(healthy_pair(), self.SLOPE)
        _, doc = self.run_json(vcd, "--correction-quantum", "2048")
        w1 = [v for v in doc["verdicts"] if v["rule_id"] == "W1"][0]
        self.assertEqual(w1["evidence"]["correction_quantum_bytes"], 2048)
        self.assertIn("303 s", w1["consequence"])

    def test_a_flat_fill_trace_passes_rather_than_skipping(self):
        """0.0 B/s is a reading, not an absence. It must reach W1 as a PASS at
        the noise floor; a fitter returning None for it would report the
        best possible result as an un-run check."""
        vcd = self.vcd_with_fill(healthy_pair(), 0.0)
        _, doc = self.run_json(vcd)
        w1 = [v for v in doc["verdicts"] if v["rule_id"] == "W1"][0]
        self.assertEqual(w1["level"], "PASS")
        self.assertEqual(w1["evidence"]["fill_slope_bytes_per_s"], 0.0)

    def test_a_malformed_probe_section_skips_rather_than_crashing(self):
        """The decoupler probes are optional cargo alongside the state block.
        A capture whose probe section is malformed -- here, values written to
        an identifier that was never declared -- is still judgeable on its
        counters, so the drift rules SKIP and the rest of the report stands.
        Letting the parse error escape would exit 1, and exit 1 means the host
        failed a rule."""
        p = self.vcd(healthy_pair())
        with open(p, "a") as f:
            f.write("#200000000\nb101 UNDECLARED\n")
        rc, doc = self.run_json(p)
        self.assertEqual(rc, 0)
        w1 = [v for v in doc["verdicts"] if v["rule_id"] == "W1"][0]
        self.assertEqual(w1["level"], "SKIP")

    def test_no_fill_probe_skips_rather_than_reading_zero(self):
        _, doc = self.run_json(self.vcd(healthy_pair()))
        for rid in ("W1", "W3"):
            v = [x for x in doc["verdicts"] if x["rule_id"] == rid][0]
            self.assertEqual(v["level"], "SKIP")
            self.assertIsNone(v["evidence"]["fill_slope_bytes_per_s"])


if __name__ == "__main__":
    unittest.main()
