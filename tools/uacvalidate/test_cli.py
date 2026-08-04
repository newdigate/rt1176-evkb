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
from wireformat import Block, synth_vcd, MAGIC, VERSION


MANIFEST = {"audio_class": 2, "speed": "HS", "channels": 8, "subslot_bytes": 4,
            "sample_rate_hz": 44100, "mode": "passive", "host_note": "synthetic",
            "max_packet_size_bytes": 1536}

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


def add_missing_marks(path, n=3):
    """Splice xscope lost-sample markers into a synthesised capture.

    Spliced rather than generated: synth_vcd models a healthy device, and
    teaching it to fake an xscope failure would be inventing a second,
    unverifiable emitter.
    """
    with open(path) as f:
        head, body = _split(f.read())
    head.insert(-1, "$var wire 1 MD Missing_Data $end")
    last = max(body)
    for i in range(n):
        body.setdefault(last + 1 + i, []).append("1MD")
    out = list(head)
    for t in sorted(body):
        out.append(f"#{t}")
        out.extend(body[t])
    with open(path, "w") as f:
        f.write("\n".join(out) + "\n")
    return path


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
        return add_missing_marks(self.vcd(healthy_pair()), n)

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

    def test_wrong_version_invalidates_before_any_rule(self):
        """The more dangerous of the two wire-format checks, because a
        revised block still parses. Same magic, renumbered words: every rule
        would read the wrong counter and report a confident verdict about the
        wrong quantity."""
        vcd = self.vcd(healthy_pair(version=99))
        rc, doc = self.run_json(vcd)
        self.assertEqual(rc, 2)
        self.assert_only_capture_verdict(doc)
        self.assertIn("99", doc["verdicts"][0]["summary"])
        self.assertIn(str(VERSION), doc["verdicts"][0]["summary"])

    def test_wrong_version_text_report_names_no_rule(self):
        rc, out, _ = self.run_cli(self.vcd(healthy_pair(version=99)))
        self.assertEqual(rc, 2)
        for rid in self.RULE_IDS:
            self.assertNotIn(f"] {rid} ", out)

    def test_manifest_contradicting_the_trace_invalidates(self):
        """The spec's second invalidating condition. A 12 B declared frame
        does not divide the 1408/1440 B packets on the wire, and the device --
        which knows the real frame size -- counted no non-multiple packets, so
        it is the declaration that is wrong.

        Left to the rules this is a cited FAIL: R4a compares 1440 B against a
        wMaxPacketSize the operator derived from the same wrong channel count.
        Accusing a conformant host of a spec violation because of an operator
        typo is the worst output this tool can produce."""
        self.write_manifest(channels=3, max_packet_size_bytes=540)
        rc, doc = self.run_json(self.vcd(healthy_pair()))
        self.assertEqual(rc, 2)
        self.assert_only_capture_verdict(doc)
        summary = doc["verdicts"][0]["summary"]
        self.assertIn("12 B", summary)      # what the manifest declared
        self.assertIn("32 B", summary)      # what the trace implies
        self.assertIn("1408", summary)      # the sizes that do not divide

    def test_the_operator_typo_that_started_this_is_invalid_not_a_fail(self):
        """`channels: 2` on an 8-channel stream -- the case the review cited.

        Divisibility cannot see it: 8 B divides 1408 B cleanly. The scale test
        can, because the manifest PREDICTS a mean packet of 352.8 B and the
        capture shows 1411.2 B. Comparing a declared prediction against an
        observation is not inferring the format from the trace; it is the
        entire reason the manifest exists.

        Before this it exited 1 -- 'this host failed a rule' -- for an
        operator typo."""
        self.write_manifest(channels=2, max_packet_size_bytes=1536)
        rc, doc = self.run_json(self.vcd(healthy_pair()))
        self.assertEqual(rc, 2)
        self.assert_only_capture_verdict(doc)
        summary = doc["verdicts"][0]["summary"]
        self.assertIn("352.8 B", summary)    # predicted
        self.assertIn("1411.2 B", summary)   # observed
        self.assertIn("4.00x", summary)      # and the ratio, spelled out

    def test_scale_catches_a_wrong_subslot_width(self):
        """16-bit subslots declared for a 32-bit stream. Divisibility misses
        this too -- 16 divides 1408 -- and it is the same class of error."""
        self.write_manifest(subslot_bytes=2, max_packet_size_bytes=1536)
        rc, _ = self.run_json(self.vcd(healthy_pair()))
        self.assertEqual(rc, 2)

    def test_scale_catches_a_wrong_sample_rate(self):
        self.write_manifest(sample_rate_hz=22050)
        rc, _ = self.run_json(self.vcd(healthy_pair()))
        self.assertEqual(rc, 2)

    def test_a_lumpy_host_is_warned_about_not_invalidated(self):
        """The counterexample that decides the FORM of the scale test.

        40/48-frame sizing against a 44/45 nominal puts every observed size
        outside a one-frame band of the declared floor/ceiling pair, so a test
        asking "does any single size land near the prediction" reports this
        capture as unreadable. But this host is doing exactly what W2 exists
        to describe -- permitted by the spec, costly, not a violation -- and
        calling it INVALID would delete W2's reason to exist.

        Its MEAN is 1408 B against a predicted 1411.2 B. The average is the
        quantity the stream's own arithmetic pins; the individual sizes are
        not."""
        lumpy = healthy_pair(size_hist_size=[40 * 32, 48 * 32, 0, 0, 0, 0, 0, 0],
                             size_hist_count=[60000, 60000, 0, 0, 0, 0, 0, 0])
        rc, doc = self.run_json(self.vcd(lumpy))
        self.assertEqual(rc, 0)
        levels = {v["rule_id"]: v["level"] for v in doc["verdicts"]}
        self.assertEqual(levels["W2"], "WARN")
        self.assertEqual(levels["R4a"], "PASS")

    def test_scale_is_not_gated_on_the_device_frame_count(self):
        """Divisibility defers to the device's own non-multiple count; scale
        must not. The two tests answer different questions -- 'does the
        declared frame divide these packets' versus 'is the declared stream
        the same size as this one' -- and the device's frame accounting speaks
        only to the first.

        Here the host genuinely sent 17 non-multiple packets AND the manifest
        is 4x wrong. R4b's FAIL is real but unreportable: it was computed
        against a format declaration the capture contradicts, so the run is
        INVALID and the operator fixes the manifest before anyone accuses this
        host of anything."""
        self.write_manifest(channels=2, max_packet_size_bytes=1536)
        rc, doc = self.run_json(self.vcd(healthy_pair(pkt_not_multiple=17)))
        self.assertEqual(rc, 2)
        self.assert_only_capture_verdict(doc)

    def test_scale_is_not_judged_on_a_partial_histogram(self):
        """An overflowed histogram means the recorded packets are not the
        whole population, so their mean is not the stream's mean. Accusing the
        manifest on the strength of a partial sample would be the same
        absence-of-evidence error R4a already refuses."""
        blocks = healthy_pair(size_hist_overflow=9000,
                              size_hist_size=[352, 0, 0, 0, 0, 0, 0, 0],
                              size_hist_count=[100, 0, 0, 0, 0, 0, 0, 0])
        rc, doc = self.run_json(self.vcd(blocks))
        self.assertNotEqual(rc, 2)
        self.assertNotIn("CAPTURE", [v["rule_id"] for v in doc["verdicts"]])

    def test_scale_tolerance_is_about_one_frame(self):
        """Inside one frame passes, outside it does not. Pinned because the
        tolerance is the whole difference between catching a 4x typo and
        invalidating every capture with ordinary packet-size wander."""
        # Both sizes are whole 32 B frames, so only the scale test is in play:
        # 1440 B is 28.8 B from the 1411.2 B prediction, 1472 B is 60.8 B.
        near = healthy_pair(size_hist_size=[1440, 0, 0, 0, 0, 0, 0, 0],
                            size_hist_count=[120000, 0, 0, 0, 0, 0, 0, 0])
        self.assertEqual(self.run_json(self.vcd(near, "near.vcd"))[0], 0)
        far = healthy_pair(size_hist_size=[1472, 0, 0, 0, 0, 0, 0, 0],
                           size_hist_count=[120000, 0, 0, 0, 0, 0, 0, 0])
        self.assertEqual(self.run_json(self.vcd(far, "far.vcd"))[0], 2)

    def test_a_consistent_manifest_is_not_flagged(self):
        """The check must not fire on the healthy capture, or it invalidates
        every run. A rejection that rejects everything passes the test above
        while making the tool useless."""
        rc, doc = self.run_json(self.vcd(healthy_pair()))
        self.assertEqual(rc, 0)
        self.assertNotIn("CAPTURE", [v["rule_id"] for v in doc["verdicts"]])

    def test_the_device_own_non_multiple_count_is_r4b_not_a_contradiction(self):
        """When the DEVICE also counted non-multiple packets, the host really
        did send them and R4b owns the finding. Swallowing that as a manifest
        contradiction would convert a genuine host defect into 'your manifest
        is wrong' and let the host off.

        The manifest here is CORRECT -- 8 channels, right scale -- and the
        packets are one byte off a whole frame. An earlier version of this
        fixture used channels=3, which is also a scale error, so the scale
        test fired and the case it meant to cover was never exercised."""
        odd = healthy_pair(pkt_not_multiple=17,
                           size_hist_size=[1409, 1441, 0, 0, 0, 0, 0, 0],
                           size_hist_count=[108000, 12000, 0, 0, 0, 0, 0, 0])
        rc, doc = self.run_json(self.vcd(odd))
        self.assertEqual(rc, 1)
        levels = {v["rule_id"]: v["level"] for v in doc["verdicts"]}
        self.assertEqual(levels["R4b"], "FAIL")

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


class TestInvalidationsPreemptTheRules(CliCase):
    """That no rule RUNS, asserted directly rather than inferred from output.

    The output-based tests above cannot see the difference between "the
    invalidation ran first" and "the rules ran and their verdicts were
    thrown away", because both print the same page. Moving the manifest
    contradiction check to after the rule sweep survived the entire suite for
    exactly that reason.

    The distinction is not cosmetic. Rules run on a capture already known to
    be untrustworthy: on contradictory data a rule can raise, and an exception
    escaping main() exits 1 -- which is this tool saying the host failed. It
    also wastes the guarantee the spec asks for, that an invalidated report
    contains no reasoning about the host at all.
    """

    def setUp(self):
        super().setUp()
        self.ran = []
        import rules as rules_mod
        from verdict import Verdict, PASS

        def spy(rule_id):
            def f(*a, **kw):
                self.ran.append(rule_id)
                return Verdict(rule_id, PASS, "stub")
            return f

        saved = (rules_mod.ALL_RULES, rules_mod.r1_feedback_polled,
                 rules_mod.w1_residual_drift, rules_mod.w3_feedback_tracked)
        rules_mod.ALL_RULES = [spy("R2"), spy("R3"), spy("R4a"), spy("R4b"),
                               spy("R7"), spy("W2")]
        rules_mod.r1_feedback_polled = spy("R1")
        rules_mod.w1_residual_drift = spy("W1")
        rules_mod.w3_feedback_tracked = spy("W3")

        def restore():
            (rules_mod.ALL_RULES, rules_mod.r1_feedback_polled,
             rules_mod.w1_residual_drift, rules_mod.w3_feedback_tracked) = saved
        self.addCleanup(restore)

    def test_the_spy_actually_fires_on_a_healthy_capture(self):
        """Guards the three tests below from passing vacuously: if the patch
        did not take, "no rule ran" would be true of every run."""
        self.run_json(self.vcd(healthy_pair()))
        self.assertEqual(sorted(self.ran),
                         ["R1", "R2", "R3", "R4a", "R4b", "R7", "W1", "W2", "W3"])

    def test_no_rule_runs_on_a_wrong_magic(self):
        self.run_json(self.vcd(healthy_pair(magic=0xDEADBEEF)))
        self.assertEqual(self.ran, [])

    def test_no_rule_runs_on_a_wrong_version(self):
        self.run_json(self.vcd(healthy_pair(version=99)))
        self.assertEqual(self.ran, [])

    def test_no_rule_runs_when_xscope_dropped_data(self):
        self.run_json(add_missing_marks(self.vcd(healthy_pair()), 3))
        self.assertEqual(self.ran, [])

    def test_no_rule_runs_on_a_contradicting_manifest(self):
        self.write_manifest(channels=3, max_packet_size_bytes=540)
        self.run_json(self.vcd(healthy_pair()))
        self.assertEqual(self.ran, [])


class TestMetricsReachTheReport(CliCase):
    def test_metrics_are_printed_and_do_not_move_the_exit_code(self):
        rc, out, _ = self.run_cli(
            self.vcd(healthy_pair(class_req_bitmap=0, host_active=1)))
        self.assertEqual(rc, 0)
        self.assertIn("metrics", out)
        self.assertIn("no class request", out)

    def test_metrics_appear_in_json(self):
        _, doc = self.run_json(self.vcd(healthy_pair()))
        labels = [k for k, _ in doc["metrics"]]
        self.assertIn("packets", labels)
        self.assertIn("class requests", labels)

    def test_probe_derived_metrics_come_from_the_capture(self):
        vcd = self.vcd_with_fill(healthy_pair(), 6.77)
        _, doc = self.run_json(vcd)
        d = dict((k, v) for k, v in doc["metrics"])
        self.assertIn("min 2000 B", d["OUT FIFO fill"])
        self.assertIn("+4.80 ppm", d["fill slope"])

    def test_no_metrics_on_an_invalidated_capture(self):
        """An INVALID capture's counters are the reason it was rejected.
        Printing metrics derived from them under the rejection would offer the
        reader exactly the numbers the verdict just said not to trust."""
        rc, out, _ = self.run_cli(self.vcd(healthy_pair(magic=0xDEADBEEF)))
        self.assertEqual(rc, 2)
        self.assertNotIn("metrics", out)


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
