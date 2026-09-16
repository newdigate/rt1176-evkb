# NEW-50 LedButton cue damage — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Narrow `synthui_led_button_set_cue()`'s damage from the whole key to the bezel's border ring (four strips), make `led_draw()` clip-aware so the narrower damage does not multiply draw tasks, and prove both with a per-op draw-task counter — with not one pixel moving anywhere.

**Architecture:** The pure geometry (`synthui_led_button_cue_boxes()`) lives in the LVGL-free math header beside `lit_box()`/`press_box()`, host-tested by a pixel-level coverage sweep against a model of LVGL's rounded-rect border mask. The widget consumes it and gains a per-layer `layer->_clip_area` test (the pattern its four sibling widgets already use). The gate gains a `led_button_tasks_op` line, re-derives its area bounds, and demonstrates every new assertion RED. acid_box gets one added assertion (`full <= 2`). Spec: `docs/superpowers/specs/2026-09-16-led-button-cue-damage-design.md`.

**Tech Stack:** C (header-only math + host tests, `cc -Wall -Wextra -Werror`), C++ LVGL 9.4 widget, CMake/ARM GCC 10 firmware, POSIX-sh QEMU gates, `tools/gate-vacuity.test.sh`.

**Two checkouts:** `~/Development/SynthUI` (the widget; sibling repo, local-first resolution means an uncommitted edit there is what the evkb examples build) and `~/Development/rt1170/evkb` (examples, gates, docs). Every `cd` below is explicit.

**House rules that bite:** `./run_qemu.sh`, never `sh run_qemu.sh`. One QEMU run at a time. Never run the sweep, the vacuity suite and the licence audit concurrently. Read `docs/KNOWN-BROKEN-GATES.md` before the sweep. Commit messages end with `Co-Authored-By: Claude Fable 5.1 <noreply@anthropic.com>`.

---

## File map

| file | change |
|---|---|
| `SynthUI/src/synthui_led_button_math.h` | + `SYNTHUI_LED_BUTTON_CUE_MARGIN_PX`, + `synthui_led_button_radius_px()`, + `synthui_led_button_cue_boxes()` |
| `SynthUI/tests/led_button_test.c` | + coverage / engagement / structure / pin / degenerate tests; file-top mutant record |
| `SynthUI/src/synthui_led_button.cpp` | `base.obj` on every dsc; `led_radius` → header; `set_cue` → four boxes; clip-aware `led_draw` |
| `evkb/examples/display/synthui_led_button_test/synthui_led_button_test.cpp` | draw-task counter, `led_button_tasks_op` line, `delta_refr()`, comment updates |
| `evkb/examples/display/synthui_led_button_test/run_qemu.sh` | re-derived bounds, tasks-line assertions, demo record |
| `evkb/examples/display/synthui_led_button_test/transcript_qemu.txt` | re-captured fixture |
| `evkb/examples/display/acid_box/run_qemu.sh` | `full <= 2` |
| `evkb/examples/display/acid_box/transcript_qemu.txt` | re-captured fixture |
| `evkb/tools/gate-vacuity.test.sh` | + 4 cases (3 led_button, 1 acid_box) |
| `evkb/evkb.cmake` | SynthUI pin bump |
| `evkb/CLAUDE.md`, the spec, memory, Linear | close-out record |

---

### Task 0: Mark the issue started

- [ ] **Step 1: Move NEW-50 to In Progress** (Linear MCP `save_issue`, `id: NEW-50`, `state: "In Progress"`). The issue's own precondition — "do not start before the acid_box bench session measures it" — was met on 2026-09-16 (touch p95 37.8 stopped vs 66.8 playing, `acid_box/transcript_hw_evkb.txt`); say so in a one-line comment (`save_comment`, `issueId: NEW-50`):

```
Started 2026-09-16. Precondition met by the acid_box bench session: touch p95 37.8 ms STOPPED vs 66.8 ms PLAYING (two boots), widget Phase B 20.6 fps. Spec: docs/superpowers/specs/2026-09-16-led-button-cue-damage-design.md.
```

---

### Task 1: `cue_boxes()` in the math header, host-tested (TDD)

**Files:**
- Modify: `~/Development/SynthUI/src/synthui_led_button_math.h`
- Modify: `~/Development/SynthUI/tests/led_button_test.c`

- [ ] **Step 1: Write the failing host test.** Insert the block below in `tests/led_button_test.c` immediately AFTER the closing `}` of the pixel-space sweep block (the line after the `if (pressed) { ... }` block's three closing braces, i.e. just before `/* --- 100x100, unpressed: the DC box in pixels, 1 unit = 1 px --- */`). Also add the two helpers `in_rrect` and `px_has` next to `px_contains` (before `int main`).

Helpers (place after `px_contains`):

```c
/* Model of LVGL's rounded-rect mask as lv_draw_sw_border.c applies it:
 * rout = radius clamped to half the short side, area_inner = coords inset
 * by the border width, rin = max(rout - width, 0).  A pixel is inside when
 * its CENTRE lies in the rect and, within a corner square of side r, within
 * r of that corner's arc centre.  Antialiasing is handled by the caller
 * dilating the ring by one pixel, not by this predicate. */
static int in_rrect(int x, int y, const synthui_led_button_px_t *a, int r)
{
    if (x < a->x1 || x > a->x2 || y < a->y1 || y > a->y2) return 0;
    if (r <= 0) return 1;
    float cx, cy;
    if      (x < a->x1 + r && y < a->y1 + r) { cx = (float)(a->x1 + r);     cy = (float)(a->y1 + r); }
    else if (x > a->x2 - r && y < a->y1 + r) { cx = (float)(a->x2 + 1 - r); cy = (float)(a->y1 + r); }
    else if (x < a->x1 + r && y > a->y2 - r) { cx = (float)(a->x1 + r);     cy = (float)(a->y2 + 1 - r); }
    else if (x > a->x2 - r && y > a->y2 - r) { cx = (float)(a->x2 + 1 - r); cy = (float)(a->y2 + 1 - r); }
    else return 1;
    const float dx = (float)x + 0.5f - cx, dy = (float)y + 0.5f - cy;
    return dx * dx + dy * dy <= (float)r * (float)r;
}

static int px_has(const synthui_led_button_px_t *b, int x, int y)
{
    return !px_empty(b) && x >= b->x1 && x <= b->x2 && y >= b->y1 && y <= b->y2;
}

static int px_overlap(const synthui_led_button_px_t *a, const synthui_led_button_px_t *b)
{
    if (px_empty(a) || px_empty(b)) return 0;
    return a->x1 <= b->x2 && b->x1 <= a->x2 && a->y1 <= b->y2 && b->y1 <= a->y2;
}
```

Test block (place after the pixel-space sweep, before the 100x100 pins):

```c
    /* --- NEW-50: cue_boxes() covers the bezel ring, and only about a third
     * of the key.  Coverage is checked PIXEL BY PIXEL against a model of the
     * border mask LVGL actually applies, dilated one pixel for antialiasing,
     * for BOTH ring widths (a cue change repaints the narrow ring it leaves
     * as well as the wide one it enters).  Coverage alone is satisfied by
     * a whole-key box, so engagement is asserted separately (the NEW-25
     * lesson); it is asserted only from 64 px up, because on a 32 px key a
     * 4 px band is legitimately a large fraction of the key. --- */
    {
        static const float cue_nonsquare[][2] = {
            {120.0f, 80.0f}, {80.0f, 120.0f}, {150.0f, 100.0f}, {100.0f, 150.0f}
        };
        int shape;
        for (shape = 0; shape < 185 + 4; ++shape) {
            float sw, sh;
            if (shape < 185) { sw = sh = (float)(16 + shape); }
            else             { sw = cue_nonsquare[shape - 185][0]; sh = cue_nonsquare[shape - 185][1]; }

            synthui_led_button_layout_t Lc;
            assert(synthui_led_button_compute_layout(sw, sh, false, &Lc));
            synthui_led_button_px_t box[4];
            synthui_led_button_cue_boxes(sw, sh, box);

            const synthui_led_button_px_t bz = synthui_led_button_rect_px(&Lc.bezel, 0);
            const int bw_px = bz.x2 - bz.x1 + 1, bh_px = bz.y2 - bz.y1 + 1;
            const int side = bw_px < bh_px ? bw_px : bh_px;
            int R = synthui_led_button_radius_px(Lc.bezel_r);
            if (R > side / 2) R = side / 2;

            /* structure: inside the bezel, pairwise disjoint */
            int i, j;
            for (i = 0; i < 4; ++i) assert(px_contains(&bz, &box[i]));
            for (i = 0; i < 4; ++i) for (j = i + 1; j < 4; ++j) assert(!px_overlap(&box[i], &box[j]));

            /* coverage, for the cue ring (index 0) and the plain ring (1) */
            int ring;
            for (ring = 0; ring < 2; ++ring) {
                const int bw = ring == 0 ? Lc.cue_bw_px : Lc.bezel_bw_px;
                const synthui_led_button_px_t inner = { bz.x1 + bw, bz.y1 + bw, bz.x2 - bw, bz.y2 - bw };
                const int rin = R - bw > 0 ? R - bw : 0;
                int x, y;
                for (y = bz.y1; y <= bz.y2; ++y) for (x = bz.x1; x <= bz.x2; ++x) {
                    if (!(in_rrect(x, y, &bz, R) && !in_rrect(x, y, &inner, rin))) continue;
                    /* (x,y) is a ring pixel: it and its 8 neighbours inside
                     * the bezel must each lie in one of the four boxes */
                    int nx, ny;
                    for (ny = y - 1; ny <= y + 1; ++ny) for (nx = x - 1; nx <= x + 1; ++nx) {
                        if (nx < bz.x1 || nx > bz.x2 || ny < bz.y1 || ny > bz.y2) continue;
                        assert(px_has(&box[0], nx, ny) || px_has(&box[1], nx, ny) ||
                               px_has(&box[2], nx, ny) || px_has(&box[3], nx, ny));
                    }
                }
            }

            /* engagement: from 64 px up, the four boxes are <= 45 % of the
             * bezel (swept worst case 36.0 % at 80x85; a whole-key box is
             * 100 %) */
            if (side >= 64) {
                long total = 0;
                for (i = 0; i < 4; ++i) {
                    if (px_empty(&box[i])) continue;
                    total += (long)(box[i].x2 - box[i].x1 + 1) * (box[i].y2 - box[i].y1 + 1);
                }
                assert(total * 100 <= (long)bw_px * bh_px * 45);
            }
        }
    }

    /* 100x100 pinned: band = ceil(17 - 13/sqrt2) + 1 = 8 + 1 = 9 */
    {
        synthui_led_button_px_t cb[4];
        synthui_led_button_cue_boxes(100.0f, 100.0f, cb);
        assert(cb[0].x1 == 0  && cb[0].y1 == 0  && cb[0].x2 == 99 && cb[0].y2 == 8);    /* top */
        assert(cb[1].x1 == 0  && cb[1].y1 == 91 && cb[1].x2 == 99 && cb[1].y2 == 99);   /* bottom */
        assert(cb[2].x1 == 0  && cb[2].y1 == 9  && cb[2].x2 == 8  && cb[2].y2 == 90);   /* left */
        assert(cb[3].x1 == 91 && cb[3].y1 == 9  && cb[3].x2 == 99 && cb[3].y2 == 90);   /* right */
        /* 120x80: the bezel is the centred 80x80 (ox=20), band 8 */
        synthui_led_button_cue_boxes(120.0f, 80.0f, cb);
        assert(cb[0].x1 == 20 && cb[0].x2 == 99 && cb[0].y1 == 0 && cb[0].y2 == 7);
        assert(cb[3].x1 == 92 && cb[3].x2 == 99);
    }
```

And in the existing `/* --- degenerate sizes --- */` block, after the `press_box` zero check, add:

```c
    {
        synthui_led_button_px_t zc[4];
        synthui_led_button_cue_boxes(0.0f, 100.0f, zc);
        int i;
        for (i = 0; i < 4; ++i) assert(zc[i].x1 == 0 && zc[i].y1 == 0 && zc[i].x2 == 0 && zc[i].y2 == 0);
    }
```

- [ ] **Step 2: Run the test to verify it fails to build.**

```bash
cd ~/Development/SynthUI && cc -Wall -Wextra -Werror -o /tmp/lbt tests/led_button_test.c && /tmp/lbt
```

Expected: compile error, `implicit declaration of function 'synthui_led_button_cue_boxes'` (and `synthui_led_button_radius_px`).

- [ ] **Step 3: Implement.** In `src/synthui_led_button_math.h`, add after `#define SYNTHUI_LED_BUTTON_DOTS_MIN_PX 34.0f`:

```c
#define SYNTHUI_LED_BUTTON_CUE_MARGIN_PX 1     /* NEW-50: antialiasing slack on the ring's inner edge */
```

Add after `synthui_led_button_circle_px()`:

```c
/* Radii: at least 1 px.  The widget's ONE radius conversion, kept here so
 * the cue band (below) is sized from the radius LVGL is actually given. */
static inline int32_t synthui_led_button_radius_px(float r)
{
    const int32_t p = (int32_t)lroundf(r);
    return p < 1 ? 1 : p;
}
```

Add after `synthui_led_button_press_box()`:

```c
/* Damage for a cue change (NEW-50): the bezel's border ring as FOUR
 * NON-OVERLAPPING strips -- out[0] top and out[1] bottom full width, out[2]
 * left and out[3] right the rows between them.  Inclusive widget-relative
 * pixels like lit_box()/press_box(); the bezel never moves, so no dy_px.
 *
 * The ring reaches bw inward on the flats but R - (R-bw)/sqrt(2) on each
 * corner diagonal, and every ring pixel has min(dx,dy) <= that -- which is
 * exactly what four strips of that depth cover.  R and bw are the ROUNDED
 * pixel values the draw uses (lv_draw_sw_border: rout = radius clamped to
 * half the short side, rin = max(rout - width, 0)); sizing from units would
 * not guarantee coverage.  cue_bw_px >= bezel_bw_px at every scale (lroundf
 * is monotone, 3.5s >= 2s, both floored at 1), so the band covers the narrow
 * ring being left as well as the wide one entered.  +MARGIN is antialiasing
 * slack; the gate's delta-equality guard is what validates it.
 * A whole-key box also covers the ring -- led_button_test asserts the four
 * boxes stay under 45 % of the key from 64 px up, or a regression to
 * whole-key damage would pass coverage.
 * Spec: docs/superpowers/specs/2026-09-16-led-button-cue-damage-design.md */
static inline void synthui_led_button_cue_boxes(float w, float h, synthui_led_button_px_t out[4])
{
    synthui_led_button_layout_t L;
    int i;
    if (!synthui_led_button_compute_layout(w, h, false, &L)) {
        for (i = 0; i < 4; ++i) { out[i].x1 = out[i].y1 = out[i].x2 = out[i].y2 = 0; }
        return;
    }
    const synthui_led_button_px_t bz = synthui_led_button_rect_px(&L.bezel, 0);
    const int32_t bw_px = bz.x2 - bz.x1 + 1;
    const int32_t bh_px = bz.y2 - bz.y1 + 1;
    const int32_t side  = bw_px < bh_px ? bw_px : bh_px;

    int32_t R = synthui_led_button_radius_px(L.bezel_r);
    if (R > side / 2) R = side / 2;
    const int32_t bw    = L.cue_bw_px;
    const int32_t inner = R - bw > 0 ? R - bw : 0;
    int32_t band = (int32_t)ceilf((float)R - (float)inner / sqrtf(2.0f)) + SYNTHUI_LED_BUTTON_CUE_MARGIN_PX;
    if (band < bw + SYNTHUI_LED_BUTTON_CUE_MARGIN_PX) band = bw + SYNTHUI_LED_BUTTON_CUE_MARGIN_PX;
    if (band > side / 2) band = side / 2;   /* tiny keys: top+bottom alone tile the bezel */

    out[0].x1 = bz.x1;            out[0].y1 = bz.y1;            out[0].x2 = bz.x2;            out[0].y2 = bz.y1 + band - 1;   /* top */
    out[1].x1 = bz.x1;            out[1].y1 = bz.y2 - band + 1; out[1].x2 = bz.x2;            out[1].y2 = bz.y2;              /* bottom */
    out[2].x1 = bz.x1;            out[2].y1 = bz.y1 + band;     out[2].x2 = bz.x1 + band - 1; out[2].y2 = bz.y2 - band;       /* left */
    out[3].x1 = bz.x2 - band + 1; out[3].y1 = bz.y1 + band;     out[3].x2 = bz.x2;            out[3].y2 = bz.y2 - band;       /* right */
}
```

- [ ] **Step 4: Run the test to verify it passes.**

```bash
cd ~/Development/SynthUI && cc -Wall -Wextra -Werror -o /tmp/lbt tests/led_button_test.c && /tmp/lbt
```

Expected: `led_button_test: all PASS`. If the coverage assert fires at some size, print `sw, sh, x, y, ring` before the assert to see which pixel is uncovered; do NOT raise `SYNTHUI_LED_BUTTON_CUE_MARGIN_PX` without recording which size and why in the header comment.

- [ ] **Step 5: Demonstrate RED with three mutants**, each applied to the header, the test run, the failing assert line noted, then REVERTED (`git checkout src/synthui_led_button_math.h` between mutants):

  1. corner term dropped — replace the `band =` line with `int32_t band = bw + SYNTHUI_LED_BUTTON_CUE_MARGIN_PX;` → expected: the coverage `assert(px_has(...) || ...)` fails (first size 16 or soon after).
  2. whole key — replace with `int32_t band = side / 2;` → expected: the engagement `assert(total * 100 <= ...)` fails at size 64.
  3. offset dropped — replace `const synthui_led_button_px_t bz = synthui_led_button_rect_px(&L.bezel, 0);` with `const synthui_led_button_px_t bz = { 0, 0, (int32_t)lroundf((w < h ? w : h)) - 1, (int32_t)lroundf((w < h ? w : h)) - 1 };` → expected: every square size passes and the coverage assert fails on the FIRST non-square shape (120x80) — confirm by printing `shape` if needed. This arm is what earns the non-square sweep.

- [ ] **Step 6: Record the mutants** in the file-top comment of `tests/led_button_test.c`, appending to the existing list:

```
 *   cue_boxes corner term dropped (band = bw+1)  -> the ring-coverage px_has assert (sweep, size 16)
 *   cue_boxes band = whole key                   -> the 45 % engagement assert (size 64)
 *   cue_boxes built from (0,0,side,side), no ox  -> the ring-coverage px_has assert on 120x80 ONLY
```

(Replace "size 16" with the size the run actually named.) Then run the whole host suite:

```bash
cd ~/Development/SynthUI && ./tests/run.sh
```

Expected: every suite prints its PASS line; exit 0.

- [ ] **Step 7: Commit.**

```bash
cd ~/Development/SynthUI && git add src/synthui_led_button_math.h tests/led_button_test.c && git commit -F - <<'EOF'
synthui_led_button: cue_boxes() -- the bezel ring as four strips of depth ceil(R-(R-bw)/sqrt2)+1, host-tested pixel by pixel (NEW-50)

Sized from the ROUNDED radius and border width LVGL is given, never from units.
Coverage is checked against a model of lv_draw_sw_border's mask (rout clamped to
half the short side, rin = rout - width) dilated one pixel, for both ring widths;
engagement (<= 45 % of the key from 64 px up, swept worst 36.0 % at 80x85) is
asserted separately because a whole-key box also covers the ring. Three mutants
RED: corner term dropped, whole key, offset dropped (fails on 120x80 only).

Co-Authored-By: Claude Fable 5.1 <noreply@anthropic.com>
EOF
```

---

### Task 2: Draw tasks attributable to the widget (`base.obj`), radius through the header

**Files:**
- Modify: `~/Development/SynthUI/src/synthui_led_button.cpp`

This is the enabling change for the instrument in Task 3: `lv_draw_finalize_task_creation` sends `LV_EVENT_DRAW_TASK_ADDED` only when `dsc->base.obj` is set (`lv_draw.c:148`), and `lv_draw_rect_dsc_init` leaves it NULL. Pixel-neutral by construction; Task 3's gate run is what proves it.

- [ ] **Step 1: Replace `led_radius` with the header's conversion and give every dsc its object.** In `src/synthui_led_button.cpp`:

Delete the `led_radius` function (lines 103-107) and replace every `led_radius(` call in `led_draw` with `synthui_led_button_radius_px(` (seven call sites: bezel, well, cap ×3, highlight, halo, led, base — `grep -n led_radius` must return nothing afterwards).

Add after `led_circle_area()`:

```cpp
/* Every rect drawn here carries base.obj: on the software path the ONLY
 * thing LVGL reads it for is LV_EVENT_DRAW_TASK_ADDED (lv_draw.c, behind
 * LV_OBJ_FLAG_SEND_DRAW_TASK_EVENTS), which is how synthui_led_button_test
 * counts draw tasks per setter (NEW-50).  lv_draw_rect_dsc_init leaves it
 * NULL, and a widget whose tasks cannot be attributed cannot be
 * instrumented.  Pixel-neutral: the goldens prove it. */
static void led_dsc_init(lv_draw_rect_dsc_t *d, const synthui_led_button_t *b)
{
    lv_draw_rect_dsc_init(d);
    d->base.obj = (lv_obj_t *)b;
}
```

Change `led_fill` and `led_grad` to take the widget and use it:

```cpp
static void led_fill(lv_layer_t *layer, const synthui_led_button_t *b, const lv_area_t *a,
                     uint32_t hex, lv_opa_t opa, int32_t radius)
{
    lv_draw_rect_dsc_t d;
    led_dsc_init(&d, b);
    d.bg_color = lv_color_hex(hex);
    d.bg_opa = opa;
    d.radius = radius;
    lv_draw_rect(layer, &d, a);
}

static void led_grad(lv_layer_t *layer, const synthui_led_button_t *b, const lv_area_t *a,
                     uint32_t top, uint32_t bottom, int32_t radius)
{
    lv_draw_rect_dsc_t d;
    led_dsc_init(&d, b);
    d.bg_opa = LV_OPA_COVER;
    d.bg_grad.dir = LV_GRAD_DIR_VER;
    d.bg_grad.stops_count = 2;
    d.bg_grad.stops[0].color = lv_color_hex(top);
    d.bg_grad.stops[0].opa = LV_OPA_COVER;
    d.bg_grad.stops[0].frac = 0;
    d.bg_grad.stops[1].color = lv_color_hex(bottom);
    d.bg_grad.stops[1].opa = LV_OPA_COVER;
    d.bg_grad.stops[1].frac = 255;
    d.radius = radius;
    lv_draw_rect(layer, &d, a);
}
```

Update every `led_fill(layer, &a, ...)` call in `led_draw` to `led_fill(layer, b, &a, ...)` and both `led_grad(layer, &a, ...)` to `led_grad(layer, b, &a, ...)`. In the bezel block and the halo block, replace `lv_draw_rect_dsc_init(&d);` with `led_dsc_init(&d, b);`.

- [ ] **Step 2: Build the widget's own gate example against the local checkout and run the gate — the goldens must not move.**

```bash
cd ~/Development/rt1170/evkb/examples/display/synthui_led_button_test && cmake --build build 2>&1 | tail -3 && ./run_qemu.sh 2>&1 | tail -4
```

Expected: `PASS: SynthUI led_button render verified`; in the printed capture `led_button_crc=0xD474F06D`, `led_button_fresh_crc=0x463C3371`, `led_button_delta_eq=PASS`. (If `cmake --build` says the toolchain file is not found, the dir predates 2026-08-14: `rm -rf build && cmake -B build -DCMAKE_TOOLCHAIN_FILE=../../../toolchain/rt1170-evkb.toolchain.cmake && cmake --build build`.)

- [ ] **Step 3: Commit.**

```bash
cd ~/Development/SynthUI && git add src/synthui_led_button.cpp && git commit -F - <<'EOF'
synthui_led_button: every draw dsc carries base.obj so its tasks are attributable; radius through the header's one conversion (NEW-50)

lv_draw_rect_dsc_init leaves base.obj NULL and lv_draw.c sends
LV_EVENT_DRAW_TASK_ADDED only when it is set -- a widget whose tasks cannot be
attributed cannot be instrumented. On the software path that event is the only
reader. Pixel-neutral: goldens 0xD474F06D / 0x463C3371 unmoved.

Co-Authored-By: Claude Fable 5.1 <noreply@anthropic.com>
EOF
```

---

### Task 3: The gate's new instrument and re-derived bounds — RED against the old widget

**Files:**
- Modify: `~/Development/rt1170/evkb/examples/display/synthui_led_button_test/synthui_led_button_test.cpp`
- Modify: `~/Development/rt1170/evkb/examples/display/synthui_led_button_test/run_qemu.sh`

This task writes the assertions that Task 4 makes green. Against the current `set_cue` they must fail — by name — which is RED demo #2 of the spec obtained without a mutant.

- [ ] **Step 1: Add the per-op draw-task counter to the example.** In `synthui_led_button_test.cpp`, after `static int32_t s_op_max[LED_OP_COUNT];` add:

```cpp
/* NEW-50: draw tasks per single setter call, max per op.  Counted from
 * LV_EVENT_DRAW_TASK_ADDED on the keys (LV_OBJ_FLAG_SEND_DRAW_TASK_EVENTS,
 * set in build_bank so the golden and fresh passes are identical), so the
 * title label never enters the count.  This is the number that sees what
 * no area bound and no golden can: LVGL renders one pass per invalidated
 * area and lv_draw_rect allocates a task BEFORE any clip test, so a
 * narrower damage box that led_draw does not clip against costs MORE
 * tasks, not fewer -- four strips x 12 = 48 against the whole key's 12. */
static int32_t s_op_tasks[LED_OP_COUNT];
static int32_t s_tasks_step = 0;

static void delta_task_cb(lv_event_t *e)
{
    (void)e;
    if (s_delta_record) s_tasks_step++;
}

/* One refresh per setter: fold this step's task count into its op's max. */
static void delta_refr(void)
{
    s_tasks_step = 0;
    lv_refr_now(NULL);
    if (s_tasks_step > s_op_tasks[s_delta_op]) s_op_tasks[s_delta_op] = s_tasks_step;
}
```

`delta_task_cb`/`delta_refr` must be declared before `build_bank()` uses the callback: move the whole "delta guards" block (from `enum { LED_OP_LIT ...` through `delta_refr`) to ABOVE `build_bank()`, or add a forward declaration `static void delta_task_cb(lv_event_t *e);` before `build_bank()`. In `build_bank()`, after `if (k->lv_state) lv_obj_add_state(b, LV_STATE_PRESSED);` add:

```cpp
        lv_obj_add_flag(b, LV_OBJ_FLAG_SEND_DRAW_TASK_EVENTS);
        lv_obj_add_event_cb(b, delta_task_cb, LV_EVENT_DRAW_TASK_ADDED, NULL);
```

In `delta_run_sequence()`: add `for (int i = 0; i < LED_OP_COUNT; i++) s_op_tasks[i] = 0;` next to the `s_op_max` reset; replace `lv_refr_now(NULL);` with `delta_refr();` in the LCG loop and in tail steps (a)–(f) — SEVEN replacements. Leave the UNTRACKED key-12 restore's `lv_refr_now(NULL)` alone.

After the `led_button_damage_op` printf add:

```cpp
    Serial1.printf("led_button_tasks_op lit=%ld press=%ld cue=%ld color=%ld\n",
                   (long)s_op_tasks[LED_OP_LIT], (long)s_op_tasks[LED_OP_PRESSED],
                   (long)s_op_tasks[LED_OP_CUE], (long)s_op_tasks[LED_OP_COLOR]);
```

- [ ] **Step 2: Re-derive the gate's bounds and add the tasks-line assertions.** In `run_qemu.sh`, replace the block from `# ENGAGEMENT, PER OP.` through `[ "$CUE" -le 10000 ] || ...` with:

```sh
# ENGAGEMENT, PER OP. Each op has its own bound, equal to its box on the
# largest key the sequence touches (keys 0..12: 100 px and 32/34 px): lit and
# colour = the halo, 58x25 = 1450; pressed = the cap group at both offsets,
# 80x83 = 6640; cue = ONE of the four bezel-ring strips (NEW-50), the top or
# bottom one, 100 x band 9 = 900 (band = ceil(R - (R-bw)/sqrt2) + 1 with
# R=17, bw=4).  These are EXACT boxes re-derived from
# synthui_led_button_math.h: a deliberate layout change trips them loudly and
# they must then be re-derived from the math, never loosened to whatever the
# run printed.  The per-op checks come BEFORE the overall max so a regression
# is named by its op.
OPLINE=$(grep -a -oE "led_button_damage_op lit=[0-9]+ press=[0-9]+ cue=[0-9]+ color=[0-9]+" "$OUT" | head -1)
[ -n "$OPLINE" ] || { echo "FAIL: per-op damage line missing"; exit 1; }
op() { echo "$OPLINE" | grep -oE "$1=[0-9]+" | cut -d= -f2; }
LIT=$(op lit); PRS=$(op press); CUE=$(op cue); COL=$(op color)
[ "$LIT" -gt 0 ] && [ "$PRS" -gt 0 ] && [ "$CUE" -gt 0 ] && [ "$COL" -gt 0 ] \
    || { echo "FAIL: an op was never exercised ($OPLINE)"; exit 1; }
[ "$LIT" -le 1450 ] || { echo "FAIL: lit damage above its box (lit=$LIT > 1450)"; exit 1; }
[ "$COL" -le 1450 ] || { echo "FAIL: colour damage above its box (color=$COL > 1450)"; exit 1; }
[ "$PRS" -le 6640 ] || { echo "FAIL: press damage above its box (press=$PRS > 6640)"; exit 1; }
[ "$CUE" -le 900 ]  || { echo "FAIL: cue damage above its box (cue=$CUE > 900)"; exit 1; }
# Overall max: the largest legitimate box is now press (6640).  Redundant
# with the per-op bounds above, kept because it is the FULL-SCREEN tripwire
# (921600) and the vacuity suite pins its presence.
DAREA=$(grep -a -oE "led_button_damage max=[0-9]+" "$OUT" | head -1 | cut -d= -f2)
[ -n "$DAREA" ] && [ "$DAREA" -gt 0 ] || { echo "FAIL: delta damage guard missing or zero"; exit 1; }
[ "$DAREA" -le 6640 ] || { echo "FAIL: delta damage not engaged (max=$DAREA)"; exit 1; }
grep -qE "led_button_damage max=[0-9]+ total=[0-9]+ steps=64 tail=6\r?$" "$OUT" || { echo "FAIL: delta sequence length changed (expected steps=64 tail=6)"; exit 1; }
# DRAW TASKS, PER OP (NEW-50).  The mechanism the area bounds cannot see:
# LVGL renders one pass per invalidated area, and lv_draw_rect allocates a
# task before any clip test (lv_draw_sw.c rejects it afterwards) out of a heap
# in uncached SDRAM.  A cue change is four strips, so a led_draw that stopped
# clipping its layers costs 4 x 12 = 48 tasks -- with every golden and the
# delta-equality guard still GREEN.  cue's bound is the one with teeth; lit,
# press and colour are single-pass ops whose counts can only rise if a setter
# starts emitting more than one box.  Bounds are pinned from the measured run
# WITH their derivation (Task 4 of the plan), and re-derived, never loosened.
TLINE=$(grep -a -oE "led_button_tasks_op lit=[0-9]+ press=[0-9]+ cue=[0-9]+ color=[0-9]+" "$OUT" | head -1)
[ -n "$TLINE" ] || { echo "FAIL: per-op draw-task line missing"; exit 1; }
tk() { echo "$TLINE" | grep -oE "$1=[0-9]+" | cut -d= -f2; }
TLIT=$(tk lit); TPRS=$(tk press); TCUE=$(tk cue); TCOL=$(tk color)
[ "$TLIT" -gt 0 ] && [ "$TPRS" -gt 0 ] && [ "$TCUE" -gt 0 ] && [ "$TCOL" -gt 0 ] \
    || { echo "FAIL: an op created no draw task ($TLINE)"; exit 1; }
```

The block being replaced runs from the `# ENGAGEMENT, PER OP.` comment through `[ "$CUE" -le 10000 ] || ...` and INCLUDES the old `DAREA=` / `-le 10000` / `steps=64` lines — they are reproduced, re-ordered, in the new block; nothing else in the gate changes. Task 4 appends the four `T*` bounds once measured.

- [ ] **Step 3: Rebuild and run the gate — it must go RED by name against the old widget, and the tasks line must read 12 for every op.**

```bash
cd ~/Development/rt1170/evkb/examples/display/synthui_led_button_test && cmake --build build 2>&1 | tail -2 && ./run_qemu.sh 2>&1 | grep -E "led_button_(damage_op|tasks_op|crc|fresh_crc|delta_eq)|^FAIL|^PASS"
```

Expected:
```
led_button_crc=0xD474F06D
led_button_delta_eq=PASS
led_button_fresh_crc=0x463C3371
led_button_damage_op lit=1450 press=6640 cue=10000 color=1450
led_button_tasks_op lit=<11|12> press=<11|12> cue=<11|12> color=<11|12>
FAIL: cue damage above its box (cue=10000 > 900)
```

The `tasks_op` reading is the BEFORE measurement: one pass per op, every layer issued regardless of clip — 12 when the key drawn that step is lit (the halo border is the twelfth task), 11 when it is not, and the max per op is whichever the sequence reached. Record the four numbers — they are the baseline the close-out compares against. If any `tasks_op` value is 0, `base.obj` is not reaching the event (Task 2) — stop and fix that before going on.

- [ ] **Step 4: Commit the example + gate (red on purpose, so say so).**

```bash
cd ~/Development/rt1170/evkb && git add examples/display/synthui_led_button_test/synthui_led_button_test.cpp examples/display/synthui_led_button_test/run_qemu.sh && git commit -F - <<'EOF'
synthui_led_button_test: per-op draw-task counter (led_button_tasks_op) and the cue bound re-derived 10000 -> 900, overall max 10000 -> 6640 (NEW-50)

RED against the current widget by design: "FAIL: cue damage above its box
(cue=10000 > 900)". Baseline task counts before the widget change:
lit=<n> press=<n> cue=<n> color=<n> -- one pass per op, every layer issued
regardless of clip. Turns green with the SynthUI change that follows.

Co-Authored-By: Claude Fable 5.1 <noreply@anthropic.com>
EOF
```

---

### Task 4: The widget change — four strips, clip-aware `led_draw` — and the task bound pinned

**Files:**
- Modify: `~/Development/SynthUI/src/synthui_led_button.cpp`
- Modify: `~/Development/rt1170/evkb/examples/display/synthui_led_button_test/run_qemu.sh`

- [ ] **Step 1: `set_cue` invalidates the four strips.** In `src/synthui_led_button.cpp` add after `led_invalidate_press_box()`:

```cpp
/* NEW-50: a cue change repaints only the bezel's border ring -- four strips
 * from the math header, non-overlapping so LVGL never even attempts a join.
 * Sibling of led_invalidate_lit_box/_press_box; the box shapes are proven on
 * the host (led_button_test's ring-coverage sweep) and the absence of stale
 * pixels by the gate's delta-equality guard. */
static void led_invalidate_cue_boxes(lv_obj_t *obj)
{
    lv_area_t c;
    lv_obj_get_coords(obj, &c);
    synthui_led_button_px_t px[4];
    synthui_led_button_cue_boxes((float)lv_area_get_width(&c), (float)lv_area_get_height(&c), px);
    for (int i = 0; i < 4; i++) led_invalidate_px(obj, &px[i]);
}
```

In `synthui_led_button_set_cue()` replace `lv_obj_invalidate(obj);   /* the bezel ring is the outer edge on four sides */` with `led_invalidate_cue_boxes(obj);`.

- [ ] **Step 2: Make `led_draw` clip-aware.** Add after `led_dsc_init()`:

```cpp
/* NEW-50: a layer is drawn only when its area meets the clip.  LVGL renders
 * one pass per invalidated area, and lv_draw_rect allocates a draw task
 * BEFORE any clip test (lv_draw_sw.c rejects it in execute_drawing, after
 * the malloc, evaluate and dispatch) out of a heap in uncached SDRAM.
 * Without this test the four cue strips cost 4 x 12 draw tasks where the
 * whole-key invalidate cost 12; with it, bezel fill + bezel border + well
 * per strip.  slide_toggle, piano_key, level_meter and seven_segment do the
 * same; this widget was the one that did not. */
static bool led_in_clip(const lv_layer_t *layer, const lv_area_t *a)
{
    lv_area_t isect;
    return lv_area_intersect(&isect, a, &layer->_clip_area);
}
```

Then in `led_draw`, wrap every layer:

```cpp
    /* 1. bezel (never moves): fill + border, the SVG stroke drawn inside the extent */
    led_area(&a, &c, &L.bezel, 0);
    if (led_in_clip(layer, &a)) {
        lv_draw_rect_dsc_t d;
        led_dsc_init(&d, b);
        d.bg_color = lv_color_hex(SYNTHUI_LED_BUTTON_BEZEL);
        d.bg_opa = LV_OPA_COVER;
        d.radius = synthui_led_button_radius_px(L.bezel_r);
        d.border_color = lv_color_hex(P.bezel_color);
        d.border_width = P.cue_border ? L.cue_bw_px : L.bezel_bw_px;  /* same source as bezel_color */
        d.border_opa = LV_OPA_COVER;
        d.border_side = LV_BORDER_SIDE_FULL;
        lv_draw_rect(layer, &d, &a);
    }

    /* 2. well (never moves) */
    led_area(&a, &c, &L.well, 0);
    if (led_in_clip(layer, &a))
        led_fill(layer, b, &a, SYNTHUI_LED_BUTTON_WELL, SYNTHUI_LED_BUTTON_WELL_OPA, synthui_led_button_radius_px(L.well_r));

    /* 3. cap: ... (existing comment unchanged) */
    led_area(&a, &c, &L.cap, dy);
    if (led_in_clip(layer, &a))
        led_fill(layer, b, &a, P.cap_mid, LV_OPA_COVER, synthui_led_button_radius_px(L.cap_r));
    led_area(&a, &c, &L.cap_top, dy);
    if (led_in_clip(layer, &a))
        led_grad(layer, b, &a, P.cap_top, P.cap_mid, synthui_led_button_radius_px(L.cap_r));
    led_area(&a, &c, &L.cap_low, dy);
    if (led_in_clip(layer, &a))
        led_grad(layer, b, &a, P.cap_mid, P.cap_low, synthui_led_button_radius_px(L.cap_r));

    /* 4. highlight */
    led_area(&a, &c, &L.highlight, dy);
    if (led_in_clip(layer, &a))
        led_fill(layer, b, &a, 0xFFFFFFu, P.highlight_opa, synthui_led_button_radius_px(L.highlight_r));

    /* 5. halo: ... (existing comment unchanged) */
    if (P.halo_on) {
        led_area(&a, &c, &L.halo, dy);
        if (led_in_clip(layer, &a)) {
            lv_draw_rect_dsc_t d;
            led_dsc_init(&d, b);
            d.bg_opa = LV_OPA_TRANSP;
            d.radius = synthui_led_button_radius_px(L.halo_r);
            d.border_color = lv_color_hex(P.halo_color);
            d.border_width = L.halo_bw_px;
            d.border_opa = SYNTHUI_LED_BUTTON_HALO_OPA;
            d.border_side = LV_BORDER_SIDE_FULL;
            lv_draw_rect(layer, &d, &a);
        }
    }

    /* 6. LED */
    led_area(&a, &c, &L.led, dy);
    if (led_in_clip(layer, &a))
        led_fill(layer, b, &a, P.led_fill, LV_OPA_COVER, synthui_led_button_radius_px(L.led_r));

    /* 7. moulding dots (dropped below 34 px) */
    if (L.dots_visible) {
        led_circle_area(&a, &c, &L.dot1, dy);
        if (led_in_clip(layer, &a)) led_fill(layer, b, &a, 0x000000u, SYNTHUI_LED_BUTTON_DOT_OPA, LV_RADIUS_CIRCLE);
        led_circle_area(&a, &c, &L.dot2, dy);
        if (led_in_clip(layer, &a)) led_fill(layer, b, &a, 0x000000u, SYNTHUI_LED_BUTTON_DOT_OPA, LV_RADIUS_CIRCLE);
    }

    /* 8. base */
    led_area(&a, &c, &L.base, dy);
    if (led_in_clip(layer, &a))
        led_fill(layer, b, &a, SYNTHUI_LED_BUTTON_BASE, P.base_opa, synthui_led_button_radius_px(L.base_r));
```

- [ ] **Step 3: Rebuild, run the gate, read the measurement.**

```bash
cd ~/Development/rt1170/evkb/examples/display/synthui_led_button_test && cmake --build build 2>&1 | tail -2 && ./run_qemu.sh 2>&1 | grep -E "led_button_(damage|damage_op|tasks_op|crc|fresh_crc|delta_eq)|^FAIL|^PASS"
```

Expected (the tasks numbers are PREDICTIONS — record what prints):
```
led_button_crc=0xD474F06D
led_button_delta_eq=PASS
led_button_fresh_crc=0x463C3371
led_button_damage max=6640 total=... steps=64 tail=6
led_button_damage_op lit=1450 press=6640 cue=900 color=1450
led_button_tasks_op lit=<~10> press=12 cue=<12 at 100 px; ~20 if a 32/34 px key sets it> color=<~10>
PASS: SynthUI led_button render verified
```

Both goldens MUST be unmoved and `delta_eq=PASS`. If `delta_eq=FAIL`: the band is too shallow somewhere — diff by printing per-step `s_tasks_step` and the step index to find the key, then re-examine the margin at THAT key's size in the host model; do not widen blindly. If a golden moved: `base.obj` or the clip test changed a pixel — that is a bug, find it (the golden is the instrument, not the obstacle).

- [ ] **Step 4: Derive and pin the four task bounds.** For the measured `cue` value, write down which key and which layers produce it: at 100 px each strip meets bezel (fill+border) + well = 3 → 12; at 32/34 px `band=4` reaches the cap group (`cap.x1=3`), so count the layers whose px rect meets each strip there. Append to `run_qemu.sh` after the `an op created no draw task` line, with the measured numbers in place of `<...>`:

```sh
# Pinned 2026-09-16 from the measured run, derivation in the comment above
# each: a layout change trips these loudly; re-derive, never loosen.
[ "$TCUE" -le <CUE_MEASURED> ] || { echo "FAIL: cue draw tasks above its bound (cue=$TCUE > <CUE_MEASURED>)"; exit 1; }   # <which key> x 4 strips x <layers per strip>
[ "$TLIT" -le <LIT_MEASURED> ] || { echo "FAIL: lit draw tasks above its bound (lit=$TLIT > <LIT_MEASURED>)"; exit 1; }
[ "$TCOL" -le <COL_MEASURED> ] || { echo "FAIL: colour draw tasks above its bound (color=$TCOL > <COL_MEASURED>)"; exit 1; }
[ "$TPRS" -le <PRS_MEASURED> ] || { echo "FAIL: press draw tasks above its bound (press=$TPRS > <PRS_MEASURED>)"; exit 1; }
```

Run the gate again; expected `PASS: SynthUI led_button render verified`.

- [ ] **Step 5: Commit both repos.**

```bash
cd ~/Development/SynthUI && git add src/synthui_led_button.cpp && git commit -F - <<'EOF'
synthui_led_button: set_cue damages the bezel ring as four strips; led_draw clips every layer against the pass (NEW-50)

Narrowing the box alone would have been a 4x pessimisation: LVGL renders one
pass per invalidated area and lv_draw_rect allocates a task before any clip
test, so four strips x 12 unconditional layers = 48 draw tasks against the
whole key's 12. With the clip test each strip draws bezel fill + bezel border +
well: 12, flat -- the win is pixel work (~25,000 px of overlapping fills to
~4,000), not task count. Goldens 0xD474F06D / 0x463C3371 unmoved,
delta equality PASS; cue damage 10000 -> 900 on the gate.

Co-Authored-By: Claude Fable 5.1 <noreply@anthropic.com>
EOF
cd ~/Development/rt1170/evkb && git add examples/display/synthui_led_button_test/run_qemu.sh && git commit -F - <<'EOF'
synthui_led_button_test: draw-task bounds pinned from the measured run with their derivation (NEW-50)

Co-Authored-By: Claude Fable 5.1 <noreply@anthropic.com>
EOF
```

---

### Task 5: Demonstrate RED, record it, re-capture the fixture

**Files:**
- Temporarily mutate: `~/Development/SynthUI/src/synthui_led_button_math.h`, `~/Development/SynthUI/src/synthui_led_button.cpp`
- Modify: `~/Development/rt1170/evkb/examples/display/synthui_led_button_test/run_qemu.sh` (header), `synthui_led_button_test.cpp` (file-top comment), `transcript_qemu.txt`

- [ ] **Step 1: Mutant A — corner term dropped, must fail delta equality.** In the SynthUI checkout edit `synthui_led_button_cue_boxes`: replace the `int32_t band = (int32_t)ceilf(...)` line with `int32_t band = bw + SYNTHUI_LED_BUTTON_CUE_MARGIN_PX;`. Then:

```bash
cd ~/Development/rt1170/evkb/examples/display/synthui_led_button_test && cmake --build build 2>&1 | tail -1 && ./run_qemu.sh 2>&1 | grep -E "led_button_(delta_crc|fresh_crc|delta_eq|damage_op)|^FAIL"
```

Expected: `led_button_delta_eq=FAIL` and `FAIL: delta render differs from full render (0x... vs 0x463C3371)`. Note the delta crc printed. Revert: `cd ~/Development/SynthUI && git checkout src/synthui_led_button_math.h`.

- [ ] **Step 2: Mutant B — clip test removed, must fail ONLY the task bound.** In the SynthUI checkout change `led_in_clip` to `{ (void)layer; (void)a; return true; }`. Rebuild and run as above, grepping also for `led_button_tasks_op|led_button_crc`.

Expected: `led_button_crc=0xD474F06D`, `led_button_fresh_crc=0x463C3371`, `led_button_delta_eq=PASS`, `led_button_tasks_op ... cue=48 ...` (lit/press/color unchanged), and `FAIL: cue draw tasks above its bound (cue=48 > N)`. **Every golden green and delta equality green with the gate red** is the result to record. Revert: `git checkout src/synthui_led_button.cpp`.

- [ ] **Step 3: Rebuild the clean widget and confirm green again.**

```bash
cd ~/Development/rt1170/evkb/examples/display/synthui_led_button_test && cmake --build build 2>&1 | tail -1 && ./run_qemu.sh 2>&1 | tail -1
```

Expected: `PASS: SynthUI led_button render verified`.

- [ ] **Step 4: Record the demos in the gate header.** In `run_qemu.sh`, after the existing `# Demonstrated RED 2026-09-16 (final review, NEW-25)` paragraph, add:

```sh
#
# Demonstrated RED 2026-09-16 (NEW-50, cue band + clip-aware led_draw):
#   - synthui_led_button_set_cue left at lv_obj_invalidate(obj) (the pre-NEW-50
#     widget, no mutant needed) against the re-derived bound
#       -> "FAIL: cue damage above its box (cue=10000 > 900)"
#   - cue_boxes corner term dropped (band = bw + 1; SynthUI
#     src/synthui_led_button_math.h) -- the strips miss the ring on the corner
#     diagonals, and the stale corners of every key whose cue toggled an odd
#     number of times leave the delta render different from a fresh one
#       -> "FAIL: delta render differs from full render (0x<A> vs 0x463C3371)"
#   - led_in_clip made `return true` (SynthUI src/synthui_led_button.cpp):
#     four strips x 12 unconditional layers
#       -> "FAIL: cue draw tasks above its bound (cue=48 > <N>)"
#     with led_button_crc=0xD474F06D, led_button_fresh_crc=0x463C3371 and
#     led_button_delta_eq=PASS all still GREEN -- the whole argument for the
#     task counter existing.  Baseline before NEW-50 (whole-key cue, no clip
#     test): tasks_op lit=<n> press=<n> cue=<n> color=<n>.
```

(Fill `<A>` and `<N>` from the runs.)

- [ ] **Step 5: Update the example's file-top comment.** In `synthui_led_button_test.cpp` replace the sentence beginning `Keys 13..15 are excluded from the LCG so the engagement bound (10000 px) is exactly the largest legitimate box of a 100 px key (a cue change repaints the whole key); a 150 px key's cue would be 22500 and say nothing about engagement.` with:

```
 * Keys 13..15 are excluded from the LCG so every per-op bound is exactly the
 * op's box on a 100 px key: press 6640 (the cap group at both offsets), lit
 * and colour 1450 (the halo), cue 900 (ONE bezel-ring strip, 100 x 9 --
 * NEW-50; it was 10000, the whole key, before).  A 150 px key would make
 * each of those larger and say nothing about engagement.
 * led_button_tasks_op (NEW-50) counts draw tasks per setter call, max per
 * op, from LV_EVENT_DRAW_TASK_ADDED on the keys: it is the only number that
 * can see led_draw losing its per-layer clip test, which would make a cue
 * change cost 48 tasks (four strips x twelve layers) while every golden and
 * the delta-equality guard stayed green.
```

- [ ] **Step 6: Re-capture the fixture** — the gate's own output changed (`tasks_op` line, cue=900), so the committed transcript is stale until this is done:

```bash
cd ~/Development/rt1170/evkb/examples/display/synthui_led_button_test && ./run_qemu.sh >/dev/null 2>&1; cp build/synthui_led_button.uart transcript_qemu.txt && grep -E "led_button_(damage_op|tasks_op)" transcript_qemu.txt
```

Expected: the two lines with the measured values. Then confirm the fixture replays green through the vacuity harness's own mechanism in Task 6.

- [ ] **Step 7: Commit.**

```bash
cd ~/Development/rt1170/evkb && git add examples/display/synthui_led_button_test/ && git commit -F - <<'EOF'
synthui_led_button_test: NEW-50 RED demos recorded (pre-fix widget, corner term dropped, clip test removed -- the last with every golden GREEN), fixture re-captured

Co-Authored-By: Claude Fable 5.1 <noreply@anthropic.com>
EOF
```

---

### Task 6: Vacuity cases for the new assertions

**Files:**
- Modify: `~/Development/rt1170/evkb/tools/gate-vacuity.test.sh` (section 16)

- [ ] **Step 1: Add three led_button cases** at the end of section 16, after the `led_button_delta_mismatch_fails_by_name` report and before its `else`. Replace `<CUE_MEASURED>` and `<CUE_MEASURED+1>` with the numbers pinned in Task 4:

```sh
    # NEW-50: the per-op draw-task line.  Absent, it must fail by name -- an
    # absent counter is not a pass (same rule as the area line above).
    grep -v "^led_button_tasks_op " "$EVKB/$LBT/transcript_qemu.txt" > "$WORK/lb_notasks.txt"
    run_gate "$LBT" "run_qemu.sh" "$WORK/lb_notasks.txt"; rc=$?
    result=0
    [ "$rc" -ne 0 ] || result=1
    echo "$OUT_TEXT" | grep -q "^FAIL: per-op draw-task line missing" || result=1
    report "led_button_missing_tasks_line_fails" $result

    # cue one task over its bound, by name.  cmp guards the mutation: a sed
    # that matched nothing would replay the green fixture.
    sed 's|^\(led_button_tasks_op lit=[0-9]* press=[0-9]*\) cue=<CUE_MEASURED> |\1 cue=<CUE_MEASURED+1> |' \
        "$EVKB/$LBT/transcript_qemu.txt" > "$WORK/lb_tasksover.txt"
    run_gate "$LBT" "run_qemu.sh" "$WORK/lb_tasksover.txt"; rc=$?
    result=0
    [ "$rc" -ne 0 ] || result=1
    cmp -s "$EVKB/$LBT/transcript_qemu.txt" "$WORK/lb_tasksover.txt" && result=1
    echo "$OUT_TEXT" | grep -q "^FAIL: cue draw tasks above its bound" || result=1
    report "led_button_cue_tasks_over_bound_fails_by_name" $result

    # cue one pixel over its re-derived 900, by name -- the bound that moved
    # an order of magnitude and would be the first to be quietly loosened.
    sed 's|^\(led_button_damage_op lit=[0-9]* press=[0-9]*\) cue=900 |\1 cue=901 |' \
        "$EVKB/$LBT/transcript_qemu.txt" > "$WORK/lb_cueover.txt"
    run_gate "$LBT" "run_qemu.sh" "$WORK/lb_cueover.txt"; rc=$?
    result=0
    [ "$rc" -ne 0 ] || result=1
    cmp -s "$EVKB/$LBT/transcript_qemu.txt" "$WORK/lb_cueover.txt" && result=1
    echo "$OUT_TEXT" | grep -q "^FAIL: cue damage above its box" || result=1
    report "led_button_cue_over_bound_fails_by_name" $result
```

- [ ] **Step 2: Run ONLY section 16 to iterate quickly** — the full suite is ~9 min. Temporarily run the suite and grep its led_button lines (nothing else may run QEMU meanwhile):

```bash
cd ~/Development/rt1170/evkb && sh tools/gate-vacuity.test.sh 2>&1 | grep -E "led_button|^SKIP|FAILED|^PASS: [0-9]" | tail -12
```

Expected: nine `PASS: led_button_*` lines including the three new ones; no `FAIL:`.

- [ ] **Step 3: Commit.**

```bash
cd ~/Development/rt1170/evkb && git add tools/gate-vacuity.test.sh && git commit -F - <<'EOF'
gate-vacuity: three NEW-50 negatives on synthui_led_button_test -- missing tasks line, cue tasks over bound, cue area over its re-derived 900, each by name

Co-Authored-By: Claude Fable 5.1 <noreply@anthropic.com>
EOF
```

---

### Task 7: acid_box — bound full-frame presents, demonstrate RED, re-capture

**Files:**
- Modify: `~/Development/rt1170/evkb/examples/display/acid_box/run_qemu.sh`
- Temporarily mutate: `~/Development/rt1170/evkb/examples/display/acid_box/acid_box.cpp` (`ui_poll`)
- Modify: `~/Development/rt1170/evkb/examples/display/acid_box/transcript_qemu.txt`, `tools/gate-vacuity.test.sh` (section 15)

- [ ] **Step 1: Rebuild acid_box's gate image against the changed widget and confirm its golden is unmoved.**

```bash
cd ~/Development/rt1170/evkb/examples/display/acid_box && cmake --build build 2>&1 | grep -E "ITCM|error" ; ./run_qemu.sh 2>&1 | grep -E "ACIDBOX_UI_SUM|ACIDBOX_ROT ops|^FAIL|^PASS" | tail -4
```

Expected: `ACIDBOX_UI_SUM=0xBB2AEE59`, the last `ACIDBOX_ROT ops=<n> full=2 ...`, `PASS`. Note the ITCM line (`ITCM: <used> B 256 KB <pct>%`): headroom = 262144 − used, expected ≈ 2,884 B as before (libSynthUI is flash-routed); record the number.

- [ ] **Step 2: Add the bound.** In `run_qemu.sh`, immediately after `[ "$ROT_OPS" -gt "$ROT_FULL" ] || { echo "FAIL: the damage path never ran (ops == full)"; exit 1; }` add:

```sh
# NEW-50: full-frame presents are bounded ABOVE as well.  LVGL's inv-area
# buffer holds LV_INV_BUF_SIZE=32 rects and on overflow invalidates the WHOLE
# SCREEN (lv_refr.c) -- every golden stays green and every frame becomes a
# full present.  The playhead now damages eight strips per step (two cue
# changes x four bezel-ring boxes), so that overflow is the regression this
# change could introduce, and `full` is the only witness that names it: the
# two start-up presents are the only legitimate full-frame presents in a run
# (full=2 on every fixture line and on silicon at ops=12538).
# Demonstrated RED 2026-09-16: lv_obj_invalidate(lv_screen_active()) added to
# ui_poll -> full=<F> -> "FAIL: full-frame presents above start-up's two (full=<F>)".
[ "$ROT_FULL" -le 2 ] || { echo "FAIL: full-frame presents above start-up's two (full=$ROT_FULL)"; exit 1; }
```

- [ ] **Step 3: Demonstrate RED.** In `acid_box.cpp`'s `ui_poll()`, temporarily add `lv_obj_invalidate(lv_screen_active());` as its first statement after `(void)t;`. Rebuild and run:

```bash
cd ~/Development/rt1170/evkb/examples/display/acid_box && cmake --build build 2>&1 | tail -1 && ./run_qemu.sh 2>&1 | grep -E "ACIDBOX_UI_SUM|ACIDBOX_ROT ops|^FAIL" | tail -3
```

Expected: `ACIDBOX_UI_SUM=0xBB2AEE59` (goldens green — that is the point), last `ACIDBOX_ROT ... full=<hundreds>`, `FAIL: full-frame presents above start-up's two (full=<F>)`. Revert `acid_box.cpp` (`git checkout examples/display/acid_box/acid_box.cpp`), rebuild, fill `<F>` into the gate comment, re-run: `PASS`.

- [ ] **Step 4: Re-capture the fixture and add the vacuity case.**

```bash
cd ~/Development/rt1170/evkb/examples/display/acid_box && ./run_qemu.sh >/dev/null 2>&1; cp build/acid_box.uart transcript_qemu.txt && grep -c "^ACIDBOX_ROT ops" transcript_qemu.txt
```

In `tools/gate-vacuity.test.sh` section 15, before `acb_bad_golden_fails_by_name`'s block, add:

```sh
    # NEW-50: full-frame presents above start-up's two must fail by name --
    # the inv-buffer-overflow signature is every golden green and every frame
    # a full present.  Every ROT line is rewritten so the LAST one (the one
    # the gate reads) carries full=3; cmp guards the mutation.
    sed 's|^\(ACIDBOX_ROT ops=[0-9]*\) full=2 |\1 full=3 |' \
        "$EVKB/$ACB/transcript_qemu.txt" > "$WORK/acb_fullover.txt"
    run_gate "$ACB" "run_qemu.sh" "$WORK/acb_fullover.txt"; rc=$?
    result=0
    [ "$rc" -ne 0 ] || result=1
    cmp -s "$EVKB/$ACB/transcript_qemu.txt" "$WORK/acb_fullover.txt" && result=1
    echo "$OUT_TEXT" | grep -q "^FAIL: full-frame presents above start-up" || result=1
    report "acb_full_presents_over_bound_fails_by_name" $result
```

Run the suite's acid_box lines:

```bash
cd ~/Development/rt1170/evkb && sh tools/gate-vacuity.test.sh 2>&1 | grep -E "acb_|acid_box" | tail -8
```

Expected: six `PASS: ...acb...` lines (five old, one new).

- [ ] **Step 5: Commit.**

```bash
cd ~/Development/rt1170/evkb && git add examples/display/acid_box/run_qemu.sh examples/display/acid_box/transcript_qemu.txt tools/gate-vacuity.test.sh && git commit -F - <<'EOF'
acid_box: full-frame presents bounded at start-up's two -- the inv-buffer-overflow witness now that the playhead damages eight bezel-ring strips per step (NEW-50); RED demo via a per-tick screen invalidate, golden 0xBB2AEE59 unmoved, fixture re-captured, vacuity negative added

Co-Authored-By: Claude Fable 5.1 <noreply@anthropic.com>
EOF
```

---

### Task 8: Push SynthUI, bump the pin, rebuild what a pin bump invalidates

**Files:**
- Modify: `~/Development/rt1170/evkb/evkb.cmake:131`

- [ ] **Step 1: Push SynthUI and take the SHA.**

```bash
cd ~/Development/SynthUI && git push origin master && git rev-parse HEAD && git status --short
```

Expected: push succeeds, a 40-char SHA, empty status. Confirm on the remote: `git ls-remote origin master` shows the same SHA.

- [ ] **Step 2: Bump the pin.** In `evkb.cmake` line 131 replace `075c137918cdf201f6c9f9823119435899b1a4da` with the full SHA and prepend to its comment's history: `Bumped 2026-09-16 (NEW-50: set_cue damages the bezel ring as four strips, led_draw clip-aware, draw dscs carry base.obj -- pixel-neutral, verified by all four goldens not moving; earlier: 2026-09-16 (synthui_led_button bezel cue width from the palette, NEW-25 final review ...` (keep the rest of the existing text).

- [ ] **Step 3: Rebuild the seven self-building gate dirs and the twelve SynthUI-linking dirs** — an `evkb.cmake` edit makes every build reconfigure, and the self-building gates would otherwise do it inside their 120 s budget and read as `exit status 124`.

```bash
cd ~/Development/rt1170/evkb && for d in examples/audio/bt_sink_test/build examples/audio/bt_tone_test/build-soak examples/audio/bt_tone_test/build-lifecycle examples/audio/bt_tone_test/build-media examples/networking/m2_hci_probe/build-avdtp examples/networking/m2_hci_probe/build-baud examples/networking/m2_hci_probe/build-reconnect examples/display/synthui_led_button_test/build examples/display/acid_box/build examples/display/synthui_knob_test/build examples/display/synthui_slide_toggle_test/build examples/display/synthui_lamp_test/build examples/display/synthui_panel_button_test/build examples/display/synthui_level_meter_test/build examples/display/synthui_fader_test/build examples/display/synthui_piano_key_test/build examples/display/vglite_lvgl_test/build examples/display/synthui_seven_segment_test/build examples/display/synthui_step_test/build; do echo "== $d"; cmake --build "$d" 2>&1 | grep -E "error|Error|ITCM|Linking" | tail -2; done
```

Expected: every dir links (`Linking CXX executable`), no `error`. A dir failing with `COMPILERPATH is UNDEFINED` or `toolchain file not found` is the pre-2026-08-14 staleness: `rm -rf` it and configure fresh with `cmake -B build -DCMAKE_TOOLCHAIN_FILE=../../../toolchain/rt1170-evkb.toolchain.cmake` from its example dir (for the seven gate-owned dirs, let the GATE rebuild them instead — run that gate once now so the reconfigure is out of the sweep's budget).

- [ ] **Step 4: acid_box ITCM, all in-place bench builds.** `build` is the tight one (2,884 B before). Rebuild the human-owned BT bench dirs IN PLACE (never reconfigure from scratch — they carry the real firmware blob):

```bash
cd ~/Development/rt1170/evkb/examples/display/acid_box && for d in build build-bt build-bench build-loopstat; do echo "== $d"; cmake --build "$d" 2>&1 | grep -E "^ *ITCM:|headroom|error" | tail -2; done
```

Expected: each prints an `ITCM:` usage line and no `headroom below` assert; compute `262144 − used` for each and record the four numbers next to the spec's §8.6 (expected unchanged within a few bytes of 2,884 / 11,604 / 11,412 / 2,756).

- [ ] **Step 5: Commit the pin.**

```bash
cd ~/Development/rt1170/evkb && git add evkb.cmake && git commit -F - <<'EOF'
evkb.cmake: SynthUI pin -> <SHA7> (NEW-50 cue band + clip-aware led_draw); pixel-neutral, all four goldens unmoved; ITCM headroom acid_box build=<n> build-bt=<n> build-bench=<n> build-loopstat=<n> B

Co-Authored-By: Claude Fable 5.1 <noreply@anthropic.com>
EOF
```

---

### Task 9: Sweep, vacuity, audit, fresh-user — one at a time

- [ ] **Step 1: Read `docs/KNOWN-BROKEN-GATES.md`**, then confirm the runner sees 141 gates:

```bash
cd ~/Development/rt1170/evkb && ./tools/run-all-qemu-gates.sh -l | tail -1
```

Expected: `(141 gate(s))`.

- [ ] **Step 2: The sweep**, output captured, nothing else running:

```bash
cd ~/Development/rt1170/evkb && ./tools/run-all-qemu-gates.sh 2>&1 | tee /private/tmp/claude-501/-Users-nicholasnewdigate-Development-rt1170-evkb/3b4f7c19-6be4-4d2e-9cf0-58a2ff2e6ef5/scratchpad/sweep.log | tail -5
```

Expected: `gates: 141 passed`, exit 0 (~24 min). A red in the load-sensitivity class (`cm4_audio_test`, `cm4_wire_int_slave_test`, `m2_rx_demo[txaggr]`, `m2_uap_lwip[uap]`, `synthui_slide_toggle_test`, `bt_tone_test[media]`) is re-run ALONE, idle, before it is believed; any other red is a regression from this work — read the gate NAMES in the summary.

- [ ] **Step 3: The vacuity suite**, alone:

```bash
cd ~/Development/rt1170/evkb && sh tools/gate-vacuity.test.sh 2>&1 | tee /private/tmp/claude-501/-Users-nicholasnewdigate-Development-rt1170-evkb/3b4f7c19-6be4-4d2e-9cf0-58a2ff2e6ef5/scratchpad/vacuity.log | grep -cE "^PASS:"; grep -E "^FAIL:|^SKIP:" /private/tmp/claude-501/-Users-nicholasnewdigate-Development-rt1170-evkb/3b4f7c19-6be4-4d2e-9cf0-58a2ff2e6ef5/scratchpad/vacuity.log
```

Expected: `66` PASS (62 + 4), no FAIL, no SKIP. Count from the run, not from this plan.

- [ ] **Step 4: The licence audit**, after the sweep, never during:

```bash
cd ~/Development/rt1170/evkb && LICENSE_AUDIT_EVKB=$(pwd) ./tools/license-audit.sh 2>&1 | tail -3
```

Expected: `LICENSE-AUDIT: PASS`, 120 manifests.

- [ ] **Step 5: Fresh-user verification — RUN the gate on a GitHub-fetched ELF.**

```bash
cd ~/Development/rt1170/evkb/examples/display/synthui_led_button_test && rm -rf build-fetch && cmake -B build-fetch -DEVKB_FORCE_FETCH=ON -DCMAKE_TOOLCHAIN_FILE=../../../toolchain/rt1170-evkb.toolchain.cmake 2>&1 | grep -iE "synthui|clone|requested ref" | head -5 && cmake --build build-fetch 2>&1 | tail -1
```

Expected: the configure log shows SynthUI cloned at the new SHA; a clean link. Then run the gate against THAT elf:

```bash
cd ~/Development/rt1170/evkb/examples/display/synthui_led_button_test && mv build build.local && ln -s build-fetch build && ./run_qemu.sh 2>&1 | grep -E "led_button_(crc|fresh_crc|tasks_op)|^PASS|^FAIL"; rm build && mv build.local build
```

Expected: both goldens and `PASS: SynthUI led_button render verified` from the fetched source. Then `rm -rf build-fetch`.

---

### Task 10: Close-out record — CLAUDE.md, spec, memory, Linear, bench hand-off

**Files:**
- Modify: `~/Development/rt1170/evkb/CLAUDE.md`, `docs/superpowers/specs/2026-09-16-led-button-cue-damage-design.md`
- Create: `~/.claude/projects/-Users-nicholasnewdigate-Development-rt1170-evkb/memory/new50-led-button-cue-damage.md`; modify `MEMORY.md`

- [ ] **Step 1: CLAUDE.md** — insert a new `✅ **Measured 2026-09-16: 141 gates discovered, 141 passed, 0 failed, 0 SKIP**` block immediately BEFORE the existing `★★ **SILICON, 2026-09-16 (bench): both halves ACCEPTED` block, with: the sweep line and wall time from `sweep.log`; vacuity 66/66; audit PASS; fresh-user verified by RUNNING; SynthUI pin `<SHA7>`; **no new gate, 141 unchanged**; the four goldens unmoved (named); the cue bound 10000 → 900 and the task counts before/after (`12/12/12/12` → measured); the ITCM numbers; and these three ★ lessons in the file's voice:
  - ★ narrowing a damage box can be a PESSIMISATION: LVGL renders one pass per inv area and `lv_draw_rect` allocates before clipping, from a heap in uncached SDRAM — four strips × 12 = 48 tasks with every golden green; only a draw-task counter sees it.
  - ★ `led_draw` issues TWELVE tasks (the bezel's `lv_draw_rect` is two), and the win is pixel work (~25,000 → ~4,000 px), not task count (12 → 12).
  - ★ a widget whose dscs lack `base.obj` cannot be instrumented — `LV_EVENT_DRAW_TASK_ADDED` never fires; none of the sibling widgets set it either.
  - ★ the 30 fps question is STILL OPEN — bench pending (below).

- [ ] **Step 2: Spec** — append a `## 10. Measured (software, 2026-09-16)` section with the same numbers, and mark §5.1's predictions confirmed or refuted line by line (cue at 100 px predicted 12; cue's actual bound-setting key; the clip-unaware mutant's 48).

- [ ] **Step 3: Memory** — write `new50-led-button-cue-damage.md` (type `project`) with the state (software DONE, bench PENDING) and the three lessons, and add its line to `MEMORY.md`.

- [ ] **Step 4: Commit and push evkb.**

```bash
cd ~/Development/rt1170/evkb && git add CLAUDE.md docs/superpowers/specs/2026-09-16-led-button-cue-damage-design.md && git commit -F - <<'EOF'
docs: NEW-50 software close-out -- cue damage 10000 -> 900 with all four goldens unmoved, draw tasks flat at 12 by design (48 without the clip test), sweep 141/141/0, vacuity 66/66, audit PASS, fresh-user verified by running the gate

Co-Authored-By: Claude Fable 5.1 <noreply@anthropic.com>
EOF
git push origin master
```

- [ ] **Step 5: Linear** — `save_comment` on NEW-50 with the software result and the bench hand-off; leave the issue **In Progress** (the 30 fps half is open):

```
Software half DONE 2026-09-16 (evkb <sha>, SynthUI <sha>). Cue damage 10000 -> 900 (four bezel-ring strips, band 9 px at 100 px); led_draw clip-aware, so draw tasks stay 12 per cue change (48 without it -- demonstrated RED with every golden green, which is why the gate now counts tasks per op). All four goldens unmoved. Sweep 141/141/0, vacuity 66/66, audit PASS.

BENCH PENDING -- the only thing that answers the issue:
1. acid_box touch p95, transport STOPPED vs PLAYING, two separate boots (percentiles cannot be differenced); before: 37.8 / 66.8 ms.
2. synthui_led_button_test Phase B fps; before: 20.6 (median frame 48.6 ms).
3. acid_box gpu golden 0xEA5AB843 re-confirmed, ACIDBOX_VSYNC timeouts=0, ROT_EQ fail=0.
Prediction on record: task count flat, pixel work ~6x less, so the gain is bounded by the fill share of the 48.6 ms frame. If 30 fps is still missed, a GC355 compositor for led_button is the honest next step and gets its own issue.
```

---

## Self-review

**Spec coverage:** §3 geometry → Task 1; §4.1 header → Task 1; §4.2 widget (set_cue, clip-aware) → Task 4; `base.obj` enabling → Task 2; §5.1 task counter + predictions → Tasks 3–4; §5.2 bounds → Task 3; §5.3 acid_box `full` → Task 7; §6 host suite + mutants → Task 1; §7.1 four RED demos → Tasks 3 (pre-fix widget), 5 (A, B), 7 (acid_box); §8 close-out → Tasks 8–9; §9 bench hand-off → Task 10. Vacuity cases (this tree's rule, not in the spec by name) → Tasks 6–7.

**Placeholders:** `<CUE_MEASURED>`, `<N>`, `<A>`, `<F>`, `<SHA7>` are deliberately measured-then-filled values, each named at the step that produces it — the spec pre-registers them as predictions, not numbers to assert now.

**Type consistency:** `synthui_led_button_cue_boxes(float, float, synthui_led_button_px_t[4])` and `synthui_led_button_radius_px(float)` are used with those signatures in Tasks 1, 2, 4; `led_fill(layer, b, &a, ...)` / `led_grad(layer, b, &a, ...)` match between Tasks 2 and 4; `delta_refr()`/`delta_task_cb` names match between Task 3's code and its build_bank hook; gate variable names `TCUE`/`TLIT`/`TCOL`/`TPRS` match between Tasks 3, 4 and 6's fixture seds.
