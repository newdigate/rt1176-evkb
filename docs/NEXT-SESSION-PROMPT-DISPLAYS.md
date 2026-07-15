# RT1176-EVKB Arduino/Teensyduino core — next-session kickoff prompt (displays)

Paste everything below the line into a fresh Claude Code session started in
`~/Development/rt1170`.

---

We're building a from-scratch **Arduino / Teensyduino core for the NXP
MIMXRT1176-EVKB** (Cortex-**M7** @ 996 MHz, XIP from FlexSPI NOR, dual-core CM7+CM4
— we target the **CM7**). Every peripheral/API is brought up **test-first in a
custom QEMU model, then proven on real silicon**, and the QEMU model is refined in
lockstep so it faithfully mirrors the CM7 hardware. Refining QEMU is a first-class
deliverable, not just a means — when a gate exposes a model gap (an unwired IRQ, a
missing register), fix the model to match what the silicon actually does.

**This task is different from most: it is LIBRARY INTEGRATION, not a new core
peripheral.** The two buses these panels ride — `Wire` (I²C / LPI2C1) and `SPI`
(LPSPI1, incl. DMA/async) — are **already brought up and HW-verified**. The goal is
to drive two physical display panels over those working buses and render text/
graphics. So this leans much less on new register/QEMU work and much more on
**library glue + hardware bring-up** (a real panel is the arbiter). Frame the whole
effort accordingly.

## Repos + working-tree heads-up

- Core (git repo, `master`, → github `teensy-cores`): `~/Development/rt1170/evkb/cores/imxrt1176`
  (the git repo root is `evkb/cores` — run `git -C ~/Development/rt1170/evkb/cores/imxrt1176 …`;
  do NOT run git from `evkb/`, where `cores/` shows as a nested untracked repo).
- **teensy-4 source** (the port reference) = `~/Development/rt1170/evkb/cores/teensy4`.
- **`newdigate/SPI`** (`~/Development/SPI`, github) and **`newdigate/Wire`**
  (`~/Development/Wire`, github) — the two buses the panels use, now Arduino
  **libraries** (moved out of the core). This is the established boundary pattern:
  Arduino libraries live in their own repos, the core provides register defs +
  low-level primitives (`DMAChannel`, `EventResponder`, `Stream`).
- QEMU (git repo, `master`, → gitlab `qemu-rt1170`): `~/Development/qemu2` (machine
  `mimxrt1170-evk`), run via `evkb/tools/qrun` (`-M mimxrt1170-evk -global
  fsl-imxrt1170.boot-xip=on`).
- **gates** = evkb (LOCAL git repo, its own working tree — check `git status` before
  assuming files vanished; the tree is shared across concurrent sessions).

**Working-tree heads-up:** `git status` in `evkb` shows pre-existing uncommitted WIP
that is NOT from the last finished task — a few `wire_*`/`wire_oled` `.cpp` edits, the
two `wire_*` `run_qemu_*.sh` runners (they also carry the qrun swap), and the two
**display experiments this task continues**, both **untracked**:
`ssd1306_display/` (a hand-rolled SSD1306-over-`Wire` driver that already builds +
draws a frame) and `mkr_ssd1306_test/` (a bare MKR-Zero A/B sketch). The ST7735
experiment already **moved** into the SPI library at
`~/Development/SPI/tests/st7735_test/` (build-only). `cores/` shows as *untracked* in
`evkb` only because it's a **separate nested git repo** (commit core changes from
inside `cores/`). Don't sweep this WIP into your commits; stage only files you
actually touch.

## Method — follow it, it's been paid for in bugs

1. **Use the superpowers workflow for every peripheral:** `brainstorming` →
   `writing-plans` → `subagent-driven-development`. Do NOT write code before the
   design is approved (the brainstorming HARD-GATE). Process skills before
   implementation skills. **Start by exploring, not implementing.**
2. **The SDK is ALWAYS the first port of call — before writing a single register.**
   - Read the **NXP MCUXpresso SDK example** for the peripheral on *this* board:
     `~/Development/mcuxsdk-ws/mcuxsdk/examples/_boards/evkbmimxrt1170/` (and the
     sibling `_boards/evkmimxrt1160/`). The example's `pin_mux.c` / driver calls are
     the canonical sequence.
   - Read the **SDK device header for ground truth** (register maps, IRQ numbers,
     base addresses): `.../devices/RT/RT1170/MIMXRT1176/MIMXRT1176_cm7_COMMON.h`
     and the peripheral `fsl_*.h` in `.../drivers/`. **Use the `cm7` header, never
     `cm4`** — IRQ numbers and which instances even have an interrupt line differ
     between cores.
   - Cross-reference **Zephyr** (`~/Development/zephyr`) — its RT1170 dts/pinctrl/
     drivers are an excellent second opinion.
   - Only crack the **RM PDF** (`~/Development/rt1170/IMXRT1170RM.pdf`, large — grep/
     extract narrowly) for gaps the SDK doesn't answer.
   Every expensive detour in this project came from guessing instead of reading the
   SDK; every fix came from reading it. **(For displays the bus SDK work is already
   banked — the I²C/SPI mux/clock/register sequences are done + HW-verified. The
   "reference to read first" for the panels themselves is the Teensy/Adafruit
   library + the panel controller datasheet, not an NXP peripheral — an SSD1306 /
   ST7735 is not a chip peripheral, it's a device hanging off the bus.)**
3. **TDD via a QEMU "gate":** write a self-contained sketch + `run_qemu_*.sh` runner
   that greps its VCOM output for `PASS`/`FAIL`. Make it fail, make it pass, THEN go
   to hardware. Copy an existing gate as a template: `evkb/wire_master_test/` (from
   the Wire lib), `~/Development/SPI/tests/spi_loopback_test/`, `evkb/usb_data_test/`.
4. **Hardware is the final arbiter.** A QEMU gate you built to match your own
   assumption proves *consistency, not correctness* — that bit us with a circular
   false-pass. Model what the silicon does (per SDK/RM), then let the gate **and**
   HW judge. **For displays this is doubly true: QEMU has no panel — the only real
   proof is pixels on glass** (see the Verification section).

## Verification triad + the MKR Zero test-bed

- **QEMU gate** — fast, deterministic logic check; also where model gaps surface.
  **For displays there is no framebuffer/panel model**, so a gate can at most be a
  *build* + an optional **bus-traffic assertion** (e.g. assert the SSD1306 init byte
  sequence appears on the LPI2C bus, or the ST7735 init on LPSPI). It cannot show a
  picture — do not over-invest here.
- **Saleae Logic** — signal-level truth (bus traffic, edges). Automate it:
  `from saleae import automation`, server `127.0.0.1:10430`, `TimedCaptureMode` →
  export raw digital CSV. Reference: `evkb/pwm_test/measure_pwm.py`. (Make sure
  Logic 2 is running with automation enabled.) Useful to confirm the init bytes /
  DC-CS timing actually leave the pins.
- **Hardware (EVKB) = the real arbiter here.** A **physical SSD1306 OLED and/or
  ST7735 TFT** wired to the Wire/SPI headers; **success = the panel shows the
  expected output** (text, quadrants, colors). Read the board's own status over VCOM
  alongside.
- **MKR Zero (SAMD21, known-good Arduino) = the bench partner / A-B reference.** Run
  the *same* Adafruit sketch on the MKR Zero to prove the wiring + panel are good
  independently of our core (it's exactly how `mkr_ssd1306_test.ino` is used — if the
  MKR flashes the panel white but the EVKB doesn't, the fault is our host, not the
  module). It's also how we proved the earlier I²C flakiness was *physical*, not
  firmware.

## Flashing & serial (macOS gotchas)

- **LinkServer, not pyOCD.** Reliable boot: `LinkServer run MIMXRT1176:MIMXRT1170-EVKB
  <img.elf>` (load+reset+run; clears the vector-catch that a plain `flash load`
  leaves the core halted on — and a USB replug is *not* always a true POR).
  Binary: `/Applications/LinkServer_26.6.137/LinkServer`. Flash an ELF explicitly:
  `LinkServer flash MIMXRT1176:MIMXRT1170-EVKB load <img>.elf` (`auto` device-select
  FAILS — always give the explicit `DEVICE:BOARD`).
- **Flash fails / probe stuck?** ALWAYS `pkill -9 -f LinkServer; pkill -9 -f redlinkserv`
  **before every flash** — a stale `redlinkserv` backend keeps the probe, so the next
  flash "Reconnected to existing LinkServer process" and dies ("stub terminated return
  code 1"). A *recurring* `Unable to retrieve DAPInfo: Hardware interface transfer error`
  even after killing both = physical probe fault → **unplug/replug the MCU-Link USB (J11)**
  (resets the probe AND power-cycles the target; clears a stuck FlexSPI NOR too). See the
  `rt1170-evkb-flashing` memory note.
- **VCOM console** = `/dev/cu.usbmodem5DQ2DDHVWO5EI3` @ **115200** (the MCU-Link
  probe's; the *other* `usbmodem14534401` is the target's native-USB CDC — don't
  confuse them). Read with **pyserial**, never `cat` (resets the port to 9600 on open
  → garbage). One-shot banners: start the pyserial reader first, then `LinkServer run`.
- Toolchain: ARM GCC 10.2.1, `ARMGCC_DIR=/Applications/ARM_10`. QEMU build:
  `cd ~/Development/qemu2/build && ninja qemu-system-arm`; running needs
  `-global fsl-imxrt1170.boot-xip=on`.
- **QEMU runaway guard:** an orphaned `qemu-system-arm` with `-d guest_errors` once
  wrote ~100 GB and filled the disk. QEMU has no native log cap, so all `run_qemu_*.sh`
  runners go through **`evkb/tools/qrun`** (`QEMU=~/Development/rt1170/evkb/tools/qrun`):
  it runs QEMU under `gtimeout` (self-kills an orphan) and caps the `.dbg` log at 100 MB
  (`QRUN_TIMEOUT`/`QRUN_MAXLOG_MB` to tune). **Copy an existing runner for any new gate**
  so it inherits the guard. `*.dbg`/`*.uart` are git-ignored — `find ~/Development/rt1170/
  evkb -name '*.dbg' -delete` to reclaim space.

## Your memory already holds the hard-won lessons

MEMORY.md auto-loads. **Read the note relevant to the peripheral before touching
it.** For displays, especially:

- **`rt1176-wire-library-move`** (I²C / `Wire`) — the bus the OLED rides. Wire is now
  the **`newdigate/Wire` library** (`~/Development/Wire`), full Teensy `TwoWire :
  Stream` API, master+slave HW-verified. `Wire`/`Wire1`/`Wire2` = LPI2C1/2/5; **the
  Arduino-header bus is `Wire` = LPI2C1** (SCL = D15 `GPIO_AD_08`, SDA = D14
  `GPIO_AD_09`, **external pull-ups required**). Consumers `import_arduino_library(Wire
  ~/Development/Wire)` and link `Wire`.
- **`rt1176-spi-library-move`** (SPI) — the bus the TFT rides. SPI is now the
  **`newdigate/SPI` library** (`~/Development/SPI`), full Teensy API incl. DMA +
  async `transfer(...,EventResponder&)`, HW-verified. Scope = **LPSPI1 only** (the
  Arduino header): SCK = D13 `GPIO_AD_28`, MOSI/SDO = D11 `GPIO_AD_30`, MISO/SDI =
  D12 `GPIO_AD_31`. Consumers `import_arduino_library(SPI ~/Development/SPI)` + link
  `SPI` (see `~/Development/SPI/tests/st7735_test/CMakeLists.txt` for the exact
  pattern, including borrowing the evkb checkout for the core + build macros).
- **`rt1170-evkb-flashing`** + **`macos-serial-capture`** — HW flash (LinkServer) +
  VCOM capture (pyserial, not `cat`).
- (Also handy: **`rt1176-pintable-pwm`** — the core now maps **all 22 Arduino-header
  digital pins** (`pinMode`/`digitalWrite` work: core pin N = header DN), so the
  ST7735 DC/CS/RST control lines can use `digitalWrite` rather than raw-GPIO register
  banging. **`rt1176-spi-dma`** — the SPI DMA/async path, if a future large-blit
  wants it.)

Subagent caveat: **verify a subagent's real git/file state — don't just trust its
self-report.** `subagent-driven-development` runs completed reliably *when given exact
specs + the gate commands*; still, independently checking each diff/gate is cheap
insurance. Read-only-review subagents are especially reliable — a read-only review is
what caught the CM7 GPIO-IRQ bug. **Watch for the CMake `file(GLOB)` trap:** adding a
NEW core `.c`/`.cpp` needs a from-scratch reconfigure (`rm -rf build && cmake -B
build …`) of every test dir, not just a rebuild (no `CONFIGURE_DEPENDS`). (Unlikely
to bite this task — displays should add *library* files, not core files — but if you
do touch the core, reconfigure.)

## Status

**Done, HW-verified, pushed** (the platform under this task): startup/clocks/SysTick,
millis/micros/delay, GPIO + full 22-pin Arduino-header pin table, digitalWrite/Read/
pinMode, LPUART `Serial1`, LPADC `analogRead`, FlexRAM, 996 MHz voltage,
`attachInterrupt`, `IntervalTimer` (PIT1), `tone()`, FlexPWM `analogWrite`, USB CDC
`Serial`/`SerialUSB`, USB Host (HID + MIDI), the full **audio stack** (I²S/SAI TX+RX,
WM8962 codec, eDMA/`DMAChannel`, `AudioStream` graph, `AudioInputI2S`/`AudioOutputI2S`,
`AudioPlaySdWav`), **SD card** (SdFat/SD over USDHC), flash-emulated **EEPROM** — and,
**central to this task, the two buses the panels use:**

- **`Wire` (I²C, LPI2C1/2/5)** — master + slave, full Teensy API, HW-verified. Now the
  `newdigate/Wire` library.
- **`SPI` (LPSPI1)** — master incl. DMA + async, HW-verified. Now the `newdigate/SPI`
  library.

**Displays — WIP already on disk (what this task continues):**
- `evkb/ssd1306_display/` (untracked) — a **hand-rolled** minimal SSD1306 128×64
  driver over `Wire` @ **0x3C**, bus @ **100 kHz** (marginal pull-ups corrupt bytes at
  400 k), with its own 5×7 font + 1 KB framebuffer; it scans the bus, inits the panel,
  draws "RT1176 / I2C OK / :)", and blinks the `0xA5`/`0xA4` all-pixels-on trick to
  prove the panel is alive. **It builds** (`build/ssd1306_display.elf` present). It is
  NOT using Adafruit_GFX.
- `evkb/mkr_ssd1306_test/mkr_ssd1306_test.ino` (untracked) — a **bare raw-`Wire`**
  MKR-Zero sketch running the *same* init + `0xA5`/`0xA4` flash: the known-good
  **A/B reference** to prove the panel/wiring independent of our core.
- `~/Development/SPI/tests/st7735_test/` — a **hand-rolled** minimal ST7735 128×160
  driver over `SPI` (SCK=D13, MOSI=D11→SDA, DC=D8, CS=D10, software reset), 8 MHz
  `SPISettings`, fills quadrants + cycles colors. **Build-only** (a HW visual demo, no
  PASS marker). It drives DC/CS via **raw GPIO9 register writes** with a now-**stale**
  comment ("the pin table only maps the LED") — the pin table maps all 22 header pins
  now, so this is a simplification candidate (`digitalWrite`).

So the panels are **partially bring-up'd at the driver level already**; the open work
is a proper library integration + real-panel verification.

## First move — displays (SSD1306 over I²C + ST7735 over SPI)

**Goal:** drive two physical display panels over the already-working buses and render
text/graphics — an **SSD1306 OLED** (I²C, addr **0x3C**, on the `Wire`/LPI2C1 header)
and an **ST7735 TFT** (SPI, on the LPSPI1 header + DC/CS/RST GPIOs). **Success =
each panel physically shows the expected output** (the OLED renders text/shapes; the
TFT shows colored graphics), with a known-good **MKR Zero** running the same sketch as
the A/B reference. This is **library glue + hardware bring-up, not a new core
peripheral** — the buses are done; keep that scope discipline (resist adding core/QEMU
work unless a real gap forces it).

Suggested decomposition (each its own spec → plan → implement cycle):
**(A) SSD1306 / I²C first** (simplest — the WIP already draws a frame; it's the
shortest path to "pixels on glass"), then **(B) ST7735 / SPI** (color, longer init,
DC/CS/RST control lines).

1. **Take stock of the WIP before designing** (don't rebuild what works). Read
   `evkb/ssd1306_display/ssd1306_display.cpp` (hand-rolled, builds, draws a frame),
   `evkb/mkr_ssd1306_test/mkr_ssd1306_test.ino` (the A/B reference), and
   `~/Development/SPI/tests/st7735_test/st7735_test.cpp` (hand-rolled, build-only).
   Decide what to keep vs. replace with a library.

2. **The core library decision (the main brainstorm question).** The established
   boundary pattern = Arduino libraries live in their **own repos** (`newdigate/*`),
   the core provides primitives. Options to weigh, per panel:
   - **SSD1306:** adopt **Adafruit_SSD1306 + Adafruit_GFX** (the real graphics API —
     fonts, shapes, `print()`), **but note these are NOT currently checked out** under
     `~/Development` (only `ST7735_t3` + `ILI9341_t3` are) — you'd clone them, and
     Adafruit_SSD1306 also pulls **Adafruit_BusIO** (`Adafruit_I2CDevice`). Versus
     **keeping/refining the hand-rolled minimal driver** (already works, zero new deps,
     but no general GFX). Versus a `newdigate/*` fork if either needs RT1176 tweaks.
   - **ST7735:** **`ST7735_t3`** (and `ILI9341_t3`) are **checked out** and are
     Teensy-optimized — but they `#include "DMAChannel.h"` + `<SPI.h>` and their
     `__IMXRT1062__` branch uses **Teensy-4-specific SPI-DMA + framebuffer/async**
     (`updateScreenAsync`, `useFrameBuffer`) that **won't map 1:1 to RT1176** (there is
     **no `__IMXRT1176__` branch**). So: either drive them via the **plain blocking-SPI
     path** (disable the DMA/framebuffer path) or **add an `__IMXRT1176__` branch** (our
     `SPI` already has DMA + async `transfer(...,EventResponder&)` if you later want the
     fast blit). Versus the Adafruit_ST7735 + GFX route, or keeping the hand-rolled
     minimal driver. **Recommend the minimal/blocking path for v1**, DMA as a follow-on.

3. **Pins are already confirmed — reuse them, don't rediscover:**
   - **SSD1306 (I²C):** `Wire` = **LPI2C1**. **SCL = D15 (`GPIO_AD_08`), SDA = D14
     (`GPIO_AD_09`)**, VCC 3V3, GND, **external pull-ups**, addr **0x3C**. Run the bus
     at **100 kHz** (the WIP note: marginal module pull-ups corrupt bytes at 400 k).
   - **ST7735 (SPI):** `SPI` = **LPSPI1**. **SCK = D13 (`GPIO_AD_28`) → SCL**, **MOSI =
     D11 (`GPIO_AD_30`) → SDA**, MISO = D12 (`GPIO_AD_31`, unused by the panel). Plus
     control GPIOs: **DC = D8, CS = D10, RST** (the WIP uses SW-reset 0x01 and drives
     DC/CS by hand). Since the core now maps all 22 header pins, **prefer
     `pinMode`/`digitalWrite`** for DC/CS/RST over the WIP's raw-GPIO9 banging.

4. **QEMU gate = build + (optional) bus-traffic assertion — there is no panel model.**
   The LPI2C model (`qemu2/hw/i2c/imxrt_lpi2c.c`) and LPSPI model
   (`qemu2/hw/ssi/imxrt_lpspi.c`) already exist and are HW-faithful, so a gate *can*
   assert the **init byte sequence hits the bus** (e.g. the SSD1306 `0xAE…0xAF` command
   stream on LPI2C, or the ST7735 init on LPSPI). But QEMU can render nothing — **do
   not build an elaborate QEMU model for this**; the effort belongs on hardware. A
   build-only gate + a short bus-assert is proportionate.

5. **Hardware is the arbiter.** Wire a real SSD1306 OLED and/or ST7735 TFT to the
   headers above; flash (`LinkServer run …`); **confirm the panel shows the expected
   output**. Read status over VCOM in parallel. If a panel is dark, flash the **same**
   Adafruit/raw sketch on the **MKR Zero** to prove the module + wiring are good before
   suspecting our host (the `mkr_ssd1306_test` pattern). Saleae on the bus can confirm
   the init bytes actually leave the pins if you need to localize a fault.

Method: **brainstorm → spec → `writing-plans` → `subagent-driven-development`, gate-
first** (no code before the brainstorming hard-gate). Explore the WIP + the two
candidate libraries first; the buses are ground truth and already HW-verified — the
real risk here is library-fit and physical wiring, not the silicon. **First action:
invoke the brainstorming skill and survey the WIP drivers + the Adafruit/Teensy
display libraries before proposing a design.**
