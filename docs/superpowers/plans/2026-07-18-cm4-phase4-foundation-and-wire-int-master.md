# CM4 Phase 4 — split-IRQ foundation + slice 4.1 (interrupt Wire-master) Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Build the qemu2 split-IRQ foundation (route `LPSPI1`/`LPI2C5`/16 eDMA IRQ lines to *both* NVICs) and prove it with slice 4.1 — a CM4 image that runs an **interrupt-driven** LPI2C5 master and services the LPI2C5 IRQ *on its own NVIC*, HW-verified wiring-free against the WM8962.

**Architecture:** One targeted qemu2 change — a `TYPE_SPLIT_IRQ` (1→2) inserted between each target peripheral's existing IRQ output and the CM7+CM4 NVICs, mirroring the machine's existing `gpio13_or` `TYPE_OR_IRQ` idiom. Slice 4.1 clones `cm4_wire_test`; the CM4 reuses the HW-verified `lpi2c1176_begin()` for clock/pins but replaces the polled transfer with a **fresh** ISR-driven master (the CM7 master is polled, so this is new logic validated against SDK `fsl_lpi2c.c`). The gate proves the register/IRQ sequence in QEMU; the wiring-free EVKB probe (`rdv=0x6243` produced by the CM4 ISR, `irqcnt>0`) is the only proof the split IRQ routes to the CM4 on silicon.

**Tech Stack:** qemu2 (C, QOM devices), bare-metal Cortex-M4 C + ARM asm, Teensy-style Arduino CM7 reporter, CMake (`teensy_add_cm4_image`), `gate-lib.sh`/`qrun` QEMU gates, LinkServer + `clean_boot.scp` for the EVKB.

**Spec:** `docs/superpowers/specs/2026-07-18-cm4-interrupt-dma-spi-wire-design.md` (§0 scope, §3 foundation, §4.1). This plan covers **only the foundation + 4.1**; slices 4.2–4.4 get their own plans after their triangulation.

**Silicon-truth loop (cm4-bringup):** qemu2 is touched → Task 6 runs the full regression set + checkpatch. The split-IRQ is a new-model trigger → Task 8 is the mandatory EVKB probe. "Silicon wins": any qemu2 divergence found on HW gets a probe-cited comment + this plan updated.

---

## Ground-truth facts (triangulated this session — all file:line verified)

| Fact | Value | Source |
|---|---|---|
| LPI2C5 = index 4; base | `0x40C34000` | `fsl-imxrt1170.c:144-146` |
| LPI2C5 NVIC IRQ | **36** (both worlds) | `fsl-imxrt1170.c:148-150`; `core_pins.h:47` |
| LPSPI1 NVIC IRQ | 38 | `fsl-imxrt1170.c:158-160`; `core_pins.h:48` |
| eDMA IRQ lines | 16 (ch i & i+16 share line i) | `imxrt_edma.h:22,24` |
| Peripheral IRQ→CM7-only wiring to replace | LPI2C `:1094`, LPSPI `:1115`, eDMA loop `:1399` | `fsl-imxrt1170.c` |
| Existing 1→N child-device idiom to mirror | `gpio13_or` `TYPE_OR_IRQ`: init `:440`, realize+wire `:1052-1074` | `fsl-imxrt1170.c` |
| qemu2 has NO `qemu_irq_split()` → use `TYPE_SPLIT_IRQ` device (`num-lines` uint16) | — | `hw/core/split-irq.c:59-63`; precedent `mps2-tz.c:862-887` |
| LPI2C models already assert IRQ (master+slave); no IRQ-raising work | `imxrt_lpi2c_update_irq` | `hw/i2c/imxrt_lpi2c.c:197-200` |
| MIER offset `0x18`; bits **mirror MSR positions** (TDIE=0,RDIE=1,EPIE=8,SDIE=9,NDIE=10,ALIE=11,FEIE=12) | RM §69.5.1.6 | `rm_full.txt:306417`; `lpi2c1176.h:56` |
| MSR/MCR/CMD defs + `MRDR_RXEMPTY` | TDF=0,RDF=1,EPF=8,SDF=9,NDF=10,ALF=11,FEF=12; CMD START=4/TXD=0/RXD=1/STOP=2; `RXEMPTY=1<<14` | `Wire/lpi2c1176.h:96-114` |
| CM4 reset handler does `cpsid i` and never re-enables | main must `cpsie i` | `cm4_wire_test/cm4/startup_cm4.S:47` |
| CM4 vector table: ext IRQ n at index 16+n; currently `.rept 118` fills 0-117 | LPI2C5 (36) → index 52 | `cm4_wire_test/cm4/startup_cm4.S:36-40` |
| NVIC ISER1 (IRQ 32-63) | `0xE000E104`, IRQ36 → bit 4 | ARMv7-M |
| license-audit GATES format | `name:elf_target` space-list | `tools/license-audit.sh:60` |

---

## File Structure

**qemu2 (the foundation — one change):**
- Modify `~/Development/qemu2/include/hw/arm/fsl-imxrt1170.h` — add `SplitIRQ` fields to `FslIMXRT1170State` + include.
- Modify `~/Development/qemu2/hw/arm/fsl-imxrt1170.c` — init the splitters (instance_init), a `connect_irq_both()` helper, rewire the 3 peripheral IRQ sites.

**evkb gate `cm4_wire_int_master_test/` (clone of `cm4_wire_test/`):**
- Create `cm4_wire_int_master_test.cpp` — CM7 reporter (5 tokens + PASS).
- Create `cm4/main_cm4.c` — CM4: `lpi2c1176_begin` + fresh ISR master + kick + tokens.
- Create `cm4/startup_cm4.S` — clone with `LPI2C5_IRQHandler` at vector index 52.
- Create `cm4/cm4.ld`, `toolchain/…` — verbatim clones.
- Create `CMakeLists.txt` — clone, names swapped.
- Create `run_qemu.sh` — clone, new token asserts.
- Create `README.md` — divergence + wiring-free-probe notes.
- Modify `tools/license-audit.sh` — add the gate to `GATES`.

**Responsibility boundaries:** the qemu2 change owns *IRQ routing only* (no peripheral-model change). `main_cm4.c` owns the fresh ISR master (the only new firmware logic); everything else is a mechanical clone. Keep `main_cm4.c` focused — clock/pins stay in the shared `lpi2c1176.c` (unchanged).

---

## Task 1: qemu2 — add SplitIRQ state fields + init the splitters

**Files:**
- Modify: `~/Development/qemu2/include/hw/arm/fsl-imxrt1170.h`
- Modify: `~/Development/qemu2/hw/arm/fsl-imxrt1170.c` (instance_init, near `gpio13_or` init `:440`)

- [ ] **Step 1: Add the include + fields to the state struct**

In `include/hw/arm/fsl-imxrt1170.h`, near the other `hw/core` includes add:

```c
#include "hw/core/split-irq.h"
```

In `struct FslIMXRT1170State`, near `OrIRQState gpio13_or;`, add:

```c
    /* Phase 4: peripheral IRQ lines fanned out to BOTH NVICs (RM Tables
     * 4-1/4-2).  Silicon truth pending -- cm4_wire_int_master_test (4.1). */
    SplitIRQ lpspi1_irq_split;
    SplitIRQ lpi2c5_irq_split;
    SplitIRQ edma_irq_split[IMXRT_EDMA_NUM_IRQS];
```

(If `IMXRT_EDMA_NUM_IRQS` is not already visible in this header, use the literal `16` with a comment — the value is fixed by `imxrt_edma.h:24`.)

- [ ] **Step 2: Initialize the splitter children in instance_init**

In `fsl-imxrt1170.c`, in the `_init` function right after the `gpio13-or`/`enet-or` `object_initialize_child` calls (`:440`/`:474`), add:

```c
    object_initialize_child(obj, "lpspi1-irq-split", &s->lpspi1_irq_split,
                            TYPE_SPLIT_IRQ);
    object_initialize_child(obj, "lpi2c5-irq-split", &s->lpi2c5_irq_split,
                            TYPE_SPLIT_IRQ);
    for (int i = 0; i < IMXRT_EDMA_NUM_IRQS; i++) {
        object_initialize_child(obj, "edma-irq-split[*]", &s->edma_irq_split[i],
                                TYPE_SPLIT_IRQ);
    }
```

(Use QOM's native `"[*]"` auto-indexing name — the dominant idiom in this
function, `lpi2c[*]`/`lpspi[*]`/`sai[*]`/… — not a hand-rolled `g_strdup_printf`.
`IMXRT_EDMA_NUM_IRQS` is already visible via the existing `imxrt_edma.h`
include; else use literal `16`, value from `imxrt_edma.h:24`.)

- [ ] **Step 3: Realize the splitters (connection-free) in `fsl_imxrt1170_realize`**

**Why (verified):** `object_initialize_child()` creates a hard obligation to
`qdev_realize()` that child before machine construction finishes —
`qdev_assert_realized_properly()` (`hw/core/qdev.c:293-301`, called
unconditionally from `qdev_machine_creation_done()`) asserts `realized` for
every QOM child, so an init-without-realize aborts **every** `mimxrt1170-evk`
instantiation. Precedent + invariant: `hw/arm/exynos4210.c:429-435`. So Task 1
must realize the splitters (still connection-free = inert); Task 2 only wires
them. Put this right after the existing `gpio13_or` realize block (≈`:1056`),
so it precedes the LPI2C/LPSPI/eDMA connect sites:

```c
    /* Phase 4: realize the fan-out splitters (num-lines=2 -> CM7+CM4 NVICs).
     * Connection-free here (inert: nothing routed through them yet); Task 2
     * wires them.  Mirrors the gpio13_or realize above; required because
     * qdev_assert_realized_properly() asserts every initialized child. */
    object_property_set_int(OBJECT(&s->lpspi1_irq_split), "num-lines", 2,
                            &error_abort);
    if (!qdev_realize(DEVICE(&s->lpspi1_irq_split), NULL, errp)) {
        return;
    }
    object_property_set_int(OBJECT(&s->lpi2c5_irq_split), "num-lines", 2,
                            &error_abort);
    if (!qdev_realize(DEVICE(&s->lpi2c5_irq_split), NULL, errp)) {
        return;
    }
    for (int i = 0; i < IMXRT_EDMA_NUM_IRQS; i++) {
        object_property_set_int(OBJECT(&s->edma_irq_split[i]), "num-lines", 2,
                                &error_abort);
        if (!qdev_realize(DEVICE(&s->edma_irq_split[i]), NULL, errp)) {
            return;
        }
    }
```

- [ ] **Step 4: Build + smoke-test that the machine still constructs**

```bash
cd ~/Development/qemu2/build && ninja qemu-system-arm
timeout 8 ./qemu-system-arm -M mimxrt1170-evk -nographic -monitor none -serial none; echo "exit=$?"
```
Expected: builds clean; the run aborts with the **baseline** `Lockup: can't
escalate 3 to HardFault` (no firmware → CPU faults on zeroed memory) — NOT an
`Assertion failed: (dev->realized)`. The realize-assert must be gone (that's
the proof the splitters are properly realized). A/B this against `git stash`
if unsure.

- [ ] **Step 5: Commit**

```bash
cd ~/Development/qemu2
git add include/hw/arm/fsl-imxrt1170.h hw/arm/fsl-imxrt1170.c
git commit -m "hw/arm/fsl-imxrt1170: add + realize per-line SplitIRQ children (Phase 4 foundation)

Route LPSPI1/LPI2C5/eDMA IRQs to both NVICs next; mirrors the existing
gpio13_or TYPE_OR_IRQ child idiom. Realized connection-free here (inert -
qdev_assert_realized_properly requires every initialized child be realized);
Task 2 wires them."
```

---

## Task 2: qemu2 — helper + rewire LPSPI1/LPI2C5/eDMA to both NVICs

**Files:**
- Modify: `~/Development/qemu2/hw/arm/fsl-imxrt1170.c` (add helper; edit `:1094`, `:1115`, `:1399`)

- [ ] **Step 1: Add the `connect_irq_both` helper**

Above `fsl_imxrt1170_realize` in `fsl-imxrt1170.c`, add (uses `s->armv7m` / `s->armv7m_m4` device handles — take them as params to avoid ordering issues):

```c
/*
 * Route a peripheral IRQ output to BOTH NVICs, as silicon does (RM Tables
 * 4-1/4-2): a TYPE_SPLIT_IRQ (1->2) whose outputs feed the CM7 and CM4 NVIC
 * inputs at the same index.  Which core actually takes the interrupt is
 * decided by that core's NVIC enable, exactly as on hardware.
 * Silicon truth: cm4_wire_int_master_test / 2026-07-.. (EVKB, split routes
 * the LPI2C5 IRQ to the CM4).  Mirrors the gpio13_or OR-gate idiom above.
 */
static void fsl_imxrt1170_connect_irq_both(SplitIRQ *split, DeviceState *m7,
                                           DeviceState *m4, SysBusDevice *dev,
                                           int out, int irqnum)
{
    /* split is already realized (num-lines=2) in the Task-1 realize block;
     * here we only WIRE it: peripheral IRQ output -> split input, split's two
     * outputs -> the CM7 and CM4 NVIC inputs at the same index. */
    sysbus_connect_irq(dev, out, qdev_get_gpio_in(DEVICE(split), 0));
    qdev_connect_gpio_out(DEVICE(split), 0, qdev_get_gpio_in(m7, irqnum));
    qdev_connect_gpio_out(DEVICE(split), 1, qdev_get_gpio_in(m4, irqnum));
}
```

(Note: the helper must be called *after* the Task-1 realize block runs — it
does, since realize is near `:1056` and the peripheral connect sites are at
`:1094`+.)

- [ ] **Step 2: Rewire LPI2C5 (index 4) — replace the CM7-only connect at `:1094`**

The LPI2C loop connects every instance to the CM7 NVIC. Special-case index 4:

```c
    for (int i = 0; i < FSL_IMXRT1170_NUM_LPI2CS; i++) {
        SysBusDevice *sbd = SYS_BUS_DEVICE(&s->lpi2c[i]);
        ...
        sysbus_mmio_map(sbd, 0, lpi2c_base[i]);
        if (i == 4) {                                   /* LPI2C5 -> both NVICs */
            fsl_imxrt1170_connect_irq_both(&s->lpi2c5_irq_split, armv7m,
                                           armv7m_m4, sbd, 0, lpi2c_irq[i]);
        } else {
            sysbus_connect_irq(sbd, 0, qdev_get_gpio_in(armv7m, lpi2c_irq[i]));
        }
    }
```

- [ ] **Step 3: Rewire LPSPI1 (index 0) — replace the CM7-only connect at `:1115`**

```c
        if (i == 0) {                                   /* LPSPI1 -> both NVICs */
            fsl_imxrt1170_connect_irq_both(&s->lpspi1_irq_split, armv7m,
                                           armv7m_m4, sbd, 0, lpspi_irq[i]);
        } else {
            sysbus_connect_irq(sbd, 0, qdev_get_gpio_in(armv7m, lpspi_irq[i]));
        }
```

- [ ] **Step 4: Rewire the 16 eDMA completion lines — replace the loop at `:1399`**

```c
    for (int i = 0; i < IMXRT_EDMA_NUM_IRQS; i++) {
        fsl_imxrt1170_connect_irq_both(&s->edma_irq_split[i], armv7m,
                                       armv7m_m4, SYS_BUS_DEVICE(&s->edma), i,
                                       i);
    }
    /* eDMA error line stays CM7-only for now (no 4.x slice needs it). */
    sysbus_connect_irq(SYS_BUS_DEVICE(&s->edma), IMXRT_EDMA_NUM_IRQS,
                       qdev_get_gpio_in(armv7m, FSL_IMXRT1170_DMA_ERROR_IRQ));
```

(`armv7m` and `armv7m_m4` are the `DeviceState *` already in scope at realize — `:968`.)

- [ ] **Step 5: Build**

Run: `cd ~/Development/qemu2/build && ninja qemu-system-arm`
Expected: builds clean.

- [ ] **Step 6: Prove inertness — the imxrt1170 functional suite still passes**

```bash
cd ~/Development/qemu2/build
export QEMU_TEST_QEMU_BINARY=$PWD/qemu-system-arm \
       QEMU_TEST_ARM_GCC=/Applications/ARM_10/bin/arm-none-eabi-gcc \
       MESON_BUILD_ROOT=$PWD \
       PYTHONPATH=$PWD/../tests/functional:$PWD/../python
./pyvenv/bin/python3 ../tests/functional/arm/test_imxrt1170.py
```

Expected: `OK` (all sub-tests pass — the split only *adds* fan-out to a powered-off/NVIC-masked CM4, so CM7-side behavior is unchanged). If any test fails, STOP — the split is not inert; investigate before proceeding.

- [ ] **Step 7: Commit**

```bash
cd ~/Development/qemu2
git add hw/arm/fsl-imxrt1170.c
git commit -m "hw/arm/fsl-imxrt1170: fan LPSPI1/LPI2C5/eDMA IRQs to both NVICs

Targeted TYPE_SPLIT_IRQ per line so the CM4 can take these interrupts on
its own NVIC (RM Tables 4-1/4-2).  Inert until CM4 software NVIC-enables a
line; imxrt1170 functional suite still green.  Foundation for Phase 4.
Silicon truth to be closed by cm4_wire_int_master_test on the EVKB."
```

---

## Task 3: Scaffold the 4.1 gate (clone) — RED (CM4 stubbed)

**Files:**
- Create: `evkb/cm4_wire_int_master_test/` (clone `evkb/cm4_wire_test/`), then edit as below.

- [ ] **Step 1: Clone the gate directory**

```bash
cd ~/Development/rt1170/evkb
cp -r cm4_wire_test cm4_wire_int_master_test
cd cm4_wire_int_master_test
rm -rf build cm4_wire.uart cm4_wire.dbg transcript_qemu.txt transcript_hw_evkb.txt
mv cm4_wire_test.cpp cm4_wire_int_master_test.cpp
```

- [ ] **Step 2: Rewrite the CM7 reporter `cm4_wire_int_master_test.cpp`**

Replace the file with (5 tokens: `irqcnt`, `mcr`, `rdv`, plus `lpcg`/`croot` informative; `rdv` world-split):

```cpp
/*
 * cm4_wire_int_master_test — Phase 4.1: the CM4 self-configures LPI2C5 and
 * runs an INTERRUPT-DRIVEN master (its own LPI2C5 ISR on the CM4 NVIC — the
 * first non-MU peripheral IRQ routed to the CM4, via the qemu2 split-IRQ).
 * The CM7 only boots the CM4 image and reports its MU tokens on LPUART1.
 *
 * Tokens (MU ch0, fixed order):
 *   irqcnt = >0         CM4 serviced the LPI2C5 IRQ (isolated routing proof)
 *   mcr    = 00000001   LPI2C MCR.MEN — master enabled
 *   lpcg   = ........    CCM_LPCG102 readback (informative)
 *   croot  = ........    CCM_CLOCK_ROOT41 readback (informative)
 *   rdv    = ????????    R15 ID read by the CM4 ISR — WORLD-SPLIT:
 *                        QEMU wm8962-stub = 0x0000; silicon = 0x6243
 *   done   = 00000001   CM4 sequence completed
 * WIRE_INT_MASTER_CM4=PASS requires irqcnt>0, mcr=1, done=1.
 */
#include "Arduino.h"
#include "core_pins.h"
#include "HardwareSerial.h"
#include "Multicore.h"
#include "MessagingUnit.h"
#include "cm4_wire_int_master.h"   /* generated by teensy_add_cm4_image */

#define WAIT_LONG 3000000u

static void phex(const char *k, uint32_t v)
{
    Serial1.print(k); Serial1.print('=');
    for (int i = 28; i >= 0; i -= 4)
        Serial1.print("0123456789ABCDEF"[(v >> i) & 0xF]);
    Serial1.println();
}
static void ptimeout(const char *k) { Serial1.print(k); Serial1.println("=TIMEOUT"); }

static bool wait_recv(uint8_t ch, uint32_t *out)
{
    for (uint32_t n = WAIT_LONG; n; n--)
        if (MU.tryReceive(ch, out)) return true;
    return false;
}

void setup()
{
    Serial1.begin(115200);
    Serial1.println("CM4WIREINT-GATE v1");

    MU.begin();
    Multicore.begin(cm4_wire_int_master, sizeof(cm4_wire_int_master));

    static const char *labels[6] = { "irqcnt", "mcr", "lpcg", "croot", "rdv", "done" };
    uint32_t v[6];
    bool ok = true;
    for (int i = 0; i < 6; i++) {
        if (wait_recv(0, &v[i])) phex(labels[i], v[i]);
        else { ptimeout(labels[i]); v[i] = 0xFFFFFFFFu; ok = false; }
    }

    /* rdv (v[4]) is world-dependent -> NOT folded into PASS. */
    bool pass = ok
        && v[0] != 0x0u          /* irqcnt: CM4 took the LPI2C5 IRQ */
        && v[1] == 0x1u          /* mcr.MEN */
        && v[5] == 0x1u;         /* done */
    Serial1.println(pass ? "WIRE_INT_MASTER_CM4=PASS" : "WIRE_INT_MASTER_CM4=FAIL");
    Serial1.println("CM4WIREINT-DONE");
}

void loop() {}
```

- [ ] **Step 3: Point CMake at the new names**

In `CMakeLists.txt`, replace every `cm4_wire_test`→`cm4_wire_int_master_test`, `cm4_wire`→`cm4_wire_int_master` (project name, `teensy_add_cm4_image(<name> …)`, `teensy_target_link_cm4_image`, the `.cpp` source). Keep the `INCLUDE_DIRS` pointing at the shared `~/Development/Wire` (for `lpi2c1176.h`) exactly as `cm4_wire_test` did.

- [ ] **Step 4: Stub the CM4 so the gate is RED**

Edit `cm4/main_cm4.c`: keep the existing `lpi2c1176_begin(...)` + MU scaffolding, but send `irqcnt=0` and skip the transaction (no ISR yet). Minimal body:

```c
    lpi2c1176_begin(LPI2C5, &lpi2c5_hw, 100000u);
    uint32_t mcr = LPI2C5->MCR & LPI2C1176_MCR_MEN;
    mu_send(0, 0u);                 /* irqcnt (stub) */
    mu_send(0, mcr);
    mu_send(0, *lpi2c5_hw.lpcg);
    mu_send(0, *lpi2c5_hw.clock_root);
    mu_send(0, 0u);                 /* rdv (stub) */
    mu_send(0, 1u);                 /* done */
    for (;;) {}
```

- [ ] **Step 5: Rewrite `run_qemu.sh` token checks**

Replace the `check` lines with:

```sh
grep -q "CM4WIREINT-GATE v1" "$OUT" || { echo "FAIL: banner missing"; exit 1; }
check "mcr=00000001"
check "rdv=00000000"                 # stub contract (HW asserts 00006243)
check "done=00000001"
grep -q "^irqcnt=00000000" "$OUT" && { echo "FAIL: irqcnt is 0 (no CM4 IRQ)"; fail=1; }
check "WIRE_INT_MASTER_CM4=PASS"
```

Also swap `CM4WIRE-DONE`→`CM4WIREINT-DONE`, `$OUT` filename to `cm4_wire_int_master.uart`, `.dbg` and ELF name accordingly. **And fix the trailing green-path `echo`** (the template's `"CM4 self-configured polled I2C verified in QEMU"`) to describe interrupt-driven I2C, e.g. `"CM4 interrupt-driven I2C master verified in QEMU"` — otherwise it mislabels the achievement once Task 5 turns the gate green.

- [ ] **Step 6: Build + run the gate — expect RED**

```bash
cd ~/Development/rt1170/evkb/cm4_wire_int_master_test
rm -rf build && cmake -B build -G Ninja \
  --toolchain toolchain/rt1170-evkb.toolchain.cmake >/dev/null && cmake --build build >/dev/null
sh run_qemu.sh
```

Expected: FAIL — `irqcnt=00000000` trips the guard, `WIRE_INT_MASTER_CM4=PASS` absent. (Confirms the gate detects the missing interrupt path.)

- [ ] **Step 7: Commit (RED)**

```bash
cd ~/Development/rt1170/evkb
git add cm4_wire_int_master_test
git commit -m "cm4_wire_int_master_test: scaffold gate (Phase 4.1, RED)

Clone of cm4_wire_test; CM7 reporter + runner assert irqcnt>0 and the
interrupt-master rdv.  CM4 stubbed (irqcnt=0) -> gate RED, as intended."
```

---

## Task 4: CM4 vector table — slot the LPI2C5 ISR at index 52

**Files:**
- Modify: `evkb/cm4_wire_int_master_test/cm4/startup_cm4.S`

- [ ] **Step 1: Split the `.rept 118` to place `LPI2C5_IRQHandler` at external IRQ 36**

Replace the external-IRQ block (`:36-40`) with:

```asm
    /* external IRQs 0..117 -> vector index 16..133 */
    .rept 36                    /* IRQ 0..35 */
    .word Default_Handler
    .endr
    .word LPI2C5_IRQHandler     /* index 52: external IRQ 36 (LPI2C5) */
    .rept 81                    /* IRQ 37..117 */
    .word Default_Handler
    .endr
    .word MU_IRQHandler         /* 134: external IRQ 118 (MU) */
```

(36 + 1 + 81 + 1 = 119 words for IRQ 0..118; table length unchanged.)

- [ ] **Step 2: Add an EMPTY `LPI2C5_IRQHandler` stub so this commit links + stays RED**

The vector table now references `LPI2C5_IRQHandler`, but its real definition
doesn't land until Task 5's `main_cm4.c` — so **this commit would fail to link
(undefined reference)** without a placeholder. In `cm4/main_cm4.c`, next to the
existing `void SysTick_Handler(void) {}` / `void MU_IRQHandler(void) {}` stubs,
add:

```c
void LPI2C5_IRQHandler(void) {}   /* Task 4 placeholder; real ISR in Task 5 */
```

This links, and the gate stays **RED** (the stub `main()` still sends
`irqcnt=0` and never NVIC-enables/kicks, so the empty handler never fires).

- [ ] **Step 3: Update the header comment**

Change the `.S` top comment to note the LPI2C5 handler at exception 16+36=52 (Phase 4.1) in addition to SysTick and MU.

- [ ] **Step 4: Build + confirm still RED, then commit**

```bash
cd ~/Development/rt1170/evkb/cm4_wire_int_master_test
cmake --build build >/dev/null && ./run_qemu.sh; echo "exit=$?"   # expect exit 1, FAIL: irqcnt is 0
cd ~/Development/rt1170/evkb
git add cm4_wire_int_master_test/cm4/startup_cm4.S cm4_wire_int_master_test/cm4/main_cm4.c
git commit -m "cm4_wire_int_master_test: vector LPI2C5_IRQHandler at IRQ 36 (Phase 4.1)

Split the .rept so external IRQ 36 (LPI2C5) points at LPI2C5_IRQHandler at
vector index 52; add an empty placeholder handler so the image links and the
gate stays RED (the real ISR + NVIC-enable land in the next commit)."
```

---

## Task 5: CM4 fresh ISR master + kick — GREEN

> **Post-implementation notes (as landed):** (1) `isr_xfer_run` must queue the
> START+addr command to MTDR *before* arming `MIER` — arming first lets the CM4
> take the IRQ in the store gap and push a data byte with no START (a
> silicon-only race QEMU's TB-boundary IRQ check hides; SDK + polled core both
> do START-first). (2) A follow-up (`c68c20b`) added a **7th token `err`**
> (order `irqcnt, mcr, lpcg, croot, rdv, err, done`) asserted `=0` in *both*
> worlds and folded into PASS (`v[5]==0 && v[6]==1`), so an interrupt-master
> fault can't read as PASS. The code blocks below show the 6-token pre-follow-up
> form; the shipped gate streams 7.

**Files:**
- Modify: `evkb/cm4_wire_int_master_test/cm4/main_cm4.c`

- [ ] **Step 1: Add MIER + NVIC defs and the transfer descriptor**

Below the `#include "lpi2c1176.h"` in `main_cm4.c`, add (MIER bits mirror MSR positions — RM §69.5.1.6):

```c
/* MIER shares MSR bit positions (RM 69.5.1.6). */
#define MIER_TDIE  LPI2C1176_MSR_TDF
#define MIER_RDIE  LPI2C1176_MSR_RDF
#define MIER_SDIE  LPI2C1176_MSR_SDF
#define MIER_NDIE  LPI2C1176_MSR_NDF
#define MIER_ALIE  LPI2C1176_MSR_ALF
#define MIER_FEIE  LPI2C1176_MSR_FEF
#define NVIC_ISER1 (*(volatile uint32_t *)0xE000E104u)   /* IRQ 32..63 */
#define IRQ_LPI2C5 36

enum { PH_WRITE, PH_READ, PH_STOP, PH_DONE, PH_ERR };
typedef struct {
    lpi2c1176_regs_t *p;
    uint8_t addr;
    const uint8_t *wr; uint32_t wn, wi;
    uint8_t *rd; uint32_t rn, ri;
    volatile int phase;
    volatile uint32_t err;
    volatile uint32_t irqcnt;
} isr_xfer_t;
static volatile isr_xfer_t X;
```

- [ ] **Step 2: Replace the empty Task-4 `LPI2C5_IRQHandler` placeholder with the real ISR**

Keep the `SysTick_Handler`/`MU_IRQHandler` empty stubs. **Replace** the empty
`void LPI2C5_IRQHandler(void) {}` placeholder that Task 4 added with the real ISR
below (the fresh logic — validate its shape against SDK `fsl_lpi2c.c
LPI2C_MasterTransferHandleIRQ` before running; the gate is the oracle):

```c
void SysTick_Handler(void) {}
void MU_IRQHandler(void) {}

void LPI2C5_IRQHandler(void)
{
    lpi2c1176_regs_t *p = X.p;
    uint32_t s = p->MSR;
    X.irqcnt++;

    if (s & (LPI2C1176_MSR_NDF | LPI2C1176_MSR_ALF | LPI2C1176_MSR_FEF)) {
        p->MSR = s;                                          /* W1C errors */
        p->MTDR = LPI2C1176_TX_CMD(LPI2C1176_CMD_STOP, 0);
        X.err = (s & LPI2C1176_MSR_NDF) ? 2u : 3u;
        X.phase = PH_ERR; p->MIER = 0; return;
    }

    if (X.phase == PH_WRITE && (s & LPI2C1176_MSR_TDF)) {
        if (X.wi < X.wn) {
            p->MTDR = LPI2C1176_TX_CMD(LPI2C1176_CMD_TXD, X.wr[X.wi++]);
        } else if (X.rn) {                                   /* -> repeated-START read */
            p->MTDR = LPI2C1176_TX_CMD(LPI2C1176_CMD_START,
                                       (uint32_t)(X.addr << 1) | 1u);
            p->MTDR = LPI2C1176_TX_CMD(LPI2C1176_CMD_RXD, (uint8_t)(X.rn - 1));
            p->MIER = MIER_RDIE | MIER_SDIE | MIER_NDIE | MIER_ALIE | MIER_FEIE;
            X.phase = PH_READ;
        } else {                                             /* write-only STOP */
            p->MTDR = LPI2C1176_TX_CMD(LPI2C1176_CMD_STOP, 0);
            p->MIER = MIER_SDIE | MIER_NDIE | MIER_ALIE | MIER_FEIE;
            X.phase = PH_STOP;
        }
        return;
    }

    if (X.phase == PH_READ && (s & LPI2C1176_MSR_RDF)) {
        uint32_t r = p->MRDR;
        if (!(r & LPI2C1176_MRDR_RXEMPTY) && X.ri < X.rn) X.rd[X.ri++] = (uint8_t)r;
        if (X.ri >= X.rn) {
            p->MTDR = LPI2C1176_TX_CMD(LPI2C1176_CMD_STOP, 0);
            p->MIER = MIER_SDIE | MIER_NDIE | MIER_ALIE | MIER_FEIE;
            X.phase = PH_STOP;
        }
        return;
    }

    if (X.phase == PH_STOP && (s & LPI2C1176_MSR_SDF)) {
        p->MSR = LPI2C1176_MSR_SDF | LPI2C1176_MSR_EPF;
        X.phase = PH_DONE; p->MIER = 0;
    }
}
```

- [ ] **Step 3: Add the kick helper (starts the transaction, waits bounded)**

```c
static uint32_t isr_xfer_run(lpi2c1176_regs_t *p, uint8_t addr,
                             const uint8_t *wr, uint32_t wn,
                             uint8_t *rd, uint32_t rn)
{
    X.p = p; X.addr = addr; X.wr = wr; X.wn = wn; X.wi = 0;
    X.rd = rd; X.rn = rn; X.ri = 0; X.err = 0; X.phase = PH_WRITE;
    p->MSR = p->MSR;                                          /* clear stale flags */
    p->MIER = MIER_TDIE | MIER_NDIE | MIER_ALIE | MIER_FEIE;  /* write phase */
    p->MTDR = LPI2C1176_TX_CMD(LPI2C1176_CMD_START,
                               (uint32_t)(addr << 1) | 0u);   /* START + addr(W) */
    for (uint32_t g = 0; g < 8000000u; g++)                   /* bounded: no hang */
        if (X.phase == PH_DONE || X.phase == PH_ERR) break;
    return X.err;
}
```

- [ ] **Step 4: Rewrite `main()` — enable IRQs, run the ISR-driven ID read, stream tokens**

```c
int main(void)
{
    lpi2c1176_begin(LPI2C5, &lpi2c5_hw, 100000u);           /* clock+pins (shared, HW-verified) */
    uint32_t mcr   = LPI2C5->MCR & LPI2C1176_MCR_MEN;
    uint32_t lpcg  = *lpi2c5_hw.lpcg;
    uint32_t croot = *lpi2c5_hw.clock_root;

    NVIC_ISER1 = (1u << (IRQ_LPI2C5 - 32));                  /* enable IRQ 36 */
    __asm volatile ("cpsie i" ::: "memory");                /* reset handler left IRQs masked */

    /* interrupt-driven R15 ID read: write [0x00,0x0F], repeated-START, read 2 */
    static const uint8_t reg_addr[2] = { 0x00u, 0x0Fu };
    uint8_t rd[2] = { 0, 0 };
    (void)isr_xfer_run(LPI2C5, WM8962_ADDR, reg_addr, 2, rd, 2);
    uint32_t rdv = ((uint32_t)rd[0] << 8) | rd[1];

    mu_send(0, X.irqcnt);
    mu_send(0, mcr);
    mu_send(0, lpcg);
    mu_send(0, croot);
    mu_send(0, rdv);
    mu_send(0, 1u);                                          /* done */
    for (;;) {}
}
```

- [ ] **Step 5: Build + run the gate — expect GREEN**

```bash
cd ~/Development/rt1170/evkb/cm4_wire_int_master_test
cmake --build build >/dev/null && sh run_qemu.sh
```

Expected: `PASS: …`, `irqcnt=` non-zero, `mcr=00000001`, `rdv=00000000` (stub), `WIRE_INT_MASTER_CM4=PASS`. If `irqcnt=00000000`: the split IRQ or NVIC enable or vector slot is wrong — check Task 2/4 and `NVIC_ISER1` bit. If it hangs at a token: the ISR state machine stalled — compare against SDK `LPI2C_MasterTransferHandleIRQ` (likely MIER phase handling).

- [ ] **Step 6: Stabilize 3× + save the QEMU transcript**

```bash
for i in 1 2 3; do sh run_qemu.sh | tail -3; done
cp cm4_wire_int_master.uart transcript_qemu.txt
```

Expected: identical PASS each run.

- [ ] **Step 7: Commit (GREEN)**

```bash
cd ~/Development/rt1170/evkb
git add cm4_wire_int_master_test
git commit -m "cm4_wire_int_master_test: interrupt-driven CM4 LPI2C5 master (Phase 4.1, GREEN)

CM4 NVIC-enables IRQ 36 and services LPI2C5 in its own ISR (fresh state
machine, validated vs SDK LPI2C_MasterTransferHandleIRQ) to read the WM8962
R15 ID.  QEMU gate green + stable 3x (irqcnt>0, rdv=0000 stub).  Proves the
split IRQ delivers to the CM4 in QEMU; EVKB probe closes silicon routing."
```

---

## Task 6: qemu2 full regression set + checkpatch

- [ ] **Step 1: Rebuild + run both functional suites**

```bash
cd ~/Development/qemu2/build && ninja qemu-system-arm
export QEMU_TEST_QEMU_BINARY=$PWD/qemu-system-arm \
       QEMU_TEST_ARM_GCC=/Applications/ARM_10/bin/arm-none-eabi-gcc \
       MESON_BUILD_ROOT=$PWD \
       PYTHONPATH=$PWD/../tests/functional:$PWD/../python
./pyvenv/bin/python3 ../tests/functional/arm/test_imxrt1170.py
./pyvenv/bin/python3 ../tests/functional/arm/test_imxrt1062.py
```

Expected: both `OK`.

- [ ] **Step 2: dualcore_mu_test transcript diff clean**

```bash
cd ~/Development/rt1170/evkb/dualcore_mu_test && sh run_qemu.sh
diff <(tr -d '\r\0' < dualcore_mu.uart) <(tr -d '\r\0' < transcript_qemu.txt)
```

Expected: no diff (the MU path is untouched by the split).

- [ ] **Step 3: checkpatch the qemu2 diff**

```bash
cd ~/Development/qemu2 && git diff HEAD~2 | ./scripts/checkpatch.pl --no-signoff -
```

Expected: `total: 0 errors, 0 warnings` (fix any style nits inline, amend the Task 2 commit).

---

## Task 7: License audit

**Files:** Modify `evkb/tools/license-audit.sh`

- [ ] **Step 1: Add the gate to `GATES`**

Append `cm4_wire_int_master_test:cm4_wire_int_master_test` to the `GATES` string (`:60`).

- [ ] **Step 2: Run the audit**

Run: `cd ~/Development/rt1170/evkb && sh tools/license-audit.sh`
Expected: `LICENSE-AUDIT: PASS` (the fresh ISR master is author-original C against the RM; `main_cm4.c` provenance header names the SDK validation source + the WM8962 fact — no copied logic; the `-MMD` depfile walk now covers `main_cm4.c` + `startup_cm4.S`).

- [ ] **Step 3: Commit**

```bash
cd ~/Development/rt1170/evkb
git add tools/license-audit.sh
git commit -m "license-audit: cover cm4_wire_int_master_test (Phase 4.1)"
```

---

## Task 8: EVKB probe (wiring-free) — silicon-truth close

**Files:** Create `evkb/cm4_wire_int_master_test/transcript_hw_evkb.txt`, `README.md`; update roadmap.

- [ ] **Step 1: Add the provenance/probe header + README**

Write `cm4/main_cm4.c`'s top comment: fresh ISR master (validated vs SDK `fsl_lpi2c.c`), shared `lpi2c1176_begin` for clock/pins, WM8962 R15=0x6243 fact (Linux, no code), and the circular-pass note (QEMU delivers the split on model wiring; only HW proves silicon routing). Write `README.md` with the token table + the `rdv` world-split (`0x0000` QEMU / `0x6243` HW) + wiring-free procedure.

- [ ] **Step 2: Flash + clean boot (uncontaminated), capture VCOM**

```bash
# reader first (survives the reset's USB re-enum)
python3 ~/Development/rt1170/rt1170-console.py /dev/cu.usbmodem<PORT> 115200 > /tmp/hwint.uart &
/Applications/LinkServer_26.6.137/LinkServer flash MIMXRT1176:MIMXRT1170-EVKB load build/cm4_wire_int_master_test.elf
sleep 3
/Applications/LinkServer_26.6.137/LinkServer probe 5DQ2DDHVWO5EI \
    runscript ~/Development/rt1170/evkb/dualcore_mu_test/clean_boot.scp
```

Confirm the clean-boot snapshot (`SCR=0`, `STAT_M4=1` held, `MUA_SR=0x00F00200`) — uncontaminated.

- [ ] **Step 3: Verify the un-fakeable silicon result**

Strip leading nulls (console reconnect), then:

```bash
tr -d '\r\0' < /tmp/hwint.uart | grep -E "irqcnt|mcr|rdv|WIRE_INT_MASTER_CM4"
```

Expected: `irqcnt` **> 0**, `mcr=00000001`, **`rdv=00006243`** (real WM8962 ID via the CM4 ISR), `WIRE_INT_MASTER_CM4=PASS`. This is the proof the split routes the LPI2C5 IRQ to the CM4 NVIC on silicon: a stuck/misrouted IRQ can't produce `irqcnt>0` *and* the correct ID.

- [ ] **Step 4: Confirm asserted tokens byte-identical to QEMU (except `rdv`)**

```bash
cd ~/Development/rt1170/evkb/cm4_wire_int_master_test
# clean the captured HW uart into transcript_hw_evkb.txt (raw CRLF kept), then:
diff <(grep -vE '^rdv=' transcript_qemu.txt) <(tr -d '\0' < transcript_hw_evkb.txt | grep -vE '^rdv=')
```

Expected: no diff on the asserted tokens; `rdv` differs by design (`0000`/`6243`).

- [ ] **Step 5: Update the roadmap + commit**

Append a Session-log entry to `.claude/skills/cm4-bringup/references/cm4-roadmap.md`: Phase 4 opened; foundation (split-IRQ helper, LPSPI1/LPI2C5/16-eDMA) landed; **4.1 ★★HW-VERIFIED** (irqcnt>0, rdv=6243, byte-identical asserted tokens); flip the "Deferred beyond Phase 3 → Interrupt/DMA" item to "Phase 4 in progress." Note 4.2 (Wire-slave) is next.

```bash
cd ~/Development/rt1170/evkb
git add cm4_wire_int_master_test/transcript_hw_evkb.txt cm4_wire_int_master_test/README.md \
        .claude/skills/cm4-bringup/references/cm4-roadmap.md
git commit -m "cm4_wire_int_master_test: HW-VERIFIED on EVKB (Phase 4.1 complete)

Wiring-free clean boot: irqcnt>0 + rdv=00006243 (real WM8962 ID read by the
CM4's own LPI2C5 ISR).  Closes the split-IRQ silicon-routing trigger.
Asserted tokens byte-identical to QEMU; roadmap updated."
```

---

## Self-Review

**Spec coverage (foundation + 4.1 scope):**
- Split-IRQ helper, targeted to LPSPI1/LPI2C5/16 eDMA, mirroring `gpio13_or` → Tasks 1-2. ✓
- 4.1 CM4 self-configures LPI2C5 (reuse `lpi2c1176_begin`) + fresh ISR master + `irqcnt`/`rdv` tokens → Tasks 3-5. ✓
- QEMU regression once (qemu2 touched) + checkpatch → Task 6. ✓
- License audit extended same change → Task 7. ✓
- Wiring-free EVKB probe, `rdv` world-split, byte-identical asserted tokens, roadmap update → Task 8. ✓
- `EventResponder` excluded / direct flag: the ISR sets `X.phase`/`X.irqcnt` directly. ✓
- OCRAM-not-DTCM: N/A for 4.1 (no DMA); applies to 4.3/4.4. ✓
- 4.2–4.4: out of this plan's scope by design (their own plans). ✓

**Placeholder scan:** `2026-07-..` in two qemu2 code comments is the probe-date convention (filled when Task 8 runs) — acceptable, matches `silicon-truth-loop.md`. `<PORT>`/`5DQ2DDHVWO5EI` are the operator's device IDs (from `rt1170-flash.sh`). No "TBD/TODO/implement later".

**Type/name consistency:** `cm4_wire_int_master` (image symbol, matches `teensy_add_cm4_image` name + the `.h`), `WIRE_INT_MASTER_CM4=PASS`, `CM4WIREINT-GATE/-DONE`, `isr_xfer_t X`, `LPI2C5_IRQHandler` (vector + C), `IRQ_LPI2C5=36`/`NVIC_ISER1` bit `(36-32)=4` — consistent across Tasks 3-5 and `run_qemu.sh`. `fsl_imxrt1170_connect_irq_both` signature matches its 3 call sites.

**Known risk to watch at execution (flagged, not a placeholder):** the fresh ISR master is the only new logic — Step 5.2 says validate its MIER/phase handling against SDK `LPI2C_MasterTransferHandleIRQ`; the gate (Step 5.5) and the 3× stability (5.6) are the oracles. If QEMU's LPI2C completes a whole transfer without re-raising TDF/RDF between commands, `irqcnt` may be small (but still >0) — the gate only requires >0, and the EVKB run exercises the real per-event cadence.
