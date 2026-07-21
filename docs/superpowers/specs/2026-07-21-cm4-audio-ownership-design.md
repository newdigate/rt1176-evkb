# Symmetric audio ownership — the Audio library on the CM4 — design

**Date:** 2026-07-21
**Goal:** either core — CM7 or CM4 — can own the entire audio pipeline (codec
config, SAI I/O, AudioStream graph, full node set including CMSIS-DSP), chosen
at build time, never both in the same firmware. Today audio is CM7-only.
Follows the CM4 bring-up methodology (silicon-truth loop, license firewall);
this is the next CM4 capability after Phase 4.

## Decisions (from brainstorm 2026-07-21)

| Question | Decision |
|---|---|
| Independence model | **Symmetric build-time ownership** — one core owns the whole pipeline per firmware; no simultaneous dual-graph, no IPC audio bridge (out of scope). |
| CM4 node set | **Full parity with the CM7** — including CMSIS-DSP compiled for Cortex-M4 (per-core manifest lib variant). |
| Silicon anchor | **Both directions + idle CM7, one firmware** — CM4 configures WM8962, audible synth tone on J101, onboard mic captured through the CM4 graph, fft256 known-answer (CMSIS-M4 proof), CM7 parked in WFI reporting zero audio IRQs taken. |

## Hard constraints (established facts)

- **DMA audio on the CM4 is impossible**: main-eDMA channel completion IRQs go
  to the CM7 only (HW-verified, [[rt1176-cm4-edma-lpsr-split]]), and SAI is not
  an eDMA_LPSR/DMAMUX_LPSR source. The existing DMA nodes
  (`input_i2s`/`output_i2s`) can never run on the CM4.
- **SAI1 IRQ 76 appears in the RM's CM4 interrupt table** (`rm_full.txt:3760`)
  — the direct FIFO-watermark interrupt path is architecturally open. This
  same table family lied about fast-GPIO on the CM4, so this claim MANDATES a
  silicon probe before anything is built on it (risk-trigger table:
  IRQ routing we now depend on).
- CM4 codec control is done tech: LPI2C5 Wire master from the CM4 is
  HW-verified (Phase 3.2/4.1), and the CM4 self-configuring a main-domain
  peripheral's clocks/pins has precedent (LPSPI1, Phase 3.1).
- CM4 memory: fixed 128K ITCM (image slot) + 128K DTCM. Audio pool lives in
  CM4 DTCM (~40 blocks ≈ 10K). CMSIS-DSP tables press on the ITCM slot budget
  — see risks.
- Known SAI silicon traps carry over: TX underruns with an empty FIFO unless
  `TCR4.FCONT=1` + prefill ([[rt1176-i2s-sai]]); RX runs synchronous
  (`RCR2.SYNC=1`); the onboard mic is the WM8962 RIGHT channel.

## 1. Probe first: `cm4_audio_probe`

Minimal CM4 firmware (probe-skeleton template): CM4 self-configures SAI1
clock/pinmux, enables the TX FIFO-watermark interrupt on its own NVIC (IRQ 76
per the RM CM4 table), counts ISR entries, prints via the established CM4
reporting path. Run on the EVKB before any node code is written.

- **Pass** → the interrupt-driven ownership design proceeds.
- **Fail** → pivot: timer-clocked polled-FIFO engine (CM4 SysTick or an
  LPSR-domain timer paces block-rate polling; FIFO depth 32 gives ~360 µs of
  slack per service at 44.1 kHz stereo). Degraded (tighter jitter margin) but
  viable; the node API above it is identical either way.

The probe also settles: whether SAI1 register access and CCM LPCG gating from
the CM4 behave identically to the CM7 path (clock-gating is itself a probe
trigger).

## 2. Shared-core interrupt-driven SAI + new nodes

- **`sai1176_*` shared C core in the Audio fork** (Phase 3.3 pattern —
  `lpspi1176`/`lpi2c1176` precedent): SAI1 clock/pinmux/TCR-RCR configuration
  plus a FIFO-watermark TX/RX interrupt engine. Compiled byte-identically by
  both cores; only NVIC enable/IRQ numbers differ per world.
- **New nodes `AudioOutputI2SInt` / `AudioInputI2SInt`**: interrupt-driven, no
  DMA, no DMAMEM. The TX ISR refills the FIFO from the current audio block and
  pends the graph's software-update IRQ once per AUDIO_BLOCK_SAMPLES —
  mirroring the DMA half-complete clocking of the existing nodes. RX twin
  fills blocks synchronously.
- These nodes compile for the CM7 too, giving it a DMA-free I2S option — and
  giving the "shared core, both worlds" proof a cheap CM7 gate.

## 3. CM4 graph engine + full parity

- `AudioStream.{h,cpp}` compiles into the CM4 core variant. The CM4 dispatch
  IRQ gets a spare slot from the CM4 vector table (triangulated at plan time;
  the CM7 keeps IRQ_SOFTWARE=44). Pool placement: CM4 DTCM.
- **CMSIS-DSP for M4**: the evkb.cmake macro grows a per-core variant — a
  second static-lib target (working name `CMSIS-DSP-CM4`) compiled with the
  CM4 flags interface from the same pinned fetch, same amalgam-only rule, same
  collision shim. fft256/1024, filter_fir, ladder, flange, tonesweep become
  CM4-capable.
- Audio nodes themselves are core-agnostic C++ (the guard sweep made the
  chip-list question architecture-based; `__ARM_ARCH_7EM__` is true on the CM4
  as well — Cortex-M4 IS ARMv7E-M, so the revived nodes compile for the CM4
  with no further guard work).

## 4. Ownership model & build integration

- Build-time exclusivity: audio sources compile into exactly ONE image per
  firmware. Enforced lightly — a CMake configure-time error if both the CM7
  executable and a CM4 image pull the SAI node sources; otherwise convention,
  consistent with the one-peripheral-one-core rule. No runtime arbitration.
- `teensy_add_cm4_image` (or a thin wrapper) gains the ability to compile
  Audio fork sources + `CMSIS-DSP-CM4` into a CM4 image.

## 5. QEMU model & gates

- qemu2: fan SAI1's IRQ line to both NVICs (`fsl_imxrt1170_connect_irq_both`
  precedent from Phase 4) and add FIFO-watermark interrupt behavior to the SAI
  model if it only services DMA today. Any model change runs the qemu2
  regression set + checkpatch. GPL one-way firewall as always.
- Gates, in order:
  1. `cm4_audio_probe` — the IRQ-routing probe (EVKB-first; QEMU version
     documents expected-divergence if the model leads silicon).
  2. `cm4_audiostream_test` — graph engine on the CM4, synthetic source/sink
     (the audiostream_test pattern, CM4 world).
  3. A CM7 gate for the interrupt-driven nodes (both-worlds shared-core
     proof; also the CM7's DMA-free option regression).
  4. **Capstone `cm4_audio_test`** — the silicon anchor from the decisions
     table: codec + SAI TX/RX + graph + fft256 known-answer on the CM4;
     CM7 boots it then parks in WFI and reports zero audio interrupts.
- License audit: new gates added to Part-2 GATES; no new source trees beyond
  the already-covered CMSIS fetch (M4 variant compiles the same files).

## 6. Risks

- **Probe failure** — pivot path defined (§1); the phase does not sink.
- **CM4 ITCM slot budget** (128K): CMSIS-DSP tables + wavetable data are
  large. Mitigations, in order: `--gc-sections` (only used tables survive);
  keeping big const data in the flash-resident part of the CM4 image layout
  rather than TCM (linker-script question at plan time); if neither suffices,
  fft1024 (largest tables) may be documented CM7-only until a bigger-slot
  option exists.
- **QEMU cross-vCPU timing** — known class (TXDSTALL race precedent);
  world-split and `-icount` conventions apply; deterministic assertions where
  possible, HW as the oracle for the rest.
- **SAI model fidelity** — this work family has repeatedly found silicon-only
  bugs (TX-clock enable, RDR0 half-word, FCONT); expect the HW capstone to be
  where the truth lands.

## Amendments (post-implementation, 2026-07-21 — Plan 1 of 2)

- Execution split into two plans; Plan 1 (`2026-07-21-cm4-audio-foundation.md`,
  SHIPPED) covers the probe, the qemu2 model, the CM4 C++ image world, and the
  graph engine; Plan 2 owns the sai1176 core, interrupt I/O nodes,
  CMSIS-DSP-CM4, and the capstone.
- §3's "AudioStream compiles into the CM4 core variant" shipped differently:
  the CM4 image world stayed explicit-SOURCES — `AudioStream.cpp` compiles
  UNMODIFIED into the CM4 *image* against the new
  `cores/imxrt1176/cm4_shim/Arduino.h` (Arduino-lite); no core variant exists.
- §3's "dispatch IRQ gets a spare slot" was wrong: IRQ 44 is CAN1 on BOTH
  cores (RM Tables 4-1/4-2) — the CM7's IRQ_SOFTWARE=44 already worked only by
  repurposed-unused-CAN1 convention, and the CM4 adopts the same slot 44.
- The probe shipped as `cm4_sai_irq_probe` (not §1's `cm4_audio_probe`);
  result: PASS on silicon, irqcnt=0x14. The qemu2 SAI model's interrupt is
  ring-occupancy-based, not watermark-accurate (QEMU irqcnt=0x40) —
  documented fidelity limit.

## Out of scope

Simultaneous dual-core audio, IPC audio bridging (a future phase could add
AudioInputIPC/AudioOutputIPC nodes on top of this), runtime ownership
handoff/arbitration, MQS/SPDIF/other I/O ports, CM4 SD/USB audio sources.
