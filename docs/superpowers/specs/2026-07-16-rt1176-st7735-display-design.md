# RT1176 ST7735 TFT display — ST7735_t3 port (blocking SPI) — Design

Date: 2026-07-16
Status: approved

## Goal

Drive a physical ST7735 128×160 SPI TFT from the MIMXRT1176-EVKB Arduino core and
render graphics through a real Arduino display library. **Success = the panel
physically shows the expected output** (colored quadrants, shapes, text) with
status echoed over VCOM. This is **library integration + hardware bring-up** — the
`SPI`/LPSPI1 bus is already brought up and HW-verified (`newdigate/SPI`); no new
core peripheral or QEMU modeling is in scope unless a real gap forces it.

## Decision (brainstormed, approved)

Port **ST7735_t3** (MIT, Paul Stoffregen; checkout at `~/Development/ST7735_t3`)
by adding an **`__IMXRT1176__` platform branch that uses only the plain blocking
SPI API**. Rejected alternatives:

- *Promote the hand-rolled WIP driver* — tiny and understood, but no
  text/fonts/graphics primitives; weak platform deliverable.
- *Adafruit_ST7735 + GFX* — canonical but not checked out, drags in the
  Adafruit_BusIO shim, duplicates what ST7735_t3 gives us in Teensy idiom.

## Architecture

### 1. Library port (`~/Development/ST7735_t3`, → `newdigate/ST7735_t3`)

- New branch in the existing checkout; becomes a `newdigate/*` repo per the
  established libraries-live-in-their-own-repos pattern. Publish only at the end.
- Add `__IMXRT1176__` as a clean **fourth platform branch** alongside KINETISK,
  KINETISL, and `__IMXRT1062__`, implemented at the **inline helper layer** in
  `ST7735_t3.h` (`beginSPITransaction` / `writecommand_cont` / `writecommand_last`
  / `writedata8_cont` / `writedata16_cont` / `..._last` / `endSPITransaction`, and
  the corresponding member-variable/`begin()` plumbing in `ST7735_t3.cpp`):
  - Bus traffic = plain blocking `SPI.transfer` / `SPI.transfer16` inside
    `SPI.beginTransaction(SPISettings(freq, MSBFIRST, SPI_MODE0))`.
  - DC and CS = software GPIO via `digitalWriteFast` / `portSetRegister` +
    `digitalPinToBitMask` (all provided by the imxrt1176 core's full 22-pin table).
  - The lib's hardware-CS logic self-resolves to the software-CS path because our
    `SPIClass::pinIsChipSelect()` returns false on 1176.
- **Compiled out for 1176:** `DMAChannel` usage, `ENABLE_ST77XX_FRAMEBUFFER`,
  `updateScreenAsync`/`useFrameBuffer`, and all `IMXRT_LPSPI_t` direct register
  access from the `__IMXRT1062__` path. DMA blits are an explicit follow-on (our
  SPI already has async `transfer(..., EventResponderRef)` when wanted).
- Init command tables (`initR(...)` tab variants), drawing primitives, and fonts
  are platform-neutral and reused verbatim.
- ST7789/ST7796 siblings in `src/` merely need to keep compiling; they are not
  verified in v1.

### 2. QEMU gate (proportionate — QEMU has no panel)

- New `tests/st7735_t3_gate/` inside the ST7735_t3 repo, copied from the
  `newdigate/SPI` test pattern: borrow the evkb checkout for the imxrt1176 core +
  `teensy-cmake-macros`, `import_arduino_library` for SPI and ST7735_t3, runner
  goes through `evkb/tools/qrun`.
- **Mandatory:** the gate sketch (graphicstest-style: `initR`, fills, rects, text)
  **compiles and links**, and the sketch runs in QEMU far enough to print a
  post-init marker on Serial1 (proves no hard fault in the init path).
- **Stretch, timeboxed:** assert the ST7735 init byte sequence
  (SWRESET/SLPOUT/COLMOD/DISPON) appears on the LPSPI1 bus — only if the existing
  `imxrt_lpspi` QEMU model exposes a cheap observation point (e.g. existing trace
  or a trivial SSI slave). **Do not build new QEMU modeling for this.**

### 3. Hardware bring-up (the arbiter)

- Wiring (already confirmed except RST): SCK = D13 (`GPIO_AD_28`) → SCL, MOSI =
  D11 (`GPIO_AD_30`) → SDA, DC = D8, CS = D10, **RST = D9** (new — use the lib's
  hardware-reset path; SW-reset 0x01 remains the fallback for an unwired RST).
  MISO/D12 unused by the panel. 3V3 + GND + backlight per module.
- Demo/gate sketch starts with `initR(INITR_BLACKTAB)`; prints the variant used
  over Serial1 so tab/inversion/BGR mismatches (wrong colors, offset image) are
  diagnosed by flipping the init flag, not by code archaeology.
- Flash via `LinkServer run MIMXRT1176:MIMXRT1170-EVKB <elf>` (pkill LinkServer +
  redlinkserv first); VCOM via pyserial @115200, reader started before flash.
- **Fault isolation ladder** if the panel is dark: (1) VCOM status confirms the
  sketch runs; (2) Saleae on SCK/MOSI/DC/CS confirms init bytes leave the pins
  (automation server 127.0.0.1:10430); (3) MKR Zero running an equivalent
  Adafruit_ST7735 sketch (arduino-cli) proves panel + wiring independent of our
  host. The WIP sketch was **build-only, never HW-run** — nothing about this panel
  is proven yet; assume nothing.

### 4. WIP cleanup (small, flagged)

`~/Development/SPI/tests/st7735_test/st7735_test.cpp`: replace raw-GPIO9 register
banging for DC/CS with `pinMode`/`digitalWrite`, delete the stale "pin table only
maps the LED" comment. Kept as the minimal hand-rolled cross-check sketch.

## Error handling

- Panel variant mismatch (colors swapped / image offset / inverted): flip
  `initR()` tab constant / `invertDisplay` / MADCTL — demo prints active config.
- Init hang on HW but not QEMU: Saleae the bus; check DC timing vs CS.
- Library compiles but wrong pixels: A/B with the hand-rolled WIP sketch (known
  byte sequence) before suspecting the port.

## Testing

- QEMU: build + post-init serial marker (mandatory), init-byte bus assert
  (stretch). Runner inherits qrun's timeout/log-cap guard.
- HW: pixels on glass = pass. Quadrants + text via ST7735_t3 API.
- License: lib is MIT; any new files carry MIT headers (per the
  prefer-permissive-licenses rule).

## Out of scope (v1)

DMA/async blits, framebuffer mode, ST7789/ST7796/ILI9341 verification, touch,
SD-card slot on display modules.
