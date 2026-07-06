# RT1176 EventResponder (minimal restore) — Design

**Status:** approved (design), ready for implementation plan
**Date:** 2026-07-06
**Sub-project:** A of 2. This is the async-completion primitive; sub-project **B** (SPI full-duplex DMA async transfer) is its driving consumer and gets its own spec → plan → implement cycle after this ships.

## Goal

Restore the RT1176 core's `EventResponder` — the Teensy async-event primitive that lets time-sensitive code (an ISR, a DMA completion) hand a callback to be run a short time later in a safe context, or immediately if the caller insists. Scope is **minimal**: the yield-deferred and immediate paths only — exactly what an async peripheral (SPI DMA first) needs to signal completion — leaving the PendSV software-interrupt path and `MillisTimer` for a later phase.

## Background / current state

- `cores/imxrt1176/EventResponder.h` is the **full verbatim Teensy 4 header**, deliberately kept as a placeholder. `EventResponder.cpp` was **gutted to an empty translation unit in "Phase-0"** with a note: the real implementation "depends on interrupt priority plumbing, SysTick/PIT hooks and the `attachInterruptVector` machinery … not yet ported … restore this `.cpp` verbatim from the Teensy 4 core once its dependencies exist."
- Since Phase-0 the core has built much of that: a **RAM vector table** (`_VectorsRam` in `startup.c`, used by `attachInterrupt` and `DMAChannel`) and a working **`yield()` hook** (`yield.cpp`, called from the `main.cpp` loop and `delay()`), currently dispatching only `serialEvent1`.
- **Caveat that shapes scope:** `delay.c` documents that `millis()` was deliberately moved to the free-running **DWT cycle counter** because "the SysTick tick ISR does not reliably advance a millisecond counter" on RT1176. `MillisTimer` depends on a reliable 1 ms `systick_isr`, so it is fragile on this silicon and is **out of scope** here.

The upshot: the yield-deferred path needs only the already-working `yield()` hook; no new SysTick or PendSV machinery. That is the minimal restore.

## Scope

**In scope (minimal restore):**
- `attach(fn)` — deferred callback, runs from `yield()` (safe context: Arduino libs, `Serial`, `String`).
- `attachImmediate(fn)` — callback runs synchronously inside `triggerEvent()` (fast; caller's context, including an ISR).
- `attachInterrupt(fn)` — **falls back to `attachImmediate`** (no PendSV software-IRQ on this core; the header's own comment sanctions "boards not supporting software triggered interrupts … implement this as attachImmediate", preserving the "prompt response" contract).
- `triggerEvent(status, data)`, `clearEvent()`, `detach()`, `getStatus()`, `getData()`, `setContext()/getContext()`, `operator bool`.
- `runFromYield()` wired into `yield()`.

**Explicitly deferred (YAGNI):**
- `MillisTimer` and `systick_isr_with_timer_events` — need a reliable 1 ms tick this silicon's SysTick can't give; revisit if/when a PIT-backed millisecond tick lands.
- The **PendSV** software-interrupt path (`attachInterrupt` as a real low-priority IRQ, `runFromInterrupt`, `pendablesrvreq_isr`, `firstInterrupt/lastInterrupt`).
- `waitForEvent()` / `attachThread()` as distinct behaviors (no scheduler/RTOS; `attachThread` already aliases `attach`).

## Architecture — files & changes

| File | Change |
|---|---|
| `cores/imxrt1176/EventResponder.h` | Adapt the kept verbatim header to the minimal surface (below). |
| `cores/imxrt1176/EventResponder.cpp` | Implement (replace the empty Phase-0 stub) the yield-list statics + methods. |
| `cores/imxrt1176/yield.cpp` | Add `EventResponder::runFromYield();` to `yield()`. |
| `evkb/eventresponder_test/` | New QEMU gate — pure firmware unit test (no QEMU device-model work). |

**Header adaptation (`EventResponder.h`):**
- Keep `EventResponder` with `attach` / `attachImmediate` / `triggerEvent` (verbatim, incl. the immediate-vs-not branch) / `clearEvent` / `runFromYield` (verbatim inline) / `getStatus` / `getData` / `setContext` / `getContext` / `operator bool` / the private `disableInterrupts`/`enableInterrupts` helpers.
- Change `attachInterrupt()` body to `attachImmediate(function)` — drop its `SCB_SHPR3`, `_VectorsRam[15] = systick_isr_with_timer_events`, and the `extern … systick_isr_with_timer_events` declaration.
- Drop the `yield_active_check_flags |= YIELD_CHECK_EVENT_RESPONDER;` line from `attach()` (that optimization symbol doesn't exist here; `yield()` will simply always call `runFromYield()`, which early-returns when the list is empty).
- Remove `MillisTimer`, `runFromInterrupt`, and the `firstInterrupt`/`lastInterrupt` statics. Keep `attachThread` (trivial inline alias of `attach`).

**Implementation (`EventResponder.cpp`):**
- Define statics: `firstYield`, `lastYield`, `runningFromYield`.
- `triggerEventNotImmediate()` — the `EventTypeYield` branch only: enqueue `this` on `firstYield/lastYield`, set `_triggered`; interrupt-guarded. (Drop the `EventTypeInterrupt`/PendSV branch.)
- `clearEvent()` and `detachNoInterrupts()` — yield-list unlink only.

**Wiring (`yield.cpp`):** inside the existing re-entrancy-guarded `yield()`, after the `serialEvent1` dispatch, call `EventResponder::runFromYield();`.

## Data flow

- **Deferred (`attach`):** `triggerEvent(status, data)` — typically from a peripheral/DMA ISR — records status/data, enqueues on the yield list, sets `_triggered`. Later the `main.cpp` loop / `delay()` calls `yield()` → `runFromYield()` dequeues one event, clears `_triggered`, calls `fn(*this)`. Guards: skip when called from interrupt context (`ipsr != 0`), and re-entrancy via `runningFromYield` (so a callback that itself calls `yield()` doesn't recurse). One event is dispatched per `yield()` call; multiple pending events drain across successive calls.
- **Immediate (`attachImmediate` / `attachInterrupt`):** `triggerEvent()` calls `fn(*this)` synchronously in the caller's context.
- **Dedup:** re-triggering an already-queued event before it dispatches overwrites its status/data and still results in a single call (Teensy semantics via `_triggered`).

## Testing — QEMU gate (`evkb/eventresponder_test/`)

Pure firmware, runs on the existing `mimxrt1170-evk` QEMU machine; **no new QEMU device model**. Stages, each printing `PASS`/`FAIL` on VCOM (`Serial1`):
- `STAGE_IMMEDIATE`: `attachImmediate` + `triggerEvent` → flag set synchronously, before any `yield()`.
- `STAGE_YIELD`: `attach` + `triggerEvent` → flag **not** yet set; call `yield()` → flag now set.
- `STAGE_CLEAR`: `attach` + `triggerEvent` + `clearEvent` + `yield()` → callback did **not** run.
- `STAGE_STATUS`: `triggerEvent(42, &x)` → callback observes `getStatus()==42` and `getData()==&x`.
- Final line `EVENTRESPONDER_ALL=PASS`. The run script greps the stage markers, mirroring the existing gates (`spi_loopback_test`, `sai_rx_test`).

## HW verification

Flash the gate; confirm via VCOM that all four stages PASS on silicon — i.e. the deferred callback really fires from the real main-loop `yield()`. EventResponder has no peripheral dependency, so this is a smoke test that the dispatch + linked-list logic run on hardware; the substantive HW proof arrives when SPI DMA (sub-project B) drives it.

## References

- Teensy 4 reference (verbatim source of truth): `~/.platformio/packages/framework-arduinoteensy/cores/teensy4/EventResponder.{h,cpp}`.
- Core hooks: `cores/imxrt1176/{yield.cpp, main.cpp, delay.c, startup.c}`.
- Consumer (sub-project B): SPI full-duplex DMA async `transfer(txbuf, rxbuf, count, EventResponder&)` — see [[rt1176-lpspi-spi]], [[rt1176-edma-dmachannel]].
