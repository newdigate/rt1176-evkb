# M0 gate sweep — after raising BUS_CLK_ROOT to 240 MHz

**Date:** 2026-07-27
**Change under test:** `cores/imxrt1176` `afff78a` — `BUS_CLK_ROOT` (CCM_CLOCK_ROOT2) from
`SYS_PLL2_OUT/3` = 176 MHz to `SYS_PLL3_OUT/2` = **240 MHz**, plus the `BYPASS` bit correction
(bit 0 → bit 16) in both PLL-usable guards.

## Result

| Check | Result |
|---|---|
| Examples rebuilt | **61 / 61 clean**, 0 build failures |
| QEMU gates | **27 passed, 1 failed** (`dualcore/cm4_audio_test`) |
| `rpi_panel_test` | PASS |
| `lvgl_rpi_panel_test` | PASS |

The 240 MHz change is clear: every example still builds, and every gate that was passing before
still passes.

## The one failure is pre-existing and unrelated

`dualcore/cm4_audio_test` fails with `fft_peak_bin=00000000` (expected `6`) and
`AUDIO_CM4_DET=FAIL`. The SAI path itself is healthy in the run — `sai_isr_count=0x400`,
`dispatch_count=0x400`, `underruns=0`, `rx_overflows=0` — so ISRs fire and dispatch; the FFT
simply sees silence.

It was bisected on one variable at a time. **Every row below failed identically:**

| Variable | Value tested | Result |
|---|---|---|
| `cores` | `afff78a` — 240 MHz (this change) | FAIL |
| `cores` | `91a7efa` — 176 MHz (previous) | FAIL |
| `cores` | `189241c` — before any bus-clock change | FAIL |
| `qemu2` | `79a9990b39` reverted (ANADIG AI-gated STABLE + PGMC) | FAIL |
| `qemu2` | whole tree at `36d3be0e1f`, the last Jul-22 commit, before **all 20** display commits | FAIL |

### What this rules out

- **Not** the 240 MHz change.
- **Not** the earlier 176 MHz change.
- **Not** qemu2 — including the entire RPi-display model series. The ANADIG model is exonerated,
  which matters independently: the RK055 work configures clock roots through it.

A first hypothesis that `79a9990b39` ("VIDEO PLL AI-gated STABLE") was responsible — suggested by
the gate's `info pll_prearm=1 (CM7 fallback engaged)` line — was **tested and refuted**.

### What remains unidentified

The stored `transcript_qemu.txt` shows `fft_peak_bin=00000006` / `AUDIO_CM4_DET=PASS`, so the gate
did pass when that transcript was committed (`ffac1d6`, 2026-07-22 11:05). The remaining search
space is:

1. `cores` commits from Jul 23-25 (ten, all display/PXP work — none obviously touching SAI).
2. The `Audio` or `CMSIS-DSP` sibling libraries.
3. **A stale transcript.** `ffac1d6` modified `CMakeLists.txt` *and* `transcript_qemu.txt` in the
   same commit, changing which pipeline the example builds by default ("default = working pre-arm
   pipeline; pure-CM4-PLL is the opt-in probe"). A transcript recorded against the old config
   would present exactly like this. **Untested — a hypothesis, not a finding.**

## Amended M0 exit criterion

The plan's original criterion was "`./tools/run-all-qemu-gates.sh` fully green". That was written
assuming the tree was green to begin with; it was not. Amended to:

> **27/28 gates green. `dualcore/cm4_audio_test` is known-broken: pre-existing, predating every
> change in this plan, cause unidentified, tracked separately. It must not be "fixed" by weakening
> the gate, and it must not be silently dropped from the sweep.**

Investigation was stopped here deliberately: the remaining search space is the CM4 audio pipeline,
which nothing in the RK055 display plan touches.
