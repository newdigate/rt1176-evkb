# Known-broken gates — do not run these by default

Gates listed here are **broken for reasons unrelated to whatever you are working on**. Running them
will give you a red result that is not your fault and not your bug, and chasing it will cost you
time on a subsystem you probably are not touching.

**Read this before running `./tools/run-all-qemu-gates.sh`.**

---

## `dualcore/cm4_audio_test` — BROKEN, cause unidentified

**Status:** broken. **Do not run by default.** Not a regression from any current work.

> ### ⚠ 2026-07-28 — it passed, 5 runs out of 5. Cause still unidentified; section stays.
>
> During the RK055-touch wrap this gate came up **green in the full sweep and in four further
> individual runs**, all on an otherwise idle machine (load average ~4–5). Not vacuous: it printed
> `fft_peak_bin=00000006`, `AUDIO_CM4_DET=PASS`, `codec_ack=1`, `cm7_audio_isers=0` — the four
> values it actually asserts — and exited 0 every time.
>
> **Unconfirmed hypothesis: machine load.** The 2026-07-27 bisect below ran while this machine was
> carrying load averages above 300 from unrelated builds. A starved host decouples QEMU's SAI
> timing from the audio graph, which would present exactly as "ISRs fire, dispatch runs, the FFT
> sees silence". That would make **every row of the bisect table below a false negative**, which is
> also why none of them moved the needle. This is a hypothesis, not a finding — it has not been
> tested by deliberately reloading the machine and re-running.
>
> One real difference from the committed transcript: `underruns` is now `0x2E4`–`0x2E7` (~740), not
> 0. The gate does not assert `underruns`, so it passes regardless, and the transcript's
> hardware-only `AUDIO_CM4=FAIL` line is likewise unasserted. Whether the underruns are the same
> host-starvation artefact is unknown.
>
> **The section stays until someone understands why.** The rule below is that the only way off this
> list is a fix, and nobody fixed anything — the gate changed behaviour on its own, which is a
> weaker reason to trust it, not a stronger one. Cheapest next step is now: re-run it under
> deliberate heavy load and see whether the old red reproduces.

**Symptom (as recorded 2026-07-27).** The gate reports:

```
fft_peak_bin=00000000        (expected 00000006)
AUDIO_CM4_DET=FAIL
GATE FAILED
```

The SAI path itself looks healthy in the same run — `sai_isr_count=0x400`, `dispatch_count=0x400`,
`underruns=0`, `rx_overflows=0`. ISRs fire and dispatch; the FFT simply sees silence.

The stored `transcript_qemu.txt` (committed 2026-07-22, `ffac1d6`) shows `fft_peak_bin=00000006` and
`AUDIO_CM4_DET=PASS`, so the gate did pass at some point.

### Platform

**It fails on macOS.** That is the only platform it has been run on here, so whether the failure is
macOS-specific or would reproduce elsewhere is **unknown and untested** — do not repeat "macOS bug"
as though it were diagnosed. If you ever run this tree on Linux, that single data point would be
genuinely valuable.

### What has already been ruled out — do not redo this

Bisected 2026-07-27, one variable at a time. **Every row below failed identically**, so none of them
is the cause:

| Variable | Value tested | Result |
|---|---|---|
| `cores` | `afff78a` — `BUS_CLK_ROOT` 240 MHz | FAIL |
| `cores` | `91a7efa` — `BUS_CLK_ROOT` 176 MHz | FAIL |
| `cores` | `189241c` — before any bus-clock change | FAIL |
| `qemu2` | `79a9990b39` reverted (ANADIG AI-gated STABLE + PGMC) | FAIL |
| `qemu2` | whole tree at `36d3be0e1f`, before **all 20** display commits | FAIL |

So it is **not** a bus-clock regression and **not** a QEMU-model regression. The ANADIG model is
exonerated. A hypothesis blaming `qemu2 79a9990b39` was tested and **refuted**, not assumed.

Full evidence: `docs/superpowers/plans/2026-07-27-m0-gate-sweep.md`.

### Remaining search space

1. `cores` commits from Jul 23–25 (ten, all display/PXP work — none obviously touching SAI).
2. The `Audio` or `CMSIS-DSP` sibling libraries.
3. **A stale transcript.** `ffac1d6` modified `CMakeLists.txt` *and* `transcript_qemu.txt` in the
   same commit, changing which pipeline the example builds by default ("default = working pre-arm
   pipeline; pure-CM4-PLL is the opt-in probe"). A transcript recorded against the *old* config
   would present exactly like this. **Untested — a hypothesis, not a finding.**

Cheapest next step: rebuild `cm4_audio_test` against `cores` at `ffd3547` (its last Jul-22 commit).
One build, one gate run, and it partitions the space into "cores Jul 23–25" vs "libraries or stale
transcript".

---

## Rules for this list

- **Do not delete, weaken, or `exit 0` a gate to get it off this list.** That defeats the entire
  point of having gates, and this project treats a gate that passes vacuously as a serious defect.
  The only way off this list is a fix.
- **Do not remove it from `tools/run-all-qemu-gates.sh`.** The runner should still know about it;
  this document is the convention that stops you *routinely* running it, not a mechanism that hides
  it. `./tools/run-all-qemu-gates.sh <pattern>` takes patterns, so run the gates you care about.
- **Any OTHER gate failing is a real regression** from whatever you are doing. Diagnose it; do not
  add it here to make a sweep green.
- When you fix one, delete its section and re-run the full sweep to confirm the count went up.

## Current expected sweep result

**30 gates** since `examples/display/rk055_touch_test` joined on 2026-07-28.

`30 passed, 0 failed, 0 SKIP` was the observed result on 2026-07-28 on an idle machine.
`29 passed, 1 failed, 0 SKIP` — the failure being `dualcore/cm4_audio_test` — is still an
acceptable outcome while the section above is unresolved. **Any other failure is a real
regression.** If you see more than one failure, check `uptime` before you check your diff: gates
starved of CPU fail with missing or truncated UART capture, which mimics a regression convincingly.

**Zero SKIPs matters as much as the pass count.** A `SKIP` means a missing ELF, i.e. a gate that
silently never ran — build the example and re-run rather than accepting it.
