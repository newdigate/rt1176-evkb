# GC355 conformance probe — gradients (NEW-32, linear only)

**Status:** approved 2026-09-02. Builds on the Phase 1/1b/2 harness in
`examples/display/vglite_conformance`. One boot, six case lines, pre-registered.

## 1. Why this exists

The quirks doc carries three gradient claims and **not one of them has a
case**. Two come from reading NXP's source; one is a single uncontrolled
sighting during the fader work. The guard layer (Phase 4) deliberately refused
to build gradient helpers on that basis. This phase turns the claims into
measurements — and re-states them precisely, because reading the driver more
carefully than the claim table did changes what the cases must ask.

## 2. What the driver actually does (read, not guessed)

Cited to `~/Development/VGLite`, `grep -n` on the files named.

**EXT API (`vg_lite_ext_linear_gradient_t`, gate
`gcFEATURE_VG_LINEAR_GRADIENT_EXT && gcFEATURE_VG_IM_INPUT`, both 1 on this
chip's `Series/gc355/0x0_1216/vg_lite_options.h`):**

- `vg_lite_set_linear_grad(grad, count, ramp, {X0,Y0,X1,Y1}, spread, premult)`
  stores the line and the float `vg_lite_color_ramp_t` stops. **Named float
  channels — there is no input word order to get wrong.**
- `vg_lite_update_linear_grad(grad)` (`vg_lite.c:7690-7710`) transforms
  `(X0,Y0)-(X1,Y1)` by `grad->matrix`, computes the **screen-space** length,
  then **overwrites `grad->matrix`** with `translate(x0,y0)·rotate·scale(len/width)`
  and **overwrites `grad->linear_grad`** with `(0,0,width,0)`, allocates a
  `width×1` `VG_LITE_ABGR8888` ramp image (`width = stops·128`) **without
  freeing any previous one**, and fills it: bytes in memory order
  **A, B, G, R** (`PackColorComponent` of `color[3],[2],[1],[0]`).
- `vg_lite_draw_linear_grad(target, path, fill, path_matrix, grad, …)`
  (`vg_lite_path.c` from `:4182`) computes the per-pixel gradient parameter
  from **`grad->matrix` alone** — `lg_step_x/y_lin`, `lg_constant_lin` via
  `inverse(&grad->matrix)` — and applies **`path_matrix` only to the path
  geometry**. **The two matrices are never composed.**

So the EXT gradient is defined in **screen space**. Moving the *path* by its
matrix does not move the *gradient*. That is the precise form of "the ramp is
placement-dependent", and it is decidable in pixels.

**Legacy API (`vg_lite_linear_gradient_t`):**

- `vg_lite_init_grad` allocates a `1024×1 VG_LITE_BGRA8888` image.
- `vg_lite_set_grad(grad, count, colors[], stops[])` (`vg_lite.c:8121`): sets
  `grad->count = 0` first; with `count == 0` (or NULL arrays) **returns
  `VG_LITE_SUCCESS` and leaves count 0**. Stops are 0..1023 integers. Colours
  are read by the driver's own `A()/R()/G()/B()` macros
  (`vg_lite_context.h:95-99`) — i.e. **`0xAARRGGBB`, NOT `vg_lite_color_t`'s
  ABGR**. A second word-order trap, in the one API where the caller packs.
- `vg_lite_update_grad` (`vg_lite.c:8167`): if `count == 0` it **substitutes
  black@0 → white@255 and sets count = 2** (the claim, confirmed in source);
  then CPU-fills the 1024-word ramp as `ARGB(a,r,g,b)` words.
- `vg_lite_draw_grad` (`vg_lite_path.c:5739`) is one line:
  `vg_lite_draw_pattern(target, path, fill, matrix, &grad->image, &grad->matrix,
  blend, VG_LITE_PATTERN_PAD, 0, 0, VG_LITE_FILTER_LINEAR)`. **Nothing
  GC255-specific in it.** The "GC255-only" claim rests on NXP's `vglite_layer.c`
  gating it on chip id, and on one sighting of solid black. The caller must
  supply `grad->matrix` mapping the 1024-wide ramp onto the path — a wrong
  matrix (e.g. identity, so 1024 ramp px span a 64 px rect) is a plausible
  alternative explanation for the black. The case sets it correctly:
  `scale(rect_w/1024)` then `translate(rect_x, rect_y)`.

## 3. The cases

All six draw the standard 80×80 rect at (24,24) used by every colour case
(`C_X/C_Y/C_W/C_H`), `BLEND_NONE`, opaque stops, `NON_ZERO`. A left-to-right
ramp **red → blue**: X0 = 24, X1 = 104 (the rect's edges), Y0 = Y1 = 64.

**Shared structural predicate `grad_profile()`** samples row 64 at three
columns — left `x=28`, mid `x=64`, right `x=100` (4 px in from each edge, so
PAD/edge interpolation cannot reach them) — and reports
`l=<r,g,b> m=<r,g,b> r=<r,g,b>` plus:

- `left ≈ red`: `R ≥ 200, B ≤ 55`
- `right ≈ blue`: `B ≥ 200, R ≤ 55`
- `mid is a mix`: `R` and `B` both in `[64, 192]`
- `monotonic`: `R(28) > R(64) > R(100)` and `B(28) < B(64) < B(100)`

Generous on purpose (the Phase 2 policy — narrow after measuring, never
before), and structural: robust to AA, to `/255` vs `/256`, and to either
premultiply reading (stops are opaque). `cover=n/a` throughout.

| Case | `run()` | Pre-registered |
|---|---|---|
| `grad/legacy-linear` | `init_grad; set_grad(2 stops, driver-ARGB words, stops 0 and 1023); matrix = scale(80/1024)·translate(24,24); update_grad; draw_grad`. **`api2=`** field carries a second `set_grad(count=0, NULL, NULL)` on a *separate* object followed by `update_grad`, reporting its return code and the resulting `count`/`colors[0]`/`colors[1]` in `detail=` (`c0n=2,c0k=FF000000,c0w=FFFFFFFF` expected) — the substitution claim measured with no extra draw. | **broken**, repeat **`unstable`** — the only sighting was per-*boot* variation; in-boot behaviour never measured. Reason recorded on the line. |
| `grad/ext-linear-static` | `set_linear_grad(2 stops, X0=24→X1=104); grad->matrix = identity; update; draw_linear_grad(path_matrix = identity)`; `clear_linear_grad` at end. **Bootstrap control** — if this is broken the three cells below say nothing. | ok |
| `grad/ext-linear-moved` | As static, but draw with `path_matrix = translate(+16, 0)` (rect now spans 40..120; ramp still 24..104) and **no** update. Predicate sampled at the **moved** rect's columns (44, 80, 116). | **broken** — left column reads ~20 % into the ramp, right column is PAD blue; `l≈red` fails. Cause: §2, the driver never composes `path_matrix` into the paint. |
| `grad/ext-linear-reupdate` | As moved, then `vg_lite_update_linear_grad` **again on the same object** before drawing. | **broken**, *identical detail to `moved`* — **NOT** the "double transform" the approved design said. By the algebra in §2 a second update re-derives the same screen line from the replaced matrix and `(0,0,width,0)`: it is idempotent (and leaks one ramp image). The move never reaches the gradient. Prediction changed from the verbal design **before the boot**, with this derivation as the reason. |
| `grad/ext-linear-rebuilt` | As moved, then `clear_linear_grad; set_linear_grad(X0=40→X1=120); identity matrix; update`, draw at `translate(+16,0)`. The `two-draws-ring` analogue: the prescribed usage measured beside its counterexample. | ok |
| `grad/ramp-word-order` | EXT, **both** stops pure red (`1,0,0,1`), static placement. Every sampled pixel must read `R=255, G=0, B=0, A=255` (exact — no interpolation between identical stops). Probes whether the hardware's `ABGR8888` sampler reads the ramp in the byte order `update_linear_grad` wrote it (A,B,G,R). | ok |

Every EXT case calls `vg_lite_clear_linear_grad` before returning, so the
repeat run starts from a fresh object and the pool is not left holding ramp
images between cases. The `reupdate` case leaks exactly one 256-px ramp by
construction; that is the finding's cost, printed as `leak=1`.

**What can be refuted.** `moved` coming back **ok** means the hardware composes
the matrices where the driver's own code path does not — a bigger finding than
the prediction, and the quirks row is rewritten with the reason. `reupdate`
coming back **ok** while `moved` is broken means a second update is *not*
idempotent on this hardware and does move the ramp — also worth knowing.
`legacy-linear` coming back **ok** retires the "GC255-only" row and points the
earlier black at the caller's matrix.

## 4. Host suite — `tests/cases_grad_test.cpp`

Compiles the real `vgc_cases_grad.cpp` against `model.h`, which grows:

- **driver entry points modelled from source** — `vg_lite_set_linear_grad`,
  `vg_lite_update_linear_grad` (the overwrite, the screen-space line, the
  A,B,G,R ramp packing; images `malloc`ed and freed by `clear`, a leak counter
  so the `reupdate` case's `leak=1` is checkable), `vg_lite_clear_linear_grad`,
  `vg_lite_draw_linear_grad` (paint parameter from `grad->matrix` only, path
  moved by `path_matrix`), and the legacy trio;
- matrix helpers `vg_lite_identity/translate/scale` (exact, the stub gains the
  declarations);
- the rasteriser's inner loop takes a per-pixel paint callback instead of a
  flat colour, applied through the existing `model_blend` and `mem_word`.

Arms, each a switch in `model.h`:

1. **correct** — all six as pre-registered *except* `legacy-linear` reports ok
   (the model has no GC255 quirk to model; its silicon verdict is the point of
   the boot). Pinned detail strings.
2. **draws nothing** — all six broken.
3. **draws black** — the legacy claim, applied to every gradient draw: all
   six broken with `l=0,0,0`.
4. **paint follows the path** — a GPU that composes `path_matrix` into the
   gradient: `moved` and `reupdate` go **ok** (the prediction inverted, by
   name), everything else unchanged. This is the arm that proves the two
   cells can *see* the question.
5. **solid first stop** — no interpolation: every case except
   `ramp-word-order` broken on `monotonic`; `ramp-word-order` **stays ok**
   (both stops identical — pinned, because it says that case is blind to this
   defect *by design*, not by accident).
6. **R/B-permuting ramp store** — the A,B,G,R packing written A,R,G,B:
   `ramp-word-order` alone goes broken, the others' *verdicts* survive (their
   predicate is direction-agnostic under a red↔blue swap) — pinned, so the
   word-order fault surfaces in exactly one cell, as in Phase 2.

Demonstrate RED before trusting: a case hard-wired to `VGC_OK` must be caught
by arms 2 and 3.

## 5. Gate, checker, docs

- `run_qemu.sh`: count 20 → **26**, six ids added to the by-name loop, summary
  `cases=26 skip=26`. Tripwires unchanged.
- `tools/gate-vacuity.test.sh`: "expected 20 case lines" → 26 in the truncated
  matrix case; refresh `transcript_qemu.txt`.
- `expected_silicon.txt`: six lines under a `GRADIENTS` block, written before
  the press, each `broken`/`unstable` with its reason.
- `docs/gc355-vglite-quirks.md`: the deferred table becomes measured rows.
- Sweep stays 124. `LICENSE-AUDIT` unaffected (no new manifest).

## 6. Safety

Both draws already ran on this silicon once (the fader's legacy attempt; the
EXT path in LVGL's backend) without wedging the front end. If any case hangs,
`vgc_timeouts` names it and the `case_begin`/`case` count says which. Nothing
here touches `build-danger`.
