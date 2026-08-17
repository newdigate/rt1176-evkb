# Acid Box Capstone Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** A playable acid groovebox — 303 voice + transport + step sequencer wired to a touch UI (8 SynthUI knobs, new step-cell widget) on the RK055, I2S headphone out — verified end-to-end: touch → pattern → sound.

**Architecture:** Thin glue over existing library code. SynthUI grows a knob input layer (vertical drag) and a `synthui_step` widget; the app connects LVGL events directly to voice/sequencer/transport setters (all internally IRQ-guarded, verified), drains sequencer note events from an `IntervalTimer` at 1 kHz (immune to UI-frame jitter), and polls engine state from an `lv_timer` at 33 ms. The qemu2 GT911 model gains a `touch-script` property so the gate can inject taps AND drags (LOCAL-ONLY qemu2 change — same GPL-firewall class as `sai1-rxinject`; fresh clones see that gate red by design).

**Tech Stack:** newdigate/Audio (`AudioSynthAcidBass`, `AudioTransport`, `AudioStepSequencer`, `AudioOutputI2S`, `AudioControlWM8962`), newdigate/SynthUI, LVGL 9.4 software renderer, GT911 touch, ARM GCC 10, CMake, qemu2 `mimxrt1170-evk`.

**Spec:** `docs/superpowers/specs/2026-08-17-acid-box-capstone-design.md`. One spec refinement, decided here: the "audio checksum" of spec §5.1 is implemented as **audio-clock-referenced per-step windowed RMS assertions** (the `acid_bass_test` idiom — float DSP is asserted by measured windows with margin, never bit-goldens; only PIXEL goldens are bit-exact in this tree).

**Branch:** `acid-box`, in a worktree per superpowers:using-git-worktrees.

**Sibling-repo edits:** SynthUI (Tasks 1–2) and qemu2 (Task 6) are separate repos with their own commits. evkb pins SynthUI; qemu2 is never pinned (GPL firewall).

---

## ★ Sequencing rule

Widgets before app, app-audio before app-UI, UI before touch gate. Every task leaves something runnable and gated. Do not reorder: Task 3's goldens are the regression net for Task 5's UI reuse, and Task 4's audio tokens are what Task 7's gate asserts.

---

### Task 1: SynthUI — knob vertical-drag input layer

**Files:**
- Create: `~/Development/SynthUI/src/synthui_knob_math.h`
- Create: `~/Development/SynthUI/tests/knob_math_test.c`
- Modify: `~/Development/SynthUI/src/synthui_knob.cpp` (constructor + one new static)
- Modify: `~/Development/SynthUI/src/synthui_knob.h` (doc comment only)

- [ ] **Step 1: Write the failing host test for the drag math**

Create `~/Development/SynthUI/tests/knob_math_test.c`:

```c
/* Host-compiled unit test for the pure drag math -- no LVGL, no target.
 * Build: cc -o /tmp/knob_math_test tests/knob_math_test.c && /tmp/knob_math_test
 * SPDX-License-Identifier: MIT */
#include "../src/synthui_knob_math.h"
#include <assert.h>
#include <math.h>
#include <stdio.h>

static int near(float a, float b) { return fabsf(a - b) < 0.01f; }

int main(void)
{
    /* 200 px of upward drag (dy = -200) is a full 280-deg sweep: from the
     * minimum it lands exactly on the maximum. From center it clamps there. */
    assert(near(synthui_knob_drag(-140.0f, -140.0f, 140.0f, 0.0f, -200), 140.0f));
    assert(near(synthui_knob_drag(   0.0f, -140.0f, 140.0f, 0.0f, -200), 140.0f));
    /* Half a full stroke (-100 px) is half the sweep: -140 -> 0. */
    assert(near(synthui_knob_drag(-140.0f, -140.0f, 140.0f, 0.0f, -100), 0.0f));
    /* Downward drag decreases; clamps at min. */
    assert(near(synthui_knob_drag(-100.0f, -140.0f, 140.0f, 0.0f, 200), -140.0f));
    /* Detent mode snaps to the nearest multiple of detent_step. */
    assert(near(synthui_knob_drag(0.0f, -140.0f, 140.0f, 35.0f, -20), 35.0f));
    assert(near(synthui_knob_drag(0.0f, -140.0f, 140.0f, 35.0f, -5), 0.0f));
    /* Zero drag is identity (no snap drift on touch-down). */
    assert(near(synthui_knob_drag(17.0f, -140.0f, 140.0f, 0.0f, 0), 17.0f));
    printf("knob_math: all PASS\n");
    return 0;
}
```

- [ ] **Step 2: Run it to make sure it fails**

```bash
cd ~/Development/SynthUI && cc -o /tmp/knob_math_test tests/knob_math_test.c && /tmp/knob_math_test
```
Expected: FAIL to compile — `synthui_knob_math.h: No such file or directory`.

- [ ] **Step 3: Implement the math header**

Create `~/Development/SynthUI/src/synthui_knob_math.h`:

```c
/* synthui_knob_math.h - pure drag arithmetic for the knob's input layer.
 * Header-only and LVGL-free so a host compiler can unit-test it directly;
 * synthui_knob.cpp includes it for the real widget.
 * SPDX-License-Identifier: MIT */
#pragma once
#include <math.h>

/* Full sweep of (max_deg - min_deg) per this many pixels of vertical drag.
 * 200 px was chosen against the RK055's ~295 DPI: a comfortable thumb
 * stroke (~17 mm) covers the whole range, and one pixel is ~1.4 deg. */
#define SYNTHUI_KNOB_DRAG_FULL_PX 200.0f

/* dy_px is SCREEN-space vertical delta (LVGL vect.y: positive = downward).
 * Upward drag increases the angle -- the touch-synth convention. detent_step
 * of 0 means continuous; otherwise the result snaps to the nearest multiple
 * (relative to 0), matching the widget's MODE_DETENTS drawing. */
static inline float synthui_knob_drag(float angle_deg, float min_deg,
                                      float max_deg, float detent_step,
                                      int dy_px)
{
    const float span = max_deg - min_deg;
    float a = angle_deg - (float)dy_px * (span / SYNTHUI_KNOB_DRAG_FULL_PX);
    if (a < min_deg) a = min_deg;
    if (a > max_deg) a = max_deg;
    if (detent_step > 0.0f)
        a = roundf(a / detent_step) * detent_step;
    if (a < min_deg) a = min_deg;   /* snap may not escape the range */
    if (a > max_deg) a = max_deg;
    return a;
}
```

- [ ] **Step 4: Run the test to verify it passes**

```bash
cd ~/Development/SynthUI && cc -o /tmp/knob_math_test tests/knob_math_test.c && /tmp/knob_math_test
```
Expected: `knob_math: all PASS`, exit 0.

- [ ] **Step 5: Wire the input layer into the widget**

In `~/Development/SynthUI/src/synthui_knob.cpp`, add near the other includes:

```c
#include "synthui_knob_math.h"
```

Add this static handler (place beside the existing draw event callback; `synthui_knob_t` is the widget struct already defined in this file — use its real angle/min/max/detent members, whatever they are named there):

```c
/* Vertical-drag input: accumulate the indev's per-poll vector while pressed.
 * PRESSING fires per indev read (10 ms under the GT911 binding), vect is the
 * delta since the previous read, so summing arrives at total drag without
 * storing a press-origin. Emits VALUE_CHANGED only when the EFFECTIVE angle
 * moved (a detent knob mid-detent stays silent). Draws nothing itself --
 * synthui_knob_set_angle() owns invalidation -- so the display path and the
 * pilot's five goldens are untouched by construction. */
static void knob_input_pressing(lv_event_t *e)
{
    lv_obj_t *obj = (lv_obj_t *)lv_event_get_target(e);
    lv_indev_t *indev = lv_indev_active();
    if (indev == NULL) return;
    lv_point_t vect;
    lv_indev_get_vect(indev, &vect);
    if (vect.y == 0) return;

    synthui_knob_t *k = (synthui_knob_t *)obj;
    const float detent =
        (k->mode == SYNTHUI_KNOB_MODE_DETENTS) ? k->detent_step : 0.0f;
    const float next = synthui_knob_drag(k->angle, k->min_deg, k->max_deg,
                                         detent, vect.y);
    if (next == k->angle) return;
    synthui_knob_set_angle(obj, next);
    lv_obj_send_event(obj, LV_EVENT_VALUE_CHANGED, NULL);
}
```

In the widget constructor (where the existing event callbacks are attached), add:

```c
    lv_obj_add_flag(obj, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(obj, knob_input_pressing, LV_EVENT_PRESSING, NULL);
```

In `~/Development/SynthUI/src/synthui_knob.h`, extend the header comment for `synthui_knob_set_angle` with one line: `Emits nothing itself; the input layer (vertical drag, 200 px = full sweep) emits LV_EVENT_VALUE_CHANGED.`

**Adaptation note (not a placeholder):** member names above (`angle`, `min_deg`, `max_deg`, `detent_step`, `mode`) must match the struct as it exists in `synthui_knob.cpp` — read the struct first and use its exact names. If `set_angle` clamps internally, still pass the clamped `next` (idempotent).

- [ ] **Step 6: Prove the display path is untouched — the pilot goldens**

```bash
cd /Users/nicholasnewdigate/Development/rt1170/evkb/examples/display/synthui_knob_test && \
  cmake --build build -j 8 && ./run_qemu.sh
```
Expected: `PASS` with all five mode goldens unchanged. If ANY golden moved, the input layer drew something — that is a defect in Step 5, never a reason to re-record.

- [ ] **Step 7: Commit (SynthUI repo)**

```bash
cd ~/Development/SynthUI && git add src/synthui_knob_math.h src/synthui_knob.cpp src/synthui_knob.h tests/knob_math_test.c && \
  git commit -m "knob: vertical-drag input layer -- 200px full sweep, detent snap, VALUE_CHANGED

Pure math in a host-testable header (tests/knob_math_test.c); the widget
handler accumulates LV_EVENT_PRESSING vectors. Draws nothing: the pilot's
five goldens verified bit-identical after the change."
```

### Task 2: SynthUI — the `synthui_step` widget

**Files:**
- Create: `~/Development/SynthUI/src/synthui_step.h`
- Create: `~/Development/SynthUI/src/synthui_step.cpp`

- [ ] **Step 1: Write the header**

Create `~/Development/SynthUI/src/synthui_step.h`:

```c
/* synthui_step.h - one step cell of a sequencer lane. The library's second
 * widget. Displays gate (fill), accent (dot, top-right), slide (bar, bottom),
 * cursor (inset ring) and selected (outline) -- all independent so a cell can
 * be e.g. gated+accented+cursor at once. Emits LV_EVENT_CLICKED (stock LVGL
 * click handling; the widget adds no input code of its own).
 * SPDX-License-Identifier: MIT */
#pragma once
#include "lvgl.h"
#ifdef __cplusplus
extern "C" {
#endif

lv_obj_t *synthui_step_create(lv_obj_t *parent);
void synthui_step_set(lv_obj_t *obj, bool gate, bool accent, bool slide);
void synthui_step_set_cursor(lv_obj_t *obj, bool on);
void synthui_step_set_selected(lv_obj_t *obj, bool on);
bool synthui_step_gate(const lv_obj_t *obj);

#ifdef __cplusplus
}
#endif
```

- [ ] **Step 2: Implement the widget**

Create `~/Development/SynthUI/src/synthui_step.cpp`, following `synthui_knob.cpp`'s class pattern exactly (same macros, same event plumbing — read that file and mirror its `lv_obj_class_t` boilerplate; the DRAW body below is complete):

```c
/* synthui_step.cpp - see header. Drawing mirrors the DC reference art the
 * knob uses; hexes below are the acid-box mockup set, chosen to sit on the
 * #101820 ground the examples use.
 * SPDX-License-Identifier: MIT */
#include "synthui_step.h"
#include <string.h>

typedef struct {
    lv_obj_t obj;
    bool gate, accent, slide, cursor, selected;
} synthui_step_t;

/* palette */
#define STEP_BG_OFF   lv_color_hex(0x1a2230)
#define STEP_BG_ON    lv_color_hex(0x39406e)
#define STEP_ACCENT   lv_color_hex(0xc2543f)
#define STEP_SLIDE    lv_color_hex(0x8f96d4)
#define STEP_CURSOR   lv_color_hex(0x3fa060)
#define STEP_SELECT   lv_color_hex(0xfdfdf9)

static void step_draw(lv_event_t *e)
{
    lv_obj_t *obj = (lv_obj_t *)lv_event_get_target(e);
    synthui_step_t *s = (synthui_step_t *)obj;
    lv_layer_t *layer = lv_event_get_layer(e);
    lv_area_t a;
    lv_obj_get_coords(obj, &a);
    const int32_t w = lv_area_get_width(&a);

    /* body fill */
    lv_draw_rect_dsc_t d;
    lv_draw_rect_dsc_init(&d);
    d.radius = w / 8;
    d.bg_color = s->gate ? STEP_BG_ON : STEP_BG_OFF;
    d.bg_opa = LV_OPA_COVER;
    if (s->selected) {
        d.border_color = STEP_SELECT;
        d.border_width = 2;
        d.border_opa = LV_OPA_COVER;
    }
    lv_draw_rect(layer, &d, &a);

    /* cursor: inset ring, drawn over the fill so it reads on ON and OFF */
    if (s->cursor) {
        lv_draw_rect_dsc_t c;
        lv_draw_rect_dsc_init(&c);
        c.radius = w / 8;
        c.bg_opa = LV_OPA_TRANSP;
        c.border_color = STEP_CURSOR;
        c.border_width = 3;
        c.border_opa = LV_OPA_COVER;
        lv_area_t ca = a;
        lv_area_increase(&ca, -3, -3);
        lv_draw_rect(layer, &c, &ca);
    }

    /* accent: dot, top-right */
    if (s->accent) {
        lv_draw_rect_dsc_t dot;
        lv_draw_rect_dsc_init(&dot);
        dot.radius = LV_RADIUS_CIRCLE;
        dot.bg_color = STEP_ACCENT;
        dot.bg_opa = LV_OPA_COVER;
        lv_area_t da = { a.x2 - w / 4 - 2, a.y1 + 4,
                         a.x2 - 4,         a.y1 + w / 4 + 2 };
        lv_draw_rect(layer, &dot, &da);
    }

    /* slide: bar along the bottom */
    if (s->slide) {
        lv_draw_rect_dsc_t bar;
        lv_draw_rect_dsc_init(&bar);
        bar.radius = 2;
        bar.bg_color = STEP_SLIDE;
        bar.bg_opa = LV_OPA_COVER;
        lv_area_t ba = { a.x1 + 5, a.y2 - 8, a.x2 - 5, a.y2 - 4 };
        lv_draw_rect(layer, &bar, &ba);
    }
}

/* class boilerplate: constructor zeroes state, adds CLICKABLE, hooks
 * step_draw on LV_EVENT_DRAW_MAIN -- mirror synthui_knob.cpp's class
 * definition (same width/height defaults pattern, instance size
 * sizeof(synthui_step_t)). */

static void set_and_invalidate(lv_obj_t *obj) { lv_obj_invalidate(obj); }

void synthui_step_set(lv_obj_t *obj, bool gate, bool accent, bool slide)
{
    synthui_step_t *s = (synthui_step_t *)obj;
    if (s->gate == gate && s->accent == accent && s->slide == slide) return;
    s->gate = gate; s->accent = accent; s->slide = slide;
    set_and_invalidate(obj);
}
void synthui_step_set_cursor(lv_obj_t *obj, bool on)
{
    synthui_step_t *s = (synthui_step_t *)obj;
    if (s->cursor == on) return;
    s->cursor = on; set_and_invalidate(obj);
}
void synthui_step_set_selected(lv_obj_t *obj, bool on)
{
    synthui_step_t *s = (synthui_step_t *)obj;
    if (s->selected == on) return;
    s->selected = on; set_and_invalidate(obj);
}
bool synthui_step_gate(const lv_obj_t *obj)
{
    return ((const synthui_step_t *)obj)->gate;
}
```

**Adaptation note:** the class-definition block ("class boilerplate" comment) is written by copying `synthui_knob.cpp`'s `lv_obj_class_t` + `_create` + constructor-cb block and renaming; the draw hook registers `step_draw` on `LV_EVENT_DRAW_MAIN`. Keep the no-change-early-return pattern above — it is what keeps redraws cheap.

- [ ] **Step 3: Compile check via any evkb LVGL example**

```bash
cd /Users/nicholasnewdigate/Development/rt1170/evkb/examples/display/synthui_knob_test && \
  cmake --build build -j 8
```
Expected: clean build (the SynthUI library target now compiles `synthui_step.cpp`; if the library lists sources explicitly, add it to that list — check `import_evkb_library`'s glob first).

- [ ] **Step 4: Commit (SynthUI repo)**

```bash
cd ~/Development/SynthUI && git add src/synthui_step.h src/synthui_step.cpp && \
  git commit -m "step: sequencer step-cell widget -- gate/accent/slide/cursor/selected

Second widget. States independent, redraw only on effective change,
input is stock LVGL clicks. Verified by evkb synthui_step_test (its
goldens live there, next commit on that side)."
```

### Task 3: evkb — `synthui_step_test` example + gate

**Files:**
- Create: `examples/display/synthui_step_test/CMakeLists.txt`
- Create: `examples/display/synthui_step_test/synthui_step_test.cpp`
- Create: `examples/display/synthui_step_test/run_qemu.sh`

- [ ] **Step 1: CMakeLists** — copy `examples/display/synthui_knob_test/CMakeLists.txt` verbatim, rename target/sources to `synthui_step_test`.

- [ ] **Step 2: The scene.** Create `synthui_step_test.cpp` — a state-matrix grid: 8 columns (off, on, on+accent, on+slide, on+accent+slide, cursor-on-off, cursor-on-on, selected-on) × 2 rows (plain, and the same with selected added) of 90 px cells, dark ground, plus the token protocol of `synthui_knob_test` (banner, one frame, `lvgl_sum` FNV, `STEP_GRID_SUM=0x%08lX`, DONE). Mirror the knob test's `setup()` shape exactly (panel begin, `lvgl_rt1176_begin`, build, render-once, sum, tokens); the scene body:

```cpp
static lv_obj_t *cell(lv_obj_t *scr, int col, int row,
                      bool g, bool a, bool s, bool cur, bool sel)
{
    lv_obj_t *c = synthui_step_create(scr);
    lv_obj_set_size(c, 74, 74);
    lv_obj_set_pos(c, 8 + col * 88, 200 + row * 96);
    synthui_step_set(c, g, a, s);
    synthui_step_set_cursor(c, cur);
    synthui_step_set_selected(c, sel);
    return c;
}
static void build_scene(lv_obj_t *scr)
{
    struct { bool g, a, s, cur; } v[8] = {
        {false,false,false,false}, {true,false,false,false},
        {true,true,false,false},   {true,false,true,false},
        {true,true,true,false},    {false,false,false,true},
        {true,false,false,true},   {true,true,true,true},
    };
    for (int r = 0; r < 2; r++)
        for (int i = 0; i < 8; i++)
            cell(scr, i, r, v[i].g, v[i].a, v[i].s, v[i].cur, r == 1);
}
```

- [ ] **Step 3: Gate.** Copy `synthui_knob_test/run_qemu.sh` structure: boot, wait for DONE, assert `PANEL_OK`, assert `STEP_GRID_SUM=` matches a recorded golden, assert the all-zero-framebuffer anti-golden `0x9BC99DC5` by name as a failure. Record the golden as `PENDING` first:

```bash
cd examples/display/synthui_step_test && \
  cmake -B build -DCMAKE_TOOLCHAIN_FILE=../../../toolchain/rt1170-evkb.toolchain.cmake && \
  cmake --build build -j 8 && ./run_qemu.sh
```
Expected on first run: FAIL (golden `PENDING` matches nothing) and the captured UART prints the real `STEP_GRID_SUM`.

- [ ] **Step 4: ★ LOOK, then adopt.** QEMU offers no frame dump, so at THIS
stage adopt the QEMU sum as *provisional*, marked
`# PROVISIONAL until Task 8 eyes-on-silicon` in `run_qemu.sh`. Task 8 dumps
the silicon frame with `tools/rt1170-screenshot.py`, a human looks at it,
and only then does the marker come off. A provisional golden pins
determinism; the eyes certify correctness — the marker keeps the two claims
separate, which is the knob pilot's lesson.
**Spec refinement, recorded:** spec §3.1 asked for the pilot's
QEMU/host-clang/silicon *triple* agreement. The host-clang leg was an
ad-hoc uncommitted harness the pilot used because no screenshot tool
existed then; with silicon frames now cheap to dump and inspect, this
example verifies two-way (QEMU sum + silicon frame under eyes) and skips
resurrecting the host build. Same evidence, one less scaffold.

- [ ] **Step 5: Vacuity.** Prove the gate fails on (a) a sentinel sum (`sed` the golden to `0xDEADBEEF`, run, expect FAIL), (b) a one-nibble mutation of the recorded value. Restore the real value after both.

- [ ] **Step 6: QEMU transcript + commit**

```bash
cd examples/display/synthui_step_test && ./run_qemu.sh   # green
# write transcript_qemu.txt from the captured UART, banner-commented as in
# vglite_lvgl_test/transcript_qemu.txt
git add examples/display/synthui_step_test && \
git commit -m "synthui_step_test: state-matrix goldens for the step widget (PROVISIONAL until silicon eyes)"
```

### Task 4: evkb — `acid_box` audio core (no UI yet)

**Files:**
- Create: `examples/display/acid_box/CMakeLists.txt` (copy `vglite_lvgl_test`'s, rename, import Audio + SynthUI + MipiDisplay soc/panels as `synthui_knob_test` and `acid_bass_test` do between them)
- Create: `examples/display/acid_box/acid_box.cpp`

- [ ] **Step 1: Write the audio core + preset pattern + pump + tokens**

`acid_box.cpp`, first slice (UI comes in Task 5 — `build_ui()` is a stub returning the screen with only the dark ground so the file compiles and the frame is deterministic):

```cpp
/* acid_box - the audio+display integration capstone.
 * Spec: docs/superpowers/specs/2026-08-17-acid-box-capstone-design.md
 * SPDX-License-Identifier: MIT */
#include <Arduino.h>
#include <Audio.h>
#include "control_wm8962.h"       // not pulled in by Audio.h -- named explicitly
#include "Display.h"
#include "lvgl_rt1176.h"
#include "lvgl_mipi_panel.h"
#include "lvgl_gt911_indev.h"
#include "synthui_knob.h"
#include "synthui_step.h"

#define CONSOLE Serial1

/* --- audio graph ---------------------------------------------------------- */
AudioTransport      transport;
AudioStepSequencer  seq(transport);
AudioSynthAcidBass  acid;
AudioAnalyzeRMS     rms;
AudioOutputI2S      out;
AudioControlWM8962  wm;
AudioConnection     cRms(acid, 0, rms, 0);
AudioConnection     cL(acid, 0, out, 0);
AudioConnection     cR(acid, 0, out, 1);

/* --- the preset: a classic 16-step acid line (A minor-ish), documented so
 * the first frame and the audio windows are deterministic. note 0 = rest. */
struct PresetStep { uint8_t note; bool gate, accent, slide; };
static const PresetStep kPreset[16] = {
    { 33, true,  true,  false },   /* 0  A1 accent          */
    { 33, true,  false, false },   /* 1  A1                 */
    {  0, false, false, false },   /* 2  rest  <- the gate's edit target */
    { 45, true,  false, true  },   /* 3  A2 slide into 4    */
    { 36, true,  false, false },   /* 4  C2                 */
    {  0, false, false, false },   /* 5  rest               */
    { 33, true,  false, false },   /* 6  A1                 */
    { 40, true,  true,  false },   /* 7  E2 accent          */
    { 33, true,  false, false },   /* 8  A1                 */
    {  0, false, false, false },   /* 9  rest               */
    { 43, true,  false, true  },   /* 10 G2 slide into 11   */
    { 45, true,  false, false },   /* 11 A2                 */
    { 33, true,  true,  false },   /* 12 A1 accent          */
    { 31, true,  false, false },   /* 13 G1                 */
    {  0, false, false, false },   /* 14 rest               */
    { 33, true,  false, true  },   /* 15 A1 slide into 0    */
};

static void load_preset(void)
{
    for (int i = 0; i < 16; i++)
        seq.step(i, kPreset[i].note, kPreset[i].gate,
                 kPreset[i].accent, kPreset[i].slide);
}

/* --- default patch: the boot angles in §4 of the spec map to these -------- */
static void default_patch(void)
{
    acid.waveform(WAVEFORM_SAWTOOTH);
    acid.cutoff(800.0f);
    acid.resonance(0.55f);
    acid.envMod(0.6f);
    acid.decay(0.28f);
    acid.accent(0.7f);
    acid.distortion(0.15f);
    acid.subLevel(0.2f);
    acid.slideTime(0.06f);
    acid.level(0.5f);              /* fixed; no knob (spec §4) */
}

/* --- note-event pump: PIT context, immune to UI frame time ---------------- */
IntervalTimer pump;
static void pump_isr(void)
{
    int n = seq.eventCount();
    for (int i = 0; i < n; i++) {
        AcidSeqEvent ev = seq.eventAt(i);
        if (ev.type == SEQ_NOTE_ON) acid.noteOn(ev.note, ev.velocity, ev.slide);
        else                        acid.noteOff(ev.note);
    }
}

/* --- per-step RMS windows, referenced to the SEQUENCER's own position ----- */
static float    stepPeakRms[16];
static int      lastSeenStep = -1;
static uint32_t barsDone = 0;
static void audio_probe_poll(void)
{
    const int s = seq.currentStep();
    if (s < 0) return;
    if (rms.available()) {
        const float v = rms.read();
        if (v > stepPeakRms[s]) stepPeakRms[s] = v;
    }
    if (s != lastSeenStep) {
        if (s == 0 && lastSeenStep == 15) {
            barsDone++;
            CONSOLE.printf("ACIDBOX_BAR=%lu RMS=[", (unsigned long)barsDone);
            for (int i = 0; i < 16; i++)
                CONSOLE.printf("%s%.4f", i ? "," : "", stepPeakRms[i]);
            CONSOLE.println("]");
            memset(stepPeakRms, 0, sizeof(stepPeakRms));
        }
        lastSeenStep = s;
    }
}

static lv_obj_t *build_ui(void);   /* Task 5; stub until then */

void setup()
{
    CONSOLE.begin(115200);
    delay(200);
    CONSOLE.println("ACIDBOX_BEGIN");

    AudioMemory(24);
    const bool codec = wm.enable();
    wm.volume(0.6f);
    CONSOLE.println(codec ? "CODEC_OK" : "CODEC_FAIL");

    const bool panel = Display.begin();
    CONSOLE.println(panel ? "PANEL_OK" : "PANEL_FAIL");
    if (!panel) { CONSOLE.println("ACIDBOX_DONE"); return; }

    lvgl_rt1176_begin();
    lvgl_mipi_panel_create(Display);

    load_preset();
    default_patch();
    transport.tempo(128.0f);
    transport.loop(0.0f, 1.0f);
    transport.looping(true);
    pump.begin(pump_isr, 1000);    /* 1 kHz drain, spec §3.3 */

    lv_screen_load(build_ui());
    uint32_t t0 = millis();
    while (!lvgl_mipi_panel_frame_done() && (millis() - t0) < 5000)
        lvgl_rt1176_loop();
    lvgl_sum_reset();
    lvgl_sum_feed(Display.framebuffer(), PANEL_FB_BYTES);
    CONSOLE.printf("ACIDBOX_UI_SUM=0x%08lX\n", (unsigned long)lvgl_sum_value());
    CONSOLE.printf("PLAYING=%d\n", transport.playing() ? 1 : 0);
    CONSOLE.println("ACIDBOX_DONE");
}

void loop()
{
    lvgl_rt1176_loop();
    audio_probe_poll();
}
```

Stub for this task only (replaced wholesale in Task 5):

```cpp
static lv_obj_t *build_ui(void)
{
    lv_obj_t *scr = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(scr, lv_color_hex(0x101820), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, LV_PART_MAIN);
    return scr;
}
```

**Adaptation note:** `AcidSeqEvent`/`SEQ_NOTE_ON` names come from `seq_step.h` — use its exact spellings (`step_seq_test.cpp` lines 77–91 are the reference wiring). `IntervalTimer::begin(fn, usec)` takes MICROseconds in the Teensy convention — 1 kHz is `begin(pump_isr, 1000)`.

- [ ] **Step 2: Build**

```bash
cd examples/display/acid_box && \
  cmake -B build -DCMAKE_TOOLCHAIN_FILE=../../../toolchain/rt1170-evkb.toolchain.cmake && \
  cmake --build build -j 8
```
Expected: clean build.

- [ ] **Step 3: Smoke it in QEMU by hand (no gate yet)** — `../../../tools/rt1170-qemu.sh build/acid_box.elf` style run; expect `ACIDBOX_BEGIN`, `CODEC_OK` (or a QEMU-codec token consistent with what `acid_bass_test` sees — read its transcript for the expected value and match), `PANEL_OK`, a UI sum, `PLAYING=0`, `ACIDBOX_DONE`, and — because the transport is stopped — NO `ACIDBOX_BAR` lines. That silence-when-stopped is itself the boot-state contract.

- [ ] **Step 4: Commit**

```bash
git add examples/display/acid_box && \
git commit -m "acid_box: audio core -- graph, preset line, 1kHz note pump, per-step RMS probe

Boots stopped and silent by design (spec boot-state decision); ACIDBOX_BAR
RMS tables print only while playing. UI is a dark-ground stub until the
next commit."
```

### Task 5: evkb — `acid_box` UI + glue

**Files:**
- Modify: `examples/display/acid_box/acid_box.cpp` (replace the `build_ui` stub; add callbacks + poller)

- [ ] **Step 1: Replace the stub with layout A + glue.** Complete code:

```cpp
/* --- UI state ------------------------------------------------------------- */
static lv_obj_t *stepCell[16];
static lv_obj_t *playBtnLabel, *bpmLabel, *noteLabel, *accBtn, *sldBtn, *waveBtnLabel;
static lv_obj_t *pitchKnob;
static int selectedStep = 0;
static int shownCursor = -1;

/* knob->param maps (spec §4). Angle in [-140,140] -> t in [0,1]. */
static inline float knob01(lv_obj_t *k)
{ return (synthui_knob_get_angle(k) + 140.0f) / 280.0f; }
static inline float expmap(float t, float lo, float hi)
{ return lo * powf(hi / lo, t); }

/* pitch map: 25 semitones C1(24)..C3(48) across the sweep, one detent each.
 * angle -140 -> note 24; +140 -> note 48; detent_step = 280/24.
 * (Defined here, above their first use in select_step.) */
static inline uint8_t angleToNote(float deg)
{ return (uint8_t)(24 + (int)roundf((deg + 140.0f) / (280.0f / 24.0f))); }
static inline float noteToAngle(uint8_t note)
{ return -140.0f + (float)(note - 24) * (280.0f / 24.0f); }

/* one callback per sound knob, param named in user_data-free form */
static void cbCut(lv_event_t *e){ acid.cutoff  (expmap(knob01((lv_obj_t*)lv_event_get_target(e)), 20.0f, 12000.0f)); }
static void cbRes(lv_event_t *e){ acid.resonance(knob01((lv_obj_t*)lv_event_get_target(e))); }
static void cbEnv(lv_event_t *e){ acid.envMod  (knob01((lv_obj_t*)lv_event_get_target(e))); }
static void cbDec(lv_event_t *e){ acid.decay   (expmap(knob01((lv_obj_t*)lv_event_get_target(e)), 0.03f, 2.0f)); }
static void cbAcc(lv_event_t *e){ acid.accent  (knob01((lv_obj_t*)lv_event_get_target(e))); }
static void cbDst(lv_event_t *e){ acid.distortion(knob01((lv_obj_t*)lv_event_get_target(e))); }
static void cbSub(lv_event_t *e){ acid.subLevel(knob01((lv_obj_t*)lv_event_get_target(e))); }
static void cbSld(lv_event_t *e){ acid.slideTime(expmap(knob01((lv_obj_t*)lv_event_get_target(e)), 0.01f, 0.3f)); }

/* cutoff also prints, for the QEMU drag assertion */
static void cbCutTok(lv_event_t *e)
{ cbCut(e); CONSOLE.printf("CUTOFF=%.1f\n", expmap(knob01((lv_obj_t*)lv_event_get_target(e)), 20.0f, 12000.0f)); }

static const char *noteName(uint8_t n)
{
    static const char *N[12] = {"C","C#","D","D#","E","F","F#","G","G#","A","A#","B"};
    static char buf[8];
    snprintf(buf, sizeof buf, "%s%d", N[n % 12], (int)(n / 12) - 1);
    return buf;
}

/* write the selected step's full state back as ONE seq.step() call and
 * refresh its cell + the editor readouts. The single write is the atomic
 * transaction of spec §3.3. */
static void commit_selected(uint8_t note, bool gate, bool accent, bool slide)
{
    seq.step(selectedStep, note, gate, accent, slide);
    synthui_step_set(stepCell[selectedStep], gate, accent, slide);
    lv_label_set_text(noteLabel, gate ? noteName(note) : "--");
    lv_obj_set_style_bg_color(accBtn, accent ? lv_color_hex(0x5b62b8) : lv_color_hex(0x232b3a), LV_PART_MAIN);
    lv_obj_set_style_bg_color(sldBtn, slide  ? lv_color_hex(0x5b62b8) : lv_color_hex(0x232b3a), LV_PART_MAIN);
    CONSOLE.printf("STEP[%d]=note%u gate%d acc%d sld%d\n",
                   selectedStep, note, gate ? 1 : 0, accent ? 1 : 0, slide ? 1 : 0);
}

static void select_step(int i)
{
    synthui_step_set_selected(stepCell[selectedStep], false);
    selectedStep = i;
    synthui_step_set_selected(stepCell[i], true);
    AcidStep st = seq.step(i);
    synthui_knob_set_angle(pitchKnob, noteToAngle(st.note ? st.note : 33));
    lv_label_set_text(noteLabel, st.gate ? noteName(st.note) : "--");
    lv_obj_set_style_bg_color(accBtn, st.accent ? lv_color_hex(0x5b62b8) : lv_color_hex(0x232b3a), LV_PART_MAIN);
    lv_obj_set_style_bg_color(sldBtn, st.slide  ? lv_color_hex(0x5b62b8) : lv_color_hex(0x232b3a), LV_PART_MAIN);
}

static void cbStepTap(lv_event_t *e)
{
    lv_obj_t *cell = (lv_obj_t *)lv_event_get_target(e);
    int i = (int)(intptr_t)lv_event_get_user_data(e);
    select_step(i);
    AcidStep st = seq.step(i);
    bool gate = !st.gate;
    uint8_t note = st.note ? st.note : 33;   /* first gate-on of a rest: A1 */
    commit_selected(note, gate, st.accent, st.slide);
    (void)cell;
}
static void cbPitch(lv_event_t *e)
{
    uint8_t note = angleToNote(synthui_knob_get_angle((lv_obj_t *)lv_event_get_target(e)));
    AcidStep st = seq.step(selectedStep);
    commit_selected(note, st.gate, st.accent, st.slide);
}
static void cbAccBtn(lv_event_t *e)
{ (void)e; AcidStep st = seq.step(selectedStep); commit_selected(st.note, st.gate, !st.accent, st.slide); }
static void cbSldBtn(lv_event_t *e)
{ (void)e; AcidStep st = seq.step(selectedStep); commit_selected(st.note, st.gate, st.accent, !st.slide); }

static void cbPlay(lv_event_t *e)
{
    (void)e;
    if (transport.playing()) transport.pause(); else transport.play();
    CONSOLE.printf("PLAYING=%d\n", transport.playing() ? 1 : 0);
}
static void cbStop(lv_event_t *e)
{ (void)e; transport.stop(); CONSOLE.println("PLAYING=0"); }
static void cbTempoUp(lv_event_t *e)
{ (void)e; transport.tempo(transport.tempo() + 1.0f); }
static void cbTempoDn(lv_event_t *e)
{ (void)e; transport.tempo(transport.tempo() - 1.0f); }
static void cbWave(lv_event_t *e)
{
    (void)e;
    static bool square = false;
    square = !square;
    acid.waveform(square ? WAVEFORM_SQUARE : WAVEFORM_SAWTOOTH);
    lv_label_set_text(waveBtnLabel, square ? "SQR" : "SAW");
}

/* 33 ms poller: cursor ring, play label, bpm readout (spec §3.3) */
static void ui_poll(lv_timer_t *t)
{
    (void)t;
    const int s = transport.playing() ? seq.currentStep() : -1;
    if (s != shownCursor) {
        if (shownCursor >= 0) synthui_step_set_cursor(stepCell[shownCursor], false);
        if (s >= 0)           synthui_step_set_cursor(stepCell[s], true);
        shownCursor = s;
    }
    lv_label_set_text(playBtnLabel, transport.playing() ? LV_SYMBOL_PAUSE : LV_SYMBOL_PLAY);
    static char bpm[16];
    snprintf(bpm, sizeof bpm, "%.1f", transport.tempo());
    lv_label_set_text(bpmLabel, bpm);
}

static lv_obj_t *mkbtn(lv_obj_t *par, const char *txt, lv_event_cb_t cb,
                       lv_obj_t **labelOut)
{
    lv_obj_t *b = lv_button_create(par);
    lv_obj_set_style_bg_color(b, lv_color_hex(0x232b3a), LV_PART_MAIN);
    lv_obj_t *l = lv_label_create(b);
    lv_label_set_text(l, txt);
    lv_obj_center(l);
    lv_obj_add_event_cb(b, cb, LV_EVENT_CLICKED, NULL);
    if (labelOut) *labelOut = l;
    return b;
}
static lv_obj_t *mkknob(lv_obj_t *scr, int col, int row, const char *name,
                        float boot01, lv_event_cb_t cb)
{
    lv_obj_t *k = synthui_knob_create(scr);
    lv_obj_set_size(k, 150, 150);
    lv_obj_set_pos(k, 15 + col * 175, 90 + row * 185);
    synthui_knob_set_mode(k, SYNTHUI_KNOB_MODE_BOUNDED);
    synthui_knob_set_angle(k, boot01 * 280.0f - 140.0f);
    lv_obj_add_event_cb(k, cb, LV_EVENT_VALUE_CHANGED, NULL);
    lv_obj_t *l = lv_label_create(scr);
    lv_label_set_text(l, name);
    lv_obj_set_style_text_color(l, lv_color_hex(0x9aa0b8), LV_PART_MAIN);
    lv_obj_set_pos(l, 15 + col * 175 + 50, 90 + row * 185 + 152);
    return k;
}

static lv_obj_t *build_ui(void)
{
    lv_obj_t *scr = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(scr, lv_color_hex(0x101820), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, LV_PART_MAIN);

    /* transport bar */
    lv_obj_t *title = lv_label_create(scr);
    lv_label_set_text(title, "ACID BOX");
    lv_obj_set_style_text_color(title, lv_color_hex(0xFFFFFF), LV_PART_MAIN);
    lv_obj_set_pos(title, 15, 24);
    lv_obj_set_pos(mkbtn(scr, "-", cbTempoDn, NULL), 300, 16);
    bpmLabel = lv_label_create(scr);
    lv_obj_set_style_text_color(bpmLabel, lv_color_hex(0xFFFFFF), LV_PART_MAIN);
    lv_obj_set_pos(bpmLabel, 356, 24);
    lv_obj_set_pos(mkbtn(scr, "+", cbTempoUp, NULL), 420, 16);
    lv_obj_t *play = mkbtn(scr, LV_SYMBOL_PLAY, cbPlay, &playBtnLabel);
    lv_obj_set_pos(play, 540, 16);
    lv_obj_set_size(play, 70, 48);
    lv_obj_t *stop = mkbtn(scr, LV_SYMBOL_STOP, cbStop, NULL);
    lv_obj_set_pos(stop, 626, 16);
    lv_obj_set_size(stop, 70, 48);

    /* knobs, boot angles = default_patch() positions (spec §4 maps) */
    mkknob(scr, 0, 0, "CUTOFF",  logf(800.0f/20.0f)/logf(12000.0f/20.0f), cbCutTok);
    mkknob(scr, 1, 0, "RESO",    0.55f, cbRes);
    mkknob(scr, 2, 0, "ENV MOD", 0.60f, cbEnv);
    mkknob(scr, 3, 0, "DECAY",   logf(0.28f/0.03f)/logf(2.0f/0.03f), cbDec);
    mkknob(scr, 0, 1, "ACCENT",  0.70f, cbAcc);
    mkknob(scr, 1, 1, "DIST",    0.15f, cbDst);
    mkknob(scr, 2, 1, "SUB",     0.20f, cbSub);
    mkknob(scr, 3, 1, "SLIDE T", logf(0.06f/0.01f)/logf(0.3f/0.01f), cbSld);

    /* editor strip (y ~ 470): pitch detent knob + note + ACC/SLD + SAW/SQR */
    pitchKnob = synthui_knob_create(scr);
    lv_obj_set_size(pitchKnob, 120, 120);
    lv_obj_set_pos(pitchKnob, 15, 470);
    synthui_knob_set_mode(pitchKnob, SYNTHUI_KNOB_MODE_DETENTS);
    synthui_knob_set_detent_step(pitchKnob, 280.0f / 24.0f);
    lv_obj_add_event_cb(pitchKnob, cbPitch, LV_EVENT_VALUE_CHANGED, NULL);
    noteLabel = lv_label_create(scr);
    lv_obj_set_style_text_color(noteLabel, lv_color_hex(0xFFFFFF), LV_PART_MAIN);
    lv_obj_set_pos(noteLabel, 150, 515);
    accBtn = mkbtn(scr, "ACC", cbAccBtn, NULL);
    lv_obj_set_pos(accBtn, 230, 505);
    sldBtn = mkbtn(scr, "SLD", cbSldBtn, NULL);
    lv_obj_set_pos(sldBtn, 340, 505);
    lv_obj_t *wave = mkbtn(scr, "SAW", cbWave, &waveBtnLabel);
    lv_obj_set_pos(wave, 560, 505);

    /* step lane: 2x8 of 82px cells at 88 pitch, y = 640/736 */
    for (int i = 0; i < 16; i++) {
        lv_obj_t *c = synthui_step_create(scr);
        lv_obj_set_size(c, 82, 82);
        lv_obj_set_pos(c, 8 + (i % 8) * 88, 640 + (i / 8) * 96);
        synthui_step_set(c, kPreset[i].gate, kPreset[i].accent, kPreset[i].slide);
        lv_obj_add_event_cb(c, cbStepTap, LV_EVENT_CLICKED, (void *)(intptr_t)i);
        stepCell[i] = c;
    }
    select_step(0);

    lv_timer_create(ui_poll, 33, NULL);
    return scr;
}
```

Also in `setup()` (after `lvgl_mipi_panel_create`): create the touch indev exactly as `lvgl_rk055_touch_test` does (`gt911` begin + `lvgl_gt911_indev` create — copy its bring-up lines and its `I2C_OK` token).

- [ ] **Step 2: Build + QEMU smoke; record the UI sum**

```bash
cmake --build build -j 8 && ../../../tools/rt1170-qemu.sh build/acid_box.elf
```
Expected: full token banner, `I2C_OK`, a stable `ACIDBOX_UI_SUM`, `PLAYING=0`, no `ACIDBOX_BAR` lines. Run twice; the sum must repeat.

- [ ] **Step 3: Commit**

```bash
git add examples/display/acid_box && \
git commit -m "acid_box: layout-A UI + thin glue -- 8 knobs, editor strip, 2x8 lane, 33ms poller"
```

### Task 6: qemu2 — GT911 `touch-script` property (LOCAL-ONLY, GPL side)

**Files:**
- Modify: `~/Development/qemu2/hw/i2c/imxrt_gt911.c`

- [ ] **Step 1: Add the property.** Keep the compiled-in script as the default so `lvgl_rk055_touch_test` is untouched. Parser: one instant per line, 40 ms cadence, `#` comments; `P x% y%` = contact down/move, `R` = release, `.` = repeat previous instant (hold). Implementation sketch to adapt into the model's existing structures (the step struct and publish loop already exist — this only swaps the array):

```c
/* property: -global imxrt-gt911.touch-script=/path/file
 * Default (property unset) keeps the built-in gt911_script[] verbatim. */
static char *touch_script_path;   /* via DEFINE_PROP_STRING in the class */

static void imxrt_gt911_load_script(IMXRTGt911State *s)
{
    if (!s->touch_script_path) { s->script = gt911_script;
                                 s->script_len = GT911_SCRIPT_LEN; return; }
    FILE *f = fopen(s->touch_script_path, "r");
    if (!f) { error_report("gt911: cannot open %s", s->touch_script_path);
              exit(1); }
    g_autofree IMXRTGt911ScriptStep *v = g_new0(IMXRTGt911ScriptStep, 512);
    int n = 0, px = 0, py = 0;
    char line[128];
    while (fgets(line, sizeof line, f) && n < 512) {
        if (line[0] == '#' || line[0] == '\n') continue;
        IMXRTGt911ScriptStep st = {0};
        if (line[0] == 'P' && sscanf(line, "P %d %d", &px, &py) == 2) {
            st.contacts = 1; st.x[0] = px; st.y[0] = py;
        } else if (line[0] == 'R') {
            st.contacts = 0;
        } else if (line[0] == '.') {
            st.contacts = 1; st.x[0] = px; st.y[0] = py;
        } else continue;
        v[n++] = st;
    }
    fclose(f);
    s->script = g_steal_pointer(&v);
    s->script_len = n;
}
```

**Adaptation note:** the real member names of `IMXRTGt911ScriptStep` are in the file (the hardcoded array shows `{contacts, {ids}, {x%}, {y%}}` — mirror them exactly); the publish loop indexes `gt911_script[s->step]` today and switches to `s->script[s->step]`. Wire `imxrt_gt911_load_script` from realize, after properties resolve.

- [ ] **Step 2: Rebuild qemu2; prove the DEFAULT is unchanged**

```bash
cd ~/Development/qemu2/build && ninja qemu-system-arm 2>&1 | tail -1
cd /Users/nicholasnewdigate/Development/rt1170/evkb/examples/display/lvgl_rk055_touch_test && ./run_qemu.sh
```
Expected: touch test PASS, byte-identical assertions — the property unset means the compiled-in script, so this gate cannot have moved.

- [ ] **Step 3: Commit (qemu2 repo).** Message notes the firewall: evkb gates using `touch-script` are red on a fresh clone by design, like `sai1-rxinject`.

### Task 7: evkb — the acid_box QEMU gate (four assertions)

**Files:**
- Create: `examples/display/acid_box/touch_script.txt`
- Create: `examples/display/acid_box/run_qemu.sh`

- [ ] **Step 1: The touch script.** Percent coordinates target layout-A geometry (720×1280): ▶ at (575/720, 40/1280) ≈ (80%, 3%); step cell 2 (top row, third cell, center x = 8+2·88+41 = 225 → 31%, y = 681 → 53%... **compute from the Task 5 constants, then verify against a live frame dump in Step 4**); cutoff knob center (90/720, 165/1280) = (12%, 13%). Script:

The literal file is generated once, checked in, and never hand-counted —
create it with exactly this (run from the example directory):

```bash
{
  echo "# acid_box gate script. One instant per line, 40 ms cadence."
  echo "# Model stalls until GT911 I2C init, so padding starts at UI-ready."
  echo "# pad 1: 50 idle instants (~2 s) for boot + first frame"
  for i in $(seq 50); do echo R; done
  echo "# tap PLAY at (80%, 3%)"
  echo "P 80 3"; echo R
  echo "# pad 2: 94 instants (~3.75 s) = two bars at 128 BPM sixteenths"
  for i in $(seq 94); do echo R; done
  echo "# tap step cell 2 at (31%, 53%) -- toggles the preset rest ON"
  echo "P 31 53"; echo R
  echo "# pad 3: two more bars"
  for i in $(seq 94); do echo R; done
  echo "# vertical drag DOWN the cutoff knob: 8 samples, y 13% -> 28%"
  echo "P 12 13"; echo "P 12 15"; echo "P 12 17"; echo "P 12 19"
  echo "P 12 21"; echo "P 12 23"; echo "P 12 25"; echo "P 12 28"
  echo R
} > touch_script.txt
```

The percent targets derive from Task 5's layout constants: ▶ center
(575, 40)/(720, 1280) → (80%, 3%); step cell 2 center
(8 + 2·88 + 41, 640 + 41) = (225, 681) → (31%, 53%); cutoff knob center
(90, 165) → (12%, 13%). Re-derive these three if Task 5's geometry moves.

- [ ] **Step 2: The gate.** `run_qemu.sh` — copy the shape of `lvgl_rk055_touch_test/run_qemu.sh` (gate-lib prologue, `gate_capture_path`, qrun, `-global imxrt-gt911.touch-script=$DIR/touch_script.txt` added to the QEMU line) and assert, in order:

```bash
# Codec token: match what acid_bass_test's transcript_qemu.txt records for
# the QEMU WM8962 model (read it and pin the same string here).
grep -q "PANEL_OK" "$OUT" || { echo "FAIL: panel"; exit 1; }
grep -q "I2C_OK"   "$OUT" || { echo "FAIL: touch bring-up"; exit 1; }

GOLD="0x00000000"   # PROVISIONAL until Task 8 eyes-on-silicon (Task 3 rule)
grep -q "ACIDBOX_UI_SUM=$GOLD" "$OUT" || { echo "FAIL: UI golden"; exit 1; }
grep -q "ACIDBOX_UI_SUM=0x9BC99DC5" "$OUT" \
    && { echo "FAIL: all-zero framebuffer (the anti-golden, by name)"; exit 1; }

grep -q "PLAYING=1" "$OUT" || { echo "FAIL: PLAY tap never landed"; exit 1; }
grep -qE "STEP\[2\]=note33 gate1" "$OUT" \
    || { echo "FAIL: step-2 tap never wrote the pattern"; exit 1; }

# Per-step RMS tables. Margins: sounding > 0.02, rest < 0.005 -- stated here
# per the acid_bass_test precedent, with 4x separation between them.
bar1=$(grep -m1 'ACIDBOX_BAR=1 ' "$OUT" | sed 's/.*RMS=\[//; s/\]//')
last=$(grep 'ACIDBOX_BAR=' "$OUT" | tail -1 | sed 's/.*RMS=\[//; s/\]//')
[ -n "$bar1" ] && [ -n "$last" ] || { echo "FAIL: no RMS tables captured"; exit 1; }

# Bar 1 is the untouched preset: every gated step sounds, every rest is silent.
echo "$bar1" | awk -F, '{
  split("0 1 3 4 6 7 8 10 11 12 13 15", g, " ");
  split("2 5 9 14", r, " ");
  for (i in g) if ($(g[i]+1) + 0 <= 0.02)  { print "FAIL: preset step " g[i] " silent (rms " $(g[i]+1) ")"; exit 1 }
  for (i in r) if ($(r[i]+1) + 0 >= 0.005) { print "FAIL: preset rest " r[i] " sounding (rms " $(r[i]+1) ")"; exit 1 }
}' || exit 1

# ★ The integration assertion (spec 5.1.4): the touched step flips.
echo "$bar1" | awk -F, '$3 + 0 >= 0.005 { print "FAIL: bar-1 step 2 not silent"; exit 1 }' || exit 1
echo "$last" | awk -F, '$3 + 0 <= 0.02  { print "FAIL: post-edit step 2 not sounding"; exit 1 }' || exit 1

# The drag: at least 3 distinct CUTOFF= values, strictly decreasing.
grep 'CUTOFF=' "$OUT" | sed 's/CUTOFF=//' | awk '
  { v[n++] = $1 }
  END {
    if (n < 3) { print "FAIL: fewer than 3 CUTOFF samples from the drag"; exit 1 }
    for (i = 1; i < n; i++) if (v[i] + 0 >= v[i-1] + 0) { print "FAIL: cutoff not strictly decreasing"; exit 1 }
  }' || exit 1

echo "PASS: acid box -- boot golden, touch play/edit/drag, pattern audible before+after"
```

- [ ] **Step 3: Run the gate**

```bash
cd examples/display/acid_box && ./run_qemu.sh
```
Expected first run: FAIL on the provisional golden; captured UART shows the real sum and the full bar tables. Adopt (provisional marker), re-run: PASS.

- [ ] **Step 4: Verify the script's hit points against a real frame.** Dump the framebuffer OFFSET truth: run QEMU, capture, and cross-check the tap percentages against the Task 5 constants by arithmetic in the gate header comment. If a tap misses (no `PLAYING=1`), the geometry comment is where the fix goes — never widen an assertion to pass.

- [ ] **Step 5: Vacuity proofs.** (a) sentinel UI sum → FAIL; (b) one-nibble mutation → FAIL; (c) delete `touch_script.txt` → the run must FAIL NAMING the missing script (QEMU `error_report` + no `PLAYING=1`), not pass vacuously; (d) restore everything, PASS.

- [ ] **Step 6: transcript_qemu.txt + commit**

```bash
git add examples/display/acid_box && \
git commit -m "acid_box: QEMU gate -- boot golden, touch-injected play/edit/drag, per-step RMS before/after

The integration assertion: bar 1 has the preset's rest at step 2 silent;
after the injected tap, a later bar has it sounding. Uses qemu2's new
imxrt-gt911.touch-script property (LOCAL-ONLY; fresh clones see this gate
red -- the GPL firewall, as with sai1-rxinject; documented in the gate
header and KNOWN-BROKEN-GATES)."
```

### Task 8: Hardware verification

**★ Precondition (user's hands): replug the MCU-Link USB — the VCOM has been dead since 2026-08-17 and hardware tokens read over serial.**

- [ ] **Step 1: Flash** per the bench protocol (reset → `flash load` → `verify` → attach reader → reset; never hold the VCOM during load).
- [ ] **Step 2: By-ear ritual, recorded in `transcript_hw_evkb.txt`:** preset line audible in headphones after ▶; cutoff swept under a finger while playing (sweep audible); step 2 tapped in (extra note audible in the loop); ACC toggled on a sounding step (louder/brighter); SLD between two steps (glide, no retrigger); tempo +/- audible.
- [ ] **Step 3: UI frames:** `python3 tools/rt1170-screenshot.py` — dump the boot frame AND the synthui_step_test frame; LOOK at both; then remove the two `PROVISIONAL` markers (Task 3 Step 4, Task 7 Step 3) if — and only if — the frames are right. Attach both PNG checks to the transcripts.
- [ ] **Step 4: Serial tokens on silicon:** `PLAYING=1`, `STEP[...]`, `CUTOFF=` lines during the by-hand session pasted into the transcript.
- [ ] **Step 5: Commit transcripts.**

### Task 9: Bookkeeping + finish

- [ ] **Step 1:** Push SynthUI (`git push origin master`), bump its pin in `evkb.cmake`, spot-check `-DEVKB_FORCE_FETCH=ON` configure of `synthui_step_test`.
- [ ] **Step 2:** `tools/license-audit.sh` GATES entries for `synthui_step_test` and `acid_box`; mutation-test each (drop → drift check must name it; restore).
- [ ] **Step 3:** `docs/KNOWN-BROKEN-GATES.md`: `acid_box` entry — what the gate proves (software render, injected touch, windowed RMS) and its fresh-clone-red dependency on the qemu2 `touch-script` property (firewall class list gains one).
- [ ] **Step 4:** `examples/README.md` display-row entries for both examples.
- [ ] **Step 5:** Full sweep — expect **97 passed, 0 failed, 0 SKIP** (or 96/1 on the documented nondeterministic gate). Read gate NAMES. Update CLAUDE.md's baseline text and count history (95 → 97), and the spec's Status line to implemented-with-results.
- [ ] **Step 6:** Commit, then **superpowers:finishing-a-development-branch**.
