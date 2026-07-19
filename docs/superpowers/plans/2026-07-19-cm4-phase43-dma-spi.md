# CM4 Phase 4.3 — DMA SPI Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** The CM4 self-configures LPSPI1 and runs full-duplex **DMA** self-loopback (the SDO→SDI / MISO→MOSI jumper) in two stages — `STAGE_BLOCKING` (poll the RX channel DONE) and `STAGE_ASYNC` (the RX major-complete interrupt fires on the **CM4's own NVIC** via the eDMA split-IRQ, setting a flag) — proving the CM4 owns an eDMA-driven peripheral end to end.

**Architecture:** Per the approved Phase 4 spec §4.3. Two eDMA channels (RX ch0: `RDR`→rxbuf, DMAMUX source `LPSPI1_RX`=36; TX ch1: txbuf→`TDR`, source `LPSPI1_TX`=37), 8-bit minor loops, `disableOnCompletion`. The RX completion is the transfer's completion (RDR drains last). The DMA path is distilled from `SPI.cpp::startDMA`/`dma_rxisr` (HW-verified) into direct TCD/DMAMUX register writes — **no `DMAChannel` class, no `EventResponder`**; `dma_rxisr` becomes a direct `dmairq++` flag. **The eDMA is a system-bus master and cannot reach either core's private TCM**, so the CM4's DMA buffers live in **OCRAM2 (`0x202C0000`, 512 K, unused by the CM7 gate)** via a new linker region — this is the one genuinely new piece beyond the 3.1 CM4 SPI + the landed eDMA split.

**No qemu2 delta:** the 16-line eDMA split is already in the foundation (4.1), and the LPSPI 8-bit-DMA `min_access_size` fix is already in qemu2 (the CM7 `spi_dma_test` passes). eDMA channel `n` → NVIC IRQ `n` (n and n+16 share; error IRQ = 16), so RX on channel 0 → **IRQ 0** → CM4 vector index 16, `NVIC_ISER0` bit 0.

**SPI silicon-truth (world-split, same as 3.1):** the qemu2 `ssi-loopback` child echoes on `CR.MEN` alone — it ignores the clock gate, clock root, and pin mux. So `rx==tx` in QEMU proves only the register/TCD/DMA sequence; the **real SDO→SDI jumper on hardware** is what proves the CM4 ungated the clock, muxed the pins, and drove a real SCK through DMA. `dmairq>0` is the isolated split-IRQ proof.

**Tech stack:** bare-metal C (CM4), teensy-cmake-macros dual-target build, qemu2 `mimxrt1170-evk`, gate-lib.sh, LinkServer + pyserial + `clean_boot.scp`.

**Repos touched:** `~/Development/rt1170/evkb` only (`git -C evkb`). The shared `lpspi1176` core in `~/Development/SPI` is consumed unchanged (no new API needed — polled `lpspi1176_begin` + direct DMA register writes).

---

### Task 1: Scaffold `cm4_spi_dma_test` (RED)

**Files:**
- Create: `evkb/cm4_spi_dma_test/CMakeLists.txt`
- Create: `evkb/cm4_spi_dma_test/toolchain/rt1170-evkb.toolchain.cmake` (copy from `cm4_spi_test/toolchain/`)
- Create: `evkb/cm4_spi_dma_test/cm4/cm4.ld` (copy from `cm4_spi_test/cm4/cm4.ld`, then add OCRAM2 — Step 3)
- Create: `evkb/cm4_spi_dma_test/cm4/startup_cm4.S`
- Create: `evkb/cm4_spi_dma_test/cm4/main_cm4.c` (READY-only stub — Task 2 replaces it)
- Create: `evkb/cm4_spi_dma_test/cm4_spi_dma_test.cpp` (CM7 reporter)
- Create: `evkb/cm4_spi_dma_test/run_qemu.sh`

- [ ] **Step 1: Copy the toolchain + linker verbatim from `cm4_spi_test`**

```bash
mkdir -p ~/Development/rt1170/evkb/cm4_spi_dma_test/cm4
cp ~/Development/rt1170/evkb/cm4_spi_test/toolchain/rt1170-evkb.toolchain.cmake ~/Development/rt1170/evkb/cm4_spi_dma_test/toolchain/
cp ~/Development/rt1170/evkb/cm4_spi_test/cm4/cm4.ld ~/Development/rt1170/evkb/cm4_spi_dma_test/cm4/
```

- [ ] **Step 2: Write `CMakeLists.txt`**

```cmake
cmake_minimum_required(VERSION 3.24)
project(cm4_spi_dma_test)

set(TEENSY_VERSION 117 CACHE STRING "")

include(FetchContent)
FetchContent_Declare(teensy_cmake_macros SOURCE_DIR ${CMAKE_CURRENT_LIST_DIR}/../teensy-cmake-macros)
FetchContent_MakeAvailable(teensy_cmake_macros)
include(${teensy_cmake_macros_SOURCE_DIR}/CMakeLists.include.txt)

import_arduino_library(cores ${CMAKE_CURRENT_LIST_DIR}/../cores/imxrt1176)

# CM4 image; consumes the shared LPSPI core (polled begin) from newdigate/SPI.
teensy_add_cm4_image(cm4_spi_dma
    LINKER  ${CMAKE_CURRENT_LIST_DIR}/cm4/cm4.ld
    SOURCES ${CMAKE_CURRENT_LIST_DIR}/cm4/startup_cm4.S
            ${CMAKE_CURRENT_LIST_DIR}/cm4/main_cm4.c
            $ENV{HOME}/Development/SPI/lpspi1176.c
    INCLUDE_DIRS $ENV{HOME}/Development/SPI)

teensy_add_executable(cm4_spi_dma_test cm4_spi_dma_test.cpp)
teensy_target_link_libraries(cm4_spi_dma_test cores)
target_link_libraries(cm4_spi_dma_test.elf stdc++)
teensy_target_link_cm4_image(cm4_spi_dma_test cm4_spi_dma)
```

- [ ] **Step 3: Add the OCRAM2 DMA-buffer region to `cm4/cm4.ld`**

In the `MEMORY { ... }` block, after the `DTCM` line add:

```
    OCRAM2 (rw) : ORIGIN = 0x202C0000, LENGTH = 512K
```

(OCRAM2 is system-visible RAM the eDMA can reach; OCRAM M4 `0x20200000` holds the staged CM4 image and OCRAM1 `0x20240000` is the CM7 gate's heap — OCRAM2 is free. Per RM system map lines 2128-2130.)

Then add a `.dmabuf` output section. After the last `> DTCM` section and before any `/DISCARD/`, add:

```
    .dmabuf (NOLOAD) : {
        . = ALIGN(32);
        *(.dmabuf)
        . = ALIGN(32);
    } > OCRAM2
```

(NOLOAD: the buffers are written at runtime, no initializers in the loadable image — keeps the `.bin` unchanged in size. 32-byte align is friendly to the eDMA/cache line.)

- [ ] **Step 4: Write `cm4/startup_cm4.S`**

Copy `cm4_spi_test/cm4/startup_cm4.S` and change ONLY the vector-table body + header comment: place `DMA_CH0_IRQHandler` at external IRQ 0 (vector index 16) and keep `MU_IRQHandler` at IRQ 118.

```asm
    /* external IRQs 0..117 -> vector index 16..133 */
    .word DMA_CH0_IRQHandler    /* index 16: external IRQ 0 (eDMA ch0 complete — RX) */
    .rept 117                   /* IRQ 1..117 */
    .word Default_Handler
    .endr
    .word MU_IRQHandler         /* 134: external IRQ 118 (MU) */
```

(Arithmetic: 16 + 1 + 117 = 134 ✓. Reset handler / VTOR / FPU / `.data`/`.bss` copy stay byte-identical to `cm4_spi_test`'s.)

- [ ] **Step 5: Write the stub `cm4/main_cm4.c` (RED — sends READY then parks)**

```c
/* Phase 4.3 scaffold stub: sends READY, parks. Task 2 replaces this with the
 * real DMA SPI firmware. */
#include <stdint.h>

#define MUB_BASE   0x40C4C000u
#define MUB_TR(n)  (*(volatile uint32_t *)(MUB_BASE + 0x00u + ((n) << 2)))
#define MUB_SR     (*(volatile uint32_t *)(MUB_BASE + 0x20u))
#define SR_TE(n)   (1u << (23 - (n)))

void DMA_CH0_IRQHandler(void) {}
void SysTick_Handler(void) {}
void MU_IRQHandler(void) {}

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

- [ ] **Step 6: Write the CM7 reporter `cm4_spi_dma_test.cpp`**

```cpp
/*
 * cm4_spi_dma_test — Phase 4.3: the CM4 self-configures LPSPI1 and runs
 * full-duplex DMA self-loopback (SDO->SDI jumper) in two stages: STAGE_BLOCKING
 * (poll the RX eDMA channel DONE) and STAGE_ASYNC (the RX major-complete IRQ
 * fires on the CM4's OWN NVIC via the eDMA split-IRQ — dmairq>0 is the isolated
 * routing proof). The CM7 only boots the CM4 image and reports MU tokens.
 *
 * Silicon-truth (world-split, same as cm4_spi_test 3.1): the qemu2 ssi-loopback
 * echoes on CR.MEN ALONE, so rxb/rxa==1 in QEMU proves only the TCD/DMA
 * sequence; the real SDO->SDI jumper on HW proves the CM4 ungated the clock +
 * muxed the pins + drove a real DMA-fed SCK.
 *
 * Tokens (MU ch0, fixed order after READY):
 *   ready  = CAFE0001   CM4 alive
 *   cr     = 00000001   LPSPI CR.MEN — master enabled
 *   cfgr1  = 00000001   CFGR1.MASTER
 *   lpcg   = ........   CCM_LPCG104 readback (informative)
 *   croot  = ........   CCM_CLOCK_ROOT43 readback (informative)
 *   rxb    = 00000001   STAGE_BLOCKING rx==tx (poll DONE)
 *   dmairq = >0         CM4 serviced the eDMA RX completion IRQ (split proof)
 *   rxa    = 00000001   STAGE_ASYNC rx==tx (interrupt-driven)
 *   done   = 00000001   CM4 sequence completed
 * SPI_DMA_CM4=PASS requires cr=1, cfgr1=1, rxb=1, dmairq>0, rxa=1, done=1.
 */
#include "Arduino.h"
#include "core_pins.h"
#include "HardwareSerial.h"
#include "Multicore.h"
#include "MessagingUnit.h"
#include "cm4_spi_dma.h"   /* generated by teensy_add_cm4_image */

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
    Serial1.println("CM4SPIDMA-GATE v1");

    MU.begin();
    Multicore.begin(cm4_spi_dma, sizeof(cm4_spi_dma));

    static const char *labels[9] =
        { "ready", "cr", "cfgr1", "lpcg", "croot", "rxb", "dmairq", "rxa", "done" };
    uint32_t v[9];
    bool ok = true;
    for (int i = 0; i < 9; i++) {
        if (wait_recv(0, &v[i])) phex(labels[i], v[i]);
        else { ptimeout(labels[i]); v[i] = 0xFFFFFFFFu; ok = false; }
    }

    bool pass = ok
        && v[0] == 0xCAFE0001u
        && v[1] == 0x1u          /* cr.MEN */
        && v[2] == 0x1u          /* cfgr1.MASTER */
        && v[5] == 0x1u          /* rxb: blocking rx==tx */
        && v[6] != 0x0u          /* dmairq: CM4 took the eDMA completion IRQ */
        && v[7] == 0x1u          /* rxa: async rx==tx */
        && v[8] == 0x1u;         /* done */
    /* lpcg/croot printed for HW diagnosis; not asserted. */
    Serial1.println(pass ? "SPI_DMA_CM4=PASS" : "SPI_DMA_CM4=FAIL");
    Serial1.println("CM4SPIDMA-DONE");
}

void loop() {}
```

- [ ] **Step 7: Write `run_qemu.sh`** (copy `cm4_spi_test/run_qemu.sh`, change names + checks)

```sh
#!/bin/sh
# QEMU gate for Phase-4.3: the CM4 self-configures LPSPI1 and runs full-duplex
# DMA self-loopback in two stages (blocking poll + async IRQ on the CM4's own
# NVIC via the eDMA split). QEMU's ssi-loopback echoes on CR.MEN alone, so
# rxb/rxa=1 proves the TCD/DMA sequence; the real SDO->SDI jumper on HW proves
# clock+pins+SCK. dmairq>0 is the isolated split-IRQ proof.
set -e
QEMU=~/Development/rt1170/evkb/tools/qrun
DIR=$(cd "$(dirname "$0")" && pwd)
. ~/Development/rt1170/evkb/tools/gate-lib.sh
gate_init
ELF="$DIR/build/cm4_spi_dma_test.elf"
OUT="$DIR/cm4_spi_dma.uart"
rm -f "$OUT"
"$QEMU" -M mimxrt1170-evk -global fsl-imxrt1170.boot-xip=on -kernel "$ELF" \
    -display none -serial file:"$OUT" -d guest_errors -D "$DIR/cm4_spi_dma.dbg" &
P=$!; gate_pid $P
for _ in $(seq 1 40); do
    [ -f "$OUT" ] && grep -q "CM4SPIDMA-DONE" "$OUT" 2>/dev/null && break
    sleep 0.25
done
kill $P 2>/dev/null; wait $P 2>/dev/null || true

echo "==== captured UART ===="; [ -f "$OUT" ] && cat "$OUT" || echo "(no UART output)"

fail=0
check() {
    if grep -q "^$1" "$OUT"; then echo "PASS: $1"; else echo "FAIL: expected $1"; fail=1; fi
}
grep -q "CM4SPIDMA-GATE v1" "$OUT" || { echo "FAIL: banner missing"; exit 1; }
check "ready=CAFE0001"
check "cr=00000001"
check "cfgr1=00000001"
check "rxb=00000001"
check "rxa=00000001"
check "done=00000001"
grep -q "^dmairq=00000000" "$OUT" && { echo "FAIL: dmairq is 0 (no CM4 eDMA IRQ)"; fail=1; }
check "SPI_DMA_CM4=PASS"
grep -q "CM4SPIDMA-DONE" "$OUT" || { echo "FAIL: DONE missing"; fail=1; }

if [ $fail -eq 0 ]; then
    echo "PASS: CM4 DMA SPI verified in QEMU"
else
    echo "GATE FAILED"; exit 1
fi
```

`chmod +x run_qemu.sh`. Invoke as `./run_qemu.sh` (never `sh run_qemu.sh`).

- [ ] **Step 8: Build + run — expect RED**

```bash
cd ~/Development/rt1170/evkb/cm4_spi_dma_test
cmake -B build -DCMAKE_TOOLCHAIN_FILE=toolchain/rt1170-evkb.toolchain.cmake >/dev/null && cmake --build build 2>&1 | tail -3
./run_qemu.sh
```

Expected: build clean; gate FAILS with `ready=CAFE0001` present but `cr=TIMEOUT`/`dmairq=TIMEOUT` — proving reporter + MU + image + gate plumbing work and only the real firmware is missing.

- [ ] **Step 9: Commit**

```bash
cd ~/Development/rt1170/evkb && git add cm4_spi_dma_test
git commit -m "cm4_spi_dma_test: scaffold Phase 4.3 gate (RED) — DMA SPI, OCRAM2 buffer region"
```

---

### Task 2: CM4 DMA SPI firmware (GREEN)

**Files:**
- Modify: `evkb/cm4_spi_dma_test/cm4/main_cm4.c` (replace the stub)

- [ ] **Step 1: Write the real `main_cm4.c`**

```c
/* cm4_spi_dma_test CM4 firmware (Phase 4.3): the CM4 SELF-CONFIGURES LPSPI1 via
 * the shared lpspi1176 core (polled begin), then runs full-duplex eDMA
 * self-loopback (SDO->SDI jumper) with 2 channels distilled from SPI.cpp's
 * startDMA/dma_rxisr — direct TCD/DMAMUX writes, NO DMAChannel/EventResponder.
 *   RX ch0: RDR(8-bit) -> rxbuf, DMAMUX src LPSPI1_RX=36 (its completion is the
 *           transfer's completion — RDR drains last)
 *   TX ch1: txbuf -> TDR(8-bit), DMAMUX src LPSPI1_TX=37
 * STAGE_BLOCKING polls the RX channel CSR.DONE; STAGE_ASYNC arms RX CSR.INTMAJOR
 * and NVIC-enables IRQ 0 (eDMA ch0 completion, on the CM4's OWN NVIC via the
 * qemu2 split-IRQ) so DMA_CH0_IRQHandler sets dmairq — the isolated routing
 * proof. Buffers live in OCRAM2 (.dmabuf): the eDMA is a system-bus master and
 * cannot reach the CM4's private DTCM.
 *
 * Silicon-truth: the qemu2 ssi-loopback echoes on CR.MEN alone, so rx==tx in
 * QEMU proves only the TCD/DMA sequence; the real SDO->SDI jumper on hardware
 * proves clock+pins+SCK. Public-domain scaffolding (N. Newdigate); shared-core
 * register logic MIT (newdigate/SPI). */
#include <stdint.h>
#include "lpspi1176.h"

/* LPSPI1 + its CCM/IOMUXC instance addresses (imxrt1176.h values; verbatim from
 * cm4_spi_test). */
#define LPSPI1 ((lpspi1176_regs_t *)0x40114000u)
static const lpspi1176_hw_t lpspi1_hw = {
    .lpcg = (volatile uint32_t *)0x40CC6D00u,          /* CCM_LPCG104_DIRECT */
    .clock_root = (volatile uint32_t *)0x40CC1580u,    /* CCM_CLOCK_ROOT43_CONTROL */
    .clock_root_val = 0u,                              /* mux0 OSC24M div1 -> 24 MHz */
    .func_clock = 24000000u,
    .sck_mux = (volatile uint32_t *)0x400E817Cu, .sck_mux_val = 0u,   /* GPIO_AD_28 ALT0 */
    .sck_pad = (volatile uint32_t *)0x400E83C0u,
    .sck_select = (volatile uint32_t *)0x400E85D0u, .sck_select_val = 1u,
    .sdo_mux = (volatile uint32_t *)0x400E8184u, .sdo_mux_val = 0u,   /* GPIO_AD_30 ALT0 */
    .sdo_pad = (volatile uint32_t *)0x400E83C8u,
    .sdo_select = (volatile uint32_t *)0x400E85D8u, .sdo_select_val = 1u,
    .sdi_mux = (volatile uint32_t *)0x400E8188u, .sdi_mux_val = 0u,   /* GPIO_AD_31 ALT0 */
    .sdi_pad = (volatile uint32_t *)0x400E83CCu,
    .sdi_select = (volatile uint32_t *)0x400E85D4u, .sdi_select_val = 1u,
    .pad_ctl_val = 0x0Cu,                              /* DSE set */
};

/* ---- eDMA (base 0x40070000; same IP as Teensy 4, relocated) ---- */
#define DMA_CR      (*(volatile uint32_t *)0x40070000u)
#define DMA_SERQ    (*(volatile uint8_t  *)0x4007001Bu)   /* set enable request (channel #) */
#define DMA_CINT    (*(volatile uint8_t  *)0x4007001Fu)   /* clear interrupt (channel #) */
#define DMA_CR_GRP1PRI (1u << 10)
#define DMA_CR_EMLM    (1u << 7)
#define DMA_CR_EDBG    (1u << 1)
#define CCM_LPCG22_DIRECT (*(volatile uint32_t *)0x40CC62C0u)  /* eDMA clock gate */
#define DMAMUX_CHCFG(ch)  (*(volatile uint32_t *)(0x40074000u + (ch) * 4u))
#define DMAMUX_ENBL       (1u << 31)
#define SRC_LPSPI1_RX     36u
#define SRC_LPSPI1_TX     37u
#define TCD_CSR_DONE      0x0080u
#define TCD_CSR_DREQ      0x0008u   /* disable channel request on major-loop completion */
#define TCD_CSR_INTMAJOR  0x0002u
#define RX_CH  0u
#define TX_CH  1u
#define IRQ_DMA_RX  0u             /* channel 0 -> NVIC IRQ 0 */
#define NVIC_ISER0  (*(volatile uint32_t *)0xE000E100u)   /* IRQ 0..31 */
#define DMA_GUARD   4000000u

/* eDMA TCD — exact hardware layout (matches cores DMAChannel TCD_t, 32 bytes). */
typedef struct __attribute__((packed, aligned(4))) {
    const volatile void *volatile SADDR;   /* 0x00 */
    int16_t  SOFF;                          /* 0x04 */
    uint16_t ATTR;                          /* 0x06 */
    uint32_t NBYTES;                        /* 0x08 */
    int32_t  SLAST;                         /* 0x0C */
    volatile void *volatile DADDR;          /* 0x10 */
    int16_t  DOFF;                          /* 0x14 */
    uint16_t CITER;                         /* 0x16 */
    int32_t  DLASTSGA;                      /* 0x18 */
    volatile uint16_t CSR;                  /* 0x1C */
    uint16_t BITER;                         /* 0x1E */
} tcd_t;
#define TCD(ch)  ((volatile tcd_t *)(0x40071000u + (ch) * 0x20u))

/* DMA buffers in OCRAM2 (system-visible; DTCM is DMA-unreachable). */
#define N 16u
static uint8_t txbuf[N] __attribute__((section(".dmabuf")));
static uint8_t rxbuf[N] __attribute__((section(".dmabuf")));

static volatile uint32_t dmairq = 0;
void DMA_CH0_IRQHandler(void)
{
    DMA_CINT = RX_CH;    /* clear ch0 interrupt request */
    dmairq++;
}
void SysTick_Handler(void) {}
void MU_IRQHandler(void) {}

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

static uint32_t g_tcr_base = 0;

/* Program one channel's TCD + DMAMUX. src/dst are byte pointers; soff/doff are
 * per-minor-loop increments (0 for a register, 1 for a buffer). 8-bit ATTR. */
static void dma_setup(uint8_t ch, const volatile void *src, volatile void *dst,
                      int16_t soff, int16_t doff, uint16_t n, uint8_t mux_src, int intr)
{
    volatile tcd_t *t = TCD(ch);
    t->SADDR = src; t->SOFF = soff; t->ATTR = 0u;   /* SSIZE=DSIZE=0 (8-bit) */
    t->NBYTES = 1u; t->SLAST = 0; t->DADDR = dst; t->DOFF = doff;
    t->CITER = n; t->BITER = n; t->DLASTSGA = 0;
    t->CSR = (uint16_t)(TCD_CSR_DREQ | (intr ? TCD_CSR_INTMAJOR : 0u));
    DMAMUX_CHCFG(ch) = ((uint32_t)mux_src & 0x7Fu) | DMAMUX_ENBL;
}

/* One full-duplex DMA transfer. async=1 -> wait on the RX completion IRQ
 * (dmairq); async=0 -> poll the RX channel CSR.DONE. Returns 1 if rx==tx. */
static uint32_t run_dma(int async)
{
    for (uint32_t i = 0; i < N; i++) rxbuf[i] = 0u;

    /* RX drains RDR -> rxbuf (register src no-incr, buffer dst incr). */
    dma_setup(RX_CH, (const volatile void *)&LPSPI1->RDR, rxbuf, 0, 1, (uint16_t)N,
              SRC_LPSPI1_RX, async);
    /* TX feeds txbuf -> TDR (buffer src incr, register dst no-incr). */
    dma_setup(TX_CH, txbuf, (volatile void *)&LPSPI1->TDR, 1, 0, (uint16_t)N,
              SRC_LPSPI1_TX, 0);

    LPSPI1->TCR = (g_tcr_base & ~LPSPI1176_TCR_FRAMESZ(0xFFF)) | LPSPI1176_TCR_FRAMESZ(7); /* 8-bit */
    LPSPI1->FCR = 0u;                                        /* watermark 0 */
    dmairq = 0;
    LPSPI1->DER = LPSPI1176_DER_TDDE | LPSPI1176_DER_RDDE;   /* both DMA requests */
    DMA_SERQ = RX_CH;                                        /* arm RX before TX */
    DMA_SERQ = TX_CH;                                        /* arm TX -> transfer runs */

    uint32_t g = 0;
    if (async) {
        while (dmairq == 0u && ++g < DMA_GUARD) { }
    } else {
        while (!(TCD(RX_CH)->CSR & TCD_CSR_DONE) && ++g < DMA_GUARD) { }
    }
    __asm volatile ("dsb" ::: "memory");
    LPSPI1->DER = 0u;                                        /* stop DMA requests */

    uint32_t okc = 1u;
    for (uint32_t i = 0; i < N; i++) if (rxbuf[i] != txbuf[i]) okc = 0u;
    return okc;
}

int main(void)
{
    /* --- eDMA global init (mirrors DMAChannel::begin) --- */
    CCM_LPCG22_DIRECT |= 1u;                                 /* ungate eDMA clock */
    DMA_CR = DMA_CR_GRP1PRI | DMA_CR_EMLM | DMA_CR_EDBG;

    /* --- self-config LPSPI1 via the shared core (4 MHz) --- */
    lpspi1176_begin(LPSPI1, &lpspi1_hw, 4000000u, &g_tcr_base);
    uint32_t cr    = LPSPI1->CR & LPSPI1176_CR_MEN;          /* -> 1 */
    uint32_t cfgr1 = LPSPI1->CFGR1 & LPSPI1176_CFGR1_MASTER; /* -> 1 */
    uint32_t lpcg  = *lpspi1_hw.lpcg;
    uint32_t croot = *lpspi1_hw.clock_root;

    for (uint32_t i = 0; i < N; i++) txbuf[i] = (uint8_t)(0xA0u ^ (i * 7u));

    /* STAGE_BLOCKING: poll the RX channel DONE (no split needed). */
    uint32_t rxb = run_dma(0);

    /* STAGE_ASYNC: RX major-complete IRQ on the CM4's own NVIC (eDMA split). */
    NVIC_ISER0 = (1u << IRQ_DMA_RX);                         /* enable IRQ 0 */
    __asm volatile ("cpsie i" ::: "memory");                /* reset handler left IRQs masked */
    uint32_t rxa = run_dma(1);

    mu_send(0, 0xCAFE0001u);   /* ready (sent after the work so tokens stream in order) */
    mu_send(0, cr);
    mu_send(0, cfgr1);
    mu_send(0, lpcg);
    mu_send(0, croot);
    mu_send(0, rxb);
    mu_send(0, dmairq);
    mu_send(0, rxa);
    mu_send(0, 1u);            /* done */
    for (;;) {}
}
```

**Note on token order:** the reporter reads `ready` first, so `mu_send(ready)` must be the first send. The work (both stages) runs *before* the sends — the CM7's `wait_recv` loop only starts consuming after `Multicore.begin`, and the MU TX FIFO (4 deep) plus the CM7's prompt draining absorb the ordering; `ready` is still the first word out. If the 4-deep FIFO back-pressures, that's fine — `mu_send` spins on `SR_TE`. (This matches how `cm4_spi_test` streams 9 tokens after its work.)

- [ ] **Step 2: Rebuild + run the gate — expect GREEN**

```bash
cd ~/Development/rt1170/evkb/cm4_spi_dma_test && cmake --build build 2>&1 | tail -3
./run_qemu.sh
```

Expected: `SPI_DMA_CM4=PASS` with `cr=1`, `cfgr1=1`, `rxb=1`, `dmairq` non-zero, `rxa=1`.

**Contingency — QEMU async IRQ:** if `rxa`/`dmairq` don't go green, root-cause with systematic-debugging BEFORE editing (read `cm4_spi_dma.dbg` + the qemu2 eDMA model's INTMAJOR→IRQ path; compare with how `spi_dma_test` (CM7) gets its RX completion). Likely suspects, in order: (a) the RX TCD needs `CSR.INTMAJOR` set *before* `SERQ` (it is); (b) the eDMA raises IRQ line `RX_CH` and the split routes it — confirm the vector index (16+0) and `NVIC_ISER0` bit 0; (c) `DMA_CINT` must clear the request or the ISR re-fires (guarded by `dmairq` counting, not hanging). Do NOT weaken the HW assertions.

- [ ] **Step 3: Run 3× for stability, save the transcript, commit**

```bash
cd ~/Development/rt1170/evkb/cm4_spi_dma_test
for i in 1 2 3; do ./run_qemu.sh >/dev/null 2>&1 && echo "run $i PASS" || echo "run $i FAIL"; done
cp cm4_spi_dma.uart transcript_qemu.txt
cd ~/Development/rt1170/evkb && git add cm4_spi_dma_test
git commit -m "cm4_spi_dma_test: CM4 DMA SPI GREEN in QEMU (Phase 4.3) — blocking + async eDMA, OCRAM2 buffers"
```

---

### Task 3: QEMU regression sanity (no qemu2 delta)

4.3 adds **no qemu2 change** (the eDMA split + LPSPI 8-bit-DMA `min_access_size` are already landed). So the full regression set is NOT re-owed. Do a targeted sanity pass only:

- [ ] **Step 1:** Re-run the CM7 DMA reference gate to confirm the shared eDMA/LPSPI path is unperturbed:

```bash
cd ~/Development/SPI/tests/spi_dma_test && ./run_qemu_spidma.sh
```

Expected: `STAGE_BLOCKING=PASS`, `STAGE_ASYNC=PASS`, `SPI_DMA_ALL=PASS`.

- [ ] **Step 2:** Re-run the 4.1 gate (shares the split-IRQ foundation + build macros):

```bash
cd ~/Development/rt1170/evkb/cm4_wire_int_master_test && ./run_qemu.sh
```

Expected: `WIRE_INT_MASTER_CM4=PASS`.

---

### Task 4: License audit

**Files:**
- Modify: `evkb/tools/license-audit.sh`

- [ ] **Step 1:** Add `cm4_spi_dma_test:cm4_spi_dma_test` to the `GATES` array (same one-line pattern as `cm4_spi_test`).

- [ ] **Step 2:** Run `~/Development/rt1170/evkb/tools/license-audit.sh` — expect PASS (all firmware sources MIT/public-domain; `lpspi1176.c` is MIT).

- [ ] **Step 3:** Commit: `git -C ~/Development/rt1170/evkb add tools/license-audit.sh && git commit -m "license-audit: cover cm4_spi_dma_test (Phase 4.3)"`

---

### Task 5: EVKB probe (HARDWARE — jumper)

**Files:**
- Create: `evkb/cm4_spi_dma_test/README.md` (jumper + procedure)
- Create: `evkb/cm4_spi_dma_test/transcript_hw_evkb.txt` (probe output)
- (a `clean_boot.scp` may be copied from `cm4_spi_test/` if that test carries one)

- [ ] **Step 1: Write `README.md`** covering: the DMA two-stage design (pointing at spec §4.3), the token table, and the wiring: **SDO (`GPIO_AD_30`) → SDI (`GPIO_AD_31`)** loopback jumper (the same MISO→MOSI jumper as Phase 3.1 `cm4_spi_test`). Note the world-split: QEMU `rxb/rxa` is a circular `ssi-loopback` pass; only the jumper proves a real DMA-driven SCK on silicon.

- [ ] **Step 2: Run the probe** (board connected; jumper installed):

```bash
pkill LinkServer; pkill redlinkserv; sleep 1
cd ~/Development/rt1170/evkb/cm4_spi_dma_test
# clean boot (uncontaminated M4-held boot) per the cm4_spi_test precedent, then:
# start the pyserial VCOM reader (115200, gtimeout) on /dev/cu.usbmodem5DQ2DDHVWO5EI3
#   BEFORE resetting (macos-serial-capture memory), then:
/Applications/LinkServer_26.6.137/LinkServer flash MIMXRT1176:MIMXRT1170-EVKB load build/cm4_spi_dma_test.elf
# reset; capture the VCOM.
```

Expected EVKB VCOM: `ready=CAFE0001`, `cr=1`, `cfgr1=1`, `rxb=1`, `dmairq>0`, `rxa=1`, `done=1`, `SPI_DMA_CM4=PASS`. **Un-fakeable:** `rxb==1 && rxa==1` require the tx pattern to return through the physical SDO→SDI jumper via DMA in both stages, and `dmairq>0` requires the CM4 to have taken the eDMA completion IRQ on its own NVIC. Split-not-routed ⇒ `dmairq=0` ⇒ `rxa` guard expires ⇒ FAIL.

**If the probe fails:** systematic-debugging — no fixes without root cause. Candidate first probes: is the jumper on `AD_30↔AD_31`? does `rxb` (blocking, no IRQ) pass while only `rxa` fails (→ IRQ routing, not DMA data)? read the LPSPI/eDMA state via extra MU tokens.

- [ ] **Step 3: Commit** the transcript + README:

```bash
cd ~/Development/rt1170/evkb && git add cm4_spi_dma_test
git commit -m "cm4_spi_dma_test: EVKB probe HW-VERIFIED — CM4 DMA SPI loopback via SDO->SDI jumper (Phase 4.3)"
```

---

### Task 6: Docs close-out

- [ ] **Step 1:** Update `.claude/skills/cm4-bringup/references/cm4-roadmap.md` — Phase 4.3 status + a dated session-log entry (discoveries: the OCRAM2 CM4 DMA-buffer region; any async-IRQ contingency outcome; next = 4.4 DMA Wire).
- [ ] **Step 2:** If anything landed differently from spec §4.3, add an as-landed note to the spec (mirroring 4.1/4.2).
- [ ] **Step 3:** Update the `rt1176-cm4-int-wire-phase4` memory + `MEMORY.md` hook with the 4.3 outcome.
- [ ] **Step 4:** Commit: `git -C ~/Development/rt1170/evkb add -A docs .claude && git commit -m "docs: Phase 4.3 roadmap/spec/memory close-out"`

---

## Self-review (done at plan-writing)

- **Spec coverage (§4.3):** distilled LPSPI1 begin (shared core) + distilled DMA path → Task 2; 2 eDMA channels TX→TDR / RX←RDR via DMAMUX 37/36 → `dma_setup`; DMAMEM buffers in OCRAM → Task 1 Step 3 + `.dmabuf`/OCRAM2; two stages STAGE_BLOCKING (poll DONE, no split) + STAGE_ASYNC (`dma_rxisr`→direct flag, eDMA split) → `run_dma`; tokens `lpcg/croot/rxb/dmairq/rxa/done` → reporter; jumper SDO→SDI probe → Task 5; no qemu2 delta → Task 3 sanity-only. No gaps.
- **Placeholders:** none — every code step carries actual code; the one open behavior (QEMU async IRQ) has a bounded, systematic contingency.
- **Type/name consistency:** `tcd_t` fields match the cores `TCD_t` layout and the 0x40071000+ch*0x20 base; `run_dma(async)` used identically in both stages; token labels match between `main_cm4.c`, the reporter, and `run_qemu.sh` (`ready/cr/cfgr1/lpcg/croot/rxb/dmairq/rxa/done`); image/target names consistent (`cm4_spi_dma`, `cm4_spi_dma_test`); `RX_CH=0`→`IRQ 0`→vector index 16→`NVIC_ISER0` bit 0 consistent across the linker/startup/firmware.
