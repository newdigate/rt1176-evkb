# UAC Host Validator — Plan 3: The Verification Corpus

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Prove the validator finds defects rather than merely producing output, by pointing it at hosts whose defects are already characterised.

**Architecture:** The `USBHost_t36` history is the corpus. Four commits are hosts with documented, independently-measured defects; each is checked out, built, run against the witness firmware, and the resulting report is asserted to name that defect and no other. Where history has no negative, a one-line injected defect supplies one. Every run is archived.

**Tech Stack:** Bench hardware — MIMXRT1170-EVKB + XMOS MC200 + XTAG-3; LinkServer; XMOS XTC 15.3.1; Python 3.

**Depends on:** Plan 1 complete (observer on silicon), Plan 2 complete (judge green).

**Design spec:** `docs/superpowers/specs/2026-08-04-uac-host-validator-design.md`

---

## Why this plan exists

A conformance tool that has only ever been run against a healthy host has been
tested for *crashes*, not for *judgement*. It will happily report all-PASS
forever. The only way to know a rule fires is to show it a host that breaks it,
and the only trustworthy way to know the rule fired *correctly* is for that
host's defect to have been characterised independently, beforehand, by a
different instrument.

`transcript_hw_evkb.txt` is that independent characterisation. Every entry
below has a measurement behind it that predates this tool.

---

## The corpus

All SHAs are in `~/Development/USBHost_t36`.

| Case | Commit | Documented defect | Expected verdict |
|---|---|---|---|
| A | `99cd466~1` | UAC1 host before "read the feedback endpoint and size packets from it" | `R1 WARN` with a glitch cadence; `W1 WARN` at ≈ −86 ppm |
| B | `1732ed3~1` | UAC2/HS host before "close the rate loop at high speed" | `R1 WARN`; `W1 WARN` ≈ +84 ppm; matches P1's open-loop signature |
| C | `d370e80~1` | before "do not stream to an unconfigured device" | `R2 FAIL` |
| D | `35b0cce~1` | before "left-justify samples within subslots" | `R3 FAIL` |
| E | `090eadb~1` | device-swap wedge: healthy API, descriptors never rebuilt | all rules `SKIP` — no packets at all |

Case D is the important discovery: the design spec assumed R3 had no
historical negative and would need injection. It has one. A host that
right-justified 24-in-4 subslots really did exist in this tree, and its fix
commit is `35b0cce`.

Case E is the subtle one. The pre-fix device-swap host reports a healthy
stream while transmitting nothing — `pkts/s=0` with `audio=ready alt=1`. The
correct validator behaviour is a page of `SKIP`s, not a `PASS`. A tool that
reports PASS here has failed, because nothing was tested.

**W3 has no historical negative.** The EMA that fixed the +4.8 ppm
dither-chasing servo was never a standalone commit, so Task 7 injects it.

---

## File Structure

| File | Responsibility |
|---|---|
| `tools/uacvalidate/corpus/README.md` | What the corpus is and why history is the source of truth. |
| `tools/uacvalidate/corpus/cases.json` | The table above, machine-readable: commit, manifest, expected verdicts. |
| `tools/uacvalidate/corpus/run_case.sh` | Build, flash, capture, judge one case. |
| `tools/uacvalidate/corpus/check_case.py` | Assert a report matches a case's expectations. |
| `tools/uacvalidate/corpus/captures/` | Archived VCDs, one per case. |
| `tools/uacvalidate/defects/*.patch` | Injected defects where history has none. |
| `examples/usb/usb_audio_graph_test/transcript_uacv_corpus.txt` | The evidence record. |

---

## Task 1: The case definitions

**Files:**
- Create: `tools/uacvalidate/corpus/cases.json`
- Create: `tools/uacvalidate/corpus/README.md`

- [ ] **Step 1: Write the case file**

Create `tools/uacvalidate/corpus/cases.json`:

```json
{
  "comment": "Each case is a host with an independently characterised defect. The evidence predating this tool is in examples/usb/usb_audio_graph_test/transcript_hw_evkb.txt.",
  "cases": [
    {
      "id": "A_uac1_no_feedback",
      "repo": "USBHost_t36",
      "commit": "99cd466~1",
      "witness_config": "1AMi2o2xxxxxx",
      "manifest": {
        "audio_class": 1, "speed": "FS", "channels": 2, "subslot_bytes": 2,
        "sample_rate_hz": 44100, "mode": "passive",
        "host_note": "USBHost_t36 99cd466~1 -- before the UAC1 feedback read"
      },
      "duration_s": 300,
      "expect": {"R1": "WARN", "W1": "WARN", "R2": "PASS", "R4b": "PASS"},
      "expect_evidence": {"R1": {"feedback_polls": 0}},
      "ground_truth": "transcript_hw_evkb.txt: open loop at bias 0 drifts +85.0 ppm; block corrections at quantum/drift cadence."
    },
    {
      "id": "B_uac2_no_feedback",
      "repo": "USBHost_t36",
      "commit": "1732ed3~1",
      "witness_config": "2AMi8o8xxxxxx",
      "manifest": {
        "audio_class": 2, "speed": "HS", "channels": 8, "subslot_bytes": 4,
        "sample_rate_hz": 44100, "mode": "passive",
        "host_note": "USBHost_t36 1732ed3~1 -- before the HS feedback loop"
      },
      "duration_s": 300,
      "expect": {"R1": "WARN", "W1": "WARN", "R2": "PASS", "R3": "PASS"},
      "expect_evidence": {"R1": {"feedback_polls": 0}},
      "ground_truth": "transcript_hw_evkb.txt UAC2 P1: fill slope +119.3 B/s = +84.6 ppm, overflow corrections every ~9.6 s."
    },
    {
      "id": "C_streams_in_alt0",
      "repo": "USBHost_t36",
      "commit": "d370e80~1",
      "witness_config": "2AMi8o8xxxxxx",
      "manifest": {
        "audio_class": 2, "speed": "HS", "channels": 8, "subslot_bytes": 4,
        "sample_rate_hz": 44100, "mode": "passive",
        "host_note": "USBHost_t36 d370e80~1 -- before the alt-0 streaming gate"
      },
      "duration_s": 180,
      "expect": {"R2": "FAIL"},
      "ground_truth": "transcript_hw_evkb.txt: the self-heal path zombie-streams the old ring at nominal into the device's alt 0."
    },
    {
      "id": "D_right_justified",
      "repo": "USBHost_t36",
      "commit": "35b0cce~1",
      "witness_config": "2AMi8o8xxxxxx",
      "manifest": {
        "audio_class": 2, "speed": "HS", "channels": 8, "subslot_bytes": 4,
        "sample_rate_hz": 44100, "mode": "passive",
        "host_note": "USBHost_t36 35b0cce~1 -- before subslot left-justification"
      },
      "duration_s": 180,
      "expect": {"R3": "FAIL"},
      "ground_truth": "35b0cce is the fix commit: 'UAC2: left-justify samples within subslots; run test_pack in the gate'."
    },
    {
      "id": "E_device_swap_wedge",
      "repo": "USBHost_t36",
      "commit": "090eadb~1",
      "witness_config": "2AMi8o8xxxxxx",
      "manifest": {
        "audio_class": 2, "speed": "HS", "channels": 8, "subslot_bytes": 4,
        "sample_rate_hz": 44100, "mode": "passive",
        "host_note": "USBHost_t36 090eadb~1 -- device-swap wedge, after an FS->HS swap"
      },
      "duration_s": 120,
      "requires_device_swap": true,
      "expect": {"R2": "SKIP", "R3": "SKIP", "R4a": "SKIP", "R4b": "SKIP"},
      "ground_truth": "transcript_hw_evkb.txt: 'audio=ready uac2=0 alt=1 pkts/s=0' -- the claim and control sequence succeed while nothing is transmitted."
    }
  ]
}
```

- [ ] **Step 2: Write the README**

Create `tools/uacvalidate/corpus/README.md` stating: what the corpus is; that each case's defect was characterised by a different instrument before this tool existed; that a case whose expected verdict changes must be justified against `transcript_hw_evkb.txt` rather than adjusted to match new output; and that **adjusting an expectation to make a case pass is the one edit that destroys the value of this directory.**

- [ ] **Step 3: Commit**

```bash
cd ~/Development/rt1170/evkb
git add tools/uacvalidate/corpus/
git commit -m "uacvalidate: the historical corpus, five characterised hosts

Each case is a commit whose defect was measured before this tool existed, so
the tool is graded against ground truth rather than against itself. Case D --
35b0cce~1, right-justified subslots -- was found while writing this plan; the
design spec had assumed R3 needed an injected defect."
```

---

## Task 2: The expectation checker

**Files:**
- Create: `tools/uacvalidate/corpus/check_case.py`
- Test: `tools/uacvalidate/corpus/test_check_case.py`

- [ ] **Step 1: Write the failing test**

Create `tools/uacvalidate/corpus/test_check_case.py`:

```python
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


if __name__ == "__main__":
    unittest.main()
```

- [ ] **Step 2: Run to verify it fails**

```bash
cd ~/Development/rt1170/evkb/tools/uacvalidate/corpus && python3 -m unittest test_check_case -v
```

Expected: FAIL — `ModuleNotFoundError: No module named 'check_case'`.

- [ ] **Step 3: Write the implementation**

Create `tools/uacvalidate/corpus/check_case.py`:

```python
#!/usr/bin/env python3
"""Assert a validator report matches a corpus case's documented expectations.

Rules the case does not mention are ignored: a case pins the defect it was
chosen for, not the whole report, so an unrelated rule changing level does not
turn into spurious corpus churn.
"""
import json
import sys


def check(case, report_json):
    by_id = {v["rule_id"]: v for v in report_json["verdicts"]}
    msgs = []
    ok = True

    for rid, expected in case["expect"].items():
        if rid not in by_id:
            ok = False
            msgs.append(f"{rid}: expected {expected}, but the rule did not run")
            continue
        got = by_id[rid]["level"]
        if got != expected:
            ok = False
            msgs.append(f"{rid}: expected {expected}, got {got}")

    for rid, fields in case.get("expect_evidence", {}).items():
        if rid not in by_id:
            ok = False
            msgs.append(f"{rid}: expected evidence but the rule did not run")
            continue
        ev = by_id[rid].get("evidence", {})
        for key, expected in fields.items():
            if key not in ev:
                ok = False
                msgs.append(f"{rid}.{key}: missing from evidence")
            elif ev[key] != expected:
                ok = False
                msgs.append(f"{rid}.{key}: expected {expected}, got {ev[key]}")

    return ok, msgs


def main():
    if len(sys.argv) != 4:
        print("usage: check_case.py cases.json <case-id> report.json",
              file=sys.stderr)
        return 2
    cases = json.load(open(sys.argv[1]))["cases"]
    case = next((c for c in cases if c["id"] == sys.argv[2]), None)
    if case is None:
        print(f"no such case: {sys.argv[2]}", file=sys.stderr)
        return 2
    ok, msgs = check(case, json.load(open(sys.argv[3])))
    for m in msgs:
        print(f"  MISMATCH {m}")
    print(f"{case['id']}: {'OK' if ok else 'MISMATCH'}")
    if not ok:
        print("\nDo NOT edit the expectation to make this pass. The "
              "expectation came from an independent measurement recorded in "
              "transcript_hw_evkb.txt. Either the validator is wrong, or the "
              "bench differs from the run that produced the ground truth -- "
              "find out which.")
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
```

- [ ] **Step 4: Run to verify they pass**

```bash
cd ~/Development/rt1170/evkb/tools/uacvalidate/corpus && python3 -m unittest test_check_case -v
```

Expected: 5 tests, all PASS.

- [ ] **Step 5: Commit**

```bash
cd ~/Development/rt1170/evkb
git add tools/uacvalidate/corpus/check_case.py tools/uacvalidate/corpus/test_check_case.py
git commit -m "uacvalidate: corpus expectation checker

A case pins the defect it was chosen for, not the whole report, so unrelated
rules changing level does not cause corpus churn. A mismatch prints an
explicit instruction not to edit the expectation, because doing so is the one
change that destroys the corpus's value."
```

---

## Task 3: Case B — UAC2/HS host that never reads feedback

Run B before A: it uses the same witness image as Plan 1 Task 9, so a failure here is more likely to be the case setup than the format.

**Files:**
- Create: `tools/uacvalidate/corpus/captures/B_uac2_no_feedback.vcd`

- [ ] **Step 1: Build the historical host**

```bash
cd ~/Development/USBHost_t36
git stash list   # confirm nothing is stashed that you will lose
git checkout -b corpus/B 1732ed3~1
cd ~/Development/rt1170/evkb/examples/usb/usb_audio_graph_test
rm -rf build-corpus && cmake -B build-corpus -DCMAKE_TOOLCHAIN_FILE=toolchain/rt1170-evkb.toolchain.cmake
cmake --build build-corpus
```

Expected: `build-corpus/usb_audio_graph_test.elf` exists. Local-first library resolution means the `USBHost_t36` checkout at `1732ed3~1` is what gets compiled.

- [ ] **Step 2: Start the collector before the host**

```bash
set +u; source /Applications/XMOS_XTC_15.3.1/SetEnv.sh
cd ~/Development/xmos/sw_usb_audio/app_usb_aud_xk_216_mc
xrun --adapter-id 3LajHPG5 \
     --xscope-file ~/Development/rt1170/evkb/tools/uacvalidate/corpus/captures/B_uac2_no_feedback \
     bin/2AMi8o8xxxxxx/app_usb_aud_xk_216_mc_2AMi8o8xxxxxx.xe
```

- [ ] **Step 3: Flash and run the host**

```bash
pkill -9 LinkServer; pkill -9 redlinkserv; pkill -9 crt_emu_cm_redlink
cd ~/Development/rt1170/evkb/examples/usb/usb_audio_graph_test
LinkServer run MIMXRT1176:MIMXRT1170-EVKB build-corpus/usb_audio_graph_test.elf
```

Do not hold the VCOM during any LinkServer operation — that has kernel-panicked this Mac.

- [ ] **Step 4: Run 300 s, stop the collector with SIGINT**

Five minutes gives the open-loop drift enough span for a clean fit. **Ctrl-C**, never `kill -9`.

- [ ] **Step 5: Judge and check**

```bash
cd ~/Development/rt1170/evkb/tools/uacvalidate
python3 -c "
import json
from manifest import Manifest
cases = json.load(open('corpus/cases.json'))['cases']
c = next(x for x in cases if x['id'] == 'B_uac2_no_feedback')
json.dump(c['manifest'], open('/tmp/B.json','w'))
"
./cli.py corpus/captures/B_uac2_no_feedback.vcd /tmp/B.json \
    --observer-sha "$(git -C ~/Development/xmos/lib_xua rev-parse --short HEAD)" \
    --json > /tmp/B_report.json
./cli.py corpus/captures/B_uac2_no_feedback.vcd /tmp/B.json
python3 corpus/check_case.py corpus/cases.json B_uac2_no_feedback /tmp/B_report.json
```

Expected: `B_uac2_no_feedback: OK`, with the human-readable report showing
`R1 WARN` (feedback_polls = 0) and `W1 WARN` at roughly +84 ppm — the same
number `transcript_hw_evkb.txt` records for the P1 open-loop gate, reached by
a different instrument.

- [ ] **Step 6: If it mismatches, diagnose before touching anything**

A `feedback_polls` value above zero means the historical host *is* polling —
check you built the right commit. An `R1 PASS` means the poll counter is
wrong, not the case. **Do not edit `cases.json`.**

- [ ] **Step 7: Commit the capture**

```bash
cd ~/Development/rt1170/evkb
git add tools/uacvalidate/corpus/captures/B_uac2_no_feedback.vcd
git commit -m "corpus: case B capture -- UAC2/HS host that never reads feedback

R1 WARN with feedback_polls=0 and W1 at the +84 ppm the P1 open-loop gate
measured independently. Archived rather than discarded: rules stay
re-runnable against past captures only if the captures survive."
```

---

## Task 4: Case D — right-justified subslots

The strongest case in the corpus. R3 is the check that most needs proving,
because a right-justified stream sounds fine and no other instrument on this
bench would catch it.

**Files:**
- Create: `tools/uacvalidate/corpus/captures/D_right_justified.vcd`

- [ ] **Step 1: Build the historical host**

```bash
cd ~/Development/USBHost_t36 && git checkout -b corpus/D 35b0cce~1
cd ~/Development/rt1170/evkb/examples/usb/usb_audio_graph_test
rm -rf build-corpus && cmake -B build-corpus -DCMAKE_TOOLCHAIN_FILE=toolchain/rt1170-evkb.toolchain.cmake
cmake --build build-corpus
```

- [ ] **Step 2: Confirm the host really does right-justify before running**

```bash
cd ~/Development/USBHost_t36 && git show 35b0cce --stat && git show 35b0cce | head -60
```

Read the diff. The pre-image is what this case exercises; if the fix commit
turns out to change something other than subslot justification, this case is
mis-labelled and must be re-derived rather than run.

- [ ] **Step 3: Capture**

```bash
set +u; source /Applications/XMOS_XTC_15.3.1/SetEnv.sh
cd ~/Development/xmos/sw_usb_audio/app_usb_aud_xk_216_mc
xrun --adapter-id 3LajHPG5 \
     --xscope-file ~/Development/rt1170/evkb/tools/uacvalidate/corpus/captures/D_right_justified \
     bin/2AMi8o8xxxxxx/app_usb_aud_xk_216_mc_2AMi8o8xxxxxx.xe
```

In another shell:

```bash
pkill -9 LinkServer; pkill -9 redlinkserv; pkill -9 crt_emu_cm_redlink
cd ~/Development/rt1170/evkb/examples/usb/usb_audio_graph_test
LinkServer run MIMXRT1176:MIMXRT1170-EVKB build-corpus/usb_audio_graph_test.elf
```

Run 180 s with the tone playing — **R3 needs signal**, and a silent stream
will correctly produce SKIP rather than FAIL. SIGINT to stop.

- [ ] **Step 4: Judge and check**

```bash
cd ~/Development/rt1170/evkb/tools/uacvalidate
python3 -c "
import json
cases = json.load(open('corpus/cases.json'))['cases']
c = next(x for x in cases if x['id'] == 'D_right_justified')
json.dump(c['manifest'], open('/tmp/D.json','w'))
"
./cli.py corpus/captures/D_right_justified.vcd /tmp/D.json --json > /tmp/D_report.json
./cli.py corpus/captures/D_right_justified.vcd /tmp/D.json
python3 corpus/check_case.py corpus/cases.json D_right_justified /tmp/D_report.json
```

Expected: `R3 FAIL`, with evidence showing `or_acc` having a non-zero low byte
and a constant top byte, and `nonsilent_frames` comfortably above 10,000.

- [ ] **Step 5: Confirm the vacuity guard on the same capture**

```bash
cd ~/Development/rt1170/evkb/tools/uacvalidate
python3 -c "
import json
from trace import read_blocks
bs = read_blocks('corpus/captures/D_right_justified.vcd')
_, b = bs[-1]
print('or_acc  0x%08X' % b.or_acc)
print('and_acc 0x%08X' % b.and_acc)
print('nonsilent', b.nonsilent_frames)
print('low byte ever set:', bool(b.or_acc & 0xFF))
print('top byte constant:', (b.or_acc >> 24) == (b.and_acc >> 24))
"
```

Expected: `low byte ever set: True`. That single boolean is the entire
justification finding, measured on silicon against a host that really did
right-justify.

- [ ] **Step 6: Commit**

```bash
cd ~/Development/rt1170/evkb
git add tools/uacvalidate/corpus/captures/D_right_justified.vcd
git commit -m "corpus: case D capture -- right-justified subslots caught on silicon

R3 FAIL against 35b0cce~1, a host that really did right-justify 24-in-4. This
is the check no other instrument on this bench can perform: the stream sounds
fine, just quiet."
```

---

## Task 5: Case C — streaming into alt 0

**Files:**
- Create: `tools/uacvalidate/corpus/captures/C_streams_in_alt0.vcd`

- [ ] **Step 1: Build**

```bash
cd ~/Development/USBHost_t36 && git checkout -b corpus/C d370e80~1
cd ~/Development/rt1170/evkb/examples/usb/usb_audio_graph_test
rm -rf build-corpus && cmake -B build-corpus -DCMAKE_TOOLCHAIN_FILE=toolchain/rt1170-evkb.toolchain.cmake
cmake --build build-corpus
```

- [ ] **Step 2: Reproduce the trigger**

This defect only appears when the self-heal path retargets a stream at a
device that has not selected an alternate setting. Per
`transcript_hw_evkb.txt`, that state is reached by a device that attaches and
re-enumerates within seconds — which `xrun`'s load sequence does.

Start the collector, let the host claim and stream normally, then `xrun` the
witness again mid-stream to force the re-enumeration. The second claim's
control sequence wedges with alt never valid, and the pre-fix host streams
into alt 0 anyway.

```bash
set +u; source /Applications/XMOS_XTC_15.3.1/SetEnv.sh
cd ~/Development/xmos/sw_usb_audio/app_usb_aud_xk_216_mc
xrun --adapter-id 3LajHPG5 \
     --xscope-file ~/Development/rt1170/evkb/tools/uacvalidate/corpus/captures/C_streams_in_alt0 \
     bin/2AMi8o8xxxxxx/app_usb_aud_xk_216_mc_2AMi8o8xxxxxx.xe
```

Flash and run the host, let it stream for 60 s, then in a third shell re-run
the same `xrun` command to reload the witness under the streaming host. Let it
sit a further 120 s. SIGINT to stop.

- [ ] **Step 3: Judge and check**

```bash
cd ~/Development/rt1170/evkb/tools/uacvalidate
python3 -c "
import json
cases = json.load(open('corpus/cases.json'))['cases']
c = next(x for x in cases if x['id'] == 'C_streams_in_alt0')
json.dump(c['manifest'], open('/tmp/C.json','w'))
"
./cli.py corpus/captures/C_streams_in_alt0.vcd /tmp/C.json --json > /tmp/C_report.json
./cli.py corpus/captures/C_streams_in_alt0.vcd /tmp/C.json
python3 corpus/check_case.py corpus/cases.json C_streams_in_alt0 /tmp/C_report.json
```

Expected: `R2 FAIL` with a non-zero `packets_while_alt0`.

- [ ] **Step 4: If the wedge does not reproduce, record that and move on**

This case depends on a timing-sensitive re-enumeration race. If three attempts
do not reproduce it, mark the case `"skipped_reason"` in `cases.json` with the
attempts made, and do **not** weaken R2 or fabricate the capture. A corpus case
that cannot be staged is an honest gap; a faked one is worse than no case.

- [ ] **Step 5: Commit**

```bash
cd ~/Development/rt1170/evkb
git add tools/uacvalidate/corpus/captures/ tools/uacvalidate/corpus/cases.json
git commit -m "corpus: case C -- streaming into alt 0

Staged by reloading the witness under a streaming pre-fix host, which is the
re-enumeration race transcript_hw_evkb.txt documents."
```

---

## Task 6: Cases A and E

**Files:**
- Create: `tools/uacvalidate/corpus/captures/A_uac1_no_feedback.vcd`
- Create: `tools/uacvalidate/corpus/captures/E_device_swap_wedge.vcd`

- [ ] **Step 1: Case A — the UAC1 image**

Case A uses the *other* witness image. Reflash the MC200 personality:

```bash
set +u; source /Applications/XMOS_XTC_15.3.1/SetEnv.sh
cd ~/Development/xmos/sw_usb_audio/app_usb_aud_xk_216_mc
xrun --adapter-id 3LajHPG5 \
     --xscope-file ~/Development/rt1170/evkb/tools/uacvalidate/corpus/captures/A_uac1_no_feedback \
     bin/1AMi2o2xxxxxx/app_usb_aud_xk_216_mc_1AMi2o2xxxxxx.xe
```

The personality change is visible on the host as a PID change — `20B1:000F`
for UAC1/FS versus `20B1:000E` for UAC2/HS. Confirm the host logs `uac2=0`
before trusting the capture.

Build `99cd466~1`, flash, run 300 s, SIGINT, judge with case A's manifest,
check.

Expected: `R1 WARN` with `feedback_polls = 0`, and `W1 WARN` at roughly
−86 ppm — the crystal offset four independent instruments have now agreed on.
`R3` will report `SKIP`: a 2-byte subslot has no spare lane to justify within.

- [ ] **Step 2: Case E — the device-swap wedge**

Build `090eadb~1`. Start the collector on the UAC1 image, let the host claim
and stream, then `xrun` the UAC2 image to swap the device's personality
without resetting the host. The pre-fix host reports a healthy stream and
transmits nothing.

Expected: every rule `SKIP` for want of packets. Specifically **not** `PASS` —
a validator that reports PASS when nothing was transmitted has failed, because
nothing was tested.

- [ ] **Step 3: Verify E's report says nothing was tested**

```bash
cd ~/Development/rt1170/evkb/tools/uacvalidate
./cli.py corpus/captures/E_device_swap_wedge.vcd /tmp/E.json | grep -c "PASS"
```

Expected: `0` occurrences of PASS among the rule lines. If any rule reports
PASS on a capture with zero packets, that rule's SKIP guard is missing — fix
the rule, not the case.

- [ ] **Step 4: Commit**

```bash
cd ~/Development/rt1170/evkb
git add tools/uacvalidate/corpus/captures/
git commit -m "corpus: cases A and E

A is the UAC1 open-loop host at -86 ppm, the crystal offset four independent
instruments now agree on. E is the device-swap wedge, whose correct report is
a page of SKIPs -- a PASS there would mean the tool blessed a host that
transmitted nothing."
```

---

## Task 7: Injected defect for W3

History has no commit where the servo chased raw dither, so this one is made.

**Files:**
- Create: `tools/uacvalidate/defects/w3-no-ema.patch`
- Create: `tools/uacvalidate/corpus/captures/F_dither_chasing.vcd`

- [ ] **Step 1: Locate the EMA**

```bash
cd ~/Development/USBHost_t36 && git checkout master
grep -rn "ema\|EMA\|>> 3\|/ 8" usb_audio*.cpp usb_audio*.h 2>/dev/null | head -20
```

Find where the feedback report is averaged — per `transcript_hw_evkb.txt` it
is a 1/8-per-report exponential moving average feeding the sizing accumulator.

- [ ] **Step 2: Make the injection and save it as a patch**

Change the servo to consume the raw report instead of the average — a
one-line change substituting the latest decoded value for the EMA output.
Then:

```bash
cd ~/Development/USBHost_t36
git diff > ~/Development/rt1170/evkb/tools/uacvalidate/defects/w3-no-ema.patch
head -20 ~/Development/rt1170/evkb/tools/uacvalidate/defects/w3-no-ema.patch
```

Add a header comment to the patch file stating what it injects, why history
has no natural case, and that it must never be applied to a shipping build.

- [ ] **Step 3: Capture and judge**

Build the injected host, capture 600 s against the `2AMi8o8` witness — the
+4.8 ppm residual needs a long span to resolve cleanly — and judge with case
B's manifest and a `host_note` naming the injection.

Expected: `R1 PASS` (the host *is* polling), and `W3 WARN` with a residual
near +4.8 ppm. That combination is the signature: reading the feedback and
still drifting is precisely what W3 exists to name, and it is invisible to R1.

- [ ] **Step 4: Revert the injection**

```bash
cd ~/Development/USBHost_t36 && git checkout -- . && git status --short
```

Expected: no modified files.

- [ ] **Step 5: Commit**

```bash
cd ~/Development/rt1170/evkb
git add tools/uacvalidate/defects/ tools/uacvalidate/corpus/captures/F_dither_chasing.vcd
git commit -m "corpus: injected W3 defect -- servo chasing raw dither

R1 PASS and W3 WARN together: the host reads the feedback and still drifts.
That pairing is what W3 exists to name, and R1 alone cannot see it. Injected
rather than historical because the EMA was never a standalone commit."
```

---

## Task 7b: Injected defects for R4a and R4b

History has no host that oversized a packet or sent a non-integral frame
count, so both R4 rules would otherwise ship having never fired on hardware.

**Files:**
- Create: `tools/uacvalidate/defects/r4a-oversize.patch`
- Create: `tools/uacvalidate/defects/r4b-partial-frame.patch`
- Create: `tools/uacvalidate/corpus/captures/G_oversize.vcd`
- Create: `tools/uacvalidate/corpus/captures/H_partial_frame.vcd`

- [ ] **Step 1: Find the packet-sizing site**

```bash
cd ~/Development/USBHost_t36 && git checkout master
grep -rn "wMaxPacketSize\|packet_bytes\|samples_this\|sizing" usb_audio*.cpp | head -20
```

- [ ] **Step 2: Inject the oversize defect**

Change the sizing calculation to add one extra audio frame to every packet,
pushing it past the endpoint's advertised `wMaxPacketSize`. Save it:

```bash
cd ~/Development/USBHost_t36
git diff > ~/Development/rt1170/evkb/tools/uacvalidate/defects/r4a-oversize.patch
```

Add a header comment to the patch naming what it injects and stating it must
never be applied to a shipping build.

- [ ] **Step 3: Capture and check R4a fires**

Build the injected host, capture 120 s against the `2AMi8o8` witness, judge
with case B's manifest and a `host_note` naming the injection.

Expected: `R4a FAIL`, with evidence listing a size above the ceiling.

If the device rejects the oversize packets outright and the histogram never
records them, that is itself worth knowing — record it in the transcript and
note that R4a is then unprovable on this device, because XUD refuses the
traffic before the decoupler can count it. **Do not weaken R4a to compensate.**

- [ ] **Step 4: Revert, then inject the partial-frame defect**

```bash
cd ~/Development/USBHost_t36 && git checkout -- .
```

Change the sizing to emit a byte count that is not a multiple of
`channels x subslot_bytes` — one extra byte per packet is enough. Save as
`r4b-partial-frame.patch`.

- [ ] **Step 5: Capture and check R4b fires**

Capture 120 s, judge.

Expected: `R4b FAIL`, with `not_multiple` or `short_discarded` non-zero. This
is the path `decouple.xc:631` already handles under a comment naming a bad
driver — the injection makes that silent tolerance audible for the first time.

- [ ] **Step 6: Revert**

```bash
cd ~/Development/USBHost_t36 && git checkout -- . && git status --short
```

Expected: no modified files.

- [ ] **Step 7: Commit**

```bash
cd ~/Development/rt1170/evkb
git add tools/uacvalidate/defects/ tools/uacvalidate/corpus/captures/G_oversize.vcd tools/uacvalidate/corpus/captures/H_partial_frame.vcd
git commit -m "corpus: injected R4a and R4b defects

Neither had a historical negative, so both rules would have shipped never
having fired on hardware. R4b exercises the path decouple.xc:631 already
handles under a comment naming a bad driver -- silent tolerance made audible."
```

---

## Task 8: Clean-host regression and the transcript

**Files:**
- Create: `tools/uacvalidate/corpus/captures/Z_clean_uac1.vcd`
- Create: `tools/uacvalidate/corpus/captures/Z_clean_uac2.vcd`
- Create: `examples/usb/usb_audio_graph_test/transcript_uacv_corpus.txt`

- [ ] **Step 1: Restore the host to master**

```bash
cd ~/Development/USBHost_t36 && git checkout master && git status --short
git branch -D corpus/B corpus/C corpus/D 2>/dev/null; true
```

Expected: clean tree on `master`.

- [ ] **Step 2: Capture a clean run on each image**

Build at `master`. Capture 1800 s against `2AMi8o8` and 1800 s against
`1AMi2o2`, following the standard procedure — collector first, host second,
SIGINT to stop.

- [ ] **Step 3: Judge both**

```bash
cd ~/Development/rt1170/evkb/tools/uacvalidate
./cli.py corpus/captures/Z_clean_uac2.vcd /tmp/clean_uac2.json; echo "exit=$?"
./cli.py corpus/captures/Z_clean_uac1.vcd /tmp/clean_uac1.json; echo "exit=$?"
```

Expected for both: `exit=0`, no `FAIL`, `R7 SKIP` (passive mode), and `W1`
`PASS` at the +0.1 to +0.16 ppm already measured — the UAC1 closed-loop soak
reported +0.1 ppm and the UAC2 P3 soak +0.156 ppm.

If `W1` reports `WARN` on a clean host, compare its ppm against those two
numbers before assuming a regression: the servo is known-good at this
residual, so a `WARN` means `DRIFT_NOISE_FLOOR_PPM` is set too tight, not that
the host broke.

- [ ] **Step 4: Write the transcript**

Create `examples/usb/usb_audio_graph_test/transcript_uacv_corpus.txt`
recording, per case: the commit, the witness image, the capture duration, the
verdicts produced, the ground-truth number from `transcript_hw_evkb.txt` it
was checked against, and any case that could not be staged with the attempts
made.

State plainly at the top that **R1 is a WARN and not a FAIL**, with the
§5.12.4.2 wording that forced it — the founding defect of this investigation
is not a spec violation, and a reader of the corpus should learn that in the
first paragraph rather than infer it from a table.

- [ ] **Step 5: Commit**

```bash
cd ~/Development/rt1170/evkb
git add tools/uacvalidate/corpus/captures/ examples/usb/usb_audio_graph_test/transcript_uacv_corpus.txt
git commit -m "corpus: clean-host regression on both images, and the transcript

Every case checked against a number that predates this tool. The transcript
leads with the R1 finding: reading USB 2.0 5.12.4.2 showed the obligation
runs to the device, not the host, so a host that ignores feedback is
conformant -- and this tool says so in the WARN it emits rather than
overclaiming a violation."
```

---

## Done when

- Cases A, B, D and E reproduce their documented defects, checked by
  `check_case.py` against expectations derived from `transcript_hw_evkb.txt`.
- Case C reproduces, or is recorded as un-stageable with the attempts made.
- The injected W3 defect produces `R1 PASS` with `W3 WARN`.
- A clean host at `master` produces exit 0 on both images, with `W1` at the
  residual the closed-loop soaks already measured.
- `~/Development/USBHost_t36` is back on `master` with a clean tree.
- Every capture is archived under `corpus/captures/`.
