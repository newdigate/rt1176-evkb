# UAC Host Validator — Plan 2: The Judge

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Turn a captured VCD plus a run manifest into a conformance report with PASS / FAIL / WARN / SKIP verdicts, cited or consequence-backed.

**Architecture:** Pure Python, stdlib only. Reads `Block` objects through Plan 1's `trace.py`, so nothing here depends on which probe mechanism the observer uses. Rules are independent functions with a uniform signature, registered in a list — adding a rule never edits an existing one. No RT1176 knowledge anywhere: input is a VCD and a manifest, output is a report.

**Tech Stack:** Python 3, stdlib only (`dataclasses`, `unittest`, `json`, `argparse`).

**Depends on:** Plan 1 Task 3 (`trace.py` with `Block`, `synth_vcd`, `read_blocks`). Hardware is **not** required — every test here runs against synthetic VCDs.

**Design spec:** `docs/superpowers/specs/2026-08-04-uac-host-validator-design.md`

---

## File Structure

| File | Responsibility |
|---|---|
| `tools/uacvalidate/manifest.py` | The operator's declaration of the run. Parsing, validation, derived quantities. |
| `tools/uacvalidate/verdict.py` | `Verdict` type and the four levels. Shared vocabulary; no logic. |
| `tools/uacvalidate/rules.py` | One function per rule. Registered in `ALL_RULES`. |
| `tools/uacvalidate/report.py` | Formatting and exit-code policy. |
| `tools/uacvalidate/cli.py` | Argument parsing, wiring, `main()`. |
| `tools/uacvalidate/test_manifest.py` | Manifest tests. |
| `tools/uacvalidate/test_rules.py` | One PASS, one FAIL and one SKIP fixture per rule. |
| `tools/uacvalidate/test_report.py` | Formatting and exit-code tests. |

---

## Task 1: The run manifest

Expectations come from the operator, never from the trace. Inference would let a mis-set expectation silently reinterpret the data — the failure that produced the 0.222-slope probe.

**Files:**
- Create: `tools/uacvalidate/manifest.py`
- Test: `tools/uacvalidate/test_manifest.py`

- [ ] **Step 1: Write the failing test**

Create `tools/uacvalidate/test_manifest.py`:

```python
import unittest

from manifest import Manifest, ManifestError


VALID = {
    "audio_class": 2,
    "speed": "HS",
    "channels": 8,
    "subslot_bytes": 4,
    "sample_rate_hz": 44100,
    "mode": "passive",
    "host_note": "RT1176 EVKB, master",
}


class TestManifest(unittest.TestCase):
    def test_parses_valid(self):
        m = Manifest.from_dict(VALID)
        self.assertEqual(m.audio_class, 2)
        self.assertEqual(m.channels, 8)
        self.assertEqual(m.mode, "passive")

    def test_frame_bytes_is_channels_times_subslot(self):
        self.assertEqual(Manifest.from_dict(VALID).frame_bytes, 32)

    def test_packets_per_second_hs_is_1000(self):
        self.assertEqual(Manifest.from_dict(VALID).packets_per_second, 1000)

    def test_packets_per_second_fs_is_1000(self):
        d = dict(VALID, audio_class=1, speed="FS", channels=2, subslot_bytes=2)
        self.assertEqual(Manifest.from_dict(d).packets_per_second, 1000)

    def test_nominal_frames_per_packet_is_fractional(self):
        m = Manifest.from_dict(VALID)
        self.assertAlmostEqual(m.nominal_frames_per_packet, 44.1, places=6)

    def test_legal_packet_sizes_are_floor_and_ceil_frames(self):
        m = Manifest.from_dict(VALID)
        self.assertEqual(m.legal_packet_sizes, {44 * 32, 45 * 32})

    def test_nominal_bytes_per_second(self):
        m = Manifest.from_dict(VALID)
        self.assertAlmostEqual(m.nominal_bytes_per_second, 44100 * 32, places=3)

    def test_rejects_unknown_mode(self):
        with self.assertRaises(ManifestError):
            Manifest.from_dict(dict(VALID, mode="magic"))

    def test_rejects_missing_field(self):
        d = dict(VALID)
        del d["channels"]
        with self.assertRaises(ManifestError):
            Manifest.from_dict(d)

    def test_rejects_class_1_at_high_speed(self):
        with self.assertRaises(ManifestError):
            Manifest.from_dict(dict(VALID, audio_class=1, speed="HS"))


if __name__ == "__main__":
    unittest.main()
```

- [ ] **Step 2: Run it to verify it fails**

```bash
cd ~/Development/rt1170/evkb/tools/uacvalidate && python3 -m unittest test_manifest -v
```

Expected: FAIL — `ModuleNotFoundError: No module named 'manifest'`.

- [ ] **Step 3: Write the implementation**

Create `tools/uacvalidate/manifest.py`:

```python
"""The operator's declaration of a validation run.

Every expectation the judge applies comes from here, never from the trace.
Inferring the format from the data would let a mis-set expectation silently
reinterpret it, which is exactly how a broken probe once produced a plausible
but meaningless drift slope.
"""
import json
import math
from dataclasses import dataclass

REQUIRED = ("audio_class", "speed", "channels", "subslot_bytes",
            "sample_rate_hz", "mode", "host_note")
MODES = ("passive", "cooperative")
SPEEDS = ("FS", "HS")


class ManifestError(Exception):
    pass


@dataclass(frozen=True)
class Manifest:
    audio_class: int
    speed: str
    channels: int
    subslot_bytes: int
    sample_rate_hz: int
    mode: str
    host_note: str

    @classmethod
    def from_dict(cls, d):
        missing = [k for k in REQUIRED if k not in d]
        if missing:
            raise ManifestError(f"manifest missing required fields: {missing}")
        if d["mode"] not in MODES:
            raise ManifestError(f"mode must be one of {MODES}, got {d['mode']!r}")
        if d["speed"] not in SPEEDS:
            raise ManifestError(f"speed must be one of {SPEEDS}, got {d['speed']!r}")
        if d["audio_class"] not in (1, 2):
            raise ManifestError(f"audio_class must be 1 or 2, got {d['audio_class']}")
        if d["audio_class"] == 1 and d["speed"] == "HS":
            raise ManifestError(
                "audio_class 1 at HS: the UAC1 image on this bench is full speed. "
                "A manifest that says otherwise would misread every packet size.")
        for k in ("channels", "subslot_bytes", "sample_rate_hz"):
            if not isinstance(d[k], int) or d[k] <= 0:
                raise ManifestError(f"{k} must be a positive integer, got {d[k]!r}")
        return cls(**{k: d[k] for k in REQUIRED})

    @classmethod
    def load(cls, path):
        with open(path) as f:
            return cls.from_dict(json.load(f))

    @property
    def frame_bytes(self):
        return self.channels * self.subslot_bytes

    @property
    def packets_per_second(self):
        """OUT data packets per second.

        One per USB frame at full speed. At high speed lib_xua's iso OUT
        endpoint is serviced once per frame as well on this device, not once
        per microframe -- the transcript's sustained pkts/s=1000 at HS is the
        measurement behind this.
        """
        return 1000

    @property
    def nominal_frames_per_packet(self):
        return self.sample_rate_hz / self.packets_per_second

    @property
    def legal_packet_sizes(self):
        """Sizes a conformant host may send: floor and ceiling of the
        fractional frames-per-packet, in bytes."""
        n = self.nominal_frames_per_packet
        lo, hi = math.floor(n), math.ceil(n)
        return {lo * self.frame_bytes, hi * self.frame_bytes}

    @property
    def nominal_bytes_per_second(self):
        return self.sample_rate_hz * self.frame_bytes
```

- [ ] **Step 4: Run tests to verify they pass**

```bash
cd ~/Development/rt1170/evkb/tools/uacvalidate && python3 -m unittest test_manifest -v
```

Expected: 9 tests, all PASS.

- [ ] **Step 5: Commit**

```bash
cd ~/Development/rt1170/evkb
git add tools/uacvalidate/manifest.py tools/uacvalidate/test_manifest.py
git commit -m "uacvalidate: the run manifest

Expectations come from the operator, never from the trace. The class-1-at-HS
rejection is deliberate: that combination does not exist on this bench, and a
manifest asserting it would misread every packet size rather than fail."
```

---

## Task 2: The verdict vocabulary

**Files:**
- Create: `tools/uacvalidate/verdict.py`

- [ ] **Step 1: Write the implementation**

No test of its own — it is a data type with no behaviour, exercised by every rule test in Task 3. Create `tools/uacvalidate/verdict.py`:

```python
"""Shared verdict vocabulary.

Four levels, with a discipline attached to each:

  PASS  -- the check ran and the host satisfied it.
  FAIL  -- a violation. MUST carry a citation. Never invented from a
           threshold; if no clause can be cited, the finding is a WARN.
  WARN  -- behaviour the spec permits but which has a measurable cost. MUST
           carry consequence arithmetic, not an opinion.
  SKIP  -- the check could not run. MUST name the missing witness. A SKIP is
           never a quiet PASS; this is the single most likely way the tool
           lies.
"""
from dataclasses import dataclass, field

PASS = "PASS"
FAIL = "FAIL"
WARN = "WARN"
SKIP = "SKIP"
INVALID = "INVALID"

LEVELS = (PASS, FAIL, WARN, SKIP, INVALID)


@dataclass
class Verdict:
    rule_id: str
    level: str
    summary: str
    citation: str = ""
    consequence: str = ""
    missing_witness: str = ""
    evidence: dict = field(default_factory=dict)

    def __post_init__(self):
        if self.level not in LEVELS:
            raise ValueError(f"unknown level {self.level!r}")
        if self.level == FAIL and not self.citation:
            raise ValueError(f"{self.rule_id}: FAIL without a citation")
        if self.level == WARN and not self.consequence:
            raise ValueError(f"{self.rule_id}: WARN without consequence arithmetic")
        if self.level == SKIP and not self.missing_witness:
            raise ValueError(f"{self.rule_id}: SKIP without naming the missing witness")
```

The constructor enforces the discipline. A rule that tries to FAIL without a clause raises rather than emitting an uncited verdict.

- [ ] **Step 2: Verify it enforces**

```bash
cd ~/Development/rt1170/evkb/tools/uacvalidate && python3 -c "
from verdict import Verdict, FAIL, WARN, SKIP
for kwargs in [dict(level=FAIL, summary='x'), dict(level=WARN, summary='x'), dict(level=SKIP, summary='x')]:
    try:
        Verdict(rule_id='R0', **kwargs); print('NOT ENFORCED', kwargs['level'])
    except ValueError as e:
        print('enforced:', e)
"
```

Expected: three `enforced:` lines.

- [ ] **Step 3: Commit**

```bash
cd ~/Development/rt1170/evkb
git add tools/uacvalidate/verdict.py
git commit -m "uacvalidate: verdict vocabulary with the discipline enforced

FAIL without a citation, WARN without consequence arithmetic, and SKIP
without a named missing witness all raise. The rules cannot quietly drift
into uncited assertions, because the type will not let them."
```

---

## Task 3: The rules

Each rule is a function taking `(blocks, manifest)` and returning a `Verdict`. `blocks` is the list of `(time, Block)` pairs from `trace.read_blocks`.

**Files:**
- Create: `tools/uacvalidate/rules.py`
- Test: `tools/uacvalidate/test_rules.py`

- [ ] **Step 1: Write the failing tests**

Create `tools/uacvalidate/test_rules.py`:

```python
import unittest

from trace import Block
from manifest import Manifest
from verdict import PASS, FAIL, WARN, SKIP
import rules


MAN = Manifest.from_dict({
    "audio_class": 2, "speed": "HS", "channels": 8, "subslot_bytes": 4,
    "sample_rate_hz": 44100, "mode": "passive", "host_note": "test",
})
MAN_COOP = Manifest.from_dict({
    "audio_class": 2, "speed": "HS", "channels": 8, "subslot_bytes": 4,
    "sample_rate_hz": 44100, "mode": "cooperative", "host_note": "test",
})


def series(first, last, duration=120.0):
    """Two blocks, start and end, which is all any counter rule needs."""
    return [(0.0, first), (duration, last)]


def healthy(**over):
    """A block from a conformant host after 120 s of 8ch 24-in-4 at 44.1 kHz."""
    base = dict(
        pkt_count=120000, pkt_short_discarded=0, pkt_not_multiple=0,
        or_acc=0xFFFFFF00, and_acc=0x00000000, nonsilent_frames=5292000,
        fb_poll_count=7500, fb_value=0x000B0000, alt_out=1, alt_transitions=1,
        class_req_bitmap=0x5, host_active=1,
        size_hist_size=[44 * 32, 45 * 32, 0, 0, 0, 0, 0, 0],
        size_hist_count=[108000, 12000, 0, 0, 0, 0, 0, 0],
    )
    base.update(over)
    return Block(**base)


class TestR1FeedbackPolled(unittest.TestCase):
    def test_pass_when_polled(self):
        v = rules.r1_feedback_polled(series(Block(alt_out=1), healthy()), MAN)
        self.assertEqual(v.level, PASS)

    def test_warn_not_fail_when_never_polled(self):
        """USB 2.0 5.12.4.2 obliges the SINK to provide feedback; it only says
        this 'allows' the host to adjust. No clause requires a host to read an
        asynchronous sink's feedback endpoint, so ignoring it is conformant
        and ruinous -- a WARN carrying consequence arithmetic, not a FAIL."""
        v = rules.r1_feedback_polled(
            series(Block(alt_out=1), healthy(fb_poll_count=0)), MAN)
        self.assertEqual(v.level, WARN)
        self.assertFalse(v.citation)
        self.assertTrue(v.consequence)

    def test_skip_when_never_streamed(self):
        v = rules.r1_feedback_polled(
            series(Block(), Block(alt_out=0, fb_poll_count=0)), MAN)
        self.assertEqual(v.level, SKIP)
        self.assertIn("stream", v.missing_witness.lower())


class TestR2NoStreamingInAlt0(unittest.TestCase):
    def test_pass_when_packets_only_in_alt_1(self):
        v = rules.r2_no_streaming_in_alt0(series(Block(alt_out=1), healthy()), MAN)
        self.assertEqual(v.level, PASS)

    def test_fail_when_packets_arrive_in_alt_0(self):
        blocks = [(0.0, Block(alt_out=0, pkt_count=0)),
                  (10.0, Block(alt_out=0, pkt_count=10000))]
        v = rules.r2_no_streaming_in_alt0(blocks, MAN)
        self.assertEqual(v.level, FAIL)
        self.assertTrue(v.citation)

    def test_skip_when_no_packets_at_all(self):
        v = rules.r2_no_streaming_in_alt0(
            [(0.0, Block(alt_out=0)), (10.0, Block(alt_out=0))], MAN)
        self.assertEqual(v.level, SKIP)


class TestR3Justification(unittest.TestCase):
    def test_pass_when_left_justified(self):
        v = rules.r3_left_justified(series(Block(), healthy()), MAN)
        self.assertEqual(v.level, PASS)

    def test_fail_when_right_justified(self):
        v = rules.r3_left_justified(
            series(Block(), healthy(or_acc=0x00FFFFFF, and_acc=0x00000000)), MAN)
        self.assertEqual(v.level, FAIL)
        self.assertTrue(v.citation)

    def test_skip_on_silence_rather_than_pass(self):
        """The lie this tool is most likely to tell: a silent stream makes the
        justification test pass for the wrong reason."""
        v = rules.r3_left_justified(
            series(Block(), healthy(or_acc=0, and_acc=0, nonsilent_frames=0)), MAN)
        self.assertEqual(v.level, SKIP)
        self.assertIn("signal", v.missing_witness.lower())

    def test_skip_just_below_threshold(self):
        v = rules.r3_left_justified(
            series(Block(), healthy(nonsilent_frames=9999)), MAN)
        self.assertEqual(v.level, SKIP)

    def test_pass_at_threshold(self):
        v = rules.r3_left_justified(
            series(Block(), healthy(nonsilent_frames=10000)), MAN)
        self.assertEqual(v.level, PASS)

    def test_skip_when_subslot_is_two_bytes(self):
        """16-bit in a 2-byte subslot has no spare lane, so there is nothing
        to justify."""
        m = Manifest.from_dict({
            "audio_class": 1, "speed": "FS", "channels": 2, "subslot_bytes": 2,
            "sample_rate_hz": 44100, "mode": "passive", "host_note": "t"})
        v = rules.r3_left_justified(series(Block(), healthy()), m)
        self.assertEqual(v.level, SKIP)


class TestR4aMaxPacketSize(unittest.TestCase):
    def test_pass_within_ceiling(self):
        v = rules.r4a_max_packet_size(series(Block(), healthy()), MAN)
        self.assertEqual(v.level, PASS)

    def test_fail_when_a_packet_is_oversized(self):
        big = healthy(size_hist_size=[44 * 32, 45 * 32, 9999, 0, 0, 0, 0, 0],
                      size_hist_count=[100000, 12000, 3, 0, 0, 0, 0, 0])
        v = rules.r4a_max_packet_size(series(Block(), big), MAN)
        self.assertEqual(v.level, FAIL)
        self.assertTrue(v.citation)

    def test_skip_when_no_packets(self):
        v = rules.r4a_max_packet_size(series(Block(), Block()), MAN)
        self.assertEqual(v.level, SKIP)


class TestR4bWholeFrames(unittest.TestCase):
    def test_pass_when_all_whole(self):
        v = rules.r4b_whole_frames(series(Block(), healthy()), MAN)
        self.assertEqual(v.level, PASS)

    def test_fail_on_non_multiple(self):
        v = rules.r4b_whole_frames(
            series(Block(), healthy(pkt_not_multiple=17)), MAN)
        self.assertEqual(v.level, FAIL)

    def test_fail_on_short_discarded(self):
        v = rules.r4b_whole_frames(
            series(Block(), healthy(pkt_short_discarded=4)), MAN)
        self.assertEqual(v.level, FAIL)

    def test_skip_when_no_packets(self):
        v = rules.r4b_whole_frames(series(Block(), Block()), MAN)
        self.assertEqual(v.level, SKIP)


class TestR7Continuity(unittest.TestCase):
    def test_skip_in_passive_mode(self):
        v = rules.r7_sample_continuity(series(Block(), healthy()), MAN)
        self.assertEqual(v.level, SKIP)
        self.assertIn("passive", v.missing_witness.lower())

    def test_pass_in_cooperative_with_no_errors(self):
        v = rules.r7_sample_continuity(series(Block(), healthy()), MAN_COOP)
        self.assertEqual(v.level, PASS)

    def test_fail_on_pattern_errors(self):
        v = rules.r7_sample_continuity(
            series(Block(), healthy(pat_err_count=9, pat_first_err_idx=41207)), MAN_COOP)
        self.assertEqual(v.level, FAIL)
        self.assertIn("41207", v.summary)

    def test_never_synced_is_distinguished_from_diverged(self):
        v = rules.r7_sample_continuity(
            series(Block(), healthy(pat_err_count=0, pat_resync_count=0,
                                    nonsilent_frames=5292000, pat_first_err_idx=0,
                                    pat_first_expected=0, pat_first_actual=0,
                                    pkt_count=120000)), MAN_COOP)
        self.assertEqual(v.level, PASS)


class TestW2PacketSizeLumpiness(unittest.TestCase):
    def test_pass_on_floor_and_ceiling_only(self):
        v = rules.w2_packet_size_shape(series(Block(), healthy()), MAN)
        self.assertEqual(v.level, PASS)

    def test_warn_on_lumpy_sizes(self):
        lumpy = healthy(size_hist_size=[40 * 32, 48 * 32, 0, 0, 0, 0, 0, 0],
                        size_hist_count=[60000, 60000, 0, 0, 0, 0, 0, 0])
        v = rules.w2_packet_size_shape(series(Block(), lumpy), MAN)
        self.assertEqual(v.level, WARN)
        self.assertTrue(v.consequence)

    def test_skip_when_no_packets(self):
        v = rules.w2_packet_size_shape(series(Block(), Block()), MAN)
        self.assertEqual(v.level, SKIP)


class TestCounterWrap(unittest.TestCase):
    def test_delta_handles_uint32_wrap(self):
        self.assertEqual(rules.delta(0xFFFFFFF0, 0x10), 0x20)

    def test_delta_normal(self):
        self.assertEqual(rules.delta(100, 350), 250)


if __name__ == "__main__":
    unittest.main()
```

- [ ] **Step 2: Run to verify it fails**

```bash
cd ~/Development/rt1170/evkb/tools/uacvalidate && python3 -m unittest test_rules -v
```

Expected: FAIL — `ModuleNotFoundError: No module named 'rules'`.

- [ ] **Step 3: Write the implementation**

Create `tools/uacvalidate/rules.py`:

```python
"""Conformance rules.

Each rule takes (blocks, manifest) and returns exactly one Verdict. Rules are
independent: adding one appends to ALL_RULES and edits nothing.

Citation discipline: a FAIL cites a clause. Citations carrying [UNVERIFIED] or
[DOC MISSING] have not been read against the primary document -- usb_20.pdf
and frmts20.pdf are not on this machine. A validator that mis-cites is worse
than one that does not cite, so the marker travels with the text into the
report rather than being quietly dropped.
"""
from verdict import Verdict, PASS, FAIL, WARN, SKIP

UINT32 = 1 << 32

# Below this many non-silent frames the byte-lane accumulators have not seen
# enough signal to distinguish justification from silence. 10,000 frames is
# 0.23 s at 44.1 kHz -- deliberately conservative, because the cost of a false
# SKIP is one more run and the cost of a false PASS is a wrong verdict.
MIN_NONSILENT_FRAMES = 10000

# R1 has NO citation, deliberately. USB 2.0 5.12.4.2 reads: "An asynchronous
# sink must provide explicit feedback to the host [...] This allows the host to
# continuously adjust the number of samples sent to the sink". Every must/shall
# in 5.12.4 involving the host places the obligation on the DEVICE, and the one
# host-facing "must" concerns an adaptive source -- the IN direction. Nothing
# requires a host to read an asynchronous sink's feedback endpoint. A host that
# ignores it is conformant and ruinous, which is what WARN is for.
CITE_R2 = ("UAC 2.0 sections 3.16.2 and 4.9.1 [partial]; USB 2.0 section "
           "9.4.10 [UNVERIFIED]")
CITE_R3 = "USB Audio Data Formats 2.0 section 2.3.1 [DOC MISSING]"
CITE_R4A = "USB 2.0 section 5.6.3 [UNVERIFIED]"
CITE_R4B = "UAC 2.0 audio frame structure [UNVERIFIED]"
CITE_R7 = ("no clause in hand -- see the design spec; this citation is the "
           "weakest in the set and may demote R7 to WARN [UNVERIFIED]")


def delta(first, last):
    """Counter difference across a uint32 wrap.

    A sample counter at 8ch x 44.1 kHz wraps in about 3.4 hours, which is
    inside the soak durations this bench already runs.
    """
    return (last - first) % UINT32


def _span(blocks):
    return blocks[0][1], blocks[-1][1]


def _duration(blocks):
    return blocks[-1][0] - blocks[0][0]


def _packets(blocks):
    a, b = _span(blocks)
    return delta(a.pkt_count, b.pkt_count)


def _streamed(blocks):
    """Did any block observe an active output stream?"""
    return any(b.alt_out > 0 for _, b in blocks)


def r1_feedback_polled(blocks, man, correction_quantum_bytes=1024,
                       fill_slope_bytes_per_s=None):
    a, b = _span(blocks)
    polls = delta(a.fb_poll_count, b.fb_poll_count)
    ev = {"feedback_polls": polls, "duration_s": round(_duration(blocks), 2)}
    if not _streamed(blocks):
        return Verdict("R1", SKIP, "no output stream was ever active",
                       missing_witness="an active stream (alt > 0)", evidence=ev)
    if polls == 0:
        if fill_slope_bytes_per_s:
            ppm = slope_to_ppm(fill_slope_bytes_per_s, man)
            seconds = correction_quantum_bytes / abs(fill_slope_bytes_per_s)
            consequence = (
                f"the device is reporting its rate and no one is listening. "
                f"Open loop, the measured drift is {ppm:+.2f} ppm, so this "
                f"host forces a block correction -- silence insertion or "
                f"dropped packets, audible either way -- every "
                f"{seconds:.0f} s. The device is entitled to do this: the "
                f"spec obliges it to publish its rate, not the host to read "
                f"it.")
        else:
            consequence = (
                f"the device published its rate for {_duration(blocks):.0f} s "
                f"and the host never read it. Drift is unbounded and the "
                f"device will block-correct at whatever cadence its own "
                f"crystal offset dictates. No fill slope was available in "
                f"this capture to quantify the cadence.")
        return Verdict(
            "R1", WARN,
            f"host never polled the feedback endpoint in "
            f"{_duration(blocks):.1f} s of streaming",
            consequence=consequence, evidence=ev)
    return Verdict("R1", PASS,
                   f"feedback polled {polls} times "
                   f"({polls / max(_duration(blocks), 1e-9):.1f}/s)", evidence=ev)


def r2_no_streaming_in_alt0(blocks, man):
    in_alt0 = 0
    prev = None
    for _, b in blocks:
        if prev is not None and b.alt_out == 0:
            in_alt0 += delta(prev.pkt_count, b.pkt_count)
        prev = b
    ev = {"packets_while_alt0": in_alt0, "total_packets": _packets(blocks)}
    if _packets(blocks) == 0 and in_alt0 == 0:
        return Verdict("R2", SKIP, "no packets arrived at all",
                       missing_witness="any OUT packet", evidence=ev)
    if in_alt0 > 0:
        return Verdict(
            "R2", FAIL,
            f"{in_alt0} packets arrived while the OUT interface was in alt 0, "
            f"which reserves no bandwidth and exposes no endpoint",
            citation=CITE_R2, evidence=ev)
    return Verdict("R2", PASS, "no packets arrived during alt 0", evidence=ev)


def r3_left_justified(blocks, man):
    a, b = _span(blocks)
    frames = delta(a.nonsilent_frames, b.nonsilent_frames)
    ev = {"or_acc": f"0x{b.or_acc:08X}", "and_acc": f"0x{b.and_acc:08X}",
          "nonsilent_frames": frames}
    if man.subslot_bytes * 8 <= 16:
        return Verdict("R3", SKIP,
                       f"{man.subslot_bytes}-byte subslot has no spare lane",
                       missing_witness="a subslot wider than the sample",
                       evidence=ev)
    if frames < MIN_NONSILENT_FRAMES:
        return Verdict(
            "R3", SKIP,
            f"only {frames} non-silent frames "
            f"(need {MIN_NONSILENT_FRAMES}); a silent stream would pass this "
            f"check for the wrong reason",
            missing_witness="sufficient signal in the stream", evidence=ev)
    if (b.or_acc & 0xFF) == 0:
        return Verdict("R3", PASS,
                       "low byte never non-zero: samples are left-justified",
                       evidence=ev)
    if (b.or_acc >> 24) == (b.and_acc >> 24):
        return Verdict(
            "R3", FAIL,
            "top byte constant while the low byte carries data: samples are "
            "right-justified in the subslot",
            citation=CITE_R3, evidence=ev)
    return Verdict(
        "R3", FAIL,
        f"low byte of the subslot carries data (or_acc=0x{b.or_acc:08X}), so "
        f"samples are not left-justified",
        citation=CITE_R3, evidence=ev)


def r4a_max_packet_size(blocks, man):
    _, b = _span(blocks)
    sizes = b.sizes()
    ceiling = max(man.legal_packet_sizes)
    ev = {"sizes": sizes, "ceiling_bytes": ceiling,
          "hist_overflow": b.size_hist_overflow}
    if not sizes:
        return Verdict("R4a", SKIP, "no packets observed",
                       missing_witness="any OUT packet", evidence=ev)
    over = {s: c for s, c in sizes.items() if s > ceiling}
    if over:
        return Verdict(
            "R4a", FAIL,
            f"packet sizes exceed the endpoint's maximum of {ceiling} B: {over}",
            citation=CITE_R4A, evidence=ev)
    return Verdict("R4a", PASS,
                   f"all packets within {ceiling} B", evidence=ev)


def r4b_whole_frames(blocks, man):
    a, b = _span(blocks)
    not_mult = delta(a.pkt_not_multiple, b.pkt_not_multiple)
    short = delta(a.pkt_short_discarded, b.pkt_short_discarded)
    ev = {"not_multiple": not_mult, "short_discarded": short,
          "frame_bytes": man.frame_bytes, "total_packets": _packets(blocks)}
    if _packets(blocks) == 0 and not_mult == 0 and short == 0:
        return Verdict("R4b", SKIP, "no packets observed",
                       missing_witness="any OUT packet", evidence=ev)
    if not_mult or short:
        return Verdict(
            "R4b", FAIL,
            f"{not_mult} packets were not a whole number of "
            f"{man.frame_bytes} B audio frames, and {short} were shorter than "
            f"one frame and silently discarded by the device",
            citation=CITE_R4B, evidence=ev)
    return Verdict("R4b", PASS,
                   f"all packets are whole multiples of {man.frame_bytes} B",
                   evidence=ev)


def r7_sample_continuity(blocks, man):
    a, b = _span(blocks)
    errs = delta(a.pat_err_count, b.pat_err_count)
    resyncs = delta(a.pat_resync_count, b.pat_resync_count)
    ev = {"pattern_errors": errs, "resyncs": resyncs,
          "first_error_index": b.pat_first_err_idx,
          "first_expected": f"0x{b.pat_first_expected:08X}",
          "first_actual": f"0x{b.pat_first_actual:08X}"}
    if man.mode != "cooperative":
        return Verdict("R7", SKIP, "continuity needs a known test pattern",
                       missing_witness="passive mode: no test pattern sent",
                       evidence=ev)
    if _packets(blocks) == 0:
        return Verdict("R7", SKIP, "no packets observed",
                       missing_witness="any OUT packet", evidence=ev)
    if errs == 0 and resyncs == 0 and b.pat_first_err_idx == 0 \
            and delta(a.nonsilent_frames, b.nonsilent_frames) < MIN_NONSILENT_FRAMES:
        return Verdict(
            "R7", SKIP,
            "the pattern never synchronised: the host's playback path is not "
            "bit-exact (volume control, mixing or resampling), so continuity "
            "cannot be judged",
            missing_witness="a bit-exact playback path", evidence=ev)
    if errs:
        return Verdict(
            "R7", FAIL,
            f"{errs} sample discontinuities, first at sample index "
            f"{b.pat_first_err_idx} (expected 0x{b.pat_first_expected:08X}, "
            f"got 0x{b.pat_first_actual:08X}); {resyncs} resyncs",
            citation=CITE_R7, evidence=ev)
    return Verdict("R7", PASS, "sample stream continuous", evidence=ev)


def w2_packet_size_shape(blocks, man):
    _, b = _span(blocks)
    sizes = b.sizes()
    legal = man.legal_packet_sizes
    ev = {"sizes": sizes, "expected": sorted(legal),
          "hist_overflow": b.size_hist_overflow}
    if not sizes:
        return Verdict("W2", SKIP, "no packets observed",
                       missing_witness="any OUT packet", evidence=ev)
    unexpected = {s: c for s, c in sizes.items() if s not in legal}
    if unexpected or b.size_hist_overflow:
        worst = max(abs(s - man.nominal_frames_per_packet * man.frame_bytes)
                    for s in sizes)
        excursion_frames = worst / man.frame_bytes
        return Verdict(
            "W2", WARN,
            f"packet sizes outside the fractional-sample floor/ceiling "
            f"{sorted(legal)}: {unexpected}"
            + (f" plus {b.size_hist_overflow} packets in sizes beyond the "
               f"8 histogram slots" if b.size_hist_overflow else ""),
            consequence=(
                f"worst deviation is {excursion_frames:.1f} audio frames from "
                f"nominal, so the device's OUT FIFO must carry that much extra "
                f"headroom on top of drift wander. Permitted by the spec with "
                f"an asynchronous feedback loop, but it consumes margin that "
                f"exists to absorb rate error."),
            evidence=ev)
    return Verdict("W2", PASS,
                   f"packet sizes are the expected floor/ceiling pair "
                   f"{sorted(legal)}", evidence=ev)


# r1_feedback_polled is NOT here: it takes the fill slope so its consequence
# can quantify the glitch cadence, so the CLI calls it directly alongside W1
# and W3. Everything in ALL_RULES has the uniform (blocks, manifest) signature.
ALL_RULES = [
    r2_no_streaming_in_alt0,
    r3_left_justified,
    r4a_max_packet_size,
    r4b_whole_frames,
    r7_sample_continuity,
    w2_packet_size_shape,
]
```

- [ ] **Step 4: Run to verify they pass**

```bash
cd ~/Development/rt1170/evkb/tools/uacvalidate && python3 -m unittest test_rules -v
```

Expected: 26 tests, all PASS.

- [ ] **Step 5: Commit**

```bash
cd ~/Development/rt1170/evkb
git add tools/uacvalidate/rules.py tools/uacvalidate/test_rules.py
git commit -m "uacvalidate: conformance rules R1-R4b, R7, W2

Every rule has a PASS, a FAIL and a SKIP fixture. The SKIP fixtures are the
important ones -- test_skip_on_silence_rather_than_pass guards the specific
lie this tool is most likely to tell, where a silent stream makes the
justification check pass for the wrong reason and blesses a right-justifying
host.

Citations carry [UNVERIFIED]/[DOC MISSING] into the report rather than
dropping the marker, because usb_20.pdf and frmts20.pdf are not on this
machine and a validator that mis-cites is worse than one that does not."
```

---

## Task 4: Drift rules W1 and W3

Separated from Task 3 because they need the fill probe, not the state block — they read probes 0–3 through `vcdfill`'s existing parsing.

**Files:**
- Modify: `tools/uacvalidate/rules.py`
- Modify: `tools/uacvalidate/test_rules.py`

- [ ] **Step 1: Write the failing tests**

Append to `tools/uacvalidate/test_rules.py`, before the `if __name__` block:

```python
class TestW1ResidualDrift(unittest.TestCase):
    def test_pass_on_a_closed_loop(self):
        v = rules.w1_residual_drift(series(Block(), healthy()), MAN,
                                    fill_slope_bytes_per_s=0.220,
                                    correction_quantum_bytes=1024)
        self.assertEqual(v.level, PASS)

    def test_warn_on_a_biased_servo(self):
        """+4.8 ppm: the dither-chasing servo. 1411200 B/s * 4.8e-6 = 6.77 B/s."""
        v = rules.w1_residual_drift(series(Block(), healthy()), MAN,
                                    fill_slope_bytes_per_s=6.77,
                                    correction_quantum_bytes=1024)
        self.assertEqual(v.level, WARN)
        self.assertIn("ppm", v.consequence)
        self.assertIn("s", v.consequence)

    def test_skip_without_a_slope(self):
        v = rules.w1_residual_drift(series(Block(), healthy()), MAN,
                                    fill_slope_bytes_per_s=None,
                                    correction_quantum_bytes=1024)
        self.assertEqual(v.level, SKIP)

    def test_ppm_uses_the_stream_byte_rate_not_four_bytes_per_frame(self):
        ppm = rules.slope_to_ppm(6.77, MAN)
        self.assertAlmostEqual(ppm, 4.80, places=1)


class TestW3FeedbackTracking(unittest.TestCase):
    def test_pass_when_sizing_tracks_feedback(self):
        v = rules.w3_feedback_tracked(series(Block(), healthy()), MAN,
                                      fill_slope_bytes_per_s=0.220)
        self.assertEqual(v.level, PASS)

    def test_warn_when_host_polls_but_ignores(self):
        v = rules.w3_feedback_tracked(series(Block(), healthy()), MAN,
                                      fill_slope_bytes_per_s=118.3)
        self.assertEqual(v.level, WARN)

    def test_skip_when_feedback_never_polled(self):
        v = rules.w3_feedback_tracked(
            series(Block(), healthy(fb_poll_count=0)), MAN,
            fill_slope_bytes_per_s=118.3)
        self.assertEqual(v.level, SKIP)
```

- [ ] **Step 2: Run to verify it fails**

```bash
cd ~/Development/rt1170/evkb/tools/uacvalidate && python3 -m unittest test_rules -v 2>&1 | tail -5
```

Expected: FAIL — `module 'rules' has no attribute 'w1_residual_drift'`.

- [ ] **Step 3: Write the implementation**

Append to `tools/uacvalidate/rules.py`, before `ALL_RULES`:

```python
# A residual this small is indistinguishable from measurement noise over any
# practical soak: at 1.4 MB/s it is under 2 B/s, inside one packet of jitter.
DRIFT_NOISE_FLOOR_PPM = 1.0


def slope_to_ppm(slope_bytes_per_s, man):
    """Convert an OUT-FIFO fill slope to a rate error in ppm.

    vcdfill.py's printed ppm column assumes 4 bytes per frame. This does not:
    it uses the manifest's actual byte rate, which is why an 8ch 24-in-4
    stream does not need the 1.4112 correction factor applied by hand.
    """
    return slope_bytes_per_s / man.nominal_bytes_per_second * 1e6


def w1_residual_drift(blocks, man, fill_slope_bytes_per_s,
                      correction_quantum_bytes):
    ev = {"fill_slope_bytes_per_s": fill_slope_bytes_per_s,
          "correction_quantum_bytes": correction_quantum_bytes}
    if fill_slope_bytes_per_s is None:
        return Verdict("W1", SKIP, "no fill-probe slope available",
                       missing_witness="probe 2 (out_fifo_fill) in the capture",
                       evidence=ev)
    ppm = slope_to_ppm(fill_slope_bytes_per_s, man)
    ev["residual_ppm"] = round(ppm, 3)
    if abs(ppm) <= DRIFT_NOISE_FLOOR_PPM:
        return Verdict("W1", PASS,
                       f"residual rate error {ppm:+.3f} ppm, at the noise floor",
                       evidence=ev)
    seconds = correction_quantum_bytes / abs(fill_slope_bytes_per_s)
    ev["seconds_between_corrections"] = round(seconds, 1)
    return Verdict(
        "W1", WARN,
        f"residual rate error {ppm:+.2f} ppm after the feedback loop",
        consequence=(
            f"the device's correction quantum is {correction_quantum_bytes} B, "
            f"so at {fill_slope_bytes_per_s:+.3f} B/s this host will force a "
            f"block correction -- silence insertion or dropped packets, "
            f"audible either way -- every {seconds:.0f} s "
            f"({seconds / 60:.1f} min)."),
        evidence=ev)


def w3_feedback_tracked(blocks, man, fill_slope_bytes_per_s):
    a, b = _span(blocks)
    polls = delta(a.fb_poll_count, b.fb_poll_count)
    ev = {"feedback_polls": polls, "device_fb_value": f"0x{b.fb_value:08X}",
          "fill_slope_bytes_per_s": fill_slope_bytes_per_s}
    if polls == 0:
        return Verdict(
            "W3", SKIP,
            "host never polled feedback, so there is no tracking to assess "
            "(R1 covers this)",
            missing_witness="any feedback poll", evidence=ev)
    if fill_slope_bytes_per_s is None:
        return Verdict("W3", SKIP, "no fill-probe slope available",
                       missing_witness="probe 2 (out_fifo_fill) in the capture",
                       evidence=ev)
    ppm = slope_to_ppm(fill_slope_bytes_per_s, man)
    ev["residual_ppm"] = round(ppm, 3)
    if abs(ppm) <= DRIFT_NOISE_FLOOR_PPM:
        return Verdict("W3", PASS,
                       f"host tracks the device's feedback: residual "
                       f"{ppm:+.3f} ppm over {polls} polls", evidence=ev)
    return Verdict(
        "W3", WARN,
        f"host polled feedback {polls} times but still runs {ppm:+.2f} ppm off",
        consequence=(
            f"the device is reporting its rate and the host is reading it, yet "
            f"a {ppm:+.2f} ppm residual remains -- the servo is receiving the "
            f"information and not acting on it correctly. A servo that chases "
            f"the raw dithered report rather than its mean produces exactly "
            f"this signature."),
        evidence=ev)
```

W1 and W3 are **not** added to `ALL_RULES` — they take extra arguments and are invoked directly by the CLI in Task 6.

- [ ] **Step 4: Run to verify they pass**

```bash
cd ~/Development/rt1170/evkb/tools/uacvalidate && python3 -m unittest test_rules -v
```

Expected: 33 tests, all PASS.

- [ ] **Step 5: Commit**

```bash
cd ~/Development/rt1170/evkb
git add tools/uacvalidate/rules.py tools/uacvalidate/test_rules.py
git commit -m "uacvalidate: W1 residual drift and W3 feedback tracking

Both state a consequence rather than a threshold: the device's correction
quantum divided by the measured slope gives seconds-to-glitch, which is
arithmetic the locked-bias sweep already validated at slope 0.9909 across
six points.

slope_to_ppm uses the manifest's real byte rate rather than vcdfill.py's
assumed 4 bytes per frame, so an 8ch 24-in-4 stream no longer needs the
1.4112 correction applied by hand."
```

---

## Task 5: The report

**Files:**
- Create: `tools/uacvalidate/report.py`
- Test: `tools/uacvalidate/test_report.py`

- [ ] **Step 1: Write the failing test**

Create `tools/uacvalidate/test_report.py`:

```python
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
        vs = [Verdict("W1", WARN, "meh", consequence="every 100 s")]
        self.assertEqual(report.exit_code(vs), 0)

    def test_zero_on_skip_only(self):
        vs = [Verdict("R7", SKIP, "n/a", missing_witness="pattern")]
        self.assertEqual(report.exit_code(vs), 0)

    def test_two_on_invalid(self):
        vs = [Verdict("CAPTURE", INVALID, "xscope dropped data")]
        self.assertEqual(report.exit_code(vs), 2)


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

    def test_header_carries_observer_sha(self):
        out = report.render(HEADER, [Verdict("R1", PASS, "ok")])
        self.assertIn("abc1234", out)

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


if __name__ == "__main__":
    unittest.main()
```

- [ ] **Step 2: Run to verify it fails**

```bash
cd ~/Development/rt1170/evkb/tools/uacvalidate && python3 -m unittest test_report -v
```

Expected: FAIL — `ModuleNotFoundError: No module named 'report'`.

- [ ] **Step 3: Write the implementation**

Create `tools/uacvalidate/report.py`:

```python
"""Report rendering and exit-code policy.

Exit codes: 0 clean (warnings and skips do not fail a run), 1 at least one
FAIL, 2 the capture itself is unusable. Separating 2 from 1 matters: an
INVALID run says nothing about the host, and treating it as a failure of the
host would be a false accusation.
"""
from verdict import PASS, FAIL, WARN, SKIP, INVALID


def exit_code(verdicts):
    if any(v.level == INVALID for v in verdicts):
        return 2
    if any(v.level == FAIL for v in verdicts):
        return 1
    return 0


def render(header, verdicts):
    lines = []
    lines.append("UAC host conformance report")
    lines.append("=" * 60)
    lines.append(f"host under test : {header.get('host_note', '')}")
    lines.append(f"capture         : {header.get('vcd', '')}")
    lines.append(f"duration        : {header.get('duration_s', 0):.1f} s")
    lines.append(f"xscope missing  : {header.get('missing_marks', 0)}")
    lines.append(f"observer build  : {header.get('observer_sha', 'unknown')}")
    lines.append("")

    for v in verdicts:
        lines.append(f"[{v.level:<7}] {v.rule_id:<5} {v.summary}")
        if v.citation:
            lines.append(f"           cites: {v.citation}")
        if v.consequence:
            lines.append(f"           consequence: {v.consequence}")
        if v.missing_witness:
            lines.append(f"           missing: {v.missing_witness}")
        if v.evidence:
            ev = ", ".join(f"{k}={val}" for k, val in v.evidence.items())
            lines.append(f"           evidence: {ev}")
        lines.append("")

    counts = {lvl: sum(1 for v in verdicts if v.level == lvl)
              for lvl in (PASS, FAIL, WARN, SKIP, INVALID)}
    summary = ", ".join(f"{counts[l]} {l}" for l in (PASS, FAIL, WARN, SKIP)
                        if counts[l] or l in (PASS, FAIL))
    if counts[INVALID]:
        summary += f", {counts[INVALID]} INVALID"
    lines.append("-" * 60)
    lines.append(summary)
    return "\n".join(lines)
```

- [ ] **Step 4: Run to verify they pass**

```bash
cd ~/Development/rt1170/evkb/tools/uacvalidate && python3 -m unittest test_report -v
```

Expected: 10 tests, all PASS.

- [ ] **Step 5: Commit**

```bash
cd ~/Development/rt1170/evkb
git add tools/uacvalidate/report.py tools/uacvalidate/test_report.py
git commit -m "uacvalidate: report rendering and exit-code policy

Exit 2 for an unusable capture is kept distinct from exit 1 for a failing
host: an INVALID run says nothing about the host, and reporting it as a
host failure would be a false accusation."
```

---

## Task 6: The CLI

**Files:**
- Create: `tools/uacvalidate/cli.py`

- [ ] **Step 1: Write the implementation**

Create `tools/uacvalidate/cli.py`:

```python
#!/usr/bin/env python3
"""uacvalidate -- judge a USB host's UAC conformance from a device-side capture.

Usage:
    ./cli.py capture.vcd manifest.json [--json]

The device reduced; this judges. Nothing here knows anything about any
particular host -- input is a VCD and a manifest, output is a report.
"""
import argparse
import json
import sys

import rules
import report
from manifest import Manifest
from trace import read_blocks, count_missing_marks, MAGIC
from verdict import Verdict, INVALID
from vcdfill import parse as parse_fill, fit_slope

# lib_xua refills the OUT FIFO with OUT_BUFFER_PREFILL bytes after a dry-out,
# and drains to half on overflow. Either is one block correction. The value is
# the device's, not a tuning knob: it sets how much drift is absorbed before
# the glitch, and therefore the seconds-to-glitch arithmetic in W1.
DEFAULT_CORRECTION_QUANTUM_BYTES = 1024


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("vcd")
    ap.add_argument("manifest")
    ap.add_argument("--json", action="store_true", help="machine-readable output")
    ap.add_argument("--correction-quantum", type=int,
                    default=DEFAULT_CORRECTION_QUANTUM_BYTES,
                    help="device OUT_BUFFER_PREFILL in bytes")
    ap.add_argument("--observer-sha", default="unknown")
    args = ap.parse_args(argv)

    man = Manifest.load(args.manifest)
    blocks = read_blocks(args.vcd)

    header = {"vcd": args.vcd, "duration_s": 0.0, "missing_marks": 0,
              "observer_sha": args.observer_sha, "host_note": man.host_note}

    if not blocks:
        v = [Verdict("CAPTURE", INVALID,
                     "no validator state blocks found in the capture")]
        print(report.render(header, v))
        return report.exit_code(v)

    header["duration_s"] = blocks[-1][0] - blocks[0][0]

    # Probes 0-3 come through vcdfill: the fill trace is the drift instrument.
    # Missing marks are counted from the VCD directly rather than from
    # vcdfill's return value, which is keyed by VCD identifier and does not
    # surface the Missing_Data signal by name.
    fill = parse_fill(args.vcd)
    missing = count_missing_marks(args.vcd)
    header["missing_marks"] = missing

    verdicts = []

    if missing:
        verdicts.append(Verdict(
            "CAPTURE", INVALID,
            f"xscope dropped data ({missing} missing marks): every counter in "
            f"this report is a lower bound, so no verdict can be trusted"))
        print(report.render(header, verdicts))
        return report.exit_code(verdicts)

    _, last = blocks[-1]
    if last.magic != MAGIC:
        verdicts.append(Verdict(
            "CAPTURE", INVALID,
            f"state block magic is 0x{last.magic:08X}, expected 0x{MAGIC:08X}: "
            f"the observer and this judge disagree about the wire format"))
        print(report.render(header, verdicts))
        return report.exit_code(verdicts)

    for rule in rules.ALL_RULES:
        verdicts.append(rule(blocks, man))

    slope = fit_slope(fill) if fill else None
    # R1 leads: whether the host reads feedback at all frames how W1 and W3
    # should be read. It takes the slope so its consequence can name a glitch
    # cadence rather than just saying "unbounded".
    verdicts.insert(0, rules.r1_feedback_polled(
        blocks, man, args.correction_quantum, slope))
    verdicts.append(rules.w1_residual_drift(blocks, man, slope,
                                            args.correction_quantum))
    verdicts.append(rules.w3_feedback_tracked(blocks, man, slope))

    if args.json:
        print(json.dumps({"header": header,
                          "verdicts": [v.__dict__ for v in verdicts]}, indent=2))
    else:
        print(report.render(header, verdicts))
    return report.exit_code(verdicts)


if __name__ == "__main__":
    sys.exit(main())
```

- [ ] **Step 2: Add `fit_slope` to vcdfill**

`cli.py` calls `vcdfill.fit_slope(fill)`. Check whether it exists:

```bash
cd ~/Development/rt1170/evkb/tools && grep -n "^def " vcdfill.py
```

If there is no `fit_slope`, add one to `vcdfill.py`. It must return the slope in bytes/second over the longest monotonic fill segment, or `None` when the fill signal is absent or too short to fit:

```python
def fit_slope(sigs):
    """Least-squares slope of the OUT-FIFO fill trace, in bytes/second.

    Fitted over the longest segment between block corrections: a correction
    is a discontinuity, and fitting across one measures the correction rather
    than the drift. Returns None when there is nothing fittable, which the
    caller reports as SKIP rather than as zero drift -- an absent instrument
    is not a reading of zero.
    """
    seg = longest_segment(sigs)
    if seg is None or len(seg) < 2:
        return None
    n = len(seg)
    mean_t = sum(t for t, _ in seg) / n
    mean_v = sum(v for _, v in seg) / n
    num = sum((t - mean_t) * (v - mean_v) for t, v in seg)
    den = sum((t - mean_t) ** 2 for t, _ in seg)
    return None if den == 0 else num / den
```

`longest_segment(sigs)` must reuse `vcdfill.py`'s existing segmentation — the module already splits the fill trace at corrections in order to fit drift. If that logic is currently inline in `vcdfill.py`'s reporting code, extract it into `longest_segment()` and have both callers use it, rather than writing a second segmenter that can disagree with the first.

- [ ] **Step 3: Make it importable**

`cli.py` imports `vcdfill`, which lives one directory up. Add at the top of `cli.py`, before the `import rules` line:

```python
import os
sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
```

- [ ] **Step 4: Smoke-test against a synthetic capture**

```bash
cd ~/Development/rt1170/evkb/tools/uacvalidate
python3 -c "
from trace import Block, synth_vcd
h = dict(pkt_count=120000, or_acc=0xFFFFFF00, and_acc=0, nonsilent_frames=5292000,
         fb_poll_count=7500, alt_out=1, class_req_bitmap=5,
         size_hist_size=[44*32,45*32,0,0,0,0,0,0],
         size_hist_count=[108000,12000,0,0,0,0,0,0])
synth_vcd('/tmp/ok.vcd', [(0.0, Block(alt_out=1)), (120.0, Block(**h))])
"
cat > /tmp/man.json <<'EOF'
{"audio_class": 2, "speed": "HS", "channels": 8, "subslot_bytes": 4,
 "sample_rate_hz": 44100, "mode": "passive", "host_note": "synthetic"}
EOF
chmod +x cli.py && ./cli.py /tmp/ok.vcd /tmp/man.json; echo "exit=$?"
```

Expected: a report with R1–R4b PASS, R7 SKIP (passive), W2 PASS, W1/W3 SKIP (no fill probe in a synthetic capture), `exit=0`.

- [ ] **Step 5: Commit**

```bash
cd ~/Development/rt1170/evkb
git add tools/uacvalidate/cli.py tools/vcdfill.py
git commit -m "uacvalidate: CLI wiring the manifest, trace and rules together

Two capture-level invalidations run before any rule: xscope missing marks
mean every counter is a lower bound, and a wrong magic means the observer and
judge disagree about the wire format. Both exit 2 rather than 1 -- they say
nothing about the host."
```

---

## Task 7: Full suite green

- [ ] **Step 1: Run everything**

```bash
cd ~/Development/rt1170/evkb/tools/uacvalidate && python3 -m unittest discover -p "test_*.py" -v
```

Expected: 4 + 9 + 33 + 10 = 56 tests, all PASS.

- [ ] **Step 2: Commit if anything needed fixing**

```bash
cd ~/Development/rt1170/evkb
git add -A tools/uacvalidate/
git commit -m "uacvalidate: full suite green"
```

---

## Done when

- `python3 -m unittest discover -p "test_*.py"` passes with every rule covered by PASS, FAIL and SKIP fixtures.
- The silence fixture makes R3 report SKIP, not PASS.
- `Verdict` refuses to construct an uncited FAIL, a consequence-free WARN, or a SKIP with no named missing witness.
- `./cli.py` produces a report and a correct exit code from a synthetic capture with no hardware attached.
