# SynthUI PianoKey Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Implement the DC PianoKey primitive as `synthui_piano_key` in SynthUI (LVGL 9, C++17 clean-room vector math, delta damage rendering), verified by pure C++ host unit tests in `SynthUI/tests/piano_key_test.cpp` and a new double-buffered QEMU-gated example `examples/display/synthui_piano_key_test` in rt1176-evkb.

**Architecture:** Spec: [`docs/superpowers/specs/2026-09-07-synthui-pianokey-design.md`](file:///Users/moolet/Development/rt1170/rt1176-evkb/docs/superpowers/specs/2026-09-07-synthui-pianokey-design.md). Pure C++17 header-only math and types (`synthui_piano_key_types.h`, `synthui_piano_key_math.h`) for viewBox mapping ($w=100, vh=\text{round}(100 \cdot h / w)$), key deflection ($dy = \text{pressed} \; ? \; vh \cdot 0.012 : 0$), LED bloom ($R_{bloom} = 1.75 \cdot ledR$), and ridged pad geometry. Single LVGL 9 widget TU (`synthui_piano_key.cpp`) with early-return guards, targeted `lv_obj_invalidate_area()` LED damage bounding box invalidation, and sub-element clipping. The consumer example runs the hardware double-buffer pipeline (`lvgl_mipi_panel_create_db()`) and executes Phase A correctness (pinned golden CRC, 64-step deterministic delta sequence, delta-equality guard, damage bounds) and Phase B (continuous animation benchmark).

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

### Task 1: `synthui_piano_key_types.h`, `synthui_piano_key_math.h` + Host Unit Test (SynthUI repo)

**Files:**
- Create: `/Users/moolet/Development/SynthUI/src/synthui_piano_key_types.h`
- Create: `/Users/moolet/Development/SynthUI/src/synthui_piano_key_math.h`
- Create: `/Users/moolet/Development/SynthUI/tests/piano_key_test.cpp`
- Modify: `/Users/moolet/Development/SynthUI/tests/run.sh`

**Interfaces:**
- Produces:
  - `synthui_piano_key_types.h`: `synthui_piano_key_type_t` (`SYNTHUI_PIANO_KEY_WHITE`, `SYNTHUI_PIANO_KEY_BLACK`), color constants.
  - `namespace synthui::piano_key`: `KeyGeom`, `compute_geom()`, `compute_led_dirty_area()`, `compute_key_dirty_area()`.

- [ ] **Step 1: Write the failing host test**

Create `/Users/moolet/Development/SynthUI/tests/piano_key_test.cpp`:
```cpp
/* piano_key_test.cpp - host unit test for pure piano key geometry & math.
 * Copyright (c) 2026 Nicholas Newdigate
 * SPDX-License-Identifier: MIT */
#undef NDEBUG
#include "../src/synthui_piano_key_math.h"
#include <cassert>
#include <cmath>
#include <cstdio>

static bool approx_eq(float a, float b) {
    return std::fabs(a - b) < 0.05f;
}

int main() {
    using namespace synthui::piano_key;

    // 1. Default white key geometry: w=46, h=158, type=white, lit=false, pressed=false, zone=0.5, padHeight=46
    KeyGeom gw{};
    assert(compute_geom(46.0f, 158.0f, SYNTHUI_PIANO_KEY_WHITE, false, false, 0.5f, 46.0f, gw));
    assert(approx_eq(gw.w, 46.0f));
    assert(approx_eq(gw.h, 158.0f));
    assert(approx_eq(gw.vw, 100.0f));
    assert(approx_eq(gw.vh, 343.0f)); // round(100 * 158 / 46) = round(343.478) = 343
    assert(approx_eq(gw.u, 0.46f));
    assert(gw.type == SYNTHUI_PIANO_KEY_WHITE);
    assert(!gw.lit);
    assert(!gw.pressed);
    assert(approx_eq(gw.dy, 0.0f));
    assert(approx_eq(gw.body_r, 4.0f));
    assert(approx_eq(gw.edge_w, 1.8f));
    assert(gw.edge_c == SYNTHUI_PIANO_KEY_COLOR_WHITE_EDGE);
    assert(approx_eq(gw.shade_a, 0.07f));
    assert(gw.body_a == SYNTHUI_PIANO_KEY_COLOR_WHITE_A_UNPRESSED);
    assert(gw.body_b == SYNTHUI_PIANO_KEY_COLOR_WHITE_B_UNPRESSED);
    assert(gw.body_c == SYNTHUI_PIANO_KEY_COLOR_WHITE_C_UNPRESSED);

    // LED coordinates: cx=50, ledY = vh * (zone + 0.055) = 343 * 0.555 = 190.365
    // ledR = min(14, 343 * 0.032 + 4) = min(14, 14.976) = 14
    assert(approx_eq(gw.led_cx, 50.0f));
    assert(approx_eq(gw.led_cy, 190.365f));
    assert(approx_eq(gw.led_r, 14.0f));
    assert(approx_eq(gw.led_bloom_r, 24.5f)); // 1.75 * 14 = 24.5
    assert(gw.led_fill == SYNTHUI_PIANO_KEY_COLOR_LED_UNLIT);

    // Pad geometry: padTop = vh * (zone + 0.14) = 343 * 0.64 = 219.52
    // padH = padHeight * 100 / w = 46 * 100 / 46 = 100
    assert(approx_eq(gw.pad_x, 19.0f));
    assert(approx_eq(gw.pad_w, 62.0f));
    assert(approx_eq(gw.pad_y, 219.52f));
    assert(approx_eq(gw.pad_h, 100.0f));

    // 4 Ridges on pad:
    // ridge fractions: 0.22, 0.42, 0.62, 0.82 of padH
    assert(approx_eq(gw.ridge_y[0], 22.0f));
    assert(approx_eq(gw.ridge_y[1], 42.0f));
    assert(approx_eq(gw.ridge_y[2], 62.0f));
    assert(approx_eq(gw.ridge_y[3], 82.0f));
    assert(approx_eq(gw.ridge_x1, 28.92f)); // 19 + 62 * 0.16 = 28.92
    assert(approx_eq(gw.ridge_x2, 71.08f)); // 19 + 62 * 0.84 = 71.08
    assert(approx_eq(gw.ridge_w, 3.5f));   // padH * 0.035 = 3.5

    // 2. White key pressed & lit
    KeyGeom gwp{};
    assert(compute_geom(46.0f, 158.0f, SYNTHUI_PIANO_KEY_WHITE, true, true, 0.5f, 46.0f, gwp));
    assert(gwp.lit);
    assert(gwp.pressed);
    assert(approx_eq(gwp.dy, 343.0f * 0.012f)); // 4.116
    assert(approx_eq(gwp.led_cy, 190.365f + 4.116f));
    assert(approx_eq(gwp.pad_y, 219.52f + 4.116f));
    assert(gwp.led_fill == SYNTHUI_PIANO_KEY_COLOR_LED_LIT);
    assert(gwp.body_a == SYNTHUI_PIANO_KEY_COLOR_WHITE_A_PRESSED);
    assert(gwp.body_b == SYNTHUI_PIANO_KEY_COLOR_WHITE_B_PRESSED);
    assert(gwp.body_c == SYNTHUI_PIANO_KEY_COLOR_WHITE_C_PRESSED);

    // 3. Black key default geometry: w=30, h=76, type=black, zone=0.1, padHeight=46
    KeyGeom gb{};
    assert(compute_geom(30.0f, 76.0f, SYNTHUI_PIANO_KEY_BLACK, false, false, 0.1f, 46.0f, gb));
    assert(approx_eq(gb.vh, 253.0f)); // round(100 * 76 / 30) = round(253.333) = 253
    assert(approx_eq(gb.u, 0.30f));
    assert(approx_eq(gb.body_r, 3.0f));
    assert(approx_eq(gb.edge_w, 1.6f));
    assert(gb.edge_c == SYNTHUI_PIANO_KEY_COLOR_BLACK_EDGE);
    assert(approx_eq(gb.shade_a, 0.25f));
    assert(gb.body_a == SYNTHUI_PIANO_KEY_COLOR_BLACK_A);
    assert(gb.body_b == SYNTHUI_PIANO_KEY_COLOR_BLACK_B);
    assert(gb.body_c == SYNTHUI_PIANO_KEY_COLOR_BLACK_C);

    // Black key LED: cy = 253 * (0.1 + 0.055) = 39.215
    // ledR = min(14, 253 * 0.032 + 4) = min(14, 12.096) = 12.096
    assert(approx_eq(gb.led_cy, 39.215f));
    assert(approx_eq(gb.led_r, 12.096f));

    // 4. Delta damage bounding box:
    int32_t x1 = 0, y1 = 0, x2 = 0, y2 = 0;
    compute_led_dirty_area(gw, 100, 200, x1, y1, x2, y2);
    int32_t dmg_w = x2 - x1 + 1;
    int32_t dmg_h = y2 - y1 + 1;
    assert(dmg_w > 0 && dmg_h > 0);
    assert((uint32_t)(dmg_w * dmg_h) <= 1500u); // Tight LED bloom bbox

    // 5. Invalid input checks
    KeyGeom bad{};
    assert(!compute_geom(0.0f, 100.0f, SYNTHUI_PIANO_KEY_WHITE, false, false, 0.5f, 40.0f, bad));
    assert(!compute_geom(50.0f, 0.0f, SYNTHUI_PIANO_KEY_WHITE, false, false, 0.5f, 40.0f, bad));

    std::printf("piano_key_test: all PASS\n");
    return 0;
}
```

- [ ] **Step 2: Add test to `tests/run.sh` and verify failure**

Modify `/Users/moolet/Development/SynthUI/tests/run.sh` to include:
```sh
c++ -std=c++17 -Wall -Wextra -Werror -o "$out/piano_key_test" tests/piano_key_test.cpp
"$out/piano_key_test"
```
Run `tests/run.sh` and verify compilation fails (missing headers).

- [ ] **Step 3: Implement `synthui_piano_key_types.h` and `synthui_piano_key_math.h`**

Create `/Users/moolet/Development/SynthUI/src/synthui_piano_key_types.h` with definitions from the spec.
Create `/Users/moolet/Development/SynthUI/src/synthui_piano_key_math.h` with `compute_geom()`, `compute_led_dirty_area()`, and `compute_key_dirty_area()`.

- [ ] **Step 4: Run tests and verify PASS**

Run `tests/run.sh` in `/Users/moolet/Development/SynthUI`.
Expected: `piano_key_test: all PASS`.

- [ ] **Step 5: Commit**

```bash
git add src/synthui_piano_key_types.h src/synthui_piano_key_math.h tests/piano_key_test.cpp tests/run.sh
git commit -m "synthui_piano_key: C++ geometry math, types, and host unit tests (NEW-28)"
```

---

### Task 2: `synthui_piano_key.h` & `synthui_piano_key.cpp` LVGL 9 Custom Widget (SynthUI repo)

**Files:**
- Create: `/Users/moolet/Development/SynthUI/src/synthui_piano_key.h`
- Create: `/Users/moolet/Development/SynthUI/src/synthui_piano_key.cpp`

**Interfaces:**
- Produces:
  - C API: `synthui_piano_key_create()`, `synthui_piano_key_set_type()`, `synthui_piano_key_set_lit()`, `synthui_piano_key_set_pressed()`, `synthui_piano_key_set_zone_top()`, `synthui_piano_key_set_pad_height()`, and matching getters.
  - C++ wrapper class: `synthui::PianoKey`.

- [ ] **Step 1: Write header `src/synthui_piano_key.h`**

Includes `<lvgl.h>`, `<stdbool.h>`, `<stdint.h>`, `"synthui_piano_key_types.h"`.
Defines extern `synthui_piano_key_class`, C API declarations, and `namespace synthui { class PianoKey; }`.

- [ ] **Step 2: Write implementation `src/synthui_piano_key.cpp`**

Implements:
- Class struct `synthui_piano_key_t` containing `lv_obj_t`, `type`, `lit`, `pressed`, `zone_top`, `pad_height`.
- Constructor with defaults: `type = WHITE`, `lit = false`, `pressed = false`, `zone_top = 0.58f`, `pad_height = 48.0f`.
- Early-return setters with targeted delta invalidations:
  - `set_lit`: calls `compute_led_dirty_area()` and `lv_obj_invalidate_area()`.
  - `set_pressed`, `set_type`, `set_zone_top`, `set_pad_height`: calls `lv_obj_invalidate()`.
- Event handler: intercepts `LV_EVENT_DRAW_MAIN`.
- Draw method `key_draw(key, layer)`:
  - Computes `KeyGeom`.
  - Performs intersection clipping against `layer->_clip_area`:
    - Key body (composed 2-stop horizontal gradient + edge stroke + shade band).
    - LED (bloom outer circle + core circle).
    - Pad (cast shadow + composed 2-stop horizontal gradient face + border stroke + 4 ridge lines with round caps).

- [ ] **Step 3: Verify host test suite still passes**

Run `tests/run.sh` in `SynthUI`.
Expected: PASS.

- [ ] **Step 4: Commit**

```bash
git add src/synthui_piano_key.h src/synthui_piano_key.cpp
git commit -m "synthui_piano_key: LVGL 9 custom widget with delta damage invalidation (NEW-28)"
```

---

### Task 3: `synthui_piano_key_test` EVKB Display Example Scaffolding & Scene (rt1176-evkb repo)

**Files:**
- Create: `/Users/moolet/Development/rt1170/rt1176-evkb/examples/display/synthui_piano_key_test/CMakeLists.txt`
- Create: `/Users/moolet/Development/rt1170/rt1176-evkb/examples/display/synthui_piano_key_test/synthui_piano_key_test.cpp`

**Interfaces:**
- Consumes:
  - `lvgl_mipi_panel_create_db()`
  - `synthui_piano_key.h`
  - `synthui_panel_button.h`
  - `synthui_seven_segment.h`

- [ ] **Step 1: Write `CMakeLists.txt`**

```cmake
cmake_minimum_required(VERSION 3.20)
project(synthui_piano_key_test C CXX ASM)
include(${CMAKE_CURRENT_LIST_DIR}/../../../evkb.cmake)

import_evkb_synthui()

add_executable(synthui_piano_key_test
    synthui_piano_key_test.cpp
)
target_link_libraries(synthui_piano_key_test PRIVATE
    teensy_main
    SynthUI
    Display
    LVGL
)
teensy_target_link_libraries(synthui_piano_key_test
    teensy_flags
)
```

- [ ] **Step 2: Write `synthui_piano_key_test.cpp`**

Implements:
- Damage monitor event callback (`damage_monitor_cb`) tracking `g_max_damage` and `g_total_damage`.
- Setup function:
  - Initializes `Display` and `lvgl_mipi_panel_create_db()`.
  - Creates dark background (`#1a1a1c`).
  - Top console strip: Play button, Stop button, SevenSegment note readout.
  - 2-Octave Keyboard:
    - 14 white keys ($w=46, h=158, \text{gap}=2$, $x=25$).
    - 10 black keys ($w=30, h=76$, calculated offsets centered on white boundaries).
  - State demo section: 6 cards displaying the 6 canonical states.
  - Phase A1: `lv_refr_now()`, `lvgl_mipi_panel_flip_sync()`, prints `PANEL_OK`, `LVGL_FLUSHED=PASS`, `LVGL_BYTES=3686400`, and calculates initial presented buffer CRC.
  - `#ifdef PIANO_KEY_EYEBALL_HOLD`: pauses loop for visual capture.
  - Phase A2: 64-step deterministic delta sequence (stepping through notes, toggling lit and pressed states).
  - Computes `piano_key_delta_crc`.
  - Full redraw of final state to compute `piano_key_fresh_crc`.
  - Checks delta equality `piano_key_delta_eq=PASS` and reports `piano_key_damage max=...` and `piano_key_vsync flips=... isrs=... timeouts=0`.
  - Prints `crc_done` and `PASS: SynthUI piano_key render verified`.
  - `loop()`: continuous animation benchmark reporting `piano_key_fps=...`.

- [ ] **Step 3: Commit**

```bash
git add examples/display/synthui_piano_key_test/CMakeLists.txt examples/display/synthui_piano_key_test/synthui_piano_key_test.cpp
git commit -m "synthui_piano_key_test: example scaffolding and 2-octave keyboard scene (NEW-28)"
```

---

### Task 4: QEMU Gate Runner, Golden Checksum, Tripwire RED, and Pin Bump (rt1176-evkb repo)

**Files:**
- Create: `/Users/moolet/Development/rt1170/rt1176-evkb/examples/display/synthui_piano_key_test/run_qemu.sh`
- Modify: `/Users/moolet/Development/rt1170/rt1176-evkb/evkb.cmake`

- [ ] **Step 1: Write `run_qemu.sh`**

Implement `run_qemu.sh` based on `synthui_seven_segment_test/run_qemu.sh`:
- Sources `gate-lib.sh`.
- Runs ELF under QEMU for 20 seconds.
- Verifies `PANEL_OK`, `LVGL_FLUSHED=PASS`, `LVGL_BYTES=3686400`.
- Verifies pinned golden CRC `piano_key_crc=0xXXXXXXXX`.
- Verifies delta equality `piano_key_delta_eq=PASS`.
- Verifies damage engagement `DAREA <= 15000`.
- Verifies VSYNC health `timeouts=0`.
- Verifies `crc_done` and `PASS: SynthUI piano_key render verified`.

- [ ] **Step 2: Build target and run initial QEMU execution**

Run CMake configure and build for `synthui_piano_key_test`.
Run `run_qemu.sh` to record the initial CRC.
Run again to verify bit-identical repeatability.

- [ ] **Step 3: Demonstrate tripwire RED**

Temporarily alter the golden CRC in `run_qemu.sh` to `0xDEADBEEF` and run `./run_qemu.sh`.
Verify the gate exits with non-zero failure ("FAIL: piano_key checksum").
Restore the golden CRC and verify it passes GREEN.

- [ ] **Step 4: Bump SynthUI pin in `evkb.cmake`**

Get the latest SynthUI commit hash (`git rev-parse HEAD` in `/Users/moolet/Development/SynthUI`).
Update line 131 in `/Users/moolet/Development/rt1170/rt1176-evkb/evkb.cmake` to point to the new commit.

- [ ] **Step 5: Commit**

```bash
git add examples/display/synthui_piano_key_test/run_qemu.sh evkb.cmake
git commit -m "synthui_piano_key_test: QEMU gate verified green, golden CRC, and pin bump (NEW-28)"
```

---

### Task 5: Framebuffer Capture, Visual Review & Subagent Review

**Files:**
- Test artifact: `/Users/moolet/.gemini/antigravity/brain/30291b7f-0e3e-4b57-9d54-3439196f5646/pianokey_panel.png`
- Test artifact: `/Users/moolet/.gemini/antigravity/brain/30291b7f-0e3e-4b57-9d54-3439196f5646/pianokey_crop.png`
- Walkthrough: `/Users/moolet/.gemini/antigravity/brain/30291b7f-0e3e-4b57-9d54-3439196f5646/walkthrough.md`

- [ ] **Step 1: Capture framebuffer via QEMU monitor**

Build with `-DPIANO_KEY_EYEBALL_HOLD` to pause on initial rendered frame.
Launch QEMU with `-qmp` or monitor console, execute `pmemsave 0x80000000 3686400 raw_fb.bin`.
Convert 720x1280 raw ARGB/XRGB pixels to PNG (`pianokey_panel.png`).
Create cropped close-up of keyboard & state cards (`pianokey_crop.png`).

- [ ] **Step 2: Subagent task code review & whole branch verification**

Dispatch code review subagent to inspect diff across both repos.
Verify clean-room provenance, pure host unit tests passing, delta damage bounds, and double-buffered tear-free rendering.

- [ ] **Step 3: Document walkthrough and present visuals to user**

Create walkthrough artifact showing verification evidence, test outputs, and screenshots.
