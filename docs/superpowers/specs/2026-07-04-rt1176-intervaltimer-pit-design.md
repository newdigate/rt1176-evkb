# RT1176 `IntervalTimer` (PIT periodic timer) — Design

**Status:** approved (brainstorming) — ready for implementation plan
**Date:** 2026-07-04
**Target:** `cores/imxrt1176` Arduino core for the MIMXRT1176-EVKB **and** `qemu2` (`mimxrt1170-evk` machine). QEMU-gated (the PIT model implements countdown/IRQ logic) **and** hardware-verified (Saleae).

## Goal

Implement Teensyduino-compatible `IntervalTimer` — periodic interrupts at a user-specified microsecond interval, backed by the RT1176 **PIT1** (4 channels sharing one IRQ). API matches the repo's `cores/teensy4/IntervalTimer.h` so Teensy sketches compile unchanged.

Second, first-class deliverable: make QEMU **derive the PIT clock from `CCM_CLOCK_ROOT2` (the bus root)** instead of a fixed 24 MHz, and have the core read the effective bus frequency **at runtime** — so the model mirrors silicon and neither side silently breaks if the bus clock is later re-routed (e.g. to SYS_PLL3 / 240 MHz).

## Scope

**In:** `IntervalTimer` class with the full Teensy API (`begin` for `uint32_t` **and** `float` µs, `update`, `end`, `priority`, `operator IRQ_NUMBER_t`); PIT1's 4 channels as a static pool shared across all instances; shared-ISR dispatch by channel; runtime bus-clock derivation; QEMU CCM→PIT clock modelling.

**Out (YAGNI):** PIT2 (left free); PIT chain mode / 64-bit lifetime timer (LTMR64); moving GPT off its current fixed QEMU clock; a full CCM clock-tree model (only ROOT2 mux 0 is decoded now); handling a QEMU clock change *while a channel is already running* (re-read at arm time is sufficient); retrofitting runtime clock derivation onto SPI/Wire/PWM (those keep their hardcoded 24 MHz for now).

## Hardware facts (RT1176 PIT — triangulated: cm7 header · Zephyr · SDK · QEMU model)

- **Instance:** use **PIT1 @ `0x400D8000`** (PIT2 @ `0x40CB0000` left free). 4 channels each.
- **Shared IRQ:** `PIT1_IRQn = 155` (PIT2 = 156). **All 4 channels OR into the one line** — the ISR must poll each channel's `TFLG` to demux (cm7 `PIT_IRQS` macro; Zephyr DT `interrupts = <155 0>`; QEMU `pit_irq[]={155,156}`). No per-channel IRQs.
- **Registers** (`PIT_Type`): `MCR` @0x00 (`FRZ` bit0 = freeze-in-debug, `MDIS` bit1 = module disable), `LTMR64H/L` @0xE0/0xE4 (unused), then `CHANNEL[4]` @0x100 stride 0x10: `LDVAL` (+0x0), `CVAL` (+0x4, RO), `TCTRL` (+0x8: `TEN` bit0, `TIE` bit1, `CHN` bit2), `TFLG` (+0xC: `TIF` bit0, **write-1-to-clear**).
- **Counting:** channel counts **down** from `LDVAL` to 0, then auto-reloads and sets `TIF` — fires every `LDVAL + 1` input-clock cycles ⇒ **`LDVAL = count − 1`**.
- **Clock:** PIT1 is fed by the **bus root, `CCM_CLOCK_ROOT2` (`kCLOCK_Root_Bus`)**. Our core currently leaves ROOT2 on **mux 0 = `OscRC48MDiv2` = 24 MHz** (`cores/imxrt1176/startup.c:319`). ⇒ **24 cycles/µs today.**
- **Clock gate:** PIT1 has a CCM LPCG (exact `CCM_LPCGnn_DIRECT` number resolved in plan from the `PIT_CLOCKS` / CCM LPCG map). Enable once at first `begin`.

## Clock derivation (the crux — same tiny table on both sides)

`CCM_CLOCK_ROOT2_CONTROL` @ `0x40CC0100`: `MUX` = bits[10:8], `DIV` = bits[7:0]. Output = `source(MUX) / (DIV + 1)`.

Mux→Hz table (encode **mux 0 = 24 MHz** precisely now; unknown mux ⇒ default 24 MHz so nothing regresses; add the SYS_PLL3/240 MHz entry from the SDK when a phase actually routes BUS there):

| MUX | source | Hz |
|-----|--------|-----|
| 0 | OscRC48MDiv2 | 24 000 000 |
| other | (resolved when used) | 24 000 000 (fallback) |

- **Core** — `pit_clock_hz()` (file-local in `IntervalTimer.cpp`) reads ROOT2, applies the table, returns `src/(DIV+1)`. Replaces the hardcoded `24000000` **for the timer only**.
- **QEMU** — `imxrt_ccm` decodes ROOT2 writes with the **same table**, drives a `bus_root_clk` Clock output; the PIT model already does `clock_get_hz(s->clk)` at arm time (`qemu2/hw/timer/imxrt_pit.c:66`), so the period tracks it automatically.
- The two tables must stay in sync — they are the literal "model mirrors silicon" contract; note that in both files.

## Architecture & files

```
cores/imxrt1176/                  (core repo → github teensy-cores)
  IntervalTimer.h     — class: templated begin/update, end, priority, operator IRQ_NUMBER_t; 4-channel pool
  IntervalTimer.cpp   — beginCycles/end, shared pit_isr, channel pool, per-channel priority table, pit_clock_hz()
  imxrt1176.h (gen)   — PIT1 base 0x400D8000; PIT_Type MCR + CHANNEL[4]{LDVAL,CVAL,TCTRL,TFLG}; TCTRL/TFLG/MCR bits; PIT LPCG accessor
  core_pins.h         — add IRQ_PIT1=155, IRQ_PIT2=156, and IRQ_PIT=IRQ_PIT1 (Teensy compat) to IRQ_NUMBER_t
qemu2/                            (qemu repo → gitlab qemu-rt1170)
  include/hw/misc/imxrt_ccm.h     — add Clock *bus_root_clk to IMXRTCCMState
  hw/misc/imxrt_ccm.c             — on ROOT2 CONTROL write: decode MUX/DIV → clock_set_hz(bus_root_clk)+clock_propagate; init at reset; qdev_init_clock_out in init
  hw/arm/fsl-imxrt1170.c          — wire PIT1 "clk" ← ccm.bus_root_clk (leave GPT + PIT2 on fixed gpt_clk)
evkb/                             (evkb repo)
  interval_timer_test/            — QEMU gate: sketch + run_qemu_intervaltimer.sh + CMakeLists (from irq_attach_test)
```

- **`IntervalTimer.cpp`** owns the feature: a static `pit_channel_t *pool[4]` allocator, a `callback[4]` table, a `priority[4]` table, the shared `pit_isr` (installed once via the existing `attachInterruptVector` + `NVIC_ENABLE_IRQ`), and `pit_clock_hz()`. Reuses the runtime RAM-vector-table + NVIC machinery exactly as `interrupt_attach.c` does.

## API & register mapping

- **`bool begin(callback_t fn, period_t µs)`** (template; `uint32_t` and `float`): `count = µs × pit_clock_hz() / 1,000,000` (64-bit intermediate to avoid overflow); reject `fn==NULL`, `count < 18` (≡ Teensy's `cycles ≥ 17`) or `count > 2^32`; else `beginCycles`.
- **`beginCycles`**: allocate a free channel from `pool` — return **`false`** if all 4 busy. On first-ever alloc: enable PIT LPCG, clear `MCR.MDIS`, install `pit_isr` via `attachInterruptVector(IRQ_PIT1, pit_isr)`, `NVIC_ENABLE_IRQ(IRQ_PIT1)`. Per channel: `callback[ch]=fn`; `LDVAL = count−1`; W1C `TFLG`; `TCTRL = TEN | TIE`. Set `NVIC_SET_PRIORITY(IRQ_PIT1, min priority across active channels)`.
- **`void update(period_t µs)`**: recompute `count`; if valid and allocated, write `channel->LDVAL = count−1` (takes effect at next reload).
- **`void end()`**: `TCTRL[ch]=0` (clear TEN/TIE), `callback[ch]=NULL`, free `pool[ch]`; recompute NVIC priority across remaining channels. Idempotent. (LPCG left enabled — Teensy convention.)
- **`void priority(uint8_t n)`**: store `priority[ch]=n`; `NVIC_SET_PRIORITY(IRQ_PIT1, min across active)`. Default 128 (matches core convention: below serial 64 / I2C 16).
- **`operator IRQ_NUMBER_t()`**: `IRQ_PIT1` if allocated, else `NVIC_NUM_INTERRUPTS`.

## ISR dispatch

Single shared `pit_isr` (installed on first `begin`). For `ch` in 0..3: if `PIT1->CHANNEL[ch].TFLG & TIF`, **W1C the flag**, then call `callback[ch]` if non-NULL. W1C-before-callback so a reload during the callback isn't lost. Callbacks run in ISR context; a slow callback delays the other channels on the shared line.

## Verification

- **QEMU gate (`interval_timer_test`)** — primary, self-contained, `run_qemu_intervaltimer.sh` greps `IT=PASS`:
  1. **fires** — `begin(cb, 1000)`; after `delay(100)`, callback count ≈ 100 (tolerance band).
  2. **channel exhaustion** — 4 `begin`s succeed, the **5th returns `false`**.
  3. **`end()` stops** — count frozen after `end()`.
  4. **frequency ratio** — 500 µs yields ≈ 2× the 1000 µs count.
  5. **clock faithfulness** — reprogram `ROOT2` DIV (e.g. ÷2), confirm the PIT rate scales proportionally — proves the CCM-derivation on both core and model.
- **Saleae / HW:** toggle a header pin in the callback; measure frequency at 24 MHz (1000 µs ⇒ 500 Hz square wave; also check a 50 µs fast rate). Reference `evkb/pwm_test/measure_pwm.py`.
- **HW caveat:** a *live* bus-clock reprogram on silicon disturbs every ROOT2 peripheral, so on HW the primary proof is frequency **accuracy at the default clock**; the DIV-scaling check (5) stays a QEMU-only test.

## Error handling

- `begin` returns `false` on: null callback, no free channel, or period out of range (`< 18` or `> 2^32` counts). `update` before `begin` is a no-op. `end` is idempotent.
- Level of the shared IRQ: all four channels share priority (the min set across active timers) — documented.
- Callbacks are ISR-context (keep short); no re-entrancy guarantees beyond that.

## Risks / open items (resolved in plan)

- **PIT LPCG number** — exact `CCM_LPCGnn_DIRECT` for PIT1 from `fsl_clock.h` `PIT_CLOCKS` / CCM LPCG map; also confirm whether the boot ROM leaves it gated on (gate may be a no-op).
- **ROOT2 mux table beyond mux 0** — exact source Hz for other muxes (esp. the SYS_PLL3 240 MHz path) from SDK `clock_config.c` / RM; only needed once a non-mux-0 source is used. Mux 0 = 24 MHz is solid.
- **QEMU Clock-output plumbing** — exact `qdev_init_clock_out` / `clock_propagate` pattern and where to hook ROOT2 decode in `imxrt_ccm.c`; crib from an existing QEMU CCM/clock model.
- **Teensy parity constants** — min-count (17) and the float rounding (`−0.5f`) matched against `teensy4/IntervalTimer.h` in the plan.
- **`pit_clock_hz()` placement** — file-local in `IntervalTimer.cpp` now; promote to a shared clock unit only if a second peripheral needs it.
