# RT1176 AudioPlaySdWav (SD → J101 capstone) — Design

**Status:** approved (design), ready for implementation plan
**Date:** 2026-07-08

## Goal

Bring up Teensy Audio's **`AudioPlaySdWav`** node on the RT1176 core — play a WAV file off the SD card, through the AudioStream graph, out the SAI1 → WM8962 → **J101 headphone jack**. This is the capstone that ties together three already-done, HW-verified subsystems: the **SD stack** ([[rt1176-sd-usdhc]]), the **AudioStream graph + AudioOutputI2S** ([[rt1176-audiostream]], [[rt1176-audiooutput-i2s]]), and the **WM8962 codec** ([[rt1176-wm8962-consolidation]]). The satisfying end-to-end demo: audible music/tone out J101, sourced from a `.wav` on a real µSD.

**The node ports with essentially zero code changes** — `play_sd_wav.{h,cpp}` is platform-generic Teensy code (no `__IMXRT...` branches) that relies entirely on the `AudioStream` API + `SD.h`, both already provided. The work is: **one core one-liner** (`NVIC_IS_ENABLED`), a **QEMU gate** that proves the played audio is byte-exact, and **HW verification** (listen on J101).

## Why this shape (exploration findings)

- **`AudioPlaySdWav` is platform-generic — verbatim port.** `class AudioPlaySdWav : public AudioStream` (`AudioStream(0, NULL)` = 0 inputs, 2 outputs via `transmit(block,0/1)`). No `__IMXRT.../KINETISK` guards anywhere (only `HAS_KINETIS_SDHC`, which stays off). It plays **16-bit PCM WAV at exactly 44100 Hz** (mono or stereo) — the 8-bit and non-44.1k (`STATE_CONVERT_*`) states are `return false` stubs, and `WAVE_FORMAT_EXTENSIBLE` is rejected. This matches the core's `AUDIO_SAMPLE_RATE_EXACT = 44100.0f` and the HW-verified 44.1 kHz SAI1/WM8962 output path exactly.
- **SD reads are fully synchronous/blocking inside `update()` — no `EventResponder`.** One flat `uint8_t buffer[512]`, refilled by `wavfile.read(buffer, 512)` (`play_sd_wav.cpp:174`) only when drained. `update()` runs from `software_isr()` at `IRQ_SOFTWARE`(44) NVIC priority **208** (low, preemptible). A 512-byte PIO read at the verified 25 MHz/4-bit SD clock is ~40 µs vs the ~2.9 ms block budget (128 samples @ 44.1 kHz) — comfortable, and *exactly* how stock Teensy does it. An overrun = audible glitch, not a crash. **So a straight port is architecturally faithful; there is no async path to preserve.**
- **`SD.h` surface is an exact match.** The node calls only `SD.open()`, `File::operator bool`, `available()`, `read(buf,512)`, `close()` — **no `seek`/`position`** (`positionMillis`/`lengthMillis` are pure byte-counter arithmetic). All provided by the new RT1176 `SD.h` ([[rt1176-sd-usdhc]]). No `File` additions needed.
- **Two compile-blocking gaps** (recon-confirmed, both trivial): (1) **`NVIC_IS_ENABLED` is missing from the core** — the node's `play()`/`stop()` call it (`play_sd_wav.cpp:68,100`); `NVIC_ENABLE/DISABLE_IRQ` exist but not `IS_ENABLED`. (2) **SPI must link** — the node includes `spi_interrupt.h` → `AudioStartUsingSPI()`/`StopUsingSPI()` unconditionally (`HAS_KINETIS_SDHC` off), needing the global `SPI` object (`SPI.usingInterrupt(IRQ_SOFTWARE)`). `newdigate/SPI` provides it; the gate links it (as `sd_fs_test` already does). These calls are harmless no-op bookkeeping on RT1176 (SD is SDIO, nothing contends the SPI bus).
- **Node slots in with no registration step** — the `AudioStream` constructor self-registers into `first_update`; wiring is ordinary `AudioConnection(playWav,0,out,0)` + `(playWav,1,out,1)`. `Audio.h` already `#include`s `play_sd_wav.h`. The evkb audio gates **cherry-pick** Audio `.cpp`s via `target_sources` (they do NOT glob the fork), so the gate must list `play_sd_wav.cpp` explicitly.
- **QEMU verification composes cleanly.** `hw/audio/imxrt_sai.c` mirrors every `SAI1_TDR0` write to a chardev tap (`qemu_chr_fe_write_all`, 2 LE bytes/sample), bound for SAI1 in `fsl-imxrt1170.c` when `-chardev id=sai1-tap` is present. `-drive if=sd,...` attaches the card to USDHC1 (`mimxrt1170-evk.c`). Both are independent subsystems → **one `qrun` can attach a WAV-bearing SD image AND capture the SAI1 tap.** The `audiooutput_i2s_test` gate already uses this tap (`check_tap.py`, peak-based); we extend it to sample-exact. **qemu2 is UNCHANGED** (tap + SD attach already exist).
- **`audio_block_t` pool = DMAMEM/OCRAM**; `AudioMemory(30)` is the tested precedent for the identical `playWav→i2s` graph (the 1060-EVKB example). Test WAV assets exist (`~/Development/sampler/*.wav`, teensy `SDTEST*.wav`, all 44.1k/16-bit); python's `wave` module generates canonical PCM deterministically (`sox` is broken on this machine).

## Scope

**In scope:**
- **Core:** add `NVIC_IS_ENABLED` to `tools/gen_imxrt1176_h.py` + regenerate `imxrt1176.h` (uses the existing `NVIC_ISER`).
- **`newdigate/Audio`:** build `play_sd_wav.{h,cpp}` as-is (verbatim; no branch). Optional cosmetic fix of the stale `control_wm8962.cpp:6` comment (references the deleted core `wm8962.{h,cpp}`).
- **Gate `evkb/sd_wav_play_test`:** firmware (`AudioPlaySdWav → AudioOutputI2S` + `AudioControlWM8962`, `AudioMemory(30)`, `SD.begin` + `play("TEST.WAV")`), CMake (imports cores/SdFat/SD/SPI/Wire + `target_sources` for `play_sd_wav.cpp`, `output_i2s.cpp`, `control_wm8962.cpp`, `memcpy_audio.S`), runner (mono + stereo FAT card images, `qrun` with `-drive if=sd` + `-chardev sai1-tap`), and `check_tap.py` doing **sample-exact** tap-vs-WAV comparison with pipeline-latency alignment.
- **Test WAVs:** generated deterministically via python `wave` — a mono sine + a stereo sine with **distinct L/R** frequencies (channel-swap detectable). Committed as gate assets.
- **HW verification:** flash, real µSD with a 16-bit/44.1k WAV, audible playback out J101 + VCOM markers.

**Explicitly deferred (YAGNI):**
- **8-bit PCM, non-44100 sample rates, resampling** — the node stubs these (`return false`); do NOT implement. The gate uses 16-bit/44.1k only.
- **`WAVE_FORMAT_EXTENSIBLE`** — rejected by the node; test WAVs must be canonical `WAVE_FORMAT_PCM`.
- **MP3/AAC/FLAC** — separate Teensy nodes, out of scope.
- **Async / double-buffered SD reads** — the node is synchronous by design; only revisit if HW shows audio glitches (it won't at ~40 µs/2.9 ms).
- **`pause`/`togglePlayPause` gate coverage** — the API exists; not gate-tested (optional smoke only).
- **Multi-file playlists, seeking, scrubbing** — not part of the capstone.

## Architecture — components

### 1. Core: `NVIC_IS_ENABLED` (`imxrt1176.h` + `tools/gen_imxrt1176_h.py`)
`imxrt1176.h` is auto-generated → add to **both**. Next to the existing NVIC helper block (the generator emits `NVIC_ISER(n)`, `NVIC_ENABLE_IRQ`, etc.), add:
```c
#define NVIC_IS_ENABLED(n) (NVIC_ISER((n) >> 5) & (1u << ((n) & 31)))
```
Mirrors `teensy4/imxrt.h:10265` semantics using this core's `NVIC_ISER`. This is the only core change and the only compile gap in the node.

### 2. `newdigate/Audio`: `play_sd_wav.{h,cpp}` (verbatim)
Built into the gate as-is — no `__IMXRT1176__` branch. It subclasses the ported `AudioStream`, uses `allocate/release/transmit`, `SD.open/read/available/close`, and the NVIC macros (now complete). The stereo deinterleave + buffer-boundary L/R carry (`consume()`, the `header[0]`/`leftover_bytes` stash) is historically fragile — port/build byte-for-byte, do not "simplify." (Optional: fix the stale `control_wm8962.cpp:6` doc comment while here.)

### 3. Gate firmware (`evkb/sd_wav_play_test/sd_wav_play_test.cpp`)
```
AudioPlaySdWav   playWav;
AudioOutputI2S   out;
AudioControlWM8962 wm;                 // HW fidelity (I2C/Wire2); tap captures TDR0 regardless
AudioConnection  cL(playWav, 0, out, 0);
AudioConnection  cR(playWav, 1, out, 1);
setup(): Serial1.begin(115200); AudioMemory(30); wm.enable();
  SD.begin(BUILTIN_SDCARD)      -> SD_WAV_MOUNT=PASS/FAIL
  playWav.play("TEST.WAV")      -> SD_WAV_PLAY=PASS (play() true)
  spin while playWav.isPlaying() (bounded) -> SD_WAV_DONE=PASS (+ positionMillis/lengthMillis)
```
The graph is clocked by the SAI1 TX DMA ISR (`update_all` on DMA half-complete); the firmware just keeps the CPU alive while the ISR-driven graph + blocking SD reads run. Markers over Serial1 (LPUART1 VCOM).

### 4. Gate CMake + runner (`evkb/sd_wav_play_test/`)
- **CMakeLists**: `import_arduino_library(cores …/cores/imxrt1176)`, `Wire` (WM8962 uses Wire2), `SdFat`, `SD` (PaulS_SD), `SPI`; `target_sources(…elf PRIVATE play_sd_wav.cpp output_i2s.cpp control_wm8962.cpp memcpy_audio.S)` with the `.S` `LANGUAGE C -x assembler-with-cpp` handling; `target_include_directories(… $HOME/Development/Audio)`. Model on `audiooutput_i2s_test/CMakeLists.txt` + `sd_fs_test`'s SD imports.
- **Runner** (`run_qemu_sd_wav.sh`): for each of `{mono, stereo}` — build a **512 MB MBR/FAT16** `card.img` and seed the WAV as `TEST.WAV` (`hdiutil attach -nomount … CRawDiskImage` → `diskutil partitionDisk … MBR "MS-DOS FAT16"` → `hdiutil attach` (mount) → `cp <wav> /Volumes/…/TEST.WAV` → `hdiutil detach`; the `sd_fs_test` recipe). Run `qrun -M mimxrt1170-evk -global fsl-imxrt1170.boot-xip=on -kernel <elf> -serial file:<vcom> -drive if=sd,format=raw,file=<card.img> -chardev file,id=sai1-tap,path=<tap> -d guest_errors,unimp -D <dbg>`. Grep VCOM markers, then `python3 check_tap.py <tap> <wav>`. Both WAVs pass → `SD_WAV_ALL=PASS`.
- **`check_tap.py`**: parse the WAV `data` chunk → reference int16 samples; read `tap.raw` as interleaved LE int16 (`SAI1_TDR0` L,R,L,R,…); **find the fixed pipeline-latency offset** (skip leading boot/header-parse silence — e.g. locate the first tap frame matching the WAV start, or cross-correlate); then compare **sample-exact**: mono asserts `tap.L == tap.R == wav[i]`; stereo asserts `tap.L == wav_L[i]` and `tap.R == wav_R[i]` for all i. Report `WAV_SAMPLES_EXACT=PASS` + a mismatch count.

### 5. Test WAVs (`evkb/sd_wav_play_test/assets/`)
Generated once via python `wave` (committed): `mono.wav` (16-bit/44.1k, a short distinctive sine, ~0.25 s) and `stereo.wav` (16-bit/44.1k, **L ≠ R** — e.g. L=440 Hz, R=880 Hz — so a channel swap fails the sample-exact check). Canonical `WAVE_FORMAT_PCM` (python `wave` produces exactly that; not Extensible).

## Data flow / behavior

`playWav.play("TEST.WAV")` → `SD.open` → header parse (RIFF/fmt/data, resolves `state_play` = 16-bit mono/stereo @44.1k). Each audio block (128 samples, ~2.9 ms), the SAI1 TX DMA ISR fires `update_all()` → `software_isr()` → `AudioPlaySdWav::update()`: if its 512-byte buffer is drained, `wavfile.read(buffer,512)` (blocking PIO, ~40 µs); `consume()` converts PCM → `audio_block_t` (mono → both outputs; stereo → deinterleave L→0, R→1) and `transmit()`s. `AudioOutputI2S::update()` receives the blocks, its ISR interleaves them into `i2s_tx_buffer` (DMAMEM/OCRAM) → SAI1 TX DMA → `SAI1_TDR0` → WM8962 DAC → J101. In QEMU, every `TDR0` write is mirrored to the `sai1-tap` chardev; on silicon it's audible.

## Testing

**QEMU gate** `sd_wav_play_test` (qemu2 unchanged — SAI1 tap + SD attach already modeled):
- Firmware: `SD_WAV_MOUNT`/`SD_WAV_PLAY`/`SD_WAV_DONE=PASS`.
- Host `check_tap.py`: **sample-exact** tap-vs-WAV for a **mono** run and a **stereo** run (distinct L/R). Proves the exact WAV PCM reaches `SAI1_TDR0` — catching byte-order, deinterleave, and channel-swap bugs, not just non-silence. `SD_WAV_ALL=PASS` requires both.

**Hardware (final arbiter — the capstone payoff):** LinkServer flash + VCOM @115200, a real µSD holding a 16-bit/44.1k WAV. **Audible playback out J101** (user confirms the sound), plus the VCOM markers. This is the satisfying end-to-end demo the whole SD + audio effort was building toward.

**Contingency:** if the gate stalls, `-d unimp` surfaces any unmodeled register (none expected — both subsystems are modeled). If sample-exact fails only near the very start/end, it's an alignment/latency-offset issue in `check_tap.py` (tune the offset search), not a firmware bug — verify the middle of the stream matches before suspecting the driver.

## Risks

- **SD-read-in-the-audio-ISR timing (HW-only).** `update()` does a blocking SD read inside `software_isr()`. QEMU's polled SDIO completes faster than silicon, so a green gate doesn't fully prove HW timing margins. ~40 µs read vs 2.9 ms budget is comfortable; worst case (FAT cluster-boundary latency) is an audible glitch, not a hang. HW is the arbiter (same class as the SDMA/DTCM caveat in [[rt1176-sd-usdhc]]).
- **Stereo deinterleave fragility.** The L/R buffer-boundary carry (`leftover_bytes`, `header[0]` stash) is historically bug-prone; the sample-exact stereo gate (distinct L/R) is specifically designed to catch a swap or off-by-one — do not simplify the ported `consume()`.
- **Sample-exact alignment** is the gate's main implementation effort — the tap has leading silence before audio; `check_tap.py` must find the offset robustly (compare the stream body, not just the edges).
- **WAV strictness** — canonical 16-bit PCM 44.1k only; non-conforming assets (Extensible, 48k, 8-bit) silently don't play. Generated test WAVs sidestep this.
- **SPI linked-but-unused** — `AudioStartUsingSPI()` fires unconditionally; harmless (SD is SDIO), but the gate must link `newdigate/SPI` or it won't build.

## References

- **Port source:** `~/Development/Audio/play_sd_wav.{h,cpp}` (verbatim node), `output_i2s.cpp` (the SAI1 TX sink, `__IMXRT1176__` branch), `control_wm8962.cpp`, `spi_interrupt.h` (AudioStart/StopUsingSPI), `memcpy_audio.S`, `Audio.h` (already includes the node).
- **Core:** `cores/imxrt1176/AudioStream.{h,cpp}` (`IRQ_SOFTWARE=44`, `AUDIO_SAMPLE_RATE_EXACT 44100`, `AUDIO_BLOCK_SAMPLES 128`, `AudioMemory`/`DMAMEM`), `imxrt1176.h` + `tools/gen_imxrt1176_h.py` (add `NVIC_IS_ENABLED`; `NVIC_ISER` exists).
- **Gate precedent:** `evkb/audiooutput_i2s_test/` (SAI1 tap + `check_tap.py` + Audio-node `target_sources` + WM8962), `SdFat/tests/sd_fs_test/` (MBR/FAT16 `card.img` via `hdiutil`/`diskutil`, SD imports). Runner infra [[rt1170-qemu]] (`qrun`) + [[rt1170-gate-lib]].
- **qemu2 (unchanged):** `hw/audio/imxrt_sai.c` (SAI1 `TDR0`→tap chardev), `hw/arm/fsl-imxrt1170.c` (sai1-tap bind + USDHC), `hw/arm/mimxrt1170-evk.c` (`-drive if=sd`→USDHC1).
- **Consumes (all done + HW-verified):** the SD stack [[rt1176-sd-usdhc]], the audio graph [[rt1176-audiostream]] / [[rt1176-audiooutput-i2s]], the codec [[rt1176-wm8962-consolidation]] / [[rt1176-i2s-sai]]; SPI [[rt1176-spi-library-move]]; `EventResponder` NOT needed [[rt1176-eventresponder]].
- **HW + repo:** flash + VCOM [[rt1170-evkb-flashing]] + [[macos-serial-capture]]; repo boundaries [[rt1170-evkb-git-repo]] (evkb gate = local repo; Audio/SPI/SdFat/SD their own repos; core git root = `evkb/cores`).
