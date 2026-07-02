# RT1176 Arduino Core — Phase 4: `Wire` (I²C master + slave) on LPI2C

**Depends on:** Phase 1 interrupt infra (RAM vectors + NVIC + `attachInterruptVector`),
the RT1176 driver idioms established by `Serial1`/`analog.c` (LPCG gate + CCM clock
root + IOMUXC mux + `hardware_t` table), and the register generator
`tools/gen_imxrt1176_h.py`.

**Goal:** Arduino-compatible `Wire` (TwoWire) on the RT1176 LPI2C peripheral —
blocking master and interrupt-driven slave — verified in QEMU and on the EVKB.

**Tech stack:** C/C++ core (register-poke, no NXP SDK at runtime); QEMU device
model (`hw/i2c/imxrt_lpi2c.c`) + machine wiring; QEMU `eeprom_at24c` as an
automated bus slave; SSD1306 OLED as the hardware master-mode target.

---

## Overview

The RT1176 core has GPIO, `delay`/`millis`, interrupt infra, `Serial1` (TX+RX),
and `analogRead` (LPADC). This phase adds `Wire`, the Arduino I²C library, in two
independently testable stages:

- **Stage A — I²C master (blocking).** The 90% path: talk to sensors, EEPROMs,
  and displays. Synchronous engine modelled on `analogRead` (issue a command,
  poll status flags with a bounded timeout). Immediately testable in QEMU
  (AT24C EEPROM) and on hardware (SSD1306 OLED).
- **Stage B — I²C slave (interrupt-driven).** The EVKB responds as an I²C target
  with `onReceive`/`onRequest` callbacks. Necessarily ISR-driven because the
  transaction timing is controlled by the external master. Requires teaching the
  QEMU model to act as a bus target and a self-contained loopback test.

The two stages share the register block, `hardware_t` table, clock/pin bring-up,
buffers, and generator changes — hence one spec, two staged implementation plans.

## Target hardware

`Wire` (the primary instance) binds to the **EVKB Arduino-header I²C bus**. The
exact instance + pads are one of:

- **LPI2C1** — `GPIO_AD_08` (SCL, ALT1) / `GPIO_AD_09` (SDA, ALT1)
- **LPI2C5** — `GPIO_LPSR_05` (SCL, ALT0) / `GPIO_LPSR_04` (SDA, ALT0) — also the
  onboard sensor/codec bus

**Resolved during planning** by cross-referencing the MIMXRT1170-EVKB schematic /
Arduino-header silkscreen (D14=SDA / D15=SCL). This choice does not affect the
architecture — only the specific `hardware_t` pad/clock-root/LPCG/IRQ constants.

Master-mode HW target: **SSD1306 128×64 mono OLED**, I²C address **0x3C**.
Slave-mode HW master: **a second dev board** running a portable master sketch.

## Architecture

### Firmware

Bottom-up, mirroring `HardwareSerial`:

1. **Register defs (generator).** Add `LPI2C` to `WANTED` in
   `gen_imxrt1176_h.py`; regenerate `imxrt1176.h` to emit, for the instance(s)
   used, `LPI2Cn_BASE`, `IRQ_LPI2Cn`, the **master** block
   (`MCR, MSR, MIER, MDER, MCFGR0..3, MDMR, MCCR0..1, MFCR, MFSR, MTDR, MRDR`)
   and the **slave** block
   (`SCR, SSR, SIER, SDER, SCFGR1..2, SAMR, SASR, SRDR, STDR`). Same mechanical
   pattern as the existing LPUART/LPADC blocks.

2. **`TwoWire` class (`Wire.h` / `Wire.cpp`).** Arduino-standard API backed by a
   per-instance `hardware_t` (fields analogous to `HardwareSerial`'s):

   ```
   hardware_t {
     uint8_t   instance;              // 0-based LPI2C index
     IRQ_NUMBER_t irq;
     void (*irq_handler)(void);
     volatile uint32_t &lpcg_register;      // CCM->LPCG[n].DIRECT
     volatile uint32_t &clock_root_reg;     // CCM->CLOCK_ROOT[n].CONTROL
     uint32_t  clock_root_val;              // mux|div for 24 MHz functional clock
     volatile uint32_t &scl_mux_reg;  uint32_t scl_mux_val;  volatile uint32_t &scl_pad_reg;
     volatile uint32_t &sda_mux_reg;  uint32_t sda_mux_val;  volatile uint32_t &sda_pad_reg;
     volatile uint32_t &scl_select_input_reg; uint32_t scl_select_input_val;
     volatile uint32_t &sda_select_input_reg; uint32_t sda_select_input_val;
     uint16_t  irq_priority;
   };
   ```

   Pads are configured **open-drain (ODE) with the pad keeper/pull enabled**;
   real bus pull-ups are provided by the breakout. SCL/SDA `SELECT_INPUT` daisy
   registers are set as with LPUART RX.

3. **Master engine (blocking, bounded).**
   - `begin()` — ungate LPCG → set CCM clock root (24 MHz) → mux SCL/SDA pads
     (open-drain) → set input daisy → clear any latched `MSR` error/bus-busy →
     program `MCCR0` for the bit rate (default 100 kHz; `setClock(freq)`
     recomputes) → `MCR.MEN=1`.
   - `beginTransmission(addr)` buffers the target address; `write(...)` fills a
     32-byte TX buffer.
   - `endTransmission(stop=true)` drives the FIFO command words
     (START+addr(W), TXD bytes, optional STOP), polling `MSR` for `NDF`
     (NACK) / `SDF` (STOP) inside a **guard-loop timeout**. Returns Arduino
     status: `0` ok, `2` address NACK, `3` data NACK, `4` other/bus error,
     `5` timeout.
   - `requestFrom(addr, n, stop=true)` issues START+addr(R) + an RXD(n) command
     and drains `MRDR` into the 32-byte RX buffer; `read()`/`available()`/`peek()`
     consume it.

4. **Slave engine (interrupt-driven).**
   - `begin(addr)` — clock/pin bring-up as above → `SAMR = addr` → enable slave
     IRQs in `SIER` (address-valid, RX-ready, TX-ready, STOP) → `SCR.SEN=1` →
     `attachInterruptVector(irq, handler)` at `irq_priority`.
   - ISR: on **address-match** start a transaction; **RX** bytes append to the
     buffer; on **STOP** after a write, fire `onReceive(count)`; on a master
     **read** (TX-ready), call `onRequest()`, which `write`s response bytes into
     `STDR`. No blocking work in the ISR.

5. **Buffers.** Fixed 32-byte TX/RX per instance (`BUFFER_LENGTH`), no malloc.

6. **Instances.** Ship **`Wire`** (Arduino-header instance) only. Additional
   instances are scaffolded only when actually brought up and tested (YAGNI).

**Files:** `Wire.h`, `Wire.cpp` (class + master + slave engines),
`Wire_instances.cpp` (`hardware_t` table(s), the `Wire` object, ISR
trampolines); `tools/gen_imxrt1176_h.py` + regenerated `imxrt1176.h`.

### QEMU

The master model (`hw/i2c/imxrt_lpi2c.c`, `CONFIG_IMXRT_LPI2C`) already exists,
creates a per-instance `I2CBus` (`i2c_init_bus`, line ~376), and all six
instances are instantiated / MMIO-mapped / IRQ- and DMA-wired in
`hw/arm/fsl-imxrt1170.c`. **No slave is attached to any LPI2C bus today.**

- **Stage A (master):** attach an **AT24C EEPROM** (`hw/nvram/eeprom_at24c.c`) to
  the Arduino-header instance's bus in the **machine init** (`mimxrt1170-evk`),
  keeping the SoC generic. No new modeling.
- **Stage B (slave):** extend the model with a **slave persona** — an `I2CSlave`
  whose `event`/`recv`/`send` callbacks shadow the slave register block
  (`SSR`/`SASR`/`SRDR`/`STDR`) and raise the peripheral IRQ. A
  **two-instances-on-one-bus loopback** (instance B's slave persona attached to
  instance A's bus) lets one firmware sketch drive A-as-master → B-as-slave. The
  exact QOM structure (slave-as-child vs. a bridge `I2CSlave`) is a planning
  detail; the shadow-registers-and-raise-IRQ behaviour is fixed.

## Testing

**QEMU gates (per stage, automated):**
- **Master** (`wire_master_test`): write N bytes to an AT24C address, read back,
  assert equality; assert `endTransmission()` returns `0` for the present address
  and `2` for an absent one; a bus scan finds exactly the EEPROM address.
- **Slave** (`wire_slave_test`): master instance writes a known payload to the
  slave address → assert `onReceive` fired with the right bytes; master
  `requestFrom` the slave → slave `onRequest` supplies a known response → assert
  the master read it back.
- **Regression:** existing `serial_test`, `analog_test`, `serial_test_rx` QEMU
  gates stay green.

**Hardware acceptance (per stage):**
- **Master (SSD1306):** (1) `endTransmission(0x3C)==0` over `Serial1` proves
  addressing/ACK; (2) SSD1306 init + full 1 KB framebuffer blit renders a
  recognizable pattern (geometric pattern or a tiny built-in font — no external
  graphics-library dependency) — the human-verified end-to-end proof. Each step
  is reported over `Serial1` so a failure localizes.
- **Slave:** the second dev board's portable master sketch writes a payload and
  reads a response from the EVKB slave, printing pass/fail; the EVKB mirrors
  received bytes over `Serial1` for cross-check.

## Error handling

**Master:**
- Status codes as above; a **bounded guard-loop timeout** (as in `analogRead`)
  guarantees a stuck SDA (e.g. missing pull-ups) returns an error instead of
  wedging the core.
- Bus-busy / arbitration-loss / FIFO error detected via `MSR`; recover by
  flushing the FIFOs (`MCR.RRF/RTF`) and, if wedged, `MCR.RST`. `begin()` clears
  latched `MSR` error bits before enabling the master.

**Slave:**
- RX overrun / TX underrun (`SSR`) tracked and recoverable; an unexpected STOP
  mid-transfer resets the transaction cleanly. No blocking in the ISR.

## Out of scope (YAGNI — deferred)

- Interrupt+DMA master (Teensy-style). The blocking engine is sufficient for
  sensors/EEPROMs/displays; there is no teensy4 I²C source to port regardless.
- 10-bit addressing, clock stretching as master, SMBus PEC/alert, multi-master
  arbitration beyond error-recovery, `setSDA`/`setSCL` software-I²C bit-bang.
- Additional `Wire1..Wire6` instances beyond the one brought up and tested.
- A full SSD1306 graphics library — the test sketch carries only the minimal
  init + blit needed to prove the bus.

## Done criteria

- **Stage A:** `wire_master_test` QEMU gate passes (EEPROM round-trip + scan);
  on hardware, `endTransmission(0x3C)==0` and the SSD1306 shows the test pattern.
- **Stage B:** `wire_slave_test` QEMU loopback gate passes (`onReceive` +
  `onRequest` verified); on hardware, the second-board master exchanges a
  payload with the EVKB slave. All prior QEMU gates remain green.
