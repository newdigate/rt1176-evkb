# RotaryKnob render-strategy bench — design

Date: 2026-08-27
Status: approved (brainstorm 2026-08-27)
Tracking: Linear NEW-12 (related; the gpu-vector cell answers its open
question) + a new issue "RotaryKnob: render-strategy bench + widget".

## 1. Goal & scope

A new example, `display/rotary_knob_bench`, renders the **RotaryKnob** design
(claude.ai/design project `79ec272e-93e2-41e7-a4cc-566b130c67f5`,
`RotaryKnob.dc.html`) through **six render paths** and measures which is
fastest on silicon, under the exact FPSBENCH workload from
`display/vglite_lvgl_test`: a 4×4 grid of 16 knobs, every knob's angle
advanced every refresh (full-scene damage), success criterion **≥30 fps**.

This spec covers the bench only (Phase 1). Phase 2 — implementing the winning
strategy as `synthui_rotary_knob` in SynthUI and replacing the existing
`synthui_knob` — gets its own spec once the numbers exist (§12).

**Baseline honesty note.** The recorded 2.83 fps (sw) / 2.45 fps (GPU) are the
*old* knob design's numbers. The bench's vector/sw cell is the comparable
baseline for the *new* design under the same workload and harness; the old
numbers are context, not a controlled comparison. The controlled comparisons
are cell-vs-cell inside one boot.

## 2. Why the RotaryKnob design changes the performance problem

The old `synthui_knob` geometry *changes shape* with angle (crescent span,
angle-driven luminance), so nothing is cacheable and every frame rebuilds
everything — that is the root of both the sw slowness (LVGL arc mask
arithmetic per frame) and NEW-12's GPU slowness (per-task `vg_lite` path
construction).

RotaryKnob is structurally different: a **static well** (ring, or bounded arc
track) plus a **rigid rotor** — a group of 2–9 filled paths that rotates as a
unit (`rotate(angle 50 50)`), no gradients, no angle-dependent shape. A rigid
rotor is exactly what makes caching legal: cached vector paths + a rotation
matrix, a cached bitmap + rotation, or a filmstrip of pre-rotated frames.

Variants (from the DC file): notch (3 paths), split (3), baton (3),
crescent (3), facet (9). The bench renders **notch** and **facet** — the
lightest and heaviest — to bracket the path-count axis. Vector cost scales
with path count; bitmap and filmstrip cost does not, and that slope is itself
a finding.

## 3. The cell matrix — 3 strategies × 2 engines × 2 variants = 12 cells

Strategy and engine are orthogonal:

| strategy | sw (CPU / LVGL) | gpu (GC355 / VGLite) |
|---|---|---|
| **vector** | `renderVals()` geometry as per-frame LVGL sw draw tasks | `vg_lite_path_t` built once at init; per frame only a rotation matrix + `vg_lite_draw` |
| **rotate-bitmap** | rotor rendered once to ARGB8888 at init; per frame LVGL sw image transform (rotate about pivot) | same rotor buffer; per frame `vg_lite_blit` with rotation matrix |
| **filmstrip** | N=64 pre-rotated frames at init; per frame plain image blit, no transform | same frames; `vg_lite_blit`, identity matrix |

Notes per cell family:

- **vector/sw** — the geometry port: discs are rotation-invariant
  (`LV_RADIUS_CIRCLE` rects); the annular ring sectors (`ring(r0,r1,a1,a2)`)
  become angle-offset LVGL arcs; polygons (facet triangles, baton quad) get
  their vertices rotated before drawing; half-discs are 180° arcs with width
  = radius. Reuse `synthui_knob.cpp`'s fold-into-[0,360) arc discipline —
  LVGL's sw arc clamps negative starts (known trap).
- **vector/gpu** — this is **NEW-12's cached-paths answer**. Paths are built
  in rotor-local coordinates once; per frame the matrix is
  translate(center)·rotate(angle). Drawn via direct `vg_lite_*` calls in a
  post-`LV_EVENT_REFR_READY` GPU pass inside the timed frame (LVGL source
  untouched) — **not** through LVGL's VG_LITE draw unit — so per-task path
  rebuild is bypassed entirely (licence firewall). The pass runs after
  LVGL's sw tasks because a direct GPU call issued inside
  `LV_EVENT_DRAW_MAIN` executes immediately, while LVGL's own deferred sw
  draw tasks (background, wells) execute later in the refresh and would
  overpaint the rotor. For gpu cells the widget draw callback paints only
  the well. Annular sectors use non-zero winding with the inner arc
  reversed (plan-level detail).
- **rotate-bitmap** — the rotor bitmap is rendered **once at init by the
  vector/sw renderer** (single source of truth for geometry) into an
  ARGB8888 buffer with transparent background. 45°-class angles are the
  resampling worst case; the per-cell CRC and the silicon eyeball make the
  quality cost visible rather than hidden.
- **filmstrip** — the classic hardware-synth trick. 64 frames = 5.625° steps.
  Init cost and RAM are reported, not hidden (§6).

The **well is rendered per frame by common sw code in every cell** — it is a
constant addend, kept identical so the A/B isolates the rotor strategy.

## 4. Bench architecture — one ELF, two phases

One image, one boot, all cells sequentially — same clocks, same memory state,
a controlled A/B (the `[irq]` gate precedent). The image links VGLite via
`import_evkb_vglite()`; GPU cells detect GPU absence at runtime
(`vglite_probe` pattern) and report it honestly.

**Phase A — correctness pass** (runs first, fast, QEMU-gateable):
each cell renders one canonical frame at **angle 45°** (= 8 filmstrip steps,
so the filmstrip cell lands exactly on-step; also off-axis, worst case for
resampling), checksums the whole framebuffer (FNV-1a via `lvgl_sum_*`, the
tree's golden arithmetic — the token stays `crc=`), prints its cell line,
then `crc_done cells=12 ...`.

**Phase B — timing pass**: per cell, the FPSBENCH method verbatim —
`LV_EVENT_REFR_START`→`REFR_READY` timing over **64 refreshes**, angles
advanced by a timer callback so every refresh carries all-16 damage. Ends
`bench_done ...` then the periodic heartbeat.

**Angle sequence** (identical for every cell): frame *i*, knob *k* →
angle = (i·5.625° + k·22.5°) mod 360°. All angles are multiples of the
filmstrip step, so the filmstrip cell does zero interpolation and every cell
does identical per-frame work.

Between cells the rotor arena (one SDRAM allocation, §6) is reused; cells
tear down their LVGL objects so damage state does not leak across cells.

## 5. Output format (integer fields only — no float printf)

Phase A, per cell:

    cell=<strategy>/<engine>/<variant> st=ok crc=0x<8hex> init_us=<n> rotor_bytes=<n>
    cell=<strategy>/gpu/<variant> st=gpu-absent          (QEMU, all six gpu cells)

then `crc_done cells=12 ok=<n> gpu_absent=<n>`.

Phase B, per cell:

    time=<strategy>/<engine>/<variant> frames=64 mfps_med=<n> us_med=<n> us_mean=<n>
    time=<strategy>/gpu/<variant> st=gpu-absent

(`mfps` = millifps, e.g. 2830 = 2.83 fps), then
`bench_done cells=12 timed=<n> gpu_absent=<n>` and the heartbeat.

Strategy tokens in `cell=`/`time=` lines are exactly `vector`, `bitmap` and
`strip` — not the prose names `rotate-bitmap`/`filmstrip` used elsewhere in
this document.

## 6. Memory & placement

- Knob size **150×150** (grid parity with `vglite_lvgl_test`: positions
  15+c·175, 120+r·175). Rotor bitmap: ARGB8888, 150×150 = 90,000 B.
- Filmstrip: 64 × 90,000 B = 5,760,000 B ≈ 5.49 MB — **SDRAM** (EXTMEM), one
  static arena rebuilt at each strip cell's init (so `init_us` carries the
  honest per-cell cost); exact bytes reported in `rotor_bytes=`.
- vg_lite buffers respect the driver's alignment requirements (the 64-byte
  command-buffer lesson from VGLite Phase 1 generalizes: misalignment hangs
  the front end while every API call returns SUCCESS — check `AQHiIdle`).
- All bench code lives in the example directory. **No SynthUI changes in
  Phase 1** — no pin churn, no fresh-clone SKIP risk. Geometry is promoted
  into SynthUI in Phase 2.
- Panel: RK055 (720×1280), same grid layout as `vglite_lvgl_test`, for
  workload parity. Board: rt1176 only (no `boards` sidecar).

## 7. Correctness discipline

Per-cell CRC goldens. Cells are **never** expected to match each other — sw
AA ≠ GPU AA ≠ resampled rotation (the two-golden-sets precedent from
`vglite_lvgl_test`; do not "fix" it). What is asserted:

- The six **sw cells' CRCs are pinned in the QEMU gate** — the Knob pilot
  proved LVGL sw rendering is bit-identical across QEMU/host/silicon, so
  these goldens carry to the bench.
- The six **GPU cells' CRCs are silicon-only**, recorded in
  `transcript_hw_evkb.txt` with the fps table, visually verified once.
- For GPU cells the CRC is computed only after `vg_lite_finish` and D-cache
  invalidate over the whole framebuffer — a CRC read through a stale cache
  is a golden for the wrong pixels.

This is what stops "renders garbage fast" from winning the bench.

## 8. QEMU gate

One gate, `run_qemu.sh` (single script → no `[variant]` suffix; gate id
`rt1176:display/rotary_knob_bench`). It waits on `^crc_done ` — the **last**
line it parses, per the mid-line-reap lesson — and asserts:

1. All six sw cell lines present, `st=ok`, CRCs exactly matching the pinned
   goldens.
2. All six gpu cell lines present with `st=gpu-absent` — the honest negative.
3. **Tripwire**: no line containing `/gpu/` may carry `crc=` or `mfps_med=`
   in a QEMU capture. A GPU result invented with no GPU present must fail by
   name.
4. `crc_done cells=12` tally.

Discipline items, all mandatory before the gate is trusted:

- **Demonstrated RED** at least twice: once against a wrong pinned CRC, once
  against a faked `/gpu/ ... crc=` line appended to a capture (the tripwire).
  Quote the demonstrations in the gate header.
- `gate-vacuity.test.sh` entry (needs the committed `transcript_qemu.txt`
  fixture — capture it **after** the gate passes, and re-capture whenever the
  example's output changes: the 2026-08-25 stale-fixture lesson).
- `tools/license-audit.sh` `GATES` manifest entry for the new example; run
  the audit.
- Sweep arithmetic: **121 → 122**; update the CLAUDE.md sweep narrative and
  re-measure by running the sweep, not by counting files.

QEMU has no GC355, so a green gate says the sw cells render correctly and the
GPU cells degrade honestly — necessary, not sufficient. Silicon is where the
bench's actual question is answered.

## 9. Hardware verification

Full run on the EVKB: both phases, all 12 cells. `transcript_hw_evkb.txt`
records the cell lines, the timing table, and the GPU CRCs. The fps table is
appended to this document (§13) and posted to the Linear issue. Bench console
via the MCU-Link VCOM; flash with LinkServer per the standing bench order
(flash load → verify → attach reader → reset).

## 10. Risks

- **GPU/CPU coherency** on the shared framebuffer: `vg_lite_finish` +
  explicit cache maintenance around every GPU cell's draw and CRC. Known
  territory from `vglite_lvgl_test`; the diagnostic is `AQHiIdle` bit 0 and
  counted ISRs, not API return codes.
- **LVGL sw arc clamp** for negative start angles — solved once in
  `synthui_knob.cpp`; reuse that fold.
- **vg_lite fill rule** for annuli (non-zero winding, reversed inner arc) —
  wrong winding renders a full disc; the per-cell CRC catches it.
- **SDRAM bandwidth** may cap all fast cells equally (framebuffer and
  filmstrip both live there). If several cells plateau at the same number,
  that plateau is the memory ceiling — report it as the finding it is.
- **QEMU pacing**: Phase A is only 12 single frames, well inside qrun's 60 s
  cap even at QEMU's rendering speed. Phase B runs after `crc_done`, so gate
  timing does not depend on it.

## 11. Deliverables

1. `examples/display/rotary_knob_bench/` — example, `run_qemu.sh`, both
   transcripts, CMakeLists (shared toolchain via `../../../evkb.cmake`).
2. Gate wired into sweep, vacuity suite, licence audit (§8).
3. Results table in §13 + Linear comment; a winner (or a measured tie)
   named, with the RAM/init/quality trade-offs stated.
4. Linear: new issue "RotaryKnob: render-strategy bench + widget" related to
   NEW-12; NEW-12 closes when the vector/gpu cell is measured.

## 12. Phase 2 shape (out of scope here)

`synthui_rotary_knob` in SynthUI implementing the winning strategy: all five
variants, the DC props surface (variant/theme/state/mode/accent/min/max/
angle/size), input layer reused from `synthui_knob` (vertical drag, unsnapped
accumulator), then replacement of the old knob in `synthui_knob_test`,
`synthui_step_test`, `acid_box` and `vglite_lvgl_test` with re-goldening —
each a deliberate step in its own spec, informed by §13's numbers.

## 13. Results (to be filled from silicon)

| cell | mfps_med | us_med | init_us | rotor_bytes | crc |
|---|---|---|---|---|---|
| *(12 rows after the hardware run)* | | | | | |
