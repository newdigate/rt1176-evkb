# Wedge-delta damage Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** `synthui_rotary_knob` angle changes damage only the old∪new index-wedge
boxes (sw) and composite scissored to them (gpu) — the measured 4× lever —
guarded by an equality test and an engagement test in `synthui_knob_test`.

**Architecture:** Spec `docs/superpowers/specs/2026-08-27-rotary-knob-delta-damage-design.md`.
Core gains `delta_clip[6]`/`delta_n` (plain `lv_area_t`, LVGL-only);
`set_angle` computes 5-sample wedge bboxes (pad 4 px) and invalidates those,
escalating to full on overflow; every other invalidation clears `delta_n`.
Compositor draws per-rect with `vg_lite_set_scissor`, unscissored when
`delta_n==0`. Existing goldens must not move; only new tokens are gated.

**Tech stack / repos:** SynthUI (`~/Development/SynthUI`, local-first) +
this repo, branch `nicnewdigate/new20b-rotary-knob-delta-damage`.
`build-fps-before/` in vglite_lvgl_test holds the PRE-change FPSBENCH ELF
for the silicon before/after.

---

### Task 1: Widget core — delta fields + set_angle (SynthUI)

**Files:**
- Modify: `~/Development/SynthUI/src/synthui_rotary_knob_private.h`
- Modify: `~/Development/SynthUI/src/synthui_rotary_knob.cpp`

- [ ] **Step 1: Private struct** — add to `synthui_rotary_knob_t` after
  `gpu_pending` (and a `#define SYNTHUI_ROTARY_DELTA_MAX 6` above the
  struct):

```c
    /* Wedge-delta damage (angle-only changes): the rects invalidated since
     * the last composite/full-invalidate. delta_n > 0 means every dirty
     * rotor pixel lies inside these rects, so the GPU composite may scissor
     * to them; 0 means full-control damage (composite unscissored). The sw
     * renderer needs none of this -- LVGL clips its draw to the damage. */
    lv_area_t delta_clip[SYNTHUI_ROTARY_DELTA_MAX];
    uint8_t delta_n;
```

- [ ] **Step 2: Core implementation** — in `synthui_rotary_knob.cpp`:
  constructor zeroes `delta_n`. Add above the setters:

```c
/* Bbox of the +/-8 deg index wedge (ring sector r16..36) at `deg`, padded
 * 4 px for AA. 5 samples across the 16 deg span at both radii bound the arc
 * to <0.1 px at every supported size, and the pad is constant because AA
 * is. NOTCH-ONLY property: the discs are rotationally invariant, so this
 * box is the EXACT damage of an angle change (spec section 2). */
static void wedge_bbox(const synthui_rotary_knob_t *k, float deg,
                       lv_area_t *a)
{
    lv_area_t coords; lv_obj_get_coords((lv_obj_t *)&k->obj, &coords);
    const float W = (float)lv_area_get_width(&coords);
    const float H = (float)lv_area_get_height(&coords);
    const float S = (W < H ? W : H) / 100.0f;
    const float cx = (float)coords.x1 + W * 0.5f;
    const float cy = (float)coords.y1 + H * 0.5f;
    float minx = 1e9f, miny = 1e9f, maxx = -1e9f, maxy = -1e9f;
    for (int i = 0; i <= 4; i++) {
        const float d = deg - 8.0f + 4.0f * (float)i;
        for (int r = 0; r < 2; r++) {
            const float rad = (r ? 36.0f : 16.0f) * S;
            const float x = cx + rad * sinf(d * RK_DEG);
            const float y = cy - rad * cosf(d * RK_DEG);
            if (x < minx) minx = x;
            if (x > maxx) maxx = x;
            if (y < miny) miny = y;
            if (y > maxy) maxy = y;
        }
    }
    a->x1 = (int32_t)minx - 4; a->y1 = (int32_t)miny - 4;
    a->x2 = (int32_t)maxx + 4; a->y2 = (int32_t)maxy + 4;
}
```

  Replace `set_angle`'s `RK_SETTER` with:

```c
void synthui_rotary_knob_set_angle(lv_obj_t *obj, float deg)
{
    LV_ASSERT_OBJ(obj, MY_CLASS);
    synthui_rotary_knob_t *k = (synthui_rotary_knob_t *)obj;
    if (k->angle == deg) return;
    /* Wedge-delta damage: an angle change moves ONLY the index wedge (the
     * discs are rotationally invariant), so damage old+new wedge boxes
     * instead of the whole control -- measured 4x on the all-16 workload.
     * Escalate to a full invalidate when the per-refresh rect budget is
     * spent; escalation is always correct, delta is only an optimization. */
    if (k->delta_n + 2 <= SYNTHUI_ROTARY_DELTA_MAX) {
        lv_area_t a;
        wedge_bbox(k, k->angle, &a);
        k->delta_clip[k->delta_n++] = a;
        lv_obj_invalidate_area(obj, &a);
        wedge_bbox(k, deg, &a);
        k->delta_clip[k->delta_n++] = a;
        k->angle = deg;
        lv_obj_invalidate_area(obj, &a);
    } else {
        k->delta_n = 0;
        k->angle = deg;
        lv_obj_invalidate(obj);
    }
}
```

  Every other invalidation site clears the pending delta — `RK_SETTER` gains
  `k->delta_n = 0;` before its `lv_obj_invalidate`, and the same line goes
  into `set_range` and `rk_input_state` (the press/release repaint).

- [ ] **Step 3: Compile check + host tests** —
  `cmake --build ~/Development/rt1170/evkb/examples/display/synthui_step_test/build`
  clean; `tests/run.sh` green.
- [ ] **Step 4: Commit (SynthUI)** —
  `git commit -m "rotary: wedge-delta damage on angle changes (notch discs are rotation-invariant)"`

### Task 2: Scissored GPU composite (SynthUI)

**Files:**
- Modify: `~/Development/SynthUI/src/vglite/synthui_rotary_knob_gpu.cpp`

- [ ] **Step 1: Per-rect scissor** — in `render_ready_cb`, replace the
  per-instance draw block (matrix setup unchanged) with:

```c
        /* Delta damage: the sw pass repainted ground+well only inside the
         * stored rects, so composite the rotor ONLY there -- unscissored
         * redraw would re-blend the body rim's AA over its own previous
         * blend and drift toward pure body colour (spec section 4).
         * delta_n == 0 is a full invalidation: composite unscissored. */
        if (k->delta_n > 0) {
            for (uint8_t r = 0; r < k->delta_n; r++) {
                const lv_area_t *c = &k->delta_clip[r];
                GPU_TRY(vg_lite_set_scissor(c->x1, c->y1,
                                            c->x2 + 1, c->y2 + 1));
                for (int p = 0; p < 3; p++)
                    GPU_TRY(vg_lite_draw(&s_target, &s_paths[p],
                                         VG_LITE_FILL_NON_ZERO, &m,
                                         VG_LITE_BLEND_SRC_OVER, col[p]));
            }
            GPU_TRY(vg_lite_set_scissor(-1, -1, -1, -1));
            k->delta_n = 0;
        } else {
            for (int p = 0; p < 3; p++)
                GPU_TRY(vg_lite_draw(&s_target, &s_paths[p],
                                     VG_LITE_FILL_NON_ZERO, &m,
                                     VG_LITE_BLEND_SRC_OVER, col[p]));
        }
```

  ★ Verify the driver's scissor edge convention against
  `VGLite/vg_lite.c` (`right/bottom` exclusive vs inclusive — the `+ 1`
  above assumes lv_area inclusive x2 → exclusive right). The Task-3 equality
  guard is the behavioral check; adjust the `+ 1` if it fails by one-pixel
  seams.
- [ ] **Step 2: Build the VGLITE consumer** —
  `cmake --build examples/display/synthui_knob_test/build` clean.
- [ ] **Step 3: Commit (SynthUI)** —
  `git commit -m "rotary gpu: scissor the composite to the delta rects (exact, no AA rim drift)"`

### Task 3: Guards in synthui_knob_test + gate

**Files:**
- Modify: `examples/display/synthui_knob_test/synthui_knob_test.cpp`
- Modify: `examples/display/synthui_knob_test/run_qemu.sh`
- Re-capture: `examples/display/synthui_knob_test/transcript_qemu.txt`

- [ ] **Step 1: Self-test phase** — after the accent sum, before
  `rk_gpu_err`/`SYNTHUI_KNOB_DONE`, add (file-scope pieces first):

```c
/* --- wedge-delta guards (spec 2026-08-27-rotary-knob-delta-damage) ------- */
static int32_t s_delta_maxarea = 0;
static bool    s_delta_record = false;
static void delta_inv_cb(lv_event_t *e)
{
    if (!s_delta_record) return;
    const lv_area_t *a = (const lv_area_t *)lv_event_get_param(e);
    const int32_t px = lv_area_get_width(a) * lv_area_get_height(a);
    if (px > s_delta_maxarea) s_delta_maxarea = px;
}

/* Two knobs (two S values, both well types); angle sequence with a wrap and
 * a detent-sized jump, one refresh per step, plus a mid-sequence state
 * toggle (exercises delta->full escalation) AFTER area recording stops. */
static uint32_t delta_run(bool fresh_only)
{
    lv_obj_t *scr = lv_obj_create(NULL); opaque_bg(scr);
    lv_obj_t *k1 = make_knob(scr, SYNTHUI_ROTARY_MODE_ENDLESS,
                             SYNTHUI_ROTARY_THEME_LIGHT, LV_STATE_DEFAULT,
                             0.0f, 150);
    lv_obj_set_pos(k1, 60, 300);
    lv_obj_t *k2 = make_knob(scr, SYNTHUI_ROTARY_MODE_BOUNDED,
                             SYNTHUI_ROTARY_THEME_LIGHT, LV_STATE_DEFAULT,
                             0.0f, 250);
    lv_obj_set_pos(k2, 320, 300);
    static const float seq[] = { 30.0f, 95.0f, 350.0f, 10.0f, -120.0f, 78.75f };
    const float last = seq[(sizeof seq / sizeof seq[0]) - 1];
    if (fresh_only) {
        /* full render straight at the final state: the reference picture */
        synthui_rotary_knob_set_angle(k1, last);
        synthui_rotary_knob_set_angle(k2, last);
        lv_obj_add_state(k2, LV_STATE_FOCUSED);
        return sum_screen(scr);
    }
    lv_screen_load(scr);
    lv_refr_now(NULL);                    /* settle the initial full render */
    lv_obj_add_event_cb(k1, delta_inv_cb, LV_EVENT_INVALIDATE_AREA, NULL);
    lv_obj_add_event_cb(k2, delta_inv_cb, LV_EVENT_INVALIDATE_AREA, NULL);
    s_delta_maxarea = 0; s_delta_record = true;
    for (unsigned i = 0; i < sizeof seq / sizeof seq[0] - 1; i++) {
        synthui_rotary_knob_set_angle(k1, seq[i]);
        synthui_rotary_knob_set_angle(k2, seq[i]);
        lv_refr_now(NULL);
    }
    s_delta_record = false;               /* the toggle below SHOULD be big */
    lv_obj_add_state(k2, LV_STATE_FOCUSED);
    lv_obj_invalidate(k2);                /* programmatic-state contract */
    lv_refr_now(NULL);
    synthui_rotary_knob_set_angle(k1, last);
    synthui_rotary_knob_set_angle(k2, last);
    lv_refr_now(NULL);
    lvgl_sum_reset();
    lvgl_sum_feed(Display.framebuffer(), PANEL_FB_BYTES);
    return lvgl_sum_value();
}
```

  and in `setup()` after the accent sum:

```c
    /* Wedge-delta guards: the sequence render must be PIXEL-IDENTICAL to a
     * fresh full render of the final state (the gate compares the two), and
     * the recorded per-step damage must stay wedge-sized. */
    const uint32_t d_seq  = delta_run(false);
    const uint32_t d_full = delta_run(true);
    Serial1.printf("KNOB_DELTA_SEQ=0x%08lX\n", (unsigned long)d_seq);
    Serial1.printf("KNOB_DELTA_FULL=0x%08lX\n", (unsigned long)d_full);
    Serial1.printf("KNOB_DELTA_EQ=%s\n", d_seq == d_full ? "PASS" : "FAIL");
    Serial1.printf("KNOB_DELTA_MAXAREA=%ld\n", (long)s_delta_maxarea);
```

  ★ Verify LVGL 9.4 sends `LV_EVENT_INVALIDATE_AREA` from
  `lv_obj_invalidate_area` (grep `lv_obj_pos.c` / `lv_obj.c`); if the event
  fires only via `lv_obj_invalidate`, hook the DISPLAY's invalidate event
  instead and filter by intersection with the knobs' coords.
- [ ] **Step 2: Build + run, verify tokens** — build; two QEMU runs;
  `KNOB_DELTA_SEQ == KNOB_DELTA_FULL` in both, stable across runs;
  `KNOB_DELTA_MAXAREA` plausibly wedge-sized (≈4–7 k); the six EXISTING
  goldens unchanged (if any moved, stop — scene code was not supposed to
  change).
- [ ] **Step 3: Gate** — append to `run_qemu.sh` after the accent golden:

```sh
# Wedge-delta guards (spec 2026-08-27-rotary-knob-delta-damage). EQUALITY is
# computed by the GATE from the two printed sums -- not a pinned golden, so
# it never re-goldens; a bbox regression (stale wedge pixels) fails here.
DSEQ=$(grep -a -oE "KNOB_DELTA_SEQ=0x[0-9A-F]{8}" "$OUT" | head -1 | cut -d= -f2)
DFUL=$(grep -a -oE "KNOB_DELTA_FULL=0x[0-9A-F]{8}" "$OUT" | head -1 | cut -d= -f2)
[ -n "$DSEQ" ] && [ -n "$DFUL" ] || { echo "FAIL: delta guard tokens missing"; exit 1; }
[ "$DSEQ" = "$DFUL" ] || { echo "FAIL: delta render differs from full render ($DSEQ vs $DFUL)"; exit 1; }
# ENGAGEMENT: the recorded per-step damage must be wedge-sized, not the
# whole control -- a silent revert to full invalidation fails HERE.
DAREA=$(grep -a -oE "KNOB_DELTA_MAXAREA=[0-9]+" "$OUT" | head -1 | cut -d= -f2)
[ -n "$DAREA" ] && [ "$DAREA" -gt 0 ] || { echo "FAIL: delta area guard missing or zero"; exit 1; }
[ "$DAREA" -le 8000 ] || { echo "FAIL: delta damage not engaged (maxarea=$DAREA)"; exit 1; }
```

- [ ] **Step 4: Demonstrated RED, both guards** — (a) rebuild with the bbox
  pad temporarily 0 (scratch edit, reverted) → equality guard fails by name;
  (b) rebuild with set_angle temporarily full-invalidating → engagement
  guard fails by name. Quote both in the gate header.
- [ ] **Step 5: Fixture** — re-run gate green, `cp` the capture over
  `transcript_qemu.txt`.
- [ ] **Step 6: Commit (evkb)**.

### Task 4: Verification sweep

- [ ] **Step 1** — four affected/control gates idle (`synthui_knob_test`,
  `synthui_step_test`, `acid_box`, `vglite_lvgl_test`): PASS with their
  EXISTING goldens (SynthUI changed under all four — their unchanged goldens
  are the no-visual-change proof; rebuild each first).
- [ ] **Step 2** — full sweep, one instance, captured: expect 121/1/0 with
  the documented `[hci]` standing red. Vacuity suite green.
  License audit PASS.
- [ ] **Step 3** — CLAUDE.md: one sentence in the NEW-20 Phase 2 paragraph
  (delta damage shipped, measured 4×, guard pair in knob_test). Commit.

### Task 5: Pin bump + fresh-user verify

- [ ] Push SynthUI; bump the pin in `evkb.cmake`; `-DEVKB_FORCE_FETCH=ON`
  scratch build of `synthui_knob_test` AND its gate run against the fetched
  ELF; commit.

### Task 6: Silicon (needs the debug-USB replug first — probe is wedged)

- [ ] **Step 1** — "before" number: flash
  `vglite_lvgl_test/build-fps-before/` (pre-change FPSBENCH sw ELF, already
  built), record FPSBENCH_MEAN_US/WORST_US.
- [ ] **Step 2** — rebuild FPSBENCH sw with the delta widget
  (`build-fps-after`), flash, record: the widget-level before/after beside
  the prototype's 310→71 ms. Append both to the vglite transcript.
- [ ] **Step 3** — `synthui_knob_test` golden build: boot, capture; assert
  `rk_engine=gpu`, `rk_gpu_err=0`, `KNOB_DELTA_SEQ == KNOB_DELTA_FULL` on
  the GPU engine (the scissor's behavioral proof), six existing GPU sums
  unchanged from Phase 2's transcript; two boots. SWD screenshot of the
  delta scene via an RK_EYEBALL_HOLD-style hold if anything looks off on
  glass. Update `transcript_hw_evkb.txt`.
- [ ] **Step 4** — restore `acid_box` to flash (still owed from the probe
  wedge). Commit transcripts.

### Task 7: Close out

- [ ] Merge branch to master, push. Linear: comment on NEW-20 (post-close
  addendum) or the issue the user prefers. Memory: update
  `new20-rotary-knob-bench.md` (delta shipped + numbers).
