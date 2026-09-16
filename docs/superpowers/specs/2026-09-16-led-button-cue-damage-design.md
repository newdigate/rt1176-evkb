# NEW-50 — narrowing `synthui_led_button`'s cue damage to the bezel band

Design, 2026-09-16.
Issue: <https://linear.app/newdigate/issue/NEW-50>

Widget: `~/Development/SynthUI` (`src/synthui_led_button.cpp`,
`src/synthui_led_button_math.h`, `tests/led_button_test.c`).
Consumers: `examples/display/synthui_led_button_test` (gate 141) and
`examples/display/acid_box` (the step lane, where cue is the playhead).

Predecessors: `2026-09-15-synthui-led-button-design.md` (the widget),
`2026-09-15-acid-box-synthui-editor-design.md` (the lane that made cue hot).

---

## 1. The defect

`synthui_led_button_set_cue()` calls `lv_obj_invalidate(obj)` — the whole
100 px key — although the cue changes only the bezel's border **colour and
width** (layer 1 of `led_draw`; `synthui_led_button.cpp:318-325`). The widget
spec chose that deliberately, and it was right for a bank of keys toggling
occasionally.

The acid_box editor made cue the **playhead**, moved twice per step by the
33 ms poller (`acid_box.cpp:1356-1357`: clear the old cell, set the new one).
The widget is now repainting ~10,000 px twice per step to move a 3.5-unit ring.

### 1.1 The evidence that it matters

Measured on silicon 2026-09-16 (transcripts in each example's
`transcript_hw_evkb.txt`):

* **acid_box touch latency**, two separate boots — percentiles cannot be
  differenced, so the two phases are RUNS, not windows within one:

  | transport | p50 | p95 |
  |---|---|---|
  | STOPPED | 29.3 ms | 37.8 ms |
  | PLAYING | 29.1 ms | **66.8 ms** |

  The median does not move. Only the tail does, and only when the playhead is
  running.

* **`synthui_led_button_test` Phase B**: **20.6 fps** against its `>= 30 fps`
  criterion (median frame 48.6 ms, four boots agreeing to 0.07 %), on a
  deliberately worst-case animation that sweeps the cue across all 16 keys
  every 15 ms.

**QEMU is blind to all of it.** The present pipeline is vsync-locked at 30 fps,
so flip counts are a ceiling, and the present path's damage AREA grew only
+0.08 % when the editor landed. Area was never the signal.

---

## 2. The mechanism, and one correction to the issue's framing

The issue attributes the cost to draw-task count (`led_draw` issues ~9 rects
where the `synthui_step` cell it replaced issued at most 4). Reading LVGL's
refresh path sharpens that into something quantitative — and partly refutes the
implied fix.

1. **`led_draw` issues 12 draw tasks, not 9.** The bezel's single
   `lv_draw_rect` produces **two** (`LV_DRAW_TASK_TYPE_FILL` and
   `LV_DRAW_TASK_TYPE_BORDER` — `lv_draw_rect.c`), then well, cap solid,
   cap_top, cap_low, highlight, halo border, LED, dot1, dot2, base.

2. **LVGL renders one pass per invalidated area** (`refr_area()`), re-walking
   the tree and re-entering `LV_EVENT_DRAW_MAIN` for every area.

3. **`lv_draw_rect` allocates a task before any clip test.** `lv_draw_add_task`
   mallocs unconditionally; the clip rejection happens per primitive, downstream
   of `execute_drawing` — `lv_draw_sw_fill.c:56` and `lv_draw_sw_border.c:97`
   each test the task's area against `t->clip_area` and return — i.e. *after*
   allocation, list insertion, `evaluate_cb` and dispatch.
   ★ **Corrected 2026-09-16, found in review:** this cited `lv_draw_sw.c:447`
   until a reviewer opened that file. Line 447 is inside `parallel_debug_draw()`,
   which is compiled out (`LVGL/port/lv_conf.h:611`, `LV_USE_PARALLEL_DRAW_DEBUG
   0`) — dead debug-overlay code, not the rejection path. The allocate-then-reject
   mechanism is unchanged; only the citation was wrong, and it had propagated into
   the plan, the gate comment and the Linear issue. **A grep hit is not a citation
   until you have read what encloses it.**

4. **LVGL's heap is the 1 MB pool in external SDRAM on a core with no D-cache**
   (`LVGL/port/lv_conf.h:117` + `LV_ATTRIBUTE_LARGE_RAM_ARRAY` →
   `.externalram`). That is precisely NEW-23's "~90 µs/task draw-task churn".

Consequence: **narrowing the box alone is a 4× pessimisation.** Four strips
would mean 4 passes × 12 unconditional tasks = **48** draw tasks where the
whole-key invalidation costs 12 — while every number the gate prints improves.
`led_draw` must become clip-aware in the same change.

With clip-awareness, each strip draws only bezel fill + bezel border + well:
**4 × 3 = 12 tasks, flat against today's 12.**

**So the win is pixel work, not task count.** Today a cue change repaints
~25,000 px of overlapping fills, gradients and antialiased edges across the
key; after, ~4,000 px in four thin strips. That is the honest expectation and
section 9 records it as a prediction to be refuted or confirmed on the bench.

Making `led_draw` clip-aware is not an invention: `synthui_level_meter`,
`synthui_piano_key`, `synthui_slide_toggle` and `synthui_seven_segment` all
test each layer against `layer->_clip_area` already. `led_button` is the one
widget in the set that does not.

---

## 3. Geometry — why four strips, and how deep

The bezel is a rounded rect of outer radius `R` with a border of width `bw`
drawn **inside** the area (LVGL's convention, which is why the math header
carries the OUTER radius). The changed pixels are the annulus between the
outer rounded rect and the inner one of radius `R − bw`.

The bounding box of a ring is the whole widget, so a single box wins nothing.
Four strips do, provided they are deep enough for the corners: on the flats the
ring reaches `bw` inward, but on the corner diagonal it reaches

```
band = R − (R − bw)/√2
```

Every band pixel satisfies `min(dx, dy) ≤ band` (the maximum of `min(dx,dy)`
over the annulus is attained on the diagonal), which is exactly the condition
four strips of depth `band` cover — top and bottom full width, left and right
the middle only.

The depth **must be computed from rounded pixel values**, not units: `R` is
what `led_radius(L.bezel_r)` hands LVGL and `bw` is `L.cue_bw_px`, both already
rounded. `cue_bw_px ≥ bezel_bw_px` at every scale (`lroundf` is monotone and
`3.5s ≥ 2s`, both floored at 1), so sizing on the cue width covers the narrow
ring as well as the wide one.

Computed from the real header (`SynthUI/src/synthui_led_button_math.h`, with a
+1 px antialiasing margin):

| key | R | cue_bw | bezel_bw | band | max box | total | whole key |
|---|---|---|---|---|---|---|---|
| 100×100 — lane cell and bank keys 0..10 | 17 | 4 | 2 | **9** | **900** | 3276 | 10000 |
| 96×96 — widget default | 16 | 3 | 2 | 8 | 768 | 2816 | 9216 |
| 150×150 — key 13 | 26 | 5 | 3 | 13 | 1950 | 7124 | 22500 |
| 120×80 — key 14 | 14 | 3 | 2 | 8 | 640 | 2304 | 6400 |
| 80×120 | 14 | 3 | 2 | 8 | 640 | 2304 | 6400 |
| 34×34 — key 12 | 6 | 1 | 1 | 4 | 136 | 480 | 1156 |
| 32×32 — key 11 | 5 | 1 | 1 | 4 | 128 | 448 | 1024 |

The four strips are **non-overlapping** (left and right span only the middle),
so `lv_refr_join_area` never even attempts a join — it requires
`lv_area_is_on`, and the strips do not touch. Had they overlapped, the join
would still have been rejected (the union is 10000 against a 1600 sum), but
non-overlapping also avoids painting each corner twice.

**A size-dependent property worth naming, because it is not universal.** At
100 px the band (9) clears the cap's inset (`cap.x1 = 10`), so a cue change
never repaints the cap group. At 32/34 px it does not (`band = 4`,
`cap.x1 = 3`): there the band is a large fraction of a small key and most
layers are reached. This is why the task-count bound is measured over the whole
scene rather than derived once at 100 px (section 5.1).

---

## 4. The change

### 4.1 `synthui_led_button_math.h`

One new function beside `lit_box()` / `press_box()`, plus one named constant:

```c
#define SYNTHUI_LED_BUTTON_CUE_MARGIN_PX 1   /* antialiasing slack */

/* Damage for a cue change: the bezel's border ring as FOUR NON-OVERLAPPING
 * strips -- out[0] top and out[1] bottom full width, out[2] left and out[3]
 * right the middle only.  Inclusive widget-relative pixels, like lit_box()
 * and press_box().  The bezel never moves, so there is no dy_px argument. */
void synthui_led_button_cue_boxes(float w, float h, synthui_led_button_px_t out[4]);
```

Depth:

```
R    = led_radius(bezel_r)          /* the radius LVGL is actually given */
bw   = cue_bw_px                    /* >= bezel_bw_px, so it covers both rings */
band = ceil(R - (R - bw)/sqrt(2)) + SYNTHUI_LED_BUTTON_CUE_MARGIN_PX
band = max(band, bw + MARGIN)       /* degenerate small-R keys */
band = min(band, half the shorter side)
```

The `+1` is slack for LVGL's antialiased border edge. It is **validated by the
delta-equality guard**, not by the comment claiming it: a margin too small
leaves stale pixels and the guard fails.

On a degenerate size (`w <= 0 || h <= 0`) the four boxes are zeroed, matching
`lit_box()`/`press_box()`'s existing behaviour.

### 4.2 `synthui_led_button.cpp`

1. `set_cue()` replaces `lv_obj_invalidate(obj)` with four
   `lv_obj_invalidate_area()` calls built from `cue_boxes()` through the
   existing `led_invalidate_px()` helper, so the widget-to-screen offset stays
   in one place.
2. `led_draw()` gains a `layer->_clip_area` intersection test per layer,
   following `synthui_slide_toggle.cpp`'s shape. **Without it the four strips
   cost 48 tasks** (section 2) — this is a correctness-of-cost requirement, not
   a tidy-up.

`set_disabled()` keeps its whole-key invalidate: it changes every layer's
colour, so a band would be wrong, and it is not on any hot path.

### 4.3 Explicitly not in scope

* No change to `lit_box()`, `press_box()`, the palette, or any drawn pixel.
* No GC355 compositor. Section 9 states the condition under which one becomes
  the honest answer, and it gets its own issue.
* No scene change in `synthui_led_button_test`, and no new tail step — see
  section 6.2 for why.

---

## 5. Instruments

### 5.1 Per-op draw-task count (new)

`synthui_led_button_test` sets `LV_OBJ_FLAG_SEND_DRAW_TASK_EVENTS` on its 16
keys and counts `LV_EVENT_DRAW_TASK_ADDED` into `s_delta_op`, beside the
existing `delta_inv_cb` area recorder. New line:

```
led_button_tasks_op lit=<n> press=<n> cue=<n> color=<n>
```

* **Metric: max tasks per single setter call** — each LCG step is one setter
  followed by `lv_refr_now`, so nothing accumulates across steps. This is the
  direct analogue of the per-op area bound.
* **Scope**: the flag is on the keys only, so the title label's tasks never
  enter the count. The library is untouched.
* **Pixel-neutral by construction**: `lv_draw_finalize_task_creation` sends the
  event and then dispatches exactly as before. The frozen goldens are what
  prove it rather than this paragraph.
* The event fires for every task the keys create: it is suppressed only while
  `info->task_running` is true, which happens only inside a callback that
  itself adds tasks, and this one does not.

**Pre-registered predictions** (to be measured, then pinned):

* cue on a 100 px key = **12** (4 strips × {bezel fill, bezel border, well}).
* cue on a 32/34 px key **higher — around 20** — because there the band reaches
  the cap group (section 3). The bound is therefore expected to be set by the
  *small* keys, not the large one.
* clip-unaware mutant = **48**.

If the measurement refutes these, the refutation is written down and the bound
re-derived from which layers intersect which strip at which size. **The bound
is never pasted over with whatever the run printed.**

### 5.2 Area bounds (re-derived)

| check | before | after | derivation |
|---|---|---|---|
| `cue` area | 10000 | **900** | 100 px key: bezel width 100 × band 9 |
| `lit`, `color` area | 1450 | 1450 | unchanged — the halo, 58×25 |
| `press` area | 6640 | 6640 | unchanged — the cap group at both offsets, 80×83 |
| overall `damage max` | 10000 | **6640** | the largest op is now `press`; 10000 would be a bound nothing can reach |

### 5.3 acid_box: a bound on full-frame presents

Eight invalidated areas per playhead step (2 cue changes × 4 boxes) against
`LV_INV_BUF_SIZE = 32`. On overflow LVGL invalidates the **whole screen**
(`lv_refr.c:328`), whose signature is "every golden green, everything slower" —
exactly the failure this change could introduce and nothing else would name.

acid_box already prints the witness: `ACIDBOX_ROT ops=<n> full=<n> px= us=
errors=`, where `full` counts full-frame presents. The gate asserts
`ops > full` but never bounds `full`. It gains an upper bound on `full`,
derived from the measured run (start-up forces two), so a `full` that climbs
per frame is named.

No other acid_box change: it is a pure consumer of the widget here, and its two
goldens are the pixel-identity proof.

---

## 6. Host suite (`SynthUI/tests/led_button_test.c`)

### 6.1 Additions

1. **Coverage.** Over a size sweep (square 16..200, plus 120×80, 80×120, 150,
   100, 96, 34, 32), model the band analytically — `inside(R) && !inside(R−bw)`
   on the rounded-rect outline, dilated 1 px for antialiasing — and assert
   every band pixel falls inside at least one of the four boxes. Run it for
   **both** `cue_bw_px` and `bezel_bw_px`: the change repaints the ring it is
   leaving as well as the one it is entering.
2. **Engagement.** Coverage alone is satisfied by a box the size of the whole
   key — the NEW-25 lesson, which applies here verbatim. So: **total ≤ 45 % of
   the key area for every size ≥ 64 px**. The worst case over that whole range
   was swept rather than guessed: **36.0 % at 80×85** (33 % at 100×100, 36 % at
   120×80, 32 % at 150×150), against 100 % for the whole-key mutant. The bound
   is deliberately not ⅓, which the real 100 px value clears by 57 px — a bound
   that tight is a false alarm waiting for a rounding change.

   It is **not** asserted below 64 px: there the band is legitimately a large
   fraction of a small key (`band = 4` on a 32 px key) and narrowing was never
   the point at that size. Nor is `band < R` asserted — it is false on tiny
   keys, where `R` rounds to 2 and the `bw + MARGIN` floor gives `band = 3`.
   The invariant that does hold everywhere is the clamp: `band ≤ half the
   shorter side`.
3. **Structure.** The four boxes lie inside the widget's pixel rect and do not
   overlap.
4. **Degenerate sizes** return zeroed boxes.

### 6.2 Mutants, RED before anything is trusted

| mutant | must fail as |
|---|---|
| `band = bw` (corner term dropped) | coverage, on the corner diagonal, by name |
| `band` = whole key | engagement |
| boxes built without `ox`/`oy` | coverage — **on 120×80 only** |

That last arm is what earns the non-square sweep, and it is why the non-square
key's coverage lives in the host suite rather than in the gate. Adding a cue
step to the scripted tail would not work: an ON→OFF pair **cancels** (the
uncovered corner pixels are repainted by neither half, so they stay correct and
the mutant survives), and leaving key 14 cued at the end would move the
final-state golden `0x463C3371`. The host sweep varies `w` and `h`
independently, which is strictly stronger coverage of `ox`/`oy` than one QEMU
key, and it keeps acceptance 1 (section 7) absolute.

---

## 7. Acceptance

1. **NO PIXEL MAY MOVE.** Four goldens stay put:
   `led_button_crc=0xD474F06D`, `led_button_fresh_crc=0x463C3371`,
   acid_box sw `ACIDBOX_UI_SUM=0xBB2AEE59` (QEMU) and acid_box gpu
   `0xEA5AB843` (bench, section 9). A narrower invalidation that changes a
   pixel is a bug, not a win.
2. **Delta equality holds unweakened.** `led_button_delta_eq=PASS` with
   `steps=64 tail=6` unchanged. It is what proves the narrower box leaves no
   stale pixels.
3. **Bounds re-derived from the math header, never loosened to the run**
   (section 5.2), with `lit`/`press`/`color` unmoved.
4. **Every new assertion demonstrated RED**, including the one the issue asks
   for by name.

### 7.1 Gate RED demos

| mutant | must fail as |
|---|---|
| cue band from `bw` alone | `FAIL: delta render differs from full render` |
| `set_cue` back to `lv_obj_invalidate(obj)` | `FAIL: cue damage above its box (cue=10000 > 900)` |
| clip tests deleted from `led_draw` | `FAIL: cue draw tasks above its bound (cue=48 > N)` — **with every golden and delta equality still GREEN** |
| a per-bar `lv_obj_invalidate(screen)` in acid_box's poller | the new `full` bound, by name |

The third row is the whole argument for the task counter existing: the
pessimisation is invisible to every other check in the gate, and to the
goldens.

---

## 8. Close-out process

1. SynthUI: implement, host suite green with its mutants demonstrated, **push**,
   record the SHA.
2. `evkb.cmake`: bump the SynthUI pin to that **pushed** SHA.
3. **Rebuild the seven self-building gate dirs before the sweep** —
   `bt_sink_test/build`, `bt_tone_test/build-{soak,lifecycle,media}`,
   `m2_hci_probe/build-{avdtp,baud,reconnect}` — or they reconfigure inside
   their 120 s budget and read as `exit status 124`. Rebuild the twelve
   SynthUI-linking dirs too.
4. Sweep **141/141/0**; then vacuity **62/62**; then `LICENSE-AUDIT`. One at a
   time — never concurrently. (An aborted vacuity run with a missing `$WORK`
   file means two overlapped.)
5. Fresh-user: `-DEVKB_FORCE_FETCH=ON`, verified by **RUNNING** the gate on the
   GitHub-fetched ELF, not by configuring one.
6. acid_box `--print-memory-usage` checked against the named 2,048 B ITCM floor
   (headroom is 2,884 B). Expected neutral — libSynthUI is flash-routed and
   `cue_boxes()` is inline header code — but checked, not argued.

---

## 9. What QEMU cannot answer, and what follows

**Whether 30 fps is reached.** The pipeline is vsync-locked at 30, so flip
counts are a ceiling and the fps question is meaningless there. The bench must
re-run both measurements of section 1.1 as **runs, not windows**:

* acid_box touch p95, transport STOPPED vs PLAYING, on two separate boots
  (percentiles cannot be differenced), against 37.8 / 66.8 ms.
* `synthui_led_button_test` Phase B fps, against 20.6.
* acid_box gpu golden `0xEA5AB843` re-confirmed, plus `ACIDBOX_VSYNC
  timeouts=0` and `ROT_EQ ... fail=0`.

**The prediction on record, so it can be refuted:** task count stays flat and
pixel work drops ~6×, so the gain is bounded by however much of the 48.6 ms
median frame is fill rather than per-task churn.

**If it does not clear 30 fps**, a GC355 compositor for `led_button` becomes
the honest answer and gets its own issue. NEW-23's fader is the precedent, but
it argues for one less strongly than it looks: that fader animates a cap that
genuinely moves, while this widget repaints to move a ring.
