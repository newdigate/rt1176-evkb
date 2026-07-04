# RT1176 `attachInterrupt` (GPIO pin interrupts) — Design

**Status:** approved (brainstorming) — ready for implementation plan
**Date:** 2026-07-04
**Target:** `cores/imxrt1176` Arduino core for the MIMXRT1176-EVKB. QEMU-gated (the GPIO model implements interrupt logic) **and** hardware-verified.

## Goal

Implement Arduino `attachInterrupt(pin, fn, mode)` / `detachInterrupt(pin)` for GPIO pin interrupts on the Arduino header, riding on the digital pin table. Supports all five modes: `RISING`, `FALLING`, `CHANGE`, `HIGH`, `LOW`.

## Scope

**In:** `attachInterrupt`/`detachInterrupt` (both already declared in `core_pins.h`), all 22 header pins, all five trigger modes, shared-ISR dispatch by pin.

**Out (YAGNI):** interrupts on non-header pads; software debounce; priority configuration API (fixed default priority); nested/re-entrant callback guarantees beyond "keep ISRs short."

## Hardware facts (RT1176 GPIO interrupts)

- **Combined IRQs** (from `MIMXRT1176_cm7.h`): `GPIO7_8_9_10_11_IRQn = 99` covers GPIO7–11; `GPIO12_Combined_0_15_IRQn = 61`, `GPIO12_Combined_16_31_IRQn = 62` cover GPIO12 (both shared with GPIO6, which we don't use). The header pins live on GPIO8 (D6), GPIO9 (most pins), GPIO11 (D0/D1/D2), GPIO12 (D14/D15) — so **all header pins except D14/D15 → IRQ 99; D14/D15 → IRQ 61/62.**
- The fast GPIO (7–11) **do** support interrupts, so the existing `digital_pin_to_info` (which uses GPIO8/9/11/12 via ALT 0xA) works directly — no GPIO-instance switching.
- **GPIO interrupt registers** (offsets from each GPIO base; bases already in `imxrt1176.h`): `ICR1 +0x0C` (pins 0–15, 2 bits/pin), `ICR2 +0x10` (pins 16–31), `IMR +0x14` (mask/enable), `ISR +0x18` (status, W1C), `EDGE_SEL +0x1C` (any-edge override). ICR field: `00`=low-level, `01`=high-level, `10`=rising, `11`=falling.
- Mode constants come from `core_pins.h` (`RISING`/`FALLING`/`CHANGE`/`HIGH`/`LOW`); the plan maps each to the ICR field / `EDGE_SEL` bit by reading their values.

## Architecture & files

```
cores/imxrt1176/
  interrupt_attach.c  — attachInterrupt/detachInterrupt, callback[] table, two shared GPIO ISRs
  core_pins.h         — add IRQ_GPIO7_8_9_10_11=99, IRQ_GPIO12_0_15=61, IRQ_GPIO12_16_31=62 to IRQ_NUMBER_t
  imxrt1176.h (gen)   — GPIO ICR1/ICR2/IMR/ISR/EDGE_SEL accessors per header GPIO port (GPIO8/9/11/12), OR computed from GPIO base in interrupt_attach.c
qemu2/hw/arm/mimxrt1170-evk.c — wire one GPIO9 header pin's output → another GPIO9 pin's input, so the gate self-stimulates an interrupt
```

- **`interrupt_attach.c`** owns the feature: a `callback[CORE_NUM_DIGITAL]` function-pointer table, `attachInterrupt`/`detachInterrupt`, and the two shared ISRs installed via the existing `attachInterruptVector` + `NVIC_ENABLE_IRQ`. Reuses `digital_pin_to_info[pin]` for GPIO base/bit.
- **No new pin work** — rides on the completed pin table.

## API & register mapping

- **`attachInterrupt(uint8_t pin, void(*fn)(void), int mode)`**: bounds-check `pin < CORE_NUM_DIGITAL`; look up `p = digital_pin_to_info[pin]`; skip if unmapped (`p->gpio==0`) or `fn==NULL`. Store `callback[pin]=fn`. Configure trigger on `p->gpio`:
  - `CHANGE` → set `EDGE_SEL` bit `p->bit`.
  - else clear `EDGE_SEL` bit and write the 2-bit ICR field for `p->bit` (`ICR1` if bit<16 else `ICR2`, shift `2*(bit%16)`): `LOW`=00, `HIGH`=01, `RISING`=10, `FALLING`=11.
  - W1C `ISR` bit `p->bit` (clear stale), set `IMR` bit `p->bit`.
  - Install the shared ISR for the pin's combined IRQ (99, or 61+62 for GPIO12) via `attachInterruptVector`; `NVIC_SET_PRIORITY` (default) + `NVIC_ENABLE_IRQ`.
  - Does **not** set `pinMode` (Arduino convention — sketch sets `INPUT`/`INPUT_PULLUP`).
- **`detachInterrupt(uint8_t pin)`**: clear `IMR` bit and `EDGE_SEL` bit on `p->gpio`, set `callback[pin]=NULL`.

## ISR dispatch

Two shared handlers, installed on demand:
- **`gpio_isr_7_11`** (IRQ 99): for each of GPIO8/9/11, `pending = ISR & IMR`; W1C `pending`; for each set bit `b`, scan the pin table for the pin whose `gpio==this port` and `bit==b`, and call its `callback` (if non-NULL).
- **`gpio_isr_12`** (IRQ 61 and 62): same for GPIO12.

Pin lookup is a linear scan of the 22-entry table — negligible. W1C happens before the callback so a re-trigger during the callback isn't lost.

## Verification

- **QEMU gate (`irq_attach_test`)** — primary, self-contained:
  - Machine change: in `mimxrt1170-evk.c`, `qdev_connect_gpio_out` from GPIO9 bit for **D13** (output) to `qdev_get_gpio_in` GPIO9 bit for **D9** (input). (Both header pins on GPIO9; the plan resolves the SoC `gpio[]` index for base `0x40C64000`.)
  - Sketch: `pinMode(13,OUTPUT)`, `pinMode(9,INPUT)`, `attachInterrupt(9, counter, RISING)`; toggle pin 13 N times → assert `count==N`; re-attach `CHANGE` → `2N`; `detachInterrupt(9)` then toggle → count unchanged. Print `IRQ=PASS`/`FAIL` + counts over Serial1. Runner asserts `IRQ=PASS`.
- **Hardware:** one jumper from an output pin to an interrupt-input pin; same sketch; VCOM shows the counts. (A physical button on an `INPUT_PULLUP` pin is an optional manual check.)
- No PWM-style analog measurement needed — this is digital and QEMU models it fully.

## Error handling
- `attachInterrupt`/`detachInterrupt` bounds-check `pin` and ignore unmapped pins (`gpio==0`).
- `HIGH`/`LOW` are level-triggered: the ISR re-fires while the level holds (documented storm caveat); the callback should clear the source or the sketch should use `detachInterrupt`. Edge modes are the verified path.
- Callbacks run in ISR context (keep short); shared-IRQ dispatch means a slow callback delays other pins on the same port.

## Risks / open items (resolved in plan)
- Exact numeric values of the `RISING`/`FALLING`/`CHANGE`/`HIGH`/`LOW` mode macros — read from `core_pins.h` in plan Task 1.
- SoC `gpio[]` array index for GPIO9 (base `0x40C64000`) for the machine wiring — from `fsl-imxrt1170.c` `gpio_base[]`.
