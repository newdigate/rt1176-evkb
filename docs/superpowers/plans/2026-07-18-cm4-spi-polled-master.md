# CM4 Phase 3.1 — SPI (LPSPI1) polled master, self-configured — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** A Cortex-M4 image self-configures LPSPI1 (clock + pins + block) and runs a polled master self-loopback (SDO→SDI jumper), streaming its observations over the MU to the CM7, which prints them on VCOM — proven in a QEMU gate and (final arbiter) on the EVKB.

**Architecture:** New gate `evkb/cm4_spi_test/` cloned from `cm4_dual_test`. The CM4 sketch (`cm4/main_cm4.c`) is a **distilled C driver** re-expressing `newdigate/SPI`'s HW-verified `SPIIMXRT1176.cpp` `begin()`+`transfer()` with direct register writes (no CM7 core, no C++ runtime); it doubles as the HW probe. The CM7 sketch is a pure MU reporter — it never touches LPSPI1 (CM4-exclusive). No qemu2/core/library changes.

**Tech Stack:** bare-metal Cortex-M4 C + `teensy_add_cm4_image` (Phase-2B macro); imxrt1176 Arduino core (CM7); qemu2 `mimxrt1170-evk` gate via `qrun`/`gate-lib.sh`; LinkServer + `clean_boot.scp` on the EVKB.

**Spec:** `docs/superpowers/specs/2026-07-18-cm4-spi-polled-master-design.md`.

---

## Why QEMU-green is necessary but not sufficient (read before Task 2)

The board fixture `hw/arm/mimxrt1170-evk.c:74-81` attaches an **`ssi-loopback`** child to LPSPI1; `imxrt_lpspi_transfer` echoes `rx=tx` **once `CR.MEN` is set — ignoring the LPCG clock gate, clock root, and pin mux**. So a CM4 image that omitted the `CCM_LPCG104` / `CCM_CLOCK_ROOT43` / IOMUXC writes would **still print `a=000000A5` and pass in QEMU**, yet produce nothing on real pins. QEMU green ⇒ the register/transfer *sequence* is correct. **Only the HW jumper run (Task 5) proves the CM4 self-configured the functional clock + pins.** Keep every clock/pin write in even though QEMU can't test it.

---

## File structure

| File | Responsibility |
|---|---|
| `evkb/cm4_spi_test/cm4/main_cm4.c` | **CM4 driver** — self-config LPSPI1 + polled loopback + MU stream (the probe). NEW. |
| `evkb/cm4_spi_test/cm4/startup_cm4.S` | CM4 reset/vectors. **Verbatim copy** of `cm4_dual_test/cm4/startup_cm4.S`. |
| `evkb/cm4_spi_test/cm4/cm4.ld` | CM4 TCM linker script. **Verbatim copy** of `cm4_dual_test/cm4/cm4.ld`. |
| `evkb/cm4_spi_test/cm4_spi_test.cpp` | **CM7 reporter** — boot CM4, read 9 MU values, print tokens + verdict. NEW. |
| `evkb/cm4_spi_test/CMakeLists.txt` | Build glue (clone of `cm4_dual_test`, renamed). NEW. |
| `evkb/cm4_spi_test/toolchain/rt1170-evkb.toolchain.cmake` | **Verbatim copy** of `cm4_dual_test/toolchain/…`. |
| `evkb/cm4_spi_test/run_qemu.sh` | Gate runner (clone, renamed tokens). NEW. |
| `evkb/cm4_spi_test/README.md` | Gate doc + token table + QEMU-vs-HW meaning + jumper. NEW. |
| `evkb/cm4_spi_test/transcript_qemu.txt` | Saved QEMU transcript (Task 2). NEW. |
| `evkb/cm4_spi_test/transcript_hw_evkb.txt` | Saved EVKB transcript (Task 5, HW). NEW. |
| `evkb/tools/license-audit.sh` | Add `cm4_spi_test` to `GATES`. MODIFY (line 59). |
| `.claude/skills/cm4-bringup/references/cm4-roadmap.md` | Mark 3.1 HW-verified (Task 5). MODIFY. |

**MU value contract (CM4 → CM7, ordered, all on channel 0):**
`cr, cfgr1, lpcg, croot, a, b, w, buf, rxok` (9 words). `cr/cfgr1/a/b/w/buf/rxok` are asserted; `lpcg/croot` are printed for diagnosis only.

---

## Task 1: Scaffold the gate + failing harness (RED)

**Files:**
- Create dir: `evkb/cm4_spi_test/` (+ `cm4/`, `toolchain/`)
- Copy: `cm4/startup_cm4.S`, `cm4/cm4.ld`, `toolchain/rt1170-evkb.toolchain.cmake` (verbatim)
- Create: `CMakeLists.txt`, `cm4/main_cm4.c` (stub), `cm4_spi_test.cpp`, `run_qemu.sh`

- [ ] **Step 1: Create the directory and copy the three shared files verbatim**

```bash
cd ~/Development/rt1170/evkb
mkdir -p cm4_spi_test/cm4 cm4_spi_test/toolchain
cp cm4_dual_test/cm4/startup_cm4.S           cm4_spi_test/cm4/startup_cm4.S
cp cm4_dual_test/cm4/cm4.ld                  cm4_spi_test/cm4/cm4.ld
cp cm4_dual_test/toolchain/rt1170-evkb.toolchain.cmake cm4_spi_test/toolchain/rt1170-evkb.toolchain.cmake
```

The startup references `SysTick_Handler` and `MU_IRQHandler` as C symbols — both are defined (empty) in `main_cm4.c` (stub below and real driver in Task 2), so the verbatim copy links unchanged.

- [ ] **Step 2: Write `evkb/cm4_spi_test/CMakeLists.txt`**

```cmake
cmake_minimum_required(VERSION 3.24)
project(cm4_spi_test)

set(TEENSY_VERSION 117 CACHE STRING "")

include(FetchContent)
FetchContent_Declare(teensy_cmake_macros SOURCE_DIR ${CMAKE_CURRENT_LIST_DIR}/../teensy-cmake-macros)
FetchContent_MakeAvailable(teensy_cmake_macros)
include(${teensy_cmake_macros_SOURCE_DIR}/CMakeLists.include.txt)

import_arduino_library(cores ${CMAKE_CURRENT_LIST_DIR}/../cores/imxrt1176)

# CM4 image via the Phase-2B first-class dual-target macro (emits cm4_spi.h).
teensy_add_cm4_image(cm4_spi
    LINKER  ${CMAKE_CURRENT_LIST_DIR}/cm4/cm4.ld
    SOURCES ${CMAKE_CURRENT_LIST_DIR}/cm4/startup_cm4.S
            ${CMAKE_CURRENT_LIST_DIR}/cm4/main_cm4.c)

teensy_add_executable(cm4_spi_test cm4_spi_test.cpp)
teensy_target_link_libraries(cm4_spi_test cores)
target_link_libraries(cm4_spi_test.elf stdc++)
teensy_target_link_cm4_image(cm4_spi_test cm4_spi)
```

- [ ] **Step 3: Write the STUB `evkb/cm4_spi_test/cm4/main_cm4.c`** (no SPI yet → gate must fail)

```c
/* cm4_spi_test CM4 firmware — STUB (Task 1). No SPI yet: the gate must FAIL
 * (RED) until Task 2 fills in the self-config + loopback + MU stream. */
#include <stdint.h>

/* The shared vector table (startup_cm4.S) references these C symbols. */
void SysTick_Handler(void) {}
void MU_IRQHandler(void) {}

int main(void)
{
    for (;;) {
    }
}
```

- [ ] **Step 4: Write the CM7 reporter `evkb/cm4_spi_test/cm4_spi_test.cpp`** (full — unchanged in Task 2)

```cpp
/*
 * cm4_spi_test — Phase-3.1: the CM4 SELF-CONFIGURES LPSPI1 and runs a polled
 * master self-loopback (external SDO->SDI jumper).  This CM7 sketch only boots
 * the CM4 image and reports the CM4's observations over the MU on LPUART1 — it
 * never touches LPSPI1 (LPSPI1 is CM4-exclusive in 3.1).
 *
 * Tokens over Serial1 (LPUART1), streamed by the CM4 over MU channel 0:
 *   cr    = 00000001   LPSPI CR.MEN — the CM4 enabled the block
 *   cfgr1 = 00000001   CFGR1.MASTER — master mode
 *   lpcg  = ........   CCM_LPCG104 readback (informative — not asserted)
 *   croot = ........   CCM_CLOCK_ROOT43 readback (informative — not asserted)
 *   a     = 000000A5   loopback echoed 0xA5   (8-bit)
 *   b     = 0000003C   loopback echoed 0x3C   (8-bit)
 *   w     = 0000BEEF   transfer16 echoed 0xBEEF (16-bit)
 *   buf   = DEADBEEF   4-byte buffer echoed {DE,AD,BE,EF}
 *   rxok  = 00000001   all loopback bytes matched
 * Verdict: SPI_CM4=PASS.  (In QEMU the ssi-loopback child echoes on CR.MEN
 * alone; on hardware the same tokens additionally prove the CM4's clock-gate +
 * pin-mux via the physical jumper — see README.)
 */
#include "Arduino.h"
#include "core_pins.h"
#include "HardwareSerial.h"
#include "Multicore.h"
#include "MessagingUnit.h"
#include "cm4_spi.h"     /* generated by teensy_add_cm4_image */

#define WAIT_LONG 3000000u

static void phex(const char *k, uint32_t v)
{
    Serial1.print(k);
    Serial1.print('=');
    for (int i = 28; i >= 0; i -= 4) {
        Serial1.print("0123456789ABCDEF"[(v >> i) & 0xF]);
    }
    Serial1.println();
}

static void ptimeout(const char *k)
{
    Serial1.print(k);
    Serial1.println("=TIMEOUT");
}

static bool wait_recv(uint8_t ch, uint32_t *out)
{
    for (uint32_t n = WAIT_LONG; n; n--) {
        if (MU.tryReceive(ch, out)) {
            return true;
        }
    }
    return false;
}

void setup()
{
    Serial1.begin(115200);
    Serial1.println("CM4SPI-GATE v1");

    MU.begin();
    Multicore.begin(cm4_spi, sizeof(cm4_spi));

    static const char *labels[9] =
        { "cr", "cfgr1", "lpcg", "croot", "a", "b", "w", "buf", "rxok" };
    uint32_t v[9];
    bool ok = true;
    for (int i = 0; i < 9; i++) {
        if (wait_recv(0, &v[i])) {
            phex(labels[i], v[i]);
        } else {
            ptimeout(labels[i]);
            v[i] = 0xFFFFFFFFu;
            ok = false;
        }
    }

    bool pass = ok
        && v[0] == 0x1u && v[1] == 0x1u          /* cr.MEN, cfgr1.MASTER */
        && v[4] == 0xA5u && v[5] == 0x3Cu        /* a, b */
        && v[6] == 0xBEEFu && v[7] == 0xDEADBEEFu /* w, buf */
        && v[8] == 0x1u;                          /* rxok */
    Serial1.println(pass ? "SPI_CM4=PASS" : "SPI_CM4=FAIL");
    Serial1.println("CM4SPI-DONE");
}

void loop()
{
}
```

- [ ] **Step 5: Write the gate runner `evkb/cm4_spi_test/run_qemu.sh`**

```sh
#!/bin/sh
# QEMU gate for Phase-3.1: the CM4 self-configures LPSPI1 and runs a polled
# self-loopback; the CM7 reports the observations over the MU on LPUART1.
# NOTE: QEMU's ssi-loopback child echoes on CR.MEN alone (ignores clock/pins),
# so a green QEMU run proves the register/transfer SEQUENCE only — the HW jumper
# run is what proves the CM4's clock-gating + pin-mux (see README / spec).
set -e
QEMU=~/Development/rt1170/evkb/tools/qrun
DIR=$(cd "$(dirname "$0")" && pwd)
. ~/Development/rt1170/evkb/tools/gate-lib.sh
gate_init
ELF="$DIR/build/cm4_spi_test.elf"
OUT="$DIR/cm4_spi.uart"
rm -f "$OUT"
"$QEMU" -M mimxrt1170-evk -global fsl-imxrt1170.boot-xip=on -kernel "$ELF" \
    -display none -serial file:"$OUT" -d guest_errors -D "$DIR/cm4_spi.dbg" &
P=$!; gate_pid $P
for _ in $(seq 1 40); do
    [ -f "$OUT" ] && grep -q "CM4SPI-DONE" "$OUT" 2>/dev/null && break
    sleep 0.25
done
kill $P 2>/dev/null; wait $P 2>/dev/null || true

echo "==== captured UART ===="; [ -f "$OUT" ] && cat "$OUT" || echo "(no UART output)"

fail=0
check() {
    if grep -q "^$1" "$OUT"; then echo "PASS: $1"; else echo "FAIL: expected $1"; fail=1; fi
}
grep -q "CM4SPI-GATE v1" "$OUT" || { echo "FAIL: banner missing"; exit 1; }
check "cr=00000001"       # LPSPI CR.MEN set (block enabled)
check "cfgr1=00000001"    # CFGR1.MASTER
check "a=000000A5"        # loopback echoed 0xA5
check "b=0000003C"        # loopback echoed 0x3C
check "w=0000BEEF"        # transfer16 echoed 0xBEEF
check "buf=DEADBEEF"      # 4-byte buffer echoed
check "rxok=00000001"     # all loopback bytes matched
check "SPI_CM4=PASS"      # verdict
# lpcg= / croot= are printed for HW diagnosis but intentionally NOT asserted.
grep -q "CM4SPI-DONE" "$OUT" || { echo "FAIL: DONE missing"; fail=1; }

if [ $fail -eq 0 ]; then
    echo "PASS: CM4 self-configured polled SPI loopback verified in QEMU"
else
    echo "GATE FAILED"; exit 1
fi
```

- [ ] **Step 6: Build the scaffold**

```bash
cd ~/Development/rt1170/evkb/cm4_spi_test
rm -rf build
cmake -B build -DCMAKE_TOOLCHAIN_FILE=toolchain/rt1170-evkb.toolchain.cmake .
cmake --build build
```
Expected: builds `build/cm4_spi_test.elf` (and generates `build/cm4_spi.h`). No errors.

- [ ] **Step 7: Run the gate — verify it FAILS (RED)**

```bash
chmod +x run_qemu.sh
./run_qemu.sh; echo "exit=$?"
```
Expected: the CM4 stub sends nothing, so the CM7 prints `cr=TIMEOUT … rxok=TIMEOUT`, then `SPI_CM4=FAIL`, `CM4SPI-DONE`. The runner prints `FAIL: expected cr=00000001` … and `GATE FAILED`, `exit=1`. **This RED confirms the harness actually tests the CM4 driver.**

- [ ] **Step 8: Commit the scaffold**

```bash
cd ~/Development/rt1170/evkb
git add cm4_spi_test/CMakeLists.txt cm4_spi_test/cm4_spi_test.cpp cm4_spi_test/run_qemu.sh \
        cm4_spi_test/cm4/startup_cm4.S cm4_spi_test/cm4/cm4.ld cm4_spi_test/cm4/main_cm4.c \
        cm4_spi_test/toolchain/rt1170-evkb.toolchain.cmake
git commit -m "cm4_spi_test: gate harness + CM7 reporter (Phase 3.1, RED)

CM4 stub sends nothing -> gate FAILs, proving the harness tests the CM4 driver.

Co-Authored-By: Claude Opus 4.8 <noreply@anthropic.com>"
```

---

## Task 2: Implement the CM4 self-config + loopback driver (GREEN)

**Files:**
- Modify (replace stub): `evkb/cm4_spi_test/cm4/main_cm4.c`
- Create: `evkb/cm4_spi_test/transcript_qemu.txt`

- [ ] **Step 1: Replace `evkb/cm4_spi_test/cm4/main_cm4.c` with the full driver**

```c
/* cm4_spi_test CM4 firmware (Phase 3.1): the CM4 SELF-CONFIGURES LPSPI1 and runs
 * a polled master self-loopback (external SDO->SDI jumper), streaming each
 * observation to the CM7 over the MU (channel 0, in order).
 *
 * Adapted from this project's own newdigate/SPI SPIIMXRT1176.cpp begin()/
 * transfer() (MIT, N. Newdigate) — the HW-verified LPSPI1 self-config +
 * polled-master sequence, re-expressed in C for the bare-metal CM4 image. No
 * logic change; every register/clock/pin value is identical. Keep in sync with
 * SPIIMXRT1176.cpp; Phase 3.3 consolidates both onto a shared C core.
 *
 * SILICON TRUTH: the qemu2 board attaches an ssi-loopback child to LPSPI1 that
 * echoes on CR.MEN ALONE — it ignores the clock gate, clock root, and pin mux.
 * So rx==tx in QEMU proves only the register/transfer sequence; the real
 * SDO->SDI jumper on hardware is what proves the CM4 ungated the clock + muxed
 * the pins + drove a real SCK.  Public-domain scaffolding (N. Newdigate);
 * adapted register logic MIT as noted above. */
#include <stdint.h>

/* ---- CCM (shared): LPSPI1 clock gate + root (imxrt1176.h) ---- */
#define CCM_LPCG104_DIRECT       (*(volatile uint32_t *)0x40CC6D00u) /* LPSPI1 gate */
#define CCM_CLOCK_ROOT43_CONTROL (*(volatile uint32_t *)0x40CC1580u) /* LPSPI1 root */

/* ---- IOMUXC (shared): SCK=GPIO_AD_28, SDO=GPIO_AD_30, SDI=GPIO_AD_31, ALT0 ---- */
#define IOMUX_MUX_AD_28  (*(volatile uint32_t *)0x400E817Cu)
#define IOMUX_MUX_AD_30  (*(volatile uint32_t *)0x400E8184u)
#define IOMUX_MUX_AD_31  (*(volatile uint32_t *)0x400E8188u)
#define IOMUX_PAD_AD_28  (*(volatile uint32_t *)0x400E83C0u)
#define IOMUX_PAD_AD_30  (*(volatile uint32_t *)0x400E83C8u)
#define IOMUX_PAD_AD_31  (*(volatile uint32_t *)0x400E83CCu)
#define IOMUX_SCK_SELECT (*(volatile uint32_t *)0x400E85D0u)  /* LPSPI1_SCK_SELECT_INPUT */
#define IOMUX_SDO_SELECT (*(volatile uint32_t *)0x400E85D8u)  /* LPSPI1_SDO_SELECT_INPUT */
#define IOMUX_SDI_SELECT (*(volatile uint32_t *)0x400E85D4u)  /* LPSPI1_SDI_SELECT_INPUT */
#define IOMUX_ALT0   0x0u
#define IOMUX_PAD    0x0Cu    /* SPIIMXRT1176.cpp pad_ctl_val (DSE set) */
#define IOMUX_DAISY  0x1u     /* select-input value */

/* ---- LPSPI1 (base 0x40114000; offsets == IMXRT_LPSPI_t / qemu2 imxrt_lpspi) ---- */
#define LPSPI1_BASE  0x40114000u
#define LPSPI_CR     (*(volatile uint32_t *)(LPSPI1_BASE + 0x10u))
#define LPSPI_CFGR1  (*(volatile uint32_t *)(LPSPI1_BASE + 0x24u))
#define LPSPI_CCR    (*(volatile uint32_t *)(LPSPI1_BASE + 0x40u))
#define LPSPI_TCR    (*(volatile uint32_t *)(LPSPI1_BASE + 0x60u))
#define LPSPI_TDR    (*(volatile uint32_t *)(LPSPI1_BASE + 0x64u))
#define LPSPI_RSR    (*(volatile uint32_t *)(LPSPI1_BASE + 0x70u))
#define LPSPI_RDR    (*(volatile uint32_t *)(LPSPI1_BASE + 0x74u))
#define CR_MEN       (1u << 0)
#define CR_RST       (1u << 1)
#define CFGR1_MASTER (1u << 0)
#define RSR_RXEMPTY  (1u << 1)
#define TCR_BASE     0u          /* MODE0, MSB-first, prescale 0 */
#define SCKDIV_4MHZ  4u          /* SCK = 24MHz / (2^0 * (4+2)) = 4 MHz */
#define SPI_TIMEOUT  100000u

/* ---- MU B side (the CM4's) ---- */
#define MUB_BASE   0x40C4C000u
#define MUB_TR(n)  (*(volatile uint32_t *)(MUB_BASE + 0x00u + ((n) << 2)))
#define MUB_SR     (*(volatile uint32_t *)(MUB_BASE + 0x20u))
#define SR_TE(n)   (1u << (23 - (n)))

/* The shared vector table (startup_cm4.S) references these C symbols. Polled
 * SPI needs neither, but the table entries must resolve. */
void SysTick_Handler(void) {}
void MU_IRQHandler(void) {}

static void mu_send(unsigned ch, uint32_t v)
{
    while (!(MUB_SR & SR_TE(ch))) {
    }
    MUB_TR(ch) = v;
}

/* Polled full-duplex transfer of one frame (framesz = bits-1), mirroring
 * SPIIMXRT1176.cpp::transfer(): load TCR, write TDR, spin on RSR.RXEMPTY, read
 * RDR.  Returns 0xFFFFFFFF on timeout (no functional clock -> nothing shifts). */
static uint32_t spi_transfer(uint32_t data, uint32_t framesz)
{
    LPSPI_TCR = TCR_BASE | (framesz & 0xFFFu);
    LPSPI_TDR = data;
    for (uint32_t g = 0; g < SPI_TIMEOUT; g++) {
        if (!(LPSPI_RSR & RSR_RXEMPTY)) {
            return LPSPI_RDR;
        }
    }
    return 0xFFFFFFFFu;
}

int main(void)
{
    /* --- self-config LPSPI1 (mirrors SPIIMXRT1176.cpp::begin) --- */
    CCM_LPCG104_DIRECT = 1u;               /* ungate the LPSPI1 clock */
    CCM_CLOCK_ROOT43_CONTROL = 0u;         /* mux0 OSC24M, div0 -> 24 MHz */

    IOMUX_MUX_AD_28 = IOMUX_ALT0;  IOMUX_PAD_AD_28 = IOMUX_PAD;  /* SCK */
    IOMUX_MUX_AD_30 = IOMUX_ALT0;  IOMUX_PAD_AD_30 = IOMUX_PAD;  /* SDO */
    IOMUX_MUX_AD_31 = IOMUX_ALT0;  IOMUX_PAD_AD_31 = IOMUX_PAD;  /* SDI */
    IOMUX_SCK_SELECT = IOMUX_DAISY;
    IOMUX_SDO_SELECT = IOMUX_DAISY;
    IOMUX_SDI_SELECT = IOMUX_DAISY;

    LPSPI_CR = CR_RST;  LPSPI_CR = 0u;     /* reset the block (MEN=0) */
    LPSPI_CFGR1 = CFGR1_MASTER;            /* master mode (write while MEN=0) */
    LPSPI_CCR = (LPSPI_CCR & ~0xFFu) | SCKDIV_4MHZ;  /* SCKDIV for 4 MHz */
    LPSPI_CR = CR_MEN;                     /* enable */

    /* --- config readbacks --- */
    uint32_t cr    = LPSPI_CR & CR_MEN;             /* -> 1 */
    uint32_t cfgr1 = LPSPI_CFGR1 & CFGR1_MASTER;    /* -> 1 */
    uint32_t lpcg  = CCM_LPCG104_DIRECT;            /* informative */
    uint32_t croot = CCM_CLOCK_ROOT43_CONTROL;      /* informative */

    /* --- polled loopback (SDO->SDI jumper) --- */
    uint32_t a = spi_transfer(0xA5u, 7u) & 0xFFu;        /* 8-bit */
    uint32_t b = spi_transfer(0x3Cu, 7u) & 0xFFu;        /* 8-bit */
    uint32_t w = spi_transfer(0xBEEFu, 15u) & 0xFFFFu;   /* 16-bit */
    uint8_t bs[4] = { 0xDEu, 0xADu, 0xBEu, 0xEFu };
    for (int i = 0; i < 4; i++) {
        bs[i] = (uint8_t)(spi_transfer(bs[i], 7u) & 0xFFu);
    }
    uint32_t buf = ((uint32_t)bs[0] << 24) | ((uint32_t)bs[1] << 16)
                 | ((uint32_t)bs[2] << 8) | (uint32_t)bs[3];

    uint32_t rxok = (a == 0xA5u && b == 0x3Cu && w == 0xBEEFu
                     && buf == 0xDEADBEEFu) ? 1u : 0u;

    /* --- stream the 9 observations to the CM7 (MU TR0, fixed order) --- */
    mu_send(0, cr);
    mu_send(0, cfgr1);
    mu_send(0, lpcg);
    mu_send(0, croot);
    mu_send(0, a);
    mu_send(0, b);
    mu_send(0, w);
    mu_send(0, buf);
    mu_send(0, rxok);

    for (;;) {
    }
}
```

- [ ] **Step 2: Rebuild**

```bash
cd ~/Development/rt1170/evkb/cm4_spi_test
cmake --build build
```
Expected: rebuilds `cm4_spi.bin` → `cm4_spi.h` → `cm4_spi_test.elf`. No errors.

- [ ] **Step 3: Run the gate — verify it PASSES (GREEN)**

```bash
./run_qemu.sh; echo "exit=$?"
```
Expected UART (`exit=0`, every `check` PASS):
```
CM4SPI-GATE v1
cr=00000001
cfgr1=00000001
lpcg=00000001
croot=00000000
a=000000A5
b=0000003C
w=0000BEEF
buf=DEADBEEF
rxok=00000001
SPI_CM4=PASS
CM4SPI-DONE
```
If `a/b/w/buf` come back `000000FF`/`0000FFFF`, the transfer timed out — check the block-config order (CFGR1 must be written with `MEN=0`; `MEN` set last). `lpcg`/`croot` are informative; their exact value does not affect the verdict.

- [ ] **Step 4: Save the QEMU transcript**

```bash
cp cm4_spi.uart transcript_qemu.txt
```

- [ ] **Step 5: Commit the driver**

```bash
cd ~/Development/rt1170/evkb
git add cm4_spi_test/cm4/main_cm4.c cm4_spi_test/transcript_qemu.txt
git commit -m "cm4_spi_test: CM4 self-configured polled SPI loopback (Phase 3.1, GREEN)

CM4 ungates LPSPI1 clock, muxes AD_28/30/31, configures the block, and runs a
polled SDO->SDI loopback; streams cr/cfgr1/a/b/w/buf/rxok over MU -> SPI_CM4=PASS.

Co-Authored-By: Claude Opus 4.8 <noreply@anthropic.com>"
```

---

## Task 3: License audit + regression

**Files:**
- Modify: `evkb/tools/license-audit.sh:59` (append `cm4_spi_test:cm4_spi_test` to `GATES`)

- [ ] **Step 1: Add the gate to the audit's `GATES` list**

In `evkb/tools/license-audit.sh`, line 59, append ` cm4_spi_test:cm4_spi_test` inside the quotes:

```sh
GATES="sd_wav_play_test:sd_wav_play_test ethernet_test:ethernet_test native_ethernet_test:native_ethernet_test cm4_boot_test:cm4_boot_test cm4_image_test:cm4_image_test cm4_intr_test:cm4_intr_test cm4_dual_test:cm4_dual_test cm4_spi_test:cm4_spi_test"
```

- [ ] **Step 2: Run the license audit — expect PASS**

The audit walks each gate's `build/*.obj.d` for copyleft, so each gate in `GATES` must be built. `cm4_spi_test` is built (Task 2). Build any other gate the audit reports as `MISSING BUILD` (they are prior, already-green gates), then re-run:

```bash
cd ~/Development/rt1170/evkb
sh tools/license-audit.sh
```
Expected: ends with `LICENSE-AUDIT: PASS` (no `COPYLEFT` hit, no `DUAL-LICENSED SOURCE NOT EMPTY`, no `MISSING BUILD`). The CM4 driver is author-original C, so it introduces no copyleft.

- [ ] **Step 3: Regression — the unaffected neighbours stay green**

```bash
cd ~/Development/rt1170/evkb/cm4_dual_test && rm -rf build && \
  cmake -B build -DCMAKE_TOOLCHAIN_FILE=toolchain/rt1170-evkb.toolchain.cmake . && \
  cmake --build build && ./run_qemu.sh; echo "cm4_dual exit=$?"
cd ~/Development/SPI/tests/spi_loopback_test && rm -rf build && \
  cmake -B build -DCMAKE_TOOLCHAIN_FILE=toolchain/rt1170-evkb.toolchain.cmake . && \
  cmake --build build && ./run_qemu_spi.sh; echo "spi_loopback exit=$?"
```
Expected: both `exit=0`. (No qemu2 change was made, so no qemu2 rebuild/regression-suite is required; if that assumption ever breaks, run the full set from `silicon-truth-loop.md §"qemu2 regression set"`.)

- [ ] **Step 4: Commit**

```bash
cd ~/Development/rt1170/evkb
git add tools/license-audit.sh
git commit -m "license-audit: cover cm4_spi_test (Phase 3.1)

Co-Authored-By: Claude Opus 4.8 <noreply@anthropic.com>"
```

---

## Task 4: README

**Files:**
- Create: `evkb/cm4_spi_test/README.md`

- [ ] **Step 1: Write `evkb/cm4_spi_test/README.md`**

````markdown
# cm4_spi_test — CM4 self-configured polled SPI (Phase 3.1)

The first per-library CM4 enablement: a bare-metal **CM4 image self-configures
LPSPI1** (the EVKB Arduino-header SPI) — ungates its clock, muxes its pins,
configures the block — and runs a **polled master self-loopback** (external
**SDO→SDI jumper**). The CM4 has no console, so it streams each observation over
the **MU** to the CM7 (a pure reporter that never touches LPSPI1), which prints
them on LPUART1/VCOM.

The CM4 driver (`cm4/main_cm4.c`) is a C re-expression of this project's own
HW-verified `newdigate/SPI` `SPIIMXRT1176.cpp` `begin()`+`transfer()` — same
registers/clock/pins, no CM7 core, no C++ runtime. (Phase 3.3 consolidates it
and the C++ class onto a shared C core.)

| token | value | proves |
|---|---|---|
| `cr`    | `00000001` | the CM4 set LPSPI `CR.MEN` — block enabled |
| `cfgr1` | `00000001` | `CFGR1.MASTER` — master mode |
| `lpcg`  | `00000001` | CCM_LPCG104 readback (**informative**, not asserted) |
| `croot` | `00000000` | CCM_CLOCK_ROOT43 readback (**informative**, not asserted) |
| `a`     | `000000A5` | polled loopback echoed `0xA5` (8-bit) |
| `b`     | `0000003C` | polled loopback echoed `0x3C` (8-bit) |
| `w`     | `0000BEEF` | `transfer16` echoed `0xBEEF` (16-bit) |
| `buf`   | `DEADBEEF` | 4-byte buffer `{DE,AD,BE,EF}` echoed |
| `rxok`  | `00000001` | all loopback bytes matched → `SPI_CM4=PASS` |

## Why the hardware run is the real proof (QEMU is necessary, not sufficient)

The qemu2 board attaches an `ssi-loopback` child to LPSPI1
(`hw/arm/mimxrt1170-evk.c`); `imxrt_lpspi_transfer` echoes `rx=tx` **as soon as
`CR.MEN` is set — ignoring the LPCG clock gate, the clock root, and the pin
mux**. So a CM4 image that skipped the `CCM_LPCG104`/`CCM_CLOCK_ROOT43`/IOMUXC
writes would **still print `a=000000A5` and pass in QEMU** — a *circular pass*
(same shape as FlexCAN's SRXDIS gap). Therefore **`rx==tx` through the physical
SDO→SDI jumper on the EVKB is the only proof the CM4 brought up the functional
clock + pins itself.** The QEMU gate proves the register/transfer *sequence*;
silicon proves the *gating*. `lpcg`/`croot` are printed for HW diagnosis (a
failure localizes to gate/root/block/shift) but are not asserted, since a CCM
status bit may read differently on silicon without meaning failure.

## Build / run (QEMU)

    cmake -B build -DCMAKE_TOOLCHAIN_FILE=toolchain/rt1170-evkb.toolchain.cmake .
    cmake --build build
    ./run_qemu.sh

## Hardware (EVKB — the final arbiter)

Jumper **SDO (GPIO_AD_30) → SDI (GPIO_AD_31)** on the Arduino header. Then, for
an uncontaminated boot (LinkServer's connect script otherwise wakes the CM4 and
pokes CLOCK_ROOT1 — which would mask a CM4 clock-config bug):

    python3 ~/Development/rt1170/rt1170-console.py \
        /dev/cu.usbmodem5DQ2DDHVWO5EI3 115200 > transcript_hw_evkb.txt &
    /Applications/LinkServer_26.6.137/LinkServer flash \
        MIMXRT1176:MIMXRT1170-EVKB load build/cm4_spi_test.elf
    sleep 3; : > transcript_hw_evkb.txt      # drop contaminated post-flash output
    /Applications/LinkServer_26.6.137/LinkServer probe 5DQ2DDHVWO5EI \
        runscript ~/Development/rt1170/evkb/dualcore_mu_test/clean_boot.scp

Confirm `SPI_CM4=PASS` and that the asserted tokens are byte-identical to
`transcript_qemu.txt`; record the observed `lpcg`/`croot`.

## Reference transcripts

- `transcript_qemu.txt` — QEMU mimxrt1170-evk
- `transcript_hw_evkb.txt` — MIMXRT1170-EVKB, clean boot, SDO→SDI jumper

## Layout

- `cm4/` — the CM4 sketch: `main_cm4.c` (self-config LPSPI1 + polled loopback +
  MU stream), `startup_cm4.S` + `cm4.ld` (shared with `cm4_dual_test`).
- `cm4_spi_test.cpp` — the CM7 sketch (boot CM4 + MU reporter; never touches SPI).
````

- [ ] **Step 2: Commit**

```bash
cd ~/Development/rt1170/evkb
git add cm4_spi_test/README.md
git commit -m "cm4_spi_test: README (Phase 3.1)

Co-Authored-By: Claude Opus 4.8 <noreply@anthropic.com>"
```

---

## Task 5: EVKB probe (HARDWARE — manual, mandatory)

This closes the clock/power-gating probe. It needs the physical board + jumper; an agentic worker cannot run it — hand off to the operator and wait for the transcript.

**Files:**
- Create: `evkb/cm4_spi_test/transcript_hw_evkb.txt`
- Modify: `.claude/skills/cm4-bringup/references/cm4-roadmap.md`

- [ ] **Step 1: Wire the loopback jumper** — SDO **GPIO_AD_30** → SDI **GPIO_AD_31** on the Arduino header (same as the CM7 SPI loopback).

- [ ] **Step 2: Flash + clean boot + capture** — run the hardware block from the README (`README.md → Hardware`). The pyserial reader must be started **before** LinkServer.

- [ ] **Step 3: Verify silicon truth** — confirm `SPI_CM4=PASS`, and diff the asserted tokens against QEMU:

```bash
cd ~/Development/rt1170/evkb/cm4_spi_test
diff <(grep -E '^(cr|cfgr1|a|b|w|buf|rxok)=' transcript_qemu.txt) \
     <(grep -E '^(cr|cfgr1|a|b|w|buf|rxok)=' transcript_hw_evkb.txt) \
  && echo "ASSERTED TOKENS BYTE-IDENTICAL"
```
Expected: `ASSERTED TOKENS BYTE-IDENTICAL`. On silicon this pass additionally means the CM4 ungated the clock, muxed the pins, and drove a real 4 MHz SCK — the proof QEMU cannot give. Note the observed `lpcg`/`croot` in the commit message.

- [ ] **Step 4: Update the roadmap** — in `.claude/skills/cm4-bringup/references/cm4-roadmap.md`: change the top "Current phase" line and the 3.1 entry from "DESIGNED + SPEC'd / ready to implement" to **"DONE + ★★HW-VERIFIED 2026-07-<dd>"**, and append a session-log entry recording the byte-identical result + observed `lpcg`/`croot`.

- [ ] **Step 5: Commit the HW result**

```bash
cd ~/Development/rt1170/evkb
git add cm4_spi_test/transcript_hw_evkb.txt .claude/skills/cm4-bringup/references/cm4-roadmap.md
git commit -m "cm4_spi_test: HW-verify CM4 self-configured SPI on the EVKB (Phase 3.1)

Asserted tokens byte-identical to QEMU with the SDO->SDI jumper: the CM4
brought up LPSPI1's clock + pins itself and clocked a real loopback. Phase 3.1
complete. Observed lpcg=<..> croot=<..>.

Co-Authored-By: Claude Opus 4.8 <noreply@anthropic.com>"
```

---

## Self-review (completed against the spec)

- **Spec coverage:** §1 goal → Tasks 1–2 + 5; §2 ground-truth addresses → Task 2 driver; §3 artifacts → Task 1 file set; §4 driver/token split → Task 2 + CM7 sketch; §5 QEMU gate/red-first → Task 1 Step 7 + Task 2 Step 3; §6 probe/clean_boot → Task 5; §7 license/qemu2 → Task 3; §8 verification sequence → Tasks 2–5; §9 risks (bounded `SPI_TIMEOUT`, CM4-exclusive) → driver; §10 arc → roadmap (already committed). No gaps.
- **Placeholder scan:** none — every code block is complete; the only `<..>` are runtime-observed HW values in a commit message (Task 5), not code.
- **Type/name consistency:** token order `cr,cfgr1,lpcg,croot,a,b,w,buf,rxok` and the 9-value contract match between `main_cm4.c` (sends), `cm4_spi_test.cpp` (`labels[9]`/`v[9]`), and `run_qemu.sh` (`check` lines). Register macro names/offsets match the core header + qemu2 model. Image symbol `cm4_spi` / header `cm4_spi.h` match the `teensy_add_cm4_image(cm4_spi …)` call.
