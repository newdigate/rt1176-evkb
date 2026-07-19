# CM4 Phase 4.2 — Interrupt Wire-slave Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** The CM4 runs an interrupt-driven I2C slave (distilled from the HW-verified `TwoWire` slave), addressed by a master — the CM7 via the QEMU LPI2C2-persona bridge in the gate, an external I2C master on the Arduino header (A4=SDA/A5=SCL) on real silicon.

**Architecture:** World-split instance binding per the amended spec §4.2 (`evkb/docs/superpowers/specs/2026-07-18-cm4-interrupt-dma-spi-wire-design.md`): one instance-agnostic CM4 slave, bound at build time to LPI2C2 (base `0x40108000`, IRQ 33) for the QEMU gate and LPI2C1 (base `0x40104000`, IRQ 32) for the HW probe. The slave init sequence moves into the shared C core (`~/Development/Wire/lpi2c1176.{h,c}`, MIT) as `lpi2c1176_slave_config`/`lpi2c1176_slave_enable`, and `TwoWire::begin(uint8_t)` migrates onto it (Phase 3.3 sequences-live-once doctrine, guarded by the existing `wire_slave_test` QEMU gate transcript). qemu2 gains exactly one new split: `lpi2c2_irq_split` (IRQ 33 → both NVICs).

**Protocol constants (identical in both worlds — 4.1 lesson: assert every value on the side that can see it):** slave address `0x42`; master writes `{0xA5, 0x5A, 0xC3}`; slave responds `0x3C` to a 1-byte read.

**Tech stack:** bare-metal C (CM4), teensy-cmake-macros dual-target build, qemu2 `mimxrt1170-evk`, gate-lib.sh, LinkServer + pyserial for the EVKB probe.

**Repos touched:** `~/Development/Wire` (its own git repo), `~/Development/qemu2` (GPL — one-way firewall: nothing flows back to firmware), `~/Development/rt1170/evkb` (`git -C evkb`; rt1170 itself is NOT a repo).

---

### Task 1: Shared-core slave block (Wire repo) + `TwoWire` migration

**Files:**
- Modify: `~/Development/Wire/lpi2c1176.h`
- Modify: `~/Development/Wire/lpi2c1176.c`
- Modify: `~/Development/Wire/WireIMXRT1176.cpp`
- Guardrail test: `~/Development/Wire/tests/wire_slave_test/run_qemu_wire_slave.sh` (exists)

- [ ] **Step 1: Green baseline — run the existing guardrail gate before touching anything**

Run: `cd ~/Development/Wire/tests/wire_slave_test && ./run_qemu_wire_slave.sh`
Expected: PASS. Save the UART transcript for the byte-identity diff:

```bash
cp ~/Development/Wire/tests/wire_slave_test/wire_slave.uart /private/tmp/claude-501/-Users-nicholasnewdigate-Development-rt1170/b11174ea-d6b3-49f4-bd1e-69803d4f2afe/scratchpad/wire_slave_baseline.uart
```

- [ ] **Step 2: Extend `lpi2c1176.h` — regs struct to the slave block + defines + API**

In `lpi2c1176.h`, replace the last member of `lpi2c1176_regs_t`:

```c
	volatile uint32_t MRDR;         /* 0x70 */
} lpi2c1176_regs_t;
```

with (offsets per RM ch.69 LPI2C memory map, lines ~306018-306037 of `rm_full.txt`; matches the HW-verified `IMXRT_LPI2C_t` slave block):

```c
	volatile uint32_t MRDR;         /* 0x70 */
	volatile uint32_t r74[39];      /* 0x74..0x10C */
	volatile uint32_t SCR;          /* 0x110 slave control */
	volatile uint32_t SSR;          /* 0x114 slave status */
	volatile uint32_t SIER;         /* 0x118 slave interrupt enable */
	volatile uint32_t SDER;         /* 0x11C slave DMA enable */
	volatile uint32_t r120;         /* 0x120 */
	volatile uint32_t SCFGR1;       /* 0x124 slave config 1 */
	volatile uint32_t SCFGR2;       /* 0x128 slave config 2 */
	volatile uint32_t r12C[5];      /* 0x12C..0x13C */
	volatile uint32_t SAMR;         /* 0x140 slave address match */
	volatile uint32_t r144[3];      /* 0x144..0x14C */
	volatile uint32_t SASR;         /* 0x150 slave address status (read clears AVF) */
	volatile uint32_t STAR;         /* 0x154 slave transmit ACK */
	volatile uint32_t r158[2];      /* 0x158..0x15C */
	volatile uint32_t STDR;         /* 0x160 slave transmit data */
	volatile uint32_t r164[3];      /* 0x164..0x16C */
	volatile uint32_t SRDR;         /* 0x170 slave receive data */
} lpi2c1176_regs_t;
```

After the existing `LPI2C1176_ASSERT` block add:

```c
LPI2C1176_ASSERT(offsetof(lpi2c1176_regs_t, SCR)    == 0x110, "LPI2C SCR");
LPI2C1176_ASSERT(offsetof(lpi2c1176_regs_t, SCFGR1) == 0x124, "LPI2C SCFGR1");
LPI2C1176_ASSERT(offsetof(lpi2c1176_regs_t, SAMR)   == 0x140, "LPI2C SAMR");
LPI2C1176_ASSERT(offsetof(lpi2c1176_regs_t, SASR)   == 0x150, "LPI2C SASR");
LPI2C1176_ASSERT(offsetof(lpi2c1176_regs_t, STDR)   == 0x160, "LPI2C STDR");
LPI2C1176_ASSERT(offsetof(lpi2c1176_regs_t, SRDR)   == 0x170, "LPI2C SRDR");
```

After the MSR defines add (bit positions verbatim from the HW-verified `WireIMXRT1176.cpp` local defines, which this change replaces):

```c
/* SCR */
#define LPI2C1176_SCR_SEN     (1u << 0)
#define LPI2C1176_SCR_RST     (1u << 1)
#define LPI2C1176_SCR_FILTEN  (1u << 4)
/* SSR (SIER mirrors these bit positions) */
#define LPI2C1176_SSR_TDF     (1u << 0)
#define LPI2C1176_SSR_RDF     (1u << 1)
#define LPI2C1176_SSR_AVF     (1u << 2)
#define LPI2C1176_SSR_SDF     (1u << 9)
#define LPI2C1176_SSR_BEF     (1u << 10)
#define LPI2C1176_SSR_FEF     (1u << 11)
#define LPI2C1176_SIER_TDIE   LPI2C1176_SSR_TDF
#define LPI2C1176_SIER_RDIE   LPI2C1176_SSR_RDF
#define LPI2C1176_SIER_AVIE   LPI2C1176_SSR_AVF
#define LPI2C1176_SIER_SDIE   LPI2C1176_SSR_SDF
#define LPI2C1176_SIER_BEIE   LPI2C1176_SSR_BEF
#define LPI2C1176_SIER_FEIE   LPI2C1176_SSR_FEF
```

Next to the master API declarations add:

```c
/* Slave init through SIER (clocks/pins + reset + address + stall config +
 * interrupt arm) — everything except the NVIC hookup and the final enable,
 * which each consumer sequences itself (CM7: attachInterruptVector/NVIC;
 * CM4: NVIC_ISER1) so the HW-verified register order is preserved. */
void lpi2c1176_slave_config(lpi2c1176_regs_t *p, const lpi2c1176_hw_t *hw,
                            uint8_t address);
/* Final SCR = SEN | FILTEN — call AFTER the consumer's NVIC enable. */
void lpi2c1176_slave_enable(lpi2c1176_regs_t *p);
```

- [ ] **Step 3: Add the two function bodies to `lpi2c1176.c`**

Append (bodies verbatim from `TwoWire::begin(uint8_t)` in `WireIMXRT1176.cpp:79-101` — keep the load-bearing CLKHOLD and TDIE/BEIE comments, they are silicon knowledge):

```c
void lpi2c1176_slave_config(lpi2c1176_regs_t *p, const lpi2c1176_hw_t *hw,
                            uint8_t address)
{
	lpi2c1176_clocks_pins(hw);
	p->SCR = LPI2C1176_SCR_RST;
	p->SCR = 0u;
	p->SAMR = ((uint32_t)address << 1);
	/* SAEN (7-bit address) | RXSTALL (bit1) | TXDSTALL (bit2): clock-stretch until
	 * the ISR drains SRDR / fills STDR, so multi-byte reads/writes stay
	 * byte-correct even when the master clocks faster than the ISR can refill. */
	p->SCFGR1 = (1u << 9) | (1u << 2) | (1u << 1);
	/* SCFGR2.CLKHOLD (bits[3:0]) sets the SCL hold time while stalling — MUST be
	 * non-zero or TXDSTALL/RXSTALL never actually hold the clock, so the ISR
	 * can't refill STDR/drain SRDR in time on multi-byte transfers. Max hold. */
	p->SCFGR2 = 0x0000000Fu;
	/* TDIE is essential: without it only the first read byte (which rides the
	 * AVF interrupt) is served; bytes 2..N need a TDF interrupt each to refill
	 * STDR.  BEIE|FEIE are essential for recovery: a glitch can latch FEF/BEF,
	 * which corrupts the slave FIFO and wedges it into permanent address-NACK;
	 * the ISR W1Cs them so the *next* transfer recovers cleanly. */
	p->SIER = LPI2C1176_SIER_TDIE | LPI2C1176_SIER_RDIE | LPI2C1176_SIER_AVIE
	        | LPI2C1176_SIER_SDIE | LPI2C1176_SIER_BEIE | LPI2C1176_SIER_FEIE;
}

void lpi2c1176_slave_enable(lpi2c1176_regs_t *p)
{
	p->SCR = LPI2C1176_SCR_SEN | LPI2C1176_SCR_FILTEN;
}
```

- [ ] **Step 4: Migrate `TwoWire::begin(uint8_t)` and `handle_slave_isr` onto the shared names**

In `WireIMXRT1176.cpp`:
1. Delete the local `#define SCR_RST/SSR_*/SIER_*` block (lines ~54-67) — the shared header now owns them.
2. Replace the body of `TwoWire::begin(uint8_t address)` with:

```cpp
void TwoWire::begin(uint8_t address) {
	is_slave = true; slave_addr = address;
	s_rx_len = 0; s_rx_idx = 0; s_tx_len = 0; s_tx_idx = 0;
	lpi2c1176_slave_config(mp(), &hardware.hw, address);
	attachInterruptVector(hardware.irq, hardware.irq_function);
	NVIC_SET_PRIORITY(hardware.irq, hardware.irq_priority);
	NVIC_ENABLE_IRQ(hardware.irq);
	lpi2c1176_slave_enable(mp());
}
```

3. In `handle_slave_isr` (and any other user of the deleted names), rename `SSR_x` → `LPI2C1176_SSR_x`. No logic change — same bits, same order.

- [ ] **Step 5: Re-run the guardrail gate and diff byte-identical**

```bash
cd ~/Development/Wire/tests/wire_slave_test && ./run_qemu_wire_slave.sh
diff /private/tmp/claude-501/-Users-nicholasnewdigate-Development-rt1170/b11174ea-d6b3-49f4-bd1e-69803d4f2afe/scratchpad/wire_slave_baseline.uart wire_slave.uart
```

Expected: gate PASS **and** an empty diff (byte-identical transcript — the Phase 3.3 migration guardrail). Also run the master gate: `cd ../wire_master_test 2>/dev/null || true` — if a `run_qemu` script exists there, run it too (same PASS expectation).

- [ ] **Step 6: Commit (Wire repo)**

```bash
cd ~/Development/Wire && git add lpi2c1176.h lpi2c1176.c WireIMXRT1176.cpp
git commit -m "lpi2c1176: add shared slave block (slave_config/slave_enable), migrate TwoWire::begin(addr) onto it

Phase 4.2 sequences-live-once: the HW-verified slave init from
WireIMXRT1176.cpp:79-101 now lives once in the shared C core, consumed by
the CM7 TwoWire class and the CM4 gate image. Guardrail: wire_slave_test
QEMU transcript byte-identical before/after."
```

---

### Task 2: qemu2 — LPI2C2 IRQ-33 split + stale slave comment

**Files:**
- Modify: `~/Development/qemu2/include/hw/arm/fsl-imxrt1170.h`
- Modify: `~/Development/qemu2/hw/arm/fsl-imxrt1170.c`
- Modify: `~/Development/qemu2/hw/i2c/imxrt_lpi2c.c` (comment only)

- [ ] **Step 1: Add the splitter field**

In `fsl-imxrt1170.h`, next to the existing `SplitIRQ lpi2c5_irq_split;` add:

```c
    SplitIRQ lpi2c2_irq_split;   /* LPI2C2 IRQ 33 -> both NVICs (Phase 4.2 CM4 slave) */
```

- [ ] **Step 2: Init + realize the splitter**

In `fsl-imxrt1170.c` instance_init, next to the `lpi2c5-irq-split` init (`:479`):

```c
    object_initialize_child(obj, "lpi2c2-irq-split", &s->lpi2c2_irq_split,
                            TYPE_SPLIT_IRQ);
```

In realize, mirror the `lpi2c5_irq_split` block (`:1097-1100`):

```c
    object_property_set_int(OBJECT(&s->lpi2c2_irq_split), "num-lines", 2,
                            &error_abort);
    if (!qdev_realize(DEVICE(&s->lpi2c2_irq_split), NULL, errp)) {
        return;
    }
```

(Realize in the SAME change as init — `object_initialize_child` without `qdev_realize` aborts every machine, the Task-1-of-4.1 lesson.)

- [ ] **Step 3: Wire IRQ 33 to both NVICs**

In the LPI2C wiring loop (`:1140-1151`), the LPI2C5 index currently gets `fsl_imxrt1170_connect_irq_both` and every other index plain `sysbus_connect_irq`. Add the `i == 1` case:

```c
        if (i == 4) {           /* LPI2C5: split to both NVICs (Phase 4.1) */
            fsl_imxrt1170_connect_irq_both(&s->lpi2c5_irq_split, armv7m,
                                           armv7m_m4, sbd, 0, lpi2c_irq[i]);
        } else if (i == 1) {    /* LPI2C2: split to both NVICs (Phase 4.2 —
                                 * the QEMU-gate slave persona; silicon routes
                                 * LPI2C1/IRQ32 to both NVICs natively) */
            fsl_imxrt1170_connect_irq_both(&s->lpi2c2_irq_split, armv7m,
                                           armv7m_m4, sbd, 0, lpi2c_irq[i]);
        } else {
            sysbus_connect_irq(sbd, 0, qdev_get_gpio_in(armv7m, lpi2c_irq[i]));
        }
```

(Adapt the exact `if` shape to what is in the file — the LPI2C5 condition may test the index differently; keep its form and add the `i == 1` arm.)

- [ ] **Step 4: Fix the stale model comment**

`imxrt_lpi2c.c:15` says "The slave interface is not modelled." — false since the `wire_slave_test` slave block landed. Replace that sentence with:

```
 * The slave block (SCR/SSR/SIER/SAMR/STDR/SRDR) is modelled far enough to
 * gate a single-byte slave response (wire_slave_test contract); multi-byte
 * slave reads are hardware-verified only.
```

- [ ] **Step 5: Rebuild qemu2 and smoke-test**

```bash
cd ~/Development/qemu2/build && ninja qemu-system-arm 2>&1 | tail -3
cd ~/Development/rt1170/evkb/cm4_wire_int_master_test && ./run_qemu.sh
cd ~/Development/Wire/tests/wire_slave_test && ./run_qemu_wire_slave.sh
```

Expected: build clean; both gates PASS (4.1 exercises the split helper, wire_slave_test exercises LPI2C1+2 with the new splitter in place).

- [ ] **Step 6: checkpatch + commit (qemu2 repo)**

```bash
cd ~/Development/qemu2 && git diff | scripts/checkpatch.pl - || true   # warnings reviewed, errors fixed
git add include/hw/arm/fsl-imxrt1170.h hw/arm/fsl-imxrt1170.c hw/i2c/imxrt_lpi2c.c
git commit -m "fsl-imxrt1170: split LPI2C2 IRQ 33 to both NVICs (Phase 4.2 CM4 slave gate)

The 4.2 QEMU gate runs the CM4 slave on the LPI2C2 persona (bridged onto
LPI2C1's bus); its IRQ must reach the CM4 NVIC. Silicon routes LPI2C1
(IRQ 32, the HW slave instance) to both NVICs natively (RM 4-1/4-2), so
no change is needed for the hardware side. Also corrects the stale
'slave interface is not modelled' comment in imxrt_lpi2c.c."
```

---

### Task 3: Scaffold `cm4_wire_int_slave_test` (RED)

**Files:**
- Modify: `evkb/teensy-cmake-macros/CMakeLists.include.txt` (add optional `DEFINES` to `teensy_add_cm4_image`)
- Create: `evkb/cm4_wire_int_slave_test/CMakeLists.txt`
- Create: `evkb/cm4_wire_int_slave_test/toolchain/rt1170-evkb.toolchain.cmake` (copy from `cm4_wire_int_master_test/toolchain/`)
- Create: `evkb/cm4_wire_int_slave_test/cm4/cm4.ld` (copy from `cm4_wire_int_master_test/cm4/`)
- Create: `evkb/cm4_wire_int_slave_test/cm4/startup_cm4.S`
- Create: `evkb/cm4_wire_int_slave_test/cm4/main_cm4.c` (READY-only stub — Task 4 replaces it)
- Create: `evkb/cm4_wire_int_slave_test/cm4_wire_int_slave_test.cpp` (full CM7 reporter)
- Create: `evkb/cm4_wire_int_slave_test/run_qemu.sh`

- [ ] **Step 1: Extend `teensy_add_cm4_image` with an optional `DEFINES` multi-arg**

In `CMakeLists.include.txt:462` change the parse line and add the flag expansion, mirroring the `INCLUDE_DIRS` pattern (empty ⇒ byte-identical command lines, preserving the 2B cmp discipline):

```cmake
    cmake_parse_arguments(CM4 "" "LINKER" "SOURCES;INCLUDE_DIRS;DEFINES" ${ARGN})
```

and next to the `_cm4_incs` loop:

```cmake
    # Optional -D defines (e.g. a world-split instance selector). Compile-only;
    # images that don't pass DEFINES get byte-identical command lines.
    set(_cm4_defs "")
    foreach(_def ${CM4_DEFINES})
        list(APPEND _cm4_defs "-D${_def}")
    endforeach()
```

then add `${_cm4_defs}` to the compile command right after `${_cm4_incs}`.

- [ ] **Step 2: Write `CMakeLists.txt` — both worlds always built**

```cmake
cmake_minimum_required(VERSION 3.24)
project(cm4_wire_int_slave_test)

set(TEENSY_VERSION 117 CACHE STRING "")

include(FetchContent)
FetchContent_Declare(teensy_cmake_macros SOURCE_DIR ${CMAKE_CURRENT_LIST_DIR}/../teensy-cmake-macros)
FetchContent_MakeAvailable(teensy_cmake_macros)
include(${teensy_cmake_macros_SOURCE_DIR}/CMakeLists.include.txt)

import_arduino_library(cores ${CMAKE_CURRENT_LIST_DIR}/../cores/imxrt1176)

# One slave source, two world bindings (spec §4.2): QEMU -> LPI2C2 persona
# (IRQ 33, bridged bus); HW -> LPI2C1 (IRQ 32, Arduino header A4/A5).
teensy_add_cm4_image(cm4_wire_int_slave_q
    LINKER  ${CMAKE_CURRENT_LIST_DIR}/cm4/cm4.ld
    SOURCES ${CMAKE_CURRENT_LIST_DIR}/cm4/startup_cm4.S
            ${CMAKE_CURRENT_LIST_DIR}/cm4/main_cm4.c
            $ENV{HOME}/Development/Wire/lpi2c1176.c
    INCLUDE_DIRS $ENV{HOME}/Development/Wire)

teensy_add_cm4_image(cm4_wire_int_slave_h
    LINKER  ${CMAKE_CURRENT_LIST_DIR}/cm4/cm4.ld
    SOURCES ${CMAKE_CURRENT_LIST_DIR}/cm4/startup_cm4.S
            ${CMAKE_CURRENT_LIST_DIR}/cm4/main_cm4.c
            $ENV{HOME}/Development/Wire/lpi2c1176.c
    INCLUDE_DIRS $ENV{HOME}/Development/Wire
    DEFINES WIRE_SLAVE_WORLD_HW=1)

# QEMU-world CM7: boots the _q image AND masters LPI2C1 itself.
teensy_add_executable(cm4_wire_int_slave_test cm4_wire_int_slave_test.cpp
    $ENV{HOME}/Development/Wire/lpi2c1176.c)
teensy_target_link_libraries(cm4_wire_int_slave_test cores)
target_link_libraries(cm4_wire_int_slave_test.elf stdc++)
target_include_directories(cm4_wire_int_slave_test.elf PRIVATE $ENV{HOME}/Development/Wire)
teensy_target_link_cm4_image(cm4_wire_int_slave_test cm4_wire_int_slave_q)

# HW-world CM7: boots the _h image, reports only (the external master owns the bus).
teensy_add_executable(cm4_wire_int_slave_test_hw cm4_wire_int_slave_test.cpp
    $ENV{HOME}/Development/Wire/lpi2c1176.c)
teensy_target_link_libraries(cm4_wire_int_slave_test_hw cores)
target_link_libraries(cm4_wire_int_slave_test_hw.elf stdc++)
target_include_directories(cm4_wire_int_slave_test_hw.elf PRIVATE $ENV{HOME}/Development/Wire)
target_compile_definitions(cm4_wire_int_slave_test_hw.elf PRIVATE WIRE_SLAVE_WORLD_HW=1)
teensy_target_link_cm4_image(cm4_wire_int_slave_test_hw cm4_wire_int_slave_h)
```

(If `teensy_add_executable` rejects a second source file, compile `lpi2c1176.c` via `target_sources(<name>.elf PRIVATE ...)` instead — verify against the macro definition in `CMakeLists.include.txt`.)

- [ ] **Step 3: Write `cm4/startup_cm4.S`**

Copy `cm4_wire_int_master_test/cm4/startup_cm4.S` and change ONLY the vector table body and header comment — LPI2C1 (IRQ 32 → index 48) and LPI2C2 (IRQ 33 → index 49) replace the LPI2C5 slot:

```asm
    /* external IRQs 0..117 -> vector index 16..133 */
    .rept 32                    /* IRQ 0..31 */
    .word Default_Handler
    .endr
    .word LPI2C1_IRQHandler     /* index 48: external IRQ 32 (LPI2C1 — HW slave) */
    .word LPI2C2_IRQHandler     /* index 49: external IRQ 33 (LPI2C2 — QEMU-gate slave) */
    .rept 84                    /* IRQ 34..117 */
    .word Default_Handler
    .endr
    .word MU_IRQHandler         /* 134: external IRQ 118 (MU) */
```

(Arithmetic check: 16 + 32 + 2 + 84 = 134 ✓. Reset handler, VTOR, FPU, .data/.bss copy stay byte-identical to 4.1's.)

- [ ] **Step 4: Write the stub `cm4/main_cm4.c` (RED — sends READY, never the tokens)**

```c
/* Phase 4.2 scaffold stub: configures nothing, sends READY then parks.
 * Task 4 replaces this with the real interrupt-driven slave. */
#include <stdint.h>

#define MUB_BASE   0x40C4C000u
#define MUB_TR(n)  (*(volatile uint32_t *)(MUB_BASE + 0x00u + ((n) << 2)))
#define MUB_SR     (*(volatile uint32_t *)(MUB_BASE + 0x20u))
#define SR_TE(n)   (1u << (23 - (n)))

void SysTick_Handler(void) {}
void MU_IRQHandler(void) {}
void LPI2C1_IRQHandler(void) {}
void LPI2C2_IRQHandler(void) {}

static void mu_send(unsigned ch, uint32_t v)
{
    while (!(MUB_SR & SR_TE(ch))) {
    }
    MUB_TR(ch) = v;
}

int main(void)
{
    mu_send(0, 0xCAFE0001u);   /* READY */
    for (;;) {}
}
```

- [ ] **Step 5: Write the full CM7 reporter `cm4_wire_int_slave_test.cpp`**

```cpp
/*
 * cm4_wire_int_slave_test — Phase 4.2: the CM4 runs an INTERRUPT-DRIVEN I2C
 * slave @0x42 (distilled from the HW-verified TwoWire slave via the shared
 * lpi2c1176 core), servicing its LPI2C IRQ on its own NVIC. World-split
 * instance (spec §4.2): QEMU = LPI2C2 persona (IRQ 33, bridged onto LPI2C1's
 * bus; this CM7 build is the polled master); HW = LPI2C1 (IRQ 32, Arduino
 * header A4=SDA/A5=SCL; an EXTERNAL I2C master drives the exchange and its
 * own serial output is the HW-side oracle for the response byte).
 *
 * Protocol constants (both worlds): master writes {A5 5A C3}, then reads 1
 * byte; the slave responds 3C.
 *
 * Tokens (MU ch0, fixed order after the READY handshake):
 *   ready  = CAFE0001   CM4 slave configured + enabled
 *   irqcnt = >0         CM4 serviced its slave IRQ on its own NVIC
 *   b0/b1/b2 = A5/5A/C3 bytes the master wrote, captured by the CM4 ISR
 *   resp   = 0000003C   byte the ISR loaded into STDR when TDF fired
 *   err    = 00000000   0 OK / 4 = QEMU wait-guard expired (stalled exchange)
 *   done   = 00000001
 * WIRE_INT_SLAVE_CM4=PASS requires irqcnt>0, b0/b1/b2, resp, err=0, done=1;
 * the QEMU build additionally requires its own wr=0 and mrd=3C.
 */
#include "Arduino.h"
#include "core_pins.h"
#include "HardwareSerial.h"
#include "Multicore.h"
#include "MessagingUnit.h"
#include "lpi2c1176.h"
#include "imxrt1176.h"

#ifdef WIRE_SLAVE_WORLD_HW
#include "cm4_wire_int_slave_h.h"
#define CM4_IMG cm4_wire_int_slave_h
#else
#include "cm4_wire_int_slave_q.h"
#define CM4_IMG cm4_wire_int_slave_q
#endif

#define SLAVE_ADDR 0x42u
#define WAIT_LONG  3000000u

static void phex(const char *k, uint32_t v)
{
    Serial1.print(k); Serial1.print('=');
    for (int i = 28; i >= 0; i -= 4)
        Serial1.print("0123456789ABCDEF"[(v >> i) & 0xF]);
    Serial1.println();
}
static void ptimeout(const char *k) { Serial1.print(k); Serial1.println("=TIMEOUT"); }

static bool wait_recv_bounded(uint8_t ch, uint32_t *out)
{
    for (uint32_t n = WAIT_LONG; n; n--)
        if (MU.tryReceive(ch, out)) return true;
    return false;
}

#ifndef WIRE_SLAVE_WORLD_HW
/* QEMU-world master: LPI2C1 via the shared core (values verbatim from the
 * HW-verified Wire lpi2c1_hardware descriptor). */
#define LPI2C1 ((lpi2c1176_regs_t *)0x40104000u)
static const lpi2c1176_hw_t lpi2c1_hw = {
    &CCM_LPCG98_DIRECT, &CCM_CLOCK_ROOT37_CONTROL, 0u,
    &IOMUXC_SW_MUX_CTL_PAD_GPIO_AD_08, 0x11u, &IOMUXC_SW_PAD_CTL_PAD_GPIO_AD_08,
    &IOMUXC_SW_MUX_CTL_PAD_GPIO_AD_09, 0x11u, &IOMUXC_SW_PAD_CTL_PAD_GPIO_AD_09,
    &IOMUXC_LPI2C1_SCL_SELECT_INPUT, 0u,
    &IOMUXC_LPI2C1_SDA_SELECT_INPUT, 0u,
    0x0000001Eu,
};
#endif

void setup()
{
    Serial1.begin(115200);
    Serial1.println("CM4WIRESLV-GATE v1");

    MU.begin();
    Multicore.begin(CM4_IMG, sizeof(CM4_IMG));

    uint32_t ready = 0;
    bool ok = true;
    if (wait_recv_bounded(0, &ready)) phex("ready", ready);
    else { ptimeout("ready"); ready = 0; ok = false; }

    uint32_t wr = 0xFFFFFFFFu, mrd = 0xFFFFFFFFu;
#ifndef WIRE_SLAVE_WORLD_HW
    if (ok) {
        lpi2c1176_begin(LPI2C1, &lpi2c1_hw, 100000u);
        static const uint8_t tx[3] = { 0xA5u, 0x5Au, 0xC3u };
        wr = lpi2c1176_master_write(LPI2C1, SLAVE_ADDR, tx, 3, 1);
        uint8_t rb = 0;
        uint32_t n = lpi2c1176_master_read(LPI2C1, SLAVE_ADDR, &rb, 1, 1);
        mrd = n ? rb : 0xFFFFFFFFu;
    }
    phex("wr", wr);
    phex("mrd", mrd);
#else
    Serial1.println("wr=EXTERN");
    Serial1.println("mrd=EXTERN");
    Serial1.println("EXT-MASTER: run ext_master now (write A5 5A C3 to 0x42, read 1 byte)");
#endif

    static const char *labels[7] = { "irqcnt", "b0", "b1", "b2", "resp", "err", "done" };
    uint32_t v[7];
    for (int i = 0; i < 7; i++) {
#ifdef WIRE_SLAVE_WORLD_HW
        while (!MU.tryReceive(0, &v[i])) {}     /* human-paced external master */
        phex(labels[i], v[i]);
#else
        if (wait_recv_bounded(0, &v[i])) phex(labels[i], v[i]);
        else { ptimeout(labels[i]); v[i] = 0xFFFFFFFFu; ok = false; }
#endif
    }

    bool pass = ok
        && ready == 0xCAFE0001u
        && v[0] != 0x0u          /* irqcnt: CM4 took its slave IRQ */
        && v[1] == 0xA5u && v[2] == 0x5Au && v[3] == 0xC3u
        && v[4] == 0x3Cu         /* resp loaded into STDR */
        && v[5] == 0x0u          /* err */
        && v[6] == 0x1u;         /* done */
#ifndef WIRE_SLAVE_WORLD_HW
    pass = pass && wr == 0u && mrd == 0x3Cu;   /* master-side oracle (QEMU) */
#endif
    Serial1.println(pass ? "WIRE_INT_SLAVE_CM4=PASS" : "WIRE_INT_SLAVE_CM4=FAIL");
    Serial1.println("CM4WIRESLV-DONE");
}

void loop() {}
```

- [ ] **Step 6: Write `run_qemu.sh`**

```sh
#!/bin/sh
# QEMU gate for Phase-4.2: the CM4 runs an interrupt-driven I2C slave @0x42 on
# the LPI2C2 persona (IRQ 33 on its own NVIC via the qemu2 split); the CM7
# masters LPI2C1 across the bridged bus with the shared polled core.
# Single-byte slave response only in QEMU (documented model limit); multi-byte
# and the external-master topology are HW-verified by the EVKB probe.
set -e
QEMU=~/Development/rt1170/evkb/tools/qrun
DIR=$(cd "$(dirname "$0")" && pwd)
. ~/Development/rt1170/evkb/tools/gate-lib.sh
gate_init
ELF="$DIR/build/cm4_wire_int_slave_test.elf"
OUT="$DIR/cm4_wire_int_slave.uart"
rm -f "$OUT"
"$QEMU" -M mimxrt1170-evk -global fsl-imxrt1170.boot-xip=on -kernel "$ELF" \
    -display none -serial file:"$OUT" -d guest_errors -D "$DIR/cm4_wire_int_slave.dbg" &
P=$!; gate_pid $P
for _ in $(seq 1 40); do
    [ -f "$OUT" ] && grep -q "CM4WIRESLV-DONE" "$OUT" 2>/dev/null && break
    sleep 0.25
done
kill $P 2>/dev/null; wait $P 2>/dev/null || true

echo "==== captured UART ===="; [ -f "$OUT" ] && cat "$OUT" || echo "(no UART output)"

fail=0
check() {
    if grep -q "^$1" "$OUT"; then echo "PASS: $1"; else echo "FAIL: expected $1"; fail=1; fi
}
grep -q "CM4WIRESLV-GATE v1" "$OUT" || { echo "FAIL: banner missing"; exit 1; }
check "ready=CAFE0001"
check "wr=00000000"
check "mrd=0000003C"
check "b0=000000A5"
check "b1=0000005A"
check "b2=000000C3"
check "resp=0000003C"
check "err=00000000"
check "done=00000001"
grep -q "^irqcnt=00000000" "$OUT" && { echo "FAIL: irqcnt is 0 (no CM4 IRQ)"; fail=1; }
check "WIRE_INT_SLAVE_CM4=PASS"
grep -q "CM4WIRESLV-DONE" "$OUT" || { echo "FAIL: DONE missing"; fail=1; }

if [ $fail -eq 0 ]; then
    echo "PASS: CM4 interrupt-driven I2C slave verified in QEMU"
else
    echo "GATE FAILED"; exit 1
fi
```

`chmod +x run_qemu.sh`. Invoke as `./run_qemu.sh` (never `sh run_qemu.sh` — gate-lib re-exec quirk).

- [ ] **Step 7: Build and run the gate — expect RED**

```bash
cd ~/Development/rt1170/evkb/cm4_wire_int_slave_test
cmake -B build -DCMAKE_TOOLCHAIN_FILE=toolchain/rt1170-evkb.toolchain.cmake >/dev/null && cmake --build build 2>&1 | tail -3
./run_qemu.sh
```

Expected: build clean (both elves + both images); gate FAILS with `ready=CAFE0001` present but `irqcnt=TIMEOUT` etc. — proving the reporter, handshake, and gate plumbing work and the only missing piece is the real slave.

- [ ] **Step 8: Commit (evkb repo)**

```bash
cd ~/Development/rt1170/evkb && git add teensy-cmake-macros/CMakeLists.include.txt cm4_wire_int_slave_test
git commit -m "cm4_wire_int_slave_test: scaffold Phase 4.2 gate (RED) + teensy_add_cm4_image DEFINES arg"
```

---

### Task 4: CM4 interrupt-driven slave (GREEN)

**Files:**
- Modify: `evkb/cm4_wire_int_slave_test/cm4/main_cm4.c` (replace the stub)

- [ ] **Step 1: Write the real `main_cm4.c`**

```c
/* cm4_wire_int_slave_test CM4 firmware (Phase 4.2): the CM4 SELF-CONFIGURES
 * an LPI2C slave @0x42 via the shared lpi2c1176 core (the same
 * slave_config/slave_enable sequence the HW-verified CM7 TwoWire slave runs),
 * NVIC-enables the instance IRQ on its OWN NVIC, and services the exchange
 * entirely in its ISR: captures the master's write bytes (RDF), serves the
 * response byte (TDF, clock-stretched by TXDSTALL), counts STOPs (SDF).
 *
 * WORLD-SPLIT INSTANCE (spec §4.2): no LPI2C is both QEMU-bus-bridged and
 * EVKB-header-accessible, so the build binds the one slave implementation to
 *   QEMU: LPI2C2 persona 0x40108000, IRQ 33 (bridged onto LPI2C1's bus;
 *         the CM7 gate build is the polled master)
 *   HW:   LPI2C1 0x40104000, IRQ 32 (Arduino header A4=AD_09=SDA /
 *         A5=AD_08=SCL; an EXTERNAL master drives the exchange)
 * ISR logic distilled from TwoWire::handle_slave_isr (WireIMXRT1176.cpp:107,
 * MIT, HW-verified) — keep in sync. Deviation: rx count is NOT reset on AVF
 * (the read-phase address match must not discard the recorded write bytes).
 *
 * Tokens (MU ch0): READY, then {irqcnt, b0, b1, b2, resp, err, done}.
 * Public-domain scaffolding (N. Newdigate); shared-core register logic MIT. */
#include <stdint.h>
#include "lpi2c1176.h"

#ifdef WIRE_SLAVE_WORLD_HW
#define SLAVE_LPI2C ((lpi2c1176_regs_t *)0x40104000u)   /* LPI2C1 */
#define SLAVE_IRQ   32u
static const lpi2c1176_hw_t slave_hw = {                /* verbatim Wire lpi2c1_hardware */
    .lpcg = (volatile uint32_t *)0x40CC6C40u,           /* CCM_LPCG98_DIRECT */
    .clock_root = (volatile uint32_t *)0x40CC1280u,     /* CCM_CLOCK_ROOT37_CONTROL */
    .clock_root_val = 0u,
    .scl_mux = (volatile uint32_t *)0x400E812Cu, .scl_mux_val = 0x11u, /* AD_08 ALT1|SION */
    .scl_pad = (volatile uint32_t *)0x400E8370u,
    .sda_mux = (volatile uint32_t *)0x400E8130u, .sda_mux_val = 0x11u, /* AD_09 ALT1|SION */
    .sda_pad = (volatile uint32_t *)0x400E8374u,
    .scl_select = (volatile uint32_t *)0x400E85ACu, .scl_select_val = 0u,
    .sda_select = (volatile uint32_t *)0x400E85B0u, .sda_select_val = 0u,
    .pad_ctl_val = 0x0000001Eu,                         /* ODE|DSE|PUE|PUS */
};
#else
#define SLAVE_LPI2C ((lpi2c1176_regs_t *)0x40108000u)   /* LPI2C2 persona */
#define SLAVE_IRQ   33u
static const lpi2c1176_hw_t slave_hw = {                /* verbatim Wire lpi2c2_hardware:
    pin refs bind to LPI2C1's IOMUXC regs (inert in QEMU) */
    .lpcg = (volatile uint32_t *)0x40CC6C60u,           /* CCM_LPCG99_DIRECT */
    .clock_root = (volatile uint32_t *)0x40CC1300u,     /* CCM_CLOCK_ROOT38_CONTROL */
    .clock_root_val = 0u,
    .scl_mux = (volatile uint32_t *)0x400E812Cu, .scl_mux_val = 0x11u,
    .scl_pad = (volatile uint32_t *)0x400E8370u,
    .sda_mux = (volatile uint32_t *)0x400E8130u, .sda_mux_val = 0x11u,
    .sda_pad = (volatile uint32_t *)0x400E8374u,
    .scl_select = (volatile uint32_t *)0x400E85ACu, .scl_select_val = 0u,
    .sda_select = (volatile uint32_t *)0x400E85B0u, .sda_select_val = 0u,
    .pad_ctl_val = 0x0000001Eu,
};
#endif

#define SLAVE_ADDR  0x42u
#define RESP_BYTE   0x3Cu
#define NVIC_ISER1  (*(volatile uint32_t *)0xE000E104u)  /* IRQ 32..63 */
#define WAIT_GUARD  50000000u   /* QEMU-world bounded exchange wait */

/* Slave exchange state, shared ISR<->main. */
static volatile struct {
    uint32_t irqcnt, rx_n, tx_served, stops, resp;
    uint8_t  b[3];
} S;

/* ---- MU B side (the CM4's) ---- */
#define MUB_BASE   0x40C4C000u
#define MUB_TR(n)  (*(volatile uint32_t *)(MUB_BASE + 0x00u + ((n) << 2)))
#define MUB_SR     (*(volatile uint32_t *)(MUB_BASE + 0x20u))
#define SR_TE(n)   (1u << (23 - (n)))

static void mu_send(unsigned ch, uint32_t v)
{
    while (!(MUB_SR & SR_TE(ch))) {
    }
    MUB_TR(ch) = v;
}

/* Distilled TwoWire::handle_slave_isr (keep in sync — see file header). */
static void slave_isr_body(void)
{
    lpi2c1176_regs_t *p = SLAVE_LPI2C;
    uint32_t ssr = p->SSR;
    S.irqcnt++;

    if (ssr & (LPI2C1176_SSR_BEF | LPI2C1176_SSR_FEF))   /* latched error -> W1C, FIFO recovers */
        p->SSR = LPI2C1176_SSR_BEF | LPI2C1176_SSR_FEF;
    if (ssr & LPI2C1176_SSR_AVF) {                       /* address match: read SASR clears AVF */
        volatile uint32_t sasr = p->SASR; (void)sasr;    /* rx count NOT reset (see header) */
    }
    if (ssr & LPI2C1176_SSR_RDF) {                       /* master wrote a byte */
        uint8_t d = (uint8_t)p->SRDR;
        if (S.rx_n < 3u) S.b[S.rx_n] = d;
        S.rx_n++;
    }
    if (ssr & LPI2C1176_SSR_TDF) {                       /* master wants a byte (TXDSTALL holds SCL) */
        p->STDR = RESP_BYTE;
        S.resp = RESP_BYTE;
        S.tx_served++;
    }
    if (ssr & LPI2C1176_SSR_SDF) {                       /* STOP -> one transfer done */
        p->SSR = LPI2C1176_SSR_SDF;
        S.stops++;
    }
}

void LPI2C1_IRQHandler(void) { slave_isr_body(); }
void LPI2C2_IRQHandler(void) { slave_isr_body(); }
void SysTick_Handler(void) {}
void MU_IRQHandler(void) {}

static int exchange_complete(void)
{
    return S.stops >= 2u && S.rx_n >= 3u && S.tx_served >= 1u;
}

int main(void)
{
    lpi2c1176_slave_config(SLAVE_LPI2C, &slave_hw, SLAVE_ADDR);
    NVIC_ISER1 = (1u << (SLAVE_IRQ - 32u));
    __asm volatile ("cpsie i" ::: "memory");   /* reset handler left IRQs masked */
    lpi2c1176_slave_enable(SLAVE_LPI2C);       /* SEN last — same order as TwoWire */

    mu_send(0, 0xCAFE0001u);                   /* READY: master may transact now */

    uint32_t err = 0u;
#ifdef WIRE_SLAVE_WORLD_HW
    while (!exchange_complete()) {             /* human-paced external master */
    }
#else
    err = 4u;                                  /* stalled-exchange sentinel (4.1 lesson) */
    for (uint32_t g = 0; g < WAIT_GUARD; g++)
        if (exchange_complete()) { err = 0u; break; }
#endif

    mu_send(0, S.irqcnt);
    mu_send(0, S.b[0]);
    mu_send(0, S.b[1]);
    mu_send(0, S.b[2]);
    mu_send(0, S.tx_served ? S.resp : 0xDEADDEADu);
    mu_send(0, err);
    mu_send(0, 1u);                            /* done */
    for (;;) {}
}
```

- [ ] **Step 2: Rebuild and run the gate — expect GREEN**

```bash
cd ~/Development/rt1170/evkb/cm4_wire_int_slave_test && cmake --build build 2>&1 | tail -3
./run_qemu.sh
```

Expected: `WIRE_INT_SLAVE_CM4=PASS` with `irqcnt>0`, `b0=000000A5 b1=0000005A b2=000000C3`, `resp=0000003C`, `mrd=0000003C`, `err=00000000`.

**Contingency (documented model limit):** if `mrd`/`resp` cannot go green because the model serves the master's read without ever running the CM4 vCPU's TDF ISR (the `wire_slave_test` single-byte contract may not transfer to a cross-vCPU slave), then: keep `resp`/`mrd` UNASSERTED in `run_qemu.sh` and the QEMU-build PASS (write-path only: `irqcnt`, `b0/b1/b2`, `err`, `done`), document the deviation in `run_qemu.sh` + the file headers, and move the response byte entirely to the HW probe's external-master oracle. Do NOT weaken the HW-side assertions. Any such change is a spec deviation — note it in the Task 8 as-landed update.

- [ ] **Step 3: Run it 3× for stability, then commit the transcript**

```bash
for i in 1 2 3; do ./run_qemu.sh >/dev/null 2>&1 && echo "run $i PASS" || echo "run $i FAIL"; done
cp cm4_wire_int_slave.uart transcript_qemu.txt
cd ~/Development/rt1170/evkb && git add cm4_wire_int_slave_test
git commit -m "cm4_wire_int_slave_test: CM4 interrupt-driven I2C slave GREEN in QEMU (Phase 4.2)"
```

---

### Task 5: qemu2 regression + checkpatch

- [ ] **Step 1: Run the full regression set** (spec §5 obligation — 4.2 touched qemu2)

Per `silicon-truth-loop.md`: `test_imxrt1170.py` + `test_imxrt1062.py` functional suites; the `dualcore_mu_test` (diff its committed `transcript_qemu.txt`), `serial_test`, mcmgr, rpmsg gates; `Wire/tests/wire_master_test` + `wire_slave_test`; `SPI/tests/spi_dma_test`; plus 4.1's `cm4_wire_int_master_test/run_qemu.sh`.
Expected: every gate PASS, every committed transcript byte-identical.

- [ ] **Step 2: checkpatch on the cumulative qemu2 diff**

```bash
cd ~/Development/qemu2 && git diff HEAD~1 | scripts/checkpatch.pl -
```

Expected: no errors (warnings reviewed). Fix + amend if needed.

---

### Task 6: License audit

**Files:**
- Modify: `evkb/tools/license-audit.sh`

- [ ] **Step 1: Add `cm4_wire_int_slave_test` to the GATES array** (same one-line pattern as `cm4_wire_int_master_test`, added in a1c6105).

- [ ] **Step 2: Run the audit**

Run: `~/Development/rt1170/evkb/tools/license-audit.sh`
Expected: PASS — all firmware sources MIT/public-domain/permissive; qemu2 GPL stays on its side of the firewall.

- [ ] **Step 3: Commit**

```bash
cd ~/Development/rt1170/evkb && git add tools/license-audit.sh && git commit -m "license-audit: cover cm4_wire_int_slave_test (Phase 4.2)"
```

---

### Task 7: EVKB probe (HARDWARE — wired external master)

**Files:**
- Create: `evkb/cm4_wire_int_slave_test/ext_master/ext_master.ino`
- Create: `evkb/cm4_wire_int_slave_test/README.md` (wiring + probe procedure)
- Create: `evkb/cm4_wire_int_slave_test/transcript_hw_evkb.txt` (probe output)

- [ ] **Step 1: Write the external-master sketch** (generic Arduino `Wire` — MKR Zero verified precedent; any 3.3V board works):

```cpp
/* Phase 4.2 external I2C master: writes the protocol constants {A5 5A C3} to
 * the CM4 slave @0x42 and reads the 1-byte response, printing both — this
 * printout is the HW-side oracle for the response byte (expect rd=3C).
 * 3.3V LOGIC ONLY (MKR Zero etc.) — never a 5V master without level shifting. */
#include <Wire.h>

void setup() {
  Serial.begin(115200);
  Wire.begin();
  Wire.setClock(100000);
}

void loop() {
  Wire.beginTransmission(0x42);
  Wire.write(0xA5); Wire.write(0x5A); Wire.write(0xC3);
  uint8_t w = Wire.endTransmission();
  delay(5);
  uint8_t n = Wire.requestFrom((uint8_t)0x42, (uint8_t)1);
  int rb = n ? Wire.read() : -1;
  Serial.print("wr="); Serial.print(w);
  Serial.print(" rd="); Serial.println(rb, HEX);   // expect wr=0 rd=3C
  delay(2000);                                     // repeats until unplugged
}
```

- [ ] **Step 2: Write `README.md`** covering: the world-split design (one paragraph, pointing at spec §4.2), the token table, the wiring:
  - external master SDA → EVKB **A4** (GPIO_AD_09), SCL → **A5** (GPIO_AD_08), **GND → GND (essential — the Phase-3.2 flakiness was a ground joint)**
  - 3.3V logic only; EVKB pads have internal pull-ups (pad_ctl `0x1E`); add external 2.2–4.7 kΩ to 3V3 if the bus is flaky
  - probe procedure: flash `cm4_wire_int_slave_test_hw`, start serial capture, run the master, read both serial outputs

- [ ] **Step 3: Run the probe** (board connected; coordinate with the user for the external-master trigger)

```bash
pkill LinkServer; pkill redlinkserv; sleep 1
cd ~/Development/rt1170/evkb/cm4_wire_int_slave_test
# clean boot per clean_boot.scp discipline, then:
LinkServer flash MIMXRT1176xxxxx:MIMXRT1170-EVKB load build/cm4_wire_int_slave_test_hw.elf
# start the pyserial reader (115200, gtimeout) BEFORE resetting, per the
# macos-serial-capture memory; then reset the board, confirm
# "EXT-MASTER: run ext_master now", and ask the user to power/run the master.
```

Expected EVKB VCOM output: `ready=CAFE0001`, then after the master runs: `irqcnt=…(>0)`, `b0=000000A5`, `b1=0000005A`, `b2=000000C3`, `resp=0000003C`, `err=00000000`, `done=00000001`, `WIRE_INT_SLAVE_CM4=PASS`.
Expected external-master serial: `wr=0 rd=3C` (the HW-side oracle for the response byte — record it in the transcript file).

**If the probe fails:** systematic-debugging — no fixes without root cause. Candidate first probes: slave address on the wire (logic analyzer or master scan), pull-up adequacy, LPI2C1 clock root readback token. HW is the only oracle for slave read-data paths (4.1 lesson).

- [ ] **Step 4: Commit transcripts + README + sketch**

```bash
cd ~/Development/rt1170/evkb && git add cm4_wire_int_slave_test
git commit -m "cm4_wire_int_slave_test: EVKB probe HW-VERIFIED — external master exchange vs the CM4 LPI2C1 slave (Phase 4.2)"
```

---

### Task 8: Docs close-out

- [ ] **Step 1:** Update `.claude/skills/cm4-bringup/references/cm4-roadmap.md` — Phase 4.2 status + session log entry (discoveries: world-split instance resolution; any Task-4 contingency outcome).
- [ ] **Step 2:** If anything landed differently from spec §4.2 (e.g., the QEMU single-byte contingency), add an as-landed note to the spec, mirroring the 4.1 precedent (fb1a2eb).
- [ ] **Step 3:** Update the `rt1176-cm4-int-wire-phase4` memory file + `MEMORY.md` hook with the 4.2 outcome.
- [ ] **Step 4:** Commit: `git -C ~/Development/rt1170/evkb add -A docs .claude && git commit -m "docs: Phase 4.2 roadmap/spec/memory close-out"`

---

## Self-review (done at plan-writing)

- **Spec coverage:** §4.2 instance binding → Tasks 3/4 (`_q`/`_h` images); shared-core extension → Task 1; qemu2 LPI2C2 split + regression → Tasks 2/5; tokens + both-world assertions → Tasks 3/4; wired external-master probe + HW-side oracle → Task 7; license firewall → Task 6; roadmap truth → Task 8. No gaps.
- **Placeholders:** none — every code step carries the actual code; the one open behavior (QEMU cross-vCPU single-byte response) has an explicit, bounded contingency with its own documentation obligation.
- **Type consistency:** `lpi2c1176_slave_config(p, hw, address)` / `lpi2c1176_slave_enable(p)` used identically in Task 1 (definition), Task 1 Step 4 (TwoWire), Task 4 (CM4). Token labels match between `main_cm4.c`, the reporter, and `run_qemu.sh`. Image/target names consistent: `cm4_wire_int_slave_q/_h`, `cm4_wire_int_slave_test[_hw]`.
