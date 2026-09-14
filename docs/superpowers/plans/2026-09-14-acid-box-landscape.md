# acid_box Landscape Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Turn `display/acid_box` into a landscape instrument — the board turned 90° counter-clockwise, the UI drawn upright on the RK055's portrait glass — with today's widgets rearranged into layout C, the rotation confined to the present and the touch mapping, and every existing gate discipline (QEMU golden, vacuity, silicon acceptance) carried across.

**Architecture:** LVGL renders a 1280×720 canvas in DIRECT single-buffer mode and never learns about rotation; on the last flush the port composites the GC355 knobs into the canvas, waits for the previous flip to retire, PXP-rotates the damaged region (this frame's ∪ last present's, 16-px snapped, pointer-offset surfaces) or the whole frame into the off-screen portrait scanout buffer, and flips. A Phase 0 probe example measures the PXP on silicon BEFORE the layout is touched and decides full-frame vs damage-grid. The GT911 binding inverts the same mapping for touch. Spec: `docs/superpowers/specs/2026-09-14-acid-box-landscape-design.md`.

**Tech Stack:** LVGL 9.4 (sibling repo `~/Development/LVGL`, `port/`), PXP driver (`~/Development/PXP`), MipiDisplay/LCDIFv2, TouchPanel GT911, SynthUI (unchanged), CMake + ARM GCC 10, qemu2 `mimxrt1170-evk` (PXP model with ROT90, GT911 `touch-script`), `tools/gate-lib.sh`, `tools/gate-vacuity.test.sh`, `tools/license-audit.sh`, LinkServer for silicon.

---

## Read this first

* **Two repos change.** The port work (Tasks 4–8) lives in `~/Development/LVGL` (its own git repo, remote `github.com/newdigate/LVGL`); everything else is in this repo. Library resolution is local-first, so the acid_box build picks up the LVGL working tree immediately; the `evkb.cmake` pin (line 130) is bumped in Task 8 after the push, and Task 13 proves the fresh-user path by RUNNING the gate on a `-DEVKB_FORCE_FETCH=ON` ELF.
* **Task 3 is a bench task** (a human at the EVKB). If the bench is not available when you reach it, do Tasks 4–8 first (they do not depend on the probe's numbers) and come back; Task 9 needs Task 3's threshold. **If Task 3's P4 is BROKEN on silicon, STOP after Task 8** and return to the spec's §4 decision rule — the remaining tasks assume P4 holds.
* **Gates do not build.** Every `./run_qemu.sh` below assumes `cmake --build build` ran first. Run gates as `./run_qemu.sh`, never `sh run_qemu.sh`.
* **The golden numbers in Tasks 2 and 10 are recorded from runs, after the frame or the predicate has been examined** — they cannot be written into this plan in advance, and the steps say exactly what to check before pinning each one.
* Build command for any example (from its directory):
  ```bash
  cmake -B build -DCMAKE_TOOLCHAIN_FILE=../../../toolchain/rt1170-evkb.toolchain.cmake && cmake --build build
  ```
* Commit messages end with `Co-Authored-By: Claude Fable 5.1 <noreply@anthropic.com>`.

## File structure

**New**
| path | responsibility |
|---|---|
| `examples/display/pxp_rotate_probe/CMakeLists.txt` | no-panel PXP example: cores + PXP only |
| `examples/display/pxp_rotate_probe/pxp_rotate_probe.cpp` | Phase A case table (correctness, sums) + Phase B timing; its OWN CPU reference |
| `examples/display/pxp_rotate_probe/run_qemu.sh` | gate 140: sums, offgrid line exists, case-count and begin/done equality |
| `examples/display/pxp_rotate_probe/transcript_qemu.txt` | committed fixture (vacuity suite) |
| `examples/display/pxp_rotate_probe/transcript_hw_evkb.txt` | the silicon run: P1–P5 measured |
| `~/Development/LVGL/port/lvgl_panel_rotation.h` | pure C geometry: present type, `map_rect`, `map_point_inv`, `snap`, `plan` |
| `~/Development/LVGL/port/tests/lvgl_panel_rotation_test.c` | unit + property tests |
| `~/Development/LVGL/port/tests/run.sh` | builds/runs the suite; three mutant arms that must FAIL by name |

**Modified**
| path | change |
|---|---|
| `~/Development/LVGL/port/lvgl_mipi_panel.h` / `.cpp` | `lvgl_mipi_panel_create_rotated()`, `set_rot_threshold()`, `canvas()`, `rot_*()` counters; `create()`/`create_db()` untouched |
| `~/Development/LVGL/port/lvgl_gt911_indev.h` / `.cpp` | `lvgl_gt911_indev_create_rotated()`; shared `create_common()`; mapping helper (unrotated arithmetic byte-identical) |
| `evkb.cmake:130` | LVGL pin |
| `examples/display/acid_box/acid_box.cpp` | geometry constants, layout C `build_ui`, rotated create/indev, compositor 1280×720, `ACIDBOX_ROT` + `ACIDBOX_ROT_EQ` lines, equality guard |
| `examples/display/acid_box/run_qemu.sh` | GEOMETRY header, new assertions, re-recorded golden |
| `examples/display/acid_box/touch_script.txt` | regenerated |
| `examples/display/acid_box/transcript_qemu.txt`, `transcript_hw_evkb.txt` | re-captured; portrait gpu golden marked RETIRED |
| `tools/gate-vacuity.test.sh` | sections 14 (probe) and 15 (acid_box) |
| `tools/license-audit.sh` | `GATES` entry for the probe |
| `CLAUDE.md` | gate count 139 → 140 |

---

### Task 1: the Phase 0 probe example (`display/pxp_rotate_probe`)

**Files:**
- Create: `examples/display/pxp_rotate_probe/CMakeLists.txt`
- Create: `examples/display/pxp_rotate_probe/pxp_rotate_probe.cpp`

- [ ] **Step 1: Write the CMakeLists**

```cmake
cmake_minimum_required(VERSION 3.24)
project(pxp_rotate_probe)

set(TEENSY_VERSION 117 CACHE STRING "")

include(${CMAKE_CURRENT_LIST_DIR}/../../../evkb.cmake)

# No panel, no LVGL: cores + PXP, exactly like pxp_blit_test.  The two buffers
# are EXTMEM (SDRAM), which the core's startup brings up before setup().
import_evkb_library(PXP)

teensy_add_executable(pxp_rotate_probe pxp_rotate_probe.cpp)
teensy_target_link_libraries(pxp_rotate_probe cores PXP)

target_link_libraries(pxp_rotate_probe.elf stdc++)
```

- [ ] **Step 2: Write the probe**

```cpp
/* pxp_rotate_probe - Phase 0 of the acid_box landscape design: does the PXP
 * rotate a 1280x720 XRGB8888 canvas into the RK055's 720x1280 scanout buffer
 * correctly -- at the origin AND at pointer-offset sub-rectangles -- and how
 * long does each op take?
 * Spec: docs/superpowers/specs/2026-09-14-acid-box-landscape-design.md, section 4
 * Copyright (c) 2026 Nicholas Newdigate
 * SPDX-License-Identifier: MIT
 *
 * NO PANEL.  The core's startup brings the SEMC SDRAM up before setup(), so
 * EXTMEM is live without Display.begin(); the two buffers here are the canvas
 * and one scanout buffer of the design, with nothing scanning them out.
 *
 * TWO PHASES, the rotary_knob_bench pattern:
 *   A  correctness, gated in QEMU: every case prints api= (what the driver
 *      said) and pixel= (what a CPU predicate found) SEPARATELY, because a
 *      driver that returns PXP_OK while writing the wrong picture is the
 *      failure this probe exists to catch.  Ends with `crc_done`.
 *   B  timing, after crc_done, NEVER gated: QEMU time is a fiction.
 *
 * WHY POINTER-OFFSET SURFACES AND NOT outputAt(): the QEMU PXP model writes a
 * ROTATED op from the output buffer's origin and ignores OUT_PS for it (its
 * own comment: "the firmware issues no letterboxed rotated op").  A source
 * surface pointing INTO the canvas and an output surface pointing INTO the
 * scanout buffer make every op a rotation at the origin -- the path
 * pxp_blit_test has already matched on silicon (PXP_ROT90_SUM=0x54F838ED) --
 * so the model and the silicon agree BY CONSTRUCTION.  Every rect is 16-px
 * grid aligned so every derived pointer is 64-byte aligned at 4 bpp: x*4 and
 * (720-y-h)*4 are multiples of 64, and the row offsets y*5120 / x*2880 are
 * multiples of 64 regardless (5120 = 64*80, 2880 = 64*45).
 *
 * The CPU reference below is DELIBERATELY independent of the port's
 * lvgl_panel_rotation.h: the two are cross-checks, not one function twice. */
#include <Arduino.h>
#include <string.h>
#include <PXP.h>

#define CONSOLE Serial1
#define PROBE_VERSION 1

#define LOG_W  1280
#define LOG_H  720
#define PHYS_W 720
#define PHYS_H 1280
#define BPP    4
#define LOG_PITCH  (LOG_W * BPP)     /* 5120 */
#define PHYS_PITCH (PHYS_W * BPP)    /* 2880 */
#define SENTINEL 0x5A5A5A5Au

/* One spare row on each: a sub-rect surface's pitch*height can run past the
 * last row of the buffer it points into.  The driver's reachable() checks the
 * REGION, not the allocation, so this is belt-and-braces, not a fix. */
EXTMEM __attribute__((aligned(64))) static uint32_t canvas[LOG_W * (LOG_H + 1)];
EXTMEM __attribute__((aligned(64))) static uint32_t dst[PHYS_W * (PHYS_H + 1)];

static uint32_t fnv1a(const void *p, size_t n)
{
    const uint8_t *b = (const uint8_t *)p;
    uint32_t h = 2166136261u;
    for (size_t i = 0; i < n; i++) { h ^= b[i]; h *= 16777619u; }
    return h;
}

/* Every canvas pixel unique in position: a misplaced or mis-strided op cannot
 * reproduce the reference by accident. */
static void fill_canvas(void)
{
    for (int y = 0; y < LOG_H; y++)
        for (int x = 0; x < LOG_W; x++)
            canvas[y * LOG_W + x] = 0xFF000000u | ((uint32_t)x << 12) | (uint32_t)y;
}

/* CW90 reference: physical (px,py) reads logical (py, 719-px). */
static inline uint32_t ref_px(int px, int py) { return canvas[(PHYS_W - 1 - px) * LOG_W + py]; }

static void fill_dst(void)
{
    for (size_t i = 0; i < (size_t)PHYS_W * PHYS_H; i++) dst[i] = SENTINEL;
}

/* Rotate the logical rect (x,y,w,h) into its physical place (720-y-h, x, h, w). */
static PXPError rot_rect(int x, int y, int w, int h)
{
    uint8_t *s = (uint8_t *)canvas + (size_t)y * LOG_PITCH + (size_t)x * BPP;
    const int px0 = PHYS_W - y - h, py0 = x;
    uint8_t *d = (uint8_t *)dst + (size_t)py0 * PHYS_PITCH + (size_t)px0 * BPP;
    PXPSurface src(s, (uint16_t)w, (uint16_t)h, PXP_XRGB8888, LOG_PITCH);
    PXPSurface out(d, (uint16_t)h, (uint16_t)w, PXP_XRGB8888, PHYS_PITCH);   /* the rotated extent */
    return PXP.op().source(src).output(out).rotate(PXP_ROT_90).run();
}

/* Predicate: inside the physical rect every pixel is the reference; outside it
 * every pixel is still the sentinel.  WHOLE buffer, so a stray write anywhere
 * is caught, not only one near the target. */
static bool check_rect(int x, int y, int w, int h)
{
    const int px0 = PHYS_W - y - h, px1 = PHYS_W - 1 - y, py0 = x, py1 = x + w - 1;
    for (int py = 0; py < PHYS_H; py++)
        for (int px = 0; px < PHYS_W; px++) {
            const bool in = px >= px0 && px <= px1 && py >= py0 && py <= py1;
            const uint32_t want = in ? ref_px(px, py) : SENTINEL;
            if (dst[py * PHYS_W + px] != want) return false;
        }
    return true;
}

struct Case { const char *id; int x, y, w, h; bool graded; };
static const Case kCases[] = {
    { "full",       0,    0,   LOG_W, LOG_H, true },
    { "corner-tl",  0,    0,   16,    16,    true },
    { "corner-tr",  1264, 0,   16,    16,    true },
    { "corner-bl",  0,    704, 16,    16,    true },
    { "corner-br",  1264, 704, 16,    16,    true },
    { "knob",       16,   512, 160,   160,   true },   /* the CUTOFF knob's damage, layout C */
    { "cell",       224,  96,  112,   112,   true },   /* step cell 2's */
    { "strip-top",  0,    0,   LOG_W, 16,    true },
    { "strip-left", 0,    0,   16,    LOG_H, true },
    { "centre",     640,  352, 16,    16,    true },
    { "tall",       1200, 592, 80,    128,   true },   /* w != h, both edges */
    /* P5, OBSERVATION only: pointers 32 B off the 64-B grid.  The driver treats
     * rotate-alone as alignment-free (pxp_blit_test PXP_ALIGN=off2:ok); this
     * records what silicon does at 8 px.  The gate asserts the line EXISTS. */
    { "offgrid",    8,    8,   16,    16,    false },
};
#define NCASES ((int)(sizeof(kCases) / sizeof(kCases[0])))

static uint32_t time_rect(int x, int y, int w, int h, int reps)
{
    uint32_t best = 0xFFFFFFFFu;
    for (int r = 0; r < reps; r++) {
        const uint32_t t0 = micros();
        (void)rot_rect(x, y, w, h);
        const uint32_t dt = micros() - t0;
        if (dt < best) best = dt;
    }
    return best;
}

void setup()
{
    CONSOLE.begin(115200);
    while (!CONSOLE && millis() < 2000) {}
    CONSOLE.println("=== BOOT ===");
    CONSOLE.printf("[APP: %s] [VER: v%u] [BUILD: %s %s]\n",
                   "pxp_rotate_probe", PROBE_VERSION, __DATE__, __TIME__);
    CONSOLE.println("pxp_rotate_probe up");

    if (!PXP.begin()) { CONSOLE.println("PXP_BEGIN=FAIL"); return; }
    CONSOLE.println("PXP_BEGIN=PASS");
    fill_canvas();

    int ok = 0, broken = 0;
    for (int i = 0; i < NCASES; i++) {
        const Case &c = kCases[i];
        /* case_begin BEFORE the op: an op that hangs leaves a begin with no
         * case line, and the gate counts both. */
        CONSOLE.printf("case_begin=%s\n", c.id);
        fill_dst();
        const PXPError e = rot_rect(c.x, c.y, c.w, c.h);
        const bool pix = (e == PXP_OK) && check_rect(c.x, c.y, c.w, c.h);
        const uint32_t sum = fnv1a(dst, (size_t)PHYS_W * PHYS_H * BPP);
        CONSOLE.printf("case=%s api=", c.id);
        if (e == PXP_OK) CONSOLE.print("ok"); else { CONSOLE.print("err"); CONSOLE.print((int)e); }
        CONSOLE.printf(" pixel=%s sum=0x%08lX\n", pix ? "ok" : "broken", (unsigned long)sum);
        if (c.graded) { if (pix) ok++; else broken++; }
    }
    CONSOLE.printf("cases=%d ok=%d broken=%d\n", NCASES, ok, broken);
    CONSOLE.println("crc_done");

    /* Phase B -- silicon only.  Min of 8, icache_bench_hw's convention. */
    CONSOLE.printf("time=full min_us=%lu\n",    (unsigned long)time_rect(0, 0, LOG_W, LOG_H, 8));
    CONSOLE.printf("time=rect160 min_us=%lu\n", (unsigned long)time_rect(16, 512, 160, 160, 8));
    CONSOLE.printf("time=rect16 min_us=%lu\n",  (unsigned long)time_rect(640, 352, 16, 16, 8));
    CONSOLE.println("probe_done");
}

void loop()
{
    static uint32_t n = 0;
    delay(1000);
    CONSOLE.printf("hb=%lu\n", (unsigned long)++n);
}
```

- [ ] **Step 3: Build**

```bash
cd examples/display/pxp_rotate_probe && cmake -B build -DCMAKE_TOOLCHAIN_FILE=../../../toolchain/rt1170-evkb.toolchain.cmake && cmake --build build
```
Expected: `build/pxp_rotate_probe.elf` (and `.hex`) exist, no warnings about `PXPSurface`.

- [ ] **Step 4: Run it in QEMU by hand and read the case lines**

```bash
../../../tools/rt1170-qemu.sh build/pxp_rotate_probe.elf 2>&1 | head -40
```
(If `rt1170-qemu.sh` needs a different invocation, `head -20 ../../../tools/rt1170-qemu.sh` documents it.)
Expected: `PXP_BEGIN=PASS`, twelve `case_begin=`/`case=` pairs, every graded case `api=ok pixel=ok`, `cases=12 ok=11 broken=0`, `crc_done`, three `time=` lines, `probe_done`. If any graded case is `pixel=broken` in QEMU, the pointer-offset arithmetic in `rot_rect`/`check_rect` disagrees with the model's `case 1` truth table (`sx = oy; sy = src_h-1-ox`) — fix the probe, not the model.

- [ ] **Step 5: Commit**

```bash
git add examples/display/pxp_rotate_probe/CMakeLists.txt examples/display/pxp_rotate_probe/pxp_rotate_probe.cpp
git commit -m "display: pxp_rotate_probe -- Phase 0 of the acid_box landscape design (spec section 4)

Rotates a 1280x720 XRGB8888 canvas into a 720x1280 buffer with PXP ROT90 at the
origin and at eleven pointer-offset, 16-px-grid sub-rects, each graded by an
independent CPU predicate (api= and pixel= printed separately), plus an
off-grid observation; Phase B times full / 160x160 / 16x16 after crc_done.

Co-Authored-By: Claude Fable 5.1 <noreply@anthropic.com>"
```

---

### Task 2: the probe's gate, fixture, GATES entry and vacuity cases (139 → 140)

**Files:**
- Create: `examples/display/pxp_rotate_probe/run_qemu.sh`
- Create: `examples/display/pxp_rotate_probe/transcript_qemu.txt`
- Modify: `tools/license-audit.sh:352` (the `GATES` list, beside `rotary_knob_bench`)
- Modify: `tools/gate-vacuity.test.sh` (append section 14 after section 13)

- [ ] **Step 1: Write the gate with the sums left as placeholders that FAIL**

```sh
#!/bin/sh
# pxp_rotate_probe -- Phase 0 of the acid_box landscape design
# (docs/superpowers/specs/2026-09-14-acid-box-landscape-design.md, section 4).
#
# WHAT THIS GATE PROVES: the PXP rotates a 1280x720 XRGB8888 canvas into a
# 720x1280 buffer correctly, at the origin (`full`) AND at pointer-offset
# sub-rectangles on the 16-px grid, with nothing written outside the target
# (a whole-buffer sentinel predicate, on the firmware side).  It is the
# regression pin for the present path acid_box's landscape build stands on.
#
# WHAT IT CANNOT PROVE: timing.  Phase B runs after crc_done and is not
# waited for -- QEMU time is a fiction; transcript_hw_evkb.txt has the numbers.
#
# VACUITY GUARDS COME FIRST.  Every pin below is satisfied VACUOUSLY by an
# empty or truncated case table, and an unfinished case is exactly how a
# hung PXP op presents -- so the case_begin count, the case count and the
# tally line are asserted before any sum is looked at.
set -e
DIR=$(cd "$(dirname "$0")" && pwd)
EVKB=$(cd "$DIR/../../.." && pwd)
QEMU="$EVKB/tools/qrun"
. "$EVKB/tools/gate-lib.sh"
gate_init
ELF="$DIR/$(gate_build_dir)/pxp_rotate_probe.elf"
OUT=$(gate_capture_path "$DIR" pxp_rotate_probe.uart)
DBG=$(gate_capture_path "$DIR" pxp_rotate_probe.dbg)
rm -f "$OUT"
"$QEMU" $(gate_qemu_machine) -kernel "$ELF" \
    -display none $(gate_console "$OUT") -d guest_errors -D "$DBG" &
P=$!; gate_pid $P
# Wait for the LAST line this gate parses (crc_done), never an earlier token
# -- the m2_rx_demo mid-line-reap lesson.
for _ in $(seq 1 200); do
    [ -f "$OUT" ] && grep -q "^crc_done" "$OUT" 2>/dev/null && break
    sleep 0.25
done
gate_reap $P
gate_require_capture "$OUT"
echo "==== captured UART ===="; cat "$OUT"

fail() { echo "FAIL: $1"; exit 1; }
grep -q "pxp_rotate_probe up" "$OUT" || fail "banner missing"
grep -q "^PXP_BEGIN=PASS"     "$OUT" || fail "PXP begin"
grep -q "^crc_done"           "$OUT" || fail "case table never finished (crc_done missing) -- a hung op presents exactly like this"

# --- vacuity guards ---------------------------------------------------------
NB=$(grep -c "^case_begin=" "$OUT" || true)
NC=$(grep -c "^case="       "$OUT" || true)
[ "$NB" -eq 12 ] || fail "expected 12 case_begin lines, got $NB"
[ "$NC" -eq 12 ] || fail "expected 12 case lines, got $NC (an unfinished case is how a hang presents)"
grep -qE "^cases=12 ok=11 broken=0\r?$" "$OUT" || fail "tally line missing or not cases=12 ok=11 broken=0"
# No GRADED case may be broken; offgrid is an observation and is excluded by name.
if grep "pixel=broken" "$OUT" | grep -qv "^case=offgrid "; then
    fail "a graded case reported pixel=broken"
fi

# --- the pins ---------------------------------------------------------------
# FNV-1a over the whole 720x1280 destination after each op.  These are
# REGRESSION pins, not correctness claims: correctness is the firmware-side
# pixel= predicate above.  Recorded from the first QEMU run after every graded
# case read pixel=ok; on a mismatch work out which of {probe geometry, PXP
# driver, QEMU model} moved -- never paste in whatever the run printed.
for want in \
  "case=full api=ok pixel=ok sum=0xFULLSUM" \
  "case=corner-tl api=ok pixel=ok sum=0xTLSUM" \
  "case=corner-tr api=ok pixel=ok sum=0xTRSUM" \
  "case=corner-bl api=ok pixel=ok sum=0xBLSUM" \
  "case=corner-br api=ok pixel=ok sum=0xBRSUM" \
  "case=knob api=ok pixel=ok sum=0xKNOBSUM" \
  "case=cell api=ok pixel=ok sum=0xCELLSUM" \
  "case=strip-top api=ok pixel=ok sum=0xSTOPSUM" \
  "case=strip-left api=ok pixel=ok sum=0xSLEFTSUM" \
  "case=centre api=ok pixel=ok sum=0xCENTRESUM" \
  "case=tall api=ok pixel=ok sum=0xTALLSUM"; do
    grep -qF "$want" "$OUT" || fail "missing/wrong: $want"
done
# P5 is an observation: the line must exist with a legal shape, any verdict.
grep -qE "^case=offgrid api=(ok|err[0-9]+) pixel=(ok|broken) sum=0x[0-9A-F]{8}\r?$" "$OUT" \
    || fail "offgrid observation line missing"

echo "PASS: pxp_rotate_probe -- 11 graded rotations pixel-exact, offgrid observed"
```

- [ ] **Step 2: Run it, expect the FIRST pin to fail by name**

```bash
chmod +x run_qemu.sh && ./run_qemu.sh; echo "exit=$?"
```
Expected: the capture prints, then `FAIL: missing/wrong: case=full api=ok pixel=ok sum=0xFULLSUM`, exit 1. This is the gate demonstrated RED on a wrong golden before any real golden is trusted.

- [ ] **Step 3: Pin the eleven sums from the capture**

```bash
grep "^case=" build/pxp_rotate_probe.uart | tr -d '\r'
```
For each graded case replace its `0x…SUM` placeholder in `run_qemu.sh` with the printed `sum=0x........` value. Run `./run_qemu.sh` twice; both must print `PASS: pxp_rotate_probe …` and the two captures' `case=` lines must be identical (`diff <(grep ^case= build/pxp_rotate_probe.uart) <(…second run…)` — copy the first capture aside before the second run).

- [ ] **Step 4: Commit the fixture**

```bash
cp build/pxp_rotate_probe.uart transcript_qemu.txt
```
Then prepend to `transcript_qemu.txt` a five-line header (plain text, before the capture): example name, `Recorded <date>`, machine `mimxrt1170-evk`, "every graded case pixel=ok on two identical runs; sums pinned in run_qemu.sh", and a one-line statement that timing lines are QEMU fiction.

- [ ] **Step 5: GATES entry**

In `tools/license-audit.sh`, in the `GATES` list, after the line `examples/display/rotary_knob_bench:rotary_knob_bench \` add:
```
examples/display/pxp_rotate_probe:pxp_rotate_probe \
```

- [ ] **Step 6: Vacuity cases — append section 14 to `tools/gate-vacuity.test.sh`** (after section 13, before the final summary/exit)

```sh
# --- 14. pxp_rotate_probe (acid_box landscape, Phase 0) ---------------------
# The sum pins are individually meaningless unless each can be shown to FAIL,
# and the count guards exist because every pin is satisfied vacuously by a
# truncated table.  Fixture is the gate's own committed transcript_qemu.txt.
PRP="examples/display/pxp_rotate_probe"
if [ -d "$EVKB/$PRP" ] && [ -f "$EVKB/$PRP/transcript_qemu.txt" ]; then
    run_gate "$PRP" "run_qemu.sh" "$EVKB/$PRP/transcript_qemu.txt"; rc=$?
    [ "$rc" -eq 0 ] && result=0 || result=1
    report "green_still_passes_pxp_rotate_probe" $result

    # A corrupted sum must fail naming the case.
    sed 's|^case=knob api=ok pixel=ok sum=0x........|case=knob api=ok pixel=ok sum=0xBADBADBA|' \
        "$EVKB/$PRP/transcript_qemu.txt" > "$WORK/prp_badsum.txt"
    run_gate "$PRP" "run_qemu.sh" "$WORK/prp_badsum.txt"; rc=$?
    result=0
    [ "$rc" -ne 0 ] || result=1
    echo "$OUT_TEXT" | grep -q "missing/wrong: case=knob" || result=1
    report "prp_bad_sum_fails_by_name" $result

    # A truncated case table (a hung op) must fail on the COUNT, not pass on
    # the cases that did finish.  crc_done is kept so the wait loop returns.
    awk '/^case_begin=cell/ { skip=1 } /^crc_done/ { skip=0 } !skip' \
        "$EVKB/$PRP/transcript_qemu.txt" > "$WORK/prp_trunc.txt"
    run_gate "$PRP" "run_qemu.sh" "$WORK/prp_trunc.txt"; rc=$?
    result=0
    [ "$rc" -ne 0 ] || result=1
    echo "$OUT_TEXT" | grep -q "expected 12 case_begin lines" || result=1
    report "prp_truncated_table_fails_by_count" $result
else
    echo "SKIP: pxp_rotate_probe vacuity (example or fixture missing)"
fi
```

- [ ] **Step 7: Run the three new cases only** (the whole suite is ~9 min; the section is self-contained)

```bash
cd /Users/nicholasnewdigate/Development/rt1170/evkb && sh tools/gate-vacuity.test.sh 2>&1 | grep -E "pxp_rotate_probe|prp_"
```
Expected:
```
PASS: green_still_passes_pxp_rotate_probe
PASS: prp_bad_sum_fails_by_name
PASS: prp_truncated_table_fails_by_count
```

- [ ] **Step 8: Confirm the runner discovers it and the audit is clean**

```bash
tools/run-all-qemu-gates.sh -l | grep -c "" ; tools/run-all-qemu-gates.sh -l | grep pxp_rotate_probe
LICENSE_AUDIT_EVKB=$(pwd) sh tools/license-audit.sh 2>&1 | tail -3
```
Expected: `141` from the count (140 gates + the trailing summary line), `rt1176:display/pxp_rotate_probe` listed, `LICENSE-AUDIT: PASS`.

- [ ] **Step 9: Commit**

```bash
git add examples/display/pxp_rotate_probe/run_qemu.sh examples/display/pxp_rotate_probe/transcript_qemu.txt tools/license-audit.sh tools/gate-vacuity.test.sh
git commit -m "display: pxp_rotate_probe gate (140th) -- eleven pinned rotation sums, count guards, three vacuity negatives

Co-Authored-By: Claude Fable 5.1 <noreply@anthropic.com>"
```

---

### Task 3: the silicon probe (BENCH) — P1–P5 measured, the present strategy decided

**Files:**
- Create: `examples/display/pxp_rotate_probe/transcript_hw_evkb.txt`
- Modify: `docs/superpowers/specs/2026-09-14-acid-box-landscape-design.md` (append §4.1 "Measured")

Use the `flashing-rt1170-evkb` skill for every board operation. Reminders from CLAUDE.md that bite here: never hold the VCOM while programming; if `flash … load build/pxp_rotate_probe.elf` exits `-11`, load the `.hex` instead; `pkill LinkServer; pkill redlinkserv; pkill crt_emu_cm_redlink` first.

- [ ] **Step 1: Flash and capture**

```bash
cd examples/display/pxp_rotate_probe
pkill LinkServer; pkill redlinkserv; pkill crt_emu_cm_redlink
LinkServer flash MIMXRT1176:MIMXRT1170-EVKB load build/pxp_rotate_probe.elf
LinkServer flash MIMXRT1176:MIMXRT1170-EVKB verify build/pxp_rotate_probe.elf
```
Then start the reader on the MCU-Link VCOM and press SW4:
```bash
python3 ../../../tools/rt1170-console.py /dev/cu.usbmodem* 115200 | tee transcript_hw_evkb.txt
```
Stop after `probe_done` plus a few `hb=` lines. Press SW4 once more with the reader still attached so the file holds TWO boots.

- [ ] **Step 2: Read the verdicts BEFORE the numbers**

```bash
tr -d '\r\000' < transcript_hw_evkb.txt | grep -E "^case=|^cases=|^time="
```
Record in the transcript header, one line each:
* P4: every graded case `pixel=ok` on both boots, sums identical between boots, and each sum IDENTICAL to the QEMU pin in `run_qemu.sh` (the pattern is deterministic, so they must be). Any `pixel=broken` on a graded case = **P4 FAILS: stop here, go to spec §4's third branch** (the GC355 blit); do not continue to Task 9.
* P5: what `case=offgrid` reported (`api=`, `pixel=`).
* P1/P2/P3: `time=full`, `time=rect160`, `time=rect16` in µs (min of 8), from both boots.

- [ ] **Step 3: Decide the threshold — write the arithmetic into the spec's new §4.1**

Let `t_full`, `t160`, `t16` be the min-of-8 µs values. Per-pixel cost `c = (t160 - t16) / (160*160 - 16*16)` µs/px (fixed cost per op ≈ `t16`).
* If `t_full <= 5000`: **threshold = 0** (every present full-frame; `plan()` stays host-tested; the acid_box gate then asserts `ops == full`).
* Otherwise: **threshold = floor((t_full - 4*t16) / c / 256) * 256** logical px — the union area beyond which one full-frame op is cheaper than ~4 rect ops. Record the three inputs, the result, and the prediction table's outcome (which of P1–P3 held, which missed and by how much) in §4.1.

- [ ] **Step 4: Commit**

```bash
git add examples/display/pxp_rotate_probe/transcript_hw_evkb.txt docs/superpowers/specs/2026-09-14-acid-box-landscape-design.md
git commit -m "pxp_rotate_probe: silicon run -- P1-P5 measured, present threshold decided (spec section 4.1)

Co-Authored-By: Claude Fable 5.1 <noreply@anthropic.com>"
```

---

### Task 4: `lvgl_panel_rotation.h` — the pure geometry, test-first (LVGL repo)

**Files:**
- Create: `~/Development/LVGL/port/lvgl_panel_rotation.h`
- Create: `~/Development/LVGL/port/tests/lvgl_panel_rotation_test.c`
- Create: `~/Development/LVGL/port/tests/run.sh`

- [ ] **Step 1: Write the unit tests (the property test is Task 5)**

```c
/* lvgl_panel_rotation_test.c - host tests for port/lvgl_panel_rotation.h.
 * Run by port/tests/run.sh, which also compiles three MUTANT copies of the
 * header and requires each to FAIL here by name.
 * SPDX-License-Identifier: MIT */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "lvgl_panel_rotation.h"

static int fails = 0;
#define CHECK(name, cond) do { \
    if (cond) printf("PASS: %s\n", name); \
    else { printf("FAIL: %s\n", name); fails++; } } while (0)

static const lvgl_panel_rot_cfg_t RK055 = { 1280, 720, 720, 1280, 100000u };

static int rect_eq(lvgl_panel_rect_t a, int32_t x1, int32_t y1, int32_t x2, int32_t y2)
{ return a.x1 == x1 && a.y1 == y1 && a.x2 == x2 && a.y2 == y2; }

static void test_cfg(void)
{
    CHECK("cfg: RK055 landscape is valid", lvgl_panel_rot_cfg_valid(&RK055, LVGL_PANEL_PRESENT_CW90));
    lvgl_panel_rot_cfg_t bad = RK055; bad.phys_w = 721;
    CHECK("cfg: mismatched physical size is refused", !lvgl_panel_rot_cfg_valid(&bad, LVGL_PANEL_PRESENT_CW90));
    CHECK("cfg: an unknown present value is refused", !lvgl_panel_rot_cfg_valid(&RK055, (lvgl_panel_present_t)0));
}

static void test_map_rect(void)
{
    /* logical (x,y) -> physical (719-y, x): the top-left logical corner lands
     * at the panel's top-RIGHT (spec section 2, checked not assumed). */
    lvgl_panel_rect_t p = lvgl_panel_rot_map_rect(&RK055, (lvgl_panel_rect_t){ 0, 0, 0, 0 });
    CHECK("map_rect: logical (0,0) -> physical (719,0)", rect_eq(p, 719, 0, 719, 0));
    p = lvgl_panel_rot_map_rect(&RK055, (lvgl_panel_rect_t){ 1279, 719, 1279, 719 });
    CHECK("map_rect: logical (1279,719) -> physical (0,1279)", rect_eq(p, 0, 1279, 0, 1279));
    p = lvgl_panel_rot_map_rect(&RK055, (lvgl_panel_rect_t){ 0, 0, 1279, 719 });
    CHECK("map_rect: the whole frame maps to the whole panel", rect_eq(p, 0, 0, 719, 1279));
    /* the CUTOFF knob of layout C: (16,520) 150x150 -> physical (50..199, 16..165) */
    p = lvgl_panel_rot_map_rect(&RK055, (lvgl_panel_rect_t){ 16, 520, 165, 669 });
    CHECK("map_rect: layout C CUTOFF knob", rect_eq(p, 50, 16, 199, 165));
}

static void test_point_round_trip(void)
{
    int ok = 1;
    for (int32_t ly = 0; ly < 720 && ok; ly += 37)
        for (int32_t lx = 0; lx < 1280 && ok; lx += 41) {
            const int32_t px = 719 - ly, py = lx;          /* the forward map, written out */
            int32_t bx, by;
            lvgl_panel_rot_map_point_inv(&RK055, px, py, &bx, &by);
            if (bx != lx || by != ly) ok = 0;
        }
    CHECK("map_point_inv: inverts the forward map on a 37x41 lattice", ok);
    int32_t lx, ly;
    lvgl_panel_rot_map_point_inv(&RK055, 719, 0, &lx, &ly);
    CHECK("map_point_inv: physical top-right is logical (0,0)", lx == 0 && ly == 0);
    /* the gate's PLAY tap: raw P 94 85 -> physical (676,1088) -> logical (1088,43) */
    lvgl_panel_rot_map_point_inv(&RK055, 676, 1088, &lx, &ly);
    CHECK("map_point_inv: the gate's PLAY tap lands at (1088,43)", lx == 1088 && ly == 43);
}

static void test_snap(void)
{
    lvgl_panel_rect_t s = lvgl_panel_rot_snap(&RK055, (lvgl_panel_rect_t){ 3, 5, 20, 21 });
    CHECK("snap: grows outward to the 16 grid", rect_eq(s, 0, 0, 31, 31));
    s = lvgl_panel_rot_snap(&RK055, (lvgl_panel_rect_t){ 16, 32, 31, 47 });
    CHECK("snap: an aligned rect is unchanged", rect_eq(s, 16, 32, 31, 47));
    s = lvgl_panel_rot_snap(&RK055, (lvgl_panel_rect_t){ 1270, 710, 1279, 719 });
    CHECK("snap: clamps at the frame's far corner", rect_eq(s, 1264, 704, 1279, 719));
    s = lvgl_panel_rot_snap(&RK055, (lvgl_panel_rect_t){ -5, -7, 3, 3 });
    CHECK("snap: clamps at the origin", rect_eq(s, 0, 0, 15, 15));
    s = lvgl_panel_rot_snap(&RK055, (lvgl_panel_rect_t){ 0, 0, 1279, 719 });
    CHECK("snap: the whole frame stays the whole frame", rect_eq(s, 0, 0, 1279, 719));
    /* every snapped edge is on the grid, whatever went in */
    int ok = 1;
    for (int32_t i = 0; i < 300 && ok; i += 7) {
        lvgl_panel_rect_t r = { i, i / 2, i + 100, i / 2 + 60 };
        s = lvgl_panel_rot_snap(&RK055, r);
        if (s.x1 % 16 || s.y1 % 16 || (s.x2 + 1) % 16 || (s.y2 + 1) % 16) ok = 0;
        if (s.x1 > r.x1 || s.y1 > r.y1 || s.x2 < r.x2 || s.y2 < r.y2) ok = 0;
    }
    CHECK("snap: always on-grid and always a superset", ok);
}

static void test_plan(void)
{
    lvgl_panel_rot_plan_t plan;
    lvgl_panel_rect_t a = { 0, 0, 15, 15 }, b = { 8, 8, 31, 31 }, away = { 640, 352, 655, 367 };

    lvgl_panel_rot_plan(&RK055, NULL, 0, NULL, 0, &plan);
    CHECK("plan: no damage -> no op, not full", !plan.full && plan.n == 0);

    lvgl_panel_rot_plan(&RK055, &a, 1, &b, 1, &plan);
    CHECK("plan: overlapping cur+prev merge into one bbox", !plan.full && plan.n == 1 && rect_eq(plan.r[0], 0, 0, 31, 31));

    lvgl_panel_rot_plan(&RK055, &a, 1, &away, 1, &plan);
    CHECK("plan: disjoint cur+prev stay two rects", !plan.full && plan.n == 2);

    lvgl_panel_rect_t big = { 0, 0, 399, 299 };                 /* 120000 px >= 100000 */
    lvgl_panel_rot_plan(&RK055, &big, 1, NULL, 0, &plan);
    CHECK("plan: union area at/over the threshold -> full", plan.full && plan.n == 0);

    lvgl_panel_rect_t under = { 0, 0, 399, 239 };               /* 96000 px < 100000 */
    lvgl_panel_rot_plan(&RK055, &under, 1, NULL, 0, &plan);
    CHECK("plan: union area under the threshold -> rects", !plan.full && plan.n == 1);

    lvgl_panel_rot_cfg_t always = RK055; always.full_threshold_px = 0;
    lvgl_panel_rot_plan(&always, &a, 1, NULL, 0, &plan);
    CHECK("plan: threshold 0 means every present is full", plan.full);
    lvgl_panel_rot_plan(&always, NULL, 0, NULL, 0, &plan);
    CHECK("plan: threshold 0 with no damage is still no op", !plan.full && plan.n == 0);

    lvgl_panel_rect_t empty = { 10, 10, 5, 5 };                  /* x2 < x1: not a rect */
    lvgl_panel_rot_plan(&RK055, &empty, 1, NULL, 0, &plan);
    CHECK("plan: a degenerate rect is dropped", !plan.full && plan.n == 0);
}

/* the property test (Task 5) is appended below this line */

int main(void)
{
    test_cfg();
    test_map_rect();
    test_point_round_trip();
    test_snap();
    test_plan();
    printf("%s: lvgl_panel_rotation host tests, %d failure(s)\n", fails ? "FAIL" : "PASS", fails);
    return fails ? 1 : 0;
}
```

- [ ] **Step 2: Write `run.sh` (clean build only for now; the mutant arms are Task 5)**

```sh
#!/bin/sh
# Builds and runs the LVGL port's host test suite; exits non-zero on any failure.
# Copyright (c) 2026 Nicholas Newdigate
# SPDX-License-Identifier: MIT
#
# Covers port/lvgl_panel_rotation.h, which is PURE by design so it needs no
# LVGL, no target and no PXP.  That matters: every QEMU gate in the rt1176-evkb
# tree sees a rotated GOLDEN, not the arithmetic that produced it.
set -eu
here=$(cd "$(dirname "$0")/.." && pwd)          # port/
out=$(mktemp -d)
trap 'rm -rf "$out"' EXIT INT TERM HUP
cc -std=c99 -Wall -Wextra -Werror -I "$here" -o "$out/rot_test" "$here/tests/lvgl_panel_rotation_test.c"
"$out/rot_test"
```

- [ ] **Step 3: Run it, expect a compile failure (the header does not exist)**

```bash
cd ~/Development/LVGL && chmod +x port/tests/run.sh && port/tests/run.sh; echo "exit=$?"
```
Expected: `fatal error: lvgl_panel_rotation.h: No such file or directory`, exit non-zero.

- [ ] **Step 4: Write the header**

```c
/* lvgl_panel_rotation.h - pure geometry for presenting an LVGL display ROTATED
 * on a panel whose scanout orientation differs from the UI's.
 * Spec: rt1176-evkb docs/superpowers/specs/2026-09-14-acid-box-landscape-design.md
 * Copyright (c) 2026 Nicholas Newdigate
 * SPDX-License-Identifier: MIT
 *
 * PURE C, no LVGL types, no hardware: host-tested by port/tests/run.sh, which
 * is the only automated coverage the present path's arithmetic has -- the
 * QEMU gates see the result (a rotated golden), not how it was produced.
 *
 * Deliberately NOT lv_display_rotation_t: that enum selects LVGL's own
 * rotation machinery (matrix transform / partial-mode software rotate), which
 * this port does not use, and its direction convention is the opposite of the
 * one below.  One value exists because one value has evidence behind it. */
#pragma once
#include <stdint.h>
#include <stdbool.h>

/* The picture is rotated 90 degrees CLOCKWISE on the glass: the top of the UI
 * lands on the panel's physical RIGHT edge -- the edge that is on top once the
 * board is turned counter-clockwise.  This is exactly PXP_ROT_90.
 *   logical  (x, y)   -> physical (phys_w - 1 - y, x)
 *   physical (px, py) -> logical  (py, phys_w - 1 - px) */
typedef enum { LVGL_PANEL_PRESENT_CW90 = 1 } lvgl_panel_present_t;

/* Inclusive corners -- lv_area_t's shape, without the dependency. */
typedef struct { int32_t x1, y1, x2, y2; } lvgl_panel_rect_t;

typedef struct {
    int32_t  log_w, log_h;         /* the LVGL display: 1280 x 720 on the RK055 */
    int32_t  phys_w, phys_h;       /* the scanout buffer: 720 x 1280 */
    uint32_t full_threshold_px;    /* damage union >= this -> ONE full-frame op; 0 = ALWAYS full */
} lvgl_panel_rot_cfg_t;

/* Snap grid.  16 px keeps every derived pointer 64-byte aligned at 4 bpp
 * (x*4, (phys_w-y-h)*4, and the row offsets y*5120 / x*2880 are all multiples
 * of 64), which is what pxp_rotate_probe proved the rotation path on. */
#define LVGL_PANEL_ROT_GRID      16
#define LVGL_PANEL_ROT_MAX_RECTS 64        /* 2 * LV_INV_BUF_SIZE */

typedef struct {
    bool full;                                    /* one whole-frame op; r[] unused */
    int  n;
    lvgl_panel_rect_t r[LVGL_PANEL_ROT_MAX_RECTS];  /* LOGICAL, grid-snapped, pairwise disjoint */
} lvgl_panel_rot_plan_t;

static inline bool lvgl_panel_rot_cfg_valid(const lvgl_panel_rot_cfg_t *c, lvgl_panel_present_t p)
{
    return p == LVGL_PANEL_PRESENT_CW90 &&
           c->log_w > 0 && c->log_h > 0 &&
           c->log_w == c->phys_h && c->log_h == c->phys_w &&
           (c->log_w % LVGL_PANEL_ROT_GRID) == 0 && (c->log_h % LVGL_PANEL_ROT_GRID) == 0;
}

static inline lvgl_panel_rect_t lvgl_panel_rot_map_rect(const lvgl_panel_rot_cfg_t *c, lvgl_panel_rect_t l)
{
    lvgl_panel_rect_t p;
    p.x1 = c->phys_w - 1 - l.y2;
    p.x2 = c->phys_w - 1 - l.y1;
    p.y1 = l.x1;
    p.y2 = l.x2;
    return p;
}

static inline void lvgl_panel_rot_map_point_inv(const lvgl_panel_rot_cfg_t *c, int32_t px, int32_t py,
                                                int32_t *lx, int32_t *ly)
{
    *lx = py;
    *ly = c->phys_w - 1 - px;
}

/* Grow OUTWARD to the grid (never inward: a shrunk rect leaves a stale sliver),
 * clamped to the frame. */
static inline lvgl_panel_rect_t lvgl_panel_rot_snap(const lvgl_panel_rot_cfg_t *c, lvgl_panel_rect_t l)
{
    const int32_t G = LVGL_PANEL_ROT_GRID;
    lvgl_panel_rect_t s;
    if (l.x1 < 0) l.x1 = 0;
    if (l.y1 < 0) l.y1 = 0;
    if (l.x2 > c->log_w - 1) l.x2 = c->log_w - 1;
    if (l.y2 > c->log_h - 1) l.y2 = c->log_h - 1;
    s.x1 = (l.x1 / G) * G;
    s.y1 = (l.y1 / G) * G;
    s.x2 = ((l.x2 + G) / G) * G - 1;
    s.y2 = ((l.y2 + G) / G) * G - 1;
    if (s.x2 > c->log_w - 1) s.x2 = c->log_w - 1;
    if (s.y2 > c->log_h - 1) s.y2 = c->log_h - 1;
    return s;
}

static inline bool lvgl_panel_rot_overlap_(lvgl_panel_rect_t a, lvgl_panel_rect_t b)
{
    return a.x1 <= b.x2 && b.x1 <= a.x2 && a.y1 <= b.y2 && b.y1 <= a.y2;
}

/* The present plan for a double-buffered scanout: the back buffer is TWO
 * presents stale, so it needs this frame's damage (`cur`) AND the previous
 * present's (`prev`).  Both lists are snapped, overlapping pairs are merged
 * into their bounding box (a SUPERSET of both -- coverage can only grow, never
 * a hole), and the union's area decides rects vs one full-frame op. */
static inline void lvgl_panel_rot_plan(const lvgl_panel_rot_cfg_t *c,
                                       const lvgl_panel_rect_t *cur, int ncur,
                                       const lvgl_panel_rect_t *prev, int nprev,
                                       lvgl_panel_rot_plan_t *out)
{
    int i, j;
    bool changed;
    uint32_t area = 0;
    out->full = false;
    out->n = 0;
    if (ncur < 0) ncur = 0;
    if (nprev < 0) nprev = 0;
    if (ncur + nprev > LVGL_PANEL_ROT_MAX_RECTS) { out->full = true; return; }
    for (i = 0; i < ncur; i++)
        if (cur[i].x2 >= cur[i].x1 && cur[i].y2 >= cur[i].y1)
            out->r[out->n++] = lvgl_panel_rot_snap(c, cur[i]);
    for (i = 0; i < nprev; i++)
        if (prev[i].x2 >= prev[i].x1 && prev[i].y2 >= prev[i].y1)
            out->r[out->n++] = lvgl_panel_rot_snap(c, prev[i]);
    if (out->n == 0) return;                          /* nothing changed: no op at all */
    if (c->full_threshold_px == 0) { out->full = true; out->n = 0; return; }
    changed = true;
    while (changed) {
        changed = false;
        for (i = 0; i < out->n && !changed; i++)
            for (j = i + 1; j < out->n; j++)
                if (lvgl_panel_rot_overlap_(out->r[i], out->r[j])) {
                    lvgl_panel_rect_t u = out->r[i];
                    if (out->r[j].x1 < u.x1) u.x1 = out->r[j].x1;
                    if (out->r[j].y1 < u.y1) u.y1 = out->r[j].y1;
                    if (out->r[j].x2 > u.x2) u.x2 = out->r[j].x2;
                    if (out->r[j].y2 > u.y2) u.y2 = out->r[j].y2;
                    out->r[i] = u;
                    out->r[j] = out->r[out->n - 1];
                    out->n--;
                    changed = true;
                    break;
                }
    }
    for (i = 0; i < out->n; i++)
        area += (uint32_t)(out->r[i].x2 - out->r[i].x1 + 1) * (uint32_t)(out->r[i].y2 - out->r[i].y1 + 1);
    if (area >= c->full_threshold_px) { out->full = true; out->n = 0; }
}
```

- [ ] **Step 5: Run the suite, expect all PASS**

```bash
port/tests/run.sh; echo "exit=$?"
```
Expected: every `PASS:` line above, the final `PASS: lvgl_panel_rotation host tests, 0 failure(s)`, exit 0.

- [ ] **Step 6: Commit (LVGL repo)**

```bash
cd ~/Development/LVGL && git add port/lvgl_panel_rotation.h port/tests/lvgl_panel_rotation_test.c port/tests/run.sh
git commit -m "port: lvgl_panel_rotation.h -- pure CW90 present geometry (map_rect, map_point_inv, snap16, plan) with a host suite

Co-Authored-By: Claude Fable 5.1 <noreply@anthropic.com>"
```

---

### Task 5: the property test and the three mutant arms (LVGL repo)

**Files:**
- Modify: `~/Development/LVGL/port/tests/lvgl_panel_rotation_test.c` (replace the marker comment before `main`)
- Modify: `~/Development/LVGL/port/tests/run.sh` (append the arms)

- [ ] **Step 1: Add the property test** — replace the line `/* the property test (Task 5) is appended below this line */` with:

```c
/* --- property: presented buffer == rotated canvas, always -------------------
 * Simulates the port: a canvas LVGL damages, two scanout buffers that
 * alternate, plan() deciding what to rotate, map_rect placing it.  The
 * reference rotator is written PER PIXEL, independently of map_rect. */
static uint32_t lcg = 12345u;
static uint32_t rnd(void) { lcg = lcg * 1664525u + 1013904223u; return lcg >> 8; }

static void ref_rotate(const lvgl_panel_rot_cfg_t *c, const uint32_t *canvas, uint32_t *phys)
{
    for (int32_t py = 0; py < c->phys_h; py++)
        for (int32_t px = 0; px < c->phys_w; px++)
            phys[py * c->phys_w + px] = canvas[(c->phys_w - 1 - px) * c->log_w + py];
}

/* The port's present, simulated: plan -> map_rect -> rotate each rect into `back`. */
static int sim_present(const lvgl_panel_rot_cfg_t *c, const uint32_t *canvas, uint32_t *back,
                       const lvgl_panel_rect_t *cur, int ncur, const lvgl_panel_rect_t *prev, int nprev,
                       int force_full, uint32_t *ops)
{
    lvgl_panel_rot_plan_t plan;
    lvgl_panel_rot_plan(c, cur, ncur, prev, nprev, &plan);
    if (force_full) { plan.full = true; plan.n = 0; }
    if (plan.full) { ref_rotate(c, canvas, back); (*ops)++; return 1; }
    for (int i = 0; i < plan.n; i++) {
        lvgl_panel_rect_t p = lvgl_panel_rot_map_rect(c, plan.r[i]);
        if (p.x1 < 0 || p.y1 < 0 || p.x2 >= c->phys_w || p.y2 >= c->phys_h || p.x2 < p.x1 || p.y2 < p.y1) {
            printf("FAIL: property: map_rect produced an out-of-range physical rect\n");
            return 0;
        }
        for (int32_t py = p.y1; py <= p.y2; py++)
            for (int32_t px = p.x1; px <= p.x2; px++)
                back[py * c->phys_w + px] = canvas[(c->phys_w - 1 - px) * c->log_w + py];
        (*ops)++;
    }
    return 1;
}

static void property(const lvgl_panel_rot_cfg_t *c, int frames, const char *name)
{
    const size_t npx = (size_t)c->log_w * (size_t)c->log_h;
    uint32_t *canvas = calloc(npx, 4), *ref = calloc(npx, 4);
    uint32_t *buf[2] = { malloc(npx * 4), malloc(npx * 4) };
    lvgl_panel_rect_t cur[8], prev[8];
    int ncur = 0, nprev = 0, forced = 2, ok = 1;
    uint32_t ops = 0;
    memset(buf[0], 0xEE, npx * 4);          /* undefined start-up contents, deliberately unequal */
    memset(buf[1], 0xDD, npx * 4);
    for (int f = 0; f < frames && ok; f++) {
        ncur = (int)(rnd() % 5);            /* 0..4 damage rects, painted with fresh values */
        for (int i = 0; i < ncur; i++) {
            int32_t x1 = (int32_t)(rnd() % (uint32_t)c->log_w), y1 = (int32_t)(rnd() % (uint32_t)c->log_h);
            int32_t x2 = x1 + (int32_t)(rnd() % 200), y2 = y1 + (int32_t)(rnd() % 200);
            if (x2 > c->log_w - 1) x2 = c->log_w - 1;
            if (y2 > c->log_h - 1) y2 = c->log_h - 1;
            cur[i].x1 = x1; cur[i].y1 = y1; cur[i].x2 = x2; cur[i].y2 = y2;
            for (int32_t y = y1; y <= y2; y++)
                for (int32_t x = x1; x <= x2; x++) canvas[y * c->log_w + x] = rnd();
        }
        uint32_t *back = buf[f & 1];
        if (!sim_present(c, canvas, back, cur, ncur, prev, nprev, forced > 0, &ops)) { ok = 0; break; }
        if (forced > 0) forced--;
        ref_rotate(c, canvas, ref);
        if (memcmp(back, ref, npx * 4) != 0) {
            printf("FAIL: property: %s: presented buffer != rotated canvas at frame %d\n", name, f);
            ok = 0;
        }
        memcpy(prev, cur, sizeof(cur));
        nprev = ncur;
    }
    if (ok) printf("PASS: property: %s (%d frames, %u ops)\n", name, frames, (unsigned)ops);
    else fails++;
    free(canvas); free(ref); free(buf[0]); free(buf[1]);
}

static void test_property(void)
{
    const lvgl_panel_rot_cfg_t small = { 256, 128, 128, 256, 6000u };
    property(&small, 2000, "256x128, threshold 6000");
    const lvgl_panel_rot_cfg_t small_full = { 256, 128, 128, 256, 0u };
    property(&small_full, 300, "256x128, always full");
    property(&RK055, 60, "1280x720 RK055, threshold 100000");
}
```
and add `test_property();` to `main()` after `test_plan();`.

- [ ] **Step 2: Run, expect PASS**

```bash
cd ~/Development/LVGL && port/tests/run.sh | tail -5
```
Expected: three `PASS: property: …` lines and `0 failure(s)`.

- [ ] **Step 3: Append the mutant arms to `run.sh`**

```sh

# --- NEGATIVE ARMS: three mutants of the header, each must FAIL by name -----
# A suite that only passes is equally consistent with a suite that cannot see
# anything.  Each arm sed-edits a COPY of the header, proves the edit applied
# (an unmatched sed is a vacuous arm), compiles the test against the copy, and
# requires a non-zero exit WITH a "FAIL:" line naming the check.
fail=0
arm() { # <name> <sed-expr>
    mkdir -p "$out/$1"
    sed "$2" "$here/lvgl_panel_rotation.h" > "$out/$1/lvgl_panel_rotation.h"
    if cmp -s "$here/lvgl_panel_rotation.h" "$out/$1/lvgl_panel_rotation.h"; then
        echo "FAIL: mutant $1 did not apply (sed matched nothing)"; fail=1; return
    fi
    cp "$here/tests/lvgl_panel_rotation_test.c" "$out/$1/"
    cc -std=c99 -Wall -Wextra -Werror -o "$out/$1/t" "$out/$1/lvgl_panel_rotation_test.c"
    if "$out/$1/t" > "$out/$1/log" 2>&1; then
        echo "FAIL: mutant $1 PASSED the suite -- the tests cannot see it"; fail=1
    elif grep -q "^FAIL: " "$out/$1/log"; then
        echo "PASS: mutant $1 caught -- $(grep -m1 '^FAIL: ' "$out/$1/log")"
    else
        echo "FAIL: mutant $1 died without naming a check"; fail=1
    fi
}
# 1. a wrong mapping (identity on the x axis instead of the CW90 reflection)
arm wrong_map   's/p.x1 = c->phys_w - 1 - l.y2;/p.x1 = l.y1;/; s/p.x2 = c->phys_w - 1 - l.y1;/p.x2 = l.y2;/'
# 2. a snap that shrinks the left edge inward instead of growing it outward
arm snap_shrink 's|s.x1 = (l.x1 / G) \* G;|s.x1 = ((l.x1 + G - 1) / G) * G;|'
# 3. a plan that forgets the previous present's damage
arm forget_prev 's/for (i = 0; i < nprev; i++)/for (i = 0; i < 0 * nprev; i++)/'
exit $fail
```
Note: the quoted `#include "lvgl_panel_rotation.h"` in the test searches the includer's directory FIRST, which is why the test is copied beside the mutated header and why the clean build passes `-I "$here"` instead.

- [ ] **Step 4: Run the whole suite**

```bash
port/tests/run.sh; echo "exit=$?"
```
Expected, at the end:
```
PASS: mutant wrong_map caught -- FAIL: map_rect: logical (0,0) -> physical (719,0)
PASS: mutant snap_shrink caught -- FAIL: snap: grows outward to the 16 grid
PASS: mutant forget_prev caught -- FAIL: plan: overlapping cur+prev merge into one bbox
exit=0
```
(The named check may differ — any `FAIL:` line is acceptable — but each arm MUST read `PASS: mutant … caught`.) If an arm reads `did not apply`, the sed pattern no longer matches the header text: fix the pattern, never the header.

- [ ] **Step 5: Commit (LVGL repo)**

```bash
git add port/tests/lvgl_panel_rotation_test.c port/tests/run.sh
git commit -m "port/tests: rotation property test (alternating scanout buffers vs a per-pixel reference) + three mutant arms that must fail by name

Co-Authored-By: Claude Fable 5.1 <noreply@anthropic.com>"
```

---

### Task 6: `lvgl_mipi_panel_create_rotated()` (LVGL repo)

**Files:**
- Modify: `~/Development/LVGL/port/lvgl_mipi_panel.h` (append)
- Modify: `~/Development/LVGL/port/lvgl_mipi_panel.cpp` (`#include <PXP.h>` after `#include <lcdifv2.h>`; append the rotated section after the existing accessors at the end of the file)

Every example that compiles this file already imports PXP (checked: 16 of 16 `CMakeLists.txt`), so the new include costs nothing.

- [ ] **Step 1: Header additions** — append to `lvgl_mipi_panel.h`:

```cpp
/* --- rotated present (acid_box landscape, spec 2026-09-14) -------------------
 * LVGL renders a landscape canvas (PANEL_HEIGHT x PANEL_WIDTH: 1280x720 on the
 * RK055) in DIRECT mode with ONE buffer that is never scanned out.  On the last
 * flush of each refresh the port (1) runs the pre-flip compose hook on the
 * canvas, (2) waits for the previous flip to retire (flip_sync), (3) PXP-
 * rotates this refresh's damage plus the previous present's -- or the whole
 * frame -- into the off-screen scanout buffer, (4) flips.  Every presented
 * buffer is therefore a COMPLETE frame: tear-free by construction, exactly as
 * the db path is.  The first two presents are forced full-frame because both
 * scanout buffers start undefined; a PXP error counts, forces the next two
 * presents full, and still flips (the pipeline cannot wedge).
 *
 * Geometry (mapping, snapping, planning) is lvgl_panel_rotation.h -- pure and
 * host-tested.  The panel must be PANEL_BYTES_PER_PIXEL == 4 (asserted).
 * create()/create_db() are untouched by this path. */
#include "lvgl_panel_rotation.h"
lv_display_t *lvgl_mipi_panel_create_rotated(DisplayClass &display, lvgl_panel_present_t present);

/* Damage-union area (logical px) at or above which a present rotates the whole
 * frame in ONE op rather than per rect; 0 = every present full-frame.  Call
 * BEFORE create_rotated().  The value is the Phase 0 probe's decision
 * (display/pxp_rotate_probe, spec section 4.1); the default is 0. */
void lvgl_mipi_panel_set_rot_threshold(uint32_t px);

/* The canvas (nullptr before create_rotated()).  After flip_sync() the scanned
 * buffer is this canvas rotated -- the equality an example can check. */
const uint8_t *lvgl_mipi_panel_canvas();

/* Diagnostics since create_rotated(): PXP ops issued, of which full-frame,
 * logical pixels rotated, cumulative microseconds inside the ops, errors. */
uint32_t lvgl_mipi_panel_rot_ops();
uint32_t lvgl_mipi_panel_rot_full();
uint32_t lvgl_mipi_panel_rot_px();
uint32_t lvgl_mipi_panel_rot_us();
uint32_t lvgl_mipi_panel_rot_errors();
```

- [ ] **Step 2: Implementation** — add `#include <PXP.h>` under `#include <lcdifv2.h>`, then append at the end of `lvgl_mipi_panel.cpp`:

```cpp
/* --- rotated present -------------------------------------------------------
 * See the header.  Shares the db path's fence (db_vsync_isr, flip_sync, the
 * pending/scanned pointers and their counters) and its pre-flip hook; owns the
 * canvas, the damage lists and the PXP ops. */
#define ROT_LOG_W        ((int32_t)PANEL_HEIGHT)
#define ROT_LOG_H        ((int32_t)PANEL_WIDTH)
#define ROT_BPP          ((uint32_t)PANEL_BYTES_PER_PIXEL)
#define ROT_LOG_PITCH    ((uint32_t)ROT_LOG_W * ROT_BPP)
#define ROT_CANVAS_BYTES (ROT_LOG_PITCH * (uint32_t)ROT_LOG_H)
static_assert(ROT_LOG_PITCH % LV_DRAW_BUF_STRIDE_ALIGN == 0,
              "the landscape canvas pitch must not be padded by LVGL: the PXP "
              "source surface reads it at exactly PANEL_HEIGHT * bpp");

/* LVGL joins a refresh's damage into at most LV_INV_BUF_SIZE areas
 * (lv_display_private.h, default 32 -- a PRIVATE header, so the number is
 * restated here rather than included). */
#define ROT_MAX_AREAS 32
static uint8_t  *s_rot_canvas = nullptr;
static uint16_t *s_rot_bufs[2] = { nullptr, nullptr };   /* [0] = alt, [1] = the boot scanout buffer */
static lvgl_panel_rot_cfg_t s_rot_cfg;
static uint32_t s_rot_threshold = 0;
static lvgl_panel_rect_t s_rot_cur[ROT_MAX_AREAS], s_rot_prev[ROT_MAX_AREAS];
static int s_rot_ncur = 0, s_rot_nprev = 0;
static int s_rot_forced_full = 0;
static uint32_t s_rot_ops = 0, s_rot_full = 0, s_rot_px = 0, s_rot_us = 0, s_rot_errors = 0;

void lvgl_mipi_panel_set_rot_threshold(uint32_t px) { s_rot_threshold = px; }

static inline PXPFormat rot_format() { return PANEL_BYTES_PER_PIXEL == 4 ? PXP_XRGB8888 : PXP_RGB565; }

/* One PXP op per planned rect (or one for the frame).  Pointer-offset
 * surfaces, never outputAt(): the QEMU model writes a rotated op from the
 * output origin and ignores OUT_PS for it, so a source pointing INTO the
 * canvas and an output pointing INTO the scanout buffer make every op a
 * rotation at the origin -- the path display/pxp_rotate_probe proved in QEMU
 * and on silicon.  For a logical rect (x,y,w,h) the physical place is
 * (phys_w-y-h, x, h, w), which is what map_rect returns. */
static void rot_present(uint16_t *back, const lvgl_panel_rot_plan_t *plan)
{
    const uint32_t t0 = micros();
    if (plan->full) {
        PXPSurface src(s_rot_canvas, (uint16_t)ROT_LOG_W, (uint16_t)ROT_LOG_H, rot_format(), (uint16_t)ROT_LOG_PITCH);
        PXPSurface out(back, (uint16_t)PANEL_WIDTH, (uint16_t)PANEL_HEIGHT, rot_format(), (uint16_t)PANEL_PITCH_BYTES);
        if (PXP.op().source(src).output(out).rotate(PXP_ROT_90).run() != PXP_OK) {
            s_rot_errors++;
            s_rot_forced_full = 2;
        }
        s_rot_ops++; s_rot_full++;
        s_rot_px += (uint32_t)ROT_LOG_W * (uint32_t)ROT_LOG_H;
    } else {
        for (int i = 0; i < plan->n; i++) {
            const lvgl_panel_rect_t l = plan->r[i];
            const lvgl_panel_rect_t p = lvgl_panel_rot_map_rect(&s_rot_cfg, l);
            const uint16_t w = (uint16_t)(l.x2 - l.x1 + 1), h = (uint16_t)(l.y2 - l.y1 + 1);
            uint8_t *sp = s_rot_canvas + (uint32_t)l.y1 * ROT_LOG_PITCH + (uint32_t)l.x1 * ROT_BPP;
            uint8_t *dp = (uint8_t *)back + (uint32_t)p.y1 * PANEL_PITCH_BYTES + (uint32_t)p.x1 * ROT_BPP;
            PXPSurface src(sp, w, h, rot_format(), (uint16_t)ROT_LOG_PITCH);
            PXPSurface out(dp, h, w, rot_format(), (uint16_t)PANEL_PITCH_BYTES);   /* the rotated extent */
            if (PXP.op().source(src).output(out).rotate(PXP_ROT_90).run() != PXP_OK) {
                s_rot_errors++;
                s_rot_forced_full = 2;     /* both buffers may now be stale: two full presents */
                break;
            }
            s_rot_ops++;
            s_rot_px += (uint32_t)w * (uint32_t)h;
        }
    }
    s_rot_us += (uint32_t)(micros() - t0);
}

static void rot_flush_cb(lv_display_t *disp, const lv_area_t *area, uint8_t *px_map)
{
    (void)px_map;                    /* DIRECT mode: always the canvas base */
    s_flushed_px += (uint32_t)lv_area_get_size(area);
    if (s_rot_ncur < ROT_MAX_AREAS) {
        s_rot_cur[s_rot_ncur].x1 = area->x1; s_rot_cur[s_rot_ncur].y1 = area->y1;
        s_rot_cur[s_rot_ncur].x2 = area->x2; s_rot_cur[s_rot_ncur].y2 = area->y2;
        s_rot_ncur++;
    } else {
        /* Cannot happen (LVGL joins into LV_INV_BUF_SIZE slots), but a dropped
         * rect would be a stale region, so collapse to the whole frame. */
        s_rot_cur[0].x1 = 0; s_rot_cur[0].y1 = 0;
        s_rot_cur[0].x2 = ROT_LOG_W - 1; s_rot_cur[0].y2 = ROT_LOG_H - 1;
        s_rot_ncur = 1;
    }
    if (!lv_display_flush_is_last(disp)) { lv_display_flush_ready(disp); return; }

    /* 1. the GPU compositor adds its pixels to the canvas (it vg_lite_finish()es). */
    if (s_db_preflip_cb) s_db_preflip_cb(s_rot_canvas);
    /* 2. the back buffer may still be on the glass until the previous flip retires. */
    lvgl_mipi_panel_flip_sync();
    const uint16_t *front = s_db_scanned_fb ? s_db_scanned_fb : s_rot_bufs[1];
    uint16_t *back = (front == s_rot_bufs[0]) ? s_rot_bufs[1] : s_rot_bufs[0];
    /* 3. rotate this refresh's damage + the previous present's into it. */
    bool force = false;
    if (s_rot_forced_full > 0) { s_rot_forced_full--; force = true; }
    lvgl_panel_rot_plan_t plan;
    lvgl_panel_rot_plan(&s_rot_cfg, s_rot_cur, s_rot_ncur, s_rot_prev, s_rot_nprev, &plan);
    if (force) { plan.full = true; plan.n = 0; }
    rot_present(back, &plan);        /* synchronous: LVGL cannot render into the canvas while the PXP reads it */
    /* 4. present -- same ordering as db_flush_cb: pending-store AFTER FlipTo. */
    lcdifv2FlipTo(back);
    s_db_pending_fb = back;
    s_db_flips++;
    s_frame_done = true;
    /* 5. this refresh's damage is the next present's "previous". */
    memcpy(s_rot_prev, s_rot_cur, sizeof(lvgl_panel_rect_t) * (size_t)s_rot_ncur);
    s_rot_nprev = s_rot_ncur;
    s_rot_ncur = 0;
    /* No flush_wait_cb on this path: the canvas is never scanned out, so
     * there is nothing for LVGL to wait for before its next render. */
    lv_display_flush_ready(disp);
}

lv_display_t *lvgl_mipi_panel_create_rotated(DisplayClass &display, lvgl_panel_present_t present)
{
    depth_probe_reference();
    LV_ASSERT_NULL(display.framebuffer());   /* Display.begin() must have succeeded */
    LV_ASSERT(display.width() == PANEL_WIDTH && display.height() == PANEL_HEIGHT);
    LV_ASSERT(present == LVGL_PANEL_PRESENT_CW90);
    LV_ASSERT(PANEL_BYTES_PER_PIXEL == 4);   /* XRGB8888 only: the grid arithmetic and the probe are 4 bpp */
    /* Not inside the assert: a side effect in an assert vanishes with it. */
    const bool pxp_ok = PXP.begin();         /* idempotent; Display::fillScreen() may have run it already */
    LV_ASSERT(pxp_ok);

    uint16_t *alt = lcdifv2AllocAltFramebuffer();
    LV_ASSERT_NULL(alt);
    /* The canvas: extmem_malloc only 4-byte-aligns, so over-allocate and round
     * up to 64 (the PXP source's burst alignment), the alt-buffer pattern.
     * CALL ONCE -- no free path. */
    uint8_t *raw = (uint8_t *)extmem_malloc(ROT_CANVAS_BYTES + 64u);
    LV_ASSERT_NULL(raw);
    s_rot_canvas = (uint8_t *)(((uintptr_t)raw + 63u) & ~(uintptr_t)63u);
    memset(s_rot_canvas, 0, ROT_CANVAS_BYTES);

    s_rot_bufs[0] = alt;
    s_rot_bufs[1] = display.framebuffer();
    s_rot_cfg.log_w = ROT_LOG_W;             s_rot_cfg.log_h = ROT_LOG_H;
    s_rot_cfg.phys_w = (int32_t)PANEL_WIDTH; s_rot_cfg.phys_h = (int32_t)PANEL_HEIGHT;
    s_rot_cfg.full_threshold_px = s_rot_threshold;
    LV_ASSERT(lvgl_panel_rot_cfg_valid(&s_rot_cfg, present));
    s_rot_ncur = s_rot_nprev = 0;
    s_rot_forced_full = 2;                   /* both scanout buffers start undefined */
    s_rot_ops = s_rot_full = s_rot_px = s_rot_us = s_rot_errors = 0;

    s_frame_done = false;
    s_flushed_px = 0;
    s_db_pending_fb = nullptr;
    s_db_scanned_fb = nullptr;
    s_db_isr_retires = 0;
    s_db_retires_consumed = 0;
    s_db_flips = s_db_vsyncs = s_db_vsync_timeouts = 0;
    s_db_wait_us = 0;

    lv_display_t *disp = lv_display_create(ROT_LOG_W, ROT_LOG_H);
    lv_display_set_flush_cb(disp, rot_flush_cb);
    /* ONE buffer, DIRECT: the canvas always holds the whole current frame. */
    lv_display_set_buffers(disp, s_rot_canvas, nullptr, ROT_CANVAS_BYTES,
                           LV_DISPLAY_RENDER_MODE_DIRECT);
    /* Attach LAST, after all state is initialised -- the ISR runs from the
     * next vsync on (create_db's rule). */
    lcdifv2AttachVsyncInterrupt(db_vsync_isr);
    return disp;
}

const uint8_t *lvgl_mipi_panel_canvas()  { return s_rot_canvas; }
uint32_t lvgl_mipi_panel_rot_ops()       { return s_rot_ops; }
uint32_t lvgl_mipi_panel_rot_full()      { return s_rot_full; }
uint32_t lvgl_mipi_panel_rot_px()        { return s_rot_px; }
uint32_t lvgl_mipi_panel_rot_us()        { return s_rot_us; }
uint32_t lvgl_mipi_panel_rot_errors()    { return s_rot_errors; }
```

- [ ] **Step 3: Prove the untouched paths still build byte-identically** — rebuild an existing RGB565 consumer and an XRGB8888 one; neither uses the new function, so their LOADABLE image must not change:

```bash
OBJCOPY=/Applications/ARM_10/bin/arm-none-eabi-objcopy
cd /Users/nicholasnewdigate/Development/rt1170/evkb/examples/display/lvgl_rk055_touch_test && \
  $OBJCOPY -O binary build/lvgl_rk055_touch_test.elf /tmp/touch_before.bin && cmake --build build && \
  $OBJCOPY -O binary build/lvgl_rk055_touch_test.elf /tmp/touch_after.bin && cmp /tmp/touch_before.bin /tmp/touch_after.bin && echo IDENTICAL
cd ../rotary_knob_bench && \
  $OBJCOPY -O binary build/rotary_knob_bench.elf /tmp/rkb_before.bin && cmake --build build && \
  $OBJCOPY -O binary build/rotary_knob_bench.elf /tmp/rkb_after.bin && cmp /tmp/rkb_before.bin /tmp/rkb_after.bin && echo IDENTICAL
```
Expected: `IDENTICAL` twice (`--gc-sections` drops the unreferenced rotated code). If a build dir refuses to configure with `COMPILERPATH is UNDEFINED`, it is the stale-build-dir trap: `rm -rf build` and configure fresh, then compare against the committed golden by running that example's gate instead of `cmp`.

- [ ] **Step 4: Commit (LVGL repo)**

```bash
cd ~/Development/LVGL && git add port/lvgl_mipi_panel.h port/lvgl_mipi_panel.cpp
git commit -m "port: lvgl_mipi_panel_create_rotated -- landscape canvas, PXP CW90 present into the off-screen scanout buffer (full-frame or 16-px damage grid), tear-free by construction

Co-Authored-By: Claude Fable 5.1 <noreply@anthropic.com>"
```

---

### Task 7: `lvgl_gt911_indev_create_rotated()` (LVGL repo)

**Files:**
- Modify: `~/Development/LVGL/port/lvgl_gt911_indev.h`
- Modify: `~/Development/LVGL/port/lvgl_gt911_indev.cpp`

- [ ] **Step 1: Header** — add `#include "lvgl_panel_rotation.h"` beside the two existing includes at the top of `lvgl_gt911_indev.h`, and after the `lvgl_gt911_indev_create` declaration add:

```cpp
/* The same binding over a display created by lvgl_mipi_panel_create_rotated():
 * raw GT911 -> the PHYSICAL panel (scaled by the part's reported resolution
 * against the panel, which is the display turned on its side) -> the inverse
 * of the present mapping -> logical.  lv_display_get_rotation() is still
 * LV_DISPLAY_ROTATION_0 and is still asserted: LVGL's own rotation is not in
 * play, the port's present is. */
lv_indev_t *lvgl_gt911_indev_create_rotated(lv_display_t *disp, GT911 &touch, lvgl_panel_present_t present);
```

- [ ] **Step 2: Implementation.** Replace `static int32_t  s_hor = 0, s_ver = 0;` with:

```cpp
/* The PHYSICAL panel in pixels -- what the GT911's reported resolution is
 * scaled against.  Unrotated: the display's own size.  CW90: the display
 * turned on its side. */
static int32_t  s_phys_w = 0, s_phys_h = 0;
static lvgl_panel_present_t s_present = (lvgl_panel_present_t)0;   /* 0 = unrotated */
static lvgl_panel_rot_cfg_t s_rot;
```
Add, above `gt911_read_cb`:

```cpp
/* Scale by what the part REPORTED against the physical panel -- identity on
 * this panel, but never assumed (v2 spec 5.4 discipline) -- then, if the
 * display is presented rotated, invert the present mapping.  The unrotated
 * arithmetic is the expression the binding has always used, unchanged. */
static void map_contact(const TouchPoint &p, lv_point_t *out)
{
    const int32_t px = (int32_t)((uint32_t)p.x * (uint32_t)s_phys_w / s_touch->resolutionX());
    const int32_t py = (int32_t)((uint32_t)p.y * (uint32_t)s_phys_h / s_touch->resolutionY());
    if (s_present == LVGL_PANEL_PRESENT_CW90) {
        lvgl_panel_rot_map_point_inv(&s_rot, px, py, &out->x, &out->y);
    } else {
        out->x = px;
        out->y = py;
    }
}
```
In `gt911_read_cb`, replace the two scaling blocks:
```cpp
                    s_point.x = (int32_t)((uint32_t)pts[i].x * (uint32_t)s_hor
                                          / s_touch->resolutionX());
                    s_point.y = (int32_t)((uint32_t)pts[i].y * (uint32_t)s_ver
                                          / s_touch->resolutionY());
```
with `map_contact(pts[i], &s_point);` and
```cpp
            s_point.x = (int32_t)((uint32_t)pts[0].x * (uint32_t)s_hor
                                  / s_touch->resolutionX());
            s_point.y = (int32_t)((uint32_t)pts[0].y * (uint32_t)s_ver
                                  / s_touch->resolutionY());
```
with `map_contact(pts[0], &s_point);`.

Replace the whole `lvgl_gt911_indev_create` definition with:

```cpp
static lv_indev_t *create_common(lv_display_t *disp, GT911 &touch)
{
    /* Both preconditions from the header, enforced loudly.  LV_ASSERT_HANDLER
     * prints LVGL_ASSERT! on Serial1 first (lvgl_rt1176_assert.h). */
    LV_ASSERT(touch.resolutionX() != 0 && touch.resolutionY() != 0);
    LV_ASSERT(lv_display_get_rotation(disp) == LV_DISPLAY_ROTATION_0);

    s_touch = &touch;
    s_pressed = false;
    s_point.x = 0; s_point.y = 0;
    s_have_primary = false;
    s_primary_id   = 0;
    s_wait_clear   = false;
    s_idle_polls = s_poll_fails = s_buffers = 0;

    lv_indev_t *indev = lv_indev_create();
    lv_indev_set_type(indev, LV_INDEV_TYPE_POINTER);
    lv_indev_set_read_cb(indev, gt911_read_cb);
    lv_indev_set_display(indev, disp);
    /* 10 ms, per-indev -- see the header's READ TIMING note. */
    lv_timer_set_period(lv_indev_get_read_timer(indev), 10);
    return indev;
}

lv_indev_t *lvgl_gt911_indev_create(lv_display_t *disp, GT911 &touch)
{
    s_present = (lvgl_panel_present_t)0;
    s_phys_w = lv_display_get_horizontal_resolution(disp);
    s_phys_h = lv_display_get_vertical_resolution(disp);
    return create_common(disp, touch);
}

lv_indev_t *lvgl_gt911_indev_create_rotated(lv_display_t *disp, GT911 &touch, lvgl_panel_present_t present)
{
    LV_ASSERT(present == LVGL_PANEL_PRESENT_CW90);
    s_present = present;
    /* CW90: the physical panel is the display turned on its side. */
    s_phys_w = lv_display_get_vertical_resolution(disp);
    s_phys_h = lv_display_get_horizontal_resolution(disp);
    s_rot.log_w  = lv_display_get_horizontal_resolution(disp);
    s_rot.log_h  = lv_display_get_vertical_resolution(disp);
    s_rot.phys_w = s_phys_w;
    s_rot.phys_h = s_phys_h;
    s_rot.full_threshold_px = 0;
    LV_ASSERT(lvgl_panel_rot_cfg_valid(&s_rot, present));
    return create_common(disp, touch);
}
```
(Keep the three `lvgl_gt911_*` counter accessors as they are.)

- [ ] **Step 3: The unrotated touch gate must still pass — it is the byte-level check on the refactor**

```bash
cd /Users/nicholasnewdigate/Development/rt1170/evkb/examples/display/lvgl_rk055_touch_test && cmake --build build && ./run_qemu.sh | tail -3
```
Expected: `PASS: …` (the touch gate's drag assertion and Idle-latch count are unchanged).

- [ ] **Step 4: Commit (LVGL repo)**

```bash
cd ~/Development/LVGL && git add port/lvgl_gt911_indev.h port/lvgl_gt911_indev.cpp
git commit -m "port: lvgl_gt911_indev_create_rotated -- scale against the physical panel, invert the CW90 present for the pointer; unrotated arithmetic unchanged

Co-Authored-By: Claude Fable 5.1 <noreply@anthropic.com>"
```

---

### Task 8: push LVGL and bump the pin

**Files:**
- Modify: `evkb.cmake:130`

- [ ] **Step 1: Push**

```bash
cd ~/Development/LVGL && git push origin master && git rev-parse HEAD
```
Expected: the push succeeds; note the 40-character SHA.

- [ ] **Step 2: Bump the pin** — in `evkb.cmake` line 130 replace `99519347838c464426c555967a9f9ded703dcef1` with the new SHA and prepend to its trailing comment: `Bumped 2026-09-XX (rotated present + rotated GT911 binding, acid_box landscape; earlier: 2026-08-30 …` (keep the existing history text after it).

- [ ] **Step 3: Prove the pin resolves from GitHub, not just locally**

```bash
cd /Users/nicholasnewdigate/Development/rt1170/evkb/examples/display/lvgl_rk055_touch_test && rm -rf build-fetch && \
cmake -B build-fetch -DEVKB_FORCE_FETCH=ON -DCMAKE_TOOLCHAIN_FILE=../../../toolchain/rt1170-evkb.toolchain.cmake 2>&1 | grep -iE "LVGL|clone|error" | head; cmake --build build-fetch 2>&1 | tail -2
```
Expected: the configure log names the new LVGL SHA in a clone/`Already at requested ref` line, the build completes. (The full fresh-user proof — running a gate on a fetched acid_box ELF — is Task 13.)

- [ ] **Step 4: Commit**

```bash
cd /Users/nicholasnewdigate/Development/rt1170/evkb && git add evkb.cmake && git commit -m "build: LVGL pin -> <sha> (rotated present + rotated GT911 binding for the acid_box landscape)

Co-Authored-By: Claude Fable 5.1 <noreply@anthropic.com>"
```

---

### Task 9: acid_box — layout C, the rotated pipeline, the ROT lines and the equality guard

**Files:**
- Modify: `examples/display/acid_box/acid_box.cpp` (the UI section at `:874-1193`, `setup()` at `:1254-1341`, `audio_probe_poll()` at `:840-858`)

- [ ] **Step 1: Replace the UI section header comment and add the geometry block.** Replace the comment block starting `/* --- UI ---` (lines 874–887, ending `left behind. */`) with:

```cpp
/* --- UI ------------------------------------------------------------------- *
 *
 * Layout C (spec 2026-09-14-acid-box-landscape-design.md section 6) on a
 * LOGICAL 1280x720 frame: the transport bar, the 2x8 step lane with the
 * selected-step editor beside it, an EMPTY band reserved for later work, and
 * the eight sound knobs along the bottom edge -- nearest the player once the
 * board lies flat, turned counter-clockwise.  The panel is still the RK055's
 * 720x1280 glass: the port presents this frame rotated 90 degrees clockwise
 * (lvgl_mipi_panel_create_rotated) and the touch binding inverts the same map,
 * so nothing here knows about rotation.
 *
 * Everything is placed from the constants below, and those constants are the
 * GATE's geometry as well as the picture's -- the touch script's raw GT911
 * percentages are derived from them through the CW90 map (run_qemu.sh's
 * GEOMETRY block), so moving a widget moves a tap point.  Regenerate the
 * script if any of these change.
 *
 * The screen still paints an OPAQUE ground, for the reason the stub did: it is
 * what makes every pixel of the frame defined, and therefore makes
 * ACIDBOX_UI_SUM a checksum of the scene rather than of whatever the allocator
 * left behind. */
static constexpr int UI_W = 1280, UI_H = 720;
/* top bar */
static constexpr int TITLE_X = 24,  TITLE_Y = 36;
static constexpr int BAR_Y = 20,    BAR_BTN_H = 48;
static constexpr int TEMPO_DN_X = 560, TEMPO_UP_X = 690, TEMPO_BTN_W = 50;
static constexpr int BPM_X = 624,   BPM_Y = 35;
static constexpr int PLAY_X = 1040, STOP_X = 1156, TRANSPORT_BTN_W = 100;   /* PLAY centre (1090,44) = the gate's tap */
/* pattern band */
static constexpr int LANE_X0 = 16,  LANE_Y0 = 96, LANE_CELL = 100, LANE_PITCH_X = 108, LANE_PITCH_Y = 112;
/* cell 2's centre is (282,146) -- the gate's edit target */
static constexpr int PITCH_X = 912, PITCH_Y = 96, PITCH_SIZE = 150;
static constexpr int NOTE_X = 1080, NOTE_Y = 110;
static constexpr int ACC_X = 1080,  SLD_X = 1176, TOG_Y = 148, TOG_W = 88, TOG_H = 56;
static constexpr int WAVE_X = 1080, WAVE_Y = 222, WAVE_W = 184, WAVE_H = 56;
/* y 308..520 is RESERVED: empty on purpose (spec section 6), not centred away */
/* sound knobs */
static constexpr int KNOB_X0 = 16,  KNOB_Y0 = 520, KNOB_SIZE = 150, KNOB_PITCH = 158;   /* CUTOFF: 16..166 x 520..670 */
static constexpr int KNOB_LABEL_DX = 50, KNOB_LABEL_DY = 152;
/* The present strategy, from display/pxp_rotate_probe's silicon run (spec
 * section 4.1): the damage-union area at or above which one full-frame PXP op
 * beats per-rect ops.  0 = every present full-frame. */
static constexpr uint32_t ACIDBOX_ROT_FULL_THRESHOLD_PX = 0u;   /* <- set from Task 3 */
```
Set `ACIDBOX_ROT_FULL_THRESHOLD_PX` to the value Task 3 wrote into spec §4.1.

- [ ] **Step 2: `mkknob` takes an index, not a (col,row).** Replace the `mkknob` definition with:

```cpp
static lv_obj_t *mkknob(lv_obj_t *scr, int i, const char *name,
                        float boot01, lv_event_cb_t cb)
{
    lv_obj_t *k = synthui_rotary_knob_create(scr);
    lv_obj_set_size(k, KNOB_SIZE, KNOB_SIZE);
    lv_obj_set_pos(k, KNOB_X0 + i * KNOB_PITCH, KNOB_Y0);
    synthui_rotary_knob_set_mode(k, SYNTHUI_ROTARY_MODE_BOUNDED);
    /* The DC default range is ±150; every angle<->param map in this file
     * hardcodes ±140, so the range is stated here instead of inherited. */
    synthui_rotary_knob_set_range(k, -140.0f, 140.0f);
    synthui_rotary_knob_set_angle(k, boot01 * 280.0f - 140.0f);
    lv_obj_add_event_cb(k, cb, LV_EVENT_VALUE_CHANGED, NULL);
    lv_obj_t *l = lv_label_create(scr);
    lv_label_set_text(l, name);
    lv_obj_set_style_text_color(l, lv_color_hex(0x9aa0b8), LV_PART_MAIN);
    lv_obj_set_pos(l, KNOB_X0 + i * KNOB_PITCH + KNOB_LABEL_DX, KNOB_Y0 + KNOB_LABEL_DY);
    return k;
}
```

- [ ] **Step 3: Rewrite the placement inside `build_ui`.** Keep the function's opening (screen create, ground colour, the SCROLL_CHAIN comment and `lv_obj_remove_flag`) and its closing (the `ui_poll(NULL)` priming comment, `lv_timer_create`, `return scr`). Replace everything between `lv_obj_remove_flag(scr, LV_OBJ_FLAG_SCROLLABLE);` and the `/* ★ RUN THE POLLER ONCE …` comment with:

```cpp
    /* transport bar */
    lv_obj_t *title = lv_label_create(scr);
    lv_label_set_text(title, "ACID BOX");
    lv_obj_set_style_text_color(title, lv_color_hex(0xFFFFFF), LV_PART_MAIN);
    lv_obj_set_pos(title, TITLE_X, TITLE_Y);
#if defined(ACIDBOX_LOOPSTAT)
    ls_attach_title(title);        /* tap = synthetic knob wiggle on/off (bench) */
#endif
    lv_obj_t *dn = mkbtn(scr, "-", cbTempoDn, NULL);
    lv_obj_set_pos(dn, TEMPO_DN_X, BAR_Y);
    lv_obj_set_size(dn, TEMPO_BTN_W, BAR_BTN_H);
    bpmLabel = lv_label_create(scr);
    lv_obj_set_style_text_color(bpmLabel, lv_color_hex(0xFFFFFF), LV_PART_MAIN);
    lv_obj_set_pos(bpmLabel, BPM_X, BPM_Y);
    lv_obj_t *up = mkbtn(scr, "+", cbTempoUp, NULL);
    lv_obj_set_pos(up, TEMPO_UP_X, BAR_Y);
    lv_obj_set_size(up, TEMPO_BTN_W, BAR_BTN_H);
    lv_obj_t *play = mkbtn(scr, LV_SYMBOL_PLAY, cbPlay, &playBtnLabel);
    lv_obj_set_pos(play, PLAY_X, BAR_Y);
    lv_obj_set_size(play, TRANSPORT_BTN_W, BAR_BTN_H);
    lv_obj_t *stop = mkbtn(scr, LV_SYMBOL_STOP, cbStop, NULL);
    lv_obj_set_pos(stop, STOP_X, BAR_Y);
    lv_obj_set_size(stop, TRANSPORT_BTN_W, BAR_BTN_H);

    /* step lane: 2x8 of LANE_CELL px cells at LANE_PITCH_X/Y */
    for (int i = 0; i < 16; i++) {
        lv_obj_t *c = synthui_step_create(scr);
        lv_obj_set_size(c, LANE_CELL, LANE_CELL);
        lv_obj_set_pos(c, LANE_X0 + (i % 8) * LANE_PITCH_X, LANE_Y0 + (i / 8) * LANE_PITCH_Y);
        synthui_step_set(c, kPreset[i].gate, kPreset[i].accent, kPreset[i].slide);
        lv_obj_add_event_cb(c, cbStepTap, LV_EVENT_CLICKED, (void *)(intptr_t)i);
        stepCell[i] = c;
    }

    /* editor: pitch detent knob + note name + ACC/SLD toggles + SAW/SQR */
    pitchKnob = synthui_rotary_knob_create(scr);
    lv_obj_set_size(pitchKnob, PITCH_SIZE, PITCH_SIZE);
    lv_obj_set_pos(pitchKnob, PITCH_X, PITCH_Y);
    /* detents are input behavior on the rotary widget (no visual mode):
     * bounded well + 24 semitone stops on the ±140 lattice the pitch maps
     * above assume. */
    synthui_rotary_knob_set_mode(pitchKnob, SYNTHUI_ROTARY_MODE_BOUNDED);
    synthui_rotary_knob_set_range(pitchKnob, -140.0f, 140.0f);
    synthui_rotary_knob_set_detent_step(pitchKnob, 280.0f / 24.0f);
    lv_obj_add_event_cb(pitchKnob, cbPitch, LV_EVENT_VALUE_CHANGED, NULL);
    noteLabel = lv_label_create(scr);
    lv_obj_set_style_text_color(noteLabel, lv_color_hex(0xFFFFFF), LV_PART_MAIN);
    lv_obj_set_pos(noteLabel, NOTE_X, NOTE_Y);
    accBtn = mkbtn(scr, "ACC", cbAccBtn, NULL);
    lv_obj_set_pos(accBtn, ACC_X, TOG_Y);
    lv_obj_set_size(accBtn, TOG_W, TOG_H);
    sldBtn = mkbtn(scr, "SLD", cbSldBtn, NULL);
    lv_obj_set_pos(sldBtn, SLD_X, TOG_Y);
    lv_obj_set_size(sldBtn, TOG_W, TOG_H);
    lv_obj_t *wave = mkbtn(scr, "SAW", cbWave, &waveBtnLabel);
    lv_obj_set_pos(wave, WAVE_X, WAVE_Y);
    lv_obj_set_size(wave, WAVE_W, WAVE_H);

    /* Sound knobs, one row along the bottom edge.  Boot angles are the INVERSE
     * of each map applied to default_patch()'s values, so the first frame
     * shows the patch the engine is actually holding -- a knob drawn at a
     * position its parameter is not at would make the very first drag jump. */
    LS_KNOB(mkknob(scr, 0, "CUTOFF",  logf(800.0f / 20.0f) / logf(12000.0f / 20.0f), cbCut));
    LS_KNOB(mkknob(scr, 1, "RESO",    0.55f, cbRes));
    LS_KNOB(mkknob(scr, 2, "ENV MOD", 0.60f, cbEnv));
    LS_KNOB(mkknob(scr, 3, "DECAY",   logf(0.28f / 0.03f) / logf(2.0f / 0.03f), cbDec));
    LS_KNOB(mkknob(scr, 4, "ACCENT",  0.70f, cbAcc));
    LS_KNOB(mkknob(scr, 5, "DIST",    0.15f, cbDst));
    LS_KNOB(mkknob(scr, 6, "SUB",     0.20f, cbSub));
    LS_KNOB(mkknob(scr, 7, "SLIDE T", logf(0.06f / 0.01f) / logf(0.3f / 0.01f), cbSld));
    select_step(0);

```
(`select_step(0)` must come AFTER the pitch knob and the cells exist, as before.)

- [ ] **Step 4: The rotation equality guard and the ROT lines.** Add immediately BEFORE `static void audio_probe_poll(void)` (i.e. above the `stepPeakRms` declarations is fine too):

```cpp
/* --- rotation witnesses (spec section 7) ------------------------------------
 * ACIDBOX_ROT: the port's PXP present counters.  An image that fell back to
 * the portrait create_db path never prints this line, so the gate fails it BY
 * NAME rather than passing on everything else.
 *
 * ACIDBOX_ROT_EQ: the EQUALITY GUARD -- gate-compared, never re-goldened, in
 * the spirit of the knob's delta guard.  After flip_sync() the presented
 * buffer and the canvas hold the same frame (single thread: no render can
 * intervene between the present and this check), so every 16th PHYSICAL row
 * -- which is every 16th LOGICAL column -- is compared against a CPU rotation
 * of the canvas.  Sampled, not exhaustive: it catches any stale region >= 16
 * logical columns wide at ~230 KB of reads; the port's host property test is
 * the exhaustive one.  The formula here is written out on purpose rather than
 * calling the port's helper -- a guard that shares the code it checks is not
 * a check. */
static uint32_t s_rotEqPass = 0, s_rotEqFail = 0;
static void rot_equality_check(void)
{
    lvgl_mipi_panel_flip_sync();
    const uint32_t *fb = (const uint32_t *)lvgl_mipi_panel_scanned_fb();
    const uint32_t *cv = (const uint32_t *)lvgl_mipi_panel_canvas();
    if (!fb || !cv) { s_rotEqFail++; return; }
    bool ok = true;
    for (uint32_t py = 0; py < PANEL_HEIGHT && ok; py += 16) {
        const uint32_t *row = fb + (size_t)py * PANEL_WIDTH;
        for (uint32_t px = 0; px < PANEL_WIDTH; px++) {
            /* physical (px,py) reads logical (py, PANEL_WIDTH-1-px) */
            const uint32_t want = cv[(size_t)(PANEL_WIDTH - 1 - px) * (size_t)UI_W + py];
            if (row[px] != want) { ok = false; break; }
        }
    }
    if (ok) s_rotEqPass++; else s_rotEqFail++;
}
static void print_rot_lines(void)
{
    CONSOLE.printf("ACIDBOX_ROT ops=%lu full=%lu px=%lu us=%lu errors=%lu\n",
                   (unsigned long)lvgl_mipi_panel_rot_ops(),
                   (unsigned long)lvgl_mipi_panel_rot_full(),
                   (unsigned long)lvgl_mipi_panel_rot_px(),
                   (unsigned long)lvgl_mipi_panel_rot_us(),
                   (unsigned long)lvgl_mipi_panel_rot_errors());
    CONSOLE.printf("ACIDBOX_ROT_EQ pass=%lu fail=%lu\n",
                   (unsigned long)s_rotEqPass, (unsigned long)s_rotEqFail);
}
```
In `audio_probe_poll`, after the per-bar `ACIDBOX_VSYNC` printf (inside the `if (s == 0 && lastSeenStep == 15)` block), add:
```cpp
            rot_equality_check();
            print_rot_lines();
```

- [ ] **Step 5: `setup()` wiring.** Replace
```cpp
    lv_display_t *disp = lvgl_mipi_panel_create_db(Display);
    diag_mark();               /* after lvgl_mipi_panel_create_db() */
```
with
```cpp
    /* Landscape: LVGL renders a 1280x720 canvas and the port presents it
     * rotated 90 degrees clockwise through the PXP into the off-screen scanout
     * buffer, then flips -- the same fenced, tear-free pipeline as create_db,
     * with the rotation confined to the present (spec section 5.B). */
    lvgl_mipi_panel_set_rot_threshold(ACIDBOX_ROT_FULL_THRESHOLD_PX);
    lv_display_t *disp = lvgl_mipi_panel_create_rotated(Display, LVGL_PANEL_PRESENT_CW90);
    diag_mark();               /* after lvgl_mipi_panel_create_rotated() */
```
Replace
```cpp
    if (vg_up && synthui_rotary_gpu_begin_deferred(
                     Display.width(), Display.height(),
                     Display.width() * PANEL_BYTES_PER_PIXEL)) {
```
with
```cpp
    /* The compositor draws into the CANVAS (the pre-flip hook hands it the
     * canvas), so it is told the canvas's geometry, not the panel's. */
    if (vg_up && synthui_rotary_gpu_begin_deferred(
                     UI_W, UI_H, UI_W * PANEL_BYTES_PER_PIXEL)) {
```
After the two lines
```cpp
    CONSOLE.printf("ACIDBOX_UI_SUM=0x%08lX\n", (unsigned long)lvgl_sum_value());
    CONSOLE.printf("PLAYING=%d\n", transport.playing() ? 1 : 0);
```
add (before the following `diag_mark()`):
```cpp
    rot_equality_check();      /* the boot present: the forced full-frame path */
    print_rot_lines();
```
Replace
```cpp
        lv_indev_t *indev = lvgl_gt911_indev_create(disp, touch);
```
with
```cpp
        lv_indev_t *indev = lvgl_gt911_indev_create_rotated(disp, touch, LVGL_PANEL_PRESENT_CW90);
```
Update the comment above `ACIDBOX_UI_SUM`'s feed: the checksum still reads `lvgl_mipi_panel_scanned_fb()` over `PANEL_FB_BYTES` — it now covers the PRESENTED, rotated portrait frame; add that sentence to the existing "db mode: checksum the PRESENTED buffer" comment.

- [ ] **Step 6: Regenerate the touch script for the new geometry** (the old script's taps land on the wrong widgets now, which would leave the gate's poll loop waiting out its 120 s ceiling). From the example directory:

```sh
{
  echo "# acid_box gate script -- GENERATED, never hand-counted.  Regenerate with"
  echo "# the block quoted in run_qemu.sh's header (and in transcript_qemu.txt)."
  echo "#"
  echo "# One instant per line, IMXRT_GT911_STEP_MS = 20 ms apart.  Percentages are"
  echo "# of the PHYSICAL 720x1280 panel; each is the CW90 map of a logical target"
  echo "# in acid_box.cpp's geometry block: physical (px,py) = (719-ly, lx), then"
  echo "# px*100/720 and py*100/1280, rounded.  The model computes"
  echo "# res*pct/100 in integers, so P 94 85 -> (676,1088) -> logical (1088,43)."
  echo "#"
  echo "# pad 1: 25 instants (~0.5 s); pads 2/3: 300 instants (~2.6 bars at 128 BPM"
  echo "# under this tree's ~0.81x QEMU audio clock)."
  for i in $(seq 25);  do echo R; done     # pad 1
  echo "P 94 85"; echo R                    # tap PLAY        (logical 1090,44)
  for i in $(seq 300); do echo R; done     # pad 2
  echo "P 80 22"; echo R                    # tap STEP CELL 2 (logical 282,146)
  for i in $(seq 300); do echo R; done     # pad 3
  for x in 19 18 17 16 15 14 13 12 11 10; do echo "P $x 7"; done   # drag CUTOFF: logical (91, 583->647), downward
  echo R
} > touch_script.txt
grep -cE '^(P|R)' touch_script.txt
```
Expected count: `640`.

- [ ] **Step 7: Build and run the gate**

```bash
cmake --build build 2>&1 | tail -3 && ./run_qemu.sh; echo "exit=$?"
```
Expected: the build succeeds; the gate FAILS at `FAIL: UI golden` (the frame changed) — every check BEFORE it (CODEC/PANEL/I2C/ACIDBOX_DONE, engine, vsync fence) passes, and the poll loop returned early because ≥ 3 `CUTOFF=` lines arrived. Confirm in the printed capture: `ACIDBOX_ROT ops=… full=2 … errors=0` at boot and `ACIDBOX_ROT_EQ pass=1 fail=0`, `PLAYING=1`, `STEP[2]=note33 gate1`. If `fail=1` at boot, the present and the guard disagree — check the physical/logical index arithmetic in `rot_equality_check` against `pxp_rotate_probe.cpp`'s `ref_px` first (same formula), then the port. If the capture shows `LVGL_ASSERT!` right after `PANEL_OK`, the canvas allocation failed (`extmem_malloc` returned NULL) — SDRAM is short by 3.6 MB and the VGLite pool or the alt buffer must give way; measure with `nm --size-sort build/acid_box.elf | grep -i extmem` before moving anything. If the CUTOFF count is under 3, the 1 % raw steps did not move the knob enough: change the drag line to 2 % steps (`for x in 19 17 15 13 11`) and note it in both headers in Task 10.

- [ ] **Step 8: Commit (golden still wrong on purpose — Task 10 pins it)**

```bash
git add acid_box.cpp touch_script.txt && git commit -m "acid_box: layout C on a 1280x720 canvas, presented CW90 through the PXP; rotated GT911 binding; ACIDBOX_ROT + equality guard witnesses; touch script regenerated (gate re-pinned in the next commit)

Co-Authored-By: Claude Fable 5.1 <noreply@anthropic.com>"
```

---

### Task 10: acid_box gate — touch script, GEOMETRY header, ROT assertions, re-recorded golden, fixture, vacuity

**Files:**
- Modify: `examples/display/acid_box/touch_script.txt` (regenerated)
- Modify: `examples/display/acid_box/run_qemu.sh`
- Modify: `examples/display/acid_box/transcript_qemu.txt`
- Modify: `tools/gate-vacuity.test.sh` (append section 15)

- [ ] **Step 1: The touch script generator** — already applied in Task 9 Step 6; re-run it only if the drag step size changed there. This is the block that replaces the one quoted in `transcript_qemu.txt` (Step 7):

```sh
{
  echo "# acid_box gate script -- GENERATED, never hand-counted.  Regenerate with"
  echo "# the block quoted in run_qemu.sh's header (and in transcript_qemu.txt)."
  echo "#"
  echo "# One instant per line, IMXRT_GT911_STEP_MS = 20 ms apart.  Percentages are"
  echo "# of the PHYSICAL 720x1280 panel; each is the CW90 map of a logical target"
  echo "# in acid_box.cpp's geometry block: physical (px,py) = (719-ly, lx), then"
  echo "# px*100/720 and py*100/1280, rounded.  The model computes"
  echo "# res*pct/100 in integers, so P 94 85 -> (676,1088) -> logical (1088,43)."
  echo "#"
  echo "# pad 1: 25 instants (~0.5 s); pads 2/3: 300 instants (~2.6 bars at 128 BPM"
  echo "# under this tree's ~0.81x QEMU audio clock)."
  for i in $(seq 25);  do echo R; done     # pad 1
  echo "P 94 85"; echo R                    # tap PLAY        (logical 1090,44)
  for i in $(seq 300); do echo R; done     # pad 2
  echo "P 80 22"; echo R                    # tap STEP CELL 2 (logical 282,146)
  for i in $(seq 300); do echo R; done     # pad 3
  for x in 19 18 17 16 15 14 13 12 11 10; do echo "P $x 7"; done   # drag CUTOFF: logical (91, 583->647), downward
  echo R
} > touch_script.txt
grep -cE '^(P|R)' touch_script.txt
```
Expected count: `640`.

- [ ] **Step 2: Rewrite the GEOMETRY block in `run_qemu.sh`'s header** (replace from `# GEOMETRY.` through the `PRESS_LOST` sentence):

```sh
# GEOMETRY.  The UI is a LOGICAL 1280x720 frame presented rotated 90 degrees
# CLOCKWISE on the 720x1280 panel (lvgl_mipi_panel_create_rotated): logical
# (lx,ly) sits at physical (719-ly, lx).  The GT911 model takes percentages of
# the PHYSICAL panel, so each tap below is a logical target from acid_box.cpp's
# geometry block, mapped, then rounded -- move a widget and the tap moves:
#   ▶            PLAY_X=1040,BAR_Y=20, 100x48 -> logical 1040..1140 x 20..68
#                centre (1090,44) -> physical (675,1090) -> P 94 85
#                -> model (676,1088) -> logical (1088,43)             ✔ inside
#   step cell 2  LANE_X0+2*108=232, LANE_Y0=96, 100x100 -> 232..332 x 96..196
#                centre (282,146) -> physical (573,282) -> P 80 22
#                -> model (576,281) -> logical (281,143)              ✔ inside
#   CUTOFF knob  KNOB_X0=16, KNOB_Y0=520, 150x150 -> 16..166 x 520..670
#                logical (91,583)..(91,647) -> physical (136..72, 91)
#                -> P 19 7 ... P 10 7, ten samples at 1 % (7.2 px) steps  ✔ inside
# The drag stays a DOWNWARD logical drag (cutoff strictly decreasing, the same
# assertion as before); rotated, it is a leftward raw drag.  It stops at logical
# y 647, 23 px inside the knob's bottom edge, on purpose: the widget does not set
# LV_OBJ_FLAG_PRESS_LOCK, so a sample past its edge would hand LVGL a different
# object and end the drag as PRESS_LOST with no further CUTOFF lines.
```
Also update the header sentence `# Room for boot + ~8 bars … + the 636-instant script` to `640-instant`.

- [ ] **Step 3: Add the rotation assertions** — insert after the vsync-fence block (after the line ending `"FAIL: vsync fence timed out during the run"; exit 1; }`):

```sh
# --- the rotated present -----------------------------------------------------
# ACIDBOX_ROT is printed only by the rotated pipeline: an image that fell back
# to the portrait create_db path has NO such line and must fail BY NAME.
# errors must be 0 on EVERY witness (boot + per bar); the LAST line must show
# ops>0 and full>=2 (the two forced start-up presents) and ops>=full.
grep -qE "^ACIDBOX_ROT ops=[0-9]+ full=[0-9]+ px=[0-9]+ us=[0-9]+ errors=0\r?$" "$OUT" \
    || { echo "FAIL: rotation present line missing or errors!=0"; exit 1; }
grep -E "^ACIDBOX_ROT " "$OUT" | grep -vqE "errors=0\r?$" \
    && { echo "FAIL: rotation errors during the run"; exit 1; }
ROT_LAST=$(grep -E "^ACIDBOX_ROT " "$OUT" | tail -1 | tr -d '\r')
ROT_OPS=$( printf '%s\n' "$ROT_LAST" | sed 's/.* ops=\([0-9]*\).*/\1/')
ROT_FULL=$(printf '%s\n' "$ROT_LAST" | sed 's/.* full=\([0-9]*\).*/\1/')
[ "$ROT_OPS" -gt 0 ] || { echo "FAIL: no PXP present ops counted"; exit 1; }
[ "$ROT_FULL" -ge 2 ] || { echo "FAIL: fewer than 2 full-frame presents (start-up forces two)"; exit 1; }
[ "$ROT_OPS" -ge "$ROT_FULL" ] || { echo "FAIL: ops < full is impossible"; exit 1; }
# The EQUALITY GUARD: the presented buffer must equal the rotated canvas on
# every sample -- at boot (full-frame path) and after every bar (damage path).
# fail=0 on every line, and the last pass count must cover boot + every bar.
grep -qE "^ACIDBOX_ROT_EQ pass=[1-9][0-9]* fail=0\r?$" "$OUT" \
    || { echo "FAIL: rotation equality guard line missing"; exit 1; }
grep -E "^ACIDBOX_ROT_EQ " "$OUT" | grep -vqE "fail=0\r?$" \
    && { echo "FAIL: rotation equality guard failed during the run"; exit 1; }
EQ_PASS=$(grep -E "^ACIDBOX_ROT_EQ " "$OUT" | tail -1 | tr -d '\r' | sed 's/.*pass=\([0-9]*\).*/\1/')
# Bars counted only up to the LAST equality line: the reap can land between a
# bar line and the guard line that follows it, and that bar is not evidence
# either way.
NBARS=$(awk '/^ACIDBOX_BAR=/ { b++ } /^ACIDBOX_ROT_EQ / { n = b } END { print n + 0 }' "$OUT")
[ "$EQ_PASS" -ge $((NBARS + 1)) ] \
    || { echo "FAIL: equality guard ran $EQ_PASS times for $NBARS bars + boot"; exit 1; }
```
If Task 3 chose threshold 0, append: `[ "$ROT_OPS" -eq "$ROT_FULL" ] || { echo "FAIL: threshold 0 means every present is full-frame"; exit 1; }`. If it chose a non-zero threshold, append instead: `[ "$ROT_OPS" -gt "$ROT_FULL" ] || { echo "FAIL: the damage path never ran (ops == full)"; exit 1; }`.

- [ ] **Step 4: Run the gate — it must fail ONLY on the golden now**

```bash
./run_qemu.sh; echo "exit=$?"
```
Expected: `FAIL: UI golden`, exit 1, with `PLAYING=1`, `STEP[2]=note33 gate1`, ≥ 3 `CUTOFF=` lines and the new ROT lines all present in the capture above it. The rotation assertions from Step 3 run BEFORE the golden check (they follow the vsync block), so reaching `UI golden` proves they all passed. If `STEP[2]` is missing or names another cell, re-check the percentages against the GEOMETRY block before touching anything else.

- [ ] **Step 5: LOOK at the frame before pinning the golden.** Boot the ELF under `tools/rt1170-qemu.sh` with a monitor socket (the recipe in `transcript_qemu.txt`, "pmemsave"), dump the PRESENTED buffer (`xp/1wx 0x4080820c` gives the LCDIFv2 layer address; `pmemsave <addr> 0x384000 /tmp/fb.raw`), and view it:

```bash
convert -size 720x1280 -depth 8 bgra:/tmp/fb.raw -rotate -90 /tmp/fb.png && open /tmp/fb.png
```
(`-rotate -90` undoes the CW90 present, so the PNG should be the upright landscape UI. If ImageMagick is absent, `python3 -c` with PIL: `Image.frombytes('RGBA',(720,1280),data,'raw','BGRA').rotate(90,expand=True)`.) Check, and write into `transcript_qemu.txt`'s header what was checked: the transport bar along the top with ▶/■ at the right; the 2×8 lane at the top-left with the preset cell-for-cell (cells 0,7,12 accent-coloured; 3,10,15 with a slide mark; 2,5,9,14 dark rests); the editor beside it; the empty reserved band; the eight knobs along the bottom with their boot angles (CUTOFF at +21.5°, as before); NOTHING mirrored (the title reads "ACID BOX" left-to-right).

- [ ] **Step 6: Pin the golden.** Replace `0x25B30A96` in `grep -qE "ACIDBOX_UI_SUM=0x25B30A96\r?$"` with the captured value, and add a `# Re-goldened 2026-09-XX (landscape, spec 2026-09-14): …` line to the comment above it stating the dump was viewed and what was checked. Run `./run_qemu.sh` TWICE; both must PASS with the same `ACIDBOX_UI_SUM`. Also confirm the anti-golden line is unchanged (0x9BC99DC5 is still the FNV of 3,686,400 zero bytes — the buffer size did not change).

- [ ] **Step 7: Re-capture the fixture**

```bash
cp build/acid_box.uart /tmp/acid_box_capture.txt
```
In `transcript_qemu.txt`: add a `RE-RECORDED 2026-09-XX (LANDSCAPE)` paragraph to the header describing the change (layout C, CW90 present, new tokens ACIDBOX_ROT / ACIDBOX_ROT_EQ, the golden move `0x25B30A96 → 0x<new>`), replace the block after `==== captured UART ====` up to the next `====` rule with the new capture, replace the quoted generator block with Step 1's, and update the `grep -cE` count to 640 and the pmemsave paragraph with the `-rotate -90` viewing step.

- [ ] **Step 8: Vacuity section 15** — append to `tools/gate-vacuity.test.sh` after section 14:

```sh
# --- 15. acid_box (landscape present + rotation guards) ----------------------
# The rotation witnesses are individually meaningless unless each can be shown
# to FAIL: an image on the portrait create_db path prints no ACIDBOX_ROT line,
# and a stale region shows up only as a non-zero equality-guard fail count.
ACB="examples/display/acid_box"
if [ -d "$EVKB/$ACB" ] && [ -f "$EVKB/$ACB/transcript_qemu.txt" ]; then
    run_gate "$ACB" "run_qemu.sh" "$EVKB/$ACB/transcript_qemu.txt"; rc=$?
    [ "$rc" -eq 0 ] && result=0 || result=1
    report "green_still_passes_acid_box" $result

    grep -v "^ACIDBOX_ROT " "$EVKB/$ACB/transcript_qemu.txt" > "$WORK/acb_norot.txt"
    run_gate "$ACB" "run_qemu.sh" "$WORK/acb_norot.txt"; rc=$?
    result=0
    [ "$rc" -ne 0 ] || result=1
    echo "$OUT_TEXT" | grep -q "rotation present line missing" || result=1
    report "acb_missing_rot_line_fails_by_name" $result

    # ONE failed equality sample anywhere in the run.  awk, not `sed 0,/re/`
    # (GNU-only): flip the first ACIDBOX_ROT_EQ line's fail=0 to fail=1.
    awk '/^ACIDBOX_ROT_EQ / && !done { sub(/fail=0/, "fail=1"); done=1 } { print }' \
        "$EVKB/$ACB/transcript_qemu.txt" > "$WORK/acb_eqfail.txt"
    run_gate "$ACB" "run_qemu.sh" "$WORK/acb_eqfail.txt"; rc=$?
    result=0
    [ "$rc" -ne 0 ] || result=1
    echo "$OUT_TEXT" | grep -q "rotation equality guard" || result=1
    report "acb_equality_guard_fires" $result

    sed 's|^ACIDBOX_UI_SUM=0x........|ACIDBOX_UI_SUM=0xBADBADBA|' \
        "$EVKB/$ACB/transcript_qemu.txt" > "$WORK/acb_badsum.txt"
    run_gate "$ACB" "run_qemu.sh" "$WORK/acb_badsum.txt"; rc=$?
    result=0
    [ "$rc" -ne 0 ] || result=1
    echo "$OUT_TEXT" | grep -q "UI golden" || result=1
    report "acb_bad_golden_fails_by_name" $result
else
    echo "SKIP: acid_box vacuity (example or fixture missing)"
fi
```

- [ ] **Step 9: Run sections 14–15 and the acid_box gate once more**

```bash
cd /Users/nicholasnewdigate/Development/rt1170/evkb && sh tools/gate-vacuity.test.sh 2>&1 | grep -E "acid_box|acb_|prp_|pxp_rotate"
```
Expected: seven `PASS:` lines (three `prp_`/probe, four `acb_`/acid_box), no `FAIL:`. Note the first ACIDBOX_ROT_EQ line in the fixture is the BOOT line; if the flipped line were instead a per-bar one the message is the same — either is a valid negative.

- [ ] **Step 10: Commit**

```bash
git add examples/display/acid_box/touch_script.txt examples/display/acid_box/run_qemu.sh examples/display/acid_box/transcript_qemu.txt tools/gate-vacuity.test.sh
git commit -m "acid_box gate: landscape geometry, regenerated touch script (640 instants), ACIDBOX_ROT + equality-guard assertions, golden re-recorded after viewing the rotated frame; four vacuity negatives

Co-Authored-By: Claude Fable 5.1 <noreply@anthropic.com>"
```

---

### Task 11: the M2_BT_OUT bench builds still link (ITCM) and every declared configuration builds

**Files:** none modified unless the ITCM floor trips.

- [ ] **Step 1: bench_check**

```bash
cd /Users/nicholasnewdigate/Development/rt1170/evkb && tools/build-bench-configs.sh -n acid_box 2>&1 | tail -15
```
Expected: both `build-benchcheck-bt` and `build-benchcheck-bench` link; the `--print-memory-usage` line shows ITCM used with headroom comfortably above the 2048 B floor (the previous headroom was 13,968 B on `build-bt`; the new port code is a few hundred bytes of libLVGL); the `-n` nm-diff prints OK or a named DIFFERS that is explained by the new `rot_*` symbols only. If the link fails with `NEW-45: ITCM headroom below 2048`, route `lvgl_mipi_panel.cpp`'s new functions to flash by adding `*lvgl_mipi_panel.cpp.obj(.text.rot_* .text._Z*rot* )` beside the existing `*acid_box.cpp.obj(...)` line in the derived linker script — and measure `rot_us` on silicon in Task 12 before accepting that.

- [ ] **Step 2: Record the headroom number** in `examples/display/acid_box/CMakeLists.txt`'s NEW-45 comment (the line that today says "measured headroom after the NEW-45 VGLite routing is ~9.8 KB"): append `; 2026-09-XX after the landscape present: <N> B on build-bt`.

- [ ] **Step 3: Commit (if the comment changed)**

```bash
git add examples/display/acid_box/CMakeLists.txt && git commit -m "acid_box: ITCM headroom after the landscape present recorded (<N> B on build-bt)

Co-Authored-By: Claude Fable 5.1 <noreply@anthropic.com>"
```

---

### Task 12: silicon acceptance (BENCH) — spec §8

**Files:**
- Modify: `examples/display/acid_box/transcript_hw_evkb.txt`
- Modify: `examples/display/acid_box/run_qemu.sh` (only if the gpu golden comment references the portrait value)

Use the `flashing-rt1170-evkb` skill. Flash the DEFAULT build first (`build/acid_box.elf`, or its `.hex` if LinkServer exits −11), verify, attach the console, SW4.

- [ ] **Step 1: Orientation and touch (§8.1).** With the board turned counter-clockwise, confirm by eye: the picture is upright, "ACID BOX" reads left-to-right at the top-left, the knobs run along the bottom. Tap, in this order, and read the console after each: cell 0, cell 7, cell 8, cell 15 (expect `STEP[0]`, `STEP[7]`, `STEP[8]`, `STEP[15]` with their gate flipped), ACC, SLD (the `STEP[15]=…acc1…` / `sld` fields flip), SAW (label toggles to SQR), ▶ (`PLAYING=1`, bars start), ■ (`PLAYING=0`). Record the console lines in the transcript. A tap on cell 0 that prints `STEP[8]` (or any other index) is the mirrored/transposed-axis failure §10 names — stop and fix the binding.

- [ ] **Step 2: gpu golden (§8.2).** Note `ACIDBOX_ENGINE=gpu` and `ACIDBOX_UI_SUM=0x…` on this boot; SW4 twice more; all three sums must be identical. Record it in `transcript_hw_evkb.txt` with the line `PORTRAIT gpu golden 0x1479CEE8 RETIRED 2026-09-XX (layout C landscape, spec 2026-09-14); landscape gpu golden 0x<new>, N boots bit-identical.` Do not delete the old value's history.

- [ ] **Step 3: Counters (§8.3).** Let it run ≥ 3 minutes with ▶ pressed. Every `ACIDBOX_VSYNC` line `timeouts=0`; every `ACIDBOX_ROT` line `errors=0`; every `ACIDBOX_ROT_EQ` line `fail=0`; `ACIDBOX_GPU_ERR=0`. Record the last of each.

- [ ] **Step 4: Performance (§8.4).** Build and flash the LOOPSTAT bench build:
```bash
cmake -B build-loopstat -DACIDBOX_LOOPSTAT=ON -DCMAKE_TOOLCHAIN_FILE=../../../toolchain/rt1170-evkb.toolchain.cmake && cmake --build build-loopstat
```
With the transport playing, drag CUTOFF back and forth for ~30 s and capture the `framestat`/`touchstat`/`loopstat` lines. Acceptance against the portrait baseline (30 fps, 32.4 ms median frame, touch p95 46 ms): fps ≥ 30 with the median frame interval within ±15 % (27.5–37.3 ms); touch p95 ≤ 53 ms. Compute per-present PXP time from two `ACIDBOX_ROT` lines: `Δus / Δflips` (flips from the matching `ACIDBOX_VSYNC` lines) and record it — this is the number that answers P1's prediction in the application rather than the probe. **A tiny sample makes a catastrophic-looking number** (CLAUDE.md): use a capture with n ≥ 100 touches.

- [ ] **Step 5: No scanout flash (§8.5).** Film the panel at 60 fps with a phone for ~10 s while dragging a knob; scrub the frames (or extract with `ffmpeg -i clip.mov -vf fps=60 f_%04d.png`) looking for any frame with a partial knob, a damage-box-sized square, or a half-old/half-new region. Record "none seen in N frames". If a flash IS seen, the present ran into the buffer on the glass: check the `flip_sync()` ordering in `rot_flush_cb` (step 2 must precede step 3) before anything else.

- [ ] **Step 6: M2_BT_OUT (§8.6).** Flash `build-bt` (M2_BT_OUT=ON, your headset's name in `M2_BT_TARGET_NAME` if needed), let it connect and stream, drag knobs for a minute: `pcmdrops=0` on the `bt_hb` lines, `ACIDBOX_VSYNC timeouts=0`, `ACIDBOX_ROT errors=0`. Record.

- [ ] **Step 7: Commit the evidence**

```bash
git add examples/display/acid_box/transcript_hw_evkb.txt
git commit -m "acid_box: landscape silicon acceptance -- orientation/touch at the four lane corners, gpu golden re-recorded (N boots), fences clean, LOOPSTAT vs the portrait baseline, no scanout flash on camera, M2_BT_OUT streams

Co-Authored-By: Claude Fable 5.1 <noreply@anthropic.com>"
```

---

### Task 13: close-out — sweep, audit, vacuity, fresh-user, CLAUDE.md

**Files:**
- Modify: `CLAUDE.md` (the gate-count lines)
- Modify: `docs/superpowers/specs/2026-09-14-acid-box-landscape-design.md` (append §11 "Close-out, measured")

- [ ] **Step 1: Rebuild every gate image that links the port** (an ELF from the old port boots fine, so an unrebuilt sweep would pass vacuously — the NEW-36 lesson):

```bash
cd /Users/nicholasnewdigate/Development/rt1170/evkb/examples && for d in $(grep -l "lvgl_mipi_panel.cpp\|lvgl_gt911_indev.cpp" */*/CMakeLists.txt | xargs -n1 dirname); do echo "== $d"; (cd "$d" && cmake --build build 2>&1 | tail -1); done
```
Expected: 16 directories, each ending in a successful link. A `COMPILERPATH is UNDEFINED` or "toolchain file not found" means a stale build dir: `rm -rf build` and configure fresh with the standard command.

- [ ] **Step 2: The full sweep, ONE instance, output captured, nothing else running** (no concurrent audit, no bench). Use a fresh short-path symlink — `/tmp/ev` points at a DIFFERENT checkout:

```bash
ln -sfn /Users/nicholasnewdigate/Development/rt1170/evkb /tmp/lsx && cd /tmp/lsx && tools/run-all-qemu-gates.sh 2>&1 | tee /tmp/sweep-landscape.log | tail -20
```
Expected: `gates: 140 passed` (exit 0), or `139 passed, 1 failed` where the ONE red is `cm4_audio_test` or another documented load-sensitivity member that passes alone immediately after (`tools/run-all-qemu-gates.sh <name>`). `0 SKIP` is the load-bearing number. Any other red is a regression from this work — fix, do not disposition.

- [ ] **Step 3: Audit and vacuity, AFTER the sweep**

```bash
cd /Users/nicholasnewdigate/Development/rt1170/evkb && LICENSE_AUDIT_EVKB=$(pwd) sh tools/license-audit.sh 2>&1 | tail -3 && sh tools/gate-vacuity.test.sh 2>&1 | grep -cE "^PASS:" && sh tools/gate-vacuity.test.sh 2>&1 | grep -E "^FAIL:" || true
```
Expected: `LICENSE-AUDIT: PASS`; the PASS count is the previous 47 + 7 = **54** (re-derive it from the run, never from this line); no `FAIL:`.

- [ ] **Step 4: Fresh-user verification — RUN the gate on a GitHub-fetched ELF**

```bash
cd examples/display/acid_box && rm -rf build-fetch && cmake -B build-fetch -DEVKB_FORCE_FETCH=ON -DCMAKE_TOOLCHAIN_FILE=../../../toolchain/rt1170-evkb.toolchain.cmake 2>&1 | grep -E "LVGL|clone|Already at" | head -5 && cmake --build build-fetch 2>&1 | tail -1
mv build build-local && ln -s build-fetch build && ./run_qemu.sh | tail -2; rm build && mv build-local build
```
Expected: the configure log shows LVGL cloned at the new pin; `PASS: acid box …` on the fetched ELF. (The `GATE_VACUITY`-style trap does not apply here: this gate has no rebuild step of its own.)

- [ ] **Step 5: CLAUDE.md.** In the "Test / verify" section: change `The sweep covers **139 gates** — NEW-41 added ONE …` to `The sweep covers **140 gates** — the acid_box LANDSCAPE work added ONE on 2026-09-XX (`display/pxp_rotate_probe`: the Phase 0 PXP CW90 rotation probe, eleven pointer-offset sub-rect sums pinned; 139 before it), and before that NEW-41 added ONE …`; change `The target is **139 passed, 0 failed, 0 SKIP**, or **138 passed, 1 failed, 0 SKIP**` to `**140 passed, 0 failed, 0 SKIP**, or **139 passed, 1 failed, 0 SKIP**`; and add a dated `✅ Measured` paragraph at the top of that list with the numbers from Steps 2–4 in the same shape as the existing entries (gates, audit, vacuity count, fresh-user, the acid_box golden move, the probe's P1–P5 outcome and the threshold chosen).

- [ ] **Step 6: Spec §11.** Append to the spec a short "Close-out, measured" section: the sweep line, the audit, the vacuity count, the fresh-user run, and the four silicon numbers from Task 12 (gpu golden, fps/frame/touch p95 vs baseline, per-present PXP µs, camera result).

- [ ] **Step 7: Commit and push**

```bash
git add CLAUDE.md docs/superpowers/specs/2026-09-14-acid-box-landscape-design.md
git commit -m "docs: acid_box landscape close-out -- sweep 140, audit PASS, vacuity 54, fresh-user run on the fetched ELF; CLAUDE.md gate count 139 -> 140

Co-Authored-By: Claude Fable 5.1 <noreply@anthropic.com>"
git push origin master
```
