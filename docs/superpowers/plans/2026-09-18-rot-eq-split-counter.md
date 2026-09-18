# acid_box ROT_EQ split counter Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Split `ACIDBOX_ROT_EQ`'s ambiguous `fail` counter into `mismatch` (the picture is wrong) and `starved` (the check never finished), keeping `fail` as their sum, so the next silicon bench can say WHICH fault NEW-53 is.

**Architecture:** Spec `docs/superpowers/specs/2026-09-18-rot-eq-split-counter-design.md` (Linear NEW-53, first action). One console line changes in `examples/display/acid_box/acid_box.cpp`; the gate pins the new format and adds three assertions; the fixture is re-spliced; two vacuity negatives show each component fires. No pixel is touched, so the goldens must not move. Two tasks: the change (gate RED-first), then close-out.

**Tech Stack:** Teensyduino-style core for i.MX RT1176, LVGL 9.4, CMake + ARM GCC 10, qemu2 through `tools/qrun`, POSIX sh gates.

---

## Three corrections to the spec, found writing this plan

Fix these in the spec in Task 1 Step 1 so the record agrees with the tree:

1. **Three increment sites, not two.** `rot_equality_check_boot()` at `acid_box.cpp:922` also does `s_rotEqFail++`, and it is a MISMATCH (a synchronous boot check of all 80 rows cannot starve). So mismatch has two sites (`:922`, `:964`) and starved one (`:935`).
2. **Field order is load-bearing.** The spec's example puts the new fields after `fail=`. The gate's second existing regex (`run_qemu.sh:210`) anchors on ` fail=0 us=[0-9]+\r?$` being CONTIGUOUS, so the order must be **`pass= mismatch= starved= fail= us=`** — components then total. That keeps `:210` and BOTH existing vacuity mutations working verbatim.
3. **One existing assertion is modified, not zero.** `run_qemu.sh:208` requires `pass=N fail=0 us=M` contiguous and breaks under any insertion. It is STRENGTHENED to pin the full new format — a tightening, never a weakening — and that is what makes it the RED-first step.

## Global Constraints

- `$EVKB` = `/Users/nicholasnewdigate/Development/rt1170/evkb`, `$AB` = `$EVKB/examples/display/acid_box`, `S=$(mktemp -d /tmp/n53.XXXX)`.
- Branch `new-53-rot-eq-split-counter` exists and holds the spec. Commit there. **Do not push.** Trailer: `Co-Authored-By: Claude Fable 5.1 <noreply@anthropic.com>`.
- Gate is `./run_qemu.sh`, **never `sh run_qemu.sh`**. **One QEMU at a time.** Run from the REAL path.
- **The goldens must not move**: `ACIDBOX_UI_SUM=0x18B7B637` after every rebuild. Movement means a pixel changed, which this change cannot do — stop and report.
- **Never weaken a gate assertion.** Strengthening `:208` is the only edit to an existing check.
- `transcript_qemu.txt` is a curated narrative AND the vacuity fixture: SPLICE (Procedure C below), never `cp`. No prose line may start with `ACIDBOX_ROT_EQ ` or `FAIL:`.
- `touch_script.txt` untouched (SHA-256 starts `d1bae9e7f117a6c1`).
- ITCM headroom is quantised to 16 B (CLAUDE.md, NEW-54 block): read a ±16 delta as noise.

## File Structure

| File | Responsibility | Task |
|---|---|---|
| `$AB/acid_box.cpp` | two counters replace one; three increment sites; the print; two comments | 1 |
| `$AB/run_qemu.sh` | `:208` strengthened; three new assertions; the `:121` example | 1 |
| `$AB/transcript_qemu.txt` | re-spliced capture; one narrative paragraph | 1 |
| `$EVKB/tools/gate-vacuity.test.sh` | two new negatives | 1 |
| `docs/superpowers/specs/2026-09-18-rot-eq-split-counter-design.md` | the three corrections | 1 |
| `CLAUDE.md`, Linear NEW-53, memory | close-out | 2 |

## Procedures

### Procedure A — build and headroom
```bash
cd "$AB" && cmake --build build 2>&1 | tail -6
itcm_headroom() { /Applications/ARM_10/bin/arm-none-eabi-size -A "$1" | awk '/^\.ARM\.exidx/{print 262144-($3+$2)}'; }
itcm_headroom "$AB/build/acid_box.elf"
```
Baseline `build` **2900**. Expect 2900 or 2916 (one 16-B quantum); anything else, say so.

### Procedure C — splice the transcript
```bash
python3 - "$AB/transcript_qemu.txt" "$AB/build/acid_box.uart" <<'EOF'
import sys
tpath, cpath = sys.argv[1:3]
t = open(tpath, encoding="utf-8").read().split("\n")
raw = open(cpath, "rb").read().replace(b"\r", b"").decode("utf-8", "replace")
cap = raw.split("\n")
if cap and cap[-1] != "": cap = cap[:-1]
while cap and cap[-1] == "": cap.pop()
i = t.index("==== captured UART ====") + 1
j = i
while t[j] != "": j += 1
assert t[i] == "=== BOOT ===" and cap[0] == "=== BOOT ===", (t[i], cap[:1])
print("replacing transcript lines %d..%d (%d lines) with %d capture lines" % (i + 1, j, j - i, len(cap)))
open(tpath, "w", encoding="utf-8").write("\n".join(t[:i] + cap + t[j:]))
EOF
```
Then `git diff` — outside the capture block only your intended narrative edit may appear.

### Procedure D — replay the fixture
```bash
cat > "$S/fake-qemu" <<'FAKE'
#!/bin/sh
target=""; prev=""
for a in "$@"; do case "$a" in file:*) [ "$prev" = "-serial" ] && target="${a#file:}" ;; esac; prev="$a"; done
if [ -n "${FAKE_CAPTURE:-}" ]; then [ -n "$target" ] && cat "$FAKE_CAPTURE" > "$target"; sleep 300; fi
exit 0
FAKE
chmod +x "$S/fake-qemu"
replay() { ( cd "$AB" && REAL_QEMU="$S/fake-qemu" FAKE_CAPTURE="$1" GATE_TIMEOUT=120 QRUN_TIMEOUT=40 ./run_qemu.sh 2>&1 | tail -2; ); }
```

---

### Task 1: The split — gate RED-first, firmware, fixture, vacuity

**Files:** `$AB/acid_box.cpp`, `$AB/run_qemu.sh`, `$AB/transcript_qemu.txt`, `$EVKB/tools/gate-vacuity.test.sh`, the spec.

- [ ] **Step 1: Correct the spec.** In `docs/superpowers/specs/2026-09-18-rot-eq-split-counter-design.md`: (a) §1's table gains a row for `:922` (boot check, mismatch); (b) §2's example line becomes `ACIDBOX_ROT_EQ pass=1696 mismatch=0 starved=0 fail=0 us=3303` with one sentence saying the order keeps `run_qemu.sh:210`'s ` fail=0 us=` anchor contiguous; (c) §4 says `:208` is STRENGTHENED to pin the full format and `:210` survives verbatim. Commit: `docs: NEW-53 spec corrections found writing the plan -- three increment sites, field order keeps the existing anchor contiguous, :208 strengthened not untouched`.

- [ ] **Step 2: Baseline.** Procedure A (expect 2900), then `cd "$AB" && ./run_qemu.sh 2>&1 | tail -1` → `PASS: acid box …`. Record `ACIDBOX_UI_SUM` from `build/acid_box.uart`: must be `0x18B7B637`.

- [ ] **Step 3: Gate first (RED).** In `$AB/run_qemu.sh`, replace the two lines

```sh
grep -qE "^ACIDBOX_ROT_EQ pass=[1-9][0-9]* fail=0 us=[0-9]+\r?$" "$OUT" \
    || { echo "FAIL: rotation equality guard line missing"; exit 1; }
```
with
```sh
# NEW-53: fail is the SUM of two things that mean different things, printed
# components-then-total so the " fail=0 us=" anchor below stays contiguous.
grep -qE "^ACIDBOX_ROT_EQ pass=[1-9][0-9]* mismatch=0 starved=0 fail=0 us=[0-9]+\r?$" "$OUT" \
    || { echo "FAIL: rotation equality guard line missing"; exit 1; }
```
Leave the next check (`grep -vqE " fail=0 us=[0-9]+\r?$"`) EXACTLY as it is. Directly after it, before `EQ_PASS=`, insert:

```sh
# NEW-53: WHICH failure.  `mismatch` = rot_eq_rows() found a sampled row where
# the PRESENTED buffer differs from a CPU rotation of the canvas -- the picture
# is wrong (acid_box.cpp:922 boot, :964 per bar).  `starved` = a check was still
# armed when the next was armed: a flip was pending on every pass it was given
# and it NEVER FINISHED (:935) -- loop() did not get enough flip-free passes.
# Both are failures (a guard that quietly stops finishing must not read like
# one that keeps passing), but a rendering defect and a scheduling shortfall are
# different faults, and until NEW-54's bench the counter could not say which.
# These run AFTER the fail=0 check above so the pre-existing vacuity mutation
# (fail=0 -> fail=1) still lands on the pre-existing message.
grep -E "^ACIDBOX_ROT_EQ " "$OUT" | grep -vqE " mismatch=0 " \
    && { echo "FAIL: rotation equality guard saw a MISMATCH -- the presented frame differed from the canvas"; exit 1; }
grep -E "^ACIDBOX_ROT_EQ " "$OUT" | grep -vqE " starved=0 " \
    && { echo "FAIL: rotation equality guard was STARVED -- a check never reached a verdict"; exit 1; }
# fail is derived (mismatch + starved) in the firmware, so this can only fire if
# a third increment path is added and the print is not updated -- a tripwire.
awk '/^ACIDBOX_ROT_EQ / {
        m = $0; sub(/.*mismatch=/, "", m); sub(/[^0-9].*/, "", m)
        s = $0; sub(/.*starved=/,  "", s); sub(/[^0-9].*/, "", s)
        f = $0; sub(/.*fail=/,     "", f); sub(/[^0-9].*/, "", f)
        if (m + s != f + 0) bad = 1
     } END { exit bad ? 1 : 0 }' "$OUT" \
    || { echo "FAIL: rotation equality guard fail != mismatch + starved"; exit 1; }
```
Also update the reap-cut example at `:121`: `"ACIDBOX_ROT_EQ pass=7 fail=0 us="` → `"ACIDBOX_ROT_EQ pass=7 mismatch=0 starved=0 fail=0 us="`. `sh -n run_qemu.sh` → clean.

- [ ] **Step 4: Run against UNCHANGED firmware.** `./run_qemu.sh 2>&1 | tail -1` → **`FAIL: rotation equality guard line missing`** (the old lines carry no `mismatch=`). That is the RED. Record it.

- [ ] **Step 5: Firmware.** In `$AB/acid_box.cpp`:
  - `:883` `static uint32_t s_rotEqPass = 0, s_rotEqFail = 0, s_rotEqUs = 0;` → `static uint32_t s_rotEqPass = 0, s_rotEqMismatch = 0, s_rotEqStarved = 0, s_rotEqUs = 0;`
  - `:922` (boot) `else s_rotEqFail++;` → `else s_rotEqMismatch++;`
  - `:935` (begin) `s_rotEqFail++;` → `s_rotEqStarved++;`
  - `:964` (step) `else s_rotEqFail++;` → `else s_rotEqMismatch++;`
  - the print:
```c
    CONSOLE.printf("ACIDBOX_ROT_EQ pass=%lu mismatch=%lu starved=%lu fail=%lu us=%lu\n",
                   (unsigned long)s_rotEqPass, (unsigned long)s_rotEqMismatch,
                   (unsigned long)s_rotEqStarved,
                   (unsigned long)(s_rotEqMismatch + s_rotEqStarved),   /* fail is DERIVED: it cannot drift from its parts */
                   (unsigned long)s_rotEqUs);
```
  - `rot_equality_begin()`'s comment: `and it is counted as a FAIL here, never` → `and it is counted as STARVED here (and so in fail, the sum), never`.
  - The witnesses' header (`:801`, the `ACIDBOX_ROT_EQ:` paragraph): append after `the port's host property test is the exhaustive one.`:
```
 *   fail= is the SUM of two counters that mean different things.  mismatch=
 *   is a real one -- a sampled row differed (the boot check, or a per-bar
 *   chunk).  starved= is a check that was still armed when the next was armed:
 *   a flip was pending on every pass it was given and it never reached a
 *   verdict.  Both are failures, but a wrong picture and a loop() that could
 *   not find a flip-free pass are different faults; NEW-54's bench reached
 *   fail=363 under knob and tempo gestures and could not say which (NEW-53).
```
  `grep -n s_rotEqFail acid_box.cpp` → no output.

- [ ] **Step 6: Build; golden; gate.** Procedure A (headroom 2900 or 2916 — say which). `./run_qemu.sh 2>&1 | tail -1` → `PASS`. `tr -d '\r' < build/acid_box.uart | grep -m1 "^ACIDBOX_UI_SUM="` → **`0x18B7B637`, unmoved**. `grep "^ACIDBOX_ROT_EQ" build/acid_box.uart | head -2` → lines in the new format, all zeros.

- [ ] **Step 7: Splice.** Procedure C. Then, immediately after the last `RE-RECORDED` paragraph, insert:
```
RE-RECORDED 2026-09-18 (NEW-53, first action -- the ROT_EQ split): the guard's
one `fail=` counter became `mismatch=` (a sampled row differed: the picture is
wrong) and `starved=` (a check still armed when the next was armed: it never
finished), with `fail=` kept as their SUM so every earlier capture stays
comparable and fail==mismatch+starved is a tripwire for a third increment path.
Printed components-then-total so the gate's existing " fail=0 us=" anchor stays
contiguous.  NO pixel changed: the golden is UNMOVED at 0x18B7B637, and this is
a re-capture only because the line format moved.  Why: NEW-54's bench read
fail=363 under gestures and could not say whether the frame was wrong or loop()
was starved -- the two land on the same counter (acid_box.cpp:922/:964 vs :935).
```
  `grep -c "^ACIDBOX_ROT_EQ .*mismatch=" transcript_qemu.txt` → 7 (all in the capture block).

- [ ] **Step 8: Replay.** Procedure D: `replay "$AB/transcript_qemu.txt"` → `PASS`.

- [ ] **Step 9: Vacuity negatives.** In `$EVKB/tools/gate-vacuity.test.sh`, directly after the `acb_guard_stops_after_boot_fails_by_name` case, insert:

```sh
    # NEW-53: the split.  Each component must be SHOWN to fire.  The mutation
    # bumps ONLY the component and leaves fail=0 -- deliberately inconsistent --
    # because bumping fail too would trip the pre-existing " fail=0 us=" check
    # first, with its pre-existing message, and the NEW check would never be
    # reached (NEW-50's lesson: know WHICH assertion caught the demo).  The sum
    # invariant needs no case of its own: both mutations here break it as a side
    # effect, and it runs after the component checks by design.
    awk '/^ACIDBOX_ROT_EQ pass=2 / && !done { sub(/mismatch=0/, "mismatch=1"); done=1 } { print }' \
        "$EVKB/$ACB/transcript_qemu.txt" > "$WORK/acb_mismatch.txt"
    run_gate "$ACB" "run_qemu.sh" "$WORK/acb_mismatch.txt"; rc=$?
    result=0
    [ "$rc" -ne 0 ] || result=1
    cmp -s "$EVKB/$ACB/transcript_qemu.txt" "$WORK/acb_mismatch.txt" && result=1
    echo "$OUT_TEXT" | grep -q "^FAIL: rotation equality guard saw a MISMATCH" || result=1
    report "acb_rot_eq_mismatch_fails_by_name" $result

    awk '/^ACIDBOX_ROT_EQ pass=2 / && !done { sub(/starved=0/, "starved=1"); done=1 } { print }' \
        "$EVKB/$ACB/transcript_qemu.txt" > "$WORK/acb_starved.txt"
    run_gate "$ACB" "run_qemu.sh" "$WORK/acb_starved.txt"; rc=$?
    result=0
    [ "$rc" -ne 0 ] || result=1
    cmp -s "$EVKB/$ACB/transcript_qemu.txt" "$WORK/acb_starved.txt" && result=1
    echo "$OUT_TEXT" | grep -q "^FAIL: rotation equality guard was STARVED" || result=1
    report "acb_rot_eq_starved_fails_by_name" $result
```
  Verify each in isolation with Procedure D's `replay` on the two mutated files: expect `FAIL: rotation equality guard saw a MISMATCH …` and `FAIL: rotation equality guard was STARVED …` respectively. Also replay the PRE-EXISTING mutation (`fail=0`→`fail=1` on a pass=2 line) and confirm it still prints `FAIL: rotation equality guard failed during the run` — the existing case is unbroken.

- [ ] **Step 10: Commit.**
```bash
cd "$EVKB" && git status --short   # exactly: acid_box.cpp, run_qemu.sh, transcript_qemu.txt, tools/gate-vacuity.test.sh
git add examples/display/acid_box/acid_box.cpp examples/display/acid_box/run_qemu.sh examples/display/acid_box/transcript_qemu.txt tools/gate-vacuity.test.sh
git commit -m "acid_box: split ROT_EQ fail into mismatch= and starved= with fail kept as the derived sum -- a wrong picture and a starved check no longer share a counter; gate pins the format and asserts each by name, two vacuity negatives, golden unmoved (NEW-53)

Co-Authored-By: Claude Fable 5.1 <noreply@anthropic.com>"
```

---

### Task 2: Close-out

- [ ] **Step 1: Vacuity, alone.** `cd "$EVKB" && sh tools/gate-vacuity.test.sh 2>&1 | tee "$S/vac.log" | grep -c "^PASS:"` → **70**; `grep "^FAIL:\|^SKIP:" "$S/vac.log"` → empty.
- [ ] **Step 2: Sweep.** Read `docs/KNOWN-BROKEN-GATES.md`. `./tools/run-all-qemu-gates.sh 2>&1 | tee "$S/sweep.log" | tail -3` → `gates: 141 passed`, exit 0. Any red not `display/acid_box`: re-run alone, idle, report by NAME.
- [ ] **Step 3: Audit, AFTER the sweep.** `LICENSE_AUDIT_EVKB=$(pwd) tools/license-audit.sh 2>&1 | tail -1` → `LICENSE-AUDIT: PASS`.
- [ ] **Step 4: `CLAUDE.md`.** In the NEW-54 block's `★★ ROT_EQ fail CONFLATES …` paragraph, append: `**SPLIT 2026-09-18 (NEW-53, first action)**: the line now reads `pass= mismatch= starved= fail= us=` with `fail` the derived sum; the gate asserts each component by name and the sum as a tripwire; `:922` (the boot check) was a third increment site the NEW-54 write-up missed, and it is a mismatch. Golden unmoved, vacuity 68 -> 70, sweep <the gates: line>. The next acid_box bench reads WHICH counter moves under NEW-54's arm-A gestures.` Commit the doc with the real sweep line in the message.
- [ ] **Step 5: Linear NEW-53** — append a short "First action DONE" note with the commit SHA and what the next bench must do. **Memory** — one paragraph in `memory/new54-acid-box-top-bar.md` under the `ROT_EQ fail` bullet: `SPLIT 2026-09-18 (branch new-53-rot-eq-split-counter): three increment sites, not two — the boot check at :922 is a mismatch.` Update its `MEMORY.md` hook.
- [ ] **Step 6: Stop.** Merge and the bench are the user's call.

---

## Self-review

- Spec §1 → T1 S1/S5; §2 format + why → T1 S1(b)/S3/S5; §3 firmware → T1 S5; §4 gate (three assertions, `:208` strengthened, `:210` verbatim) → T1 S3; §5 vacuity (two negatives, sum deliberately uncovered) → T1 S9; §6 goldens/ITCM/touch script → T1 S2/S6, constraints; §7 out of scope → nothing added; §8 acceptance → T2.
- Names consistent: `s_rotEqMismatch`, `s_rotEqStarved`, `acb_rot_eq_mismatch_fails_by_name`, `acb_rot_eq_starved_fails_by_name`, FAIL texts `saw a MISMATCH` / `was STARVED` / `fail != mismatch + starved`.
- No placeholders except `<the gates: line>`, which does not exist until Task 2 Step 2 measures it.
