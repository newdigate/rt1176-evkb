# CM4 Phase 3.3 — Shared C Register/Clock Core Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Extract the HW-verified LPSPI1/LPI2C5 register+clock sequences into one
freestanding C core per library (`SPI/lpspi1176.{h,c}`, `Wire/lpi2c1176.{h,c}`)
that the CM7 C++ classes and the CM4 gate images both compile and call — with
**zero behavior change**, proven by byte-identical gate transcripts.

**Architecture:** Per spec
`evkb/docs/superpowers/specs/2026-07-18-cm4-shared-c-core-consolidation-design.md`.
Shared core bodies = the CM7 logic verbatim (D1–D5 all resolve to it). Hardware
addresses flow in via a C pointer-struct (`*_hw_t`); register blocks via an
offset-asserted overlay (`*_regs_t`) cross-asserted against
`IMXRT_LPSPI_t`/`IMXRT_LPI2C_t` in the library .cpp. `teensy_add_cm4_image`
gains an optional `INCLUDE_DIRS` arg (compile-only `-I`).

**Tech Stack:** freestanding C11 (compiles under CM4 flags `-mcpu=cortex-m4
… -ffreestanding -Wall -Wextra` AND inside the CM7 C++ libraries — so:
`extern "C"` guards + a `static_assert`/`_Static_assert` shim), CMake macros,
QEMU gates via gate-lib.sh.

**Guardrail state (already captured, session scratchpad `baselines/`):**
- 6 gate uarts + stdouts: `cm4_spi`, `cm4_wire`, `spi_loopback`, `spi_dma`,
  `wire_master`, `wire_slave` (all PASS pre-refactor; cm4 uarts ==
  checked-in `transcript_qemu.txt`).
- `cm4bins.pre.md5` + `cm4_dual.cm4.bin.pre` (macro-edit cmp reference):
  `cm4_dual` a064add34b3f69a10155d683cff99745.

**Execution notes (apply to every task):**
- The CM4 custom-command compiles do NOT track header deps for rebuild — after
  editing a shared `.h`, `touch` the CM4 `main_cm4.c`/shared `.c` (or delete the
  gate `build/cm4_*.o*`) before rebuilding.
- Gate runners MUST be invoked as `./run_qemu*.sh` from their own directory
  (gate-lib re-execs `$0` under gtimeout; `sh run_qemu.sh` breaks).
- Repos and commit targets: `evkb/teensy-cmake-macros` (own repo),
  `~/Development/SPI`, `~/Development/Wire`, `evkb` (gates+docs). `git -C <dir>`.
- Do NOT "fix" latent quirks while moving code (e.g. `requestFrom(quantity=0)`
  N-1 wraparound) — byte-identical refactor only; quirks move as-is.

---

### Task 1: `teensy_add_cm4_image` INCLUDE_DIRS (macro repo)

**Files:**
- Modify: `evkb/teensy-cmake-macros/CMakeLists.include.txt:460-513`

- [ ] **Step 1.1: Edit the macro.** In `teensy_add_cm4_image`:

Change the parse line:
```cmake
    cmake_parse_arguments(CM4 "" "LINKER" "SOURCES;INCLUDE_DIRS" ${ARGN})
```

After the `set(_cm4_flags …)` block, add:
```cmake
    # Optional include dirs (e.g. a library's shared C core). Compile-only:
    # images that don't pass INCLUDE_DIRS get byte-identical command lines,
    # so their .cm4.bin cannot change (2B cmp discipline).
    set(_cm4_incs "")
    foreach(_inc ${CM4_INCLUDE_DIRS})
        get_filename_component(_inc_abs "${_inc}" ABSOLUTE)
        list(APPEND _cm4_incs "-I${_inc_abs}")
    endforeach()
```

Change the compile COMMAND (link command unchanged):
```cmake
            COMMAND "${_gcc}" ${_cm4_flags} ${_cm4_incs} -MMD -MF "${_obj}.d"
```

Update the doc comment above the function (line ~448) to:
```cmake
#   teensy_add_cm4_image(<name> LINKER <cm4.ld> SOURCES <a.S> <b.c> ...
#                        [INCLUDE_DIRS <dir> ...])
```

- [ ] **Step 1.2: Prove untouched images are byte-identical.**

```bash
cmake --build ~/Development/rt1170/evkb/cm4_dual_test/build -j8
cmp ~/Development/rt1170/evkb/cm4_dual_test/build/cm4_dual.cm4.bin \
    "$SCRATCH/baselines/cm4_dual.cm4.bin.pre" && echo CM4DUAL-BIN-IDENTICAL
```
Expected: `CM4DUAL-BIN-IDENTICAL`. (If cmake skips recompiling because the
command line is unchanged, force it: delete `build/cm4_dual.*.o*` first — the
point is a fresh compile under the edited macro.)

- [ ] **Step 1.3: Commit (macros repo).**
```bash
git -C ~/Development/rt1170/evkb/teensy-cmake-macros add CMakeLists.include.txt
git -C ~/Development/rt1170/evkb/teensy-cmake-macros commit -m "teensy_add_cm4_image: optional INCLUDE_DIRS (compile-only -I) for shared library C cores (Phase 3.3); absent => byte-identical command lines (cm4_dual .cm4.bin cmp-verified)"
```

---

### Task 2: SPI shared core `lpspi1176.{h,c}` + class delegation

**Files:**
- Create: `~/Development/SPI/lpspi1176.h`
- Create: `~/Development/SPI/lpspi1176.c`
- Modify: `~/Development/SPI/SPIIMXRT1176.h` (struct + includes)
- Modify: `~/Development/SPI/SPIIMXRT1176.cpp` (delegation + instance table)

- [ ] **Step 2.1: Create `lpspi1176.h`** (MIT header comment in the style of
`SPIIMXRT1176.h`, stating: "this project's HW-verified RT1176 LPSPI1 bring-up
(SPIIMXRT1176.cpp, MIT), re-expressed as the single shared C core (Phase 3.3);
consumed by the CM7 SPIClass and the CM4 gate images"):

```c
#ifndef LPSPI1176_H
#define LPSPI1176_H

#include <stdint.h>
#include <stddef.h>

#if defined(__cplusplus)
#define LPSPI1176_ASSERT(c, m) static_assert(c, m)
extern "C" {
#else
#define LPSPI1176_ASSERT(c, m) _Static_assert(c, m)
#endif

/* LPSPI register-block overlay (offsets per RT1170 RM; layout equals the
 * core's IMXRT_LPSPI_t — cross-asserted in SPIIMXRT1176.cpp). */
typedef struct {
	volatile uint32_t VERID;       /* 0x00 */
	volatile uint32_t PARAM;       /* 0x04 */
	volatile uint32_t r08, r0C;
	volatile uint32_t CR;          /* 0x10 */
	volatile uint32_t SR;          /* 0x14 */
	volatile uint32_t IER;         /* 0x18 */
	volatile uint32_t DER;         /* 0x1C */
	volatile uint32_t CFGR0;       /* 0x20 */
	volatile uint32_t CFGR1;       /* 0x24 */
	volatile uint32_t r28, r2C;
	volatile uint32_t DMR0;        /* 0x30 */
	volatile uint32_t DMR1;        /* 0x34 */
	volatile uint32_t r38, r3C;
	volatile uint32_t CCR;         /* 0x40 */
	volatile uint32_t r44[5];
	volatile uint32_t FCR;         /* 0x58 */
	volatile uint32_t FSR;         /* 0x5C */
	volatile uint32_t TCR;         /* 0x60 */
	volatile uint32_t TDR;         /* 0x64 */
	volatile uint32_t r68, r6C;
	volatile uint32_t RSR;         /* 0x70 */
	volatile uint32_t RDR;         /* 0x74 */
} lpspi1176_regs_t;

LPSPI1176_ASSERT(offsetof(lpspi1176_regs_t, CR)    == 0x10, "LPSPI CR");
LPSPI1176_ASSERT(offsetof(lpspi1176_regs_t, DER)   == 0x1C, "LPSPI DER");
LPSPI1176_ASSERT(offsetof(lpspi1176_regs_t, CFGR1) == 0x24, "LPSPI CFGR1");
LPSPI1176_ASSERT(offsetof(lpspi1176_regs_t, CCR)   == 0x40, "LPSPI CCR");
LPSPI1176_ASSERT(offsetof(lpspi1176_regs_t, FCR)   == 0x58, "LPSPI FCR");
LPSPI1176_ASSERT(offsetof(lpspi1176_regs_t, TCR)   == 0x60, "LPSPI TCR");
LPSPI1176_ASSERT(offsetof(lpspi1176_regs_t, TDR)   == 0x64, "LPSPI TDR");
LPSPI1176_ASSERT(offsetof(lpspi1176_regs_t, RSR)   == 0x70, "LPSPI RSR");
LPSPI1176_ASSERT(offsetof(lpspi1176_regs_t, RDR)   == 0x74, "LPSPI RDR");

/* Hardware description: CCM gate/root + the SCK/SDO/SDI pads (RM pin names;
 * the Arduino-facing class maps mosi->SDO, miso->SDI). */
typedef struct {
	volatile uint32_t *lpcg;         /* CCM LPCG DIRECT (write 1 to ungate) */
	volatile uint32_t *clock_root;   /* CCM CLOCK_ROOT CONTROL */
	uint32_t clock_root_val;         /* 0 => mux0 OSC24M div1 -> 24 MHz */
	uint32_t func_clock;             /* resulting functional clock (Hz) */
	volatile uint32_t *sck_mux;  uint32_t sck_mux_val;  volatile uint32_t *sck_pad;
	volatile uint32_t *sck_select;  uint32_t sck_select_val;
	volatile uint32_t *sdo_mux;  uint32_t sdo_mux_val;  volatile uint32_t *sdo_pad;
	volatile uint32_t *sdo_select;  uint32_t sdo_select_val;
	volatile uint32_t *sdi_mux;  uint32_t sdi_mux_val;  volatile uint32_t *sdi_pad;
	volatile uint32_t *sdi_select;  uint32_t sdi_select_val;
	uint32_t pad_ctl_val;            /* one pad config for all three pins */
} lpspi1176_hw_t;

/* CR */
#define LPSPI1176_CR_MEN        (1u << 0)
#define LPSPI1176_CR_RST        (1u << 1)
/* CFGR1 */
#define LPSPI1176_CFGR1_MASTER  (1u << 0)
/* TCR fields */
#define LPSPI1176_TCR_FRAMESZ(n)  ((uint32_t)((n) & 0xFFFu))   /* n = bits-1 */
#define LPSPI1176_TCR_PRESCALE(p) (((uint32_t)(p) & 0x7u) << 27)
#define LPSPI1176_TCR_CPHA      (1u << 30)
#define LPSPI1176_TCR_CPOL      (1u << 31)
#define LPSPI1176_TCR_LSBF      (1u << 23)
/* RSR */
#define LPSPI1176_RSR_RXEMPTY   (1u << 1)
/* DER (DMA enable; used by the CM7 DMA path) */
#define LPSPI1176_DER_TDDE      (1u << 0)
#define LPSPI1176_DER_RDDE      (1u << 1)

#define LPSPI1176_TIMEOUT       100000u

/* Ungate+root the clock, mux the pins, reset the block, master mode,
 * program the divider for clock_hz (writing *tcr_base's PRESCALE), enable. */
void lpspi1176_begin(lpspi1176_regs_t *p, const lpspi1176_hw_t *hw,
                     uint32_t clock_hz, uint32_t *tcr_base);
void lpspi1176_end(lpspi1176_regs_t *p, const lpspi1176_hw_t *hw);
/* CCR.SCKDIV + PRESCALE bits of *tcr_base for the requested SCK
 * (SCK = func_clock / (2^prescale * (SCKDIV+2))). */
void lpspi1176_set_clock_hz(lpspi1176_regs_t *p, uint32_t func_clock,
                            uint32_t clock_hz, uint32_t *tcr_base);
/* Polled full-duplex single frame (framesz = bits-1): load TCR, write TDR,
 * spin on RSR.RXEMPTY, read RDR. 0xFFFFFFFF on timeout. */
uint32_t lpspi1176_transfer_frame(lpspi1176_regs_t *p, uint32_t tcr_base,
                                  uint32_t data, uint32_t framesz);

#if defined(__cplusplus)
}
#endif
#endif /* LPSPI1176_H */
```

- [ ] **Step 2.2: Create `lpspi1176.c`** (same MIT+provenance header). Bodies
are `SPIIMXRT1176.cpp` verbatim modulo names:

```c
#include "lpspi1176.h"

void lpspi1176_set_clock_hz(lpspi1176_regs_t *p, uint32_t func_clock,
                            uint32_t clock_hz, uint32_t *tcr_base)
{
	if (clock_hz == 0u) clock_hz = 1000u;        /* guard divide-by-zero; clamp to slow */
	uint32_t prescale = 0, sckdiv = 0;
	for (prescale = 0; prescale < 8u; prescale++) {
		uint32_t pdiv = 1u << prescale;
		uint32_t denom = pdiv * clock_hz;
		uint32_t div = (func_clock + denom - 1u) / denom;   /* ceil(func/(pdiv*clk)) */
		if (div < 2u) div = 2u;
		sckdiv = div - 2u;
		if (sckdiv <= 255u) break;
	}
	if (prescale > 7u) { prescale = 7u; sckdiv = 255u; }
	uint32_t men = p->CR & LPSPI1176_CR_MEN;
	p->CR = 0u;                                  /* CCR is writable only with MEN=0 */
	p->CCR = (p->CCR & ~0xFFu) | (sckdiv & 0xFFu);
	if (men) p->CR = LPSPI1176_CR_MEN;
	*tcr_base = (*tcr_base & ~(0x7u << 27)) | LPSPI1176_TCR_PRESCALE(prescale);
}

void lpspi1176_begin(lpspi1176_regs_t *p, const lpspi1176_hw_t *hw,
                     uint32_t clock_hz, uint32_t *tcr_base)
{
	*hw->lpcg = 1u;                              /* ungate LPSPI clock */
	*hw->clock_root = hw->clock_root_val;
	*hw->sck_mux = hw->sck_mux_val;  *hw->sck_pad = hw->pad_ctl_val;
	*hw->sdo_mux = hw->sdo_mux_val;  *hw->sdo_pad = hw->pad_ctl_val;
	*hw->sdi_mux = hw->sdi_mux_val;  *hw->sdi_pad = hw->pad_ctl_val;
	*hw->sck_select = hw->sck_select_val;
	*hw->sdo_select = hw->sdo_select_val;
	*hw->sdi_select = hw->sdi_select_val;
	p->CR = LPSPI1176_CR_RST;  p->CR = 0u;       /* reset the block (MEN=0) */
	p->CFGR1 = LPSPI1176_CFGR1_MASTER;           /* master mode (write while MEN=0) */
	*tcr_base = 0u;                              /* MODE0, MSB first */
	lpspi1176_set_clock_hz(p, hw->func_clock, clock_hz, tcr_base);
	p->CR = LPSPI1176_CR_MEN;                    /* enable */
}

void lpspi1176_end(lpspi1176_regs_t *p, const lpspi1176_hw_t *hw)
{
	p->CR = 0u;
	*hw->lpcg = 0u;
}

uint32_t lpspi1176_transfer_frame(lpspi1176_regs_t *p, uint32_t tcr_base,
                                  uint32_t data, uint32_t framesz)
{
	p->TCR = tcr_base | LPSPI1176_TCR_FRAMESZ(framesz);
	p->TDR = data;
	for (uint32_t g = 0; g < LPSPI1176_TIMEOUT; g++) {
		if (!(p->RSR & LPSPI1176_RSR_RXEMPTY)) return p->RDR;
	}
	return 0xFFFFFFFFu;
}
```

- [ ] **Step 2.3: Recompose `SPIIMXRT1176.h`.** Add `#include "lpspi1176.h"`
(after `<EventResponder.h>`). Replace the `SPI_Hardware_t` typedef
(lines 86-103) with:

```cpp
	typedef struct {
		lpspi1176_hw_t hw;                       // shared C core hardware desc
		void (*dma_rxisr)();
		const uint8_t  miso_pin[CNT_MISO_PINS];  // SDI
		const uint8_t  mosi_pin[CNT_MOSI_PINS];  // SDO
		const uint8_t  sck_pin[CNT_SCK_PINS];
		const uint8_t  cs_pin[CNT_CS_PINS];
	} SPI_Hardware_t;
```

Add a private accessor next to `port()`:
```cpp
	lpspi1176_regs_t *lp() { return (lpspi1176_regs_t *)port_addr; }
```

- [ ] **Step 2.4: Delegate in `SPIIMXRT1176.cpp`.**
Delete the local `CR_/CFGR1_/TCR_/RSR_/DER_` macro block and `SPI_TIMEOUT`
(lines 48-65). Add after the includes:

```cpp
#include <stddef.h>

// The shared C core's overlay must equal the core header's (same silicon).
static_assert(offsetof(lpspi1176_regs_t, CR)    == offsetof(IMXRT_LPSPI_t, CR),    "CR");
static_assert(offsetof(lpspi1176_regs_t, DER)   == offsetof(IMXRT_LPSPI_t, DER),   "DER");
static_assert(offsetof(lpspi1176_regs_t, CFGR1) == offsetof(IMXRT_LPSPI_t, CFGR1), "CFGR1");
static_assert(offsetof(lpspi1176_regs_t, CCR)   == offsetof(IMXRT_LPSPI_t, CCR),   "CCR");
static_assert(offsetof(lpspi1176_regs_t, FCR)   == offsetof(IMXRT_LPSPI_t, FCR),   "FCR");
static_assert(offsetof(lpspi1176_regs_t, TCR)   == offsetof(IMXRT_LPSPI_t, TCR),   "TCR");
static_assert(offsetof(lpspi1176_regs_t, TDR)   == offsetof(IMXRT_LPSPI_t, TDR),   "TDR");
static_assert(offsetof(lpspi1176_regs_t, RSR)   == offsetof(IMXRT_LPSPI_t, RSR),   "RSR");
static_assert(offsetof(lpspi1176_regs_t, RDR)   == offsetof(IMXRT_LPSPI_t, RDR),   "RDR");
static_assert(sizeof(lpspi1176_regs_t) == sizeof(IMXRT_LPSPI_t), "LPSPI size");
```

Replace method bodies:
```cpp
void SPIClass::begin() {
	lpspi1176_begin(lp(), &hardware.hw, 4000000u, &tcr_base);   // default 4 MHz
}

void SPIClass::end() { lpspi1176_end(lp(), &hardware.hw); }

void SPIClass::setClockDividerHz(uint32_t clockHz) {
	lpspi1176_set_clock_hz(lp(), hardware.hw.func_clock, clockHz, &tcr_base);
}

uint8_t SPIClass::transfer(uint8_t data) {
	return (uint8_t)lpspi1176_transfer_frame(lp(), tcr_base, data, 7u);
}

uint16_t SPIClass::transfer16(uint16_t data) {
	return (uint16_t)lpspi1176_transfer_frame(lp(), tcr_base, data, 15u);
}
```
(`transfer(void*,size_t)`, `beginTransaction`, `endTransaction`,
`setBitOrder`, `setDataMode` keep their bodies but rename macros:
`TCR_CPOL/TCR_CPHA/TCR_LSBF` → `LPSPI1176_TCR_*`. In `startDMA`, rename
`TCR_FRAMESZ` → `LPSPI1176_TCR_FRAMESZ`, `DER_TDDE|DER_RDDE` →
`LPSPI1176_DER_TDDE|LPSPI1176_DER_RDDE`. Timeout constant is now only used
inside the shared core.)

Replace the instance table (lines 218-228) — same values, `&` addresses,
positional init, shared-desc order (sck, sdo=SDO/AD_30/mosi, sdi=SDI/AD_31/miso):
```cpp
const SPIClass::SPI_Hardware_t SPIClass::spiclass_lpspi1_hardware = {
	{ &CCM_LPCG104_DIRECT, &CCM_CLOCK_ROOT43_CONTROL, 0u, 24000000u,
	  &IOMUXC_SW_MUX_CTL_PAD_GPIO_AD_28, 0x0u, &IOMUXC_SW_PAD_CTL_PAD_GPIO_AD_28,
	    &IOMUXC_LPSPI1_SCK_SELECT_INPUT, 0x1u,
	  &IOMUXC_SW_MUX_CTL_PAD_GPIO_AD_30, 0x0u, &IOMUXC_SW_PAD_CTL_PAD_GPIO_AD_30,
	    &IOMUXC_LPSPI1_SDO_SELECT_INPUT, 0x1u,
	  &IOMUXC_SW_MUX_CTL_PAD_GPIO_AD_31, 0x0u, &IOMUXC_SW_PAD_CTL_PAD_GPIO_AD_31,
	    &IOMUXC_LPSPI1_SDI_SELECT_INPUT, 0x1u,
	  0x0000000Cu },
	SPIClass::dma_rxisr,
	{ 0 }, { 0 }, { 0 }, { 0 },
};
```
Also update the comment above it (field order now sck/sdo/sdi via the shared
desc) and the "Register-op source of truth" mapping comment (lines 37-46) to
point at `lpspi1176.c` as the single sequence home.

- [ ] **Step 2.5: Gate + byte-identity.**
```bash
cd ~/Development/SPI/tests/spi_loopback_test && cmake --build build -j8 && ./run_qemu_spi.sh
cd ~/Development/SPI/tests/spi_dma_test      && cmake --build build -j8 && ./run_qemu_spidma.sh
cmake --build ~/Development/SPI/tests/st7735_test/build -j8
diff ~/Development/SPI/tests/spi_loopback_test/spi.uart    "$SCRATCH/baselines/spi_loopback.uart"
diff ~/Development/SPI/tests/spi_dma_test/spidma.uart      "$SCRATCH/baselines/spi_dma.uart"
```
Expected: both runners PASS, both diffs empty, st7735 builds. (CONFIGURE_DEPENDS
re-globs `lpspi1176.c` into the lib automatically.)

- [ ] **Step 2.6: Commit (SPI repo).**
```bash
git -C ~/Development/SPI add lpspi1176.h lpspi1176.c SPIIMXRT1176.h SPIIMXRT1176.cpp
git -C ~/Development/SPI commit -m "lpspi1176: shared C register/clock core (Phase 3.3); SPIClass delegates; sequences verbatim, transcripts byte-identical (spi_loopback/spi_dma vs pre-refactor baselines)"
```

---

### Task 3: `cm4_spi_test` onto the shared core

**Files:**
- Modify: `evkb/cm4_spi_test/cm4/main_cm4.c`
- Modify: `evkb/cm4_spi_test/CMakeLists.txt:14-17`

- [ ] **Step 3.1: CMakeLists** — extend the image:
```cmake
teensy_add_cm4_image(cm4_spi
    LINKER  ${CMAKE_CURRENT_LIST_DIR}/cm4/cm4.ld
    SOURCES ${CMAKE_CURRENT_LIST_DIR}/cm4/startup_cm4.S
            ${CMAKE_CURRENT_LIST_DIR}/cm4/main_cm4.c
            $ENV{HOME}/Development/SPI/lpspi1176.c
    INCLUDE_DIRS $ENV{HOME}/Development/SPI)
```

- [ ] **Step 3.2: Rewrite `main_cm4.c`.** Keep the file's provenance comment
block (update: "Phase 3.3: the sequence now IS the shared `lpspi1176.c` from
newdigate/SPI — no more keep-in-sync mirror; this file keeps only the MU
scaffolding, the per-instance address table, and the token flow"), the
SILICON TRUTH paragraph, MU defines, `mu_send`, and the handler stubs. Full
new content:

```c
#include <stdint.h>
#include "lpspi1176.h"

/* LPSPI1 + its CCM/IOMUXC instance addresses (imxrt1176.h values; the CM7
 * library binds the same registers via the header macros). */
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

int main(void)
{
    /* --- self-config LPSPI1 via the shared core (default 4 MHz) --- */
    uint32_t tcr_base = 0;
    lpspi1176_begin(LPSPI1, &lpspi1_hw, 4000000u, &tcr_base);

    /* --- config readbacks --- */
    uint32_t cr    = LPSPI1->CR & LPSPI1176_CR_MEN;           /* -> 1 */
    uint32_t cfgr1 = LPSPI1->CFGR1 & LPSPI1176_CFGR1_MASTER;  /* -> 1 */
    uint32_t lpcg  = *lpspi1_hw.lpcg;                         /* informative */
    uint32_t croot = *lpspi1_hw.clock_root;                   /* informative */

    /* --- polled loopback (SDO->SDI jumper) --- */
    uint32_t a = lpspi1176_transfer_frame(LPSPI1, tcr_base, 0xA5u, 7u) & 0xFFu;
    uint32_t b = lpspi1176_transfer_frame(LPSPI1, tcr_base, 0x3Cu, 7u) & 0xFFu;
    uint32_t w = lpspi1176_transfer_frame(LPSPI1, tcr_base, 0xBEEFu, 15u) & 0xFFFFu;
    uint8_t bs[4] = { 0xDEu, 0xADu, 0xBEu, 0xEFu };
    for (int i = 0; i < 4; i++) {
        bs[i] = (uint8_t)(lpspi1176_transfer_frame(LPSPI1, tcr_base, bs[i], 7u) & 0xFFu);
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

- [ ] **Step 3.3: Rebuild + reconfigure.** The SOURCES list changed →
```bash
cd ~/Development/rt1170/evkb/cm4_spi_test && cmake -S . -B build && cmake --build build -j8
```
Expected: configure + build clean (new `cm4_spi.lpspi1176.c.o` appears).

- [ ] **Step 3.4: Gate 3× + byte-identity.**
```bash
cd ~/Development/rt1170/evkb/cm4_spi_test
for i in 1 2 3; do ./run_qemu.sh | tail -1; done
diff cm4_spi.uart transcript_qemu.txt && echo TRANSCRIPT-IDENTICAL
```
Expected: `PASS: CM4 self-configured polled SPI loopback verified in QEMU` ×3,
`TRANSCRIPT-IDENTICAL`.

- [ ] **Step 3.5: Commit (evkb repo).**
```bash
git -C ~/Development/rt1170/evkb add cm4_spi_test/cm4/main_cm4.c cm4_spi_test/CMakeLists.txt
git -C ~/Development/rt1170/evkb commit -m "cm4_spi_test: consume the shared lpspi1176 C core (Phase 3.3); QEMU transcript byte-identical (stable 3x); local sequence mirror deleted"
```

---

### Task 4: Wire shared core `lpi2c1176.{h,c}` + class delegation

**Files:**
- Create: `~/Development/Wire/lpi2c1176.h`
- Create: `~/Development/Wire/lpi2c1176.c`
- Modify: `~/Development/Wire/WireIMXRT1176.h` (struct + includes, drop
  `wait_flag`/`bus_recover` members)
- Modify: `~/Development/Wire/WireIMXRT1176.cpp` (delegation + instance tables)

- [ ] **Step 4.1: Create `lpi2c1176.h`** (MIT + provenance header as in 2.1,
citing `WireIMXRT1176.cpp`):

```c
#ifndef LPI2C1176_H
#define LPI2C1176_H

#include <stdint.h>
#include <stddef.h>

#if defined(__cplusplus)
#define LPI2C1176_ASSERT(c, m) static_assert(c, m)
extern "C" {
#else
#define LPI2C1176_ASSERT(c, m) _Static_assert(c, m)
#endif

/* LPI2C master register-block overlay (offsets per RT1170 RM; equals the
 * master block of the core's IMXRT_LPI2C_t — cross-asserted in
 * WireIMXRT1176.cpp. The slave block stays on IMXRT_LPI2C_t: CM7-only). */
typedef struct {
	volatile uint32_t VERID;        /* 0x00 */
	volatile uint32_t PARAM;        /* 0x04 */
	volatile uint32_t r08, r0C;
	volatile uint32_t MCR;          /* 0x10 */
	volatile uint32_t MSR;          /* 0x14 */
	volatile uint32_t MIER;         /* 0x18 */
	volatile uint32_t MDER;         /* 0x1C */
	volatile uint32_t MCFGR0;       /* 0x20 */
	volatile uint32_t MCFGR1;       /* 0x24 */
	volatile uint32_t MCFGR2;       /* 0x28 */
	volatile uint32_t MCFGR3;       /* 0x2C */
	volatile uint32_t r30[4];
	volatile uint32_t MDMR;         /* 0x40 */
	volatile uint32_t r44;
	volatile uint32_t MCCR0;        /* 0x48 */
	volatile uint32_t r4C;
	volatile uint32_t MCCR1;        /* 0x50 */
	volatile uint32_t r54;
	volatile uint32_t MFCR;         /* 0x58 */
	volatile uint32_t MFSR;         /* 0x5C */
	volatile uint32_t MTDR;         /* 0x60 */
	volatile uint32_t r64[3];
	volatile uint32_t MRDR;         /* 0x70 */
} lpi2c1176_regs_t;

LPI2C1176_ASSERT(offsetof(lpi2c1176_regs_t, MCR)    == 0x10, "LPI2C MCR");
LPI2C1176_ASSERT(offsetof(lpi2c1176_regs_t, MSR)    == 0x14, "LPI2C MSR");
LPI2C1176_ASSERT(offsetof(lpi2c1176_regs_t, MCFGR1) == 0x24, "LPI2C MCFGR1");
LPI2C1176_ASSERT(offsetof(lpi2c1176_regs_t, MCCR0)  == 0x48, "LPI2C MCCR0");
LPI2C1176_ASSERT(offsetof(lpi2c1176_regs_t, MTDR)   == 0x60, "LPI2C MTDR");
LPI2C1176_ASSERT(offsetof(lpi2c1176_regs_t, MRDR)   == 0x70, "LPI2C MRDR");

/* Hardware description: CCM gate/root + SCL/SDA pads. */
typedef struct {
	volatile uint32_t *lpcg;         /* CCM LPCG DIRECT (write 1 to ungate) */
	volatile uint32_t *clock_root;   /* CCM CLOCK_ROOT CONTROL */
	uint32_t clock_root_val;
	volatile uint32_t *scl_mux;  uint32_t scl_mux_val;  volatile uint32_t *scl_pad;
	volatile uint32_t *sda_mux;  uint32_t sda_mux_val;  volatile uint32_t *sda_pad;
	volatile uint32_t *scl_select;  uint32_t scl_select_val;
	volatile uint32_t *sda_select;  uint32_t sda_select_val;
	uint32_t pad_ctl_val;            /* open-drain pad config */
} lpi2c1176_hw_t;

/* MCR */
#define LPI2C1176_MCR_MEN   (1u << 0)
#define LPI2C1176_MCR_RST   (1u << 1)
#define LPI2C1176_MCR_RTF   (1u << 8)
#define LPI2C1176_MCR_RRF   (1u << 9)
/* MSR */
#define LPI2C1176_MSR_TDF   (1u << 0)
#define LPI2C1176_MSR_RDF   (1u << 1)
#define LPI2C1176_MSR_EPF   (1u << 8)
#define LPI2C1176_MSR_SDF   (1u << 9)
#define LPI2C1176_MSR_NDF   (1u << 10)
#define LPI2C1176_MSR_ALF   (1u << 11)
#define LPI2C1176_MSR_FEF   (1u << 12)
/* MTDR commands (data in [7:0], cmd in [10:8]) */
#define LPI2C1176_TX_CMD(cmd, data)  (((uint32_t)(cmd) << 8) | ((data) & 0xFFu))
#define LPI2C1176_CMD_TXD    0u
#define LPI2C1176_CMD_RXD    1u
#define LPI2C1176_CMD_STOP   2u
#define LPI2C1176_CMD_START  4u
#define LPI2C1176_MRDR_RXEMPTY (1u << 14)

#define LPI2C1176_TIMEOUT   100000u

/* Ungate+root the clock and mux the pads (shared by master and slave init). */
void lpi2c1176_clocks_pins(const lpi2c1176_hw_t *hw);
/* clocks_pins + master reset + timing for clock_hz + enable. */
void lpi2c1176_begin(lpi2c1176_regs_t *p, const lpi2c1176_hw_t *hw,
                     uint32_t clock_hz);
void lpi2c1176_end(lpi2c1176_regs_t *p, const lpi2c1176_hw_t *hw);
/* MCFGR1 prescale + MCCR0 timing (~60% low). MEN save/restore. */
void lpi2c1176_set_clock(lpi2c1176_regs_t *p, uint32_t freq);
/* Wait until any bit in mask, or an error bit / timeout. 1 = success; on
 * failure *err: NACK -> 2 (if *err was 0xFF: address) else 3 (data);
 * ALF/FEF -> 4; timeout -> 5. Error flags are W1C'd. */
int lpi2c1176_wait_flag(lpi2c1176_regs_t *p, uint32_t mask,
                        uint32_t error_mask, uint32_t *err);
/* Flush TX/RX FIFOs + W1C latched flags after a NACK/error. */
void lpi2c1176_bus_recover(lpi2c1176_regs_t *p);
/* Polled master write: START+addr(W), per-byte TDF wait, optional STOP with
 * ACK/NACK judged at the SDF wait (watching NDF — TDF leads the ACK bit by a
 * byte-time on silicon). 0 ok / 2 addr-NACK / 3 data-NACK / 4 error / 5 timeout. */
uint32_t lpi2c1176_master_write(lpi2c1176_regs_t *p, uint8_t addr,
                               const uint8_t *data, uint32_t len, int send_stop);
/* Polled master read: (repeated-)START+addr(R), RXD N-1 encoding, per-byte
 * RDF wait, optional STOP. Returns bytes read. */
uint32_t lpi2c1176_master_read(lpi2c1176_regs_t *p, uint8_t addr,
                              uint8_t *dst, uint32_t quantity, int send_stop);

#if defined(__cplusplus)
}
#endif
#endif /* LPI2C1176_H */
```

- [ ] **Step 4.2: Create `lpi2c1176.c`** (bodies = `WireIMXRT1176.cpp`
verbatim; the ACK-at-STOP comment moves with the code):

```c
#include "lpi2c1176.h"

void lpi2c1176_clocks_pins(const lpi2c1176_hw_t *hw)
{
	*hw->lpcg = 1u;                              /* ungate LPI2C clock */
	*hw->clock_root = hw->clock_root_val;        /* 24 MHz functional clock */
	*hw->scl_mux = hw->scl_mux_val;  *hw->scl_pad = hw->pad_ctl_val;
	*hw->sda_mux = hw->sda_mux_val;  *hw->sda_pad = hw->pad_ctl_val;
	*hw->scl_select = hw->scl_select_val;
	*hw->sda_select = hw->sda_select_val;
}

void lpi2c1176_set_clock(lpi2c1176_regs_t *p, uint32_t freq)
{
	const uint32_t src = 24000000u;
	uint32_t pre = 0, div = 0;
	for (pre = 0; pre < 8u; pre++) { div = (src >> pre) / freq; if (div <= 120u) break; }
	uint32_t clklo = (div * 6u) / 10u;           /* ~60% low time (I2C tLOW>tHIGH) */
	uint32_t clkhi = (div > clklo) ? (div - clklo) : 1u;
	if (clklo > 63u) clklo = 63u;
	if (clkhi > 63u) clkhi = 63u;
	if (clkhi < 1u) clkhi = 1u;
	uint32_t men = p->MCR & LPI2C1176_MCR_MEN;
	p->MCR = men & ~LPI2C1176_MCR_MEN;           /* MCCR/MCFGR need MEN=0 */
	p->MCFGR1 = (p->MCFGR1 & ~0x7u) | (pre & 0x7u);
	p->MCCR0 = (clklo) | (clkhi << 8) | ((clkhi / 2u) << 16) | ((clkhi / 2u) << 24);
	if (men) p->MCR = LPI2C1176_MCR_MEN;
}

void lpi2c1176_begin(lpi2c1176_regs_t *p, const lpi2c1176_hw_t *hw,
                     uint32_t clock_hz)
{
	lpi2c1176_clocks_pins(hw);
	p->MCR = LPI2C1176_MCR_RST;                  /* reset the master block */
	p->MCR = 0u;
	lpi2c1176_set_clock(p, clock_hz);            /* program MCCR0/MCFGR1 */
	p->MCR = LPI2C1176_MCR_MEN;                  /* enable */
}

void lpi2c1176_end(lpi2c1176_regs_t *p, const lpi2c1176_hw_t *hw)
{
	p->MCR = 0u;
	*hw->lpcg = 0u;
}

int lpi2c1176_wait_flag(lpi2c1176_regs_t *p, uint32_t mask,
                        uint32_t error_mask, uint32_t *err)
{
	for (uint32_t g = 0; g < LPI2C1176_TIMEOUT; g++) {
		uint32_t s = p->MSR;
		if (s & error_mask) {
			if (s & LPI2C1176_MSR_NDF) *err = (*err == 0xFFu) ? 2u : 3u; /* addr vs data NACK */
			else *err = 4u;                       /* ALF/FEF/other */
			p->MSR = s;                           /* W1C the flags */
			return 0;
		}
		if (s & mask) return 1;
	}
	*err = 5u;                                    /* timeout */
	return 0;
}

void lpi2c1176_bus_recover(lpi2c1176_regs_t *p)
{
	p->MCR = LPI2C1176_MCR_MEN | LPI2C1176_MCR_RTF | LPI2C1176_MCR_RRF; /* reset FIFOs, stay enabled */
	p->MCR = LPI2C1176_MCR_MEN;
	p->MSR = p->MSR;                              /* W1C any latched flags */
}

uint32_t lpi2c1176_master_write(lpi2c1176_regs_t *p, uint8_t addr,
                               const uint8_t *data, uint32_t len, int send_stop)
{
	uint32_t err = 0xFFu;                        /* a NACK now is an address NACK (2) */
	p->MSR = p->MSR;                             /* clear stale flags */
	p->MTDR = LPI2C1176_TX_CMD(LPI2C1176_CMD_START, (uint32_t)(addr << 1) | 0u);
	/* IMPORTANT: do NOT treat TDF here as "address ACKed". TDF asserts a
	 * byte-time BEFORE the ACK bit is sampled on silicon; ACK/NACK is judged
	 * at completion (STOP) below, matching the NXP SDK. */
	for (uint32_t i = 0; i < len; i++) {
		if (!lpi2c1176_wait_flag(p, LPI2C1176_MSR_TDF,
		        LPI2C1176_MSR_NDF | LPI2C1176_MSR_ALF | LPI2C1176_MSR_FEF, &err)) {
			lpi2c1176_bus_recover(p);
			return err;
		}
		err = 0u;                                /* past the address */
		p->MTDR = LPI2C1176_TX_CMD(LPI2C1176_CMD_TXD, data[i]);
	}
	if (send_stop) {
		p->MTDR = LPI2C1176_TX_CMD(LPI2C1176_CMD_STOP, 0);
		/* Completion is the correct point to judge ACK/NACK -- watch NDF here. */
		if (!lpi2c1176_wait_flag(p, LPI2C1176_MSR_SDF,
		        LPI2C1176_MSR_NDF | LPI2C1176_MSR_ALF | LPI2C1176_MSR_FEF, &err)) {
			lpi2c1176_bus_recover(p);
			return err;
		}
		p->MSR = LPI2C1176_MSR_SDF | LPI2C1176_MSR_EPF;
	}
	return 0u;
}

uint32_t lpi2c1176_master_read(lpi2c1176_regs_t *p, uint8_t addr,
                              uint8_t *dst, uint32_t quantity, int send_stop)
{
	uint32_t err = 0xFFu, n = 0;
	p->MSR = p->MSR;
	p->MTDR = LPI2C1176_TX_CMD(LPI2C1176_CMD_START, (uint32_t)(addr << 1) | 1u);
	if (!lpi2c1176_wait_flag(p, LPI2C1176_MSR_TDF,
	        LPI2C1176_MSR_NDF | LPI2C1176_MSR_ALF | LPI2C1176_MSR_FEF, &err)) {
		if (send_stop) p->MTDR = LPI2C1176_TX_CMD(LPI2C1176_CMD_STOP, 0);
		return 0;
	}
	p->MTDR = LPI2C1176_TX_CMD(LPI2C1176_CMD_RXD, (uint8_t)(quantity - 1)); /* N-1 encoding */
	for (uint32_t i = 0; i < quantity; i++) {
		err = 0u;
		if (!lpi2c1176_wait_flag(p, LPI2C1176_MSR_RDF,
		        LPI2C1176_MSR_ALF | LPI2C1176_MSR_FEF, &err)) break;
		uint32_t r = p->MRDR;
		if (r & LPI2C1176_MRDR_RXEMPTY) break;
		dst[n++] = (uint8_t)(r & 0xFFu);
	}
	if (send_stop) {
		p->MTDR = LPI2C1176_TX_CMD(LPI2C1176_CMD_STOP, 0);
		lpi2c1176_wait_flag(p, LPI2C1176_MSR_SDF,
		        LPI2C1176_MSR_ALF | LPI2C1176_MSR_FEF, &err);
		p->MSR = LPI2C1176_MSR_SDF | LPI2C1176_MSR_EPF;
	}
	return n;
}
```

- [ ] **Step 4.3: Recompose `WireIMXRT1176.h`.** Add `#include "lpi2c1176.h"`
after `"imxrt1176.h"`. Replace the `I2C_Hardware_t` typedef (lines 54-66) with:

```cpp
	typedef struct {
		lpi2c1176_hw_t hw;                   // shared C core hardware desc
		IRQ_NUMBER_t irq;
		void (*irq_function)(void);
		uint16_t irq_priority;
	} I2C_Hardware_t;
```

In the private section: delete the `wait_flag` and `bus_recover` declarations
(lines 159-160); add next to `port()`:
```cpp
	lpi2c1176_regs_t *mp() { return (lpi2c1176_regs_t *)port_addr; }
```

- [ ] **Step 4.4: Delegate in `WireIMXRT1176.cpp`.**
Delete the master-side macro block: `MSR_*`, `MCR_*`, `TX_CMD`, `CMD_*`,
`MRDR_RXEMPTY`, `WIRE_TIMEOUT` (keep the slave `SCR_/SSR_/SIER_` block).
Add after the includes:

```cpp
#include <stddef.h>

// The shared C core's master overlay must equal the core header's.
static_assert(offsetof(lpi2c1176_regs_t, MCR)    == offsetof(IMXRT_LPI2C_t, MCR),    "MCR");
static_assert(offsetof(lpi2c1176_regs_t, MSR)    == offsetof(IMXRT_LPI2C_t, MSR),    "MSR");
static_assert(offsetof(lpi2c1176_regs_t, MCFGR1) == offsetof(IMXRT_LPI2C_t, MCFGR1), "MCFGR1");
static_assert(offsetof(lpi2c1176_regs_t, MCCR0)  == offsetof(IMXRT_LPI2C_t, MCCR0),  "MCCR0");
static_assert(offsetof(lpi2c1176_regs_t, MTDR)   == offsetof(IMXRT_LPI2C_t, MTDR),   "MTDR");
static_assert(offsetof(lpi2c1176_regs_t, MRDR)   == offsetof(IMXRT_LPI2C_t, MRDR),   "MRDR");
```

Replace method bodies:
```cpp
void TwoWire::begin() {
	lpi2c1176_begin(mp(), &hardware.hw, clock_hz);
}

void TwoWire::end() { lpi2c1176_end(mp(), &hardware.hw); }

void TwoWire::setClock(uint32_t freq) {
	clock_hz = freq;
	lpi2c1176_set_clock(mp(), freq);
}

uint8_t TwoWire::endTransmission(uint8_t sendStop) {
	uint8_t r = (uint8_t)lpi2c1176_master_write(mp(), tx_addr, tx_buf, tx_len, sendStop);
	tx_len = 0;
	return r;
}

uint8_t TwoWire::requestFrom(uint8_t address, uint8_t quantity, uint8_t sendStop) {
	if (quantity > BUFFER_LENGTH) quantity = BUFFER_LENGTH;
	rx_idx = 0;
	rx_len = (uint8_t)lpi2c1176_master_read(mp(), address, rx_buf, quantity, sendStop);
	return rx_len;
}
```
Delete the `TwoWire::wait_flag` and `TwoWire::bus_recover` definitions
(their bodies now live in `lpi2c1176.c`).

In `begin(uint8_t address)` replace the six clock/pin lines
(`hardware.lpcg = 1u; … hardware.sda_select_input = hardware.sda_select_val;`)
with:
```cpp
	lpi2c1176_clocks_pins(&hardware.hw);
```
(the SCR/SAMR/SCFGR/SIER/NVIC body is unchanged).

Replace the three instance tables (same values, `&` addresses, hw first):
```cpp
// EVKB Arduino-header I2C = LPI2C1 on GPIO_AD_08 (SCL) / GPIO_AD_09 (SDA),
// ALT1|SION (0x11). Pad 0x1E = ODE|DSE|PUE|PUS (internal pull-up). CLOCK_ROOT37
// (24 MHz) / LPCG98. Values verbatim from the HW-verified Wire_instances.cpp.
const TwoWire::I2C_Hardware_t TwoWire::lpi2c1_hardware = {
	{ &CCM_LPCG98_DIRECT,
	  &CCM_CLOCK_ROOT37_CONTROL, 0u,
	  &IOMUXC_SW_MUX_CTL_PAD_GPIO_AD_08, 0x11u, &IOMUXC_SW_PAD_CTL_PAD_GPIO_AD_08,
	  &IOMUXC_SW_MUX_CTL_PAD_GPIO_AD_09, 0x11u, &IOMUXC_SW_PAD_CTL_PAD_GPIO_AD_09,
	  &IOMUXC_LPI2C1_SCL_SELECT_INPUT, 0u,
	  &IOMUXC_LPI2C1_SDA_SELECT_INPUT, 0u,
	  0x0000001Eu },
	IRQ_LPI2C1, wire_isr, 16u,
};

// LPI2C2: QEMU-loopback slave persona only (no physical EVKB pins). Pin refs
// bind to LPI2C1's IOMUXC regs (inert in QEMU). CLOCK_ROOT38 / LPCG99.
const TwoWire::I2C_Hardware_t TwoWire::lpi2c2_hardware = {
	{ &CCM_LPCG99_DIRECT,
	  &CCM_CLOCK_ROOT38_CONTROL, 0u,
	  &IOMUXC_SW_MUX_CTL_PAD_GPIO_AD_08, 0x11u, &IOMUXC_SW_PAD_CTL_PAD_GPIO_AD_08,
	  &IOMUXC_SW_MUX_CTL_PAD_GPIO_AD_09, 0x11u, &IOMUXC_SW_PAD_CTL_PAD_GPIO_AD_09,
	  &IOMUXC_LPI2C1_SCL_SELECT_INPUT, 0u,
	  &IOMUXC_LPI2C1_SDA_SELECT_INPUT, 0u,
	  0x0000001Eu },
	IRQ_LPI2C2, wire1_isr, 16u,
};

// LPI2C5: onboard eCompass/WM8962 codec bus on GPIO_LPSR_05 (SCL) /
// GPIO_LPSR_04 (SDA), ALT0|SION (0x10). LPSR-domain pad 0x0A. CLOCK_ROOT41
// (mux 1) / LPCG102.
const TwoWire::I2C_Hardware_t TwoWire::lpi2c5_hardware = {
	{ &CCM_LPCG102_DIRECT,
	  &CCM_CLOCK_ROOT41_CONTROL, (1u << 8),
	  &IOMUXC_SW_MUX_CTL_PAD_GPIO_LPSR_05, 0x10u, &IOMUXC_SW_PAD_CTL_PAD_GPIO_LPSR_05,
	  &IOMUXC_SW_MUX_CTL_PAD_GPIO_LPSR_04, 0x10u, &IOMUXC_SW_PAD_CTL_PAD_GPIO_LPSR_04,
	  &IOMUXC_LPI2C5_SCL_SELECT_INPUT, 0u,
	  &IOMUXC_LPI2C5_SDA_SELECT_INPUT, 0u,
	  0x0000000Au },
	IRQ_LPI2C5, wire2_isr, 16u,
};
```
(The `wire_isr`/`wire1_isr`/`wire2_isr` forward declarations already precede
the tables — unchanged.)

- [ ] **Step 4.5: Gate + byte-identity.**
```bash
cd ~/Development/Wire/tests/wire_master_test && cmake --build build -j8 && ./run_qemu_wire.sh
cd ~/Development/Wire/tests/wire_slave_test  && cmake --build build -j8 && ./run_qemu_wire_slave.sh
cmake --build ~/Development/Wire/tests/wire_oled_test/build -j8
diff ~/Development/Wire/tests/wire_master_test/wire.uart      "$SCRATCH/baselines/wire_master.uart"
diff ~/Development/Wire/tests/wire_slave_test/wire_slave.uart "$SCRATCH/baselines/wire_slave.uart"
```
Expected: both runners PASS, both diffs empty, oled builds.

- [ ] **Step 4.6: Commit (Wire repo).**
```bash
git -C ~/Development/Wire add lpi2c1176.h lpi2c1176.c WireIMXRT1176.h WireIMXRT1176.cpp
git -C ~/Development/Wire commit -m "lpi2c1176: shared C register/clock core (Phase 3.3); TwoWire master path delegates (slave stays C++/NVIC); sequences verbatim, transcripts byte-identical (wire_master/wire_slave vs pre-refactor baselines)"
```

---

### Task 5: `cm4_wire_test` onto the shared core

**Files:**
- Modify: `evkb/cm4_wire_test/cm4/main_cm4.c`
- Modify: `evkb/cm4_wire_test/CMakeLists.txt:14-17`

- [ ] **Step 5.1: CMakeLists** — extend the image:
```cmake
teensy_add_cm4_image(cm4_wire
    LINKER  ${CMAKE_CURRENT_LIST_DIR}/cm4/cm4.ld
    SOURCES ${CMAKE_CURRENT_LIST_DIR}/cm4/startup_cm4.S
            ${CMAKE_CURRENT_LIST_DIR}/cm4/main_cm4.c
            $ENV{HOME}/Development/Wire/lpi2c1176.c
    INCLUDE_DIRS $ENV{HOME}/Development/Wire)
```

- [ ] **Step 5.2: Rewrite `main_cm4.c`.** Keep the provenance block (updated
for 3.3: sequences now ARE `lpi2c1176.c`; this file = MU scaffolding + address
table + the WM8962 token protocol), the SILICON TRUTH paragraph (incl. the
ACK-at-STOP note pointing at the shared core), MU defines, `mu_send`, handler
stubs, the WM8962/absent addresses and the three-transaction flow. Full new
content:

```c
#include <stdint.h>
#include "lpi2c1176.h"

/* LPI2C5 + its CCM/LPSR-IOMUXC instance addresses (imxrt1176.h values; the
 * CM7 library binds the same registers via the header macros). */
#define LPI2C5 ((lpi2c1176_regs_t *)0x40C34000u)
static const lpi2c1176_hw_t lpi2c5_hw = {
    .lpcg = (volatile uint32_t *)0x40CC6CC0u,          /* CCM_LPCG102_DIRECT */
    .clock_root = (volatile uint32_t *)0x40CC1480u,    /* CCM_CLOCK_ROOT41_CONTROL */
    .clock_root_val = (1u << 8),                       /* mux 1 -> 24 MHz */
    .scl_mux = (volatile uint32_t *)0x40C08014u, .scl_mux_val = 0x10u, /* GPIO_LPSR_05 ALT0|SION */
    .scl_pad = (volatile uint32_t *)0x40C08054u,
    .sda_mux = (volatile uint32_t *)0x40C08010u, .sda_mux_val = 0x10u, /* GPIO_LPSR_04 ALT0|SION */
    .sda_pad = (volatile uint32_t *)0x40C08050u,
    .scl_select = (volatile uint32_t *)0x40C08084u, .scl_select_val = 0u,
    .sda_select = (volatile uint32_t *)0x40C08088u, .sda_select_val = 0u,
    .pad_ctl_val = 0x0Au,                              /* LPSR open-drain */
};

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

int main(void)
{
    /* --- self-config LPI2C5 via the shared core (~100 kHz) --- */
    lpi2c1176_begin(LPI2C5, &lpi2c5_hw, 100000u);

    /* --- config readbacks --- */
    uint32_t mcr   = LPI2C5->MCR & LPI2C1176_MCR_MEN;  /* -> 1 */
    uint32_t lpcg  = *lpi2c5_hw.lpcg;                  /* informative */
    uint32_t croot = *lpi2c5_hw.clock_root;            /* informative */

    /* --- 1. reset-write R15<-0x6243 (WM8962_Init's own first write) --- */
    static const uint8_t reset_wr[4] = { 0x00u, 0x0Fu, 0x62u, 0x43u };
    uint32_t ack = lpi2c1176_master_write(LPI2C5, WM8962_ADDR, reset_wr, 4, 1);

    /* --- 2. zero-byte probe of an absent address -> address NACK --- */
    uint32_t nack = lpi2c1176_master_write(LPI2C5, ABSENT_ADDR, 0, 0, 1);

    /* --- 3. device-ID read-back of R15 (write reg addr, repeated START) --- */
    static const uint8_t reg_addr[2] = { 0x00u, 0x0Fu };
    uint8_t rd[2] = { 0, 0 };
    uint32_t rdn = 0, rdv = 0;
    if (lpi2c1176_master_write(LPI2C5, WM8962_ADDR, reg_addr, 2, 0) == 0u) {  /* no STOP */
        rdn = lpi2c1176_master_read(LPI2C5, WM8962_ADDR, rd, 2, 1);
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

- [ ] **Step 5.3: Rebuild + reconfigure.**
```bash
cd ~/Development/rt1170/evkb/cm4_wire_test && cmake -S . -B build && cmake --build build -j8
```

- [ ] **Step 5.4: Gate 3× + byte-identity.**
```bash
cd ~/Development/rt1170/evkb/cm4_wire_test
for i in 1 2 3; do ./run_qemu.sh | tail -1; done
diff cm4_wire.uart transcript_qemu.txt && echo TRANSCRIPT-IDENTICAL
```
Expected: `PASS: CM4 self-configured polled I2C verified in QEMU` ×3,
`TRANSCRIPT-IDENTICAL`.

- [ ] **Step 5.5: Commit (evkb repo).**
```bash
git -C ~/Development/rt1170/evkb add cm4_wire_test/cm4/main_cm4.c cm4_wire_test/CMakeLists.txt
git -C ~/Development/rt1170/evkb commit -m "cm4_wire_test: consume the shared lpi2c1176 C core (Phase 3.3); QEMU transcript byte-identical (stable 3x); local sequence mirror deleted"
```

---

### Task 6: Cross-gate regressions + license audit

- [ ] **Step 6.1: Rebuild the audit's consumer gates** (refreshes depfiles so
the new lib files enter the link manifests; sd_wav_play imports Wire+SPI+Audio):
```bash
cmake --build ~/Development/rt1170/evkb/sd_wav_play_test/build -j8
cmake --build ~/Development/rt1170/evkb/sd_test/build -j8
```
Expected: clean builds (compile break here = a delegation error — fix before
proceeding).

- [ ] **Step 6.2: Run the license audit.**
```bash
sh ~/Development/rt1170/evkb/tools/license-audit.sh; echo "rc=$?"
```
Expected: `LICENSE-AUDIT: PASS`, rc=0. The new files are MIT (part 1 sweeps
`~/Development/SPI` + `Wire` already; part 2 sees them via `.obj.d`/`.o.d`).
If a gap appears (e.g. a depfile misses a new file), extend the audit in the
SAME change and re-run.

- [ ] **Step 6.3: Re-run the two cm4 gates once more** (post-everything sanity):
```bash
cd ~/Development/rt1170/evkb/cm4_spi_test  && ./run_qemu.sh | tail -1
cd ~/Development/rt1170/evkb/cm4_wire_test && ./run_qemu.sh | tail -1
```

- [ ] **Step 6.4: Commit only if the audit needed extending** (otherwise no
changes exist in this task).

---

### Task 7: EVKB silicon anchor (wiring-free) — per spec §6

Probe decision recap: no risk trigger mandates a probe, but the CM4 binaries
changed, so the phase gets a wiring-free `cm4_wire_test` re-flash as its
silicon anchor. Follow `references/silicon-truth-loop.md` + the
`rt1170-evkb-flashing` procedure (LinkServer, not pyOCD; kill stale daemons;
start the serial reader BEFORE the reset; clean_boot.scp for an M4-held boot).
**If the board is not connected: skip, and queue the re-probe in the roadmap
(Task 8 wording covers both outcomes).**

- [ ] **Step 7.1: Flash + clean-boot capture** (exact commands per the
existing `cm4_wire_test/README.md` HW procedure from 3.2):
```bash
pkill LinkServer; pkill redlinkserv; sleep 1
# flash cm4_wire_test.axf/elf per README, then clean_boot.scp, capturing VCOM
# @115200 with the pyserial+gtimeout reader started BEFORE the reset.
```

- [ ] **Step 7.2: Verify byte-identity vs the checked-in HW transcript.**
```bash
# strip any leading NULs from the capture (USB re-enum artifact), keep CRLF
diff /tmp/hw_capture.txt ~/Development/rt1170/evkb/cm4_wire_test/transcript_hw_evkb.txt \
  && echo HW-TRANSCRIPT-IDENTICAL
```
Expected: identical — incl. `rdv=00006243` (the real codec ID over the
CM4-configured bus, now via the shared core). If it differs: STOP — silicon
wins; diagnose per silicon-truth-loop before any further commit.

- [ ] **Step 7.3: (conditional) `cm4_spi_test` re-flash** only if the
SDO(AD_30)→SDI(AD_31) jumper is still fitted; verify against its
`transcript_hw_evkb.txt` the same way. Otherwise queue in the roadmap.

- [ ] **Step 7.4: Roadmap/README notes if transcripts were re-captured**
(no new transcript files expected — byte-identity means the checked-in ones
remain exact; note the re-probe date in the roadmap entry instead).

---

### Task 8: Roadmap + docs close-out

**Files:**
- Modify: `evkb/.claude/skills/cm4-bringup/references/cm4-roadmap.md`
- Modify (already created): spec at
  `evkb/docs/superpowers/specs/2026-07-18-cm4-shared-c-core-consolidation-design.md`
- This plan: `evkb/docs/superpowers/plans/2026-07-18-cm4-shared-c-core-consolidation.md`

- [ ] **Step 8.1: Update the roadmap.**
- Header block: "Next work item" advances past 3.3 (next = the deferred list /
  Phase 4 decision).
- §3.3 flips to `✅ DONE` with a status paragraph: shared cores
  `SPI/lpspi1176.{h,c}` + `Wire/lpi2c1176.{h,c}`; CM7 classes delegate; cm4
  gates consume them (INCLUDE_DIRS macro arg); all transcripts byte-identical
  (QEMU; cm4 gates stable 3×); audit PASS; silicon anchor result (or queued).
- Session log: dated entry with commits, discoveries, probe outcome.
- Queued checks: add the `cm4_spi_test` jumper re-probe if skipped in 7.3.

- [ ] **Step 8.2: Commit (evkb repo).**
```bash
git -C ~/Development/rt1170/evkb add .claude/skills/cm4-bringup/references/cm4-roadmap.md \
  docs/superpowers/specs/2026-07-18-cm4-shared-c-core-consolidation-design.md \
  docs/superpowers/plans/2026-07-18-cm4-shared-c-core-consolidation.md
git -C ~/Development/rt1170/evkb commit -m "Phase 3.3 shared C core consolidation: spec+plan+roadmap (DONE; transcripts byte-identical; silicon anchor <result>)"
```

- [ ] **Step 8.3: Auto-memory.** Write the session memory file
(`rt1176-cm4-shared-c-core.md`) + MEMORY.md index line.

---

## Self-Review (performed at write time)

1. **Spec coverage:** §3.1 shared files → T2/T4; §3.2 CM7 delegation → T2.3-2.4 /
   T4.3-4.4; §3.3 CM4 side + INCLUDE_DIRS → T1/T3/T5; §4 gates/baselines →
   pre-captured + T2.5/T3.4/T4.5/T5.4/T6; §5 license → headers in T2/T4 + T6.2;
   §6 probe → T7; §7 commits → per-task + T8. No gaps.
2. **Placeholder scan:** none — every code step carries the full content; the
   only "per README" reference (7.1) points at a checked-in procedure file that
   exists (`cm4_wire_test/README.md`), consistent with 3.2's operator flow.
3. **Type consistency:** `lpspi1176_hw_t` field order matches between the
   header (2.1), the CM7 table (2.4), and the CM4 designated init (3.2 — order-
   independent). `lpi2c1176_*` names consistent across 4.1/4.2/4.4/5.2.
   `tcr_base` is `uint32_t*` in all callers; `mp()`/`lp()` defined before use.
