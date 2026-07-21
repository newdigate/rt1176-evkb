# CM4 Audio Pipeline (Plan 2 of 2) Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Complete symmetric audio ownership: the shared interrupt-driven SAI engine + `AudioOutputI2SInt`/`AudioInputI2SInt` nodes (both cores), CMSIS-DSP compiled into CM4 images, the ownership-claim guard, and the capstone — the CM4 owning codec + SAI + graph + FFT with the CM7 parked in WFI.

**Architecture:** Four capabilities: (1) `sai1176` shared C core (distilled from the HW-verified `cores/imxrt1176/I2S.cpp` + `Audio/output_i2s.cpp` __IMXRT1176__ sequences, lpi2c1176-style address-parameterized) + the two interrupt-driven nodes in the Audio fork, proven first on the **CM7** (`i2s_int_test`, audible); (2) CMSIS-DSP amalgams compiled into CM4 images via a new evkb.cmake helper, proven by `cm4_fft_test` (known-answer FFT on the M4); (3) the capstone `cm4_audio_test`; (4) wrap (audit, Audio pin bump, status doc, roadmap). Spec: `2026-07-21-cm4-audio-ownership-design.md` (+ its amendments). Foundation facts (Plan 1, all HW-verified): SAI1 IRQ 76 on the CM4 NVIC; CM4 C++ image world (`cm4_cpp_test` scaffolding: runtime_stubs.c, mu_report.h, init_array cm4.ld, `cpsie i`, entry `main`); `cm4_shim/Arduino.h` (IRQ 44 convention, attachInterruptVector no-op → static vector + extern-C wrapper); DWT enable required for `software_isr`.

**Key register/derivation facts (verified in Plan 1 / earlier phases — do not re-derive):**
- SAI1 base 0x40404000, TCSR at **+0x08** (not +0x00), TDR0 +0x20, RDR0 +0xA0 (verify against `imxrt1176.h` before use — the +0xA0 offset was NOT probed in Plan 1), TMR +0x60, RMR +0xE0.
- `FRIE`=bit 8, `FRF`=bit 16, `FRDE`=bit 0 (TCSR/RCSR); TX FRF when count≤watermark, RX FRF when count>watermark; `TCR1.TFW`/`RCR1.RFW` 5-bit; FIFO 32×32-bit; TX needs `TCR4.FCONT` (bit 28) + 16-word prefill before `TE|BCE`; RX synchronous `RCR2.SYNC(1)`, RX enable also asserts TX (shared BCLK/FS — see `Audio/input_i2s.cpp:141-147`).
- 44.1 kHz clocking: Audio PLL loopDiv=30/num=1056/den=10000, clock root 64 = `(4<<8)|15`, LPCG123, pads AD_17/21/22/23 (+RX AD_20, `IOMUXC_SAI1_RX_DATA0_SELECT_INPUT=0`) — the complete sequence is `Audio/output_i2s.cpp:534-632` (config_i2s, __IMXRT1176__ branch) and `input_i2s.cpp:93-147`.
- WM8962: LPI2C5 from the CM4 is HW-verified (`cm4_wire_test`, `cm4_wire_int_master_test`); codec register program = `examples/audio/i2s_audio_test/i2s_audio_test.cpp` LocalWM8962 (lines 12-76; addr 0x1A, 16-bit regs framed {0x00,reg,hi,lo}, INIT_PRE → PLL-bit clear → INIT_POST → 3× write-sequencer with 0x5D bit0 poll → routing → ADDCTL3/CLK4). QEMU's wm8962 stub reads 0x0000 (world-split convention: assert write-side ACKs in QEMU, values on HW).
- The CM4 cannot use DMA for SAI (main-eDMA IRQs CM7-only). ★New-territory flag: the CM4 bringing up the ANATOP Audio PLL (AI writes) has no precedent — clock behavior is a probe trigger; the capstone HW run IS the probe; if the PLL refuses from the CM4, fallback = CM7 pre-arms the PLL before boot (a 3-line CM7-side concession, documented, ownership of everything else stays CM4).

---

### Task 1: `sai1176` shared core + interrupt-driven nodes + CM7 proof (`i2s_int_test`)

**Files:**
- Create (Audio fork): `sai1176.h`, `sai1176.c` — freestanding C11, address-parameterized like `Wire/lpi2c1176.{h,c}` (read that header's design comment first; same rules: no NVIC calls inside, consumer sequences enable; both worlds compile the same object logic).
- Create (Audio fork): `output_i2s_int.h/.cpp` (`AudioOutputI2SInt`), `input_i2s_int.h/.cpp` (`AudioInputI2SInt`) — guarded `#if defined(__IMXRT1176__) || defined(EVKB_CM4_WORLD)`.
- Create (evkb): `examples/audio/i2s_int_test/` gate.

- [ ] **Step 1: `sai1176.{h,c}`** — API (exact signatures):

```c
typedef struct {           // register addresses, supplied by the consumer
    uint32_t sai_base;     // 0x40404000 for SAI1
    uint32_t ccm_root;     // clock root reg addr
    uint32_t lpcg;         // LPCG reg addr
    // pad/mux/daisy addrs for mclk/bclk/sync/txd/rxd + gpr0
    uint32_t pad_mclk, pad_bclk, pad_sync, pad_txd, pad_rxd, rxd_daisy, gpr0;
} sai1176_hw_t;

void sai1176_pll_init_44k(void);                       // ANATOP AI writes (from output_i2s.cpp:578-597)
void sai1176_config(const sai1176_hw_t *hw);           // clocks+pads+TX regs (TCR1=16 watermark, FCONT) + RX regs (RCR1=0, SYNC)
void sai1176_tx_start_int(const sai1176_hw_t *hw);     // 16-word prefill, then TCSR = TE|BCE|FRIE
void sai1176_rx_start_int(const sai1176_hw_t *hw);     // RCSR = RE|BCE|FRIE|FR (TX must already run)
uint32_t sai1176_tx_service(const sai1176_hw_t *hw, const int16_t *l, const int16_t *r, uint32_t frames);  // feed up to `frames` L/R pairs while FRF; returns frames consumed
uint32_t sai1176_rx_service(const sai1176_hw_t *hw, int16_t *l, int16_t *r, uint32_t frames);              // drain while FRF; returns frames read
```

Bodies are distilled from `cores/imxrt1176/I2S.cpp` (config, prefill, FCONT) and `Audio/output_i2s.cpp`/`input_i2s.cpp` __IMXRT1176__ branches (44.1k PLL, pads, TDR0/RDR0 at +0 — 16-bit right-packed per FBT=15). MIT headers; file comment cites both sources + the Plan-1 probe. **Verify RDR0's offset (+0xA0) and the RCSR offset against `imxrt1176.h` before writing** — RX register offsets were not exercised by the Plan-1 probe.

- [ ] **Step 2: the nodes.** `AudioOutputI2SInt`: double-buffered block queue exactly like `output_i2s.cpp`'s block_left_1st/2nd logic (copy the update()/release discipline — same fork, MIT); `begin()` calls sai1176_config + tx_start_int and hooks the ISR; a static `isr()` feeds the FIFO via `sai1176_tx_service` from the current blocks and, each time 128 frames complete, advances blocks and pends `IRQ_SOFTWARE` (this ISR is the graph clock — mirror the DMA-node's half-buffer cadence). `AudioInputI2SInt`: fills a block from `sai1176_rx_service` in the same ISR path (TX ISR services both directions — one IRQ line; check FRF of both sides), submits via the input_i2s block pattern. NVIC hookup is per-world in `begin()`: CM7 = `attachInterruptVector(IRQ_SAI1, isr); NVIC_ENABLE_IRQ(IRQ_SAI1)` (IRQ_SAI1=76 — add `#define IRQ_SAI1 76` locally if the core lacks it); CM4 = the consumer places `SAI1_IRQHandler` (extern-C wrapper calling the node's isr) in the static vector table and writes NVIC_ISER2 + `cpsie i` (document in the node header).

- [ ] **Step 3: CM7 gate `examples/audio/i2s_int_test`** — sine → AudioOutputI2SInt, WM8962 via the Audio fork's `control_wm8962` + Wire (the `audiooutput_i2s_test` pattern — read its CMakeLists for the import set), plus AudioInputI2SInt → analyze_peak (mic). QEMU: assert the deterministic side (graph runs off the SAI ISR: token when N software-IRQ dispatches occurred; FEF underrun flag stays 0; peak from rx-inject if the model path cooperates — else world-split: document and assert on HW). Use the **poll-loop runner pattern** (NOT fixed sleep — the audiooutput flake lesson): poll for the DONE token up to 40×0.25s. HW: audible 1 kHz on J101 + mic peak > threshold on the right channel. Transcripts committed.

- [ ] **Step 4: Commits** — Audio fork (nodes+core, one commit), evkb (gate). Trailer on both. No pushes yet.

---

### Task 2: CMSIS-DSP into CM4 images + `cm4_fft_test`

**Files:** Modify `evkb.cmake`; create `examples/dualcore/cm4_fft_test/`.

- [ ] **Step 1: evkb.cmake helper** — after `import_evkb_cmsis_dsp()`, add:

```cmake
# CMSIS-DSP for CM4 images: the image world compiles per-source (no CMake
# targets), so expose the amalgam source list + include dirs for
# teensy_add_cm4_image(SOURCES ... INCLUDE_DIRS ...). Reuses the same pinned
# fetch and the same generated collision-shim dir as the CM7 target (the shim
# is world-agnostic: cm4_shim/Arduino.h has the same __disable_irq/__enable_irq
# inline collision with cmsis_gcc.h that the CM7 core does).
macro(evkb_cmsis_dsp_cm4_sources OUT_SRCS OUT_INCS)
    evkb_library_dir(CMSIS-DSP _evkb_cmsisdsp_dir)
    evkb_library_dir(CMSIS-Core _evkb_cmsiscore_dir)
    _evkb_cmsis_dsp_amalgams("${_evkb_cmsisdsp_dir}" ${OUT_SRCS})
    _evkb_cmsis_dsp_shim("${_evkb_cmsisdsp_dir}" _evkb_cmsis_shim_dir)
    set(${OUT_INCS}
        "${_evkb_cmsis_shim_dir}"
        "${_evkb_cmsisdsp_dir}/Include"
        "${_evkb_cmsisdsp_dir}/PrivateInclude"
        "${_evkb_cmsiscore_dir}/CMSIS/Core/Include")
endmacro()
```

Refactor step (same edit): extract the existing amalgam `foreach` from
`import_evkb_cmsis_dsp()` into `_evkb_cmsis_dsp_amalgams(<dspdir> <outvar>)`
and the `file(CONFIGURE)` shim generation into
`_evkb_cmsis_dsp_shim(<dspdir> <outdirvar>)`, then have BOTH macros call the
shared helpers — the amalgam list and the shim must live exactly once. Verify
the refactor didn't disturb the CM7 path: reconfigure+rebuild
`examples/framework/arm_math_test` and re-run its gate (must stay green).
`--gc-sections` at the image link keeps unused kernels out of ITCM.

- [ ] **Step 2: gate `cm4_fft_test`** — scaffold from `cm4_cpp_test`; the CM4 image compiles the three known-answer stages of `examples/framework/arm_math_test/arm_math_test.cpp` (FFT bin-8 with the [5e7,9e7] bounds, FIR echo, sin sweep) ported to MU reporting (drop Serial prints; report fft_bin, fft_mag2, fir_ok, sin_ok, done marker). CM7 relay asserts `FFT_CM4=PASS`. Note: sinf/cosf/lrintf need libm — the image links `-nostdlib`; add `-lm -lgcc` to the image LINK step ONLY if link errors demand it (macro change, reported), else precompute the FFT input table in Python-free integer form: fill from a 64-entry const q15 table included in the source (preferred — keeps the image freestanding; the plan chooses THIS: generate the 256-pt cos input from the existing `AudioWaveformSine` q15 table logic — actually simplest: hardcode the input as `(q15_t)(16384*cos(2π·8i/256))` values emitted as a const array in the source, generated by a comment-documented one-liner). Implementer picks the const-array route; sin-vs-libm stage is REPLACED by a q31 known-answer spot check (arm_sin_q31 at 5 fixed angles vs precomputed expected values ±1e-4 scaled) so libm is not needed at all.
- [ ] **Step 3: QEMU + HW green, transcripts, commit.** ITCM watch: report the image's size (`arm-none-eabi-size`) — if .text+.rodata approach 128K, that is a finding for the capstone.

---

### Task 3: Capstone `cm4_audio_test` — the CM4 owns the pipeline

**Files:** Create `examples/dualcore/cm4_audio_test/`; modify `evkb.cmake` (+ownership claim helper).

- [ ] **Step 1: ownership guard in evkb.cmake:**

```cmake
# Build-time audio-ownership exclusivity (spec §4): a firmware claims audio
# for exactly one core. Called by convention from gate CMakeLists that compile
# SAI I/O node sources.
macro(evkb_claim_audio_owner CORE)
    get_property(_owner GLOBAL PROPERTY EVKB_AUDIO_OWNER)
    if(_owner AND NOT _owner STREQUAL "${CORE}")
        message(FATAL_ERROR "audio owner already claimed by ${_owner}; cannot also claim ${CORE} (one core owns audio per firmware)")
    endif()
    set_property(GLOBAL PROPERTY EVKB_AUDIO_OWNER "${CORE}")
endmacro()
```

Add `evkb_claim_audio_owner(CM7)` to `i2s_int_test`'s CMakeLists (Task 1 retrofit, same commit) and `evkb_claim_audio_owner(CM4)` to the capstone's.

- [ ] **Step 2: the CM4 image** — SOURCES: cm4_cpp_test scaffold files + `${EVKB_CORES_DIR}/AudioStream.cpp` + Audio fork's `sai1176.c`, `output_i2s_int.cpp`, `input_i2s_int.cpp`, `synth_sine.cpp`, `analyze_peak.cpp`, `analyze_fft256.cpp`, `data_waveforms.c`, `data_windows.c`, `utility/sqrt_integer.c` + CMSIS amalgams via `evkb_cmsis_dsp_cm4_sources` + `cm4/codec_wm8962.c` (the LocalWM8962 register program transcribed over raw `lpi2c1176_*` calls — Wire/lpi2c1176.c in SOURCES, LPI2C5 literals from `cm4_wire_test/cm4/main_cm4.c`; MIT attribution comment). `cm4/main_cm4.cpp`: ctors → DWT enable → codec init (report each phase over MU) → graph statics (sine 1 kHz → AudioOutputI2SInt; AudioInputI2SInt → peak + fft256) → `begin()`s with the CM4 NVIC hookup (SAI1_IRQHandler wrapper at index 92 — reuse the cm4_sai_irq_probe startup edit; Software_IRQHandler at 60 from cm4_audiostream_test) → run ~2 s → report {codec_ack, sai_isr_count, dispatch_count, out_underruns(FEF), mic_peak_q15, fft_peak_bin, fft_peak_mag} → done marker → WFI.
- [ ] **Step 3: the CM7 side** — boot the image, then **park**: a WFI loop that only services the MU (count every wakeup; assert at the end that no SAI/eDMA/audio IRQ was ever enabled on the CM7 NVIC — read-back NVIC_ISERx bits for IRQs 76 and the eDMA range and print them as `cm7_audio_isers=0`). Print all CM4 observations; verdict `AUDIO_CM4=PASS` requires codec_ack ok, sai_isr_count>1000, dispatch_count>500, underruns==0, fft_peak_bin == the sine's bin, and (HW-only) mic_peak above threshold — QEMU world-split: mic_peak asserted `>=0` only, fft bin asserted exactly (the synth path is deterministic; QEMU's SAI drains TX via its ring so the TX side runs).
- [ ] **Step 4: QEMU gate green (poll-loop runner); HW: audible 1 kHz on J101 from a CM4-owned pipeline + mic capture; transcripts.** If the ANATOP PLL refuses CM4 writes (silence + zero ISRs on HW only): apply the documented fallback (CM7 pre-arms the PLL via `sai1176_pll_init_44k()` before `Multicore.begin`, one commented block in the CM7 sketch), re-run, and record the finding as a ★★silicon truth in the report — that is a probe result, not a failure.
- [ ] **Step 5: Commits** (Audio if nodes needed fixes; evkb gate + evkb.cmake).

---

### Task 4: Wrap — audit, pins, docs, roadmap, push

- [ ] license-audit GATES += `examples/audio/i2s_int_test:i2s_int_test examples/dualcore/cm4_fft_test:cm4_fft_test examples/dualcore/cm4_audio_test:cm4_audio_test`; run → PASS.
- [ ] Push Audio; bump the Audio pin in evkb.cmake; FORCE_FETCH proof (capstone configure+build+QEMU boot green); push evkb (+cores/macros if touched).
- [ ] `Audio/docs/rt1170-evkb-status.md`: new section "CM4 ownership" — the interrupt-driven nodes' rows (✅ HW-verified via i2s_int_test / cm4_audio_test), a note that DMA nodes remain CM7-only forever (eDMA IRQ routing), the ownership-claim convention, and CMSIS-DSP availability on the M4 (`cm4_fft_test`). Changelog + roadmap entries (Phase A unchanged — this is the CM4 track).
- [ ] cm4-roadmap.md: Phase 5 Plan 2 complete (capstone result incl. any PLL finding, ITCM size numbers); session log entry. Push everything; report final `status -sb` for all repos.

## Verification (whole plan)

1. `i2s_int_test` audible on HW (CM7 ownership via int nodes) + QEMU green.
2. `cm4_fft_test` green both worlds (CMSIS on M4; image size reported).
3. `cm4_audio_test`: audible on HW from the CM4-owned pipeline, mic captured, CM7 parked with zero audio IRQs enabled; QEMU green under world-split assertions.
4. Ownership guard FATAL_ERROR demonstrated once (configure a scratch dir claiming both — show the error text — then delete it).
5. Audit PASS; pins == pushed HEADs; FORCE_FETCH green; docs/roadmap true.
