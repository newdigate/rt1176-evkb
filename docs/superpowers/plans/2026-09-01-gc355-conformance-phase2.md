# GC355 Conformance Phase 2 — Colour & Blend — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Add five colour/blend cases to `examples/display/vglite_conformance` (matrix 15 → 20) so the probe finally tests `VG_LITE_BLEND_SRC_OVER` — the blend mode both shipping compositors use exclusively and which no case has ever exercised — plus a colour predicate layer and the host-suite arms that make those cases trustworthy. Sweep count unchanged at 124.

**Architecture:** Spec: `docs/superpowers/specs/2026-09-01-gc355-conformance-phase2-design.md`. Colour predicates live in a new `vgc_color.h` because `vgc_predicates.h`'s central predicate is explicitly invalid for coloured fills. The word-order bootstrap is solved inside `color/solid-word-order`, which asserts both an order-agnostic identity and that it matches the mapping downstream code assumes — no cross-case state. Colour cases go in their own `vgc_cases_color.cpp`, following the one-file-per-phase pattern the Phase 1 review recommended (and `vgc_cases_path.cpp` is already 1156 lines).

**Tech Stack:** unchanged — the Phase 1 harness, its arena, its four-arm host suite, its gate, its pre-registered expectation and drift checker.

**House rules that bind every task:**
- `./run_qemu.sh`, never `sh run_qemu.sh`.
- **`★` marks a MEASURED fact.** Several `★`s in this project carried wrong mechanisms and had to be corrected from source. Do not write one you have not verified.
- Every new guard must be DEMONSTRATED RED **on its targeted line**, not merely red. A red arriving for another reason proves nothing — that has bitten repeatedly here.
- Fixtures are captured AFTER the gate is final.
- `expected_silicon.txt` is pre-registered BEFORE the boot; corrections carry reasons.
- `VGC_DETAIL_MAX` is 96 and details must contain NO SPACES.

---

## File structure

| File | Responsibility |
|---|---|
| `examples/display/vglite_conformance/vgc_color.h` | **Create.** Pure colour predicates. No vg_lite, no Arduino — host-compilable, like `vgc_predicates.h`. |
| `examples/display/vglite_conformance/tests/color_test.c` | **Create.** Host unit test for the above, with demonstrated mutants. |
| `examples/display/vglite_conformance/vgc_cases_color.cpp` | **Create.** The five colour/blend cases and `vgc_color_cases[]`. |
| `examples/display/vglite_conformance/tests/model.h` | **Create.** The reference rasteriser, EXTRACTED from `cases_path_geom_test.cpp` and extended with colour + `SRC_OVER`, so both host suites share ONE blend implementation. |
| `examples/display/vglite_conformance/tests/cases_color_test.cpp` | **Create.** Host suite for the colour cases: one correct arm plus three negative colour arms. |
| `examples/display/vglite_conformance/vgc_harness.h` | **Modify.** Promote `vgc_cover_t` and `vgc_cover_na()` only (the tolerance helpers stay static — YAGNI); add `vgc_draw_path_blend`, `vgc_clear_to`, `VGC_ABGR_A`; declare the third case table. |
| `examples/display/vglite_conformance/vglite_conformance.cpp` | **Modify.** Implement the promoted helpers; iterate `vgc_color_cases`. |
| `examples/display/vglite_conformance/vgc_cases_path.cpp` | **Modify.** Drop `vgc_cover_t`/`vgc_cover_na()` (now in the harness); keep the tolerance helpers; add the `evenodd` pass-1 coverage. |
| `examples/display/vglite_conformance/tests/cases_path_geom_test.cpp` | **Modify.** Use the extracted model. All 199 checks must still pass. |
| `examples/display/vglite_conformance/tests/run.sh` | **Modify.** Run the two new suites. |
| `examples/display/vglite_conformance/run_qemu.sh` | **Modify.** Count 20; five new ids; anchored summary. |
| `examples/display/vglite_conformance/expected_silicon.txt` | **Modify.** Five pre-registered lines with reasons. |
| `tools/gate-vacuity.test.sh`, `docs/gc355-vglite-quirks.md`, `CLAUDE.md` | **Modify.** Registration and record. |

## Constants used throughout

| name | value | why |
|---|---|---|
| sample point | `(64,64)` | centre of the 80×80 rect at (24,24) — 40 px from any edge, so coverage is exactly 1.0 |
| backdrop grey | `0x40` = 64 | non-zero so `dst*(1-a)` is exercised |
| source alpha | `0x80` = 128 | α = 128/255 ≈ 0.502 |
| case 2 target | 128 | `255 × 128/255` = 128.0 |
| case 3 target | 160 | `255×0.502 + 64×0.498` = 128.0 + 31.9 = **159.9** |
| case 4 target | 207 | `255×0.502 + 159.9×0.498` = 128.0 + 79.6 = **207.6** |

---

### Task 1: Promote the coverage helpers and add the blend/clear primitives

Pure refactor plus three additions. **No behaviour may change**: all 280 host checks and the QEMU gate must be identical afterwards.

**Files:**
- Modify: `examples/display/vglite_conformance/vgc_harness.h`
- Modify: `examples/display/vglite_conformance/vglite_conformance.cpp`
- Modify: `examples/display/vglite_conformance/vgc_cases_path.cpp`

- [ ] **Step 1: Promote `vgc_cover_t` and `vgc_cover_na()` only**

They are `static` in `vgc_cases_path.cpp` (around lines 166-202), so a second case file cannot reach them.

**Promote only the type and `vgc_cover_na()`.** Every Phase 2 case is `cover=n/a` — filled area is not the question a colour case asks — so `vgc_cover_within` / `_axis` / `_aa` are not needed here and stay `static` in the path file. Promoting them now would be building for a Phase 3 that has not been designed.

The reason to promote even this much is not reuse but the **duplicated literal**: without it, the exact string `cover=n/a` would be spelled in two files, and a later change to the field name would have to find both. Move the type and `vgc_cover_na()` into `vgc_harness.h` (declaration) and `vglite_conformance.cpp` (definition), dropping `static`, and **carry the comments across verbatim** — the "it is a RULE, not a per-case exclusion list" note is load-bearing.

Add to `vgc_harness.h`:

```c
/* ---- coverage verdict -----------------------------------------------------
 * A case's pixel= verdict is `structural predicate AND fill within tolerance
 * of the analytic area`, so pixel=ok means THE PICTURE IS RIGHT rather than
 * merely that the structure is. The comparison rides in detail= as
 * cover=ok / cover=stray:N / cover=short:N / cover=n/a.
 *
 * ★ TOLERANCE IS k*PERIMETER, NEVER A PERCENTAGE OF AREA. Rasterisation error
 * lives on the BOUNDARY. A 5% area band would have put a false `broken` on
 * path/self-intersecting -- a CONTROL -- which carries 474 px of all-diagonal
 * boundary on only 2792 px of area. */
typedef struct {
    int  ok;
    char s[24];     /* "cover=stray:" (12) + an int (11) + NUL */
} vgc_cover_t;

/* Not applicable: the case has no analytic area to compare against -- every
 * Phase 2 colour case, and any path case whose structural predicate failed.
 * Never fails a case on its own. The tolerance helpers (vgc_cover_within /
 * _axis / _aa) stay static in vgc_cases_path.cpp: nothing outside it needs
 * them yet. */
vgc_cover_t vgc_cover_na(void);
```

- [ ] **Step 2: Add the alpha-carrying colour macro and a clear-to-colour**

In `vgc_harness.h`, beside `VGC_ABGR`:

```c
/* VGC_ABGR with an explicit alpha. vg_lite_color_t is ABGR (0xAABBGGRR) --
 * red in the LOW byte, alpha in the HIGH byte. Phase 2's blend cases need a
 * non-opaque source; every Phase 1 case was opaque, which is exactly why
 * blend/none-honours-alpha exists. */
#define VGC_ABGR_A(a, r, g, b) (((uint32_t)(a) << 24) | ((uint32_t)(b) << 16) | \
                                ((uint32_t)(g) << 8) | (uint32_t)(r))

/* Clear the scratch to an arbitrary colour and finish. vgc_clear() is this
 * with VGC_BG_COLOR; the blend cases need a NON-ZERO backdrop, because
 * SRC_OVER over black is degenerate (dst*(1-a) vanishes) and cannot
 * distinguish a correct blend from one that ignores the destination. */
vg_lite_error_t vgc_clear_to(uint32_t abgr);
```

In `vglite_conformance.cpp`, reimplement `vgc_clear` in terms of it:

```c
vg_lite_error_t vgc_clear_to(uint32_t abgr)
{
    const vg_lite_error_t e = vg_lite_clear(&vgc_scratch, NULL, abgr);
    if (e != VG_LITE_SUCCESS) return e;
    return vg_lite_finish();
}

vg_lite_error_t vgc_clear(void) { return vgc_clear_to(VGC_BG_COLOR); }
```

- [ ] **Step 3: Add the blend-selecting draw**

`vgc_draw_path` currently hardcodes `VG_LITE_BLEND_NONE`. Do NOT change its signature — ~20 Phase 1 call sites use it and their results are measured. Add a variant and make the existing one delegate, so there is ONE implementation of the status-accumulation contract:

In `vgc_harness.h`:

```c
/* Draw with an explicit blend mode. vgc_draw_path() is this with
 * VG_LITE_BLEND_NONE -- which is what all fifteen Phase 1 cases use, and what
 * NO shipping code uses: both compositors use SRC_OVER exclusively. That gap
 * is why Phase 2 exists. */
void vgc_draw_path_blend(vg_lite_path_t *p, vg_lite_fill_t rule, uint32_t color,
                         vg_lite_blend_t blend, vg_lite_error_t *acc);
```

In `vglite_conformance.cpp`:

```c
void vgc_draw_path_blend(vg_lite_path_t *p, vg_lite_fill_t rule, uint32_t color,
                         vg_lite_blend_t blend, vg_lite_error_t *acc)
{
    const vg_lite_error_t e = vg_lite_draw(&vgc_scratch, p, rule, vgc_ident(),
                                           blend, color);
    if (e != VG_LITE_SUCCESS && *acc == VG_LITE_SUCCESS) *acc = e;
}

void vgc_draw_path(vg_lite_path_t *p, vg_lite_fill_t rule, uint32_t color,
                   vg_lite_error_t *acc)
{
    vgc_draw_path_blend(p, rule, color, VG_LITE_BLEND_NONE, acc);
}
```

- [ ] **Step 4: Declare the third case table**

In `vgc_harness.h`, beside the existing two:

```c
/* Defined in vgc_cases_color.cpp. Runs AFTER the path cases: if basic filling
 * is broken, no colour verdict below it means anything. */
extern const vgc_case_t vgc_color_cases[];
extern const size_t     vgc_color_case_count;
```

- [ ] **Step 5: Verify nothing changed**

```bash
cd examples/display/vglite_conformance && cmake --build build
./tests/run.sh
./run_qemu.sh
```

Expected: `predicates_test: OK (39 checks)`, `arena_test: OK (42 checks)`, `cases_path_geom_test: OK (199 checks)`, `run.sh: OK (3 suites)` — **280 total, unchanged** — and `PASS: VGLite conformance harness negative verified`.

The build will fail to link until `vgc_color_cases` exists. Add a temporary placeholder in `vgc_cases_color.cpp` for this task only:

```cpp
/* vgc_cases_color.cpp - colour and blend cases (Phase 2 spec section 3).
 * Copyright (c) 2026 Nicholas Newdigate
 * SPDX-License-Identifier: MIT */
#include "vgc_harness.h"

const vgc_case_t vgc_color_cases[] = { { NULL, NULL, NULL, NULL } };
const size_t     vgc_color_case_count = 0;
```

and add it plus the run-loop line to `CMakeLists.txt` / `setup()`:

```c
    for (size_t i = 0; i < vgc_color_case_count; i++)     run_case(&vgc_color_cases[i]);
```

placed AFTER the path loop and BEFORE the dangerous loop.

- [ ] **Step 6: Commit**

```bash
git add examples/display/vglite_conformance/
git commit -m "NEW-32 Phase 2: promote coverage helpers, add blend/clear primitives"
```

---

### Task 2: `vgc_color.h` and its host unit test

**Files:**
- Create: `examples/display/vglite_conformance/vgc_color.h`
- Create: `examples/display/vglite_conformance/tests/color_test.c`
- Modify: `examples/display/vglite_conformance/tests/run.sh`

- [ ] **Step 1: Write the failing test**

Create `tests/color_test.c`. Follow `tests/predicates_test.c`'s conventions exactly: a `CHECK(cond)` macro printing `PASS:`/`FAIL: <expr>  (line N)`, a failure flag, **no early abort**, and a `<name>: OK (N checks)` / `FAILED (M of N)` trailer.

```c
/* Host-compiled unit test for the conformance probe's COLOUR predicates.
 * Copyright (c) 2026 Nicholas Newdigate
 * SPDX-License-Identifier: MIT
 *
 * Companion to predicates_test.c, and separate for the same reason vgc_color.h
 * is separate from vgc_predicates.h: vgc_is_filled is a THRESHOLD valid only
 * under the black-background/white-fill convention, and says so. A pure-red
 * fill has green=0 and reads as UNFILLED for every pixel -- an instrument
 * fabricating a defect. These predicates answer a different question and carry
 * a different validity condition. */
#include "../vgc_color.h"
#include <stdio.h>

static int failed = 0, checks = 0;
#define CHECK(c) do { checks++; if (c) printf("PASS: %s\n", #c); \
    else { printf("FAIL: %s  (line %d)\n", #c, __LINE__); failed = 1; } } while (0)

int main(void)
{
    /* --- vgc_ch: the order-agnostic byte accessor ----------------------- */
    CHECK(vgc_ch(0x11223344u, 0) == 0x44);
    CHECK(vgc_ch(0x11223344u, 1) == 0x33);
    CHECK(vgc_ch(0x11223344u, 2) == 0x22);
    CHECK(vgc_ch(0x11223344u, 3) == 0x11);
    /* out of range must be impossible to mistake for a real byte, the way
     * vgc_count_runs_col returns -1 rather than a plausible 0 */
    CHECK(vgc_ch(0x11223344u, 4) == -1);
    CHECK(vgc_ch(0x11223344u, -1) == -1);

    /* --- saturation / zero counts: how case 1 asserts identity without
     * naming a channel -------------------------------------------------- */
    CHECK(vgc_saturated_channels(0x00FF0000u) == 1);
    CHECK(vgc_saturated_channels(0xFFFFFFFFu) == 4);
    CHECK(vgc_saturated_channels(0x00000000u) == 0);
    /* 0xFE is NOT saturated -- the identity test must not accept "nearly" */
    CHECK(vgc_saturated_channels(0x00FE0000u) == 0);
    CHECK(vgc_zero_channels(0x00FF0000u) == 3);
    CHECK(vgc_zero_channels(0x00000000u) == 4);
    CHECK(vgc_zero_channels(0x01010101u) == 0);

    /* the exact shape case 1 asserts: one saturated, three zero */
    CHECK(vgc_saturated_channels(0xFF0000FFu) == 2);   /* alpha + red: NOT the shape */

    /* --- vgc_near ------------------------------------------------------- */
    CHECK(vgc_near(128, 128, 0) == 1);
    CHECK(vgc_near(124, 128, 4) == 1);
    CHECK(vgc_near(132, 128, 4) == 1);
    CHECK(vgc_near(123, 128, 4) == 0);
    CHECK(vgc_near(133, 128, 4) == 0);
    /* symmetric: a tolerance that only worked one way would let a whole class
     * of undershoot through */
    CHECK(vgc_near(128, 124, 4) == 1);
    CHECK(vgc_near(128, 133, 4) == 0);

    printf("--\n%s: %s (%d checks)\n", "color_test",
           failed ? "FAILED" : "OK", checks);
    return failed;
}
```

- [ ] **Step 2: Run it and watch it fail**

Run: `./examples/display/vglite_conformance/tests/run.sh`
Expected: compile error — `fatal error: '../vgc_color.h' file not found`.

- [ ] **Step 3: Write `vgc_color.h`**

```c
/* vgc_color.h - PURE colour predicates for the GC355 conformance probe.
 * Copyright (c) 2026 Nicholas Newdigate
 * SPDX-License-Identifier: MIT
 *
 * Spec: docs/superpowers/specs/2026-09-01-gc355-conformance-phase2-design.md
 *
 * SEPARATE FROM vgc_predicates.h ON PURPOSE. That file's central predicate,
 * vgc_is_filled, is a THRESHOLD on the green channel valid ONLY under the
 * black-background/white-fill convention, and its own header says so: a
 * pure-red fill has green=0 and reads as UNFILLED for every pixel. Different
 * question, different validity condition, different file. */
#ifndef VGC_COLOR_H
#define VGC_COLOR_H

#include <stdint.h>

/* Byte `i` of a memory word, i in [0,3], 0 = least significant.
 *
 * ★ THE ORDER-AGNOSTIC ACCESSOR EVERYTHING ELSE IS BUILT FROM. It deliberately
 * says nothing about which byte is red: that is what color/solid-word-order
 * measures, and a predicate that assumed it would make every Phase 2 answer
 * depend on an unsettled Phase 2 fact.
 *
 * Returns -1 out of range rather than a plausible byte value -- the same rule
 * vgc_count_runs_col follows, so an instrument bug can never be spelled as a
 * real measurement. */
static inline int vgc_ch(uint32_t px, int i)
{
    if (i < 0 || i > 3) return -1;
    return (int)((px >> (i * 8)) & 0xFFu);
}

/* How many of the four bytes are exactly 0xFF. Used by color/solid-word-order
 * to assert "exactly one channel is saturated" WITHOUT naming which -- the
 * half of that case's verdict that is valid before the word order is known.
 * 0xFE does not count: the identity test must not accept "nearly". */
static inline int vgc_saturated_channels(uint32_t px)
{
    int n = 0;
    for (int i = 0; i < 4; i++) if (vgc_ch(px, i) == 0xFF) n++;
    return n;
}

/* How many of the four bytes are exactly 0x00. */
static inline int vgc_zero_channels(uint32_t px)
{
    int n = 0;
    for (int i = 0; i < 4; i++) if (vgc_ch(px, i) == 0x00) n++;
    return n;
}

/* |a - b| <= tol, symmetric. Every blend tolerance is spelled at its call site
 * rather than hidden in a helper, so the number a case depends on is visible
 * in the case. */
static inline int vgc_near(int a, int b, int tol)
{
    const int d = a - b;
    return (d >= -tol && d <= tol);
}

/* ---- named channel indices ------------------------------------------------
 * ★ JUSTIFIED BY color/solid-word-order, WHICH ASSERTS THIS MAPPING.
 *
 * The scratch is VG_LITE_BGRA8888, whose memory words are ARGB: red in bits
 * 23:16, i.e. byte 2. (vg_lite_color_t is the OTHER order, ABGR -- red in the
 * low byte -- which is why VGC_ABGR exists. Getting the two confused does not
 * fail, it renders the wrong colour while every status says success;
 * vglite_probe measured that.)
 *
 * These are a compile-time ASSUMPTION, and color/solid-word-order exists to
 * check it: that case fills pure red and asserts both that exactly one channel
 * is saturated AND that it is VGC_R. If that case is broken, every colour
 * verdict below it is suspect -- the role path/single-contour-rect plays for
 * geometry. Do not use these without reading that case's result first. */
#define VGC_B 0
#define VGC_G 1
#define VGC_R 2
#define VGC_A 3

#endif /* VGC_COLOR_H */
```

- [ ] **Step 4: Add the suite to `tests/run.sh` and run it**

Extend `run.sh` to build and run `color_test.c` alongside the others, keeping the per-suite trailer and the derived suite count.

Run: `./examples/display/vglite_conformance/tests/run.sh`
Expected: `color_test: OK (21 checks)` plus the three existing suites; `run.sh: OK (4 suites)`.

- [ ] **Step 5: Demonstrate three mutants RED, each on its targeted line**

| mutant | must fail |
|---|---|
| `vgc_ch` drops the range guard (`return (px >> (i*8)) & 0xFF;`) | the two out-of-range checks |
| `vgc_saturated_channels` accepts `>= 0xFE` | the `0x00FE0000u == 0` check |
| `vgc_near` becomes one-sided (`return d <= tol;`) | the `vgc_near(128, 133, 4) == 0` check |

Apply each, run, confirm the failing line is the one targeted, revert, confirm green. Paste the actual output.

- [ ] **Step 6: Commit**

```bash
git add examples/display/vglite_conformance/vgc_color.h \
        examples/display/vglite_conformance/tests/color_test.c \
        examples/display/vglite_conformance/tests/run.sh
git commit -m "NEW-32 Phase 2: colour predicates + host unit test"
```

---

### Task 3: Extract the reference rasteriser and teach it SRC_OVER

**Files:**
- Create: `examples/display/vglite_conformance/tests/model.h`
- Modify: `examples/display/vglite_conformance/tests/cases_path_geom_test.cpp`

- [ ] **Step 1: Extract**

Move the model rasteriser out of `cases_path_geom_test.cpp` (its `vgc_draw_path`, `vgc_finish_into`, `vgc_ident`, `vgc_clear`, `vgc_fb`, `vgc_px`, `vgc_scratch_sum`, the path parser, the arm-selecting globals) into `tests/model.h`. Both host suites then share **one** implementation.

**Why extraction rather than a second copy:** Task 5 needs a `SRC_OVER` implementation. Two implementations of the same blend formula in two test files will silently diverge — the identical argument that promoted `vgc_draw_path` and `vgc_fb` into the harness during Phase 1, where a case-local `fb()` would already have disagreed with the harness on the engine-absent path.

- [ ] **Step 2: Verify the extraction changed nothing**

Run: `./examples/display/vglite_conformance/tests/run.sh`
Expected: `cases_path_geom_test: OK (199 checks)` — **the same 199**, all four arms, unchanged. If the count moved, something was lost in the move; find it rather than adjusting the number.

- [ ] **Step 3: Teach the model colour and SRC_OVER**

The model currently writes `color` directly. Give it a blend parameter and implement both modes:

```c
/* The reference blend. SRC_OVER is `src*a + dst*(1-a)` per channel, with a
 * taken from the source colour's alpha byte scaled 1/255.
 *
 * ★ THIS IS ONE OF TWO INDEPENDENT DERIVATIONS, DELIBERATELY. If the model
 * implemented the same formula the case expects and nothing else, arm 1 would
 * prove only that the predicate reads what the model wrote -- circular. Every
 * expected value in vgc_cases_color.cpp is ALSO derived by hand in
 * expected_silicon.txt, and the two must agree. That is the discipline that
 * validated the pentagram in Phase 1b (analytic 2792.30 vs model 2792). */
static uint32_t model_blend(uint32_t src, uint32_t dst, vg_lite_blend_t mode)
{
    if (mode == VG_LITE_BLEND_NONE) return src;
    /* SRC_OVER */
    const int a = (int)((src >> 24) & 0xFFu);
    uint32_t out = 0;
    for (int i = 0; i < 4; i++) {
        const int s = (int)((src >> (i * 8)) & 0xFFu);
        const int d = (int)((dst >> (i * 8)) & 0xFFu);
        const int v = (s * a + d * (255 - a) + 127) / 255;
        out |= ((uint32_t)v) << (i * 8);
    }
    return out;
}
```

**Note the `+ 127` rounding.** State in a comment that this is round-to-nearest and that whether the hardware rounds the same way is exactly what the generous tolerances in `vgc_cases_color.cpp` exist to absorb — the model must not be treated as defining the correct answer to within 1 LSB.

The model's `vgc_draw_path` keeps its signature and delegates; add `vgc_draw_path_blend` to match the harness.

- [ ] **Step 4: Verify again**

Run: `./examples/display/vglite_conformance/tests/run.sh`
Expected: still `cases_path_geom_test: OK (199 checks)` — every Phase 1 case draws with `BLEND_NONE`, which returns `src` unchanged, so nothing may move.

- [ ] **Step 5: Commit**

```bash
git add examples/display/vglite_conformance/tests/
git commit -m "NEW-32 Phase 2: extract the reference rasteriser, teach it SRC_OVER"
```

---

### Task 4: The five colour/blend cases

**Files:**
- Modify: `examples/display/vglite_conformance/vgc_cases_color.cpp` (replace the placeholder)

- [ ] **Step 1: Write the case file**

```cpp
/* vgc_cases_color.cpp - colour and blend cases.
 * Copyright (c) 2026 Nicholas Newdigate
 * SPDX-License-Identifier: MIT
 *
 * Spec: docs/superpowers/specs/2026-09-01-gc355-conformance-phase2-design.md
 *
 * ★ WHY THIS PHASE EXISTS. All fifteen Phase 1 cases render with
 * VG_LITE_BLEND_NONE. Both shipping compositors use VG_LITE_BLEND_SRC_OVER --
 * exclusively, twelve call sites. So the matrix had never once tested the
 * blend mode production uses.
 *
 * ★ SAMPLING. Every case here reads ONE SOLID INTERIOR PIXEL at (64,64), the
 * centre of an 80x80 rect at (24,24) -- 40 px from any edge, so coverage is
 * exactly 1.0 and cannot confound the alpha term. cover= is n/a throughout:
 * filled AREA is not the question these cases ask.
 *
 * ★ CASES 2-4 USE GREYSCALE ON PURPOSE. White and grey are channel-symmetric,
 * so a channel permutation is INVISIBLE to them. That is the intent: a
 * word-order fault then surfaces in EXACTLY ONE PLACE, case 1, instead of
 * reddening four cases at once with no obvious first cause. Asymmetric colours
 * would make each case an independent word-order check at the cost of
 * diagnosis; this tree already settled that trade with
 * path/single-contour-rect. */
#include "vgc_harness.h"
#include "vgc_color.h"
#include <stdio.h>
#include <string.h>

#define C_X 24
#define C_Y 24
#define C_W 80
#define C_H 80
#define C_SX 64      /* sample point: centre, 40 px from every edge */
#define C_SY 64

#define C_BACKDROP_GREY 0x40    /* 64 -- NON-ZERO so dst*(1-a) is exercised */
#define C_SRC_ALPHA     0x80    /* 128/255 = 0.502 */

/* Emit the standard 80x80 rect and hand back an inited path. */
static vg_lite_error_t c_rect_path(vg_lite_path_t *p)
{
    vgc_emit_rect_cw(C_X, C_Y, C_W, C_H);
    return vgc_finish_path(p, C_X, C_Y, C_X + C_W, C_Y + C_H);
}

/* ---- 1. color/solid-word-order --------------------------------------------
 * ★★ THE BOOTSTRAP CONTROL, AND IT SOLVES A CIRCULARITY.
 *
 * Writing colour predicates requires knowing the memory word order. The word
 * order is what this phase measures. This case breaks the circle by asserting
 * TWO things, only the first of which needs the answer:
 *
 *   1. exactly one channel saturated and the rest zero -- ORDER-AGNOSTIC,
 *      valid without knowing which channel it is; and
 *   2. that channel is VGC_R, the mapping every downstream predicate assumes.
 *
 * So one case both MEASURES the identity and VALIDATES the assumption the rest
 * of the phase rests on, with no cross-case state: the guard is inside the
 * case rather than in a global. (A runtime-resolved channel map was considered
 * and rejected -- it would reintroduce exactly the neighbour-reading state
 * Phase 1 deleted s_fmt_fill to remove, and make results depend on table
 * order.)
 *
 * If this is broken, EVERY colour verdict below it is suspect -- the role
 * path/single-contour-rect plays for geometry.
 *
 * Opaque and BLEND_NONE, so nothing about blending can confound it. */
static vg_lite_error_t run_word_order(void)
{
    vg_lite_error_t acc = VG_LITE_SUCCESS;
    vg_lite_path_t p;
    const vg_lite_error_t fe = c_rect_path(&p);
    if (fe != VG_LITE_SUCCESS) return fe;
    vgc_draw_path(&p, VG_LITE_FILL_NON_ZERO, VGC_ABGR(0xFF, 0x00, 0x00), &acc);
    vgc_finish_into(&acc);
    return acc;
}

static vgc_verdict_t check_word_order(char *d, size_t n)
{
    const uint32_t px = vgc_px(C_SX, C_SY);
    const int sat  = vgc_saturated_channels(px);
    const int zero = vgc_zero_channels(px);
    /* alpha is opaque here, so the shape is: TWO saturated (red + alpha) and
     * TWO zero (green, blue). Reported as the raw word too, because if this
     * case is wrong the raw value is the only thing worth reading. */
    const int is_red = (vgc_ch(px, VGC_R) == 0xFF);
    snprintf(d, n, "px=0x%08lX,sat=%d,zero=%d,red_at_%d=%d,cover=n/a",
             (unsigned long)px, sat, zero, VGC_R, is_red);
    return (sat == 2 && zero == 2 && is_red) ? VGC_OK : VGC_BROKEN;
}

/* ---- 2. color/premultiplied-srcover ---------------------------------------
 * White at alpha 0x80 over BLACK, SRC_OVER. Exercises the src*a term alone.
 *   ~128 = correct       (255 * 128/255)
 *   ~64  = double-premultiply (the source premultiplied, then multiplied again)
 *   255  = alpha ignored entirely
 * Three outcomes, three different defects, one number. */
#define C2_TARGET 128
#define C2_TOL    4      /* covers /255 vs /256 scaling and either rounding way */

static vg_lite_error_t run_premul_srcover(void)
{
    vg_lite_error_t acc = VG_LITE_SUCCESS;
    vg_lite_path_t p;
    const vg_lite_error_t ce = vgc_clear_to(VGC_ABGR(0x00, 0x00, 0x00));
    if (ce != VG_LITE_SUCCESS) return ce;
    const vg_lite_error_t fe = c_rect_path(&p);
    if (fe != VG_LITE_SUCCESS) return fe;
    vgc_draw_path_blend(&p, VG_LITE_FILL_NON_ZERO,
                        VGC_ABGR_A(C_SRC_ALPHA, 0xFF, 0xFF, 0xFF),
                        VG_LITE_BLEND_SRC_OVER, &acc);
    vgc_finish_into(&acc);
    return acc;
}

static vgc_verdict_t check_premul_srcover(char *d, size_t n)
{
    const int g = vgc_ch(vgc_px(C_SX, C_SY), VGC_G);
    snprintf(d, n, "g=%d,expect=%d,tol=%d,cover=n/a", g, C2_TARGET, C2_TOL);
    return vgc_near(g, C2_TARGET, C2_TOL) ? VGC_OK : VGC_BROKEN;
}

/* ---- 3. blend/srcover-arithmetic ------------------------------------------
 * White at alpha 0x80 over GREY 0x40, SRC_OVER. The backdrop is non-zero, so
 * BOTH terms of src*a + dst*(1-a) are exercised -- case 2 over black cannot
 * distinguish a correct blend from one that ignores the destination.
 *   255*0.502 + 64*0.498 = 128.0 + 31.9 = 159.9 */
#define C3_TARGET 160
#define C3_TOL    4

static vg_lite_error_t c3_draw(int times, vg_lite_error_t *acc)
{
    const vg_lite_error_t ce =
        vgc_clear_to(VGC_ABGR(C_BACKDROP_GREY, C_BACKDROP_GREY, C_BACKDROP_GREY));
    if (ce != VG_LITE_SUCCESS) return ce;
    for (int i = 0; i < times; i++) {
        vgc_arena_reset();
        vg_lite_path_t p;
        const vg_lite_error_t fe = c_rect_path(&p);
        if (fe != VG_LITE_SUCCESS) return fe;
        vgc_draw_path_blend(&p, VG_LITE_FILL_NON_ZERO,
                            VGC_ABGR_A(C_SRC_ALPHA, 0xFF, 0xFF, 0xFF),
                            VG_LITE_BLEND_SRC_OVER, acc);
        vgc_finish_into(acc);
    }
    return VG_LITE_SUCCESS;
}

static vg_lite_error_t run_srcover_arith(void)
{
    vg_lite_error_t acc = VG_LITE_SUCCESS;
    const vg_lite_error_t e = c3_draw(1, &acc);
    return e != VG_LITE_SUCCESS ? e : acc;
}

static vgc_verdict_t check_srcover_arith(char *d, size_t n)
{
    const int g = vgc_ch(vgc_px(C_SX, C_SY), VGC_G);
    snprintf(d, n, "g=%d,expect=%d,tol=%d,cover=n/a", g, C3_TARGET, C3_TOL);
    return vgc_near(g, C3_TARGET, C3_TOL) ? VGC_OK : VGC_BROKEN;
}

/* ---- 4. blend/srcover-double ----------------------------------------------
 * ★ THIS RETIRES A MISLEADING QUIRK-TABLE ENTRY RATHER THAN CONFIRMING IT.
 *
 * The Phase 1 spec lists "SRC_OVER of AA paths is not idempotent --
 * double-composited edges drift". That is arithmetically CORRECT compositing,
 * true of every conforming implementation: a 50% term composited twice gives
 * 0.75s + 0.25d. A case confirming "twice != once" would confirm nothing about
 * this GPU.
 *
 * So this case does not test idempotence. It tests whether the SECOND
 * composite lands where the SAME FORMULA predicts:
 *   255*0.502 + 159.9*0.498 = 128.0 + 79.6 = 207.6
 * Tolerance is wider than case 3's because error compounds through two
 * composites. */
#define C4_TARGET 207
#define C4_TOL    6

static vg_lite_error_t run_srcover_double(void)
{
    vg_lite_error_t acc = VG_LITE_SUCCESS;
    const vg_lite_error_t e = c3_draw(2, &acc);
    return e != VG_LITE_SUCCESS ? e : acc;
}

static vgc_verdict_t check_srcover_double(char *d, size_t n)
{
    const int g = vgc_ch(vgc_px(C_SX, C_SY), VGC_G);
    snprintf(d, n, "g=%d,expect=%d,tol=%d,cover=n/a", g, C4_TARGET, C4_TOL);
    return vgc_near(g, C4_TARGET, C4_TOL) ? VGC_OK : VGC_BROKEN;
}

/* ---- 5. blend/none-honours-alpha ------------------------------------------
 * ★ RECORDED, NOT JUDGED -- and it earns its place for a reason that only
 * appeared while scoping this phase: ALL FIFTEEN PHASE 1 CASES USE BLEND_NONE
 * WITH AN OPAQUE COLOUR. If BLEND_NONE silently honours alpha we have never
 * seen it, and it changes how every Phase 1 result should be read the moment
 * anyone passes a non-opaque colour through it.
 *
 * Both readings are defensible, so BOTH are ok:
 *   255  -- "no blend" means dst = src: the source colour is written raw and
 *           its alpha byte lands in the target's alpha channel but modulates
 *           nothing. This is the conventional reading and the one every Phase
 *           1 case implicitly relied on.
 *   ~128 -- the rasteriser always modulates by alpha and BLEND_NONE only drops
 *           the DESTINATION term, giving dst = src*a = 255*0.502 = 128.
 *           Note this is distinguishable from SRC_OVER, which over this grey
 *           backdrop would give 160 -- so a reading of ~160 would mean
 *           BLEND_NONE is silently doing SRC_OVER, a third model and a real
 *           finding.
 * ANY THIRD VALUE is broken: it would mean BLEND_NONE is doing something
 * neither model predicts, which IS a finding.
 *
 * The Phase 1 results are unaffected either way -- every one of them used an
 * opaque colour -- but the DOCUMENTATION of what BLEND_NONE does would need
 * correcting. */
#define C5_A_TARGET 128     /* alpha honoured, destination discarded */
#define C5_B_TARGET 255     /* alpha ignored */
#define C5_TOL      4

static vg_lite_error_t run_none_alpha(void)
{
    vg_lite_error_t acc = VG_LITE_SUCCESS;
    vg_lite_path_t p;
    const vg_lite_error_t ce =
        vgc_clear_to(VGC_ABGR(C_BACKDROP_GREY, C_BACKDROP_GREY, C_BACKDROP_GREY));
    if (ce != VG_LITE_SUCCESS) return ce;
    const vg_lite_error_t fe = c_rect_path(&p);
    if (fe != VG_LITE_SUCCESS) return fe;
    vgc_draw_path(&p, VG_LITE_FILL_NON_ZERO,
                  VGC_ABGR_A(C_SRC_ALPHA, 0xFF, 0xFF, 0xFF), &acc);
    vgc_finish_into(&acc);
    return acc;
}

static vgc_verdict_t check_none_alpha(char *d, size_t n)
{
    const int g = vgc_ch(vgc_px(C_SX, C_SY), VGC_G);
    const int honoured = vgc_near(g, C5_A_TARGET, C5_TOL);
    const int ignored  = vgc_near(g, C5_B_TARGET, C5_TOL);
    snprintf(d, n, "g=%d,honoured=%d,ignored=%d,cover=n/a", g, honoured, ignored);
    return (honoured || ignored) ? VGC_OK : VGC_BROKEN;
}

/* ---- the table ------------------------------------------------------------
 * ORDER IS PART OF THE INSTRUMENT: color/solid-word-order is FIRST because
 * every case below it reads a named channel, and it is the case that JUSTIFIES
 * the naming. Read its result before any of theirs. */
const vgc_case_t vgc_color_cases[] = {
    { "color/solid-word-order",       run_word_order,      check_word_order,      NULL },
    { "color/premultiplied-srcover",  run_premul_srcover,  check_premul_srcover,  NULL },
    { "blend/srcover-arithmetic",     run_srcover_arith,   check_srcover_arith,   NULL },
    { "blend/srcover-double",         run_srcover_double,  check_srcover_double,  NULL },
    { "blend/none-honours-alpha",     run_none_alpha,      check_none_alpha,      NULL },
};
const size_t vgc_color_case_count =
    sizeof(vgc_color_cases) / sizeof(vgc_color_cases[0]);
```

★ **Case 5 admits exactly two values and rejects a third that matters.** 255 (src written raw) and ~128 (`src*a`, destination dropped) are both defensible readings of "no blend". **~160 is NOT admissible** — that is what `SRC_OVER` gives over this backdrop, so it would mean `BLEND_NONE` is silently blending, which is a real finding and must read as `broken`. The `C5_TOL` of 4 keeps the 128 and 160 bands 28 apart, so they cannot be confused.

- [ ] **Step 2: Build and run under QEMU**

```bash
cd examples/display/vglite_conformance && cmake --build build
timeout 25 ../../../tools/qrun -M mimxrt1170-evk -global fsl-imxrt1170.boot-xip=on \
  -kernel build/vglite_conformance.elf -display none -serial file:/tmp/p2.uart
grep -ac "^vgc case=" /tmp/p2.uart      # expect 20
grep -a "^vgc_summary" /tmp/p2.uart     # expect cases=20 ... skip=20
```

- [ ] **Step 3: Check detail lengths**

Compute the worst case for all five details against `VGC_DETAIL_MAX` (96, 95 usable). Case 1's is the longest — `px=0x%08lX` alone is 18 chars. Report the numbers. If any exceeds, shorten KEY names, never raise the cap.

- [ ] **Step 4: Commit**

```bash
git add examples/display/vglite_conformance/vgc_cases_color.cpp
git commit -m "NEW-32 Phase 2: the five colour and blend cases"
```

---

### Task 5: The colour host suite and its three negative arms

**Files:**
- Create: `examples/display/vglite_conformance/tests/cases_color_test.cpp`
- Modify: `examples/display/vglite_conformance/tests/run.sh`

- [ ] **Step 1: Write the suite**

Compile the REAL `run()`/`check()` functions from `vgc_cases_color.cpp` against `tests/model.h`, in four arms:

| arm | models | must happen |
|---|---|---|
| 1 | a correct GPU (`SRC_OVER` per the formula) | all five cases `ok` |
| 2 | **alpha-ignoring** — writes `src` regardless of α | cases 2, 3, 4 BROKEN **by name**; case 1 stays `ok` |
| 3 | **double-premultiply** — applies α twice | case 2 BROKEN at ≈64, its named failure mode |
| 4 | **channel-permuting** — swaps R and B | **case 1 only** BROKEN; cases 2–5 stay `ok` |

Arm 4 is the assertion of §7's design claim: greyscale makes cases 2–4 blind to a permutation *on purpose*, so a word-order fault surfaces in exactly one place. **Assert it rather than assuming it** — if cases 2–4 also went broken there, the greyscale reasoning would be wrong and the diagnosis argument with it.

Follow `cases_path_geom_test.cpp`'s conventions: `CHECK_CASE`, per-case printed line plus assertions, symptom pins so a case cannot go red for an unrelated reason, and a header block stating plainly that this exercises the cases against MODELS and says **nothing** about real silicon.

- [ ] **Step 2: Run all suites**

Run: `./examples/display/vglite_conformance/tests/run.sh`
Expected: five suites green — `predicates_test`, `arena_test`, `color_test`, `cases_path_geom_test (199)`, `cases_color_test`. Report the new total.

- [ ] **Step 3: Demonstrate the suite catches a hard-wired case**

Hard-wire `check_srcover_arith` to `return VGC_OK;` unconditionally. Confirm **arm 1 stays green** (it would report ok anyway) and **arm 2 catches it by name**. Revert, confirm green, paste the output.

This is the Phase 1 lesson repeated: a positive-only suite is equally consistent with a matrix that cannot detect anything.

- [ ] **Step 4: Commit**

```bash
git add examples/display/vglite_conformance/tests/
git commit -m "NEW-32 Phase 2: colour host suite with three negative arms"
```

---

### Task 6: Close the `evenodd` pass-1 coverage gap

**Files:**
- Modify: `examples/display/vglite_conformance/vgc_cases_path.cpp`

- [ ] **Step 1: Cover pass 1**

`path/evenodd-vs-nonzero` renders twice and only pass 2 (NON_ZERO, solid 80×80 = 6400) is coverage-checked. Pass 1 is the EVEN_ODD ring — **the arm that cuts a hole** — and hole cutting is the leading hypothesis for why nested multi-contour paths mis-cover (`two-contour-ring-nonzero` is `cover=short:769`; `four-nested-rings` is `cover=stray:1115`; both cut holes, while the same-winding pass that does not cut one covers correctly).

Add one file static capturing pass 1's filled count, analytic `80² − 32² = 5376`, axis-aligned tolerance `perimeter/8` where perimeter is `320 + 128 = 448`, so `tol = 56`. Report both in the detail and require BOTH to pass.

- [ ] **Step 2: Detail budget**

That case's detail is already 70 bytes, the longest in the matrix. Adding two fields will exceed 95. **Shorten the existing key names** (`eo_rim`/`eo_centre`/`nz_rim`/`nz_centre` → `eor`/`eoc`/`nzr`/`nzc`) rather than raising `VGC_DETAIL_MAX`. Recount and report.

- [ ] **Step 3: Verify**

Run: `./examples/display/vglite_conformance/tests/run.sh` — arm 1 must still report that case `ok` (the model renders both passes correctly). Then run the QEMU gate.

- [ ] **Step 4: Commit**

```bash
git add examples/display/vglite_conformance/vgc_cases_path.cpp
git commit -m "NEW-32: cover the evenodd EVEN_ODD pass -- the one uncovered hole-cutting arm"
```

---

### Task 7: Registration — gate, expectation, vacuity, docs

**Files:** `run_qemu.sh`, `expected_silicon.txt`, `tools/gate-vacuity.test.sh`, `docs/gc355-vglite-quirks.md`, `CLAUDE.md`

- [ ] **Step 1: `run_qemu.sh`** — `CASES -eq 20`; the five new ids appended to the by-name loop in table order; the anchored summary grep updated to `cases=20 ok=0 broken=0 skip=20`.

- [ ] **Step 2: `expected_silicon.txt`** — five PRE-REGISTERED lines, each with a written reason. Predictions:

| id | verdict | repeat | reason to write |
|---|---|---|---|
| `color/solid-word-order` | `ok` | `same` | The bootstrap control; `vglite_probe` already measured `vg_lite_color_t` as ABGR and the BGRA8888 target's words as ARGB, so `VGC_R = 2` is expected to hold. A `broken` here invalidates every colour verdict below it. |
| `color/premultiplied-srcover` | `ok` | `same` | ≈128 expected. `broken` would name which defect: ≈64 double-premultiply, 255 alpha ignored. |
| `blend/srcover-arithmetic` | `ok` | `same` | ≈160 expected; the first case to exercise both terms of the blend. |
| `blend/srcover-double` | `ok` | `same` | ≈207, derived from the same formula. Confirms the "drift" is arithmetic, not a quirk. |
| `blend/none-honours-alpha` | `ok` | `same` | Both readings admissible; pre-register which we believe (α **ignored**, 255 — every Phase 1 case relied on `BLEND_NONE` writing the colour it was given) so a flip to the other still shows as drift and still needs a reason. |

Add a header note recording that the tolerances are **deliberately generous first** and will be narrowed after the boot, with the reasoning — and that narrowing them is a measurement, not a re-golden.

- [ ] **Step 3: `tools/gate-vacuity.test.sh`** — the truncated-matrix case greps `"expected 15 case lines, got 14"`; update to 20/19. Run the suite; the count should stay 29.

- [ ] **Step 4: Docs** — a `## Colour & blend — Phase 2` section in `docs/gc355-vglite-quirks.md` with the five rows marked **PENDING**, and the existing Phase 2 section's "NOT YET PROBED" list trimmed to just the gradient cases. In `CLAUDE.md`, update the case count and add one line that Phase 2's colour/blend cases are built and awaiting a boot.

- [ ] **Step 5: Verify and re-capture the fixture**

```bash
cd examples/display/vglite_conformance && ./run_qemu.sh && cp build/vglite_conformance.uart transcript_qemu.txt
cd ../../.. && sh tools/gate-vacuity.test.sh | grep -c "^PASS:"     # 29
./tools/run-all-qemu-gates.sh -l | tail -1                          # 124 gate(s)
LICENSE_AUDIT_EVKB=$(pwd) ./tools/license-audit.sh | tail -1        # PASS
```

- [ ] **Step 6: Commit**

```bash
git add -A && git commit -m "NEW-32 Phase 2: register the colour cases (gate 20, pre-registration, docs)"
```

---

### Task 8: One bench boot

- [ ] **Step 1: Flash the `.hex`, not the `.elf`**

`LinkServer flash … load` refuses this example's ELF (`code -11`, 0 bytes written, 4/4 reproducible). Clear stale daemons first — `pkill -f LinkServer; pkill -f redlinkserv; pkill -f crt_emu` — and hold nothing on the VCOM while programming.

```bash
/Applications/LinkServer_26.6.137/LinkServer flash MIMXRT1176:MIMXRT1170-EVKB load   build/vglite_conformance.hex
/Applications/LinkServer_26.6.137/LinkServer flash MIMXRT1176:MIMXRT1170-EVKB verify build/vglite_conformance.hex
```

- [ ] **Step 2: Attach the reader, then ask for ONE press**

The MCU-Link VCOM is the port whose suffix matches the probe serial in LinkServer's own log (`[5DQ2DDHVWO5EI]` → `/dev/cu.usbmodem5DQ2DDHVWO5EI3`). Use a generous timeout and let the reader settle before asking — a window that closes early, or a reader still reconnecting through the reset, costs a press. Captures are classified as binary: **use `grep -a`**.

- [ ] **Step 3: Read case 1 FIRST**

`color/solid-word-order` gates the interpretation of everything below it. If it is broken, stop and diagnose before reading any other colour verdict.

Then diff against the pre-registration:

```bash
./tools/vglite-conformance-check.sh examples/display/vglite_conformance/transcript_hw_evkb.txt
```

**All twenty Phase 1/1b verdicts must be unchanged.** If any moved, that is a bigger finding than the colour results and must be chased first.

- [ ] **Step 4: Second boot**

Required by the standing repeated-boot rule — a defect hid behind exactly one boot in NEW-20, and in this project `four-nested-rings` read `repeat=same` on one boot and `differs` on the next. Diff the two captures; only known-nondeterministic cases may differ.

- [ ] **Step 5: Record**

Update `expected_silicon.txt` (with reasons, never by pasting the transcript), narrow the tolerances to what the hardware actually does — **recorded as a deliberate narrowing with the measurement behind it** — and update the quirks doc, `CLAUDE.md` and NEW-32.

★ If `BLEND_NONE` turns out to honour alpha, say plainly that the Phase 1 results are unaffected (every one used an opaque colour) but the *documentation* of what `BLEND_NONE` does was wrong.

★ If the SRC_OVER cases pass, **retire the "SRC_OVER of AA paths is not idempotent" entry from the quirk table** and say why: the drift is correct compositing, and the case measured the second composite landing where the formula predicts.

---

## Self-review notes

**Spec coverage.** §2 scope → Tasks 4, 6, 7. §3 cases → Task 4. §4 bootstrap → Task 4 case 1. §5 predicate layer → Task 2. §6 sampling and tolerances → Task 4's constants and Task 7 Step 2. §7 greyscale rationale → asserted by Task 5 arm 4, not merely documented. §8 testing → Tasks 3 and 5, including the circularity guard (Task 3 Step 3) and the failing-branch arm requirement (Task 5 Step 3). §9 gate/expectation → Task 7. §10 non-goals: no gradients, no other blend modes, no compositor changes anywhere in this plan.

**Case 5's admissible set is closed, not open.** Two values are `ok` (255 raw, ~128 alpha-modulated) and a third plausible one — ~160, which is `SRC_OVER`'s answer over this backdrop — is deliberately `broken`, because `BLEND_NONE` silently blending would be a finding rather than a defensible reading. The tolerances keep the 128 and 160 bands 28 apart.

**Known gap:** case 5's two admissible outcomes are handled by the case returning `ok` for either, not by the expectation file's `pair:` mechanism — `pair:` is for joint outcomes ACROSS cases and does not apply to one case with two acceptable values. The expectation still pins ONE of them so a flip shows as drift.

---

## As-built — what changed mid-flight, and why

The plan shipped over 16 commits. Five changes were made against the plan's own
text, each because implementation or measurement showed the plan was wrong.

**1. The promotion was narrowed (Task 1).** The plan promoted all four
`vgc_cover_*` helpers out of `vgc_cases_path.cpp`. Every Phase 2 case is
`cover=n/a`, so three of them would have been promoted for an undesigned Phase 3.
Only `vgc_cover_t` and `vgc_cover_na()` moved, and the justification is killing a
duplicated `cover=n/a` literal rather than reuse. `vgc_cover_na()` also became a
`static inline` in the header — a `.cpp` definition would have forced Task 5's
colour suite to link all fifteen path cases to reach one nine-character constant.

**2. The spec contradicted itself, and measurement settled it (Task 4).** §3
admitted reading B as `ok`; §8 required the alpha-ignoring arm to break cases 2–4.
With a *saturated white* source those are mutually exclusive — `S + D*(1-Sa)` is
observationally identical to writing `S` raw. Resolved by judging the **alpha
row** as well (`inc/vg_lite.h:462`, unambiguous under both readings, 255 vs 128),
which is a better check than the one it replaced and covers a part of the
operator nothing else looked at. Case 5 is deliberately excluded — `BLEND_NONE`'s
row is `A: Sa`, so 128 is correct there.

**3. The model had to model the target twice over.** First the ABGR→ARGB swizzle
(Task 3): the model stored `vg_lite_color_t` order where the target stores memory
order, and Task 5's first case would have read the blue byte and reported a
defect that is not there. Then, after the boot, the blend itself: the model
implemented reading A and the silicon does **B**.

**4. ★★ The measured reading is PINNED, and that was a real defect the plan
missed.** `tools/vglite-conformance-check.sh` compares only
`<id> <pixel> <repeat>`. Cases returning `ok` under *either* reading are
**invisible** to it — an SDK re-vendor flipping `SRC_OVER` back would have left
every verdict `ok` and the checker green, which is exactly the "quirk that
silently disappears" the expectation file exists to catch. The plan's own
self-review claimed "the expectation still pins ONE of them so a flip shows as
drift"; it did not. Admitting both readings was right *before* the boot and wrong
to leave standing after it. Two arms now prove the pin: a reading-A model reddens
cases 2 and 3 by name with `model=A`, and a modulating-`BLEND_NONE` model reddens
case 5.

**5. `blend/srcover-double` declines to break under reading A, and that is
correct.** It predicts `v2` from the *measured* `v1`, so it holds under either
operator — its documented reading-agnostic design. Nothing in the tree could test
that claim until the reading-A arm existed; the arm now pins the moved values
(`v1=160,v2=208,pred=208`), which distinguishes "correctly declined" from "not
connected to the blend at all".

★ **One claim had to be narrowed after the fact.** The measurement write-up said
*both* compositors feed non-premultiplied colour to `SRC_OVER`. Only the fader
does — `synthui_rotary_knob_gpu.cpp` has no `abgr_a` and forces `0xFF000000`, so
at α=255 `S + D*(1-Sa)` reduces to `S`, correct under either reading. Inferred
from shared `SRC_OVER` usage without checking each compositor's colour
constructor; checking the constructor is what settles it.
