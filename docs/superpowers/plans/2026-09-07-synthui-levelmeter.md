# SynthUI LevelMeter Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Implement the DC LevelMeter primitive as `synthui_level_meter` in SynthUI (LVGL 9, C++ clean-room vector math, delta damage rendering), verified by C++ host unit tests and a new double-buffered QEMU-gated example `examples/display/synthui_level_meter_test` in rt1176-evkb.

**Architecture:** Spec: [`docs/superpowers/specs/2026-09-07-synthui-levelmeter-design.md`](file:///Users/moolet/Development/rt1170/rt1176-evkb/docs/superpowers/specs/2026-09-07-synthui-levelmeter-design.md). Pure C++17 header-only math and types (`synthui_level_meter_types.h`, `synthui_level_meter_math.h`) for viewBox mapping ($w=100, vh=\text{round}(100 \cdot h / w)$), configurable segment counts ($N \in [3, 30]$), and flipped-segment bounding box calculation. Single LVGL 9 widget TU (`synthui_level_meter.cpp`) with early-return guards and scoped `lv_obj_invalidate_area()` delta damage invalidation. The consumer example runs the hardware double-buffer pipeline (`lvgl_mipi_panel_create_db()`) and executes Phase A correctness (pinned golden CRC, 64-step deterministic delta sequence, delta-equality guard, damage bounds) and Phase B (continuous animation benchmark).

**Tech Stack:** C++17, LVGL 9.4.0 (vendored in rt1176-evkb tree), SynthUI (`/Users/moolet/Development/SynthUI`, local-first), rt1176-evkb CMake + QEMU (`/Users/moolet/Development/qemu-rt1170/build/qemu-system-arm`).

## Global Constraints
- Clean-room provenance rules: `reference/` is never compiled; clean-room vector rebuilds from written descriptions only.
- Implement widget and math in modern C++ (C++17).
- `./run_qemu.sh`, never `sh run_qemu.sh`.
- Use the QEMU binary at `/Users/moolet/Development/qemu-rt1170/build/qemu-system-arm`.
- Goldens are RECORDED from two bit-identical consecutive QEMU runs, never derived.
- A demonstrated-RED is required for gate assertions.
- Delta damage engagement guard: `max_damage <= 15000` px.

---

### Task 1: `synthui_level_meter_types.h`, `synthui_level_meter_math.h` + Host Unit Test (SynthUI repo)

**Files:**
- Create: `/Users/moolet/Development/SynthUI/src/synthui_level_meter_types.h`
- Create: `/Users/moolet/Development/SynthUI/src/synthui_level_meter_math.h`
- Create: `/Users/moolet/Development/SynthUI/tests/level_meter_test.cpp`
- Modify: `/Users/moolet/Development/SynthUI/tests/run.sh`

**Interfaces:**
- Produces: `namespace synthui::level_meter`:
  - Constants: `MIN_SEGMENTS`, `MAX_SEGMENTS`, `DEFAULT_SEGMENTS`, `COLOR_RED`, `COLOR_AMBER`, `COLOR_GREEN`, `COLOR_SLOT_BG`, `COLOR_HIGHLIGHT`, `COLOR_PANEL_DEF`
  - Types: `Point`, `Rect`, `SegmentGeom`, `MeterLayout`
  - Functions: `clamp()`, `compute_layout()`, `get_segment_color()`, `get_segment_geom()`, `compute_segment_states()`, `get_flipped_bounds()`

- [ ] **Step 1: Write the failing host test**

Create `/Users/moolet/Development/SynthUI/tests/level_meter_test.cpp`:
```cpp
/* level_meter_test.cpp - host unit test for pure level meter geometry & math.
 * Copyright (c) 2026 Nicholas Newdigate
 * SPDX-License-Identifier: MIT */
#undef NDEBUG
#include "../src/synthui_level_meter_math.h"
#include <cassert>
#include <cmath>
#include <cstdio>

static bool approx_eq(float a, float b) {
    return std::fabs(a - b) < 0.05f;
}

int main() {
    using namespace synthui::level_meter;

    // 1. Layout calculation: default size W=34, H=200, N=10, value=0.6, peak=0.8
    MeterLayout layout{};
    assert(compute_layout(34.0f, 200.0f, 10, 0.6f, 0.8f, layout));
    assert(approx_eq(layout.w, 34.0f));
    assert(approx_eq(layout.h, 200.0f));
    assert(approx_eq(layout.vh, 588.0f)); // round(100 * 200 / 34)
    assert(approx_eq(layout.u, 0.34f));
    assert(layout.n == 10);
    assert(layout.lit == 6); // round(0.6 * 10)
    assert(layout.peak_idx == 7); // round(0.8 * 10) - 1

    // Geometry parameters:
    // pad = 588 * 0.02 = 11.76
    // pitch = (588 - 23.52) / 10 = 56.448
    // seg_h = 56.448 * 0.7 = 39.5136
    assert(approx_eq(layout.pad, 11.76f));
    assert(approx_eq(layout.pitch, 56.448f));
    assert(approx_eq(layout.seg_h, 39.5136f));

    // 2. Clamping of segment counts N in [3, 30]
    MeterLayout l_clamp{};
    assert(compute_layout(34.0f, 200.0f, 1, 0.5f, -1.0f, l_clamp));
    assert(l_clamp.n == 3);
    assert(compute_layout(34.0f, 200.0f, 50, 0.5f, -1.0f, l_clamp));
    assert(l_clamp.n == 30);

    // 3. Color thresholds for N=10:
    // i=0..6: f < 0.72 -> Green
    // i=7..8: f in [0.72, 0.9) -> Amber
    // i=9: f >= 0.9 -> Red
    assert(get_segment_color(0, 10) == COLOR_GREEN);
    assert(get_segment_color(6, 10) == COLOR_GREEN); // f = 6/9 = 0.666
    assert(get_segment_color(7, 10) == COLOR_AMBER); // f = 7/9 = 0.777
    assert(get_segment_color(8, 10) == COLOR_AMBER); // f = 8/9 = 0.888
    assert(get_segment_color(9, 10) == COLOR_RED);   // f = 9/9 = 1.0

    // 4. Segment geometry verification for bottom segment (i=0) and top segment (i=9)
    SegmentGeom s0{};
    get_segment_geom(layout, 0, false, s0);
    assert(s0.on == true); // i=0 < lit(6)
    assert(s0.is_peak == false);
    assert(approx_eq(s0.slot.x, 10.0f));
    assert(approx_eq(s0.slot.w, 80.0f));
    assert(approx_eq(s0.slot.h, 39.5f));
    assert(approx_eq(s0.highlight.x, 18.0f));
    assert(approx_eq(s0.highlight.w, 64.0f));
    assert(approx_eq(s0.core_opa, 1.0f));
    assert(approx_eq(s0.hl_opa, 0.5f));
    assert(s0.color == COLOR_GREEN);

    SegmentGeom s7{}; // Peak segment
    get_segment_geom(layout, 7, false, s7);
    assert(s7.on == true); // peakIdx == 7
    assert(s7.is_peak == true); // 7 >= lit(6)
    assert(approx_eq(s7.glow_opa, 0.22f)); // isPeak glow
    assert(s7.color == COLOR_AMBER);

    SegmentGeom s9{}; // Top segment, unlit
    get_segment_geom(layout, 9, false, s9);
    assert(s9.on == false);
    assert(s9.is_peak == false);
    assert(approx_eq(s9.core_opa, 0.17f)); // ghost
    assert(approx_eq(s9.hl_opa, 0.06f));
    assert(s9.color == COLOR_RED);

    // With clip=true, top segment must turn on
    SegmentGeom s9_clipped{};
    get_segment_geom(layout, 9, true, s9_clipped);
    assert(s9_clipped.on == true);
    assert(approx_eq(s9_clipped.core_opa, 1.0f));

    // 5. Delta damage tracking:
    // When value changes from 0.6 to 0.7 (lit goes from 6 to 7),
    // segment 6 flips from OFF to ON.
    uint32_t state_old = compute_segment_states(layout, false);
    MeterLayout layout_new = layout;
    layout_new.lit = 7;
    uint32_t state_new = compute_segment_states(layout_new, false);
    assert(state_old != state_new);

    int min_flipped = -1;
    int max_flipped = -1;
    assert(get_flipped_range(state_old, state_new, layout.n, min_flipped, max_flipped));
    assert(min_flipped == 6);
    assert(max_flipped == 6);

    std::printf("level_meter_test: all PASS\n");
    return 0;
}
```

- [ ] **Step 2: Run test to verify it fails**

Run: `c++ -std=c++17 -Wall -Wextra -Werror tests/level_meter_test.cpp -o /tmp/level_meter_test`
Expected: FAIL (header not found).

- [ ] **Step 3: Implement `synthui_level_meter_types.h` and `synthui_level_meter_math.h`**

Create `/Users/moolet/Development/SynthUI/src/synthui_level_meter_types.h` and `/Users/moolet/Development/SynthUI/src/synthui_level_meter_math.h` as designed in the spec.

- [ ] **Step 4: Update `tests/run.sh` and verify host tests pass**

Add `level_meter_test.cpp` compilation to `/Users/moolet/Development/SynthUI/tests/run.sh`:
```sh
c++ -std=c++17 -Wall -Wextra -Werror -o "$out/level_meter_test" tests/level_meter_test.cpp
"$out/level_meter_test"
```
Run `./tests/run.sh` in `/Users/moolet/Development/SynthUI`.
Expected: `level_meter_test: all PASS` and all other tests PASS.

- [ ] **Step 5: Commit Task 1 in SynthUI repo**

```bash
git add src/synthui_level_meter_types.h src/synthui_level_meter_math.h tests/level_meter_test.cpp tests/run.sh
git commit -m "synthui_level_meter: C++ geometry math, types, and host unit tests (NEW-26)"
```

---

### Task 2: LVGL 9 Custom Widget Implementation (SynthUI repo)

**Files:**
- Create: `/Users/moolet/Development/SynthUI/src/synthui_level_meter.h`
- Create: `/Users/moolet/Development/SynthUI/src/synthui_level_meter.cpp`

**Interfaces:**
- Produces:
  - C API: `synthui_level_meter_create()`, `synthui_level_meter_set_value()`, `synthui_level_meter_get_value()`, `synthui_level_meter_set_peak()`, `synthui_level_meter_get_peak()`, `synthui_level_meter_set_clip()`, `synthui_level_meter_get_clip()`, `synthui_level_meter_set_segments()`, `synthui_level_meter_get_segments()`, `synthui_level_meter_set_panel_color()`, `synthui_level_meter_get_panel_color()`, `synthui_level_meter_class`
  - C++ API: `class synthui::LevelMeter`

- [ ] **Step 1: Implement `synthui_level_meter.h`**

Create `/Users/moolet/Development/SynthUI/src/synthui_level_meter.h` with C function prototypes and `synthui::LevelMeter` C++ wrapper class.

- [ ] **Step 2: Implement `synthui_level_meter.cpp`**

Create `/Users/moolet/Development/SynthUI/src/synthui_level_meter.cpp`:
- `synthui_level_meter_t` widget struct.
- Constructor, destructor, event callback, and `meter_draw()`.
- Two-pass rendering:
  - Pass 1: Panel background rect (`panel_color`) clipped to `layer->_clip_area`.
  - Pass 2: For each segment $i \in [0, n - 1]$ intersecting `layer->_clip_area`:
    - Slot background (`#12161A`, cover, radius `rx * u`).
    - Glow (if `on`): outer rounded rect inflated by `0.35 * seg_h * u`, color `color`, opacity 56 (`is_peak`) or 87 (normal lit).
    - Core segment: color `color`, opacity 255 (`on`) or 43 (unlit ghost 17%).
    - Highlight strip: `#FFFFFF`, opacity 128 (`on`) or 15 (unlit ghost 6%).
- Early-return setters with delta damage invalidation:
  - `synthui_level_meter_set_value`: checks equality or NaN, calculates flipped segments, computes dirty area enclosing changed segments with glow margin, and calls `lv_obj_invalidate_area()`.
  - `synthui_level_meter_set_peak`: checks equality or NaN, calculates old vs new peak segment, invalidates old and new segment areas via `lv_obj_invalidate_area()`.
  - `synthui_level_meter_set_clip`: checks equality, invalidates top segment $n-1$ area via `lv_obj_invalidate_area()`.
  - `synthui_level_meter_set_segments`: checks equality, calls `lv_obj_invalidate()`.
  - `synthui_level_meter_set_panel_color`: checks equality, calls `lv_obj_invalidate()`.

- [ ] **Step 3: Run host test suite**

Run `./tests/run.sh` in `SynthUI`. Ensure clean pass.

- [ ] **Step 4: Commit Task 2 in SynthUI repo**

```bash
git add src/synthui_level_meter.h src/synthui_level_meter.cpp
git commit -m "synthui_level_meter: LVGL 9 custom widget with delta damage invalidation (NEW-26)"
```

---

### Task 3: Consumer Example Scaffolding & 8-Meter Test Scene (rt1176-evkb repo)

**Files:**
- Create: `/Users/moolet/Development/rt1170/rt1176-evkb/examples/display/synthui_level_meter_test/CMakeLists.txt`
- Create: `/Users/moolet/Development/rt1170/rt1176-evkb/examples/display/synthui_level_meter_test/synthui_level_meter_test.cpp`
- Create: `/Users/moolet/Development/rt1170/rt1176-evkb/examples/display/synthui_level_meter_test/run_qemu.sh`

**Interfaces:**
- Consumes: `synthui_level_meter.h`, `lvgl_mipi_panel_create_db(Display)`
- Produces: `synthui_level_meter_test.elf`, `run_qemu.sh`

- [ ] **Step 1: Create `CMakeLists.txt`**

```cmake
cmake_minimum_required(VERSION 3.24)
project(synthui_level_meter_test)

add_compile_definitions(LV_COLOR_DEPTH=32 PANEL_BYTES_PER_PIXEL=4)

set(TEENSY_VERSION 117 CACHE STRING "")

include(${CMAKE_CURRENT_LIST_DIR}/../../../evkb.cmake)

import_evkb_lvgl()
import_evkb_synthui()
import_evkb_library(MipiDisplay soc panels/rk055)
import_evkb_library(PXP)

evkb_library_dir(LVGL _lvgl_dir)

teensy_add_executable(synthui_level_meter_test
    synthui_level_meter_test.cpp
    ${_lvgl_dir}/port/lvgl_mipi_panel.cpp
)
teensy_target_link_libraries(synthui_level_meter_test cores MipiDisplay PXP)

target_link_libraries(synthui_level_meter_test.elf SynthUI LVGL stdc++)

target_include_directories(synthui_level_meter_test.elf PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
)

if(LEVEL_METER_EYEBALL_HOLD)
    add_compile_definitions(LEVEL_METER_EYEBALL_HOLD=${LEVEL_METER_EYEBALL_HOLD})
endif()
```

- [ ] **Step 2: Create `synthui_level_meter_test.cpp`**

Setup double-buffered RK055 panel (`lvgl_mipi_panel_create_db(Display)`).
Register damage monitor callback (`LV_EVENT_INVALIDATE_AREA`).
Build 8-meter console scene:
- Master L/R: 24 segments, $W=48, H=380$ at $(80, 80)$ and $(144, 80)$.
- Tracks 1–4: 16 segments, $W=36, H=240$ at $(240, 80)$, $(290, 80)$, $(340, 80)$, $(390, 80)$.
- Aux 1–2: 10 segments, $W=42, H=180$ at $(470, 80)$ and $(530, 80)$ (Aux 2 has `clip=true`).
Phase A1:
- `lv_refr_now(disp); lvgl_mipi_panel_flip_sync();`
- Compute FNV-1a 32-bit CRC over 3,686,400 bytes presented buffer.
- Print `level_meter_crc=0x...`
Phase A2 (64-step deterministic delta sequence):
- Reset damage counters (`g_max_damage = 0`, `g_total_damage = 0`).
- For 64 steps: animate master, track, and aux values/peaks. `lv_refr_now(disp); lvgl_mipi_panel_flip_sync();`
- Read `delta_crc = fnv1a_32(...)`.
- `lv_obj_invalidate(scr); lv_refr_now(disp); lvgl_mipi_panel_flip_sync();`
- Read `fresh_crc = fnv1a_32(...)`.
- Compare `delta_crc == fresh_crc` -> `level_meter_delta_eq=PASS`.
- Print `level_meter_damage max=... total=...`
- Print `level_meter_vsync flips=... isrs=... timeouts=0`
- Print `crc_done` and `PASS: SynthUI level_meter render verified`.
Phase B: Continuous benchmark in `loop()`.

- [ ] **Step 3: Create `run_qemu.sh`**

Implement QEMU test runner verifying UART tokens, golden CRC, delta equality, damage bound (`max_damage <= 15000`), and VSYNC health.

- [ ] **Step 4: Build executable**

```bash
cmake -B build -S .
cmake --build build -j8
```

- [ ] **Step 5: Commit Task 3 in rt1176-evkb**

```bash
git add examples/display/synthui_level_meter_test/
git commit -m "synthui_level_meter_test: example scaffolding and 8-meter synth console scene (NEW-26)"
```

---

### Task 4: Verification, QEMU Gate, Golden CRC, Eyeball Hold, and Pin Bump (rt1176-evkb repo)

**Files:**
- Modify: `/Users/moolet/Development/rt1170/rt1176-evkb/examples/display/synthui_level_meter_test/run_qemu.sh`
- Modify: `/Users/moolet/Development/rt1170/rt1176-evkb/evkb.cmake`
- Create: `/Users/moolet/Development/rt1170/rt1176-evkb/examples/display/synthui_level_meter_test/synthui_level_meter_scene.png`

- [ ] **Step 1: Execute QEMU gate to record golden CRC across consecutive runs**

Run `./run_qemu.sh` twice to confirm bit-exact deterministic CRC.
Update golden CRC check in `run_qemu.sh`.

- [ ] **Step 2: Demonstrate tripwire RED**

Temporarily alter golden CRC in `run_qemu.sh` (e.g. to `0xDEADBEEF`), run `./run_qemu.sh`, verify it fails with non-zero exit code and reports checksum failure. Revert back to verified golden CRC.

- [ ] **Step 3: Verify all gate assertions pass green**

Run `./run_qemu.sh`.
Confirm:
- `PANEL_OK`
- Initial golden CRC match
- `level_meter_delta_eq=PASS`
- `max_damage <= 15000`
- `timeouts=0`
- `PASS: SynthUI level_meter render verified`

- [ ] **Step 4: Build with eyeball hold and capture framebuffer screenshot**

Build with `-DLEVEL_METER_EYEBALL_HOLD=1`.
Launch QEMU, connect to monitor or use `-monitor unix:...`, issue `pmemsave 0x80000000 3686400 /tmp/level_meter_fb.raw`.
Convert raw XRGB8888 (720×1280) to PNG:
`python3 -c "from PIL import Image; ..."`
Save to:
- `/Users/moolet/Development/rt1170/rt1176-evkb/examples/display/synthui_level_meter_test/synthui_level_meter_scene.png`
- Artifact directory.

- [ ] **Step 5: Bump SynthUI pin in `evkb.cmake`**

Get HEAD commit SHA in SynthUI (`git rev-parse HEAD`).
Update line 131 in `/Users/moolet/Development/rt1170/rt1176-evkb/evkb.cmake` with new SHA.

- [ ] **Step 6: Commit Task 4 in rt1176-evkb**

```bash
git add examples/display/synthui_level_meter_test/ run_qemu.sh evkb.cmake
git commit -m "synthui_level_meter_test: QEMU gate verified green, golden CRC, and pin bump (NEW-26)"
```
