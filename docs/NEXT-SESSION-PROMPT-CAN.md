# RT1176-EVKB Arduino/Teensyduino core — next-session kickoff prompt (FlexCAN)

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

- Core (git repo, `master`, → github `teensy-cores`): `~/Development/rt1170/evkb/cores/imxrt1176`
- QEMU (git repo, `master`, → gitlab `qemu-rt1170`): `~/Development/qemu2` (machine `mimxrt1170-evk`)

**Working-tree heads-up:** `git status` in `evkb` will show pre-existing uncommitted WIP that is NOT from the last finished task — a few `wire_*`/`wire_oled` `.cpp` edits, some display experiments (`ssd1306_display/`, `st7735_test/`, `mkr_ssd1306_test/`, untracked), and the two `wire_*` `run_qemu_*.sh` runners (they also carry the qrun swap). `cores/` shows as *untracked* here only because it's a **separate nested git repo** (→ github `teensy-cores`, already pushed — commit core changes from inside `cores/`). Don't sweep this WIP into your commits; stage only files you actually touch.

- **The core git root is `evkb/cores`** — run `git -C ~/Development/rt1170/evkb/cores/imxrt1176 …`; do NOT run git from `evkb/`, where `cores/` shows as a nested untracked repo. **`teensy-4` source** (the port reference) is at `~/Development/rt1170/evkb/cores/teensy4`.
- **`cores/imxrt1176/imxrt1176.h` is AUTO-GENERATED** → any new core register defs (e.g. a CAN clock-root / LPCG or an LPSR-pad IOMUXC entry) go in **BOTH** `imxrt1176.h` **AND** `tools/gen_imxrt1176_h.py`. (Note: FlexCAN_T4 carries its own FlexCAN register block in `imxrt_flexcan.h`, so the CAN peripheral regs may not need core additions — but the clock/pin plumbing likely does.)

## Method — follow it, it's been paid for in bugs

1. **Use the superpowers workflow for every peripheral:** `brainstorming` →
   `writing-plans` → `subagent-driven-development`. Do NOT write code before the
   design is approved (the brainstorming HARD-GATE). Process skills before
   implementation skills.
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
   SDK; every fix came from reading it. (attachInterrupt's CM7 routing was solved by
   the SDK `fast_gpio` example for `evkbmimxrt1170`.)
3. **TDD via a QEMU "gate":** write a self-contained sketch + `run_qemu_*.sh` runner
   that greps its VCOM output for `PASS`/`FAIL`. Make it fail, make it pass, THEN go
   to hardware. Copy an existing gate as a template: `evkb/irq_attach_test/`,
   `evkb/spi_loopback_test/`, `evkb/wire_master_test/`, `evkb/usb_data_test/`.
4. **Hardware is the final arbiter.** A QEMU gate you built to match your own
   assumption proves *consistency, not correctness* — that bit us with a circular
   false-pass. Model what the silicon does (per SDK/RM), then let the gate **and**
   HW judge. For core-specific behaviour, confirm against the cm7 header and on the
   board.

## Verification triad + the MKR Zero test-bed

- **QEMU gate** — fast, deterministic logic check; also where model gaps surface.
- **Saleae Logic** — signal-level truth (duty, frequency, edges, bus traffic).
  Automate it: `from saleae import automation`, server `127.0.0.1:10430`,
  `TimedCaptureMode` → export raw digital CSV. Reference: `evkb/pwm_test/measure_pwm.py`.
  (Make sure Logic 2 is running with automation enabled.)
- **Hardware (EVKB)** — flash + read the board's own output.
- **MKR Zero (SAMD21, known-good Arduino) = the bench partner / test bed.** Run
  reference sketches on it to talk to the EVKB over a bus (I²C master↔slave, SPI),
  to generate known stimulus for the EVKB to read, or to A/B a behaviour against a
  trusted Arduino core. (It's how we proved the I²C flakiness was *physical*, not
  firmware.) **⚠ CAN caveat: the MKR Zero has NO CAN peripheral**, so — unlike I²C/
  SPI — it **cannot** be the bus partner for a real CAN link. A two-node CAN bus
  needs a *second* CAN node (a second EVKB, or a USB-CAN adapter). This is exactly
  why the first CAN gate uses **internal loopback** (no partner required).

## Flashing & serial (macOS gotchas)

- **LinkServer, not pyOCD.** Reliable boot: `LinkServer run MIMXRT1176:MIMXRT1170-EVKB
  <img.elf>` (load+reset+run; clears the vector-catch that a plain `flash load`
  leaves the core halted on — and a USB replug is *not* always a true POR).
  Binary: `/Applications/LinkServer_26.6.137/LinkServer`.
- **Flash fails / probe stuck?** ALWAYS `pkill -9 -f LinkServer; pkill -9 -f redlinkserv`
  **before every flash** — a stale `redlinkserv` backend keeps the probe, so the next
  flash "Reconnected to existing LinkServer process" and dies ("stub terminated return
  code 1"). A *recurring* `Unable to retrieve DAPInfo: Hardware interface transfer error`
  even after killing both = physical probe fault → **unplug/replug the MCU-Link USB (J11)**
  (resets the probe AND power-cycles the target; clears a stuck FlexSPI NOR too). See the
  `rt1170-evkb-flashing` memory note.
- **VCOM console** = `/dev/cu.usbmodem5DQ2DDHVWO5EI3` @ **115200**. Read with
  **pyserial**, never `cat` (resets the port to 9600 on open → garbage). One-shot
  banners: start the pyserial reader first, then `LinkServer run`. **Note:** the
  board also now enumerates a **native-USB CDC** port (`Serial`, VID 0x1209, e.g.
  `/dev/cu.usbmodem14534401`) — that's a *second* modem node, distinct from the
  MCU-Link VCOM; don't confuse them. See the `macos-serial-capture` memory note.
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
it.** For FlexCAN specifically:

- **`rt1176-usb-host-hid`** — established that **J47 is the CAN3 connector** (NOT the
  USB host port; that was a stale 1060-EVKB-era assumption). Also the `DMAMEM`/
  `.bss.dma` zero-init lessons if CAN ever grows DMA buffers.
- **`rt1176-spi-library-move`** + **`rt1176-wire-library-move`** — the teensy4→library
  **hybrid-port pattern** you'll reuse for FlexCAN_T4 (API shape from the lib;
  register/clock/pin logic retargeted to RT1176 in a dedicated `__IMXRT1176__` branch;
  the lib lives in its own `newdigate/…` repo, the core provides register defs +
  low-level primitives).
- **`rt1170-evkb-flashing`** + **`macos-serial-capture`** — HW flash (LinkServer) +
  VCOM capture.
- **`rt1170-qemu`** — the `qrun` gate runner. **`rt1170-evkb-git-repo`** — repo
  boundaries (evkb is its own repo; shared working tree; `cores/` is a nested
  teensy-cores repo).

If you learn something non-obvious, write a new note.

Subagent caveat: **verify a subagent's real git/file state — don't just trust its
self-report.** This project's `subagent-driven-development` runs (file-edit/build
subagents) complete reliably and notify with accurate results *when given exact
specs + the gate commands*; still, independently checking each diff/gate is cheap
insurance and is what keeps a verbatim port honest (e.g. `diff` the ported file
against its teensy4 source; re-run the gate yourself). Read-only-review subagents
are especially reliable — a read-only review is what caught the CM7 GPIO-IRQ bug.
**Watch for the CMake `file(GLOB)` trap:** adding a NEW core `.c`/`.cpp` needs a
from-scratch reconfigure (`rm -rf build && cmake -B build …`) of every test dir, not
just a rebuild (no `CONFIGURE_DEPENDS`).

## Status

**Done, HW-verified:** startup/clocks/SysTick, millis/micros/delay, GPIO + full
Arduino-header pin table (`LED_BUILTIN=3`), digitalWrite/Read/pinMode, LPUART
`Serial1`, LPADC `analogRead`, FlexRAM config, 996 MHz OverDrive voltage, FlexPWM
`analogWrite`/`Freq`/`Res`, `attachInterrupt` (all 5 modes, CM7 GPIO2/3/5/6),
`IntervalTimer` (PIT1), `tone()`/`noTone()`, USB CDC `Serial`/`SerialUSB`, the full
audio stack (I²S/SAI TX+RX + WM8962 codec, eDMA/`DMAChannel`, `EventResponder`,
`AudioStream` + `AudioInputI2S` + `AudioOutputI2S`), flash-emulated `EEPROM`, the
**SD card** (SdFat over USDHC/SDIO + FAT) and the `AudioPlaySdWav` SD→J101 capstone,
and **USB Host** (`USBHost_t36` HID keyboard + mouse) + **USB MIDI**. `Wire` (I²C
master+slave) and `SPI` (LPSPI1 master + full-duplex DMA) have been **moved into
their own `newdigate/Wire` + `newdigate/SPI` libraries** (the hybrid-port boundary
pattern). QEMU has faithful models for LPUART, LPSPI, LPI2C, FlexSPI, SAI, eDMA, the
ChipIdea USB (device + host + MIDI), USDHC — **and FlexCAN** (see below).

**Next: FlexCAN (CAN3 on J47)** — this session. Remaining backlog after: RTC (SNVS)
/ Watchdog (WDOG/RTWDOG) / CAN-FD / the various display + sensor libraries.

## First move — FlexCAN (CAN3 on J47), internal-loopback gate first

**Goal:** bring up **FlexCAN on CAN3** (the EVKB's **J47** connector), Teensy-style
via the **`FlexCAN_T4`** API (`CAN_message_t`, `read()` / `write()`, mailbox
callbacks). **Success v1 = a frame transmitted from one message buffer loops back
(CTRL1.LPB internal loopback) into an Rx mailbox with byte-exact data** — this is
both QEMU-gateable **and** a clean first HW proof that needs **no external bus, no
partner, no wiring**. Start minimal (classic 8-byte frames, polled/interrupt);
CAN-FD and a real two-node bus are follow-ons.

**★ Read this before you plan — the QEMU landscape is NOT what the backlog assumed.**
Unlike SAI (net-new model), **a FlexCAN model already exists and is already wired in.**
Confirm it first, then scope the *actual* remaining work.

1. **SDK first (do NOT guess a single CAN register).** Read the FlexCAN examples for
   *this* board: `~/Development/mcuxsdk-examples/_boards/evkbmimxrt1170/driver_examples/
   flexcan/` — variants `loopback` (functional, internal MB→MB, **no board settings /
   no wiring** → your gate template), `interrupt_transfer` (a real **two-board** bus),
   `loopback_transfer`, `ping_pong_buffer_transfer`, each with `cm7/` + `cm4/` +
   `pin_mux.c`. That example is canonical for:
   - **Instance = `CAN3`** (`#define EXAMPLE_CAN CAN3`), base **`0x40C3C000`**, **IRQ 48**
     (`CAN3_IRQn`; CAN1=`0x400C4000`/44, CAN2=`0x400C8000`/46) — confirm in the **cm7**
     header `MIMXRT1176_cm7_COMMON.h`.
   - **Pins → J47:** **CAN3_TX = `GPIO_LPSR_00`** (pin N6, `IOMUXC_GPIO_LPSR_00_FLEXCAN3_TX`),
     **CAN3_RX = `GPIO_LPSR_01`** (pin R6, `IOMUXC_GPIO_LPSR_01_FLEXCAN3_RX`). **Note the
     `GPIO_LPSR` pad domain** — a *different* IOMUXC region than the AD/EMC/SD pins the
     other drivers used, so the mux/pad-config path differs.
   - **Clock:** source **OSC24M** (`FLEXCAN_CLOCK_SOURCE_SELECT=1`, divider 1 → 24 MHz);
     clock root **`kCLOCK_Root_Can3`** (root index 24), LPCG **`kCLOCK_Can3`** (85).
   - The **freeze → configure → normal-mode** handshake (MCR.FRZ|HALT → FRZACK; clear
     HALT → run) and MCR.**MAXMB reset = 0x0F** (16 mailboxes).
   - **Real-bus board settings (for later):** remove jumpers **J102 + J103**, male-to-male
     CAN cable between two boards' **J47**. And **confirm the on-board CAN transceiver**
     (part + any standby/EN GPIO) from the EVKB schematic — the loopback gate doesn't
     need it, but a real bus does.
   Cross-check Zephyr (`~/Development/zephyr` `nxp,flexcan` dts/driver).

2. **Teensy reference for the API shape (hybrid port, like `SPI`/`Wire`).**
   `~/Development/FlexCAN_T4` — a templated header/`.tpp` library
   (`FlexCAN_T4<CAN3, RX_SIZE_256, TX_SIZE_16>`, `CAN_message_t`, `read()`/`write()`,
   mailbox callbacks; regs in `imxrt_flexcan.h`, impl in `FlexCAN_T4.tpp`). It has an
   **`#if defined(__IMXRT1062__)`** branch (Teensy 4, CAN1/2/3) and a Kinetis branch —
   **but NO 1176 branch.** Port the same way as SPI/Wire: **add an `__IMXRT1176__`
   branch**, retargeting the seams — `setClock()`/`CCM_CCGR…` → RT1176 **LPCG +
   clock-root**; the `IOMUXC_SW_MUX/PAD_CTL_PAD_…` pin mux → the **LPSR** pads +
   `FLEXCAN3_RX_SELECT_INPUT` (daisy-chain); base addr in `imxrt_flexcan.h`. Likely
   lands as a **`newdigate/FlexCAN` library** per the boundary pattern (core keeps
   register/clock/pin primitives; the Arduino library lives in its own repo). **Defer
   CAN-FD** (`FlexCAN_T4FD.tpp`) — the QEMU model has no FD path yet.

3. **QEMU — a model ALREADY EXISTS; verify + extend, don't build net-new.**
   `~/Development/qemu2/hw/net/can/imxrt_flexcan.c` (~400 lines, header
   `include/hw/net/imxrt_flexcan.h`, `CONFIG_IMXRT_FLEXCAN`) already models the
   freeze-mode handshake, self-clearing soft reset, MDIS/LPMACK low-power handshake,
   message-buffer transmit (`CODE=TX_DATA`), **internal loopback (`CTRL1.LPB`)** — a
   TX'd frame is matched against the accept-all Rx MBs and copied in with its IFLAG —
   and per-buffer IFLAG/IMASK + the ORed MB interrupt. It's **already instantiated in
   `hw/arm/fsl-imxrt1170.c`** for flexcan1/2/3 at `0x400C4000`/`0x400C8000`/`0x40C3C000`,
   IRQs 44/46/48. **There is already a loopback gate to copy:**
   `tests/functional/arm/imxrt1062/flexcan_loopback_test.c` (bare-metal: MAXMB-reset
   check → freeze → `CTRL1.LPB` → MB0=`RX_EMPTY` accept-all → MB8=`TX_DATA` → assert
   loopback landed in MB0 + TX reverted to `TX_INACTIVE`; greps **`FLEXCAN LB OK`** via
   `test_imxrt1170.py`-style runner). So the loopback deliverable is mostly **(a) write
   the 1170-side gate** (CAN3 @ `0x40C3C000`, driven by *your* FlexCAN_T4 port, not raw
   pokes) and **(b) extend the model only where the driver touches registers the model
   currently stubs.** ★ **Known model gaps = the real QEMU scope if you go past
   loopback:** (i) it does **NOT** attach to QEMU's CAN bus subsystem, so it can't
   exchange frames with another node (no multi-node/real-bus test in QEMU); (ii)
   **no CAN-FD** (classic 8-byte MBs only). Per the method, extending the model to
   mirror the silicon is a first-class goal — but scope it to what the gate exercises.

4. **HW (real arbiter).** Flash; **internal loopback on CAN3/J47 first** (no wiring —
   the clean first proof). Then, for a real bus: **the MKR Zero can't be the partner
   (no CAN peripheral)** — use a **second EVKB** (the `interrupt_transfer` example:
   remove J102/J103, CAN cable between the two J47s) or a **USB-CAN adapter**, and
   confirm the on-board transceiver + any standby GPIO from the schematic. **Saleae**
   on CAN3_TX/RX (`GPIO_LPSR_00/01`) to see the bit timing + framing. CAN bit-timing
   is exactly where a QEMU-vs-silicon clock gap would hide — trust the Saleae/scope
   over the model.

Method: **brainstorm → spec → `writing-plans` → `subagent-driven-development`, gate-
first** (no code before the brainstorming hard-gate). The SDK `flexcan` example + the
cm7 header are ground truth — never guess a clock divisor, an LPSR pad mux, or a
mailbox register. **First action: invoke the brainstorming skill and explore the
existing QEMU FlexCAN model / the 1062 loopback gate / FlexCAN_T4 / the EVKB CAN3
pin+clock landscape before proposing a design.**
