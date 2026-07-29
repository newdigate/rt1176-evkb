# RT1176 → LVGL `lv_indev` over the GT911 (v3: "a finger drives a widget") — Design

**Date:** 2026-07-29
**Status:** validated design, ready for an implementation plan
**Fulfils:** the "**v3** — LVGL `lv_indev` binding on top of v2" roadmap entry of
`2026-07-27-rt1176-rk055-display-design.md` (§2), on top of
`2026-07-28-rt1176-rk055-touch-design.md` (v2, complete and hardware-verified).

---

## 1. Goal

Bind the GT911 to LVGL as an **`lv_indev`**, so a finger drives real LVGL widgets on the
RK055HDMIPI4MA0 — and prove that **LVGL reacted**, which is a different claim from v2's "the
coordinates are right". A checksum cannot distinguish "the widget moved because of a touch" from
"the scene changed for some other reason"; §6 is built around assertions that can.

**Two facts found during design, worth stating up front:**

- **NXP's own LVGL binding has the Idle/Released conflation this project has now recorded four
  times.** Traced: `GT911_ReadRawTouchData()` returns `kStatus_TOUCHPANEL_NotReady` when the
  buffer-ready bit is clear (`fsl_gt911.c:261`); `GT911_GetSingleTouch()` collapses every
  non-success to `kStatus_Fail` (`fsl_gt911.c:290`); `DEMO_ReadTouch()` maps that to
  `LV_INDEV_STATE_REL` (`lvgl_support.c:575–580`). The reference implementation forwards **Idle
  as Released** and **an I²C fault as Released**. Facts transcribed with citation; the reference
  is consulted for register/behaviour facts only, never copied.
- **The LVGL tick is sane** (brief risk 4, closed): `lvgl_rt1176.cpp` sets
  `lv_tick_set_cb(millis)`, so indev press/release timing is real milliseconds.

---

## 2. Scope

**In scope (v3):**

- **`LVGL/port/lvgl_gt911_indev.{h,cpp}`** — the first input binding in the LVGL port.
- **`examples/display/lvgl_rk055_panel_test/`** — a *static* LVGL scene on the RK055 with a
  golden render checksum. This is the first LVGL example on this panel at all, and the first
  MIPI-DSI LVGL golden a human can actually confirm on glass (the RPi panel's golden pins
  reproducibility only — that panel is disconnected and hardware-blocked).
- **`examples/display/lvgl_rk055_touch_test/`** — the reactive scene and the touch gate proper.
- **qemu2**: an extension of the GT911 model's phase-3 script (§6.3) — stricter, never looser.

**Out of scope — named, not silently dropped:**

- **Gestures.** `LV_USE_GESTURE_RECOGNITION` stays 0.
- **Rotation.** v3 is portrait-only, and the binding *asserts* it (§4.3).
- **Interrupt-driven reads, and the INT line (D6) entirely.** v2 left two open findings on that
  line — `SWITCH1=0x05` (panel programmed falling-edge, firmware attached RISING) and
  `INT_EDGES > BUFFERS` with `POLL_FAILS=0` (predicted symptom of a floating input; no pull-up on
  `CTP_INT` in the RevC3 netlist). The v3 read path is polled, so the line buys nothing, and
  re-attaching a rising-edge interrupt to a chattering line would import an open failure domain
  into a gate whose claim is about LVGL. **v3 does not touch D6.** The findings stay open and
  belong to a scope-on-`GPIO_AD_00` session.
- **I²C fault-injection coverage for `GT911::read()`** (brief risk 6, checked 2026-07-29): not
  done, and stays out — it is a TouchPanel unit-test concern, orthogonal to the LVGL seam. The
  other risk-6 follow-up (the silent-failure-pattern sweep of gate runners) landed as
  `gate-vacuity.test.sh` (`da09eec`).
- **v4 double-buffering / vsync.** Tearing is accepted (§8.1).
- **The FT5406.** The seam it needs is the file boundary in `port/` (§3.2).

---

## 3. Architecture

### 3.1 Two examples, not one

The render golden and the touch-reaction assertions are different claims with different
lifetimes. A golden checksum wants a static scene; a touch gate wants a scene that moves. Splitting
them means the one thing only a human can check — "these pixels look right on this glass" — lives
in a gate that is not also moving under a finger, and the touch gate is free to assert on
reaction alone.

### 3.2 The binding lives in `LVGL/port/`, and takes a `GT911&`

Checked against the build (`evkb.cmake:201–218`): `import_evkb_lvgl()` compiles LVGL core plus
**only** `port/lvgl_rt1176.cpp` into the LVGL static library. Bindings are compiled by each
example into its own executable (`lvgl_rpi_panel_test/CMakeLists.txt`). So
`port/lvgl_gt911_indev.cpp` including `<gt911.h>` is invisible to every example that does not
list it — there is no library-level coupling to pay, and the precedent is exact:
`lvgl_ili9341_create(ILI9341_t3&)` already takes its concrete driver, and the two display
bindings coexist in `port/` each depending on a library the other ignores.

When the FT5406 arrives it becomes `port/lvgl_ft5406_indev.cpp` beside this one, and any shared
base is extracted *then*, from two real implementations (same reasoning as v2 spec §3.2).

**Rejected: a callback seam** (neither library knowing the other). It pushes the `Poll` enum
across a function-pointer boundary — and that flattening seam is precisely where all four
recorded instances of the Idle/Released bug happened. The enum's job is to make the distinction
uncollapsible; a design whose first act is to collapse it is worse than a named dependency.

### 3.3 Public surface

```cpp
// LVGL/port/lvgl_gt911_indev.h
lv_indev_t *lvgl_gt911_indev_create(lv_display_t *disp, GT911 &touch);

// Diagnostics, mirroring the display bindings' style (plain accessors, reset by create()):
uint32_t lvgl_gt911_idle_polls();   // polls that returned Poll::Idle
uint32_t lvgl_gt911_poll_fails();   // polls that returned Poll::Failed
```

`create()`:

- asserts `lv_display_get_rotation(disp) == LV_DISPLAY_ROTATION_0` (§4.3),
- creates the indev (`lv_indev_create`, `LV_INDEV_TYPE_POINTER`, `lv_indev_set_read_cb`,
  `lv_indev_set_display`),
- sets the indev's read timer to **10 ms** via
  `lv_timer_set_period(lv_indev_get_read_timer(indev), 10)` — a per-indev setting, so
  `lv_conf.h` is untouched and no other example's golden can shift (§5.2).

**Precondition (asserted in spirit, documented in the header):** `touch.begin()` must have
succeeded. `GT911::read()` before a successful `begin()` touches no bus and returns `Failed`
(driver contract), which the binding forwards as "no change" — safe, but a caller that skipped
`begin()` gets a dead pointer and a rising `poll_fails`, so the example checks `begin()` itself
and refuses to create the indev without it, exactly as the display examples check
`Display.begin()`.

---

## 4. The binding's behaviour

### 4.1 The read callback — the `Poll` distinction survives the seam

One `touch.read(pts, 5, &n)` per `read_cb` invocation, then a four-way branch. **This table is
the design;** the plan may not merge rows:

| `Poll` result | Forwarded to LVGL |
|---|---|
| `Contacts` | primary-tracking policy (§4.2); point scaled per §4.3; `LV_INDEV_STATE_PRESSED` while the primary is present |
| `Released` | `LV_INDEV_STATE_RELEASED`; primary cleared; panel-clear latch re-arms (§4.2) |
| `Idle` | **the latched previous state and point, unchanged** — "nothing new" is not an event |
| `Failed` | **also the latched state** — a bus glitch is not a touch-up; `poll_fails` increments |

Rows 3 and 4 are where NXP's reference is wrong at this same seam (§1), and row 4 closes at the
LVGL layer the same hole the v2 amendment closed inside the driver: a fault can never be
reported as a release, at either layer.

`data->continue_reading` stays false: one poll per LVGL read cycle. The GT911's own handshake
(it publishes nothing new until acknowledged) is the flow control; there is no queue to drain.

### 4.2 Multi-touch policy: one primary, no re-adoption until clear

`LV_INDEV_TYPE_POINTER` carries one point; the driver returns up to five contacts sorted by
track id. Policy:

- The first contact seen on an otherwise-clear panel becomes the **primary**, identified by its
  **track id** — not its array slot, which silently changes meaning when a lower id arrives
  (the driver's sort guarantees exactly that).
- While the primary's id is present in a `Contacts` result, its coordinates are the pointer;
  all other contacts are ignored.
- The poll in which the primary's id is absent reports `LV_INDEV_STATE_RELEASED` — even if
  other contacts remain.
- **No new primary is adopted until a poll reports zero contacts** (`Released`, or a `Contacts`
  count that has returned to zero — in practice the part publishes that as `Released`).

What this prevents: the pointer teleporting from a lifted finger onto a surviving one in a
single poll — a fabricated ~360 px fling that LVGL would obediently turn into a scroll or drag,
which is what the vendor reference's `points[0]` behaviour produces. What it costs, stated
honestly: after any multi-finger contact, **all** fingers must lift before the next touch
registers. On a single-pointer UI that is the correct reading of an ambiguous hand, and §6.3
makes it an asserted behaviour, not an accident.

### 4.3 Coordinate mapping, and the rotation assert

The mapping is a **scale only** — no swap, no mirror, no offset:

```
lv_x = raw_x * hor_res / touch.resolutionX()
lv_y = raw_y * ver_res / touch.resolutionY()
```

(Arithmetic transcribed as a fact from `lvgl_support.c:599–600`, the non-rotated GT911 path.)
v2 hardware-proved this identity mapping with a finger: five targets hit in order, hits 10–45 px
off centre, `SWITCH1` bit 3 (X2Y) = 0. The panel reports `RES=720x1280`; the binding scales by
the *reported* values and asserts nothing about them (v2 spec §5.4 discipline).

**Rotation is unavailable in this configuration, and the binding enforces that loudly.**
Checked against LVGL v9.4: `lv_display_set_rotation()` only stores the value and swaps the
reported resolution (`lv_display.c:934–941`); actual pixel rotation needs either
`LV_DRAW_TRANSFORM_USE_MATRIX` (0 in our `lv_conf.h:172`) or a rotation buffer that
direct-render mode has nowhere to put. Setting it would make LVGL render landscape geometry at
the panel's portrait stride — skewed pixels written into live scanout, the exact hazard
`lvgl_mipi_panel.cpp`'s static_asserts exist for — while the touch mapping silently pointed
somewhere else. And QEMU can never catch it, for the same shared-assumption reason as
orientation. So `lvgl_gt911_indev_create()` **asserts
`lv_display_get_rotation(disp) == LV_DISPLAY_ROTATION_0`**: the day someone wants landscape,
the failure is loud, at create time, at the site whose contract broke.

---

## 5. Timing: who polls, and why 10 ms

### 5.1 Poll inside `read_cb`; the part's handshake is the queue

LVGL polls input on its own timer; the binding polls the GT911 inside that callback. Nothing
can be dropped: the GT911 holds each published buffer until the host acknowledges it (the
mandatory status clear), and publishes nothing new meanwhile. The QEMU model enforces the same
handshake — it refuses to advance its script until the firmware clears `0x814E`.

**Rejected: polling from `loop()` into a latch that `read_cb` samples.** LVGL would then
subsample: a tap whose press and release both land between two LVGL reads vanishes entirely,
non-deterministically. Making that correct needs an event FIFO — machinery that rebuilds, worse,
the flow control the part's handshake already provides. It also splits the touch state across
two call sites, which is the shape all four recorded Idle/Released bugs grew in.

### 5.2 The read period is a verification decision, not a tuning knob

At LVGL's default 33 ms read period, the model (re-armed 20 ms after each ack) has a fresh
buffer waiting at **every** poll — `Idle` never occurs, and the §4.1 latch is dead code in the
gate. A binding that forwarded Idle as Released would pass perfectly: a vacuous green of
exactly the kind v2 caught four of.

At **10 ms** — roughly a real GT911's ~100 Hz report cadence — roughly every other poll is
`Idle`, *including mid-drag*. Forwarding those as releases shatters the scripted drag into ten
taps; the handle never leaves the left edge; the gate goes red (§6.2). The period is set
per-indev in `create()` (§3.3), so no `lv_conf.h` change and no other golden moves.

Two guards keep this honest:

- `IDLE_POLLS > 0` is asserted, so the latch assertion can never be satisfied vacuously by a
  timing drift that removes idle polls.
- `POLL_FAILS == 0` is asserted, so a dead bus cannot read as a quiet panel.

---

## 6. The examples and the gates

### 6.1 `lvgl_rk055_panel_test` — the render golden, finally on real glass

The `lvgl_rpi_panel_test` shape on the RK055: static scene (opaque background, montserrat
labels, fixed bar), `import_evkb_lvgl()` + `import_evkb_library(MipiDisplay soc panels/rk055)` +
PXP, direct-render via `lvgl_mipi_panel_create(Display)` — the binding is already panel-neutral,
which this example proves. `PANEL_INIT_BEFORE_SCANOUT=1` honoured (a panel property, per v1).

Tokens: `DISPLAY_OK`, `LVGL_FLUSHED=PASS`, `LVGL_BYTES=1843200` (720·1280·2), `LVGL_SUM=0x…`
golden. The golden is **recorded, not derived**, per the RPi gate's provenance discipline — and
the hardware pass has a human confirm the scene on glass, which upgrades this golden from
"deterministic" to "correct": the first MIPI-DSI LVGL golden in the tree with that property.

### 6.2 `lvgl_rk055_touch_test` — the scene is the assertion

Every widget exists to make one specific wrong binding fail. Layout is dictated by the QEMU
script's coordinates (percentages of 720×1280):

| Widget | Where | Asserts | Kills |
|---|---|---|---|
| Checkable buttons **1–5** (~200×160 px) | the five tap points: (108,128), (612,128), (108,1152), (612,1152), (360,640) | all five end **CHECKED** | stuck, mirrored, swapped, or unscaled coordinates — a fixed point checks at most one; this is real LVGL hit-testing and event delivery |
| **Drag handle** (~100×140 px, `LV_EVENT_PRESSING`-driven) | starts at the band's left, y≈640 | ends at **x≈648** having moved **≥8** times | Idle forwarded as Released — the drag shatters into ten taps and the handle never leaves the left edge |
| **HOLD** (checkable) | (180,640) — phase-3 primary | ends **CHECKED** | the second contact leaking through (pointer jumps off HOLD before release → no click) |
| **TRAP** (checkable) | (540,640) — phase-3 secondary | ends **UNCHECKED** | re-adoption of the surviving finger — a press the hand never made |

Plus `IDLE_POLLS>0`, `POLL_FAILS=0` (§5.2), and failure tokens that name the widget, the phase,
and the last coordinate seen (bare booleans stay banned).

**`TRAP=UNCHECKED` is vacuous alone** — a dead touch path satisfies it. It is asserted **as a
pair** with `HOLD=CHECKED` (which proves the phase-3 press reached LVGL at all), and the gate
says so in a comment.

**The scene checksum is demoted, not deleted.** The initial frame is rendered and checksummed
**before the indev is created** — static and golden on both QEMU and glass. It is safe by
construction: the GT911 model stalls its script until the first buffer is acknowledged, and
nobody polls until the indev exists. It asserts "the scene built correctly" and nothing about
touch. **No post-touch checksum is asserted at all** — reaction is proven by widget state and
position, not pixels.

### 6.3 The QEMU script extension — stricter, never looser

Phase 3's three two-contact instants appear and vanish together, so §4.2's no-re-adoption rule
is never exercised — TRAP would be decoration. The script gains **two instants carrying only
id 1** (at 75 %, the surviving finger) between the two-contact hold and the final release:

```
{2, {0,1}, {25,75}, {50,50}}   × 3      (existing)
{1, {1,0}, {75,0},  {50,0}}    × 2      (NEW — primary lifted, id 1 remains)
{0, …}                                  (existing final release)
```

This is a real hand doing a real thing, it is strictly *more* faithful, and it incidentally
tests that nothing in the stack assumes slot 0 means id 0. The change is allowed under the
project rule ("only to make it stricter or more faithful"). The existing `rk055_touch_test`
gate asserts tokens, not script internals — its phase 3 completes at `MULTI_OK` on the
two-contact instants and its final counters don't constrain the tail. **Confirmed by reading;
M2 re-runs the gate to confirm by measurement** before anything is built on top.

`IMXRT_GT911_SCRIPT_POINTS` stays 2; the build-time orderings
(`SCRIPT_POINTS ≤ CFG_POINTS ≤ MAX_POINTS`) are untouched.

### 6.4 What each gate is allowed to prove

| Claim | QEMU | Hardware |
|---|---|---|
| scene renders correctly | golden checksum (deterministic) | **a human eye on glass (correct)** |
| coordinates → LVGL point mapping | the scale arithmetic, against the model's blob | the real panel's blob; orientation (hardware-only by construction, as ever) |
| Idle is latched, not forwarded | **the drag assertion + `IDLE_POLLS>0`** — deterministic idle interleave at 10 ms | not provable: a finger cannot produce a deterministic idle-poll interleave |
| a fault is not a release | not provable: the model cannot fault I²C (v2 finding, unchanged) | not deliberately provable either; `POLL_FAILS=0` documents the run was clean |
| no re-adoption while a finger remains | **HOLD/TRAP pair against the extended script** | the same ritual done by a hand (§7) |
| tearing | invisible — no timing model | visible, accepted, documented (§8.1) |

### 6.5 Hardware ritual

Per the standing flashing discipline (VCOM-free `flash load` → `verify` → attach
`tools/rt1170-console.py` → reset via backgrounded `LinkServer run`; `pkill` all three probe
daemons first). Precondition checked, not assumed: RK055 on `J48`, RPi panel disconnected,
nothing on D6/D9 or J25 odd 13/15, no jumper from `irq_attach_test`.

Serial-prompted, like v2: tap buttons 1–5 in order; drag the handle left→right in one stroke;
place two fingers on HOLD and TRAP together, **lift the HOLD finger first**, then the other.
Same assertions as QEMU minus the idle-interleave claim; evidence is un-fakeable for the same
reasons as v2 (hit error ≠ 0, sample counts ≫ 8). Transcript lands in
`transcript_hw_evkb.txt`.

---

## 7. Decomposition

Each milestone red-then-green before the next begins.

| | Milestone | Ships |
|---|---|---|
| **M1** | **`lvgl_rk055_panel_test`.** | Example + `run_qemu.sh` + recorded golden; hardware pass with human confirmation on glass; both transcripts. |
| **M2** | **The script extension, proven harmless first.** | qemu2 change + the existing `rk055_touch_test` gate re-run green against it, *before* anything depends on the new instants. |
| **M3** | **The binding + the touch gate.** | `lvgl_gt911_indev.{h,cpp}`; `lvgl_rk055_touch_test` + `run_qemu.sh` green on every §6.2 token. |
| **M4** | **Hardware.** | The §6.5 ritual; `transcript_hw_evkb.txt`. |
| **M5** | **Wrap.** | Two `GATES` entries in `tools/license-audit.sh` (the drift check goes red first, by design); LVGL pin bump in `evkb.cmake` (the port gains files; local-first hides a stale pin — push and pin); docs; memory update. **Sweep expectation: 68 → 70**, `cm4_audio_test` intermittency rules unchanged (`docs/KNOWN-BROKEN-GATES.md` governs). |

Before every gate run: check `uptime` and `ps` for a competing runner (v2 lost real time to a
starved host mimicking a regression).

---

## 8. Risks

### 8.1 Tearing (accepted, bounded)

Direct render, no back buffer, no vsync fence — v1's stated trade, now with a widget dragging
across live scanout as the worst case. Accepted for v3 because: (a) it cannot corrupt
verification — QEMU asserts LVGL-side state/position, the only checksum is pre-touch, and the
hardware pass asserts where the handle *ended*, not how it looked mid-flight; (b) exposure is a
~100×140 px redraw against a 59 Hz scanout — a sliced edge for a frame on a moving widget;
(c) fixing it properly changes the display binding's contract (buffer flip + vsync fence),
invalidating the golden-checksum discipline — a full milestone, which is what v4 is. The
`lvgl_rk055_touch_test` header documents the artefact so nobody scopes a "flicker bug" that is
actually the design.

### 8.2 The 10 ms idle interleave is load-bearing for one assertion (low)

If a future model or LVGL change makes fresh buffers always available at 10 ms, the latch
assertion silently loses its subject. `IDLE_POLLS>0` is the tripwire: the gate fails loudly
instead of passing vacuously, and whoever trips it re-tunes the period, never deletes the
assertion.

### 8.3 The drag handle's motion depends on LVGL internals (low)

The handle follows the pointer via LVGL's own press-and-move delivery. If LVGL's internal
drag/scroll thresholds eat small motions, the scripted 64-px steps could under-deliver moves.
Mitigation: the handle is a plain object moved from `LV_EVENT_PRESSING` coordinates, not an
`lv_slider`, so no widget-internal threshold applies; the assertion is on the final position
(±half a step) and the move count (≥8), both of which survive coalescing.

### 8.4 Two goldens on one panel (low)

`lvgl_rk055_panel_test`'s golden and the touch test's pre-touch checksum are different scenes
and independent numbers; a font or `lv_conf.h` change moves both. That is the same class of
re-record event the RPi gate already documents — the provenance comment carries the rule.

---

## 9. Licence firewall

- LVGL is MIT; the new binding and both examples are MIT from their first commit.
- NXP's `lvgl_support.c` / `fsl_gt911.c` are BSD-3, consulted for **facts with citation** (§1,
  §4.3); no code structure copied. The §1 bug trace is analysis of reference behaviour, not code
  reuse.
- M5's `GATES` entries keep `tools/license-audit.sh`'s drift check honest — it fails red first
  when the examples land, which is the designed order.

---

## 10. Open questions for the plan stage

1. Exact widget geometry (the table's sizes are minima sized to v2's observed 10–45 px hit
   error; the plan fixes final rects so no two targets' hit areas overlap).
2. Whether the drag handle asserts intermediate monotonicity (x strictly increasing across
   moves) or only endpoint + count. Leaning endpoint + count: monotonicity re-proves the
   driver's ordering, which v2 already owns.
3. Whether `lvgl_gt911_indev_create()` returns `nullptr` or asserts when `touch.begin()` was
   skipped. Leaning assert, matching `lvgl_mipi_panel_create()`'s precondition style.
4. The touch example's serial prompt text for the hardware ritual (must name the lift order for
   HOLD/TRAP unambiguously).
