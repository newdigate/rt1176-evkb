# RotaryKnob render-strategy bench — design

Date: 2026-08-27
Status: approved (brainstorm 2026-08-27)
Tracking: Linear **NEW-20** ("RotaryKnob: render-strategy bench + widget") —
the issue this document is the design for. Linear NEW-12 is related but
separate: the gpu-vector cell answers its open question.

## 1. Goal & scope

A new example, `display/rotary_knob_bench`, renders the **RotaryKnob** design
(claude.ai/design project `79ec272e-93e2-41e7-a4cc-566b130c67f5`,
`RotaryKnob.dc.html`) through **six render paths** and measures which is
fastest on silicon, under the exact FPSBENCH workload from
`display/vglite_lvgl_test`: a 4×4 grid of 16 knobs, every knob's angle
advanced every refresh (all-16-knob damage — 16 × 150×150 = 360,000 px, not
the full 921,600-px screen; §4 and the firmware both say so), success
criterion **≥30 fps**
(evaluated against `mfps_med` ≥ 30000 — a render rate, not a displayed one;
§5 says why the distinction matters).

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

then `crc_done cells=12 ok=<n> gpu_absent=<n> failed=<n>`.

**GPU cells carry a trailing ` gpu_err=<n>` on their `cell=` and `time=` lines**
— the count of `vg_lite_*` calls (map/draw/blit/finish) that did not return
`VG_LITE_SUCCESS` in that cell. **The hardware transcript must show `gpu_err=0`
on every GPU cell**; a non-zero value invalidates that cell's timing outright,
because a rejected blit draws nothing and therefore times beautifully. It is
appended for GPU cells only, so `sw` lines stay byte-identical for the gate's
greps. `failed=<n>` counts cells that could not be built at all (e.g.
`st=vg-overflow`); `ok + gpu_absent + failed == cells` is the vacuity check.

Phase B, per cell:

    time=<strategy>/<engine>/<variant> frames=64 mfps_med=<n> us_med=<n> us_min=<n> us_max=<n> us_mean=<n>
    time=<strategy>/gpu/<variant> st=gpu-absent
    time=<strategy>/<engine>/<variant> st=vg-overflow        (cell could not be built)
    time=<strategy>/<engine>/<variant> st=timeout nsamp=<n>   (cell wedged; 120 s cap)

(`mfps` = millifps, e.g. 2830 = 2.83 fps), then
`bench_done cells=12 timed=<n> gpu_absent=<n> failed=<n>` and the heartbeat.
`timed + gpu_absent + failed == cells` is the vacuity check, as in `crc_done`;
both `st=vg-overflow` and `st=timeout` count toward `failed`. The `st=timeout`
line carries the trailing ` gpu_err=<n>` on GPU cells (a cell that starved
mid-render is exactly where "was the GPU erroring?" is the first question);
the two build-failure lines do not, matching Phase A.

**Ranking is by `mfps_med`.** `us_mean` is reported beside it but **can be
outlier-dominated** — measured, not feared: two cells whose medians agreed to
within 0.3% reported means 45% apart because a single ~3 s frame landed in one
of them. `us_max` is what identifies such a culprit, and `us_min`/`us_max`
together are what say whether the mean describes the workload or one stall.
Read §13's table off `mfps_med`.

**`mfps_med` is a WORK RATE, not a display rate.** It is the reciprocal of the
median `LV_EVENT_REFR_START`→`LV_EVENT_REFR_READY` interval — the render
capacity of the path, GPU work and `vg_lite_finish` included. It is not frames
reaching the glass: `LV_DEF_REFR_PERIOD` is 33 ms, so LVGL caps *displayed* fps
near 30 no matter how fast a cell renders. §1's **≥30 fps** criterion is
therefore evaluated against `mfps_med` (≥30000), which is the quantity that can
exceed the cap and the quantity that discriminates between the six paths.

Strategy tokens in `cell=`/`time=` lines are exactly `vector`, `bitmap` and
`strip` — not the prose names `rotate-bitmap`/`filmstrip` used elsewhere in
this document.

## 6. Memory & placement

- Knob size **150×150** (grid parity with `vglite_lvgl_test`: positions
  15+c·175, 120+r·175). Rotor bitmap: ARGB8888, **150 rows at stride 640 B
  (160 px — 150 rounded up to the GPU's 16-pixel alignment) = 96,000 B**.
- Filmstrip: **64 × 96,000 B = 6,144,000 B ≈ 5.86 MB** — **SDRAM** (EXTMEM), one
  static arena rebuilt at each strip cell's init (so `init_us` carries the
  honest per-cell cost); exact bytes reported in `rotor_bytes=`.
- ★ **The 16-pixel stride is a correctness requirement, not padding hygiene.**
  This part has `gcFEATURE_VG_16PIXELS_ALIGNED=1` and
  `gcFEATURE_VG_ERROR_CHECK=1`, so `srcbuf_align_check`
  (`VGLite/vg_lite.c:1854-1861`, called from `vg_lite_blit` at `:4533`) returns
  `VG_LITE_INVALID_ARGUMENT` for a BGRA8888 source whose stride is not a
  multiple of 16 px × 4 B = 64 B. At the natural 150×4 = 600 the GPU bitmap and
  strip cells would draw **nothing** while posting excellent times. The build
  sets `LV_DRAW_BUF_STRIDE_ALIGN=64 LV_DRAW_BUF_ALIGN=64` so LVGL's canvas
  derives the same 640 and the CPU painter cannot disagree with the GPU
  consumer. Bonus: 96,000 is itself 64-aligned, so every filmstrip frame
  starts aligned (at 90,000 only 1 frame in 64 did). Measured: the padding is
  **checksum-neutral** — all six software goldens were unchanged by the switch.
- vg_lite buffers respect the driver's alignment requirements (the 64-byte
  command-buffer lesson from VGLite Phase 1 generalizes: misalignment hangs
  the front end while every API call returns SUCCESS — check `AQHiIdle`).
- ★ **GPU blit sources are software-premultiplied and declared
  `premultiplied = 0`.** That pairing looks wrong and is not: the cells blit
  with `VG_LITE_BLEND_SRC_OVER`, whose arithmetic is `S + D*(1 - Sa)` — the
  premultiplied Porter-Duff over — so the source must arrive premultiplied.
  **The straight-alpha alternative does exist on this part** (an earlier
  revision of this section said otherwise): the driver accepts
  `VG_LITE_BLEND_NORMAL_LVGL` and runs it *in hardware* —
  `convert_blend` maps it to `SRC_OVER`'s own HW value
  (`vg_lite.c:2560-2564`), and with `gcFEATURE_VG_SRC_PREMULTIPLIED = 0` here
  the driver forces `in_premult = 0` for it (`vg_lite.c:4777-4779`), which is
  precisely the straight-alpha lever. What `gcFEATURE_VG_LVGL_SUPPORT = 0`
  gates is only LVGL's *own* choice — `lv_vg_lite_support_blend_normal()`
  reads that bit and declines the mode. We follow LVGL because that is the
  combination this driver is exercised with. It is exactly what LVGL 9.4's own
  VG_LITE backend does against this driver (`lv_draw_vg_lite_img.c:66`
  premultiplies iff `!lv_vg_lite_support_blend_normal()`;
  `lv_vg_lite_utils.c:958` then picks `SRC_OVER`; LVGL never assigns
  `premultiplied`). Silicon bring-up should still **look for dark fringes on
  antialiased rotor edges** in the GPU bitmap/strip cells: if they appear, the
  hardware is premultiplying a second time, and the cheaper experiment is
  **straight alpha + `VG_LITE_BLEND_NORMAL_LVGL`** (skip `rkg_premultiply` for
  the GPU path *and* change the blend together). Dropping `rkg_premultiply`
  while leaving `SRC_OVER` in place is **not** the fix — that is wrong in the
  opposite direction and yields bright halos instead of dark ones.
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
- For GPU cells the CRC is computed only after `vg_lite_finish` — that
  ordering is what stops the checksum racing the hardware.
  **No cache maintenance is performed, deliberately.** The `imxrt1176` core
  never writes `SCB_CCR`, so the D-cache is never enabled on this part, SDRAM
  is coherent for free, and `arm_dcache_*` are no-ops — a clean/invalidate
  here would be cargo cult. This is the same invariant
  `port/lvgl_mipi_panel.cpp` documents for its flush, and the same forward
  hazard: **if the D-cache is ever enabled over SDRAM, this site and that
  flush become ONE change, not two** — and the failure would be invisible in
  QEMU, which models no cache. A port to rt1062 is exactly that change: the
  `teensy4` core *does* enable the D-cache.

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

**Expect visible tearing on the glass during Phase B** — the GPU composites
rotors directly into the live scanout buffer, so the LCDIFv2 can scan out a
half-composited frame. That is anticipated and is not a defect to chase; the
bench measures render time, and double-buffering it would change the thing
being measured.

**Task 10's second boot doubles as an order/drift control.** Cell order is
fixed, so every cell always runs at the same point in the sweep — which means
thermal drift, SDRAM refresh contention or arena-reuse effects would bias the
same cells the same way in every boot and be invisible in one run. Compare the
two boots' `mfps_med` column: agreement bounds the systematic error on the
ranking, and disagreement is a finding in its own right.

## 10. Risks

- **GPU/CPU ordering** on the shared framebuffer: `vg_lite_finish` before any
  CRC. **Not** a coherency risk on this core — the `imxrt1176` core never
  enables the D-cache (`SCB_CCR` untouched; `arm_dcache_*` are no-ops), so no
  cache maintenance is performed or needed, exactly as
  `port/lvgl_mipi_panel.cpp` documents for its own flush. It *becomes* a real
  risk the day the D-cache is enabled over SDRAM, or on a port to rt1062
  whose `teensy4` core enables it. Known territory from `vglite_lvgl_test`;
  the diagnostic is `AQHiIdle` bit 0 and counted ISRs, not API return codes.
- **LVGL sw arc clamp** for negative start angles — solved once in
  `synthui_knob.cpp`; reuse that fold.
- **vg_lite fill rule** for annuli (non-zero winding, reversed inner arc).
  Note the earlier claim that "wrong winding renders a full disc" was
  **wrong** and has been removed: each ring sector is a single closed
  contour — outer arc, line across, inner arc reversed, close — and the
  reversal is what *closes* the contour, not what punches the hole. Such a
  contour fills identically under non-zero and even-odd. The per-cell CRC
  still covers the geometry; it simply is not covering this.
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

| cell | mfps_med | us_med | us_min | us_max | us_mean | init_us | rotor_bytes | crc |
|---|---|---|---|---|---|---|---|---|
| *(12 rows after the hardware run)* | | | | | | | | |

Rank on `mfps_med` (§5): `us_mean` can be dominated by a single stalled frame,
and `us_max` is what exposes one. Every GPU row must read `gpu_err=0` or its
timing is void.

**The GPU rows are a pipelining LOWER BOUND.** Each timed frame ends in
`vg_lite_finish()`, which serialises the CPU and the GC355 — the CPU waits out
the whole rotor pass having nothing else to do. A shipped widget can overlap
that work with the next frame's software drawing, so a GPU cell that merely
ties its software counterpart here is not thereby a tie in production. Treat
these numbers as "at least this fast", and say so when naming the winner.
