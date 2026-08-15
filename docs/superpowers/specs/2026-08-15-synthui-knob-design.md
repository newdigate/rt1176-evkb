# SynthUI Knob pilot — design

Date: 2026-08-15. Status: approved in session, pending implementation.
Parent spec: `2026-08-15-synthui-repo-design.md` (the SynthUI repo, created,
`c6dcfff`). Reference source: SynthUI `reference/dc/Knob.dc.html`.

## 1. What this builds

The first SynthUI widget: `synthui_knob`, an LVGL 9 custom widget in
`~/Development/SynthUI/src/`, ported from the DC Knob's `renderVals()` math —
plus its consuming example `examples/display/synthui_knob_test/` (rt1176-only)
with a QEMU gate on the RK055 panel and hardware verification on the bench
glass. Sweep baseline moves 91 → 92.

## 2. Scope decisions (user-approved)

- **Full prop surface, simple shading**: all 4 modes (endless / bounded /
  detents / arc), all 4 states (idle / active / focus / disabled), full
  geometry. Gradients are deliberately flattened for v1.
- **Angle-driven crescent luminance** (user's design): instead of the SVG's
  world-fixed gradient, the crescent is a solid color whose lightness follows
  the knob angle relative to a fixed top-left light — the fixed-light illusion
  without gradient machinery (§5.3).
- **RK055 panel** (720×1280 MIPI-DSI): the synth UI's actual glass, QEMU-gated
  today, on the bench, and its existing golden is glass-confirmed.
- **No touch interaction in v1** (§9).

## 3. Facts established during design (verified, not assumed)

- **`lv_draw_arc_dsc_t` has no gradient field** (`color, width, start_angle,
  end_angle, center, img_src, radius` — read from `src/draw/lv_draw_arc.h`),
  so the crescent cannot port its gradient directly; solid color it is.
  LVGL arcs ARE annulus sectors (radius = outer, width = outer − inner), which
  is exactly the SVG crescent's two-arcs-plus-two-lines path.
- **LVGL arc angles are 0° = 3 o'clock, clockwise.** The DC Knob's polar
  helper `P(r,θ)` is 0° = 12 o'clock, clockwise. Every arc draw therefore
  subtracts 90°: `lv_angle = dc_angle − 90`.
- **`lv_draw_fill_dsc_t` carries `lv_grad_dsc_t grad`** (`lv_draw_rect.h`), so
  the non-rotating face keeps a native vertical two-stop gradient for free.
- **`lv_color_mix(c1, c2, uint8_t mix)`** exists (`misc/lv_color_op.h`) — the
  crescent luminance lerp needs nothing custom.
- **Custom-widget pattern**: `lv_obj_class_t` extension (`core/lv_obj_class.h`)
  with constructor + event callback drawing in `LV_EVENT_DRAW_MAIN` — the
  standard LVGL 9 extended-widget mechanism.
- **The gate oracle** (`port/lvgl_rt1176.h`): `lvgl_sum_reset()` /
  `lvgl_sum_feed(ptr, bytes)` / `lvgl_sum_value()` / `lvgl_sum_bytes()`,
  FNV-1a-32. Contract in the header: reset IMMEDIATELY before the frame you
  checksum; a forgotten reset fails silently (different number, not an error).
- **RK055 panel API** (from `lvgl_rk055_panel_test.cpp`, which is the direct
  template): `lvgl_rt1176_begin()`, `lvgl_mipi_panel_create(Display)`, poll
  `lvgl_mipi_panel_frame_done()` under `lvgl_rt1176_loop()`, checksum
  `Display.framebuffer()` over `PANEL_FB_BYTES`, `LVGL_BYTES` from
  `lvgl_mipi_panel_flushed_px() * PANEL_BYTES_PER_PIXEL`. Its golden is
  **glass-confirmed** (header comment contrasts it with `lvgl_rpi_panel_test`,
  whose golden pins reproducibility only). Tokens print on `Serial1`.
- **Library resolution** (`teensy-cmake-macros` README): `teensy_declare_library`
  → local-first under `TEENSY_LIB_ROOT`, else CPM at the pinned ref. SynthUI
  has NO remote yet, so its declaration is LOCAL-ONLY: on this bench it
  resolves to `~/Development/SynthUI`; a fresh clone fails at fetch until
  SynthUI is pushed — the same documented class as the local-only qemu2 gate
  dependencies (CLAUDE.md already names that precedent).
- **`import_evkb_lvgl()` precedent** (`evkb.cmake:241`): LVGL is a plain CMake
  STATIC target (not `teensy_add_library`) with PUBLIC include dirs, PRIVATE
  `teensy_flags`, PUBLIC `m`; examples link `LVGL` directly on the `.elf`.
  SynthUI follows the same shape because `teensy_target_link_libraries`
  rewrites names to `<name>.o` and would not propagate LVGL's includes.
- **DC Knob surface** (from `Knob.dc.html`): props `size` (24–320 px, default
  120), `angle` (float deg), `mode` (endless default / bounded / detents /
  arc), `state` (idle / active / focus / disabled), `sweep` (60–330, default
  215, clamped 30–340), `ticks` (0–24, default 8), `min` (default −140), `max`
  (default 140), `detentStep` (default 35). Geometry in a 0–100 viewBox: face
  circle r=33 stroke 2.6; crescent annulus r=21..30 spanning `sweep` centred
  on `angle`; ticks r=37.5→45.5 (major →49, only the top marker in decorated
  modes); detents r=37.5→44; stops r=37→49; arc track/value r=38.5 width 3.2;
  pointer dot at r=25.5, radius 4; cap r=20 at 0.55 opacity. Palette hexes as
  written in the component (ink `#2b2e5c`, crescent `#8f96d4→#282b60`, etc.).

## 4. Widget API (`SynthUI/src/synthui_knob.h`)

```c
lv_obj_t *synthui_knob_create(lv_obj_t *parent);

typedef enum {
    SYNTHUI_KNOB_MODE_ENDLESS = 0,
    SYNTHUI_KNOB_MODE_BOUNDED,
    SYNTHUI_KNOB_MODE_DETENTS,
    SYNTHUI_KNOB_MODE_ARC,
} synthui_knob_mode_t;

void synthui_knob_set_angle(lv_obj_t *obj, float deg);       /* default 0   */
void synthui_knob_set_mode(lv_obj_t *obj, synthui_knob_mode_t m);
void synthui_knob_set_sweep(lv_obj_t *obj, float deg);       /* default 215, clamp 30..340 */
void synthui_knob_set_tick_count(lv_obj_t *obj, uint8_t n);  /* default 8, cap 24 */
void synthui_knob_set_range(lv_obj_t *obj, float min_deg, float max_deg); /* default -140..140 */
void synthui_knob_set_detent_step(lv_obj_t *obj, float deg); /* default 35  */
float synthui_knob_get_angle(const lv_obj_t *obj);
```

Size comes from the normal LVGL size (`lv_obj_set_size`); the widget scales
its 0–100 geometry to `min(w, h)`. DC `state` maps to LVGL native states —
idle = `LV_STATE_DEFAULT`, active = `LV_STATE_PRESSED`, focus =
`LV_STATE_FOCUSED`, disabled = `LV_STATE_DISABLED` — read back in the draw
handler to select the DC palette row; no style-system theming in v1. Setters
clamp exactly as `renderVals()` does, then `lv_obj_invalidate()`.

## 5. Rendering (`LV_EVENT_DRAW_MAIN`)

**5.1 Scaling.** `S = min(width, height) / 100.0f`; the polar helper is the
`P(r,θ)` port: `x = cx + r·S·sin(θ·D)`, `y = cy − r·S·cos(θ·D)`.

**5.2 Draw order** (back to front, exactly `renderVals()`'s element order):
ticks (`lv_draw_line`, major w=3.4·S / minor 2.4·S; endless shows `ticks`,
decorated modes show only the top marker) → detents (mode=detents,
`lv_draw_line` w=2·S) → stops (bounded|detents, w=3.4·S) → arc track+value
(mode=arc, `lv_draw_arc` r=38.5·S w=3.2·S; value arc spans min→clamped angle)
→ face (`lv_draw_rect`, `LV_RADIUS_CIRCLE`, vertical `LV_GRAD_DIR_VER`
faceFrom→faceTo, border ringColor w=2.6·S) → crescent (`lv_draw_arc`,
r=30·S, width=9·S, angles `[angle−sweep/2−90, angle+sweep/2−90]`, solid color
§5.3) → pointer dot (`lv_draw_rect` radius-circle at P(25.5, angle), r=4·S)
→ cap (`lv_draw_rect` radius-circle r=20·S, `LV_OPA_55`-ish: opa 140).
Disabled state renders the whole widget at opa ≈ 166 (0.65) like the SVG's
group alpha.

**5.3 Crescent luminance (the user's simplification).** Light direction is
top-left = −45° in the DC convention (matches the SVG gradient axis
(18,14)→(84,88)):

```c
/* t: 0 at angle=-45 (lightest), 1 at angle=+135 (darkest) */
float t = (1.0f - cosf((angle + 45.0f) * (float)M_PI / 180.0f)) * 0.5f;
lv_color_t cres = lv_color_mix(cresTo, cresFrom, (uint8_t)(255.0f * (1.0f - t)));
```

`cresFrom`/`cresTo` come from the state's palette row (active brightens,
disabled desaturates, exactly the DC table). Turning the knob visibly
brightens/darkens the sweep.

## 6. Example + gate (`examples/display/synthui_knob_test/`)

**Scene**: deterministic 4×4 grid on the RK055 — rows = modes (endless,
bounded, detents, arc), columns = states (idle, active, focus, disabled) —
each knob 150 px at a fixed, distinct angle per cell (angles chosen in the
plan; fixed forever once goldens are recorded). Template:
`lvgl_rk055_panel_test.cpp`.

**Gate phases** (all tokens before any animation):
1. Render the full grid; poll `frame_done`; `lvgl_sum_reset()`; feed the whole
   framebuffer → `DISPLAY_OK`, `LVGL_FLUSHED=PASS`,
   `LVGL_BYTES=<full-frame bytes>`, `KNOB_SUM_ALL=0x…`.
2. Per-mode phases — the acid-bass lesson (a single aggregate can silently
   stop testing half the feature): for each mode, re-style one full-width
   strip showing only that mode's row, force a full refresh, reset, feed →
   `KNOB_SUM_ENDLESS=0x…`, `KNOB_SUM_BOUNDED=0x…`, `KNOB_SUM_DETENTS=0x…`,
   `KNOB_SUM_ARC=0x…`. A frozen or never-registered mode changes its own
   token and no other.
3. `KNOB_TEST_DONE` — then, and only then, a continuous angle animation on
   one hero knob (eyes-on-glass payoff; never checksummed).

**Gate assertions** (`run_qemu.sh`, via `gate-lib.sh` exactly like the RK055
gate): `gate_require_capture`, then `DISPLAY_OK`, `LVGL_FLUSHED=PASS`, the
full-frame `LVGL_BYTES` value, all five `KNOB_SUM_*` goldens, `KNOB_TEST_DONE`.
Goldens are RECORDED, not derived — stable across two consecutive QEMU runs
before recording, glass-confirmed in the same commit, with the RK055/RPi
gates' re-record comment discipline reproduced.

## 7. Build integration

- `evkb.cmake`: `teensy_declare_library(SynthUI SynthUI
  https://github.com/newdigate/SynthUI <SynthUI-HEAD-sha> .)` with a
  LOCAL-ONLY comment (no remote yet; fresh clones fail here until first push —
  qemu2-precedent class), plus `import_evkb_synthui()` in the
  `import_evkb_lvgl()` shape: plain STATIC target from `SynthUI/src/*.cpp`
  (glob, one level), PUBLIC include `SynthUI/src`, PUBLIC link `LVGL` (pulls
  LVGL headers + `m`), PRIVATE `teensy_flags`. Example links
  `synthui_knob_test.elf ← SynthUI LVGL stdc++`.
- No `boards` sidecar (rt1176-only — the RK055 is a MIPI-DSI panel; the
  RT1062 has no MIPI-DSI host).
- `tools/license-audit.sh`: GATES entry for `examples/display/synthui_knob_test`.
  SynthUI is MIT throughout (its LICENSE covers `src/`); nothing from
  `reference/` is compiled — the widget is a clean-room port of the component's
  *math*, which the SynthUI README's provenance rules permit explicitly.
- CLAUDE.md sweep baseline: 91 → 92, re-measured by running the sweep, not by
  counting files (house rule).

## 8. Verification

1. QEMU gate green; goldens stable across two runs before recording.
2. Hardware: flash via LinkServer (VCOM-free, reader attached only after),
   eyes on the RK055 glass confirm the 4×4 grid and the animation; goldens
   glass-confirmed in the same commit that records them;
   `transcript_qemu.txt` + `transcript_hw_evkb.txt` committed as evidence.
3. Full sweep: expect **92 passed, 0 failed, 0 SKIP** (or 91/1/0 on the
   documented nondeterministic dual-core gate), gates built first, read gate
   NAMES on any failure.
4. `license-audit.sh` PASS with the new GATES entry walked.

## 9. Non-goals (v1)

- Touch interaction (GT911 indev wiring — v2; the port pieces exist).
- Gradient-faithful crescent, focus-glow effects beyond ring color.
- The other eight components; any style-system/theming surface.
- Pushing SynthUI; RT1062 support for this example.
- PXP/VGLite acceleration (sw render is the measured-correct choice at these
  sizes; revisit trigger stays "image-heavy scenes", per the v9 census).
