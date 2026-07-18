# CM4 Phase 3.2 — Wire/I2C (LPI2C5) polled master, self-configured — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** A Cortex-M4 image self-configures LPI2C5 (clock + LPSR pins + block) and runs three polled I2C master transactions against the real on-board WM8962 codec — reset-write ACK, absent-address NACK, and device-ID read-back — streaming observations over the MU to the CM7, proven in QEMU and (final arbiter, wiring-free) on the EVKB.

**Architecture:** New gate `evkb/cm4_wire_test/` cloned from `cm4_spi_test`. The CM4 driver (`cm4/main_cm4.c`) is a distilled C re-expression of `newdigate/Wire`'s HW-verified `WireIMXRT1176.cpp` master path (`begin`/`endTransmission`/`requestFrom` — already fully polled) plus the WM8962 protocol from `newdigate/Audio`'s `control_wm8962.cpp`. CM7 = MU reporter only; LPI2C5 is CM4-exclusive. No qemu2/core/library changes.

**Tech Stack:** bare-metal Cortex-M4 C + `teensy_add_cm4_image`; imxrt1176 Arduino core (CM7); qemu2 `mimxrt1170-evk` gate via `qrun`/`gate-lib.sh`; LinkServer + `clean_boot.scp` on the EVKB.

**Spec:** `docs/superpowers/specs/2026-07-18-cm4-wire-polled-master-design.md`.

---

## Why QEMU-green is necessary but not sufficient — and the one expected divergence

Same circular-pass structure as 3.1: the qemu2 LPI2C model + `wm8962-stub` complete transfers on `MCR.MEN` alone, ignoring the CCM clock gate and LPSR pin mux. A CM4 that skipped the `CCM_LPCG102`/`ROOT41`/IOMUXC writes still goes green in QEMU. **Only silicon's `ack=0` + `rdv=6243` prove the CM4's clock-gating + LPSR pins.** Keep every clock/pin write in.

**`rdv` is deliberately world-split** (approved design): the stub reads `0x0000` (QEMU runner asserts that); the real WM8962 answers its device ID `0x6243` = the R15 readback default (HW check asserts that). The transcripts differ on **exactly that one line** — documented, precedent `cm4_intr_test`'s `systick`.

**Two silicon-truth details the driver MUST preserve** (both from the HW-verified Wire code, both mirrored by the silicon-corrected qemu model):
1. **Never judge ACK at TDF** — TDF asserts a byte-time before the ACK bit is sampled; ACK/NACK is judged at the STOP-completion (`SDF`) wait watching `NDF`.
2. The **absent-address probe** is a zero-data-byte write: START(0x2A|W) → STOP → the SDF wait sees NDF with `err` still `0xFF` → address-NACK (`2`).

---

## File structure

| File | Responsibility |
|---|---|
| `evkb/cm4_wire_test/cm4/main_cm4.c` | **CM4 driver** — self-config LPI2C5 + 3 transactions + MU stream (the probe). NEW. |
| `evkb/cm4_wire_test/cm4/startup_cm4.S` | **Verbatim copy** of `cm4_spi_test/cm4/startup_cm4.S`. |
| `evkb/cm4_wire_test/cm4/cm4.ld` | **Verbatim copy** of `cm4_spi_test/cm4/cm4.ld`. |
| `evkb/cm4_wire_test/cm4_wire_test.cpp` | **CM7 reporter** — boot CM4, read 8 MU values, print tokens + verdict. NEW. |
| `evkb/cm4_wire_test/CMakeLists.txt` | Clone of `cm4_spi_test`, names swapped. NEW. |
| `evkb/cm4_wire_test/toolchain/rt1170-evkb.toolchain.cmake` | **Verbatim copy**. |
| `evkb/cm4_wire_test/run_qemu.sh` | Gate runner. NEW. |
| `evkb/cm4_wire_test/README.md` | Token table + divergence doc + wiring-free HW procedure. NEW (Task 4). |
| `evkb/cm4_wire_test/transcript_qemu.txt` / `transcript_hw_evkb.txt` | Saved transcripts (Tasks 2 / 5). |
| `evkb/tools/license-audit.sh` | Append `cm4_wire_test:cm4_wire_test` to `GATES`. MODIFY. |
| `.claude/skills/cm4-bringup/references/cm4-roadmap.md` | 3.2 → HW-VERIFIED (Task 5). MODIFY. |

**MU value contract (CM4 → CM7, channel 0, fixed order, 8 words):**
`mcr, lpcg, croot, ack, nack, rdn, rdv, done`. Asserted: `mcr=1, ack=0, nack=2, rdn=2, done=1` (+ per-world `rdv`). Informative: `lpcg, croot`.

---

## Task 1: Scaffold the gate + failing harness (RED)

**Files:**
- Create dir: `evkb/cm4_wire_test/` (+ `cm4/`, `toolchain/`)
- Copy: `cm4/startup_cm4.S`, `cm4/cm4.ld`, `toolchain/rt1170-evkb.toolchain.cmake` (verbatim from `cm4_spi_test/`)
- Create: `CMakeLists.txt`, `cm4/main_cm4.c` (stub), `cm4_wire_test.cpp`, `run_qemu.sh`

- [ ] **Step 1: Create the directory and copy the three shared files verbatim**

```bash
cd ~/Development/rt1170/evkb
mkdir -p cm4_wire_test/cm4 cm4_wire_test/toolchain
cp cm4_spi_test/cm4/startup_cm4.S            cm4_wire_test/cm4/startup_cm4.S
cp cm4_spi_test/cm4/cm4.ld                   cm4_wire_test/cm4/cm4.ld
cp cm4_spi_test/toolchain/rt1170-evkb.toolchain.cmake cm4_wire_test/toolchain/rt1170-evkb.toolchain.cmake
```

- [ ] **Step 2: Write `evkb/cm4_wire_test/CMakeLists.txt`**

```cmake
cmake_minimum_required(VERSION 3.24)
project(cm4_wire_test)

set(TEENSY_VERSION 117 CACHE STRING "")

include(FetchContent)
FetchContent_Declare(teensy_cmake_macros SOURCE_DIR ${CMAKE_CURRENT_LIST_DIR}/../teensy-cmake-macros)
FetchContent_MakeAvailable(teensy_cmake_macros)
include(${teensy_cmake_macros_SOURCE_DIR}/CMakeLists.include.txt)

import_arduino_library(cores ${CMAKE_CURRENT_LIST_DIR}/../cores/imxrt1176)

# CM4 image via the Phase-2B first-class dual-target macro (emits cm4_wire.h).
teensy_add_cm4_image(cm4_wire
    LINKER  ${CMAKE_CURRENT_LIST_DIR}/cm4/cm4.ld
    SOURCES ${CMAKE_CURRENT_LIST_DIR}/cm4/startup_cm4.S
            ${CMAKE_CURRENT_LIST_DIR}/cm4/main_cm4.c)

teensy_add_executable(cm4_wire_test cm4_wire_test.cpp)
teensy_target_link_libraries(cm4_wire_test cores)
target_link_libraries(cm4_wire_test.elf stdc++)
teensy_target_link_cm4_image(cm4_wire_test cm4_wire)
```

- [ ] **Step 3: Write the STUB `evkb/cm4_wire_test/cm4/main_cm4.c`** (no I2C yet → gate must fail)

```c
/* cm4_wire_test CM4 firmware — STUB (Task 1). No I2C yet: the gate must FAIL
 * (RED) until Task 2 fills in the self-config + transactions + MU stream. */
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

- [ ] **Step 4: Write the CM7 reporter `evkb/cm4_wire_test/cm4_wire_test.cpp`** (full — unchanged in Task 2)

```cpp
/*
 * cm4_wire_test — Phase-3.2: the CM4 SELF-CONFIGURES LPI2C5 (the on-board
 * codec bus) and runs three polled I2C master transactions against the REAL
 * WM8962 codec @0x1A.  This CM7 sketch only boots the CM4 image and reports
 * the CM4's observations over the MU on LPUART1 — it never touches
 * Wire/LPI2C (LPI2C5 is CM4-exclusive in 3.2).
 *
 * Tokens over Serial1 (LPUART1), streamed by the CM4 over MU channel 0:
 *   mcr   = 00000001   LPI2C MCR.MEN — the CM4 enabled the master block
 *   lpcg  = ........   CCM_LPCG102 readback (informative — not asserted)
 *   croot = ........   CCM_CLOCK_ROOT41 readback (informative — not asserted)
 *   ack   = 00000000   reset-write R15<-0x6243 to WM8962 @0x1A ACKed (err 0)
 *   nack  = 00000002   probe of absent address 0x2A NACKed (addr-NACK err 2)
 *   rdn   = 00000002   ID read-back of R15 returned 2 bytes
 *   rdv   = ????????   the 16-bit read value — WORLD-SPLIT by design:
 *                      QEMU wm8962-stub reads 0x0000; real silicon answers
 *                      the WM8962 device ID 0x6243.  NOT folded into PASS
 *                      here; each world's checker asserts its own value.
 *   done  = 00000001   CM4 sequence completed
 * Verdict: WIRE_CM4=PASS requires mcr/ack/nack/rdn/done at expected values.
 */
#include "Arduino.h"
#include "core_pins.h"
#include "HardwareSerial.h"
#include "Multicore.h"
#include "MessagingUnit.h"
#include "cm4_wire.h"     /* generated by teensy_add_cm4_image */

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
    Serial1.println("CM4WIRE-GATE v1");

    MU.begin();
    Multicore.begin(cm4_wire, sizeof(cm4_wire));

    static const char *labels[8] =
        { "mcr", "lpcg", "croot", "ack", "nack", "rdn", "rdv", "done" };
    uint32_t v[8];
    bool ok = true;
    for (int i = 0; i < 8; i++) {
        if (wait_recv(0, &v[i])) {
            phex(labels[i], v[i]);
        } else {
            ptimeout(labels[i]);
            v[i] = 0xFFFFFFFFu;
            ok = false;
        }
    }

    /* rdv (v[6]) is intentionally NOT part of PASS — world-dependent. */
    bool pass = ok
        && v[0] == 0x1u          /* mcr.MEN */
        && v[3] == 0x0u          /* ack: reset write ACKed */
        && v[4] == 0x2u          /* nack: addr-NACK at 0x2A */
        && v[5] == 0x2u          /* rdn: 2 bytes read back */
        && v[7] == 0x1u;         /* done */
    Serial1.println(pass ? "WIRE_CM4=PASS" : "WIRE_CM4=FAIL");
    Serial1.println("CM4WIRE-DONE");
}

void loop()
{
}
```

- [ ] **Step 5: Write the gate runner `evkb/cm4_wire_test/run_qemu.sh`**

```sh
#!/bin/sh
# QEMU gate for Phase-3.2: the CM4 self-configures LPI2C5 and runs polled I2C
# transactions against the wm8962-stub; the CM7 reports over the MU on LPUART1.
# NOTE: the LPI2C model + stub respond on MCR.MEN alone (clock/pins ignored),
# so QEMU proves the register/transfer SEQUENCE only — the wiring-free HW run
# proves the CM4's clock-gating + LPSR pin-mux (see README / spec).
# rdv is WORLD-SPLIT by design: this runner asserts the stub contract
# (rdv=00000000); the HW check asserts the WM8962 device ID (rdv=00006243).
set -e
QEMU=~/Development/rt1170/evkb/tools/qrun
DIR=$(cd "$(dirname "$0")" && pwd)
. ~/Development/rt1170/evkb/tools/gate-lib.sh
gate_init
ELF="$DIR/build/cm4_wire_test.elf"
OUT="$DIR/cm4_wire.uart"
rm -f "$OUT"
"$QEMU" -M mimxrt1170-evk -global fsl-imxrt1170.boot-xip=on -kernel "$ELF" \
    -display none -serial file:"$OUT" -d guest_errors -D "$DIR/cm4_wire.dbg" &
P=$!; gate_pid $P
for _ in $(seq 1 40); do
    [ -f "$OUT" ] && grep -q "CM4WIRE-DONE" "$OUT" 2>/dev/null && break
    sleep 0.25
done
kill $P 2>/dev/null; wait $P 2>/dev/null || true

echo "==== captured UART ===="; [ -f "$OUT" ] && cat "$OUT" || echo "(no UART output)"

fail=0
check() {
    if grep -q "^$1" "$OUT"; then echo "PASS: $1"; else echo "FAIL: expected $1"; fail=1; fi
}
grep -q "CM4WIRE-GATE v1" "$OUT" || { echo "FAIL: banner missing"; exit 1; }
check "mcr=00000001"      # LPI2C MCR.MEN set (master enabled)
check "ack=00000000"      # WM8962 reset-write ACKed (err 0)
check "nack=00000002"     # absent addr 0x2A -> address NACK (err 2)
check "rdn=00000002"      # ID read-back returned 2 bytes
check "rdv=00000000"      # stub contract: all reads 0x00 (HW expects 00006243)
check "done=00000001"     # CM4 sequence completed
check "WIRE_CM4=PASS"     # verdict
# lpcg= / croot= are printed for HW diagnosis but intentionally NOT asserted.
grep -q "CM4WIRE-DONE" "$OUT" || { echo "FAIL: DONE missing"; fail=1; }

if [ $fail -eq 0 ]; then
    echo "PASS: CM4 self-configured polled I2C verified in QEMU"
else
    echo "GATE FAILED"; exit 1
fi
```

- [ ] **Step 6: Build the scaffold**

```bash
cd ~/Development/rt1170/evkb/cm4_wire_test
rm -rf build
cmake -B build -DCMAKE_TOOLCHAIN_FILE=toolchain/rt1170-evkb.toolchain.cmake .
cmake --build build
```
Expected: builds `build/cm4_wire_test.elf` (and generates `build/cm4_wire.h`). No errors.

- [ ] **Step 7: Run the gate — verify it FAILS (RED)**

```bash
chmod +x run_qemu.sh
./run_qemu.sh; echo "exit=$?"
```
Expected: the CM4 stub sends nothing → the CM7 prints `mcr=TIMEOUT …`, eventually `WIRE_CM4=FAIL`; the runner prints `FAIL: expected mcr=00000001` … and `GATE FAILED`, `exit=1`. (As in 3.1's RED, the runner's 10 s poll may truncate the *displayed* UART while all 8 waits time out — the exit-1 verdict is still correct; this self-resolves in Task 2 when the CM4 replies instantly.) **This RED confirms the harness tests the CM4 driver.** If it PASSES or fails differently (build error, missing banner), STOP and report.

- [ ] **Step 8: Commit the scaffold**

```bash
cd ~/Development/rt1170/evkb
git add cm4_wire_test/CMakeLists.txt cm4_wire_test/cm4_wire_test.cpp cm4_wire_test/run_qemu.sh \
        cm4_wire_test/cm4/startup_cm4.S cm4_wire_test/cm4/cm4.ld cm4_wire_test/cm4/main_cm4.c \
        cm4_wire_test/toolchain/rt1170-evkb.toolchain.cmake
git commit -m "cm4_wire_test: gate harness + CM7 reporter (Phase 3.2, RED)

CM4 stub sends nothing -> gate FAILs, proving the harness tests the CM4 driver.

Co-Authored-By: Claude Opus 4.8 <noreply@anthropic.com>"
```

---

## Task 2: Implement the CM4 self-config + I2C driver (GREEN)

**Files:**
- Modify (replace stub): `evkb/cm4_wire_test/cm4/main_cm4.c`
- Create: `evkb/cm4_wire_test/transcript_qemu.txt`

- [ ] **Step 1: Replace `evkb/cm4_wire_test/cm4/main_cm4.c` with the full driver**

```c
/* cm4_wire_test CM4 firmware (Phase 3.2): the CM4 SELF-CONFIGURES LPI2C5 (the
 * on-board WM8962 codec bus) and runs three polled I2C master transactions,
 * streaming each observation to the CM7 over the MU (channel 0, in order):
 *   1. reset-write R15<-0x6243 to the WM8962 @0x1A  -> ack  (expect 0)
 *   2. zero-byte probe of absent address 0x2A       -> nack (expect 2)
 *   3. device-ID read-back of R15 (repeated START)  -> rdn=2, rdv
 *
 * Adapted from this project's own newdigate/Wire WireIMXRT1176.cpp master path
 * (begin/setClock/endTransmission/requestFrom, MIT, N. Newdigate) and the
 * WM8962 register protocol from newdigate/Audio control_wm8962.cpp (MIT) — the
 * HW-verified LPI2C5 self-config + polled-master sequences, re-expressed in C
 * for the bare-metal CM4 image.  No logic change; register/clock/pin values
 * identical.  Keep in sync with WireIMXRT1176.cpp; Phase 3.3 consolidates onto
 * a shared C core.  The R15 readback default 0x6243 (= device ID) is a
 * hardware FACT taken from the Linux wm8962.c reg_default table (2026-07-18);
 * no code was taken from that GPL source.
 *
 * SILICON TRUTH: the qemu2 LPI2C model + wm8962-stub respond on MCR.MEN alone
 * (clock gate / clock root / LPSR pin mux ignored), so a green QEMU run proves
 * only the register/transfer sequence; the wiring-free EVKB run (real codec
 * ACK + ID) is what proves the CM4 brought up the clock and LPSR pins itself.
 * ACK/NACK is judged at STOP completion (SDF wait watching NDF), NEVER at TDF
 * — TDF leads the ACK bit by a byte-time on silicon (WireIMXRT1176.cpp note;
 * the qemu model's deferred-NDF mirrors exactly this).
 * Public-domain scaffolding (N. Newdigate); adapted register logic MIT. */
#include <stdint.h>

/* ---- CCM (shared): LPI2C5 clock gate + root (imxrt1176.h) ---- */
#define CCM_LPCG102_DIRECT       (*(volatile uint32_t *)0x40CC6CC0u) /* LPI2C5 gate */
#define CCM_CLOCK_ROOT41_CONTROL (*(volatile uint32_t *)0x40CC1480u) /* LPI2C5 root */
#define CROOT41_VAL  (1u << 8)   /* mux 1 -> 24 MHz (WireIMXRT1176 lpi2c5_hardware) */

/* ---- LPSR IOMUXC: SCL=GPIO_LPSR_05, SDA=GPIO_LPSR_04, ALT0|SION ---- */
#define IOMUX_MUX_LPSR_04 (*(volatile uint32_t *)0x40C08010u)  /* SDA */
#define IOMUX_MUX_LPSR_05 (*(volatile uint32_t *)0x40C08014u)  /* SCL */
#define IOMUX_PAD_LPSR_04 (*(volatile uint32_t *)0x40C08050u)
#define IOMUX_PAD_LPSR_05 (*(volatile uint32_t *)0x40C08054u)
#define IOMUX_SCL_SELECT  (*(volatile uint32_t *)0x40C08084u)  /* LPI2C5_SCL_SELECT_INPUT */
#define IOMUX_SDA_SELECT  (*(volatile uint32_t *)0x40C08088u)  /* LPI2C5_SDA_SELECT_INPUT */
#define IOMUX_ALT0_SION 0x10u
#define IOMUX_PAD_OD    0x0Au    /* LPSR open-drain pad config (lpi2c5_hardware) */
#define IOMUX_DAISY     0x0u

/* ---- LPI2C5 (base 0x40C34000; offsets == IMXRT_LPI2C_t / qemu2 imxrt_lpi2c) ---- */
#define LPI2C5_BASE  0x40C34000u
#define LPI2C_MCR    (*(volatile uint32_t *)(LPI2C5_BASE + 0x10u))
#define LPI2C_MSR    (*(volatile uint32_t *)(LPI2C5_BASE + 0x14u))
#define LPI2C_MCFGR1 (*(volatile uint32_t *)(LPI2C5_BASE + 0x24u))
#define LPI2C_MCCR0  (*(volatile uint32_t *)(LPI2C5_BASE + 0x48u))
#define LPI2C_MTDR   (*(volatile uint32_t *)(LPI2C5_BASE + 0x60u))
#define LPI2C_MRDR   (*(volatile uint32_t *)(LPI2C5_BASE + 0x70u))
#define MCR_MEN  (1u << 0)
#define MCR_RST  (1u << 1)
#define MCR_RTF  (1u << 8)
#define MCR_RRF  (1u << 9)
#define MSR_TDF  (1u << 0)
#define MSR_RDF  (1u << 1)
#define MSR_EPF  (1u << 8)
#define MSR_SDF  (1u << 9)
#define MSR_NDF  (1u << 10)
#define MSR_ALF  (1u << 11)
#define MSR_FEF  (1u << 12)
#define TX_CMD(cmd, data)  (((uint32_t)(cmd) << 8) | ((data) & 0xFFu))
#define CMD_TXD    0u
#define CMD_RXD    1u
#define CMD_STOP   2u
#define CMD_START  4u
#define MRDR_RXEMPTY (1u << 14)
/* setClock(100000) @24 MHz -> pre=1, div=120, clklo=63 (clamped from 72),
 * clkhi=48, DATAVD=SETHOLD=clkhi/2=24 (WireIMXRT1176.cpp::setClock math). */
#define MCFGR1_VAL  0x1u
#define MCCR0_VAL   0x1818303Fu
#define WIRE_TIMEOUT 100000u

#define WM8962_ADDR  0x1Au
#define ABSENT_ADDR  0x2Au   /* clear of WM8962 0x1A + FXLS8974 accel 0x18 */

/* ---- MU B side (the CM4's) ---- */
#define MUB_BASE   0x40C4C000u
#define MUB_TR(n)  (*(volatile uint32_t *)(MUB_BASE + 0x00u + ((n) << 2)))
#define MUB_SR     (*(volatile uint32_t *)(MUB_BASE + 0x20u))
#define SR_TE(n)   (1u << (23 - (n)))

/* The shared vector table (startup_cm4.S) references these C symbols. Polled
 * I2C needs neither, but the table entries must resolve. */
void SysTick_Handler(void) {}
void MU_IRQHandler(void) {}

static void mu_send(unsigned ch, uint32_t v)
{
    while (!(MUB_SR & SR_TE(ch))) {
    }
    MUB_TR(ch) = v;
}

/* Wait until any bit in `mask` is set, or an error bit appears / timeout.
 * Mirrors TwoWire::wait_flag: on NDF, *err's prior value classifies the NACK
 * (0xFF = address NACK -> 2, else data NACK -> 3); ALF/FEF -> 4; timeout 5. */
static int wait_flag(uint32_t mask, uint32_t error_mask, uint32_t *err)
{
    for (uint32_t g = 0; g < WIRE_TIMEOUT; g++) {
        uint32_t s = LPI2C_MSR;
        if (s & error_mask) {
            if (s & MSR_NDF) *err = (*err == 0xFFu) ? 2u : 3u;
            else *err = 4u;
            LPI2C_MSR = s;                       /* W1C the flags */
            return 0;
        }
        if (s & mask) return 1;
    }
    *err = 5u;
    return 0;
}

/* After a NACK/error, flush the FIFOs so the next transaction starts clean
 * (TwoWire::bus_recover). */
static void bus_recover(void)
{
    LPI2C_MCR = MCR_MEN | MCR_RTF | MCR_RRF;
    LPI2C_MCR = MCR_MEN;
    LPI2C_MSR = LPI2C_MSR;
}

/* Polled master write, mirroring TwoWire::endTransmission(sendStop):
 * START+addr(W), per byte TDF-wait + TXD, optional STOP with the ACK/NACK
 * judged at the SDF wait (watching NDF).  Returns 0 ok / 2 addr-NACK /
 * 3 data-NACK / 4 error / 5 timeout. */
static uint32_t i2c_write(uint8_t addr, const uint8_t *data, uint32_t len,
                          int send_stop)
{
    uint32_t err = 0xFFu;                        /* NACK now = address NACK */
    LPI2C_MSR = LPI2C_MSR;                       /* clear stale flags */
    LPI2C_MTDR = TX_CMD(CMD_START, (uint32_t)(addr << 1) | 0u);
    for (uint32_t i = 0; i < len; i++) {
        if (!wait_flag(MSR_TDF, MSR_NDF | MSR_ALF | MSR_FEF, &err)) {
            bus_recover();
            return err;
        }
        err = 0u;                                /* past the address */
        LPI2C_MTDR = TX_CMD(CMD_TXD, data[i]);
    }
    if (send_stop) {
        LPI2C_MTDR = TX_CMD(CMD_STOP, 0);
        if (!wait_flag(MSR_SDF, MSR_NDF | MSR_ALF | MSR_FEF, &err)) {
            bus_recover();
            return err;
        }
        LPI2C_MSR = MSR_SDF | MSR_EPF;
    }
    return 0u;
}

/* Polled master read, mirroring TwoWire::requestFrom: repeated-START+addr(R),
 * RXD with N-1 encoding, per-byte RDF-wait + MRDR, STOP.  Returns bytes read. */
static uint32_t i2c_read(uint8_t addr, uint8_t *buf, uint32_t quantity)
{
    uint32_t err = 0xFFu, n = 0;
    LPI2C_MSR = LPI2C_MSR;
    LPI2C_MTDR = TX_CMD(CMD_START, (uint32_t)(addr << 1) | 1u);
    if (!wait_flag(MSR_TDF, MSR_NDF | MSR_ALF | MSR_FEF, &err)) {
        LPI2C_MTDR = TX_CMD(CMD_STOP, 0);
        return 0;
    }
    LPI2C_MTDR = TX_CMD(CMD_RXD, (uint8_t)(quantity - 1));
    for (uint32_t i = 0; i < quantity; i++) {
        err = 0u;
        if (!wait_flag(MSR_RDF, MSR_ALF | MSR_FEF, &err)) break;
        uint32_t r = LPI2C_MRDR;
        if (r & MRDR_RXEMPTY) break;
        buf[n++] = (uint8_t)(r & 0xFFu);
    }
    LPI2C_MTDR = TX_CMD(CMD_STOP, 0);
    wait_flag(MSR_SDF, MSR_ALF | MSR_FEF, &err);
    LPI2C_MSR = MSR_SDF | MSR_EPF;
    return n;
}

int main(void)
{
    /* --- self-config LPI2C5 (mirrors TwoWire::begin for lpi2c5_hardware) --- */
    CCM_LPCG102_DIRECT = 1u;                 /* ungate the LPI2C5 clock */
    CCM_CLOCK_ROOT41_CONTROL = CROOT41_VAL;  /* mux 1 -> 24 MHz */

    IOMUX_MUX_LPSR_05 = IOMUX_ALT0_SION;  IOMUX_PAD_LPSR_05 = IOMUX_PAD_OD;  /* SCL */
    IOMUX_MUX_LPSR_04 = IOMUX_ALT0_SION;  IOMUX_PAD_LPSR_04 = IOMUX_PAD_OD;  /* SDA */
    IOMUX_SCL_SELECT = IOMUX_DAISY;
    IOMUX_SDA_SELECT = IOMUX_DAISY;

    LPI2C_MCR = MCR_RST;  LPI2C_MCR = 0u;    /* reset the master block */
    LPI2C_MCFGR1 = MCFGR1_VAL;               /* prescale 1 (MEN=0) */
    LPI2C_MCCR0  = MCCR0_VAL;                /* ~100 kHz timing (MEN=0) */
    LPI2C_MCR = MCR_MEN;                     /* enable */

    /* --- config readbacks --- */
    uint32_t mcr   = LPI2C_MCR & MCR_MEN;              /* -> 1 */
    uint32_t lpcg  = CCM_LPCG102_DIRECT;               /* informative */
    uint32_t croot = CCM_CLOCK_ROOT41_CONTROL;         /* informative */

    /* --- 1. reset-write R15<-0x6243 (WM8962_Init's own first write) --- */
    static const uint8_t reset_wr[4] = { 0x00u, 0x0Fu, 0x62u, 0x43u };
    uint32_t ack = i2c_write(WM8962_ADDR, reset_wr, 4, 1);

    /* --- 2. zero-byte probe of an absent address -> address NACK --- */
    uint32_t nack = i2c_write(ABSENT_ADDR, 0, 0, 1);

    /* --- 3. device-ID read-back of R15 (write reg addr, repeated START) --- */
    static const uint8_t reg_addr[2] = { 0x00u, 0x0Fu };
    uint8_t rd[2] = { 0, 0 };
    uint32_t rdn = 0, rdv = 0;
    if (i2c_write(WM8962_ADDR, reg_addr, 2, 0) == 0u) {   /* no STOP */
        rdn = i2c_read(WM8962_ADDR, rd, 2);
        rdv = ((uint32_t)rd[0] << 8) | rd[1];
    }

    /* --- stream the 8 observations to the CM7 (MU TR0, fixed order) --- */
    mu_send(0, mcr);
    mu_send(0, lpcg);
    mu_send(0, croot);
    mu_send(0, ack);
    mu_send(0, nack);
    mu_send(0, rdn);
    mu_send(0, rdv);
    mu_send(0, 1u);                          /* done */

    for (;;) {
    }
}
```

- [ ] **Step 2: Rebuild**

```bash
cd ~/Development/rt1170/evkb/cm4_wire_test
cmake --build build
```
Expected: rebuilds `cm4_wire.bin` → `cm4_wire.h` → `cm4_wire_test.elf`. No errors.

- [ ] **Step 3: Run the gate — verify it PASSES (GREEN)**

```bash
./run_qemu.sh; echo "exit=$?"
```
Expected UART (`exit=0`, every `check` PASS):
```
CM4WIRE-GATE v1
mcr=00000001
lpcg=00000001
croot=00000100
ack=00000000
nack=00000002
rdn=00000002
rdv=00000000
done=00000001
WIRE_CM4=PASS
CM4WIRE-DONE
```
Debug hints if not: `ack=5` = TDF/SDF timeout (block-config order: MCFGR1/MCCR0 need MEN=0, MEN last); `nack=0` = the model delivered self-ACK (wrong address encoding — check `(addr<<1)|0`); `rdn=0` = the repeated-START write phase failed. Report real output; do NOT patch register logic to force a pass — it is verified; diagnose instead.

- [ ] **Step 4: Save the QEMU transcript**

```bash
cp cm4_wire.uart transcript_qemu.txt
```

- [ ] **Step 5: Run the gate twice more (stability)**

```bash
./run_qemu.sh >/dev/null 2>&1; echo "run2=$?"; ./run_qemu.sh >/dev/null 2>&1; echo "run3=$?"
```
Expected: `run2=0`, `run3=0`.

- [ ] **Step 6: Commit the driver**

```bash
cd ~/Development/rt1170/evkb
git add cm4_wire_test/cm4/main_cm4.c cm4_wire_test/transcript_qemu.txt
git commit -m "cm4_wire_test: CM4 self-configured polled I2C to WM8962 (Phase 3.2, GREEN)

CM4 ungates LPI2C5's clock (LPCG102/ROOT41 mux1), muxes the LPSR pads, and
runs reset-write ACK / absent-addr NACK / ID read-back against the codec bus;
streams mcr/ack/nack/rdn/rdv over MU -> WIRE_CM4=PASS. rdv=0000 here is the
wm8962-stub contract; silicon answers 0x6243 (asserted in the HW run).

Co-Authored-By: Claude Opus 4.8 <noreply@anthropic.com>"
```

---

## Task 3: License audit + regression

**Files:**
- Modify: `evkb/tools/license-audit.sh` (append to `GATES`)

- [ ] **Step 1: Add the gate to the audit's `GATES` list**

In `evkb/tools/license-audit.sh` (the `GATES="..."` line), append ` cm4_wire_test:cm4_wire_test` inside the quotes, after `cm4_spi_test:cm4_spi_test`:

```sh
GATES="sd_wav_play_test:sd_wav_play_test ethernet_test:ethernet_test native_ethernet_test:native_ethernet_test cm4_boot_test:cm4_boot_test cm4_image_test:cm4_image_test cm4_intr_test:cm4_intr_test cm4_dual_test:cm4_dual_test cm4_spi_test:cm4_spi_test cm4_wire_test:cm4_wire_test"
```

- [ ] **Step 2: Run the license audit — expect PASS**

```bash
cd ~/Development/rt1170/evkb
sh tools/license-audit.sh
```
Expected: `cm4_wire_test: … dep paths … checked` in the part-2 walk; ends `LICENSE-AUDIT: PASS`. All sibling gates were built as of 3.1; rebuild any reported `MISSING BUILD` (they are prior green gates) and re-run. Do NOT bypass the audit; report BLOCKED if a sibling genuinely cannot build.

- [ ] **Step 3: Regression — the mirrored neighbours stay green**

```bash
cd ~/Development/rt1170/evkb/cm4_spi_test && ./run_qemu.sh >/dev/null 2>&1; echo "cm4_spi exit=$?"
cd ~/Development/Wire/tests/wire_master_test && rm -rf build && \
  cmake -B build -DCMAKE_TOOLCHAIN_FILE=toolchain/rt1170-evkb.toolchain.cmake . >/dev/null && \
  cmake --build build >/dev/null && ./run_qemu_wire.sh; echo "wire_master exit=$?"
```
Expected: both `exit=0`. (`cm4_spi_test` shares the startup/linker/MU pattern; `wire_master_test` is the library gate whose logic the CM4 driver mirrors — untouched, must stay green. If `cm4_spi_test/build` is missing, rebuild it first the same way.) No qemu2 rebuild — nothing changed there.

- [ ] **Step 4: Commit**

```bash
cd ~/Development/rt1170/evkb
git add tools/license-audit.sh
git commit -m "license-audit: cover cm4_wire_test (Phase 3.2)

Co-Authored-By: Claude Opus 4.8 <noreply@anthropic.com>"
```

---

## Task 4: README

**Files:**
- Create: `evkb/cm4_wire_test/README.md`

- [ ] **Step 1: Write `evkb/cm4_wire_test/README.md`**

````markdown
# cm4_wire_test — CM4 self-configured polled I2C to the WM8962 (Phase 3.2)

The second per-library CM4 enablement: a bare-metal **CM4 image
self-configures LPI2C5** (the on-board codec bus — `CCM_LPCG102`,
`CCM_CLOCK_ROOT41` mux 1, **LPSR-domain** pads `GPIO_LPSR_05/04`) and runs
three **polled I2C master transactions against the real on-board WM8962
@0x1A**, streaming observations over the **MU** to the CM7 (a pure reporter
that never touches Wire/LPI2C), which prints them on LPUART1/VCOM.

The CM4 driver (`cm4/main_cm4.c`) is a C re-expression of this project's own
HW-verified `newdigate/Wire` `WireIMXRT1176.cpp` master path + the WM8962
protocol from `newdigate/Audio` `control_wm8962.cpp` — same registers, clock,
pins; no CM7 core, no C++ runtime. (Phase 3.3 consolidates onto a shared C
core.)

| token | QEMU | HW | proves |
|---|---|---|---|
| `mcr`   | `00000001` | `00000001` | the CM4 enabled the LPI2C master block |
| `lpcg`  | `00000001` | `00000001` | CCM_LPCG102 readback (**informative**) |
| `croot` | `00000100` | `00000100` | CCM_CLOCK_ROOT41 readback (**informative**) |
| `ack`   | `00000000` | `00000000` | reset-write `R15←0x6243` @0x1A **ACKed** |
| `nack`  | `00000002` | `00000002` | absent addr `0x2A` **address-NACKed** (avoids WM8962 0x1A + FXLS8974 accel 0x18) |
| `rdn`   | `00000002` | `00000002` | ID read-back returned 2 bytes (repeated START) |
| `rdv`   | `00000000` | `00006243` | **the one expected divergence** — see below |
| `done`  | `00000001` | `00000001` | CM4 sequence completed |

## `rdv`: the deliberate QEMU-vs-silicon split

QEMU's `wm8962-stub` (`hw/i2c/wm8962_stub.c`) "ACKs all writes and returns
0x00 for all reads — it is NOT a codec model", so the QEMU runner asserts
`rdv=00000000` (the stub contract).  Real silicon answers the **WM8962 device
ID `0x6243`** — the R15 readback default (Linux `wm8962.c` reg_default
`{ 15, 0x6243 }`, used as a hardware fact only).  Write-the-ID-then-read-the-ID
is self-evidencing: a stuck-low bus reads `0x0000`, stuck-high reads `0xFFFF` —
only a live codec on a CM4-brought-up bus says `0x6243`.  The two committed
transcripts therefore differ on **exactly this one line** (precedent:
`cm4_intr_test`'s `systick` characterisation token).

## Why the hardware run is the real proof

Same circular-pass structure as `cm4_spi_test`: the qemu2 LPI2C model + stub
respond on `MCR.MEN` alone, ignoring the clock gate, clock root, and LPSR pin
mux — a CM4 that skipped those writes would still pass in QEMU.  Silicon's
`ack=0` + `rdv=6243` through the on-board pull-ups are the only proof the CM4
itself brought up the clock and the LPSR pins.  ACK/NACK is judged at STOP
completion (never at TDF, which leads the ACK bit by a byte-time on silicon —
the qemu model's deferred-NDF was corrected against this exact trap).

## Build / run (QEMU)

    cmake -B build -DCMAKE_TOOLCHAIN_FILE=toolchain/rt1170-evkb.toolchain.cmake .
    cmake --build build
    ./run_qemu.sh

## Hardware (EVKB — the final arbiter, WIRING-FREE)

No jumper needed — the WM8962 and its pull-ups are soldered on (codec I2C
needs no MCLK for register access).  For an uncontaminated boot:

    python3 ~/Development/rt1170/rt1170-console.py \
        /dev/cu.usbmodem5DQ2DDHVWO5EI3 115200 > /tmp/hw.uart &
    /Applications/LinkServer_26.6.137/LinkServer flash \
        MIMXRT1176:MIMXRT1170-EVKB load build/cm4_wire_test.elf
    sleep 3; : > /tmp/hw.uart      # drop contaminated post-flash output
    /Applications/LinkServer_26.6.137/LinkServer probe 5DQ2DDHVWO5EI \
        runscript ~/Development/rt1170/evkb/dualcore_mu_test/clean_boot.scp

Confirm `WIRE_CM4=PASS`, the asserted tokens byte-identical to
`transcript_qemu.txt`, and **`rdv=00006243`**.  Strip the leading `\0` block
from the capture (console reconnect artifact): `tr -d '\000' < /tmp/hw.uart >
transcript_hw_evkb.txt`.

## Reference transcripts

- `transcript_qemu.txt` — QEMU mimxrt1170-evk (`rdv=00000000`, stub)
- `transcript_hw_evkb.txt` — EVKB clean boot (`rdv=00006243`, real codec)

## Layout

- `cm4/` — the CM4 sketch: `main_cm4.c` (self-config LPI2C5 + 3 transactions +
  MU stream), `startup_cm4.S` + `cm4.ld` (shared with `cm4_spi_test`).
- `cm4_wire_test.cpp` — the CM7 sketch (boot CM4 + MU reporter; never touches Wire).
````

- [ ] **Step 2: Commit**

```bash
cd ~/Development/rt1170/evkb
git add cm4_wire_test/README.md
git commit -m "cm4_wire_test: README (Phase 3.2)

Co-Authored-By: Claude Opus 4.8 <noreply@anthropic.com>"
```

---

## Task 5: EVKB probe (HARDWARE — manual, mandatory, wiring-free)

Closes the clock-gating + LPSR address-map probe. Needs the physical board (no
jumper); hand off to the operator.

**Files:**
- Create: `evkb/cm4_wire_test/transcript_hw_evkb.txt`
- Modify: `.claude/skills/cm4-bringup/references/cm4-roadmap.md`

- [ ] **Step 1: Flash + clean boot + capture** — run the README's Hardware block. Reader started **before** LinkServer.

- [ ] **Step 2: Verify silicon truth**

```bash
cd ~/Development/rt1170/evkb/cm4_wire_test
tr -d '\000' < /tmp/hw.uart > transcript_hw_evkb.txt
# asserted tokens byte-identical:
diff <(grep -E '^(mcr|ack|nack|rdn|done)=' transcript_qemu.txt) \
     <(grep -E '^(mcr|ack|nack|rdn|done)=' transcript_hw_evkb.txt) \
  && echo "ASSERTED TOKENS BYTE-IDENTICAL"
# the one expected divergence, asserted per-world:
grep -q '^rdv=00000000' transcript_qemu.txt    && echo "QEMU rdv OK (stub)"
grep -q '^rdv=00006243' transcript_hw_evkb.txt && echo "HW rdv OK (device ID)"
grep -q 'WIRE_CM4=PASS' transcript_hw_evkb.txt && echo "HW PASS"
```
Expected: all four echoes. If HW `rdv` is neither `6243` nor `0000`, **silicon
wins**: record the measured value, update README + spec §7, and investigate
before concluding.

- [ ] **Step 3: Update the roadmap** — flip 3.2 to **DONE + ★★HW-VERIFIED
2026-07-<dd>** (top header + the 3.2 entry), append a session-log entry
recording the asserted-identical result + the observed `rdv`, and name 3.3 as
next.

- [ ] **Step 4: Commit the HW result**

```bash
cd ~/Development/rt1170/evkb
git add cm4_wire_test/transcript_hw_evkb.txt .claude/skills/cm4-bringup/references/cm4-roadmap.md
git commit -m "cm4_wire_test: HW-verify CM4 self-configured I2C on the EVKB (Phase 3.2)

Wiring-free clean-boot run: asserted tokens byte-identical to QEMU and
rdv=00006243 -- the real WM8962 answered its device ID over a bus whose
clock, root (mux 1), and LPSR pads the CM4 configured itself. Phase 3.2
complete.

Co-Authored-By: Claude Opus 4.8 <noreply@anthropic.com>"
```

---

## Self-review (completed against the spec)

- **Spec coverage:** §1 goal/transactions → Tasks 1–2 + 5; §2 ground truth → all addresses/values in the Task 2 driver match the verified table; §3 artifacts + token set → Task 1 file set + the 8-value contract; §3.1 rdv split → CM7 excludes v[6] from PASS, runner asserts `00000000`, Task 5 asserts `00006243`; §4 probe (wiring-free, clean_boot) → Task 5; §5 license (fact-only Linux note in the provenance header; GATES same-change) → Task 2 header + Task 3; §6 verification sequence → Tasks 1–5 incl. stability runs + `wire_master_test` regression; §7 risks (STOP-judged ACK, bus_recover, bounded spins, silicon-wins fallback for rdv) → driver + Task 5 Step 2. No gaps.
- **Placeholder scan:** none; the only `<dd>` is the HW run date (operator-filled), not code.
- **Type/name consistency:** token order `mcr,lpcg,croot,ack,nack,rdn,rdv,done` identical in `main_cm4.c` (send order), `cm4_wire_test.cpp` (`labels[8]`, PASS indices 0/3/4/5/7), `run_qemu.sh` (checks), README table. Image symbol `cm4_wire`/header `cm4_wire.h` match `teensy_add_cm4_image(cm4_wire …)`. Register macros match `imxrt1176.h` offsets and the Wire lib's bit defines.
