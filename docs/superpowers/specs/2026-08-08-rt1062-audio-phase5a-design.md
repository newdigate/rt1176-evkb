# RT1062 Audio Output (Phase 5a) — Design

**Goal.** `examples/audio/audiooutput_i2s_test` builds and gates on both boards,
and an audible 1 kHz tone comes out of the MIMXRT1060-EVKB's WM8960 codec.

**This is the first time the Audio library compiles for `__IMXRT1062__` in this
tree.** All twelve `examples/audio/*` are rt1176-only today.

**Predecessor:** Phase 4 (`2026-08-08-rt1062-usb-audio-phase4-design.md`) put USB
host audio on this board. Phase 5b, the capstone, sits on top of both.

---

## 1. Why this is its own phase

The board-axis design's Phase 5 is a capstone: *USB audio IN → AudioStream graph
→ WM8960 line out*. That is **two subsystems**, and it was split for two reasons.

**Isolation.** No part of the Audio stack — `AudioStream`, `output_i2s`, any
codec driver — has ever been built or gated for rt1062. Bringing it up inside a
four-subsystem capstone means any failure presents as "no sound" with five
candidate causes and no way to bisect. A tone example isolates it for a few
minutes' work.

**A physical problem with the capstone as written.** USB audio IN needs a signal
*source*. With nothing plugged into the adapter's microphone jack, the graph
faithfully renders silence and the speaker proves nothing. 5b has to solve that
— a loopback cable from the adapter's output to its own input is the likely
answer, and is the kind of un-fakeable assertion this tree prefers — but it is
5b's problem, not this phase's.

---

## 2. Scope

| Decision | Choice | Rejected |
|---|---|---|
| Split | **Audio path first, capstone second** | one phase; capstone-first spike |
| Example | **Port `audiooutput_i2s_test`** | new rt1062-only example; port `tone_test` |
| QEMU tap | **Port `sai1-tap` only** | port `sai1-rxinject` too; no tap on rt1062 |

`audiooutput_i2s_test` is the only audio example with the full
`AudioSynthWaveformSine → AudioOutputI2S → codec` path **and** an existing gate.
`tone_test` and `audiostream_test` are graph-only — they touch neither I2S nor a
codec, so they would prove nothing about the thing this phase exists to prove.

A new rt1062-only example was rejected: it would duplicate an existing one and
break the board-axis pattern, where the same example runs on both boards and the
differences live behind guards.

`sai1-rxinject` is deliberately deferred. It is the RX-side mirror of the tap and
this phase has no input path; landing an ungated model feature is what this
tree's discipline argues against. 5b adds it against its own gate.

---

## 3. What actually differs between the boards

Three things, and the third is an asymmetry in the *helpful* direction.

**The codec is a different chip.** The MIMXRT1170-EVKB has a **WM8962**; the
MIMXRT1060-EVKB has a **WM8960**. Different parts, different register maps,
different drivers — `control_wm8962.cpp` and `control_wm8960.cpp`, both already
present in the Audio fork.

**The I2C bus differs, and needs no guard.** The 1170's codec is on LPI2C5, the
1060's on LPI2C1, both at address `0x1A`. This is already handled *inside* the
drivers: `control_wm8960.cpp` uses `Wire` (LPI2C1 on the teensy4 core),
`control_wm8962.cpp` uses `Wire2` (LPI2C5). Swapping the class swaps the bus.

**The QEMU models are not equally faithful.** `mimxrt1060-evk` instantiates a
real `TYPE_WM8960` model; `mimxrt1170-evk` has a stub that, in its own words,
"ACKs writes and returns 0 on reads — it does not model codec registers". So the
rt1062 half may eventually be able to assert codec configuration the rt1176 half
structurally cannot. **Not pursued in this phase** — recorded because it is the
opposite of the usual direction and someone will otherwise assume parity.

---

## 4. Changes

### 4.1 Firmware — `examples/audio/audiooutput_i2s_test/`

| Change | Why |
|---|---|
| `CONSOLE` alias | `Serial1` on imxrt1176, `Serial6` on teensy4 — LPUART1 on both. Identical block to the four existing two-board examples. On the EVKB, `Serial1` is LPUART6 and reaches only header pins D0/D1, not the DAPLink VCOM. |
| Codec guard | `AudioControlWM8962` → `AudioControlWM8960` under `ARDUINO_MIMXRT1060_EVKB`, with the matching `control_wm89xx.h` include. |
| `TEENSY_VERSION` guard | Unguarded it caches 117 and silently builds an **RT1176** image into `build-rt1062/`, which boots the wrong machine and fails looking like a board problem. |
| `CMakeLists.txt` | Compile `control_wm8960.cpp` for rt1062 in place of `control_wm8962.cpp`. |
| New `toolchain/rt1062-evkb.toolchain.cmake` | Verbatim copy; same directory depth as the existing examples. |
| New `boards` (`rt1176`, `rt1062`) | Declares the gate on both boards. |

### 4.2 qemu2 — LOCAL-ONLY

Bind the `sai1-tap` chardev to SAI1 in `hw/arm/fsl-imxrt1062.c`, mirroring
`hw/arm/fsl-imxrt1170.c:1239-1250` (`qemu_chr_find("sai1-tap")`). The tap is a
property of the shared `TYPE_IMXRT_SAI` model
(`DEFINE_PROP_CHR("tap", IMXRTSAIState, tap)`) and the 1060 machine already
instantiates that model — only the *binding* is missing. Without it the tap file
is empty and `check_tap.py` fails.

★ Consequence, exactly as for `usb_descriptor_survey` and
`dualcore/cm4_usb_irq_probe`: **a fresh clone sees the rt1062 half red.** That is
the GPL firewall working. Document it in `KNOWN-BROKEN-GATES.md`; do not work
around it.

### 4.3 The gate

One change: `-serial file:"$VCOM"` → `$(gate_console "$VCOM")`. The tap chardev,
`check_tap.py`, and both existing assertions work unchanged once the binding
exists.

It asserts `STAGE_SYNTH` (synth peak in 0.40–0.60) and `STAGE_TONE`.

★ **`STAGE_TONE` is amplitude-only.** `check_tap.py` computes `peak > 4000` over
the raw int16 tap; there is no frequency analysis. So it proves *non-silent
samples reached SAI1 TDR* — not that they form a 1 kHz tone, and not the sample
rate. That belongs in the gate header, not only here.

Related and harmless: `mimxrt1060-evk` passes `sai_sample_rate = 0` ("leave the
SAI default", 48 kHz) while the Audio library is 44100. This does **not** affect
the gate — the tap mirrors raw TDR writes independently of the audio backend, and
the assertion is amplitude-only. It will matter in 5b.

★ **Known, and deliberately NOT changed here: this gate uses `sleep 5`.** That is
the fixed-sleep pattern CLAUDE.md documents as load-sensitive — the `sleep 3` in
`serial_test` produced reds that said nothing about the firmware and passed on
retry, repeatedly, during the 2026-07-29 sweep. Adding a second board doubles
this gate's exposure to it.

It is left alone because the obvious fix does not apply. The sibling gates poll
for a terminal UART token, but here the console token (`STAGE_SYNTH`) arrives
*before* the thing the sleep is actually waiting for: the **tap file** has to
accumulate enough samples for `check_tap.py`. Polling `STAGE_SYNTH` would reap
earlier than `sleep 5` does and could empty the tap — trading a load-sensitive
gate for a load-sensitive gate that fails in a more confusing place.

The correct fix is to poll on the tap reaching a sample count, which is a real
change to a gate this phase is otherwise only touching one line of. **If the
rt1062 half flakes under `-j 2`, that is the fix** — not a longer sleep.

### 4.4 Silicon

Audible 1 kHz tone from the WM8960 line out on the MIMXRT1060-EVKB, with a
committed `transcript_hw_evkb.txt`.

### 4.5 Close-out

`build-rt1062` added to `tools/license-audit.sh` `GATES` (verify **by name** —
the audit's drift check matches on the example directory and is blind to a
missing build-directory entry). Sweep **86 → 87**, zero SKIP. `CLAUDE.md` and
`docs/KNOWN-BROKEN-GATES.md` updated.

---

## 5. Risks

| Risk | Handling |
|---|---|
| **The Audio block pool lives in DMAMEM, which Phase 3 left uncached board-wide.** This phase is its first real exercise. | Phase 4 measured `pkts/s=1000` sustained with uncached DMAMEM, so the cost is probably nil — but that was the USB ring, not the audio graph, and the graph touches far more of that memory. Detectors: `STAGE_SYNTH` in band, and a clean tone on silicon. If it starves, the fix is a dedicated non-cached section, **not** reverting the uncached mapping. |
| The Audio library hides an RT1176 assumption | This is its first compile for `__IMXRT1062__`, which is what surfaces one. `output_i2s.cpp` carries 7 `__IMXRT1062__` guards against 3 for `__IMXRT1176__`, so upstream support exists — it has simply never been built in this tree. If one appears it belongs in the library, not the example. |
| `STAGE_TONE` passes while the tone is wrong | Accepted and documented: the assertion is amplitude-only by construction. Silicon is what proves audibility, which is why 4.4 exists. |
| A fresh clone sees the rt1062 gate red | Same GPL-firewall situation as two existing gates. Record it; do not work around it. |

---

## 6. Definition of done

- [ ] `audiooutput_i2s_test` builds for both boards; rt1062 entry point `0x60001000`
- [ ] `sai1-tap` bound on `fsl-imxrt1062`; rt1062 tap file non-empty
- [ ] Gate green on both boards, asserting `STAGE_SYNTH` and `STAGE_TONE`
- [ ] Audible 1 kHz tone from the WM8960 on silicon
- [ ] `transcript_hw_evkb.txt` committed
- [ ] Sweep 87 gates, zero SKIP
- [ ] `LICENSE-AUDIT: PASS` with `build-rt1062` walked
- [ ] `CLAUDE.md` and `KNOWN-BROKEN-GATES.md` updated

## 7. Not in this phase

USB audio IN, the 5b capstone graph, `sai1-rxinject`, any frequency-domain
assertion on the tap, and the other eleven `examples/audio/*`.
