# RT1176 eDMA Driver (DMAChannel) — Design

**Date:** 2026-07-05
**Status:** Approved (brainstorming)
**Depends on:** [I2S/SAI audio](2026-07-05-rt1176-i2s-sai-audio-design.md) (provides the SAI1 TX path this converts to DMA)

## Goal

Port a generic eDMA driver — a faithful `DMAChannel` — to the imxrt1176 Arduino/Teensyduino core, and prove it by converting the I²S TX from polled to DMA-fed. This is the parked prerequisite for EDMA-fed I²S streaming + the Teensy `AudioStream` graph, and unblocks async/DMA modes for SPI, Serial, and continuous ADC.

**Success:** a known 1 kHz sine transmitted out SAI1 **by the eDMA** (CPU idle), verified in QEMU (mem-to-mem + DMA-fed data path, bit-exact) then audible on silicon (WM8962 / J101).

## Background — key findings from recon

1. **RT1176 DMA is the *classic* eDMA (v2) + DMAMUX** — 32 channels, 32-byte TCDs — the **same architecture as Teensy 4 (RT1062)**, just relocated: eDMA `0x40070000`, DMAMUX `0x40074000`. Verified via the NXP SDK: RT1170 uses `drivers/edma/fsl_edma.c` (classic), *not* eDMA4. (Not eDMA3/eDMA4 as initially assumed.)
2. **The Teensy 4 `DMAChannel` reference is already in-tree** — `evkb/cores/teensy4/DMAChannel.{h,cpp}` (592 + 147 lines). Its `TCD_t` (SADDR/SOFF/ATTR/NBYTES/SLAST/DADDR/DOFF/CITER/DLASTSGA/CSR/BITER) is byte-identical to what we need.
3. **QEMU already models the whole chain deeply** — `hw/dma/imxrt_edma.c` runs real TCD transfers (software `START` *and* DMAMUX hardware-request), scatter-gather, DONE + INTMAJOR/INTHALF interrupts (32 ch, 16 IRQs, ch *n* and *n+16* share vector *n*); the RT1170 SoC wires SAI/LPSPI/LPUART `dma-tx` → DMAMUX → eDMA. So a gate can test DMA **end-to-end and deterministically**, including the streaming ring.

Net: a low-risk **port + register-def + wire**, not a from-scratch driver. The only genuinely tricky part — the hardware-triggered streaming ring with half/complete interrupts — is exactly what QEMU can exercise.

## Scope

**v1 delivers:** the `DMAChannel` engine **+ a DMA-fed I²S TX ring** as the flagship proof.

**API:** a **full verbatim port** of Teensy's `DMAChannel.{h,cpp}` — every trigger mode and helper — so Teensy libraries (Audio, DmaSPI, …) compile unmodified. Since the reference exists, the extra modes are near-zero incremental cost.

**Explicitly deferred (YAGNI):**
- Converting SPI / Serial / ADC to DMA (each its own follow-on spec; their DMAMUX slots are already known: LPSPI1 TX/RX 37/36, LPUART1 TX/RX 8/9, LPI2C1 48).
- The full `AudioStream` graph (this builds the seam it plugs into, not the graph).
- I²S RX / audio capture.
- The second eDMA in the LPSR domain (`kCLOCK_Edma_Lpsr`, LPCG 23) — v1 uses the main eDMA only.

## Architecture & data flow

```
audio ring (DMAMEM/OCRAM)
    │  eDMA channel — TCD: SADDR=ring, DADDR=SAI1.TDR0, SOFF=+2, minor=2B,
    │                       CITER=nFrames*2, circular (SLAST wraps SADDR)
    ▼
SAI1 TX FIFO request (TCSR.FRDE) ──DMAMUX src 55──▶ eDMA ch request
    ▼  each request moves one sample → TDR0; CPU idle
at half / full CITER → INTHALF / INTMAJOR ─▶ channel ISR (refill + count + clear)
```

## File layout

All under `cores/imxrt1176/` unless noted.

| File | Change | Responsibility |
|---|---|---|
| `DMAChannel.h` | **new** | Verbatim port of teensy4 header (592 lines): `DMABaseClass` (`TCD_t`, source/dest/buffer/circular, transferSize/Count), `DMASetting`, `DMAChannel` (begin, triggerAtHardwareEvent, enable/disable, attachInterrupt, interruptAtHalf/Completion, TCD access). Only edit: `#include "imxrt1176.h"`. |
| `DMAChannel.cpp` | **new** | Verbatim port (147 lines): `dma_channel_allocated_mask`, `begin()` channel allocation, `release`, `DMAPriorityOrder`. Three retargets (below). |
| `imxrt1176.h` | mod (generated) | eDMA + DMAMUX register block. |
| `tools/gen_imxrt1176_h.py` | mod | Emit that block; **reconcile to empty diff first** (the I²S/PIT/USB discipline). |
| `core_pins.h` | mod | `IRQ_DMA_CH0..15 = 0..15`, `IRQ_DMA_ERROR = 16` in `IRQ_NUMBER_t`. |
| `I2S.h` / `I2S.cpp` | mod | Extract `configureSAI()` helper; add `beginDMA()` DMA-fed TX path beside the polled `write()`. |
| `evkb/edma_test/` | **new** | QEMU gate: Stage A mem-to-mem, Stage B DMA-fed SAI1 ring via the existing `sai1-tap`; then HW. |

## Register definitions (`imxrt1176.h`, via generator)

Emit the classic-eDMA macro set **at RT1176 bases, using the same symbol names the ported code references** (making the port a near-verbatim diff).

**eDMA — base `0x40070000`, TCD block `0x40071000`:**
- Control/status: `DMA_CR` (bits `GRP1PRI`, `EMLM`, `EDBG`), `DMA_ES`, `DMA_ERQ`, `DMA_EEI`, `DMA_INT`, `DMA_ERR`, `DMA_HRS`.
- 8-bit command regs: `DMA_CEEI` `DMA_SEEI` `DMA_CERQ` `DMA_SERQ` `DMA_CDNE` `DMA_SSRT` `DMA_CERR` `DMA_CINT`.
- Priority: `DMA_DCHPRI0..31` (byte block @ base+0x100, accessed via `&DMA_DCHPRI3` + Teensy's swizzle) + bits `DMA_DCHPRI_ECP`, `DMA_DCHPRI_DPA`.
- TCD bitfields: `DMA_TCD_CSR_INTMAJOR/INTHALF/DREQ/DONE/ESG/START/MAJORELINK`, `DMA_TCD_CSR_MAJORLINKCH(n)`, `DMA_TCD_BITER_ELINK`, `DMA_TCD_BITER_ELINKYES_ELINK/LINKCH[_MASK]`. (TCD data is overlaid by the ported `TCD_t` — no per-field macros.)

**DMAMUX — base `0x40074000`:** `DMAMUX_CHCFG0..31` (indexed `&DMAMUX_CHCFG0 + ch`) + bits `DMAMUX_CHCFG_ENBL` (1<<31), `_TRIG` (1<<30), `_A_ON` (1<<29), `_SOURCE(n)` (n & 0x7F). Source constants (v1 needs SAI1_TX; add the rest from the RM for later): `DMAMUX_SOURCE_SAI1_TX=55`, `_SAI1_RX=54`, `_LPSPI1_TX=37`, `_LPSPI1_RX=36`, `_LPUART1_TX=8`, `_LPUART1_RX=9`, `_LPI2C1=48`.

**eDMA clock gate:** `CCM_LPCG22_DIRECT @ 0x40CC62C0` (LPCG index 22 = `kCLOCK_Edma`; `CCM_LPCG_n_DIRECT = 0x40CC6000 + n*0x20`), bit 0 = enable.

**SAI:** add `SAI_TCSR_FRDE` (TCSR bit 0, FIFO Request DMA Enable) — not yet in the header.

## `DMAChannel` port + IRQ dispatch

`DMAChannel.h` ports verbatim; all register macros resolve from the block above. `attachInterrupt` uses the core's existing relocated-vector idiom — `_VectorsRam[channel + IRQ_DMA_CH0 + 16] = isr; NVIC_ENABLE_IRQ(IRQ_DMA_CH0 + channel)` — identical to the GPIO/PIT attach paths already in the core.

`DMAChannel.cpp` ports verbatim with **exactly three retargets**:

| Teensy (RT1062) | RT1176 |
|---|---|
| `TCD = 0x400E9000 + ch*32` (lines 52, 79) | `0x40071000 + ch*32` |
| `CCM_CCGR5 \|= CCM_CCGR5_DMA(CCM_CCGR_ON)` | `CCM_LPCG22_DIRECT \|= 1` (idempotent; boot ROM may already clock it) |
| `IRQ_DMA_CH0` (from imxrt.h) | added to `core_pins.h` |

16-channel model (`DMA_MAX_CHANNELS = 16`); channels *n* and *n+16* share NVIC vector *n*, and we allocate 0–15 — matches QEMU's `channel n and n+16 share line n`. Channel ISRs clear their flag via `clearInterrupt()` → `DMA_CINT = channel`.

## I²S-DMA streaming integration

Keep the polled `write()` intact. Refactor the SAI register setup out of `begin()` into a private `configureSAI()` shared by both modes (targeted, low-risk).

New API on `I2SClass`:

```cpp
// ring = interleaved L,R,L,R… in DMAMEM; plays continuously via eDMA.
// refillHalf (optional) is the AudioStream seam: called from the DMA ISR with a
// pointer to the just-drained half so the app regenerates it. nullptr = loop the
// same buffer (static tone). Returns false if no DMA channel is free.
bool     beginDMA(const int16_t *ring, size_t nFrames, void (*refillHalf)(int16_t *half) = nullptr);
uint32_t dmaBlockCount() const;   // half/complete ISR count; the gate asserts it advances
```

TCD configuration (mirrors Teensy `AudioOutputI2S`):

| TCD field | Value | Meaning |
|---|---|---|
| `SADDR` / `SOFF` | `ring` / `+2` | walk the ring, 16-bit steps |
| `ATTR` | SSIZE=DSIZE=1 | 16-bit src + dst |
| `NBYTES` | `2` | one sample per DMA request |
| `DADDR` / `DOFF` | `&SAI1_TDR0` / `0` | fixed FIFO register |
| `CITER` / `BITER` | `nFrames*2` | samples per full loop |
| `SLAST` | `-(nFrames*2*2)` | wrap `SADDR` to ring start (circular) |
| `CSR` | `INTHALF \| INTMAJOR` | fire ISR at half + full |

Then `triggerAtHardwareEvent(DMAMUX_SOURCE_SAI1_TX)` (slot 55), `enable()`, and finally `TCSR = TE | BCE | FRDE`. The SAI's FIFO request (`FRDE`) now paces the eDMA instead of the CPU; **no polled pre-fill** (the first DMA request fills the FIFO). The internal ISR bumps `dmaBlockCount`, calls `refillHalf` if set, and clears the flag.

`beginDMA` and polled `write()` are mutually exclusive modes on the same SAI instance.

## Testing — QEMU gate + HW bring-up

`evkb/edma_test/`, two deterministic QEMU stages then silicon:

- **Stage A — mem-to-mem** (engine alone): a `DMAChannel` software-triggers (`START`) a known pattern `src[]→dst[]`; assert `dst == src`, `DONE` set, and the completion ISR ran. Pure QEMU, no peripheral. Proves TCD setup, channel allocation, and IRQ dispatch in isolation.
- **Stage B — DMA-fed I²S** (flagship): `beginDMA(sine_ring, 96)`, empty `loop()`. QEMU's eDMA moves `ring → TDR0` paced by the SAI `dma-tx` request; the **existing `sai1-tap`** captures the samples → `check_tap.py` compares bit-exact to the sine (same check the polled I²S gate used). Also assert `dmaBlockCount` advanced (proves INTHALF/INTMAJOR). Exercises the entire SAI→DMAMUX→eDMA chain.
- **HW:** flash; same 1 kHz sine audible on J101, now **DMA-driven with the CPU idle** (a fast-toggled LED in `loop()` demonstrates the core is free). Saleae shows clean I²S. Audible proof = DMA streaming works on silicon.

Gate harness reuses the I²S pattern: `run_qemu_edma.sh` (qrun + `-chardev file` tap) greps `STAGE_A_PASS`, runs `check_tap.py`, checks `STAGE_B_PASS`.

## Error handling & gotchas

- **Ring in DMAMEM/OCRAM**, never DTCM — DTCM is DMA-unreachable on this part (bit us in USB; see the serialusb note). The gate's `sine_ring` and the mem-to-mem buffers use `DMAMEM`.
- **Cache coherency:** OCRAM DMA buffer — a single cache flush after building the static sine (or confirm OCRAM is non-cacheable via MPU; the core's dcache ops are currently no-ops, so likely already fine — verify on HW).
- **Channel exhaustion:** `begin()` sets `TCD == 0` when none free; `beginDMA` checks and returns `false`.
- **`SLAST` wrap** must be exactly `-(bytes in ring)` or `SADDR` walks off the buffer → noise/fault.
- **eDMA clock:** `CCM_LPCG22_DIRECT |= 1` in `begin()` (idempotent). QEMU is agnostic to clock gating; this matters only on silicon.
- **Mode exclusivity:** don't mix polled `write()` and `beginDMA()` on SAI1.

## References

- Teensy 4 reference: `evkb/cores/teensy4/DMAChannel.{h,cpp}`.
- NXP classic eDMA driver: `nxp/mcuxsdk-core/drivers/edma/fsl_edma.{c,h}`.
- QEMU model: `qemu2/hw/dma/imxrt_edma.c`, `hw/dma/imxrt_dmamux.c`; SoC wiring `hw/arm/fsl-imxrt1170.c`.
- Prior art (same core): I²S/SAI, IntervalTimer/PIT, SerialUSB specs in this directory.
