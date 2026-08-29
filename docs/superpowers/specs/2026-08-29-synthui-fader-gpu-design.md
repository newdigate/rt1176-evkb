# SynthUI Fader GPU compositor — design

Date: 2026-08-29
Status: approved (brainstorm 2026-08-29 evening)
Tracking: Linear **NEW-23** ("SynthUI Fader: tear-free 30 fps widget") — the
escalation the base spec (`2026-08-29-synthui-fader-design.md` §10) reserved
for a 30 fps miss. Related: NEW-20 (the rotary compositor this transplants).

## 1. Goal, and what the diagnosis established

The sw-only fader measured **11 fps** (us_med 84 047) on the 16-bank worst
case. The instrumented follow-up (fence-wait counter in the LVGL port,
flushed-px, 1-fader and min-tick scaling phases — one boot, four numbers)
pinned the cost precisely:

- flip fence: 14 ms of 84 (14 **µs** with one fader) — innocent;
- pixels: ~0.4 µs/px, ≈17 ms of the delta frame — innocent;
- **~5.3 ms per damaged widget, ~90 µs per draw task even when fully
  clipped away** — LVGL 9 heap-allocates every draw task from a pool this
  port places in uncached SDRAM, on a core that never enables the D-cache.
  This is NEW-20's "LVGL sw floor" (knob sw-delta 71 ms), now explained.

No damage-shrinking reaches 30 fps against that floor. The fix here is the
**rotary's GPU compositor pattern**: in GPU mode the sw path stops drawing
faders entirely, so the per-widget task churn disappears and the GC355 does
the pixels. Proven: the knob went 14 → 32.1 fps vsync-locked on a heavier
scene with exactly this structure.

**The criterion, stated precisely** (NEW-20 precedent): ≥30 fps is evaluated
as **`mfps_med ≥ 30000`** — a render rate. `LV_DEF_REFR_PERIOD` (33 ms
default) caps wall-clock `fps_avg` near 30 regardless of render speed;
`fps_avg`, the vsync witness, and `fd_fps_fullinv` are reported honestly
alongside but are not the acceptance number.

## 2. Non-goals

- No change to the sw rendering path — every QEMU golden must hold
  bit-for-bit (the gate proves it).
- No platform sw-floor work: pool relocation (which BusFaulted `usb_init` in
  a one-variable probe — mechanism unexplained), D-cache enablement, and
  LVGL-level task batching are **filed as a separate platform issue**, not
  attempted here.
- No shared multi-widget compositor infrastructure (§7 documents the
  app-level pattern instead).
- No new gate: `rt1176:display/synthui_fader_test` stays the one gate
  (count stays 123); it gains engine tripwires only.

## 3. Widget core changes (SynthUI)

Mirror the rotary's two-TU split exactly:

- New `src/synthui_fader_private.h`: moves `synthui_fader_t` out of the
  core TU; adds the instance registry (`prev`/`next`, head pointer
  `synthui_fader_list`), the `gpu_pending` flag, and exports the palette
  resolver `synthui_fader_palette(const synthui_fader_t *, synthui_fader_palette_t *)`
  (the pure function the base design prepared — it moves to the header now,
  exactly as its comment promised). The geometry helpers the compositor
  needs (`fd_geom`, `fd_cap_y`) become non-static
  (`synthui_fader_geom` / `synthui_fader_cap_y`) with their unit-space
  contract documented.
- Core TU: constructor links / destructor unlinks the registry (destructor
  is new — the sw-only widget had none); `fd_draw` gains the rotary's
  three-line GPU gate at its top: when `synthui_fader_gpu_enabled` is true,
  set `gpu_pending = true` and return — LVGL paints only the screen ground
  beneath. `synthui_fader_gpu_enabled` is defined in the CORE (false
  forever in a build without `src/vglite/`), so the core never references
  GPU symbols — the rotary's exact linking discipline.
- **Damage logic is untouched and engine-shared**: `set_value`'s cap-extent
  union, the press/release cap-only invalidate, and full invalidates on
  config changes stay as-is; the compositor consumes whatever LVGL rendered.

## 4. The GPU TU (`src/vglite/synthui_fader_gpu.{h,cpp}`)

API mirrors the rotary: `synthui_fader_gpu_begin_deferred(w, h, stride)`
(returns false and leaves everything sw if init fails),
`synthui_fader_gpu_compose_into(uint8_t *back_buffer)` (the pre-flip hook),
`synthui_fader_gpu_errors()`.

- **Deferred pre-flip compose only** — no live-scanout mode. The compositor
  draws into the just-rendered back buffer before the flip (the tear-free
  structure the scanout-flash finding forced on the rotary).
- **Scissor to the display's rendered areas** (`disp->inv_areas` as joined
  by LVGL) — never to widget-stored rects (the superset-join hole class,
  caught in NEW-20 review).
- For each registered fader with `gpu_pending` set, draw its FULL content
  (panel, ticks, rod, center line, cap) clipped by the scissor, then clear
  the flag. Colors come from `synthui_fader_palette` — the one function both
  engines share, so they cannot disagree on state → color.
- **Cached paths, never per-frame construction** (the NEW-12/NEW-20
  lesson): per instance, a "well" path set (panel rect, tick lines, rod,
  center) built lazily on first compose and rebuilt only when a config
  setter dirties it (a `gpu_geom_dirty` flag set by `set_ticks`,
  `set_center`, `set_panel`, and size changes); the cap is a rigid path
  group under a per-frame **translation** matrix from
  `synthui_fader_cap_y` — cheaper than the knob's rotation. Every path is
  single-contour (the winding-2 multi-subpath track was the knob's one
  source of per-boot nondeterminism).
- **Cap gradient** via vg_lite's linear-gradient ramp, cached per palette
  state and rebuilt only on state change. Pixel parity with the sw
  gradient is NOT required — two golden sets, never reconciled (house
  precedent since vglite_lvgl_test).
- Waits follow the fixed VGLite port discipline (flag AND `AQHiIdle`);
  errors accumulate in the counter the example prints.

## 5. Example & gate changes

`examples/display/synthui_fader_test` gains the knob test's wiring, verbatim
in structure: `import_evkb_synthui(VGLITE)` (which pulls in the VGLite
driver itself), the 2 MB EXTMEM pool (64-aligned, memset before init — the
never-zeroed-EXTMEM lesson), `vg_lite_hal_probe_chip_id()` BEFORE
`vg_lite_init` (which spins on absent hardware — the honest-negative
probe), `synthui_fader_gpu_begin_deferred` + `lvgl_mipi_panel_set_preflip_cb`
only when the GPU is genuinely up, and new tokens:

- `fd_engine=sw|gpu` printed after the probe;
- `fd_gpu_err=N` and `fd_gpu_diag irqs=… wait_timeouts=…` printed ONLY in
  gpu mode (silicon), never in sw mode.

Gate (`run_qemu.sh`) additions, all demonstrated RED before trust:

- assert `fd_engine=sw` anchored — QEMU has no GC355;
- tripwire: `fd_engine=gpu` appearing in QEMU fails by name;
- tripwire: `fd_gpu_err=` appearing in QEMU fails by name;
- every existing assertion and golden stays byte-identical (the sw path is
  untouched; the knob test is the precedent that a VGLITE-flavored ELF
  leaves QEMU goldens unmoved).

Fixture re-captured after the gate runs (it gains the `fd_engine=sw` line);
the three vacuity cases must still pass against the new fixture.

## 6. Silicon acceptance

1. `fd_engine=gpu`, `fd_gpu_err=0`, sane `fd_gpu_diag` (irqs ≥ composes —
   the stale-flag detector), `fd_vsync … timeouts=0`.
2. A **gpu golden set** for `fd_crc`/`fd_delta_crc`/`fd_fresh_crc`,
   recorded in `transcript_hw_evkb.txt`, **bit-stable across ≥3 SW4 boots**
   (one knob defect hid behind exactly one boot).
3. The delta-equality guard holds ON THE GPU PATH every boot
   (`fd_delta_eq=PASS` with gpu checksums) — this guard caught all three of
   the knob's silicon compositor defects; it is the acceptance instrument,
   not the fps number.
4. The engagement guard stays in band (damage logic is engine-shared).
5. **`fd_fps` `mfps_med ≥ 30000`** on the 16-bank (§1's criterion), with
   `fd_probe_delta` showing the fence still healthy; `fd_fps_fullinv` and
   the Phase C probes recorded for the platform issue's baseline.
6. Eyes/camera pass during the animation — checksums can never see scanout
   artifacts.

STOP-rule: if the gpu path misses `mfps_med 30000`, record everything and
stop — the next lever is the platform issue, not more compositor work.

## 7. Mixed scenes (documented pattern, not built)

The panel port exposes ONE pre-flip slot. An app composing several widget
families (knobs + faders) writes a two-line wrapper calling each family's
`*_gpu_compose_into` in any order — the families draw disjoint widgets, so
ordering is irrelevant. No shared registry or dispatcher until a real scene
needs one (acid_box's eventual fader strip is the natural first customer).

## 8. Error handling & honest negatives

- Probe fails or `begin_deferred` fails → `fd_engine=sw`, widget fully
  functional on the sw path (the base widget IS the fallback).
- A compose called with no pending faders is a cheap no-op.
- vg_lite errors increment the counter and leave the sw-rendered ground
  visible rather than crashing; `fd_gpu_err=0` is asserted at acceptance.

## 9. Sequencing

1. **SynthUI**: private header + registry/`gpu_pending` refactor of the core
   TU (sw behavior identical — knob test AND fader gate both stay green,
   proving it), then the GPU TU. Compile rides `synthui_knob_test`'s
   VGLITE build (its glob picks up `src/vglite/*.cpp`), gate-checked.
2. **evkb**: example wiring + gate tripwires, QEMU green with unchanged
   goldens, fixture re-captured, vacuity re-run.
3. **Silicon**: gpu goldens ×3 boots, equality guard, fps acceptance,
   camera pass.
4. **Close-out**: push SynthUI + LVGL (the port wait counter from the
   diagnosis is already committed there), bump BOTH pins, fresh-fetch
   gate-run verify, sweep + audit + vacuity, NEW-23 resolution, file the
   platform sw-floor issue (uncached pool, ~90 µs/task numbers, the
   pool→DTCM `usb_init` BusFault repro recipe).

## 10. Risks

- **GC355 per-boot determinism** — the knob burned a day on exactly this
  (winding-2 track). Mitigated by single-contour paths and the ≥3-boot
  golden rule; the equality guard is the tripwire.
- **Gradient ramp fidelity/stability** across boots — cached ramp, rebuilt
  only on state change; covered by the same repeated-boot goldens.
- **Residual sw floor**: LVGL still walks 16 dirty areas for the ground
  fill. The knob's 32.1 fps locked with the same 16-area walk bounds this
  risk; the fader's areas are smaller.
- **Bench wedges** (MCU-Link DAP after repeated flash cycles) — replug the
  DEBUG USB; SW4-press loops with a persistent reader remain the procedure.
