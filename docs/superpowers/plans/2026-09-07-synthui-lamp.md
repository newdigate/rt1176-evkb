# SynthUI Lamp Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Implement the DC Lamp primitive as `synthui_lamp` in SynthUI (LVGL 9, sw delta rendering), verified by host unit tests and a new double-buffered QEMU-gated example `examples/display/synthui_lamp_test` in rt1176-evkb.

**Architecture:** Spec: `docs/superpowers/specs/2026-09-07-synthui-lamp-design.md`. Single widget TU in SynthUI with 5-layer draw pass (`DRAW_MAIN`), early-return delta setters on unchanged state, and scoped widget-level invalidation. The consumer example runs the hardware double-buffer pipeline (`lvgl_mipi_panel_create_db()`) and executes Phase A correctness (pinned golden CRC, 64-step LCG delta sequence, delta-equality guard, damage bounds) and Phase B (continuous animation benchmark).

**Tech Stack:** LVGL 9.4.0 (vendored in rt1176-evkb tree), SynthUI (`~/Development/SynthUI`, local-first), rt1176-evkb CMake + QEMU (`~/Development/qemu-rt1170/build/qemu-system-arm`).

**House rules that bind every task:**
- `./run_qemu.sh`, never `sh run_qemu.sh`.
- Use the QEMU binary at `/Users/moolet/Development/qemu-rt1170/build/qemu-system-arm`.
- Goldens are RECORDED from two bit-identical consecutive QEMU runs, never derived.
- A demonstrated-RED is required for gate assertions.

---

### Task 1: `synthui_lamp_math.h` + Host Unit Test (SynthUI repo)

**Files:**
- Create: `/Users/moolet/Development/SynthUI/src/synthui_lamp_math.h`
- Create: `/Users/moolet/Development/SynthUI/tests/lamp_test.c`
- Modify: `/Users/moolet/Development/SynthUI/tests/run.sh`

**Interfaces:**
- Produces: `synthui_lamp_geom_t`, `synthui_lamp_compute_geom(float w, float h, synthui_lamp_shape_t shape, synthui_lamp_geom_t *g)`

- [ ] **Step 1: Write the failing host test**

Create `/Users/moolet/Development/SynthUI/tests/lamp_test.c`:
```c
/* lamp_test.c - host unit test for pure lamp geometry & math.
 * Copyright (c) 2026 Nicholas Newdigate
 * SPDX-License-Identifier: MIT */
#undef NDEBUG
#include "../src/synthui_lamp_math.h"
#include <assert.h>
#include <math.h>
#include <stdio.h>

static int approx_eq(float a, float b) { return fabsf(a - b) < 0.05f; }

int main(void)
{
    synthui_lamp_geom_t g;

    /* 48x48 square round lamp (DC default) */
    assert(synthui_lamp_compute_geom(48.0f, 48.0f, SYNTHUI_LAMP_SHAPE_ROUND, &g));
    assert(approx_eq(g.vw, 100.0f));
    assert(approx_eq(g.vh, 100.0f));
    assert(approx_eq(g.u, 0.48f));
    assert(approx_eq(g.cx, 50.0f));
    assert(approx_eq(g.cy, 50.0f));
    assert(approx_eq(g.r, 38.0f));       /* 50 - 12 */
    assert(approx_eq(g.glow_w, 22.0f));  /* 100 * 0.22 */
    assert(approx_eq(g.glow_r, 49.0f));  /* 38 + 11 */

    /* 72x38 pill lamp */
    assert(synthui_lamp_compute_geom(72.0f, 38.0f, SYNTHUI_LAMP_SHAPE_PILL, &g));
    assert(approx_eq(g.vw, 100.0f));
    assert(approx_eq(g.vh, 53.0f));      /* round(100*38/72) = 53 */
    assert(approx_eq(g.u, 0.72f));
    assert(approx_eq(g.bx, 12.0f));
    assert(approx_eq(g.bw, 76.0f));      /* 100 - 24 */
    assert(approx_eq(g.br, 12.72f));     /* 53 * 0.24 */

    /* 52x20 bar lamp */
    assert(synthui_lamp_compute_geom(52.0f, 20.0f, SYNTHUI_LAMP_SHAPE_BAR, &g));
    assert(approx_eq(g.br, 2.0f));       /* bar corner radius is fixed at 2 */

    /* Degenerate dimensions */
    assert(!synthui_lamp_compute_geom(0.0f, 48.0f, SYNTHUI_LAMP_SHAPE_ROUND, &g));
    assert(!synthui_lamp_compute_geom(48.0f, 0.0f, SYNTHUI_LAMP_SHAPE_ROUND, &g));

    printf("lamp_test: all PASS\n");
    return 0;
}
```

Append to `/Users/moolet/Development/SynthUI/tests/run.sh`:
```sh
cc -Wall -Wextra -Werror -o "$out/lamp_test" tests/lamp_test.c
"$out/lamp_test"
```

- [ ] **Step 2: Run to verify it fails**

Run: `cd /Users/moolet/Development/SynthUI && ./tests/run.sh`
Expected: FAIL — `fatal error: '../src/synthui_lamp_math.h' file not found`.

- [ ] **Step 3: Implement `synthui_lamp_math.h`**

Create `/Users/moolet/Development/SynthUI/src/synthui_lamp_math.h`:
```c
/* synthui_lamp_math.h - pure geometry arithmetic for SynthUI Lamp.
 * Header-only and LVGL-free for direct host unit testing.
 * Copyright (c) 2026 Nicholas Newdigate
 * SPDX-License-Identifier: MIT */
#ifndef SYNTHUI_LAMP_MATH_H
#define SYNTHUI_LAMP_MATH_H

#include <stdbool.h>
#include <math.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    SYNTHUI_LAMP_SHAPE_ROUND = 0,
    SYNTHUI_LAMP_SHAPE_BAR,
    SYNTHUI_LAMP_SHAPE_PILL,
} synthui_lamp_shape_t;

typedef struct {
    float w, h;
    float vw, vh, u;
    float inner_w, inner_h, top_line;
    float cx, cy, r;
    float bx, by, bw, bh, br;
    float glow_w, glow_r;
} synthui_lamp_geom_t;

static inline bool synthui_lamp_compute_geom(float w, float h, synthui_lamp_shape_t shape,
                                             synthui_lamp_geom_t *g)
{
    if (w <= 0.0f || h <= 0.0f) return false;
    g->w = w;
    g->h = h;
    g->vw = 100.0f;
    g->vh = roundf((100.0f * h) / w);
    g->u = w / 100.0f;

    g->inner_w = g->vw - 3.2f;
    g->inner_h = g->vh - 3.2f;
    g->top_line = g->vh * 0.06f;
    if (g->top_line < 1.2f) g->top_line = 1.2f;

    const float m = 12.0f;
    const float min_dim = g->vw < g->vh ? g->vw : g->vh;

    g->cx = g->vw * 0.5f;
    g->cy = g->vh * 0.5f;
    g->r = (min_dim * 0.5f) - m;
    g->glow_w = min_dim * 0.22f;
    g->glow_r = g->r + (g->glow_w * 0.5f);

    g->bx = m;
    g->by = g->vh * 0.26f;
    g->bw = g->vw - (m * 2.0f);
    g->bh = g->vh * 0.48f;
    g->br = (shape == SYNTHUI_LAMP_SHAPE_PILL) ? (g->vh * 0.24f) : 2.0f;

    return true;
}

#ifdef __cplusplus
}
#endif
#endif
```

- [ ] **Step 4: Run test to verify it passes**

Run: `cd /Users/moolet/Development/SynthUI && ./tests/run.sh`
Expected: PASS (`lamp_test: all PASS`).

- [ ] **Step 5: Commit in SynthUI**

```bash
cd /Users/moolet/Development/SynthUI
git add src/synthui_lamp_math.h tests/lamp_test.c tests/run.sh
git commit -m "synthui_lamp: geometry math and host unit tests (NEW-24)"
```

---

### Task 2: `synthui_lamp.h` and `synthui_lamp.cpp` (SynthUI repo)

**Files:**
- Create: `/Users/moolet/Development/SynthUI/src/synthui_lamp.h`
- Create: `/Users/moolet/Development/SynthUI/src/synthui_lamp.cpp`

**Interfaces:**
- Produces: `synthui_lamp_class`, `synthui_lamp_create()`, `synthui_lamp_set_on()`, `synthui_lamp_get_on()`, `synthui_lamp_set_shape()`, `synthui_lamp_get_shape()`, `synthui_lamp_set_color()`, `synthui_lamp_get_color()`

- [ ] **Step 1: Create `src/synthui_lamp.h`**

Write `/Users/moolet/Development/SynthUI/src/synthui_lamp.h` with the public API, color macros, shape enum, and prototypes matching the design spec.

- [ ] **Step 2: Create `src/synthui_lamp.cpp`**

Write `/Users/moolet/Development/SynthUI/src/synthui_lamp.cpp`:
- LVGL 9 class definition with `.name = "synthui_lamp"`, default size 48×48, constructor, destructor, and event callback.
- Constructor removes `LV_OBJ_FLAG_SCROLLABLE` and initializes defaults: `shape = SYNTHUI_LAMP_SHAPE_ROUND`, `color = SYNTHUI_LAMP_COLOR_DEFAULT`, `on = true`.
- Event callback handles `LV_EVENT_DRAW_MAIN` with the 5-layer draw sequence (bezel, well, glow if lit, core, top highlight line).
- Early-return setters for `set_on`, `set_shape`, and `set_color` that only call `lv_obj_invalidate(obj)` when values differ.

- [ ] **Step 3: Run host tests**

Run: `cd /Users/moolet/Development/SynthUI && ./tests/run.sh`
Expected: PASS.

- [ ] **Step 4: Commit in SynthUI**

```bash
cd /Users/moolet/Development/SynthUI
git add src/synthui_lamp.h src/synthui_lamp.cpp
git commit -m "synthui_lamp: LVGL 9 custom widget implementation (NEW-24)"
```

---

### Task 3: `examples/display/synthui_lamp_test` (rt1176-evkb repo)

**Files:**
- Create: `/Users/moolet/Development/rt1170/rt1176-evkb/examples/display/synthui_lamp_test/CMakeLists.txt`
- Create: `/Users/moolet/Development/rt1170/rt1176-evkb/examples/display/synthui_lamp_test/synthui_lamp_test.cpp`

**Interfaces:**
- Consumes: `synthui_lamp.h`, `lvgl_mipi_panel.h`, `Display.h`
- Produces: `synthui_lamp_test.elf`

- [x] **Step 1: Create `CMakeLists.txt`**

Create `/Users/moolet/Development/rt1170/rt1176-evkb/examples/display/synthui_lamp_test/CMakeLists.txt`:
```cmake
cmake_minimum_required(VERSION 3.20)
project(synthui_lamp_test)

include(${CMAKE_CURRENT_LIST_DIR}/../../../evkb.cmake)
import_evkb_cores()
import_evkb_display()
import_evkb_pxp()
import_evkb_lvgl()
import_evkb_synthui()

teensy_add_executable(synthui_lamp_test
    synthui_lamp_test.cpp
)

teensy_target_link_libraries(synthui_lamp_test cores MipiDisplay PXP)
target_link_libraries(synthui_lamp_test.elf SynthUI LVGL stdc++)

target_include_directories(synthui_lamp_test.elf PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
)
```

- [x] **Step 2: Create `synthui_lamp_test.cpp`**

Implement `synthui_lamp_test.cpp` with:
- 16-lamp bank (4×4 grid on 720×1280 RK055 panel) exercising Round, Bar, Pill shapes, On/Off states, all 6 DC colors, and Disabled state.
- Double-buffered display setup via `lvgl_mipi_panel_create_db()`.
- Initial full render producing `lamp_crc` checksum over the presented buffer.
- 64-step deterministic delta sequence driven by fixed-seed LCG toggling lamps.
- Delta-equality check comparing delta CRC with fresh full-render CRC (`lamp_delta_crc == lamp_fresh_crc`).
- Damage engagement tracking (`lamp_damage max=... total=...`).
- Tokens: `SYNTHUI_LAMP_BEGIN`, `PANEL_OK`, `LVGL_FLUSHED=PASS`, `LVGL_BYTES=3686400`, `lamp_crc=0x...`, `lamp_delta_crc=0x...`, `lamp_fresh_crc=0x...`, `lamp_delta_eq=PASS`, `lamp_damage max=...`, `crc_done`, `PASS: SynthUI lamp render verified`.
- Phase B continuous chaser animation loop measuring frame time (`lamp_fps`).

- [x] **Step 3: Build the example**

```bash
mkdir -p /Users/moolet/Development/rt1170/rt1176-evkb/examples/display/synthui_lamp_test/build
cd /Users/moolet/Development/rt1170/rt1176-evkb/examples/display/synthui_lamp_test/build
cmake .. -DCMAKE_TOOLCHAIN_FILE=../../../../cmake/toolchain-arm-none-eabi.cmake
cmake --build .
```
Expected: `synthui_lamp_test.elf` created successfully.

- [x] **Step 4: Commit scaffold**

```bash
cd /Users/moolet/Development/rt1170/rt1176-evkb
git add examples/display/synthui_lamp_test
git commit -m "synthui_lamp_test: example scaffolding and 16-lamp test scene (NEW-24)"
```

---

### Task 4: QEMU Gate Runner & Golden Verification (rt1176-evkb repo)

**Files:**
- Create: `/Users/moolet/Development/rt1170/rt1176-evkb/examples/display/synthui_lamp_test/run_qemu.sh`
- Create: `/Users/moolet/Development/rt1170/rt1176-evkb/examples/display/synthui_lamp_test/transcript_qemu.txt`

- [ ] **Step 1: Create `run_qemu.sh`**

Create `examples/display/synthui_lamp_test/run_qemu.sh` using `tools/qrun` (pointing to `/Users/moolet/Development/qemu-rt1170/build/qemu-system-arm` via environment if needed):
- Asserts `PANEL_OK`
- Asserts `LVGL_FLUSHED=PASS`
- Asserts `LVGL_BYTES=3686400`
- Asserts pinned `lamp_crc=0x...`
- Asserts `lamp_delta_eq=PASS`
- Asserts `lamp_damage max <= 10000`
- Asserts `crc_done` and `PASS: SynthUI lamp render verified`

- [ ] **Step 2: Run in QEMU to record initial golden CRC**

Run QEMU to capture output and record the bit-identical golden CRC across two consecutive boots. Pin the CRC in `run_qemu.sh`.

- [ ] **Step 3: Demonstrate Tripwires RED**

Temporarily flip the expected CRC in `run_qemu.sh` to demonstrate that a checksum mismatch fails the gate. Restore the true golden.

- [ ] **Step 4: Verify gate passes GREEN**

Run: `cd /Users/moolet/Development/rt1170/rt1176-evkb/examples/display/synthui_lamp_test && ./run_qemu.sh`
Expected: `PASS: SynthUI lamp render verified`. Save output to `transcript_qemu.txt`.

- [ ] **Step 5: Commit gate and test results**

```bash
cd /Users/moolet/Development/rt1170/rt1176-evkb
git add examples/display/synthui_lamp_test/run_qemu.sh examples/display/synthui_lamp_test/transcript_qemu.txt
git commit -m "synthui_lamp_test: QEMU gate verified green with golden CRC (NEW-24)"
```
