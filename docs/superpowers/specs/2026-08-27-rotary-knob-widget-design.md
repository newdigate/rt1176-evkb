# synthui_rotary_knob — NEW-20 Phase 2 design

Date: 2026-08-27
Status: approved (scope fixed by the NEW-20 Phase-2 scope decision, recorded on
the Linear issue 2026-08-27 17:01)
Tracking: Linear **NEW-20** Phase 2. Phase 1 (the bench) is merged (adf5858);
its spec is `2026-08-27-rotary-knob-bench-design.md` and its §13 numbers are
what this design implements.

## 1. Goal & scope

A production widget, **`synthui_rotary_knob`**, in the SynthUI sibling repo,
implementing the **RotaryKnob** design (`RotaryKnob.dc.html`, design project
`79ec272e-93e2-41e7-a4cc-566b130c67f5`) on the bench's measured winner:
**vector/gpu** — three `vg_lite` paths built once, a rotation matrix per frame
— with the vector/sw LVGL path as the always-present fallback (QEMU, and any
board without the GC355).

Scope per the issue's Phase-2 decision:

- **Notch variant only.** Split, baton, crescent and facet are deferred; no
  variant prop is exposed (adding one later is additive).
- The rest of the DC props surface **is** implemented: theme (light/dark),
  state (idle/active/focus/disabled via LVGL states), mode (endless/bounded),
  accent, min/max, angle, size.
- The old `synthui_knob` is **replaced** in its consumers and **deleted from
  SynthUI**. `synthui_knob_math.h` (the input arithmetic) and its host test
  stay — the new widget reuses them verbatim.

Consumers replaced and re-goldened: `display/synthui_knob_test` (becomes the
new widget's test, both engines), `display/acid_box`, `display/vglite_lvgl_test`.
★ The issue also lists `synthui_step_test`, but that example contains **no
knobs** (checked, not assumed — its one "knob" hit is prose). It is left
untouched, and its unchanged golden becomes the **control** that proves the
SynthUI library change disturbed no other widget.

Out of scope, deliberately: the two ≥30 fps levers the bench named (removing
the LVGL sw well/ground floor; overlapping CPU and GPU instead of
finish-per-refresh). The compositor here reproduces the bench's
finish-per-refresh ordering, whose correctness is proven; pipelining is a
follow-up with its own measurement.

## 2. The DC contract (read from RotaryKnob.dc.html, not remembered)

Fetched 2026-08-27 from the design project. `renderVals()` semantics:

- **Well** (never rotates): mode ≠ bounded → circle r39, fill `well`, stroke
  `wellStroke` w1.6 (state=focus: stroke = **base.index** — the theme's index
  color, NOT the accent — w3). mode = bounded → circle r39 fill only (no
  stroke) plus an **arc track at r43**, min→max, stroke `wellStroke`, w3,
  round linecap.
- **Rotor** (rigid, `rotate(angle 50 50)`), notch: body disc r36, inner disc
  r27, index ring(16, 36, −8°, +8°). Identical to the bench's `rk_geometry`
  notch — the geometry the goldens and the silicon numbers were measured on.
- **Palette** (THEME.light / THEME.dark from the DC file, hex-exact):

  | token | light | dark |
  |---|---|---|
  | well / wellStroke | #dcdce6 / #b6b8cc | #14141c / #34344a |
  | body / inner / index | #282b60 / #333871 / #fcfbf6 | #3c4176 / #4a5090 / #ffd24a |
  | bodyActive / innerActive | #31356f / #3d4283 | #464c88 / #565da4 |
  | bodyOff / innerOff / indexOff / wellOff | #9a9cae / #a6a8b8 / #dcdce6 / #e4e4ea | #2a2a36 / #32323f / #55555f / #101016 |

- **State resolution**: disabled → Off colors for body/inner/index/well
  (accent ignored; wellStroke stays the base stroke). active → Active
  body/inner. accent (when set and not disabled) replaces `index`.
- **Defaults**: angle 0, size 120, mode endless, theme light, state idle,
  min −150, max +150, accent unset (theme index). Accent's DC options:
  #fcfbf6, #ffd24a, #5be0a0, #ff6a52.

Known accepted divergence, inherited from the bench: SVG strokes are centred
on the radius, LVGL borders sit inside it — a half-stroke-width difference the
goldens absorb (the bench's §6 note). Same for the r43 track: LVGL arc radius
names the outer edge, so the track is drawn with outer radius 44.5 (= 43 +
3/2), width 3.

## 3. Widget API (SynthUI `src/synthui_rotary_knob.{h,cpp}`)

```c
typedef enum { SYNTHUI_ROTARY_MODE_ENDLESS = 0, SYNTHUI_ROTARY_MODE_BOUNDED } synthui_rotary_mode_t;
typedef enum { SYNTHUI_ROTARY_THEME_LIGHT = 0, SYNTHUI_ROTARY_THEME_DARK } synthui_rotary_theme_t;
#define SYNTHUI_ROTARY_ACCENT_DEFAULT 0xFFFFFFFFu   /* revert to theme index */

extern const lv_obj_class_t synthui_rotary_knob_class;
lv_obj_t *synthui_rotary_knob_create(lv_obj_t *parent);
void  synthui_rotary_knob_set_angle(lv_obj_t *obj, float deg);      /* default 0 */
void  synthui_rotary_knob_set_mode(lv_obj_t *obj, synthui_rotary_mode_t m);
void  synthui_rotary_knob_set_theme(lv_obj_t *obj, synthui_rotary_theme_t t);
void  synthui_rotary_knob_set_accent(lv_obj_t *obj, uint32_t rgb_hex);
void  synthui_rotary_knob_set_range(lv_obj_t *obj, float min_deg, float max_deg); /* default -150..150 */
void  synthui_rotary_knob_set_detent_step(lv_obj_t *obj, float deg); /* 0 = continuous (default) */
float synthui_rotary_knob_get_angle(const lv_obj_t *obj);
```

- **State** maps from LVGL exactly as the old knob mapped it: PRESSED→active,
  FOCUSED→focus, DISABLED→disabled (disabled wins). Programmatic state
  changes need `lv_obj_invalidate()` — the widget defines no local styles, so
  LVGL's style-diff repaint never fires (the old knob's documented lesson; the
  input layer's state callback handles the touch case).
- **detent_step is not a DC prop** — it is input behavior carried over from
  the old knob because `acid_box`'s pitch knob depends on it. It quantises
  only what is displayed; the drag accumulator stays unsnapped
  (`synthui_knob_math.h`, reused verbatim, including the min/max clamp in
  every mode — endless mode's clamp is the old knob's shipped behavior, kept
  for parity rather than re-litigated here).
- **Input layer**: verbatim port of the old knob's — PRESSED seeds the
  accumulator from the current angle, PRESSING integrates `vect.y`
  (200 px = full sweep), snap-for-display, `LV_EVENT_VALUE_CHANGED` on
  change, invalidate on PRESSED/RELEASED/PRESS_LOST, SCROLLABLE and
  SCROLL_CHAIN_VER cleared in the constructor.
- **Palette** lives in a pure, LVGL-free header
  `src/synthui_rotary_palette.h` (mirrors `synthui_knob_math.h`): inputs
  (theme, disabled, active, focus, accent hex or sentinel), outputs the five
  hexes {well, wellStroke, body, inner, index}. Single source of truth for
  the sw draw, the GPU compositor, and a host unit test.
- A **destructor_cb** unlinks the instance from the compositor's registry
  (§4); the constructor links it. The registry is maintained unconditionally
  — two pointers per instance, no GPU dependency.

## 4. GPU compositor (SynthUI `src/vglite/synthui_rotary_knob_gpu.{h,cpp}`)

The bench's post-render pass, productised. Opt-in per application; never
compiled unless asked for (§5).

```c
bool     synthui_rotary_gpu_begin(void *framebuffer, int32_t w, int32_t h,
                                  int32_t stride_bytes);
uint32_t synthui_rotary_gpu_errors(void);   /* cumulative failed vg_lite_* calls */
```

- The app owns GPU bring-up exactly as the bench did: `vg_lite_init_mem` →
  chip-ID probe → `vg_lite_init`. Only then `synthui_rotary_gpu_begin()`,
  which wraps + maps the framebuffer as the vg_lite target (`vg_lite_map` —
  unmapped draws "succeed" and change nothing, the vglite_probe lesson),
  builds the notch path set once (3 paths, S32 coords in centred viewBox
  units ×16, ~110 arena words ≈ 352 B), registers an
  `LV_EVENT_RENDER_READY` callback on the default display, and installs a
  hook pointer into the widget core. Any failure → returns false, installs
  nothing, and the widget stays fully software — the honest negative.
- **Draw split** (the bench's proven ordering): with the hook installed, the
  widget's DRAW_MAIN paints the **well only** (sw — it is static and focus
  logic stays in one place) and sets the instance's `gpu_pending` flag. At
  RENDER_READY — sent from `refr_invalid_areas`, structurally never on an
  empty refresh, after every sw area has been rendered and (synchronously)
  flushed — the compositor walks the registry, draws each pending instance's
  three cached paths with matrix translate(cx,cy)·rotate(angle)·scale(S/16)
  where S = min(w,h)/100, fill NON_ZERO, blend SRC_OVER, colors from the
  shared palette (converted to ABGR — vg_lite_color_t is ABGR, red in the low
  byte), clears the flag, and issues **one `vg_lite_finish()`** iff anything
  was drawn.
- `gpu_pending` is set only when DRAW_MAIN actually ran, so hidden objects,
  other screens, and deleted objects (unlinked by the destructor) can never
  be composited. A partial-overlap redraw (another object's damage clipping
  through a knob) recomposites the full rotor over its own previous AA edge
  pixels — a second SRC_OVER of the same premultiplied edge darkens it
  fractionally. Accepted and documented: no consumer in this tree damages a
  knob partially (angle changes invalidate the whole object), and the fix
  (vg_lite scissor per damage area) is deferred until something needs it.
- **No blits anywhere** — cached paths only — so the GC355's 64-byte
  source-stride rule does not apply and consumers need no
  `LV_DRAW_BUF_STRIDE_ALIGN` change (that was a bitmap/strip-cell
  requirement; existing goldens elsewhere stay untouched by construction).
- Every vg_lite call goes through the bench's GPU_TRY error counter, read
  back by `synthui_rotary_gpu_errors()`; examples print it so a rejected
  draw can never pass silently ("drew nothing, timed beautifully" is the
  bench's named worst outcome — same rule for correctness runs).
- No D-cache maintenance, same invariant as the bench and
  `lvgl_mipi_panel.cpp`: the imxrt1176 core never enables the D-cache. The
  forward hazard note travels with the code (an rt1062 port makes this site
  and the panel flush one change, not two).

## 5. Build plumbing (evkb.cmake)

`import_evkb_synthui()` grows an optional flavor: `import_evkb_synthui(VGLITE)`
additionally calls `import_evkb_vglite()`, compiles `src/vglite/*.cpp` into
the SynthUI target, adds `src/vglite` to its PUBLIC include dirs, and links
VGLite PUBLIC. The plain call is byte-for-byte today's behavior — the core
glob is non-recursive, so the GPU sources are invisible to it, and the widget
core references the GPU code only through the hook pointer the GPU side
installs (no undefined symbols in either direction). LVGL itself stays the
**software** renderer in every consumer (`LV_USE_DRAW_VG_LITE=0`) — the
one-ELF-both-engines safety argument is Phase 1's, unchanged.

## 6. Consumers

**`display/synthui_knob_test`** — rewritten as the new widget's test; keeps
its directory name and single gate id (`rt1176:display/synthui_knob_test`).
Links VGLite alongside software LVGL (the bench's CMake pattern) and does the
probe → init → `synthui_rotary_gpu_begin()` dance; prints
`rk_engine=gpu` or `rk_engine=sw` (plus `rk_gpu_err=<n>` after the sums when
gpu). Scenes, all checksummed via the existing `sum_screen()` machinery:

1. 4×4 grid: rows = {endless/light, bounded/light, endless/dark,
   bounded/dark}, cols = states {idle, active, focus, disabled} at angles
   {−105, −35, 35, 105} — `KNOB_SUM_ALL` (plus the LVGL_BYTES whole-screen
   guard, unchanged).
2. One screen per row config — `KNOB_SUM_ENDLESS_LIGHT`,
   `KNOB_SUM_BOUNDED_LIGHT`, `KNOB_SUM_ENDLESS_DARK`, `KNOB_SUM_BOUNDED_DARK`
   (per-feature goldens; the acid-bass lesson).
3. Accent screen: the DC's four accent options, endless/light/idle —
   `KNOB_SUM_ACCENT`. Covers the accent-overrides-index path and (by column
   4 being disabled elsewhere) leaves accent-ignored-when-disabled to the
   grid.
4. `SYNTHUI_KNOB_DONE`, then the hero spin (eyes-on only, signed angles).

QEMU gate: six pinned goldens, `rk_engine=sw` asserted, and a tripwire — no
`rk_engine=gpu` and no `rk_gpu_err=` may appear in a QEMU capture.
Demonstrated RED per the standing rule: once against a wrong golden, once
against a faked gpu line. On silicon the same ELF renders every scene through
the GC355; the six GPU sums, `rk_gpu_err=0`, and the glass check land in
`transcript_hw_evkb.txt` (two-golden-sets discipline — sw and gpu sums are
never reconciled).

**`display/acid_box`** — mechanical swap: `mkknob()` → rotary knob with
`SYNTHUI_ROTARY_MODE_BOUNDED` + **explicit `set_range(−140, 140)`** (the DC
default is ±150; acid_box's angle↔param maps hardcode ±140, so the range is
now stated at the call site instead of inherited); pitch knob → bounded +
`set_detent_step(280/24)` + the same explicit range. No GPU attach — the
capstone stays sw this phase. Input math is unchanged, so the gate's scripted
CUTOFF drag produces the same VALUE_CHANGED stream; only `ACIDBOX_UI_SUM`
re-goldens (grep the gate for other framebuffer sums before assuming: today
there is exactly one).

**`display/vglite_lvgl_test`** — swap the 16-knob grid to the rotary widget;
rows become {endless/light, bounded/light, endless/dark, bounded/dark} (the
old file's four modes no longer exist). Everything else — the two-build
split, FPSBENCH, GRADPROBE, swd_log — stays. No compositor attach: this
example's purpose is LVGL's **own** VG_LITE draw unit, and mixing a second,
direct vg_lite client into LVGL's command stream is exactly the kind of
uncontrolled interaction the bench avoided. Software golden re-pinned in the
gate; GPU-build golden and fresh FPSBENCH numbers re-recorded on silicon
(the recorded 2.83/2.45 fps are the old knob's — the file's header note and
CLAUDE.md's line about the fps criterion get updated to say which knob the
numbers describe).

## 7. Verification

- **SynthUI host tests**: `knob_math_test` unchanged (still passes —
  proves the input layer's contract survived the move);
  new `rotary_palette_test` pinning the §2 table — every
  (theme × {idle,active,focus,disabled} × accent/none) → five hexes,
  DC-exact.
- **QEMU gates**: three re-goldened gates green; each demonstrated RED
  against a corrupted golden first (and knob_test's gpu tripwire fired by
  name). `synthui_step_test` green **unchanged** — the cross-widget control.
- **Vacuity suite**: re-capture `transcript_qemu.txt` for the three changed
  examples after their gates pass (the 2026-08-25 stale-fixture lesson);
  suite re-run green.
- **Sweep**: still 122 gates (no gate added or removed); re-measured by
  running it, with the standing KNOWN-BROKEN exceptions. License audit run
  (GATES entries unchanged — same directories — but the audit walks the new
  SynthUI/VGLite dep edges).
- **Silicon** (bench order: flash load → verify → attach reader → reset):
  `synthui_knob_test` — six GPU sums recorded, `rk_gpu_err=0`, glass check
  including the hero spin; `vglite_lvgl_test` build-vglite — GPU golden +
  FPSBENCH re-recorded; `acid_box` — glass + touch spot-check. Transcripts
  committed.
- **Pin flow**: SynthUI committed + pushed, `evkb.cmake` pin bumped, then a
  `-DEVKB_FORCE_FETCH=ON` scratch build of `synthui_knob_test` **and its
  gate run against the fetched-source ELF** (a configure proves resolution;
  only a gate run proves behavior — the 2026-08-24 lesson).

## 8. Risks

- **LVGL sw arc clamp** (negative starts render truncated wedges): the fold
  discipline is copied from the old knob's `draw_arc_seg`, which the bounded
  track (−140 start) exercises on every draw.
- **LV_USE_FLOAT=1** in the vglite_lvgl_test GPU build: all coordinate casts
  go through the old knob's lroundf-into-`lv_value_precise_t` pattern, which
  compiles and renders under both settings (proven by the old knob's two
  goldens there).
- **Widget deleted mid-frame / wrong-screen composite**: excluded
  structurally (`gpu_pending` set only by DRAW_MAIN; destructor unlinks).
- **GPU error silence**: every call counted; examples print the counter;
  knob_test's hardware transcript must show `rk_gpu_err=0` or its GPU sums
  are void (the bench's rule, restated for correctness instead of timing).
- **Old-knob deletion breaking an unknown consumer**: only this tree consumes
  SynthUI (checked by grep across examples; SynthUI went public 2026-08-17
  with a rewritten history). The removal commit is separate, so a revert is
  one cherry-pick.
