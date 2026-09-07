# SynthUI SlideToggle Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Implement the DC SlideToggle primitive as `synthui_slide_toggle` in SynthUI (LVGL 9, C++17 clean-room vector math, delta damage rendering), verified by pure C++ host unit tests in `SynthUI/tests/slide_toggle_test.cpp` and a new double-buffered QEMU-gated example `examples/display/synthui_slide_toggle_test` in rt1176-evkb.

**Architecture:** Spec: [`docs/superpowers/specs/2026-09-07-synthui-slidetoggle-design.md`](file:///Users/moolet/Development/rt1170/rt1176-evkb/docs/superpowers/specs/2026-09-07-synthui-slidetoggle-design.md). Pure C++17 header-only math and types (`synthui_slide_toggle_types.h`, `synthui_slide_toggle_math.h`) for viewBox mapping ($w=100, vh=\text{round}(100 \cdot h / w)$), housing, slot/well, knob (shadow, body, top highlight, vertical ridges), and waveform glyph coordinates. Single LVGL 9 widget TU (`synthui_slide_toggle.cpp`) with early-return guards, targeted `lv_obj_invalidate_area()` slider well damage bounding box invalidation, and sub-element clipping. The consumer example runs the hardware double-buffer pipeline (`lvgl_mipi_panel_create_db()`) and executes Phase A correctness (pinned golden CRC, 64-step deterministic delta sequence, delta-equality guard, damage bounds) and Phase B (continuous animation benchmark).

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

### Task 1: `synthui_slide_toggle_types.h`, `synthui_slide_toggle_math.h` + Host Unit Test (SynthUI repo)

**Files:**
- Create: `/Users/moolet/Development/SynthUI/src/synthui_slide_toggle_types.h`
- Create: `/Users/moolet/Development/SynthUI/src/synthui_slide_toggle_math.h`
- Create: `/Users/moolet/Development/SynthUI/tests/slide_toggle_test.cpp`
- Modify: `/Users/moolet/Development/SynthUI/tests/run.sh`

**Interfaces:**
- Produces:
  - `synthui_slide_toggle_types.h`: `synthui_slide_toggle_glyph_t` (`NONE`, `SAW`, `SQUARE`, `TRI`, `PULSE`), color constants.
  - `namespace synthui::slide_toggle`: `SlideToggleGeom`, `GlyphPath`, `compute_geom()`, `get_glyph_path()`, `compute_knob_dirty_area()`.

- [ ] **Step 1: Write the failing host test**

Create `/Users/moolet/Development/SynthUI/tests/slide_toggle_test.cpp`:
```cpp
/* slide_toggle_test.cpp - host unit test for pure slide toggle geometry & math.
 * Copyright (c) 2026 Nicholas Newdigate
 * SPDX-License-Identifier: MIT */
#undef NDEBUG
#include "../src/synthui_slide_toggle_math.h"
#include <cassert>
#include <cmath>
#include <cstdio>

static bool approx_eq(float a, float b) {
    return std::fabs(a - b) < 0.05f;
}

int main() {
    using namespace synthui::slide_toggle;

    // 1. Default 2-position toggle geometry: w=150, h=52, saw/square, panel=#b9bcbc
    SlideToggleGeom g{};
    assert(compute_geom(150.0f, 52.0f, 2, 0,
                        SYNTHUI_SLIDE_TOGGLE_GLYPH_SAW,
                        SYNTHUI_SLIDE_TOGGLE_GLYPH_SQUARE,
                        SYNTHUI_SLIDE_TOGGLE_COLOR_PANEL_DEFAULT, false, g));
    assert(approx_eq(g.w, 150.0f));
    assert(approx_eq(g.h, 52.0f));
    assert(approx_eq(g.vw, 100.0f));
    assert(approx_eq(g.vh, 35.0f)); // round(100 * 52 / 150) = round(34.6667) = 35
    assert(approx_eq(g.u, 1.5f));
    assert(g.positions == 2);
    assert(g.value == 0);
    assert(!g.disabled);
    assert(g.has_left);
    assert(g.has_right);
    assert(approx_eq(g.hx, 22.0f));
    assert(approx_eq(g.hw, 56.0f)); // 78 - 22 = 56
    assert(approx_eq(g.hh, 25.2f)); // 35 * 0.72 = 25.2
    assert(approx_eq(g.hy, 4.9f));  // (35 - 25.2) / 2 = 4.9

    // Inset = max(1.6, 35 * 0.06) = max(1.6, 2.1) = 2.1
    assert(approx_eq(g.well_x, 24.1f));
    assert(approx_eq(g.well_y, 7.0f));
    assert(approx_eq(g.well_w, 51.8f)); // 56 - 4.2 = 51.8
    assert(approx_eq(g.well_h, 21.0f)); // 25.2 - 4.2 = 21.0

    // Knob W = 51.8 / 2 = 25.9, Knob H = 21.0
    assert(approx_eq(g.knob_w, 25.9f));
    assert(approx_eq(g.knob_h, 21.0f));
    assert(approx_eq(g.knob_x, 24.1f));
    assert(approx_eq(g.knob_y, 7.0f));

    // Glyph paths
    GlyphPath saw = get_glyph_path(SYNTHUI_SLIDE_TOGGLE_GLYPH_SAW);
    assert(saw.num_points == 6);
    assert(approx_eq(saw.points[0].x, 2.0f) && approx_eq(saw.points[0].y, 15.0f));
    assert(approx_eq(saw.points[1].x, 7.0f) && approx_eq(saw.points[1].y, 6.0f));
    assert(approx_eq(saw.points[5].x, 17.0f) && approx_eq(saw.points[5].y, 6.0f));

    GlyphPath square = get_glyph_path(SYNTHUI_SLIDE_TOGGLE_GLYPH_SQUARE);
    assert(square.num_points == 8);

    GlyphPath tri = get_glyph_path(SYNTHUI_SLIDE_TOGGLE_GLYPH_TRI);
    assert(tri.num_points == 5);

    GlyphPath pulse = get_glyph_path(SYNTHUI_SLIDE_TOGGLE_GLYPH_PULSE);
    assert(pulse.num_points == 9);

    GlyphPath none = get_glyph_path(SYNTHUI_SLIDE_TOGGLE_GLYPH_NONE);
    assert(none.num_points == 0);

    // Dirty area
    int32_t dx1, dy1, dx2, dy2;
    compute_knob_dirty_area(g, 100, 200, dx1, dy1, dx2, dy2);
    int32_t dmg_w = dx2 - dx1 + 1;
    int32_t dmg_h = dy2 - dy1 + 1;
    assert(dmg_w > 0 && dmg_h > 0);
    assert(dmg_w * dmg_h <= 15000);

    printf("PASS: synthui_slide_toggle host unit tests\n");
    return 0;
}
```

- [ ] **Step 2: Run test to verify it fails**

Run:
```bash
c++ -std=c++17 -Wall -Wextra -Werror -I/Users/moolet/Development/SynthUI/src /Users/moolet/Development/SynthUI/tests/slide_toggle_test.cpp -o /tmp/slide_toggle_test
```
Expected: FAIL (`synthui_slide_toggle_math.h: No such file or directory`).

- [ ] **Step 3: Implement `synthui_slide_toggle_types.h` and `synthui_slide_toggle_math.h`**

Create `/Users/moolet/Development/SynthUI/src/synthui_slide_toggle_types.h` and `/Users/moolet/Development/SynthUI/src/synthui_slide_toggle_math.h` with clean-room math and geometry calculations matching the reference specification.

- [ ] **Step 4: Run test to verify it passes and update `tests/run.sh`**

Compile and run test, verify output `PASS: synthui_slide_toggle host unit tests`.
Update `/Users/moolet/Development/SynthUI/tests/run.sh` to include `slide_toggle_test`.
Run `sh tests/run.sh` in SynthUI directory.

- [ ] **Step 5: Commit in SynthUI**

```bash
cd /Users/moolet/Development/SynthUI
git add src/synthui_slide_toggle_types.h src/synthui_slide_toggle_math.h tests/slide_toggle_test.cpp tests/run.sh
git commit -m "synthui_slide_toggle: C++ geometry math, types, and host unit tests (NEW-30)"
```

---

### Task 2: LVGL 9 Custom Widget Implementation (`src/synthui_slide_toggle.h`, `.cpp`)

**Files:**
- Create: `/Users/moolet/Development/SynthUI/src/synthui_slide_toggle.h`
- Create: `/Users/moolet/Development/SynthUI/src/synthui_slide_toggle.cpp`

**Interfaces:**
- Produces:
  - C API: `synthui_slide_toggle_create()`, `synthui_slide_toggle_set_value()`, `get_value()`, `set_positions()`, `get_positions()`, `set_left_glyph()`, `get_left_glyph()`, `set_right_glyph()`, `get_right_glyph()`, `set_panel_color()`, `get_panel_color()`, `set_disabled()`, `get_disabled()`.
  - C++ API: `synthui::SlideToggle` wrapper class.
  - Class definition: `synthui_slide_toggle_class`.

- [ ] **Step 1: Write `src/synthui_slide_toggle.h`**

Include standard LVGL 9 headers, declare C API functions, and provide the C++ `synthui::SlideToggle` class wrapper.

- [ ] **Step 2: Implement `src/synthui_slide_toggle.cpp`**

Implement `synthui_slide_toggle_class`, constructor, event handler (`LV_EVENT_DRAW_MAIN`, `LV_EVENT_CLICKED`), and `slide_toggle_draw()`.
Ensure:
1. Early returns on all property setters if value is unchanged.
2. `synthui_slide_toggle_set_value()` computes dirty area via `compute_knob_dirty_area()` and calls `lv_obj_invalidate_area()`.
3. Drawing routine checks `layer->_clip_area`: skips left/right glyph rasterization if outside `_clip_area`.
4. Renders panel background, housing, well, knob shadow, knob body, top highlight, and vertical ridges.

- [ ] **Step 3: Run host test suite**

Run `/Users/moolet/Development/SynthUI/tests/run.sh`.

- [ ] **Step 4: Commit in SynthUI**

```bash
cd /Users/moolet/Development/SynthUI
git add src/synthui_slide_toggle.h src/synthui_slide_toggle.cpp
git commit -m "synthui_slide_toggle: LVGL 9 custom widget with delta damage invalidation (NEW-30)"
```

---

### Task 3: EVKB Display Example Scaffolding & Scene (`rt1176-evkb`)

**Files:**
- Create: `examples/display/synthui_slide_toggle_test/CMakeLists.txt`
- Create: `examples/display/synthui_slide_toggle_test/synthui_slide_toggle_test.cpp`

**Scene & Pipeline Requirements:**
- Panel: $720 \times 1280$ RK055 panel.
- Double-buffered hardware pipeline: `lvgl_mipi_panel_create_db(Display)`.
- Multi-state gallery scene displaying:
  - 2-position toggles with waveform pairs (saw/square, tri/pulse, none/saw).
  - 3-position and 4-position toggles.
  - Panel color variations (`#b9bcbc`, `#6d7a85`, `#39434b`, `#d6d4cf`).
  - Disabled states.
- Support `SLIDE_TOGGLE_EYEBALL_HOLD` compile definition.
- Phase A1: Full initial render, print `slide_toggle_crc=0xXXXXXXXX`.
- Phase A2: 64-step deterministic delta sequence cycling positions.
- Guard assertions:
  - `delta_crc == fresh_crc` (`slide_toggle_delta_eq=PASS`).
  - `slide_toggle_damage max <= 15000`.
  - VSYNC health: `flips > 0`, `isrs > 0`, `timeouts = 0`.
  - Completion token: `crc_done`.
- Phase B: Continuous animation benchmark reporting `slide_toggle_fps`.

- [ ] **Step 1: Write `CMakeLists.txt`**

Create `examples/display/synthui_slide_toggle_test/CMakeLists.txt`.

- [ ] **Step 2: Implement `synthui_slide_toggle_test.cpp`**

Implement full hardware double-buffered pipeline and gallery scene.

- [ ] **Step 3: Build the example ELF**

```bash
mkdir -p /Users/moolet/Development/rt1170/rt1176-evkb/examples/display/synthui_slide_toggle_test/build
cd /Users/moolet/Development/rt1170/rt1176-evkb/examples/display/synthui_slide_toggle_test/build
cmake ..
cmake --build . -j8
```
Verify `synthui_slide_toggle_test.elf` compiles with zero warnings or errors.

---

### Task 4: QEMU Gate Runner & CI Verification

**Files:**
- Create: `examples/display/synthui_slide_toggle_test/run_qemu.sh`
- Modify: `evkb.cmake` (update SynthUI commit SHA at line 131)

- [ ] **Step 1: Create `run_qemu.sh`**

Implement gate script sourcing `tools/gate-lib.sh`, checking `PANEL_OK`, CRC, delta equality, damage bounds, and VSYNC health.

- [ ] **Step 2: Run QEMU gate to record golden CRC**

Run `./run_qemu.sh` consecutively to confirm deterministic bit-identical CRC.
Pin the golden CRC in `run_qemu.sh`.

- [ ] **Step 3: Demonstrate tripwire RED**

Temporarily edit `run_qemu.sh` to require `slide_toggle_crc=0xDEADBEEF`, run `./run_qemu.sh`, verify failure exit code 1.
Restore the valid pinned CRC, verify green exit code 0.

- [ ] **Step 4: Bump SynthUI commit pin in `evkb.cmake`**

Get HEAD SHA from `/Users/moolet/Development/SynthUI`.
Update line 131 of `evkb.cmake`.

- [ ] **Step 5: Commit in rt1176-evkb**

```bash
cd /Users/moolet/Development/rt1170/rt1176-evkb
git add evkb.cmake examples/display/synthui_slide_toggle_test
git commit -m "synthui_slide_toggle_test: QEMU gate verified green, golden CRC, and pin bump (NEW-30)"
```

---

### Task 5: Framebuffer Eyeball Capture, Visual Verification & Linear Ticket Completion

- [ ] **Step 1: Build eyeball hold ELF and dump scanout framebuffer**

Build with `-DSLIDE_TOGGLE_EYEBALL_HOLD=1`.
Launch QEMU with monitor socket.
Read LCDIFv2 layer 0 address (`0x4080820c`).
Execute `pmemsave` to dump 3686400 bytes.
Verify host-side FNV-1a matches the golden CRC.

- [ ] **Step 2: Convert framebuffer to PNG and inspect**

Use Python PIL to convert BGRX raw framebuffer to PNG.
Inspect widgets, glyphs, ridged knobs, and panel colors.

- [ ] **Step 3: Write walkthrough artifact**

Create `/Users/moolet/.gemini/antigravity/brain/82af6c6a-3f12-4646-bb4e-b990939acc6f/walkthrough.md`.

- [ ] **Step 4: Update Linear issue NEW-30**

Post completion comment to Linear issue NEW-30 with verification results, golden CRC, and benchmark FPS.
