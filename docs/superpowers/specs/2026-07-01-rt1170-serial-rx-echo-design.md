# RT1176 Arduino Core — Phase 2: `Serial1` RX echo

**Date:** 2026-07-01
**Status:** Approved (design), pending implementation plan
**Depends on:** Phase 1 (2026-07-01-rt1170-serial-lpuart1-design.md) — interrupt-driven `Serial1` on LPUART1.

## Goal

Validate the LPUART1 **receive** path end-to-end on the EVKB VCOM: bytes sent to
`Serial1` arrive via the RX interrupt, are buffered, dispatched through
`yield()` → `serialEvent1()`, and echoed back — with hard evidence that the RX
**interrupt** (not merely an incidental FIFO peek) did the work. Verified in QEMU
via an injected input stream, then on hardware.

## Background / current state

Phase 1 delivered interrupt-driven `Serial1`. The RX half is already fully built
and needs no logic changes:
- `begin()` sets `CTRL = TE | RE | RIE` — receiver + RX interrupt enabled.
- `HardwareSerialIMXRT::IRQHandler()` RX branch: on `STAT.RDRF`/`IDLE`, reads
  `DATA` into the RX ring buffer.
- `available()`/`read()`/`peek()` operate on the ring buffer AND peek the live RX
  FIFO count (`(WATER>>24)&0x7`), so bytes drain even if the interrupt never fires.
- `yield()` calls `serialEvent1()` when `Serial1.available() > 0`.

QEMU models RX (`hw/char/imxrt_lpuart.c`): `imxrt_lpuart_receive()` pushes to the
RX FIFO, `update_rdrf` sets `RDRF` when the FIFO exceeds the RX watermark, and
`update_irq` raises the LPUART1 IRQ when `RIE && RDRF`.

Because `available()`/`read()` also peek the FIFO, a pure polling echo could pass
without the RX interrupt ever firing. To make the test *prove* the interrupt path,
this phase adds one small diagnostic counter in the ISR.

## Architecture

Three pieces; no change to RX buffering logic.

1. **Driver diagnostic** — a `volatile uint32_t serial1_rx_isr_count` incremented
   in the RX branch of `HardwareSerialIMXRT::IRQHandler()`, exposed via `extern`.
   This is the only way to distinguish "RX interrupt fired" from "bytes drained by
   an `available()` FIFO peek". ~2 lines.
2. **Echo sketch** (`serial_test_rx/`).
3. **QEMU RX-injection harness** (socket chardev + Python driver) and a **hardware
   pyserial send/echo test**.

### Driver diagnostic (the "suspenders")

In `HardwareSerial.cpp`, add at file scope:
```c
volatile uint32_t serial1_rx_isr_count = 0;
```
and increment it once per RX-servicing pass, inside the `if (port->STAT & (RDRF|IDLE))`
block of `IRQHandler()` (guarded so only LPUART1's instance counts — since only
`Serial1` exists, an unconditional increment in the shared `IRQHandler()` is
acceptable and simplest; note this in a comment). Declare `extern volatile uint32_t
serial1_rx_isr_count;` in `HardwareSerial.h` so the sketch can read it.

Rationale for keeping it in the driver rather than the sketch: the count must come
from inside the ISR; a sketch cannot observe the ISR otherwise. This is a
diagnostic hook, not a behavior change.

### Echo sketch (`serial_test_rx/serial_test_rx.cpp`)

- `serialEvent1()` — the ONLY echo path:
  ```cpp
  void serialEvent1() {
      while (Serial1.available()) { Serial1.write((uint8_t)Serial1.read()); echoed++; }
  }
  ```
  Exercises RX ISR → ring buffer → `yield()` → `serialEvent1()` → echo.
- `setup()` — `Serial1.begin(115200); Serial1.println("RT1176 RX echo ready");`
- `loop()` — does NOT read/echo (avoids a read race with `serialEvent1`). Once
  per ~1 s prints one status line: `[status] rx_isr=<serial1_rx_isr_count> echoed=<echoed>`.
  `loop()` returns to the core `main()` which calls `yield()` each iteration,
  driving `serialEvent1()`.
- `echoed` is a `volatile uint32_t` file-scope counter; `serial1_rx_isr_count` is
  read from the driver `extern`.

The three signals: echoed bytes (functional RX proof), `rx_isr > 0`
(interrupt-fired proof), `echoed == bytes_sent` (serialEvent dispatch proof).

### QEMU RX-injection harness

`-serial file:` cannot inject input, so bind LPUART1 (the first `-serial`) to a
**socket chardev**:
```
-chardev socket,id=u1,host=127.0.0.1,port=<PORT>,server=on,wait=off -serial chardev:u1
```
`run_qemu_rx.sh` launches QEMU with this chardev and runs a Python driver
(`qemu_rx_driver.py`) that:
1. connects to the socket,
2. reads until it sees the `RT1176 RX echo ready` banner (timeout-bounded),
3. sends a known payload, e.g. `b"hello\n"`,
4. reads the echoed bytes back and asserts they equal the payload,
5. reads a subsequent `[status] rx_isr=N echoed=M` line and asserts `N > 0` and
   `M >= 6` (the 6 payload bytes),
6. exits 0 on success (prints `PASS`), non-zero with a diagnostic on failure.

`sleep`/timeout bounded so it cannot hang. If QEMU is stale:
`ninja -C ~/Development/qemu2/build qemu-system-arm`.

### Hardware test (`capture_rx_hw.py`)

`pyserial` opens the VCOM at 115200, reads the banner, writes `b"hello\n"`, reads
the echo, asserts the echo matches, then reads a `[status]` line and asserts
`rx_isr > 0`. Automatable in one session (send + read) — no button press needed
while the board is running. (If the board is halted post-flash, power-cycle per the
Phase-1 note.)

## Data flow

Host sends byte → QEMU/silicon LPUART1 RX FIFO → `RDRF` → LPUART1 IRQ →
`IRQHandler()` RX branch (`serial1_rx_isr_count++`, byte → RX ring) → `yield()`
sees `available()>0` → `serialEvent1()` drains ring, `Serial1.write()` echoes → TX
path returns the byte to the host.

## Error handling

- **RX overrun** (`STAT.OR`): already handled by the Phase-1 ISR (flag cleared,
  byte dropped); not exercised by this small test but not regressed.
- **QEMU socket not ready:** the Python driver retries the connect with a bounded
  timeout and fails with a clear message rather than hanging.
- **No echo / rx_isr==0:** the driver prints which assertion failed (echo mismatch
  vs. interrupt-never-fired vs. banner-not-seen) to localize the fault.

## Testing / verification

1. **QEMU (primary gate):** `run_qemu_rx.sh` → socket driver sends `hello\n`,
   asserts echo + `rx_isr>0` + `echoed>=6` → `PASS`.
2. **Hardware:** `capture_rx_hw.py` over the MCU-Link VCOM → same assertions on
   silicon.
3. **Regression:** the Phase-1 `serial_test` TX banner/counter QEMU test still
   passes (the RX-ISR counter addition must not perturb TX).

## Out of scope (YAGNI)

- Line editing, command parsing, or any echo transformation (pure byte echo only).
- Flow control, 9-bit, RX DMA.
- Per-path tagging with both `serialEvent1()` and `loop()` echoing (rejected during
  design to avoid a read race; `serialEvent1()` owns echo, `loop()` only reports).
- RX for LPUART instances other than `Serial1`.
