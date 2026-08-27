# RotaryKnob Render-Strategy Bench Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Build `examples/display/rotary_knob_bench` — one ELF that renders the RotaryKnob design through 12 cells ({vector, bitmap, strip} × {sw, gpu} × {notch, facet}), gates the sw half in QEMU with pinned checksums, and measures all 12 on silicon under the FPSBENCH workload (16-knob grid, ≥30 fps criterion).

**Architecture:** LVGL stays a pure SOFTWARE build (`import_evkb_lvgl()`, `LV_USE_DRAW_VG_LITE=0`) and the VGLite driver is linked as a plain library for DIRECT `vg_lite_*` calls — that combination is what makes one ELF safe everywhere (LVGL's VG_LITE draw unit has NO runtime fallback; direct calls guarded by the chip-ID probe do, per `vglite_probe`). sw cells draw the rotor in the widget's `LV_EVENT_DRAW_MAIN`; gpu cells draw the rotor in a post-`LV_EVENT_REFR_READY` GPU pass inside the timed frame (draw-callback-time direct GPU calls would be overpainted by LVGL's deferred sw tasks). A state machine driven from `loop()` sequences Phase A (one canonical frame per cell → FNV checksum) then Phase B (64 timed refreshes per cell).

**Tech Stack:** LVGL 9.4 (vendored, sw renderer, XRGB8888 direct mode on RK055 720×1280), VGLite v7 (GC355), MipiDisplay/PXP, gate-lib.sh + qrun, FNV-1a via `lvgl_sum_*`.

**Spec:** `docs/superpowers/specs/2026-08-27-rotary-knob-bench-design.md`. Tracking: NEW-20 (related NEW-12).

**Key file references** (read these before the task that needs them):
- `examples/display/vglite_lvgl_test/vglite_lvgl_test.cpp` — FPSBENCH method, GPU probe order, grid layout, the no-runtime-fallback account.
- `examples/display/vglite_probe/vglite_probe.cpp` — `vg_lite_init_mem` → `vg_lite_hal_probe_chip_id` → `vg_lite_init` → `vg_lite_map(USER_MEMORY)`; ABGR color trap; VLC opcodes; EXTMEM pool siting.
- `~/Development/SynthUI/src/synthui_knob.cpp` — `polar()`, `draw_arc_seg()` negative-angle fold, `draw_disc()`.
- `examples/display/synthui_knob_test/run_qemu.sh` — golden-checksum gate shape.
- `examples/networking/m2_uap_probe/run_qemu_uap.sh` — token-poll wait loop.
- `tools/gate-vacuity.test.sh` — `run_gate` fixture harness.

---

## File Structure

```
examples/display/rotary_knob_bench/
  CMakeLists.txt          # LVGL(sw) + VGLite(direct) + MipiDisplay/PXP
  rk_geometry.h           # RotaryKnob design geometry: palette, variants,
  rk_geometry.cpp         #   sw draw (well/rotor), canvas rotor render,
                          #   premultiply, vg_lite path build (arc→cubic)
  rotary_knob_bench.cpp   # bench harness: GPU probe, bench widget, cell
                          #   table, Phase A/B state machine, output
  run_qemu.sh             # the gate (Task 7)
  transcript_qemu.txt     # fixture, captured after the gate passes (Task 7)
  transcript_hw_evkb.txt  # silicon record (Task 10)
Modified:
  tools/gate-vacuity.test.sh   # green + 2 mutation cases (Task 8)
  tools/license-audit.sh       # GATES entry (Task 9)
  CLAUDE.md                    # sweep 121→122 narrative (Task 9)
  docs/superpowers/specs/2026-08-27-rotary-knob-bench-design.md  # Task 1 + §13
```

Responsibilities: `rk_geometry` knows the DESIGN (what the knob looks like) and every way to express it (LVGL sw ops, ARGB pixels, vg_lite paths); `rotary_knob_bench.cpp` knows the BENCH (cells, sequencing, measurement, output). Nothing in SynthUI/LVGL/VGLite changes; no `evkb.cmake` pin moves.

Shared constants (defined once in `rk_geometry.h`, used everywhere):

```c
#define RKB_KNOB_PX   150               /* grid parity with vglite_lvgl_test */
#define RKB_KNOB_S    (RKB_KNOB_PX / 100.0f)   /* viewBox 0..100 scale = 1.5 */
#define RKB_STRIP_N   64                /* filmstrip steps: 5.625° */
#define RKB_STEP_DEG  (360.0f / RKB_STRIP_N)
#define RKB_CANON_DEG 45.0f             /* Phase A angle = 8 strip steps */
```

---

### Task 1: Spec amendments discovered during planning

**Files:**
- Modify: `docs/superpowers/specs/2026-08-27-rotary-knob-bench-design.md`

Three facts surfaced while grounding the plan in the codebase; the spec must match the build before code exists.

- [ ] **Step 1: Amend §6 sizes for grid parity.** The spec said knob 120 px; `vglite_lvgl_test`'s grid — which §1/§6 promise workload parity with — uses **150×150 knobs at (15+c·175, 120+r·175)**. Replace the §6 bullet list's first two items with:

```markdown
- Knob size **150×150** (grid parity with `vglite_lvgl_test`: positions
  15+c·175, 120+r·175). Rotor bitmap: ARGB8888, 150×150 = 90,000 B.
- Filmstrip: 64 × 90,000 B = 5,760,000 B ≈ 5.49 MB — **SDRAM** (EXTMEM), one
  static arena rebuilt at each strip cell's init (so `init_us` carries the
  honest per-cell cost); exact bytes reported in `rotor_bytes=`.
```

- [ ] **Step 2: Amend §3's gpu-cell mechanism note.** In the §3 "vector/gpu" bullet, replace "via the sanctioned `#include <vg_lite.h>` hook from the widget draw callback" with:

```markdown
via direct `vg_lite_*` calls in a post-`LV_EVENT_REFR_READY` GPU pass inside
the timed frame (LVGL source untouched). The pass runs after LVGL's sw tasks
because a direct GPU call issued inside `LV_EVENT_DRAW_MAIN` executes
immediately, while LVGL's own deferred sw draw tasks (background, wells)
execute later in the refresh and would overpaint the rotor. For gpu cells the
widget draw callback paints only the well.
```

- [ ] **Step 3: Amend §4/§5/§7 tokens.** In §4, change "CRCs the grid region of the framebuffer" to "checksums the whole framebuffer (FNV-1a via `lvgl_sum_*`, the tree's golden arithmetic — the token stays `crc=`)". In §5, note the strategy tokens are exactly `vector`, `bitmap`, `strip`.

- [ ] **Step 4: Commit**

```bash
cd ~/Development/rt1170/evkb
git add docs/superpowers/specs/2026-08-27-rotary-knob-bench-design.md
git commit -m "docs: rotary-knob-bench spec amendments from planning (150px grid parity, post-REFR_READY gpu pass, FNV tokens)"
```

---

### Task 2: Example scaffold — panel up, GPU probed honestly, one ELF boots in QEMU

**Files:**
- Create: `examples/display/rotary_knob_bench/CMakeLists.txt`
- Create: `examples/display/rotary_knob_bench/rotary_knob_bench.cpp` (skeleton)

- [ ] **Step 1: Write `CMakeLists.txt`**

```cmake
cmake_minimum_required(VERSION 3.24)
project(rotary_knob_bench)

# XRGB8888 like every RK055 LVGL example. Directory scope so the definitions
# reach the LVGL and MipiDisplay objects, not just this target.
add_compile_definitions(LV_COLOR_DEPTH=32 PANEL_BYTES_PER_PIXEL=4)

set(TEENSY_VERSION 117 CACHE STRING "")

include(${CMAKE_CURRENT_LIST_DIR}/../../../evkb.cmake)

# ★ ONE ELF, DELIBERATELY -- and it is safe where vglite_lvgl_test's single
# binary was not. That example compiles LVGL's VG_LITE DRAW UNIT, which
# registers unconditionally in lv_init() and claims tasks it cannot execute on
# absent hardware (black screen, every liveness check green) -- hence its
# build-time split. THIS example never enables LV_USE_DRAW_VG_LITE: LVGL is
# always the software renderer, and the GPU is reached only by DIRECT
# vg_lite_* calls guarded by the chip-ID probe (vglite_probe's pattern, one
# binary two outcomes). The bench needs both engines in one boot -- that is
# what makes the A/B controlled.
import_evkb_lvgl()
import_evkb_vglite()
import_evkb_library(MipiDisplay soc panels/rk055)
import_evkb_library(PXP)    # Display::fillScreen() paints via the PXP

evkb_library_dir(LVGL _lvgl_dir)

teensy_add_executable(rotary_knob_bench
    rotary_knob_bench.cpp
    rk_geometry.cpp
    ${_lvgl_dir}/port/lvgl_mipi_panel.cpp)
teensy_target_link_libraries(rotary_knob_bench cores MipiDisplay PXP)

# LVGL and VGLite are plain CMake static-lib targets;
# teensy_target_link_libraries() would rewrite the names and lose their PUBLIC
# include dirs. LVGL is named directly because this target compiles
# port/lvgl_mipi_panel.cpp itself.
target_link_libraries(rotary_knob_bench.elf LVGL VGLite stdc++)
```

Note: `rk_geometry.cpp` does not exist until Task 3 — create it as an empty placeholder now so the build configures:

```bash
cd ~/Development/rt1170/evkb/examples/display/rotary_knob_bench
printf '/* rk_geometry.cpp - filled in by Task 3 */\n' > rk_geometry.cpp
printf '/* rk_geometry.h - filled in by Task 3 */\n#ifndef RK_GEOMETRY_H\n#define RK_GEOMETRY_H\n#endif\n' > rk_geometry.h
```

- [ ] **Step 2: Write the skeleton `rotary_knob_bench.cpp`**

```cpp
/* rotary_knob_bench - RotaryKnob render-strategy bench (12 cells).
 * Copyright (c) 2026 Nicholas Newdigate
 * SPDX-License-Identifier: MIT
 *
 * Spec: docs/superpowers/specs/2026-08-27-rotary-knob-bench-design.md
 *
 * ONE ELF, BOTH ENGINES. LVGL is ALWAYS the software renderer here
 * (LV_USE_DRAW_VG_LITE stays 0 -- see CMakeLists for why that is what makes a
 * single binary safe); the GC355 is reached only by direct vg_lite_* calls
 * guarded by the chip-ID probe. QEMU has no GC355, so there every gpu cell
 * reports st=gpu-absent -- asserted by the gate, never a silent skip.
 */
#include <Arduino.h>
#include <string.h>
#include "Display.h"
#include "lvgl_rt1176.h"
#include "lvgl_mipi_panel.h"
#include "rk_geometry.h"

extern "C" {
#include "vg_lite.h"
#include "vg_lite_platform.h"
}

/* Same pool siting and reasoning as vglite_probe: EXTMEM (SDRAM), not DMAMEM
 * -- a 2 MB pool overflows the 512K OCRAM at link time, and the GPU reaches
 * SDRAM as a bus master exactly as it reaches the framebuffer. */
#define VGLITE_POOL_BYTES (2u * 1024u * 1024u)
EXTMEM __attribute__((aligned(64))) static uint8_t vglite_pool[VGLITE_POOL_BYTES];
#define TESS_W 256
#define TESS_H 256

static bool s_gpu = false;
static vg_lite_buffer_t s_target;     /* the panel framebuffer, mapped */

void setup()
{
    Serial1.begin(115200);
    delay(200);
    Serial1.println("rotary_knob_bench up");

    const bool ok = Display.begin();
    Serial1.println(ok ? "PANEL_OK" : "PANEL_FAIL");
    if (!ok) { Serial1.println("rkb_fatal=panel"); return; }
    Display.fillScreen(0x0000);

    /* ★ ASK BEFORE COMMITTING (vglite_probe): vg_lite_init() SPINS on absent
     * hardware, so the chip-ID read is what makes the absent case a clean
     * negative instead of a hang. */
    vg_lite_init_mem(VGLITE_RT1176_REGISTER_BASE, 0u, vglite_pool, VGLITE_POOL_BYTES);
    const uint32_t chip_id = vg_lite_hal_probe_chip_id();
    if (chip_id != 0u && vg_lite_init(TESS_W, TESS_H) == VG_LITE_SUCCESS) {
        memset(&s_target, 0, sizeof(s_target));
        s_target.width   = Display.width();
        s_target.height  = Display.height();
        s_target.stride  = Display.width() * PANEL_BYTES_PER_PIXEL;
        s_target.tiled   = VG_LITE_LINEAR;
        s_target.format  = VG_LITE_BGRA8888;   /* = panel XRGB8888 memory order */
        s_target.memory  = (void *)Display.framebuffer();
        s_target.address = (uint32_t)(uintptr_t)Display.framebuffer();
        /* ★ REGISTER the framebuffer with the driver or every draw "succeeds"
         * and changes nothing (vglite_probe, measured on silicon). */
        s_gpu = (vg_lite_map(&s_target, VG_LITE_MAP_USER_MEMORY, 0) == VG_LITE_SUCCESS);
    }
    Serial1.printf("gpu=%s chip_id=0x%08lX\n", s_gpu ? "present" : "absent",
                   (unsigned long)chip_id);

    lvgl_rt1176_begin();
    lvgl_mipi_panel_create(Display);
    /* State machine armed in Task 4; nothing more yet. */
}

void loop()
{
    lvgl_rt1176_loop();
}
```

- [ ] **Step 3: Build**

```bash
cd ~/Development/rt1170/evkb/examples/display/rotary_knob_bench
cmake -B build -DCMAKE_TOOLCHAIN_FILE=../../../toolchain/rt1170-evkb.toolchain.cmake
cmake --build build
```

Expected: `rotary_knob_bench.elf` produced, no warnings from our files.

- [ ] **Step 4: Boot it in QEMU manually**

```bash
cd ~/Development/rt1170/evkb/examples/display/rotary_knob_bench
../../../tools/qrun qemu-system-arm -M mimxrt1170-evk,boot-xip=on -kernel build/rotary_knob_bench.elf \
    -display none -serial file:build/scaffold.uart -d guest_errors -D build/scaffold.dbg &
sleep 20; pkill -f rotary_knob_bench.elf || true
cat build/scaffold.uart
```

Expected: `rotary_knob_bench up`, `PANEL_OK`, `gpu=absent chip_id=0x00000000`. (If the `-M` invocation differs from what `gate_qemu_machine` emits, copy the exact flags another display gate uses — `tools/gate-lib.sh` is the authority.)

- [ ] **Step 5: Commit**

```bash
cd ~/Development/rt1170/evkb
git add examples/display/rotary_knob_bench
git commit -m "rotary_knob_bench: scaffold -- panel + honest GPU probe, one ELF"
```

---

### Task 3: Geometry module + vector/sw cells through Phase A

**Files:**
- Create (replace placeholders): `examples/display/rotary_knob_bench/rk_geometry.h`, `rk_geometry.cpp`
- Modify: `examples/display/rotary_knob_bench/rotary_knob_bench.cpp`

- [ ] **Step 1: Write `rk_geometry.h`**

```c
/* rk_geometry.h - RotaryKnob design geometry (RotaryKnob.dc.html, light/idle).
 * Copyright (c) 2026 Nicholas Newdigate
 * SPDX-License-Identifier: MIT
 *
 * Single source of truth for the design: the LVGL-sw expression renders the
 * screen for sw cells AND paints the rotor bitmaps/filmstrips the bitmap and
 * strip cells (both engines) consume; the vg_lite expression is the cached
 * path set the vector/gpu cell draws. Convention throughout is the DC file's:
 * 0 deg = 12 o'clock, clockwise positive, viewBox 0..100.
 */
#ifndef RK_GEOMETRY_H
#define RK_GEOMETRY_H

#include <lvgl.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#include "vg_lite.h"

#define RKB_KNOB_PX   150
#define RKB_KNOB_S    (RKB_KNOB_PX / 100.0f)
#define RKB_STRIP_N   64
#define RKB_STEP_DEG  (360.0f / RKB_STRIP_N)
#define RKB_CANON_DEG 45.0f

typedef enum { RKG_NOTCH = 0, RKG_FACET } rkg_variant_t;

/* LVGL-sw expression, screen coords. cx/cy = knob centre, S = px per viewBox
 * unit, th = rotor angle in degrees. */
void rkg_draw_well_sw(lv_layer_t *layer, float cx, float cy, float S);
void rkg_draw_rotor_sw(lv_layer_t *layer, rkg_variant_t v,
                       float cx, float cy, float S, float th);

/* Render the rotor alone (transparent background) into a side*side
 * ARGB8888 buffer via an LVGL canvas. */
void rkg_render_rotor_argb(rkg_variant_t v, uint32_t *buf, int side, float th);

/* Straight-alpha ARGB8888 -> premultiplied, in place (gpu blit sources). */
void rkg_premultiply(uint32_t *buf, size_t npx);

/* vg_lite expression: build the rotor's cached paths in CENTRED viewBox
 * units x16 (S32 coords, 1/16-unit precision; pair with a matrix scale of
 * S/16). Returns the path count; fills paths[] and colors_abgr[].
 * out_bytes gets the total path-data byte count. */
#define RKG_VG_MAX_PATHS 9
int rkg_build_vg_paths(rkg_variant_t v, vg_lite_path_t *paths,
                       uint32_t *colors_abgr, size_t *out_bytes);

#ifdef __cplusplus
}
#endif
#endif
```

- [ ] **Step 2: Write `rk_geometry.cpp`**

```c
/* rk_geometry.cpp - see rk_geometry.h.
 * Copyright (c) 2026 Nicholas Newdigate
 * SPDX-License-Identifier: MIT */
#include "rk_geometry.h"
#include <math.h>
#include <string.h>

#define RKG_DEG (3.14159265358979f / 180.0f)

/* ---- palette: RotaryKnob.dc.html THEME.light, state idle ---- */
#define RKG_WELL        0xdcdce6
#define RKG_WELL_STROKE 0xb6b8cc
#define RKG_BODY        0x282b60
#define RKG_INNER       0x333871
#define RKG_INDEX       0xfcfbf6
static const uint32_t RKG_TONES[8] = { 0x5b61a8, 0x4a4f92, 0x3a3f7d, 0x333871,
                                       0x3a3f7d, 0x4a4f92, 0x5b61a8, 0x6a70b8 };

/* P(r, th): 0 deg = 12 o'clock, clockwise -- the DC convention (and
 * synthui_knob's polar()). */
static void rkg_polar(float cx, float cy, float S, float r, float deg,
                      float *x, float *y)
{
    *x = cx + r * S * sinf(deg * RKG_DEG);
    *y = cy - r * S * cosf(deg * RKG_DEG);
}

static void draw_disc(lv_layer_t *l, float x, float y, float rpx, uint32_t hex)
{
    lv_draw_rect_dsc_t d; lv_draw_rect_dsc_init(&d);
    d.radius = LV_RADIUS_CIRCLE;
    d.bg_color = lv_color_hex(hex); d.bg_opa = LV_OPA_COVER;
    lv_area_t a = { (int32_t)lroundf(x - rpx), (int32_t)lroundf(y - rpx),
                    (int32_t)lroundf(x + rpx), (int32_t)lroundf(y + rpx) };
    lv_draw_rect(l, &d, &a);
}

/* ring(r0, r1, a1, a2) as an LVGL arc: radius names the OUTER edge, width
 * extends inward. The fold below is synthui_knob's draw_arc_seg lesson --
 * LVGL's sw arc clamps a negative start to 0 and renders a truncated wedge,
 * so fold the start into [0,360) and carry the span. */
static void draw_ring_sector(lv_layer_t *l, float cx, float cy, float S,
                             float r0, float r1, float a1, float a2,
                             uint32_t hex)
{
    lv_draw_arc_dsc_t a; lv_draw_arc_dsc_init(&a);
    a.center.x = (int32_t)lroundf(cx); a.center.y = (int32_t)lroundf(cy);
    a.radius = (uint16_t)lroundf(r1 * S);
    a.width  = (int32_t)lroundf((r1 - r0) * S); if (a.width < 1) a.width = 1;
    float span = a2 - a1;
    if (span <= 0.0f) return;
    if (span > 360.0f) span = 360.0f;
    float s0 = fmodf(a1 - 90.0f, 360.0f);   /* LVGL measures from 3 o'clock */
    if (s0 < 0.0f) s0 += 360.0f;
    a.start_angle = (lv_value_precise_t)s0;
    a.end_angle   = (lv_value_precise_t)(s0 + span);
    a.color = lv_color_hex(hex); a.opa = LV_OPA_COVER;
    lv_draw_arc(l, &a);
}

void rkg_draw_well_sw(lv_layer_t *l, float cx, float cy, float S)
{
    /* SVG: circle r39 fill + centred stroke w1.6. LVGL's border sits inside
     * the radius rather than straddling it -- a half-stroke-width difference
     * the per-cell goldens absorb (every cell draws the well identically). */
    lv_draw_rect_dsc_t d; lv_draw_rect_dsc_init(&d);
    d.radius = LV_RADIUS_CIRCLE;
    d.bg_color = lv_color_hex(RKG_WELL); d.bg_opa = LV_OPA_COVER;
    d.border_color = lv_color_hex(RKG_WELL_STROKE); d.border_opa = LV_OPA_COVER;
    d.border_width = (int32_t)lroundf(1.6f * S);
    if (d.border_width < 1) d.border_width = 1;
    const float r = 39.0f * S;
    lv_area_t a = { (int32_t)lroundf(cx - r), (int32_t)lroundf(cy - r),
                    (int32_t)lroundf(cx + r), (int32_t)lroundf(cy + r) };
    lv_draw_rect(l, &d, &a);
}

void rkg_draw_rotor_sw(lv_layer_t *l, rkg_variant_t v,
                       float cx, float cy, float S, float th)
{
    if (v == RKG_NOTCH) {
        draw_disc(l, cx, cy, 36.0f * S, RKG_BODY);
        draw_disc(l, cx, cy, 27.0f * S, RKG_INNER);
        draw_ring_sector(l, cx, cy, S, 16.0f, 36.0f, th - 8.0f, th + 8.0f,
                         RKG_INDEX);
    } else { /* RKG_FACET: 8 triangles + index sector, all rotated by th */
        for (int i = 0; i < 8; i++) {
            const float a1 = (float)i * 45.0f + 22.5f + th;
            lv_draw_triangle_dsc_t t; lv_draw_triangle_dsc_init(&t);
            t.color = lv_color_hex(RKG_TONES[i]); t.opa = LV_OPA_COVER;
            float x, y;
            t.p[0].x = (lv_value_precise_t)lroundf(cx);
            t.p[0].y = (lv_value_precise_t)lroundf(cy);
            rkg_polar(cx, cy, S, 36.0f, a1, &x, &y);
            t.p[1].x = (lv_value_precise_t)lroundf(x);
            t.p[1].y = (lv_value_precise_t)lroundf(y);
            rkg_polar(cx, cy, S, 36.0f, a1 + 45.0f, &x, &y);
            t.p[2].x = (lv_value_precise_t)lroundf(x);
            t.p[2].y = (lv_value_precise_t)lroundf(y);
            lv_draw_triangle(l, &t);
        }
        draw_ring_sector(l, cx, cy, S, 20.0f, 36.0f, th - 22.5f, th + 22.5f,
                         RKG_INDEX);
    }
}

void rkg_render_rotor_argb(rkg_variant_t v, uint32_t *buf, int side, float th)
{
    memset(buf, 0, (size_t)side * (size_t)side * 4u);
    lv_obj_t *cv = lv_canvas_create(lv_screen_active());
    lv_canvas_set_buffer(cv, buf, side, side, LV_COLOR_FORMAT_ARGB8888);
    lv_layer_t layer;
    lv_canvas_init_layer(cv, &layer);
    rkg_draw_rotor_sw(&layer, v, side * 0.5f, side * 0.5f,
                      (float)side / 100.0f, th);
    lv_canvas_finish_layer(cv, &layer);   /* dispatches synchronously */
    lv_obj_delete(cv);
}

void rkg_premultiply(uint32_t *buf, size_t npx)
{
    for (size_t i = 0; i < npx; i++) {
        const uint32_t p = buf[i], a = p >> 24;
        const uint32_t r = (((p >> 16) & 0xFFu) * a) / 255u;
        const uint32_t g = (((p >> 8)  & 0xFFu) * a) / 255u;
        const uint32_t b = ((p & 0xFFu) * a) / 255u;
        buf[i] = (a << 24) | (r << 16) | (g << 8) | b;
    }
}

/* ---- vg_lite path build -------------------------------------------------- */
/* ★ vg_lite_color_t IS ABGR (vglite_probe's measured lesson): red in the LOW
 * byte. Convert once here so nobody upstream can get it backwards. */
static uint32_t abgr(uint32_t hex)
{
    return 0xFF000000u | ((hex & 0xFFu) << 16) | (hex & 0xFF00u)
           | ((hex >> 16) & 0xFFu);
}

/* S32 path data in centred viewBox units x16: integer coords keep the format
 * vglite_probe proved on this driver, x16 keeps ~0.1 px precision at S=1.5.
 * The drawing matrix carries scale(S/16). */
#define RKG_FIX 16.0f
#define RKG_VG_ARENA_WORDS 4096
static int32_t s_vg_arena[RKG_VG_ARENA_WORDS];
static size_t  s_vg_used;

static void emit(int32_t w)
{
    if (s_vg_used < RKG_VG_ARENA_WORDS) s_vg_arena[s_vg_used++] = w;
}
static int32_t fx(float f) { return (int32_t)lroundf(f * RKG_FIX); }
/* centred polar, viewBox units (no scale -- the matrix scales) */
static void cpol(float r, float deg, float *x, float *y)
{
    *x = r * sinf(deg * RKG_DEG);
    *y = -r * cosf(deg * RKG_DEG);
}

/* Emit cubics approximating the arc r, a1 -> a2 (current point must already
 * be at (r, a1)). Standard k = (4/3)tan(delta/4); a negative span flips the
 * tangent sign via tan, so the inner (reversed) arc of a ring needs no
 * special case. */
static void emit_arc(float r, float a1, float a2)
{
    const float span = a2 - a1;
    int nseg = (int)ceilf(fabsf(span) / 90.0f);
    if (nseg < 1) nseg = 1;
    const float step = span / (float)nseg;
    const float d = (4.0f / 3.0f) * tanf(step * RKG_DEG / 4.0f) * r;
    for (int i = 0; i < nseg; i++) {
        const float b1 = a1 + (float)i * step, b2 = b1 + step;
        float x1, y1, x2, y2;
        cpol(r, b1, &x1, &y1);
        cpol(r, b2, &x2, &y2);
        /* unit tangent of p(t)=(r sin t, -r cos t) is (cos t, sin t) */
        emit(VLC_OP_CUBIC);
        emit(fx(x1 + d * cosf(b1 * RKG_DEG))); emit(fx(y1 + d * sinf(b1 * RKG_DEG)));
        emit(fx(x2 - d * cosf(b2 * RKG_DEG))); emit(fx(y2 - d * sinf(b2 * RKG_DEG)));
        emit(fx(x2)); emit(fx(y2));
    }
}

static void emit_circle(float r)
{
    float x, y;
    cpol(r, 0.0f, &x, &y);
    emit(VLC_OP_MOVE); emit(fx(x)); emit(fx(y));
    emit_arc(r, 0.0f, 360.0f);
    emit(VLC_OP_CLOSE);
}

static void emit_ring(float r0, float r1, float a1, float a2)
{
    float x, y;
    cpol(r1, a1, &x, &y);
    emit(VLC_OP_MOVE); emit(fx(x)); emit(fx(y));
    emit_arc(r1, a1, a2);
    cpol(r0, a2, &x, &y);
    emit(VLC_OP_LINE); emit(fx(x)); emit(fx(y));
    emit_arc(r0, a2, a1);               /* reversed inner edge */
    emit(VLC_OP_CLOSE);
}

static void emit_tri(float r, float a1, float a2)
{
    float x, y;
    emit(VLC_OP_MOVE); emit(0); emit(0);
    cpol(r, a1, &x, &y);
    emit(VLC_OP_LINE); emit(fx(x)); emit(fx(y));
    cpol(r, a2, &x, &y);
    emit(VLC_OP_LINE); emit(fx(x)); emit(fx(y));
    emit(VLC_OP_CLOSE);
}

/* Close one path object over the arena words emitted since 'start'. */
static void finish_path(vg_lite_path_t *p, size_t start)
{
    emit(VLC_OP_END);
    memset(p, 0, sizeof(*p));
    vg_lite_init_path(p, VG_LITE_S32, VG_LITE_HIGH,
                      (uint32_t)((s_vg_used - start) * sizeof(int32_t)),
                      &s_vg_arena[start],
                      -41.0f * RKG_FIX, -41.0f * RKG_FIX,
                      41.0f * RKG_FIX, 41.0f * RKG_FIX);
}

int rkg_build_vg_paths(rkg_variant_t v, vg_lite_path_t *paths,
                       uint32_t *colors_abgr, size_t *out_bytes)
{
    s_vg_used = 0;
    int n = 0;
    size_t start;
    if (v == RKG_NOTCH) {
        start = s_vg_used; emit_circle(36.0f);
        finish_path(&paths[n], start); colors_abgr[n++] = abgr(RKG_BODY);
        start = s_vg_used; emit_circle(27.0f);
        finish_path(&paths[n], start); colors_abgr[n++] = abgr(RKG_INNER);
        start = s_vg_used; emit_ring(16.0f, 36.0f, -8.0f, 8.0f);
        finish_path(&paths[n], start); colors_abgr[n++] = abgr(RKG_INDEX);
    } else {
        for (int i = 0; i < 8; i++) {
            const float a1 = (float)i * 45.0f + 22.5f;
            start = s_vg_used; emit_tri(36.0f, a1, a1 + 45.0f);
            finish_path(&paths[n], start); colors_abgr[n++] = abgr(RKG_TONES[i]);
        }
        start = s_vg_used; emit_ring(20.0f, 36.0f, -22.5f, 22.5f);
        finish_path(&paths[n], start); colors_abgr[n++] = abgr(RKG_INDEX);
    }
    *out_bytes = s_vg_used * sizeof(int32_t);
    return n;
}
```

- [ ] **Step 3: Add the bench widget, cell table, and Phase A machine to `rotary_knob_bench.cpp`.** Replace the file's content below the `s_target` declaration (keep the includes, pool, `s_gpu`, `s_target`, and `setup()`'s panel/GPU section) with:

```cpp
/* ---- cell table ---------------------------------------------------------- */
typedef enum { RKB_VECTOR = 0, RKB_BITMAP, RKB_STRIP } rkb_strat_t;
typedef struct { rkb_strat_t strat; bool gpu; rkg_variant_t var; } rkb_cell_t;

static const rkb_cell_t CELLS[12] = {
    { RKB_VECTOR, false, RKG_NOTCH }, { RKB_VECTOR, false, RKG_FACET },
    { RKB_VECTOR, true,  RKG_NOTCH }, { RKB_VECTOR, true,  RKG_FACET },
    { RKB_BITMAP, false, RKG_NOTCH }, { RKB_BITMAP, false, RKG_FACET },
    { RKB_BITMAP, true,  RKG_NOTCH }, { RKB_BITMAP, true,  RKG_FACET },
    { RKB_STRIP,  false, RKG_NOTCH }, { RKB_STRIP,  false, RKG_FACET },
    { RKB_STRIP,  true,  RKG_NOTCH }, { RKB_STRIP,  true,  RKG_FACET },
};
static const char *STRAT_NAME[3]  = { "vector", "bitmap", "strip" };
static const char *VAR_NAME[2]    = { "notch", "facet" };
#define ENGINE_NAME(c) ((c)->gpu ? "gpu" : "sw")

/* ---- per-knob state and scene -------------------------------------------- */
typedef struct { lv_obj_t *obj; float angle; float cx, cy; } rkb_knob_t;
static rkb_knob_t g_knob[16];
static const rkb_cell_t *g_cell = NULL;      /* active cell, NULL = none */

/* ---- rotor/strip arenas (SDRAM; rebuilt at each cell's init) ------------- */
#define ROTOR_PX   (RKB_KNOB_PX * RKB_KNOB_PX)
#define ROTOR_B    (ROTOR_PX * 4)
EXTMEM __attribute__((aligned(64))) static uint32_t g_rotor[ROTOR_PX];
EXTMEM __attribute__((aligned(64))) static uint32_t g_strip[RKB_STRIP_N][ROTOR_PX];

static lv_image_dsc_t g_rotor_dsc;
static lv_image_dsc_t g_strip_dsc[RKB_STRIP_N];
static vg_lite_buffer_t g_rotor_vgbuf;
static vg_lite_buffer_t g_strip_vgbuf[RKB_STRIP_N];

/* vector/gpu cached paths */
static vg_lite_path_t g_vg_paths[RKG_VG_MAX_PATHS];
static uint32_t       g_vg_colors[RKG_VG_MAX_PATHS];
static int            g_vg_npaths = 0;

static uint32_t g_init_us = 0, g_rotor_bytes = 0;

/* ---- widget draw: LV_EVENT_DRAW_MAIN on a plain lv_obj ------------------- */
static void knob_draw_cb(lv_event_t *e)
{
    rkb_knob_t *k = (rkb_knob_t *)lv_event_get_user_data(e);
    lv_layer_t *layer = lv_event_get_layer(e);
    lv_obj_t *obj = lv_event_get_current_target_obj(e);
    lv_area_t coords; lv_obj_get_coords(obj, &coords);
    const float cx = (float)coords.x1 + RKB_KNOB_PX * 0.5f;
    const float cy = (float)coords.y1 + RKB_KNOB_PX * 0.5f;

    /* The well is common SW code in EVERY cell -- the constant addend that
     * keeps the A/B about the rotor strategy alone (spec section 3). */
    rkg_draw_well_sw(layer, cx, cy, RKB_KNOB_S);
    if (g_cell == NULL || g_cell->gpu) return;   /* gpu rotor: post-refresh pass */

    switch (g_cell->strat) {
    case RKB_VECTOR:
        rkg_draw_rotor_sw(layer, g_cell->var, cx, cy, RKB_KNOB_S, k->angle);
        break;
    case RKB_BITMAP: {
        lv_draw_image_dsc_t d; lv_draw_image_dsc_init(&d);
        d.src = &g_rotor_dsc;
        d.rotation = (int32_t)lroundf(k->angle * 10.0f);  /* 0.1 deg units */
        d.pivot.x = RKB_KNOB_PX / 2; d.pivot.y = RKB_KNOB_PX / 2;
        d.antialias = 1;
        lv_draw_image(layer, &d, &coords);
        break;
    }
    case RKB_STRIP: {
        const int idx = ((int)lroundf(k->angle / RKB_STEP_DEG))
                        & (RKB_STRIP_N - 1);
        lv_draw_image_dsc_t d; lv_draw_image_dsc_init(&d);
        d.src = &g_strip_dsc[idx];
        lv_draw_image(layer, &d, &coords);
        break;
    }
    }
}

/* ---- gpu rotor pass (post-REFR_READY, inside the timed frame) ------------ */
static void gpu_rotor_pass(void)
{
    for (int k = 0; k < 16; k++) {
        vg_lite_matrix_t m; vg_lite_identity(&m);
        vg_lite_translate(g_knob[k].cx, g_knob[k].cy, &m);
        vg_lite_rotate(g_knob[k].angle, &m);
        switch (g_cell->strat) {
        case RKB_VECTOR:
            vg_lite_scale(RKB_KNOB_S / 16.0f, RKB_KNOB_S / 16.0f, &m);
            for (int p = 0; p < g_vg_npaths; p++)
                vg_lite_draw(&s_target, &g_vg_paths[p], VG_LITE_FILL_NON_ZERO,
                             &m, VG_LITE_BLEND_SRC_OVER, g_vg_colors[p]);
            break;
        case RKB_BITMAP:
            vg_lite_translate(-(RKB_KNOB_PX * 0.5f), -(RKB_KNOB_PX * 0.5f), &m);
            vg_lite_blit(&s_target, &g_rotor_vgbuf, &m,
                         VG_LITE_BLEND_SRC_OVER, 0, VG_LITE_FILTER_BI_LINEAR);
            break;
        case RKB_STRIP: {
            const int idx = ((int)lroundf(g_knob[k].angle / RKB_STEP_DEG))
                            & (RKB_STRIP_N - 1);
            vg_lite_translate(-(RKB_KNOB_PX * 0.5f), -(RKB_KNOB_PX * 0.5f), &m);
            vg_lite_blit(&s_target, &g_strip_vgbuf[idx], &m,
                         VG_LITE_BLEND_SRC_OVER, 0, VG_LITE_FILTER_BI_LINEAR);
            break;
        }
        }
    }
    /* Retire before anyone reads the framebuffer -- checksumming earlier
     * would race the hardware (vglite_probe). */
    vg_lite_finish();
}

/* ---- cell lifecycle ------------------------------------------------------ */
static void vg_wrap_argb(vg_lite_buffer_t *b, uint32_t *px)
{
    memset(b, 0, sizeof(*b));
    b->width = RKB_KNOB_PX; b->height = RKB_KNOB_PX;
    b->stride = RKB_KNOB_PX * 4;
    b->tiled = VG_LITE_LINEAR;
    b->format = VG_LITE_BGRA8888;   /* premultiplied ARGB8888 words */
    b->memory = px;
    b->address = (uint32_t)(uintptr_t)px;
    vg_lite_map(b, VG_LITE_MAP_USER_MEMORY, 0);
}

static void cell_build_assets(const rkb_cell_t *c)
{
    const uint32_t t0 = micros();
    g_rotor_bytes = 0;
    switch (c->strat) {
    case RKB_VECTOR:
        if (c->gpu) {
            size_t bytes = 0;
            g_vg_npaths = rkg_build_vg_paths(c->var, g_vg_paths, g_vg_colors,
                                             &bytes);
            g_rotor_bytes = (uint32_t)bytes;
        }
        break;
    case RKB_BITMAP:
        rkg_render_rotor_argb(c->var, g_rotor, RKB_KNOB_PX, 0.0f);
        if (c->gpu) {
            rkg_premultiply(g_rotor, ROTOR_PX);
            vg_wrap_argb(&g_rotor_vgbuf, g_rotor);
        } else {
            memset(&g_rotor_dsc, 0, sizeof(g_rotor_dsc));
            g_rotor_dsc.header.magic = LV_IMAGE_HEADER_MAGIC;
            g_rotor_dsc.header.cf = LV_COLOR_FORMAT_ARGB8888;
            g_rotor_dsc.header.w = RKB_KNOB_PX;
            g_rotor_dsc.header.h = RKB_KNOB_PX;
            g_rotor_dsc.header.stride = RKB_KNOB_PX * 4;
            g_rotor_dsc.data_size = ROTOR_B;
            g_rotor_dsc.data = (const uint8_t *)g_rotor;
        }
        g_rotor_bytes = ROTOR_B;
        break;
    case RKB_STRIP:
        for (int i = 0; i < RKB_STRIP_N; i++) {
            rkg_render_rotor_argb(c->var, g_strip[i], RKB_KNOB_PX,
                                  (float)i * RKB_STEP_DEG);
            if (c->gpu) {
                rkg_premultiply(g_strip[i], ROTOR_PX);
                vg_wrap_argb(&g_strip_vgbuf[i], g_strip[i]);
            } else {
                memset(&g_strip_dsc[i], 0, sizeof(g_strip_dsc[i]));
                g_strip_dsc[i].header.magic = LV_IMAGE_HEADER_MAGIC;
                g_strip_dsc[i].header.cf = LV_COLOR_FORMAT_ARGB8888;
                g_strip_dsc[i].header.w = RKB_KNOB_PX;
                g_strip_dsc[i].header.h = RKB_KNOB_PX;
                g_strip_dsc[i].header.stride = RKB_KNOB_PX * 4;
                g_strip_dsc[i].data_size = ROTOR_B;
                g_strip_dsc[i].data = (const uint8_t *)g_strip[i];
            }
        }
        g_rotor_bytes = (uint32_t)ROTOR_B * RKB_STRIP_N;
        break;
    }
    g_init_us = micros() - t0;
}

static lv_obj_t *cell_build_scene(const rkb_cell_t *c, float angle)
{
    g_cell = c;
    lv_obj_t *scr = lv_obj_create(NULL);
    /* Opaque ground: every pixel defined, so the checksum means something
     * (vglite_lvgl_test). No labels -- no font dependence in the goldens. */
    lv_obj_set_style_bg_color(scr, lv_color_hex(0x101820), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, LV_PART_MAIN);
    for (int r = 0; r < 4; r++) {
        for (int col = 0; col < 4; col++) {
            const int k = r * 4 + col;
            lv_obj_t *o = lv_obj_create(scr);
            lv_obj_remove_style_all(o);
            lv_obj_set_size(o, RKB_KNOB_PX, RKB_KNOB_PX);
            lv_obj_set_pos(o, 15 + col * 175, 120 + r * 175);
            lv_obj_add_event_cb(o, knob_draw_cb, LV_EVENT_DRAW_MAIN, &g_knob[k]);
            g_knob[k].obj = o;
            g_knob[k].angle = angle;
            g_knob[k].cx = 15.0f + col * 175.0f + RKB_KNOB_PX * 0.5f;
            g_knob[k].cy = 120.0f + r * 175.0f + RKB_KNOB_PX * 0.5f;
        }
    }
    lv_obj_t *old = lv_screen_active();
    lv_screen_load(scr);
    if (old) lv_obj_delete(old);
    return scr;
}

/* ---- Phase A state machine (Phase B arrives in Task 5) ------------------- */
typedef enum { BS_IDLE = 0, BS_A_START, BS_A_WAIT, BS_DONE_A } rkb_state_t;
static rkb_state_t g_state = BS_IDLE;
static int g_ci = 0;                    /* cell index 0..11 */
static uint32_t g_ok = 0, g_absent = 0;
static volatile uint32_t g_refr_count = 0;
static uint32_t g_refr_at_start = 0;

static void refr_cb(lv_event_t *e)
{
    if (lv_event_get_code(e) == LV_EVENT_REFR_READY) {
        /* gpu rotor pass INSIDE the frame, before anyone counts it done */
        if (g_cell && g_cell->gpu && s_gpu) gpu_rotor_pass();
        g_refr_count++;
    }
}

static void cell_print_a(const rkb_cell_t *c, uint32_t crc)
{
    Serial1.printf("cell=%s/%s/%s st=ok crc=0x%08lX init_us=%lu rotor_bytes=%lu\n",
                   STRAT_NAME[c->strat], ENGINE_NAME(c), VAR_NAME[c->var],
                   (unsigned long)crc, (unsigned long)g_init_us,
                   (unsigned long)g_rotor_bytes);
}

static void bench_step(void)
{
    switch (g_state) {
    case BS_IDLE:
        break;
    case BS_A_START: {
        if (g_ci >= 12) {
            Serial1.printf("crc_done cells=12 ok=%lu gpu_absent=%lu\n",
                           (unsigned long)g_ok, (unsigned long)g_absent);
            g_state = BS_DONE_A;         /* Task 5 chains Phase B here */
            break;
        }
        const rkb_cell_t *c = &CELLS[g_ci];
        if (c->gpu && !s_gpu) {
            Serial1.printf("cell=%s/gpu/%s st=gpu-absent\n",
                           STRAT_NAME[c->strat], VAR_NAME[c->var]);
            g_absent++; g_ci++;
            break;                        /* stay in BS_A_START, next cell */
        }
        cell_build_assets(c);
        cell_build_scene(c, RKB_CANON_DEG);
        g_refr_at_start = g_refr_count;
        g_state = BS_A_WAIT;
        break;
    }
    case BS_A_WAIT:
        /* One full refresh has retired (gpu pass included, done in refr_cb)
         * and the direct-mode flush is complete. */
        if (g_refr_count > g_refr_at_start && lvgl_mipi_panel_frame_done()) {
            lvgl_sum_reset();
            lvgl_sum_feed(Display.framebuffer(), PANEL_FB_BYTES);
            cell_print_a(&CELLS[g_ci], lvgl_sum_value());
            g_ok++; g_ci++;
            g_cell = NULL;
            g_state = BS_A_START;
        }
        break;
    case BS_DONE_A:
        break;
    }
}
```

And at the end of `setup()` (after `lvgl_mipi_panel_create(Display);`), add:

```cpp
    lv_display_add_event_cb(lv_display_get_default(), refr_cb,
                            LV_EVENT_REFR_READY, NULL);
    g_state = BS_A_START;
```

And make `loop()`:

```cpp
void loop()
{
    lvgl_rt1176_loop();
    bench_step();
}
```

- [ ] **Step 4: Build and run in QEMU twice; confirm sw CRC stability**

```bash
cd ~/Development/rt1170/evkb/examples/display/rotary_knob_bench
cmake --build build
for run in 1 2; do
  ../../../tools/qrun qemu-system-arm -M mimxrt1170-evk,boot-xip=on -kernel build/rotary_knob_bench.elf \
      -display none -serial file:build/a$run.uart -d guest_errors -D build/a$run.dbg &
  sleep 60; pkill -f rotary_knob_bench.elf || true
done
grep "^cell=" build/a1.uart
diff <(grep "^cell=" build/a1.uart | sed 's/init_us=[0-9]*//') \
     <(grep "^cell=" build/a2.uart | sed 's/init_us=[0-9]*//')
```

Expected: 12 `cell=` lines per run — six `st=ok` with checksums (the code above wires ALL six sw cells) and six `st=gpu-absent`, then `crc_done cells=12 ok=6 gpu_absent=6`. The binding check of THIS task is the diff: every sw cell's checksum identical across the two runs (`init_us` excluded). Task 4 then verifies the bitmap/strip cells' content specifically (that the rotation actually happened); if one of those four misrenders here, note it and fix it in Task 4, but the vector cells must be right now.

- [ ] **Step 5: Eyeball one frame.** Optional but cheap: `tools/rt1170-qemu.sh` has no framebuffer view; skip visual QEMU checks — the CRC stability plus Task 10's silicon eyeball are the verification.

- [ ] **Step 6: Commit**

```bash
cd ~/Development/rt1170/evkb
git add examples/display/rotary_knob_bench
git commit -m "rotary_knob_bench: geometry module + vector cells through Phase A"
```

---

### Task 4: bitmap/sw and strip/sw cells

**Files:**
- Modify: `examples/display/rotary_knob_bench/rotary_knob_bench.cpp` (nothing new to write if Task 3's `cell_build_assets`/`knob_draw_cb` were entered in full — this task VERIFIES those paths and fixes what QEMU disagrees with)

- [ ] **Step 1: Build and run in QEMU twice** (same commands as Task 3 Step 4, captures `b1.uart`/`b2.uart`).

Expected now: **six** `st=ok` cells (`vector/sw/*`, `bitmap/sw/*`, `strip/sw/*`), six `st=gpu-absent`, then `crc_done cells=12 ok=6 gpu_absent=6`. The two runs' `cell=` lines identical modulo `init_us`.

- [ ] **Step 2: Sanity-check the bitmap rotation actually happened.** `bitmap/sw/notch` at 45° must NOT have the same checksum as a 0° render. Temporarily change `RKB_CANON_DEG` to `0.0f`, rebuild, run once, and confirm `bitmap/sw/notch`'s crc CHANGES vs the 45° run (an unrotated blit would make them equal — that would be the transform silently not applied). Also confirm at 0° that `bitmap/sw/notch` and `strip/sw/notch` crcs are EQUAL to each other (strip frame 0 is the same pixels as the rotor bitmap, and rotation by 0 with antialias may resample — if they differ, note why in a comment rather than forcing it; the binding assertion is the 45°-vs-0° difference). Revert `RKB_CANON_DEG` to `45.0f` and rebuild.

- [ ] **Step 3: Record the six sw checksums** from the stable runs — they become the gate goldens in Task 7. Write them down in the commit message.

- [ ] **Step 4: Commit**

```bash
cd ~/Development/rt1170/evkb
git add examples/display/rotary_knob_bench
git commit -m "rotary_knob_bench: bitmap/sw + strip/sw cells verified stable in QEMU (goldens: <paste the six cell lines>)"
```

---

### Task 5: Phase B timing pass + bench_done + heartbeat

**Files:**
- Modify: `examples/display/rotary_knob_bench/rotary_knob_bench.cpp`

- [ ] **Step 1: Add the timing machinery.** Extend the state enum and globals:

```cpp
typedef enum { BS_IDLE = 0, BS_A_START, BS_A_WAIT, BS_B_START, BS_B_RUN,
               BS_DONE } rkb_state_t;

#define RKB_TIMED_N 64
static uint32_t g_us[RKB_TIMED_N];
static volatile uint32_t g_nsamp = 0;
static volatile bool g_timing = false;
static uint32_t g_t0 = 0;
static uint32_t g_frame = 0;
static lv_timer_t *g_anim = NULL;
static uint32_t g_timed = 0, g_absent_b = 0;
```

Replace `refr_cb` with the both-phases version (FPSBENCH method: REFR_START stamps t0, REFR_READY — after the gpu pass — stamps the sample):

```cpp
static void refr_cb(lv_event_t *e)
{
    const lv_event_code_t code = lv_event_get_code(e);
    if (code == LV_EVENT_REFR_START) {
        g_t0 = micros();
    } else if (code == LV_EVENT_REFR_READY) {
        if (g_cell && g_cell->gpu && s_gpu) gpu_rotor_pass();
        g_refr_count++;
        if (g_timing && g_nsamp < RKB_TIMED_N)
            g_us[g_nsamp++] = micros() - g_t0;
    }
}
```

(Also register `LV_EVENT_REFR_START` in `setup()` next to the READY registration.)

Add the animation timer callback — the spec's angle sequence, all multiples of the strip step so every cell does identical per-frame work (bitmap/sw rounds to 0.1°, a comment-worthy nit, not a defect):

```cpp
static void anim_cb(lv_timer_t *t)
{
    (void)t;
    g_frame++;
    for (int k = 0; k < 16; k++) {
        g_knob[k].angle = fmodf((float)g_frame * RKB_STEP_DEG
                                + (float)k * 22.5f, 360.0f);
        lv_obj_invalidate(g_knob[k].obj);
    }
}
```

Add the Phase B states to `bench_step()` (and change `BS_DONE_A` handling: when Phase A finishes, set `g_ci = 0; g_state = BS_B_START;` instead of parking):

```cpp
    case BS_B_START: {
        if (g_ci >= 12) {
            Serial1.printf("bench_done cells=12 timed=%lu gpu_absent=%lu\n",
                           (unsigned long)g_timed, (unsigned long)g_absent_b);
            g_state = BS_DONE;
            break;
        }
        const rkb_cell_t *c = &CELLS[g_ci];
        if (c->gpu && !s_gpu) {
            Serial1.printf("time=%s/gpu/%s st=gpu-absent\n",
                           STRAT_NAME[c->strat], VAR_NAME[c->var]);
            g_absent_b++; g_ci++;
            break;
        }
        cell_build_assets(c);
        cell_build_scene(c, 0.0f);
        g_frame = 0; g_nsamp = 0; g_timing = true;
        g_anim = lv_timer_create(anim_cb, 15, NULL);
        g_state = BS_B_RUN;
        break;
    }
    case BS_B_RUN:
        if (g_nsamp >= RKB_TIMED_N) {
            g_timing = false;
            lv_timer_delete(g_anim); g_anim = NULL;
            /* median + mean over the 64 samples (insertion sort, n=64) */
            uint32_t s[RKB_TIMED_N]; uint64_t sum = 0;
            for (int i = 0; i < RKB_TIMED_N; i++) { s[i] = g_us[i]; sum += g_us[i]; }
            for (int i = 1; i < RKB_TIMED_N; i++) {
                uint32_t v = s[i]; int j = i - 1;
                while (j >= 0 && s[j] > v) { s[j + 1] = s[j]; j--; }
                s[j + 1] = v;
            }
            const uint32_t us_med = s[RKB_TIMED_N / 2];
            const uint32_t mfps = (uint32_t)(1000000000ull / (us_med ? us_med : 1));
            const rkb_cell_t *c = &CELLS[g_ci];
            Serial1.printf("time=%s/%s/%s frames=%u mfps_med=%lu us_med=%lu us_mean=%lu\n",
                           STRAT_NAME[c->strat], ENGINE_NAME(c), VAR_NAME[c->var],
                           (unsigned)RKB_TIMED_N, (unsigned long)mfps,
                           (unsigned long)us_med,
                           (unsigned long)(sum / RKB_TIMED_N));
            g_timed++; g_ci++;
            g_cell = NULL;
            g_state = BS_B_START;
        }
        break;
    case BS_DONE:
        break;
```

Add a heartbeat to `loop()`:

```cpp
void loop()
{
    lvgl_rt1176_loop();
    bench_step();
    static uint32_t hb_last = 0, hb_n = 0;
    if (g_state == BS_DONE && millis() - hb_last >= 2000) {
        hb_last = millis();
        Serial1.printf("hb n=%lu\n", (unsigned long)hb_n++);
    }
}
```

- [ ] **Step 2: Build; verify Phase A is UNCHANGED and Phase B produces well-formed lines.** Phase B in QEMU takes minutes (sw renders of 16-knob damage per refresh) and its numbers are meaningless — this run verifies FORMAT and completion only:

```bash
cd ~/Development/rt1170/evkb/examples/display/rotary_knob_bench
cmake --build build
QRUN_TIMEOUT=900 ../../../tools/qrun qemu-system-arm -M mimxrt1170-evk,boot-xip=on -kernel build/rotary_knob_bench.elf \
    -display none -serial file:build/phaseb.uart -d guest_errors -D build/phaseb.dbg &
sleep 840; pkill -f rotary_knob_bench.elf || true
grep -E "^(cell|crc_done|time|bench_done|hb)" build/phaseb.uart
```

Expected: the same 12 `cell=` lines and `crc_done cells=12 ok=6 gpu_absent=6` as Task 4 (byte-identical checksums — Phase B code must not have moved Phase A's pixels), then six `time=.../sw/... frames=64 mfps_med=... us_med=... us_mean=...` lines, six `time=.../gpu/... st=gpu-absent`, `bench_done cells=12 timed=6 gpu_absent=6`, at least one `hb`. If QEMU cannot finish in 14 min, capture what it produced, confirm at least one full `time=` line's format, and note the runtime — the gate never waits for Phase B, so this is informational.

- [ ] **Step 3: Commit**

```bash
cd ~/Development/rt1170/evkb
git add examples/display/rotary_knob_bench
git commit -m "rotary_knob_bench: Phase B timing pass, bench_done, heartbeat"
```

---### Task 6: GPU cells compile and degrade honestly in QEMU

The gpu code paths were all written in Tasks 3–5 (`gpu_rotor_pass`, `rkg_build_vg_paths`, `vg_wrap_argb`, premultiply). This task proves the QEMU half of their contract; silicon proves the other half in Task 10.

- [ ] **Step 1: Confirm the absent path end-to-end.** In the Task 5 capture (`build/phaseb.uart`):

```bash
grep -c "st=gpu-absent" build/phaseb.uart          # expect 12 (6 cell= + 6 time=)
grep -E "/gpu/" build/phaseb.uart | grep -cE "crc=|mfps_med=" || echo CLEAN
```

Expected: `12`, then `CLEAN` — no gpu line ever carries a result in QEMU. This is the tripwire's ground truth.

- [ ] **Step 2: Check `vg_lite_map`'s failure mode when GPU absent.** `vg_wrap_argb()` calls `vg_lite_map` unconditionally — but only from `cell_build_assets`, which Phase A/B only reach for gpu cells when `s_gpu` is true. Verify by reading the state machine that no `vg_lite_*` call is reachable with `s_gpu == false` (the `c->gpu && !s_gpu` guards short-circuit before `cell_build_assets`). Add this assertion as a comment at `cell_build_assets`:

```c
/* Only reached for gpu cells when s_gpu -- the BS_*_START guards
 * short-circuit the absent case before any vg_lite_* call. QEMU proves it:
 * 12 st=gpu-absent lines and zero guest_errors. */
```

- [ ] **Step 3: Check the QEMU debug log is clean:** `grep -i "invalid\|error" build/phaseb.dbg | head` — expect nothing GPU-related (the LPUART/LCDIF chatter other display gates tolerate is fine; compare with `vglite_probe`'s dbg if unsure).

- [ ] **Step 4: Commit** (comment addition only):

```bash
cd ~/Development/rt1170/evkb
git add examples/display/rotary_knob_bench
git commit -m "rotary_knob_bench: document the gpu-absent guard; QEMU tripwire ground truth verified"
```

---

### Task 7: The gate — written, DEMONSTRATED RED twice, fixture captured

**Files:**
- Create: `examples/display/rotary_knob_bench/run_qemu.sh`
- Create: `examples/display/rotary_knob_bench/transcript_qemu.txt`

- [ ] **Step 1: Write `run_qemu.sh`** (fill the six `0x????????` goldens with Task 4's recorded values — they MUST come from two identical consecutive runs, never from a single run):

```sh
#!/bin/sh
set -e
DIR=$(cd "$(dirname "$0")" && pwd)
EVKB=$(cd "$DIR/../../.." && pwd)
QEMU="$EVKB/tools/qrun"
. "$EVKB/tools/gate-lib.sh"
gate_init
ELF="$DIR/$(gate_build_dir)/rotary_knob_bench.elf"
OUT=$(gate_capture_path "$DIR" rotary_knob_bench.uart)
DBG=$(gate_capture_path "$DIR" rotary_knob_bench.dbg)
rm -f "$OUT"
# Phase A renders 6 full sw scenes; QRUN_TIMEOUT raised accordingly. Phase B
# runs after crc_done and is deliberately NOT waited for (spec section 8).
QRUN_TIMEOUT=${QRUN_TIMEOUT:-120} "$QEMU" $(gate_qemu_machine) -kernel "$ELF" \
    -display none -serial file:"$OUT" -d guest_errors -D "$DBG" &
P=$!; gate_pid $P
# Wait for the LAST line this gate parses (crc_done), never an earlier token
# -- the m2_rx_demo mid-line-reap lesson.
for _ in $(seq 1 440); do
    [ -f "$OUT" ] && grep -q "^crc_done " "$OUT" 2>/dev/null && break
    sleep 0.25
done
gate_reap $P
gate_require_capture "$OUT"
echo "==== captured UART ===="; cat "$OUT"

grep -q "rotary_knob_bench up" "$OUT" || { echo "FAIL: banner missing"; exit 1; }
grep -q "PANEL_OK" "$OUT"             || { echo "FAIL: panel bring-up"; exit 1; }
grep -q "^gpu=absent" "$OUT"          || { echo "FAIL: QEMU must report gpu=absent"; exit 1; }

# ★ TRIPWIRE FIRST: no gpu cell may report a result with no GPU present. A
# fabricated result is worse than a missing one, so this outranks the golden
# checks. DEMONSTRATED RED <date>: appending
# 'cell=vector/gpu/notch st=ok crc=0xDEADBEEF init_us=1 rotor_bytes=0' to a
# passing capture failed here by name.
if grep -E "^(cell|time)=[a-z]+/gpu/" "$OUT" | grep -qE "crc=|mfps_med="; then
    echo "FAIL: a GPU cell reported a result with no GPU present"; exit 1
fi

# GOLDEN CHECKSUMS -- FNV-1a over the whole 720x1280 XRGB8888 framebuffer,
# one per sw cell, canonical angle 45 deg. RECORDED from two identical
# consecutive QEMU runs (Task 4), silicon-confirmed in Task 10. On a mismatch
# work out WHICH of {rk_geometry, LVGL pin, lv_conf, scene} moved; never
# paste in whatever the run printed.
# DEMONSTRATED RED <date>: a deliberately wrong vector/sw/notch golden failed
# by name below.
for want in \
  "cell=vector/sw/notch st=ok crc=0x???????? " \
  "cell=vector/sw/facet st=ok crc=0x???????? " \
  "cell=bitmap/sw/notch st=ok crc=0x???????? " \
  "cell=bitmap/sw/facet st=ok crc=0x???????? " \
  "cell=strip/sw/notch st=ok crc=0x???????? " \
  "cell=strip/sw/facet st=ok crc=0x???????? "; do
    grep -qF "$want" "$OUT" || { echo "FAIL: missing/wrong: $want"; exit 1; }
done

# Every gpu cell must be PRESENT with the honest negative -- an absent line is
# a silently skipped cell, which is the SKIP-hides-in-a-count hazard.
for c in vector bitmap strip; do
  for v in notch facet; do
    grep -q "^cell=$c/gpu/$v st=gpu-absent" "$OUT" || {
        echo "FAIL: gpu cell $c/$v did not report gpu-absent"; exit 1; }
  done
done

grep -qE "^crc_done cells=12 ok=6 gpu_absent=6[[:space:]]*$" "$OUT" || {
    echo "FAIL: crc_done tally wrong or missing"; exit 1; }
echo "PASS: rotary_knob_bench Phase A verified (6 sw goldens, 6 honest gpu negatives)"
```

```bash
chmod +x examples/display/rotary_knob_bench/run_qemu.sh
```

- [ ] **Step 2: Run the gate; measure its time**

```bash
cd ~/Development/rt1170/evkb/examples/display/rotary_knob_bench
./run_qemu.sh
```

Expected: PASS. Note the wall time; if crc_done lands in under 30 s, the 440×0.25 s poll and QRUN_TIMEOUT=120 are ~4× margin — keep them. If it is slower, scale both to ~4× measured.

- [ ] **Step 3: DEMONSTRATE RED #1 — wrong golden.** Edit one golden (`vector/sw/notch` crc → `0xDEADBEEF`), run, confirm `FAIL: missing/wrong: cell=vector/sw/notch ...`, restore the real value, re-run, confirm PASS. Record the date in the gate header's "DEMONSTRATED RED" comment.

- [ ] **Step 4: DEMONSTRATE RED #2 — the tripwire.** Run the gate's QEMU by hand, append the fake line, and run just the assertion block against it:

```bash
cp "$(ls build/rotary_knob_bench.uart 2>/dev/null || echo build/gate/rotary_knob_bench.uart)" /tmp/rkb_tamper.uart
echo "cell=vector/gpu/notch st=ok crc=0xDEADBEEF init_us=1 rotor_bytes=0" >> /tmp/rkb_tamper.uart
grep -E "^(cell|time)=[a-z]+/gpu/" /tmp/rkb_tamper.uart | grep -qE "crc=|mfps_med=" && echo "TRIPWIRE FIRES"
```

Expected: `TRIPWIRE FIRES`. (Task 8's vacuity case runs the WHOLE gate against this tamper via the fixture harness — this step just proves the grep logic before wiring it.) Record the date in the gate header.

- [ ] **Step 5: Capture the fixture.** With the gate freshly PASSING:

```bash
cp <the OUT path the gate printed> transcript_qemu.txt
```

(★ Re-capture this file whenever the example's output changes — the 2026-08-25 stale-fixture lesson: the QEMU sweep can NOT catch a stale fixture, only the vacuity suite replays it.)

- [ ] **Step 6: Verify the sweep discovers exactly one new gate**

```bash
cd ~/Development/rt1170/evkb
./tools/run-all-qemu-gates.sh -l | grep rotary_knob_bench
./tools/run-all-qemu-gates.sh -l | tail -1
```

Expected: `rt1176:display/rotary_knob_bench` (no `[variant]` suffix — single script), and the trailing summary says **122 gate(s)**.

- [ ] **Step 7: Commit**

```bash
git add examples/display/rotary_knob_bench/run_qemu.sh examples/display/rotary_knob_bench/transcript_qemu.txt
git commit -m "rotary_knob_bench: gate -- 6 pinned sw goldens, gpu tripwire, demonstrated red twice"
```

---

### Task 8: Vacuity-suite coverage

**Files:**
- Modify: `tools/gate-vacuity.test.sh`

- [ ] **Step 1: Read the file's case sections** (its `run_gate <rel> <gate> [fixture]` helper and the existing green/mutation cases) and append, following the local phrasing conventions:

```sh
# --- rotary_knob_bench: green fixture passes; tamper and bad-golden fail ----
RKB="examples/display/rotary_knob_bench"
if [ -d "$EVKB/$RKB" ] && [ -f "$EVKB/$RKB/transcript_qemu.txt" ]; then
    run_gate "$RKB" run_qemu.sh "$EVKB/$RKB/transcript_qemu.txt"; rc=$?
    report "green_still_passes_rotary_knob_bench" $rc

    # Tripwire: a fabricated gpu result must fail BY NAME even though every
    # genuine assertion still passes on the rest of the capture.
    cp "$EVKB/$RKB/transcript_qemu.txt" "$WORK/rkb_tamper.txt"
    echo "cell=vector/gpu/notch st=ok crc=0xDEADBEEF init_us=1 rotor_bytes=0" >> "$WORK/rkb_tamper.txt"
    run_gate "$RKB" run_qemu.sh "$WORK/rkb_tamper.txt"; rc=$?
    if [ $rc -ne 0 ] && printf '%s' "$OUT_TEXT" | grep -q "GPU cell reported a result"; then
        report "rkb_gpu_tripwire_fires" 0
    else
        report "rkb_gpu_tripwire_fires" 1
    fi

    # A corrupted golden must fail naming the cell, not pass or die silently.
    sed 's/^cell=vector\/sw\/notch st=ok crc=0x......../cell=vector\/sw\/notch st=ok crc=0xBADBADBA/' \
        "$EVKB/$RKB/transcript_qemu.txt" > "$WORK/rkb_badcrc.txt"
    run_gate "$RKB" run_qemu.sh "$WORK/rkb_badcrc.txt"; rc=$?
    if [ $rc -ne 0 ] && printf '%s' "$OUT_TEXT" | grep -q "missing/wrong: cell=vector/sw/notch"; then
        report "rkb_bad_golden_fails_by_name" 0
    else
        report "rkb_bad_golden_fails_by_name" 1
    fi
else
    echo "SKIP: rotary_knob_bench vacuity (example or fixture missing)"
fi
```

- [ ] **Step 2: Run the suite** (needs the example built — it is, from Task 7):

```bash
cd ~/Development/rt1170/evkb
sh tools/gate-vacuity.test.sh
```

Expected: all pre-existing cases plus the three new ones PASS, exit 0. If `green_still_passes` fails, the fixture and the gate disagree — fix whichever is stale (usually re-capture the fixture), never weaken the gate.

- [ ] **Step 3: Commit**

```bash
git add tools/gate-vacuity.test.sh
git commit -m "gate-vacuity: rotary_knob_bench green + tripwire + bad-golden cases"
```

---

### Task 9: License audit, CLAUDE.md arithmetic, full sweep

**Files:**
- Modify: `tools/license-audit.sh` (the `GATES` list, around line 315)
- Modify: `CLAUDE.md`

- [ ] **Step 1: Add the GATES entry** — in the `GATES=` list in `tools/license-audit.sh`, next to the other display entries (alphabetical-ish placement beside `examples/display/synthui_knob_test:synthui_knob_test`):

```
examples/display/rotary_knob_bench:rotary_knob_bench \
```

- [ ] **Step 2: Run the audit from the repo root** (★ if run from anywhere else, pass `LICENSE_AUDIT_EVKB=$(pwd)` — the wrong-tree trap):

```bash
cd ~/Development/rt1170/evkb
./tools/license-audit.sh
```

Expected: `LICENSE-AUDIT: PASS` with the new manifest entry walked (a dep-path count printed for `examples/display/rotary_knob_bench`).

- [ ] **Step 3: Update CLAUDE.md's sweep narrative.** In the paragraph beginning "★ **Before running `./tools/run-all-qemu-gates.sh`, read", update the gate count from 121 to 122 and append to the arithmetic chain (following the existing style):

```markdown
NEW-20 added ONE — `display/rotary_knob_bench`, the RotaryKnob render-strategy
bench: its gate pins the six SOFTWARE cells' goldens and asserts the six GPU
cells report an honest `gpu-absent` in QEMU (the tripwire: no `/gpu/` line may
carry a result there). The GPU cells and every fps number are silicon-only —
a green gate says Phase A renders correctly and degrades honestly, nothing
about speed. 121 before it; the runner's `-l` reports 122.
```

Also update the "The target is **121 passed…**" sentence to 122, and leave every dated ✅ measurement block untouched (they are history).

- [ ] **Step 4: Run the FULL sweep** — the count is re-measured by running, never by counting files. `~/Development/rt1170/evkb` is 93 bytes so no short-path symlink is needed (and `/tmp/ev` points at a DIFFERENT checkout — do not use it):

```bash
cd ~/Development/rt1170/evkb
./tools/run-all-qemu-gates.sh
```

Expected: `gates: 122 passed`, exit 0 (`0 SKIP` is the load-bearing number). If `rt1176:dualcore/cm4_audio_test` alone is red, re-run it idle before reading it as a regression — it is the documented nondeterministic gate.

- [ ] **Step 5: Add the sweep measurement to CLAUDE.md** following the house convention (a dated ✅ line: count, exit code, that the new gate was green and its wall time), and commit:

```bash
git add tools/license-audit.sh CLAUDE.md
git commit -m "audit + CLAUDE.md: rotary_knob_bench gate wired in; sweep re-measured at 122/0/0"
```

---

### Task 10: Silicon — the measurement the bench exists for

**Files:**
- Create: `examples/display/rotary_knob_bench/transcript_hw_evkb.txt`
- Modify: `docs/superpowers/specs/2026-08-27-rotary-knob-bench-design.md` (§13 results)

- [ ] **Step 1: Flash with the standing bench discipline** (VCOM free during programming; no `pkill -9` mid-flash; LinkServer `run` is silent-not-hung — and NEW-8's note says prefer flash load/verify + pyocd reset over a wedging `run`):

```bash
pkill LinkServer; pkill redlinkserv; pkill crt_emu_cm_redlink
cd ~/Development/rt1170/evkb/examples/display/rotary_knob_bench
LinkServer flash MIMXRT1176:MIMXRT1170-EVKB load build/rotary_knob_bench.elf
LinkServer flash MIMXRT1176:MIMXRT1170-EVKB verify build/rotary_knob_bench.elf
```

- [ ] **Step 2: Attach the console FIRST, then reset** (boot output needed):

```bash
python3 ../../../tools/rt1170-console.py /dev/cu.usbmodem* 115200 | tee /tmp/rkb_hw1.log &
pyocd reset -t mimxrt1170 || (LinkServer run MIMXRT1176:MIMXRT1170-EVKB build/rotary_knob_bench.elf &)
```

**Duration budget — do NOT Ctrl-C early.** Phase A is seconds. Phase B runs 64
timed frames per cell: ≈2 s for a fast cell, ≈23 s for one at 2.83 fps, and a
**wedged cell burns its full 120 s timeout**. Nominal is a few minutes; the
worst case is ~24 min. Keep the console attached throughout and let it reach
`bench_done`. Then Ctrl-C the reader.

- [ ] **Step 3: Verify the run.** Check every one of these — they are ordered so that a failure explains the next line rather than being explained by it.

  1. `gpu=present chip_id=` nonzero. On the attempt path the line also carries `vg_init=<n> vg_map=<n>`; both must be **0**. A `gpu=absent` *with* a nonzero chip id and a nonzero code means the GPU is present but init or map failed — a different fault entirely from "no GC355", and the two used to be indistinguishable.
  2. Tallies: `crc_done cells=12 ok=12 gpu_absent=0 failed=0` and `bench_done cells=12 timed=12 gpu_absent=0 failed=0`. **`failed=0` is part of the assertion** — a nonzero value means a cell could not be built at all.
  3. **All twelve `/gpu/` lines must read `gpu_err=0`.** A nonzero count **voids that cell's timing outright**: a rejected blit draws nothing and therefore times beautifully. Never rank a cell whose `gpu_err` is nonzero — diagnose it first.
  4. Explicit negatives: **no `st=timeout`** and **no `st=vg-overflow`** anywhere. If `st=timeout` appears, read that cell's `gpu_err` *first* — a wedge downstream of a rejected call is a symptom, not the cause.
  5. **12** `cell=... st=ok` lines (no `gpu-absent` on silicon) and 12 `time=` lines.
  6. The six sw checksums must equal the QEMU goldens EXACTLY (the pilot's QEMU≡silicon determinism). The six gpu checksums are new — **run a second boot** and confirm they are stable across boots before recording them.

- [ ] **Step 4: Read the `time=` lines correctly.** Fields are `frames=64 mfps_med=<n> us_med=<n> us_min=<n> us_max=<n> us_mean=<n>` plus `gpu_err=<n>` on gpu cells.
  - **Rank on `mfps_med`, never on `us_mean`.** The mean is dragged by any single stall; the median is the honest steady-state number.
  - **Flag any cell whose `us_max` is a large multiple of its `us_med`** — that is a stall worth naming in the write-up even when the median looks fine.
  - The pass criterion is **`mfps_med` ≥ 30000**, and it is a **RENDER** rate, not a displayed one: `LV_DEF_REFR_PERIOD` is 33 ms, so displayed fps cannot exceed ~30 no matter how fast the renderer is. A cell can pass this criterion and still show 30 fps on the glass; that is expected, not a defect.

- [ ] **Step 5: Watch the glass — during Phase B, not Phase A.** Phase A flashes past in well under a second; there is nothing to see. Phase B gives ~2 s per cell of *moving* rotors, which is the window to look at:
  - Shape is right: well ring, dark rotor, light index wedge; facet cells show the 8-tone fan.
  - **Wedge direction matches the cell's sw sibling.** `vg_lite_rotate` is clockwise-positive on a Y-down target (`vg_lite_matrix.c:140-147`, applied `p' = M·p`), which matches `rkg_polar` and LVGL, so a **mirrored** rotor is a real defect to investigate — not a sign convention to flip.
  - **C1 REGRESSION GUARD — the strip cross-check.** `strip/gpu`'s wedge must sit at the **same angle** as `strip/sw`'s. **Twice the angle means C1 has regressed**: the filmstrip frame is already rotated, so a rotate in the blit matrix both doubles the angle and reintroduces the resample the filmstrip exists to avoid (which would also make `strip/gpu` time identically to `bitmap/gpu` — check that too).
  - Tearing is expected (§9, single-buffered direct mode); it is not a defect.
  - **After `bench_done` the final scene stays static on the glass — `strip/gpu/facet`. That is the frame to photograph.**
  - If a gpu cell's rotor is missing while its checksum is "stable": the all-zeros FNV is **0x9BC99DC5**, but the scene ground is opaque `0x101820`, so a blank-rotor frame will *not* equal that constant. Compare each gpu cell against **its sw sibling's structure on the glass**, not against the zero constant alone. `AQHiIdle` bit 0 and `vg_lite_os_irq_count()` are the diagnostics, and `gpu_err` (step 3.3) should already have caught it.

- [ ] **Step 6: Record.** Save the full log as `transcript_hw_evkb.txt` (with a header comment noting date, board, both boots' gpu checksums). Fill spec §13's **nine-column** table — `cell | mfps_med | us_med | us_min | us_max | us_mean | init_us | rotor_bytes | crc` — the last three coming from the Phase A `cell=` lines. State the winner (or measured tie) in one paragraph under the table, with the RAM/init/quality trade-offs spec §11.3 requires, **and record the pipelining caveat alongside it**: the bench calls `vg_lite_finish()` once per frame, so every GPU row is a **lower bound** — a pipelined renderer that never stalls on the GPU could do better than these numbers show.

- [ ] **Step 7: Commit**

```bash
cd ~/Development/rt1170/evkb
git add examples/display/rotary_knob_bench/transcript_hw_evkb.txt docs/superpowers/specs/2026-08-27-rotary-knob-bench-design.md
git commit -m "rotary_knob_bench: silicon run -- 12/12 cells, fps table recorded, winner named"
```

- [ ] **Step 8: Linear.** Post the results table as a comment on NEW-20. NEW-12's open question (are cached GPU paths fast?) is now answered by the `vector/gpu` cells — close NEW-12 with a comment linking the table and naming the measured number. (Use the Linear MCP `save_comment` / `save_issue state=Done`; confirm with the user if anything about the numbers makes closing feel premature.)

---

## Self-Review Notes (performed at write time)

- **Spec coverage:** §1–§11 all map to tasks (§2 is context; §12 is out of scope by design; §13 is Task 10). Task 1 amends the spec where planning contradicted it.
- **Known deliberate deviations from the spec text, fixed by Task 1:** 150 px knobs; post-REFR_READY gpu pass; FNV-1a naming; strategy tokens.
- **Type consistency:** `rkg_*` signatures in Task 3's header match every later use; `RKB_*` constants defined once in `rk_geometry.h`; cell/time line formats identical across firmware (Task 3/5), gate (Task 7), and vacuity (Task 8).
- **Open risks the executor should expect:** (1) exact QEMU `-M` flags — take them from `gate-lib.sh`, not this plan; (2) `lv_image_dsc_t` field names (`header.magic`/`header.stride`) — check the vendored `lv_image_dsc_t` in `~/Development/LVGL/lvgl/src/draw/lv_image_decoder.h` if the compile disagrees; (3) `lvgl_mipi_panel_frame_done()` latching semantics — the REFR_READY count carries per-cell freshness, `frame_done` is a one-time flush confirmation; if it never re-arms per frame that is fine. **(4) was `vg_lite_rotate` sign convention — DELETED, resolved from source rather than left to the bench:** `vg_lite_rotate` builds `[[cos,-sin],[sin,cos]]` and applies it as `p' = M·p` (`vg_lite_matrix.c:140-147`), which on a **Y-down** target is clockwise-positive — the same convention as `rkg_polar` and as LVGL's `rotation`. There is therefore no sign to flip: a mirrored rotor on the glass would be a **real defect to diagnose**, and "negate the angle for gpu paths" would paper over it.

---

## Execution addendum (2026-08-27): review findings folded in

Task 3 was implemented from the code blocks above and then reviewed. **The code
blocks in this plan were deliberately NOT rewritten** — they remain the record
of what was planned. This section records what shipped instead, so a future
re-executor does not reintroduce the defects by transcribing the stale blocks.

### C1 — rotor stride must be 64-byte aligned (CONFIRMED, FIXED)

The plan's arenas were `150 × 150 × 4 = 90,000 B`, stride 600. This part has
`gcFEATURE_VG_16PIXELS_ALIGNED = 1` and `gcFEATURE_VG_ERROR_CHECK = 1`
(`VGLite/Series/gc355/0x0_1216/vg_lite_options.h:80,109`), so
`srcbuf_align_check` (`VGLite/vg_lite.c:1854-1861`, reached from `vg_lite_blit`
at `:4533`) requires a BGRA8888 source stride to be a multiple of
`16 px × 4 B = 64 B`. `600 % 64 = 24` → **all four `bitmap/gpu` and `strip/gpu`
cells would have returned `VG_LITE_INVALID_ARGUMENT` and drawn nothing, while
posting excellent frame times.** A benchmark that rewards not drawing is the
worst failure available to this example.

Shipped:
- `CMakeLists.txt` adds `LV_DRAW_BUF_STRIDE_ALIGN=64 LV_DRAW_BUF_ALIGN=64`
  (both `#ifndef`-guarded in `port/lv_conf.h:182-188`;
  `lvgl_mipi_panel.cpp`'s `PANEL_PITCH_BYTES % STRIDE_ALIGN == 0` still holds,
  2880 % 64 == 0).
- `rk_geometry.h` gains `RKB_ROTOR_STRIDE_PX 160` / `RKB_ROTOR_STRIDE_B` /
  `RKB_ROTOR_BYTES`; arenas are `RKB_KNOB_PX × RKB_ROTOR_STRIDE_PX`.
- `rkg_render_rotor_argb` clears the full padded extent, and two
  **`static_assert`s** in `rk_geometry.cpp` tie `LV_DRAW_BUF_STRIDE_ALIGN == 64`
  and `RKB_ROTOR_STRIDE_B == 640` together, so the painter and the GPU consumer
  cannot silently diverge. (Commit `e701c19` replaced an earlier runtime
  `LV_ASSERT` with these: the invariant is entirely compile-time for the one
  width this example uses, and the runtime form ran 64× per strip cell *inside*
  the `init_us` measurement window. Mutation-tested — setting the stride to 150
  fails the build by name.)
- `vg_wrap_argb` stride, `lv_image_dsc_t.header.stride` → 640;
  `rotor_bytes` now reports the honest 96,000 / 6,144,000.

**Measured: checksum-neutral.** All six software goldens were byte-identical
before and after the change, so the padding columns are genuinely not sampled.

### C2 — "double premultiply" — INVESTIGATED AND NOT APPLIED

The review called for deleting `rkg_premultiply` on the grounds that the driver
enables hardware source premultiply. **That did not survive checking, and the
software premultiply was kept.** Recorded because the argument is easy to
re-derive and get wrong a second time:

- The cited mechanism does not apply. `vg_lite.c:4742`'s `premul_flag` tests
  `blend` against `OPENVG_BLEND_SRC..ADDITIVE` (0x2000–0x2009) and
  `VG_LITE_BLEND_NORMAL_LVGL..MULTIPLY_LVGL` (11–14). Our blend is
  `VG_LITE_BLEND_SRC_OVER = 1`, in neither range, so `premul_flag == 0` and the
  **first** branch is taken — a branch that sets identical registers for
  `source->premultiplied` 0 **and** 1, and therefore cannot mean "the hardware
  premultiplies because you declared 0".
- `VG_LITE_BLEND_SRC_OVER` is documented as `S + D*(1 - Sa)`
  (`inc/vg_lite.h:461`) — the *premultiplied* over. Removing the premultiply
  while keeping that blend would be wrong in the opposite direction.
- Decisive precedent: LVGL 9.4's own VG_LITE backend, against this same driver
  with `gcFEATURE_VG_LVGL_SUPPORT = 0`, software-premultiplies
  (`lv_draw_vg_lite_img.c:66`), selects `SRC_OVER`
  (`lv_vg_lite_utils.c:958`), and never assigns `premultiplied` anywhere.
  Our code reproduces that combination exactly.

The reasoning is now a long comment above `rkg_premultiply`, and spec §6 tells
silicon bring-up what a genuine double premultiply would look like (dark
fringes on antialiased rotor edges) and which knob to turn if it appears.

### I1 — GPU error codes are now counted, not discarded

Every `vg_lite_map`/`draw`/`blit`/`finish` in the GPU path goes through
`GPU_TRY()`, incrementing a per-cell `g_gpu_err`. GPU `cell=`/`time=` lines
gain a trailing ` gpu_err=<n>`; **`sw` lines are unchanged**, so gate greps
written against the plan's format still match. Spec §5 requires `gpu_err=0` in
the hardware transcript.

### I2 — `lvgl_mipi_panel_frame_done()` conjunct dropped from `BS_A_WAIT`

It was vacuous: the flag latches on the first full refresh ever and is cleared
only by another `create()`, so from cell 1 onward it was a constant true.
`LV_EVENT_REFR_READY` alone is sufficient — it is sent from `refr_finish`
after rendering and flushing (`lv_refr.c:439`), and this binding's flush is
synchronous.

### I3 — empty-refresh hazard documented for Phase B (Task 5)

`REFR_READY` also fires when nothing was invalid (`lv_refr.c:415` → `:439`).
Phase A is immune because it arms its counter right after `lv_screen_load()`,
which invalidates everything. **Phase B must not inherit that assumption**: it
must gate both its timing sample and the GPU rotor pass on real damage
(`LV_EVENT_RENDER_READY`, or a `lvgl_mipi_panel_flushed_px()` delta), or it
will time empty frames and blit rotors onto a frame whose well was never
redrawn. A comment at `refr_cb` says so.

### I4 — the ordering invariant is now written down

No refresh may run between `cell_build_assets()` and `cell_build_scene()`:
`lv_canvas_finish_layer()` invalidates areas of the *outgoing* screen (64 times
for a strip cell), and only the subsequent `lv_screen_load()` supersedes them.
Holds because both run back-to-back in `bench_step()`, after
`lvgl_rt1176_loop()` returns. Also noted: `finish_layer` dispatching outside a
refresh is safe only because `LV_DRAW_SW_DRAW_UNIT_CNT == 1`.

### I5 — path-arena overflow is a named failure

`emit()` set a sticky flag; `rkg_build_vg_paths` returns **-1** on overflow and
the cell is reported `st=vg-overflow`, counted in a new `failed=` field on
`crc_done`, and **not rendered** — a truncated path set would still draw and
still time. Cannot fire today (~110 of 4096 words); it is a guard for Phase 2's
additional variants.

### Minor

Canvas create/delete hoisted out of the 64-frame strip loop into
`rkg_render_strip_argb` (one canvas, re-pointed per frame — `init_us` should
measure rendering, not object churn; measured ~6–9 % faster);
`#include "vg_lite.h"` moved out of `rk_geometry.h`'s `extern "C"` (it has its
own guard); comments added for the absent-D-cache rationale at
`vg_lite_finish`, the idempotent re-map (safe only because the vg_lite GPU base
is 0), and the deliberate truncating `lv_value_precise_t` angle casts.

### Also worth carrying forward

Open risk (3) in the self-review above — "`frame_done` latching semantics …
if it never re-arms per frame that is fine" — was **too generous**: it does not
re-arm, which made the conjunct dead code rather than merely redundant. See I2.
