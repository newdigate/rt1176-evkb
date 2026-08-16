# VGLite Foundation Implementation Plan (Phase 1 of 2)

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Stand up the `VGLite` sibling library with a bare-metal port of NXP's MIT VGLite driver, and prove the GC355 GPU actually renders on the RK055 panel — independent of LVGL.

**Architecture:** A new `~/Development/VGLite` repo vendors NXP's OS-agnostic VGLite core unchanged and adds a `port/baremetal/` layer replacing the two FreeRTOS-coupled files. A `display/vglite_probe` example initialises the GPU, reports its feature bits, renders one filled path into the panel framebuffer, and checksums it — QEMU gates the GPU-absent path, hardware proves the GPU-present one.

**Tech Stack:** NXP/Vivante VGLite (MIT), i.MX RT1176 GC355 (GPU2D @ 0x4180_0000, IRQ 60), MipiDisplay RK055, ARM GCC 10, CMake, gate-lib.sh/qrun.

**Spec:** `docs/superpowers/specs/2026-08-16-vglite-gc355-design.md` (master `0028edc`). Read §2 and §4 before starting.

**Why this is Phase 1 of 2.** Phase 2 (LVGL draw-unit wiring, the 16-knob bench, the ≥30 fps target) depends on three facts this phase produces: whether the GPU initialises bare-metal at all, what `vg_lite_query_feature()` actually reports on this silicon, and what tessellation-buffer size is workable. Writing Phase 2 steps now would be speculation. **Phase 1 is independently valuable**: it either proves the GPU is usable or kills the project cheaply.

---

## Standing constraints

- **The main evkb checkout is on `master` and shared with other sessions.** Do all work in the worktree created in Task 1. Never commit to the main checkout.
- **`~/Development/gs-vglite_examples_rt1170` is READ-ONLY reference.** Copy from it; never modify it.
- ★ **Do NOT set `_BAREMETAL 1`.** It looks like the answer and is not: `VGLiteKernel/rtos/vg_lite_hal.c` under that switch hardcodes an FPGA `registerMemBase = 0x43c80000` (line ~46), calls Xilinx's `Xil_DCacheFlush()` (line ~181), and *still* declares `int_queue` as a FreeRTOS `xSemaphoreHandle` (line ~148). It is Xilinx FPGA bring-up scaffolding, not a Cortex-M port. We write a real port instead.
- **Cache maintenance is NOT needed.** The `imxrt1176` core never writes `SCB_CCR`, so CPU and GPU see coherent memory. Do not port the rt1062 D-cache handling here; `vg_lite_hal_barrier()` needs only a `__DSB()`.
- Hardware steps: flash VCOM-free (`flash load` → `flash verify` → attach reader → background `LinkServer run`). A held port during flashing can panic the Mac.

## File structure

```
~/Development/VGLite/                       NEW sibling repo, MIT
├── LICENSE                                 MIT (Vivante/NXP text preserved)
├── README.md                               what this is, what was ported and why
├── VENDORING.md                            provenance + the zero-GPL verification
├── library.properties
├── .gitignore
├── inc/                                    vendored verbatim (vg_lite.h etc.)
├── VGLite/                                 vendored verbatim (OS-agnostic core)
├── VGLiteKernel/                           vendored verbatim EXCEPT rtos/
└── port/baremetal/
    ├── vg_lite_os.c                        replaces VGLite/rtos/vg_lite_os.c
    ├── vg_lite_hal.c                       replaces VGLiteKernel/rtos/vg_lite_hal.c
    └── vg_lite_platform.h                  replaces VGLiteKernel/rtos/vg_lite_platform.h

evkb worktree:
├── evkb.cmake                              + declaration + import_evkb_vglite()
├── examples/display/vglite_probe/          NEW: CMakeLists, .cpp, run_qemu.sh, transcripts
├── tools/license-audit.sh                  + REPOS root + GATES entry
└── docs/KNOWN-BROKEN-GATES.md              + SKIP-class entry
```

---

### Task 1: Worktree and branch

- [ ] **Step 1: Create the worktree**

```bash
git -C /Users/nicholasnewdigate/Development/rt1170/evkb worktree add \
    /Users/nicholasnewdigate/Development/rt1170/evkb/.claude/worktrees/vglite \
    -b vglite master
```

Expected: `Preparing worktree (new branch 'vglite')`. Call this path `$WT` throughout.

- [ ] **Step 2: Confirm the main checkout is untouched**

```bash
git -C /Users/nicholasnewdigate/Development/rt1170/evkb status --short
```

Expected: empty.

### Task 2: Create the VGLite repo and vendor the driver

**Files:** all of `~/Development/VGLite` (new repo)

- [ ] **Step 1: Verify the source is licence-clean before copying anything**

```bash
cd ~/Development/gs-vglite_examples_rt1170/common/vglite
grep -rl "GNU General Public\|GNU Lesser\|Mozilla Public" . | wc -l
```

Expected: `0`. If non-zero, STOP and report — the whole project rests on this.

- [ ] **Step 2: Init the repo and copy the vendored trees**

```bash
SRC=~/Development/gs-vglite_examples_rt1170/common/vglite
DST=~/Development/VGLite
test ! -e "$DST" && echo CLEAR
git init -b master "$DST"
mkdir -p "$DST/port/baremetal"
cp -R "$SRC/inc" "$DST/"
cp -R "$SRC/VGLite" "$DST/"
cp -R "$SRC/VGLiteKernel" "$DST/"
cp "$SRC/LICENSE.txt" "$DST/LICENSE"
rm -rf "$DST/VGLite/rtos" "$DST/VGLiteKernel/rtos"
```

The two `rtos/` directories are deliberately removed — `port/baremetal/` replaces them. Everything else is verbatim upstream.

- [ ] **Step 3: Confirm what remains is OS-agnostic**

```bash
cd ~/Development/VGLite
grep -rl "FreeRTOS\|xSemaphore\|vTaskDelay\|pdTRUE" VGLite VGLiteKernel inc | wc -l
```

Expected: `0`. Any hit means an RTOS dependency survived outside `rtos/` — report it rather than deleting code.

- [ ] **Step 4: Write `VENDORING.md`**

```markdown
# Vendoring

Upstream: NXP `gs-vglite_examples_rt1170`, `common/vglite/` (local reference
checkout at `~/Development/gs-vglite_examples_rt1170`). Vivante VGLite,
`VGLITE_HEADER_VERSION 6`, `VGLITE_VERSION_2_0`.

## Licence

**MIT throughout.** Verified before vendoring:

    grep -rl "GNU General Public\|GNU Lesser\|Mozilla Public" . | wc -l   -> 0

`VGLiteKernel/vg_lite_kernel.h` carries "The MIT License (MIT), Copyright (c)
2014 - 2020 Vivante Corporation".

★ This is NOT the copy bundled with LVGL. LVGL's `src/libs/vg_lite_driver/`
carries a dual-licensed VGLiteKernel that trips `tools/license-audit.sh`
Part 1 on 5 files, and it is correctly pruned in the LVGL vendoring. That
pruning stays. This repo is the MIT copy NXP ships, and it is what
`#include <vg_lite.h>` resolves to for LVGL's draw unit.

## What was taken, and what was not

Vendored verbatim: `inc/`, `VGLite/`, `VGLiteKernel/`.

**Removed:** `VGLite/rtos/` and `VGLiteKernel/rtos/` — the FreeRTOS port layer
(`vg_lite_os.c`, 27 FreeRTOS references; `vg_lite_hal.c`, 3). This tree is
bare-metal (`LV_USE_OS 0`), so `port/baremetal/` replaces them.

★ **The upstream `_BAREMETAL` switch is not a bare-metal port.** Under it the
HAL hardcodes an FPGA `registerMemBase = 0x43c80000`, calls Xilinx's
`Xil_DCacheFlush()`, and still declares `int_queue` as a FreeRTOS
`xSemaphoreHandle`. It is Xilinx FPGA scaffolding. Do not enable it.

## Re-vendoring

Re-copy the three directories, delete both `rtos/` trees, re-run the grep
above, and re-check `port/baremetal/` against any changed function signatures
in `VGLite/vg_lite.c` and `VGLiteKernel/vg_lite_kernel.c`.
```

- [ ] **Step 5: Write `library.properties`**

```text
name=VGLite
version=0.1.0
author=Vivante Corporation, NXP
maintainer=Nicholas Newdigate
sentence=Vivante VGLite vector GPU driver for the NXP i.MX RT1176 GC355, bare-metal.
paragraph=NXP's MIT-licensed VGLite driver vendored for this tree, with a bare-metal port layer replacing the FreeRTOS one. Provides vg_lite.h for LVGL's VG_LITE draw unit.
category=Display
url=https://github.com/newdigate/VGLite
architectures=*
```

- [ ] **Step 6: Write `.gitignore` and `README.md`**

`.gitignore`:
```text
build/
.DS_Store
```

`README.md`:
```markdown
# VGLite

Vivante VGLite vector-GPU driver for the i.MX RT1176's **GC355** (GPU2D at
`0x4180_0000`, IRQ 60, `GPU2D_CLK_ROOT`), vendored from NXP and ported to
bare metal for the rt1176-evkb tree.

MIT throughout — see `VENDORING.md` for provenance and the licence
verification.

## Layout

- `inc/`, `VGLite/`, `VGLiteKernel/` — vendored verbatim, OS-agnostic.
- `port/baremetal/` — this tree's port: GPU2D clock/IRQ bring-up, contiguous
  allocation, and a bounded polled completion wait. Replaces upstream's
  FreeRTOS `rtos/` layer.

## Consumers

LVGL's `LV_USE_DRAW_VG_LITE` backend compiles against this repo's
`vg_lite.h` via the `#include <vg_lite.h>` path (with
`LV_USE_VG_LITE_DRIVER` and `LV_USE_VG_LITE_THORVG` both 0), so LVGL needs
no source modification.

Status: bare-metal port + GPU-alive probe. LVGL integration is Phase 2.
```

- [ ] **Step 7: Commit**

```bash
cd ~/Development/VGLite
git add -A
git commit -m "vendor: NXP VGLite driver (MIT) with the FreeRTOS port removed

Co-Authored-By: Claude Fable 5 <noreply@anthropic.com>"
git log --oneline | cat
```

Expected: one commit.

### Task 3: Establish the hardware facts the port needs

**Files:** none (research; findings go into Task 4/5 comments)

- [ ] **Step 1: Confirm the register base and IRQ from the reference manual**

```bash
grep -n "4180_0000\|GPU2D" /Users/nicholasnewdigate/Development/rt1170/rm_full.txt | head -20
```

Record: peripheral base (expect `0x4180_0000`) and IRQ number (expect 60).

- [ ] **Step 2: Find how the NXP example enables the GPU clock**

```bash
grep -rn "GPU2D\|gpu2d\|LPCG128\|CLOCK_ROOT68\|ClockRoot" \
  ~/Development/gs-vglite_examples_rt1170/evkmimxrt1170_01_SimplePath/source/ \
  ~/Development/gs-vglite_examples_rt1170/common/ 2>/dev/null | grep -iv "vglite/VGLite" | head -20
```

Record the exact clock-root and LPCG writes. If the example relies on SDK
`CLOCK_*` helpers this tree does not have, note the register writes they
expand to — `grep -n "CLOCK_ROOT68\|clk_enable_gpu2d" rm_full.txt` gives the
register addresses and field layout.

- [ ] **Step 3: Find the driver's expected init entry points**

```bash
grep -n "vg_lite_init_mem\|registerMemBase\|gpuMemBase\|contiguous" \
  ~/Development/gs-vglite_examples_rt1170/common/vglite/VGLiteKernel/rtos/vg_lite_hal.c | head -20
```

`vg_lite_platform.h` declares:
```c
void vg_lite_init_mem(uint32_t register_mem_base, uint32_t gpu_mem_base,
                      volatile void *contiguous_mem_base, uint32_t contiguous_mem_size);
void vg_lite_IRQHandler(void);
```
Record how the FreeRTOS HAL stores and uses each argument — the bare-metal
port must honour the same contract.

- [ ] **Step 4: Write the findings into `$WT` as a scratch note and commit it**

Create `$WT/examples/display/vglite_probe/HARDWARE-NOTES.md` with the register
base, IRQ number, the exact clock-enable writes, and the `vg_lite_init_mem`
contract, each with its source (RM line number or example file:line). Commit:

```bash
git -C $WT add examples/display/vglite_probe/HARDWARE-NOTES.md
git -C $WT commit -m "vglite_probe: recorded GC355 register/clock/IRQ facts with sources

Co-Authored-By: Claude Fable 5 <noreply@anthropic.com>"
```

This is deliberate: the next two tasks write code against these facts, and a
wrong constant here is a hang, not a compile error.

### Task 4: Bare-metal `vg_lite_os.c`

**Files:**
- Create: `~/Development/VGLite/port/baremetal/vg_lite_os.c`

The FreeRTOS original is at
`~/Development/gs-vglite_examples_rt1170/common/vglite/VGLite/rtos/vg_lite_os.c`
— read it for each function's contract, then implement the bare-metal
equivalent. `vg_lite_os.h` (vendored, in `VGLite/`) is the authority on
signatures; do not change it.

Bare-metal semantics, function by function (all 19 in the header):

| function | bare-metal implementation |
|---|---|
| `vg_lite_os_set_tls` / `get_tls` / `reset_tls` | one file-static `void *` — single-threaded, so TLS is a global |
| `vg_lite_os_malloc` / `free` | `malloc` / `free` |
| `vg_lite_os_sleep(ms)` | `delay(ms)` (Arduino core) |
| `vg_lite_os_initialize` / `deinitialize` | clear the static event state; return `VG_LITE_SUCCESS`-equivalent (`0`) |
| `vg_lite_os_lock` / `unlock` | no-op returning success — the driver is never called from an ISR in this design |
| `vg_lite_os_init_event` / `delete_event` | set / clear `event->signal` via the header's `vg_lite_os_set_event_state` macro |
| `vg_lite_os_signal_event` | set `event->signal` (called from the IRQ handler) |
| `vg_lite_os_wait_event` / `vg_lite_os_wait` | **bounded** polled wait on `event->signal`, with a timeout |
| `vg_lite_os_submit` | hand the command buffer to the kernel layer exactly as the RTOS version does, minus the semaphore take |
| `vg_lite_os_wait_interrupt` | bounded poll of the interrupt flag/value the IRQ handler records |
| `vg_lite_os_IRQHandler` | clear the GPU interrupt status, record the value, signal the event |
| `vg_lite_os_query_context_switch` | return 0 — one context, never switches |

- [ ] **Step 1: Write the file**

Follow the RTOS original's structure and comments. Two requirements that are
not negotiable, because a mistake in either is a hang rather than a wrong
pixel:

1. **Every wait is bounded and reports its timeout.** Model it on
   `lvgl_mipi_panel_flip_sync()` in `~/Development/lvgl/port/lvgl_mipi_panel.cpp`
   — a deadline loop that returns false rather than spinning forever.
2. **A timeout counter is exported for assertion.** Add to the file:

```c
/* Bounded-wait diagnostics. A GPU that never signals must NAME itself in the
 * UART rather than presenting as a dead board -- the same contract as the
 * LCDIFv2 vsync fence (VSYNC_TIMEOUTS). */
static volatile uint32_t s_wait_timeouts;
uint32_t vg_lite_os_wait_timeouts(void) { return s_wait_timeouts; }
```

and declare it in `port/baremetal/vg_lite_platform.h` (Task 6).

- [ ] **Step 2: Commit**

```bash
cd ~/Development/VGLite
git add port/baremetal/vg_lite_os.c
git commit -m "port: bare-metal vg_lite_os -- polled completion with bounded waits

Co-Authored-By: Claude Fable 5 <noreply@anthropic.com>"
```

(It cannot compile standalone yet — the HAL and the build wiring arrive in
Tasks 5-6. Compilation is verified in Task 6 Step 3.)

### Task 5: Bare-metal `vg_lite_hal.c`

**Files:**
- Create: `~/Development/VGLite/port/baremetal/vg_lite_hal.c`

Original: `.../common/vglite/VGLiteKernel/rtos/vg_lite_hal.c`. All 15 functions:

| function | bare-metal implementation |
|---|---|
| `vg_lite_hal_delay(ms)` | `delay(ms)` |
| `vg_lite_hal_barrier` | `__DSB()` **only** — no cache maintenance (see constraints) |
| `vg_lite_hal_initialize` | enable the GPU2D clock (Task 3 findings), attach the IRQ, reset driver state |
| `vg_lite_hal_deinitialize` | detach the IRQ, gate the clock |
| `vg_lite_hal_allocate_contiguous` | carve from the static pool declared in Task 6; return logical == physical (flat map, no MMU) |
| `vg_lite_hal_free_contiguous` / `free_os_heap` | return the carve to the pool |
| `vg_lite_hal_peek(addr)` / `poke(addr, data)` | `*(volatile uint32_t *)(registerMemBase + addr)` read / write |
| `vg_lite_hal_query_mem` | report pool total and free from the same accounting |
| `vg_lite_hal_map` / `unmap` | identity — `*gpu = physical`, no MMU on this part |
| `vg_lite_hal_submit` / `wait` / `wait_interrupt` | delegate to the `vg_lite_os_*` equivalents from Task 4 |

- [ ] **Step 1: Write the file**

Requirements:
- `vg_lite_init_mem()` must store `register_mem_base`, `gpu_mem_base`,
  `contiguous_mem_base` and `contiguous_mem_size` in file statics; every
  `peek`/`poke`/allocation reads them. Do not hardcode `0x4180_0000` in the
  HAL — the example passes it in, which keeps the port board-agnostic.
- The contiguous allocator may be a bump allocator with a free-all: VGLite
  allocates its command and tessellation buffers at init and holds them.
  Note that in a comment so nobody mistakes it for a general heap.
- Attach the GPU2D IRQ (number from Task 3) to a handler that calls
  `vg_lite_IRQHandler()`.

- [ ] **Step 2: Commit**

```bash
cd ~/Development/VGLite
git add port/baremetal/vg_lite_hal.c
git commit -m "port: bare-metal vg_lite_hal -- clock, IRQ, flat contiguous pool

Co-Authored-By: Claude Fable 5 <noreply@anthropic.com>"
```

### Task 6: `vg_lite_platform.h`, the memory pool, and the build

**Files:**
- Create: `~/Development/VGLite/port/baremetal/vg_lite_platform.h`
- Modify: `$WT/evkb.cmake`

- [ ] **Step 1: Write `port/baremetal/vg_lite_platform.h`**

```c
/* vg_lite_platform.h - bare-metal platform contract for the RT1176 GC355.
 * Replaces upstream VGLiteKernel/rtos/vg_lite_platform.h.
 * SPDX-License-Identifier: MIT */
#ifndef _VG_LITE_PLATFORM_H
#define _VG_LITE_PLATFORM_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Upstream contract, unchanged. */
void vg_lite_init_mem(uint32_t register_mem_base, uint32_t gpu_mem_base,
                      volatile void *contiguous_mem_base,
                      uint32_t contiguous_mem_size);
void vg_lite_IRQHandler(void);

/* This port's additions. */

/* GC355 on the i.MX RT1176. Passed to vg_lite_init_mem() by the application
 * rather than hardcoded in the HAL, so the port stays board-agnostic. */
#define VGLITE_RT1176_REGISTER_BASE   0x41800000u
#define VGLITE_RT1176_GPU2D_IRQ       60

/* Bounded-wait diagnostics: a GPU that never signals must name itself rather
 * than presenting as a dead board. Assert this is 0 in gates. */
uint32_t vg_lite_os_wait_timeouts(void);

#ifdef __cplusplus
}
#endif
#endif
```

★ Verify `VGLITE_RT1176_REGISTER_BASE` and `VGLITE_RT1176_GPU2D_IRQ` against
the Task 3 findings before committing. If they disagree, the Task 3 findings
win — they came from the reference manual.

- [ ] **Step 2: Add the declaration and import macro to `$WT/evkb.cmake`**

Below the `SynthUI` declaration line add (sha from
`git -C ~/Development/VGLite rev-parse HEAD`):

```cmake
teensy_declare_library(VGLite        VGLite               https://github.com/newdigate/VGLite          <sha> .) # LOCAL-ONLY (unpushed): resolves under TEENSY_LIB_ROOT; fresh clones fail here until VGLite's first push. Not Arduino-layout: use import_evkb_vglite().
```

After the `import_evkb_synthui()` macro add:

```cmake
# --- VGLite ------------------------------------------------------------------
# Vivante VGLite (MIT) for the GC355, plus this tree's bare-metal port. A plain
# STATIC target for the same reason LVGL and SynthUI are: consumers need the
# PUBLIC include dir, which teensy_target_link_libraries() would not propagate.
# The upstream rtos/ layer is NOT vendored -- port/baremetal/ replaces it.
macro(import_evkb_vglite)
    if(NOT TARGET VGLite)
        if(TEENSY_FORCE_FETCH OR NOT EXISTS "${TEENSY_LIB_ROOT}/VGLite/inc")
            message(FATAL_ERROR "import_evkb_vglite(): VGLite is LOCAL-ONLY "
                "(unpushed) -- its pinned URL cannot be fetched yet. Expected a "
                "checkout at ${TEENSY_LIB_ROOT}/VGLite.")
        endif()
        evkb_library_dir(VGLite _evkb_vglite_dir)
        file(GLOB _evkb_vglite_srcs CONFIGURE_DEPENDS
             "${_evkb_vglite_dir}/VGLite/*.c"
             "${_evkb_vglite_dir}/VGLiteKernel/*.c"
             "${_evkb_vglite_dir}/port/baremetal/*.c")
        add_library(VGLite STATIC ${_evkb_vglite_srcs})
        target_include_directories(VGLite PUBLIC
             "${_evkb_vglite_dir}/inc"
             "${_evkb_vglite_dir}/port/baremetal")
        target_link_libraries(VGLite PRIVATE teensy_flags)
        target_link_libraries(VGLite PUBLIC m)
    endif()
endmacro()
```

- [ ] **Step 3: Prove it compiles** — configure and build the probe scaffold from
  Task 7 Step 1, which is the first consumer. If the port has compile errors
  (missing symbols, signature drift against `vg_lite_os.h`), fix them in the
  port files, not by editing vendored code.

- [ ] **Step 4: Commit both repos**

```bash
cd ~/Development/VGLite && git add port/baremetal/vg_lite_platform.h && \
  git commit -m "port: bare-metal platform header with RT1176 GC355 constants

Co-Authored-By: Claude Fable 5 <noreply@anthropic.com>"
git -C $WT add evkb.cmake && git -C $WT commit -m "build: declare VGLite (local-only) + import_evkb_vglite()

Co-Authored-By: Claude Fable 5 <noreply@anthropic.com>"
```

### Task 7: The `vglite_probe` example

**Files:**
- Create: `$WT/examples/display/vglite_probe/CMakeLists.txt`
- Create: `$WT/examples/display/vglite_probe/vglite_probe.cpp`

- [ ] **Step 1: `CMakeLists.txt`**

```cmake
cmake_minimum_required(VERSION 3.24)
project(vglite_probe)

# XRGB8888 to match the panel binding used by the LVGL examples.
add_compile_definitions(PANEL_BYTES_PER_PIXEL=4)

set(TEENSY_VERSION 117 CACHE STRING "")

include(${CMAKE_CURRENT_LIST_DIR}/../../../evkb.cmake)

import_evkb_vglite()
# panels/rk055 selects the panel; one MIPI-DSI host, one panel dir ever live.
import_evkb_library(MipiDisplay soc panels/rk055)
import_evkb_library(PXP)    # Display::fillScreen() paints via the PXP

teensy_add_executable(vglite_probe vglite_probe.cpp)
teensy_target_link_libraries(vglite_probe cores MipiDisplay PXP)

# VGLite is a plain CMake static-lib target (see import_evkb_vglite for why
# teensy_target_link_libraries cannot link it).
target_link_libraries(vglite_probe.elf VGLite stdc++)
```

- [ ] **Step 2: `vglite_probe.cpp`** — the GPU-alive proof. No LVGL.

```cpp
/* vglite_probe - does the GC355 initialise and render on this board?
 * Copyright (c) 2026 Nicholas Newdigate
 * SPDX-License-Identifier: MIT
 *
 * Phase 1 of the VGLite bring-up: prove the GPU works INDEPENDENT of LVGL.
 * On QEMU there is no GC355 model, so vg_lite_init() is expected to fail and
 * the probe reports VGLITE_INIT=ABSENT -- that is the gated path. On silicon
 * the GPU must initialise, report its feature bits, and fill one path.
 */
#include <Arduino.h>
#include "Display.h"
#include "vg_lite.h"
#include "vg_lite_platform.h"

/* Contiguous pool for VGLite's command and tessellation buffers. Sized
 * generously -- 64 MB SDRAM is available and Phase 2 will tune it against
 * measured throughput. DMAMEM places it in OCRAM/SDRAM rather than DTCM,
 * which the GPU cannot reach. */
#define VGLITE_POOL_BYTES (2u * 1024u * 1024u)
DMAMEM static uint8_t vglite_pool[VGLITE_POOL_BYTES] __attribute__((aligned(64)));

/* Tessellation buffer geometry passed to vg_lite_init(). */
#define TESS_W 256
#define TESS_H 256

static void report_features(void)
{
    static const struct { vg_lite_feature_t bit; const char *name; } feats[] = {
        { gcFEATURE_BIT_VG_SCISSOR,             "SCISSOR" },
        { gcFEATURE_BIT_VG_RADIAL_GRADIENT,     "RADIAL_GRADIENT" },
        { gcFEATURE_BIT_VG_LINEAR_GRADIENT_EXT, "LINEAR_GRADIENT_EXT" },
        { gcFEATURE_BIT_VG_QUALITY_8X,          "QUALITY_8X" },
        { gcFEATURE_BIT_VG_BORDER_CULLING,      "BORDER_CULLING" },
        { gcFEATURE_BIT_VG_PE_PREMULTIPLY,      "PE_PREMULTIPLY" },
    };
    for (unsigned i = 0; i < sizeof(feats) / sizeof(feats[0]); i++)
        Serial1.printf("VGLITE_FEATURE %s=%lu\n", feats[i].name,
                       (unsigned long)vg_lite_query_feature(feats[i].bit));
}

void setup()
{
    Serial1.begin(115200);
    delay(200);
    Serial1.println("VGLITE_PROBE_BEGIN");

    const bool ok = Display.begin();
    Serial1.println(ok ? "PANEL_OK" : "PANEL_FAIL");
    if (!ok) { Serial1.println("VGLITE_PROBE_DONE"); return; }
    Display.fillScreen(0x0000);

    /* Hand the driver its register window and its contiguous pool. */
    vg_lite_init_mem(VGLITE_RT1176_REGISTER_BASE, 0u,
                     vglite_pool, VGLITE_POOL_BYTES);

    const vg_lite_error_t err = vg_lite_init(TESS_W, TESS_H);
    if (err != VG_LITE_SUCCESS) {
        /* Expected on QEMU: no GC355 model. This is a REPORTED outcome, not a
         * crash -- the same binary must run in both places. */
        Serial1.printf("VGLITE_INIT=ABSENT err=%d\n", (int)err);
        Serial1.println("VGLITE_PROBE_DONE");
        return;
    }
    Serial1.println("VGLITE_INIT=OK");
    report_features();

    /* Render one filled path into the panel framebuffer. Geometry is fixed,
     * so the checksum is a golden. */
    vg_lite_buffer_t target;
    memset(&target, 0, sizeof(target));
    target.width  = Display.width();
    target.height = Display.height();
    target.stride = Display.width() * PANEL_BYTES_PER_PIXEL;
    target.format = VG_LITE_BGRA8888;
    target.memory = Display.framebuffer();
    target.address = (uint32_t)(uintptr_t)Display.framebuffer();

    int32_t path_data[] = {
        2,  100, 100,      /* MOVE_TO 100,100 */
        4,  400, 100,      /* LINE_TO 400,100 */
        4,  400, 400,      /* LINE_TO 400,400 */
        4,  100, 400,      /* LINE_TO 100,400 */
        0,                 /* END */
    };
    vg_lite_path_t path;
    memset(&path, 0, sizeof(path));
    vg_lite_init_path(&path, VG_LITE_S32, VG_LITE_HIGH,
                      sizeof(path_data), path_data,
                      0.0f, 0.0f, 640.0f, 640.0f);

    vg_lite_matrix_t matrix;
    vg_lite_identity(&matrix);

    const vg_lite_error_t derr =
        vg_lite_draw(&target, &path, VG_LITE_FILL_EVEN_ODD, &matrix,
                     VG_LITE_BLEND_NONE, 0xFF3399FFu);
    Serial1.printf("VGLITE_DRAW=%s err=%d\n",
                   derr == VG_LITE_SUCCESS ? "OK" : "FAIL", (int)derr);
    vg_lite_finish();

    Serial1.printf("VGLITE_TIMEOUTS=%lu\n",
                   (unsigned long)vg_lite_os_wait_timeouts());

    /* FNV-1a over the framebuffer: the golden for what the GPU drew. */
    uint32_t sum = 2166136261u;
    const uint8_t *p = (const uint8_t *)Display.framebuffer();
    const size_t n = (size_t)Display.width() * Display.height() * PANEL_BYTES_PER_PIXEL;
    for (size_t i = 0; i < n; i++) { sum ^= p[i]; sum *= 16777619u; }
    Serial1.printf("VGLITE_SUM=0x%08lX\n", (unsigned long)sum);

    Serial1.println("VGLITE_PROBE_DONE");
}

void loop() { }
```

★ The path opcode encoding (`2` = MOVE_TO, `4` = LINE_TO, `0` = END) and the
`vg_lite_init_path` / `vg_lite_draw` signatures must be checked against
`~/Development/VGLite/inc/vg_lite.h` and a working NXP example
(`evkmimxrt1170_01_SimplePath/source/SimplePath.c`) before building — the
header is the authority, and API drift here is a compile error, not a silent
bug.

- [ ] **Step 3: Build**

```bash
cd $WT/examples/display/vglite_probe
cmake -B build -DCMAKE_TOOLCHAIN_FILE=../../../toolchain/rt1170-evkb.toolchain.cmake
cmake --build build
```

Expected: `vglite_probe.elf`. Fix compile errors in `port/baremetal/`, never in
vendored code; if a vendored file genuinely needs a change, stop and report it
so it can be recorded in `VENDORING.md` as a patch.

- [ ] **Step 4: Boot in QEMU** (the GPU-absent path)

```bash
cd $WT && gtimeout --kill-after=5s 20s ./tools/rt1170-qemu.sh \
    examples/display/vglite_probe/build/vglite_probe.elf </dev/null > /tmp/probe.log 2>&1 || true
grep -E "VGLITE_|PANEL_" /tmp/probe.log
```

Expected: `VGLITE_PROBE_BEGIN`, `PANEL_OK`, `VGLITE_INIT=ABSENT err=<n>`,
`VGLITE_PROBE_DONE`. **A hang here is the failure mode to watch for** — it
means a wait is unbounded (Task 4). If it hangs, fix the wait; do not raise
the timeout to hide it.

- [ ] **Step 5: Commit**

```bash
git -C $WT add examples/display/vglite_probe
git -C $WT commit -m "vglite_probe: GPU-alive example -- init, features, one filled path

Co-Authored-By: Claude Fable 5 <noreply@anthropic.com>"
```

### Task 8: The QEMU gate (software-absent path)

**Files:**
- Create: `$WT/examples/display/vglite_probe/run_qemu.sh` (mode 755)
- Create: `$WT/examples/display/vglite_probe/transcript_qemu.txt`

- [ ] **Step 1: Write the gate with a deliberately wrong assertion first**

```sh
#!/bin/sh
set -e
DIR=$(cd "$(dirname "$0")" && pwd)
# Tools come from THIS checkout, derived from the gate's own location.
EVKB=$(cd "$DIR/../../.." && pwd)
QEMU="$EVKB/tools/qrun"
. "$EVKB/tools/gate-lib.sh"
gate_init
ELF="$DIR/$(gate_build_dir)/vglite_probe.elf"
OUT=$(gate_capture_path "$DIR" vglite_probe.uart)
DBG=$(gate_capture_path "$DIR" vglite_probe.dbg)
rm -f "$OUT"
"$QEMU" $(gate_qemu_machine) -kernel "$ELF" \
    -display none -serial file:"$OUT" -d guest_errors -D "$DBG" &
P=$!; gate_pid $P
# 12s: RK055 bring-up margin, as lvgl_rk055_panel_test uses. The probe does no
# software rendering, so it needs no more.
sleep 12; gate_reap $P
gate_require_capture "$OUT"
echo "==== captured UART ===="; cat "$OUT"
grep -q "PANEL_OK" "$OUT" || { echo "FAIL: panel bring-up"; exit 1; }
# THE POINT OF THIS GATE: QEMU has no GC355 model, so vg_lite_init() MUST fail
# and the probe MUST report that failure rather than hanging or crashing. This
# asserts the fallback path -- the same binary that accelerates on silicon.
# See docs/KNOWN-BROKEN-GATES.md for what this gate does NOT cover.
grep -qE "VGLITE_INIT=ABSENT\r?$|VGLITE_INIT=ABSENT err=" "$OUT" || \
    { echo "FAIL: expected VGLITE_INIT=ABSENT under QEMU"; exit 1; }
grep -q "VGLITE_PROBE_DONE" "$OUT" || { echo "FAIL: no completion token"; exit 1; }
echo "PASS: VGLite probe fallback verified"
```

```bash
chmod 755 $WT/examples/display/vglite_probe/run_qemu.sh
```

- [ ] **Step 2: Prove the gate can fail** — temporarily change the
  `VGLITE_INIT=ABSENT` grep to `VGLITE_INIT=OK`, run `./run_qemu.sh`, confirm
  it fails with `FAIL: expected VGLITE_INIT=ABSENT under QEMU` and exit 1,
  then restore it and confirm PASS. Run from the example dir, never `sh run_qemu.sh`.

- [ ] **Step 3: Run twice, both PASS**

```bash
cd $WT/examples/display/vglite_probe && ./run_qemu.sh && ./run_qemu.sh; echo "exit=$?"
```

Expected: `PASS: VGLite probe fallback verified` twice, `exit=0`.

- [ ] **Step 4: Save `transcript_qemu.txt`** — the full output of one passing
  run (UART echo + PASS line), house style.

- [ ] **Step 5: Commit**

```bash
git -C $WT add examples/display/vglite_probe
git -C $WT commit -m "vglite_probe: QEMU gate asserting the GPU-absent fallback

Co-Authored-By: Claude Fable 5 <noreply@anthropic.com>"
```

### Task 9: Hardware — does the GC355 actually render?

**Files:**
- Create: `$WT/examples/display/vglite_probe/transcript_hw_evkb.txt`

**This is the task the whole phase exists for.** Everything before it is
scaffolding; this is where the project is proven or killed.

- [ ] **Step 1: Flash, VCOM-free**

```bash
pkill LinkServer; pkill redlinkserv; pkill crt_emu_cm_redlink; sleep 1
lsof -t -- /dev/cu.usbmodem5DQ2DDHVWO5EI3 && echo "PORT HELD - ABORT" && exit 1
ELF=$WT/examples/display/vglite_probe/build/vglite_probe.elf
/Applications/LinkServer_26.6.137/LinkServer flash MIMXRT1176:MIMXRT1170-EVKB load "$ELF" --erase-all
/Applications/LinkServer_26.6.137/LinkServer flash MIMXRT1176:MIMXRT1170-EVKB verify "$ELF"
```

Expected: `LOAD_EXIT=0`, then "File matches flash. verify succeeded". A
`No connection to chip's debug port` failure means the probe needs a physical
power-cycle — ask, do not retry in a loop.

- [ ] **Step 2: Capture the boot**

```bash
PY=/usr/local/Caskroom/miniconda/base/bin/python3
$PY $WT/tools/rt1170-console.py /dev/cu.usbmodem5DQ2DDHVWO5EI3 115200 > /tmp/vglite_hw.log 2>&1 &
sleep 3
/Applications/LinkServer_26.6.137/LinkServer run MIMXRT1176:MIMXRT1170-EVKB "$ELF" > /dev/null 2>&1 &
sleep 20; kill %1 %2 2>/dev/null; cat /tmp/vglite_hw.log
```

Required outcome:
- `VGLITE_INIT=OK` — the GPU initialised bare-metal. **This is the load-bearing result.**
- `VGLITE_FEATURE …` lines — record every value; Phase 2 depends on them.
- `VGLITE_DRAW=OK`
- `VGLITE_TIMEOUTS=0` — no bounded wait gave up. A non-zero count means the
  completion path is wrong even if pixels appeared.
- `VGLITE_SUM=0x…` — stable across two runs.

If `VGLITE_INIT` reports `ABSENT` on hardware, the port is not working: check
the clock enable first (an ungated GPU2D reads back zeros and fails init),
then the register base, then the IRQ. Report rather than guessing.

- [ ] **Step 3: Eyes on glass** — a blue-ish filled square (~300×300 at
  100,100) must be visible on the RK055. Confirm with the user; a checksum
  alone does not prove the GPU drew rather than the CPU. Record who confirmed.

- [ ] **Step 4: Write `transcript_hw_evkb.txt`** — the capture plus a dated
  note of the glass confirmation and the feature-bit table. Commit:

```bash
git -C $WT add examples/display/vglite_probe/transcript_hw_evkb.txt
git -C $WT commit -m "vglite_probe: hardware transcript -- GC355 renders on the EVKB

Co-Authored-By: Claude Fable 5 <noreply@anthropic.com>"
```

### Task 10: Licence audit, docs, and wrap

**Files:**
- Modify: `$WT/tools/license-audit.sh`
- Modify: `$WT/docs/KNOWN-BROKEN-GATES.md`
- Modify: `$WT/examples/README.md`

- [ ] **Step 1: Add the REPOS root and the GATES entry**

In `tools/license-audit.sh`, add `$LIB_ROOT/VGLite` to the `REPOS` list (with
a comment: joined 2026-08-16 with `display/vglite_probe`, MIT-verified), and
add to `GATES`, sorted among the display entries:

```
examples/display/vglite_probe:vglite_probe \
```

- [ ] **Step 2: Mutation-test the REPOS root** — the check must be load-bearing,
  not decorative:

```bash
cd $WT
LICENSE_AUDIT_EVKB=$PWD LICENSE_AUDIT_GATES="examples/display/vglite_probe:vglite_probe" \
  LICENSE_AUDIT_REPOS="$HOME/Development/teensy-cores $HOME/Development/MipiDisplay $HOME/Development/PXP" \
  ./tools/license-audit.sh 2>&1 | grep -E "OUTSIDE|SWEPT"
```

Expected: `OUTSIDE SWEPT ROOTS in examples/display/vglite_probe (… add the repo to REPOS)`.
Then re-run with the real REPOS (drop the `LICENSE_AUDIT_REPOS` override) and
confirm the example's dep walk completes with no `OUTSIDE` line.

★ `LICENSE_AUDIT_EVKB` is REQUIRED from a worktree — the script defaults `EVKB`
to `$HOME/Development/rt1170/evkb` and would otherwise audit a tree where this
example does not exist, reporting `MISSING BUILD`. Note also that a
worktree-scoped run reports `MISSING BUILD` for every OTHER example (they are
built in the main checkout), so the full-tree `LICENSE-AUDIT: PASS` can only be
measured after this branch merges to master and the example is built there.

- [ ] **Step 3: `docs/KNOWN-BROKEN-GATES.md`** — add a section in house style:

```markdown
## `rt1176:display/vglite_probe` — QEMU gates the FALLBACK, not the GPU

★ **QEMU has no GC355 model.** This gate asserts `VGLITE_INIT=ABSENT`: that
`vg_lite_init()` fails cleanly and the probe reports it rather than hanging.
That is a real assertion — an unbounded wait in the bare-metal port would hang
here and fail the gate — but it proves the FALLBACK path, not the GPU.

**The GPU path is verified on silicon only**, in `transcript_hw_evkb.txt`:
`VGLITE_INIT=OK`, the feature-bit table, `VGLITE_DRAW=OK`,
`VGLITE_TIMEOUTS=0`, a recorded `VGLITE_SUM`, and a human eye on the RK055.

★ **The GPU and software paths do NOT produce identical pixels** — hardware
antialiasing differs from LVGL's masks. Two golden sets, never one. Do not
"fix" a mismatch by copying one over the other.

Also SKIP-class on a fresh clone, like `display/synthui_knob_test`: VGLite is
unpushed, so `import_evkb_vglite()` FATAL_ERRORs, the example cannot configure,
and the runner reports SKIP `(not built)` rather than a failure.
```

- [ ] **Step 4: Add the example to `$WT/examples/README.md`**'s display section,
  one line in the neighbours' voice: what it proves (GC355 initialises
  bare-metal, renders a path, reports feature bits; QEMU gates the fallback).

- [ ] **Step 5: Run the full sweep and update `CLAUDE.md`**

```bash
cd $WT && ./tools/run-all-qemu-gates.sh
```

Read the runner's own counts and gate NAMES. Update CLAUDE.md's sweep
paragraph with the measured numbers in its established style. Do not infer the
count from file counting — the house rule is to measure.

- [ ] **Step 6: Commit and report**

```bash
git -C $WT add tools/license-audit.sh docs/KNOWN-BROKEN-GATES.md examples/README.md CLAUDE.md
git -C $WT commit -m "tools+docs: vglite_probe audit entries, SKIP/GPU-coverage notes, sweep baseline

Co-Authored-By: Claude Fable 5 <noreply@anthropic.com>"
```

Final report must state: whether `VGLITE_INIT=OK` on silicon, the full feature-bit
table, `VGLITE_TIMEOUTS`, the sweep numbers, and the audit result. Then invoke
**superpowers:finishing-a-development-branch**.

---

## What Phase 2 will need from this phase

Do not start Phase 2 until these are recorded in `transcript_hw_evkb.txt`:
1. `VGLITE_INIT=OK` on silicon — otherwise there is no project.
2. The complete feature-bit table — it decides which LVGL VG_LITE features can
   be enabled (`lv_vg_lite_grad.c` branches on `VG_RADIAL_GRADIENT` and
   `VG_LINEAR_GRADIENT_EXT`; `lv_draw_vg_lite.c` on `VG_SCISSOR`).
3. Whether the 2 MB pool and 256×256 tessellation buffer sufficed, and what
   `vg_lite_hal_query_mem()` reported free after init — Phase 2 tunes both
   against the 16-knob scene.
