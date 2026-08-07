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
>
> **★ A second variable, recorded because it confounds the first: the host was REBOOTED between
> the red runs and the green ones.** Earlier in the same session `uptime` read `up 45 days` under
> load 300+; the green runs above were taken at `up 32 mins` under load ~4. So "load" and "45 days
> of uptime" changed together, and this evidence cannot separate them. A 45-day-old host
> accumulates its own pathologies — memory pressure, thermal state, leaked file descriptors in
> long-lived daemons — any of which could starve QEMU as effectively as the build did. Whoever
> tests the load hypothesis should reload the machine *without* rebooting it first, or the
> experiment answers the wrong question.

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

**2026-07-30 — gate count moved 68 → 70.** Two new LVGL RK055 gates joined the sweep
(`examples/display/lvgl_rk055_panel_test` and `examples/display/lvgl_rk055_touch_test`). The
expectation is now `70 passed, 0 failed, 0 SKIP` or `69 passed, 1 failed, 0 SKIP`, with the same
single permitted intermittent failure (`dualcore/cm4_audio_test`). The paragraphs below record the
68-gate history and still apply otherwise.

**2026-07-30 — gate count moved 70 → 71.** `examples/display/lvgl_rk055_flip_test` joined the
sweep (LVGL double buffering + page flip on vsync on the RK055). The expectation is now
`71 passed, 0 failed, 0 SKIP` or `70 passed, 1 failed, 0 SKIP`, same single permitted
intermittent failure (`dualcore/cm4_audio_test`).

**2026-07-30 — v5 (vsync ISR fence): gate count unchanged at 71.** No new gate. The flip gate
(`lvgl_rk055_flip_test`) gained a `VSYNC_ISRS=120` assertion (the flip fence is now driven by the
LCDIFv2 vsync interrupt rather than polling), and the touch gate (`lvgl_rk055_touch_test`)
migrated to double buffering — its render golden was re-recorded and came out value-unchanged.

**2026-07-30 — v6 (PXP sync copy): gate count moved 71 → 72.** `examples/display/lvgl_pxp_copy_bench`
joined the sweep. It asserts PXP-vs-CPU copy **correctness** (checksum match per rectangle case);
the timing table is hardware-only and lives in its `transcript_hw_evkb.txt`. The expectation is now
`72 passed, 0 failed, 0 SKIP` or `71 passed, 1 failed, 0 SKIP`, same single permitted intermittent
failure (`dualcore/cm4_audio_test`).

**68 gates** since `tools/run-all-qemu-gates.sh` discovery widened from `run_qemu.sh` to
`run_qemu*.sh` on 2026-07-29 (it had been sweeping 29 of them; the other 38, named for what they
test, were never run by anything) and `examples/display/rk055_touch_test` joined on 2026-07-28.

Two outcomes are acceptable while the section above is unresolved, because **`cm4_audio_test` is
intermittent, not reliably broken**:

- `67 passed, 1 failed, 0 SKIP` — the failure being `dualcore/cm4_audio_test`. Observed on
  2026-07-29 across five consecutive sweeps plus a direct run.
- `68 passed, 0 failed, 0 SKIP` — observed 2026-07-28, 5 runs out of 5, on an idle machine.

Same pinned `cores` (`2f15ff5`) in both cases, so it is not a core-version difference. That the
same gate can be consistently red one day and 5/5 green the next is itself the finding; treat the
section above as "intermittent, cause unidentified" rather than "broken".

**Any OTHER failure is a real regression.** If you see more than one failure, check `uptime` before
you check your diff: gates starved of CPU fail with missing or truncated UART capture, which mimics
a regression convincingly.

That failure mode is now *legible* rather than misleading — gates assert `gate_require_capture`
before grepping, so a starved run says `no UART capture ... QEMU produced no serial output` instead
of blaming the firmware with `banner missing`. The five thinnest-budget gates (`enet_test` at
`sleep 1`; `analog_test`, `dac_test`, `irq_attach_test`, `serial_test` at `sleep 3`) were converted
to bounded poll-for-token loops on 2026-07-29 for the same reason. That reduces the exposure; it has
not been shown to eliminate it, and the trigger is still unidentified — 2x CPU saturation did not
reproduce it.

**Zero SKIPs matters as much as the pass count.** A `SKIP` means a missing ELF, i.e. a gate that
silently never ran — build the example and re-run rather than accepting it.

**The sweep covers 67 gates, up from 29 (2026-07-29).** `run-all-qemu-gates.sh` used to discover
`run_qemu.sh` only, which silently excluded the 38 gates named for what they test
(`run_qemu_usb.sh`, `run_qemu_lwip.sh`, …) — over half the suite, never swept, so nothing was
checking whether they still passed. Discovery is now `run_qemu*.sh`. If you are comparing against
an older transcript, `28 passed` was that narrower set, not a regression.

**2026-07-31 (v7, XRGB8888):** sweep re-measured on branch `lvgl-xrgb-v7` after the
three RK055 LVGL examples migrated to XRGB8888 and the bench went dual-format
(`CASES=28`): still **72 gates** (no gate added or removed; the bench re-pinned
internally). The v7 bench's first silicon run exposed a QEMU PXP model divergence
(byte-preserving 32-bit copies vs. silicon's X:=computed-alpha-0) — fixed
stricter/more-faithful in qemu2 `0df62eb15a`; the bench oracle now asserts the
measured contract. Expectation unchanged: 72/0/0 or the documented
`cm4_audio_test` singleton, zero SKIPs.

**2026-07-31 (v8, PXP compositing):** `pxp_composite_test` joins the sweep: **72 → 73
gates**. Its oracle's blend/colorkey/ROP semantics are SILICON-MEASURED (the RM
contradicts itself twice; the transcript's RESOLVED AMBIGUITIES section is the
authority), and the QEMU AS datapath (qemu2 `4333c645ff`) was written FROM that
measurement — P2-before-P3, inverted deliberately after v7's X-byte finding.
Expectation: 73/0/0 or the documented `cm4_audio_test` singleton, zero SKIPs.

**2026-08-01 (v9, draw-unit economics):** `pxp_draw_bench` joins the sweep: **73 → 74
gates**. The milestone's verdict: draw-unit ADOPTION DECLINED — hardware showed fills
are CPU-won (write-streaming ~630 MB/s beats the PXP), and the census proved neither
LVGL scene generates image tasks, the op class where the PXP wins 12–63×. The bench +
census probe (-DDRAW_CENSUS) stand as the instrument; REVISIT when an image-heavy
scene (large images/sprites/photos) enters the tree. Expectation: 74/0/0 or the
documented `cm4_audio_test` singleton, zero SKIPs.

**2026-08-06 (USB audio input, Stage B):** `usb/usb_audio_capture_test` joins the
sweep: **74 → 75 gates**. Clean baseline re-measured on an idle machine with
`-j 2`: **75 passed, 0 failed, 0 SKIP**.

**2026-08-06 (USB audio duplex, Stage C):** `usb/usb_audio_duplex_test` joins the
sweep: **75 → 76 gates**. Measured at `-j 2`: **75 passed, 1 failed, 0 SKIP** —
the failure being the documented `dualcore/cm4_audio_test` singleton, which
compiles none of that session's sources (UAC2 parser, iTD pool, duplex example).
Expectation is therefore `76/0/0` or `75/1/0` with that one gate red, zero SKIPs
either way.

A counting trap worth one line, because it cost a puzzled minute: `-l` prints a
trailing `(N gate(s))` summary line, so `run-all-qemu-gates.sh -l | wc -l` is one
MORE than the gate count. The runner is still the right thing to ask — that
correction is above and stands — but ask it for the number it prints, not for
the number of lines it prints.

**2026-08-06 (emulated USB audio device):** `usb/usb_descriptor_survey` joins
the sweep: **76 → 77 gates**. First gate in this tree to run the USB audio
stack against an ACTUAL emulated audio device rather than an empty bus —
QEMU's `hw/usb/dev-audio.c` on the OTG2 host bus. It asserts real enumeration
(104-byte config retrieved), the interface walk, bcdADC, FORMAT_TYPE_I
(2 ch, 16-bit, 48000) and the endpoint's synchronisation type.

Two facts about that model, established by measurement and worth not
rediscovering:

- **It offers 48000 Hz only.** `USBAUDIO_SAMPLE_RATE` is a compile-time
  `#define` baked into its descriptor tables, with no property to change it.
  Every streaming example here ships asking for 44100 to match the Audio
  library, so they enumerate against it and are correctly refused by format.
  That is why the gate lives on `usb_descriptor_survey`, which never binds and
  is therefore format-agnostic.
- **Isochronous data does not flow against it.** An OUT sketch built for 48000
  enumerates, claims, selects alt 1, completes the whole control sequence with
  `ctrl=0/0/0`, prints "streaming started" — and sits at `pkts/s=0`. Cause not
  established; the leading suspicion is a split-transaction/TT mismatch, since
  siTD is for a full-speed device behind a high-speed hub and QEMU has this one
  on the root port. So this gate covers enumeration and the descriptor plane
  ONLY. Silicon remains the only proof that audio moves.

Two corrections to how this sweep gets counted and read, both learned the hard
way in that session:

**The gate count must come from the runner, not from `find`.** Discovery used to
descend into `build*` directories, where `-DEVKB_FORCE_FETCH` clones peripheral
libraries that ship gates of their own — four, under `usb_audio_graph_test/
build-fetch/_deps/` in the SPI and SdFat trees. A bare `find` returned 79, which
made the documented 74 look stale when it was exactly right, and the four extras
became permanent SKIPs for images this repo never builds. That last part is the
real harm: this file and CLAUDE.md both treat a non-zero SKIP as proof the sweep
under-reported, so a discovery bug that manufactures four of them permanently
disables the only integrity check on the count. Discovery now prunes `build*`
(`tools/run-all-qemu-gates.sh`); use `-l` to count.

**Load sensitivity is not confined to `cm4_audio_test`.** A `-j 4` sweep run
while the same machine was concurrently building examples, flashing the EVKB and
driving hardware came back `74 passed, 1 failed` — and the failure was
`dualcore/cm4_wire_int_slave_test`, never previously suspect, with
`cm4_audio_test` PASSING in that same run. The failing gate re-ran green in 1 s
on an idle machine and compiles none of the code that sweep was testing
(`err=00000004`, `WIRE_INT_SLAVE_CM4=FAIL` under load; clean idle).

So the rule "any failure other than `cm4_audio_test` is a real regression" is too
narrow as written: it is the DUAL-CORE gates as a class that are load-sensitive,
and which one shows it varies. Re-run a lone dual-core red on an idle machine
before believing it — and check whether the gate even compiles what you changed.
Neither of those excuses a red that survives both tests.

**2026-08-07 (the real stack enumerates on the CM4):**
`dualcore/cm4_usb_enum_probe` joins the sweep: **78 → 79 gates**. Phase 7.2c —
the CM4 image links the actual USBHost_t36 transport core, calls
`USBHost::begin()`, and reports the attached device's VID/PID. **No qemu2
change was needed**: 7.1's IRQ-135 split is the only model work this arc
required, so unlike 7.1 there is no red-first-then-model-change story here.

The negative control is checked in instead, as `transcript_qemu_red_vector.txt`:
vector index 151 repointed at `Default_Handler` and nothing else touched. The
CM4 reaches stage 3, takes the interrupt, and spins in the default handler — the
transcript truncates with `STAGES=FAIL` and no VID/PID. Worth keeping because it
shows the shape a misrouted vector actually has: a **hang**, not a quiet zero.
Two more notes on this gate:

- **`vid`/`pid` are the whole oracle, and only the GATE asserts them.** Every
  other token could in principle be produced by firmware talking to itself;
  `46F4:0002` is QEMU's `usb-audio` (`hw/usb/dev-audio.c:43-44`) and the CM4
  image has no knowledge of those numbers. The firmware asserts only
  `vid != 0`, deliberately, so the identical image serves a silicon run with
  whatever device is in J47 — the device-specific oracle belongs to whoever
  knows which device is attached.
- ★ **`claims` is the interrupt assertion, which is why there is no `irqcnt`
  token.** `claim()` is reachable only via `claim_drivers` ←
  `enumeration_receive` ← `followup_Transfer`, and that runs inside
  `USBHost::isr()`. There is no polled path to it.

**2026-08-07 (CM4 takes the USB port):** `dualcore/cm4_usb_irq_probe` joins the
sweep: **77 → 78 gates**. Phase 7.1 — the CM4 self-configures LPCG115, the
USBPHY2 480 MHz PLL, EHCI reset, host mode and port power, then takes USB
OTG2's **IRQ 135** on its own NVIC. HW-verified on the EVKB across two runs.
Needed a qemu2 change (`7c9bd4cbe6`, local-only per the GPL firewall): a
`TYPE_SPLIT_IRQ` fanning 135 to both NVICs, since the model wired both USB
IRQs to the CM7 only. Run **red-first**, with `transcript_qemu_red.txt`
checked in before that change.

Three things about this gate worth not rediscovering:

- **`PHY_PLL_CM4` is a world-split token: FAIL in QEMU, PASS on silicon, and
  the gate asserts only that the token is PRESENT.** qemu2 instantiates stock
  `TYPE_IMX_USBPHY`, an i.MX6-era model whose register file ends at
  `USBPHY_VERSION` (0x80); the RT1176's `PLL_SIC` quad at 0xA0/A4/A8/AC is
  unmodelled, so the lock poll cannot succeed for *either* core. Pre-existing
  and already recorded in HW-verified firmware — `USBHost_t36/ehci.cpp:248`
  literally reads "QEMU: PLL_SIC reads 0 -> times out, proceeds". Do not
  "fix" this by asserting lock in the gate.

- ★★**The device must be HOTPLUGGED, not present at reset — and the first two
  attempts at this gate were VACUOUS in two different ways.** ChipIdea defers
  attach to the guest's PP write (`chipidea.c:230` → `hcd-ehci.c:1066`), so a
  present-from-reset device raises PCD during the firmware's *stage 5* and
  the stage-6 "clear stale status" W1C wipes it before PCE is armed. The
  first red therefore had `irqcnt=0` with `stsraw=0` — no interrupt condition
  ever occurred while anything was listening, so the red said nothing about
  routing and the qemu2 split would not have flipped it. The second attempt
  polled for the firmware's `s6` marker before hotplugging — but the CM7 was
  batching all its mailbox reads before printing, so `s6` only reached the
  UART *after* the window closed and the plug again landed too late, with the
  identical symptom. The gate now streams each token as it arrives and
  hotplugs on `s6`. **`irqcnt=0` is the legitimate negative result here, so
  almost every possible mistake wears its costume** — that is why the gate
  asserts `PLUG_TRANSITION` (a real ccs 0→1 edge) *before* it asserts the
  interrupt, and why the firmware reports a post-window raw `USBSTS`.

- **A delay loop calibrated by counting instructions was off by ~2.3×.** The
  CM4's observation window was written as a `volatile` decrement loop with a
  "~3 cycles/iter" estimate; it is nearer 7 (LDR+SUBS+STR+CMP+B), so a
  nominal 8 s window ran ~18.7 s and collided with the CM7's receive timeout.
  The transcript truncated mid-sequence, which reads as a CM4 hang rather
  than a miscalibrated delay. Delays are now measured with DWT CYCCNT.
