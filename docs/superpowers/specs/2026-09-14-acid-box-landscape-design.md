# acid_box in landscape: rotate the picture 90°, not the widgets

Design, brainstormed and approved 2026-09-14.  `display/acid_box` becomes a landscape instrument: the
board is turned 90° counter-clockwise on the bench and the UI is drawn upright on the RK055's portrait
glass.  The layout is rewritten for 1280×720; every existing widget is kept; nothing in the audio or
Bluetooth glue changes.  The rotation lives in exactly two seams -- the present and the touch mapping -- and
is invisible to LVGL, SynthUI, the GC355 knob compositor and the application.

## 1. Goal and non-goals

**Goal.**  Replace acid_box's portrait layout with a landscape one (one layout, one gate, one golden set per
engine -- the portrait layout is retired, not kept behind an option), using the same controls it has today:
the transport bar, eight sound knobs, the pitch knob with note / ACC / SLD / SAW, and the 2×8 step lane.

**Non-goals.**  No new widgets; the free band the layout leaves is reserved for a later spec (§6).  No
change to MipiDisplay -- `Display.begin()` still brings the panel up in its native 720×1280 mode.  No change
to SynthUI.  No second layout, no build option, no sibling example.

## 2. Orientation, pinned before anything else

"90° counter-clockwise" was ambiguous and was settled with a drawing.  **The board is turned CCW and the
picture stays upright**, so the top of the UI lands on the panel's physical RIGHT edge (the edge that ends
up on top after the turn).  In pixels that is a 90° CLOCKWISE rotation of the picture on the glass:

    logical (x, y) on the 1280×720 frame  ->  physical (719 - y, x) on the 720×1280 panel
    physical (px, py)                     ->  logical (py, 719 - px)            (the inverse, for touch)

That is exactly `PXP_ROT_90` -- the rotation `display/pxp_blit_test` has already matched on silicon
(`PXP_ROT90_SUM=0x54F838ED`, identical in QEMU), and the QEMU model's `case 1` truth table (`sx = oy;
sy = src_h-1 - ox`) reproduces it.  Checked, not assumed: logical (0,0) maps to physical (719,0), the
top-right corner of the portrait panel, which after a CCW turn is the top-LEFT of what the user sees.

## 3. Approach, and the two alternatives it was chosen over

**Chosen: a landscape canvas, PXP rotates at present.**

    LVGL 1280×720, rotation 0, DIRECT, ONE buffer  ->  canvas C (SDRAM, 3.6 MB, never scanned out)
        ->  pre-present hook: GC355 knob compositor draws into C (unchanged API)
        ->  PXP ROT90, C -> the off-screen portrait scanout buffer (full frame, or damage rects -- §4 decides)
        ->  LCDIFv2 flip at vsync (the existing fence, unchanged)
    touch: GT911 raw -> physical 720×1280 -> the inverse map above -> logical

Why: everything that renders keeps believing the panel is 1280×720 with no rotation, so LVGL's software
renderer, SynthUI, the compositor's `begin_deferred(w, h, stride)` contract and the layout code are all
untouched.  The rotation is gateable in QEMU because the PXP model does it and its sums match silicon.  The
pipeline stays tear-free BY CONSTRUCTION: the canvas always holds a complete frame, so every buffer ever
presented is a complete frame.

**Rejected: LVGL's own rotation (`lv_display_set_rotation`, CPU rotate in flush).**  It requires PARTIAL
render mode (the port is built on DIRECT double-buffering, and `lv_display_set_matrix_rotation` refuses
anything else), the CPU rotate runs over uncached SDRAM at the ~20 MB/s the v6 copy bench measured (a full
refresh ≈ 180 ms), and the GC355 compositor would need a logical→physical transform -- GPU code no gate in
this tree can see, in a library with other consumers.  `lvgl_gt911_indev.h` records the same conclusion.

**Held in reserve: the GC355 rotates at present (`vg_lite_blit` with a 90° matrix).**  Probably the fastest
present on silicon, but QEMU has no GC355, so PXP would still have to exist as the fallback: two present
paths and one more sw/gpu golden divergence.  It becomes the plan only if the probe below fails (§4).

## 4. Phase 0: the PXP rotation probe, before any layout work

Two facts about the code shape the probe.  (a) The QEMU model always writes a ROTATED op from the output
buffer's origin and ignores `outputAt()` -- its own comment says "the firmware issues no letterboxed rotated
op".  (b) The PXP driver's rotate guard (`PXP_ERR_ALIGN`, RM 52.6.4) wants 64-byte-aligned windows.

So damage-only rotation does NOT use `outputAt()`.  A damaged rectangle is rotated with **pointer-offset
surfaces**: a source `PXPSurface` pointing INTO the canvas at the rectangle (pitch 5120) and an output
surface pointing INTO the scanout buffer at the rotated position (pitch 2880); to the PXP each op is a
rotation at the origin -- the path already proven -- and the model and silicon agree by construction.
Every rectangle is first snapped OUTWARD so that `x`, `y`, `x+w` and `y+h` are multiples of 16: then
`x*4` and `(720-y-h)*4` are multiples of 64, and the row offsets `y*5120` / `x*2880` are multiples of 64
regardless (5120 = 64·80, 2880 = 64·45), so every derived pointer satisfies the alignment guard.  For a
logical rect `(x, y, w, h)` the physical destination rect is `(720-y-h, x, h, w)`.

**`display/pxp_rotate_probe`** -- a new example in the `rotary_knob_bench` mould: Phase A (gated in QEMU)
is correctness with deterministic sums plus a case-count / begin-vs-done equality check so an empty or
truncated case table cannot pass vacuously; Phase B (after `crc_done`, never gated) is silicon timing.  It
needs no panel -- only SDRAM (the canvas plus a portrait destination, 7.4 MB).  It carries its OWN CPU
reference rotation, deliberately independent of the port's `map_rect` (§5.A), so the two are cross-checks.

Predictions, registered here before the bench runs:

| # | case | prediction |
|---|------|------------|
| P1 | full-frame 1280×720 XRGB8888 ROT90, SDRAM→SDRAM | ~25 ms (15–40), extrapolated from v6's ~150 MB/s PXP copy |
| P2 | 160×160 rect (one knob's damage) | ≤ 1 ms |
| P3 | fixed cost per op | tens of µs (v6 measured 4 µs for 16×16) |
| P4 | grid-snapped sub-rects at several non-origin positions, incl. the four edges and corners | bit-exact against the CPU reference, identical in QEMU and on silicon, sentinel border untouched |
| P5 | an off-grid (unaligned) sub-rect | OBSERVATION only: does the driver reject it, and does silicon care? (`pxp_blit_test` recorded `PXP_ALIGN=off2:ok`) |

**Decision rule.**
* P1 ≤ ~5 ms: rotate the full frame on every present; `plan()` (§5.A) still exists and is host-tested, but
  its threshold is set so every present is full-frame -- one code path, not two.
* Otherwise, if P4 holds: damage-grid present.  Each present rotates this frame's damage ∪ last frame's
  (the two scanout buffers alternate, so the back buffer is two presents stale), snapped to 16 px, and
  switches to ONE full-frame op when the union's area passes the threshold P1–P3 set.
* P4 fails on silicon: stop, and switch the SILICON present to the GC355 blit.  The QEMU model is not
  changed to paper over a divergence -- that rule is the tree's, not this spec's.

### 4.1 Measured on silicon, 2026-09-15

`examples/display/pxp_rotate_probe/transcript_hw_evkb.txt`: two complete SW4 boots, `errs=0` on every
timing line, every sum bit-identical between the boots AND to the QEMU pins.

| # | predicted | measured | verdict |
|---|-----------|----------|---------|
| P1 | ~25 ms (15–40) | **13 060 µs** (13 061) | missed, FASTER -- 0.0142 µs/px, ~70 MB/s of XRGB8888; the v6 copy extrapolation was 2× pessimistic |
| P2 | ≤ 1 ms | **366 µs** (367) | held |
| P3 | "tens of µs" | **5 µs** | missed as worded -- the cited v6 figure (4 µs) was right, the prediction's words were not |
| P4 | bit-exact, QEMU = silicon | all 11 graded `pixel=ok`, sums identical to QEMU | **held** |
| P5 | observation | `case=offgrid api=ok pixel=ok` | silicon rotates correctly 32 B off the 64-B grid too; not relied on -- the port still snaps |

**Decision: damage-grid present, threshold 915 456 logical px** (99.33 % of the 921 600-px frame), from
`c = (366 − 5) / (160² − 16²) = 0.014244 µs/px` and `floor((13060 − 4·5) / c / 256) · 256`.  The rotate cost
is linear in pixels and the per-op overhead is ~5 µs, so rects win until the union is essentially the whole
screen -- a full present costs 13 ms of a 33 ms frame, a knob's damage (cur ∪ prev) well under 1 ms.  The
two start-up presents and PXP error recovery stay forced full.  The minimum over both boots is used (the
first boot's 367 µs would give 912 896).

## 5. The port and the touch binding

Four pieces, one per repo.  Nothing that exists today changes behaviour: every piece is a new function or a
new file, so no other example's golden can move.

### 5.A `LVGL/port/lvgl_panel_rotation.h` -- new, pure C, host-tested

* `lvgl_panel_present_t` with a single value, `LVGL_PANEL_PRESENT_CW90` ("the picture is rotated 90°
  clockwise on the glass").  Deliberately NOT `lv_display_rotation_t`: that enum means LVGL's own rotation
  machinery, which this design does not use, and its direction convention is the opposite of ours.  Any
  other value asserts, so there is no code path without evidence behind it.
* Pure functions: `map_rect` (logical → physical, §2), `map_point_inv` (physical → logical, for touch),
  `snap16` (grow a rect outward to the 16-px grid, clamped to the frame), and
  `plan(cur, prev) -> rects | FULL` (merge the snapped union; FULL past the area threshold).
* Host suite `LVGL/port/tests/run.sh`: round-trip properties, edges, corners, overlap merging, the
  threshold boundary, and the load-bearing property test of §7.  NEGATIVE ARMS, which are the point: a CCW
  mutant mapping, a snap that shrinks instead of grows, and a plan that forgets the previous frame's damage
  must each fail BY NAME.

### 5.B `lvgl_mipi_panel_create_rotated(DisplayClass &, lvgl_panel_present_t)`

* Asserts the panel is 720×1280 XRGB8888.  Allocates ONE 64-byte-aligned 1280×720 canvas in SDRAM (the
  call-once pattern of `lcdifv2AllocAltFramebuffer`) plus the existing alternate scanout buffer.
  `lv_display_create(1280, 720)`, single buffer, `LV_DISPLAY_RENDER_MODE_DIRECT`, LVGL rotation 0.
* `flush_cb` accumulates each flushed area.  On the LAST flush, in this order:
  1. `preflip_cb(canvas)` -- the GPU compositor draws into the canvas and ends with its own `vg_lite_finish`;
  2. `flip_sync()` -- the back buffer may still be on the glass until the previous flip retires; the wait
     moves from before-render (today's `flush_wait_cb`) to before-present;
  3. `plan(cur, prev)`, then one synchronous PXP op per rect (`run()` waits, so LVGL cannot start the next
     render into the canvas while the PXP still reads it);
  4. `FlipTo(back)`, pending store, `flips++`, `frame_done`;
  5. `prev = cur`, then `lv_display_flush_ready()` immediately.  No `flush_wait_cb` is registered: the
     canvas is never scanned out, so there is nothing for LVGL to wait for.
* Why it is tear-free: the back buffer holds frame n−2 plus the rotated union of frames n−1 and n, copied
  from a canvas that is always complete -- a complete frame, by construction.  In the db path LVGL itself
  copies last frame's damage between the two scanout buffers (`lv_refr.c` sync areas); with one canvas
  that copy disappears, and the rotate op does that work instead -- so the rotate is not purely added cost.
* Start-up: the first TWO presents are forced full-frame, because both scanout buffers start undefined.
* PXP failure is LOUD and cannot wedge the pipeline: it counts `rot_errors`, forces the next present to
  full-frame, and still flips.  The gate asserts `rot_errors=0`.
* New counters `rot_ops`, `rot_full`, `rot_px`, `rot_us` (cumulative micros inside the PXP ops),
  `rot_errors`.  The existing `flips` / `vsyncs` / `timeouts` / `isrs` / `wait_us` keep their meanings.
* `scanned_fb()` still names the PORTRAIT buffer on the glass, so `ACIDBOX_UI_SUM` checksums what is
  actually presented, rotation included (`PANEL_FB_BYTES` is the same 3,686,400 either way).
* `create()` and `create_db()` are untouched.

### 5.C `lvgl_gt911_indev_create_rotated(disp, touch, present)`

Raw GT911 → physical 720×1280 (scaled by the part's REPORTED resolution against the PHYSICAL panel, not the
landscape display) → `map_point_inv` → logical.  The existing `rotation == LV_DISPLAY_ROTATION_0` assert
stays and is still true.  `lvgl_gt911_indev_create()` is unchanged.

### 5.D SynthUI: no change

acid_box calls `synthui_rotary_gpu_begin_deferred(1280, 720, 5120)` instead of passing
`Display.width()/height()`.  Deferred mode lazily wraps whichever buffer the hook hands it; with one canvas
that is one wrap, and the "a third buffer is a bug" tripwire is never reached.

### 5.E Checked in the plan, not assumed

* SDRAM has room for another 3.6 MB beside the two scanout buffers and the VGLite pool.
* ITCM: the new port code lands in libLVGL, which the `M2_BT_OUT` bench builds keep in ITCM (13,968 B of
  headroom on `build-bt`, a 2 KB floor that fails by name).  `bench_check` rebuilds every declared bench
  configuration.
* The LVGL pin in `evkb.cmake` moves; the fresh-user path is verified by RUNNING the acid_box gate on a
  `-DEVKB_FORCE_FETCH=ON` ELF, never by reading the SHA.

## 6. Layout C in acid_box (logical 1280×720)

Chosen from three mockups: bands, with the pattern up top and the sound knobs along the edge nearest the
player once the board lies flat.  **The band between the pattern and the knobs (y 308..520) is left EMPTY
on purpose -- it is reserved for later work and is not centred away.**  Every size is a widget the file
already creates; the mockup's divider line is not built.

| band | placement | gate target → raw GT911 script |
|------|-----------|--------------------------------|
| top bar | title (24, 36) · `−` (560, 20) 50×48 · bpm (624, 35) · `+` (690, 20) 50×48 · ▶ (1040, 20) 100×48 · ■ (1156, 20) 100×48 | ▶ centre (1090, 44) → **`P 94 85`**, lands at (1088, 42), inside 1040..1140 × 20..68 |
| pattern | lane 2×8, 100 px cells at 108×112 pitch from (16, 96) → 16..872 × 96..308 · pitch knob (912, 96) 150 px · note (1080, 110) · ACC (1080, 148) 88×56 · SLD (1176, 148) 88×56 · SAW (1080, 222) 184×56 | cell 2 centre (282, 146) → **`P 80 22`**, lands at (282, 143), inside 232..332 × 96..196 |
| *reserved* | y 308..520, empty | — |
| sound knobs | 8 × 150 px at 158 pitch from (16, 520) → 16..1272 × 520..670, labels below at today's offset | CUTOFF drag, logical (91, 582 → 647) → **`P 19 7` … `P 10 7`** at 1 % steps; every sample inside 520..670 so the drag never ends as PRESS_LOST |

The script's percentages are of the PHYSICAL panel (the GT911 model's resolution), produced by a generator
that applies §2's inverse -- never hand-counted, keeping the rule `touch_script.txt`'s header already
states.  The drag stays a downward drag in logical coordinates (cutoff strictly DECREASING, today's
assertion); rotated, it is a leftward raw drag, and the touch mapping makes that invisible to LVGL.

Code structure in `acid_box.cpp`: the scattered `lv_obj_set_pos` literals become ONE block of named
geometry constants (`BAR_Y`, `LANE_X0/Y0/CELL/PITCH_X/PITCH_Y`, `KNOB_X0/Y0/SIZE/PITCH`, …) with the
tap-point comment beside them; `mkknob()` and the lane loop compute from those.  `setup()` swaps
`create_db` for `create_rotated(Display, LVGL_PANEL_PRESENT_CW90)` and `lvgl_gt911_indev_create` for the
rotated variant; the compositor is begun with 1280×720 / 5120.  Knob maps, callbacks, the poller and all
audio/BT glue are untouched.

## 7. Testing

**Host suites** (`LVGL/port/tests/run.sh`, new): unit tests over the pure functions (§5.A), and the
load-bearing PROPERTY TEST -- simulate the canvas plus the two alternating scanout buffers, feed random
damage sequences through `plan()` and a reference rotator, and assert the PRESENTED buffer equals the
rotated canvas after every present.  The three negative arms of §5.A each fail by name.

**QEMU gates, 139 → 140.**
* `display/pxp_rotate_probe` (new): P4 sums, the sentinel border, the case-count check (§4).
* `display/acid_box`, reworked: `touch_script.txt` regenerated from §6; the GEOMETRY header re-derived; the
  sw golden re-recorded (it checksums the presented, rotated portrait buffer); the RMS / CUTOFF / STEP
  assertions unchanged.
* A new `ACIDBOX_ROT ops= full= px= us= errors=` line, printed at boot beside `ACIDBOX_VSYNC` and with every
  `ACIDBOX_BAR`; the gate asserts `ops>0`, `full>=2` (the two forced start-up presents) and `errors=0`.  An
  image that fell back to the portrait `create_db` path has no such line and fails by name.
* A ROTATION EQUALITY GUARD, gate-compared and never re-goldened, in the spirit of the knob's delta guard:
  after `flip_sync()` in `loop()` the presented buffer and the canvas hold the same frame (single thread --
  no render can intervene between the present and the check), so every 16th PHYSICAL row (= every 16th
  logical column) is compared against a CPU rotation of the canvas.  Runs once at boot (the full-frame
  path) and once per `ACIDBOX_BAR` (the damage path, driven by the cursor and the scripted edits); prints
  `ACIDBOX_ROT_EQ pass= fail=`; the gate asserts `fail=0` and `pass >= bars+1`.  Sampled, not exhaustive: it
  catches any stale region ≥ 16 logical columns wide at ~230 KB per check; the host property test is the
  exhaustive one.
* Vacuity suite: cases for the probe; the acid_box `transcript_qemu.txt` fixture re-captured (a fixture
  goes stale silently -- CLAUDE.md, 2026-08-25); the `ACIDBOX_ROT` line and the equality guard each
  DEMONSTRATED RED before they are trusted.

## 8. Silicon acceptance (acid_box on the EVKB)

1. **Orientation and touch.**  The picture is upright with the board turned CCW, by eye.  Tapping cell *i*
   prints `STEP[i]` for that exact cell, checked at all four corners of the lane, plus ACC / SLD / SAW / ▶ / ■.
2. **gpu golden re-recorded**, bit-identical across ≥ 2 SW4 boots.  The portrait `0x1479CEE8` is recorded
   as RETIRED in `transcript_hw_evkb.txt`, not deleted.
3. **Counters.**  `ACIDBOX_VSYNC timeouts=0`, `ACIDBOX_ROT errors=0`, `ACIDBOX_ROT_EQ fail=0`.
4. **Performance**, `ACIDBOX_LOOPSTAT` during a knob drag against the portrait baseline (30 fps, 32.4 ms
   median frame, touch p95 46 ms): 30 fps with the median within ±15 %; touch p95 no worse than +15 %;
   per-present `rot_us` reported.
5. **No scanout flash**: 60 fps camera video during knob animation.  Checksums cannot see a tear; the camera
   is the instrument (the NEW-20 scanout-flash finding).
6. **`M2_BT_OUT`**: every bench configuration builds via `bench_check` with the ITCM floor met, and streams
   with `pcmdrops=0` while dragging.

## 9. Close-out

Sweep at **140 passed, 0 failed, 0 SKIP** (or the documented `cm4_audio_test` exception; acid_box itself
stays in the fresh-clone-red set for its local-only `touch-script` dependency).  `LICENSE-AUDIT: PASS` with
a `GATES` entry for the probe.  Vacuity suite green.  LVGL pin bumped and the fresh-user run done on the
fetched ELF.  CLAUDE.md's gate count and this example's entries updated; `docs/KNOWN-BROKEN-GATES.md`
unchanged unless the probe reveals a model divergence, which is then documented rather than hidden.

## 10. Risks, named

* **P1 is a guess** from a copy bench; the probe exists because of it.  A ~25 ms full-frame present would
  eat most of the 33 ms budget, which is why the damage path is designed in from the start.
* **A rotated op at a non-origin pointer has never run on silicon.**  P4 is the first time; the GC355 blit
  is the escape hatch, chosen on measurement, not argument.
* **The present now runs inside `flush_cb`**, so any PXP stall stalls LVGL's refresh.  `run()`'s 100 ms
  timeout bounds it, `rot_errors` names it, and the forced full-frame recovery keeps the picture coherent.
* **Touch scaling uses the physical panel**; a binding that scaled by the landscape display would put every
  contact ~44 % off along one axis and still "work" for taps near the centre -- the four-corner check in §8.1
  is there to catch exactly that.
