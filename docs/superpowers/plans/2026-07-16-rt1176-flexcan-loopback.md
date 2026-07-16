# RT1176 FlexCAN CAN3 Internal-Loopback Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Bring up FlexCAN on CAN3 (EVKB J47) via a hybrid `FlexCAN_T4` port so a frame written from a Tx mailbox loops back (`CTRL1.LPB`) byte-exact into an Rx mailbox — proven in QEMU (polled + interrupt gates) and on real silicon.

**Architecture:** Four artifacts. (1) QEMU model gains an `MCR.SRXDIS` gate on loopback self-delivery so the gate is honest. (2) The `imxrt1176` core gains CAN3 clock/pin/IRQ *plumbing only* (FlexCAN_T4 carries its own register block). (3) A new `newdigate/FlexCAN` library adds a `#elif defined(__IMXRT1176__)` branch to each 1062-gated seam. (4) Two library gates (polled, interrupt) validate end-to-end.

**Tech Stack:** C++ templated Teensy library (`FlexCAN_T4.h`/`.tpp`), C QEMU device model, `teensy-cmake-macros` + `qrun` gate harness, `arm-none-eabi-gcc` 10.2.1, LinkServer + pyserial for HW.

**Reference spec:** `docs/superpowers/specs/2026-07-16-rt1176-flexcan-loopback-design.md`

---

## Repo boundaries (read before editing)

| Repo | git root | Push target |
|---|---|---|
| Core | `~/Development/rt1170/evkb/cores/imxrt1176` | github teensy-cores |
| QEMU | `~/Development/qemu2` | gitlab qemu-rt1170 |
| Library (NEW) | `~/Development/FlexCAN` | github newdigate/FlexCAN |
| Gates | `~/Development/FlexCAN/tests/` | (in the library repo) |

- `git -C ~/Development/rt1170/evkb/cores/imxrt1176 …` for core commits (NOT from `evkb/`).
- `evkb` itself is a separate repo; only the spec/plan docs are committed there.
- Do **not** sweep pre-existing `evkb` WIP into commits.

## File structure (what changes)

**QEMU (`~/Development/qemu2`):**
- Modify: `hw/net/can/imxrt_flexcan.c` — `flexcan_deliver()` gains an SRXDIS early-return + comment updates.
- Modify: `tests/functional/arm/imxrt1062/flexcan_loopback_test.c` — add SRXDIS negative-then-positive check.

**Core (`~/Development/rt1170/evkb/cores/imxrt1176`):**
- Modify: `core_pins.h` — add `IRQ_CAN1..IRQ_CAN3_ERROR` (44–49) to `IRQ_NUMBER_t`.
- Modify: `tools/gen_imxrt1176_h.py` — append CAN3 clock/LPCG + LPSR pad `#define`s.
- Modify: `imxrt1176.h` — same `#define`s, hand-added to keep both in sync (the header is generated; keep parity).

**Library (`~/Development/FlexCAN`, new):**
- Create: the repo (copied from `~/Development/FlexCAN_T4`, MIT LICENSE preserved).
- Modify: `FlexCAN_T4.h` — `CAN_DEV_TABLE` + `_CAN1/2/3` pointer guards.
- Modify: `FlexCAN_T4.tpp` — `#elif defined(__IMXRT1176__)` branches in constructor, ISR fwd-decls, `begin()`, `setTX()`, `setRX()`, `setBaudRate()` clock, the IFLAG/IMASK 64-bit guards, and the ISR definitions.
- Create: `tests/flexcan_loopback_test/` (CMake + runner + sketch).
- Create: `tests/flexcan_interrupt_test/` (CMake + runner + sketch).

---

## Ground-truth constants (verified — do not re-derive)

| Name | Value |
|---|---|
| CAN3 base | `0x40C3C000` (CAN1 `0x400C4000`, CAN2 `0x400C8000`) |
| CAN3 IRQ | `48` (CAN1 `44`, CAN2 `46`) |
| CAN3 clock root | index **24** → `CCM_CLOCK_ROOT24_CONTROL` @ `0x40CC0C00`, mux **1** (Osc24MOut, external 24 MHz), div raw **0** (÷1) = 24 MHz |
| CAN3 LPCG | **85** → `CCM_LPCG85_DIRECT` @ `0x40CC6AA0` |
| CAN3_TX pad | `GPIO_LPSR_00`: mux `0x40C08000`=**`0x10`** (ALT0\|SION), pad `0x40C08040`=**`0x02`** |
| CAN3_RX pad | `GPIO_LPSR_01`: mux `0x40C08004`=**`0x10`** (ALT0\|SION), pad `0x40C08044`=**`0x02`**, select-input `0x40C08080`=**`0x00`** |
| FlexCAN_T4 TX code | `FLEXCAN_MB_CODE_TX_ONCE` = `0xC` (== QEMU model's `FLEXCAN_CODE_TX_DATA`) |
| SRXDIS bit | `FLEXCAN_MCR_SRX_DIS` = `1<<17` (lib) / `FLEXCAN_MCR_SRXDIS` = `1u<<17` (QEMU, already defined) |
| After `begin()` | MB0–7 = `RX_EMPTY` (MB0-3 std, MB4-7 ext), MB8–15 = `TX_INACTIVE` (default `disableFIFO` layout) |

---

## Task 1: QEMU — SRXDIS-gate the loopback (honest loopback)

**Files:**
- Modify: `~/Development/qemu2/tests/functional/arm/imxrt1062/flexcan_loopback_test.c`
- Modify: `~/Development/qemu2/hw/net/can/imxrt_flexcan.c`

This task is self-contained in QEMU (no core/library dependency). TDD: extend the existing raw-poke test with an SRXDIS check that fails against the current model, then add the model gate.

- [ ] **Step 1: Add the failing SRXDIS check to the 1062 test**

In `flexcan_loopback_test.c`, add the MCR bit macro near the other MCR defines (after line 62 `#define MCR_FRZACK  (1u << 24)`):
```c
#define MCR_SRXDIS  (1u << 17)
```
Then, in `main()`, insert this block immediately **after** the `IFLAG1 = 0xFFFFFFFFu;` line (currently line 95, right after leaving freeze) and **before** the existing `/* Transmit from MB8 ... */` block:
```c
    /* --- SRXDIS negative check: with self-reception disabled, a looped-back
     * frame must NOT land in an Rx mailbox even under CTRL1.LPB. --- */
    MCR |= MCR_SRXDIS;
    MB_ID(8) = 0x123u << 18;
    MB_W0(8) = 0x11223344u;
    MB_W1(8) = 0x55667788u;
    MB_CS(8) = CODE(TX_DATA) | (8u << 16);
    if (MB_CODE(MB_CS(0)) == RX_FULL) {
        fail("srxdis: self-frame received while SRXDIS set");
    }
    MCR &= ~MCR_SRXDIS;              /* re-enable self-reception            */
    MB_CS(0) = CODE(RX_EMPTY);       /* re-arm MB0 as Rx                    */
    IFLAG1 = 0xFFFFFFFFu;            /* clear the Tx-complete flag from above */
```
(The existing transmit block that follows re-arms MB8 and performs the positive check — MB0 must now become `RX_FULL`.)

- [ ] **Step 2: Run it to verify it FAILS (model ignores SRXDIS today)**

```bash
cd ~/Development/qemu2
GCC=/Applications/ARM_10/bin/arm-none-eabi-gcc
"$GCC" -mcpu=cortex-m7 -mthumb -nostdlib -ffreestanding -O2 -Wall \
  -T tests/functional/arm/imxrt1062/p2test.ld \
  -o /tmp/flexcan_lb.elf tests/functional/arm/imxrt1062/flexcan_loopback_test.c
~/Development/rt1170/evkb/tools/qrun -M mimxrt1060-evk \
  -kernel /tmp/flexcan_lb.elf -display none -serial file:/tmp/flexcan_lb.uart
cat /tmp/flexcan_lb.uart
```
Expected: output contains `FLEXCAN LB FAIL: srxdis: self-frame received while SRXDIS set` (the current model delivers regardless of SRXDIS).

- [ ] **Step 3: Add the SRXDIS gate to the model**

In `~/Development/qemu2/hw/net/can/imxrt_flexcan.c`, in `flexcan_deliver()`, add an early return at the very top of the function body (before `uint32_t maxmb = ...`):
```c
    /*
     * Self-reception disable (MCR.SRXDIS): the module does not receive frames
     * it transmitted itself.  In this model the only delivery path IS internal
     * loopback of the module's own frames, so SRXDIS suppresses it entirely.
     * FlexCAN_T4's enableLoopBack() clears SRXDIS on purpose; without that the
     * looped frame is dropped on silicon, so gate it here to match.
     */
    if (s->mcr & FLEXCAN_MCR_SRXDIS) {
        return;
    }
```
Also update the file's top block comment (the `internal loopback (CTRL1.LPB)` bullet, ~line 20-23) to note "...and is suppressed when MCR.SRXDIS (self-reception disable) is set."

- [ ] **Step 4: Rebuild QEMU and verify the test PASSES**

```bash
cd ~/Development/qemu2/build && ninja qemu-system-arm
GCC=/Applications/ARM_10/bin/arm-none-eabi-gcc
"$GCC" -mcpu=cortex-m7 -mthumb -nostdlib -ffreestanding -O2 -Wall \
  -T ../tests/functional/arm/imxrt1062/p2test.ld \
  -o /tmp/flexcan_lb.elf ../tests/functional/arm/imxrt1062/flexcan_loopback_test.c
~/Development/rt1170/evkb/tools/qrun -M mimxrt1060-evk \
  -kernel /tmp/flexcan_lb.elf -display none -serial file:/tmp/flexcan_lb.uart
grep -q "FLEXCAN LB OK" /tmp/flexcan_lb.uart && echo "PASS: SRXDIS-gated loopback"
```
Expected: `FLEXCAN LB OK` present, `PASS: SRXDIS-gated loopback` printed. No `FAIL` lines.

- [ ] **Step 5: Commit**

```bash
cd ~/Development/qemu2
git add hw/net/can/imxrt_flexcan.c tests/functional/arm/imxrt1062/flexcan_loopback_test.c
git commit -m "hw/net/can: gate FlexCAN loopback delivery on MCR.SRXDIS

Self-reception disable must suppress the internal-loopback self-delivery,
matching silicon: FlexCAN_T4's enableLoopBack() clears SRXDIS to enable it.
Extends the 1062 loopback test with an SRXDIS negative check.

Co-Authored-By: Claude Opus 4.8 <noreply@anthropic.com>"
```

---

## Task 2: Core — add CAN IRQ numbers

**Files:**
- Modify: `~/Development/rt1170/evkb/cores/imxrt1176/core_pins.h`

- [ ] **Step 1: Insert the CAN IRQ entries**

In `IRQ_NUMBER_t`, immediately after the line `IRQ_LPSPI1 = 38, IRQ_LPSPI2, IRQ_LPSPI3, IRQ_LPSPI4, IRQ_LPSPI5, IRQ_LPSPI6, /* = 43 */`, add a new line:
```c
    IRQ_CAN1 = 44, IRQ_CAN1_ERROR = 45, IRQ_CAN2 = 46, IRQ_CAN2_ERROR = 47, IRQ_CAN3 = 48, IRQ_CAN3_ERROR = 49,
```
(Values 44–49 are in the unoccupied 44–75 gap; the next existing entry is `IRQ_SAI1 = 76`, so nothing renumbers.)

- [ ] **Step 2: Verify the value resolves to 48**

```bash
cd ~/Development/rt1170/evkb/cores/imxrt1176
printf '#include "core_pins.h"\n#include <stdio.h>\nint main(){printf("%%d\\n",(int)IRQ_CAN3);}\n' > /tmp/irqchk.c
cc -I. /tmp/irqchk.c -o /tmp/irqchk 2>/dev/null && /tmp/irqchk
```
Expected: prints `48`. (If the host `cc` can't parse the ARM headers, instead just `grep -n 'IRQ_CAN3 = 48' core_pins.h` — must match.)

- [ ] **Step 3: Commit**

```bash
git -C ~/Development/rt1170/evkb/cores/imxrt1176 add core_pins.h
git -C ~/Development/rt1170/evkb/cores/imxrt1176 commit -m "imxrt1176: add FlexCAN IRQ numbers (CAN1/2/3 = 44/46/48)

Co-Authored-By: Claude Opus 4.8 <noreply@anthropic.com>"
```

---

## Task 3: Core — CAN3 clock/LPCG + LPSR pad defines

**Files:**
- Modify: `~/Development/rt1170/evkb/cores/imxrt1176/tools/gen_imxrt1176_h.py`
- Modify: `~/Development/rt1170/evkb/cores/imxrt1176/imxrt1176.h`

The header is generated, but per project convention we keep BOTH files in sync by hand-adding the identical block (avoids a risky full regenerate).

- [ ] **Step 1: Add the CAN3 block to the generator**

In `tools/gen_imxrt1176_h.py`, find the LPUART1 hardcoded block (the lines defining `CCM_CLOCK_ROOT25_CONTROL` / `CCM_LPCG86_DIRECT`, ~line 173-178) and add a sibling block right after it:
```python
    L += ["",
          "/* FlexCAN3 (J47): clock root 24 (mux 1 = Osc24MOut, div 1 = 24 MHz) +",
          " * LPCG 85, and the CAN3 TX/RX pads in the LPSR IOMUXC (0x40C08000). */",
          "#define CCM_CLOCK_ROOT24_CONTROL (*(volatile uint32_t *)0x40CC0C00u)",
          "#define CCM_LPCG85_DIRECT        (*(volatile uint32_t *)0x40CC6AA0u)",
          "#define IOMUXC_SW_MUX_CTL_PAD_GPIO_LPSR_00 (*(volatile uint32_t *)0x40C08000u)",
          "#define IOMUXC_SW_MUX_CTL_PAD_GPIO_LPSR_01 (*(volatile uint32_t *)0x40C08004u)",
          "#define IOMUXC_SW_PAD_CTL_PAD_GPIO_LPSR_00 (*(volatile uint32_t *)0x40C08040u)",
          "#define IOMUXC_SW_PAD_CTL_PAD_GPIO_LPSR_01 (*(volatile uint32_t *)0x40C08044u)",
          "#define IOMUXC_FLEXCAN3_RX_SELECT_INPUT    (*(volatile uint32_t *)0x40C08080u)"]
```
**First** grep to confirm none already exist (avoid a duplicate `#define`):
```bash
grep -nE 'GPIO_LPSR_00|GPIO_LPSR_01|CCM_CLOCK_ROOT24|CCM_LPCG85|FLEXCAN3_RX_SELECT' \
  ~/Development/rt1170/evkb/cores/imxrt1176/tools/gen_imxrt1176_h.py \
  ~/Development/rt1170/evkb/cores/imxrt1176/imxrt1176.h
```
Expected: no matches (all five names are new). If `LPSR_00/01` already exist elsewhere, drop the duplicate mux/pad lines and keep only the new ones.

- [ ] **Step 2: Add the identical block to `imxrt1176.h`**

In `imxrt1176.h`, find the LPUART1 block (`CCM_CLOCK_ROOT25_CONTROL` / `CCM_LPCG86_DIRECT`, ~line 329-330) and add right after it:
```c
/* FlexCAN3 (J47): clock root 24 (mux 1 = Osc24MOut, div 1 = 24 MHz) +
 * LPCG 85, and the CAN3 TX/RX pads in the LPSR IOMUXC (0x40C08000). */
#define CCM_CLOCK_ROOT24_CONTROL (*(volatile uint32_t *)0x40CC0C00u)
#define CCM_LPCG85_DIRECT        (*(volatile uint32_t *)0x40CC6AA0u)
#define IOMUXC_SW_MUX_CTL_PAD_GPIO_LPSR_00 (*(volatile uint32_t *)0x40C08000u)
#define IOMUXC_SW_MUX_CTL_PAD_GPIO_LPSR_01 (*(volatile uint32_t *)0x40C08004u)
#define IOMUXC_SW_PAD_CTL_PAD_GPIO_LPSR_00 (*(volatile uint32_t *)0x40C08040u)
#define IOMUXC_SW_PAD_CTL_PAD_GPIO_LPSR_01 (*(volatile uint32_t *)0x40C08044u)
#define IOMUXC_FLEXCAN3_RX_SELECT_INPUT    (*(volatile uint32_t *)0x40C08080u)
```

- [ ] **Step 3: Verify generator parity (regenerate to a temp file and diff)**

```bash
cd ~/Development/rt1170/evkb/cores/imxrt1176
python3 tools/gen_imxrt1176_h.py --help 2>/dev/null | head -1 || true
# Regenerate to a temp path if the generator supports an output arg; otherwise
# just confirm the two new-block greps match in both files:
grep -c 'CCM_CLOCK_ROOT24_CONTROL\|CCM_LPCG85_DIRECT\|GPIO_LPSR_00\|FLEXCAN3_RX_SELECT_INPUT' imxrt1176.h
```
Expected: `imxrt1176.h` contains all five defines (count ≥ 5). If the generator writes `imxrt1176.h` directly with no output arg, instead run it and `git diff imxrt1176.h` — the diff must contain **only** the CAN3 block (no reordering).

- [ ] **Step 4: Commit**

```bash
git -C ~/Development/rt1170/evkb/cores/imxrt1176 add tools/gen_imxrt1176_h.py imxrt1176.h
git -C ~/Development/rt1170/evkb/cores/imxrt1176 commit -m "imxrt1176: add CAN3 clock root 24 + LPCG 85 + LPSR TX/RX pad defines

Co-Authored-By: Claude Opus 4.8 <noreply@anthropic.com>"
```

---

## Task 4: Library — scaffold `newdigate/FlexCAN`

**Files:**
- Create: `~/Development/FlexCAN/` (repo)

- [ ] **Step 1: Seed the repo from upstream, preserving MIT LICENSE**

```bash
SRC=~/Development/FlexCAN_T4
DST=~/Development/FlexCAN
mkdir -p "$DST"
# Copy the library sources (NOT the upstream .git), keep LICENSE + attribution.
cp "$SRC"/FlexCAN_T4.h "$SRC"/FlexCAN_T4.tpp "$SRC"/imxrt_flexcan.h \
   "$SRC"/kinetis_flexcan.h "$SRC"/circular_buffer.h "$SRC"/isotp.h \
   "$SRC"/isotp.tpp "$SRC"/isotp_server.h "$SRC"/isotp_server.tpp \
   "$SRC"/FlexCAN_T4FD.tpp "$SRC"/FlexCAN_T4FDTimings.tpp \
   "$SRC"/LICENSE "$SRC"/README.md "$SRC"/keywords.txt "$DST"/
mkdir -p "$DST"/tests
cd "$DST" && git init -q && git add -A
git commit -q -m "Import FlexCAN_T4 (MIT, Antonio Brewer) as newdigate/FlexCAN base

Co-Authored-By: Claude Opus 4.8 <noreply@anthropic.com>"
ls "$DST"
```
Expected: `FlexCAN_T4.h`, `FlexCAN_T4.tpp`, `imxrt_flexcan.h`, `LICENSE`, `tests/` present.

- [ ] **Step 2: Confirm the license is MIT**

```bash
head -3 ~/Development/FlexCAN/LICENSE
```
Expected: `MIT License` / `Copyright (c) 2019 Antonio Brewer`. (A full per-file header audit is Task 7.)

---

## Task 5: Library — the `__IMXRT1176__` port + polled loopback gate

**Files:**
- Modify: `~/Development/FlexCAN/FlexCAN_T4.h`
- Modify: `~/Development/FlexCAN/FlexCAN_T4.tpp`
- Create: `~/Development/FlexCAN/tests/flexcan_loopback_test/CMakeLists.txt`
- Create: `~/Development/FlexCAN/tests/flexcan_loopback_test/run_qemu_flexcan.sh`
- Create: `~/Development/FlexCAN/tests/flexcan_loopback_test/flexcan_loopback_test.cpp`

Write the gate first (red), then add the seams until it builds and passes (green).

- [ ] **Step 1: Write the loopback gate sketch**

`~/Development/FlexCAN/tests/flexcan_loopback_test/flexcan_loopback_test.cpp`:
```cpp
#include "Arduino.h"
#include "HardwareSerial.h"
#include "FlexCAN_T4.h"

static void hex2(uint8_t v) { const char* h="0123456789ABCDEF"; Serial1.print(h[v>>4]); Serial1.print(h[v&0xF]); }

FlexCAN_T4<CAN3, RX_SIZE_256, TX_SIZE_16> can3;

void setup() {
	Serial1.begin(115200);
	while (!Serial1) {}

	can3.begin();                 // clock/pins/IRQ + default MB layout (0-7 RX, 8-15 TX)
	can3.setBaudRate(1000000);    // classic 1 Mbit; clears CTRL1.LOM (TX default)
	can3.enableLoopBack(1);       // clears MCR.SRXDIS, sets CTRL1.LPB

	CAN_message_t tx;
	tx.id = 0x123;
	tx.len = 8;
	for (uint8_t i = 0; i < 8; i++) tx.buf[i] = 0x11 * (i + 1);   // 11 22 .. 88
	can3.write(tx);               // finds MB8 (TX_INACTIVE) -> transmit -> loopback to MB0

	CAN_message_t rx;
	bool got = false;
	for (int tries = 0; tries < 100000 && !got; tries++) {
		if (can3.read(rx)) got = true;
	}

	bool ok = got && rx.id == 0x123 && rx.len == 8;
	for (uint8_t i = 0; i < 8; i++) if (rx.buf[i] != (uint8_t)(0x11 * (i + 1))) ok = false;

	Serial1.print("flexcan got="); Serial1.print(got ? 1 : 0);
	Serial1.print(" id=0x"); hex2(rx.id >> 8); hex2(rx.id & 0xFF);
	Serial1.print(" len="); Serial1.print(rx.len);
	Serial1.print(" data=");
	for (uint8_t i = 0; i < 8; i++) { hex2(rx.buf[i]); Serial1.print(' '); }
	Serial1.println();
	Serial1.println(ok ? "FLEXCAN_LOOPBACK=PASS" : "FLEXCAN_LOOPBACK=FAIL");
}
void loop() {}
```

- [ ] **Step 2: Write the gate CMakeLists.txt**

`~/Development/FlexCAN/tests/flexcan_loopback_test/CMakeLists.txt`:
```cmake
cmake_minimum_required(VERSION 3.24)
project(flexcan_loopback_test)

set(TEENSY_VERSION 117 CACHE STRING "")

if(NOT DEFINED EVKB)
    set(EVKB $ENV{HOME}/Development/rt1170/evkb)
endif()

include(FetchContent)
FetchContent_Declare(teensy_cmake_macros SOURCE_DIR ${EVKB}/teensy-cmake-macros)
FetchContent_MakeAvailable(teensy_cmake_macros)
include(${teensy_cmake_macros_SOURCE_DIR}/CMakeLists.include.txt)

import_arduino_library(cores ${EVKB}/cores/imxrt1176)
import_arduino_library(FlexCAN ${CMAKE_CURRENT_LIST_DIR}/../..)

teensy_add_executable(flexcan_loopback_test flexcan_loopback_test.cpp)
teensy_target_link_libraries(flexcan_loopback_test cores FlexCAN)

target_link_libraries(flexcan_loopback_test.elf stdc++)
```
Then copy the toolchain dir from the SPI gate so `cmake` finds it:
```bash
cp -R ~/Development/SPI/tests/spi_loopback_test/toolchain \
      ~/Development/FlexCAN/tests/flexcan_loopback_test/ 2>/dev/null || \
  echo "check where the toolchain file lives; SPI gate references toolchain/rt1170-evkb.toolchain.cmake"
```

- [ ] **Step 3: Write the runner**

`~/Development/FlexCAN/tests/flexcan_loopback_test/run_qemu_flexcan.sh`:
```sh
#!/bin/sh
set -e
QEMU=~/Development/rt1170/evkb/tools/qrun
DIR=$(cd "$(dirname "$0")" && pwd)
. ~/Development/rt1170/evkb/tools/gate-lib.sh
gate_init
ELF="$DIR/build/flexcan_loopback_test.elf"; OUT="$DIR/flexcan.uart"
rm -f "$OUT"
"$QEMU" -M mimxrt1170-evk -global fsl-imxrt1170.boot-xip=on -kernel "$ELF" \
    -display none -serial file:"$OUT" -d guest_errors -D "$DIR/flexcan.dbg" &
P=$!; gate_pid $P; sleep 3; kill $P 2>/dev/null; wait $P 2>/dev/null || true
echo "==== captured ===="; cat "$OUT"
grep -q "FLEXCAN_LOOPBACK=PASS" "$OUT" || { echo "FAIL: FlexCAN loopback"; exit 1; }
echo "PASS: FlexCAN CAN3 internal loopback verified"
```
```bash
chmod +x ~/Development/FlexCAN/tests/flexcan_loopback_test/run_qemu_flexcan.sh
```

- [ ] **Step 4: Attempt to build — expect a COMPILE/LINK failure (red)**

```bash
cd ~/Development/FlexCAN/tests/flexcan_loopback_test
rm -rf build && cmake -B build -DCMAKE_TOOLCHAIN_FILE=toolchain/rt1170-evkb.toolchain.cmake . && cmake --build build 2>&1 | tail -20
```
Expected: FAIL — e.g. `CAN3` not a member of `CAN_DEV_TABLE` for `__IMXRT1176__`, and/or undefined `IRQ_CAN3`/clock symbols once the table compiles. This confirms the port is needed.

- [ ] **Step 5: Port seam — `CAN_DEV_TABLE` + instance pointers (`FlexCAN_T4.h`)**

In `FlexCAN_T4.h`, change the `CAN_DEV_TABLE` enum (currently `#if defined(__IMXRT1062__)` ... CAN1/2/3) to add a 1176 arm — insert before the closing `#endif` of that first block:
```cpp
typedef enum CAN_DEV_TABLE {
#if defined(__IMXRT1062__)
  CAN0 = (uint32_t)0x0,
  CAN1 = (uint32_t)0x401D0000,
  CAN2 = (uint32_t)0x401D4000,
  CAN3 = (uint32_t)0x401D8000
#elif defined(__IMXRT1176__)
  CAN1 = (uint32_t)0x400C4000,
  CAN2 = (uint32_t)0x400C8000,
  CAN3 = (uint32_t)0x40C3C000
#endif
```
And widen the instance-pointer guard (currently `#if defined(__IMXRT1062__)` around `_CAN1/_CAN2/_CAN3`):
```cpp
#if defined(__IMXRT1062__) || defined(__IMXRT1176__)
static FlexCAN_T4_Base* _CAN1 = nullptr;
static FlexCAN_T4_Base* _CAN2 = nullptr;
static FlexCAN_T4_Base* _CAN3 = nullptr;
#endif
```

- [ ] **Step 6: Port seam — constructor + ISR forward decls (`FlexCAN_T4.tpp`)**

Widen the ISR forward-declaration guard at the top of `FlexCAN_T4.tpp` (currently `#if defined(__IMXRT1062__)` around `static void flexcan_isr_canN();`):
```cpp
#if defined(__IMXRT1062__) || defined(__IMXRT1176__)
static void flexcan_isr_can3();
static void flexcan_isr_can2();
static void flexcan_isr_can1();
#endif
```
In the constructor `FCTP_FUNC FCTP_OPT::FlexCAN_T4()`, add a 1176 arm mirroring the 1062 `_CANx = this` assignment (find the `#if defined(__IMXRT1062__)` block that does `if ( _bus == CAN3 ) _CAN3 = this;`) — change it to `#if defined(__IMXRT1062__) || defined(__IMXRT1176__)` (the assignment is pure and portable).

- [ ] **Step 7: Port seam — `begin()` clock/IRQ block (`FlexCAN_T4.tpp`)**

Directly after the 1062 `#if defined(__IMXRT1062__) ... #endif` block inside `begin()` (the one setting `nvicIrq`/`_VectorsRam`/`CCM_CCGRx`), add:
```cpp
#if defined(__IMXRT1176__)
  /* CAN3 protocol clock: root 24 = Osc24MOut (mux 1) / 1 = 24 MHz, LPCG 85 on. */
  if ( _bus == CAN3 ) {
    CCM_CLOCK_ROOT24_CONTROL = CCM_CLOCK_ROOT_CONTROL_MUX(1) | CCM_CLOCK_ROOT_CONTROL_DIV(0);
    __asm__ volatile("dsb" ::: "memory");
    CCM_LPCG85_DIRECT = 1u;
    __asm__ volatile("dsb" ::: "memory");
    nvicIrq = IRQ_CAN3;
    attachInterruptVector(IRQ_CAN3, flexcan_isr_can3);
    busNumber = 3;
  }
  if ( _bus == CAN2 ) { nvicIrq = IRQ_CAN2; attachInterruptVector(IRQ_CAN2, flexcan_isr_can2); busNumber = 2; }
  if ( _bus == CAN1 ) { nvicIrq = IRQ_CAN1; attachInterruptVector(IRQ_CAN1, flexcan_isr_can1); busNumber = 1; }
#endif
```
(CAN1/2 get no clock/pins — no consumer; only CAN3 is fully wired. The common block below and the closing `NVIC_ENABLE_IRQ(nvicIrq)` are already portable.)

- [ ] **Step 8: Port seam — `setTX()` / `setRX()` LPSR pads (`FlexCAN_T4.tpp`)**

After the 1062 block inside `setTX()`, add:
```cpp
#if defined(__IMXRT1176__)
  if ( _bus == CAN3 ) {
    /* GPIO_LPSR_00 -> FLEXCAN3_TX, ALT0 | SION (0x10), pad 0x02. */
    IOMUXC_SW_MUX_CTL_PAD_GPIO_LPSR_00 = 0x10;
    IOMUXC_SW_PAD_CTL_PAD_GPIO_LPSR_00 = 0x02;
  }
#endif
```
After the 1062 block inside `setRX()`, add:
```cpp
#if defined(__IMXRT1176__)
  if ( _bus == CAN3 ) {
    /* GPIO_LPSR_01 -> FLEXCAN3_RX, select-input daisy 0, ALT0 | SION, pad 0x02. */
    IOMUXC_FLEXCAN3_RX_SELECT_INPUT = 0x00;
    IOMUXC_SW_MUX_CTL_PAD_GPIO_LPSR_01 = 0x10;
    IOMUXC_SW_PAD_CTL_PAD_GPIO_LPSR_01 = 0x02;
  }
#endif
```
(Not exercised by the loopback gate — internal loopback does not route to the pads — but correct for the real bus. The LPSR IOMUXC clock is already up, per the working LPI2C5/Wire2 precedent on LPSR_04/05.)

- [ ] **Step 9: Port seam — `setBaudRate()` clock + 64-bit IFLAG/IMASK guards (`FlexCAN_T4.tpp`)**

In `setBaudRate()`, change the clock-frequency selection from:
```cpp
#if defined(__IMXRT1062__)
  uint32_t clockFreq = getClock() * 1000000;
#else
  uint32_t clockFreq = 16000000;
#endif
```
to add a 1176 arm:
```cpp
#if defined(__IMXRT1062__)
  uint32_t clockFreq = getClock() * 1000000;
#elif defined(__IMXRT1176__)
  uint32_t clockFreq = 24000000;   /* CAN3 root = Osc24MOut / 1 */
#else
  uint32_t clockFreq = 16000000;
#endif
```
Then widen the four 64-bit accessor guards (`readIFLAG`, `writeIFLAG`, `writeIMASK`, `readIMASK`, ~tpp:388-419) from `#if defined(__IMXRT1062__)` to `#if defined(__IMXRT1062__) || defined(__IMXRT1176__)` (the 1176 FlexCAN also has 64 mailboxes → IFLAG2/IMASK2).

- [ ] **Step 10: Port seam — ISR definitions (`FlexCAN_T4.tpp`)**

Widen the ISR-definition guard (currently `#if defined(__IMXRT1062__)` around `static void flexcan_isr_canN() { if (_CANx) _CANx->flexcan_interrupt(); }`) to `#if defined(__IMXRT1062__) || defined(__IMXRT1176__)` (the dispatch body is portable).

- [ ] **Step 11: Rebuild — expect it to COMPILE and LINK now**

```bash
cd ~/Development/FlexCAN/tests/flexcan_loopback_test
rm -rf build && cmake -B build -DCMAKE_TOOLCHAIN_FILE=toolchain/rt1170-evkb.toolchain.cmake . && cmake --build build 2>&1 | tail -15
```
Expected: builds to `build/flexcan_loopback_test.elf` with no errors. If a *further* 1062-only symbol surfaces (e.g. inside `flexcan_interrupt()` or `writeTxMailbox`), widen that guard to include `__IMXRT1176__` and rebuild (these bodies are pure FlexCAN register ops — see the risk table).

- [ ] **Step 12: Run the gate — expect PASS (green)**

```bash
cd ~/Development/qemu2/build && ninja qemu-system-arm   # ensure Task-1 model is built
~/Development/FlexCAN/tests/flexcan_loopback_test/run_qemu_flexcan.sh
```
Expected: captured UART shows `flexcan got=1 id=0x0123 len=8 data=11 22 33 44 55 66 77 88` and `FLEXCAN_LOOPBACK=PASS`, then `PASS: FlexCAN CAN3 internal loopback verified`.

- [ ] **Step 13: Commit the library port + gate**

```bash
cd ~/Development/FlexCAN
git add FlexCAN_T4.h FlexCAN_T4.tpp tests/flexcan_loopback_test
git commit -m "FlexCAN_T4: add __IMXRT1176__ branch (CAN3) + polled loopback gate

CAN_DEV_TABLE 1176 bases, begin() clock-root 24 + LPCG 85 + IRQ 48 via
attachInterruptVector, setTX/RX LPSR_00/01 pads, setBaudRate 24 MHz, 64-bit
IFLAG/IMASK guards. Gate proves CTRL1.LPB loopback byte-exact into MB0.

Co-Authored-By: Claude Opus 4.8 <noreply@anthropic.com>"
```

---

## Task 6: Library — interrupt (onReceive) gate

**Files:**
- Create: `~/Development/FlexCAN/tests/flexcan_interrupt_test/{CMakeLists.txt,run_qemu_flexcan.sh,flexcan_interrupt_test.cpp}`

- [ ] **Step 1: Write the interrupt gate sketch**

`~/Development/FlexCAN/tests/flexcan_interrupt_test/flexcan_interrupt_test.cpp`:
```cpp
#include "Arduino.h"
#include "HardwareSerial.h"
#include "FlexCAN_T4.h"

static void hex2(uint8_t v) { const char* h="0123456789ABCDEF"; Serial1.print(h[v>>4]); Serial1.print(h[v&0xF]); }

FlexCAN_T4<CAN3, RX_SIZE_256, TX_SIZE_16> can3;

static volatile bool rx_fired = false;
static CAN_message_t rx_msg;

static void onCan3(const CAN_message_t &msg) {
	rx_msg = msg;
	rx_fired = true;
}

void setup() {
	Serial1.begin(115200);
	while (!Serial1) {}

	can3.begin();
	can3.setBaudRate(1000000);
	can3.enableLoopBack(1);
	can3.onReceive(onCan3);       // global RX callback
	can3.enableMBInterrupts();    // enable MB interrupts incl. RX MB0

	CAN_message_t tx;
	tx.id = 0x321;
	tx.len = 8;
	for (uint8_t i = 0; i < 8; i++) tx.buf[i] = 0xA0 + i;   // A0..A7
	can3.write(tx);               // loopback delivery raises the MB IRQ

	for (int tries = 0; tries < 100000 && !rx_fired; tries++) {
		can3.events();            // FlexCAN_T4 drains the ISR-captured frame to the callback
	}

	bool ok = rx_fired && rx_msg.id == 0x321 && rx_msg.len == 8;
	for (uint8_t i = 0; i < 8; i++) if (rx_msg.buf[i] != (uint8_t)(0xA0 + i)) ok = false;

	Serial1.print("flexcan irq fired="); Serial1.print(rx_fired ? 1 : 0);
	Serial1.print(" id=0x"); hex2(rx_msg.id >> 8); hex2(rx_msg.id & 0xFF);
	Serial1.print(" data=");
	for (uint8_t i = 0; i < 8; i++) { hex2(rx_msg.buf[i]); Serial1.print(' '); }
	Serial1.println();
	Serial1.println(ok ? "FLEXCAN_IRQ=PASS" : "FLEXCAN_IRQ=FAIL");
}
void loop() {}
```
**Note on `onReceive`/`events()`:** FlexCAN_T4's ISR captures RX frames into an internal queue; `events()` (or `read()`) dispatches them to the registered handler. If, on inspection of the 1176-compiled path, the callback is invoked directly from the ISR instead, the `events()` loop is harmless. Confirm the dispatch path by reading `flexcan_interrupt()` + `events()` during implementation; adjust the drain call if needed.

- [ ] **Step 2: Write CMakeLists.txt + runner (copy from Task 5, rename)**

Copy `CMakeLists.txt`, `run_qemu_flexcan.sh`, and `toolchain/` from `../flexcan_loopback_test/`, then in both files replace `flexcan_loopback_test` → `flexcan_interrupt_test` and the grep sentinel `FLEXCAN_LOOPBACK=PASS` → `FLEXCAN_IRQ=PASS`:
```bash
D=~/Development/FlexCAN/tests
cp -R "$D/flexcan_loopback_test/toolchain" "$D/flexcan_interrupt_test/"
sed 's/flexcan_loopback_test/flexcan_interrupt_test/g' \
  "$D/flexcan_loopback_test/CMakeLists.txt" > "$D/flexcan_interrupt_test/CMakeLists.txt"
sed -e 's/flexcan_loopback_test/flexcan_interrupt_test/g' \
    -e 's/FLEXCAN_LOOPBACK=PASS/FLEXCAN_IRQ=PASS/' \
    -e 's/FlexCAN loopback/FlexCAN interrupt/' \
  "$D/flexcan_loopback_test/run_qemu_flexcan.sh" > "$D/flexcan_interrupt_test/run_qemu_flexcan.sh"
chmod +x "$D/flexcan_interrupt_test/run_qemu_flexcan.sh"
```

- [ ] **Step 3: Build**

```bash
cd ~/Development/FlexCAN/tests/flexcan_interrupt_test
rm -rf build && cmake -B build -DCMAKE_TOOLCHAIN_FILE=toolchain/rt1170-evkb.toolchain.cmake . && cmake --build build 2>&1 | tail -15
```
Expected: builds to `build/flexcan_interrupt_test.elf`.

- [ ] **Step 4: Run the gate — expect PASS**

```bash
~/Development/FlexCAN/tests/flexcan_interrupt_test/run_qemu_flexcan.sh
```
Expected: `flexcan irq fired=1 id=0x0321 data=A0 A1 A2 A3 A4 A5 A6 A7`, `FLEXCAN_IRQ=PASS`, `PASS`. If `fired=0`, the CAN3 vector isn't reaching the ISR — verify `attachInterruptVector(IRQ_CAN3,...)` ran and the QEMU model asserts IRQ 48 on MB0 delivery (Task-1 model wires `flexcan[2] -> NVIC 48`); debug with systematic-debugging before proceeding.

- [ ] **Step 5: Commit**

```bash
cd ~/Development/FlexCAN
git add tests/flexcan_interrupt_test
git commit -m "FlexCAN_T4: add CAN3 interrupt/onReceive loopback gate

Co-Authored-By: Claude Opus 4.8 <noreply@anthropic.com>"
```

---

## Task 7: License audit + push all repos

**Files:** (audit only, then push)

- [ ] **Step 1: Per-file license audit of the new library**

```bash
cd ~/Development/FlexCAN
for f in FlexCAN_T4.h FlexCAN_T4.tpp imxrt_flexcan.h circular_buffer.h kinetis_flexcan.h; do
  echo "== $f =="; head -12 "$f" | grep -iE 'license|copyright|GPL|MIT|BSD|SPDX' || echo "(no explicit header)"
done
```
Expected: MIT / no-copyleft. Flag any GPL/LGPL. `imxrt_flexcan.h` carries a stale `kinetis_flexcan.h` comment (cosmetic) — confirm it has no copyleft license claim. If any file is copyleft or unlicensed, stop and surface it before pushing (per the prefer-permissive-licenses rule).

- [ ] **Step 2: Create the github repo and push the library**

```bash
cd ~/Development/FlexCAN
gh repo create newdigate/FlexCAN --public --source=. --remote=origin --push \
  --description "FlexCAN_T4 with an i.MX RT1176 (CAN3) branch" 2>&1 | tail -5 || \
  { git remote add origin git@github.com:newdigate/FlexCAN.git; git push -u origin HEAD; }
```

- [ ] **Step 3: Push core + QEMU**

```bash
git -C ~/Development/rt1170/evkb/cores/imxrt1176 push
git -C ~/Development/qemu2 push
```
Expected: core (teensy-cores) and qemu2 push succeed. `evkb` stays local (only spec/plan docs live there — commit those separately if desired).

---

## Task 8: Hardware verification (final arbiter)

**Files:** none (flash + capture)

Both gate ELFs run on real silicon. Internal loopback needs no wiring or partner.

- [ ] **Step 1: Flash + capture the polled loopback gate**

```bash
pkill -9 -f LinkServer; pkill -9 -f redlinkserv; sleep 1
# start the pyserial VCOM reader FIRST (background), then flash:
python3 - <<'PY' &
import serial,sys,time
p=serial.Serial('/dev/cu.usbmodem5DQ2DDHVWO5EI3',115200,timeout=15)
t=time.time()
while time.time()-t<15:
    line=p.readline().decode(errors='replace').strip()
    if line: print(line)
    if 'FLEXCAN_LOOPBACK' in line: break
PY
sleep 1
/Applications/LinkServer_26.6.137/LinkServer run MIMXRT1176:MIMXRT1170-EVKB \
  ~/Development/FlexCAN/tests/flexcan_loopback_test/build/flexcan_loopback_test.elf
wait
```
Expected on VCOM: `flexcan got=1 id=0x0123 len=8 data=11 22 33 44 55 66 77 88` and `FLEXCAN_LOOPBACK=PASS`.

- [ ] **Step 2: Flash + capture the interrupt gate**

Repeat Step 1 with the `flexcan_interrupt_test` ELF; expect `FLEXCAN_IRQ=PASS` (`id=0x0321`, `data=A0..A7`).

- [ ] **Step 3: If HW differs from QEMU, model the silicon**

Per the method, a QEMU-vs-silicon gap (e.g. clock/bit-timing, a missing pad enable, SRXDIS/LOM nuance) is a first-class QEMU fix — capture the real behaviour (Saleae is N/A for internal loopback), correct the model or driver, re-run the gate, and re-flash. Record the result in a HW-RESULTS note.

- [ ] **Step 4: Write the memory note**

Add `~/.claude/projects/-Users-nicholasnewdigate-Development-rt1170/memory/rt1176-flexcan.md` (+ MEMORY.md pointer) capturing: CAN3 @ 0x40C3C000/IRQ48, clock root24 mux1/÷1=24MHz + LPCG85, LPSR_00/01 pads, the SRXDIS honesty fix, hybrid-port seams, both gates' PASS strings, and any HW gotcha found in Step 3. Link `[[rt1176-spi-library-move]]`, `[[rt1176-usb-host-hid]]` (J47=CAN3), `[[rt1170-evkb-flashing]]`.

---

## Deferred (not in this plan)
CAN-FD (`FlexCAN_T4FD`), a real two-node bus (2nd EVKB / USB-CAN + transceiver & standby-GPIO check; remove J102/J103), CAN1/CAN2 pin+clock bring-up.
