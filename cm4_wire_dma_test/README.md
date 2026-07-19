# cm4_wire_dma_test — CM4 interrupt-driven DMA via eDMA_LPSR (★★HW-VERIFIED)

The genuine **interrupt-driven DMA on the CM4**: the CM4 self-configures LPI2C5
and **`eDMA_LPSR`**, DMA-reads the WM8962 device ID (R15 = `0x6243`), and the
eDMA completion interrupt fires on the **CM4's own NVIC natively** (`dmairq>0`).

## Why `eDMA_LPSR` (the silicon truth)

The RT1176 has **two eDMAs** (RM Tables 4-1/4-2, HW-confirmed):
- **Main eDMA** (`0x40070000`) — channel IRQs → **CM7 only**. The CM4 can drive
  it (data works) but can never take its completion IRQ. (See
  `cm4_spi_dma_test` — the CM4 SPI-DMA data path works but `dmairq=0`.)
- **`eDMA_LPSR`** (`0x40C14000`) — channel IRQs → **CM4** (native, IRQ 0-15).

So CM4 interrupt-driven DMA **requires** `eDMA_LPSR` + an LPSR-domain
peripheral. LPI2C5 (the soldered WM8962) is one; LPSPI1/LPI2C1 (main domain)
are not. See the `rt1176-cm4-edma-lpsr-split` memory.

## The DMA chain (all CM4-configured)

LPI2C5 RX-FIFO DMA request → **DMAMUX1/LPSR source 52** (`0x40C18000`) →
**`eDMA_LPSR` channel 0** (TCD `0x40C15000`) drains `MRDR` → OCRAM2 `rxbuf`
→ major-loop `INTMAJOR` → **CM4 NVIC IRQ 0** → `dmairq++`. Buffers in OCRAM2
(the eDMA can't reach the CM4's private DTCM). The register-pointer write reuses
the shared MIT `lpi2c1176` core + the 4.1 cold-bus polled discipline; only the
`eDMA_LPSR` plumbing is new.

## Tokens (MU ch0)

`ready=CAFE0001`, then `{croot, rdv, dmairq, err, done}`:

| Token | Expect | Meaning |
|-------|--------|---------|
| `croot` | — | LPI2C5 CLOCK_ROOT41 readback (informative) |
| `rdv` | `6243` (HW) / `0000` (QEMU) | WM8962 R15 device ID **DMA'd by eDMA_LPSR** — world-split, asserted HW-side |
| `dmairq` | >0 | CM4 serviced the `eDMA_LPSR` completion IRQ on its own NVIC (the milestone) |
| `err` | `0` | 0 OK / 4 = DMA stall guard expired |
| `done` | `1` | completed |

`WIRE_DMA_CM4=PASS` = `ready`, `dmairq>0`, `err=0`, `done=1`.

## Silicon-truth / world-split

QEMU's `wm8962-stub` reads `0x0000` for every register, so `rdv=0000` in QEMU
proves only the DMA/TCD sequence; the **real WM8962 on hardware returns
`0x6243`**, which is un-fakeable and also validates the 32-bit-MRDR byte
extraction. `dmairq>0` requires the `eDMA_LPSR`→CM4 IRQ to actually route on
silicon. Two documented QEMU-model accommodations (32-bit MRDR access;
RXD-before-RDDE ordering) are silicon-neutral — see the `main_cm4.c` header.

## Gates

- **QEMU:** `cmake -B build -DCMAKE_TOOLCHAIN_FILE=toolchain/rt1170-evkb.toolchain.cmake && cmake --build build && ./run_qemu.sh` (never `sh run_qemu.sh`).
- **HW (wiring-free):** WM8962 is soldered on LPI2C5 — no jumper, no external
  device, unaffected by the OTG2/A5 header issue. `pkill LinkServer redlinkserv`,
  start the pyserial reader on `/dev/cu.usbmodem5DQ2DDHVWO5EI3` @115200, flash
  `build/cm4_wire_dma_test.elf`, reset. ★★HW-VERIFIED 2026-07-19, stable 3×:
  `rdv=00006243`, `dmairq=00000002`, `WIRE_DMA_CM4=PASS`
  (`transcript_hw_evkb.txt`).
