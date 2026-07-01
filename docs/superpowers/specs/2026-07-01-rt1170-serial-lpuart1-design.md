# RT1176 Arduino Core — Phase 1: Interrupt-driven `Serial1` on LPUART1

**Date:** 2026-07-01
**Status:** Approved (design), pending implementation plan
**Depends on:** Phase 0 core (2026-06-30-rt1170-arduino-core-phase0-design.md)

## Goal

Bring up a full-parity Teensyduino `HardwareSerial` — TX + RX ring buffers, an
LPUART1 interrupt handler, and the `Print`/`Stream` API — on the MIMXRT1170-EVKB's
VCOM console (LPUART1). Verified first in the custom QEMU model, then on hardware
at 115200 baud through the MCU-Link virtual COM port.

This unlocks `Serial1.print()` debugging for all later phases and provides the
first real-clock validation of the CCM configuration (baud rate is derived from
the peripheral clock, so garbage output means the clock tree is wrong).

## Background / current state

Phase 0 delivered a working RT1176 core: XIP boot, 996 MHz CCM bring-up, GPIO
output, and DWT-cycle-based `millis()/micros()/delay()`, verified by a ~2 Hz blink
on the EVKB. The core deliberately has **no peripheral interrupt support**:

- `startup.c` installs a flash vector table `_VectorsFlash[16 + 16]` — only a
  16-entry NVIC tail. LPUART1 is IRQ 20, out of range.
- `core_pins.h` has no `attachInterruptVector`, no NVIC enable/priority macros,
  no `IRQ_NUMBER_t` enum.
- `imxrt1176.h` has no LPUART register definitions.

The custom QEMU board (`~/Development/qemu2`) **already models the LPUART fully**
(`hw/char/imxrt_lpuart.c`): all 12 instances mapped (LPUART1 @ `0x4007C000`),
BAUD/STAT/CTRL/DATA/FIFO/WATER registers, an RX FIFO, and the combined TX/RX
interrupt (IRQ 20 for LPUART1). Transmission is instantaneous (TDRE and TC always
asserted). This means the interrupt path is exercised in emulation, not just
polled TX.

teensy4's `HardwareSerial` (`~/Development/rt1170/evkb/cores/teensy4/`) is a
761-line interrupt-driven driver, but it is RT106x-specific: `CCM_CCGR` clock
gates, `IOMUXC_LPUART6_*_SELECT_INPUT` daisy-chains, XBAR trigger inputs, and
multi-candidate pin-mux tables. The ring-buffer + ISR + `Print`/`Stream`
structure ports cleanly; the hardware-access layer is rewritten for RT1176.

## Architecture

Four layers, bottom-up, each independently testable:

1. **Interrupt infrastructure** (new, foundational, reusable)
2. **LPUART register layer** (generated defs)
3. **Driver** (`Print`/`Stream`/`HardwareSerialIMXRT`, one instance)
4. **Board bring-up** (clock root, baud divisor, console pin mux)

### Layer 1 — Interrupt infrastructure

- Add a RAM vector table `_VectorsRam[16 + NVIC_NUM_INTERRUPTS]` where
  `NVIC_NUM_INTERRUPTS = 217` (RT1176 CM7 IRQ count). Placed in DTCM, 1024-byte
  aligned (covers the 233-entry table's alignment requirement).
- `ResetHandler` (`startup.c`) copies `_VectorsFlash` → `_VectorsRam` and sets
  `SCB_VTOR = (uint32_t)_VectorsRam` so handlers can be installed at runtime.
- Port from teensy4 into `core_pins.h`:
  - `IRQ_NUMBER_t` enum. Minimum required: `IRQ_LPUART1 = 20 .. IRQ_LPUART12 = 31`.
    (Full enum may be emitted, but only these are needed now — YAGNI.)
  - `attachInterruptVector(IRQ_NUMBER_t irq, void (*function)(void))` —
    writes `_VectorsRam[16 + irq] = function`.
  - `NVIC_ENABLE_IRQ(n)`, `NVIC_DISABLE_IRQ(n)`, `NVIC_SET_PRIORITY(n, p)`,
    `NVIC_CLEAR_PENDING(n)` — standard Cortex-M NVIC register macros.

This is the Teensy-faithful model (runtime-installable handlers) and is reused by
every future interrupt-driven peripheral (timers, other UARTs, I2C, SPI).

### Layer 2 — LPUART register layer

- Extend `tools/gen_imxrt1176_h.py` to emit the LPUART register block for LPUART1
  at base `0x4007C000`: `VERID, PARAM, GLOBAL, PINCFG, BAUD, STAT, CTRL, DATA,
  MATCH, MODIR, FIFO, WATER` (register offsets from the RT1176 CMSIS
  `LPUART_Type` struct).
- Emit `LPUART1..LPUART12` base addresses for future instances (defs only; no
  driver instances beyond `Serial1` in this phase).
- Regenerate `imxrt1176.h`. This is consistent with how GPIO/CCM/IOMUX regs were
  produced in Phase 0.

### Layer 3 — Driver

- Port largely verbatim (formatting + base classes, hardware-agnostic):
  `Print.cpp`, `Print.h`, `Printable.h`, `Stream.h` (and `Stream.cpp` if the
  ported `Print` requires it).
- Port and adapt for RT1176:
  - `HardwareSerial.h` — the `HardwareSerialIMXRT` class and `hardware_t` struct.
    Collapse the multi-candidate pin-mux arrays to the single fixed console pin
    pair (the EVKB hard-wires LPUART1 to the MCU-Link VCOM). Replace the
    `CCM_CCGR` gate field with the RT1176 LPCG gate. Keep TX/RX ring buffer
    sizes and the `Stream` API surface.
  - `HardwareSerial.cpp` — port `begin()`, `end()`, `write()`, `read()`,
    `available()`, `availableForWrite()`, `flush()`, `peek()`, and the combined
    ISR (`IRQHandler`). Ring-buffer logic is unchanged; register access is
    rewritten for the RT1176 LPUART register names/semantics.
  - `HardwareSerial1.cpp` — one instance: `Serial1` bound to LPUART1, its
    `hardware_t`, and its TX/RX buffers; installs `IRQHandler_Serial1` via
    `attachInterruptVector(IRQ_LPUART1, ...)` in `begin()`.
- `serialEvent()` — weak hook, called from `yield()` when RX data is available
  (matches Phase 0's `yield()` weak stub, now given a body path).

**Naming:** LPUART1 → `Serial1`, Teensy-faithful. `Serial` remains reserved for
the USB CDC device in a later phase. No `Serial`→`Serial1` alias in this phase
(decided during design review); sketches use `Serial1`.

### Layer 4 — Board bring-up (hardware-specific)

- **Clock:** configure the LPUART1 clock root (`CCM_CLOCK_ROOT25`) — source
  OscRC48MDiv2 (24 MHz), divider 1 — and enable the LPUART1 LPCG clock gate.
  QEMU ignores the clock functionally (instantaneous TX) but hardware requires it.
- **Baud:** compute `BAUD.OSR` and `BAUD.SBR` for 115200 from the 24 MHz root in
  `begin(baud)` (standard LPUART oversampling formula; OSR in [4..32], pick the
  divisor pair with least error). Set in the `BAUD` register.
- **Pins:** IOMUX the EVKB console pads to LPUART1 TXD/RXD. Exact pads and ALT
  modes are confirmed from the SDK board files (`pin_mux.c` /
  `evkbmimxrt1170`) during the plan — the EVKB routes LPUART1 to the MCU-Link
  VCOM (the same path Phase-0 Zephyr `hello_world` used at 115200).

## Data flow

- **TX:** `Serial1.write(b)` → push to TX ring buffer, enable TIE → LPUART1 ISR
  drains buffer into `DATA` while `STAT.TDRE`, disables TIE when empty.
  `flush()` blocks until the buffer and shift register are empty (`STAT.TC`).
- **RX:** byte arrives → `STAT.RDRF` → LPUART1 ISR reads `DATA`, pushes to RX ring
  buffer. `available()`/`read()`/`peek()` operate on the ring buffer.
  `yield()` calls `serialEvent()` when `available() > 0`.

## Error handling

- **RX overrun** (`STAT.OR`): ISR clears the flag (write-1-to-clear) and drops the
  byte; no hang. Matches teensy4 behaviour.
- **TX buffer full:** `write()` blocks (spins on `yield()`) until space frees, per
  Arduino `HardwareSerial` contract.
- **`begin()` before pins/clock ready:** `begin()` is the single entry point that
  configures clock root, LPCG, pins, baud, CTRL (TE/RE/RIE), NVIC — order fixed so
  the peripheral is fully live before the ISR is enabled.

## Testing / verification

1. **QEMU-first** (primary regression gate): a sketch prints a startup banner and
   an incrementing counter on `Serial1`. Run the firmware under the custom QEMU
   `mimxrt1170-evk` machine with `-serial file:<out>` (or `-serial stdio`
   captured). Assert the banner and at least N counter lines appear. Because QEMU
   models the LPUART IRQ, this exercises the full ISR path, not just polled TX.
2. **Hardware:** flash via LinkServer
   (`flash MIMXRT1176:MIMXRT1170-EVKB load <img>`), capture the MCU-Link VCOM
   with the pyserial helper at 115200 (NOT `cat` — it resets baud to 9600), and
   confirm output matches QEMU. Secondary check: compare `Serial1.println(millis())`
   cadence against wall-clock to validate the 996 MHz-derived time base.

## Out of scope (YAGNI — deferred to later phases)

- USB CDC `Serial` device.
- LPUART instances other than `Serial1` (register bases are emitted, but no
  driver instances).
- DMA-based serial transfers and XBAR triggering.
- 9-bit mode, RS-485 RTS/CTS flow control, `addMemoryForRead/Write` buffer
  resizing.
- Full `IRQ_NUMBER_t` enum beyond the LPUART range.
