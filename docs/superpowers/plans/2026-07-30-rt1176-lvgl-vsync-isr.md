# LVGL vsync ISR + touch test on double buffering (v5) — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** The flip fence becomes interrupt-signalled (first LCDIFv2 ISR on silicon) while
keeping v4's bounded degraded mode, and `lvgl_rk055_touch_test` migrates to double
buffering as the ISR's concurrency stress test.

**Architecture:** Spec `docs/superpowers/specs/2026-07-30-rt1176-lvgl-vsync-isr-design.md`
— read it first, especially §3.2's ownership table and §6.2's negative tests. Repos:
qemu2 (paint guard, I1), MipiDisplay (attach primitive, I2), LVGL (ISR fence, I3), evkb
(gate re-pins, touch migration, wrap).

**Verified facts (sanity-check, don't re-derive):**
- `IRQ_LCDIFV2 = 55` — `cores/imxrt1176/core_pins.h:50`; `attachInterruptVector`
  (`core_pins.h:80`); `NVIC_ENABLE_IRQ`/`NVIC_SET_PRIORITY` (`imxrt1176.h:167,172`);
  the core's default peripheral priority is **128** (`IntervalTimer.h:8`) — spec §10 Q1
  answered.
- `LCDIFV2_INT_ENABLE_D0`/`LCDIFV2_INT_STATUS_D0`/`LCDIFV2_INT_VSYNC`
  (`imxrt1176.h:2030-2076`). UNDERRUN and VS_BLANK share INT_STATUS — W1C **only** VSYNC.
- QEMU model: level IRQ gated on `status & enable` (`imxrt_lcdifv2.c:174-180`); the vsync
  timer fires ~60 Hz while the display is on. **Consequence: the ISR runs on EVERY vsync,
  not only when a flip is pending — raw ISR-entry counts are runtime-dependent and must
  NOT be pinned. The deterministic count is ISR *retires* (one per flip).** The spec's
  `VSYNC_ISR` token therefore counts retires; its §6.1 wording ("observed and retired
  every flip") already says exactly that.
- The touch example currently checksums `Display.framebuffer()`
  (`lvgl_rk055_touch_test.cpp:~300`) — WRONG under `create_db` (frame 1 renders into the
  alt buffer). The migration checksums `lvgl_mipi_panel_scanned_fb()` after an explicit
  `flip_sync`, exactly as the flip test does. The pixel CONTENT is unchanged, so the
  re-recorded golden may legitimately equal the old `0xE1559496` — record what is
  measured either way.
- v4 binding state to modify: `~/Development/LVGL/port/lvgl_mipi_panel.cpp:87-137`
  (state block, `flip_sync`, `db_flush_wait_cb`, `db_flush_cb`).
- Re-record rule (from `lvgl_rk055_panel_test/run_qemu.sh`): stable across two runs AND
  a human eye on the glass, **in the same commit** — so the touch migration's code+gate
  edits are HELD UNCOMMITTED until the Task-6 bench confirm, then committed together
  with the transcript.

**Conventions:** gates as `./run_qemu.sh` only; `uptime`/`ps` before gate runs; each repo
commits separately; **commit any fix before using `git checkout --` to revert a
temporary negative-test edit** (the v4 process scar); printf lines ≤ 127 chars.

---

## Task 1: QEMU model — the active-address paint guard (I1)

**Files:**
- Modify: `~/Development/qemu2/hw/display/imxrt_lcdifv2.c` (`imxrt_lcdifv2_get_layer0`)

- [ ] **Step 1:** In `imxrt_lcdifv2_get_layer0()`, fold the latch state into the enable
gate. The existing block computes `out->enabled` from `EN` + the SAFETY_EN rule; extend
it:

```c
    {
        uint32_t ab_mode = d5 & LCDIFV2_CTRLDESCL5_AB_MODE_MASK;
        bool safety_ok   = (ab_mode != 0) || (d5 & LCDIFV2_CTRLDESCL5_SAFETY_EN);
        /* No shadow load has latched yet -> nothing is on the glass.  This is
         * what the state-struct comment ("dark until the first load") promises;
         * before this guard the console paint path could read guest address 0
         * for the sub-frame window between DISP_ON and the first vsync tick.
         * Unreachable by any gate (-display none) -- comment-honesty, not gate
         * support.  It also makes the PANEL_SUM tap serve its sentinel until
         * the first latch, which is the faithful reading. */
        out->enabled = (d5 & LCDIFV2_CTRLDESCL5_EN) && safety_ok
                       && (s->layer0_active_addr != 0);
    }
```

- [ ] **Step 2:** Rebuild; re-run ALL SEVEN display/touch gates sequentially
(`rk055_panel_test rpi_panel_test lvgl_rk055_panel_test lvgl_rpi_panel_test
rk055_touch_test lvgl_rk055_touch_test lvgl_rk055_flip_test`), uptime/ps first. All
`EXIT=0`. Any red: STOP, understand, never weaken.

```bash
ninja -C ~/Development/qemu2/build qemu-system-arm
```

- [ ] **Step 3: Commit (qemu2, master):**

```
lcdifv2: nothing is on the glass until the first shadow load latches

get_layer0 now gates `enabled` on layer0_active_addr != 0, implementing
what the state-struct comment already promised -- the paint path could
read guest address 0 in the sub-frame window before the first latch.
Flagged by the v4 final review.  All seven display/touch gates green.
```

---

## Task 2: MipiDisplay — `lcdifv2AttachVsyncInterrupt` (I2)

**Files:**
- Modify: `~/Development/MipiDisplay/soc/lcdifv2.h` (declaration)
- Modify: `~/Development/MipiDisplay/soc/lcdifv2.cpp` (ISR + attach)

- [ ] **Step 1: Declaration** (append after the v4 primitives, same comment style):

```cpp
// Enable the LCDIFv2 vsync interrupt (INT_ENABLE_D0 bit 0, IRQ 55) and
// register the ONE callback it invokes.  The ISR lives here, in the
// controller's owner: it write-1-clears ONLY the VSYNC bit (UNDERRUN and
// VS_BLANK share INT_STATUS and belong to other diagnostics) and then calls
// `cb` -- WHICH RUNS IN INTERRUPT CONTEXT.  Keep it to flag work: no bus
// traffic, no LVGL calls, no printf.  A second attach replaces the first.
// The polled v4 primitives above never touch this path.
void lcdifv2AttachVsyncInterrupt(void (*cb)(void));
```

- [ ] **Step 2: Implementation** (append to the .cpp's v4 section):

```cpp
static void (*volatile s_vsync_cb)(void) = nullptr;

static void lcdifv2_vsync_isr() {
  // W1C ONLY the vsync bit -- UNDERRUN/VS_BLANK share this register and are
  // read by other diagnostics.  This is also what drops the level IRQ line.
  LCDIFV2_INT_STATUS_D0 = LCDIFV2_INT_VSYNC;
  void (*cb)(void) = s_vsync_cb;
  if (cb) cb();
}

void lcdifv2AttachVsyncInterrupt(void (*cb)(void)) {
  s_vsync_cb = cb;
  attachInterruptVector(IRQ_LCDIFV2, lcdifv2_vsync_isr);
  // 128 = the core's default peripheral priority (IntervalTimer's default).
  NVIC_SET_PRIORITY(IRQ_LCDIFV2, 128);
  NVIC_ENABLE_IRQ(IRQ_LCDIFV2);
  // Enable LAST: no interrupt can arrive before the vector and callback are
  // in place.
  LCDIFV2_INT_ENABLE_D0 |= LCDIFV2_INT_VSYNC;
}
```

- [ ] **Step 3: Compile proof** — clean rebuild + gate of `lvgl_rk055_flip_test`
(nothing calls the new function yet): build green, gate `EXIT=0`.

- [ ] **Step 4: Commit (MipiDisplay, master):**

```
lcdifv2: the vsync interrupt, owned here

lcdifv2AttachVsyncInterrupt(): vector + priority 128 + NVIC enable, then
INT_ENABLE last so nothing fires before the callback is in place.  The
ISR W1Cs only the VSYNC bit (UNDERRUN/VS_BLANK share INT_STATUS) and
calls the one registered callback in interrupt context.  First consumer
is the v5 LVGL flip fence.
```

---

## Task 3: LVGL binding — the ISR-signalled fence (I3a)

**Files:**
- Modify: `~/Development/LVGL/port/lvgl_mipi_panel.h` (accessor + doc updates)
- Modify: `~/Development/LVGL/port/lvgl_mipi_panel.cpp` (the v4 db section)

- [ ] **Step 1: Replace the v4 db state block and functions** (`.cpp:87-137` region)
with the ISR-signalled shape. The state block, with the ownership table:

```cpp
/* --- v5: ISR-signalled flip fence -----------------------------------------
 * OWNERSHIP TABLE -- single writer per field, and it is the design:
 *   field                thread writes            ISR writes
 *   s_db_pending_fb      set (flush_cb), clear    clear (retire)
 *                        (timeout abandon ONLY)
 *   s_db_scanned_fb      --                       set (retire)
 *   s_db_isr_retires     --                       increment
 *   s_db_flips           increment (flush_cb)     --
 *   s_db_vsyncs          increment (flip_sync)    --
 *   s_db_vsync_timeouts  increment (flip_sync)    --
 * Pointer stores are naturally atomic on ARMv7-M (aligned 32-bit).  The one
 * dual-writer field is pending_fb, and its two clears cannot both matter:
 * the timeout abandon runs 40 ms after the set, and an ISR retire racing
 * that exact store either wins (flip landed; the timeout tick miscounts one
 * "timeout" against a landed flip -- self-describing next to a healthy
 * s_db_isr_retires) or loses (true abandon).  Neither corrupts a pointer. */
static const uint16_t *volatile s_db_pending_fb = nullptr;
static const uint16_t *volatile s_db_scanned_fb = nullptr;
static volatile uint32_t s_db_isr_retires = 0;
static uint32_t s_db_flips = 0, s_db_vsyncs = 0, s_db_vsync_timeouts = 0;

/* INTERRUPT CONTEXT.  Flag work only: retire the pending flip.  Runs on
 * EVERY vsync (~60 Hz) once create_db has attached it; the retire fires only
 * when a flip is pending, and s_db_isr_retires counts RETIRES -- one per
 * flip, deterministic -- not ISR entries, which are runtime-dependent. */
static void db_vsync_isr()
{
    const uint16_t *p = s_db_pending_fb;
    if (p) {
        s_db_scanned_fb = p;
        s_db_pending_fb = nullptr;
        s_db_isr_retires++;
    }
}

void lvgl_mipi_panel_flip_sync()
{
    if (s_db_pending_fb == nullptr) return;
    /* Bounded wait on the ISR-retired flag -- v4's 40 ms bound, degraded
     * mode and counters survive the ISR migration unchanged; only the thing
     * polled moved from the device register into RAM the ISR owns. */
    const uint32_t t0 = micros();
    while (s_db_pending_fb != nullptr) {
        if ((uint32_t)(micros() - t0) > 40000u) {
            s_db_vsync_timeouts++;
            s_db_pending_fb = nullptr;   /* thread-side abandon; see table */
            return;
        }
    }
    s_db_vsyncs++;
}
```

`db_flush_wait_cb` is unchanged (calls `flip_sync`; its comment block stays verbatim).
`db_flush_cb`'s last-flush branch: drop the `lcdifv2VsyncArm()` line (the ISR consumes
the status bit now) and keep everything else, including FlipTo-before-pending-store and
the no-flush_ready deferral:

```cpp
        lcdifv2FlipTo((const uint16_t *)px_map);
        /* Pending-store AFTER FlipTo, same conservative direction as v4's
         * FlipTo-before-Arm: a vsync in the gap means the ISR sees no
         * pending flip and the retire lands one frame later -- an extra
         * frame of wait, never a false pass. */
        s_db_pending_fb = (const uint16_t *)px_map;
        s_db_flips++;
        s_frame_done = true;
```

`create_db` gains, after the buffer setup: `lcdifv2AttachVsyncInterrupt(db_vsync_isr);`
and resets `s_db_isr_retires = 0` with the other counters.
`lvgl_mipi_panel_scanned_fb()` returns through the volatile. New accessor:

```cpp
uint32_t lvgl_mipi_panel_vsync_isrs() { return s_db_isr_retires; }
```

Header: declare the accessor (doc: "flips retired BY THE ISR — one per flip,
deterministic; raw ISR entries are runtime-dependent and deliberately not counted"),
and update the create_db doc block's READ TIMING/degraded-mode paragraphs to say the
fence is ISR-signalled with the polled bound as consumer (keep the degraded-mode
sentence verbatim — it is still true).

- [ ] **Step 2: Commit (LVGL, master)** — commit BEFORE any negative-test edits:

```
lvgl_mipi_panel: the flip fence is ISR-signalled, wait-consumed

The LCDIFv2 vsync ISR (via lcdifv2AttachVsyncInterrupt) retires the
pending flip into volatiles the bounded flush_wait_cb consumes -- v4's
40 ms bound, timeout counter and degrade-to-unfenced mode survive
unchanged; only the thing polled moved from the device register into
RAM the ISR owns.  Ownership table at the site: single writer per
field, one documented exception (the timeout abandon).  The ISR never
calls LVGL.  s_db_isr_retires counts retires (deterministic, one per
flip), never ISR entries (runtime-dependent).
```

---

## Task 4: Flip gate re-pin + the two negative tests (I3b)

**Files:**
- Modify: `examples/display/lvgl_rk055_flip_test/lvgl_rk055_flip_test.cpp` (one print)
- Modify: `examples/display/lvgl_rk055_flip_test/run_qemu.sh` (one pin + comments)
- Update: `examples/display/lvgl_rk055_flip_test/transcript_qemu.txt`

- [ ] **Step 1:** In the example's counter block, add after the `VSYNCS=` print:

```cpp
    Serial1.printf("VSYNC_ISRS=%lu\n", (unsigned long)lvgl_mipi_panel_vsync_isrs());
```

In `run_qemu.sh`, after the `^VSYNCS=120$` line:

```sh
# One retire per flip, BY THE ISR -- interrupt delivery end-to-end
# (INT_ENABLE, NVIC, vector, W1C) under the modelled level IRQ.  Retires,
# not ISR entries: the ISR runs on every ~60 Hz vsync and raw entry counts
# are runtime-dependent -- pinning them would flake by design.
grep -q "^VSYNC_ISRS=120$" "$OUT" || { echo "FAIL: ISR did not retire every flip"; exit 1; }
```

Extend the VSYNCS derivation comment: the wait now consumes the ISR-set flag; the "if
this reads low" clause covers a dead ISR too.

- [ ] **Step 2:** Rebuild example, run gate twice — green ×2, counters byte-identical,
`VSYNC_ISRS=120`. If the value is stably ≠120, re-derive (retires must equal flips) —
that would mean a retire raced or was lost: STOP and understand before pinning anything.

- [ ] **Step 3: Negative test 1 (stale-buffer flip)** — the v4 edit, verbatim, in
`db_flush_cb` (LVGL repo; the fix is already committed so `checkout --` is safe now).
Rebuild, run: expect RED at `FLIP_B=MATCH`. Revert, verify LVGL tree clean.

- [ ] **Step 4: Negative test 2 (interrupt never enabled) — the spec's safety proof.**
In `~/Development/MipiDisplay/soc/lcdifv2.cpp`, temporarily comment out the
`LCDIFV2_INT_ENABLE_D0 |= LCDIFV2_INT_VSYNC;` line. Rebuild, run the gate. **Expected:
RED at `^VSYNC_TIMEOUTS=0$` — and the gate MUST COMPLETE rather than hang** (every wait
trips the 40 ms bound; the run degrades unfenced but finishes; tap sums likely mismatch
too — the failing token that matters is the timeout counter, note which token actually
fires first). If the gate hangs to its harness ceiling with no tokens, the degraded mode
did not survive: STOP EVERYTHING — the design's central safety claim is false. Revert,
verify MipiDisplay tree clean, rebuild.

- [ ] **Step 5:** Final green run; `cp lvgl_rk055_flip.uart transcript_qemu.txt`; commit
(evkb, branch — see Task 7 note on branching):

```
lvgl_rk055_flip_test: VSYNC_ISRS=120 -- the ISR retires every flip

Negative test 1 (stale flip): red at FLIP_B.  Negative test 2 (interrupt
never enabled): red at VSYNC_TIMEOUTS with the gate COMPLETING -- the
bounded degraded mode survived the ISR migration, measured not assumed.
```

Record both negative outcomes with their actual tokens in the commit body.

---

## Task 5: Touch-test migration (I4) — edits + QEMU, HELD UNCOMMITTED

**Files (all `examples/display/lvgl_rk055_touch_test/`):**
- Modify: `lvgl_rk055_touch_test.cpp` (create swap, checksum source, header caveat,
  corroboration prints)
- Modify: `run_qemu.sh` (golden re-record, corroboration asserts)

- [ ] **Step 1: The migration edits.**
- `lvgl_mipi_panel_create(Display)` → `lvgl_mipi_panel_create_db(Display)` (cpp:288).
- The pre-touch checksum block: after the frame_done render loop, add
  `lvgl_mipi_panel_flip_sync();` then feed the checksum from
  `lvgl_mipi_panel_scanned_fb()` instead of `Display.framebuffer()` — with a comment:
  under create_db the first frame renders into the ALT buffer; `framebuffer()` would
  checksum the stale zeroed scanout buffer and the assertion would silently stop
  covering the scene. (Guard: if `scanned_fb()` is null, print `SCENE_SUM_FAIL
  reason=no-flip` and bail — a flip that never landed must not checksum garbage.)
- Header comment: replace the "TEARING is expected and accepted (spec 8.1)" paragraph
  with: double-buffered as of v5 — the drag renders off-screen and flips at vsync
  (ISR-fenced); the v3 transcript's "very minor flickering" note is retired by
  re-verification on glass, and tearing language should no longer appear.
- After the `BUFFERS=` print, add corroboration prints:
  `FLIPS=`, `VSYNC_TIMEOUTS=` (from the binding accessors).

- [ ] **Step 2: Gate edits.** Replace the golden pin (`LVGL_SUM=0xE1559496`,
`run_qemu.sh:36`) with `0xRECORDME` + re-record provenance comment (the value may
legitimately come back identical — the pixel content is unchanged, only the buffer it
was rendered into moved; record what is measured). Add, in the honesty-guards section
(the IDLE_POLLS idiom for the non-zero check — spec §10 Q3 resolved):

```sh
# Flip corroboration (the touch phases are the claim; these say the fence ran).
# NOT pinned exactly: refresh count under touch load is input-dependent, and a
# pinned value here would be vacuous precision.
grep -q "^FLIPS=" "$OUT" || { echo "FAIL: flip count missing"; exit 1; }
grep -q "^FLIPS=0$" "$OUT" && { echo "FAIL: no flips -- the db path is not live"; exit 1; }
grep -q "^VSYNC_TIMEOUTS=0$" "$OUT" || { echo "FAIL: a vsync wait gave up"; exit 1; }
```

- [ ] **Step 3:** Rebuild, run the gate twice: red only at the RECORDME golden; every
other token green **byte-identical to v3's values** (`BTN1..5`, `DRAG_END x=598
moves=9`, `HOLD/TRAP`, `BUFFERS=27`, `POLL_FAILS=0`) — the GT911 path is untouched and
any drift there is a real regression from the binding change: STOP if so. Record the
stable measured `LVGL_SUM` ×2; pin it; run green ×1. **Do NOT commit** — the re-record
rule requires the on-glass confirm in the same commit (Task 6).

---

## Task 6: Hardware (I5) — flip counters, the finger ritual, and the commit

- [ ] **Step 1: Flip test on silicon.** Standard ritual (VCOM-free load/verify, console,
backgrounded run) with `build/lvgl_rk055_flip_test.elf`. Expect the QEMU counters
(`REFRESHES=FLIPS=VSYNCS=VSYNC_ISRS=120`, `VSYNC_TIMEOUTS=0`) — the first LCDIFv2
interrupt on silicon, at the real 58.7 Hz. Operator: eye on the sweep (still smooth, no
tearing). Update the flip test's `transcript_hw_evkb.txt` with this run (append a dated
v5 section; keep the v4 record).

- [ ] **Step 2: Touch test on silicon.** Flash the migrated build. Console FIRST, then
reset. The operator: confirm the scene on the glass (the golden's on-glass half), then
the full v3 ritual — buttons 1–5, the drag (**eye on the handle: the v3 "very minor
flickering" should be gone**), HOLD/TRAP with the lift order (place HOLD first). Expect
all v3 tokens + `FLIPS=` nonzero + `VSYNC_TIMEOUTS=0` + `LVGL_TOUCH_OK`. Remember the
first watch waits 10 minutes — still start promptly.

- [ ] **Step 3: The same-commit rule discharges here.** Write the touch test's new
`transcript_hw_evkb.txt` (append a dated v5 section, house format, operator words
verbatim including the drag observation), then commit EVERYTHING from Tasks 5+6
together (evkb):

```
lvgl_rk055_touch_test: double-buffered -- the ISR fence under real touch

create -> create_db; the pre-touch checksum now reads the SCANNED buffer
(framebuffer() would silently checksum the stale scanout buffer under
double buffering); golden re-recorded per the rule -- stable x2 in QEMU
and confirmed on glass in this same commit.  All v3 reaction assertions
held byte-identical.  The v3 drag flicker is gone on glass.
```

Plus the flip test's transcript update in its own small commit.

---

## Task 7: Wrap (I6)

**Branching note:** if this work was done on a feature branch (recommended:
`lvgl-vsync-isr-v5` — same flow as v3/v4), the wrap lands there and the merge follows
the finishing skill. Commits in Tasks 4/6 land on that branch.

- [ ] **Step 1:** Pushes + pins: MipiDisplay push → bump its SHA in `evkb.cmake`; LVGL
push → bump; qemu2 push. Force-fetch proof against `lvgl_rk055_touch_test`.
- [ ] **Step 2:** Docs: sweep expectation **stays 71** (no new example) — re-run the full
sweep anyway and confirm `71/0/0` (or `70/1/0` = cm4_audio_test); README capability rows:
update the double-buffer row (ISR-fenced as of v5) and the touch row (double-buffered);
no KNOWN-BROKEN-GATES count change, but append a dated line noting the flip/touch gates'
re-pins if their values moved.
- [ ] **Step 3:** Audit re-run (no new GATES entry needed — no new example; it must stay
green).
- [ ] **Step 4:** Commit the wrap; memory (new `rt1176-lvgl-vsync-isr` project memory +
update the v4 memory's "Next" line); **delete
`docs/superpowers/next-session-lvgl-vsync-isr-brainstorm.md`** — its outstanding tasks
are complete — in its own commit, per the established pattern.

---

## Self-review notes (kept for the executor)

- **Spec coverage:** §3.1→Task 2; §3.2→Task 3 (ownership table verbatim); §3.3→Task 5;
  §5→Task 1; §6.1→Task 4 Steps 1-2; §6.2→Task 4 Steps 3-4; §6.4→Task 6; §7 I1-I6→Tasks
  1-7. §10: Q1 answered (128), Q2 resolved (binding accessor `lvgl_mipi_panel_vsync_isrs`),
  Q3 resolved (IDLE_POLLS idiom).
- **Two deliberate spec refinements, recorded here rather than silently:** (a) the
  VSYNC_ISR token counts *retires* because raw ISR entries are runtime-dependent (~60 Hz
  free-running) — the spec's own wording "observed and retired every flip" is what the
  code implements; (b) the touch migration must move the checksum source to
  `scanned_fb()` — checksumming `framebuffer()` under create_db reads the wrong buffer,
  a defect the spec's "one-line create swap plus the consequences" glossed; the plan
  makes the consequence explicit.
- **The negative tests' revert safety:** the Task-3 LVGL commit and Task-2 MipiDisplay
  commit land BEFORE any temporary sabotage edits, so `git checkout --` reverts only the
  sabotage (the v4 scar's rule, applied).
