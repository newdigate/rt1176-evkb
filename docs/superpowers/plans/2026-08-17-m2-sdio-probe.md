# W1 — SDIO host layer + M.2 probe Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Bring uSDHC1 up in **SDIO** mode and read the M2-MAYA-W161's identity
off the wire — manufacturer code and card ID from its CIS — proving the IW416 is
alive and addressable.

**Architecture:** A new `M2Radio` sibling library whose `sdio/` subdirectory is a
minimal SDIO host: reset, clock, CMD5/CMD3/CMD7 enumeration, CMD52 direct
register access, and a CIS tuple walk. It is **not** built on SdFat — SdFat's
`SdioTeensy` is an SD-*memory* driver (CMD0/CMD8/ACMD41/CMD2) with no SDIO path.
What is reused, by copying rather than coupling, is its proven RT1176
clock/pad/reset sequence, which is the part that already works on this silicon.

**Tech Stack:** C++ (Arduino/Teensyduino core), CMake + ARM GCC 10, QEMU
`mimxrt1170-evk`, POSIX-sh gates via `tools/gate-lib.sh`.

**Spec:** `docs/superpowers/specs/2026-08-17-m2-maya-w161-design.md`
**Board facts:** `docs/m2-evkb-revc3.md` — read this first, especially the
uSDHC1 two-socket warning.

---

## What this plan does NOT do

- No CMD53 block transfers, no firmware download, no DMA, no card interrupt.
  Those are W2.
- No SdFat changes. The two drivers stay independent and mutually exclusive.

---

## The three facts that shape every task

1. **uSDHC1 has two sockets on it.** The microSD slot J15 and the M.2 socket J54
   are wired in parallel. **Hardware runs need J15 empty.** Nothing in software
   can work around this.
2. **QEMU has no SDIO function model.** It attaches an SD *memory* card, which by
   spec ignores CMD5. So the QEMU gate asserts the **no-IO-function fallback** —
   a meaningful negative test, but not proof the module works. All positive
   evidence lives in `transcript_hw_evkb.txt`. Same relationship
   `display/vglite_probe` has to the GC355.
3. **The USDHC clock root depends on boot-ROM state.** SdFat's RT1176 branch
   carries a `HARDWARE-VERIFY` note: `CLOCK_ROOT58` is fed from `SYS_PLL2_PFD2`,
   which the core's `startup.c` never programs. If enumeration fails on silicon
   with sane-looking code, suspect the clock before the protocol.

---

## File structure

| File | Responsibility |
|---|---|
| `~/Development/M2Radio/LICENSE` (create) | MIT |
| `~/Development/M2Radio/library.properties` (create) | Arduino library metadata |
| `~/Development/M2Radio/README.md` (create) | What it is, the uSDHC1 warning |
| `~/Development/M2Radio/sdio/SdioHost.h` (create) | Public API: `begin()`, `cmd52Read/Write`, `readCis()`, status enum |
| `~/Development/M2Radio/sdio/SdioHost.cpp` (create) | USDHC1 bring-up + command engine + enumeration |
| `examples/networking/m2_sdio_probe/m2_sdio_probe.cpp` (create) | The probe firmware |
| `examples/networking/m2_sdio_probe/CMakeLists.txt` (create) | Build |
| `examples/networking/m2_sdio_probe/run_qemu.sh` (create) | The gate |
| `evkb.cmake` (modify) | `teensy_declare_library(M2Radio ...)` |
| `tools/license-audit.sh` (modify) | `GATES` entry |

**Keep `SdioHost.cpp` under ~400 lines.** If it grows past that, stop and report
— the command engine and the enumeration logic are separable and the split
should be a deliberate decision, not a drift.

---

## ⚠️ Decision required before Task 5

`teensy_declare_library` takes a GitHub URL and a SHA. `M2Radio` does not exist
on GitHub yet. Until it is **created and pushed**, local-first resolution works
but `-DEVKB_FORCE_FETCH=ON` and any fresh clone will fail, and the gate becomes
**SKIP-class** — which `CLAUDE.md` calls out specifically, because a SKIP hides
inside a count while every other exception shows up red and by name.

Creating a public repo is an outward-facing action. **Ask the user before doing
it.** Do not push without explicit approval. If approval is withheld, Task 5
must add a `docs/KNOWN-BROKEN-GATES.md` entry recording the SKIP and why, so it
is at least visible.

---

## Task 1: Create the M2Radio library skeleton

**Files:**
- Create: `~/Development/M2Radio/{LICENSE,library.properties,README.md,.gitignore}`

- [ ] **Step 1: Initialise the repo**

```bash
mkdir -p ~/Development/M2Radio/sdio && cd ~/Development/M2Radio && git init -b master
```

- [ ] **Step 2: Write `LICENSE`**

MIT, copyright `2026 Nicholas Newdigate`. Use the standard MIT text — copy the
structure from `~/Development/PXP/LICENSE` if one exists there, otherwise the
canonical SPDX MIT text.

- [ ] **Step 3: Write `library.properties`**

```
name=M2Radio
version=0.1.0
author=Nicholas Newdigate
maintainer=Nicholas Newdigate
sentence=SDIO host and NXP IW416 driver for the MIMXRT1170-EVKB M.2 socket.
paragraph=Brings up the u-blox MAYA-W1 / NXP IW416 combo module on the EVKB M.2 socket J54. The sdio/ subdirectory is a standalone SDIO host for uSDHC1.
category=Communication
architectures=imxrt1176
```

- [ ] **Step 4: Write `README.md`**

```markdown
# M2Radio

SDIO host and NXP **IW416** support for the MIMXRT1170-EVKB M.2 socket (J54),
as used by the u-blox `M2-MAYA-W161-00C` card.

Subdirectories are imported selectively:

    import_evkb_library(M2Radio sdio)

* `sdio/` — a minimal SDIO host for uSDHC1: enumeration (CMD5/CMD3/CMD7),
  direct register access (CMD52), and CIS parsing. Independent of SdFat.

## ⚠️ uSDHC1 carries two card sockets

On the MIMXRT1170-EVKB the M.2 socket **and** the microSD slot J15 are wired in
parallel onto the same six MCU balls, every series resistor fitted. They cannot
both be used. **Remove any microSD card before using this library.**

There is no way to power the M.2 module down: `WL_3V3` comes through a ferrite
with no switch. Physical removal is the only isolation.

Full board map: `docs/m2-evkb-revc3.md` in the `rt1176-evkb` repo.

## Licence

MIT. Nothing is vendored here.
```

- [ ] **Step 5: Write `.gitignore`**

```
build/
build-*/
.DS_Store
```

- [ ] **Step 6: Commit**

```bash
cd ~/Development/M2Radio && git add -A && \
  git commit -m "M2Radio: library skeleton

SDIO host and IW416 support for the MIMXRT1170-EVKB M.2 socket. Subdirectories
are imported selectively, following the MipiDisplay pattern."
```

Do **not** push yet — see the decision note above.

---

## Task 2: The failing gate

Written first so red→green is real. It fails at build: `SdioHost` does not exist.

**Files:**
- Create: `examples/networking/m2_sdio_probe/m2_sdio_probe.cpp`
- Create: `examples/networking/m2_sdio_probe/CMakeLists.txt`
- Create: `examples/networking/m2_sdio_probe/run_qemu.sh`

- [ ] **Step 1: Write the probe firmware**

Create `examples/networking/m2_sdio_probe/m2_sdio_probe.cpp`:

```cpp
// M.2 (J54) SDIO probe -- brings uSDHC1 up in SDIO mode and reads the card's
// identity off the wire.  Board map: docs/m2-evkb-revc3.md.
//
// WARNING: uSDHC1 carries BOTH the M.2 socket and the microSD slot J15 on this
// board.  Remove any microSD card before running this on hardware.
//
// A GREEN QEMU GATE DOES NOT MEAN THE MODULE WORKS.  QEMU has no SDIO function
// model -- it attaches an SD *memory* card, which by spec ignores CMD5, so the
// gate asserts the NO-IO-FUNCTION fallback.  That is a real test of the failure
// path, and nothing more.  Positive evidence lives in transcript_hw_evkb.txt.
#include <Arduino.h>
#include <SdioHost.h>

static SdioHost sdio;

static const char *statusName(SdioHost::Status s) {
    switch (s) {
        case SdioHost::OK:              return "ok";
        case SdioHost::NO_IO_FUNCTION:  return "no-io-function";
        case SdioHost::CMD_TIMEOUT:     return "cmd-timeout";
        case SdioHost::CMD_CRC:         return "cmd-crc";
        case SdioHost::CLOCK_UNSTABLE:  return "clock-unstable";
        case SdioHost::BAD_CIS:         return "bad-cis";
    }
    return "unknown";
}

void setup() {
    Serial1.begin(115200);
    while (!Serial1) {}
    Serial1.println("RT1176 M.2 SDIO probe up");

    SdioHost::Status st = sdio.begin();
    // Always print a reason code, never a bare absence.  "Nothing found" is
    // also what a dead image produces, so the gate needs a positive token
    // proving we arrived here deliberately.
    Serial1.print("sdio_begin=");
    Serial1.print(statusName(st));
    Serial1.print(" rc=");
    Serial1.println((int)st);

    if (st == SdioHost::OK) {
        Serial1.print("io_functions=");
        Serial1.println(sdio.ioFunctionCount());
        Serial1.print("rca=0x");
        Serial1.println(sdio.rca(), HEX);
        Serial1.print("cccr_rev=0x");
        Serial1.println(sdio.cccrRevision(), HEX);

        uint16_t manf = 0, card = 0;
        SdioHost::Status cs = sdio.readManfId(&manf, &card);
        if (cs == SdioHost::OK) {
            // These come off the wire from the card's CIS.  The firmware has no
            // knowledge of them -- that is what makes this assertion real.
            Serial1.print("manfid=0x");
            Serial1.println(manf, HEX);
            Serial1.print("cardid=0x");
            Serial1.println(card, HEX);
        } else {
            Serial1.print("cis_error=");
            Serial1.println(statusName(cs));
        }
    }
    Serial1.println("probe_done");
}

void loop() {
    static uint32_t n = 0;
    // Heartbeat: proves the image is still running after the probe rather than
    // having wedged in it.  A fallback gate without this cannot tell "took the
    // fallback" from "died".
    Serial1.print("alive=");
    Serial1.println(n++);
    delay(200);
}
```

- [ ] **Step 2: Write `CMakeLists.txt`**

```cmake
cmake_minimum_required(VERSION 3.24)
project(m2_sdio_probe)

if(NOT DEFINED TEENSY_VERSION)
    set(TEENSY_VERSION 117 CACHE STRING "")
endif()

include(${CMAKE_CURRENT_LIST_DIR}/../../../evkb.cmake)

import_evkb_library(M2Radio sdio)

teensy_add_executable(m2_sdio_probe m2_sdio_probe.cpp)
teensy_target_link_libraries(m2_sdio_probe cores M2Radio)

target_link_libraries(m2_sdio_probe.elf stdc++)
```

- [ ] **Step 3: Write the gate**

Create `examples/networking/m2_sdio_probe/run_qemu.sh`:

```sh
#!/bin/sh
set -e
DIR=$(cd "$(dirname "$0")" && pwd)
EVKB=$(cd "$DIR/../../.." && pwd)
QEMU="$EVKB/tools/qrun"
. "$EVKB/tools/gate-lib.sh"
gate_init
ELF="$DIR/$(gate_build_dir)/m2_sdio_probe.elf"
OUT=$(gate_capture_path "$DIR" serial.uart)
rm -f "$OUT"
"$QEMU" $(gate_qemu_machine) -kernel "$ELF" \
    -display none $(gate_console "$OUT") \
    -d guest_errors -D "$(gate_capture_path "$DIR" serial.dbg)" &
P=$!; gate_pid $P
for _ in $(seq 1 40); do
    [ -f "$OUT" ] && grep -q "alive=2" "$OUT" 2>/dev/null && break
    sleep 0.25
done
gate_reap $P
gate_require_capture "$OUT"
echo "==== captured UART ===="; cat "$OUT"
grep -q "RT1176 M.2 SDIO probe up" "$OUT" || { echo "FAIL: banner missing"; exit 1; }
# This gate asserts the DEVICE-ABSENT path.  QEMU attaches an SD *memory* card,
# which by spec ignores CMD5, so the correct outcome is a clean no-io-function
# verdict -- NOT success.  A green run here says nothing about the real IW416;
# see transcript_hw_evkb.txt for that.
grep -q "^sdio_begin=no-io-function" "$OUT" || {
    echo "FAIL: expected the no-io-function fallback"
    echo "      (if this now says ok=, QEMU gained an SDIO model -- update the gate deliberately)"
    exit 1; }
# A reason code alone is not enough: "nothing found" is also what a wedged or
# dead image looks like.  probe_done proves we left begin(), and alive= proves
# the image is still running afterwards.
grep -q "^probe_done" "$OUT" || { echo "FAIL: probe never completed"; exit 1; }
grep -q "^alive=2" "$OUT" || { echo "FAIL: no heartbeat after the probe"; exit 1; }
# The fallback must not claim identity it cannot have read.
if grep -q "^manfid=" "$OUT"; then
    echo "FAIL: reported a manufacturer ID with no IO function present"; exit 1
fi
echo "PASS: SDIO enumerate reached the no-io-function fallback cleanly"
```

- [ ] **Step 4: Make it executable and confirm the red**

```bash
chmod +x examples/networking/m2_sdio_probe/run_qemu.sh
cd examples/networking/m2_sdio_probe && cmake -B build -DCMAKE_TOOLCHAIN_FILE=../../../toolchain/rt1170-evkb.toolchain.cmake
```

Expected: **FAIL** at configure with an unknown library `M2Radio` (it is not
declared in `evkb.cmake` yet) — or, once Task 5 declares it, a compile error for
the missing `SdioHost.h`. Either is the right red. Anything else, fix that first.

- [ ] **Step 5: Commit the red**

```bash
git add examples/networking/m2_sdio_probe
git commit -m "test: failing M.2 SDIO probe gate

Asserts the no-io-function fallback plus a positive reason code, probe_done and
a post-probe heartbeat -- 'nothing found' is also what a dead image produces, so
absence alone must not read as a pass. Red: SdioHost does not exist."
```

---

## Task 3: SdioHost — USDHC1 bring-up

**Files:**
- Create: `~/Development/M2Radio/sdio/SdioHost.h`
- Create: `~/Development/M2Radio/sdio/SdioHost.cpp`

- [ ] **Step 1: Write the header**

Create `~/Development/M2Radio/sdio/SdioHost.h`:

```cpp
// Minimal SDIO host for the i.MX RT1176 uSDHC1 controller.
//
// Deliberately NOT built on SdFat: SdioTeensy is an SD-memory driver
// (CMD0/CMD8/ACMD41/CMD2) with no SDIO path at all.  What IS reused -- by
// copying, not by coupling -- is its RT1176 clock/pad/reset sequence, which is
// already proven on this silicon.
//
// WARNING: on the MIMXRT1170-EVKB, uSDHC1 carries BOTH the M.2 socket J54 and
// the microSD slot J15, wired in parallel.  Only one card may be present.
#pragma once
#include <Arduino.h>

class SdioHost {
public:
    enum Status : int8_t {
        OK             =  0,
        NO_IO_FUNCTION = -1,  // CMD5 got no response, or the card reports 0 functions
        CMD_TIMEOUT    = -2,
        CMD_CRC        = -3,
        CLOCK_UNSTABLE = -4,
        BAD_CIS        = -5,
    };

    // Reset the controller, mux the pads, start the 400 kHz identification
    // clock, then CMD5 -> CMD3 -> CMD7.  Returns NO_IO_FUNCTION when no SDIO
    // card answers -- which is the expected result with an SD memory card
    // present, and in QEMU.
    Status begin();

    uint8_t  ioFunctionCount() const { return m_ioFunctions; }
    uint16_t rca()             const { return m_rca; }
    uint8_t  cccrRevision()    const { return m_cccrRev; }

    // CMD52 IO_RW_DIRECT.  `fn` is the SDIO function number (0 = CCCR/CIS).
    Status cmd52Read(uint8_t fn, uint32_t addr, uint8_t *out);
    Status cmd52Write(uint8_t fn, uint32_t addr, uint8_t value);

    // Walk function 0's CIS for the CISTPL_MANFID tuple.  Both values come off
    // the wire; the driver has no built-in expectation of either.
    Status readManfId(uint16_t *manufacturer, uint16_t *card);

private:
    Status sendCommand(uint8_t index, uint32_t arg, uint32_t xferFlags, uint32_t *resp);
    Status setClock(uint32_t hz);

    uint8_t  m_ioFunctions = 0;
    uint16_t m_rca         = 0;
    uint8_t  m_cccrRev     = 0;
    uint32_t m_cisPtr      = 0;
};
```

- [ ] **Step 2: Write the bring-up half of the implementation**

Create `~/Development/M2Radio/sdio/SdioHost.cpp`. Start with the register
overlay, the pad/clock sequence, and controller reset. The pad values and the
clock root below are copied verbatim from SdFat's proven `__IMXRT1176__` branch
(`~/Development/SdFat/src/SdCard/SdioTeensy.cpp`) — do not re-derive them.

```cpp
#include "SdioHost.h"

// USDHC1 register overlay.  Offsets match the RT1176 USDHC register map; the
// core already carries USDHC1_* macros for the SdFat port, but this library
// keeps its own overlay so it does not depend on SdFat's headers.
#define USDHC1_BASE 0x40418000u
#define REG(off) (*(volatile uint32_t *)(USDHC1_BASE + (off)))
#define DS_ADDR      REG(0x00)
#define BLK_ATT      REG(0x04)
#define CMD_ARG      REG(0x08)
#define CMD_XFR_TYP  REG(0x0C)
#define CMD_RSP0     REG(0x10)
#define CMD_RSP1     REG(0x14)
#define PRES_STATE   REG(0x24)
#define PROT_CTRL    REG(0x28)
#define SYS_CTRL     REG(0x2C)
#define INT_STATUS   REG(0x30)
#define INT_STATUS_EN REG(0x34)
#define INT_SIGNAL_EN REG(0x38)
#define MIX_CTRL     REG(0x48)
#define VEND_SPEC    REG(0xC0)

// INT_STATUS bits
static const uint32_t INT_CC   = 1u << 0;   // command complete
static const uint32_t INT_CTOE = 1u << 16;  // command timeout
static const uint32_t INT_CCE  = 1u << 17;  // command CRC error
static const uint32_t INT_CEBE = 1u << 18;  // command end-bit error
static const uint32_t INT_CIE  = 1u << 19;  // command index error
static const uint32_t INT_CMD_ERR = INT_CTOE | INT_CCE | INT_CEBE | INT_CIE;

// CMD_XFR_TYP response types
static const uint32_t RSP_NONE   = 0u << 16;
static const uint32_t RSP_136    = 1u << 16;
static const uint32_t RSP_48     = 2u << 16;
static const uint32_t RSP_48BUSY = 3u << 16;
static const uint32_t CHK_CRC    = 1u << 19;
static const uint32_t CHK_IDX    = 1u << 20;

static void gpioMux(uint8_t mode) {
    IOMUXC_SW_MUX_CTL_PAD_GPIO_SD_B1_00 = mode;  // USDHC1_CMD
    IOMUXC_SW_MUX_CTL_PAD_GPIO_SD_B1_01 = mode;  // USDHC1_CLK
    IOMUXC_SW_MUX_CTL_PAD_GPIO_SD_B1_02 = mode;  // USDHC1_DATA0
    IOMUXC_SW_MUX_CTL_PAD_GPIO_SD_B1_03 = mode;  // USDHC1_DATA1
    IOMUXC_SW_MUX_CTL_PAD_GPIO_SD_B1_04 = mode;  // USDHC1_DATA2
    IOMUXC_SW_MUX_CTL_PAD_GPIO_SD_B1_05 = mode;  // USDHC1_DATA3
}

static void enablePads() {
    // RT1176 SW_PAD_CTL fields: bit4 ODE, bits[3:2] PULL (01=up,10=down,
    // 11=none), bit1 PDRV (0=high drive).  CMD/DATA = pull-up + high drive;
    // CLK = pull-down + high drive.  Copied from SdFat's proven RT1176 branch --
    // the 1062 PKE/DSE/SPEED encoding does not exist on this part.
    const uint32_t DATA_PAD = 0x04;
    const uint32_t CLK_PAD  = 0x08;
    gpioMux(0);  // ALT0 = USDHC1
    IOMUXC_SW_PAD_CTL_PAD_GPIO_SD_B1_00 = DATA_PAD;
    IOMUXC_SW_PAD_CTL_PAD_GPIO_SD_B1_01 = CLK_PAD;
    IOMUXC_SW_PAD_CTL_PAD_GPIO_SD_B1_02 = DATA_PAD;
    IOMUXC_SW_PAD_CTL_PAD_GPIO_SD_B1_03 = DATA_PAD;
    IOMUXC_SW_PAD_CTL_PAD_GPIO_SD_B1_04 = DATA_PAD;
    IOMUXC_SW_PAD_CTL_PAD_GPIO_SD_B1_05 = DATA_PAD;
}

// Root frequency for the divider maths below.  MUST track initClock().
static const uint32_t BASE_CLOCK = 198000000U;

static void initClock() {
    // USDHC1 clock root (CLOCK_ROOT58) <- SYS_PLL2_PFD2 (mux 4) / 2 (DIV 1),
    // then ungate USDHC1 (LPCG117).
    //
    // HARDWARE-VERIFY: this assumes the boot ROM leaves SYS_PLL2_PFD2 at
    // ~396 MHz.  The imxrt1176 core's startup.c brings up only the ARM PLL and
    // AHB, never PLL2.  If enumeration fails on silicon with sane-looking code,
    // suspect this before the protocol -- measure the root, and if PFD2 differs
    // fall back to OSC_24M: MUX(1)|DIV(0) with BASE_CLOCK = 24000000U.
    CCM_CLOCK_ROOT58_CONTROL = CCM_CLOCK_ROOT_CONTROL_MUX(4) | CCM_CLOCK_ROOT_CONTROL_DIV(1);
    CCM_LPCG117_DIRECT = 1;
}
```

- [ ] **Step 3: Implement reset and `setClock`**

Append to `SdioHost.cpp`:

```cpp
SdioHost::Status SdioHost::setClock(uint32_t hz) {
    // Gate the card clock while changing the divider, then wait for SDSTB.
    uint32_t base = BASE_CLOCK;
    uint32_t bestPre = 0, bestDiv = 0;
    uint32_t bestErr = 0xFFFFFFFFu;
    for (uint32_t pre = 1; pre <= 256; pre <<= 1) {
        for (uint32_t div = 1; div <= 16; div++) {
            uint32_t f = base / (pre * div);
            if (f > hz) continue;
            uint32_t err = hz - f;
            if (err < bestErr) { bestErr = err; bestPre = pre; bestDiv = div; }
        }
    }
    if (bestPre == 0) return CLOCK_UNSTABLE;

    // SYS_CTRL: SDCLKFS = prescaler>>1, DVS = divisor-1.
    uint32_t sdclkfs = bestPre >> 1;
    uint32_t dvs     = bestDiv - 1;
    SYS_CTRL = (SYS_CTRL & ~0x0000FFF0u) | (sdclkfs << 8) | (dvs << 4) | (0xEu << 16);

    for (uint32_t i = 0; i < 100000; i++) {
        if (PRES_STATE & (1u << 3)) return OK;   // SDSTB
    }
    return CLOCK_UNSTABLE;
}
```

- [ ] **Step 4: Build it in isolation to catch syntax errors early**

There is no test harness for the library alone; the gate is the test. Confirm it
compiles by continuing to Task 4 and building the example. If you want an early
check, `cd examples/networking/m2_sdio_probe && cmake --build build` after Task 5
declares the library.

- [ ] **Step 5: Commit**

```bash
cd ~/Development/M2Radio && git add sdio/ && \
  git commit -m "sdio: USDHC1 pad mux, clock root and divider

Clock/pad values copied verbatim from SdFat's proven RT1176 branch, including
its HARDWARE-VERIFY note about the boot-ROM PLL2 PFD2 dependency."
```

---

## Task 4: SdioHost — command engine and enumeration

**Files:**
- Modify: `~/Development/M2Radio/sdio/SdioHost.cpp`

- [ ] **Step 1: Implement `sendCommand`**

```cpp
SdioHost::Status SdioHost::sendCommand(uint8_t index, uint32_t arg,
                                       uint32_t xferFlags, uint32_t *resp) {
    // Wait for CIHB (command inhibit) to clear.
    for (uint32_t i = 0; PRES_STATE & (1u << 0); i++) {
        if (i > 1000000) return CMD_TIMEOUT;
    }
    INT_STATUS = 0xFFFFFFFFu;          // clear stale status (w1c)
    CMD_ARG    = arg;
    MIX_CTRL  &= ~0x3Fu;               // no data phase for any command here
    CMD_XFR_TYP = ((uint32_t)index << 24) | xferFlags;

    uint32_t st = 0;
    for (uint32_t i = 0; ; i++) {
        st = INT_STATUS;
        if (st & (INT_CC | INT_CMD_ERR)) break;
        if (i > 1000000) return CMD_TIMEOUT;
    }
    INT_STATUS = st;                   // w1c

    if (st & INT_CTOE) return CMD_TIMEOUT;
    if (st & (INT_CCE | INT_CEBE | INT_CIE)) return CMD_CRC;
    if (resp) *resp = CMD_RSP0;
    return OK;
}
```

- [ ] **Step 2: Implement `begin()`**

```cpp
SdioHost::Status SdioHost::begin() {
    m_ioFunctions = 0; m_rca = 0; m_cccrRev = 0; m_cisPtr = 0;

    initClock();
    enablePads();

    // Reset the whole controller (SYS_CTRL RSTA) and wait for it to clear.
    SYS_CTRL |= (1u << 24);
    for (uint32_t i = 0; SYS_CTRL & (1u << 24); i++) {
        if (i > 1000000) return CLOCK_UNSTABLE;
    }
    PROT_CTRL = (PROT_CTRL & ~0x6u);   // 1-bit bus width for identification
    INT_STATUS_EN = 0xFFFFFFFFu;
    INT_SIGNAL_EN = 0;                 // polled, no interrupts in W1

    Status s = setClock(400000);       // identification clock
    if (s != OK) return s;
    delayMicroseconds(2000);           // >= 74 clocks at 400 kHz before CMD5

    // CMD5 IO_SEND_OP_COND, arg 0 -> read the card's OCR.  R4 has no CRC and
    // no index, so neither is checked.  An SD MEMORY card ignores CMD5 by
    // spec, so a timeout here is the expected no-card-of-our-kind answer, not
    // an error to report as a failure.
    uint32_t r4 = 0;
    s = sendCommand(5, 0, RSP_48, &r4);
    if (s != OK) return NO_IO_FUNCTION;

    uint8_t nfn = (r4 >> 28) & 0x7;
    if (nfn == 0) return NO_IO_FUNCTION;

    // Re-issue CMD5 with the voltage window until the card reports ready.
    const uint32_t OCR_32_34 = 0x00300000u;
    for (uint32_t i = 0; i < 1000; i++) {
        s = sendCommand(5, r4 & 0x00FFFFFFu ? (r4 & 0x00FFFFFFu) : OCR_32_34,
                        RSP_48, &r4);
        if (s != OK) return s;
        if (r4 & 0x80000000u) break;   // C bit: initialisation complete
        delayMicroseconds(1000);
        if (i == 999) return CMD_TIMEOUT;
    }
    m_ioFunctions = (r4 >> 28) & 0x7;

    // CMD3 SEND_RELATIVE_ADDR -> R6, RCA in bits 31:16.
    uint32_t r6 = 0;
    s = sendCommand(3, 0, RSP_48 | CHK_CRC | CHK_IDX, &r6);
    if (s != OK) return s;
    m_rca = (uint16_t)(r6 >> 16);

    // CMD7 SELECT_CARD with the RCA -> R1b.
    s = sendCommand(7, (uint32_t)m_rca << 16, RSP_48BUSY | CHK_CRC | CHK_IDX, nullptr);
    if (s != OK) return s;

    s = setClock(25000000);            // leave identification speed
    if (s != OK) return s;

    // CCCR revision (function 0, address 0x00) and the function-0 CIS pointer
    // (0x09..0x0B, little-endian).
    uint8_t b = 0;
    s = cmd52Read(0, 0x00, &b);
    if (s != OK) return s;
    m_cccrRev = b & 0x0F;

    m_cisPtr = 0;
    for (int i = 0; i < 3; i++) {
        s = cmd52Read(0, 0x09 + i, &b);
        if (s != OK) return s;
        m_cisPtr |= (uint32_t)b << (8 * i);
    }
    return OK;
}
```

- [ ] **Step 3: Implement CMD52 and the CIS walk**

```cpp
// CMD52 argument layout: bit31 R/W, bits30:28 function, bit27 RAW,
// bits25:9 register address, bits7:0 write data.
static inline uint32_t cmd52Arg(bool write, uint8_t fn, uint32_t addr, uint8_t data) {
    return ((uint32_t)write << 31) | ((uint32_t)(fn & 0x7) << 28) |
           ((addr & 0x1FFFFu) << 9) | data;
}

SdioHost::Status SdioHost::cmd52Read(uint8_t fn, uint32_t addr, uint8_t *out) {
    uint32_t r5 = 0;
    Status s = sendCommand(52, cmd52Arg(false, fn, addr, 0),
                           RSP_48 | CHK_CRC | CHK_IDX, &r5);
    if (s != OK) return s;
    if (out) *out = (uint8_t)(r5 & 0xFF);
    return OK;
}

SdioHost::Status SdioHost::cmd52Write(uint8_t fn, uint32_t addr, uint8_t value) {
    return sendCommand(52, cmd52Arg(true, fn, addr, value),
                       RSP_48 | CHK_CRC | CHK_IDX, nullptr);
}

SdioHost::Status SdioHost::readManfId(uint16_t *manufacturer, uint16_t *card) {
    if (m_cisPtr == 0) return BAD_CIS;
    // Walk the tuple chain looking for CISTPL_MANFID (0x20).  Tuple format is
    // <code><link><body...>; 0xFF terminates the chain.  Bound the walk -- a
    // corrupt link byte must not spin forever.
    uint32_t addr = m_cisPtr;
    for (int guard = 0; guard < 128; guard++) {
        uint8_t code = 0, link = 0;
        Status s = cmd52Read(0, addr, &code);
        if (s != OK) return s;
        if (code == 0xFF) return BAD_CIS;            // end of chain, no MANFID
        s = cmd52Read(0, addr + 1, &link);
        if (s != OK) return s;
        if (code == 0x20) {                          // CISTPL_MANFID
            if (link < 4) return BAD_CIS;
            uint8_t b[4];
            for (int i = 0; i < 4; i++) {
                s = cmd52Read(0, addr + 2 + i, &b[i]);
                if (s != OK) return s;
            }
            if (manufacturer) *manufacturer = (uint16_t)(b[0] | (b[1] << 8));
            if (card)         *card         = (uint16_t)(b[2] | (b[3] << 8));
            return OK;
        }
        addr += 2 + link;
    }
    return BAD_CIS;
}
```

- [ ] **Step 4: Commit**

```bash
cd ~/Development/M2Radio && git add sdio/ && \
  git commit -m "sdio: CMD5/CMD3/CMD7 enumeration, CMD52, CIS MANFID walk

CMD5 timing out is reported as NO_IO_FUNCTION rather than an error: an SD memory
card ignores CMD5 by spec, so that is the expected answer when one is present."
```

---

## Task 5: Wire the library into the build, then go green

**Files:**
- Modify: `evkb.cmake` (the `teensy_declare_library` block, lines 110-128)
- Modify: `tools/license-audit.sh` (the `GATES` list)

- [ ] **Step 1: Resolve the publish decision**

**Ask the user** whether `M2Radio` should be created and pushed to
`github.com/newdigate/M2Radio`. Do not create a public repo without approval.
Their answer determines Step 2.

- [ ] **Step 2a: If approved — push, then declare with the real SHA**

```bash
cd ~/Development/M2Radio && gh repo create newdigate/M2Radio --public --source=. --remote=origin --push
git rev-parse HEAD
```

Then add to `evkb.cmake`, after the `SdFat` line:

```cmake
teensy_declare_library(M2Radio        M2Radio              https://github.com/newdigate/M2Radio         <SHA> .) # subdir chosen by the importer: import_evkb_library(M2Radio sdio)
```

- [ ] **Step 2b: If NOT approved — declare anyway, and record the SKIP**

Use the same line with a placeholder SHA of `0000000000000000000000000000000000000000`, then add an entry to `docs/KNOWN-BROKEN-GATES.md` recording that
`rt1176:networking/m2_sdio_probe` is SKIP-class on a fresh clone because
`M2Radio` is local-only, why that matters (a SKIP hides in a count while every
other exception shows up red and by name), and what resolves it (pushing the
repo and bumping the pin). Follow the format of the resolved SynthUI/VGLite
entry already in that file.

- [ ] **Step 3: Build and run the gate — this is the green**

```bash
cd examples/networking/m2_sdio_probe && rm -rf build && \
  cmake -B build -DCMAKE_TOOLCHAIN_FILE=../../../toolchain/rt1170-evkb.toolchain.cmake && \
  cmake --build build && ./run_qemu.sh
```

Expected: `PASS: SDIO enumerate reached the no-io-function fallback cleanly`.

If it fails on `sdio_begin=` with `cmd-timeout` rather than `no-io-function`:
`begin()` is reporting a raw command failure where it should be concluding
"no SDIO card". That distinction is the whole point of the gate — fix `begin()`,
not the gate.

- [ ] **Step 4: Add the GATES entry and run the licence audit**

In `tools/license-audit.sh`, add `examples/networking/m2_sdio_probe:m2_sdio_probe`
to the `GATES` string alongside the other `examples/networking/` entries.

```bash
cd ~/Development/rt1176-evkb-m2-maya-w161 && ./tools/license-audit.sh
```

Expected: `LICENSE-AUDIT: PASS`. M2Radio is MIT with nothing vendored, so the
header sweep and binary-provenance checks have nothing to object to.

- [ ] **Step 5: Confirm discovery and re-measure the sweep**

Read `docs/KNOWN-BROKEN-GATES.md` first. Build every gate-owning example before
sweeping — a missing ELF reports as SKIP, not failure.

```bash
./tools/run-all-qemu-gates.sh -l | grep m2_sdio
./tools/run-all-qemu-gates.sh
```

Expected: `rt1176:networking/m2_sdio_probe` listed, and **95 passed, 0 failed,
0 SKIP** (94 + this gate) — or 94/1/0 if the nondeterministic
`rt1176:dualcore/cm4_audio_test` is red; re-run that one idle before believing
it. If Step 2b applied, expect **1 SKIP** and say so explicitly rather than
letting it pass unremarked.

- [ ] **Step 6: Update the measured baseline in `CLAUDE.md`**

Update the sweep count to what you actually measured, add `m2_sdio_probe` to the
running list of what moved it, and refresh the `✅ Measured` line with today's
date and real figures. Record what you ran, not what you expected.

- [ ] **Step 7: Commit**

```bash
git add evkb.cmake tools/license-audit.sh CLAUDE.md docs/KNOWN-BROKEN-GATES.md
git commit -m "build: declare M2Radio; gate networking/m2_sdio_probe"
```

---

## Task 6: Hardware verification — **STOP, needs the board**

Another session shares this EVKB. Confirm it is free before starting.

**Files:**
- Create: `examples/networking/m2_sdio_probe/transcript_hw_evkb.txt`
- Modify: `docs/m2-evkb-revc3.md` (record what the module actually reported)

- [ ] **Step 1: Confirm the board is free and the microSD slot is EMPTY**

J15 must have no card in it. This is a bus constraint — with a card present,
both it and the M.2 module answer on the same wires. Physically check the slot.

- [ ] **Step 2: Clear stale probe daemons**

This yanks the board from any active debug session, so confirm first.

```bash
pkill LinkServer; pkill redlinkserv; pkill crt_emu_cm_redlink
```

- [ ] **Step 3: Flash without holding the VCOM**

```bash
LinkServer flash MIMXRT1176:MIMXRT1170-EVKB load build/m2_sdio_probe.elf
```

Verify, **then** attach the reader, then reset — in that order.

- [ ] **Step 4: Capture**

```bash
ls /dev/cu.usbmodem*
```

```bash
python3 ../../../tools/rt1170-console.py /dev/cu.usbmodem0000000000001 115200
```

Expected on real hardware: `sdio_begin=ok`, a non-zero `io_functions`, a
plausible `rca`, and — the assertion that matters — `manfid=0x2DF` (Marvell/NXP)
with a `cardid` this firmware has never seen. **Record whatever it actually
says.** If `manfid` is not `0x2DF`, that is a finding, not a bug to paper over:
write down the real value.

If it reports `sdio_begin=cmd-timeout`, read the `HARDWARE-VERIFY` note in
`initClock()` before touching protocol code — the clock root depends on
boot-ROM PLL2 state the core never programs.

- [ ] **Step 5: Save the transcript and record the result**

Save the session to `examples/networking/m2_sdio_probe/transcript_hw_evkb.txt`,
following the format of neighbouring examples. Add the observed manufacturer and
card ID to `docs/m2-evkb-revc3.md`.

- [ ] **Step 6: Commit**

```bash
cd ~/Development/rt1176-evkb-m2-maya-w161
git add examples/networking/m2_sdio_probe/transcript_hw_evkb.txt docs/m2-evkb-revc3.md
git commit -m "test: M.2 SDIO probe hardware verification

The IW416's manufacturer and card ID read off the wire -- values the firmware
has no knowledge of."
```

---

## Notes for whoever executes this

- **Two repos.** Tasks 1, 3 and 4 commit in `~/Development/M2Radio`; tasks 2, 5
  and 6 in `~/Development/rt1176-evkb-m2-maya-w161`.
- **The gate is the test.** Run gates as `./run_qemu.sh`, never
  `sh run_qemu.sh` — they re-exec under `gtimeout`.
- **Do not "fix" the gate to make it green.** It asserts the module-absent
  fallback on purpose. If it starts passing for a different reason, that is a
  change to make deliberately, with the comment updated to match.
- **A green sweep does not prove a build directory can still configure.** Gates
  run cached ELFs; `rm -rf` a build dir if it errors on `COMPILERPATH is
  UNDEFINED` or a missing toolchain.
