# SynthUI Fader — tear-free 30 fps widget — design

Date: 2026-08-29
Status: approved (brainstorm 2026-08-29)
Tracking: Linear **NEW-23** ("SynthUI Fader: tear-free 30 fps widget").
Related: NEW-20 (RotaryKnob) — the architecture, delta discipline, db
pipeline, and gate guards this design inherits.

## 1. Goal & scope

Implement the **Fader** primitive from the DC reference set
(`SynthUI/reference/dc/Fader.dc.html`) as a SynthUI LVGL 9 widget
(`src/synthui_fader`), verified by a new evkb example
`examples/display/synthui_fader_test` and its QEMU gate (sweep 122 → 123).

Success criteria (from the issue), with how each is met:

| Criterion | Met by |
|---|---|
| Sustained ≥30 fps while animating | Phase B on silicon: 16-fader bank, per-fader phase offsets, ≥60 s vsync-locked (§10) |
| Double-buffered | `lvgl_mipi_panel_create_db()` pipeline; goldens read the **presented** buffer (`flip_sync()` + `scanned_fb()`) |
| No visible tearing or glitching | Complete-frame flips by construction (sw renders into the back buffer before flip) + one camera/eye pass on silicon — checksums can never see scanout artifacts — + the per-boot delta-equality guard for render correctness |

Decisions fixed in brainstorming (2026-08-29):

- **SW-delta first, measure.** LVGL software rendering with delta damage
  only; **no GPU/VGLite TU in this issue**. The knob needed the GC355
  because 16 complex notch discs were expensive to redraw and rotation made
  damage wedge-shaped. The fader is the knob's opposite: its cap is a pure
  vertical translation (damage = one axis-aligned rect) and its shapes —
  rects, lines, one gradient — are exactly what the sw renderer is fastest
  at. An early silicon measurement (§10) decides whether a GPU TU is ever
  needed; missing 30 fps re-opens that as its own brainstorm.
- **Worst case = 16-fader bank**: 2×8 grid at the DC default 78×210 px on
  the 720×1280 panel, all animating with per-fader phase offsets —
  methodological parity with the knob's all-16 case, so fps numbers are
  comparable across widgets.
- **Input = 1:1 relative drag** (§8). The cap never jumps to the finger.
- **Verification = new example + gate**, not an extension of
  `synthui_knob_test` (a regression in either widget must red its own gate).

## 2. Non-goals

- No GPU compositor TU (the checkpoint in §10 may spawn one as separate work).
- No detent/snap logic — the center line is visual only (a pan-fader center
  detent is a plausible follow-up, deliberately excluded).
- No labels or scale text (the DC reference has none).
- No horizontal orientation.
- No change to `synthui_rotary_knob` or any existing golden.

## 3. Widget files & API (SynthUI)

New files, MIT headers, rotary naming conventions:

- `src/synthui_fader.h` — public API
- `src/synthui_fader.cpp` — the widget (single TU; no private header and no
  instance registry until a second engine exists — YAGNI)
- `src/synthui_fader_math.h` — the drag mapping as pure functions
  (host-tested in `tests/`)

```c
lv_obj_t *synthui_fader_create(lv_obj_t *parent);
void  synthui_fader_set_value(lv_obj_t *obj, float v01);    /* clamped 0..1; delta-invalidates */
float synthui_fader_get_value(const lv_obj_t *obj);
void  synthui_fader_set_ticks(lv_obj_t *obj, uint8_t n);    /* clamp 2..33, default 13 */
void  synthui_fader_set_center(lv_obj_t *obj, bool on);     /* default false */
void  synthui_fader_set_panel(lv_obj_t *obj, uint32_t rgb); /* default SYNTHUI_FADER_PANEL_DEFAULT */
```

The four DC panel greys ship as named constants
(`SYNTHUI_FADER_PANEL_DEFAULT` = 0x6D7A85, plus 0x5B6570, 0x7D8994,
0x4A535C); `set_panel` takes any rgb so the constants are a convenience, not
an enum.

States follow the rotary contract: **no local styles**, so programmatic
`lv_obj_add_state`/`remove_state` need a manual invalidate.
`LV_STATE_PRESSED` selects the *active* palette, `LV_STATE_DISABLED` the
*disabled* palette. The input layer sets/clears PRESSED itself and
invalidates only the cap extent box (press changes cap colors only);
DISABLED and all config setters invalidate the whole widget. Drag emits
`LV_EVENT_VALUE_CHANGED`.

Palette resolution lives in one pure function inside the widget TU
(`state → colors`), host-testable and extractable — the cheap way to keep
the two-engines-share-one-palette door open without building it now.

## 4. Geometry — written description of the DC reference

All geometry is defined in **viewBox-unit space**: width 100 units, height
`vh = round(100·h/w)` units, scale `u = w/100` px per unit. Formulas are
evaluated in unit space and rounded to px only at draw time, identically in
full and delta paths (same draw code — §6).

- `value ∈ [0,1]`; value 1 puts the cap at the top.
- `capH = max(14, 0.11·vh)`
- `pad = 0.06·vh`, `top = pad`, `bottom = vh − pad`
- `travel = bottom − top − capH`
- `capY = top + (1 − value)·travel`
- **Ticks** (N = ticks, default 13): horizontal lines x 8..92 at
  `y_i = top + capH/2 + i·travel/(N−1)`, i = 0..N−1; line width
  `max(1.4, 0.012·vh)`; opacity 0.62 when `i % 4 == 0`, else 0.34.
- **Rod** (the slot): x 46.5, width 7, corner radius 1.5,
  y `top + capH/2 − 2`, height `travel + 4`, fill #14181B.
- **Center line** (option): x 4..96 at `y = top + capH/2 + travel/2`,
  width 2.4.
- **Cap**, a group translated to (0, capY), drawn in this order:
  - shadow: rect x 6, y 2.5, w 88, h capH, r 2, #1B1F22 at 45 % opacity
  - body: rect x 4, y 0, w 88, h capH, r 2, vertical 3-stop gradient
    (0 → capTop, 0.46 → capMid, 1 → capLow), stroke #20262A width 1.6
  - groove: rect x 4, y `capH/2 − 0.07·capH`, w 88, h `0.14·capH`, #20262A
  - gloss ×2: rects x 9, w 78, h `max(1.5, 0.12·capH)`, at y `0.16·capH`
    and `0.68·capH`, white at 75 % opacity (30 % disabled)

At the default 78×210: vh = 269, capH ≈ 29.6 u, u = 0.78 px/u — cap ≈ 69×23
px, travel ≈ 207 u ≈ 162 px.

## 5. Palette

| | capTop | capMid | capLow | ticks | gloss opa | center |
|---|---|---|---|---|---|---|
| idle | #F4F5F4 | #DCDEDD | #B6BABA | #E8EEF0 | 0.75 | #20262A |
| active (PRESSED) | #FFFFFF | #DCDEDD | #C8CBCA | #E8EEF0 | 0.75 | #20262A |
| disabled | #D2D5D4 | #B4B8B8 | #9AA0A1 | #C8CDD0 | 0.30 | #8F9598 |

## 6. Rendering

One pass in `DRAW_MAIN`, clipped by LVGL to the damage: panel fill → ticks →
rod → center line → cap (shadow, body, groove, gloss). Draw order guarantees
the cap paints over ticks/rod inside any damaged slice.

The DC cap's 3-stop gradient renders as **two stacked 2-stop gradient
rects** (top→mid over [0, 0.46·capH], mid→low over the rest), so nothing
depends on `LV_GRADIENT_MAX_STOPS`. Corner treatment: the stroked rounded
body rect is drawn first filled with capMid, then the two gradient rects
inset by the stroke width with square corners — the four corner pixels keep
capMid, sub-pixel at r ≈ 1.6 px.

## 7. Delta damage model

Define the **cap extent box** for a given capY, in unit space:
x from 3.2 (body x 4 minus half-stroke 0.8) to 94 (shadow right edge),
y from `capY − 0.8` to `capY + capH + 2.5` — the union of the stroked body
and the offset shadow. Rounded outward to px and inflated by 2 px.

- `set_value` invalidates **one rect**: `extent(oldY) ∪ extent(newY)` —
  a single rect because the motion is purely vertical — via one
  `lv_obj_invalidate_area`.
- Press/release invalidates the current cap extent box.
- Everything else (ticks, center, panel, DISABLED, resize) invalidates the
  whole widget.

Correctness of partial repaints is **proved per boot, not assumed**, by the
gate's delta-equality guard (§9): the same one-pass draw code runs clipped,
so a bug here is a checksum inequality, by name.

## 8. Input layer

`synthui_fader_math.h`, pure and host-tested:

```c
/* dy_up_px = press_y − current_y (up positive). travel_px ≤ 0 → anchor. */
float synthui_fader_drag(float anchor, float dy_up_px, float travel_px);
```

returns `clamp01(anchor + dy_up_px / travel_px)`. On `LV_EVENT_PRESSED` the
widget anchors the current value and the press y; each `PRESSING` event maps
the displacement through `synthui_fader_drag` (1:1 — dragging the full
travel length sweeps the full range) and emits `LV_EVENT_VALUE_CHANGED` on
change; `RELEASED`/`PRESS_LOST` clears PRESSED. Host tests
(`tests/fader_math_test.c`): sign (up increases), scaling (travel_px of drag
= full range), clamping at both ends, zero/negative travel, NaN
displacement → anchor.

## 9. Consumer example & gate (evkb)

`examples/display/synthui_fader_test`, `import_evkb_synthui()` (no VGLITE —
sw only), on the db pipeline (`lvgl_mipi_panel_create_db()`); all checksums
read the presented buffer (`flip_sync()` + `scanned_fb()`), never
`Display.framebuffer()`.

**Scene** (deterministic): 16 faders, 2×8 grid, each 78×210, exact layout
fixed in the plan. Config axes exercised in the golden scene: values `i/15`;
center=on for i ∈ {4..7}; i = 12 DISABLED; panel grey `i % 4` through the
four constants; ticks 33 for i = 8, 5 for i = 15, default 13 elsewhere.

**Phase A (gated in QEMU):**

1. Full render → `fd_crc=0x…` — the committed golden.
2. Scripted delta sequence: 64 steps, per-step per-fader value deltas from a
   fixed-seed integer LCG, |Δvalue| ≤ 0.06 → `fd_delta_crc=0x…`; then a
   forced fresh full render at the same final values → `fd_fresh_crc=0x…`.
   The gate asserts the two are **equal to each other** (gate-compared,
   never re-goldened).
3. Engagement check: per-step rendered-area px measured by the same
   mechanism as `synthui_knob_test` → `fd_damage max=… total=… steps=64`;
   the gate asserts `max ≤` a constant fixed in the plan. Analytic
   single-frame estimate ≈ 48 k px/step (16 × ~75×40 px extents at
   ΔcapY ≤ ~10 px); LVGL direct-mode two-buffer sync roughly doubles what
   the display renders, so the constant is set from a **measured green run
   plus headroom**, then demonstrated RED by reverting delta to full
   invalidation (full scene ≈ 262 k px/step single-frame — ≥4×
   discrimination by construction).
4. `fd_vsync frames=… timeouts=0` — gated, as in the knob test.
5. `crc_done`.

**Phase B (after `crc_done`, deliberately NOT gated — QEMU timing is
meaningless):** ≥60 s animation, `value_i(t) = 0.5 + 0.5·sin(2π·0.5·t +
i·2π/16)` (max per-frame Δvalue ≈ 0.052 at 30 fps, matching the engagement
envelope), printing `fd_fps=…`. Silicon is where this number answers the
issue.

**Bookkeeping:** gate id `rt1176:display/synthui_fader_test` (single script,
no variant suffix); sweep target 122 → 123; vacuity cases added to
`gate-vacuity.test.sh` (green fixture replays green; corrupted golden fails
by name; a missing `fd_damage` token must fail, absent counter ≠ pass);
fixture `transcript_qemu.txt` captured **after** the gate runs (2026-08-25
staleness lesson); `license-audit.sh` `GATES` entry added; CLAUDE.md gate
arithmetic updated when the sweep is measured.

## 10. Measurement checkpoint & escalation

The first silicon run happens **as soon as Phase A passes locally, before
polish**. Pass = sustained ≥30 fps vsync-locked over ≥60 s on the 16-bank
(`fd_fps`), goldens bit-identical to QEMU, repeated-boot stable (≥3 boots —
NEW-20's one-defect-hid-behind-one-boot lesson), plus one camera/eye pass
during animation. Also record `fd_fps` with delta disabled (full invalidate)
once, as the honest baseline showing what delta buys.

If the measurement misses 30 fps: **stop and re-brainstorm** a GPU TU as its
own scoped work (the rotary compositor pattern — registry, `gpu_pending`,
deferred pre-flip compose — transplants; §3's pure palette function is the
prepared seam). Numbers land in NEW-23 and the example's
`transcript_hw_evkb.txt` either way.

## 11. Error handling

- `set_value(NaN)` is ignored (value unchanged); ±inf clamps to 0/1.
- `set_ticks` clamps to 2..33.
- Degenerate sizes: formulas stay valid (capH floor 14 u); drag guards
  `travel_px ≤ 0` by returning the anchor.
- All setters on a wrong-class object hit the LVGL class assert, as with the
  rotary.

## 12. Provenance

The DC set is SynthUI's **own MIT content**, and the README names deriving
from it as the sanctioned route — the firewall exists for the removed
ReBirth material, none of which is consulted. §4 is the written description
this widget is built from; nothing under `reference/` is compiled or
converted into arrays. While touching the README, its stale status text
(which still names the deleted `src/synthui_knob` as the only widget) is
refreshed to name the current widgets.

## 13. Sequencing

1. **SynthUI**: `synthui_fader_math.h` + host tests (TDD) → widget rendering
   → input layer → README touch. evkb builds against the local checkout
   (local-first resolution).
2. **evkb**: example scene + Phase A + gate + vacuity + audit entry, green
   in QEMU.
3. **Silicon checkpoint** (§10) — early, before polish.
4. **Close-out**: push SynthUI, bump the `evkb.cmake` pin, verify the
   fresh-user path with `-DEVKB_FORCE_FETCH=ON` **by running the gate
   against the fetched-source ELF** (a configure only proves the subdir
   resolves), full sweep + license audit + vacuity suite, transcripts
   committed, CLAUDE.md and NEW-23 updated.

## 14. Risks

- **30 fps miss on silicon** — bounded by the early checkpoint and the
  prepared escalation path (§10).
- **Direct-mode two-buffer sync** doubles rendered px per step — included in
  the engagement constant's measured basis; still far under the full-render
  floor.
- **Vsync lock caps at the panel refresh** — the knob's locked pipeline
  measured 32.1 fps, so ≥30 is attainable but not generous; Phase B reports
  the locked number because that is what the eye sees.
- **A committed fixture goes stale silently** — mitigated by capturing the
  fixture after the gate runs and by the vacuity suite (2026-08-25 lesson).
