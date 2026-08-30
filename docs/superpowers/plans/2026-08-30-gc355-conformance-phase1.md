# GC355/VGLite Conformance Probe — Phase 1 Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Build the re-runnable GC355 conformance harness as a new gated example `examples/display/vglite_conformance` (sweep 123 → 124), populate it with the spec's paths/contours/winding cases, gate the honest QEMU negative with an "no case may report ok" tripwire, and land the `docs/gc355-vglite-quirks.md` skeleton plus the `expected_silicon.txt` / `tools/vglite-conformance-check.sh` drift pair.

**Architecture:** Spec: `docs/superpowers/specs/2026-08-30-gc355-conformance-design.md`. A case table of `{id, run, check}` triples runs one at a time into a single 128×128 BGRA8888 EXTMEM scratch buffer (clear → draw → finish → check), each case printing TWO independent verdicts — `api=` (what the driver said) and `pixel=` (what a structural CPU-side predicate found) — plus a `repeat=` field from running every case twice. No panel, no LVGL, no scanout: this example links `cores` + `VGLite` only. Predicates are pure functions over a pixel buffer, unit-tested on the host before they are ever trusted on silicon.

**Tech Stack:** VGLite (`~/Development/VGLite`, local-first, pinned in `evkb.cmake`) with this tree's `port/baremetal`; evkb CMake + QEMU gates; ARM GCC 10; host `cc` for the predicate unit test; LinkServer for the single silicon boot.

**House rules that bind every task:**
- `./run_qemu.sh`, never `sh run_qemu.sh` (it re-execs itself under `gtimeout`).
- Every new guard must be DEMONSTRATED RED by name before it is trusted, and the demonstration quoted in the gate header.
- Fixtures (`transcript_qemu.txt`) are captured AFTER the gate runs, from the gate's own capture (`cp build/vglite_conformance.uart transcript_qemu.txt`) — a committed fixture that drifts from its gate goes stale SILENTLY and only `gate-vacuity.test.sh` can see it.
- `expected_silicon.txt` is written BEFORE the bench run (a pre-registered prediction). If silicon disagrees, the file is corrected WITH a reason line — never rubber-stamped.
- Do not touch `~/Development/VGLite/VGLite/` or `VGLiteKernel/` — vendored NXP source, re-vendor liability (spec §3).
- Nothing in the default build may hang the GPU (spec §6).

---

## File structure

| File | Responsibility |
|---|---|
| `examples/display/vglite_conformance/vgc_predicates.h` | PURE pixel-buffer predicates (fill count, run count, row extent, FNV-1a). No vg_lite, no Arduino — host-compilable. |
| `examples/display/vglite_conformance/tests/predicates_test.c` | Host unit test for the above, against synthetic buffers. |
| `examples/display/vglite_conformance/tests/arena_test.c` | Host unit test for the path arena, against a stubbed `vg_lite_init_path`. Added in Task 2 after review: the arena is the highest-risk unit in the harness and was the only one without a test. |
| `examples/display/vglite_conformance/tests/run.sh` | Builds + runs both host tests with the system `cc`. |
| `examples/display/vglite_conformance/vgc_harness.h` | Case-table types, scratch-buffer geometry/extern, colour helpers, shared path arena API, and the shared per-case draw helpers (`vgc_ident`, `vgc_draw_path`, `vgc_finish_into`, `vgc_fb`). |
| `examples/display/vglite_conformance/vgc_arena.cpp` | The path arena, split out in Task 2 so it can be host-tested independently of VGLite init and the run loop. |
| `examples/display/vglite_conformance/vgc_cases_path.cpp` | The 12 Phase-1 paths/contours/winding cases + the case table. |
| `examples/display/vglite_conformance/vgc_dangerous.cpp` | The opt-in `-DVGC_DANGEROUS=1` case (unterminated path). Empty table otherwise. |
| `examples/display/vglite_conformance/vglite_conformance.cpp` | `setup()`: init, engine detection, scratch map, the run loop, the summary line. |
| `examples/display/vglite_conformance/CMakeLists.txt` | cores + VGLite only. |
| `examples/display/vglite_conformance/run_qemu.sh` | The gate: honest negative + tripwires. |
| `examples/display/vglite_conformance/transcript_qemu.txt` | Committed fixture (captured after the gate runs). |
| `examples/display/vglite_conformance/expected_silicon.txt` | Pre-registered per-case expectation with reasons. |
| `tools/vglite-conformance-check.sh` | Diffs a transcript against `expected_silicon.txt`; fails on drift in EITHER direction. |
| `docs/gc355-vglite-quirks.md` | The reference document: verdict · safe usage · evidence, one row per feature. |
| `tools/license-audit.sh` (modify) | New `GATES` entry. |
| `tools/gate-vacuity.test.sh` (modify) | Three new demonstrated-RED cases. |
| `CLAUDE.md` (modify) | Sweep 123 → 124, and the quirks-doc cross-link. |

## Decisions this plan locks in (and why)

1. **No panel.** Spec §4 says the panel is not involved. The core brings up the SEMC SDRAM in `startup.c` before `setup()` (`semc_sdram_init()`), so `EXTMEM` is live with no `Display.begin()`. Dropping `MipiDisplay`/`PXP` removes the 12 s bring-up margin `vglite_probe`'s gate needs and removes a whole subsystem from the failure surface.

2. **Tessellation buffer is 64×64, SMALLER than the 128×128 target.** This is deliberate and load-bearing. A tess buffer ≥ the target puts the driver in its `ts_is_fullscreen == 1` regime — the regime in which, per spec §1, scissor left/top clamping is silently disabled. The production compositors render a 720×1280 target with a 256×256 tess buffer, i.e. multi-tile. Phase 1 must run in the SAME regime as production or its answers are about a configuration nothing ships. Phase 3's `scissor/tess-fullscreen` case will deliberately re-init the other way to probe it.

3. **Every path ends with an explicit `VLC_OP_END`, never a trailing `VLC_OP_CLOSE`.** `vg_lite_init_path()` rewrites a trailing CLOSE into an END in place, and its `VG_LITE_S8` branch does so through an `(int*)` cast (`vg_lite_path.c:~200`) — it writes 4 bytes where 1 was intended. Ending on an explicit END means that fixup never fires, so no case is measuring the fixup instead of the hardware.

4. **`is_filled` thresholds the GREEN channel at 128.** Green sits at bits 15:8 in both ARGB and ABGR, so the predicate is immune to the R/B word-order question the spec defers to Phase 2. Background is opaque black, fill is opaque white, so thresholding at the midpoint approximates ≥50 % coverage regardless of how the driver applies antialiasing — which is what keeps these predicates structural rather than golden.

5. **The summary line is the spec's, with `repeat_differs=<n>` APPENDED.** Spec §4 fixes the field list and §5 requires the repeat check; the count has to be assertable, so the spec's line is kept as an exact prefix and the new field goes last. Noted here as a deliberate additive extension.

6. **A case may render more than once inside its `run()`** (`path/evenodd-vs-nonzero`, the format cases) and stash intermediate readings in its own statics. Such a case supplies an optional `sum()` accumulating an FNV over EVERY sub-render, so the `repeat=` check still covers all of them rather than only the last one left in the scratch buffer.

---

### Task 1: Pure predicates + host unit test

**Files:**
- Create: `examples/display/vglite_conformance/vgc_predicates.h`
- Create: `examples/display/vglite_conformance/tests/predicates_test.c`
- Create: `examples/display/vglite_conformance/tests/run.sh`

- [ ] **Step 1: Write the failing test**

Create `examples/display/vglite_conformance/tests/predicates_test.c`:

```c
/* Host-compiled unit test for the conformance probe's PIXEL PREDICATES --
 * no vg_lite, no Arduino, no target.  Build: tests/run.sh
 * Copyright (c) 2026 Nicholas Newdigate
 * SPDX-License-Identifier: MIT
 *
 * These predicates are the instrument. Spec section 11 names "a predicate is
 * wrong rather than the hardware" as a risk of the whole exercise, and a
 * predicate that miscounts on silicon is indistinguishable from silicon that
 * misrenders. Synthetic buffers with KNOWN answers are the only place that
 * confusion can be settled, so it is settled here, before any GPU is asked
 * anything. */
#undef NDEBUG          /* assertions must survive -DNDEBUG; BEFORE every include */
#include "../vgc_predicates.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

#define W 16
#define H 16
static uint32_t buf[W * H];

static void clear_bg(void)
{
    for (int i = 0; i < W * H; i++) buf[i] = 0xFF000000u;   /* opaque black */
}
static void set_fill(int x0, int y0, int x1, int y1)        /* half-open */
{
    for (int y = y0; y < y1; y++)
        for (int x = x0; x < x1; x++) buf[y * W + x] = 0xFFFFFFFFu;
}

int main(void)
{
    /* --- vgc_is_filled: the midpoint threshold, on the GREEN channel ---- */
    assert(vgc_is_filled(0xFFFFFFFFu) == 1);   /* white  */
    assert(vgc_is_filled(0xFF000000u) == 0);   /* black  */
    assert(vgc_is_filled(0xFF008000u) == 1);   /* g=0x80, exactly at the threshold */
    assert(vgc_is_filled(0xFF007F00u) == 0);   /* g=0x7F, just under */
    /* Immune to R/B word order: a pure-red pixel is UNFILLED whichever way
     * round the driver packs it, and a pure-green pixel is FILLED either way. */
    assert(vgc_is_filled(0xFFFF0000u) == 0);
    assert(vgc_is_filled(0xFF0000FFu) == 0);
    assert(vgc_is_filled(0xFF00FF00u) == 1);

    /* --- vgc_count_filled ----------------------------------------------- */
    clear_bg();
    assert(vgc_count_filled(buf, W, H, W) == 0);
    set_fill(2, 3, 10, 7);                       /* 8 wide, 4 tall = 32 */
    assert(vgc_count_filled(buf, W, H, W) == 32);
    clear_bg();
    set_fill(0, 0, W, H);
    assert(vgc_count_filled(buf, W, H, W) == W * H);

    /* stride is in WORDS and may exceed the width: pixels past `w` on a row
     * must NOT be counted (the scratch buffer's stride is a byte stride the
     * caller converts, and a stride bug is exactly the kind of off-by-a-row
     * error that would make every case wrong in the same plausible way) */
    clear_bg();
    for (int y = 0; y < H; y++) buf[y * W + (W - 1)] = 0xFFFFFFFFu;  /* last column */
    assert(vgc_count_filled(buf, W - 1, H, W) == 0);
    assert(vgc_count_filled(buf, W,     H, W) == H);

    /* --- vgc_count_runs_col: the multi-contour predicate ------------------ */
    clear_bg();
    assert(vgc_count_runs_col(buf, W, H, W, 8) == 0);
    set_fill(0, 2, W, 4);                        /* one band */
    assert(vgc_count_runs_col(buf, W, H, W, 8) == 1);
    set_fill(0, 6, W, 8);
    set_fill(0, 10, W, 12);
    set_fill(0, 14, W, 16);                      /* four bands, last touching the edge */
    assert(vgc_count_runs_col(buf, W, H, W, 8) == 4);
    /* a band running to the bottom edge still closes its run */
    clear_bg();
    set_fill(0, 14, W, 16);
    assert(vgc_count_runs_col(buf, W, H, W, 8) == 1);
    /* adjacent bands with no gap are ONE run -- this is what makes the
     * predicate answer "how many contours rendered", not "how many were
     * emitted": four bars merged into one solid block must read as 1 */
    clear_bg();
    set_fill(0, 2, W, 6);
    set_fill(0, 6, W, 10);
    assert(vgc_count_runs_col(buf, W, H, W, 8) == 1);
    /* the column argument is honoured */
    clear_bg();
    set_fill(0, 2, 4, 6);                        /* only x in [0,4) */
    assert(vgc_count_runs_col(buf, W, H, W, 2) == 1);
    assert(vgc_count_runs_col(buf, W, H, W, 8) == 0);

    /* --- vgc_filled_rows: the degenerate-geometry extent ------------------ */
    /* ★ THE SENTINEL MUST BE A VALUE THE IMPLEMENTATION CANNOT PRODUCE.
     * vgc_filled_rows initialises its own internal lo/hi to -1, so a test
     * sentinel of -1 makes the untouched-out-param assertion VACUOUS: delete
     * the implementation's `if (n)` guard -- the single most likely way to
     * break that contract -- and the unguarded write stores -1 over -1, which
     * this assertion cannot distinguish from not writing at all. Measured: the
     * -1 version stayed GREEN against exactly that defect. -99 is outside the
     * implementation's reachable range, so the assertion has power. The
     * contract matters because path/degenerate-zero-area distinguishes
     * "nothing was drawn" from "row 0 is filled" using it. */
    int ymin = -99, ymax = -99;
    clear_bg();
    assert(vgc_filled_rows(buf, W, H, W, &ymin, &ymax) == 0);
    assert(ymin == -99 && ymax == -99);           /* untouched when nothing is filled */
    set_fill(3, 7, 9, 9);                         /* rows 7..8 */
    assert(vgc_filled_rows(buf, W, H, W, &ymin, &ymax) == 12);
    assert(ymin == 7 && ymax == 8);

    /* --- vgc_fnv: pinned against the reference FNV-1a --------------------- */
    /* offset basis alone, for a zero-length input */
    assert(vgc_fnv("", 0) == 2166136261u);
    /* "a" -> 0xE40C292C, the published FNV-1a 32-bit test vector */
    assert(vgc_fnv("a", 1) == 0xE40C292Cu);
    /* "foobar" -> 0xBF9CF968, ditto */
    assert(vgc_fnv("foobar", 6) == 0xBF9CF968u);
    /* order-sensitive: a checksum that is not is useless as a repeat probe */
    assert(vgc_fnv("ab", 2) != vgc_fnv("ba", 2));

    printf("predicates_test: OK\n");
    return 0;
}
```

Create `examples/display/vglite_conformance/tests/run.sh`:

```sh
#!/bin/sh
# Host unit test for the conformance probe's pixel predicates. Runs on the
# development machine's own cc -- no toolchain, no board, no QEMU.
set -e
DIR=$(cd "$(dirname "$0")" && pwd)
OUT=$(mktemp -d "${TMPDIR:-/tmp}/vgc-tests.XXXXXX")
trap 'rm -rf "$OUT"' EXIT INT TERM HUP
cc -std=c11 -Wall -Wextra -Werror -O1 -o "$OUT/predicates_test" "$DIR/predicates_test.c"
"$OUT/predicates_test"
```

Make it executable:

```bash
chmod +x examples/display/vglite_conformance/tests/run.sh
```

- [ ] **Step 2: Run the test to verify it fails**

Run: `./examples/display/vglite_conformance/tests/run.sh`

Expected: FAIL at compile time — `fatal error: '../vgc_predicates.h' file not found`.

- [ ] **Step 3: Write the predicates**

Create `examples/display/vglite_conformance/vgc_predicates.h`:

```c
/* vgc_predicates.h - PURE pixel-buffer predicates for the GC355 conformance
 * probe. No vg_lite, no Arduino, no target headers: this file compiles on the
 * host and is unit-tested there (tests/predicates_test.c).
 * Copyright (c) 2026 Nicholas Newdigate
 * SPDX-License-Identifier: MIT
 *
 * Spec: docs/superpowers/specs/2026-08-30-gc355-conformance-design.md section 5.
 *
 * ★ STRUCTURAL, NEVER CHECKSUMS. A predicate here answers a question about
 * GEOMETRY -- how many separated runs are filled down this column, is the
 * centre of this ring background -- so it stays true across antialiasing
 * differences, driver revisions and the two engines. A golden checksum
 * answers "did this render change", which decays into a re-goldening ritual
 * the moment anything legitimately moves. The one checksum here (vgc_fnv) is
 * used ONLY to compare a render against an identical re-render in the same
 * boot, which is a question about determinism rather than correctness. */
#ifndef VGC_PREDICATES_H
#define VGC_PREDICATES_H

#include <stddef.h>
#include <stdint.h>

/* ★ Threshold the GREEN channel at the midpoint.
 *
 * Green occupies bits 15:8 in BOTH ARGB and ABGR, so this predicate cannot be
 * fooled by the word-order question (vg_lite_color_t is ABGR; the BGRA8888
 * target's memory words are ARGB) that Phase 2's colour cases exist to settle.
 * Reading red or blue here would silently make every Phase-1 answer depend on
 * an unsettled Phase-2 fact.
 *
 * The scratch buffer is cleared to opaque BLACK and every Phase-1 case fills
 * with opaque WHITE, so the midpoint is the ~50 % coverage contour whatever
 * the driver does about antialiasing -- interior pixels are fully white,
 * exterior fully black, and only the boundary lands in between. That is what
 * makes a filled-pixel COUNT comparable with an analytic area. */
static inline int vgc_is_filled(uint32_t px)
{
    return ((px >> 8) & 0xFFu) >= 0x80u;
}

/* Filled pixels in the w x h region. `stride_words` is the row pitch in
 * 32-bit words and may exceed `w`; pixels beyond `w` on a row are NOT
 * counted. */
static inline int vgc_count_filled(const uint32_t *fb, int w, int h,
                                   int stride_words)
{
    int n = 0;
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++)
            if (vgc_is_filled(fb[(size_t)y * stride_words + x])) n++;
    return n;
}

/* Number of SEPARATED filled runs down column `x`, top to bottom.
 *
 * This is the multi-contour predicate: four disjoint bars emitted in one path
 * give 4 if every contour rendered and 1 if only the first did (or if they
 * merged). Adjacent runs with no background row between them count as ONE --
 * deliberately, because "how many distinct filled regions are visible" is the
 * question, not "how many rectangles were emitted". */
static inline int vgc_count_runs_col(const uint32_t *fb, int w, int h,
                                     int stride_words, int x)
{
    if (x < 0 || x >= w) return 0;
    int runs = 0, in = 0;
    for (int y = 0; y < h; y++) {
        const int f = vgc_is_filled(fb[(size_t)y * stride_words + x]);
        if (f && !in) runs++;
        in = f;
    }
    return runs;
}

/* Filled-pixel count, and the first/last row containing any filled pixel.
 * `*ymin`/`*ymax` are left UNTOUCHED when nothing is filled -- an out-param
 * that reads 0 in that case would be indistinguishable from "row 0 is
 * filled", which is exactly the ambiguity the degenerate-geometry case has to
 * resolve. */
static inline int vgc_filled_rows(const uint32_t *fb, int w, int h,
                                  int stride_words, int *ymin, int *ymax)
{
    int n = 0, lo = -1, hi = -1;
    for (int y = 0; y < h; y++) {
        int row = 0;
        for (int x = 0; x < w; x++)
            if (vgc_is_filled(fb[(size_t)y * stride_words + x])) row++;
        if (row) { if (lo < 0) lo = y; hi = y; n += row; }
    }
    if (n) { *ymin = lo; *ymax = hi; }
    return n;
}

/* FNV-1a over raw bytes. Same arithmetic as every other checksum in this tree
 * so the numbers are comparable in kind. Used ONLY for the repeat= check. */
static inline uint32_t vgc_fnv(const void *p, size_t n)
{
    const uint8_t *b = (const uint8_t *)p;
    uint32_t sum = 2166136261u;
    for (size_t i = 0; i < n; i++) { sum ^= b[i]; sum *= 16777619u; }
    return sum;
}

#endif /* VGC_PREDICATES_H */
```

- [ ] **Step 4: Run the test to verify it passes**

Run: `./examples/display/vglite_conformance/tests/run.sh`

Expected: `predicates_test: OK`, exit 0.

- [ ] **Step 5: Mutate one predicate to prove the test can see it**

A test that has never failed on a real defect is decoration. Mutate `vgc_count_runs_col` so it counts filled pixels instead of runs, and re-run.

★ Use `if (f && in >= 0) runs++;`, NOT the naive `if (f) runs++;`. The naive form orphans `in` and dies under this file's own `-Werror` on `-Wunused-but-set-variable` — a red that proves the COMPILER noticed, not that the TEST can see the defect. `in >= 0` is always true here, so the mutant is semantically identical while keeping `in` read.

Run: `./examples/display/vglite_conformance/tests/run.sh`

Expected: FAIL — assertion on the one-band case (`== 1`, gets 2). Revert the mutation and re-run to confirm `predicates_test: OK`.

Then mutate the OTHER contract: delete the `if (n)` guard at the end of `vgc_filled_rows` and re-run.

Expected: FAIL on the untouched-out-param assertion. If it stays GREEN, the test's sentinel has collided with the implementation's internal init and the assertion is vacuous — see the ★ note in the test above. Revert and confirm green.

- [ ] **Step 6: Commit**

```bash
git add examples/display/vglite_conformance/vgc_predicates.h \
        examples/display/vglite_conformance/tests/
git commit -m "NEW-32: conformance probe pixel predicates + host unit test"
```

#### As-built: what review changed, and the contracts later tasks inherit

Task 1 shipped over three commits — `f89f287` (as planned), `845b5ca` and `924b89d` (review fixes). **The code blocks above are the starting point, not the final state**; the committed files are authoritative. Two rounds of review found four surviving mutants, each demonstrated RED and then closed. What later tasks must know:

1. **`vgc_count_runs_col` returns `-1` for an out-of-range `x`, not `0`.** The original `0` was indistinguishable from "this column has no filled runs", so a typo'd column index in a `check()` would read as *the contour did not render* — the instrument fabricating a GC355 defect, which is the exact risk this whole exercise is built to avoid. Changed while there were zero callers.
2. **Callers of `vgc_filled_rows` must seed `ymin`/`ymax` with a value outside `[0,h)`**, and must check the return value before reading them. Task 3's `check_degenerate` does both.
3. **`stride_words >= w` is a documented precondition**, not a checked one — deliberately, since every caller is in-tree and a runtime check would be overbuilding for an instrument whose value is its simplicity.
4. **`vgc_is_filled` is valid ONLY under the black-background/white-fill convention.** Phase 2's colour cases will need their own predicate — a pure-red fill makes this one return 0 for every pixel, reading as "nothing rendered".
5. **The test now follows the tree's `PASS:`/`FAIL:` per-check convention** (`tools/gate-vacuity.test.sh`'s shape) rather than aborting on the first `assert`. For an instrument, "which predicates are still trustworthy" is the question you most want answered on a red run — a first-failure abort tells you nothing about the other four. 39 checks; a red run reports `predicates_test: FAILED (N of 39 checks)`.

★ **The single most valuable fix was the FNV NUL case.** A `strlen`-flavoured `vgc_fnv` (`i < n && b[i]`) survived the original suite. The scratch buffer clears to `0xFF000000`, whose first byte is `0x00`, so that bug would return the offset basis for *every* render and make the `repeat=` determinism check vacuously green forever — with nothing else in the tree able to see it. Both new pins (`0xDC954658`, `0x050C5D1F`) were computed independently before being written down.

---

### Task 2: Harness header and the example skeleton (builds, engine-absent path only)

Get an ELF that builds and runs the absent path end to end, with an EMPTY case table. This is the smallest thing that can be gated, and it separates "the build/link/init works" from "the cases are right".

**Files:**
- Create: `examples/display/vglite_conformance/vgc_harness.h`
- Create: `examples/display/vglite_conformance/vglite_conformance.cpp`
- Create: `examples/display/vglite_conformance/CMakeLists.txt`
- Create: `examples/display/vglite_conformance/vgc_dangerous.cpp`

- [ ] **Step 1: Write the harness header**

Create `examples/display/vglite_conformance/vgc_harness.h`:

```c
/* vgc_harness.h - case-table types and shared state for the GC355/VGLite
 * conformance probe.
 * Copyright (c) 2026 Nicholas Newdigate
 * SPDX-License-Identifier: MIT
 *
 * Spec: docs/superpowers/specs/2026-08-30-gc355-conformance-design.md
 *
 * ★ TWO VERDICTS PER CASE, ALWAYS BOTH PRINTED. Every GC355 defect this tree
 * has hit shares one property: the driver reported success. So a case reports
 * what the API said (`api=`) and, independently, what the pixels say
 * (`pixel=`). `api=success pixel=broken` is the cell this whole example
 * exists to populate. */
#ifndef VGC_HARNESS_H
#define VGC_HARNESS_H

#include <stddef.h>
#include <stdint.h>

extern "C" {
#include "vg_lite.h"
}

/* Scratch render target. 128x128 BGRA8888 in EXTMEM (SDRAM at 0x80000000,
 * brought up by the core's startup before setup()). EXTMEM because the GPU
 * reaches it as a bus master exactly as it reaches a framebuffer, and because
 * the imxrt1176 core never enables the D-cache -- so a CPU read after
 * vg_lite_finish() sees the GPU's pixels with no maintenance. */
#define VGC_W 128
#define VGC_H 128

/* ★ TESSELLATION BUFFER SMALLER THAN THE TARGET, ON PURPOSE.
 * A tess buffer >= the target puts the driver in its ts_is_fullscreen == 1
 * regime, in which (spec section 1) scissor left/top clamping is silently
 * disabled -- a different machine from the one the production compositors
 * run on (720x1280 target, 256x256 tess: multi-tile). 64x64 against a 128x128
 * target keeps Phase 1 in the SAME regime as shipping code. Phase 3's
 * scissor/tess-fullscreen case deliberately probes the other one. */
#define VGC_TESS_W 64
#define VGC_TESS_H 64

/* vg_lite_color_t is ABGR (0xAABBGGRR) -- red in the LOW byte. Measured in
 * vglite_probe; getting it backwards does not fail, it renders the wrong
 * colour while every status says success. */
#define VGC_ABGR(r, g, b) (0xFF000000u | ((uint32_t)(b) << 16) | \
                           ((uint32_t)(g) << 8) | (uint32_t)(r))
#define VGC_BG_COLOR   VGC_ABGR(0x00, 0x00, 0x00)   /* opaque black */
#define VGC_FILL_COLOR VGC_ABGR(0xFF, 0xFF, 0xFF)   /* opaque white */

extern vg_lite_buffer_t vgc_scratch;

/* Clear the scratch target to VGC_BG_COLOR and finish. Every run() starts
 * here, so a case cannot contaminate its neighbour. */
vg_lite_error_t vgc_clear(void);

/* FNV-1a over the whole scratch buffer, rows only (stride-aware). */
uint32_t vgc_scratch_sum(void);

/* Read one scratch pixel as a memory word. */
uint32_t vgc_px(int x, int y);

/* ---- shared path arena ----------------------------------------------------
 * Cases emit path words here. vg_lite_draw() -> push_data() memcpys the path
 * into the command buffer before returning (vg_lite_path.c; vg_lite_init_path
 * never sets the upload bit), so the arena is reusable the instant the
 * preceding draw returns. Overflow is COUNTED and REFUSED rather than
 * truncating: a truncated path has no VLC_OP_END, and unterminated path data
 * is exactly what hangs the Vivante front end while every call still returns
 * VG_LITE_SUCCESS. */
#define VGC_ARENA_WORDS 512

void     vgc_arena_reset(void);
void     vgc_emit(int32_t w);
/* Terminates the path with an explicit VLC_OP_END and inits `p` over the
 * words emitted since the last vgc_arena_reset()/vgc_finish_path().
 *
 * ★ EXPLICIT END, NEVER A TRAILING CLOSE. vg_lite_init_path() rewrites a
 * trailing VLC_OP_CLOSE into VLC_OP_END in place, and its VG_LITE_S8 branch
 * does so through an (int*) cast -- four bytes where one was meant. Ending on
 * an explicit END means the fixup never fires, so no case here is measuring
 * that fixup instead of the hardware.
 *
 * Bounds are padded one unit on every side: the driver derives its
 * tessellation window from this box (rounded, not exact), and an exact bound
 * can land a half-pixel short at a tile boundary.
 *
 * Returns false if the arena overflowed; the caller must NOT draw. */
bool vgc_finish_path(vg_lite_path_t *p, float x0, float y0, float x1, float y1);

/* Emit a closed axis-aligned rect contour (CW: x,y -> x+w,y -> x+w,y+h ->
 * x,y+h). Coordinates are S32 path units == scratch pixels (identity matrix,
 * no fixed-point scaling: these cases probe geometry, not transforms). */
void vgc_emit_rect_cw(float x, float y, float w, float h);
/* The same rect wound the other way (CCW), for non-zero hole cutting. */
void vgc_emit_rect_ccw(float x, float y, float w, float h);

/* ---- the case table ------------------------------------------------------ */
typedef enum { VGC_SKIP = 0, VGC_OK = 1, VGC_BROKEN = 2 } vgc_verdict_t;

#define VGC_DETAIL_MAX 96

typedef struct {
    const char *id;                 /* stable slug -- expected_silicon.txt keys on it */
    /* Issues the vg_lite calls under test into vgc_scratch and finishes.
     * Returns VG_LITE_SUCCESS if every call succeeded, else the FIRST
     * non-success code. The harness has already cleared the scratch. */
    vg_lite_error_t (*run)(void);
    /* Reads scratch pixels and answers ONE structural question. Writes a
     * short "k=v,k=v" string (no spaces) into `detail`. */
    vgc_verdict_t (*check)(char *detail, size_t detail_len);
    /* OPTIONAL. A case whose run() renders more than once leaves only its
     * LAST sub-render in the scratch buffer, so the harness's default
     * repeat= sum would cover only that one. Such a case supplies this to
     * return an FNV accumulated over EVERY sub-render. NULL => the harness
     * uses vgc_scratch_sum(). */
    uint32_t (*sum)(void);
} vgc_case_t;

/* Defined in vgc_cases_path.cpp */
extern const vgc_case_t vgc_path_cases[];
extern const size_t     vgc_path_case_count;

/* Defined in vgc_dangerous.cpp. Empty unless built -DVGC_DANGEROUS=1. */
extern const vgc_case_t vgc_dangerous_cases[];
extern const size_t     vgc_dangerous_case_count;

#endif /* VGC_HARNESS_H */
```

- [ ] **Step 2: Write the main TU**

Create `examples/display/vglite_conformance/vglite_conformance.cpp`:

```cpp
/* vglite_conformance - which vg_lite features behave correctly on this GC355?
 * Copyright (c) 2026 Nicholas Newdigate
 * SPDX-License-Identifier: MIT
 *
 * Spec: docs/superpowers/specs/2026-08-30-gc355-conformance-design.md
 * Reference doc this feeds: docs/gc355-vglite-quirks.md
 *
 * Sibling to display/vglite_probe, which answers the PRIOR question (does the
 * GPU initialise and render at all). This one asks, feature by feature,
 * whether what it renders is RIGHT -- because every GC355 defect this tree has
 * hit reported success while producing the wrong picture.
 *
 * ONE BINARY, TWO OUTCOMES, both reported, neither hangs:
 *   QEMU     has no GC355, so the chip-ID probe reads 0, the run prints
 *            vgc_engine=absent and every case reports pixel=skip. That is the
 *            gated path (run_qemu.sh), and its tripwire is that NO case may
 *            report ok with no GPU present.
 *   Silicon  the whole matrix in ONE boot -- see transcript_hw_evkb.txt and
 *            expected_silicon.txt.
 *
 * NO PANEL. The core brings up the SEMC SDRAM in startup before setup(), so
 * EXTMEM is live without Display.begin(); nothing here scans out, flips or
 * touches LVGL. Prints to Serial1 (LPUART1, the console every gate captures).
 */
#include <Arduino.h>
#include <string.h>
#include "vgc_harness.h"
#include "vgc_predicates.h"

extern "C" {
#include "vg_lite_platform.h"
}

/* Contiguous pool for VGLite's command and tessellation buffers. EXTMEM, not
 * DMAMEM: OCRAM is 512K on this part and already spoken for -- a 2 MB pool
 * there overflows the region at link time. (Same reasoning as vglite_probe.) */
#define VGC_POOL_BYTES (2u * 1024u * 1024u)
EXTMEM __attribute__((aligned(64))) static uint8_t vgc_pool[VGC_POOL_BYTES];

/* The scratch render target. 64-byte aligned: Vivante wants 64-byte-aligned
 * buffers and a misaligned one does not fail, it hangs the front end while
 * every API call returns VG_LITE_SUCCESS. */
EXTMEM __attribute__((aligned(64)))
static uint8_t vgc_scratch_mem[VGC_W * VGC_H * 4];

vg_lite_buffer_t vgc_scratch;

static bool s_gpu = false;

/* ---- harness services ----------------------------------------------------- */

uint32_t vgc_px(int x, int y)
{
    const uint32_t *row = (const uint32_t *)((const uint8_t *)vgc_scratch_mem
                                             + (size_t)y * VGC_W * 4);
    return row[x];
}

uint32_t vgc_scratch_sum(void)
{
    return vgc_fnv(vgc_scratch_mem, (size_t)VGC_W * VGC_H * 4);
}

vg_lite_error_t vgc_clear(void)
{
    const vg_lite_error_t e = vg_lite_clear(&vgc_scratch, NULL, VGC_BG_COLOR);
    if (e != VG_LITE_SUCCESS) return e;
    return vg_lite_finish();
}

/* ---- path arena ----------------------------------------------------------- */

static int32_t s_arena[VGC_ARENA_WORDS];
static size_t  s_used;
static size_t  s_start;
static bool    s_overflow;

void vgc_arena_reset(void) { s_used = 0; s_start = 0; s_overflow = false; }

void vgc_emit(int32_t w)
{
    if (s_used < VGC_ARENA_WORDS) s_arena[s_used++] = w;
    else s_overflow = true;
}

bool vgc_finish_path(vg_lite_path_t *p, float x0, float y0, float x1, float y1)
{
    vgc_emit(VLC_OP_END);
    if (s_overflow) { s_overflow = false; s_start = s_used; return false; }
    memset(p, 0, sizeof(*p));
    vg_lite_init_path(p, VG_LITE_S32, VG_LITE_HIGH,
                      (uint32_t)((s_used - s_start) * sizeof(int32_t)),
                      &s_arena[s_start],
                      x0 - 1.0f, y0 - 1.0f, x1 + 1.0f, y1 + 1.0f);
    s_start = s_used;
    return true;
}

void vgc_emit_rect_cw(float x, float y, float w, float h)
{
    vgc_emit(VLC_OP_MOVE); vgc_emit((int32_t)x);       vgc_emit((int32_t)y);
    vgc_emit(VLC_OP_LINE); vgc_emit((int32_t)(x + w)); vgc_emit((int32_t)y);
    vgc_emit(VLC_OP_LINE); vgc_emit((int32_t)(x + w)); vgc_emit((int32_t)(y + h));
    vgc_emit(VLC_OP_LINE); vgc_emit((int32_t)x);       vgc_emit((int32_t)(y + h));
    vgc_emit(VLC_OP_CLOSE);
}

void vgc_emit_rect_ccw(float x, float y, float w, float h)
{
    vgc_emit(VLC_OP_MOVE); vgc_emit((int32_t)x);       vgc_emit((int32_t)y);
    vgc_emit(VLC_OP_LINE); vgc_emit((int32_t)x);       vgc_emit((int32_t)(y + h));
    vgc_emit(VLC_OP_LINE); vgc_emit((int32_t)(x + w)); vgc_emit((int32_t)(y + h));
    vgc_emit(VLC_OP_LINE); vgc_emit((int32_t)(x + w)); vgc_emit((int32_t)y);
    vgc_emit(VLC_OP_CLOSE);
}

/* ---- the run loop --------------------------------------------------------- */

static uint32_t s_ok, s_broken, s_skip, s_differs, s_cases;

static const char *verdict_name(vgc_verdict_t v)
{
    switch (v) {
    case VGC_OK:     return "ok";
    case VGC_BROKEN: return "broken";
    default:         return "skip";
    }
}

static void run_case(const vgc_case_t *c)
{
    /* ★ PRINT THE ID BEFORE ISSUING ANY CALL (spec section 6). Two known
     * traps HANG the front end rather than failing, and a hang costs the whole
     * matrix and a bench cycle. A case_begin line with no matching case line
     * names the culprit in the transcript. */
    Serial1.printf("vgc case_begin=%s\n", c->id);

    s_cases++;
    if (!s_gpu) {
        s_skip++;
        Serial1.printf("vgc case=%s api=skip pixel=skip detail=engine=absent repeat=skip\n",
                       c->id);
        return;
    }

    char detail[VGC_DETAIL_MAX];
    detail[0] = '\0';

    vgc_arena_reset();
    vgc_clear();
    const vg_lite_error_t api = c->run();
    const uint32_t sum1 = c->sum ? c->sum() : vgc_scratch_sum();
    const vgc_verdict_t v = c->check(detail, sizeof(detail));

    /* Second identical run. Per-boot and per-run nondeterminism is a
     * first-class GC355 symptom (7 boots, 7 checksums on the fader), so a
     * case that renders differently on an identical re-run is a finding in
     * its own right -- independent of whether the first render was correct,
     * which is why repeat= is its own field and does not fold into pixel=. */
    vgc_arena_reset();
    vgc_clear();
    (void)c->run();
    const uint32_t sum2 = c->sum ? c->sum() : vgc_scratch_sum();
    const bool same = (sum1 == sum2);
    if (!same) s_differs++;

    if (v == VGC_OK)          s_ok++;
    else if (v == VGC_BROKEN) s_broken++;
    else                      s_skip++;

    if (api == VG_LITE_SUCCESS)
        Serial1.printf("vgc case=%s api=success pixel=%s detail=%s repeat=%s\n",
                       c->id, verdict_name(v), detail, same ? "same" : "differs");
    else
        Serial1.printf("vgc case=%s api=error:%d pixel=%s detail=%s repeat=%s\n",
                       c->id, (int)api, verdict_name(v), detail,
                       same ? "same" : "differs");
}

void setup()
{
    Serial1.begin(115200);
    delay(200);
    Serial1.println("VGC_BEGIN");

    vg_lite_init_mem(VGLITE_RT1176_REGISTER_BASE, 0u, vgc_pool, VGC_POOL_BYTES);

    /* ★ ASK BEFORE COMMITTING. vg_lite_init() assumes the GPU exists and
     * SPINS when it does not (measured on QEMU). The chip-ID read is what
     * makes the absent case deterministic instead of a hang, and it is why one
     * binary serves both paths. */
    const uint32_t chip_id = vg_lite_hal_probe_chip_id();
    Serial1.printf("vgc_chip_id=0x%08lX\n", (unsigned long)chip_id);

    const char *absent_reason = NULL;
    if (chip_id == 0u) {
        absent_reason = "no_chip_id";
    } else {
        const vg_lite_error_t err = vg_lite_init(VGC_TESS_W, VGC_TESS_H);
        if (err != VG_LITE_SUCCESS) {
            Serial1.printf("vgc_init_err=%d\n", (int)err);
            absent_reason = "init_failed";
        } else {
            memset(&vgc_scratch, 0, sizeof(vgc_scratch));
            vgc_scratch.width   = VGC_W;
            vgc_scratch.height  = VGC_H;
            vgc_scratch.stride  = VGC_W * 4;
            vgc_scratch.tiled   = VG_LITE_LINEAR;
            vgc_scratch.format  = VG_LITE_BGRA8888;
            vgc_scratch.memory  = (void *)vgc_scratch_mem;
            vgc_scratch.address = (uint32_t)(uintptr_t)vgc_scratch_mem;
            /* ★ The GPU will not touch memory the kernel does not know about.
             * Without the map every draw returns SUCCESS and not one pixel
             * changes (vglite_probe, measured on silicon). */
            const vg_lite_error_t merr =
                vg_lite_map(&vgc_scratch, VG_LITE_MAP_USER_MEMORY, 0);
            if (merr != VG_LITE_SUCCESS) {
                Serial1.printf("vgc_map_err=%d\n", (int)merr);
                absent_reason = "map_failed";
            } else {
                s_gpu = true;
            }
        }
    }

    if (s_gpu) {
        Serial1.printf("vgc_engine=gpu target=%dx%d fmt=%d tess=%dx%d\n",
                       VGC_W, VGC_H, (int)VG_LITE_BGRA8888,
                       VGC_TESS_W, VGC_TESS_H);
    } else {
        Serial1.printf("vgc_engine=absent reason=%s\n", absent_reason);
    }

    for (size_t i = 0; i < vgc_path_case_count; i++)      run_case(&vgc_path_cases[i]);
    for (size_t i = 0; i < vgc_dangerous_case_count; i++) run_case(&vgc_dangerous_cases[i]);

    /* The spec's summary line, with repeat_differs appended (the spec's field
     * list is an exact PREFIX of this one -- an additive extension, because
     * the repeat count has to be assertable by the gate). */
    Serial1.printf("vgc_summary engine=%s cases=%lu ok=%lu broken=%lu skip=%lu "
                   "dangerous=%s repeat_differs=%lu\n",
                   s_gpu ? "gpu" : "absent",
                   (unsigned long)s_cases, (unsigned long)s_ok,
                   (unsigned long)s_broken, (unsigned long)s_skip,
#ifdef VGC_DANGEROUS
                   "on",
#else
                   "off",
#endif
                   (unsigned long)s_differs);

    /* A non-zero count means a bounded wait gave up, so the completion path
     * is wrong even where the pixels look right. */
    Serial1.printf("vgc_timeouts=%lu vgc_irqs=%lu\n",
                   (unsigned long)vg_lite_os_wait_timeouts(),
                   (unsigned long)vg_lite_os_irq_count());
    Serial1.println("VGC_DONE");
}

/* Heartbeat so a bench operator can tell a finished matrix from a hung one.
 * Touches no GPU state. */
void loop()
{
    static uint32_t last = 0;
    if (millis() - last >= 5000u) {
        last = millis();
        Serial1.printf("vgc_hb t=%lu\n", (unsigned long)last);
    }
}
```

- [ ] **Step 3: Write the dangerous-case TU (empty by default)**

Create `examples/display/vglite_conformance/vgc_dangerous.cpp`:

```cpp
/* vgc_dangerous.cpp - cases that can HANG the Vivante front end, compiled
 * ONLY under -DVGC_DANGEROUS=1.
 * Copyright (c) 2026 Nicholas Newdigate
 * SPDX-License-Identifier: MIT
 *
 * Spec section 6. The default build's matrix must never depend on the port's
 * bounded waits surviving a hang -- a hang there costs the whole matrix and a
 * bench cycle. Each case prints its case_begin line before issuing the call
 * (the harness does that for every case), so a hang is attributable from the
 * transcript, and vgc_summary's dangerous=on/off records which build produced
 * a given matrix.
 *
 * Build the opt-in matrix in its own directory, never in build/:
 *   cmake -B build-danger -DVGC_DANGEROUS=ON \
 *         -DCMAKE_TOOLCHAIN_FILE=../../../toolchain/rt1170-evkb.toolchain.cmake
 *   cmake --build build-danger
 * The gate only ever runs build/, so it always sees dangerous=off. */
#include "vgc_harness.h"

#ifdef VGC_DANGEROUS

#include "vgc_predicates.h"
#include <stdio.h>
#include <string.h>

/* path/unterminated -- path data with NO VLC_OP_END.
 *
 * SUSPECTED HANG (spec section 6). Both compositors' comments record that
 * unterminated path data is what hangs the front end while every vg_lite_*
 * call keeps returning VG_LITE_SUCCESS, but that has never been PROBED: it was
 * inferred from an arena-overflow near-miss. This case is the probe.
 *
 * The arena is written directly rather than through vgc_finish_path(), whose
 * whole job is to append the END this case must omit. */
static int32_t s_unterm[] = {
    VLC_OP_MOVE, 24, 24,
    VLC_OP_LINE, 104, 24,
    VLC_OP_LINE, 104, 104,
    VLC_OP_LINE, 24, 104,
    VLC_OP_CLOSE,
    /* deliberately no VLC_OP_END */
};

static vg_lite_error_t run_unterminated(void)
{
    vg_lite_path_t p;
    memset(&p, 0, sizeof(p));
    vg_lite_init_path(&p, VG_LITE_S32, VG_LITE_HIGH,
                      (uint32_t)sizeof(s_unterm), s_unterm,
                      23.0f, 23.0f, 105.0f, 105.0f);
    vg_lite_matrix_t m;
    vg_lite_identity(&m);
    const vg_lite_error_t e = vg_lite_draw(&vgc_scratch, &p, VG_LITE_FILL_NON_ZERO,
                                           &m, VG_LITE_BLEND_NONE, VGC_FILL_COLOR);
    const vg_lite_error_t f = vg_lite_finish();
    return e != VG_LITE_SUCCESS ? e : f;
}

static vgc_verdict_t check_unterminated(char *d, size_t n)
{
    /* Reaching here at all is the finding: the call RETURNED. Whether it drew
     * the square is secondary and is recorded rather than judged. */
    const int centre = vgc_is_filled(vgc_px(64, 64));
    snprintf(d, n, "returned=1,centre=%d", centre);
    return centre ? VGC_OK : VGC_BROKEN;
}

const vgc_case_t vgc_dangerous_cases[] = {
    { "path/unterminated", run_unterminated, check_unterminated, NULL },
};
const size_t vgc_dangerous_case_count =
    sizeof(vgc_dangerous_cases) / sizeof(vgc_dangerous_cases[0]);

#else

/* A zero-length array is not standard C++, and a NULL table with a zero count
 * would make the run loop's bounds depend on a pointer nobody dereferences.
 * One never-iterated entry keeps both honest. */
const vgc_case_t vgc_dangerous_cases[] = { { NULL, NULL, NULL, NULL } };
const size_t vgc_dangerous_case_count = 0;

#endif /* VGC_DANGEROUS */
```

- [ ] **Step 4: Write CMakeLists.txt**

Create `examples/display/vglite_conformance/CMakeLists.txt`:

```cmake
cmake_minimum_required(VERSION 3.24)
project(vglite_conformance)

set(TEENSY_VERSION 117 CACHE STRING "")

include(${CMAKE_CURRENT_LIST_DIR}/../../../evkb.cmake)

# VGLite only. NO MipiDisplay, NO PXP, NO LVGL: spec section 4 keeps the panel
# out of this example entirely (no scanout, no flip). The core's startup brings
# up the SEMC SDRAM before setup(), so EXTMEM is live without Display.begin(),
# and dropping the panel removes a whole subsystem from the failure surface --
# and the 12s bring-up margin vglite_probe's gate has to allow for.
import_evkb_vglite()

teensy_add_executable(vglite_conformance
    vglite_conformance.cpp
    vgc_cases_path.cpp
    vgc_dangerous.cpp)
teensy_target_link_libraries(vglite_conformance cores)

# VGLite is a plain CMake static-lib target (see import_evkb_vglite for why
# teensy_target_link_libraries cannot link it).
target_link_libraries(vglite_conformance.elf VGLite stdc++)

# Opt-in matrix including the cases that can HANG the front end (spec
# section 6). NEVER configured into build/ -- use a separate build-danger dir.
if(VGC_DANGEROUS)
    add_compile_definitions(VGC_DANGEROUS=1)
endif()
```

- [ ] **Step 5: Add a placeholder case table so the skeleton links**

Create `examples/display/vglite_conformance/vgc_cases_path.cpp` with an empty table for now (Task 3 fills it):

```cpp
/* vgc_cases_path.cpp - paths, contours and winding cases (spec section 5).
 * Copyright (c) 2026 Nicholas Newdigate
 * SPDX-License-Identifier: MIT */
#include "vgc_harness.h"

const vgc_case_t vgc_path_cases[] = { { NULL, NULL, NULL, NULL } };
const size_t     vgc_path_case_count = 0;
```

- [ ] **Step 6: Build**

```bash
cd examples/display/vglite_conformance
cmake -B build -DCMAKE_TOOLCHAIN_FILE=../../../toolchain/rt1170-evkb.toolchain.cmake
cmake --build build
```

Expected: `build/vglite_conformance.elf` produced, no warnings about undefined `vgc_path_cases`.

- [ ] **Step 7: Run it under QEMU by hand**

```bash
cd examples/display/vglite_conformance && ../../../tools/qrun -M mimxrt1170-evk -global fsl-imxrt1170.boot-xip=on -kernel build/vglite_conformance.elf -display none -serial stdio
```

Expected output (Ctrl-C after `VGC_DONE`):

```
VGC_BEGIN
vgc_chip_id=0x00000000
vgc_engine=absent reason=no_chip_id
vgc_summary engine=absent cases=0 ok=0 broken=0 skip=0 dangerous=off repeat_differs=0
vgc_timeouts=0 vgc_irqs=0
VGC_DONE
```

`cases=0` is expected at this task and is exactly why the gate (Task 5) asserts a non-zero case count.

- [ ] **Step 8: Commit**

```bash
git add examples/display/vglite_conformance/
git commit -m "NEW-32: vglite_conformance harness skeleton (engine-absent path)"
```

#### As-built: what review changed, and the API Task 3 codes against

Task 2 shipped over three commits — `fbf5b77` (as planned, with two justified deviations), `8414ea7` (API fixes) and `19e87ce` (the arena split + its host test). **The blocks above are the starting point; the committed files are authoritative.** The API Task 3 must use:

```c
const uint32_t   *vgc_fb(void);                 /* the ONE access path to the scratch */
uint32_t          vgc_px(int x, int y);         /* range-checked; OOB counted, see below */
uint32_t          vgc_px_oob(void);  void vgc_px_oob_reset(void);
vg_lite_matrix_t *vgc_ident(void);
void              vgc_draw_path(vg_lite_path_t *p, vg_lite_fill_t rule,
                                uint32_t color, vg_lite_error_t *acc);
void              vgc_finish_into(vg_lite_error_t *acc);
vg_lite_error_t   vgc_finish_path(vg_lite_path_t *p, float x0, float y0,
                                  float x1, float y1);   /* was bool */
void              vgc_emit_rect_cw (int32_t x, int32_t y, int32_t w, int32_t h);
void              vgc_emit_rect_ccw(int32_t x, int32_t y, int32_t w, int32_t h);
```

★ **The finding that mattered most: `vgc_finish_path` left `*p` as stack garbage on the overflow path.** Task 3 declares `vg_lite_path_t p;` uninitialised in eight `run()` bodies, so a case that ignored the failure would have drawn a path whose `path` pointer and `path_length` were stack garbage — exactly the unterminated-path-data condition that hangs the Vivante front end while every call returns SUCCESS. **The harness would have been manufacturing the failure it exists to measure.** Fixed by zeroing `*p` before the early return, and by returning `vg_lite_error_t` so a discarded result looks wrong.

Other changes Task 3 inherits:
- **Two access paths to the scratch buffer collapsed into one.** A case-local `fb()` returning `vgc_scratch.memory` would already disagree with the harness on the engine-absent path, where `.memory` is NULL while the backing array is valid.
- **No status is discarded any more.** A failed `vgc_clear()` now yields `pixel=skip detail=clear_failed:N` instead of a verdict computed from a contaminated buffer, and the repeat run's API status is compared and surfaced as `api2=` in the detail.
- **`detail` is sanitised** (spaces → `_`, empty → `none`) so it can never break the checker's field parse, and a NULL table entry is a named skip line rather than a hard fault.
- **`vgc_px` out-of-range is COUNTED, not sentinelled** — `0` is transparent black, a word a Phase-2 blending case could legitimately produce, so no return value is safely "impossible". A non-zero count turns the case into `detail=px_oob:N` and a skip, so an instrument bug can never be spelled as `ok` or `broken`.
- **The corrected S8 fixup fact**, read from the source: `vg_lite_init_path` READS byte `num-1` but WRITES one byte at `4*(num-1)` — for an 11-byte S8 path, a single-byte write **29 bytes past the end**. Materially worse than "four bytes where one was meant". Ending on an explicit `VLC_OP_END` means the branch never fires.

★ **The arena was the highest-risk unit in the harness and had no test** while Task 1's simpler pure predicates had 39 checks. It is now `vgc_arena.cpp` with `tests/arena_test.c` — 42 checks pinning path word counts, bounds padding, sequential non-overlap, mid-sequence reset, overflow refusal with `*p` zeroed, and the `VLC_OP_END` terminator. Three mutants demonstrated RED, each failing the line it targets, and the deleted-early-return and off-by-one mutants are DISTINGUISHABLE from each other rather than lumped. Host suite total is now **81 checks** across the two tests, both run by `tests/run.sh`.

---

### Task 3: The twelve paths/contours/winding cases

**Files:**
- Modify: `examples/display/vglite_conformance/vgc_cases_path.cpp` (replace wholesale)

Coordinates below are scratch pixels; the matrix is identity and the path format is `VG_LITE_S32` (or the format under test), so path units ARE pixels. No fixed-point scaling — these cases probe geometry, not transforms.

- [ ] **Step 1: Write the case file**

Replace `examples/display/vglite_conformance/vgc_cases_path.cpp` with:

```cpp
/* vgc_cases_path.cpp - paths, contours and winding (spec section 5).
 * Copyright (c) 2026 Nicholas Newdigate
 * SPDX-License-Identifier: MIT
 *
 * The area that produced the ONE-CONTOUR-PER-PATH rule, which is why the spec
 * probes it first: on this GC355 a path renders only its FIRST contour, every
 * subpath after the first VLC_OP_MOVE vanishing while every vg_lite_* call
 * returns VG_LITE_SUCCESS. Three cases here measure that directly
 * (multi-contour-disjoint, two-contour-ring-nonzero, evenodd-vs-nonzero) and
 * each is PAIRED with a control that must pass if the harness is sound:
 * single-contour-rect (the baseline -- if THIS is broken nothing else in the
 * matrix means anything), two-draws-ring (the safe usage) and
 * self-intersecting (a single contour whose fill rules must both be honoured).
 * If a control fails, suspect the harness, not the silicon (spec section 11).
 *
 * Every path ends with an explicit VLC_OP_END via vgc_finish_path(); see the
 * note there for why a trailing CLOSE is never used. */
#include "vgc_harness.h"
#include "vgc_predicates.h"
#include <stdio.h>
#include <string.h>

/* ---- shared drawing helpers live in the HARNESS ----------------------------
 * ★ `vgc_ident()`, `vgc_draw_path()`, `vgc_finish_into()` and `vgc_fb()` are
 * declared in vgc_harness.h, NOT defined here. They were local to this file in
 * the plan's first draft and were promoted in Task 2 for two reasons that only
 * showed up under review:
 *  - draw/finish implement a contract the HEADER already states ("the FIRST
 *    non-success code"). A contract specified in one place and re-implemented
 *    in each of Phases 1, 2 and 3 will silently diverge.
 *  - a local `fb()` returning `vgc_scratch.memory` would be a SECOND access
 *    path to the scratch buffer, disagreeing with the harness's own
 *    `vgc_px`/`vgc_scratch_sum` the moment a later case re-points the buffer
 *    -- and already disagreeing on the engine-absent path, where
 *    `vgc_scratch.memory` is NULL while the backing array is valid. */

/* ---- 1. path/single-contour-rect ------------------------------------------
 * THE BASELINE. One closed rect, one contour, one draw. 80x80 at (24,24). */

#define R_X 24
#define R_Y 24
#define R_W 80
#define R_H 80
#define R_AREA (R_W * R_H)      /* 6400 */

static vg_lite_error_t run_single_rect(void)
{
    vg_lite_error_t acc = VG_LITE_SUCCESS;
    vg_lite_path_t p;
    vgc_emit_rect_cw(R_X, R_Y, R_W, R_H);
    { const vg_lite_error_t fe = vgc_finish_path(&p, R_X, R_Y, R_X + R_W, R_Y + R_H);
          if (fe != VG_LITE_SUCCESS) return fe; }
    vgc_draw_path(&p, VG_LITE_FILL_NON_ZERO, VGC_FILL_COLOR, &acc);
    vgc_finish_into(&acc);
    return acc;
}

static vgc_verdict_t check_single_rect(char *d, size_t n)
{
    const int fill   = vgc_count_filled(vgc_fb(), VGC_W, VGC_H, VGC_W);
    const int centre = vgc_is_filled(vgc_px(64, 64));
    const int corner = vgc_is_filled(vgc_px(10, 10));
    snprintf(d, n, "fill=%d,expect=%d,centre=%d,corner=%d",
             fill, R_AREA, centre, corner);
    /* +/-6% absorbs the antialiased boundary (perimeter 320 px, at most a
     * pixel either way on each edge) without admitting a wrong shape. */
    const int lo = R_AREA - R_AREA * 6 / 100, hi = R_AREA + R_AREA * 6 / 100;
    return (centre && !corner && fill >= lo && fill <= hi) ? VGC_OK : VGC_BROKEN;
}

/* ---- 2. path/multi-contour-disjoint ----------------------------------------
 * Four separated bars in ONE path. Count filled runs down column x=64.
 * EXPECTED BROKEN on this GC355: runs=1 (only the first contour renders). */

static const int BAR_Y[4] = { 16, 44, 72, 100 };
#define BAR_H 16

static vg_lite_error_t run_multi_contour(void)
{
    vg_lite_error_t acc = VG_LITE_SUCCESS;
    vg_lite_path_t p;
    for (int i = 0; i < 4; i++) vgc_emit_rect_cw(R_X, BAR_Y[i], R_W, BAR_H);
    { const vg_lite_error_t fe = vgc_finish_path(&p, R_X, BAR_Y[0], R_X + R_W, BAR_Y[3] + BAR_H);
          if (fe != VG_LITE_SUCCESS) return fe; }
    vgc_draw_path(&p, VG_LITE_FILL_NON_ZERO, VGC_FILL_COLOR, &acc);
    vgc_finish_into(&acc);
    return acc;
}

static vgc_verdict_t check_multi_contour(char *d, size_t n)
{
    const int runs = vgc_count_runs_col(vgc_fb(), VGC_W, VGC_H, VGC_W, 64);
    const int fill = vgc_count_filled(vgc_fb(), VGC_W, VGC_H, VGC_W);
    snprintf(d, n, "runs=%d,expect=4,fill=%d", runs, fill);
    return runs == 4 ? VGC_OK : VGC_BROKEN;
}

/* ---- 3. path/two-contour-ring-nonzero --------------------------------------
 * Outer CW + reversed inner CCW in ONE path under NON_ZERO: the classic hole.
 * EXPECTED BROKEN: the inner contour is dropped and the ring fills solid. */

#define I_X 48
#define I_Y 48
#define I_W 32
#define I_H 32

static vg_lite_error_t run_ring_two_contour(void)
{
    vg_lite_error_t acc = VG_LITE_SUCCESS;
    vg_lite_path_t p;
    vgc_emit_rect_cw(R_X, R_Y, R_W, R_H);
    vgc_emit_rect_ccw(I_X, I_Y, I_W, I_H);
    { const vg_lite_error_t fe = vgc_finish_path(&p, R_X, R_Y, R_X + R_W, R_Y + R_H);
          if (fe != VG_LITE_SUCCESS) return fe; }
    vgc_draw_path(&p, VG_LITE_FILL_NON_ZERO, VGC_FILL_COLOR, &acc);
    vgc_finish_into(&acc);
    return acc;
}

/* Shared by cases 3 and 4: rim filled AND centre background is the ring. */
static vgc_verdict_t check_ring(char *d, size_t n)
{
    const int rim    = vgc_is_filled(vgc_px(32, 64));   /* inside outer, outside inner */
    const int centre = vgc_is_filled(vgc_px(64, 64));   /* inside inner */
    snprintf(d, n, "rim=%d,centre=%d,expect=rim1centre0", rim, centre);
    return (rim && !centre) ? VGC_OK : VGC_BROKEN;
}

/* ---- 4. path/two-draws-ring ------------------------------------------------
 * THE SAFE-USAGE CONTROL for case 3. Same ring, built as a filled plate with
 * an inset plate in the background colour over it: two single-contour paths,
 * two draws. This is the construction both shipping compositors use. */

static vg_lite_error_t run_ring_two_draws(void)
{
    vg_lite_error_t acc = VG_LITE_SUCCESS;
    vg_lite_path_t outer, inner;

    vgc_emit_rect_cw(R_X, R_Y, R_W, R_H);
    { const vg_lite_error_t fe = vgc_finish_path(&outer, R_X, R_Y, R_X + R_W, R_Y + R_H);
          if (fe != VG_LITE_SUCCESS) return fe; }
    vgc_draw_path(&outer, VG_LITE_FILL_NON_ZERO, VGC_FILL_COLOR, &acc);

    vgc_emit_rect_cw(I_X, I_Y, I_W, I_H);
    { const vg_lite_error_t fe = vgc_finish_path(&inner, I_X, I_Y, I_X + I_W, I_Y + I_H);
          if (fe != VG_LITE_SUCCESS) return fe; }
    vgc_draw_path(&inner, VG_LITE_FILL_NON_ZERO, VGC_BG_COLOR, &acc);

    vgc_finish_into(&acc);
    return acc;
}

/* ---- 5. path/evenodd-vs-nonzero --------------------------------------------
 * Nested rects with the SAME winding, drawn under each fill rule. EVEN_ODD
 * must cut the hole, NON_ZERO must fill solid -- that difference IS the fill
 * rule, and it is the only thing this case asks about.
 *
 * Renders twice inside one run(), so it supplies sum_evenodd() accumulating
 * over BOTH sub-renders (see vgc_harness.h). */

static int      s_eo_rim, s_eo_centre;
static uint32_t s_eo_sum;

static void emit_nested(void)
{
    vgc_emit_rect_cw(R_X, R_Y, R_W, R_H);
    vgc_emit_rect_cw(I_X, I_Y, I_W, I_H);   /* same winding, deliberately */
}

static vg_lite_error_t run_evenodd_nonzero(void)
{
    vg_lite_error_t acc = VG_LITE_SUCCESS;
    vg_lite_path_t p;

    /* pass 1: EVEN_ODD, sampled and then cleared away */
    emit_nested();
    { const vg_lite_error_t fe = vgc_finish_path(&p, R_X, R_Y, R_X + R_W, R_Y + R_H);
          if (fe != VG_LITE_SUCCESS) return fe; }
    vgc_draw_path(&p, VG_LITE_FILL_EVEN_ODD, VGC_FILL_COLOR, &acc);
    vgc_finish_into(&acc);
    s_eo_rim    = vgc_is_filled(vgc_px(32, 64));
    s_eo_centre = vgc_is_filled(vgc_px(64, 64));
    s_eo_sum    = vgc_scratch_sum();

    /* pass 2: NON_ZERO, left in the scratch for check() to read live */
    vgc_arena_reset();
    const vg_lite_error_t ce = vgc_clear();
    if (ce != VG_LITE_SUCCESS && acc == VG_LITE_SUCCESS) acc = ce;
    emit_nested();
    { const vg_lite_error_t fe = vgc_finish_path(&p, R_X, R_Y, R_X + R_W, R_Y + R_H);
          if (fe != VG_LITE_SUCCESS) return fe; }
    vgc_draw_path(&p, VG_LITE_FILL_NON_ZERO, VGC_FILL_COLOR, &acc);
    vgc_finish_into(&acc);
    return acc;
}

static uint32_t sum_evenodd_nonzero(void)
{
    /* Both sub-renders, folded: FNV of the pass-1 sum's bytes chained into
     * the live pass-2 buffer, so a difference in EITHER pass shows up. */
    const uint32_t a = s_eo_sum, b = vgc_scratch_sum();
    uint32_t pair[2] = { a, b };
    return vgc_fnv(pair, sizeof(pair));
}

static vgc_verdict_t check_evenodd_nonzero(char *d, size_t n)
{
    const int nz_rim    = vgc_is_filled(vgc_px(32, 64));
    const int nz_centre = vgc_is_filled(vgc_px(64, 64));
    snprintf(d, n, "eo_rim=%d,eo_centre=%d,nz_rim=%d,nz_centre=%d",
             s_eo_rim, s_eo_centre, nz_rim, nz_centre);
    return (s_eo_rim && !s_eo_centre && nz_rim && nz_centre) ? VGC_OK : VGC_BROKEN;
}

/* ---- 6. path/self-intersecting ---------------------------------------------
 * A pentagram: ONE contour that crosses itself. The centre pentagon is EMPTY
 * under EVEN_ODD (crossing number 2) and FILLED under NON_ZERO (winding 2),
 * and the five tips are filled under both. This is the case that distinguishes
 * the two fill rules on a single contour -- so unlike case 5 it should pass on
 * this GC355, which makes it case 5's control as well as its own probe.
 *
 * Vertices: r=50 about (64,64), at -90 + k*144 degrees, k = 0..4, connected in
 * that order. Integers, rounded once here so the geometry is fixed. */

static const int STAR[5][2] = {
    {  64,  14 },   /* -90 deg */
    {  93, 104 },   /*  54    */
    {  16,  49 },   /* 198    */
    { 112,  49 },   /* 342    */
    {  35, 104 },   /* 126    */
};

static int      s_star_eo_centre, s_star_eo_tip;
static uint32_t s_star_eo_sum;

static void emit_star(void)
{
    vgc_emit(VLC_OP_MOVE); vgc_emit(STAR[0][0]); vgc_emit(STAR[0][1]);
    for (int i = 1; i < 5; i++) {
        vgc_emit(VLC_OP_LINE); vgc_emit(STAR[i][0]); vgc_emit(STAR[i][1]);
    }
    vgc_emit(VLC_OP_CLOSE);
}

static vg_lite_error_t run_self_intersecting(void)
{
    vg_lite_error_t acc = VG_LITE_SUCCESS;
    vg_lite_path_t p;

    emit_star();
    { const vg_lite_error_t fe = vgc_finish_path(&p, 16, 14, 112, 104);
      if (fe != VG_LITE_SUCCESS) return fe; }
    vgc_draw_path(&p, VG_LITE_FILL_EVEN_ODD, VGC_FILL_COLOR, &acc);
    vgc_finish_into(&acc);
    s_star_eo_centre = vgc_is_filled(vgc_px(64, 64));   /* centre pentagon */
    s_star_eo_tip    = vgc_is_filled(vgc_px(64, 22));   /* inside the top point */
    s_star_eo_sum    = vgc_scratch_sum();

    vgc_arena_reset();
    const vg_lite_error_t ce = vgc_clear();
    if (ce != VG_LITE_SUCCESS && acc == VG_LITE_SUCCESS) acc = ce;
    emit_star();
    { const vg_lite_error_t fe = vgc_finish_path(&p, 16, 14, 112, 104);
      if (fe != VG_LITE_SUCCESS) return fe; }
    vgc_draw_path(&p, VG_LITE_FILL_NON_ZERO, VGC_FILL_COLOR, &acc);
    vgc_finish_into(&acc);
    return acc;
}

static uint32_t sum_self_intersecting(void)
{
    uint32_t pair[2] = { s_star_eo_sum, vgc_scratch_sum() };
    return vgc_fnv(pair, sizeof(pair));
}

static vgc_verdict_t check_self_intersecting(char *d, size_t n)
{
    const int nz_centre = vgc_is_filled(vgc_px(64, 64));
    const int nz_tip    = vgc_is_filled(vgc_px(64, 22));
    snprintf(d, n, "eo_centre=%d,eo_tip=%d,nz_centre=%d,nz_tip=%d",
             s_star_eo_centre, s_star_eo_tip, nz_centre, nz_tip);
    return (!s_star_eo_centre && s_star_eo_tip && nz_centre && nz_tip)
           ? VGC_OK : VGC_BROKEN;
}

/* ---- 7-11. path/format-* ---------------------------------------------------
 * The SAME right triangle in each of the four path coordinate formats. The
 * driver reads path data as an array of the FORMAT's element width -- opcodes
 * included (vg_lite_path.c get_data_size) -- so each format needs its own
 * typed array rather than a shared int32_t arena.
 *
 * Vertices (10,10),(70,10),(10,70): area 1800 px, and inside the signed 8-bit
 * range so VG_LITE_S8 can express it. Each format's own case checks its fill
 * count against that analytic area, so each stands alone; path/format-agreement
 * then checks all four counts against EACH OTHER, which is the cross-format
 * question the per-format cases cannot ask.
 *
 * Every array ends with an explicit VLC_OP_END -- especially load-bearing for
 * S8, whose CLOSE->END fixup in vg_lite_init_path() writes through an (int*)
 * cast (4 bytes where 1 was meant). Ending on END means it never fires. */

#define TRI_AREA 1800

static int8_t  s_tri_s8[]  = { VLC_OP_MOVE, 10, 10, VLC_OP_LINE, 70, 10,
                               VLC_OP_LINE, 10, 70, VLC_OP_CLOSE, VLC_OP_END };
static int16_t s_tri_s16[] = { VLC_OP_MOVE, 10, 10, VLC_OP_LINE, 70, 10,
                               VLC_OP_LINE, 10, 70, VLC_OP_CLOSE, VLC_OP_END };
static int32_t s_tri_s32[] = { VLC_OP_MOVE, 10, 10, VLC_OP_LINE, 70, 10,
                               VLC_OP_LINE, 10, 70, VLC_OP_CLOSE, VLC_OP_END };
/* ★ THE FP32 ARRAY ABOVE IS WRONG AS FIRST WRITTEN — corrected in Task 3.
 * An opcode is ONE BYTE AT THE BASE OF A FORMAT-WIDTH SLOT, not a value of
 * the format's type. `(float)VLC_OP_MOVE` is 2.0f = 0x40000000, whose first
 * byte is 0x00 == VLC_OP_END, so the path terminates immediately: measured on
 * the host reference rasteriser as `format-fp32 broken fill=0` and
 * `format-agreement broken … fp32=0` — two fabricated BROKENs.
 * Confirmed in two independent places in the driver:
 *   vg_lite_path.c:223-227  the FP32 CLOSE->END fixup tests
 *                           *(char*)((float*)path_data + num - 1) -- a CHAR
 *                           read at the float slot's base;
 *   vg_lite_stroke.c:5148   builds an FP32 path as
 *                           `cpath = (char*)pathdata + offset;
 *                            *cpath = VLC_OP_MOVE; fpath++;`
 *                           -- one byte written, the whole 4-byte slot skipped.
 * S8/S16/S32 get the right byte 0 for free on little-endian ARM, which is why
 * only FP32 was affected. The committed file builds the FP32 array through its
 * BYTES (a memcpy-based helper), rebuilt on each use so nothing can be left
 * mutated between the harness's two identical runs. */
static float   s_tri_f32[11];   /* built byte-wise -- see the ★ note above */

static int s_fmt_fill[4];   /* s8, s16, s32, fp32 */

static vg_lite_error_t run_triangle(vg_lite_format_t fmt, void *data,
                                    uint32_t bytes, int slot)
{
    vg_lite_error_t acc = VG_LITE_SUCCESS;
    vg_lite_path_t p;
    memset(&p, 0, sizeof(p));
    vg_lite_init_path(&p, fmt, VG_LITE_HIGH, bytes, data, 9.0f, 9.0f, 71.0f, 71.0f);
    vgc_draw_path(&p, VG_LITE_FILL_NON_ZERO, VGC_FILL_COLOR, &acc);
    vgc_finish_into(&acc);
    s_fmt_fill[slot] = vgc_count_filled(vgc_fb(), VGC_W, VGC_H, VGC_W);
    return acc;
}

static vg_lite_error_t run_fmt_s8(void)
{ return run_triangle(VG_LITE_S8, s_tri_s8, (uint32_t)sizeof(s_tri_s8), 0); }
static vg_lite_error_t run_fmt_s16(void)
{ return run_triangle(VG_LITE_S16, s_tri_s16, (uint32_t)sizeof(s_tri_s16), 1); }
static vg_lite_error_t run_fmt_s32(void)
{ return run_triangle(VG_LITE_S32, s_tri_s32, (uint32_t)sizeof(s_tri_s32), 2); }
static vg_lite_error_t run_fmt_f32(void)
{ return run_triangle(VG_LITE_FP32, s_tri_f32, (uint32_t)sizeof(s_tri_f32), 3); }

static vgc_verdict_t check_fmt(int slot, char *d, size_t n)
{
    const int fill = s_fmt_fill[slot];
    /* +/-8%: the triangle's perimeter is ~205 px, so a pixel of antialiased
     * boundary either way is ~5.7% of 1800. */
    const int lo = TRI_AREA - TRI_AREA * 8 / 100, hi = TRI_AREA + TRI_AREA * 8 / 100;
    snprintf(d, n, "fill=%d,expect=%d", fill, TRI_AREA);
    return (fill >= lo && fill <= hi) ? VGC_OK : VGC_BROKEN;
}
static vgc_verdict_t check_fmt_s8 (char *d, size_t n) { return check_fmt(0, d, n); }
static vgc_verdict_t check_fmt_s16(char *d, size_t n) { return check_fmt(1, d, n); }
static vgc_verdict_t check_fmt_s32(char *d, size_t n) { return check_fmt(2, d, n); }
static vgc_verdict_t check_fmt_f32(char *d, size_t n) { return check_fmt(3, d, n); }

/* path/format-agreement: renders all four again, in one run, and compares.
 * It does NOT reuse s_fmt_fill from the earlier cases -- reading state left by
 * a neighbouring case would make this case's answer depend on the table's
 * ORDER, which is exactly the coupling the one-case-at-a-time design forbids. */

static int      s_agree[4];
static uint32_t s_agree_sum;

static vg_lite_error_t run_fmt_agreement(void)
{
    vg_lite_error_t acc = VG_LITE_SUCCESS;
    uint32_t sums[4];
    struct { vg_lite_format_t f; void *d; uint32_t n; } v[4] = {
        { VG_LITE_S8,   s_tri_s8,  (uint32_t)sizeof(s_tri_s8)  },
        { VG_LITE_S16,  s_tri_s16, (uint32_t)sizeof(s_tri_s16) },
        { VG_LITE_S32,  s_tri_s32, (uint32_t)sizeof(s_tri_s32) },
        { VG_LITE_FP32, s_tri_f32, (uint32_t)sizeof(s_tri_f32) },
    };
    for (int i = 0; i < 4; i++) {
        const vg_lite_error_t ce = vgc_clear();
        if (ce != VG_LITE_SUCCESS && acc == VG_LITE_SUCCESS) acc = ce;
        vg_lite_path_t p;
        memset(&p, 0, sizeof(p));
        vg_lite_init_path(&p, v[i].f, VG_LITE_HIGH, v[i].n, v[i].d,
                          9.0f, 9.0f, 71.0f, 71.0f);
        vgc_draw_path(&p, VG_LITE_FILL_NON_ZERO, VGC_FILL_COLOR, &acc);
        vgc_finish_into(&acc);
        s_agree[i] = vgc_count_filled(vgc_fb(), VGC_W, VGC_H, VGC_W);
        sums[i]    = vgc_scratch_sum();
    }
    s_agree_sum = vgc_fnv(sums, sizeof(sums));
    return acc;
}

static uint32_t sum_fmt_agreement(void) { return s_agree_sum; }

static vgc_verdict_t check_fmt_agreement(char *d, size_t n)
{
    snprintf(d, n, "s8=%d,s16=%d,s32=%d,fp32=%d",
             s_agree[0], s_agree[1], s_agree[2], s_agree[3]);
    for (int i = 1; i < 4; i++)
        if (s_agree[i] != s_agree[0] || s_agree[i] == 0) return VGC_BROKEN;
    return VGC_OK;
}

/* ---- 12. path/degenerate-zero-area -----------------------------------------
 * A zero-height rect. BOTH outcomes are acceptable -- nothing drawn, or a
 * hairline on the degenerate row -- because the point is that the outcome is
 * DEFINED and RECORDED rather than a crash or a hang. Anything OUTSIDE the
 * degenerate row band is a real defect: it means the rasteriser invented
 * geometry. */

#define DEG_Y 64

static vg_lite_error_t run_degenerate(void)
{
    vg_lite_error_t acc = VG_LITE_SUCCESS;
    vg_lite_path_t p;
    vgc_emit(VLC_OP_MOVE); vgc_emit(R_X);         vgc_emit(DEG_Y);
    vgc_emit(VLC_OP_LINE); vgc_emit(R_X + R_W);   vgc_emit(DEG_Y);
    vgc_emit(VLC_OP_LINE); vgc_emit(R_X + R_W);   vgc_emit(DEG_Y);
    vgc_emit(VLC_OP_LINE); vgc_emit(R_X);         vgc_emit(DEG_Y);
    vgc_emit(VLC_OP_CLOSE);
    { const vg_lite_error_t fe = vgc_finish_path(&p, R_X, DEG_Y, R_X + R_W, DEG_Y);
          if (fe != VG_LITE_SUCCESS) return fe; }
    vgc_draw_path(&p, VG_LITE_FILL_NON_ZERO, VGC_FILL_COLOR, &acc);
    vgc_finish_into(&acc);
    return acc;
}

static vgc_verdict_t check_degenerate(char *d, size_t n)
{
    int ymin = -1, ymax = -1;
    const int fill = vgc_filled_rows(vgc_fb(), VGC_W, VGC_H, VGC_W, &ymin, &ymax);
    snprintf(d, n, "fill=%d,ymin=%d,ymax=%d", fill, ymin, ymax);
    if (fill == 0) return VGC_OK;                       /* nothing drawn: fine */
    return (ymin >= DEG_Y - 1 && ymax <= DEG_Y + 1) ? VGC_OK : VGC_BROKEN;
}

/* ---- the table -------------------------------------------------------------
 * ORDER IS PART OF THE INSTRUMENT. single-contour-rect is first because it is
 * the baseline: if it is broken, nothing below it means anything. The gate and
 * expected_silicon.txt key on these ids, so they are stable -- rename one and
 * both go red, which is the intent. */
const vgc_case_t vgc_path_cases[] = {
    { "path/single-contour-rect",     run_single_rect,       check_single_rect,       NULL },
    { "path/multi-contour-disjoint",  run_multi_contour,     check_multi_contour,     NULL },
    { "path/two-contour-ring-nonzero",run_ring_two_contour,  check_ring,              NULL },
    { "path/two-draws-ring",          run_ring_two_draws,    check_ring,              NULL },
    { "path/evenodd-vs-nonzero",      run_evenodd_nonzero,   check_evenodd_nonzero,   sum_evenodd_nonzero },
    { "path/self-intersecting",       run_self_intersecting, check_self_intersecting, sum_self_intersecting },
    { "path/format-s8",               run_fmt_s8,            check_fmt_s8,            NULL },
    { "path/format-s16",              run_fmt_s16,           check_fmt_s16,           NULL },
    { "path/format-s32",              run_fmt_s32,           check_fmt_s32,           NULL },
    { "path/format-fp32",             run_fmt_f32,           check_fmt_f32,           NULL },
    { "path/format-agreement",        run_fmt_agreement,     check_fmt_agreement,     sum_fmt_agreement },
    { "path/degenerate-zero-area",    run_degenerate,        check_degenerate,        NULL },
};
const size_t vgc_path_case_count =
    sizeof(vgc_path_cases) / sizeof(vgc_path_cases[0]);
```

- [ ] **Step 2: Build**

```bash
cd examples/display/vglite_conformance && cmake --build build
```

Expected: clean build. If `vgc_scratch.memory` is not accepted where `fb()` casts it, that is a real error — fix by casting through `(const uint32_t *)(const void *)vgc_scratch.memory`.

- [ ] **Step 3: Run under QEMU by hand and read the matrix**

```bash
cd examples/display/vglite_conformance && ../../../tools/qrun -M mimxrt1170-evk -global fsl-imxrt1170.boot-xip=on -kernel build/vglite_conformance.elf -display none -serial stdio
```

Expected: 12 `vgc case_begin=` lines, 12 `vgc case=` lines all with `api=skip pixel=skip … repeat=skip`, and:

```
vgc_summary engine=absent cases=12 ok=0 broken=0 skip=12 dangerous=off repeat_differs=0
```

- [ ] **Step 4: Commit**

```bash
git add examples/display/vglite_conformance/vgc_cases_path.cpp
git commit -m "NEW-32: paths/contours/winding conformance cases"
```

#### As-built: what changed, and the third host suite

Shipped as `db4c38a` plus a follow-up landing the host geometry test. Changes from the blocks above:

1. **The FP32 array was wrong** — see the ★ note in the code above. It would have fabricated two BROKENs on the bench.
2. **`check_degenerate` seeds `-99`, not `-1`** — `-1` is the one sentinel `vgc_predicates.h` documents as unusable, and Task 1 measured a broken predicate staying green with it.
3. **`s_fmt_fill[4]` dropped; `check_fmt` counts live pixels** like every other case. That removes four pieces of static state and with them the only route by which one format case could read another's leftovers.
4. **`check_fmt_agreement`'s non-zero test was dead as written** (`!= s_agree[0] || == 0` — equality already implies slot 0 non-zero once the loop passes). Split so each test has one job.
5. `VGC_FINISH_OR_RETURN` replaces the ten inline error blocks; the `return` is in the name so the control flow is visible at the call site.
6. `tests/stub/vg_lite.h` gained `VG_LITE_FP32 = 3` (verified against the real `inc/vg_lite.h`: S8/S16/S32/FP32 really are 0/1/2/3) — the stub now serves two suites and its header says what it models and what each suite must supply for itself.

★ **`path/two-draws-ring` draws twice WITHOUT clearing between, and that is correct** — it reads like a violation of the harness's "a multi-render case must clear itself" rule but is not. That rule concerns sub-renders measured one after another (cases 5, 6, 11); case 4's two draws compose ONE picture and the second is *meant* to land on the first. A ★ note in the file says so, because the obvious "fix" would silently erase the plate the inset punches through.

★ **`tests/cases_path_geom_test.cpp` is the third host suite, and its NEGATIVE arm is the point.** It compiles the REAL `run()`/`check()`/`sum()` functions and the real arena against a scanline reference rasteriser, in two arms:
- **positive** — a correct rasteriser (all contours, both fill rules): all twelve must report `ok`;
- **negative** — a rasteriser re-broken to drop every contour after the first, i.e. THIS GC355's actual defect: `path/multi-contour-disjoint`, `path/two-contour-ring-nonzero` and `path/evenodd-vs-nonzero` must go BROKEN **by name**, and all six controls plus the whole format set must stay `ok`.

The negative arm is what makes the matrix trustworthy: it proves the probe cases can go red on the defect they are aimed at, and that a red there is not a harness artefact. Without it a green positive arm would be equally consistent with a matrix that cannot detect anything. It also settled three numbers the target cannot check — the star's sample points (centre 15.00 px clear of the nearest edge, tip 2.45 px), case 1's ±6 % band against a 320 px perimeter, and that pixel-centre sampling alone costs −1.7 % of the triangle's area, which the ±8 % band must hold *on top of* antialiasing.

★ **It says nothing about what the silicon does.** It exercises this file's geometry against a MODEL of a correct GPU and a MODEL of the known defect. The silicon answer is `transcript_hw_evkb.txt` diffed against `expected_silicon.txt`, and a green host run must never be read as a bench result.

---

### Task 4: Verify the dangerous build compiles (and stays out of `build/`)

**Files:** none modified — this is a build-configuration check.

- [ ] **Step 1: Configure and build the opt-in matrix in its own directory**

```bash
cd examples/display/vglite_conformance
cmake -B build-danger -DVGC_DANGEROUS=ON -DCMAKE_TOOLCHAIN_FILE=../../../toolchain/rt1170-evkb.toolchain.cmake
cmake --build build-danger
```

Expected: clean build, `build-danger/vglite_conformance.elf` produced.

- [ ] **Step 2: Confirm the two builds differ in exactly the expected way**

```bash
cd examples/display/vglite_conformance
../../../tools/qrun -M mimxrt1170-evk -global fsl-imxrt1170.boot-xip=on -kernel build-danger/vglite_conformance.elf -display none -serial stdio | grep -E "vgc_summary|case_begin=path/unterminated"
```

Expected: `cases=13`, `dangerous=on`, and a `vgc case_begin=path/unterminated` line. Then confirm the default build is untouched:

```bash
cd examples/display/vglite_conformance
../../../tools/qrun -M mimxrt1170-evk -global fsl-imxrt1170.boot-xip=on -kernel build/vglite_conformance.elf -display none -serial stdio | grep vgc_summary
```

Expected: `cases=12 … dangerous=off`.

- [ ] **Step 3: Add `build-danger` to the example's ignore surface**

Confirm the repo's existing `.gitignore` already covers `build*` — run `git status --short examples/display/vglite_conformance/` and check neither build directory is listed as untracked. If either appears, add `build*/` to the repo `.gitignore` in the same style as the existing entries.

- [ ] **Step 4: Commit (only if `.gitignore` changed)**

```bash
git add .gitignore && git commit -m "NEW-32: ignore vglite_conformance build-danger dir"
```

---

### Task 5: The QEMU gate

**Files:**
- Create: `examples/display/vglite_conformance/run_qemu.sh`

- [ ] **Step 1: Write the gate**

Create `examples/display/vglite_conformance/run_qemu.sh`:

```sh
#!/bin/sh
set -e
DIR=$(cd "$(dirname "$0")" && pwd)
# Tools come from THIS checkout, derived from the gate's own location (a
# hardcoded path silently loads a different tree's gate-lib.sh from a worktree).
EVKB=$(cd "$DIR/../../.." && pwd)
QEMU="$EVKB/tools/qrun"
. "$EVKB/tools/gate-lib.sh"
gate_init
ELF="$DIR/$(gate_build_dir)/vglite_conformance.elf"
# Run artifacts go through gate_capture_path, never "$DIR/<name>".
OUT=$(gate_capture_path "$DIR" vglite_conformance.uart)
DBG=$(gate_capture_path "$DIR" vglite_conformance.dbg)
rm -f "$OUT"
"$QEMU" $(gate_qemu_machine) -kernel "$ELF" \
    -display none $(gate_console "$OUT") -d guest_errors -D "$DBG" &
P=$!; gate_pid $P
# 10s. This example brings up NO PANEL (spec section 4: no scanout, no LVGL),
# so it has none of vglite_probe's RK055 margin to allow for -- every token
# lands inside setup(), ~1s in on an idle machine.
sleep 10; gate_reap $P
gate_require_capture "$OUT"
echo "==== captured UART ===="; cat "$OUT"

# ★ WHAT THIS GATE PROVES, AND WHAT IT DOES NOT.
#
# QEMU has no GC355 model, so this asserts the HONEST NEGATIVE: the firmware
# asks whether a GPU is present, is told no, and reports every case as skip --
# cleanly, without hanging, and WITHOUT CLAIMING ANY CASE PASSED. It proves
# nothing about whether the GC355 renders these cases correctly; that answer is
# silicon-only and lives in transcript_hw_evkb.txt, checked against
# expected_silicon.txt by tools/vglite-conformance-check.sh.
# See docs/KNOWN-BROKEN-GATES.md and the sibling display/vglite_probe gate.

grep -q "VGC_BEGIN" "$OUT" || { echo "FAIL: banner missing"; exit 1; }
# Value greps are ANCHORED (CR-tolerant \r?$): a partial match is not a match.
grep -qE "vgc_chip_id=0x00000000\r?$" "$OUT" || \
    { echo "FAIL: chip-ID probe missing or non-zero under QEMU"; exit 1; }
grep -qE "vgc_engine=absent reason=no_chip_id\r?$" "$OUT" || \
    { echo "FAIL: expected vgc_engine=absent under QEMU (no GC355 model)"; exit 1; }

# ★ THE TRIPWIRE THIS GATE EXISTS FOR. With no GPU present, no case may claim
# it passed and no vg_lite call may claim it succeeded. Without these two
# lines the gate would go green against a harness that reports `ok` for
# everything it never ran -- which is precisely the vacuous-pass shape this
# tree's gates are written to refuse.
# Demonstrated RED 2026-08-30: a fabricated
#   `vgc case=path/single-contour-rect api=success pixel=ok detail=x repeat=same`
# line appended to the capture -> "FAIL: TRIPWIRE a case reported ok in QEMU".
grep -q "pixel=ok" "$OUT" && { echo "FAIL: TRIPWIRE a case reported ok in QEMU"; exit 1; }
grep -q "api=success" "$OUT" && { echo "FAIL: TRIPWIRE an API call succeeded in QEMU"; exit 1; }
grep -q "pixel=broken" "$OUT" && { echo "FAIL: TRIPWIRE a case reported broken in QEMU"; exit 1; }

# The matrix must be NON-EMPTY and COMPLETE. A zero case count would satisfy
# every tripwire above vacuously (the harness skeleton did exactly that before
# the case table landed), and a truncated capture would look like a smaller
# matrix that happened to pass. Both are refused here: the case-line count is
# compared against the summary's own cases= field, and against the expected 12.
CASES=$(grep -a -c "^vgc case=" "$OUT" || true)
BEGINS=$(grep -a -c "^vgc case_begin=" "$OUT" || true)
[ "$CASES" -eq 12 ] || { echo "FAIL: expected 12 case lines, got $CASES"; exit 1; }
[ "$BEGINS" -eq "$CASES" ] || \
    { echo "FAIL: $BEGINS case_begin lines but $CASES case lines (a case did not finish)"; exit 1; }

# Every Phase-1 case id must be PRESENT BY NAME. A renamed or silently dropped
# case would keep the count assertion green only if another appeared in its
# place, but a table that shrinks and grows in the same commit is exactly the
# drift this names.
for id in path/single-contour-rect path/multi-contour-disjoint \
          path/two-contour-ring-nonzero path/two-draws-ring \
          path/evenodd-vs-nonzero path/self-intersecting \
          path/format-s8 path/format-s16 path/format-s32 path/format-fp32 \
          path/format-agreement path/degenerate-zero-area; do
    grep -qE "^vgc case=$id api=skip pixel=skip " "$OUT" || \
        { echo "FAIL: missing/wrong case line for $id"; exit 1; }
done

# The summary must AGREE with the case lines, and must say the default build.
# Demonstrated RED 2026-08-30: cases=12 edited to cases=11 in the capture ->
# "FAIL: summary line missing or disagrees with the case lines".
grep -qE "^vgc_summary engine=absent cases=12 ok=0 broken=0 skip=12 dangerous=off repeat_differs=0\r?$" "$OUT" || \
    { echo "FAIL: summary line missing or disagrees with the case lines"; exit 1; }

# A bounded wait that gave up means the completion path is wrong even when the
# outcome looks right. Under QEMU no wait should ever run.
grep -qE "vgc_timeouts=0 vgc_irqs=0\r?$" "$OUT" || \
    { echo "FAIL: a bounded wait timed out or an IRQ fired with no GPU"; exit 1; }
grep -q "VGC_DONE" "$OUT" || { echo "FAIL: no completion token"; exit 1; }
echo "PASS: VGLite conformance harness negative verified"
```

```bash
chmod +x examples/display/vglite_conformance/run_qemu.sh
```

- [ ] **Step 2: Run the gate**

Run: `cd examples/display/vglite_conformance && ./run_qemu.sh`

Expected: the captured UART, then `PASS: VGLite conformance harness negative verified`, exit 0.

- [ ] **Step 3: Demonstrate both new assertions RED — by replaying a doctored capture through the REAL gate**

Do NOT edit the gate to point it at a file. `tools/qrun`'s `REAL_QEMU` hook is the tree's mechanism for exactly this: a fake QEMU writes a chosen capture to the gate's own `-serial file:` target, so the UNMODIFIED gate runs against it. (This is the same harness `gate-vacuity.test.sh` uses, and Task 6 turns these two demonstrations into permanent cases there.)

Create the fake once:

```bash
cat > /tmp/vgc-fake-qemu <<'FAKE'
#!/bin/sh
target=""; prev=""
for a in "$@"; do
    case "$a" in file:*) [ "$prev" = "-serial" ] && target="${a#file:}" ;; esac
    prev="$a"
done
[ -n "$target" ] && cat "$FAKE_CAPTURE" > "$target"
sleep 300
FAKE
chmod +x /tmp/vgc-fake-qemu
```

Tripwire — a fabricated passing verdict:

```bash
cd examples/display/vglite_conformance
cp build/vglite_conformance.uart /tmp/vgc_tamper.uart
echo "vgc case=path/single-contour-rect api=success pixel=ok detail=fill=6400 repeat=same" >> /tmp/vgc_tamper.uart
REAL_QEMU=/tmp/vgc-fake-qemu FAKE_CAPTURE=/tmp/vgc_tamper.uart GATE_TIMEOUT=120 QRUN_TIMEOUT=40 ./run_qemu.sh; echo "rc=$?"
```

Expected: `FAIL: TRIPWIRE a case reported ok in QEMU`, `rc=1`.

- [ ] **Step 4: Demonstrate the count assertion RED**

```bash
cd examples/display/vglite_conformance
grep -v "^vgc case=path/degenerate-zero-area" build/vglite_conformance.uart > /tmp/vgc_short.uart
REAL_QEMU=/tmp/vgc-fake-qemu FAKE_CAPTURE=/tmp/vgc_short.uart GATE_TIMEOUT=120 QRUN_TIMEOUT=40 ./run_qemu.sh; echo "rc=$?"
```

Expected: `FAIL: expected 12 case lines, got 11`, `rc=1`.

Then confirm the gate is still green for real (no `REAL_QEMU` in the environment):

```bash
cd examples/display/vglite_conformance && ./run_qemu.sh | tail -1
```

Expected: `PASS: VGLite conformance harness negative verified`.

- [ ] **Step 5: Capture the committed fixture**

The fixture must come from a real gate run, AFTER the gate is final — a fixture captured before a gate's assertions settle goes stale silently and only `gate-vacuity.test.sh` can see it.

```bash
cd examples/display/vglite_conformance
./run_qemu.sh
cp build/vglite_conformance.uart transcript_qemu.txt
```

- [ ] **Step 6: Commit**

```bash
git add examples/display/vglite_conformance/run_qemu.sh \
        examples/display/vglite_conformance/transcript_qemu.txt
git commit -m "NEW-32: vglite_conformance QEMU gate (honest negative + ok tripwire)"
```

---

### Task 6: Register the gate — sweep 123 → 124, licence audit, vacuity cases

**Files:**
- Modify: `tools/license-audit.sh` (the `GATES` list)
- Modify: `tools/gate-vacuity.test.sh` (append a new section)
- Modify: `CLAUDE.md`

- [ ] **Step 1: Confirm the runner discovers the new gate**

```bash
./tools/run-all-qemu-gates.sh -l | tail -3
```

Expected: `(124 gate(s))`, and `rt1176:display/vglite_conformance` present in the listing.

- [ ] **Step 2: Add the GATES entry and confirm the drift check was the thing that would have caught its absence**

First prove the check fires. Run:

```bash
LICENSE_AUDIT_EVKB=$(pwd) ./tools/license-audit.sh
```

Expected: `GATES DRIFT: examples/display/vglite_conformance has a run_qemu.sh but is missing from the Part-2 GATES list`.

Now add the entry. In `tools/license-audit.sh`, in the `GATES` list, insert immediately after the `examples/display/vglite_lvgl_test:vglite_lvgl_test \` line:

```
examples/display/vglite_conformance:vglite_conformance \
```

- [ ] **Step 3: Re-run the audit**

```bash
LICENSE_AUDIT_EVKB=$(pwd) ./tools/license-audit.sh
```

Expected: `LICENSE-AUDIT: PASS`, with a line showing `examples/display/vglite_conformance` walked at a non-zero dep-path count.

- [ ] **Step 4: Add three vacuity cases**

In `tools/gate-vacuity.test.sh`, insert immediately BEFORE the final `exit $FAILED`:

```sh
# --- 9. vglite_conformance: green fixture passes; a fabricated ok verdict and
# a truncated matrix both fail by name (NEW-32).  The ok tripwire is the whole
# reason this gate exists -- QEMU has no GC355, so a harness that reported `ok`
# for cases it never ran would sail through every other assertion. The
# truncated-matrix case pins the count check, which is what stops a short
# capture reading as a smaller matrix that happened to pass.
VGC="examples/display/vglite_conformance"
if [ -d "$EVKB/$VGC" ] && [ -f "$EVKB/$VGC/transcript_qemu.txt" ]; then
    run_gate "$VGC" "run_qemu.sh" "$EVKB/$VGC/transcript_qemu.txt"; rc=$?
    [ "$rc" -eq 0 ] && result=0 || result=1
    report "green_still_passes_vglite_conformance" $result

    # A fabricated passing verdict must fail BY NAME even though every genuine
    # assertion still passes on the rest of the capture.
    cp "$EVKB/$VGC/transcript_qemu.txt" "$WORK/vgc_ok.txt"
    echo "vgc case=path/single-contour-rect api=success pixel=ok detail=fill=6400 repeat=same" \
        >> "$WORK/vgc_ok.txt"
    run_gate "$VGC" "run_qemu.sh" "$WORK/vgc_ok.txt"; rc=$?
    result=0
    [ "$rc" -ne 0 ] || result=1                                          # must not pass
    echo "$OUT_TEXT" | grep -q "a case reported ok in QEMU" || result=1  # and name it
    report "vgc_ok_tripwire_fires" $result

    # A capture missing one case line must fail as a short matrix, not pass as
    # a smaller one.
    grep -v "^vgc case=path/degenerate-zero-area" \
        "$EVKB/$VGC/transcript_qemu.txt" > "$WORK/vgc_short.txt"
    run_gate "$VGC" "run_qemu.sh" "$WORK/vgc_short.txt"; rc=$?
    result=0
    [ "$rc" -ne 0 ] || result=1
    echo "$OUT_TEXT" | grep -q "expected 12 case lines, got 11" || result=1
    report "vgc_truncated_matrix_fails" $result
else
    echo "SKIP: vglite_conformance vacuity (example or fixture missing)"
fi

```

- [ ] **Step 5: Run the vacuity suite**

```bash
sh tools/gate-vacuity.test.sh
```

Expected: the three new cases report `PASS:`, and the suite's total is **28** `PASS:` lines (25 before this task). Verify with:

```bash
sh tools/gate-vacuity.test.sh | grep -c "^PASS:"
```

Expected: `28`. If it is not, count the `report` call sites (`grep -c "^    report\|^report" tools/gate-vacuity.test.sh`) and reconcile — a pass count in this tree is a claim like any other and gets re-derived from a run, never carried in from a summary.

- [ ] **Step 6: Update CLAUDE.md**

Three edits, all in the "Test / verify — the two-gate rule" section:

1. Change `The sweep covers **123 gates**` to `The sweep covers **124 gates**`, and change the trailing `That arithmetic is CHECKED against the runner rather than trusted: `-l` reports 123.` to `… reports 124.`

2. Immediately after the `NEW-23 added ONE — display/synthui_fader_test …` paragraph block (after its final `★` bullet), insert:

```markdown
NEW-32 Phase 1 added ONE — `display/vglite_conformance`, the GC355/VGLite
conformance harness (spec 2026-08-30): a case table of `{id, run, check}`
triples rendering one at a time into a 128×128 BGRA8888 EXTMEM scratch, each
case printing TWO INDEPENDENT VERDICTS — `api=` (what the driver said) and
`pixel=` (what a structural CPU-side predicate found) — because every GC355
defect this tree has hit reported success while producing the wrong picture.
Phase 1 is the twelve paths/contours/winding cases. Its gate asserts the
HONEST NEGATIVE (`vgc_engine=absent`, all twelve `pixel=skip`) with three
tripwires — no case may report `pixel=ok`, none may report `pixel=broken`, and
no `api=success` may appear with no GPU — plus a case-line COUNT check, since
every tripwire above is satisfied vacuously by an empty matrix. 123 before it.
★ **No panel.** The core's startup brings up the SEMC SDRAM before `setup()`,
so EXTMEM is live without `Display.begin()`; this is the only display example
that links neither MipiDisplay nor LVGL.
★ **The tessellation buffer is 64×64 against a 128×128 target, deliberately.**
A tess buffer ≥ the target puts the driver in its `ts_is_fullscreen == 1`
regime, where scissor left/top clamping is silently disabled — a different
machine from the one the shipping compositors run on (720×1280 target, 256×256
tess). Phase 3's `scissor/tess-fullscreen` case probes the other regime on
purpose.
★ The silicon matrix is ONE boot, diffed against the PRE-REGISTERED
`expected_silicon.txt` by `tools/vglite-conformance-check.sh`, which fails on
drift in EITHER direction: a quirk that silently DISAPPEARS after an SDK bump
matters as much as a new one, because it means the driver changed under us.
`docs/gc355-vglite-quirks.md` is the reference the matrix feeds.
```

3. In the "Architecture" section's VGLite paragraph, after the `★★ **This GC355/driver renders ONLY THE FIRST CONTOUR …**` block, append:

```markdown
  ★ **`docs/gc355-vglite-quirks.md` is the reference for all of this** — one
  row per feature (verdict · safe usage · evidence), every row citing the
  `display/vglite_conformance` case id that establishes it, so a claim without
  a probe case is visibly a claim without evidence.
```

- [ ] **Step 7: Commit**

```bash
git add tools/license-audit.sh tools/gate-vacuity.test.sh CLAUDE.md
git commit -m "NEW-32: register vglite_conformance gate (sweep 124), audit + vacuity cases"
```

---

### Task 7: `expected_silicon.txt` (pre-registered) and the drift checker

The expectations file is written BEFORE the bench run. That ordering is the point: a file written afterwards is a transcription of whatever the board printed, which is exactly the rubber-stamp hazard spec §11 names.

**Files:**
- Create: `examples/display/vglite_conformance/expected_silicon.txt`
- Create: `tools/vglite-conformance-check.sh`

- [ ] **Step 1: Write the pre-registered expectations**

Create `examples/display/vglite_conformance/expected_silicon.txt`:

```
# expected_silicon.txt -- the committed expectation for the GC355 conformance
# matrix. One line per case: <id> <pixel-verdict> <repeat>  # reason
#
# ★ PRE-REGISTERED. Every line below was written BEFORE the first silicon boot,
# from the design's predictions (spec section 5), so the bench run CONFIRMS or
# REFUTES a stated expectation rather than defining one. A file written after
# the fact is a transcription, not a check.
#
# ★ EVERY CHANGE TO THIS FILE CARRIES A REASON, and a verdict flipping to `ok`
# must be EXPLAINED -- did the driver change, or did our usage? -- rather than
# merely accepted. Same hazard as re-goldening a checksum (spec section 11).
#
# tools/vglite-conformance-check.sh diffs a transcript against this and fails
# on drift in EITHER direction: a quirk that silently DISAPPEARS after an SDK
# re-vendor matters as much as a new one appearing, because it means the safe
# usage we built on can be simplified -- and more importantly that the driver
# moved under us.

path/single-contour-rect      ok     same  # the baseline; a broken control invalidates the whole matrix
path/multi-contour-disjoint   broken same  # only the first contour renders: expect runs=1, not 4
path/two-contour-ring-nonzero broken same  # first-contour-only: the reversed inner contour is dropped, the ring fills solid
path/two-draws-ring           ok     same  # SAFE USAGE control for the case above -- plate + inset plate, two single-contour draws
path/evenodd-vs-nonzero       broken same  # the EVEN_ODD hole needs the second contour, which is dropped; nz half is expected right
path/self-intersecting        ok     same  # ONE contour, so first-contour-only does not bite; both fill rules must be honoured
path/format-s8                ok     same
path/format-s16               ok     same
path/format-s32               ok     same
path/format-fp32              ok     same
path/format-agreement         ok     same  # the four formats must agree with each other, not merely each be plausible
path/degenerate-zero-area     ok     same  # either nothing drawn or a hairline on row 64 +/-1; anything else is invented geometry
```

- [ ] **Step 2: Write the checker**

Create `tools/vglite-conformance-check.sh`:

```sh
#!/bin/sh
# vglite-conformance-check.sh -- diff a GC355 conformance transcript against
# the committed expectation, and fail on drift in EITHER direction.
#
#   tools/vglite-conformance-check.sh <transcript> [expected]
#
# Default expected: examples/display/vglite_conformance/expected_silicon.txt
#
# ★ DRIFT IN EITHER DIRECTION. A new `broken` is obviously a finding. So is a
# quirk that silently turns `ok` after an SDK re-vendor or a driver bump: it
# means the safe usage two compositors are built on can be simplified, and more
# importantly that the driver changed under us without anyone deciding to. A
# checker that only looked for regressions would report that as health.
#
# ★ A CASE PRESENT IN ONE FILE AND NOT THE OTHER IS ALSO DRIFT. A matrix that
# quietly shrinks passes any per-case comparison vacuously -- the same way a
# SKIP hides in a gate count.
set -e
[ $# -ge 1 ] || { echo "usage: $0 <transcript> [expected]" >&2; exit 2; }
TRANSCRIPT=$1
HERE=$(cd "$(dirname "$0")/.." && pwd)
EXPECTED=${2:-$HERE/examples/display/vglite_conformance/expected_silicon.txt}

[ -s "$TRANSCRIPT" ] || { echo "FAIL: no transcript at $TRANSCRIPT"; exit 1; }
[ -s "$EXPECTED" ]   || { echo "FAIL: no expectation file at $EXPECTED"; exit 1; }

WORK=$(mktemp -d "${TMPDIR:-/tmp}/vgc-check.XXXXXX") || exit 2
trap 'rm -rf "$WORK"' EXIT INT TERM HUP

# "<id> <pixel> <repeat>" from the transcript's case lines.
grep -a "^vgc case=" "$TRANSCRIPT" \
  | sed -E 's/^vgc case=([^ ]+) .*pixel=([a-z]+) .*repeat=([a-z]+).*$/\1 \2 \3/' \
  | sort > "$WORK/actual"
# The same, from the expectation file (comments and blank lines dropped).
sed -e 's/#.*$//' -e '/^[[:space:]]*$/d' "$EXPECTED" \
  | awk '{ print $1, $2, $3 }' \
  | sort > "$WORK/expected"

[ -s "$WORK/actual" ] || { echo "FAIL: no 'vgc case=' lines in $TRANSCRIPT"; exit 1; }

# The engine must be gpu: an absent-engine transcript is all-skip and would
# "differ" from every line for a reason that is not drift.
grep -qa "^vgc_engine=gpu " "$TRANSCRIPT" || \
    { echo "FAIL: $TRANSCRIPT is not a GPU run (no 'vgc_engine=gpu' line)"; exit 1; }

if diff -u "$WORK/expected" "$WORK/actual" > "$WORK/diff"; then
    N=$(wc -l < "$WORK/actual" | tr -d ' ')
    echo "VGLITE-CONFORMANCE: PASS ($N cases match $EXPECTED)"
    exit 0
fi

echo "VGLITE-CONFORMANCE: DRIFT"
echo "  -- is the expectation, ++ is $TRANSCRIPT"
cat "$WORK/diff"
echo
echo "Do NOT paste the transcript over the expectation. Work out WHICH changed:"
echo "  a verdict -> broken : a new quirk, or our usage regressed"
echo "  a verdict -> ok     : the driver changed under us -- say WHY in the reason"
echo "  repeat -> differs   : nondeterminism, a finding in its own right"
echo "  a case appearing/vanishing : the table moved; the gate's id list must agree"
exit 1
```

```bash
chmod +x tools/vglite-conformance-check.sh
```

- [ ] **Step 3: Demonstrate the checker RED and GREEN against a synthetic transcript**

Build a synthetic "silicon" transcript matching the expectation exactly:

```bash
cd /Users/nicholasnewdigate/Development/rt1170/evkb
{ echo "vgc_engine=gpu target=128x128 fmt=1 tess=64x64"
  sed -e 's/#.*$//' -e '/^[[:space:]]*$/d' examples/display/vglite_conformance/expected_silicon.txt \
  | awk '{ printf "vgc case=%s api=success pixel=%s detail=x repeat=%s\n", $1, $2, $3 }'
} > /tmp/vgc_synth.txt
./tools/vglite-conformance-check.sh /tmp/vgc_synth.txt
```

Expected: `VGLITE-CONFORMANCE: PASS (12 cases match …)`, exit 0.

Now three RED demonstrations:

```bash
# a. a verdict flips to broken
sed 's|^vgc case=path/two-draws-ring api=success pixel=ok|vgc case=path/two-draws-ring api=success pixel=broken|' /tmp/vgc_synth.txt > /tmp/vgc_drift1.txt
./tools/vglite-conformance-check.sh /tmp/vgc_drift1.txt; echo "rc=$?"
```
Expected: `VGLITE-CONFORMANCE: DRIFT`, the diff naming `path/two-draws-ring`, `rc=1`.

```bash
# b. a known quirk silently turns ok -- must ALSO be drift
sed 's|^vgc case=path/multi-contour-disjoint api=success pixel=broken|vgc case=path/multi-contour-disjoint api=success pixel=ok|' /tmp/vgc_synth.txt > /tmp/vgc_drift2.txt
./tools/vglite-conformance-check.sh /tmp/vgc_drift2.txt; echo "rc=$?"
```
Expected: `VGLITE-CONFORMANCE: DRIFT` naming `path/multi-contour-disjoint`, `rc=1`.

```bash
# c. the matrix shrinks
grep -v "path/degenerate-zero-area" /tmp/vgc_synth.txt > /tmp/vgc_drift3.txt
./tools/vglite-conformance-check.sh /tmp/vgc_drift3.txt; echo "rc=$?"
```
Expected: `VGLITE-CONFORMANCE: DRIFT` showing the missing line, `rc=1`.

```bash
# d. and the absent-engine capture is refused rather than diffed
./tools/vglite-conformance-check.sh examples/display/vglite_conformance/transcript_qemu.txt; echo "rc=$?"
```
Expected: `FAIL: … is not a GPU run (no 'vgc_engine=gpu' line)`, `rc=1`.

- [ ] **Step 4: Commit**

```bash
git add examples/display/vglite_conformance/expected_silicon.txt \
        tools/vglite-conformance-check.sh
git commit -m "NEW-32: pre-registered silicon expectations + drift checker"
```

---

### Task 8: `docs/gc355-vglite-quirks.md` skeleton

**Files:**
- Create: `docs/gc355-vglite-quirks.md`

- [ ] **Step 1: Write the document**

Create `docs/gc355-vglite-quirks.md`:

```markdown
# GC355 / VGLite quirks — verdict · safe usage · evidence

The reference for what this board's Vivante GC355 GPU2D, driven by this tree's
vendored NXP VGLite driver and bare-metal port, actually does — as opposed to
what its API says it does.

**Every row cites the conformance case id that establishes it.** A row with no
case id is a claim without evidence, and is marked as such. Re-run the evidence
with one bench boot:

```sh
cd examples/display/vglite_conformance
cmake --build build
# flash, press SW4, capture -> transcript_hw_evkb.txt
../../../tools/vglite-conformance-check.sh transcript_hw_evkb.txt
```

- Probe: `examples/display/vglite_conformance` (spec:
  `docs/superpowers/specs/2026-08-30-gc355-conformance-design.md`).
- Expectation: `examples/display/vglite_conformance/expected_silicon.txt`
  (pre-registered; changes carry a reason).
- Checker: `tools/vglite-conformance-check.sh` — fails on drift in EITHER
  direction, because a quirk that quietly disappears means the driver moved
  under us.
- The prior question — *does the GPU initialise and render at all* — is
  `examples/display/vglite_probe`.

**Silicon status: NOT YET MEASURED.** The verdict column below carries the
PRE-REGISTERED expectation. It is replaced with the measured answer, and the
evidence column with the transcript date, after the Phase 1 bench boot.

## Paths, contours & winding — Phase 1

| Feature | Verdict | Safe usage | Evidence |
|---|---|---|---|
| A single-contour filled path | expected OK | The only path shape to rely on. | `path/single-contour-rect` |
| Several disjoint contours in ONE path | expected BROKEN | **One contour per path, one `vg_lite_draw` per contour.** Every subpath after the first `VLC_OP_MOVE` is dropped. | `path/multi-contour-disjoint` |
| A hole cut by a reversed inner contour (non-zero) | expected BROKEN | Don't. Draw a filled plate, then an inset plate in the backdrop colour — two single-contour draws. | `path/two-contour-ring-nonzero`, control `path/two-draws-ring` |
| `VG_LITE_FILL_EVEN_ODD` vs `VG_LITE_FILL_NON_ZERO` across nested contours | expected BROKEN | The fill rule cannot rescue a dropped contour; the rules differ only where a second contour survives, which it does not. | `path/evenodd-vs-nonzero` |
| Fill rules on ONE self-intersecting contour | expected OK | Both rules honoured — a pentagram's centre is empty under EVEN_ODD, filled under NON_ZERO. This is the fill-rule usage that works. | `path/self-intersecting` |
| Path coordinate formats S8 / S16 / S32 / FP32 | expected OK | All four usable; opcodes are stored at the format's element width, so each format needs its own typed array. | `path/format-s8`, `-s16`, `-s32`, `-fp32`, `path/format-agreement` |
| Degenerate (zero-area) geometry | expected OK | Safe to emit; the outcome is nothing or a hairline on the degenerate row. Do not rely on which. | `path/degenerate-zero-area` |
| A path with no `VLC_OP_END` | UNPROBED in the default build | Never emit one. Refuse a truncated path rather than drawing it. | `path/unterminated` (opt-in `-DVGC_DANGEROUS=ON` build only — spec §6) |

### Known-but-not-yet-probed (carried from the compositors' ★ comments)

These are recorded here so the reference is complete, and marked so that a
claim with no case id is visibly a claim without evidence.

| Quirk | How it presented | Case id |
|---|---|---|
| Command buffer must be 64-byte aligned | Front end hangs; every call returns `VG_LITE_SUCCESS` | *(none — a driver-internal allocation; see spec §6)* |
| The target must be `vg_lite_map`'d | Every draw "succeeds" and changes nothing | *(none — the harness would not run at all without it)* |
| Waits could consume stale IRQ flags | A "successful" wait that never waited | *(none — fixed in the port, VGLite 2e17773; `vgc_timeouts=`/`vgc_irqs=` witness it)* |

## Gradients & colour — Phase 2

**Not yet probed.** Spec §5 lists the cases: `grad/legacy-linear`,
`grad/ext-linear-static`, `grad/ext-linear-moved`, `grad/ramp-word-order`,
`color/solid-word-order`, `color/premultiplied-srcover`, `blend/modes`.

What the compositors currently assert without a probe case, and which Phase 2
must confirm or refute:

- Legacy `vg_lite_draw_grad` is GC255-only; on GC355 it rendered solid black
  with a per-boot-varying checksum while every call returned success.
- The EXT linear-gradient ramp is PLACEMENT-DEPENDENT:
  `vg_lite_update_linear_grad` overwrites both `grad->matrix` and
  `grad->linear_grad` and allocates a new ramp surface without freeing the
  previous one, so a moving widget cannot cache a ramp.
- `vg_lite_color_t` is ABGR (red in the low byte) while the ramp image's words
  are ARGB.
- `vg_lite_set_grad` returns success with `count=0` and silently substitutes a
  black→white ramp.

## Images, blits & scissor — Phase 3

**Not yet probed.** Spec §5 lists: `blit/basic`, `blit/stride-64`,
`blit/stride-unaligned`, `blit/formats`, `scissor/basic`,
`scissor/tess-fullscreen`.

Note that Phase 1 runs with a tessellation buffer SMALLER than its target
(64×64 against 128×128), deliberately, so it is in the multi-tile regime the
shipping compositors use. `scissor/tess-fullscreen` is the case that probes the
other regime — the one in which the driver's left/top scissor clamping is
skipped because `ts_is_fullscreen != 0`.

## Guard layer — Phase 4

**Not yet built.** `VGLite/port/vglite_guard.h` will enforce, in our port code
(never in the vendored driver), only what the probe CONFIRMED — which is why it
comes last. See spec §8.
```

- [ ] **Step 2: Cross-link from the VGLite library README**

Append to `~/Development/VGLite/README.md`:

```markdown
## Behaviour on the RT1176's GC355

This driver's API reports success for several things this silicon does not do.
`rt1176-evkb`'s `docs/gc355-vglite-quirks.md` is the reference — one row per
feature, each citing the `display/vglite_conformance` probe case that
establishes it.
```

- [ ] **Step 3: Commit**

```bash
git add docs/gc355-vglite-quirks.md
git commit -m "NEW-32: gc355-vglite-quirks.md skeleton (Phase 1 rows)"
```

(The VGLite README change is in a sibling repo; commit it there:
`git -C ~/Development/VGLite add README.md && git -C ~/Development/VGLite commit -m "docs: point at rt1176-evkb's GC355 quirks reference"`.)

---

### Task 9: Full sweep, audit, vacuity — the pre-bench verification

Nothing goes to the bench until the tree is green, because a red sweep during a
bench session is indistinguishable from something the bench work broke.

- [ ] **Step 1: Run the full sweep**

The sweep must run through a SHORT-PATH SYMLINK if this checkout's path is long
(four gates open `mon.sock`, and macOS caps `sun_path` at 104 bytes). This
checkout is `~/Development/rt1170/evkb` (93 bytes), which fits — run it in
place. Check first, and check where any existing symlink points before using
one:

```bash
echo -n "/Users/nicholasnewdigate/Development/rt1170/evkb/examples/dualcore/cm4_usb_irq_probe/mon.sock" | wc -c
```

Expected: under 104. Then:

```bash
cd /Users/nicholasnewdigate/Development/rt1170/evkb && ./tools/run-all-qemu-gates.sh 2>&1 | tail -30
```

Expected: `gates: 124 passed` (exit 0), or `123 passed, 1 failed` if the failure
is `m2_hci_probe[hci]` — that gate's build dir carries the BT bench workstream's
cache variables and is red BY DESIGN on this machine until that dir returns to
the gate configuration (`grep -E "M2_BT_UART_DNLD|M2RADIO_IW416_BT_FW" examples/networking/m2_hci_probe/build/CMakeCache.txt` confirms it). Any OTHER
failure is a real regression from this work — read the gate NAMES, not the count.

- [ ] **Step 2: Run the licence audit**

```bash
LICENSE_AUDIT_EVKB=$(pwd) ./tools/license-audit.sh 2>&1 | tail -20
```

Expected: `LICENSE-AUDIT: PASS`, with `examples/display/vglite_conformance` in
the walked list.

- [ ] **Step 3: Run the vacuity suite and the predicate unit test**

```bash
sh tools/gate-vacuity.test.sh; echo "vacuity rc=$?"
./examples/display/vglite_conformance/tests/run.sh
```

Expected: vacuity `rc=0` with 28 `PASS:` lines; `predicates_test: OK`.

- [ ] **Step 4: Commit any fixups**

```bash
git add -A && git commit -m "NEW-32: pre-bench verification fixups"
```

(Skip if nothing changed.)

---

### Task 10: The single silicon boot

ONE boot for the whole matrix. The operator presses SW4 by hand, so every step
below is ordered to make that exactly one press.

**Files:**
- Create: `examples/display/vglite_conformance/transcript_hw_evkb.txt`
- Possibly modify: `examples/display/vglite_conformance/expected_silicon.txt`

- [ ] **Step 1: Clear stale probe daemons and flash — WITH NO SERIAL READER ATTACHED**

★ Do NOT hold the VCOM while programming: `LinkServer` dies with
`request to clear DAP error failed - status 131` and the port re-enumerates
mid-attempt. Nothing may hold `/dev/cu.usbmodem*` for this step.

```bash
pkill LinkServer; pkill redlinkserv; pkill crt_emu_cm_redlink
cd /Users/nicholasnewdigate/Development/rt1170/evkb/examples/display/vglite_conformance
/Applications/LinkServer_26.6.137/LinkServer flash MIMXRT1176:MIMXRT1170-EVKB load build/vglite_conformance.elf
/Applications/LinkServer_26.6.137/LinkServer flash MIMXRT1176:MIMXRT1170-EVKB verify build/vglite_conformance.elf
```

Expected: both report success. `LinkServer` is silent for a minute or more
while it programs — silent is not hung.

- [ ] **Step 2: Attach the console reader and ASK THE OPERATOR FOR ONE SW4 PRESS**

```bash
ls /dev/cu.usbmodem*
```

then, with the correct port:

```bash
python3 ../../../tools/rt1170-console.py /dev/cu.usbmodem<PORT> 115200 | tee build/hw.uart
```

**Tell the operator, verbatim:** "Reader attached. **Press SW4 once now.**
The whole matrix runs in one boot and finishes in about a second; you'll see
`VGC_DONE` followed by a `vgc_hb` heartbeat every 5 s. One press is all that's
needed — don't press again."

Wait for `VGC_DONE`, then let two heartbeats print (proof the image is alive
and not merely finished), then Ctrl-C the reader.

★ If the DAP wedges on a later attempt (`DAPInfo`/`Wire not connected` errors
while the VCOM still works), replug the DEBUG USB — a board power cycle does
NOT clear it. Never `pkill -9` LinkServer mid-flash-program: that has left this
target unreachable at the wire level, needing a full power cycle.

- [ ] **Step 3: Save the transcript**

```bash
cd /Users/nicholasnewdigate/Development/rt1170/evkb/examples/display/vglite_conformance
cp build/hw.uart transcript_hw_evkb.txt
```

Sanity checks before believing anything in it:

```bash
grep -E "^vgc_engine=|^vgc_summary|^vgc_timeouts=" transcript_hw_evkb.txt
grep -c "^vgc case=" transcript_hw_evkb.txt
```

Expected: `vgc_engine=gpu …`, `cases=12`, `dangerous=off`, and 12 case lines.
If `vgc_timeouts=` is non-zero, the completion path is wrong even where the
pixels look right — that is a finding, not a nuisance, and it goes in the doc.

- [ ] **Step 4: Diff against the pre-registered expectation**

```bash
cd /Users/nicholasnewdigate/Development/rt1170/evkb
./tools/vglite-conformance-check.sh examples/display/vglite_conformance/transcript_hw_evkb.txt
```

Two outcomes, and BOTH are results:

- `VGLITE-CONFORMANCE: PASS` — the design's predictions were right; the quirks
  doc's "expected" verdicts become measured verdicts unchanged.
- `VGLITE-CONFORMANCE: DRIFT` — a prediction was wrong. **Do not paste the
  transcript over the expectation.** For each drifting line work out whether
  the PREDICATE is wrong (check the paired control: if `path/single-contour-rect`
  or `path/two-draws-ring` is broken, suspect the harness, not the silicon —
  spec §11) or the SILICON is. Then correct `expected_silicon.txt` with a
  reason line saying which, and re-run the checker.

- [ ] **Step 5: Fill in the quirks document**

In `docs/gc355-vglite-quirks.md`:
- Replace `**Silicon status: NOT YET MEASURED.**` with
  `**Silicon status: measured <YYYY-MM-DD>**, transcript `examples/display/vglite_conformance/transcript_hw_evkb.txt`, VGLite pin `<sha from evkb.cmake>`, Series `gc355/0x0_1216`.`
- Replace each `expected OK` / `expected BROKEN` in the Phase-1 table with the
  MEASURED verdict, and put the case's `detail=` numbers in the Evidence column
  alongside the case id (e.g. `path/multi-contour-disjoint` → `runs=1 (expected 4)`).
- If any case reported `repeat=differs`, add a row for it under a new
  **Nondeterminism** heading with its two checksums — that is a first-class
  finding, not a footnote.

- [ ] **Step 6: Commit**

```bash
git add examples/display/vglite_conformance/transcript_hw_evkb.txt \
        examples/display/vglite_conformance/expected_silicon.txt \
        docs/gc355-vglite-quirks.md
git commit -m "NEW-32: GC355 conformance matrix measured on silicon (Phase 1)"
```

---

### Task 11: Close-out

- [ ] **Step 1: Re-run the fixture-freshness triangle**

The example's output has not changed since Task 5, but confirm rather than
assume — a committed fixture goes stale SILENTLY, and only this suite sees it:

```bash
cd /Users/nicholasnewdigate/Development/rt1170/evkb
sh tools/gate-vacuity.test.sh | grep vglite_conformance
./examples/display/vglite_conformance/run_qemu.sh | tail -1
```

Expected: all three `vglite_conformance` vacuity cases `PASS:`, and
`PASS: VGLite conformance harness negative verified`.

- [ ] **Step 2: Record the measurement in CLAUDE.md**

Add a new bullet at the TOP of the measured-sweep list in the "Test / verify"
section, in the established form, with the ACTUAL numbers from Task 9 Step 1
and Task 10 — never carried forward from this plan:

```markdown
✅ **Measured <date>: 124 gates discovered, <N> passed, <M> failed**, on the
NEW-32 Phase 1 close-out (`display/vglite_conformance`, the GC355 conformance
harness). <disposition of any red, with evidence>. `LICENSE-AUDIT: PASS` the
same day with the new `examples/display/vglite_conformance` manifest entry
walked (<N> dep paths). Vacuity suite <N>/<N>, the three new conformance cases
included (fabricated `pixel=ok` and a truncated matrix both fail by name).
Silicon matrix: <N> ok, <M> broken, repeat_differs=<K> — diffed against the
PRE-REGISTERED `expected_silicon.txt` by `tools/vglite-conformance-check.sh`
(<PASS | which lines drifted and why>).
```

- [ ] **Step 3: Update the Linear issue**

Post the Phase-1 result to NEW-32: the sweep number, the measured matrix, which
predictions held and which did not, and the link to
`docs/gc355-vglite-quirks.md`.

- [ ] **Step 4: Commit**

```bash
git add CLAUDE.md && git commit -m "NEW-32 Phase 1 close-out: sweep 124, GC355 conformance matrix recorded"
```

---

## Self-review notes

**Spec coverage (§ by §):**
- §4 (case table, two verdicts, single scratch, one at a time, summary line) — Tasks 2, 3. The summary line adds `repeat_differs=` as an explicit, documented additive extension.
- §5 (paths/contours/winding predicates, structural not checksums) — Task 3; the "every case runs twice / `repeat=`" requirement is in the Task 2 run loop, with the multi-sub-render `sum()` hook so it is not vacuous for cases 5, 6 and 11.
- §6 (dangerous cases opt-in, id printed before the call, `dangerous=` in the summary) — Tasks 2 and 4; `case_begin` is printed for EVERY case, not only dangerous ones, since a hang anywhere costs the same bench cycle.
- §7 (QEMU honest negative + `pixel=ok` tripwire, sweep 123→124, audit entry, demonstrated-RED vacuity, `expected_silicon.txt`, `tools/vglite-conformance-check.sh`) — Tasks 5, 6, 7.
- §9 (the reference document) — Task 8.
- §10 phase 1 ("harness + paths/contours/winding, gate, silicon matrix, doc skeleton") — the whole plan.
- §11 (risks) — the paired controls are in Task 3 and named in Task 10 Step 4's triage; the pre-registration ordering answers the rubber-stamp risk.

Out of scope by design, per §3 and §10: gradients/colour (Phase 2), blits/scissor (Phase 3), the guard layer and the compositor retrofit (Phase 4). No vendored VGLite source is touched anywhere in this plan.
