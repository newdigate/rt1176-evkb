# RT1176 SPI full-duplex DMA transfer (blocking + async) — Design

**Status:** approved (design), ready for implementation plan
**Date:** 2026-07-06
**Sub-project:** B of the SPI-DMA-TX effort. Consumes the restored [[rt1176-eventresponder]] (sub-project A, done + HW-verified + pushed).

## Goal

Add DMA-backed full-duplex `transfer` to the RT1176 SPI (LPSPI) driver — a **blocking** `transfer(txbuf, rxbuf, count)` and an **async** `transfer(txbuf, rxbuf, count, EventResponder&)` — so bulk SPI I/O runs off the eDMA instead of the per-byte polled loop, with async completion delivered through EventResponder. Direct port of the Teensy 4 IMXRT LPSPI DMA transfer (same silicon IP).

## Background / current state

- `cores/imxrt1176/SPI.{h,cpp}`: polled master-only. `hardware_t` holds `cr/cfgr1/ccr/tcr/tdr/rsr/rdr/lpcg/clock_root` + pin refs. `transfer(byte)`/`transfer16`/`transfer(buf,count)` (byte-by-byte, in-place full-duplex). `beginTransaction` sets `tcr_base` (CPOL/CPHA/LSBF/prescale). **Manual CS** (`endTransaction` is a no-op). The core header has `LPSPI1_SR/TCR/TDR` but **not** `DER`/`FCR` or the DMA bits — added generator-first.
- eDMA + Teensy `DMAChannel` port is DONE ([[rt1176-edma-dmachannel]]); `DMAMUX_SOURCE_LPSPI1_TX=37` / `LPSPI1_RX=36` already defined. I2S `beginDMA`/`beginReceiveDMA` are the working precedent.
- EventResponder restored (yield-deferred + immediate); `event.triggerEvent()` from the completion ISR runs the user callback at `yield()` (safe) or immediately.
- Reference: Teensy 4 `libraries/SPI/SPI.cpp` IMXRT section (`#elif … __IMXRT1062__`) — identical LPSPI IP. Its async transfer: RX DMAChannel `source(RDR)`/`destinationBuffer(rxbuf)`, TX `destination(TDR)`/`sourceBuffer(txbuf)`, both `disableOnCompletion`; `TCR` FRAMESZ(7), `FCR=0`, `DER=TDDE|RDDE`; RX-completion ISR fires the EventResponder.
- QEMU `hw/ssi/imxrt_lpspi.c` already models `dma_tx`/`dma_rx` GPIOs, `DER` (`TDDE`/`RDDE`), `TCR_RXMSK`, `FCR`, and the machine wires an SSI loopback.

## Scope

**In scope:** blocking + async full-duplex DMA `transfer`; 8-bit frames (byte buffers); manual CS (unchanged); RX-completion drives done/event; the async overload returns `false` if a transfer is already in progress (not re-entrant).

**Explicitly deferred (YAGNI):** 16-bit/word DMA transfers; TX-only (RXMSK) DMA; automatic CS via `TCR` PCS; DMA for `SPI1`/`SPI2` instances (this core exposes one SPI = LPSPI1); chained/queued transfers.

## Architecture — files & changes

| File | Change |
|---|---|
| `cores/imxrt1176/tools/gen_imxrt1176_h.py` + generated `imxrt1176.h` | Add `LPSPI1_DER`, `LPSPI1_FCR` regs + bits `LPSPI_DER_TDDE`(1<<0), `LPSPI_DER_RDDE`(1<<1), `LPSPI_TCR_FRAMESZ(n)`, `LPSPI_FCR_TXWATER(n)`. Generator-first (reconcile to empty diff). |
| `cores/imxrt1176/SPI.h` | Add `volatile uint32_t &der, &fcr;` to `hardware_t`; declare the two `transfer` overloads; private `DMAChannel *_dmaTX=nullptr, *_dmaRX=nullptr; EventResponder *_dma_event_responder=nullptr; volatile bool _transfer_done=true;` + `static void rxisr();` + `void startDMA(const void*, void*, size_t);`. |
| `cores/imxrt1176/SPI_instances.cpp` | Add `der`/`fcr` refs to the `SPI` hardware literal (positional). |
| `cores/imxrt1176/SPI.cpp` | Implement the two transfers, `startDMA`, `rxisr`. |
| `evkb/spi_dma_test/` | New gate. |
| `qemu2/hw/ssi/imxrt_lpspi.c` | **Only if** §5 verification shows the finite transfer mis-drives. |

## API

- `void transfer(const void *txbuf, void *rxbuf, size_t count)` — blocking DMA full-duplex; returns with `rxbuf` filled.
- `bool transfer(const void *txbuf, void *rxbuf, size_t count, EventResponder &event)` — async; returns `true` when started, **`false` if a transfer is already in progress**; fires `event` from the RX-completion ISR.
- Existing polled `transfer(byte)`/`transfer16`/`transfer(buf,count)` unchanged. `_dmaTX`/`_dmaRX` allocated lazily on first DMA use.

## Data flow

**`startDMA(tx, rx, count)`** (shared): RX channel — `source(*(volatile uint8_t*)&LPSPI1_RDR)`, `destinationBuffer((uint8_t*)rxbuf, count)`, `disableOnCompletion`, `triggerAtHardwareEvent(DMAMUX_SOURCE_LPSPI1_RX)`, `interruptAtCompletion`, `attachInterrupt(rxisr)`. TX channel — `destination(*(volatile uint8_t*)&LPSPI1_TDR)`, `sourceBuffer((uint8_t*)tx, count)`, `disableOnCompletion`, `triggerAtHardwareEvent(DMAMUX_SOURCE_LPSPI1_TX)`. Then `hw->tcr = (hw->tcr & ~LPSPI_TCR_FRAMESZ(31)) | LPSPI_TCR_FRAMESZ(7)`; `hw->fcr = 0`; `hw->der = LPSPI_DER_TDDE | LPSPI_DER_RDDE`; `_transfer_done = false`; enable RX then TX.

**`rxisr()`** — RX complete means the transfer is complete (full-duplex: the Nth byte received ⇒ the Nth byte was sent): `_dmaRX->clearInterrupt(); hw->der = 0; _transfer_done = true; if (_dma_event_responder) _dma_event_responder->triggerEvent();`. This core exposes a single SPI instance, so the static ISR reaches its DMA state directly (via the `SPI` global / a static self-pointer set in `startDMA`).

**Blocking** `transfer(tx,rx,count)`: `_dma_event_responder = nullptr; startDMA(...); while (!_transfer_done) yield();`. The `yield()`-spin is cooperative — `serialEvent1` and other deferred EventResponders run while the DMA moves the data; the RX ISR (interrupt-driven, independent of `yield`) sets `_transfer_done`, so the spin always terminates.

**Async** `transfer(tx,rx,count,event)`: `if (!_transfer_done) return false;` (a transfer is in flight) `; _dma_event_responder = &event; startDMA(...); return true;`.

**Not re-entrant.** `_dmaTX`/`_dmaRX`/`_transfer_done` are shared per-instance, so only one transfer runs at a time. The async `bool` return signals "busy"; a callback dispatched during a blocking transfer's `yield()`-spin must not start another SPI DMA transfer (documented). `_transfer_done` is the in-progress flag (idle = `true`).

## LPSPI register config

From the reference: `TCR` FRAMESZ(7) (8-bit); `FCR = 0` (watermark 0 — request on any FIFO room/data); `DER = TDDE | RDDE`; on completion `DER = 0`. **No `RXMSK`** — full-duplex captures RX.

## QEMU

The LPSPI model already has `dma_tx`/`dma_rx` + `DER` + the SSI loopback, so likely **no QEMU change**. The one risk to verify during implementation: the *level* `dma_tx`/`dma_rx` assertions (`dma_tx` while `MEN && TDDE`; `dma_rx` while `RDDE && rx_fifo` non-empty) driving exactly `count` bytes per channel through the eDMA hardware-request path for a **finite** full-duplex transfer. If it moves `count` bytes correctly and the RX-completion IRQ fires (as I2S `beginDMA` did), zero QEMU work. If it over/under-drives, a small pacing fix in the same family as the SAI `tx_tick` (level-vs-edge) / eDMA `single_minor` precedents — modeled and re-verified against the RT1062 suite if touched.

## Testing — QEMU gate (`evkb/spi_dma_test/`, SSI loopback)

Mirrors `spi_loopback_test` scaffolding (CMake + run script + toolchain). Stages on VCOM (`Serial1`):
- `STAGE_BLOCKING`: fill `txbuf[N]` with a known pattern; `SPI.transfer(txbuf, rxbuf, N)`; assert `rxbuf == txbuf` (loopback echo). `SPI_DMA_BLOCKING=PASS`.
- `STAGE_ASYNC`: an `EventResponder` with a callback that sets a flag (`attach`, deferred); `bool ok = SPI.transfer(txbuf, rxbuf2, N, event)` returns `true` immediately; `yield()` until the flag; assert `rxbuf2 == txbuf` **and** the callback fired. `SPI_DMA_ASYNC=PASS`.
- Final `SPI_DMA_ALL=PASS`. Run script greps the markers + a settle window sized to avoid the cold-start race (`sleep 5`, per the EventResponder gate lesson).

## HW verification

Flash; external **SDO→SDI jumper** (as `spi_loopback_test` was HW-verified). Both stages PASS on silicon (`rxbuf == txbuf` + the async callback fired) proves blocking and async full-duplex DMA on real hardware. Flash + VCOM scriptable (LinkServer + debug VCOM `/dev/cu.usbmodem5DQ2DDHVWO5EI3` @115200).

## References

- Teensy 4 SPI DMA reference: `~/.platformio/packages/framework-arduinoteensy/libraries/SPI/SPI.cpp` (IMXRT section).
- [[rt1176-lpspi-spi]] (the polled SPI this extends), [[rt1176-edma-dmachannel]] (DMAChannel + DMAMUX), [[rt1176-eventresponder]] (async completion), [[rt1176-i2s-sai]] (the `beginDMA` precedent).
