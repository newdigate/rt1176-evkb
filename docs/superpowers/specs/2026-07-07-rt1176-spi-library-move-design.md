# RT1176 SPI → library move (full Teensy API) — Design

**Status:** approved (design), ready for implementation plan
**Date:** 2026-07-07

## Goal

Move SPI out of the core (`cores/imxrt1176/SPI.{h,cpp}` + `SPI_instances.cpp`, teensy-cores) and into the **`newdigate/SPI`** library (`/Users/nicholasnewdigate/Development/SPI`, github), inline with how Teensy structures it (SPI is an Arduino **library**, not a core file — same boundary as the just-moved WM8962). Unlike the WM8962 move (a byte-identical relocation), the RT1176 SPI is **re-implemented to the full Teensy SPI API** by adapting the library's existing `__IMXRT1062__` (Teensy 4 / MIMXRT1060-EVKB) branch to RT1176 silicon. The SPI **tests move into the SPI repo** so `evkb` no longer depends on the library.

## Why this shape (exploration findings)

- The core's SPI is a **lean ~157-line from-scratch** driver (custom `hardware_t` of flat LPSPI register refs; API = `begin`/`beginTransaction`/`transfer`×forms/`transfer16` + the session's DMA `transfer(...,EventResponder&)`). Nothing in the core `#include`s it (no core→library coupling created by the move).
- `newdigate/SPI` is the **full Teensy SPI library** — AVR/KINETISK/KINETISL/`__IMXRT1062__` branches, all gated on `TEENSYDUINO`, with the complete API (`setMOSI/MISO/SCK`, `setCS`, `pinIsMOSI/…`, `usingInterrupt`, `transfer/16/32`, async `transfer(...,EventResponderRef)`, `dma_rxisr`) and a `SPI_Hardware_t` (per-bus pin-candidate tables, DMA channels, IRQ). It already carries a working `__IMXRT1062__` EVKB branch (`SPI`/`SPI1`/`SPI2` on LPSPI1/3/4). **Our core is not `TEENSYDUINO`.**
- RT1176 LPSPI is the **same IP family** as the 1062, so the port is bounded to the platform layer: clocks (CCM clock roots + LPCG gating), pins (`GPIO_AD_*` pads + `SELECT_INPUT` daisy — values already in our current `hardware_t`), base address (`0x40114000`), IRQ, DMAMUX (`LPSPI1_RX/TX`). The core has **flat** `LPSPI1_CR` defs, not an `IMXRT_LPSPI_t` port struct (the 1062 branch uses the struct).
- Consumers: evkb gates `spi_loopback_test`, `spi_dma_test`, `st7735_test`, all using the SPI API. Their harness is evkb-centric (`teensy-cmake-macros` `import_arduino_library`, `tools/qrun` for QEMU). The SPI repo's `examples/` are Arduino `.ino` sketches.

## Scope

**In scope:** the RT1176 `SPI` instance = **LPSPI1** (the Arduino header) with the **full Teensy API + DMA/async**, as a `#elif defined(__IMXRT1176__)` branch in `newdigate/SPI` adapted from `__IMXRT1062__`. The core gains an `IMXRT_LPSPI_t` port struct + `IMXRT_LPSPI1_ADDRESS`; the core `SPI.{h,cpp}` + `SPI_instances.cpp` are deleted. The three SPI gates move into `newdigate/SPI/tests/` (self-contained). HW-verified.

**Explicitly deferred (YAGNI):** `SPI1`/`SPI2` (LPSPI3/4) — no current consumer or HW test (would be QEMU-plumbing-only). `transfer32` beyond what the 1062 branch gives for free. Applying the same "tests-with-library" move to the audio gates (a later cleanup).

## Architecture — components

### Core (`teensy-cores`) — register defs only
Add to `cores/imxrt1176/imxrt1176.h`: an `IMXRT_LPSPI_t` struct (the LPSPI register-block overlay: `CR/SR/DER/CFGR0/CFGR1/CCR/FCR/FSR/TCR/TDR/RSR/RDR…`, matching the Teensy `imxrt.h` layout) + `#define IMXRT_LPSPI1_ADDRESS 0x40114000`. Register *structs* are core material in Teensy (the core stays the provider of register defs + `DMAChannel`/`EventResponder`; only the SPI *driver* moves). Delete `SPI.{h,cpp}` + `SPI_instances.cpp`.

### Library (`newdigate/SPI`) — the `__IMXRT1176__` branch
- **Include gate:** widen the top `#if defined(__arm__) && defined(TEENSYDUINO)` (which pulls in `DMAChannel`/`EventResponder` + defines `SPI_HAS_TRANSFER_ASYNC`) so `__IMXRT1176__` also qualifies.
- **`SPI.h`:** add `#elif defined(__IMXRT1176__)` in the platform `SPISettings`/`SPIClass` chain — the full `SPIClass` adapted from the `__IMXRT1062__` declaration (same public API + `SPI_Hardware_t` shape), retargeted: `port()` over `IMXRT_LPSPI_t`, RT1176 pin-candidate tables (`sck/sdo/sdi` = `GPIO_AD_28/30/31`, the `SELECT_INPUT` daisy vals), DMAMUX `LPSPI1_RX/TX`, IRQ. Single-bus (`CNT_*_PINS=1`) for LPSPI1.
- **`SPI.cpp`:** add `#elif defined(__IMXRT1176__)` with the method bodies adapted from the 1062 branch (begin/setClockDivider/set{MOSI,MISO,SCK,CS}/pinIs*/beginTransaction/endTransaction/transfer×forms/`usingInterrupt`/`initDMAChannels`/async `transfer(...,EventResponderRef)`/`dma_rxisr`/end), retargeted to RT1176 clocks (LPCG ungate + CCM clock root) + pins + the DMAMUX sources. The `SPI_Hardware_t spiclass_lpspi1_hardware` table (RT1176 values, from our current `SPI_instances.cpp` `lpspi1_hw`) + `SPIClass SPI(IMXRT_LPSPI1_ADDRESS, SPIClass::spiclass_lpspi1_hardware)`. Preserve the HW-verified DMA behavior from [[rt1176-spi-dma]] (RX-completion-as-done, `DER=TDDE|RDDE`, 8-bit FRAMESZ, `triggerAtHardwareEvent(LPSPI1_RX/TX)`) expressed in the library's async framework.

### Tests (move into `newdigate/SPI/tests/`)
`spi_loopback_test`, `spi_dma_test`, `st7735_test` move from `evkb/` into `newdigate/SPI/tests/<name>/`, each self-contained: its `CMakeLists.txt` (using `teensy-cmake-macros` `import_arduino_library(cores <teensy-cores>)` + `import_arduino_library(SPI <this repo>)` + `teensy_add_executable`), its `run_qemu_*.sh` (via `qrun`), toolchain, and firmware (unchanged `.cpp`, but `#include <SPI.h>` now resolving to the library). The core, `teensy-cmake-macros`, and `qrun` are referenced as external checkouts (independent repos/tools), **not** evkb. The three `evkb/spi_*` gate dirs are **deleted** — `evkb` no longer references SPI.

## Data flow / behavior

The SPI API behavior is preserved (master-mode LPSPI transfers; DMA full-duplex via two `DMAChannel`s with RX-completion as the done signal + `EventResponder` for async), now expressed through the Teensy library's `SPIClass`. **Not byte-identical** — it's a re-implementation on a different code structure — so behavior is validated on hardware, not by diff.

## Testing

**QEMU gates** (now in `newdigate/SPI/tests/`, run via `qrun`): the plumbing — `spi_loopback_test` (LPSPI loopback echo), `spi_dma_test` (the DMA + async `EventResponder` completion), and `st7735_test` (build/compile + the SPI command/data stream). These prove the graph/register plumbing regardless of real timing.

**Hardware (final arbiter — re-implementation, not byte-identical):**
- `spi_loopback_test` — SDO→SDI jumper, transferred bytes echo back (per [[rt1176-lpspi-spi]]).
- `spi_dma_test` — DMA full-duplex transfer completes + the async `EventResponder` callback fires (per [[rt1176-spi-dma]]).
- `st7735_test` — a real ST7735 SPI display renders (exercises the full API — `begin`/`beginTransaction`/`transfer`/`transfer16` + pin setup — end to end).

Controller drives scriptable flash + VCOM; the user confirms the jumper/display results (bench moments, like prior SPI HW tests).

## Risks

- **Re-implementation, not a move** — the biggest risk. Mitigated by adapting the *proven* 1062 branch (same IP) + re-verifying on HW via all three gates (loopback, DMA, display). The clock/pin/IRQ/DMAMUX retarget is the error-prone surface — cross-check every value against the current HW-verified `SPI_instances.cpp` `hardware_t` + [[rt1176-lpspi-spi]]/[[rt1176-spi-dma]].
- **`IMXRT_LPSPI_t` struct correctness** — the field offsets must match the RT1176 LPSPI map (`0x40114000`); cross-check against the flat `LPSPI1_*` addresses already in `imxrt1176.h` (`CR@+0x10`, `SR@+0x14`, `DER@+0x1C`, `CFGR1@+0x24`, `CCR@+0x40`, `FCR@+0x58`, `TCR@+0x60`, `TDR@+0x64`, `RSR@+0x70`, `RDR@+0x74`).
- **Include-gate / `TEENSYDUINO`** — the RT1176 branch must compile without `TEENSYDUINO`; verify the widened gate pulls in `DMAChannel`/`EventResponder` and that no 1062-only Teensy dependency (e.g. `CORE_PIN*_CONFIG` semantics, `IRQ_LPSPI*`) leaks in unadapted.
- **Test-harness paths** — the moved gates reference the core/`teensy-cmake-macros`/`qrun` by workspace path; acceptable (external deps), documented in each test's CMake.

## References

- Core (source of the RT1176 SPI to re-express): `cores/imxrt1176/SPI.{h,cpp}`, `SPI_instances.cpp` (the HW-verified `lpspi1_hw` pin/clock table), `imxrt1176.h` (flat `LPSPI1_*` addresses).
- Library (target + template): `/Users/nicholasnewdigate/Development/SPI` `SPI.{h,cpp}` — the `__IMXRT1062__` branch (full API + `SPI_Hardware_t` + async), `examples/`.
- [[rt1176-lpspi-spi]] (the LPSPI bring-up: pins, clock root 43/LPCG104, master-polled), [[rt1176-spi-dma]] (the full-duplex DMA + async `EventResponder`, the QEMU LPSPI `min_access_size` fix), [[rt1176-edma-dmachannel]] (`DMAChannel` + DMAMUX), [[rt1176-eventresponder]], and the WM8962 move [[rt1176-wm8962-consolidation]] (the precedent for core→library relocation).
