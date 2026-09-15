# SynthUI LedButton — tear-free 30 fps widget — design

Date: 2026-09-15
Status: approved (brainstorm 2026-09-15)
Tracking: Linear **NEW-25** ("SynthUI LedButton: tear-free 30 fps widget").
Related: NEW-24 (Lamp), NEW-27 (PanelButton), NEW-29 (SevenSegment) — the
sw-delta widget pattern, db pipeline and gate guards this design inherits;
NEW-20 (RotaryKnob) for the delta-equality discipline. Consumer:
`2026-09-15-acid-box-synthui-editor-design.md` (the acid_box step lane and
ACC/SLD keys), which is why the API below has a `pressed` LATCH.

Mockup of the first consumer: https://claude.ai/artifact/JjHojwmcoZ7U62ekzVkLin

## 1. Goal & Scope

Implement the **LedButton** primitive from the DC reference set
(`SynthUI/reference/dc/LedButton.dc.html`, the "909-led-button" step key of
`Step Buttons Sheet.dc.html`) as a SynthUI LVGL 9 custom widget
(`src/synthui_led_button`), verified by host tests in `SynthUI/tests/` and a
new double-buffered evkb display example
`examples/display/synthui_led_button_test` gated under QEMU (qemu2,
`~/Development/qemu2`) on the `mimxrt1170-evk` machine.

Success criteria (from the Linear issue), with how each is met:

| Criterion | Met by |
|---|---|
| Sustained ≥30 fps while animating | SW-delta rendering: seven flat rects and two dots per key; a state change invalidates one box. Phase B on silicon animates all 16 keys and measures the frame interval (§8). |
| Double-buffered | `lvgl_mipi_panel_create_db()`; flips on the VSYNC ISR; goldens read the PRESENTED buffer (`flip_sync()` + `scanned_fb()`). |
| No visible tearing or glitching | Back-buffer render + vsync flip by construction; per-boot delta-equality guard (delta sequence CRC == fresh full render). |
| Delta rendering | Each setter invalidates only the box its state paints (§7); no-op sets return without invalidating. |

The widget is the DC reference and nothing more: one LED, four boolean
states, four LED colours. Everything the acid_box lane needs beyond that
(accent, slide, step numbers) lives OUTSIDE the widget by design — lamps and
labels owned by the application — so the primitive stays reusable.

## 2. Non-Goals

- No GPU/VGLite compositor TU. A step key does not animate continuously
  under a drag the way the fader does; the silicon fps checkpoint (§8)
  decides whether a GPU path is ever needed, and if so it is a separate
  issue, not a scope creep here.
- No toggle logic. The widget emits stock `LV_EVENT_CLICKED`; the
  application sets `lit`. Same authority model as `synthui_step` and
  `synthui_panel_button`.
- No label or step number. The sheet says "numbering belongs to the row";
  labels are external `lv_label`s.
- No row widget. A 16-key row composes from this primitive; the reverse
  does not decompose (brainstorm approach C, declined).
- No custom colour pairs beyond the four DC enums (YAGNI; a pair setter is
  a one-line addition later if a consumer needs it).

## 3. Widget Files & Public API (SynthUI)

New files, clean-room vector rebuild from the written description in §4
(never from `reference/` art), per the repository's provenance rules:

- `src/synthui_led_button_types.h` — colour enum and on/off table, LVGL-free
- `src/synthui_led_button_math.h` — header-only pure geometry, LVGL-free
- `src/synthui_led_button.h` — public LVGL 9 widget API
- `src/synthui_led_button.cpp` — widget implementation (`synthui_led_button_class`)
- `tests/led_button_test.c` — host unit tests, wired into `tests/run.sh`

### Types (`synthui_led_button_types.h`)

```c
typedef enum {
    SYNTHUI_LED_BUTTON_RED = 0,   /* DC default */
    SYNTHUI_LED_BUTTON_AMBER,
    SYNTHUI_LED_BUTTON_GREEN,
    SYNTHUI_LED_BUTTON_BLUE,
} synthui_led_button_color_t;

/* DC LED table, 0xRRGGBB: {on, off} per colour. */
#define SYNTHUI_LED_BUTTON_RED_ON     0xFF3B30u
#define SYNTHUI_LED_BUTTON_RED_OFF    0x5E2B28u
#define SYNTHUI_LED_BUTTON_AMBER_ON   0xFFA41Fu
#define SYNTHUI_LED_BUTTON_AMBER_OFF  0x5C3D18u
#define SYNTHUI_LED_BUTTON_GREEN_ON   0x4BE060u
#define SYNTHUI_LED_BUTTON_GREEN_OFF  0x254A2Bu
#define SYNTHUI_LED_BUTTON_BLUE_ON    0x5AA8FFu
#define SYNTHUI_LED_BUTTON_BLUE_OFF   0x22384Fu
```

### Widget API (`synthui_led_button.h`)

```c
extern const lv_obj_class_t synthui_led_button_class;

lv_obj_t *synthui_led_button_create(lv_obj_t *parent);

void synthui_led_button_set_lit(lv_obj_t *obj, bool lit);
bool synthui_led_button_get_lit(const lv_obj_t *obj);

/* A LATCH: the key stays sunk until cleared.  The DRAWN pressed state is
 * (latch || LV_STATE_PRESSED), so a finger on any key sinks it while held,
 * and the latch keeps the selected key sunk afterwards. */
void synthui_led_button_set_pressed(lv_obj_t *obj, bool pressed);
bool synthui_led_button_get_pressed(const lv_obj_t *obj);

void synthui_led_button_set_cue(lv_obj_t *obj, bool cue);
bool synthui_led_button_get_cue(const lv_obj_t *obj);

/* Greys the cap and LED, suppresses the halo, sets LV_STATE_DISABLED
 * (clearing any press) and clears LV_OBJ_FLAG_CLICKABLE; restored on
 * re-enable.  A key disabled by lv_obj_add_state(LV_STATE_DISABLED) also
 * draws grey, like the sibling widgets. */
void synthui_led_button_set_disabled(lv_obj_t *obj, bool disabled);
bool synthui_led_button_get_disabled(const lv_obj_t *obj);

void synthui_led_button_set_color(lv_obj_t *obj, synthui_led_button_color_t color);
synthui_led_button_color_t synthui_led_button_get_color(const lv_obj_t *obj);
```

Class defaults: `width_def = height_def = 96` (the DC default size), the
red colour, all four booleans false. The constructor removes
`LV_OBJ_FLAG_SCROLLABLE` (taps are the whole input story; a scrollable key
swallows taps as drags — `synthui_step`'s reasoning verbatim).

Input: the class event handler reacts to `LV_EVENT_PRESSED`, `RELEASED`,
`PRESS_LOST` and `INDEV_RESET` only to invalidate the press box (when the
latch is not holding the key down); it adds no gesture logic. The drawn
pressed and disabled states are read from the object's LVGL state at draw
time, so `lv_obj_add_state` draws correctly; a programmatic state change after
the first render is the caller's to invalidate, because LVGL does not repaint
a style-less widget on a state change. `LV_EVENT_CLICKED` reaches the
application's handler from the base class as it does today for `synthui_step`.

## 4. Geometry — Written Description of DC Reference

All coordinates are in the reference's 100-unit box. The widget scales by
`s = min(w, h) / 100` and centres the 100×100 box in its area, so a
non-square widget draws a square key with equal side margins. The press
offset is `dy_px = lroundf(2.5 · s)` whole pixels when the drawn pressed state
is on, else `0`, added to each layer AFTER that layer is rounded to pixels.
Every layer except the bezel and the well moves by `dy_px`, so a press is a
pure pixel translation and never resizes a layer. (Adding 2.5 units before
rounding was the first design; review measured the LED slipping a pixel
against the cap and shrinking 15 → 14 px at the 96 px default.) Below, `+dy`
in a coordinate names the layers that move.

Draw order (back to front):

1. **Bezel** — rounded rect (1, 1, 98×98), radius 16, fill `#1C1C1E`,
   stroke `#3A3A3D` width 2. Cue: stroke `#FF3B30` width 3.5. (The stroke is
   drawn as an LVGL border on the same rect; SVG centres a stroke on the edge
   and LVGL draws a border inside it, so the rect is grown by half the stroke
   width to keep the outer extent equal.)
2. **Well** — rounded rect (7, 6, 86×88), radius 13, fill `#101012` at 90 %.
3. **Cap** — rounded rect (10, 9+dy, 80×80), radius 11, vertical gradient
   top → mid (at 62 %) → low. The vendored `lv_conf.h` sets
   `LV_GRADIENT_MAX_STOPS 2`, so the cap is drawn as TWO stacked two-stop
   rects meeting at 62 % of the height: rows 0..49.6 top→mid, rows 49.6..80
   mid→low, over a solid mid fill. LVGL rounds all four corners of a rect,
   so each half's inner corners show the mid fill beneath (within a few
   levels of the gradient there) and the cap's outer antialiased edge is
   composited slightly heavier: deterministic, visually negligible, pinned by
   the golden. The math header exposes the split so the host test pins it.
4. **Highlight** — rounded rect (14, 12+dy, 72×11), radius 5.5, white at
   55 % (28 % when pressed).
5. **Halo** (lit and not disabled only) — the LED rect grown by 5 units on
   every side, no fill, border width 10 in the LED's ON colour at 38 %. The
   LED fill (next) covers the inner half of the border, which is what the
   SVG stroke-over-fill produces.
6. **LED** — rounded rect (26, 19+dy, 48×15), radius 3.5, fill ON or OFF
   colour from the table; `#4A4A4C` when disabled.
7. **Moulding dots** — two circles r 1.7 at (38, 26.5+dy) and (62, 26.5+dy),
   black at 22 %. **Dropped when the key is smaller than 34 px** (the sheet:
   "below 34px the LED dots drop out; everything else holds").
8. **Base** — rounded rect (10, 82+dy, 80×7), radius 3.5, fill `#8E8B84` at
   85 % (50 % when pressed).

### Palette

| Role | Normal | Pressed | Disabled |
|---|---|---|---|
| cap top | `#F7F5F1` | `#DEDCD7` | `#E2E1DE` |
| cap mid | `#E8E6E1` | `#CBC9C3` | `#D2D1CE` |
| cap low | `#C9C7C1` | `#B4B2AD` | `#BCBBB8` |
| highlight α | 0.55 | 0.28 | 0.55 |
| base α | 0.85 | 0.50 | 0.85 |
| LED | on/off table | on/off table | `#4A4A4C`, no halo |
| bezel stroke | `#3A3A3D` w2 | same | same |
| bezel stroke, cue | `#FF3B30` w3.5 | same | same |

Disabled combines with pressed (the cap takes the disabled set; `dy` still
applies) and with cue (the bezel still turns red). The reference's rule holds
throughout: **state change is colour and one 2.5-unit offset; nothing
resizes.**

## 5. Rendering (`DRAW_MAIN`)

`led_draw()` computes the layout once per draw from the math header
(`synthui_led_button_layout(w, h, pressed, &L)` fills scaled `lv_area_t`
boxes for every layer) and issues the eight draws above with
`lv_draw_rect` (radius, bg, bg_grad, border) and `lv_draw_arc`-free circles
(dots are `lv_draw_rect` with `LV_RADIUS_CIRCLE`). No allocation, no
per-draw state.

## 6. State Management & Delta Damage Model

The instance holds `lit, pressed(latch), cue, disabled, color` and a cached
`drawn_pressed` (latch || `LV_STATE_PRESSED`). Every setter early-returns
when the value is unchanged, then invalidates ONLY the box its state paints,
in pixels, computed by the math header through the same rounding the draw uses:

| Change | Invalidated box (100-unit coords, before scaling) |
|---|---|
| `lit`, `color` | the halo's pixel area at the current `dy_px` (at 100 px: x 21..78, y 14..38 unpressed, 17..41 pressed) |
| drawn pressed (latch or LV state) | union of the moving layers' pixel areas at `dy_px` 0 and pressed (at 100 px: x 10..89, y 9..91) — cap, highlight, halo, LED, dots and base all move |
| `cue` | the bezel ring: the whole widget (the ring is the outer 3.5 units on four sides; four strips would save little and complicate the guard) |
| `disabled` | the whole widget |

A `pressed` latch change while `LV_STATE_PRESSED` is on (or vice versa) does
not change `drawn_pressed` and invalidates nothing.

## 7. Host Unit Tests (`SynthUI/tests/led_button_test.c`)

Pure-C tests over `synthui_led_button_math.h` and the types header, run by
`tests/run.sh` alongside the other suites:

- every layout box scales linearly with `s` and is centred for a non-square
  widget;
- `dy_px` is 0 unpressed and `lroundf(2.5·s)` pressed, and the layout's
  rects are identical in both states (the offset is applied after rounding,
  to exactly the layers listed in §4);
- a pixel-space containment sweep over squares 8..400 and four non-square
  sizes, both press states, through the same `rect_px`/`circle_px` conversion
  the widget uses;
- damage boxes: the lit box contains the LED and the halo at both offsets;
  the press box contains every moving layer at BOTH offsets; both lie inside
  the widget;
- the dot dropout threshold is exactly 34 px (33 → no dots, 34 → dots);
- the cap split sits at 62 % and the two halves tile the cap exactly (no gap,
  no overlap);
- the colour table returns the DC pairs and disabled maps to `#4A4A4C`;
- palette selection: pressed vs disabled vs normal cap sets.

Each guard is demonstrated RED against a mutant before it is trusted (a
press box that ignores `dy`; a lit box that omits the halo; a split at 50 %).

## 8. Consumer Example & Gate (`rt1176-evkb`)

`examples/display/synthui_led_button_test` on the `synthui_panel_button_test`
pattern (`create_db`, XRGB8888, `import_evkb_synthui()`, no VGLITE flavour):

**Scene**: a 4×4 bank of 16 keys, 100 px, on the 720×1280 portrait panel,
covering: off; lit in red, amber, green, blue; pressed (latched); lit +
pressed; cue; cue + lit; disabled; disabled + lit (must show no halo); a
32 px key (no dots); a 34 px key (dots); a 150 px key; a non-square 120×80
key (centred square); and one key under LVGL's real `LV_STATE_PRESSED`
(set via `lv_obj_add_state`) to pin that the LV state draws identically to
the latch.

**Gate `run_qemu.sh` asserts, in order**: `PANEL_OK`;
`led_button_scene=16 grid=4x4`; `LVGL_FLUSHED=PASS`; `LVGL_BYTES=3686400`
(anchored); the render golden `led_button_crc=0x........` (FNV-1a over the
presented buffer, recorded from consecutive QEMU runs, altered-CRC
demonstrated RED); the **delta-equality guard** — a 64-step LCG sequence
toggling `lit`/`pressed`/`cue`/`color` across the bank rendered through the
widget's delta damage must equal a fresh full render of the final state
(`led_button_delta_crc` == `led_button_fresh_crc`, `led_button_delta_eq=PASS`);
the **engagement bound** — `led_button_damage max=N` with `N <= 10000` px
(a latch change on a 100 px key is 80×83 = 6,640 px; a `cue` change is the whole
key, 10,000 — the bound is therefore exactly the widget's largest legitimate
box, and a reversion to full-screen invalidation fails here and nowhere
else); the vsync witness `led_button_vsync flips=N isrs=N timeouts=0`;
`crc_done`; `PASS: SynthUI led_button render verified`.

**Phase B** (after `crc_done`, NOT gated): a loop animating all 16 keys
(playhead cue sweeping, LEDs toggling) prints `led_button_fps`. QEMU timing
is meaningless; silicon is where the ≥30 fps criterion is answered. Below
30 fps on the bench, a GPU compositor is filed as a separate issue.

**Close-out**: `GATES` entry in `tools/license-audit.sh`, README rows
(root and `examples/README.md`), `transcript_qemu.txt` fixture, a
`gate-vacuity.test.sh` case (green fixture replays; corrupted golden fails
by name; a missing damage counter fails by name), SynthUI pushed and the
`evkb.cmake` pin bumped, fresh-user `-DEVKB_FORCE_FETCH=ON` verified by
RUNNING the gate on the fetched ELF, sweep 140 → **141**, `LICENSE-AUDIT:
PASS` after (never during) the sweep, `transcript_hw_evkb.txt` with the
silicon golden and the Phase B fps.

## 9. Risks

- **The cap split is visible if the two halves' radii are wrong.** The math
  header owns the split; the host test tiles the halves; the golden pins the
  pixels.
- **A halo drawn as an outer border is 5 units wider than the LED**, so a
  lit box that forgets it leaves stale halo pixels when the LED goes off —
  exactly what the delta-equality guard exists to catch.
- **`LV_STATE_PRESSED` and the latch draw the same thing**, so a bug that
  drops the latch on release would be invisible in a scene with no touch. The
  bank includes a latched key with no LV state and the acid_box consumer
  exercises the release path on silicon.
