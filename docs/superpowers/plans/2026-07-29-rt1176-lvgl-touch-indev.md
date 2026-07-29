# LVGL `lv_indev` over the GT911 (v3) — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Bind the GT911 to LVGL as an `lv_indev` so a finger drives real LVGL widgets on the
RK055 panel, proven by a QEMU gate that asserts LVGL's *reaction* (widget state and position)
and a hardware run with a real finger.

**Architecture:** Two new examples (`lvgl_rk055_panel_test` — static render golden;
`lvgl_rk055_touch_test` — the reactive touch gate), one new binding
(`LVGL/port/lvgl_gt911_indev.{h,cpp}`, taking `GT911&`), and a strictly-more-faithful extension
of the QEMU GT911 model's phase-3 script. Spec:
`docs/superpowers/specs/2026-07-29-rt1176-lvgl-touch-indev-design.md` — **read it first**, the
"why" of every decision below lives there.

**Tech stack:** LVGL 9.4 (vendored, `~/Development/LVGL`), TouchPanel `gt911` driver
(`~/Development/TouchPanel`), MipiDisplay `panels/rk055`, qemu2 (`~/Development/qemu2`), CMake +
ARM GCC 10, LinkServer for hardware.

**Read before starting:** repo `CLAUDE.md` (two-gate rule, flashing ritual, sweep rules),
`~/Development/TouchPanel/gt911/gt911.h` (the driver's contract — the binding is a caller of
`read()` and every "FOUR THINGS THAT WILL BITE" item applies),
`~/Development/qemu2/include/hw/i2c/imxrt_gt911.h` ("WHAT IT CANNOT PROVE").

**Conventions that apply to every task:**
- Run gates as `./run_qemu.sh`, never `sh run_qemu.sh`.
- Before every gate run: `uptime` and `ps aux | grep -E 'qemu-system|run_qemu' | grep -v grep`
  — a starved host or competing runner mimics regressions.
- Three repos are touched: `evkb` (this repo), `~/Development/LVGL`, `~/Development/qemu2`.
  Each gets its own commits. Never mix repos in one commit.
- All firmware output goes to `Serial1` (LPUART; QEMU captures it). Keep every `printf` format
  short enough that its widest substitution fits 127 bytes (`Print::printf` truncates silently).

---

## Task 1: `lvgl_rk055_panel_test` — the static example (M1)

The first LVGL render on the RK055, mirroring `lvgl_rpi_panel_test`'s shape. Static scene —
no animation, no time-derived text — so the render checksum is a stable golden.

**Files:**
- Create: `examples/display/lvgl_rk055_panel_test/CMakeLists.txt`
- Create: `examples/display/lvgl_rk055_panel_test/lvgl_rk055_panel_test.cpp`
- Create: `examples/display/lvgl_rk055_panel_test/toolchain/` (copied)
- Create: `examples/display/lvgl_rk055_panel_test/run_qemu.sh` (Task 2)

- [ ] **Step 1: Create the directory and copy the toolchain**

```bash
cd ~/Development/rt1170/evkb/examples/display
mkdir -p lvgl_rk055_panel_test
cp -r lvgl_rpi_panel_test/toolchain lvgl_rk055_panel_test/toolchain
```

- [ ] **Step 2: Write `CMakeLists.txt`**

The RK055 needs no Wire (three GPIOs, no ATtiny — contrast the RPi panel). PXP is needed by
`Display::fillScreen()`.

```cmake
cmake_minimum_required(VERSION 3.24)
project(lvgl_rk055_panel_test)

set(TEENSY_VERSION 117 CACHE STRING "")

include(${CMAKE_CURRENT_LIST_DIR}/../../../evkb.cmake)

import_evkb_lvgl()
# panels/rk055 selects the panel; the RT1176's single MIPI-DSI host means
# exactly one panel directory may ever be on the include path.
import_evkb_library(MipiDisplay soc panels/rk055)
import_evkb_library(PXP)    # Display::fillScreen() paints via the PXP

evkb_library_dir(LVGL _lvgl_dir)

teensy_add_executable(lvgl_rk055_panel_test
    lvgl_rk055_panel_test.cpp
    ${_lvgl_dir}/port/lvgl_mipi_panel.cpp)
teensy_target_link_libraries(lvgl_rk055_panel_test cores MipiDisplay PXP)

# LVGL is a plain CMake static-lib target (like CMSIS-DSP), not a
# teensy_add_library() one: teensy_target_link_libraries() rewrites each name to
# <name>.o and would not propagate LVGL's PUBLIC include dirs. Link it directly,
# exactly as lvgl_rpi_panel_test does.
target_link_libraries(lvgl_rk055_panel_test.elf LVGL stdc++)
```

- [ ] **Step 3: Write `lvgl_rk055_panel_test.cpp`**

```cpp
/* lvgl_rk055_panel_test - LVGL 9.4 widgets on the RK055HDMIPI4MA0
 * (720x1280 portrait RGB565, DIRECT render into the live LCDIFv2 scanout
 * buffer). Copyright (c) 2026 Nicholas Newdigate
 * SPDX-License-Identifier: MIT
 *
 * The scene is deliberately STATIC: no animation, no time-derived text, bitmap
 * fonts only.  LVGL's software renderer is deterministic, so the FNV-1a of the
 * pixels it produces is a stable golden value.
 *
 * UNLIKE lvgl_rpi_panel_test, whose golden pins REPRODUCIBILITY only (that
 * panel is disconnected and hardware-blocked), THIS golden is confirmed by a
 * human eye on real glass -- the first MIPI-DSI LVGL golden in the tree with
 * that property.  See transcript_hw_evkb.txt.
 *
 * What each layer proves:
 *   PANEL_OK      the RK055 chain (PLL_528 roots + LCDIFv2 + MIPI-DSI +
 *                 HX8394) came up -- rk055_panel_test pins the transport in
 *                 detail; here it is a precondition, because in DIRECT mode a
 *                 framebuffer no display owns would still checksum perfectly.
 *   LVGL_*        the renderer drew the expected pixels into that framebuffer.
 *
 * NOTE: uses Serial1 (the LPUART console run_qemu.sh captures via
 * `-serial file:`), not Serial (native USB CDC), like every sibling gate.
 */
#include <Arduino.h>
#include "Display.h"
#include "lvgl_rt1176.h"
#include "lvgl_mipi_panel.h"

/* No LVGL draw buffers are declared here, on purpose: DIRECT mode renders into
 * the panel's own scanout framebuffer, which MipiDisplay already allocated.
 * See lvgl_mipi_panel.h. */

static void build_scene()
{
    lv_obj_t *scr = lv_screen_active();
    /* Opaque background: forces LVGL to paint every pixel of the first
     * refresh, so the checksum covers a fully-defined frame. */
    lv_obj_set_style_bg_color(scr, lv_color_hex(0x101820), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, LV_PART_MAIN);

    lv_obj_t *title = lv_label_create(scr);
    lv_label_set_text(title, "RT1176 + LVGL");
    lv_obj_set_style_text_font(title, &lv_font_montserrat_28, LV_PART_MAIN);
    lv_obj_set_style_text_color(title, lv_color_hex(0xFFFFFF), LV_PART_MAIN);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 80);

    lv_obj_t *sub = lv_label_create(scr);
    lv_label_set_text(sub, "RK055HDMIPI4MA0 720x1280 MIPI-DSI");
    lv_obj_set_style_text_font(sub, &lv_font_montserrat_14, LV_PART_MAIN);
    lv_obj_set_style_text_color(sub, lv_color_hex(0x9FD4FF), LV_PART_MAIN);
    lv_obj_align(sub, LV_ALIGN_TOP_MID, 0, 132);

    lv_obj_t *bar = lv_bar_create(scr);
    lv_obj_set_size(bar, 520, 32);
    lv_obj_align(bar, LV_ALIGN_CENTER, 0, 40);
    lv_bar_set_value(bar, 62, LV_ANIM_OFF);   /* fixed value: no animation */
}

void setup()
{
    Serial1.begin(115200);
    delay(200);
    Serial1.println("LVGL_RK055_PANEL_BEGIN");

    const bool ok = Display.begin();
    Serial1.println(ok ? "PANEL_OK" : "PANEL_FAIL");
    if (!ok) {
        /* Safe-but-only-just: with no lv_init(), lv_timer_handler() in loop()
         * returns immediately (lv_timer_run is zero from static init).  Same
         * contract as lvgl_rpi_panel_test -- see its comment before "fixing". */
        Serial1.println("LVGL_RK055_PANEL_DONE");
        return;
    }

    /* Defined starting state for any pixel LVGL leaves unpainted. */
    Display.fillScreen(0x0000);

    lvgl_rt1176_begin();
    lvgl_mipi_panel_create(Display);
    build_scene();

    /* Render one full frame. */
    uint32_t t0 = millis();
    while (!lvgl_mipi_panel_frame_done() && (millis() - t0) < 5000) {
        lvgl_rt1176_loop();
    }

    /* DIRECT mode: hash the finished framebuffer in one go AFTER the refresh
     * (per-flush hashing would cover only dirty areas).  Same ordering as
     * lvgl_rpi_panel_test -- see its comment. */
    lvgl_sum_reset();
    lvgl_sum_feed(Display.framebuffer(), PANEL_FB_BYTES);

    Serial1.printf("LVGL_FLUSHED=%s\n", lvgl_mipi_panel_frame_done() ? "PASS" : "FAIL");
    /* LVGL_BYTES is the flushed AREA (x2 for bytes), not the feed extent --
     * 720*1280*2 = 1843200 only if LVGL redrew the whole screen.  The feed is
     * one unconditional PANEL_FB_BYTES call; printing lvgl_sum_bytes() would
     * assert a literal against itself. */
    Serial1.printf("LVGL_BYTES=%lu\n",
                   (unsigned long)(lvgl_mipi_panel_flushed_px() * PANEL_BYTES_PER_PIXEL));
    Serial1.printf("LVGL_SUM=0x%08lX\n", (unsigned long)lvgl_sum_value());
    Serial1.println("LVGL_RK055_PANEL_DONE");
}

/* Static scene: nothing re-invalidates, so the handler only services timers.
 * Keeps the scene live on the glass for the hardware confirmation. */
void loop() { lvgl_rt1176_loop(); }
```

- [ ] **Step 4: Build**

```bash
cd ~/Development/rt1170/evkb/examples/display/lvgl_rk055_panel_test
cmake -B build -DCMAKE_TOOLCHAIN_FILE=toolchain/rt1170-evkb.toolchain.cmake
cmake --build build
```

Expected: `lvgl_rk055_panel_test.elf` and `.hex` produced, zero errors.

---

## Task 2: The M1 QEMU gate, red-then-green golden recording

**Files:**
- Create: `examples/display/lvgl_rk055_panel_test/run_qemu.sh`
- Create: `examples/display/lvgl_rk055_panel_test/transcript_qemu.txt`

- [ ] **Step 1: Write `run_qemu.sh` with a deliberately-red golden**

`LVGL_SUM=0xRECORDME` cannot match, so the first run is red by construction — proving the
assertion fires before the value it pins is trusted.

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
ELF="$DIR/build/lvgl_rk055_panel_test.elf"; OUT="$DIR/lvgl_rk055_panel.uart"
rm -f "$OUT"
"$QEMU" -M mimxrt1170-evk -global fsl-imxrt1170.boot-xip=on -kernel "$ELF" \
    -display none -serial file:"$OUT" -d guest_errors -D "$DIR/lvgl_rk055_panel.dbg" &
P=$!; gate_pid $P
# 12s: panel bring-up (PLL roots + LCDIFv2 + MIPI-DSI + HX8394) plus a 720x1280
# software render -- the same cold-binary margin lvgl_rpi_panel_test uses for
# its 800x480.
sleep 12; gate_reap $P
gate_require_capture "$OUT"
echo "==== captured UART ===="; cat "$OUT"
# The panel chain itself must come up before LVGL means anything: in DIRECT
# mode LVGL renders straight into the LCDIFv2's live scanout buffer, so a
# framebuffer no display owns would still checksum perfectly.
grep -q "PANEL_OK"          "$OUT" || { echo "FAIL: panel bring-up"; exit 1; }
grep -q "LVGL_FLUSHED=PASS" "$OUT" || { echo "FAIL: no full refresh"; exit 1; }
# The flushed AREA -- 720*1280*2 -- is the only evidence LVGL redrew the WHOLE
# screen; a partial repaint and a scene edit both just change LVGL_SUM.
grep -q "LVGL_BYTES=1843200" "$OUT" || { echo "FAIL: wrong byte count"; exit 1; }
# GOLDEN CHECKSUM -- FNV-1a over the whole 720x1280 framebuffer after LVGL's
# software renderer drew the scene.  RECORDED, not derived; provenance below.
# UNLIKE the RPi gate's golden, this one is CONFIRMED ON GLASS by a human
# (transcript_hw_evkb.txt), so it pins correctness, not just reproducibility.
# On a mismatch work out WHICH of {LVGL pin, lv_conf.h, fonts, scene} changed;
# do NOT paste in whatever the board printed.  TO RE-RECORD: stable across two
# runs AND a human eye on the glass, in the SAME commit.
#
# Provenance: recorded YYYY-MM-DD against vendored LVGL 9.4.0, lv_conf.h as of
# that commit, montserrat 14/28.
grep -q "LVGL_SUM=0xRECORDME" "$OUT" || { echo "FAIL: render checksum"; exit 1; }
echo "PASS: LVGL RK055 panel render verified"
```

```bash
chmod +x run_qemu.sh
```

- [ ] **Step 2: Run the gate; verify it fails on the checksum, and capture the real value**

```bash
uptime; ps aux | grep -E 'qemu-system|run_qemu' | grep -v grep
./run_qemu.sh; echo "EXIT=$?"
grep "LVGL_SUM=" lvgl_rk055_panel.uart
```

Expected: `FAIL: render checksum`, `EXIT=1`, with `PANEL_OK`, `LVGL_FLUSHED=PASS` and
`LVGL_BYTES=1843200` all present in the capture and a concrete `LVGL_SUM=0x…` printed.
If `LVGL_BYTES` is not 1843200, stop — the scene did not repaint the whole screen (check the
opaque background) — do not proceed to record a golden over a partial repaint.

- [ ] **Step 3: Confirm the checksum is stable across a second run**

```bash
./run_qemu.sh || true
grep "LVGL_SUM=" lvgl_rk055_panel.uart
```

Expected: byte-identical `LVGL_SUM` to Step 2. If it differs, the scene is not deterministic —
find the nondeterminism (animation? time-derived text?) before continuing.

- [ ] **Step 4: Pin the golden, set the provenance date, and go green**

Edit `run_qemu.sh`: replace `0xRECORDME` with the recorded value and `YYYY-MM-DD` with today.

```bash
./run_qemu.sh; echo "EXIT=$?"
```

Expected: `PASS: LVGL RK055 panel render verified`, `EXIT=0`.

- [ ] **Step 5: Save the QEMU transcript and commit**

```bash
cp lvgl_rk055_panel.uart transcript_qemu.txt
cd ~/Development/rt1170/evkb
git add examples/display/lvgl_rk055_panel_test
git commit -m "lvgl_rk055_panel_test: first LVGL render on the RK055, QEMU golden recorded"
```

Note: `tools/license-audit.sh` part 2 will now fail (new gate not in `GATES`) — that is the
designed order; Task 9 adds the entries. Do not add them early: the audit going red first is
the proof the drift check works.

---

## Task 3: M1 hardware — the golden gets a human eye

**Files:**
- Create: `examples/display/lvgl_rk055_panel_test/transcript_hw_evkb.txt`

- [ ] **Step 1: Preconditions**

Confirm (do not assume): RK055 latched on `J48`; RPi 7" panel disconnected; nothing on D6/D9
or J25 odd pins 13/15 (no leftover `irq_attach_test` D13→D9 jumper).

- [ ] **Step 2: Flash — VCOM-free, then attach the console**

```bash
pkill LinkServer; pkill redlinkserv; pkill crt_emu_cm_redlink
cd ~/Development/rt1170/evkb/examples/display/lvgl_rk055_panel_test
/Applications/LinkServer_26.6.137/LinkServer flash MIMXRT1176:MIMXRT1170-EVKB load   build/lvgl_rk055_panel_test.elf
/Applications/LinkServer_26.6.137/LinkServer flash MIMXRT1176:MIMXRT1170-EVKB verify build/lvgl_rk055_panel_test.elf
```

Expected: both exit 0. Then attach the reader and reset (there is no standalone `reset`
subcommand; a backgrounded `run` is the reset):

```bash
ls /dev/cu.usbmodem*
python3 ../../../tools/rt1170-console.py /dev/cu.usbmodem<probe> 115200 > hw_run.log 2>&1 &
/Applications/LinkServer_26.6.137/LinkServer run MIMXRT1176:MIMXRT1170-EVKB build/lvgl_rk055_panel_test.elf > /dev/null 2>&1 &
sleep 8; cat hw_run.log
```

Expected: the same token sequence as QEMU — `PANEL_OK`, `LVGL_FLUSHED=PASS`,
`LVGL_BYTES=1843200`, and an `LVGL_SUM`. **The hardware `LVGL_SUM` must equal the QEMU
golden** — the renderer is deterministic CPU work; a mismatch means memory corruption or a
different binary, and must be understood, not recorded.

- [ ] **Step 3: The human confirmation — this step is the point of M1**

Ask the operator to confirm on the glass, and record their answer verbatim in the transcript:
dark blue-grey background; "RT1176 + LVGL" large white text near the top; light-blue subtitle
under it; a horizontal bar ~62 % filled at centre. Portrait orientation, text upright, no
skew, no tearing (scene is static).

- [ ] **Step 4: Write `transcript_hw_evkb.txt` and commit**

Follow the format of `examples/display/rk055_touch_test/transcript_hw_evkb.txt`: date, board,
setup, firmware SHAs, flash evidence (`LOAD/VERIFY` exit codes), the verbatim console capture,
and the operator's visual confirmation. Then:

```bash
pkill LinkServer; pkill redlinkserv; pkill crt_emu_cm_redlink
cd ~/Development/rt1170/evkb
git add examples/display/lvgl_rk055_panel_test/transcript_hw_evkb.txt
git commit -m "lvgl_rk055_panel_test: hardware evidence -- the golden confirmed on glass"
```

---

## Task 4: The QEMU script extension, proven harmless first (M2)

Add two id-1-only instants to the GT911 model's phase 3, then prove the **existing**
`rk055_touch_test` gate still passes before anything depends on the new instants.

**Files:**
- Modify: `~/Development/qemu2/hw/i2c/imxrt_gt911.c:56-59` (the script table's phase-3 tail)

- [ ] **Step 1: Extend the script**

In `gt911_script[]`, between the three two-contact instants and the final release, insert:

```c
    /* --- phase 3b: the PRIMARY lifts while id 1 remains ------------------- */
    /* Two instants carrying ONLY track id 1, at the same place it was.  This
     * is the seam a single-pointer consumer gets wrong: the contact that
     * remains must NOT become the pointer (no re-adoption until the panel is
     * clear), and nothing may assume slot 0 carries id 0.  Kept AFTER the
     * two-contact holds so consumers that stop at the holds (rk055_touch_test
     * stops consuming at its MULTI_OK) never reach these instants -- measured,
     * not assumed: that gate still pins BUFFERS=22. */
    { 1, {1, 0}, {75, 0}, {50, 0} },
    { 1, {1, 0}, {75, 0}, {50, 0} },
```

The resulting phase-3 block reads:

```c
    /* --- phase 3: two contacts, distinct IDs, well separated -------------- */
    { 2, {0, 1}, {25, 75}, {50, 50} },
    { 2, {0, 1}, {25, 75}, {50, 50} },
    { 2, {0, 1}, {25, 75}, {50, 50} },
    /* --- phase 3b: the PRIMARY lifts while id 1 remains ------------------- */
    /* (comment as above) */
    { 1, {1, 0}, {75, 0}, {50, 0} },
    { 1, {1, 0}, {75, 0}, {50, 0} },
    { 0, {0, 0}, {0, 0}, {0, 0} },
```

Script length goes 25 → 27 (`GT911_SCRIPT_LEN` is `ARRAY_SIZE`, self-adjusting).
`IMXRT_GT911_SCRIPT_POINTS` stays 2; the `QEMU_BUILD_BUG_ON` orderings are untouched.

- [ ] **Step 2: Rebuild QEMU**

```bash
ninja -C ~/Development/qemu2/build qemu-system-arm
```

Expected: clean build. (If ninja is not configured in that tree, `cd ~/Development/qemu2/build
&& make -j8 qemu-system-arm`.)

- [ ] **Step 3: Re-run the existing v2 gate against the extended script**

```bash
cd ~/Development/rt1170/evkb/examples/display/rk055_touch_test
uptime; ps aux | grep -E 'qemu-system|run_qemu' | grep -v grep
./run_qemu.sh; echo "EXIT=$?"
```

Expected: `EXIT=0`, and specifically `BUFFERS=22` still exact — the v2 firmware stops
consuming at its `MULTI_OK`, so the new tail instants are never reached. **If `BUFFERS` moved,
stop: the insertion point is wrong** (the new instants must come *after* all three two-contact
holds, or v2's phase 3 consumes an id-1-only instant and its `MULTI_OK` accounting shifts).

- [ ] **Step 4: Commit (qemu2 repo)**

```bash
cd ~/Development/qemu2
git add hw/i2c/imxrt_gt911.c
git commit -m "gt911: phase 3b -- the primary lifts while id 1 remains

Two scripted instants carrying only track id 1, between the two-contact
holds and the final release.  Exercises the no-re-adoption-until-clear
policy a single-pointer consumer needs, and that nothing assumes slot 0
carries id 0.  Strictly more faithful; rk055_touch_test still pins
BUFFERS=22 (measured) because it stops consuming at MULTI_OK."
```

---

## Task 5: The binding — `LVGL/port/lvgl_gt911_indev.{h,cpp}` (M3a)

**Files:**
- Create: `~/Development/LVGL/port/lvgl_gt911_indev.h`
- Create: `~/Development/LVGL/port/lvgl_gt911_indev.cpp`

- [ ] **Step 1: Write the header**

```cpp
/* lvgl_gt911_indev.h - LVGL pointer-input binding for the GT911 capacitive
 * touch controller (i.MX RT1176, TouchPanel library).  The first input
 * binding in this port; the FT5406 gets a sibling file when it arrives, and
 * any shared base is extracted then, from two real implementations.
 * SPDX-License-Identifier: MIT */
#pragma once
#include "gt911.h"
#include "lvgl_rt1176.h"

/* Creates an LV_INDEV_TYPE_POINTER over `touch` and binds it to `disp`.
 *
 * PRECONDITIONS (asserted, not merely documented):
 *   - touch.begin() has SUCCEEDED.  Checked via resolutionX/Y() != 0, which
 *     the driver guarantees exactly distinguishes a successful begin().
 *     read() before a successful begin() touches no bus and returns Failed,
 *     which this binding forwards as "no change" -- safe, but a permanently
 *     dead pointer, so it is refused loudly here instead.
 *   - lv_display_get_rotation(disp) == LV_DISPLAY_ROTATION_0.  In this port's
 *     configuration LVGL rotation CANNOT work (direct render + no matrix
 *     transform: LV_DRAW_TRANSFORM_USE_MATRIX is 0), and setting it would skew
 *     the renderer against the live scanout stride while this binding's
 *     mapping pointed somewhere else again.  QEMU can never catch either
 *     (model and firmware share the orientation assumption), so the failure
 *     is made loud at create time.  The day landscape is wanted, that is a
 *     display-binding milestone (v4+), not an edit to this assert.
 *
 * READ TIMING: sets this indev's read timer to 10 ms (a real GT911 publishes
 * at ~100 Hz).  Per-indev via lv_indev_get_read_timer() -- lv_conf.h's 33 ms
 * default is untouched, so no other example's timing or golden can shift.
 * At 33 ms the QEMU model (re-armed 20 ms after each ack) would have a fresh
 * buffer at EVERY poll and the Idle latch below would be dead code in the
 * gate; at 10 ms idle polls occur mid-drag, which is what makes the touch
 * gate's drag assertion able to fail.  See the spec, "the read period is a
 * verification decision".
 *
 * STATE MODEL (the spec's table; do not merge branches):
 *   Contacts  primary-contact policy (below), LV_INDEV_STATE_PRESSED
 *   Released  LV_INDEV_STATE_RELEASED, primary cleared
 *   Idle      the LATCHED previous state, unchanged -- "nothing new since the
 *             part was acknowledged" is not an event, and a poll loop outruns
 *             the part's publish rate, so Idle is the COMMON case
 *   Failed    ALSO the latched state -- a bus glitch is not a touch-up.
 *             (NXP's own binding forwards both Idle and Failed as RELEASED:
 *             fsl_gt911.c:261,290 + lvgl_support.c:575-580.  That is the bug
 *             this project has now recorded four instances of.)
 *
 * PRIMARY-CONTACT POLICY (one pointer from up to five contacts): the first
 * contact on an otherwise-clear panel becomes the primary, identified by its
 * TRACK ID (the driver sorts by id, so an array slot silently changes meaning
 * when a lower id arrives).  While present, its coordinates are the pointer;
 * other contacts are ignored.  When it disappears the pointer RELEASES, and
 * no new primary is adopted until a poll reports zero contacts.  This makes
 * the pointer-teleports-between-fingers artefact unrepresentable, at the
 * documented cost that after a multi-finger touch, ALL fingers must lift
 * before the next touch registers.
 *
 * COORDINATES are scaled by the resolution the part REPORTED
 * (resolutionX/Y()), never by an assumed 720x1280.  Scale only -- no swap, no
 * mirror: the identity mapping v2 proved with a finger on this panel.
 *
 * NOT REENTRANT, single instance: one GT911 on this board, module state like
 * the display bindings.  read_cb runs from lv_timer_handler() only. */
lv_indev_t *lvgl_gt911_indev_create(lv_display_t *disp, GT911 &touch);

/* Diagnostics since create() -- reset by create(), same style as the display
 * bindings' counters.  idle_polls is the touch gate's proof its Idle-latch
 * assertion is not vacuous (assert > 0); poll_fails must be 0 on a clean run
 * (QEMU cannot fault I2C, so any non-zero there is unmodelled behaviour);
 * buffers counts fresh buffers consumed (Contacts + Released) -- against a
 * scripted QEMU run it is exact and pins that every instant was consumed. */
uint32_t lvgl_gt911_idle_polls();
uint32_t lvgl_gt911_poll_fails();
uint32_t lvgl_gt911_buffers();
```

- [ ] **Step 2: Write the implementation**

```cpp
/* lvgl_gt911_indev.cpp - see lvgl_gt911_indev.h.
 * SPDX-License-Identifier: MIT */
#include <Arduino.h>
#include "lvgl_gt911_indev.h"

static GT911   *s_touch = nullptr;
static int32_t  s_hor = 0, s_ver = 0;     /* display resolution, cached at create() */

/* Latched pointer state -- what Idle and Failed forward unchanged. */
static bool       s_pressed = false;
static lv_point_t s_point   = {0, 0};

/* Primary-contact tracking (header: "PRIMARY-CONTACT POLICY"). */
static bool    s_have_primary = false;
static uint8_t s_primary_id   = 0;
static bool    s_wait_clear   = false;   /* primary lifted while others remained */

static uint32_t s_idle_polls = 0;
static uint32_t s_poll_fails = 0;
static uint32_t s_buffers    = 0;

/* Not volatile, same rationale as the display bindings: with LV_USE_OS ==
 * LV_OS_NONE everything here runs from lv_timer_handler() in one thread. */

static void gt911_read_cb(lv_indev_t *indev, lv_indev_data_t *data)
{
    (void)indev;
    TouchPoint pts[5];
    uint8_t n = 0;

    /* The four-way branch IS the design (spec table); do not merge arms.
     * Idle and Failed both fall through to the latched state below. */
    switch (s_touch->read(pts, 5, &n)) {
    case GT911::Poll::Idle:
        s_idle_polls++;
        break;
    case GT911::Poll::Failed:
        /* A bus glitch is not a touch-up.  The latched state stands. */
        s_poll_fails++;
        break;
    case GT911::Poll::Released:
        s_buffers++;
        s_pressed      = false;
        s_have_primary = false;
        s_wait_clear   = false;          /* the panel is clear: re-arm adoption */
        break;
    case GT911::Poll::Contacts:
        s_buffers++;
        if (s_have_primary) {
            bool found = false;
            for (uint8_t i = 0; i < n; i++) {
                if (pts[i].id == s_primary_id) {
                    /* Scale by what the part REPORTED; identity on this panel,
                     * but never assumed (v2 spec 5.4 discipline). */
                    s_point.x = (int32_t)((uint32_t)pts[i].x * (uint32_t)s_hor
                                          / s_touch->resolutionX());
                    s_point.y = (int32_t)((uint32_t)pts[i].y * (uint32_t)s_ver
                                          / s_touch->resolutionY());
                    found = true;
                    break;
                }
            }
            if (!found) {
                /* The primary lifted while other contacts remain: release, and
                 * adopt nothing until the panel reports clear.  This is the arm
                 * phase 3b of the QEMU script exists to reach. */
                s_pressed      = false;
                s_have_primary = false;
                s_wait_clear   = true;
            }
        } else if (!s_wait_clear && n > 0) {
            /* First contact on a clear panel: adopt as primary, by track id. */
            s_primary_id   = pts[0].id;
            s_have_primary = true;
            s_pressed      = true;
            s_point.x = (int32_t)((uint32_t)pts[0].x * (uint32_t)s_hor
                                  / s_touch->resolutionX());
            s_point.y = (int32_t)((uint32_t)pts[0].y * (uint32_t)s_ver
                                  / s_touch->resolutionY());
        }
        /* s_wait_clear && Contacts: surviving fingers are ignored entirely. */
        break;
    }

    data->state = s_pressed ? LV_INDEV_STATE_PRESSED : LV_INDEV_STATE_RELEASED;
    data->point = s_point;
    /* continue_reading stays false: the GT911 publishes nothing new until the
     * read above acknowledged it, so there is never a queue to drain. */
}

lv_indev_t *lvgl_gt911_indev_create(lv_display_t *disp, GT911 &touch)
{
    /* Both preconditions from the header, enforced loudly.  LV_ASSERT_HANDLER
     * prints LVGL_ASSERT! on Serial1 first (lvgl_rt1176_assert.h). */
    LV_ASSERT(touch.resolutionX() != 0 && touch.resolutionY() != 0);
    LV_ASSERT(lv_display_get_rotation(disp) == LV_DISPLAY_ROTATION_0);

    s_touch = &touch;
    s_hor = lv_display_get_horizontal_resolution(disp);
    s_ver = lv_display_get_vertical_resolution(disp);
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

uint32_t lvgl_gt911_idle_polls() { return s_idle_polls; }
uint32_t lvgl_gt911_poll_fails() { return s_poll_fails; }
uint32_t lvgl_gt911_buffers()    { return s_buffers; }
```

- [ ] **Step 3: Commit (LVGL repo)**

There is no standalone test for the binding — it compiles only against the core and is proven
by Task 7's gate (whose negative-test steps deliberately break this file and watch the gate go
red). Committing now gives Task 6 a stable file to build against.

```bash
cd ~/Development/LVGL
git add port/lvgl_gt911_indev.h port/lvgl_gt911_indev.cpp
git commit -m "port: lvgl_gt911_indev -- pointer indev over the GT911

First input binding in the port.  Latches across Idle AND Failed (a bus
glitch is not a touch-up -- NXP's reference forwards both as released),
primary-contact policy by track id with no re-adoption until the panel
is clear, coordinates scaled by the reported resolution, rotation-0
asserted at create, 10 ms per-indev read period."
```

---

## Task 6: `lvgl_rk055_touch_test` — the reactive example (M3b)

**Files:**
- Create: `examples/display/lvgl_rk055_touch_test/CMakeLists.txt`
- Create: `examples/display/lvgl_rk055_touch_test/lvgl_rk055_touch_test.cpp`
- Create: `examples/display/lvgl_rk055_touch_test/toolchain/` (copied)

- [ ] **Step 1: Directory and toolchain**

```bash
cd ~/Development/rt1170/evkb/examples/display
mkdir -p lvgl_rk055_touch_test
cp -r lvgl_rpi_panel_test/toolchain lvgl_rk055_touch_test/toolchain
```

- [ ] **Step 2: Write `CMakeLists.txt`**

```cmake
cmake_minimum_required(VERSION 3.24)
project(lvgl_rk055_touch_test)

set(TEENSY_VERSION 117 CACHE STRING "")

include(${CMAKE_CURRENT_LIST_DIR}/../../../evkb.cmake)

import_evkb_lvgl()
import_evkb_library(MipiDisplay soc panels/rk055)
import_evkb_library(PXP)              # Display::fillScreen() paints via the PXP
import_evkb_library(TouchPanel gt911) # controller chosen by the importer
import_evkb_library(Wire)             # LPI2C5 = Wire2, the panel/codec/touch bus

evkb_library_dir(LVGL _lvgl_dir)

teensy_add_executable(lvgl_rk055_touch_test
    lvgl_rk055_touch_test.cpp
    ${_lvgl_dir}/port/lvgl_mipi_panel.cpp
    ${_lvgl_dir}/port/lvgl_gt911_indev.cpp)
teensy_target_link_libraries(lvgl_rk055_touch_test cores MipiDisplay PXP TouchPanel Wire)

# Plain-target link for LVGL, exactly as the sibling LVGL gates do.
target_link_libraries(lvgl_rk055_touch_test.elf LVGL stdc++)

# GT911::read() is [[nodiscard]] and this tree carries no -Werror: without this
# a dropped poll result warns and the build SUCCEEDS.  Scoped to this target,
# same rationale as rk055_touch_test's identical line.
target_compile_options(lvgl_rk055_touch_test.elf
                       PRIVATE $<$<COMPILE_LANGUAGE:CXX>:-Werror=unused-result>)
```

- [ ] **Step 3: Write `lvgl_rk055_touch_test.cpp`**

```cpp
/* lvgl_rk055_touch_test - a finger drives LVGL widgets on the RK055 (v3).
 * Copyright (c) 2026 Nicholas Newdigate
 * SPDX-License-Identifier: MIT
 *
 * v2 (rk055_touch_test) proved COORDINATES.  This gate proves LVGL REACTED --
 * a different claim, and one a render checksum cannot make: a checksum cannot
 * tell "the widget moved because of a touch" from "the scene changed".  So
 * every widget here exists to make one specific wrong binding fail:
 *
 *   BTN1..5 (checkable, under the five scripted tap points)  all must end
 *       CHECKED.  A stuck, mirrored, swapped or unscaled coordinate checks at
 *       most one.  This is real LVGL hit-testing and click delivery.
 *   THE DRAG HANDLE  must end at the band's right (>= DRAG_END_MIN) having
 *       moved >= DRAG_MIN_MOVES times.  THE STAR WITNESS for the Idle latch:
 *       at the binding's 10 ms read period, Idle polls occur MID-DRAG by
 *       construction; forward one as "released" and the drag shatters into
 *       taps -- the handle never leaves the left edge and this gate goes red.
 *   HOLD (checkable, at the phase-3 primary)  must end CHECKED: the
 *       two-contact press reached LVGL and released cleanly.
 *   TRAP (checkable, at the phase-3 secondary)  must end UNCHECKED: the
 *       pointer never re-adopted the surviving finger.  VACUOUS ALONE -- a
 *       dead touch path also leaves it unchecked -- so it is only asserted
 *       as a PAIR with HOLD=CHECKED, which proves the phase-3 press arrived.
 *
 * The scene checksum here is DEMOTED: taken before the indev exists (safe:
 * the QEMU model stalls its script until the first buffer is acknowledged,
 * and nobody polls until the indev is created), it asserts "the scene built
 * correctly" and NOTHING about touch.  No post-touch checksum is asserted.
 *
 * TEARING is expected and accepted (spec 8.1): direct render, no back buffer,
 * no vsync fence -- a dragged widget may show a sliced edge for a frame on
 * real glass.  That is v1's documented trade, fixed by v4's double-buffer
 * milestone, NOT a bug in this example.  Do not scope a "flicker bug" here.
 *
 * QEMU vs HARDWARE: in QEMU the virtual GT911 replays a model-owned script
 * (5 taps, a 10-sample drag, two-contact holds, then phase 3b: the primary
 * lifts while id 1 remains).  The model places contacts at the same
 * percentages this file places widgets, so a green QEMU run proves the
 * binding and LVGL's delivery, NEVER the orientation -- only the hardware
 * run's finger settles that (same asymmetry as v2, stated not papered over).
 *
 * Uses Serial1 (LPUART; QEMU captures it), like every sibling gate.
 */
#include <Arduino.h>
#include <Wire.h>
#include <Display.h>
#include "gt911.h"
#include "lvgl_rt1176.h"
#include "lvgl_mipi_panel.h"
#include "lvgl_gt911_indev.h"

// D9 = GPIO_AD_01 = touch reset; D6 = GPIO_AD_00 = touch interrupt (owned by
// begin(); v3 never attaches an interrupt to it -- the two v2 INT findings
// stay open and stay out of this gate's failure domain).
static constexpr uint8_t TOUCH_RST_PIN = 9;
static constexpr uint8_t TOUCH_INT_PIN = 6;
static GT911 touch(Wire2, TOUCH_RST_PIN, TOUCH_INT_PIN);

// --- Geometry (spec 6.2) -----------------------------------------------------
// Everything is placed against the QEMU script's percentages of 720x1280:
// taps at (15,10) (85,10) (15,90) (85,90) (50,50) percent; the drag along
// y=50% from x=10% to 90%; phase-3 contacts at x=25% and 75%.
struct Rect { int16_t x, y, w, h; };
static constexpr Rect BTN_RECT[5] = {
    {   8,   48, 200, 160 },   // centre (108,128)  = tap 1
    { 512,   48, 200, 160 },   // centre (612,128)  = tap 2
    {   8, 1072, 200, 160 },   // centre (108,1152) = tap 3
    { 512, 1072, 200, 160 },   // centre (612,1152) = tap 4
    { 290,  570, 140, 140 },   // centre (360,640)  = tap 5
};
static constexpr Rect HANDLE_RECT = {  22, 570, 100, 140 };  // drag start x=72 inside
static constexpr Rect HOLD_RECT   = { 125, 570, 120, 140 };  // centre (185,640); contact (180,640)
static constexpr Rect TRAP_RECT   = { 485, 570, 110, 140 };  // centre (540,640); contact (540,640)

// The handle follows the pointer x; these two are what the gate asserts.
// QEMU: final drag sample at 90% = x=648, handle x = 648-50 = 598; 9 of the
// 10 samples change x (the first lands where the handle already is).
static constexpr int16_t DRAG_END_MIN   = 560;   // handle x >= this = "arrived"
static constexpr int32_t DRAG_MIN_MOVES = 8;

// COMPILE-TIME PROOF the band widgets are disjoint and the drag cannot brush
// TRAP.  Same discipline as v2's target-overlap assert: these are properties
// of the geometry that an edit destroys silently while every gate stays green
// (the model taps dead-centre wherever the rects now are).
static constexpr bool rectsDisjointX(const Rect &a, const Rect &b) {
    return (a.x + a.w <= b.x) || (b.x + b.w <= a.x);
}
static_assert(rectsDisjointX(HANDLE_RECT, HOLD_RECT) &&
              rectsDisjointX(HOLD_RECT, BTN_RECT[4]) &&
              rectsDisjointX(BTN_RECT[4], TRAP_RECT),
              "band widgets overlap: a tap or drag sample could hit two at once");
// The handle's final position (pointer 648 -> x 598) must clear TRAP's right
// edge, or the drag itself would end resting on the trap it must not touch.
static_assert(TRAP_RECT.x + TRAP_RECT.w <= 598,
              "the drag's end position overlaps TRAP");

// Phase patience: sized for a bench operator reading prompts (v2's rationale);
// QEMU's script finishes each phase in well under a second.
static constexpr uint32_t PHASE_TIMEOUT_MS = 60000;
// Quiet time with no fresh buffer before the final verdict is read: long
// enough that the tail of the script (2 x 20 ms steps) cannot still be in
// flight, short enough to cost nothing.
static constexpr uint32_t DRAIN_QUIET_MS = 500;

// --- Widgets -----------------------------------------------------------------
static lv_obj_t *s_btn[5];
static lv_obj_t *s_hold, *s_trap, *s_handle;
static int32_t   s_drag_moves = 0;

static void handle_pressing_cb(lv_event_t *e)
{
    lv_obj_t  *obj   = (lv_obj_t *)lv_event_get_target(e);
    lv_indev_t *indev = lv_event_get_indev(e);
    if (!indev) return;
    lv_point_t p;
    lv_indev_get_point(indev, &p);
    /* Centre the handle on the pointer x; y stays put.  Clamped to the panel
     * so a wild coordinate cannot park it off-screen. */
    int32_t nx = p.x - HANDLE_RECT.w / 2;
    if (nx < 0) nx = 0;
    if (nx > (int32_t)PANEL_WIDTH - HANDLE_RECT.w)
        nx = (int32_t)PANEL_WIDTH - HANDLE_RECT.w;
    if (nx != lv_obj_get_x(obj)) {
        lv_obj_set_x(obj, nx);
        s_drag_moves++;
    }
}

static lv_obj_t *make_checkable(const Rect &r, const char *text,
                                const lv_font_t *font)
{
    lv_obj_t *btn = lv_button_create(lv_screen_active());
    lv_obj_set_pos(btn, r.x, r.y);
    lv_obj_set_size(btn, r.w, r.h);
    lv_obj_add_flag(btn, LV_OBJ_FLAG_CHECKABLE);
    /* Explicit colours in BOTH states: deterministic pixels for the pre-touch
     * golden, and a visible flip for the bench operator. */
    lv_obj_set_style_bg_color(btn, lv_color_hex(0x1C2B3E), LV_PART_MAIN);
    lv_obj_set_style_bg_color(btn, lv_color_hex(0x2EA043),
                              LV_PART_MAIN | LV_STATE_CHECKED);
    lv_obj_t *label = lv_label_create(btn);
    lv_label_set_text(label, text);
    lv_obj_set_style_text_font(label, font, LV_PART_MAIN);
    lv_obj_center(label);
    return btn;
}

static void build_scene()
{
    lv_obj_t *scr = lv_screen_active();
    /* The drag must move the HANDLE, not scroll the screen. */
    lv_obj_remove_flag(scr, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_color(scr, lv_color_hex(0x101820), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, LV_PART_MAIN);

    static const char *NUM[5] = { "1", "2", "3", "4", "5" };
    for (uint8_t i = 0; i < 5; i++)
        s_btn[i] = make_checkable(BTN_RECT[i], NUM[i], &lv_font_montserrat_28);

    s_hold = make_checkable(HOLD_RECT, "HOLD", &lv_font_montserrat_14);
    s_trap = make_checkable(TRAP_RECT, "TRAP", &lv_font_montserrat_14);

    s_handle = lv_obj_create(scr);
    lv_obj_set_pos(s_handle, HANDLE_RECT.x, HANDLE_RECT.y);
    lv_obj_set_size(s_handle, HANDLE_RECT.w, HANDLE_RECT.h);
    lv_obj_remove_flag(s_handle, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_color(s_handle, lv_color_hex(0xB07020), LV_PART_MAIN);
    lv_obj_add_event_cb(s_handle, handle_pressing_cb, LV_EVENT_PRESSING, NULL);
}

// --- Phase driver ------------------------------------------------------------
// Watches are SEQUENTIAL and state-based: LVGL reacts whenever it reacts; the
// firmware only observes widget state and prints tokens on transitions.  The
// QEMU script fires the events in exactly this order, so the same watches
// serve both the model and a prompted bench operator.

// Pump LVGL until `obj` reports CHECKED or the phase times out.
[[nodiscard]] static bool waitChecked(lv_obj_t *obj, const char *tok)
{
    const uint32_t deadline = millis() + PHASE_TIMEOUT_MS;
    while ((int32_t)(millis() - deadline) < 0) {
        lvgl_rt1176_loop();
        if (lv_obj_has_state(obj, LV_STATE_CHECKED)) {
            Serial1.printf("%s=CHECKED\n", tok);
            return true;
        }
    }
    Serial1.printf("%s_FAIL reason=timeout\n", tok);
    return false;
}

[[nodiscard]] static bool phaseButtons()
{
    static const char *TOK[5] = { "BTN1", "BTN2", "BTN3", "BTN4", "BTN5" };
    for (uint8_t i = 0; i < 5; i++) {
        // Centre coordinates in the prompt, for the bench operator.
        Serial1.printf("TOUCH_PROMPT phase=A tap button %u at=(%d,%d)\n",
                       (unsigned)(i + 1),
                       (int)(BTN_RECT[i].x + BTN_RECT[i].w / 2),
                       (int)(BTN_RECT[i].y + BTN_RECT[i].h / 2));
        if (!waitChecked(s_btn[i], TOK[i])) return false;
    }
    return true;
}

[[nodiscard]] static bool phaseDrag()
{
    Serial1.println("TOUCH_PROMPT phase=B drag the amber handle to the right "
                    "edge of its band, one continuous stroke");
    const uint32_t deadline = millis() + PHASE_TIMEOUT_MS;
    while ((int32_t)(millis() - deadline) < 0) {
        lvgl_rt1176_loop();
        if (lv_obj_get_x(s_handle) >= DRAG_END_MIN && s_drag_moves >= DRAG_MIN_MOVES) {
            Serial1.printf("DRAG_END x=%ld moves=%ld\n",
                           (long)lv_obj_get_x(s_handle), (long)s_drag_moves);
            return true;
        }
    }
    Serial1.printf("DRAG_FAIL reason=timeout x=%ld moves=%ld\n",
                   (long)lv_obj_get_x(s_handle), (long)s_drag_moves);
    return false;
}

[[nodiscard]] static bool phaseHold()
{
    Serial1.println("TOUCH_PROMPT phase=C two fingers, one on HOLD one on "
                    "TRAP, together; lift the HOLD finger FIRST, then the other");
    return waitChecked(s_hold, "HOLD");
}

// Pump LVGL until no fresh buffer has arrived for DRAIN_QUIET_MS, so the
// script's tail (or the operator's remaining finger) cannot still be in
// flight when TRAP's final state is read.  Bounded by the phase timeout.
static void drainQuiet()
{
    const uint32_t deadline = millis() + PHASE_TIMEOUT_MS;
    uint32_t last_buffers = lvgl_gt911_buffers();
    uint32_t quiet_since  = millis();
    while ((int32_t)(millis() - deadline) < 0) {
        lvgl_rt1176_loop();
        if (lvgl_gt911_buffers() != last_buffers) {
            last_buffers = lvgl_gt911_buffers();
            quiet_since  = millis();
        } else if (millis() - quiet_since >= DRAIN_QUIET_MS) {
            return;
        }
    }
}

void setup()
{
    Serial1.begin(115200);
    delay(200);
    Serial1.println("LVGL_RK055_TOUCH_BEGIN");

    const bool disp_ok = Display.begin();
    Serial1.println(disp_ok ? "PANEL_OK" : "PANEL_FAIL");
    if (!disp_ok) { Serial1.println("LVGL_RK055_TOUCH_DONE"); return; }

    Display.fillScreen(0x0000);
    lvgl_rt1176_begin();
    lv_display_t *disp = lvgl_mipi_panel_create(Display);
    build_scene();

    /* Render and checksum the initial frame BEFORE the indev exists.  Safe by
     * construction: the QEMU model stalls its script until the first buffer
     * is acknowledged, and nobody polls until the indev is created below.
     * This golden asserts "the scene built correctly" -- nothing about touch,
     * and no post-touch checksum is asserted anywhere. */
    uint32_t t0 = millis();
    while (!lvgl_mipi_panel_frame_done() && (millis() - t0) < 5000) {
        lvgl_rt1176_loop();
    }
    lvgl_sum_reset();
    lvgl_sum_feed(Display.framebuffer(), PANEL_FB_BYTES);
    Serial1.printf("LVGL_FLUSHED=%s\n", lvgl_mipi_panel_frame_done() ? "PASS" : "FAIL");
    Serial1.printf("LVGL_BYTES=%lu\n",
                   (unsigned long)(lvgl_mipi_panel_flushed_px() * PANEL_BYTES_PER_PIXEL));
    Serial1.printf("LVGL_SUM=0x%08lX\n", (unsigned long)lvgl_sum_value());

    /* Touch bring-up -- v2's proven sequence.  The caller owns Wire2 (it is
     * also the codec's bus). */
    Wire2.begin();
    Wire2.setClock(400000);
    if (!touch.begin()) {
        Serial1.printf("TOUCH_FAIL err=%s i2c=%u id=0x%08lX\n",
                       GT911::errorName(touch.lastError()),
                       (unsigned)touch.lastI2cStatus(),
                       (unsigned long)touch.lastDeviceId());
        Serial1.println("LVGL_RK055_TOUCH_DONE");
        return;
    }
    Serial1.println("I2C_OK");
    Serial1.printf("ADDR=0x%02X\n", (unsigned)touch.address());
    Serial1.println("GT911_OK");
    Serial1.printf("CFG_OK RES=%ux%u POINTS=%u\n",
                   (unsigned)touch.resolutionX(), (unsigned)touch.resolutionY(),
                   (unsigned)touch.configuredPoints());

    /* From here LVGL polls the part every 10 ms and the script (in QEMU) or
     * the operator (on the bench) drives the widgets. */
    lvgl_gt911_indev_create(disp, touch);

    const bool a = phaseButtons();
    const bool b = a && phaseDrag();
    const bool c = b && phaseHold();
    drainQuiet();

    /* TRAP is read only after the drain, because a broken re-adoption checks
     * it at the script's FINAL release -- after HOLD=CHECKED. */
    const bool trap_clear = !lv_obj_has_state(s_trap, LV_STATE_CHECKED);
    Serial1.printf("TRAP=%s\n", trap_clear ? "UNCHECKED" : "CHECKED");
    Serial1.printf("IDLE_POLLS=%lu\n", (unsigned long)lvgl_gt911_idle_polls());
    Serial1.printf("POLL_FAILS=%lu\n", (unsigned long)lvgl_gt911_poll_fails());
    Serial1.printf("BUFFERS=%lu\n", (unsigned long)lvgl_gt911_buffers());

    if (a && b && c && trap_clear &&
        lvgl_gt911_idle_polls() > 0 && lvgl_gt911_poll_fails() == 0) {
        Serial1.println("LVGL_TOUCH_OK");
    }
    Serial1.println("LVGL_RK055_TOUCH_DONE");
}

/* The scene stays live; widget state persists for the bench operator. */
void loop() { lvgl_rt1176_loop(); }
```

- [ ] **Step 4: Build**

```bash
cd ~/Development/rt1170/evkb/examples/display/lvgl_rk055_touch_test
cmake -B build -DCMAKE_TOOLCHAIN_FILE=toolchain/rt1170-evkb.toolchain.cmake
cmake --build build
```

Expected: clean build. (A `-Werror=unused-result` failure means a `read()`/`waitChecked` result
was dropped — that error firing is the flag working, not a toolchain problem.)

---

## Task 7: The M3 QEMU gate — goldens, green, then two deliberate breaks

**Files:**
- Create: `examples/display/lvgl_rk055_touch_test/run_qemu.sh`
- Create: `examples/display/lvgl_rk055_touch_test/transcript_qemu.txt`

- [ ] **Step 1: Write `run_qemu.sh` with red placeholders**

Three measured values are pinned exactly in QEMU (`LVGL_SUM`, `DRAG_END`, `BUFFERS`); all start
as unmatchable placeholders so each assertion is seen red before its value is trusted.

```sh
#!/bin/sh
set -e
DIR=$(cd "$(dirname "$0")" && pwd)
EVKB=$(cd "$DIR/../../.." && pwd)
QEMU="$EVKB/tools/qrun"
. "$EVKB/tools/gate-lib.sh"
gate_init
ELF="$DIR/build/lvgl_rk055_touch_test.elf"; OUT="$DIR/lvgl_rk055_touch.uart"
rm -f "$OUT" "$DIR/lvgl_rk055_touch.dbg"
"$QEMU" -M mimxrt1170-evk -global fsl-imxrt1170.boot-xip=on -kernel "$ELF" \
    -display none -serial file:"$OUT" -d guest_errors -D "$DIR/lvgl_rk055_touch.dbg" &
P=$!; gate_pid $P
# Poll for the DONE token rather than burning a fixed window; ceiling 20 s
# (80 x 0.25).  A healthy run is done in ~6 s wall: panel bring-up + one
# 720x1280 render + the 27-instant script at 2 instants per ~40 ms.
i=0
while [ $i -lt 80 ]; do
    [ -f "$OUT" ] && grep -q "LVGL_RK055_TOUCH_DONE" "$OUT" 2>/dev/null && break
    sleep 0.25
    i=$((i+1))
done
gate_reap $P
gate_require_capture "$OUT"
echo "==== captured UART ===="; cat "$OUT"

# --- the scene, before any touch --------------------------------------------
grep -q "PANEL_OK"           "$OUT" || { echo "FAIL: panel bring-up"; exit 1; }
grep -q "LVGL_FLUSHED=PASS"  "$OUT" || { echo "FAIL: no full refresh"; exit 1; }
grep -q "LVGL_BYTES=1843200" "$OUT" || { echo "FAIL: wrong byte count"; exit 1; }
# Pre-touch golden: taken BEFORE the indev exists (the model stalls its script
# until the first ack, and nobody polls until the indev is created), so it is
# static on QEMU and glass alike.  It asserts the scene BUILT correctly and
# says nothing about touch -- no post-touch checksum exists in this gate.
# Provenance: recorded YYYY-MM-DD, LVGL 9.4.0, montserrat 14/28.  Re-record
# rules as in lvgl_rk055_panel_test/run_qemu.sh.
grep -q "LVGL_SUM=0xRECORDME" "$OUT" || { echo "FAIL: scene checksum"; exit 1; }

# --- touch bring-up (v2 owns the depth here; these are preconditions) --------
grep -q "I2C_OK"    "$OUT" || { echo "FAIL: touch bring-up"; exit 1; }
grep -q "ADDR=0x5D" "$OUT" || { echo "FAIL: wrong latched address"; exit 1; }
grep -q "GT911_OK"  "$OUT" || { echo "FAIL: product ID"; exit 1; }
grep -q "CFG_OK"    "$OUT" || { echo "FAIL: config blob"; exit 1; }

# --- LVGL's REACTION -- the claims this gate exists for ----------------------
# Five checkable buttons under the five scripted taps.  Real LVGL hit-testing
# and click delivery: a stuck/mirrored/swapped/unscaled coordinate checks at
# most one of them.
for t in 1 2 3 4 5; do
  grep -q "^BTN$t=CHECKED$" "$OUT" || { echo "FAIL: button $t never checked"; exit 1; }
done
# THE STAR WITNESS.  The handle follows the pointer only while LVGL believes
# the press CONTINUES across the Idle polls between drag samples (the binding
# runs at 10 ms; the model publishes every 20 ms).  Forward Idle as released
# and the handle stays at the left edge.  Pinned exactly: the script's last
# sample is x=648 -> handle 598; 9 of 10 samples change x (the first lands
# where the handle already is).  Legitimate changes to script or geometry
# re-derive this comment or the pin is meaningless.
grep -q "^DRAG_END x=598 moves=9$" "$OUT" || { echo "FAIL: drag did not track (Idle latch?)"; exit 1; }
# ASSERTED AS A PAIR, deliberately: TRAP=UNCHECKED alone is satisfied by a
# dead touch path.  HOLD=CHECKED proves the phase-3 press reached LVGL;
# TRAP=UNCHECKED then proves the pointer never re-adopted the finger that
# remained after the primary lifted (script phase 3b).
grep -q "^HOLD=CHECKED$"   "$OUT" || { echo "FAIL: phase-3 press never reached LVGL"; exit 1; }
grep -q "^TRAP=UNCHECKED$" "$OUT" || { echo "FAIL: pointer re-adopted the surviving finger"; exit 1; }

# --- honesty guards ----------------------------------------------------------
# IDLE_POLLS>0: the drag assertion above is about behaviour ACROSS idle polls;
# if timing drift ever removes them, this trips rather than letting the latch
# assertion pass vacuously (spec 8.2).
grep -q "^IDLE_POLLS=" "$OUT" || { echo "FAIL: idle-poll count missing"; exit 1; }
grep -q "^IDLE_POLLS=0$" "$OUT" && { echo "FAIL: no idle polls -- latch untested, re-tune the period"; exit 1; }
grep -q "^POLL_FAILS=0$" "$OUT" || { echo "FAIL: failed poll(s) -- QEMU cannot fault I2C, unmodelled"; exit 1; }
# Every scripted instant consumed, exactly: 10 taps+releases, 10 drag samples,
# 1 release, 3 two-contact holds, 2 phase-3b, 1 final release = 27.
grep -q "^BUFFERS=RECORDME$" "$OUT" || { echo "FAIL: buffer count moved -- script/phase boundary shifted"; exit 1; }
grep -q "LVGL_TOUCH_OK" "$OUT" || { echo "FAIL: firmware verdict withheld"; exit 1; }

# --- the model's own guards --------------------------------------------------
[ -f "$DIR/lvgl_rk055_touch.dbg" ] || { echo "FAIL: no guest-error log"; exit 1; }
if grep -q "gt911: guest wrote config" "$DIR/lvgl_rk055_touch.dbg"; then
  echo "FAIL: firmware wrote the GT911 config space"; exit 1
fi
echo "PASS: LVGL reacted to every scripted contact"
```

```bash
chmod +x run_qemu.sh
```

- [ ] **Step 2: Run red, record the three values, confirm stability**

```bash
uptime; ps aux | grep -E 'qemu-system|run_qemu' | grep -v grep
./run_qemu.sh; echo "EXIT=$?"     # expect FAIL: scene checksum, EXIT=1
grep -E "LVGL_SUM=|DRAG_END |BUFFERS=" lvgl_rk055_touch.uart
./run_qemu.sh || true              # second run
grep -E "LVGL_SUM=|DRAG_END |BUFFERS=" lvgl_rk055_touch.uart
```

Expected on both runs, identically: a concrete `LVGL_SUM`, `DRAG_END x=598 moves=9`,
`BUFFERS=27`, and all of `BTN1..5=CHECKED`, `HOLD=CHECKED`, `TRAP=UNCHECKED`,
`IDLE_POLLS=` non-zero, `POLL_FAILS=0`, `LVGL_TOUCH_OK` present in the capture.

If `DRAG_END` differs from `x=598 moves=9`: re-derive from the script (last sample 90 % of
720 = 648, minus half the 100 px handle = 598; 9 distinct x values after the first) — a
different stable value means the geometry or script changed, so fix the comment *and* the pin
together. If it is unstable across runs, stop and understand (it must be deterministic: the
script is ack-driven, the read period fixed).

- [ ] **Step 3: Pin the three values and go green**

Edit `run_qemu.sh`: `0xRECORDME` → recorded sum, `BUFFERS=RECORDME` → `BUFFERS=27` (the
recorded value), provenance date. Then:

```bash
./run_qemu.sh; echo "EXIT=$?"
```

Expected: `PASS: LVGL reacted to every scripted contact`, `EXIT=0`.

- [ ] **Step 4: Negative test 1 — forward Idle as Released; the gate must go red on the drag**

In `~/Development/LVGL/port/lvgl_gt911_indev.cpp`, temporarily change the Idle arm to the bug
this gate exists to catch:

```cpp
    case GT911::Poll::Idle:
        s_idle_polls++;
        s_pressed = false;          /* TEMPORARY: the recorded bug, do not commit */
        break;
```

```bash
cd ~/Development/rt1170/evkb/examples/display/lvgl_rk055_touch_test
cmake --build build && ./run_qemu.sh; echo "EXIT=$?"
```

Expected: **red**, `EXIT=1`, failing at `DRAG_END` (or the gate's poll ceiling if the drag
phase times out) — the taps still pass (press and release both arrive as published buffers),
which is exactly why the drag is the star witness. If this run is GREEN, stop everything: the
gate is vacuous and the design's central claim is untested.

- [ ] **Step 5: Negative test 2 — allow re-adoption; the gate must go red on TRAP**

Revert Step 4's edit first. Then temporarily remove the adoption guard:

```cpp
        } else if (n > 0) {          /* TEMPORARY: s_wait_clear guard removed */
```

```bash
cmake --build build && ./run_qemu.sh; echo "EXIT=$?"
```

Expected: **red**, `EXIT=1`, at `TRAP=UNCHECKED` — the binding re-adopts id 1 at the first
phase-3b instant, presses TRAP at (540,640), and the script's final release clicks it. This
is also the proof that Task 4's script extension has teeth. If green, the extension or the
pair-assertion is wrong; stop and understand.

- [ ] **Step 6: Revert, final green, transcript, commit**

Revert the Step 5 edit (verify `git -C ~/Development/LVGL diff` is empty), rebuild, re-run:

```bash
cmake --build build && ./run_qemu.sh; echo "EXIT=$?"    # expect green
cp lvgl_rk055_touch.uart transcript_qemu.txt
cd ~/Development/rt1170/evkb
git add examples/display/lvgl_rk055_touch_test
git commit -m "lvgl_rk055_touch_test: LVGL reacts to the scripted GT911 contacts

Five checkable buttons, a drag handle that is the Idle-latch's star
witness, and a HOLD/TRAP pair proving no re-adoption of a surviving
finger.  Both deliberate binding breaks (Idle->released, re-adoption)
measured red before this green was trusted."
```

Record in the commit body or transcript the two negative-test outcomes (which token each broke)
— that measurement is the gate's licence to be believed.

---

## Task 8: M4 hardware — a real finger drives the widgets

**Files:**
- Create: `examples/display/lvgl_rk055_touch_test/transcript_hw_evkb.txt`

- [ ] **Step 1: Preconditions** — as Task 3 Step 1, verbatim (J48, no RPi panel, D6/D9/J25 clear).

- [ ] **Step 2: Flash and run** — same ritual as Task 3 Step 2, with
`build/lvgl_rk055_touch_test.elf`. Attach the console *before* the reset (the bring-up tokens
matter), then follow the serial prompts on the bench:

1. Tap buttons 1–5 in the prompted order (each flips green and prints `BTNn=CHECKED`).
2. Drag the amber handle to the right end of its band in one continuous stroke.
3. Two fingers: one on HOLD, one on TRAP, together. **Lift the HOLD finger first**, then the
   other.

Expected transcript shape: all bring-up tokens (`PANEL_OK` … `CFG_OK RES=720x1280 POINTS=5`),
`BTN1..5=CHECKED`, a `DRAG_END` with `moves` far above 9 (a real finger samples 20–30+ times,
per v2's 23–34), `HOLD=CHECKED`, `TRAP=UNCHECKED`, `POLL_FAILS=0`, `LVGL_TOUCH_OK`.
`IDLE_POLLS` will be enormous (idle dominates on silicon); only non-zero matters.
The hardware `LVGL_SUM` must equal the QEMU golden (deterministic renderer, same binary).

**What only this run proves:** the coordinate→widget mapping/orientation (QEMU shares the
firmware's assumption by construction), and that a real hand's timing — slow drags, hesitant
lifts — survives the latch and the primary policy. Tearing on the dragged handle is expected
and accepted (spec §8.1); note what was actually seen.

- [ ] **Step 3: Write `transcript_hw_evkb.txt`** — same format as Task 3 Step 4, including the
operator's description of what the widgets visibly did (buttons flipping green under the
finger, the handle tracking, TRAP never flipping), and any tearing observed on the drag.

- [ ] **Step 4: Commit**

```bash
pkill LinkServer; pkill redlinkserv; pkill crt_emu_cm_redlink
cd ~/Development/rt1170/evkb
git add examples/display/lvgl_rk055_touch_test/transcript_hw_evkb.txt
git commit -m "lvgl_rk055_touch_test: hardware evidence -- a finger drives the widgets"
```

---

## Task 9: M5 wrap — audit entries, pins, docs, sweep, memory

- [ ] **Step 1: Watch the licence audit fail, then add the two GATES entries**

```bash
cd ~/Development/rt1170/evkb
./tools/license-audit.sh; echo "EXIT=$?"
```

Expected: **red**, naming both new examples as gates missing from `GATES` (the drift check).
Then edit `tools/license-audit.sh`, adding to the `GATES` list (alphabetical, next to the other
`examples/display` entries):

```
examples/display/lvgl_rk055_panel_test:lvgl_rk055_panel_test \
examples/display/lvgl_rk055_touch_test:lvgl_rk055_touch_test \
```

```bash
./tools/license-audit.sh; echo "EXIT=$?"
```

Expected: green, `EXIT=0`. (Part 2 needs every listed gate built — both new examples must
still have their `build/` from Tasks 1 and 6.)

- [ ] **Step 2: Push the LVGL repo and bump its pin**

```bash
cd ~/Development/LVGL
git log --oneline -3           # the binding commit from Task 5
git push origin master
git rev-parse HEAD
```

Edit `evkb.cmake:70`: replace the LVGL SHA (`6dbb5abe…`) with the new HEAD SHA. Then prove the
pin is buildable without the local checkout:

```bash
cd ~/Development/rt1170/evkb/examples/display/lvgl_rk055_touch_test
cmake -B build-fetch -DCMAKE_TOOLCHAIN_FILE=toolchain/rt1170-evkb.toolchain.cmake -DEVKB_FORCE_FETCH=ON
cmake --build build-fetch
rm -rf build-fetch
```

Expected: clean build from the pinned fetch ("fresh user" mode).

- [ ] **Step 3: Update the sweep baseline and docs**

- `CLAUDE.md`: the two-gate section's sweep expectation — `68 gates` → `70 gates`, and the
  expected outcomes `68 passed / 67 passed, 1 failed` → `70 passed / 69 passed, 1 failed`
  (the `cm4_audio_test` intermittency rules stay word-for-word).
- `docs/KNOWN-BROKEN-GATES.md`: append a dated line noting the gate count moved 68 → 70 on
  this date (two new LVGL RK055 gates) so the two files keep agreeing, per CLAUDE.md's own
  instruction.
- `README.md` / `examples/README.md`: add both examples to the capability/example tables,
  following the existing display rows' format. Note on the panel test's row that its golden is
  human-confirmed on glass (the RPi LVGL golden pins reproducibility only).
- `docs/arduino-header-revc3.md`: no change — v3 adds no new pin usage (D6 untouched).

- [ ] **Step 4: Full sweep**

```bash
cd ~/Development/rt1170/evkb
uptime    # a starved host mimics regressions; sweep on a sane load only
./tools/run-all-qemu-gates.sh
```

Expected: **70 passed, 0 failed, 0 SKIP** or **69 passed, 1 failed, 0 SKIP** with the one
failure being `dualcore/cm4_audio_test` (intermittent — do not chase, do not delete). Any
other failure is a real regression from this work: fix before proceeding. Any SKIP means a
gate-owning example is unbuilt and the sweep under-measured — build it and re-run.

- [ ] **Step 5: Commit the wrap**

```bash
cd ~/Development/rt1170/evkb
git add tools/license-audit.sh evkb.cmake CLAUDE.md docs/KNOWN-BROKEN-GATES.md README.md examples/README.md
git commit -m "wrap v3: audit entries for the two LVGL RK055 gates, LVGL pin bump, sweep 68 -> 70"
```

- [ ] **Step 6: Memory**

Write a new memory `rt1176-lvgl-touch-indev` (type: project) to
`/Users/nicholasnewdigate/.claude/projects/-Users-nicholasnewdigate-Development-rt1170-evkb/memory/`
with a one-line pointer in that directory's `MEMORY.md`. Record: v3 complete (if it is),
where the binding lives, the four-way Poll branch + primary policy as the two load-bearing
shapes, the 10 ms read-period rationale, the two negative tests that were measured red, what
the hardware run proved that QEMU could not, and that D6/the two v2 INT findings remain open.
Link `[[rt1176-rk055-touch]]` and `[[rt1176-rk055-display]]`. If the sibling memory
`rt1176-rk055-touch` (in the parent workspace's memory dir) is reachable, update its "Next:"
line; otherwise note the v3 completion only in the new file.

---

## Self-review notes (kept for the executor)

- **Spec coverage:** §2 scope → Tasks 1–8; §3 architecture → Tasks 5–6; §4 behaviour → Task 5
  (the four-way branch and policy are verbatim); §5 timing → Task 5 Step 2 + Task 7's
  IDLE_POLLS guards; §6.1 → Tasks 1–3; §6.2 → Tasks 6–7; §6.3 → Task 4; §6.5 → Task 8;
  §7 decomposition → task order; §9 licence → Task 9 Step 1; §10 open questions resolved here:
  geometry fixed in Task 6 (Q1), endpoint+count only (Q2), assert not nullptr (Q3), prompt
  text in Task 6's `phaseHold()` (Q4).
- **Measured-value placeholders** (`0xRECORDME`, `BUFFERS=RECORDME`, `YYYY-MM-DD`) are the
  tree's red-then-green golden discipline, not plan gaps: each has an explicit measure step,
  a stability check, and a pin step.
- **Cross-repo commit order matters:** qemu2 (Task 4) before the touch gate exists; LVGL
  (Task 5) before the example builds; LVGL push + pin (Task 9) after everything is proven.
