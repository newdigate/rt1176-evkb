# cm4_spi_dma_test — Phase 4.3: CM4 DMA-driven SPI

The CM4 self-configures **LPSPI1** (shared `lpspi1176_begin`, the same core the
CM7 `SPIClass` runs) and runs full-duplex **eDMA** self-loopback over the
SDO→SDI jumper in two stages:

- **STAGE_BLOCKING** — 2 eDMA channels (RX ch0: `RDR`→rxbuf, TX ch1:
  txbuf→`TDR`), armed by LPSPI DMA requests (`DER`), completion detected by
  polling the RX channel `CSR.DONE`. No split-IRQ needed.
- **STAGE_ASYNC** — same, but the RX major-loop completion raises the eDMA ch0
  interrupt on the **CM4's own NVIC** (IRQ 0, via the qemu2 per-line
  split-IRQ from the Phase 4.1 foundation); `DMA_CH0_IRQHandler` sets
  `dmairq`. `dmairq>0` is the isolated routing proof.

The DMA path is distilled from `SPI.cpp::startDMA`/`dma_rxisr` into direct
TCD/DMAMUX register writes — no `DMAChannel` class, no `EventResponder`.

**Buffers live in OCRAM2 (`0x202C0000`, `.dmabuf` section):** the eDMA is a
system-bus master and cannot reach the CM4's private DTCM. OCRAM2 is
system-visible and unused by the CM7 gate (whose heap is OCRAM1
`0x20240000`+512 K, ending exactly at OCRAM2).

## Tokens (MU ch0)

`ready=CAFE0001`, then `{cr, cfgr1, lpcg, croot, rxb, dmairq, rxa, done}`:

| Token | Expect | Meaning |
|-------|--------|---------|
| `cr` | `1` | LPSPI `CR.MEN` — master enabled |
| `cfgr1` | `1` | `CFGR1.MASTER` |
| `lpcg`/`croot` | — | clock readbacks (informative, not asserted) |
| `rxb` | `1` | STAGE_BLOCKING rx==tx (poll DONE) |
| `dmairq` | >0 | CM4 serviced the eDMA RX completion IRQ (split proof) |
| `rxa` | `1` | STAGE_ASYNC rx==tx (interrupt-driven) |
| `done` | `1` | CM4 sequence completed |

`SPI_DMA_CM4=PASS` requires `cr=1`, `cfgr1=1`, `rxb=1`, `dmairq>0`, `rxa=1`,
`done=1`.

## ★★HW FINDING (2026-07-19): the interrupt stage is CM7-only on silicon

QEMU passes both stages, but the **hardware** probe (SDO→SDI jumper) showed
`rxb=1, rxa=1` (the CM4-driven **DMA data path works**) yet **`dmairq=0`**: the
**main eDMA's completion interrupt is wired to the CM7's NVIC, not the CM4's**.
The RT1176 has two eDMAs — the main eDMA (`0x40070000`, IRQs → CM7) and
`eDMA_LPSR` (`0x40C14000`, IRQs → CM4). The CM4 can *drive* the main eDMA but
cannot take its completion IRQ. So on silicon this test is a **polled**
CM4-DMA result; genuine CM4 interrupt-driven DMA lives on `eDMA_LPSR` + an
LPSR peripheral (the DMA-Wire slice on LPI2C5). QEMU's single-eDMA split hid
this. See the `rt1176-cm4-edma-lpsr-split` memory. `transcript_hw_evkb.txt`
records the finding.

## Silicon-truth (world-split, same as Phase 3.1 `cm4_spi_test`)

The qemu2 `ssi-loopback` child echoes tx→rx on `CR.MEN` **alone** — it ignores
the clock gate, clock root, and pin mux. So `rxb`/`rxa` in QEMU prove only the
TCD/DMA register sequence; the **real SDO→SDI jumper on hardware** is what
proves the CM4 ungated the clock, muxed the pins, and drove a real DMA-fed SCK.

## QEMU gate

```sh
cmake -B build -DCMAKE_TOOLCHAIN_FILE=toolchain/rt1170-evkb.toolchain.cmake
cmake --build build
./run_qemu.sh          # NOT `sh run_qemu.sh` (gate-lib re-exec quirk)
```

## HW probe (jumper)

### Wiring (EVKB Arduino header)

A single loopback jumper — the same **SDO→SDI (MOSI→MISO)** jumper as Phase 3.1:

| From | To |
|------|----|
| **SDO** = `GPIO_AD_30` (MOSI) | **SDI** = `GPIO_AD_31` (MISO) |

No external device, no pull-ups. SCK = `GPIO_AD_28`.

### Procedure

1. `cmake --build build`.
2. `pkill LinkServer; pkill redlinkserv` before flashing.
3. Start the pyserial VCOM reader (115200, `gtimeout`, `/dev/cu.usbmodem5DQ2DDHVWO5EI3`)
   **before** resetting (per the macOS serial-capture note).
4. Flash `build/cm4_spi_dma_test.elf` (LinkServer), reset.
5. Expect: `ready=CAFE0001`, `cr=1`, `cfgr1=1`, `rxb=1`, `dmairq>0`, `rxa=1`,
   `done=1`, `SPI_DMA_CM4=PASS`.

**Un-fakeable:** `rxb==1 && rxa==1` require the tx pattern to return through the
physical SDO→SDI jumper via DMA in *both* stages; `dmairq>0` requires the CM4 to
have taken the eDMA completion IRQ on its own NVIC. Split-not-routed ⇒
`dmairq=0` ⇒ the async guard expires ⇒ FAIL.

`transcript_hw_evkb.txt` holds the probe output once run.
