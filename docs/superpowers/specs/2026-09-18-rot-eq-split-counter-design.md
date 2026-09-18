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
- **`fail == mismatch + starved` becomes a checkable invariant.** A third increment path added later
  breaks the equality and the gate names it. Replacing `fail` outright would lose that tripwire.

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
- a `starved=0` assertion, failing by name — **the scheduling check**;
- the sum invariant `fail == mismatch + starved`, failing by name.

★ **This cannot make the gate more load-sensitive**, and that is worth stating because this tree has
six gates in a documented load-sensitivity class. `fail=0` already implies `mismatch=0` and
`starved=0`, and that assertion has passed across every sweep to date. The new checks are a strict
refinement of one that already holds, not a new demand on the host.

## 5. Vacuity

Both existing acid_box cases are unaffected (they mutate `fail=`, which survives). Two negatives are
added so each component is SHOWN to fire rather than assumed — the suite's own doctrine:

- `acb_rot_eq_mismatch_fails_by_name` — `mismatch=0` → `mismatch=1` on a per-bar line;
- `acb_rot_eq_starved_fails_by_name` — `starved=0` → `starved=1` on a per-bar line.

Each carries the suite's `cmp` mutation guard. Vacuity **68 → 70**.

The sum invariant deliberately gets no vacuity case: the mutations above each break it as a side
effect, so a dedicated one would prove nothing the two already prove. This is recorded rather than
left to look like an omission.

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

Silicon (NEW-53, not this change): re-run NEW-54's arm A — playing plus knob/tempo gestures, ~50 min
past bar 1700 — and read which counter moves. That single reading decides whether NEW-53 is a
rendering defect or a `loop()` scheduling problem, which nothing in the tree can currently say.
