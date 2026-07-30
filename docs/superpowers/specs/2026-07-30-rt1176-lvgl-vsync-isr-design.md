# RT1176 → LVGL vsync ISR + the touch test goes double-buffered (v5) — Design

**Date:** 2026-07-30
**Status:** validated design, ready for an implementation plan
**Fulfils:** both follow-ons staged out of v4
(`2026-07-30-rt1176-lvgl-double-buffer-design.md` §2): the vsync-ISR refinement and the
migration of `lvgl_rk055_touch_test` to double buffering. Builds on v3 (touch, complete)
and v4 (double buffer, complete) — both hardware-verified and merged.

---

## 1. Goal

Graduate the flip fence from polled to **interrupt-signalled** — the first LCDIFv2
interrupt on silicon in this tree — without surrendering v4's bounded, loud degraded mode;
then put the touch test on the double-buffered path, which simultaneously retires its
documented tearing caveat and stress-tests the ISR under concurrent input load.

**Facts this design stands on, verified in source:**

1. **The real LCDIFv2 IRQ is 55 (`LCDIFv2_IRQn`)** — `cores/imxrt1176/imxrt1176.h:2012`
   (the model's "IRQ 55 follows domain 0" comment agrees with silicon, not the other way
   round). The core has the full raw-IRQ API: `attachInterruptVector` (`core_pins.h:80`),
   `NVIC_ENABLE_IRQ`, `NVIC_SET_PRIORITY` (`imxrt1176.h:167,172`).
2. **LVGL v9.4 designed `flushing` for ISR clearing** — `volatile int flushing; /* ...
   cleared from IRQ */` (`lv_display_private.h:83-85`) — but v5 deliberately does **not**
   use that path (§3.2): the ISR never calls LVGL at all.
3. **The QEMU model already delivers the interrupt**: level IRQ gated on
   `INT_STATUS_D0 & INT_ENABLE_D0` (`imxrt_lcdifv2.c:174-180`, `lcdifv2_update_irq`), so
   enabling the VSYNC bit delivers IRQ 55 in QEMU today and the ISR's write-1-clear drops
   the line. Interrupt *delivery* is QEMU-provable; latency and context are not (§6.3).
4. `INT_STATUS_D0` is shared: VSYNC (bit 0), UNDERRUN (bit 1), VS_BLANK (bit 2)
   (`imxrt1176.h:2076-2078`). The underrun diagnostics elsewhere in the tree read this
   register — the ISR must W1C **only** the VSYNC bit.

---

## 2. Scope

**In scope (v5):**

- **`lcdifv2AttachVsyncInterrupt(void (*cb)(void))`** in MipiDisplay `soc/lcdifv2`
  (§3.1) — the controller's owner owns its interrupt.
- **`lvgl_mipi_panel_create_db()` switches internally** to the ISR-signalled fence
  (§3.2). No API fork: the polled v4 primitives (`lcdifv2VsyncArm/Seen`) remain in
  MipiDisplay for other callers, but the binding stops polling the device register.
- **`lvgl_rk055_flip_test`** re-pins its counters and re-measures its negative tests
  against the ISR path, plus one new negative test (§6.2).
- **`lvgl_rk055_touch_test` migrates to `create_db`** — pre-touch golden re-recorded
  under the re-record rule (stable ×2 + a human eye on glass, in the same commit), full
  bench ritual re-run (§6.4).
- **The v4 model-debt fix**: the QEMU paint path gains the `layer0_active_addr != 0`
  guard its own state-struct comment promises ("dark until the first load") — a
  more-faithful three-liner, regression-proven like every model change.

**Out of scope — named, not silently dropped:**

- **`lv_display_flush_ready()` from the ISR** (the pure-ISR shape). Rejected for v5, not
  deferred vaguely: it deletes the bounded degraded mode v4 shipped — a dead vsync line
  becomes an unbounded `while (disp->flushing);` spin inside LVGL, a hung UI with no
  token. This tree degrades loud; it does not hang silent. If the residual wait is ever
  *measured* to matter, that measurement opens the next session.
- **PXP-accelerated drawing, XRGB8888** — unclaimed milestones, unchanged.
- **The v2 INT-line findings** (GT911, D6). This ISR is the *LCDIFv2's* interrupt — no
  shared pin, no shared failure domain. The scope-on-`GPIO_AD_00` session remains open.

---

## 3. Architecture

### 3.1 MipiDisplay owns the interrupt

```cpp
// Enable the LCDIFv2 vsync interrupt (INT_ENABLE_D0, IRQ 55) and register the
// one callback it invokes.  The ISR lives HERE, in the controller's owner:
// it write-1-clears ONLY the VSYNC bit (UNDERRUN/VS_BLANK share the register
// and belong to other diagnostics) and then calls `cb` -- which runs in
// INTERRUPT CONTEXT and must behave accordingly.  One consumer; a second
// attach replaces the first.  Never called by the polled v4 path.
void lcdifv2AttachVsyncInterrupt(void (*cb)(void));
```

Implementation: `attachInterruptVector(IRQ_LCDIFV2, isr)`, NVIC priority at the core's
default-peripheral level (the plan pins the exact value from a sibling user like
IntervalTimer), `NVIC_ENABLE_IRQ`, then `LCDIFV2_INT_ENABLE_D0 |= LCDIFV2_INT_VSYNC` —
enable last, so no interrupt can arrive before the vector is set.

### 3.2 The binding: ISR-signalled, wait-consumed

**The ISR never calls LVGL.** The callback does the minimum interrupt-context work:

```
isr callback:  if (a flip is pending AND the shadow load has LATCHED --
                   the self-clearing SHADOW_LOAD_EN bit is the ground truth):
                   { scanned_fb = pending_fb; pending_fb = null; retires++; }
               (retires, not entries, is the counted quantity: the ISR runs on
                every ~60 Hz vsync and raw entry counts are runtime-dependent.
                The latch guard closes a tens-of-cycles IRQ-propagation window
                found by the final review -- a stale vsync's interrupt arriving
                after the pending store must not retire a flip that latches
                only at the NEXT vsync.)
```

`flush_wait_cb` keeps its v4 role and its v4 guarantees — the 40 ms bound, the
`vsync_timeouts` counter, the documented degrade-to-unfenced mode — but now polls the
**ISR-retired volatile** instead of the device register. The flip protocol is otherwise
v4's, including its two load-bearing orderings:

- last flush: `FlipTo(px_map)` **then** the pending-store, **no `flush_ready`** (LVGL
  clears `flushing` itself after `flush_wait_cb` returns — the v4 lesson, unchanged). A
  vsync landing between `FlipTo` and the pending-store means the ISR sees no pending flip
  and the retire happens one frame later — one extra frame of wait, never a false pass:
  the same conservative direction as v4's FlipTo-before-Arm.
- **Every cross-context static becomes `volatile`** (`pending_fb`, `scanned_fb`, the
  counters) — the change the v3/v4 "not volatile, single thread" comments pre-marked for
  exactly this milestone. Single-writer disciplines per field: the ISR writes
  `scanned_fb`/`pending_fb`-clear/`vsync_isr_count`; the thread writes `pending_fb`-set
  and everything else. No read-modify-write races by construction; the plan documents the
  per-field ownership table at the site.

**Degraded mode, restated because it survives:** a vsync that never comes trips the 40 ms
bound in `flush_wait_cb`; `vsync_timeouts` increments; the pending flip is abandoned; the
frame renders unfenced (v1 behaviour) until vsync recovers. A dead interrupt is a loud
counter, not a hang — and §6.2's new negative test proves that path by construction.

### 3.3 The touch test on `create_db`

A one-line create swap plus the consequences: the pre-touch scene golden changes (the
scene now renders into the alt buffer first) and is re-recorded under the re-record rule;
every v3 reaction assertion (BTN1..5, DRAG_END, HOLD/TRAP, IDLE_POLLS, POLL_FAILS,
BUFFERS) is unchanged in meaning and expected to hold byte-identically — the GT911
handshake is untouched. The touch gate additionally grows the flip counters as
corroborating tokens (FLIPS>0, VSYNC_TIMEOUTS=0), *not* as pinned exact values: refresh
count under touch load is input-dependent, and pinning it would be a vacuous-precision
assertion.

---

## 4. What the migration retires, and what it must not disturb

Retired: the touch example's header caveat ("TEARING is expected and accepted") and the
v3 transcript's standing note that v4 owns the fix. Must not disturb: the v3 evidence
(transcripts stay as history), the GT911 driver, the indev binding, the 10 ms read
period, and the QEMU touch script — none of them change. The touch gate's phase-3b
re-adoption assertions run exactly as before, now over ISR-fenced flips.

---

## 5. QEMU model change

One three-line fix (the v4 review's item): `lcdifv2_update_display` (the console paint
path) gains `layer0_active_addr != 0` in its gating, implementing what the state-struct
comment already promises. Unreachable by any gate (`-display none`) — this is
comment-honesty enforcement, not gate support. Regression duty as always: all seven
display/touch gates re-run green before anything builds on the tree.

No other model change: interrupt delivery (§1 fact 3) already exists.

---

## 6. Verification

### 6.1 Flip gate, re-derived

Existing assertions unchanged in meaning (`FLIP_A/B=MATCH` pairs, `DISTINCT=OK`,
`REFRESHES=FLIPS=120`, `VSYNC_TIMEOUTS=0`, `FLIP_OK`); the counter comments re-derive:

- **`VSYNC_ISR=120`** (new token): the ISR observed and retired every flip — interrupt
  delivery end-to-end (INT_ENABLE, NVIC, vector, W1C handshake) under the modelled level
  IRQ.
- `VSYNCS=120` keeps its v4 meaning (landings consumed by the wait path) — via a
  thread-owned consumed-counter lagging the ISR's retire counter, so the retire is
  STICKY: a wait arriving a whole refresh after the vsync still consumes that landing
  (v4's latched INT_STATUS read was implicitly sticky; the first ISR build lost that and
  the pin caught it at VSYNCS=3). The v4 history paragraph in the gate comment stays; its "if this reads
  low, that regression is back" clause now also covers a dead ISR.

### 6.2 Negative tests (measured red before the green is trusted)

1. **Stale-buffer flip** (v4's) — re-measured against the ISR path; expected red at
   `FLIP_B=MATCH`.
2. **NEW: interrupt never enabled** — temporarily skip the `INT_ENABLE_D0` write in
   `lcdifv2AttachVsyncInterrupt`. The ISR never fires; the waits trip the 40 ms bound
   **with the gate completing rather than hanging** — the proof that the degraded mode
   survived the ISR migration. (As measured: the example bails at the first MISMATCH, so
   the red lands at a MATCH token with `VSYNC_TIMEOUTS=1` in the transcript — one
   timeout, not one per frame; the completing-not-hanging property is the claim.) If the
   run hangs to the harness timeout instead, the central safety claim is false: STOP.

### 6.3 What each side is allowed to prove

| Claim | QEMU | Hardware |
|---|---|---|
| interrupt delivery (enable → vector → W1C) | **the modelled level IRQ; `VSYNC_ISR=120`** | the same counters on silicon |
| ISR latency, preemption, real interrupt context | fiction (a 60 Hz timer in virtual time) | **the only proof — first LCDIFv2 ISR on silicon** |
| bounded degrade on a dead line | **negative test 2, by construction** | not deliberately provoked |
| touch reactions under ISR-fenced flips | the v3 script assertions, unchanged | **the full finger ritual, re-run** |
| no tearing on the drag | invisible | the operator's eye, again |

### 6.4 Hardware ritual

Flip test first (counters + `VSYNC_ISR` on silicon, eye on the sweep), then the migrated
touch test: the full v3 bench ritual — buttons 1–5, the drag (eye on the handle: the v3
transcript's "very minor flickering" should now be gone), HOLD/TRAP with the lift order —
plus the golden's on-glass confirmation for the re-record. Transcripts in both examples,
house format.

---

## 7. Decomposition

| | Milestone | Gate |
|---|---|---|
| **I1** | Model: the `active_addr != 0` paint guard. | All seven display/touch gates re-run green. |
| **I2** | `lcdifv2AttachVsyncInterrupt` in MipiDisplay. | Compile proof; exercised for real by I3. |
| **I3** | Binding: ISR-signalled fence, volatiles, per-field ownership. Flip gate re-pinned (`VSYNC_ISR=120`), negative tests 1+2 measured red. | The re-pinned flip gate. |
| **I4** | Touch-test migration + golden re-record (stable ×2; on-glass confirm rides with I5). | The touch gate, all v3 assertions + flip corroboration. |
| **I5** | Hardware: flip counters, then the full finger ritual. | Both transcripts. |
| **I6** | Wrap: MipiDisplay/LVGL pushes + pin bumps, qemu2 push, sweep expectation **unchanged at 71** (no new example — re-measure anyway), docs, memory, **delete the session brief**. |

---

## 8. Risks

1. **ISR-context bugs are invisible to QEMU's timing** (highest). The model's interrupt
   fires from a coarse timer in a single-threaded event loop — real preemption points do
   not exist there. Mitigations: the single-writer field discipline (§3.2), the minimal
   ISR body (no LVGL, no bus traffic — the v3 GT911 reentrancy rule by construction), and
   the touch stress test on silicon.
2. **The golden re-record masks a real render change.** Mitigated by the rule itself:
   stable ×2, human eye, same commit — plus every *reaction* assertion staying green.
3. **Priority interactions.** IRQ 55 joins a system with audio/DMA interrupts. v5 uses
   the core's default peripheral priority; the ISR is ~10 instructions. If the bench
   shows touch or audio anomalies, priority tuning is a finding to document, not a quiet
   tweak.
4. **A second attach caller.** The primitive documents replace-not-stack semantics; the
   binding is today's only caller.

---

## 9. Licence firewall

All firmware-side work MIT in its home repos; the qemu2 guard is GPL-2.0-or-later inside
qemu2 as ever, never compiled into firmware. No new example, so no new `GATES` entry —
the audit's drift check stays satisfied by the existing entries (re-run green at wrap).

---

## 10. Open questions for the plan stage

1. The exact NVIC priority value (read it from IntervalTimer or another in-core
   `attachInterruptVector` user; do not invent one).
2. Whether `vsync_isr_count` should be exposed through the binding
   (`lvgl_mipi_panel_vsync_isrs()`) or printed by the example from a MipiDisplay
   accessor — leaning binding accessor, symmetric with the existing counters.
3. Whether the touch gate's new flip-corroboration tokens are `FLIPS>0` via grep-negation
   (the IDLE_POLLS idiom) or a firmware-computed `FLIPS_OK` token — leaning the
   IDLE_POLLS idiom, already proven in that gate.
