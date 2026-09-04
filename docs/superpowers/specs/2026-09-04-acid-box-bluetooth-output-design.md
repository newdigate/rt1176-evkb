# Acid Box over Bluetooth — AudioOutputBluetooth in the acid_box capstone — Design

**Date:** 2026-09-04
**Programme:** M.2 Bluetooth A2DP (BT-3 / NEW-9 follow-up). Builds on the headset
A2DP path proven on silicon (DISCOVER fix + media batching → clean tone on a
Shokz OpenMove).
**Status:** Design, approved in brainstorming; pending spec review.

## Problem

`examples/display/acid_box` is the Acid Box capstone: an `AudioSynthAcidBass`
voice rendered through `AudioOutputI2S` (WM8962 codec) while a SynthUI runs on
the GC355/MIPI panel with GT911 touch. BT-3 proved A2DP audio out to a real
headset in a standalone tone example (`audio/bt_tone_test`,
`AudioOutputBluetooth`). This joins the two: play the acid bass on a Bluetooth
headset while the capstone's UI runs — the capstone's audio over Bluetooth.

## Goal

With an opt-in build flag, acid_box streams its synth audio to a Shokz OpenMove
headset over A2DP **at the same time** as the local WM8962 output, with the
SynthUI still rendering and responding to touch, and the tone clean (drops=0
sustained). The default build and the golden-image gate stay byte-identical.

## Non-goals

- No change to acid_box's default build, its golden gate, or its visuals.
- No A2DP sink, AVRCP, or absolute-volume; a single SBC source stream only.
- No new QEMU gate (the BT stack is bench-verified; QEMU has no GC355 or IW416
  audio path). The acceptance is silicon + ear.
- Not promoting `AudioOutputBluetooth` into a shared library (a future refactor;
  see Open questions).

## Design

### 1. `M2_BT_OUT` build flag (opt-in, default OFF)

`examples/display/acid_box/CMakeLists.txt` gains
`option(M2_BT_OUT "Stream the synth audio to a Bluetooth A2DP sink" OFF)`. When
OFF, nothing below compiles in and the ELF is byte-identical to today's — the
`run_qemu.sh` golden gate is untouched. When ON it:

- `add_definitions(-DM2_BT_OUT=1)` and the BT bench knobs the headset path needs
  (`M2_BT_UART_DNLD`, `M2_BT_WAKE_PULSE`, `M2_BT_RTS_FLOW`, `M2_BT_FAST_BAUD` +
  rate, `M2_BT_TARGET_NAME`, `M2_BT_CONNECT_RETRY`, the `M2RADIO_IW416_BT_FW`
  blob), mirroring `audio/bt_tone_test`'s CMake block. SSP (no `M2_BT_LEGACY_PIN`)
  for the headset.
- `import_evkb_library(M2Radio sdio iw416 hci bt)` — the same manifest
  bt_tone_test links.
- adds the shared BT-audio bridge source to the target:
  `../../audio/bt_tone_test/AudioOutputBluetooth.cpp` (+ its include dir), so
  there is ONE copy, not a fork.

### 2. `AudioOutputBluetooth` — an externally-clocked mode

Today `AudioOutputBluetooth::poll()` self-clocks: it calls
`AudioStream::update_all()` (paced by `micros()`) to drive the graph, because
bt_tone_test has no hardware audio clock. acid_box's graph is already clocked by
the `AudioOutputI2S` SAI DMA ISR at the exact hardware audio rate, so a second
self-clock would double-drive the graph.

Add a backward-compatible mode (one bool, default keeps today's behavior):

- `void setSelfClock(bool on);` (default `true`). acid_box calls
  `btout.setSelfClock(false)` before `begin()`.
- `poll()` gates the `update_all()` block on the flag: self-clock ON → today's
  behavior (bt_tone_test, unchanged); OFF → `poll()` ONLY drains the packetiser
  to L2CAP. `update()` (the SBC encode + ring push) still runs — it is called by
  the I2S ISR's `update_all()` walk, since `AudioOutputBluetooth` is an
  `AudioStream` node in the update chain.

bt_tone_test is unaffected (it never calls `setSelfClock`, so it self-clocks as
before; its two gates stay green). The media batching + `MediaPacketizer::pending()`
fix from the residual-drops work is inherited unchanged.

### 3. Graph wiring

acid_box keeps `acid → out` (I2S L/R) and `acid → rms`. Add, under `M2_BT_OUT`:

```
AudioOutputBluetooth btout;
AudioConnection      cBtL(acid, 0, btout, 0);
AudioConnection      cBtR(acid, 0, btout, 1);   // mono duplicated to both channels
```

`AudioMemory` may need a few more blocks for the extra fan-out (acid's output now
feeds three consumers: out, rms, btout); bump `AudioMemory(24)` to a measured
value if the pool underruns (checked on the bench via `AudioMemoryUsageMax()`).

### 4. Bring-up and loop integration (under `M2_BT_OUT`)

setup(), after the existing codec/display/touch init, runs the BT bring-up copied
from bt_tone_test: `m2ReleaseWifiReset()`, wake pulse, RTS flow, `btFirmwareDownload()`,
`hci.begin()`, HCI Reset + identity, fast-baud switch, then
`A2dpSource::connect(M2_BT_TARGET_NAME, …)` (~30 s, blocking). During it the local
WM8962 audio already plays (the SAI is running) and the panel shows the rendered
SynthUI. On success, `btout.setSelfClock(false); btout.begin(src);`.

loop() gains, under `M2_BT_OUT`, before/after the render+touch work:
`yield();` (drives the yield-attached `HciPump` that parses NCP/credits),
`src.service();` and `btout.poll();`. acid_box's own IntervalTimer "pump"
(the 1 kHz LVGL tick) is unrelated and untouched.

### 5. Coexistence (verified during design)

- acid_box references NONE of the BT pins — the M.2 reset GPIOs
  (`GPIO_AD_16`/`GPIO_AD_31` = GPIO9.15/9.30) or the wake/RTS pad
  (`GPIO_DISP_B2_13`). Console is LPUART1; BT HCI is LPUART2 (free). The RK055 is
  MIPI-DSI, not the parallel `GPIO_DISP_B2` RGB bus, so the RTS pad is free.
- `M2_BT_RTS_FLOW` toggles the 1 G ENET PHY reset (R1866); acid_box uses no ENET.
- The SBC encode runs in the audio software ISR (IRQ_SOFTWARE); the GC355
  compositor and LVGL run in the main loop. No shared-ISR conflict by design;
  the open question is total CPU headroom (below).

## Testing / acceptance

Silicon only (QEMU models neither the GC355 audio-visual path nor the IW416).

- **Acceptance:** with `-DM2_BT_OUT=ON -DM2_BT_TARGET_NAME=Shokz` + real BT
  firmware + SSP, the acid bass plays a **clean, sustained tone on the Shokz**
  (drops=0 over ≥ 2 min via a heartbeat) WHILE the SynthUI renders and a touch
  gesture changes the sound — captured in `transcript_hw_evkb.txt`.
- **No regression:** `M2_BT_OUT=OFF` default build is byte-identical; the acid_box
  golden gate (`run_qemu.sh`) and bt_tone_test's gates stay green; BT host tests
  green; `LICENSE-AUDIT: PASS`.
- **CPU-headroom check:** report `AudioMemoryUsageMax()`, the media drops/packets
  rate, and whether the render frame-rate / touch latency degrades. If the CM7
  cannot sustain both, the documented fallback is BT-only audio (mute/omit the
  WM8962 output) to free the codec DMA path — decided by measurement.

## Risks & open questions

- **CPU headroom is the real risk** and is silicon-only. Falsifiable on the first
  bench run (drops climb, or the UI stutters). Fallback: BT-only.
- **Shared `AudioOutputBluetooth` source path.** Referencing bt_tone_test's copy
  couples the two examples' layout. Acceptable now; the clean long-term home is a
  shared Arduino-facing BT-audio bridge (it can't live in M2Radio's Arduino-free
  `bt/`). A future refactor, out of scope here.
- **Bring-up UX.** The ~30 s blocking connect freezes touch during startup. Fine
  for a bench test; a non-blocking connect is a later polish, not in scope.

## File-by-file changes

- `examples/display/acid_box/CMakeLists.txt` — `option(M2_BT_OUT)` + the BT
  import/knobs/firmware block + the shared `AudioOutputBluetooth.cpp` source,
  all under the flag.
- `examples/display/acid_box/acid_box.cpp` — under `M2_BT_OUT`: the BT includes,
  objects (`A2dpSource`, transport, pump, `AudioOutputBluetooth btout`), the two
  `AudioConnection`s, the setup() bring-up, and the loop() service calls.
- `examples/audio/bt_tone_test/AudioOutputBluetooth.{h,cpp}` — `setSelfClock()`
  + gate `poll()`'s `update_all()` on it. bt_tone_test behavior unchanged.
- `examples/display/acid_box/transcript_hw_evkb.txt` — append the BT-out silicon
  evidence (kept additive; the existing capstone transcript stays).
