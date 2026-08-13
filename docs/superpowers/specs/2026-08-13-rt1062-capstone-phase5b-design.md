# RT1062 Capstone (Phase 5b) — Design

**Goal.** USB audio IN → AudioStream graph → WM8960 line out on the
MIMXRT1060-EVKB, both legs in one graph, proven by a loopback-cable round trip
you can hear: the tone the firmware plays out over USB comes back through the
adapter's ADC and leaves the board's own codec.

**Predecessors:** Phase 5a (`2026-08-08-rt1062-audio-phase5a-design.md`) put the
graph→WM8960 leg on this board; Phase 4 put USB audio OUT on it; Stage B/C
(`2026-08-05-uac-host-input-and-duplex-design.md`) proved USB audio capture on
rt1176 silicon. This phase joins them, per the board-axis design's Phase 5:
"the RT1060-EVKB can do something the RT1176 capstone could not: it has a
working on-board codec on the same board as the USB host port."

---

## 1. Scope

| Decision | Choice | Rejected |
|---|---|---|
| Graph shape | **Both legs in the graph** (sine→USB OUT and USB IN→I2S) | driver-tone OUT leg; rate-servoed duplex (Stage C's declared non-goal) |
| Capstone boards | **rt1062 only** (`boards` sidecar names just rt1062) | two-board (the 1170 has its own capstone, `dualcore/cm4_graph_usb_capstone`) |
| QEMU story | **Capstone gets a control-plane gate; `sai1-rxinject` lands via a two-board port of `audio/audioinput_i2s_test`** | capstone-only (leaves the 5a rxinject promise unmet); teaching QEMU usb-audio to capture (a device model project for hardware that exists nowhere on the bench) |
| Silicon bar | **Loopback cable**: adapter headphone-out → its own mic-in | ambient-mic monitoring (weaker evidence, kept as an optional extra) |

---

## 2. The one new component: `AudioInputUSBHost`

USB capture has never fed the AudioStream graph — Stage C's echo mode drained
the FIFO by hand in the sketch. The node lives in the Audio fork
(`input_usbhost.{h,cpp}`), mirroring `output_usbhost`'s structure, and wraps
the capture API the duplex example already uses: `USBAudioOut::read(buf,
frames)`, which yields normalized stereo int16 regardless of wire format
(`uac_pack16` handles the 1ch/16 geometry underneath).

`update()` runs at the graph rate (the I2S clock): when ≥128 frames are queued
it emits L/R blocks — mono capture fans to both channels, as echo mode already
does — otherwise it emits silence and counts an underrun. Overrun is the
driver FIFO's problem and is already bounded there. The adapter's crystal is
−86 ppm against the graph clock (measured five independent times in this
tree), which surfaces here as ~4 frames/s of drift; the underrun counter makes
it visible in the heartbeat instead of mysterious. No rate servo: an
occasional silence block (~one per half minute) is audible as at most a tick,
and Stage C explicitly declared cross-crystal ASRC a different project.

## 3. The capstone example

`examples/usb/usb_audio_capstone_test`, rt1062-only. Graph:

```
sine (1 kHz) ──► AudioOutputUSBHost          (OUT leg: USB to the adapter)
AudioInputUSBHost ──► AudioAnalyzePeak       (IN leg: what came back)
                 └──► AudioOutputI2S ──► WM8960 line out
```

Heartbeat once per second: out/in packet counters, `in_peak`, the OUT node's
`dropped()`, the IN node's underruns, EHCI error flags. STAGE tokens for the
gate.

### 3.1 Clock ownership — by declaration order, zero driver surgery

Exactly one node owns `update_responsibility`. **`AudioOutputI2S` is declared
first and claims it** (5a proved that clocking works on this board);
`AudioOutputUSBHost`'s pacing already self-disables when it does not own the
clock — `frame_consumed()` opens with `if (!update_responsibility ...)
return;` — so it degrades to a graph-paced FIFO writer with no code change.
Its existing `dropped()` counter then records the −86 ppm drift resync: the
FIFO gains ~7.6 samples/s against ~3300 samples of headroom above target
(`USB_AUDIO_FIFO_SAMPLES` 4096 − `FIFO_TARGET_SAMPLES` 768), one counted drop
roughly every seven minutes. The declaration order is load-bearing
and carries a comment saying exactly that; this is also the first real
exercise of the non-owner path, which is named in the risks.

### 3.2 The capstone's QEMU gate — control plane, honestly

QEMU's `usb-audio` is playback-only (`audio_be_open_out` and nothing else) and
iso data does not flow against it at all (Phase 4's measurement), so the round
trip is silicon-only evidence. The gate asserts what QEMU reaches:

- enumeration of `46F4:0002` and alt selection on the OUT interface,
- zero EHCI error flags (`xact_err=0 babble=0 buf_err=0`),
- graph liveness via console tokens, and
- **the sai1-tap accumulating at rate with peak exactly 0** — real silence
  pushed by a live graph. An empty tap means the graph is dead; a growing
  all-zero tap means the graph runs and no capture data exists, which is the
  correct QEMU outcome. This inverts 5a's `check_tap.py` assertion, so the
  checker grows an `--expect-silence` mode rather than a second script.

The gate header states the limitation the same way 5a's does: the tap proves
plumbing, the loopback transcript proves audio.

All per-run artifacts go through `gate_capture_path` (mandatory since the
`-j 2` collision fix), and the reap wait follows the audiooutput gate's
dual-liveness poll pattern, not a fixed sleep.

## 4. The `sai1-rxinject` promise: port `audio/audioinput_i2s_test`

5a deferred the RX-side injector "against its own gate"; this is that gate.
The example goes two-board by the 5a recipe — CONSOLE alias (it uses bare
`Serial1` today), `BOARD_CODEC_T` guard (it constructs `AudioControlWM8962`),
unguarded `TEENSY_VERSION` fixed, codec-per-board CMake, verbatim toolchain
copy, `boards` sidecar — plus its gate moves to `gate_capture_path` and
`gate_console` in the same pass.

qemu2 (LOCAL-ONLY, never pushed) gets the `sai1-rxinject` binding on
`fsl-imxrt1062`, mirroring `fsl-imxrt1170.c`'s existing block the same way
5a's Task 1 mirrored the tap. The rt1176 gate's mechanism — `gen_inject.py`
writes known samples, a FIFO pumps them into the chardev, the firmware asserts
`AudioInputI2S` sees the expected peak — is machine-agnostic and needs only
the console/capture-path changes.

★ Consequence, same as the tap: **a fresh clone sees the rt1062 half red** —
the GPL firewall working, recorded in `KNOWN-BROKEN-GATES.md`.

## 5. Silicon

Bench: adapter in J47, 3.5 mm male-to-male from its headphone-out to its own
mic-in, speaker on the WM8960 line-out. The bar: **the 1 kHz tone is audible
from the WM8960**, having traversed USB OUT → adapter DAC → cable → adapter
ADC → USB IN → graph → I2S. The transcript records the audible tone, `in_peak`
in band, and the drift counters (`dropped()`, underruns) over a multi-minute
soak — they should tick at single-digit counts, not run away.

Known unknowns at the bench, mitigations in hand: the adapter's mic input may
apply AGC or expect electret bias, shifting `in_peak` — the heartbeat prints
it, and the OUT tone amplitude is adjustable; if the loopback level is
hopeless, the fallback bar is ambient-mic monitoring, recorded as such.

## 6. Close-out

Sweep **87 → 89** (audioinput's rt1062 half, the capstone's rt1062 gate), zero
SKIP. `tools/license-audit.sh` GATES gains **two** build-directory entries —
verified by name; the drift check cannot see them. `CLAUDE.md` count and
parenthetical updated; `KNOWN-BROKEN-GATES.md` entry covering the two new
fresh-clone-red halves and what the capstone gate does and does not prove.

---

## 7. Risks

| Risk | Handling |
|---|---|
| First consumer of capture-FIFO-into-graph; underrun behavior unproven | Counters in the heartbeat; QEMU gate proves the silence path; silicon soak watches the counters |
| `AudioOutputUSBHost` non-owner mode is guarded-by-code but never exercised | The capstone is its first test; `dropped()` staying at drift-rate (not runaway) is the check, in QEMU and on silicon |
| Adapter mic AGC/bias skews loopback level | `in_peak` printed; tone amplitude adjustable; ambient-mic fallback documented |
| Declaration order silently decides the clock owner | Comment at the declarations; heartbeat exposes the symptom (runaway `dropped()`) if it regresses |
| The `--expect-silence` tap mode passes vacuously | It requires the tap to GROW at rate; an empty tap fails. Negative-test it the way the 5a poll fix was: fake QEMU feeding a nonzero tap must fail it |

## 8. Definition of done

- [ ] `AudioInputUSBHost` in the Audio fork, underrun/overrun counted
- [ ] Capstone example builds (rt1062), both legs in one graph, I2S owns the clock
- [ ] Capstone QEMU gate green: control plane + zero errors + silent-tap-at-rate
- [ ] `audioinput_i2s_test` builds and gates on both boards; rxinject bound on `fsl-imxrt1062` (LOCAL-ONLY)
- [ ] Loopback tone audible from the WM8960; transcript with `in_peak` and drift counters committed
- [ ] Sweep 89, zero SKIP; `LICENSE-AUDIT: PASS` with both new build dirs walked
- [ ] `CLAUDE.md`, `KNOWN-BROKEN-GATES.md` updated; Audio-fork pin bumped

## 9. Not in this phase

Rate servo / ASRC, feedback-endpoint reading on the host side, teaching QEMU
usb-audio to capture, a two-board capstone, the other ten `examples/audio/*`
on rt1062, and any frequency-domain tap assertion.
