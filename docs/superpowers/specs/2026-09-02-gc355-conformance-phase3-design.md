# GC355 conformance probe — Phase 3: images, blits & scissor

**Status:** approved 2026-09-02. Builds on the Phase 1/1b/2/gradients harness in
`examples/display/vglite_conformance`. One boot, six case lines, pre-registered.

## 1. Why this exists

The original design (§4) listed six Phase 3 case ids and none was ever
built. Two claims about this GPU have been load-bearing for shipping code
without a case: the **64-byte source-stride rule** (the rotary bench pads its
rotor frames for it) and the **scissor-is-defeated-in-the-fullscreen-regime**
rule (the fader's header warns that `vg_lite_init()` with the panel's own size
defeats per-fader scissoring). Both come from reading source. This phase
measures them, and reading the driver more carefully sharpened both.

## 2. What the driver actually does (read, not guessed)

Cited to `~/Development/VGLite`, `grep -n` on the files named.

**Scissor is two mechanisms, not one.** `vg_lite_set_scissor`
(`vg_lite_image.c:263`) writes only `s_context.scissor[]`, `scissor_set` and
`scissor_dirty` — no register. It is then applied in two different places:

- **right/bottom → hardware.** `set_render_target` (`vg_lite.c:3626`) pushes
  register `0x0A13 = MIN(scissor.right, width) | MIN(scissor.bottom, height) << 16`
  whenever `scissor_set`, in every regime.
- **left/top → the tessellation window only.** In `vg_lite_draw`
  (`vg_lite_path.c:1217-1260`) the scissor clamps `point_min`/`point_max`,
  and the tile loop starts at `point_min`. That whole block is guarded by
  `if (ts_is_fullscreen == 0)`; in the fullscreen regime (`:1208-1215`,
  `dst_align_width <= tess_w && target->height <= tess_h`) `point_min` is
  forced to `(0,0)` and the scissor is never consulted.

So in the fullscreen regime a scissored draw **loses its left and top edges
and keeps its right and bottom**. That is the sharp form of "clipping silently
disabled", and it is decidable per edge.

**The blit stride rule is a driver check.** `vg_lite_blit` (`vg_lite.c:4339`)
calls `srcbuf_align_check` (`:1716`) — whose address checks are compiled out
on this chip (`gcFEATURE_VG_SRC_ADDRESS_64BYTES_ALIGNED 0`,
`_DETAIL_ALIGNED 0`) — and then, under `gcFEATURE_VG_16PIXELS_ALIGNED 1`,
`_check_source_aligned(format, stride)` (`:1383`): `FORMAT_ALIGNMENT(stride, 64)`
for the 32-bpp formats, 32 for 16-bpp, 16 for 8-bpp. A misaligned stride
returns **`VG_LITE_INVALID_ARGUMENT` (1)** before any command is built. So
`blit/stride-unaligned` measures whether that refusal happens and nothing is
drawn — and it is safe in the default build, because no hardware is touched.

**Blit geometry.** The source's four corners are transformed by `matrix`,
bounded, and clamped to the target (`:4428-4520`); an empty result returns
without drawing. Translate-only matrices take the plain path.

## 3. The cases

All blit cases target `vgc_scratch` (128×128 BGRA8888, the 64×64 tessellation
buffer — the shipping multi-tile regime), `BLEND_NONE`, `VG_LITE_FILTER_POINT`,
and a **16×16 two-colour checkerboard** with 4×4 cells: red `(255,0,0)` and
blue `(0,0,255)`, placed by `translate(24,24)`. The predicate `blit_profile()`
samples the centres of cells (0,0), (1,0), (0,1), (1,1) — at (26,26), (30,26),
(26,30), (30,30) — plus one pixel outside the image at (64,64), and reports
`c00=R.G.B,c10=…,c01=…,c11=…,out=R.G.B`.

- a cell is *red* if `R ≥ 248 && G ≤ 7 && B ≤ 7`, *blue* the mirror (the
  tolerance exists for the RGB565 case's 5-bit expansion: `0xF800` → 248 by
  shift, 255 by replication; both are correct);
- **checker**: c00 red, c10 blue, c01 blue, c11 red — *and* `out` untouched
  (black, the harness clear).

| Case | Source | Predicted |
|---|---|---|
| `blit/basic` | 16 px wide, stride **64 B** (natural). | ok |
| `blit/stride-64` | Same image in a **32-px-wide** buffer: data 64 B + 64 B padding per row, stride **128 B**, width still 16. A driver or GPU using the width as the pitch shears every odd row. | ok |
| `blit/stride-unaligned` | 20-px-wide buffer, stride **80 B**, width 16. Verdict is "**defined outcome**": `api` must be an error *and* every sample untouched (black). `api=success` with any paint, or a refusal that painted, is broken. | ok, `api=error:1` |
| `blit/formats` | RGB565 source, stride 32 B (16 px × 2 B; `FORMAT_ALIGNMENT(stride, 32)` satisfied), same checker. Also reports `order=` — which of R/B came back where — because the 16-bit component order is a convention this tree has never measured. **Pre-registered: red in the low 5 bits** (`0x001F`), by the same convention the measured BGRA8888 word follows (first-named component in the low bits). | ok |
| `scissor/basic` | 120×120 rect at (4,4) under `set_scissor(40,40,88,88)`, drawn into `vgc_scratch`. Reports `L,T,R,B` = whether a pixel just *outside* each edge (36,64) (64,36) (91,64) (64,91) stayed black, and `in` = (64,64) painted. All four clipped, interior painted. | ok |
| `scissor/tess-fullscreen` | The same rect and scissor drawn into **`vgc_small`**, a second 64×64 BGRA8888 target (stride 256 → `dst_align_width` 64 ≤ 64, height 64 ≤ 64: the fullscreen regime under the existing tess buffer). Scissor `(20,20,44,44)`; outside samples (16,32) (32,16) (47,32) (32,47); `in` = (32,32). | **broken: `L=0,T=0,R=1,B=1`** — left and top painted past the scissor, right and bottom clipped by `0x0A13` |

Every scissor case ends with `vg_lite_set_scissor(-1,-1,-1,-1)` so no scissor
leaks into the next case (the shipping fader does the same at `:627`). The
`vgc_small` target is cleared by its own `vgc_clear_small()` at the start of
its case, since the harness clears only `vgc_scratch`.

**What can lose.** `tess-fullscreen` reading `L=1,T=1` means the hardware
clips on all four edges regardless — the fader's warning would be retired.
`L=0,T=0,R=0,B=0` would mean the scissor is dead in that regime entirely, a
different and larger finding than the one predicted. `stride-unaligned`
returning `api=success` means the driver check is not what gates this chip.
`formats` reading `order=high` flips a convention. Each gets a written reason.

**Left out, deliberately:** A8/L8 sources (no consumer; their blend path takes
the `color` argument down a special branch) and `vg_lite_scissor_rects` (the
mask-layer scissor, `gcFEATURE_VG_MASK 1`, unused by anything here).

## 4. Harness additions

- `vgc_small` — second render target, 64×64 BGRA8888, EXTMEM, 64-B aligned,
  mapped in `setup()` beside `vgc_scratch`; `vgc_px_small(x,y)` and
  `vgc_clear_small()`. On the host: `s_fb_small`.
- `vgc_draw_path_to(target, p, rule, color, acc)` — `vgc_draw_path_blend`
  with an explicit target. `vgc_draw_path_blend` is untouched (334 checks pin
  it).
- `vgc_blit(source, matrix, filter, acc)` — into `vgc_scratch`, `BLEND_NONE`.
- Blit sources are `EXTMEM`, 64-B aligned, mapped once per buffer with
  `vg_lite_map(VG_LITE_MAP_USER_MEMORY)` (a static flag per buffer — the
  repeat run must not map twice). On the host `vg_lite_map` is a no-op and
  `EXTMEM` is empty.

## 5. Host suite — `tests/cases_blit_test.cpp`

`model.h` grows: `vg_lite_set_scissor` (state), `vg_lite_map` (no-op),
`vgc_draw_path_to` (rasterises into the chosen fb, applying the scissor **as
the driver does per regime**: all four edges when the target is `vgc_scratch`,
right/bottom only when it is `vgc_small`), and `vgc_blit` (the driver's stride
check reproduced from `_check_source_aligned`; then per-pixel sampling through
`inverse(matrix)`, point filter, BGRA8888 straight and RGB565 expanded by
shift with red in the low bits).

Arms:

1. **correct** — all six as pre-registered.
2. **draws nothing** — all six broken *except* `stride-unaligned`, whose
   defined outcome is exactly "nothing drawn" (pinned, like
   `degenerate-zero-area` in the path suite).
3. **ignores the scissor** — `scissor/basic` broken `L=0,T=0,R=0,B=0`,
   `tess-fullscreen` broken with the same four zeros (distinguishes "scissor
   dead" from "left/top lost"), blits untouched.
4. **clips all four in fullscreen** — the fader's warning false:
   `tess-fullscreen` goes **ok**; nothing else moves. The arm that proves the
   case sees the question.
5. **width as pitch** — the GPU walks rows by `width*bpp`: `stride-64`
   broken (sheared), `basic`/`formats`/`unaligned` unchanged (their stride
   equals their pitch, or they are refused first).
6. **alignment check absent** — `stride-unaligned` draws: `api=success` and
   a sheared checker, so it must go broken by name.

Demonstrate RED before trusting: `check_scissor_basic` hard-wired to `OK`
must be caught by arms 2 and 3.

## 6. Gate, checker, docs

- `run_qemu.sh`: count 26 → **32**, six ids by name, summary `cases=32 skip=32`.
- `tools/gate-vacuity.test.sh`: truncated-matrix case 32/31; fixture refreshed.
- `expected_silicon.txt`: six lines under a `PHASE 3` block, before the press.
- `docs/gc355-vglite-quirks.md`: the Phase 3 section becomes measured rows.
- Sweep stays 124.

## 7. Safety

Nothing here can hang the front end: the unaligned blit is refused by the
driver before a command exists, and every other draw is a well-formed path or
a valid blit of a mapped buffer. If anything wedges, `vgc_timeouts` and the
`case_begin`/`case` count name it.
