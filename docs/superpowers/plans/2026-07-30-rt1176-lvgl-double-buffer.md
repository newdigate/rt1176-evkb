# LVGL double buffering with page flip on vsync (v4) — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Remove tearing from the LVGL MIPI direct-render path: two framebuffers, LVGL
renders off-screen, the LCDIFv2 flips at vsync via its shadow-load, and a gate proves the
panel scanned each buffer in turn.

**Architecture:** Spec `docs/superpowers/specs/2026-07-30-rt1176-lvgl-double-buffer-design.md`
— read it first. Four repos: qemu2 (faithful shadow-load, F1), MipiDisplay (three
primitives, F2), LVGL (`lvgl_mipi_panel_create_db`, F3), evkb (`lvgl_rk055_flip_test` +
gate + wrap).

**Tech stack:** LVGL 9.4 (`flush_wait_cb`, direct 2-buffer mode — `refr_sync_areas` owns
coherency), LCDIFv2 shadow registers, the virtual HX8394 `PANEL_SUM` tap, CMake + ARM GCC 10,
LinkServer.

**Verified facts the code below leans on (do not re-derive, do sanity-check):**
- `lv_display.c:449`: `disp->buf_act = disp->buf_1` — LVGL renders into **buf1 first**, so
  buf1 must be the buffer the panel is NOT scanning. NXP's port confirms the ordering
  (`lvgl_support.c:244-267`: panel shows `s_frameBuffer[1]`, buf1 = `s_frameBuffer[0]`).
- In DIRECT mode `flush_cb`'s `px_map` is the frame base — NXP passes `color_p` straight to
  `setFrameBuffer` (`lvgl_support.c:426-434`, fact with citation, no code copied).
- `lv_refr.c:1435`: `flush_wait_cb` is polled in LVGL's own wait loop.
- Core register macros (`cores/imxrt1176/imxrt1176.h:2030-2078`): `LCDIFV2_INT_STATUS_D0`,
  `LCDIFV2_INT_VSYNC` (bit 0, write-1-clear), `LCDIFV2_CTRLDESCL4(0)`,
  `LCDIFV2_CTRLDESCL5(0)`, `LCDIFV2_CTRLDESCL5_SHADOW_LOAD_EN`.
- QEMU model: `imxrt_lcdifv2_get_layer0()` reads `addr` straight from the CTRLDESCL4
  register (`imxrt_lcdifv2.c:73,80`) — that is the line F1 changes; the vsync timer cb is
  `lcdifv2_vsync_timer_cb` (`:191`); `INT_STATUS` W1C is at `:361`. The HX8394 tap's
  `PANEL_SUM` calls `imxrt_lcdifv2_scan_checksum()` on read (`imxrt_hx8394.c:382-402`) and
  serves a sentinel unless the layer is enabled — after F1 it hashes the **latched** address,
  i.e. what the panel actually scans.
- `MipiDisplay soc/lcdifv2.cpp` allocates the fb via `extmem_malloc(PANEL_FB_BYTES +
  FB_ALIGN)` + round-up; its CTRLDESCL5 verify already masks out `SHADOW_LOAD_EN`
  (`lcdifv2.cpp:238`), so the model's new self-clear cannot break it.

**Conventions:** gates as `./run_qemu.sh` only; `uptime`/`ps` before gate runs; each repo
commits separately; firmware prints to `Serial1`; printf lines ≤ 127 chars.

---

## Task 1: QEMU model — shadow-load latches at vsync (F1)

**Files:**
- Modify: `~/Development/qemu2/include/hw/display/imxrt_lcdifv2.h` (state struct)
- Modify: `~/Development/qemu2/hw/display/imxrt_lcdifv2.c` (write path, vsync cb, accessor,
  reset, vmstate)

- [ ] **Step 1: Add the latch state.** In the header's `IMXRTLcdifv2State`, next to the
existing register array, add:

```c
    /*
     * Faithful shadow-load (RM ch.48): CTRLDESCL4's ADDR is a SHADOW register.
     * A SHADOW_LOAD_EN pulse arms a load; at the next vsync tick the shadow
     * registers AS THEY STAND AT THAT MOMENT are copied to the active side,
     * and SHADOW_LOAD_EN self-clears.  Scanout and the panel tap read the
     * ACTIVE address, so "which buffer is on the glass" is finally a modelled
     * fact rather than whatever was last written.  layer0_active_addr == 0
     * means "nothing latched yet" -- scanout is dark until the first load,
     * which on this machine happens within one frame period of DISP_ON.
     */
    uint32_t layer0_active_addr;
    bool     shadow_load_pending;
```

- [ ] **Step 2: Arm on the SHADOW_LOAD_EN write.** In the register write handler, where
`LCDIFV2_CTRLDESCL5_0` is stored (the default store path covers it today — add an explicit
case AFTER the store or adjust the existing one):

```c
    case LCDIFV2_CTRLDESCL5_0:
        s->regs[lcdifv2_reg_idx(offset)] = value;
        if (value & LCDIFV2_CTRLDESCL5_SHADOW_LOAD_EN) {
            /* Armed, not applied: the copy happens at the next vsync tick.
             * The bit reads back 1 until then -- that is the observable
             * hardware behaviour lcdifv2.cpp:211-220 documents (and its
             * init verify already masks the bit out, measured in Step 5). */
            s->shadow_load_pending = true;
        }
        break;
```

(Use the file's actual switch shape — if CTRLDESCL5 currently falls through to a generic
store, add the case with the same `lcdifv2_reg_idx` idiom as its neighbours.)

- [ ] **Step 3: Latch in the vsync timer callback.** At the top of
`lcdifv2_vsync_timer_cb()`, before the INT_STATUS bits are raised:

```c
    if (s->shadow_load_pending) {
        /* Copy the shadow registers as they stand at THIS vsync.  A write to
         * CTRLDESCL4 after the pulse but before this tick therefore lands --
         * exactly the RM's shadow semantics (the pulse arms the load; the
         * load copies whatever the shadow holds when vsync arrives). */
        s->layer0_active_addr = s->regs[lcdifv2_reg_idx(LCDIFV2_CTRLDESCL4_0)];
        s->regs[lcdifv2_reg_idx(LCDIFV2_CTRLDESCL5_0)] &=
            ~LCDIFV2_CTRLDESCL5_SHADOW_LOAD_EN;
        s->shadow_load_pending = false;
    }
```

- [ ] **Step 4: Scanout reads the active address; reset and vmstate.**
- In `imxrt_lcdifv2_get_layer0()`: `out->addr = s->layer0_active_addr;` (replacing the read
  of `d4`; keep `d4` only if other fields need it — they don't, delete the now-unused local).
- In the device reset function: `s->layer0_active_addr = 0; s->shadow_load_pending = false;`
- In the vmstate description: add `VMSTATE_UINT32(layer0_active_addr, IMXRTLcdifv2State)`
  and `VMSTATE_BOOL(shadow_load_pending, IMXRTLcdifv2State)`, and bump `version_id` (and
  `minimum_version_id`) by one.

- [ ] **Step 5: Rebuild and re-run all four existing display gates — green BEFORE anything
depends on the change.**

```bash
ninja -C ~/Development/qemu2/build qemu-system-arm
cd ~/Development/rt1170/evkb
for g in rk055_panel_test rpi_panel_test lvgl_rk055_panel_test lvgl_rpi_panel_test; do
  (cd examples/display/$g && ./run_qemu.sh > /tmp/f1_$g.log 2>&1; echo "$g EXIT=$?")
done
```

Expected: all four `EXIT=0`. Their init pulses `SHADOW_LOAD_EN` once; the latch lands at the
first vsync tick (≤ ~17 ms of model time), far inside every gate's settle window — but this
step MEASURES that instead of assuming it. Also re-run `rk055_touch_test` and
`lvgl_rk055_touch_test` (they own framebuffers via the same path):

```bash
for g in rk055_touch_test lvgl_rk055_touch_test; do
  (cd examples/display/$g && ./run_qemu.sh > /tmp/f1_$g.log 2>&1; echo "$g EXIT=$?")
done
```

Expected: both `EXIT=0`. **Any red here is a real regression from the model change — STOP
and understand it; do not weaken the model.**

- [ ] **Step 6: Commit (qemu2 repo, on master)**

```bash
cd ~/Development/qemu2
git add include/hw/display/imxrt_lcdifv2.h hw/display/imxrt_lcdifv2.c
git commit -m "lcdifv2: shadow-load latches at vsync and self-clears

CTRLDESCL4's ADDR becomes a true shadow register: a SHADOW_LOAD_EN pulse
arms a copy that lands at the next vsync tick, the bit self-clears, and
scanout plus the HX8394 PANEL_SUM tap read the ACTIVE address.  Closes
the plain-RW divergence MipiDisplay's lcdifv2.cpp documents, and makes
'the panel scanned buffer A, then B' a provable gate claim for the v4
double-buffer work.  All six display/touch gates re-run green."
```

---

## Task 2: MipiDisplay primitives (F2)

**Files:**
- Modify: `~/Development/MipiDisplay/soc/lcdifv2.h` (three declarations + docs)
- Modify: `~/Development/MipiDisplay/soc/lcdifv2.cpp` (implementations)

- [ ] **Step 1: Declarations.** Append to `soc/lcdifv2.h` (after the existing
`lcdifv2Begin` declaration, matching its comment style):

```cpp
// --- v4 double-buffer primitives --------------------------------------------
// Allocate a second framebuffer with the same size, alignment and zero-fill as
// the one lcdifv2Begin() allocated.  nullptr on allocation failure -- the
// caller must fail LOUDLY, never fall back to single-buffer silently.
uint16_t *lcdifv2AllocAltFramebuffer();

// Program the shadowed CTRLDESCL4 ADDR and pulse SHADOW_LOAD_EN.  The flip
// TAKES EFFECT AT THE NEXT VSYNC, not at the call: until then the panel keeps
// scanning the previous buffer.  Pair with the vsync helpers below to know
// when it landed.
void lcdifv2FlipTo(const uint16_t *fb);

// Vsync event over INT_STATUS_D0 (write-1-to-clear), POLLED from thread
// context -- deliberately not the SHADOW_LOAD_EN self-clear, and deliberately
// not an interrupt (v4 stages the ISR out; see the vsync-isr session brief).
// arm() clears the latched bit; seen() reports whether a vsync arrived since.
void lcdifv2VsyncArm();
bool lcdifv2VsyncSeen();
```

- [ ] **Step 2: Implementations.** In `soc/lcdifv2.cpp`, factor the existing allocation
into a helper if trivial, or write the alt allocator to mirror `lcdifv2Begin()`'s exact
sequence (same `FB_ALIGN` constant, same round-up, same zero-fill):

```cpp
uint16_t *lcdifv2AllocAltFramebuffer() {
  // Mirrors lcdifv2Begin()'s allocation exactly: extmem_malloc only 4-byte
  // aligns, so over-allocate and round up to FB_ALIGN for the AXI burst fetch.
  uint8_t *raw = (uint8_t *)extmem_malloc(PANEL_FB_BYTES + FB_ALIGN);
  if (!raw) return nullptr;
  uint16_t *fb = (uint16_t *)(((uintptr_t)raw + FB_ALIGN - 1) &
                              ~(uintptr_t)(FB_ALIGN - 1));
  memset(fb, 0, PANEL_FB_BYTES);
  return fb;
}

void lcdifv2FlipTo(const uint16_t *fb) {
  LCDIFV2_CTRLDESCL4(0) = (uint32_t)(uintptr_t)fb;
  // One-shot trigger; hardware latches at the next vsync and self-clears.
  LCDIFV2_CTRLDESCL5(0) |= LCDIFV2_CTRLDESCL5_SHADOW_LOAD_EN;
}

void lcdifv2VsyncArm()  { LCDIFV2_INT_STATUS_D0 = LCDIFV2_INT_VSYNC; }
bool lcdifv2VsyncSeen() { return (LCDIFV2_INT_STATUS_D0 & LCDIFV2_INT_VSYNC) != 0; }
```

(If `FB_ALIGN`/allocation live inside `lcdifv2Begin()`'s scope, hoist the constant to file
scope rather than duplicating the value.)

- [ ] **Step 3: Compile proof.** No standalone gate (three registers deep; the Task 5 gate
exercises them for real). Prove compilation via an existing consumer:

```bash
cd ~/Development/rt1170/evkb/examples/display/lvgl_rk055_panel_test
rm -rf build && cmake -B build -DCMAKE_TOOLCHAIN_FILE=toolchain/rt1170-evkb.toolchain.cmake && cmake --build build 2>&1 | tail -2
./run_qemu.sh > /tmp/f2_gate.log 2>&1; echo "EXIT=$?"
```

Expected: clean build, gate `EXIT=0` (nothing calls the new functions yet).

- [ ] **Step 4: Commit (MipiDisplay repo, on master)**

```bash
cd ~/Development/MipiDisplay
git add soc/lcdifv2.h soc/lcdifv2.cpp
git commit -m "lcdifv2: alt-framebuffer alloc, FlipTo, and polled vsync events

Three v4 primitives: a second framebuffer with lcdifv2Begin()'s exact
allocation discipline, a shadow-load flip that takes effect at the next
vsync, and INT_STATUS-based vsync arm/seen polled from thread context --
deliberately not the SHADOW_LOAD_EN self-clear, and deliberately not an
interrupt (staged to the vsync-isr session).  Panel API untouched."
```

---

## Task 3: The binding — `lvgl_mipi_panel_create_db` (F3a)

**Files:**
- Modify: `~/Development/LVGL/port/lvgl_mipi_panel.h`
- Modify: `~/Development/LVGL/port/lvgl_mipi_panel.cpp`

- [ ] **Step 1: Header additions** (append after the existing declarations, keeping the
file's voice):

```cpp
/* --- v4: double-buffered create ------------------------------------------
 * Two framebuffers; LVGL renders off-screen and the LCDIFv2 flips at vsync.
 * LVGL ITSELF keeps the buffers coherent (refr_sync_areas, lv_refr.c:647)
 * -- this binding only supplies the second buffer and the flip.
 *
 * THE INVARIANT: LVGL never renders into a buffer the panel is scanning.
 * It holds because rendering waits on flush_wait_cb, and flush_wait_cb
 * returns only after the vsync at which the pending flip latched.
 *
 * BUFFER ORDER IS LOAD-BEARING: LVGL renders into buf1 FIRST
 * (lv_display.c:449), and the panel is scanning display.framebuffer() when
 * this is called -- so buf1 is the freshly allocated ALT buffer and buf2 is
 * the scanout one.  Swap them and the first frame renders into live scanout.
 *
 * Allocates the alt framebuffer itself (lcdifv2AllocAltFramebuffer) and
 * asserts on failure -- there is no silent single-buffer fallback.
 * The v1 accessors (frame_done, flushed_px) work for this path too. */
lv_display_t *lvgl_mipi_panel_create_db(DisplayClass &display);

/* Diagnostics since create_db(), reset by it:
 *   flips           shadow-load pulses issued (one per full refresh)
 *   vsyncs          vsync events consumed waiting for flips to land
 *   vsync_timeouts  waits that gave up (2 frame periods) -- 0 on any healthy
 *                   run; a non-zero value means vsync is dead and the gate
 *                   must go red on it rather than hang. */
uint32_t lvgl_mipi_panel_flips();
uint32_t lvgl_mipi_panel_vsyncs();
uint32_t lvgl_mipi_panel_vsync_timeouts();

/* The buffer the panel is scanning after the last landed flip (the pointer
 * handed to the last lcdifv2FlipTo whose vsync was consumed), or nullptr
 * before the first flip lands.  The flip test checksums exactly this. */
const uint16_t *lvgl_mipi_panel_scanned_fb();

/* Block until the pending flip has landed (bounded: two frame periods).
 * No-op when none is pending.  flush_wait_cb calls this; the flip test also
 * calls it directly so it can read the panel tap between a landed flip and
 * the next render. */
void lvgl_mipi_panel_flip_sync();
```

- [ ] **Step 2: Implementation** (append to the .cpp; existing v1 statics untouched):

```cpp
/* --- v4 double-buffer state (same no-volatile rationale as above) --------- */
static const uint16_t *s_db_pending_fb = nullptr; /* flip issued, not landed  */
static const uint16_t *s_db_scanned_fb = nullptr; /* what the panel shows now */
static uint32_t s_db_flips = 0, s_db_vsyncs = 0, s_db_vsync_timeouts = 0;

void lvgl_mipi_panel_flip_sync()
{
    if (!s_db_pending_fb) return;
    /* Bounded: two frame periods at 58.7 Hz is ~34 ms; a vsync that has not
     * arrived by then is dead, and hanging here would eat the gate's timeout
     * with no token.  The counter is the loud part (spec open question 2). */
    const uint32_t t0 = micros();
    while (!lcdifv2VsyncSeen()) {
        if ((uint32_t)(micros() - t0) > 40000u) {
            s_db_vsync_timeouts++;
            s_db_pending_fb = nullptr;   /* stop re-waiting on a dead line */
            return;
        }
    }
    s_db_vsyncs++;
    s_db_scanned_fb = s_db_pending_fb;
    s_db_pending_fb = nullptr;
}

static void db_flush_wait_cb(lv_display_t *disp)
{
    (void)disp;
    lvgl_mipi_panel_flip_sync();
}

static void db_flush_cb(lv_display_t *disp, const lv_area_t *area, uint8_t *px_map)
{
    s_flushed_px += (uint32_t)lv_area_get_size(area);
    if (lv_display_flush_is_last(disp)) {
        /* px_map is the FRAME BASE in DIRECT mode (fact: NXP's port hands
         * color_p straight to setFrameBuffer, lvgl_support.c:426-434) -- the
         * buffer LVGL just finished rendering, which becomes the new front.
         *
         * ORDER: FlipTo BEFORE VsyncArm.  If a vsync lands between the two,
         * the flip latched at it and we merely wait one extra frame -- safe.
         * Arming first would let a vsync in the gap satisfy the wait while
         * the flip had NOT latched: a render into live scanout, the exact
         * bug this binding exists to prevent. */
        lcdifv2FlipTo((const uint16_t *)px_map);
        lcdifv2VsyncArm();
        s_db_pending_fb = (const uint16_t *)px_map;
        s_db_flips++;
        s_frame_done = true;
    }
    lv_display_flush_ready(disp);
}

lv_display_t *lvgl_mipi_panel_create_db(DisplayClass &display)
{
    LV_ASSERT_NULL(display.framebuffer());   /* Display.begin() must have succeeded */
    LV_ASSERT(display.width() == PANEL_WIDTH && display.height() == PANEL_HEIGHT);

    uint16_t *alt = lcdifv2AllocAltFramebuffer();
    LV_ASSERT_NULL(alt);                     /* no silent single-buffer fallback */

    s_frame_done = false;
    s_flushed_px = 0;
    s_db_pending_fb = nullptr;
    s_db_scanned_fb = nullptr;
    s_db_flips = s_db_vsyncs = s_db_vsync_timeouts = 0;

    lv_display_t *disp = lv_display_create((int32_t)display.width(),
                                           (int32_t)display.height());
    lv_display_set_flush_cb(disp, db_flush_cb);
    lv_display_set_flush_wait_cb(disp, db_flush_wait_cb);
    /* buf1 = ALT (LVGL renders it first), buf2 = the live scanout buffer.
     * See the header: this order is load-bearing. */
    lv_display_set_buffers(disp, alt, display.framebuffer(), PANEL_FB_BYTES,
                           LV_DISPLAY_RENDER_MODE_DIRECT);
    return disp;
}

uint32_t lvgl_mipi_panel_flips()          { return s_db_flips; }
uint32_t lvgl_mipi_panel_vsyncs()         { return s_db_vsyncs; }
uint32_t lvgl_mipi_panel_vsync_timeouts() { return s_db_vsync_timeouts; }
const uint16_t *lvgl_mipi_panel_scanned_fb() { return s_db_scanned_fb; }
```

- [ ] **Step 3: Commit (LVGL repo, on master).** First compilation happens in Task 4.

```bash
cd ~/Development/LVGL
git add port/lvgl_mipi_panel.h port/lvgl_mipi_panel.cpp
git commit -m "port: lvgl_mipi_panel_create_db -- double-buffered direct render

Two buffers (buf1 = the alt buffer, because LVGL renders buf1 first and
the panel is scanning the other one), a flip at vsync via the LCDIFv2
shadow-load, and a deferred wait through flush_wait_cb that blocks only
when the next refresh actually needs the buffer.  FlipTo-before-VsyncArm
ordering is load-bearing and documented at the site.  v1 create untouched."
```

---

## Task 4: `lvgl_rk055_flip_test` — the example (F3b)

**Files:**
- Create: `examples/display/lvgl_rk055_flip_test/CMakeLists.txt`
- Create: `examples/display/lvgl_rk055_flip_test/lvgl_rk055_flip_test.cpp`
- Create: `examples/display/lvgl_rk055_flip_test/toolchain/` (copied)

- [ ] **Step 1: Directory + toolchain**

```bash
cd ~/Development/rt1170/evkb/examples/display
mkdir -p lvgl_rk055_flip_test
cp -r lvgl_rk055_panel_test/toolchain lvgl_rk055_flip_test/toolchain
```

- [ ] **Step 2: `CMakeLists.txt`**

```cmake
cmake_minimum_required(VERSION 3.24)
project(lvgl_rk055_flip_test)

set(TEENSY_VERSION 117 CACHE STRING "")

include(${CMAKE_CURRENT_LIST_DIR}/../../../evkb.cmake)

import_evkb_lvgl()
import_evkb_library(MipiDisplay soc panels/rk055)
import_evkb_library(PXP)    # Display::fillScreen() paints via the PXP

evkb_library_dir(LVGL _lvgl_dir)

teensy_add_executable(lvgl_rk055_flip_test
    lvgl_rk055_flip_test.cpp
    ${_lvgl_dir}/port/lvgl_mipi_panel.cpp)
teensy_target_link_libraries(lvgl_rk055_flip_test cores MipiDisplay PXP)

target_link_libraries(lvgl_rk055_flip_test.elf LVGL stdc++)

# -DFLIP_DEMO_SINGLE=ON builds the SINGLE-buffer variant of the same animation
# (v1 create, tearing visible) for the Task-6 bench before/after.  The QEMU
# gate never runs that variant; it exists purely so the hardware demonstration
# has an honest "before".
option(FLIP_DEMO_SINGLE "single-buffer tearing demo variant" OFF)
if(FLIP_DEMO_SINGLE)
    target_compile_definitions(lvgl_rk055_flip_test.elf PRIVATE FLIP_DEMO_SINGLE=1)
endif()
```

- [ ] **Step 3: `lvgl_rk055_flip_test.cpp`**

```cpp
/* lvgl_rk055_flip_test - double buffering with page flip on vsync (v4).
 * Copyright (c) 2026 Nicholas Newdigate
 * SPDX-License-Identifier: MIT
 *
 * An animated box sweeps the panel while LVGL renders into the OFF-SCREEN
 * buffer and the LCDIFv2 flips at vsync.  The scene is deliberately
 * GOLDEN-FREE: it is time-driven, so the gate asserts flip DISCIPLINE --
 * never pixel checksums of a fixed scene:
 *
 *   FLIP_A/B MATCH   firmware's FNV-1a of the buffer it just flipped in
 *                    equals the virtual HX8394 tap's PANEL_SUM -- the panel
 *                    SCANNED that buffer.  Two consecutive frames, and
 *                    A != B is itself asserted (DISTINCT), or alternation
 *                    would be unfalsifiable.  QEMU-only by construction:
 *                    the tap is emulator fiction (branch on TAP_ID, exactly
 *                    as rk055_panel_test does).
 *   FLIPS==REFRESHES one shadow-load pulse per full refresh.
 *   VSYNCS==FLIPS    every flip's landing was consumed exactly once.
 *   VSYNC_TIMEOUTS=0 no wait gave up; a dead vsync names itself.
 *
 *   WHAT ONLY HARDWARE PROVES: tearing's ABSENCE -- an eye on the sweeping
 *   box (QEMU has no partial-scanout model), and the real 58.7 Hz cadence.
 *   Build with -DFLIP_DEMO_SINGLE=ON for the v1 single-buffer "before".
 *
 * Uses Serial1 (LPUART; QEMU captures it), like every sibling gate.
 */
#include <Arduino.h>
#include <Display.h>
#include "lvgl_rt1176.h"
#include "lvgl_mipi_panel.h"

/* The HX8394 tap window (QEMU-only; reads as 0 on silicon).  Address and
 * TAP_ID protocol: transcribe the exact constants and the TAP_ID guard from
 * examples/display/rk055_panel_test/rk055_panel_test.cpp -- same oracle,
 * same rules (branch on TAP_ID, never assume the tap exists). */
/* <implementer: copy the tap constants/read helpers from rk055_panel_test> */

static constexpr int32_t  BOX_W = 120, BOX_H = 120;
static constexpr int32_t  BOX_STEP = 12;        /* px per frame */
static constexpr uint32_t TOTAL_FRAMES = 120;   /* 2 full sweeps of 720 px */
static constexpr uint32_t FRAME_TIMEOUT_MS = 2000;

static lv_obj_t *s_box;
static int32_t   s_box_x = 0;

static void build_scene()
{
    lv_obj_t *scr = lv_screen_active();
    lv_obj_remove_flag(scr, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_color(scr, lv_color_hex(0x101820), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, LV_PART_MAIN);

    s_box = lv_obj_create(scr);
    lv_obj_set_size(s_box, BOX_W, BOX_H);
    lv_obj_set_pos(s_box, 0, (int32_t)(PANEL_HEIGHT / 2) - BOX_H / 2);
    lv_obj_set_style_bg_color(s_box, lv_color_hex(0xE0A030), LV_PART_MAIN);
}

/* Advance the box one step and pump LVGL until exactly one more full refresh
 * (== one more flip) has happened.  False on timeout. */
[[nodiscard]] static bool renderFrame()
{
    s_box_x = (s_box_x + BOX_STEP) % (int32_t)(PANEL_WIDTH - BOX_W);
    lv_obj_set_x(s_box, s_box_x);
    const uint32_t want = lvgl_mipi_panel_flips() + 1;
    const uint32_t deadline = millis() + FRAME_TIMEOUT_MS;
    while ((int32_t)(millis() - deadline) < 0) {
        lvgl_rt1176_loop();
        if (lvgl_mipi_panel_flips() >= want) return true;
    }
    return false;
}

/* FNV-1a over a whole framebuffer, matching the tap's arithmetic. */
static uint32_t fb_sum(const uint16_t *fb)
{
    lvgl_sum_reset();
    lvgl_sum_feed(fb, PANEL_FB_BYTES);
    return lvgl_sum_value();
}

void setup()
{
    Serial1.begin(115200);
    delay(200);
    Serial1.println("LVGL_RK055_FLIP_BEGIN");

    const bool ok = Display.begin();
    Serial1.println(ok ? "PANEL_OK" : "PANEL_FAIL");
    if (!ok) { Serial1.println("LVGL_RK055_FLIP_DONE"); return; }
    Display.fillScreen(0x0000);

    lvgl_rt1176_begin();
#if defined(FLIP_DEMO_SINGLE)
    /* The bench "before": same animation, v1 single-buffer direct render,
     * tearing accepted and expected.  No flip assertions are possible or
     * attempted -- the counters below would all read 0. */
    lvgl_mipi_panel_create(Display);
    Serial1.println("MODE=SINGLE_BUFFER_DEMO");
#else
    lvgl_mipi_panel_create_db(Display);
    Serial1.println("MODE=DOUBLE_BUFFER");
#endif
    build_scene();

    bool pass = true;

#if !defined(FLIP_DEMO_SINGLE)
    /* --- frames 1 and 2: the panel-scanned-this-buffer proof ------------- */
    uint32_t sumA = 0, sumB = 0;
    for (uint8_t f = 0; f < 2 && pass; f++) {
        if (!renderFrame()) {
            Serial1.printf("FLIP_FAIL frame=%u reason=refresh-timeout\n", f + 1u);
            pass = false;
            break;
        }
        lvgl_mipi_panel_flip_sync();   /* the flip must LAND before we look */
        const uint16_t *fb = lvgl_mipi_panel_scanned_fb();
        const uint32_t fw = fb ? fb_sum(fb) : 0;
        /* <implementer: read PANEL_SUM via the tap, guarded by TAP_ID as in
         *  rk055_panel_test; on hardware print ..._HW=TAP_ABSENT instead of
         *  asserting -- the tap is emulator fiction> */
        const uint32_t tap = 0; /* <- replace with the guarded tap read */
        const char *which = (f == 0) ? "A" : "B";
        Serial1.printf("FLIP_%s_SUM=0x%08lX PANEL_%s_SUM=0x%08lX\n",
                       which, (unsigned long)fw, which, (unsigned long)tap);
        if (f == 0) sumA = fw; else sumB = fw;
        /* MATCH/MISMATCH token per frame; MISMATCH clears `pass`. */
    }
    if (pass && sumA == sumB) {
        Serial1.println("FLIP_FAIL reason=frames-identical (vacuous alternation)");
        pass = false;
    } else if (pass) {
        Serial1.println("DISTINCT=OK");
    }
#endif

    /* --- the remaining frames: discipline counters ------------------------ */
    uint32_t frames_done = lvgl_mipi_panel_flips();
    while (pass && frames_done < TOTAL_FRAMES) {
        if (!renderFrame()) {
            Serial1.printf("FLIP_FAIL frame=%lu reason=refresh-timeout\n",
                           (unsigned long)(frames_done + 1));
            pass = false;
            break;
        }
        frames_done = lvgl_mipi_panel_flips();
    }
    lvgl_mipi_panel_flip_sync();

    Serial1.printf("REFRESHES=%lu\n", (unsigned long)frames_done);
    Serial1.printf("FLIPS=%lu\n", (unsigned long)lvgl_mipi_panel_flips());
    Serial1.printf("VSYNCS=%lu\n", (unsigned long)lvgl_mipi_panel_vsyncs());
    Serial1.printf("VSYNC_TIMEOUTS=%lu\n",
                   (unsigned long)lvgl_mipi_panel_vsync_timeouts());
    if (pass && lvgl_mipi_panel_vsync_timeouts() == 0) {
        Serial1.println("FLIP_OK");
    }
    Serial1.println("LVGL_RK055_FLIP_DONE");
}

/* Keep sweeping for the bench eye -- this loop is the hardware demonstration.
 * millis-paced so QEMU's virtual clock and silicon behave alike. */
void loop()
{
    static uint32_t last = 0;
    if (millis() - last >= 33) {
        last = millis();
        s_box_x = (s_box_x + BOX_STEP) % (int32_t)(PANEL_WIDTH - BOX_W);
        lv_obj_set_x(s_box, s_box_x);
    }
    lvgl_rt1176_loop();
}
```

Two `<implementer:>` markers above are deliberate: the tap constants and the guarded
TAP_ID/PANEL_SUM read must be transcribed from `rk055_panel_test.cpp` (the authoritative
in-tree user of the tap) rather than duplicated here from memory — copy that example's
constants, read helper, and its `TAP_ABSENT` hardware branch, and wire the MATCH/MISMATCH
print where the placeholder `tap` sits. Everything else is verbatim deliverable.

- [ ] **Step 4: Build**

```bash
cd ~/Development/rt1170/evkb/examples/display/lvgl_rk055_flip_test
cmake -B build -DCMAKE_TOOLCHAIN_FILE=toolchain/rt1170-evkb.toolchain.cmake
cmake --build build
```

Expected: clean link of `lvgl_rk055_flip_test.elf` + `.hex`.

---

## Task 5: The gate, then two deliberate breaks (F3c)

**Files:**
- Create: `examples/display/lvgl_rk055_flip_test/run_qemu.sh`
- Create: `examples/display/lvgl_rk055_flip_test/transcript_qemu.txt`

- [ ] **Step 1: `run_qemu.sh`** — the sibling gates' shell skeleton (`gate-lib.sh`,
`qrun`, poll for `LVGL_RK055_FLIP_DONE`, ceiling 80×0.25 s), then:

```sh
grep -q "PANEL_OK"            "$OUT" || { echo "FAIL: panel bring-up"; exit 1; }
grep -q "MODE=DOUBLE_BUFFER"  "$OUT" || { echo "FAIL: wrong build variant in the gate"; exit 1; }
# THE CORE CLAIM -- the panel SCANNED buffer A, then buffer B (model latches
# the flip at vsync; the tap sums the latched address).  MATCH is printed by
# firmware only when fw-sum == tap-sum for that frame.
grep -q "FLIP_A=MATCH" "$OUT" || { echo "FAIL: panel did not scan buffer A"; exit 1; }
grep -q "FLIP_B=MATCH" "$OUT" || { echo "FAIL: panel did not scan buffer B"; exit 1; }
# Vacuity guard: identical frames would make the alternation proof unfalsifiable.
grep -q "DISTINCT=OK" "$OUT" || { echo "FAIL: frames identical -- alternation unproven"; exit 1; }
# Discipline: one flip per refresh, every landing consumed once, no dead waits.
grep -q "^REFRESHES=120$"      "$OUT" || { echo "FAIL: refresh count"; exit 1; }
grep -q "^FLIPS=120$"          "$OUT" || { echo "FAIL: flip count"; exit 1; }
grep -q "^VSYNCS=120$"         "$OUT" || { echo "FAIL: vsync count"; exit 1; }
grep -q "^VSYNC_TIMEOUTS=0$"   "$OUT" || { echo "FAIL: a vsync wait gave up"; exit 1; }
grep -q "FLIP_OK"              "$OUT" || { echo "FAIL: firmware verdict withheld"; exit 1; }
[ -f "$DIR/lvgl_rk055_flip.dbg" ] || { echo "FAIL: no guest-error log"; exit 1; }
grep -q "guest" "$DIR/lvgl_rk055_flip.dbg" && { echo "FAIL: guest errors logged"; exit 1; }
```

(The exact MATCH token spelling follows what Task 4's tap wiring prints — keep firmware and
gate agreeing; the counter pins are exact because the flow is ack-driven and deterministic.
If the first run shows stable values other than 120/120/120, re-derive — `TOTAL_FRAMES` plus
the two assertion frames — fix firmware or pin together, and say which in the commit.)

- [ ] **Step 2: First run.** `chmod +x run_qemu.sh`, uptime/ps check, `./run_qemu.sh` —
expected green (no goldens to record). Run twice; the counters must be byte-identical.

- [ ] **Step 3: Negative test 1 — flip to the STALE buffer.** In
`~/Development/LVGL/port/lvgl_mipi_panel.cpp`, temporarily make `db_flush_cb` flip to the
buffer LVGL is about to render next instead of the one it just finished:

```cpp
        static const uint16_t *prev = nullptr;                 /* TEMPORARY */
        lcdifv2FlipTo(prev ? prev : (const uint16_t *)px_map); /* TEMPORARY */
        prev = (const uint16_t *)px_map;                       /* TEMPORARY */
```

Rebuild the example, run the gate. **Expected: red at `FLIP_B=MATCH`** (frame A may
coincidentally match — both buffers start similar — but B's tap sum is a stale frame).
If green, STOP: the tap/latch path is not proving what §6 claims. Revert.

- [ ] **Step 4: Negative test 2 — identical frames.** In the example, temporarily set
`BOX_STEP = 0`. Rebuild, run. **Expected: red at `DISTINCT=OK`** (the vacuity guard doing
its job). Revert, rebuild.

- [ ] **Step 5: Final green ×1, transcript, commit (evkb).** Verify
`git -C ~/Development/LVGL diff` is empty. `cp lvgl_rk055_flip.uart transcript_qemu.txt`.

```bash
cd ~/Development/rt1170/evkb
git add examples/display/lvgl_rk055_flip_test
git commit -m "lvgl_rk055_flip_test: the panel scanned buffer A, then B

Double-buffered LVGL animation; the gate proves flip discipline and --
via the model's vsync-latched shadow load -- that the virtual panel
actually scanned each buffer in turn.  Negative tests: flipping to the
stale buffer red at FLIP_B, identical frames red at DISTINCT."
```

Record both negative-test outcomes in the commit body (as v3 did).

---

## Task 6: Hardware — the eye on the sweep (F4)

**Files:**
- Create: `examples/display/lvgl_rk055_flip_test/transcript_hw_evkb.txt`

- [ ] **Step 1: The "before".** Build the single-buffer variant and flash it (standard
VCOM-free ritual; the operator should be present — the demonstration is visual):

```bash
cd ~/Development/rt1170/evkb/examples/display/lvgl_rk055_flip_test
cmake -B build-single -DCMAKE_TOOLCHAIN_FILE=toolchain/rt1170-evkb.toolchain.cmake -DFLIP_DEMO_SINGLE=ON
cmake --build build-single
# flash build-single/lvgl_rk055_flip_test.elf, attach console, reset
```

Operator watches the sweeping box for ~30 s and describes what they see (expected: tearing/
slicing on the moving edge — this is v1's accepted artefact, now on display deliberately).
Record their words verbatim.

- [ ] **Step 2: The "after".** Flash the normal double-buffer build
(`build/lvgl_rk055_flip_test.elf`), console attached first this time — capture the token
stream (`MODE=DOUBLE_BUFFER`, both `FLIP_*_SUM` lines with `PANEL_*_HW=TAP_ABSENT`, the
counters, `FLIP_OK`). Operator watches the same sweep: expected **no tearing**. Record
verbatim. `VSYNC_TIMEOUTS=0` on silicon is the first hardware proof of the INT_STATUS
polling path.

- [ ] **Step 3: Transcript + commit.** `transcript_hw_evkb.txt` in the house format: date,
board, firmware SHAs (evkb + MipiDisplay + LVGL + qemu2), both runs' captures, both operator
statements, and the explicit claim table (what silicon proved: no tearing, real cadence,
INT_STATUS on silicon; what it cannot see: the tap sums). Then:

```bash
rm -rf build-single
cd ~/Development/rt1170/evkb
git add examples/display/lvgl_rk055_flip_test/transcript_hw_evkb.txt
git commit -m "lvgl_rk055_flip_test: hardware evidence -- tearing before, none after"
```

---

## Task 7: Wrap (F5)

- [ ] **Step 1: Licence audit red-then-green.** `./tools/license-audit.sh` — expect red
naming the new gate; add to `GATES` (alphabetical position):

```
examples/display/lvgl_rk055_flip_test:lvgl_rk055_flip_test \
```

Re-run: green.

- [ ] **Step 2: Push and pin.** MipiDisplay: push master, update its SHA in `evkb.cmake`
(from `931ed1f4…`). LVGL: push master, update its SHA (from `9a2e9b75…`). qemu2: push
master. Then the force-fetch proof against the flip test:

```bash
cd ~/Development/rt1170/evkb/examples/display/lvgl_rk055_flip_test
cmake -B build-fetch -DCMAKE_TOOLCHAIN_FILE=toolchain/rt1170-evkb.toolchain.cmake -DEVKB_FORCE_FETCH=ON
cmake --build build-fetch && rm -rf build-fetch
```

- [ ] **Step 3: Docs.** `CLAUDE.md` sweep line **70 → 71** (`71/0/0` or `70/1/0`,
`cm4_audio_test` wording untouched); dated note in `docs/KNOWN-BROKEN-GATES.md`; a row for
the flip test in `README.md` (double buffering + vsync flip, HW-verified, tearing
before/after) and `examples/README.md`.

- [ ] **Step 4: Full sweep.** uptime/ps first. Expected **71 passed, 0 failed, 0 SKIP** or
**70/1/0** with the one failure `cm4_audio_test`. Anything else: a real regression, stop.

- [ ] **Step 5: Commit.**

```bash
git add tools/license-audit.sh evkb.cmake CLAUDE.md docs/KNOWN-BROKEN-GATES.md README.md examples/README.md
git commit -m "wrap v4: flip-test gate in the audit, three pin bumps, sweep 70 -> 71"
```

- [ ] **Step 6: Memory.** Update `rt1176-lvgl-touch-v3-design`'s "next" line (v4 shipped)
and write a new `rt1176-lvgl-double-buffer` project memory: the latch model change, the
buffer-order and FlipTo-before-Arm load-bearing details, the negative-test tokens, what the
bench eye saw before/after, and that the vsync-ISR brief
(`docs/superpowers/next-session-lvgl-vsync-isr-brainstorm.md`) is the successor session —
**do not delete that brief; it belongs to the next session.**

---

## Self-review notes (kept for the executor)

- **Spec coverage:** §3.1→Task 2; §3.2/3.3→Task 3; §4→Task 4; §5→Task 1 (with the six-gate
  regression duty); §6→Tasks 4–5 (tokens, vacuity guard, negative tests); §7 F1–F5→Tasks
  1–7; §10 open questions resolved: Q1 geometry fixed in Task 4 (120 px box, step 12,
  120 frames = 2 sweeps; consecutive frames always differ because the box moves 12 px),
  Q2 bounded wait + `VSYNC_TIMEOUTS` counter asserted 0, Q3 the existing continuous tap
  suffices once scanout follows the latched address (F1's design) — the implementer
  re-confirms against `imxrt_hx8394.c` while wiring the tap read.
- **Known deliberate non-verbatim spots:** the two `<implementer:>` tap markers in Task 4
  (transcribe from `rk055_panel_test.cpp`, the authoritative tap consumer) and the exact
  MATCH token spelling (firmware and gate must agree; Task 5 Step 1 says so). Everything
  else is verbatim deliverable.
- **Spec §5 wording nuance:** the spec says CTRLDESCL4 writes between pulse and vsync "do
  not reach the scanned address" — precisely: not *before* the vsync; the latch copies the
  shadow **as it stands at the vsync**. Task 1 Step 3's comment states the exact semantics;
  the spec sentence is amended alongside this plan's commit.
