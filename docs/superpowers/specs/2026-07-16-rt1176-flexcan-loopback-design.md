# RT1176 FlexCAN (CAN3 / J47) — internal-loopback bring-up

**Date:** 2026-07-16
**Status:** Approved (brainstorming) → ready for `writing-plans`
**Peripheral:** FlexCAN on **CAN3** (EVKB **J47** connector), Teensy-style via the **`FlexCAN_T4`** API.

---

## 1. Goal & success criteria

Bring up **FlexCAN on CAN3** of the MIMXRT1176-EVKB (Cortex-M7) via the Teensy
**`FlexCAN_T4`** API (`CAN_message_t`, `read()`/`write()`, mailbox callbacks),
**hybrid-ported** exactly like the SPI/Wire core→library moves (library API shape +
our RT1176 register/clock/pin logic in a new `__IMXRT1176__` branch).

**v1 success = a classic 8-byte frame written from a Tx message buffer loops back
(`CTRL1.LPB` internal loopback) into an Rx message buffer with byte-exact data and
matching identifier** — proven in a QEMU gate **and** on real silicon. Internal
loopback needs **no external bus, no partner, no wiring** — it is both QEMU-gateable
and the clean first hardware proof.

Two gates, in order:
1. **Polled** — `write()` then poll `read()`.
2. **Interrupt** — `onReceive()` callback driven by the CAN3 ISR.

**Explicitly deferred:** CAN-FD (`FlexCAN_T4FD`), a real two-node bus, and CAN1/CAN2
pin/clock bring-up.

---

## 2. Ground truth established during exploration

| Fact | Source |
|---|---|
| CAN3 base = `0x40C3C000`, IRQ = 48; CAN1=`0x400C4000`/44, CAN2=`0x400C8000`/46 | SDK cm7 header; `qemu2/hw/arm/fsl-imxrt1170.c:183-187` |
| CAN3 clock: OSC24M (`FLEXCAN_CLOCK_SOURCE_SELECT=1`), ÷1 → **24 MHz**; clock root `kCLOCK_Root_Can3` = **24**, LPCG `kCLOCK_Can3` = **85** | SDK `flexcan/loopback/cm7/app.h` + `hardware_init.c`; `fsl_clock.h:589,825` |
| CAN3 pins → J47: **`GPIO_LPSR_00`=FLEXCAN3_TX**, **`GPIO_LPSR_01`=FLEXCAN3_RX** (LPSR pad domain) | task brief; SDK `interrupt_transfer/pin_mux.c` (to be read during planning) |
| Internal loopback needs **no pin mux** — SDK loopback `pin_mux.c` configures only the debug UART | SDK `flexcan/loopback/pin_mux.c` |
| FlexCAN_T4 register access is **base-relative** (`FLEXCANb_MCR(b)=*(b+0)` …) → portable to any base | `FlexCAN_T4/imxrt_flexcan.h` |
| FlexCAN_T4 is **MIT** (Antonio Brewer). Its `imxrt_flexcan.h` carries a stale `kinetis_flexcan.h` header comment → per-file license audit before publishing | `FlexCAN_T4/LICENSE`, `imxrt_flexcan.h:1-8` |
| Core has **no** FlexCAN defs, but has `attachInterruptVector`/`_VectorsRam`/`NVIC_ENABLE_IRQ` and the CCM clock-root/LPCG pattern | `core_pins.h:74-75`, `imxrt1176.h:107-112,329-330` |
| `IRQ_NUMBER_t` is hand-maintained in `core_pins.h` (`IRQ_LPSPI6 = 43`) — **not** generated | `core_pins.h:42-48` |
| Adjacent clock defs: root25 @ `0x40CC0C80`, LPCG86 @ `0x40CC6AC0` → **CAN3 root24 @ `0x40CC0C00`, LPCG85 @ `0x40CC6AA0`** (pattern: root n @ `0x40CC0000+n*0x80`, LPCG n @ `0x40CC6000+n*0x20`) | `imxrt1176.h:329-330`; `gen_imxrt1176_h.py:23-27,80-85` |

### 2.1 FlexCAN_T4 `begin()` sequence (already supported by the QEMU model)
`begin()` (`FlexCAN_T4.tpp:93-180`) after the platform clock/IRQ block runs a common
config the existing model already handles: `setTX();setRX();` → clear `MCR.MDIS` →
enter freeze → `CTRL1|=LOM` → `softReset()` → wait `FRZ_ACK` → set
`MCR.SRXDIS|IRMQ|AEN|LPRIO_EN|SLF_WAK|WAK_SRC`, clear DMA/FD bits →
`CTRL2|=RRS|EACEN|MRP` → `disableFIFO()` (clears `MAXMB+1 = 16` mailboxes — matches the
model's tuned `MAXMB=0x0F` reset) → exit freeze → `NVIC_ENABLE_IRQ`.

### 2.2 The loopback enable sequence and the honesty gap
- `setBaudRate(baud, TX)` (default) **clears `CTRL1.LOM`** (`tpp:540`).
- `enableLoopBack(1)` (`tpp:1784`) **clears `MCR.SRXDIS`** (bit 17) *and* sets
  `CTRL1.LPB` (bit 12).
- On silicon, if `SRXDIS` stays set the module does **not** receive its own
  transmitted frame even under `LPB`. The current QEMU `flexcan_deliver()` ignores
  `SRXDIS` → it would deliver anyway → a driver that skipped `enableLoopBack()` would
  pass in QEMU but fail on hardware (circular pass). **This spec closes that gap.**

Correct sketch flow: `begin()` → `setBaudRate(1000000)` → `enableLoopBack(1)` → set up
an accept-all Rx mailbox → `write()` → `read()`.

---

## 3. Architecture — four artifacts

| Artifact | Repo (git root) | Nature of change |
|---|---|---|
| **`newdigate/FlexCAN`** library | NEW, seeded from `~/Development/FlexCAN_T4` (MIT) → new github repo | Add `__IMXRT1176__` branch to 5 seams; gates live in `tests/` |
| **Core `imxrt1176`** | `~/Development/rt1170/evkb/cores/imxrt1176` (github teensy-cores) | Clock + LPSR-pin + IRQ plumbing only |
| **QEMU model** | `~/Development/qemu2` (gitlab qemu-rt1170) | `SRXDIS`-gate the loopback delivery |
| **Gates** | `newdigate/FlexCAN/tests/` | Two gate dirs + qrun runners |

**Boundary (SPI/Wire pattern):** the **library** owns the API surface and the FlexCAN
register block (its own `imxrt_flexcan.h`); the **core** owns clock/pin/IRQ
primitives. **No core FlexCAN register overlay** (unlike `IMXRT_LPSPI_t`/`IMXRT_LPI2C_t`)
— FlexCAN_T4 carries its own. **No `evkb→FlexCAN` dependency** — the gates live in the
library repo, like `newdigate/SPI/tests`.

---

## 4. Library port — the `__IMXRT1176__` seams

Seed a new repo `~/Development/FlexCAN` from `~/Development/FlexCAN_T4`. Add a
`#elif defined(__IMXRT1176__)` branch to each 1062-gated seam (leave the 1062/Kinetis
branches untouched):

1. **`CAN_DEV_TABLE`** (`FlexCAN_T4.h:285-298`): 1176 entry →
   `CAN1=0x400C4000`, `CAN2=0x400C8000`, **`CAN3=0x40C3C000`**.
2. **Top include gate + `_CAN1/2/3` ISR pointers** (`.h:334-338`) + the
   `FlexCAN_T4::setClock` decl gate (`.h:504-506,529-531`) + the FD-include gate
   (`.h:563-566`): admit `__IMXRT1176__` where 1062 is admitted, **except** keep the
   `FlexCAN_T4FD` includes 1062-only. Widen the top `#include "Arduino.h"` path so
   RT1176 (not `TEENSYDUINO`) compiles the class (mirror the SPI include-gate widening).
3. **`begin()`** (`.tpp:93-180`): 1176 branch sets `busNumber` + `nvicIrq`
   (`IRQ_CAN1/2/3`), installs the ISR (`attachInterruptVector(IRQ_CAN3, flexcan_isr_can3)`),
   enables **CAN3 clock root 24** (mux=1 OSC24M, ÷1) + **LPCG 85**. Then the existing
   common config flows unchanged.
4. **`setTX()/setRX()`** (`.tpp:558-650`): 1176 branch muxes `GPIO_LPSR_00`→FLEXCAN3_TX,
   `GPIO_LPSR_01`→FLEXCAN3_RX and writes the FLEXCAN3_RX select-input daisy-chain.
   **Values taken verbatim from the SDK `interrupt_transfer/pin_mux.c` + cm7 header
   `IOMUXC_GPIO_LPSR_00_FLEXCAN3_TX`/`_01_FLEXCAN3_RX` — no guessing.** *Not exercised
   by the loopback gate* (internal loopback does not route to the pads); verified on
   the real-bus follow-on.
5. **`setClock()/getClock()`** (`.tpp:71-91`): 1176 branch — `getClock()` returns
   `24000000` so `setBaudRate`'s timing math is correct; `setClock()` sets the CAN3
   root (or is a no-op since `begin()` already set it).
6. **`setBaudRate()`** (`.tpp:474-544`): 1176 branch reuses the 1062 classic-timing
   math (same 24 MHz source clock) and applies the same `CTRL1.LOM` handling.
7. **ISR dispatch**: 1176 `flexcan_isr_can1/2/3` route to `_CAN1/2/3->flexcan_interrupt()`
   (extend the 1062 pattern).

**License audit** (per the prefer-permissive rule): confirm FlexCAN_T4.h/.tpp are MIT;
audit `imxrt_flexcan.h` (stale `kinetis_flexcan.h` comment) and `circular_buffer.h`
per-file headers before publishing.

---

## 5. Core changes (`evkb/cores/imxrt1176`)

1. **`core_pins.h`** (hand-maintained enum): add `IRQ_CAN1=44 … IRQ_CAN3=48` to
   `IRQ_NUMBER_t` (fill the 44–48 range; the ORed message-buffer IRQ is all
   FlexCAN_T4 uses).
2. **`imxrt1176.h` AND `tools/gen_imxrt1176_h.py`** (both — the header is generated):
   - `CCM_CLOCK_ROOT24_CONTROL` @ `0x40CC0C00` (CAN3 root).
   - `CCM_LPCG85_DIRECT` @ `0x40CC6AA0` (CAN3 gate).
   - `IOMUXC_LPSR` pad defines for `GPIO_LPSR_00`/`GPIO_LPSR_01` (SW_MUX_CTL,
     SW_PAD_CTL) + the FLEXCAN3_RX select-input register — addresses/ALT from the cm7
     header + SDK pin_mux.
3. **No** FlexCAN base/register defs in core (the library owns them).

The MUX/DIV field encoding reuses the existing core clock-root pattern (e.g. the
LPI2C1 root37 setup) so the divisor is matched to the SDK, not guessed.

---

## 6. QEMU change — honest loopback (`hw/net/can/imxrt_flexcan.c`)

**Gate loopback self-delivery on `MCR.SRXDIS`.** In `flexcan_deliver()` (or the
`flexcan_transmit()` call site), when `s->mcr & FLEXCAN_MCR_SRXDIS` is set, do **not**
deliver the module's own transmitted frame into its Rx mailboxes. `FLEXCAN_MCR_SRXDIS`
(bit 17) is already defined in `imxrt_flexcan.h:56` and is part of `mcr` (already in
VMState) — no new state.

- **Effect:** the gate now *requires* the correct `begin → setBaudRate →
  enableLoopBack` sequence (which clears SRXDIS) to pass. This removes the circular
  pass.
- **No regression:** the existing 1062 raw-poke gate leaves SRXDIS=0 (self-reception
  enabled) → still delivers → still `FLEXCAN LB OK`.
- Update the model + header block comments to document the SRXDIS behaviour.
- **LOM** (listen-only) modeling is noted as a *future* fidelity item and is **out of
  scope** (over-modeling risks a false FAIL; the standard flow clears LOM via
  `setBaudRate(TX)` anyway).

---

## 7. Gates (TDD, in `newdigate/FlexCAN/tests/`)

Copy an existing gate dir (`evkb/spi_loopback_test` style; runners via `qrun`).

### 7.1 `flexcan_loopback_test` (polled) → greps `FLEXCAN_LB=PASS`
```
FlexCAN_T4<CAN3, RX_SIZE_256, TX_SIZE_16> can3;
can3.begin();
can3.setBaudRate(1000000);
can3.enableLoopBack(1);          // clears SRXDIS, sets LPB
can3.setMB(MB0, RX, STD);        // designate an Rx mailbox (begin() left MBs RX_INACTIVE)
can3.setMBFilter(ACCEPT_ALL);    // accept-all on that mailbox
CAN_message_t tx; tx.id=0x123; tx.len=8; tx.buf = {0x11..0x88};
can3.write(tx);                  // auto-picks a Tx mailbox
// poll can3.read(rx); assert rx.id==0x123 && rx.len==8 && bytes match
```
Must FAIL first (before the library/core/QEMU changes), then PASS. The exact
`setMB`/`setMBFilter`/`read` call sequence is confirmed during planning by reading the
FlexCAN_T4 `read()`/`setMB()`/`setMBFilter()` bodies (the library may auto-distribute
Rx mailboxes; the sketch must guarantee at least one `RX_EMPTY` accept-all buffer).

### 7.2 `flexcan_interrupt_test` (ISR) → greps `FLEXCAN_IRQ=PASS`
`onReceive(handler)` + `enableMBInterrupts()`; `write()`; the CAN3 ISR (via
`attachInterruptVector`) delivers the looped frame to the callback; assert byte-exact.
Exercises `IMASK`/`IFLAG` + the vectored interrupt path.

CMake per gate: `import_arduino_library(cores <evkb>/cores/imxrt1176)` +
`import_arduino_library(FlexCAN <this repo>)` + `FlexCAN` on the
`teensy_target_link_libraries` line; `COREPATH`/`EVKB_ROOT` walk fixed as for the moved
SPI/Wire gates.

---

## 8. Verification sequence

1. **QEMU build:** `cd ~/Development/qemu2/build && ninja qemu-system-arm`.
2. **Gate 1 (polled):** run red (pre-change) → green (post-change) via `qrun`.
3. **Gate 2 (interrupt):** green.
4. **Hardware (final arbiter):** `pkill -9 -f LinkServer redlinkserv`; `LinkServer run
   MIMXRT1176:MIMXRT1170-EVKB <elf>`; capture VCOM (`/dev/cu.usbmodem5DQ2DDHVWO5EI3`
   @115200) with **pyserial reader started first**. Confirm **`FLEXCAN_LB=PASS`** and
   **`FLEXCAN_IRQ=PASS`** on silicon.
5. **Saleae:** N/A for internal loopback (does not drive the pads) — reserved for the
   real-bus follow-on.

Independently `diff`/review each subagent's edits; re-run each gate yourself
(subagent-verification rule). Adding a new core clock/pin `.h` change → confirm a
from-scratch reconfigure of the gate dirs (CMake `file(GLOB)` trap).

---

## 9. Risks & mitigations

| Risk | Mitigation |
|---|---|
| LPSR pad mux/select-input values wrong | Take verbatim from SDK `interrupt_transfer/pin_mux.c` + cm7 header; **loopback gate does not depend on pins**, so a pin error cannot block v1 |
| CAN3 clock-root MUX/DIV field encoding | Match the existing core clock-root setter (LPI2C1 root37); OSC24M mux=1, ÷1 per SDK `app.h` |
| Circular QEMU pass | SRXDIS gating (§6) makes the gate require the real enable sequence; HW is the final arbiter |
| `FlexCAN_T4FD` pulled into 1176 build | Keep FD includes 1062-only |
| Non-MIT file embedded in FlexCAN_T4 | Per-file license audit before publishing the new repo |
| CMake `file(GLOB)` staleness after new core files | From-scratch `rm -rf build && cmake -B build …` of gate dirs |

---

## 10. Deferred (follow-ons)

- **CAN-FD** — `FlexCAN_T4FD.tpp`; QEMU model has no FD path.
- **Real two-node bus** — needs a 2nd EVKB or USB-CAN adapter (the MKR Zero has **no**
  CAN peripheral), remove jumpers **J102/J103**, CAN cable between the two **J47**s,
  and confirmation of the on-board CAN transceiver + any standby/EN GPIO from the EVKB
  schematic. QEMU model does not attach to the CAN bus subsystem (no multi-node).
- **CAN1/CAN2** pin + clock bring-up (bases added now; no consumer).
