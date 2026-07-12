# RT1176-EVKB Arduino core — outstanding work & Teensy parity gaps

**As of:** 2026-07-12 (after Ethernet milestone 4, FNET + NativeEthernet).
**Scope:** what remains to reach practical Teensy-4.x (teensy4 core) parity on the
MIMXRT1170-EVKB CM7, plus infrastructure work that has no Teensy equivalent.
Everything listed as done below is QEMU-gated AND HW-verified unless noted.

## Done (for orientation — see memory notes / docs/superpowers for detail)

Core boot (FlexRAM, 996 MHz OverDrive, startup), GPIO + attachInterrupt, pin table +
FlexPWM analogWrite, tone, LPADC analogRead, Serial1 (LPUART1), USB CDC Serial
(SerialUSB), IntervalTimer (PIT), EventResponder, DMAChannel (eDMA), SPI (lib,
master + full-duplex DMA async), Wire (lib, master+slave), I2S/SAI TX+RX + WM8962,
AudioStream framework + AudioInput/OutputI2S + AudioPlaySdWav, EEPROM
(flash-emulated), SD card (USDHC/SdFat), USB host (HID kbd/mouse, MIDI), SDRAM/SEMC
+ extmem_malloc, SNVS RTC, Ethernet: enet.c driver → lwIP → Arduino Ethernet API
(newdigate/Ethernet, lwIP BSD) → FNET + NativeEthernet (Apache-2.0/MIT alternative).

## 1. Queued milestones (kickoff prompts already written in `docs/`)

| Milestone | Prompt file | Notes |
|---|---|---|
| FlexCAN (CAN bus) | `NEXT-SESSION-PROMPT-CAN.md` | FlexCAN_T4-style port; J47 = CAN3; clock/pin plumbing needs core + `gen_imxrt1176_h.py` additions |
| DAC | `NEXT-SESSION-PROMPT-DAC.md` | No on-chip DAC on RT1176 and no teensy4 DAC to port — design question (external DAC / PWM / SAI approach) |
| Displays (SSD1306 I²C, ST7735 SPI) | `NEXT-SESSION-PROMPT-DISPLAYS.md` | Library integration over already-verified Wire/SPI. CAUTION: untracked WIP exists in the shared tree (`ssd1306_display/`, `st7735_test/`, `mkr_ssd1306_test/`, `wire_oled` edits) — check for a concurrent session before starting |
| USB device HID (Keyboard/Mouse/Joystick) | `NEXT-SESSION-PROMPT-USB-DEVICE-HID.md` | USB device currently CDC-only; ChipIdea IP byte-identical to Teensy 4 → teensy4 port |
| USB host MSC (flash-drive files) | `NEXT-SESSION-PROMPT-USB-HOST-MSC.md` | Sub-project B deferred from `rt1176-usb-host-hid`; USB host + SdFat/FS foundations already proven — most "unblocked" next milestone |

## 2. Deferred sub-items from completed milestones

- **Ethernet (both stacks):** UDP multicast (`beginMulticast` / IGMP), mDNS
  (`EthernetMDNS` compiles but needs multicast), TLS (FNET has mbedTLS hooks;
  forced off for RT1176), IEEE-1588 / adjustable-timer timestamps, MAC-from-OCOTP-
  fuses helper (gates use a locally-administered MAC), HW checksum offload
  (deliberately off — QEMU's imx.enet lacks TX checksum insertion; re-enabling is
  an HW-only experiment).
- **Audio library:** only the graph engine + `AudioInputI2S`/`AudioOutputI2S`/
  `AudioPlaySdWav` (+ peak) are RT1176-verified. The wider node set (mixers,
  synths, effects, `play_sd_raw`, record/queue objects) is unexercised on this
  core; mostly platform-independent but needs gate coverage.

## 3. Core-surface parity gaps (no kickoff prompts yet)

- **More serial ports:** only `Serial1` (LPUART1) + USB CDC exist. Teensy 4.1 has
  Serial1–8. The FNET port's serial glue had to `#if`-out `Serial2..Serial7` —
  any library assuming multiple HardwareSerial instances will hit this.
- **More Wire/SPI buses:** public buses are LPI2C1 (`Wire`) and LPSPI1 (`SPI`)
  only. LPI2C5 exists internally as `Wire2` for the WM8962 codec. Teensy exposes
  Wire/Wire1/Wire2 and SPI/SPI1/SPI2.
- **`WProgram.h` is deliberately trimmed:** `IntervalTimer.h` is not included
  transitively (bit NativeEthernet in milestone 4 — fixed library-side); other
  teensy4 headers (CrashReport etc.) also absent. Worth a deliberate
  include-parity pass so stock Teensy sketches/libraries compile unmodified.
- **Filesystem on the FlexSPI NOR:** `FS.h` exists and flash-emulated EEPROM owns
  the top 256 K, but there is no LittleFS over the remaining ~16 MB NOR
  (Teensy 4.1 ships LittleFS).
- **Teensy runtime niceties:** CrashReport, watchdog (WDOG), tempmon
  (temperature monitor), Snooze/low-power modes — none started.
- **USB device classes beyond CDC + (queued) HID:** MIDI device, RawHID,
  Serial+Keyboard/Mouse composite configs, MTP.
- **CM4 second core:** out of scope so far (CM7 only).

## 4. Infrastructure / distribution

- **Packaging:** the core builds only via CMake + teensy-cmake-macros. An
  installable Arduino IDE package (`boards.txt`/`platform.txt`) or PlatformIO
  board definition would make `teensy-cores` usable outside this repo's harness.
- **Repo/push state:** `evkb` (gates + docs) is a LOCAL-only git repo by design;
  `cores/` (github `teensy-cores`) and `qemu2` (gitlab `qemu-rt1170`) push state
  varies per milestone — check `status -sb` before assuming published.
- **QEMU model debt:** models added per-milestone mirror silicon behavior we
  exercised; peripherals in §1/§3 will need their QEMU models (or `-icount`
  timing care) brought up in lockstep, per the established method.
