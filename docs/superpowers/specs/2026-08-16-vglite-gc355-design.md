# VGLite / GC355 GPU acceleration — design

Date: 2026-08-16. Status: approved in session, pending implementation.
Motivated by a measurement, not a hunch: see §1.

## 1. Why — the measurement that justifies this

`display/synthui_knob_test` renders one 360 px `synthui_knob` in **~75 ms**
(73.9 / 76.2 ms mean, 76.5 ms worst, three samples) on the real EVKB at
996 MHz. That is a **13 fps ceiling** for a single knob.

Derived facts:
- **0.57 µs per knob-area pixel ≈ 567 cycles/px.** That is the signature of
  LVGL's mask-based software arc/radius rendering: the knob is ~5 overlapping
  masked shapes (face + border, crescent arc, cap, pointer, ticks) and each
  re-touches the same pixels with a fresh mask evaluation.
- **`LV_DEF_REFR_PERIOD 33` is NOT the limiter.** The render is 2.3× slower
  than the refresh period. This was assumed to be a contributor before
  measuring and it is not; raising the period changes nothing.
- **Cost scales with area**, so a 150 px knob ≈ 13 ms (~77 fps). A single
  normal-size knob is fine on the CPU. A panel is not: the 16-knob grid is
  ~208 ms ⇒ ~5 fps.

Method: an `#ifdef FPSBENCH` variant timing `lv_refr_now()` with `micros()`,
built into a separate `build-fpsbench/` so the golden-producing ELF was never
touched. The bench is worth re-landing as a permanent guarded variant
(precedent: `DEPTH_DEMO_565`, `FLIP_DEMO_SINGLE`).

**Separately — flicker is a different defect with a different fix.** The
example uses `lvgl_mipi_panel_create()`, the v1 SINGLE-buffer DIRECT binding
whose own header says "TEARING: v1 accepts it". `lvgl_mipi_panel_create_db()`
(v4/v5 double buffer + vsync flip) already exists and is gated. Double
buffering removes tearing; it does **not** raise 13 fps. Both are worth doing
and they are independent.

## 2. Facts established during design (verified, not assumed)

**The GPU exists.** Vivante **GC355** as GPU2D: RM §53 including §53.4.2
"VGLite Graphics API"; peripheral at **0x4180_0000** (AHB), **IRQ 60**,
`GPU2D_CLK_ROOT` (CLOCK_ROOT68, to 500 MHz, LPCG128). The claim "RT1176 has no
VGLite GPU" in `docs/superpowers/specs/2026-07-27-rt1176-lvgl-design.md` (§3,
§5 table) and `LVGL/VENDORING.md:22` is **false**; a correction task exists.

**A licence-clean driver exists.** `~/Development/gs-vglite_examples_rt1170/common/vglite/`
(NXP, 20 working RT1170 VGLite examples) is **MIT-only**:
`grep -rl "GNU General Public\|GNU Lesser"` returns **zero** files, and
`VGLiteKernel/vg_lite_kernel.h` carries "The MIT License (MIT), Copyright (c)
2014-2020 Vivante Corporation". The dual-licence objection is real but applies
**only to LVGL's bundled copy** (`src/libs/vg_lite_driver/`), whose pruning was
correct and stays.

**LVGL's backend was never pruned.** `lvgl/src/draw/vg_lite/` is intact — 20
`.c` files including `lv_draw_vg_lite_arc.c`, exactly the primitive the knob
needs. Only the driver was removed.

**There is a sanctioned external-driver hook.** `lv_draw_vg_lite_type.h:23-31`:
```c
#if LV_USE_VG_LITE_THORVG      → others/vg_lite_tvg/vg_lite.h
#elif LV_USE_VG_LITE_DRIVER    → libs/vg_lite_driver/inc/vg_lite.h   (pruned)
#else                          → #include <vg_lite.h>                (include path)
#endif
```
With both switches 0, LVGL compiles against whatever `vg_lite.h` is on the
include path. **No LVGL source edit, no un-pruning, firewall untouched.**

**The core driver is OS-agnostic; only the port layer is not.**
`grep -rl "FreeRTOS\|xSemaphore\|vTaskDelay"` over `VGLite/*.c` and
`VGLiteKernel/vg_lite_kernel.c` returns nothing. The FreeRTOS coupling is
confined to two files: `VGLite/rtos/vg_lite_os.c` (27 references) and
`VGLiteKernel/rtos/vg_lite_hal.c` (3). **Those two are the work.**

**LVGL negotiates GPU capabilities at runtime**, so a GC355 feature gap
degrades rather than breaks: `vg_lite_query_feature()` is consulted for
`VG_SCISSOR`, `VG_RADIAL_GRADIENT`, `VG_IM_REPEAT_REFLECT`,
`VG_LINEAR_GRADIENT_EXT`, `VG_24BIT`, `VG_INDEX_ENDIAN`
(`lv_draw_vg_lite.c:141`, `lv_vg_lite_grad.c:168/182/634`,
`lv_vg_lite_utils.c:89/493/520`). Driver header is `VGLITE_HEADER_VERSION 6`,
`VGLITE_VERSION_2_0`.

**Cache coherency is free here.** The `imxrt1176` core never writes `SCB_CCR`,
so CPU/GPU views of memory agree without maintenance — the exact opposite of
the rt1062 D-cache trap that cost a full silicon session. Do **not** copy
rt1062 cache handling into this port.

**QEMU has no GC355 model.** Confirmed by absence; this is what forces §5.

## 3. Scope

Enable LVGL's VGLite draw unit on the GC355 so vector UI (knobs first, the
rest of the SynthUI set after) renders on the GPU, with a software fallback in
the same binary.

**Success criterion (approved):** the **16-knob grid animating at ≥30 fps**
(≤33 ms for all sixteen). Software is ~208 ms, so this needs ~6×. The target
is deliberately the shippable workload — a synth panel — not the 360 px
synthetic worst case.

## 4. Architecture

**4.1 New sibling library `~/Development/VGLite`** — its own repo, MIT, with a
`VENDORING.md` recording provenance (NXP `gs-vglite_examples_rt1170`, the
zero-GPL verification command and its result) and what was deliberately not
taken. Layout mirrors upstream: `VGLite/`, `VGLiteKernel/`, `inc/`, plus:

**4.2 `port/baremetal/` — the crux.** Replaces the two FreeRTOS files:
- **`vg_lite_os.c`** — GPU completion signalling. The GPU2D IRQ (60) sets a
  `volatile` flag; the wait is a **bounded** polled loop that reports a
  timeout rather than hanging. This is the idiom already proven on this board
  by `lvgl_mipi_panel_flip_sync()`'s vsync fence — follow it, including the
  timeout counter as an asserted token.
- **`vg_lite_hal.c`** — contiguous allocation for command and tessellation
  buffers, register base `0x4180_0000`, IRQ attach, and clock enable
  (LPCG128 + `GPU2D_CLK_ROOT`). No MMU: the GC355 on this part is used flat.

**4.3 LVGL wiring** — in `LVGL/port/lv_conf.h`: `LV_USE_DRAW_VG_LITE 1`,
`LV_USE_VG_LITE_DRIVER 0`, `LV_USE_VG_LITE_THORVG 0`; `VGLite/inc` reaches the
include path via the import macro. `LV_VG_LITE_USE_GPU_INIT` decides whether
LVGL calls our init or we init before `lv_init()` — settle by reading
`lv_draw_vg_lite.c`'s init path during implementation, not by guessing.

**4.4 Build** — `import_evkb_vglite()` in `evkb.cmake`, in the
`import_evkb_synthui()` shape (plain STATIC target, PUBLIC include of `inc`,
PRIVATE `teensy_flags`). Declared in the manifest like any sibling.

**4.5 Runtime fallback — one binary, two paths.** If `vg_lite_init()` fails
(or the GPU is absent, as in QEMU) the VGLite draw unit must **not** register,
and LVGL's software unit handles everything. Verify LVGL's draw-unit
registration permits conditional registration — if it does not, the fallback
becomes a compile-time variant and §5's gate story changes, so settle this
**first** in implementation.

## 5. Verification — how the two-gate rule is satisfied (approved)

QEMU cannot model the GC355, so the two paths are verified differently and the
split is documented rather than hidden.

- **QEMU gate → the software path.** The GPU is absent, `vg_lite_init()`
  fails, the fallback engages: `VGLITE_INIT=ABSENT` plus deterministic
  goldens exactly as today. **0 SKIP is preserved** and the gate still proves
  the scene, the plumbing and the fallback.
- **Hardware → the GPU path.** `VGLITE_INIT=OK`, its own recorded goldens
  (`GPU_SUM_*`), and a **measured** `RENDER_US` asserted under threshold.
  Recorded only when stable across two runs **and** confirmed by eye on the
  RK055, in the same commit — the house ritual.
- **`docs/KNOWN-BROKEN-GATES.md`** gains an entry stating precisely what QEMU
  does and does not cover for this example, in the style of the existing
  local-only entries.

★ **The GPU and software paths will NOT produce identical pixels.** Hardware
antialiasing differs from LVGL's masks. Two golden sets, never one; a future
reader must not "fix" a mismatch by copying one over the other. This is the
same discipline as the ILI9341-vs-RPi golden distinction already in the tree.

## 6. Risks, named

1. **Bare-metal port correctness** is the main risk — a missed IRQ or a wrong
   wait turns into a hang, not a wrong pixel. Mitigation: bounded waits with
   an asserted timeout counter, exactly as the vsync fence does.
2. **Tessellation buffer sizing** is a memory/throughput tradeoff
   (`vg_lite_init(w, h)` sets it). Measure at two or three sizes rather than
   picking one; 64 MB SDRAM means memory is not scarce.
3. **GC355 feature gaps** — mitigated by LVGL's runtime `vg_lite_query_feature`
   negotiation (§2); expect degradation, not failure.
4. **Conditional draw-unit registration** (§4.5) — settle before building the
   gate, because it determines whether one binary can serve both paths.
5. **A fresh clone cannot build this** until VGLite is pushed — same
   SKIP-class situation as SynthUI, and it needs the same
   `KNOWN-BROKEN-GATES` treatment.

## 7. Non-goals

- Touch input, and any SynthUI widget beyond the knob.
- Replacing PXP: it stays the blit/CSC/flip engine. The v9 census (fills are
  CPU-won) is about fills and is not re-litigated here.
- A GC355 model in qemu2 — explicitly declined in favour of §5.
- Un-pruning LVGL's bundled driver, or relaxing the licence audit.
- Pushing VGLite or SynthUI to GitHub.

## 8. Verification checklist

1. `LICENSE-AUDIT: PASS` with `$LIB_ROOT/VGLite` added as a REPOS root and the
   new example's GATES entry walked. Mutation-test the root as was done for
   SynthUI (remove it, confirm `OUTSIDE SWEPT ROOTS` fires).
2. QEMU gate green on the software path, `0 SKIP`.
3. Hardware: `VGLITE_INIT=OK`, GPU goldens stable across two runs, eyes on
   glass, `transcript_hw_evkb.txt` committed.
4. **The number**: 16-knob grid ≥30 fps, reported as measured CPU-vs-GPU
   frame times for the same scene.
5. Full sweep re-measured (run it, don't count files) and CLAUDE.md updated.
