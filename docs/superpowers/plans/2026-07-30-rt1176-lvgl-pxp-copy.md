# PXP-accelerated LVGL sync copy (v6) — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Measure CPU-vs-PXP copy cost on real silicon across a size/offset matrix, prove
PXP-copy correctness in QEMU per case, and adopt a thresholded PXP `buf_copy_cb` handler
in the double-buffered examples **only if the user approves the measured table** (P2).

**Architecture:** Spec `docs/superpowers/specs/2026-07-30-rt1176-lvgl-pxp-copy-design.md`
— read it first, especially §3 (the chaining handler), §4 (the bench's two halves), and
§6's P2 decision point, which is a HARD STOP for user review. Repos: evkb (bench +
adoption), LVGL (the handler, only if P3a).

**Verified facts (sanity-check, don't re-derive):**
- LVGL's global handler seam: `lv_draw_buf_get_handlers()` (`lv_draw_buf.h:136`) returns
  the mutable struct; `buf_copy_cb` is the copy hook (`lv_draw_buf.h:82,116,290`);
  `refr_sync_areas` reaches it via `lv_draw_buf_copy` (`lv_refr.c:736-738`).
- **`PXPSurface` has no origin field** (`PXP.h:97-126`: `{data, width, height, pitch,
  format}`, pitch validated ≥ one row, 0 = invalid) — a sub-rect copy is expressed by
  an OFFSET BASE (`base + y*pitch + x*2` for RGB565) with the FULL buffer pitch and the
  rect's w/h as the surface dimensions. The bench's odd-offset cases exist to prove this
  arithmetic before any handler uses it. `PXPClass::blit(src, dst)` + `wait(timeout_ms)`
  (`PXP.h:178,181`); mirror `examples/display/pxp_blit_test`'s begin/blit idioms — it is
  the authoritative in-tree consumer.
- **DWT CYCCNT already runs**: enabled at startup (`cores/imxrt1176/startup.c:444-446`),
  read via `ARM_DWT_CYCCNT` (`imxrt1176.h:156`), 996 MHz core → µs = cycles/996.
- `lv_draw_buf_init(&buf, w, h, cf, stride, data, data_size)` builds an `lv_draw_buf_t`
  over an existing buffer (verify the exact v9.4 signature in
  `lvgl/src/draw/lv_draw_buf.h` before use); `LV_COLOR_FORMAT_RGB565`.
- Extmem allocation discipline: `extmem_malloc(bytes + 64)` + round up to 64 (the
  framebuffer allocator's exact pattern).
- QEMU models the PXP (three green gates); whether its blit completes synchronously is
  UNKNOWN — so the gate's negative test must not depend on wait-timing. The prescribed
  sabotage is an off-by-one base address (deterministic divergence everywhere).

**Conventions:** gates as `./run_qemu.sh` only; uptime/ps before runs; commit fixes
before `git checkout --` reverts of sabotage; printf lines ≤ 127 chars; branch
`lvgl-pxp-v6` off master.

---

## Task 1: `lvgl_pxp_copy_bench` — the example (P1a)

**Files:**
- Create: `examples/display/lvgl_pxp_copy_bench/CMakeLists.txt`
- Create: `examples/display/lvgl_pxp_copy_bench/lvgl_pxp_copy_bench.cpp`
- Create: `examples/display/lvgl_pxp_copy_bench/toolchain/` (copied from a sibling)

- [ ] **Step 1: Directory + toolchain** (copy from `lvgl_rk055_panel_test`).

- [ ] **Step 2: `CMakeLists.txt`** — note: no MipiDisplay, no Wire, no panel; this is a
memory benchmark:

```cmake
cmake_minimum_required(VERSION 3.24)
project(lvgl_pxp_copy_bench)

set(TEENSY_VERSION 117 CACHE STRING "")

include(${CMAKE_CURRENT_LIST_DIR}/../../../evkb.cmake)

import_evkb_lvgl()      # lv_draw_buf_copy IS the thing under test (CPU side)
import_evkb_library(PXP)

teensy_add_executable(lvgl_pxp_copy_bench lvgl_pxp_copy_bench.cpp)
teensy_target_link_libraries(lvgl_pxp_copy_bench cores PXP)

target_link_libraries(lvgl_pxp_copy_bench.elf LVGL stdc++)
```

- [ ] **Step 3: `lvgl_pxp_copy_bench.cpp`.** Structure (exact code below is the
deliverable; the two `<implementer:>` markers are resolved from in-tree authorities):

```cpp
/* lvgl_pxp_copy_bench - CPU vs PXP for LVGL's cross-buffer sync copy (v6).
 * Copyright (c) 2026 Nicholas Newdigate
 * SPDX-License-Identifier: MIT
 *
 * TWO CLAIMS IN ONE EXAMPLE, per the two-gate rule:
 *   CORRECTNESS (QEMU-gated): for every case in the matrix, a PXP sub-rect
 *       copy produces a byte-identical destination to LVGL's own
 *       lv_draw_buf_copy.  The checksum covers the WHOLE destination buffer,
 *       so an out-of-bounds write is as red as a wrong pixel.  The offset-
 *       base arithmetic proven here is the arithmetic the future handler
 *       (spec 3) will use -- proven BEFORE that handler exists.
 *   TIMING (hardware-only): DWT cycle counts for both paths per case.  QEMU
 *       has no timing model; its numbers are printed but VACUOUS, and the
 *       transcript says so.  The hardware table is the v6 P2 decision input.
 *
 * The matrix deliberately includes odd widths, odd x-offsets and edge-hugging
 * rects: PXPSurface has no origin field, so sub-rects are offset base
 * addresses -- exactly where pitch arithmetic goes wrong silently.
 *
 * Uses Serial1 (LPUART; QEMU captures it), like every sibling gate.
 */
#include <Arduino.h>
#include "lvgl_rt1176.h"
#include "PXP.h"

static constexpr uint32_t BUF_W = 720, BUF_H = 1280;
static constexpr uint32_t BUF_STRIDE = BUF_W * 2u;          /* unpadded RGB565 */
static constexpr uint32_t BUF_BYTES  = BUF_STRIDE * BUF_H;

struct Case { uint16_t w, h, x, y; };
/* The matrix (spec 9 Q1, fixed here): button-scale through full-screen, with
 * odd offsets/widths and edge-hugging rects.  14 cases; the gate pins the
 * count so a dropped case cannot pass silently. */
static constexpr Case CASES[] = {
    {  16,   16,   0,    0}, {  16,   16,  13,    7},
    {  64,   64,   0,    0}, {  64,   64,  13,    7},
    { 120,  140,   0,    0}, { 120,  140, 599, 1139},
    { 200,  160,   8,   48}, { 360,  320,   0,    0},
    { 360,  320, 180,  480}, { 719,    1,   1,    0},
    {   1, 1280, 719,    0}, { 720,  640,   0,  320},
    { 720,  640,   0,  640}, { 720, 1280,   0,    0},
};
static constexpr uint8_t NUM_CASES = sizeof(CASES) / sizeof(CASES[0]);

static uint16_t *s_src, *s_dst;
static lv_draw_buf_t s_src_db, s_dst_db;

static uint16_t *alloc_buf() {
    uint8_t *raw = (uint8_t *)extmem_malloc(BUF_BYTES + 64);
    if (!raw) return nullptr;
    return (uint16_t *)(((uintptr_t)raw + 63) & ~(uintptr_t)63);
}

/* Position-dependent fills (the GT911 blob-filler lesson: every byte pays
 * into the sum, so a misplaced or short copy always moves it). */
static void fill_src() {
    for (uint32_t i = 0; i < BUF_BYTES / 2; i++)
        s_src[i] = (uint16_t)(0xA53Cu ^ (i * 2654435761u >> 16));
}
static void fill_dst() {
    for (uint32_t i = 0; i < BUF_BYTES / 2; i++)
        s_dst[i] = (uint16_t)(0x0F1Eu ^ (i * 40503u >> 8));
}

static uint32_t dst_sum() {                   /* FNV-1a over the WHOLE dest */
    lvgl_sum_reset();
    lvgl_sum_feed(s_dst, BUF_BYTES);
    return lvgl_sum_value();
}

static uint32_t cycles_us(uint32_t cyc) { return cyc / 996u; }

void setup()
{
    Serial1.begin(115200);
    delay(200);
    Serial1.println("PXP_COPY_BENCH_BEGIN");

    s_src = alloc_buf(); s_dst = alloc_buf();
    if (!s_src || !s_dst) { Serial1.println("ALLOC_FAIL"); Serial1.println("PXP_COPY_BENCH_DONE"); return; }
    Serial1.println("ALLOC_OK");

    lvgl_rt1176_begin();   /* lv_init: the default draw-buf handlers exist */
    /* <implementer: verify lv_draw_buf_init's exact v9.4 signature in
     *  lvgl/src/draw/lv_draw_buf.h and initialise s_src_db/s_dst_db over the
     *  two buffers: w=BUF_W, h=BUF_H, LV_COLOR_FORMAT_RGB565, BUF_STRIDE,
     *  data, BUF_BYTES.  Check the return if it has one.> */

    /* <implementer: PXP bring-up exactly as pxp_blit_test does it (begin(),
     *  any clock/reset ritual it performs, and its blit invocation idiom --
     *  PXP.blit(src,dst) or PXP.op().source().output().run(), whichever that
     *  example uses).  Print PXP_OK or PXP_FAIL accordingly and bail on fail.> */

    fill_src();
    bool all_ok = true;

    for (uint8_t i = 0; i < NUM_CASES; i++) {
        const Case &c = CASES[i];
        lv_area_t area;
        area.x1 = c.x; area.y1 = c.y;
        area.x2 = (int32_t)c.x + c.w - 1; area.y2 = (int32_t)c.y + c.h - 1;

        /* --- CPU path: LVGL's own copy, default handlers ------------------ */
        fill_dst();
        uint32_t t0 = ARM_DWT_CYCCNT;
        lv_draw_buf_copy(&s_dst_db, &area, &s_src_db, &area);
        uint32_t cpu_cyc = ARM_DWT_CYCCNT - t0;
        const uint32_t cpu = dst_sum();

        /* --- PXP path: offset-base sub-rect surfaces ---------------------- */
        fill_dst();
        uint16_t *sp = s_src + (uint32_t)c.y * BUF_W + c.x;
        uint16_t *dp = s_dst + (uint32_t)c.y * BUF_W + c.x;
        PXPSurface ssrc(sp, c.w, c.h, PXP_RGB565, BUF_STRIDE);
        PXPSurface sdst(dp, c.w, c.h, PXP_RGB565, BUF_STRIDE);
        t0 = ARM_DWT_CYCCNT;
        /* <implementer: the blit+wait idiom from pxp_blit_test; treat any
         *  PXPError as a per-case failure token, not a hang> */
        uint32_t pxp_cyc = ARM_DWT_CYCCNT - t0;
        const uint32_t pxp = dst_sum();

        const bool ok = (cpu == pxp);
        all_ok = all_ok && ok;
        Serial1.printf("CASE i=%u r=%ux%u+%u+%u CPU=0x%08lX PXP=0x%08lX %s cpu_us=%lu pxp_us=%lu\n",
                       (unsigned)(i + 1), c.w, c.h, c.x, c.y,
                       (unsigned long)cpu, (unsigned long)pxp,
                       ok ? "MATCH" : "MISMATCH",
                       (unsigned long)cycles_us(cpu_cyc),
                       (unsigned long)cycles_us(pxp_cyc));
    }

    Serial1.println("NOTE timings are hardware-only; QEMU numbers are vacuous");
    Serial1.printf("CASES=%u\n", (unsigned)NUM_CASES);
    if (all_ok) Serial1.println("COPY_BENCH_OK");
    Serial1.println("PXP_COPY_BENCH_DONE");
}

void loop() {}
```

(Format-name note: use the PXP library's actual RGB565 enumerator — grep `PXP.h` for it;
`PXP_RGB565` above is a placeholder for whatever the library names it. Widest CASE line
is ~110 chars — within the 127 budget; verify after substitution.)

- [ ] **Step 4: Build.** Standard configure+build; clean link expected.

---

## Task 2: The correctness gate + negative test (P1b)

**Files:**
- Create: `examples/display/lvgl_pxp_copy_bench/run_qemu.sh`
- Create: `examples/display/lvgl_pxp_copy_bench/transcript_qemu.txt`

- [ ] **Step 1: `run_qemu.sh`** — sibling skeleton (gate-lib, qrun, poll for
`PXP_COPY_BENCH_DONE`, ceiling 80×0.25 s), then:

```sh
grep -q "ALLOC_OK" "$OUT" || { echo "FAIL: extmem alloc"; exit 1; }
grep -q "PXP_OK"   "$OUT" || { echo "FAIL: PXP bring-up"; exit 1; }
# Every case must MATCH, by name -- a dropped case cannot hide behind a count.
for i in $(seq 1 14); do
  grep -q "^CASE i=$i .* MATCH " "$OUT" || { echo "FAIL: case $i did not match"; exit 1; }
done
grep -q "MISMATCH" "$OUT" && { echo "FAIL: at least one case mismatched"; exit 1; }
# The count pin catches a matrix edit that forgot the loop above.
grep -q "^CASES=14$" "$OUT" || { echo "FAIL: case count"; exit 1; }
grep -q "COPY_BENCH_OK" "$OUT" || { echo "FAIL: bench verdict withheld"; exit 1; }
# Timings are NOT asserted anywhere: hardware-only, vacuous in QEMU -- the
# NOTE token in the transcript says so and this comment is the gate's half.
[ -f "$DIR/pxp_copy_bench.dbg" ] || { echo "FAIL: no guest-error log"; exit 1; }
grep -q "guest" "$DIR/pxp_copy_bench.dbg" && { echo "FAIL: guest errors"; exit 1; }
echo "PASS: PXP copy == CPU copy across the matrix"
```

- [ ] **Step 2:** Run twice; green ×2 with all 14 MATCH lines byte-identical (sums are
deterministic; the µs fields will differ run-to-run — that is fine, the greps don't pin
them).

- [ ] **Step 3: Negative test — off-by-one base.** Temporarily change the PXP source
pointer arithmetic to `s_src + (uint32_t)c.y * BUF_W + c.x + 1`. Rebuild, run: expect
RED with MISMATCH on (at minimum) every case whose width makes the +1 visible —
deterministic in QEMU and on silicon alike, no dependence on the model's wait timing.
If green, STOP: the whole-buffer checksum is not covering what it claims. Revert,
rebuild, final green ×1.

- [ ] **Step 4:** `cp` the uart to `transcript_qemu.txt`; commit the example + gate
(branch `lvgl-pxp-v6`):

```
lvgl_pxp_copy_bench: PXP sub-rect copy == LVGL's copy, 14 cases

The offset-base arithmetic the future buf_copy handler will use, proven
before that handler exists: whole-destination checksums across a matrix
of sizes, odd offsets and edge rects.  Timings printed but hardware-only.
Negative test (base+1): red at <n> MISMATCH cases.
```

---

## Task 3: P2 — the hardware table and the HARD STOP

- [ ] **Step 1:** Flash the bench (standard VCOM-free ritual), capture the full CASE
table from the console. Sanity: all 14 MATCH on silicon too (the correctness claim
re-proven on the real PXP), `COPY_BENCH_OK`.

- [ ] **Step 2:** Write `transcript_hw_evkb.txt` (house format) with the complete table
and a short analysis block: cpu_us and pxp_us per case, the crossover size (the smallest
case where pxp_us < cpu_us), and the full-screen ratio.

- [ ] **Step 3: STOP. Present the table to the user** with a recommendation:
- If PXP wins clearly above some size: recommend adoption with `threshold_px` = the
  crossover (rounded conservative — adopt only clearly-winning sizes), and name the
  expected benefit for the two real workloads (button-press sync ≈ 32 KB; full-screen
  animation sync = 1.8 MB).
- If PXP never wins (or wins only marginally at full-screen): recommend P3b — the
  close-out. Commit the transcript either way:

```
lvgl_pxp_copy_bench: hardware table -- the P2 decision input
```

**Do not proceed to Task 4 without the user's explicit adoption decision.**

---

## Task 4A (if adopted): the handler + adoption (P3a)

**Files:**
- Create: `~/Development/LVGL/port/lvgl_pxp_copy.h` and `.cpp`
- Modify: both db examples' CMakeLists (compile the new port file; flip test adds
  `import_evkb_library(PXP)` — the touch test: check whether PXP is already imported)
- Modify: both examples' `.cpp` (install call + one corroboration print) and gates

- [ ] **Step 1: The handler** — spec §3's contract, concretely:

```cpp
/* lvgl_pxp_copy.cpp core shape (header carries the spec-3 contract comment) */
static lv_draw_buf_copy_cb_t s_default_copy = nullptr;
static uint32_t s_pxp_copies = 0, s_fallbacks = 0;
static uint32_t s_threshold_px = 0;

static void pxp_copy_cb(lv_draw_buf_t *dest, const lv_area_t *dest_area,
                        const lv_draw_buf_t *src, const lv_area_t *src_area)
{
    /* Accelerated shape: RGB565 both sides, equal-size areas, area >=
     * threshold, strides sane, both buffers PXP-reachable.  ANYTHING else
     * falls through to the saved default -- never a silent wrong copy. */
    ...checks... -> { s_fallbacks++; s_default_copy(dest, dest_area, src, src_area); return; }
    /* offset-base surfaces exactly as the bench proved, blit + wait;
     * any PXPError -> count a fallback and run the default copy anyway. */
    s_pxp_copies++;
}

void lvgl_pxp_copy_install(uint32_t threshold_px)
{
    lv_draw_buf_handlers_t *h = lv_draw_buf_get_handlers();
    s_default_copy = h->buf_copy_cb;   /* verify field name against the header */
    s_threshold_px = threshold_px;
    h->buf_copy_cb = pxp_copy_cb;
}
```

The exact `lv_draw_buf_copy_cb_t` parameter order comes from `lv_draw_buf.h:82` — read
it, do not trust this sketch. The wrapper must also verify `dest_area`/`src_area` are
the same size (the sync copy always is; anything else falls through).

- [ ] **Step 2: Adoption.** In both db examples: `lvgl_pxp_copy_install(<threshold>);`
after `lvgl_rt1176_begin()`, with the threshold constant citing the P2 transcript by
case number. Add one print near the counters: `PXP_COPIES=` and `PXP_FALLBACKS=`.
Gates add (the IDLE_POLLS idiom):

```sh
grep -q "^PXP_COPIES=" "$OUT" || { echo "FAIL: pxp copy count missing"; exit 1; }
grep -q "^PXP_COPIES=0$" "$OUT" && { echo "FAIL: handler installed but never engaged"; exit 1; }
```

- [ ] **Step 3:** Rebuild + re-run both gates: **every existing token byte-identical**
(goldens, MATCH pairs, counters — a correct copy changes no pixels), plus the new
corroboration greps green. Any golden drift = the handler corrupted a copy: STOP.
Commit LVGL (handler) first, then evkb (adoption + gates); push LVGL + pin bump waits
for the wrap.

---

## Task 4B (if declined): the close-out (P3b)

- [ ] README capability row: "PXP sync-copy: **measured, not adopted** — see the bench
transcript; crossover [did not exist / at NxM, below the real workloads' benefit]".
No handler is written; the bench + gate + transcript stand as the deliverable. Commit.

---

## Task 5 (if 4A): hardware re-rituals (P4)

- [ ] Flip test: flash, counters + eye on the sweep; touch test: flash, the full finger
ritual. Both must behave identically to v5 (PXP_COPIES>0 now in the transcript).
Append dated v6 sections to both `transcript_hw_evkb.txt`; commit.

---

## Task 6: Wrap (P5)

- [ ] License audit red-first: add `examples/display/lvgl_pxp_copy_bench:lvgl_pxp_copy_bench \`
to `GATES`; re-run green.
- [ ] If 4A: push LVGL, bump its pin, force-fetch proof.
- [ ] Docs: CLAUDE.md sweep **71 → 72**; KNOWN-BROKEN-GATES dated note; README rows
(per 4A/4B outcome); `examples/README.md` display row.
- [ ] Full sweep: expect **72/0/0** or **71/1/0** (cm4_audio_test).
- [ ] Commit wrap; memory (`rt1176-lvgl-pxp-copy` project memory recording the table,
the decision, and the crossover number — or the no-adoption reasoning); then the
finishing skill for the merge.

---

## Self-review notes (kept for the executor)

- **Spec coverage:** §3→Task 4A; §4→Tasks 1-2; §5's negative test→Task 2 Step 3 (the
  off-by-one sabotage replaces the spec's skip-the-wait suggestion because the QEMU
  model's blit-completion timing is unknown — deterministic divergence beats
  timing-dependent divergence; recorded here as a deliberate refinement); §6 P1-P5→Tasks
  1-6; P2's hard stop→Task 3 Step 3.
- **Two `<implementer:>` markers** (lv_draw_buf_init signature; pxp_blit_test's
  begin/blit idiom) resolve from in-tree authorities, per the established pattern; the
  PXP format enumerator name likewise.
- **The bench never installs any handler** — it calls `lv_draw_buf_copy` with the stock
  handlers and drives the PXP directly; the handler's arithmetic is proven by the bench
  before the handler exists (P1 before P3), which is what makes P3b a cheap outcome.
