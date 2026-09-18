# acid_box ROT_EQ: split `fail` into `mismatch` and `starved` — design

Date: 2026-09-18
Status: approved (brainstorm 2026-09-18)
Tracking: Linear **NEW-53**, first action.
Follows: `2026-09-17-acid-box-synthui-top-bar-design.md` (NEW-54), whose bench produced the
finding this closes — see `examples/display/acid_box/transcript_hw_evkb.txt`, NEW-54 section.

## 1. The problem, measured

`ACIDBOX_ROT_EQ`'s `fail` counter is incremented from **three places** in `acid_box.cpp`, and they
mean two different things:

| site | meaning | implication |
|---|---|---|
| `:922` | `rot_equality_check_boot()` — the SYNCHRONOUS boot check found a sampled row that differed. It compares all 80 rows in one call, so it cannot starve; this site is a mismatch, and the NEW-54 write-up missed it | **the picture is wrong** |
| `:964` | `rot_eq_rows()` found a sampled row where the PRESENTED buffer differs from a CPU rotation of the canvas | **the picture is wrong** |
| `:935` | a check still armed when the next was armed — "a flip was pending on every pass it was given", so it never reached a verdict (`rot_equality_step()` returns early whenever `isrs + timeouts < flips`) | **the check never finished** — `loop()` did not get enough flip-free passes |

A rendering defect and a scheduling shortfall land on the same number. The NEW-54 bench could not
separate them: `us=` is indistinguishable between the regimes (~3170..3321 failing against
~3175..3323 clean, so passes WERE running), `pass` kept incrementing while `fail` climbed, and 239
lines carried `flips!=isrs` — exactly the condition the starvation path needs. That bench's arm A
reached `fail=363` under knob-drag and tempo-press gestures, i.e. `loop()` load, so **starvation is
at least as plausible there as a rendering fault**.

Consequence, and the reason this is NEW-53's first action: **no acid_box `ROT_EQ fail>0` reading in
this tree's history is causal** — NEW-53's own `fail=5` (2026-09-16) included, which has been
carried as a suspected *rendering* defect ever since.

Counting a starved check as a failure is CORRECT and stays: `rot_equality_begin()`'s comment says a
check that quietly stops finishing would otherwise read exactly like one that keeps passing. The
defect is not that starvation counts — it is that it counts *indistinguishably*.

## 2. The change

One line moves:

```
ACIDBOX_ROT_EQ pass=1696 mismatch=0 starved=0 fail=0 us=3303
```

The order is **components then total**, and that is load-bearing rather than aesthetic: the gate's
second existing assertion (`run_qemu.sh:210`) anchors on ` fail=0 us=[0-9]+\r?$` being CONTIGUOUS, so
putting the new fields ahead of `fail=` keeps that regex — and both existing vacuity mutations —
working verbatim.

`fail` is **kept as the sum**, and that is the design decision rather than a convenience:

- every existing assertion (`run_qemu.sh:208/210`) and both existing vacuity mutations keep working
  unmodified, so the split cannot silently weaken what is already proven;
- captures stay comparable across the change — including NEW-54's two committed arms
  (`bench-captures/new54-witness-*.csv`) and NEW-53's `fail=5`;
- **`fail == mismatch + starved` holds BY CONSTRUCTION** — the firmware derives `fail` in the print
  and keeps no `s_rotEqFail`, so the total cannot disagree with its parts and there is nothing for a
  gate to check. *(corrected during implementation: the gate check was unreachable behind the
  component checks.* This bullet originally claimed the equality was a checkable invariant and a
  tripwire for a future third increment path; both halves were wrong — a derived total cannot drift,
  and the check was written, found unreachable, and deleted. Keeping `fail` is justified by the two
  bullets above on their own.*)*

Rejected: `pass= mismatch= starved= us=` with no `fail`. Tidier, but it rewrites every reader and
assertion and proves nothing new.

## 3. Firmware

Two counters replace one; `s_rotEqFail` becomes a derived sum, not stored state:

- `:922` (`rot_equality_check_boot`, the synchronous boot check) increments `s_rotEqMismatch`.
- `:935` (`rot_equality_begin`, the "still active" branch) increments `s_rotEqStarved`.
- `:964` (`rot_equality_step`, the `!s_rotEqOk` branch) increments `s_rotEqMismatch`.
- The print emits `fail=` as `mismatch + starved`, so the sum cannot drift from its parts by
  construction — there is no third variable to forget to update.

Both live beside `s_rotEqPass` in the same `ROTWIT_FN` (flash) region; no ITCM-resident code grows.
The witnesses' header comment gains the two meanings, because the whole point is that a future
reader knows which number they are looking at.

## 4. Gate

`run_qemu.sh:208` is **STRENGTHENED** — it required `pass=N fail=0 us=M` contiguous, which any
insertion breaks, and it is re-pinned to the FULL new format
(`pass=N mismatch=0 starved=0 fail=0 us=M`). That is a tightening, never a weakening, and it is what
makes the gate edit the RED-first step: the unchanged firmware fails it by name. `run_qemu.sh:210`
(` fail=0 us=` anchored at end of line) survives VERBATIM, which is what the field order in §2 buys.

Alongside that, the gate adds:

- a `mismatch=0` assertion, failing by name — **the rendering-correctness check**;
- a `starved=0` assertion, failing by name — **the scheduling check**.

*(corrected during implementation: the gate check was unreachable behind the component checks.* A
third assertion — the sum `fail == mismatch + starved` — was listed here, written, and then deleted:
by the time it ran, the `fail=0` check at `:210` and the two new component checks had pinned all
three fields on every line to 0, so `0 + 0 == 0` and it could never fire. `run_qemu.sh` carries a
comment in its place saying why there is no check.*)*

*(corrected during review: the component checks were unreachable on real data behind the `fail=0`
check.* Both by-name checks are placed **BEFORE** the pre-existing ` fail=0 us=` check, not after it.
`fail` is the derived sum, so a real mismatch or starve carries `fail>0` on the same line and the
`fail=0` check fires first — behind it, the by-name checks could only fire on a capture whose `fail`
disagreed with its components, which the firmware cannot print. Measured with the original ordering:
captures shaped `mismatch=1 … fail=1` and `starved=1 … fail=1` both printed the generic "failed
during the run". The `fail=0` check stays behind them as the historic catch-all.*)*

★ **This cannot make the gate more load-sensitive**, and that is worth stating because this tree has
six gates in a documented load-sensitivity class. `fail=0` already implies `mismatch=0` and
`starved=0`, and that assertion has passed across every sweep to date. The new checks are a strict
refinement of one that already holds, not a new demand on the host.

## 5. Vacuity

Both existing acid_box cases are unaffected (they mutate `fail=`, which survives). Two negatives are
added so each component is SHOWN to fire rather than assumed — the suite's own doctrine:

- `acb_rot_eq_mismatch_fails_by_name` — `mismatch=0` → `mismatch=1` on a per-bar line;
- `acb_rot_eq_starved_fails_by_name` — `starved=0` → `starved=1` on a per-bar line.

*(corrected during review: the component checks were unreachable on real data behind the `fail=0`
check.* Each mutation bumps `fail` **together with** its component, so the mutated line is one the
firmware could actually print — `fail` is the derived sum. The first version bumped the component
alone, leaving `fail=0`, which was a workaround for the ordering defect above rather than a
property of the fixture; with the checks reordered it is neither needed nor honest.*)*

Each carries the suite's `cmp` mutation guard. Vacuity **68 → 70**.

There is no third vacuity case because there is nothing to mutate: the firmware derives `fail` from
its two parts in the print, so the sum cannot disagree with them and no gate asserts it. *(corrected
during implementation: the gate check was unreachable behind the component checks.* This paragraph
originally said the sum needed no case because the two mutations above break it as a side effect.
That is true and immaterial — the component check fires first, so the sum check was never evaluated
on a broken line at all. The honest reason is the one above: the invariant is a property of the
firmware's print, not a claim a gate could test.*)*

## 6. What must NOT move

- **The goldens.** This is a console-line change; no pixel is touched. `ACIDBOX_UI_SUM` must stay
  `0x18B7B637` (QEMU) after a rebuild. If it moves, something changed that should not have — stop.
- **ITCM headroom**, beyond the 16-byte quantum (see the NEW-54 CLAUDE.md block: the metric can only
  report multiples of 16, so ±16 B is noise and only a multiple of 32 is signal).
- **`touch_script.txt`** — SHA-256 still starts `d1bae9e7f117a6c1`.
- Gate count stays **141**; vacuity goes to 70.

## 7. Out of scope

Anything that attempts to FIX the starvation, and any change to when a check is armed or skipped.
This change makes the next bench able to say WHICH fault it saw; it does not chase either. NEW-53's
finding A (the deterministic 288 ms frame) is untouched.

## 8. Acceptance

QEMU: gate green with the new assertions, goldens unmoved, sweep 141/0/0, vacuity 70/70, audit PASS.
Both new negatives demonstrated RED by name.

*(added during review: the WIRING is demonstrated in QEMU after all.)* A green run leaves both
counters at 0, so no green gate can show that `:929`/`:971` feed `mismatch` and `:942` feeds
`starved` — swapping them would go red nowhere. The `f7` recipe closes that: the LVGL port made to
forget the previous present's damage (`rot_flush_cb` passing 0 for `nprev`), acid_box rebuilt, the
real gate run. Six per-bar checks read `mismatch=1..6 starved=0`, the boot golden held at
`0x18B7B637`, and the gate named the mismatch path — where the same recipe pre-split could only say
"failed during the run". Port reverted, gate green again, LVGL clean at its pin. Recorded in
`transcript_qemu.txt` (RE-RECORDED 2026-09-18 and the `f7` entry). The STARVED path remains
QEMU-unreachable and stays a silicon claim.

Silicon (NEW-53, not this change): re-run NEW-54's arm A — playing plus knob/tempo gestures, ~50 min
past bar 1700 — and read which counter moves. That single reading decides whether NEW-53 is a
rendering defect or a `loop()` scheduling problem, which nothing in the tree can currently say.
