# CM4 Audio Foundation (Plan 1 of 2) Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Prove SAI1's FIFO interrupt reaches the CM4 NVIC on silicon, model it in qemu2, give the CM4 image world a C++ runtime, and run the AudioStream graph engine on the CM4 — the foundation the audio-pipeline plan (Plan 2: sai1176 shared core, interrupt I/O nodes, CMSIS-DSP-M4, capstone) builds on.

**Architecture:** Four capabilities in dependency order: (1) `cm4_sai_irq_probe` — CM4 self-configures SAI1 TX and counts FIFO-request interrupts on its own NVIC (EVKB is the oracle; QEMU expected-FAIL until (2)); (2) qemu2 fans SAI1 IRQ 76 to both NVICs and models RX-side + level-correct FIFO interrupts; (3) `teensy_add_cm4_image` learns C++ (g++, static ctors, minimal runtime stubs) proven by `cm4_cpp_test`; (4) AudioStream.cpp compiles into a CM4 image against a new Arduino-lite shim, proven by `cm4_audiostream_test` (the audiostream_test graph, CM4 world). Spec: `docs/superpowers/specs/2026-07-21-cm4-audio-ownership-design.md`.

**Tech Stack:** teensy-cmake-macros CM4 image machinery, qemu2 `fsl-imxrt1170` machine (GPL — one-way firewall), gate-lib/qrun, LinkServer.

**Established facts this plan relies on (from triangulation, 2026-07-21):**
- RM Table 4-2 (CM4) line 3760: SAI1 = IRQ 76, same as CM7. This table family lied about fast-GPIO → the probe is mandatory before dependence.
- IRQ 44 is **CAN1 on both cores** (Table 4-1/4-2 line ~3665), NOT a reserved slot. The CM7 audio stack's `IRQ_SOFTWARE=44` works because CAN1 is unused by repo convention (FlexCAN uses CAN3). The CM4 adopts the same convention — documented, not assumed.
- SAI bits (RM §58.5): TCSR/RCSR `FRIE`=bit 8, `FRF`=bit 16 (TX: count ≤ watermark; RX: count > watermark), `FRDE`=bit 0; `TCR1.TFW`/`RCR1.RFW` 5-bit; FIFO 32×32-bit. HW-verified config sequence lives in `cores/imxrt1176/I2S.cpp` (`configureSAI()` lines 44-90: clock root mux 4 div 16, LPCG, pads AD_17/21/22/23 mux 0x10/0, TCR1=16, TCR2=MSEL(1)|BCD|BCP|DIV(7), TCR4 with FCONT bit 28, 16-word prefill before TE|BCE).
- qemu2: SAI model `hw/audio/imxrt_sai.c` — `imxrt_sai_update()` (lines 128-146) asserts `s->irq` only for TX (`tx_on && (tcsr & (FRIE|FWIE))`), ignores RX and FIFO level; machine `hw/arm/fsl-imxrt1170.c` wires SAI IRQs CM7-only (line 1227); `fsl_imxrt1170_connect_irq_both` (lines 795-802) currently fans LPI2C5 (1152), LPI2C2 (1162), LPSPI1 (1188).
- CM4 images: bare-metal C, explicit SOURCES, flags at `teensy-cmake-macros/CMakeLists.include.txt:516-518` (`-mcpu=cortex-m4 -mfpu=fpv4-sp-d16 -mfloat-abi=hard -ffreestanding …`), gcc at line 513, `-nostdlib -Wl,--gc-sections` link at 561. `.rodata` → ITCM (128K). No C++ today. CM4 reports via MU (TR ch0), CM7 prints (`cm4_wire_test` pattern). CM4 uses a static vector table in `cm4/startup_cm4.S` + raw `NVIC_ISER*` writes — no attachInterruptVector.
- `AudioStream.cpp`: `update_setup()` at `cores/imxrt1176/AudioStream.cpp:298` (attachInterruptVector + NVIC), `software_isr` at 316 (uses `ARM_DWT_CYCCNT` — Cortex-M4 has DWT CYCCNT too), `IRQ_SOFTWARE` is `#ifndef`-guarded at `AudioStream.h:46-47`.

**Repos touched:** evkb (gates, cores shim, macros), qemu2 (model). NO Audio-fork changes in this plan.

---

### Task 1: `cm4_sai_irq_probe` — the phase-gating silicon probe

The CM4 self-configures SAI1 TX (distilled from the HW-verified `I2S.cpp` sequence, as literals per the CM4 gate convention), enables the FIFO-request interrupt, and counts ISR entries. EVKB is the oracle; in QEMU this gate is EXPECTED-FAIL until Task 2 (IRQ not fanned) — the runner asserts the failure mode explicitly so the expectation is recorded, then Task 2 flips it.

**Files:**
- Create: `examples/dualcore/cm4_sai_irq_probe/CMakeLists.txt`
- Create: `examples/dualcore/cm4_sai_irq_probe/cm4_sai_irq_probe.cpp` (CM7 side)
- Create: `examples/dualcore/cm4_sai_irq_probe/cm4/startup_cm4.S` (copy from `examples/dualcore/cm4_wire_test/cm4/startup_cm4.S`, one edit: add `SAI1_IRQHandler` at vector index 16+76 — follow the file's existing `LPI2C5_IRQHandler`-at-index-52 pattern, weak-default style identical)
- Create: `examples/dualcore/cm4_sai_irq_probe/cm4/main_cm4.c`
- Create: `examples/dualcore/cm4_sai_irq_probe/cm4/cm4.ld` (copy from `cm4_wire_test/cm4/cm4.ld`, unchanged)
- Create: `examples/dualcore/cm4_sai_irq_probe/run_qemu.sh` (755)
- Create: `examples/dualcore/cm4_sai_irq_probe/toolchain/rt1170-evkb.toolchain.cmake` (copy from `cm4_wire_test/toolchain/`)

- [ ] **Step 1: Copy the scaffolding**

```bash
cd ~/Development/rt1170/evkb/examples/dualcore
mkdir -p cm4_sai_irq_probe/cm4 cm4_sai_irq_probe/toolchain
cp cm4_wire_test/toolchain/rt1170-evkb.toolchain.cmake cm4_sai_irq_probe/toolchain/
cp cm4_wire_test/cm4/cm4.ld cm4_sai_irq_probe/cm4/
cp cm4_wire_test/cm4/startup_cm4.S cm4_sai_irq_probe/cm4/
```

- [ ] **Step 2: Add the SAI1 vector to `cm4/startup_cm4.S`**

Open the copied `startup_cm4.S`, find how `LPI2C5_IRQHandler` is placed at vector index 52 (16+36) and declared (`.weak` + default-handler alias). Add `SAI1_IRQHandler` at vector index 92 (16+76) in exactly the same style. Do not restructure anything else. (The vector table is position-indexed — count carefully or use the file's existing index comments.)

- [ ] **Step 3: Write `cm4/main_cm4.c`**

```c
// cm4_sai_irq_probe: does SAI1's FIFO-request interrupt (IRQ 76 per RM Table
// 4-2 line 3760) reach the CM4 NVIC on real silicon? The fast-GPIO precedent
// says this table cannot be trusted without a probe.
//
// The CM4 self-configures SAI1 TX exactly as the HW-verified CM7 sequence
// does (cores/imxrt1176/I2S.cpp configureSAI(), distilled to literals per the
// CM4 gate convention), but enables FRIE (bit 8, FIFO-request INTERRUPT)
// instead of FRDE (bit 0, DMA). TX watermark 16 on a 32-deep FIFO: FRF stays
// asserted whenever count <= 16, so with the FIFO drained the IRQ fires
// immediately on enable and re-fires as the shift clock drains words.
// Observations stream to the CM7 over the MU (cm4_wire_test pattern).
#include <stdint.h>

#define REG32(a) (*(volatile uint32_t *)(a))

// --- MU B side (CM4), TR channel 0 --- (cm4_wire_test/cm4/main_cm4.c pattern)
#define MUB_BASE   0x40C4C000u
#define MUB_TR0    REG32(MUB_BASE + 0x20u)
#define MUB_SR     REG32(MUB_BASE + 0x60u)
#define MU_SR_TE0  (1u << 23)

static void mu_send(uint32_t v) {
    while (!(MUB_SR & MU_SR_TE0)) {}
    MUB_TR0 = v;
}

// --- SAI1 @ 0x40404000 (RM 58.5) ---
#define SAI1_BASE  0x40404000u
#define SAI1_TCSR  REG32(SAI1_BASE + 0x00u)
#define SAI1_TCR1  REG32(SAI1_BASE + 0x04u)
#define SAI1_TCR2  REG32(SAI1_BASE + 0x08u)
#define SAI1_TCR3  REG32(SAI1_BASE + 0x0Cu)
#define SAI1_TCR4  REG32(SAI1_BASE + 0x10u)
#define SAI1_TCR5  REG32(SAI1_BASE + 0x14u)
#define SAI1_TDR0  REG32(SAI1_BASE + 0x20u)
#define SAI1_TMR   REG32(SAI1_BASE + 0x60u)
#define TCSR_TE    (1u << 31)
#define TCSR_BCE   (1u << 28)
#define TCSR_SR    (1u << 24)
#define TCSR_FR    (1u << 25)
#define TCSR_FRF   (1u << 16)
#define TCSR_FRIE  (1u << 8)

// --- clocking/pins: literals from cores/imxrt1176/I2S.cpp configureSAI() ---
#define CCM_CLOCK_ROOT64_CONTROL REG32(0x40CC0000u + 0x80u * 64u) // sai1 root
#define CCM_LPCG123_DIRECT       REG32(0x40CC0000u + 0x6000u + 0x20u * 123u)
#define IOMUXC_GPR_GPR0          REG32(0x400E4000u + 0x00u)
#define PADMUX_AD_17             REG32(0x400E8000u + 0x10u + 4u * 17u) // MCLK
#define PADMUX_AD_21             REG32(0x400E8000u + 0x10u + 4u * 21u) // TX_DATA00
#define PADMUX_AD_22             REG32(0x400E8000u + 0x10u + 4u * 22u) // TX_BCLK
#define PADMUX_AD_23             REG32(0x400E8000u + 0x10u + 4u * 23u) // TX_SYNC

#define NVIC_ISER2 REG32(0xE000E108u)          // IRQs 64..95; SAI1 = 76

volatile uint32_t sai_irq_count = 0;

void SAI1_IRQHandler(void) {
    sai_irq_count++;
    // Feed one word per entry so FRF eventually clears if the FIFO fills;
    // after 64 entries stop feeding and disable FRIE so the probe terminates.
    if (sai_irq_count < 64u) SAI1_TDR0 = 0u;
    else SAI1_TCSR &= ~TCSR_FRIE;
}

void cm4_main(void) {
    // NOTE: the Audio PLL is expected to be OFF in this probe (nobody set it
    // up); the SAI bit clock then free-runs from whatever the root mux
    // provides. That is fine: the probe only needs FRF && FRIE -> NVIC entry,
    // which is FIFO-level logic, not clock-quality logic.
    CCM_CLOCK_ROOT64_CONTROL = (4u << 8) | (15u << 0);   // mux 4, div 16
    CCM_LPCG123_DIRECT = 1u;                              // ungate SAI1
    PADMUX_AD_17 = 0x10u; PADMUX_AD_22 = 0x10u; PADMUX_AD_23 = 0x10u;
    PADMUX_AD_21 = 0x0u;
    IOMUXC_GPR_GPR0 |= (1u << 8);                         // SAI1_MCLK_DIR

    SAI1_TCSR = TCSR_SR; SAI1_TCSR = 0u;
    SAI1_TCSR = TCSR_FR; SAI1_TCSR = 0u;
    SAI1_TMR  = 0u;
    SAI1_TCR1 = 16u;                                      // watermark 16
    SAI1_TCR2 = (1u << 26) | (1u << 24) | (1u << 25) | 7u; // MSEL(1)|BCD|BCP|DIV(7)
    SAI1_TCR3 = (1u << 16);                               // TCE
    SAI1_TCR4 = (1u << 16) | (15u << 8) | (1u << 4) | (1u << 3) | (1u << 1) | 1u
              | (1u << 28);                               // FRSZ|SYWD|MF|FSD|FSE|FSP|FCONT
    SAI1_TCR5 = (15u << 24) | (15u << 16) | (15u << 8);   // WNW|W0W|FBT

    mu_send(0xCAFE0001u);                 // ready marker
    NVIC_ISER2 = (1u << (76u - 64u));     // enable IRQ 76 on the CM4 NVIC
    SAI1_TCSR = TCSR_TE | TCSR_BCE | TCSR_FRIE;  // empty FIFO: FRF=1 now

    // Let interrupts accumulate; a bounded spin, then report.
    for (volatile uint32_t i = 0; i < 2000000u; i++) {}
    mu_send(sai_irq_count);               // the probe's answer
    mu_send((SAI1_TCSR >> 16) & 0x7u);    // FRF/FWF/FEF snapshot
    mu_send(0xD0DE0001u);                 // done marker
    for (;;) { __asm volatile ("wfi"); }
}
```

**Register-literal check is part of this step:** before building, verify every literal above against `cores/imxrt1176/I2S.cpp` lines 44-80 and `imxrt1176.h`'s definitions (CCM root 64 offset, LPCG 123 offset, IOMUXC pad offsets for AD_17/21/22/23, GPR0 bit). The values shown were derived from the I2S.cpp readback but MUST be cross-checked against the actual header macro expansions — a wrong pad offset silently breaks the probe. If any literal differs, fix the literal and note it in the commit message.

- [ ] **Step 4: Write the CM7 side `cm4_sai_irq_probe.cpp`**

```cpp
#include "Arduino.h"
#include "HardwareSerial.h"
#include "Multicore.h"
#include "MessagingUnit.h"
#include "cm4_sai_probe.h"   // generated by teensy_add_cm4_image(cm4_sai_probe ...)

// CM7 role: boot the CM4 probe, relay its MU observations to LPUART1.
// The CM7 does NOT touch SAI1 (single-owner rule: this probe's owner is
// the CM4).

static bool wait_recv(int ch, uint32_t &out) {
    for (uint32_t i = 0; i < 3000000u; i++) {
        if (MU.tryReceive(ch, out)) return true;
    }
    return false;
}
static void phex(const char *k, uint32_t v) {
    Serial1.print(k); Serial1.print("=");
    for (int s = 28; s >= 0; s -= 4) Serial1.print("0123456789ABCDEF"[(v >> s) & 0xF]);
    Serial1.println();
}

void setup() {
    Serial1.begin(115200);
    while (!Serial1) {}
    Serial1.println("CM4SAIIRQ-GATE v1");
    MU.begin();
    Multicore.begin(cm4_sai_probe, sizeof(cm4_sai_probe));

    uint32_t ready = 0, irqcnt = 0, flags = 0, done = 0;
    bool ok = wait_recv(0, ready) && wait_recv(0, irqcnt)
           && wait_recv(0, flags) && wait_recv(0, done);
    phex("ready",  ready);
    phex("irqcnt", irqcnt);
    phex("flags",  flags);
    phex("done",   done);
    bool pass = ok && ready == 0xCAFE0001u && done == 0xD0DE0001u && irqcnt > 0u;
    Serial1.println(pass ? "SAI_IRQ_CM4=PASS" : "SAI_IRQ_CM4=FAIL");
    Serial1.println("CM4SAIIRQ-DONE");
}
void loop() {}
```

(Image header/symbol naming: `teensy_add_cm4_image(cm4_sai_probe ...)` emits `cm4_sai_probe.h` defining `static const uint32_t cm4_sai_probe[]` — verify against how `cm4_wire_test.cpp` includes its `cm4_wire` image before building; if the emitted convention differs, follow it.)

- [ ] **Step 5: Write `CMakeLists.txt`**

```cmake
cmake_minimum_required(VERSION 3.24)
project(cm4_sai_irq_probe)

set(TEENSY_VERSION 117 CACHE STRING "")

include(${CMAKE_CURRENT_LIST_DIR}/../../../evkb.cmake)

teensy_add_cm4_image(cm4_sai_probe
    LINKER  ${CMAKE_CURRENT_LIST_DIR}/cm4/cm4.ld
    SOURCES ${CMAKE_CURRENT_LIST_DIR}/cm4/startup_cm4.S
            ${CMAKE_CURRENT_LIST_DIR}/cm4/main_cm4.c)

teensy_add_executable(cm4_sai_irq_probe cm4_sai_irq_probe.cpp)
teensy_target_link_libraries(cm4_sai_irq_probe cores)
target_link_libraries(cm4_sai_irq_probe.elf stdc++)
teensy_target_link_cm4_image(cm4_sai_irq_probe cm4_sai_probe)
```

(Adjust the `#include "cm4_sai_irq_probe_img.h"` in the .cpp to the actual generated header name `cm4_sai_probe.h` and array symbol `cm4_sai_probe` — check `cm4_wire_test` for the exact convention and make the .cpp match.)

- [ ] **Step 6: Write `run_qemu.sh`** — EXPECTED-FAIL variant (red until Task 2)

```sh
#!/bin/sh
set -e
QEMU=~/Development/rt1170/evkb/tools/qrun
DIR=$(cd "$(dirname "$0")" && pwd)
. ~/Development/rt1170/evkb/tools/gate-lib.sh
gate_init
ELF="$DIR/build/cm4_sai_irq_probe.elf"; OUT="$DIR/cm4_sai_irq.uart"
rm -f "$OUT"
"$QEMU" -M mimxrt1170-evk -global fsl-imxrt1170.boot-xip=on -kernel "$ELF" \
    -display none -serial file:"$OUT" -d guest_errors -D "$DIR/cm4_sai_irq.dbg" &
P=$!; gate_pid $P; sleep 8; kill $P 2>/dev/null; wait $P 2>/dev/null || true
echo "==== captured ===="; cat "$OUT"
grep -q "CM4SAIIRQ-DONE" "$OUT" || { echo "FAIL: no done"; exit 1; }
grep -q "SAI_IRQ_CM4=PASS" "$OUT" || { echo "FAIL: no CM4 SAI IRQ"; exit 1; }
echo "PASS: SAI_IRQ_CM4"
```

Then `chmod +x run_qemu.sh`.

- [ ] **Step 7: Build; run in QEMU expecting FAIL; record the failure mode**

```bash
cd ~/Development/rt1170/evkb/examples/dualcore/cm4_sai_irq_probe
cmake -B build -DCMAKE_TOOLCHAIN_FILE=toolchain/rt1170-evkb.toolchain.cmake && cmake --build build
./run_qemu.sh
```

Expected TODAY: `irqcnt=00000000` → `SAI_IRQ_CM4=FAIL` → runner exits 1 (qemu2 wires SAI1 IRQ to the CM7 NVIC only, `fsl-imxrt1170.c:1227`). Record the exact output. If it PASSES in QEMU today, STOP — the model's wiring differs from the triangulated fact; report.

- [ ] **Step 8: HW probe on the EVKB** (board rules: pkill daemons; `tools/rt1170-console.py` reader on `/dev/cu.usbmodem5DQ2DDHVWO5EI3` BEFORE `LinkServer run`; never `cat`)

```bash
pkill LinkServer; pkill redlinkserv; sleep 1
cd ~/Development/rt1170/evkb/examples/dualcore/cm4_sai_irq_probe
python3 ~/Development/rt1170/evkb/tools/rt1170-console.py /dev/cu.usbmodem5DQ2DDHVWO5EI3 115200 > transcript_hw_evkb.txt 2>&1 &
sleep 1
/Applications/LinkServer_26.6.137/LinkServer run MIMXRT1176:MIMXRT1170-EVKB build/cm4_sai_irq_probe.elf
sleep 8
pkill -f rt1170-console.py; pkill LinkServer; pkill redlinkserv
grep "SAI_IRQ_CM4" transcript_hw_evkb.txt
```

**This is the phase gate.** `SAI_IRQ_CM4=PASS` with `irqcnt>0` → proceed. FAIL with everything else sane (`ready`/`done` markers present) → the RM table lied again: STOP the plan, report the transcript verbatim — the coordinator pivots to the polled-fallback design (spec §1). Do not improvise a workaround.

- [ ] **Step 9: Commit** (gate + both transcripts)

```bash
cd ~/Development/rt1170/evkb
git add examples/dualcore/cm4_sai_irq_probe
git commit -m "probe: SAI1 FIFO-request IRQ reaches the CM4 NVIC (RM Table 4-2 confirmed on EVKB; QEMU expected-FAIL until the model fans IRQ 76)"
```

End the commit message body with: `Co-Authored-By: Claude Fable 5 <noreply@anthropic.com>`

---

### Task 2: qemu2 — fan SAI IRQ 76 to both NVICs + honest FIFO-interrupt model

**Files (qemu2 repo, `~/Development/qemu2` — GPL, one-way firewall):**
- Modify: `include/hw/arm/fsl-imxrt1170.h` (SplitIRQ member for sai1)
- Modify: `hw/arm/fsl-imxrt1170.c` (~line 1227 SAI wiring; SplitIRQ init near the lpi2c5 pattern at 1152/1099-1109)
- Modify: `hw/audio/imxrt_sai.c` (`imxrt_sai_update`, lines 128-146)

- [ ] **Step 1: Add the split-IRQ fan for SAI1** — mirror the LPI2C5 pattern exactly: a `SplitIRQ sai1_irq_split` member in the state struct, `num-lines=2` init alongside the existing splits (fsl-imxrt1170.c:1099-1109 region), and at the SAI wiring loop (line 1227) route instance 0 (SAI1) through `fsl_imxrt1170_connect_irq_both(&s->sai1_irq_split, armv7m, armv7m_m4, sbd, 0, 76)`; SAI2-4 keep their CM7-only wiring (out of scope).

- [ ] **Step 2: Model RX FIFO interrupts + level-correct FRF in `imxrt_sai_update()`** — extend the IRQ expression from `tx_on && (tcsr & (FRIE|FWIE))` to also OR the RX term `rx_on && (rcsr & (FRIE|FWIE))`, and gate the FRIE term on the modeled FIFO level vs watermark where the model tracks a level (if the model synthesizes FRF only on register read — lines 187-194 — keep the level semantics as close as the existing ring state allows and document the fidelity limit in a comment citing this probe/date). Do NOT overbuild a cycle-accurate FIFO; the gates need: FRIE+empty-TX-FIFO ⇒ IRQ asserted, feeding past watermark ⇒ deasserts, RX mirror.

- [ ] **Step 3: Rebuild qemu2 and flip the probe gate**

```bash
cd ~/Development/qemu2/build && ninja qemu-system-arm 2>&1 | tail -3
cd ~/Development/rt1170/evkb/examples/dualcore/cm4_sai_irq_probe && ./run_qemu.sh
```

Expected: NOW `SAI_IRQ_CM4=PASS` in QEMU (irqcnt > 0). Compare `irqcnt` with the HW transcript — exact equality is NOT expected (model pacing differs); the gate asserts >0, and any divergence beyond that is documented in the gate README, not absorbed.

- [ ] **Step 4: qemu2 regression set + checkpatch** — run the regression list from `evkb/.claude/skills/cm4-bringup/references/silicon-truth-loop.md` (all dualcore gates + the audio gates at minimum: cm4_wire_test, cm4_wire_int_master_test, cm4_wire_dma_test, dualcore_mu_test, audiostream_test, filter_fir_test, guard_sweep_test, i2s_audio_test, sai_rx_test, audioinput/audiooutput_i2s tests) — ALL must stay green; then `scripts/checkpatch.pl` on the qemu2 diff.

- [ ] **Step 5: Commit qemu2** — commit message cites the probe: `hw/arm/fsl-imxrt1170: fan SAI1 IRQ 76 to both NVICs; imxrt_sai: RX FIFO-request IRQs (EVKB probe cm4_sai_irq_probe 2026-07-21: CM4 NVIC receives SAI1 FRF interrupts on silicon)`. Do not push qemu2 unless its repo convention says so (check `git -C ~/Development/qemu2 status -sb` remote setup and report).

---

### Task 3: CM4 C++ runtime — `teensy_add_cm4_image` learns C++, proven by `cm4_cpp_test`

**Files:**
- Modify: `teensy-cmake-macros/CMakeLists.include.txt` (~lines 503-571)
- Modify: `teensy-cmake-macros/cm4_slot.ld.in` + the per-example `cm4.ld` used by the new gate (init_array section)
- Create: `examples/dualcore/cm4_cpp_test/` (full gate: CMakeLists, CM7 .cpp, cm4/{startup_cm4.S,main_cm4.cpp,runtime_stubs.c,cm4.ld}, run_qemu.sh, toolchain copy)

- [ ] **Step 1: Macro change** — in `teensy_add_cm4_image`'s per-source compile loop (line ~513): select the compiler by extension (`.cpp`/`.cc` → `arm-none-eabi-g++`, else gcc as today), and give C++ sources the extra flags `-fno-exceptions -fno-rtti -fno-threadsafe-statics -fno-use-cxa-atexit` appended to the existing `_cm4_flags`. C sources' command lines must remain byte-identical to today (the reproducible-.bin property): only .cpp sources get the new path.

- [ ] **Step 2: Linker + startup support for static constructors** — in the gate's `cm4.ld` (copied) add, inside the ITCM `.text` output section after `*(.rodata*)`:

```ld
    . = ALIGN(4);
    __init_array_start = .;
    KEEP(*(.init_array*))
    __init_array_end = .;
```

Apply the same three lines to `teensy-cmake-macros/cm4_slot.ld.in` (slot images get ctors too). In the gate's `cm4/main_cm4.cpp` world, run them before `cm4_main` logic via a small C function in `runtime_stubs.c`:

```c
// Minimal C++ runtime for the freestanding CM4 image world (-nostdlib).
#include <stdint.h>
typedef void (*init_fn)(void);
extern init_fn __init_array_start[], __init_array_end[];
void cm4_run_ctors(void) {
    for (init_fn *f = __init_array_start; f < __init_array_end; f++) (*f)();
}
void __cxa_pure_virtual(void) { for (;;) {} }
// gcc may emit calls to these even in freestanding C++:
void *memset(void *d, int c, unsigned long n) {
    unsigned char *p = d; while (n--) *p++ = (unsigned char)c; return d;
}
void *memcpy(void *d, const void *s, unsigned long n) {
    unsigned char *p = d; const unsigned char *q = s;
    while (n--) *p++ = *q++; return d;
}
```

(If the existing `startup_cm4.S` already zeroes .bss and copies .data — it does — the ctor call is made the FIRST thing in the C++ `cm4_main`; do not modify startup_cm4.S beyond the vector additions.)

- [ ] **Step 3: `cm4_cpp_test` gate** — CM4 image compiled from `main_cm4.cpp` proving, via MU-reported values: (a) a static object's constructor ran before main (ctor sets a magic field → report it), (b) virtual dispatch works (base pointer → derived override returns a distinct constant), (c) memset/memcpy link. CM7 side prints and asserts `ctor=CAFEC201`, `virt=CAFEC202`, `CPP_CM4=PASS`. Full sources follow the cm4_wire_test/Task-1 shapes (MU send/recv helpers identical); keep the CM4 image under ten small files.

Example CM4 `main_cm4.cpp` core (complete):

```cpp
#include <stdint.h>
extern "C" {
#include "mu_report.h"   // mu_send(), as in Task 1 (factor the 12 lines into cm4/mu_report.h)
void cm4_run_ctors(void);
}

struct Base { virtual uint32_t id() const { return 0xDEAD0000u; } virtual ~Base() {} };
struct Derived : Base { uint32_t id() const override { return 0xCAFEC202u; } };

struct CtorProof { uint32_t magic; CtorProof() : magic(0xCAFEC201u) {} };
static CtorProof g_proof;
static Derived  g_derived;
static Base    *g_base = &g_derived;

extern "C" void cm4_main(void) {
    cm4_run_ctors();
    mu_send(g_proof.magic);      // 0xCAFEC201 iff the ctor ran
    mu_send(g_base->id());       // 0xCAFEC202 iff vtables work
    mu_send(0xD0DE0002u);
    for (;;) { __asm volatile ("wfi"); }
}
```

- [ ] **Step 4: Build + QEMU green + regression** — the gate passes in QEMU (pure CPU); ALSO rebuild one existing C-only CM4 gate (`cm4_wire_test`) and re-run it to prove the macro change didn't disturb C image builds (byte-identical `.cm4.bin` if possible — compare checksums before/after the macro change).

- [ ] **Step 5: HW run + commit** (same board recipe; transcripts committed; commit macros + gate together: `feat: CM4 image world learns C++ (g++ path, init_array, freestanding stubs) + cm4_cpp_test gate`).

---

### Task 4: AudioStream on the CM4 — Arduino-lite shim + `cm4_audiostream_test`

**Files:**
- Create: `cores/imxrt1176/cm4_shim/Arduino.h` (the Arduino-lite subset for CM4 images)
- Create: `cores/imxrt1176/cm4_shim/README.md` (what the shim is, what belongs in it, what must never go in it)
- Create: `examples/dualcore/cm4_audiostream_test/` (full gate)
- NO edits to `AudioStream.{h,cpp}` unless compilation forces them — any forced edit is a reported deviation.

- [ ] **Step 1: The shim** — `cores/imxrt1176/cm4_shim/Arduino.h` provides ONLY what `AudioStream.{h,cpp}` and synthetic test nodes need, for the CM4 world:

```c
// Arduino-lite for the freestanding CM4 image world. This is NOT the CM7
// Arduino.h: it provides the minimal surface AudioStream + gate nodes need.
// Grow it deliberately; anything with hardware behind it must come from a
// shared-core library or a probe-backed sequence, never from CM7 headers.
#pragma once
#include <stdint.h>
#include <stddef.h>
#include <string.h>   // freestanding prototypes; impls from runtime_stubs.c

#define EVKB_CM4_WORLD 1

// --- interrupt control (CM4 = same ARMv7-M primitives) ---
static inline void __disable_irq(void) { __asm volatile ("cpsid i" ::: "memory"); }
static inline void __enable_irq(void)  { __asm volatile ("cpsie i" ::: "memory"); }

// --- NVIC, CM4 world: raw registers, static vector table (no RAM vectors) ---
typedef int IRQ_NUMBER_t;
#define NVIC_ISER_BASE 0xE000E100u
#define NVIC_ICER_BASE 0xE000E180u
#define NVIC_ISPR_BASE 0xE000E200u
#define NVIC_IPRI_BASE 0xE000E400u
static inline void NVIC_ENABLE_IRQ(int n)  { *(volatile uint32_t *)(NVIC_ISER_BASE + 4u*((uint32_t)n >> 5)) = 1u << (n & 31); }
static inline void NVIC_DISABLE_IRQ(int n) { *(volatile uint32_t *)(NVIC_ICER_BASE + 4u*((uint32_t)n >> 5)) = 1u << (n & 31); }
static inline void NVIC_SET_PENDING(int n) { *(volatile uint32_t *)(NVIC_ISPR_BASE + 4u*((uint32_t)n >> 5)) = 1u << (n & 31); }
static inline void NVIC_SET_PRIORITY(int n, int p) { *(volatile uint8_t *)(NVIC_IPRI_BASE + (uint32_t)n) = (uint8_t)p; }

// The CM4 vector table is static (startup_cm4.S). attachInterruptVector is a
// no-op here: the handler symbol must already be in the table. AudioStream's
// software_isr is aliased to the table slot by the gate's startup file.
typedef void (*voidFuncPtr)(void);
static inline void attachInterruptVector(IRQ_NUMBER_t irq, voidFuncPtr f) { (void)irq; (void)f; }

// --- DWT cycle counter (Cortex-M4 has DWT/CYCCNT; enable in gate init) ---
#define ARM_DWT_CYCCNT (*(volatile uint32_t *)0xE0001004u)

// IRQ_SOFTWARE: same repurposed-CAN1 slot 44 as the CM7 world (CAN1 unused by
// repo convention on BOTH cores — FlexCAN work uses CAN3). AudioStream.h's
// #ifndef guard picks this up.
#define IRQ_SOFTWARE ((IRQ_NUMBER_t)44)
```

**The attachInterruptVector-as-no-op decision is load-bearing and must be verified**: `update_setup()` calls it then enables the NVIC line — on the CM4 the vector must be static. The gate's `startup_cm4.S` therefore places `software_isr` (the symbol from AudioStream.cpp, C++-mangled? — NO: it's declared as a free function; check its linkage in AudioStream.cpp line 54/316; if it has C++ linkage add an `extern "C"` wrapper in the gate) at vector index 60 (16+44). If `software_isr`'s symbol can't be placed directly, the gate adds a thin `extern "C" void Software_IRQHandler(void) { software_isr(); }` — decide by reading the actual linkage, document in the gate.

- [ ] **Step 2: The gate** — `cm4_audiostream_test`: CM4 image compiles `cores/imxrt1176/AudioStream.cpp` + the shim + a TestSource/TestSink pair (ported from `examples/audio/audiostream_test/audiostream_test.cpp`'s classes, MU-reporting instead of Serial): pend IRQ_SOFTWARE 8 times (the audiostream_test pump), assert blocks flow in order and the pool drains to zero, stream `{flow, noleak, received, mem_max}` over the MU; CM7 prints + asserts `AUDIOSTREAM_CM4=PASS`. CMakeLists: the CM4 image's SOURCES list includes `${EVKB_CORES_DIR}/AudioStream.cpp` with `INCLUDE_DIRS ${CMAKE_CURRENT_LIST_DIR}/cm4 ${EVKB_CORES_DIR}/cm4_shim ${EVKB_CORES_DIR}` — shim dir BEFORE the core dir so `#include "Arduino.h"` resolves to the shim while `#include "AudioStream.h"` resolves to the real engine header. AudioMemory pool: `AudioStream::initialize_memory` with a static block array in the CM4 image (mirror the AudioMemory macro's expansion by hand — read `AudioStream.h`'s macro and instantiate its two statics directly in main_cm4.cpp).

- [ ] **Step 3: Build.** Expected wrinkles to resolve by reading (not guessing): `AudioStream.h` may include CM7-only headers beyond Arduino.h — if so, extend the shim minimally (each addition documented in the shim README) rather than editing AudioStream.h; report every shim addition. If AudioStream.cpp itself cannot compile without modification, STOP and report the exact error — an `#ifdef EVKB_CM4_WORLD` in core code is a design decision the coordinator must approve.

- [ ] **Step 4: QEMU green + HW green** (both worlds' transcripts committed; the graph is pure CPU — expect QEMU==HW token-identical).

- [ ] **Step 5: Commit** — shim + gate: `feat: AudioStream graph engine runs on the CM4 (Arduino-lite shim, IRQ 44 convention) + cm4_audiostream_test`.

---

### Task 5: License audit, roadmap, docs, push

- [ ] **Step 1:** Add the three new gates to `tools/license-audit.sh` GATES (`examples/dualcore/cm4_sai_irq_probe:cm4_sai_irq_probe examples/dualcore/cm4_cpp_test:cm4_cpp_test examples/dualcore/cm4_audiostream_test:cm4_audiostream_test`); run → `LICENSE-AUDIT: PASS`.
- [ ] **Step 2:** Update `evkb/.claude/skills/cm4-bringup/references/cm4-roadmap.md`: new phase entry (CM4 audio foundation) with the probe result, the IRQ-44-is-CAN1 convention note, C++-world capability, and Plan 2's queued scope. Append the dated session-log entry.
- [ ] **Step 3:** Update `examples/README.md` dualcore row (+3 gates). The Audio status doc is NOT touched in this plan (no Audio-fork change; Plan 2 owns that).
- [ ] **Step 4:** Commit + push evkb. qemu2 push per its convention (report status either way).

---

## Verification (whole plan)

1. `cm4_sai_irq_probe`: HW transcript with `irqcnt>0` committed; QEMU red-then-green across Task 2 (both recorded).
2. qemu2 regression set green after the model change; checkpatch clean.
3. `cm4_cpp_test` + `cm4_audiostream_test` green in QEMU and on the EVKB, transcripts committed.
4. C-only CM4 images unchanged (cm4_wire_test re-run green; .cm4.bin checksum comparison reported).
5. license-audit PASS; roadmap + examples README updated; everything pushed.

## Explicitly deferred to Plan 2

sai1176 shared core, AudioOutputI2SInt/AudioInputI2SInt, WM8962-from-CM4 via the Audio control node, CMSIS-DSP-CM4 target, the both-directions capstone `cm4_audio_test`, CM7-idle assertion pattern, ownership-exclusivity CMake check, Audio status-doc updates.
