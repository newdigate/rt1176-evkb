# RT1176 AudioInputI2S Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Bring up `AudioInputI2S` on RT1176 — SAI-RX audio captured into `audio_block_t`s and transmitted to the AudioStream graph — verified via `AudioInputI2S → AudioAnalyzePeak` in QEMU (SAI injector) then on the onboard mic.

**Architecture:** Mirror Teensy's layering — the **core** (teensy-cores) provides `AudioStream` + register defs + `DMAChannel`; the **Audio fork** (`newdigate/Audio`) gets the RT1176 ports (`config_i2s` at 44.1 kHz, `AudioInputI2S`, a new `AudioControlWM8962`, `analyze_peak`). The nodes never touch the core's `I2SClass`.

**Tech Stack:** C++ Teensy Audio library, RT1176 SAI1 + ANATOP AI audio-PLL, eDMA `DMAChannel`, `AudioStream` software-IRQ dispatch, WM8962 codec (LPI2C5), QEMU `mimxrt1170-evk` + SAI RX injector.

**Spec:** `evkb/docs/superpowers/specs/2026-07-06-rt1176-audioinput-i2s-design.md`. Sub-project B (A = [[rt1176-audiostream]], done).

**Repos:** core `cores/imxrt1176` = teensy-cores (github `origin/master`); **Audio fork** `~/Development/Audio` = `git@github.com:newdigate/Audio.git` (github `origin/master`) — node ports commit HERE; `evkb` = local-only; qemu2 = gitlab (only if a model tweak surfaces). Commit to `master`; push only when asked.

**KEY constraints:** the DMA buffer MUST be `DMAMEM` (DTCM is DMA-unreachable). The gate build compiles the core (globbed) **plus the four fork files** (outside the glob — add them explicitly to the gate's CMake sources + add `~/Development/Audio` to the include path). QEMU's SAI is injector/timer-paced, so the gate proves the **capture→graph→peak plumbing regardless of the exact clock** — 44.1 kHz correctness is a **hardware** item (Task 5 checks the frame-sync).

**Reference:** Teensy `~/.platformio/.../libraries/Audio/{input_i2s,output_i2s,control_wm8960,analyze_peak}.*`; the RT1176 48 kHz template = `cores/imxrt1176/I2S.cpp` (`sai1_audio_pll_init`, `configureSAI`) + `wm8962.cpp` (verified record init).

---

## Task 1: `config_i2s()` at 44.1 kHz (the hard part) — Audio fork

**Files:**
- Modify: `~/Development/Audio/output_i2s.cpp` (add/adapt the RT1176 `config_i2s` for 44.1 kHz)
- Modify: `~/Development/Audio/output_i2s.h` if needed (declarations)

This re-derives the ANATOP AI audio-PLL + SAI clock for 44.1 kHz. **It's a compute-the-constants port, not new mechanism** — the core's `I2S.cpp` `sai1_audio_pll_init()` is the proven 48 kHz template (same AI-write sequence, same QEMU `imxrt_anadig` model).

- [ ] **Step 1: Read the 48 kHz template and derive the 44.1 kHz constants**

Read `cores/imxrt1176/I2S.cpp` (`sai1_audio_pll_init` — the `ai_write(subaddr, val)` sequence programming PLL_AUDIO CTRL0/DIV/NUM/DENOM via the ANATOP AI interface; the AI sub-addresses CTRL0=0x00, and the loop-divider/num/denom fields) and `configureSAI` (the `clock_root` = "mux 4 (Audio PLL), div 16" and the SAI `TCR2/TCR4/TCR5` bit-clock/frame setup). Also read the Teensy `output_i2s.cpp` `__IMXRT1062__` `config_i2s` + `set_audioClock(c0,c1,c2)` for the SAI register structure (word width, sync, MCLK).

Derive for 44.1 kHz: target MCLK = 44100 × 256 = **11.2896 MHz** (or 44100 × 512 = 22.5792 MHz then /2 at the clock root — pick to match the core's div-16 root convention). Compute the PLL_AUDIO fractional loop divider + NUM/DENOM to hit that from the 24 MHz OSC ref (the 48 kHz template's divider is the worked example to scale). Document the chosen `ai_write` values + the resulting frame-sync (should be 44.1 kHz: MCLK / (256) or /(2×bitclkdiv×32)). **Report the derivation** (target MCLK, PLL multiplier/num/denom, root div, expected frame-sync).

- [ ] **Step 2: Implement RT1176 `config_i2s()` in `output_i2s.cpp`**

Add a `#elif defined(__IMXRT1176__)` branch (guard matching the core's target define) to `AudioOutputI2S::config_i2s()`. It must: replicate the AI-write audio-PLL init at the 44.1 kHz constants from Step 1 (port `sai1_audio_pll_init`'s sequence — the AI infrastructure regs `ANADIG_PLL_AUDIO_CTRL`, `ai_write`, are in the core's `imxrt1176.h`; if `ai_write` is `static` in `I2S.cpp`, replicate the one-line indirect-write helper in `output_i2s.cpp`); set the SAI1 clock root (mux 4/Audio PLL, div to yield 44.1 kHz); ungate SAI1 (LPCG); set the pin mux for the SAI clock pins + `SAI1_MCLK_DIR` via `IOMUXC_GPR_GPR0`; configure `SAI1_TCR2/TCR3/TCR4/TCR5` for 16-bit, TX-clock-master, `FCONT`; and (RX synchronous to TX) leave RX config to `AudioInputI2S`. Reference the exact SAI register values from the core's `configureSAI()` (they're HW-verified) — recompute ONLY the clock rate. Guard the whole thing so a non-RT1176 build is unaffected.

- [ ] **Step 3: Compile-check** (against a throwaway harness, since the gate isn't built until Task 4)

There's no runnable test yet. Confirm the file compiles for the target by adding it to a scratch build, or defer the compile-verify to Task 4's gate (which is the first point everything links). Minimum here: `gcc -fsyntax-only` won't have the headers, so instead **stage the compile-check into Task 4** and here just self-review the register/PLL values against the core template. State clearly that Task 1 is verified at Task 4's build.

- [ ] **Step 4: Commit (Audio fork)**

```bash
cd ~/Development/Audio
git add output_i2s.cpp output_i2s.h
git commit -m "config_i2s: RT1176 SAI + audio-PLL setup at 44.1 kHz (ANATOP AI, ported from the core's 48 kHz template)

Co-Authored-By: Claude Opus 4.8 <noreply@anthropic.com>"
```

---

## Task 2: `AudioInputI2S` — Audio fork

**Files:**
- Modify: `~/Development/Audio/input_i2s.cpp` (RT1176 `__IMXRT1176__` branch of `AudioInputI2S::begin`/`isr`/`update`)
- Modify: `~/Development/Audio/input_i2s.h` if the static members/guards need it

- [ ] **Step 1: Add the RT1176 branch to `input_i2s.cpp`**

Add `#elif defined(__IMXRT1176__)` branches mirroring the existing `__IMXRT1062__` code (read it — it's the template). Statics (already declared): `DMAMEM __attribute__((aligned(32))) static uint32_t i2s_rx_buffer[AUDIO_BLOCK_SAMPLES];` plus `block_left`/`block_right`/`block_offset`/`update_responsibility`/`dma`.
`begin()`:
```cpp
    dma.begin(true);
    AudioOutputI2S::config_i2s();
    // RXD pin: GPIO_AD_20 = SAI1_RXD0 (ALT0), input daisy
    IOMUXC_SW_MUX_CTL_PAD_GPIO_AD_20 = 0;      // ALT0
    IOMUXC_SW_PAD_CTL_PAD_GPIO_AD_20 = 0x02;
    IOMUXC_SAI1_RX_DATA0_SELECT_INPUT = 0;
    dma.TCD->SADDR = (void *)((uint32_t)&SAI1_RDR0 + 2);
    dma.TCD->SOFF = 0;
    dma.TCD->ATTR = DMA_TCD_ATTR_SSIZE(1) | DMA_TCD_ATTR_DSIZE(1);
    dma.TCD->NBYTES_MLNO = 2;
    dma.TCD->SLAST = 0;
    dma.TCD->DADDR = i2s_rx_buffer;
    dma.TCD->DOFF = 2;
    dma.TCD->CITER_ELINKNO = sizeof(i2s_rx_buffer) / 2;
    dma.TCD->DLASTSGA = -sizeof(i2s_rx_buffer);
    dma.TCD->BITER_ELINKNO = sizeof(i2s_rx_buffer) / 2;
    dma.TCD->CSR = DMA_TCD_CSR_INTHALF | DMA_TCD_CSR_INTMAJOR;
    dma.triggerAtHardwareEvent(DMAMUX_SOURCE_SAI1_RX);
    SAI1_RCSR = SAI_RCSR_RE | SAI_RCSR_BCE | SAI_RCSR_FRDE | SAI_RCSR_FR;
    update_responsibility = update_setup();
    dma.enable();
    dma.attachInterrupt(isr);
```
Confirm the exact macro spellings against `imxrt1176.h` (`SAI1_RDR0`, `SAI1_RCSR`, the `SAI_RCSR_*` bits from the SAI RX work, `DMAMUX_SOURCE_SAI1_RX`, `DMA_TCD_*`, the `IOMUXC_SW_*_GPIO_AD_20` + `IOMUXC_SAI1_RX_DATA0_SELECT_INPUT`) — adapt spellings if they differ; report any adaptation. `isr()` and `update()` are the same deinterleave+transmit logic as the `__IMXRT1062__` path — reuse them (they're guarded `#if defined(KINETISK) || defined(__IMXRT1062__)` → add `|| defined(__IMXRT1176__)`).

- [ ] **Step 2: Compile-check deferred to Task 4** (self-review vs the 1062 template; the gate is the first link point).

- [ ] **Step 3: Commit (Audio fork)**

```bash
cd ~/Development/Audio && git add input_i2s.cpp input_i2s.h
git commit -m "AudioInputI2S: RT1176 SAI1-RX DMA capture -> audio_block_t (mic on right ch)

Co-Authored-By: Claude Opus 4.8 <noreply@anthropic.com>"
```

---

## Task 3: `AudioControlWM8962` — Audio fork

**Files:**
- Create: `~/Development/Audio/control_wm8962.h`, `~/Development/Audio/control_wm8962.cpp`

- [ ] **Step 1: Create the node from `control_wm8960` + the core's verified init**

Read `~/Development/Audio/control_wm8960.{h,cpp}` (the `AudioControl`-derived interface: `enable()`, `volume()`, `inputLevel()`, `inputSelect()`) and `cores/imxrt1176/wm8962.cpp` (the `WM8962Codec::begin` record-init register sequence — LPI2C5 @0x1A, the INIT_PRE/POST, SEQ, VOLUME, ROUTE writes with `leftInputPGASource=Input1`/`rightInputPGASource=Input3`, MICBIAS, the CLK4/ADDCTL3 clock config). Create `AudioControlWM8962 : public AudioControl` whose `enable()` runs that exact WM8962 register sequence over `Wire`/the LPI2C5 the core uses (reuse the core's `WM8962Codec` directly if simplest — `#include` it and call `Codec.begin(Wire2, 0x1A)` — OR inline the register writes). **Set the codec clocking for 44.1 kHz** (the `CLK4`/ratio + `ADDCTL3` sample-rate case for 44.1 kHz, not the 48 kHz values). Mic is on the RIGHT channel (Input3).

- [ ] **Step 2: Commit (Audio fork)**

```bash
cd ~/Development/Audio && git add control_wm8962.h control_wm8962.cpp
git commit -m "AudioControlWM8962: codec control node (record init at 44.1 kHz, mic on Input3)

Co-Authored-By: Claude Opus 4.8 <noreply@anthropic.com>"
```

---

## Task 4: QEMU gate (`evkb/audioinput_i2s_test/`) — the integration point

**Files:**
- Create: `evkb/audioinput_i2s_test/{audioinput_i2s_test.cpp, CMakeLists.txt, run_qemu_audioinput.sh, toolchain/}`
- Verify present in the fork: `analyze_peak.{cpp,h}` (as-is)

- [ ] **Step 1: Scaffold + wire the fork files into the build**

```bash
mkdir -p ~/Development/rt1170/evkb/audioinput_i2s_test
cp -R ~/Development/rt1170/evkb/sai_rx_test/toolchain ~/Development/rt1170/evkb/audioinput_i2s_test/
cp ~/Development/rt1170/evkb/sai_rx_test/CMakeLists.txt ~/Development/rt1170/evkb/audioinput_i2s_test/
cp ~/Development/rt1170/evkb/sai_rx_test/run_qemu_sai_rx.sh ~/Development/rt1170/evkb/audioinput_i2s_test/run_qemu_audioinput.sh
cp ~/Development/rt1170/evkb/sai_rx_test/gen_inject.py ~/Development/rt1170/evkb/audioinput_i2s_test/
```
Edit `CMakeLists.txt`: rename to `audioinput_i2s_test`; **add the four fork sources explicitly** to the target (they're outside the core glob) — `~/Development/Audio/{input_i2s.cpp,output_i2s.cpp,control_wm8962.cpp,analyze_peak.cpp}` — and add `~/Development/Audio` to `target_include_directories`. (Read `sai_rx_test/CMakeLists.txt` to see the target/glob structure first.)

- [ ] **Step 2: Write the firmware**

Create `audioinput_i2s_test.cpp`:
```cpp
#include "Arduino.h"
#include "HardwareSerial.h"
#include "AudioStream.h"
#include "input_i2s.h"
#include "analyze_peak.h"
#include "control_wm8962.h"

AudioInputI2S      in;
AudioAnalyzePeak   peak;
AudioConnection    patchCord(in, 0, peak, 0);   // ch 0 = left; use (in,1,peak,0) if verifying the mic's right ch
AudioControlWM8962 wm;

void setup() {
    Serial1.begin(115200);
    while (!Serial1) {}
    AudioMemory(24);
    wm.enable();
    // in.begin() runs inside AudioInputI2S's ctor already (AudioStream(0,NULL){begin();}) OR call it if not
    float pk = 0.0f;
    uint32_t t0 = millis();
    while (millis() - t0 < 400) {              // let a few blocks flow
        if (peak.available()) { float v = peak.read(); if (v > pk) pk = v; }
        yield();
    }
    bool ok = pk > 0.02f;                       // clearly non-zero from the injected signal
    Serial1.print("info peak="); Serial1.println(pk, 4);
    Serial1.println(ok ? "STAGE_PEAK=PASS" : "STAGE_PEAK=FAIL");
    Serial1.println(ok ? "AUDIOINPUT_ALL=PASS" : "AUDIOINPUT_ALL=FAIL");
}
void loop() {}
```
Note: whether `AudioInputI2S` auto-`begin()`s in its ctor (Teensy does) vs needs an explicit `in.begin()` — check `input_i2s.h` (`AudioInputI2S(void) : AudioStream(0, NULL) { begin(); }`) and match. Verify the mic-channel: the SAI RX finding is the mic on the RIGHT channel, so the injector's right-channel signal drives `peak` if the patchcord uses input from `in` channel 1 — pick the channel the injector feeds and the mic uses (adjust `AudioConnection(in, 1, peak, 0)` if needed and note it).

- [ ] **Step 3: Injector run script**

Adapt `run_qemu_audioinput.sh` from `sai_rx_test`'s (it already wires the `sai1-rxinject` chardev + the fifo pump feeding `gen_inject.py`'s signal). Point at `audioinput_i2s_test.elf`; keep the injector chardev + fifo pump; grep `STAGE_PEAK=PASS` + `AUDIOINPUT_ALL=PASS`; `sleep 5`. `gen_inject.py` already emits a sine — reuse it (a generous frame count so the graph gets several blocks).

- [ ] **Step 4: Build + run**

```bash
cd ~/Development/rt1170/evkb/audioinput_i2s_test && rm -rf build \
  && cmake -B build -DCMAKE_TOOLCHAIN_FILE=toolchain/rt1170-evkb.toolchain.cmake . >/dev/null \
  && cmake --build build 2>&1 | tail -3 && ./run_qemu_audioinput.sh 2>&1 | tail -10
```
Expected: `info peak=<non-zero>`, `STAGE_PEAK=PASS`, `AUDIOINPUT_ALL=PASS`. **This is the first point Tasks 1-3 actually compile+link+run together** — a build error here points back to the offending task's file (fix in that file, re-commit in the fork). Run twice for stability. If `peak≈0`: the injector isn't reaching the graph — check the DMA (SAI1_RX=54), the `isr` deinterleave, `update_setup`/`IRQ_SOFTWARE` dispatch, and the `AudioConnection` channel vs the injected channel.

- [ ] **Step 5: Commit the gate (evkb)** + any fork fixes (fork)

```bash
cd ~/Development/rt1170/evkb && git add audioinput_i2s_test/
git commit -m "audioinput_i2s_test: QEMU gate (SAI injector -> AudioInputI2S -> analyze_peak) green

Co-Authored-By: Claude Opus 4.8 <noreply@anthropic.com>"
# if Tasks 1-3 needed fixes to compile/run, commit those in ~/Development/Audio too
```

---

## Task 5: Hardware mic test (PAUSE for user)

Controller drives flash + VCOM; the user makes noise near the onboard mic.

- [ ] **Step 1: Flash + capture (baseline quiet, then user makes noise)** — like the SAI RX mic test:
```bash
LS=/Applications/LinkServer_26.6.137/LinkServer
PORT=/dev/cu.usbmodem5DQ2DDHVWO5EI3
# controller: flash audioinput_i2s_test.elf, capture VCOM ~20s; ask the user to tap/talk near the mic
```
Have the firmware print the `info peak=` line periodically in `loop()` (add a 500 ms periodic peak print for the HW build) so the human sees it rise on sound.

- [ ] **Step 2: Verify on silicon**

Expected: `peak` low when quiet, **rises clearly when the user makes sound** near the mic (mic on the right channel / WM8962 Input3 — use the right-channel patchcord on HW). That is the silicon proof of the full chain: mic → WM8962 ADC → SAI RX → DMA → AudioInputI2S → graph → analyze_peak. If measurable, sanity-check the SAI frame-sync ≈ 44.1 kHz (the 44.1 kHz clock correctness). No commit (verification only).

---

## Task 6: Memory note + push

- [ ] **Step 1:** Write `~/.claude/.../memory/rt1176-audioinput-i2s.md` — AudioInputI2S = raw SAI+DMA capture node in the Audio fork (Teensy layering; nodes in newdigate/Audio, framework in the core); `config_i2s` at 44.1 kHz (ANATOP AI audio-PLL re-derived from the core's 48 kHz `sai1_audio_pll_init`; MCLK 11.2896 MHz); DMAMEM `i2s_rx_buffer`; isr deinterleaves + pends `IRQ_SOFTWARE`; `AudioControlWM8962` from the core's verified WM8962 init; mic on right channel; gate reuses the SAI RX injector; QEMU proves plumbing, HW proves the 44.1 kHz + mic. Record the derived PLL constants + the `AudioConnection` channel used. Link `[[rt1176-audiostream]]`, `[[rt1176-sai-rx]]`, `[[rt1176-i2s-sai]]`, `[[rt1176-edma-dmachannel]]`. Add the `MEMORY.md` pointer.
- [ ] **Step 2 (when the user asks):** push the Audio fork (`cd ~/Development/Audio && git push origin master`); cores only if touched; evkb local-only.

---

## Self-Review

**Spec coverage:** §config_i2s(44.1k) → Task 1; §AudioInputI2S → Task 2; §AudioControlWM8962 → Task 3; §analyze_peak + §Testing(injector→peak) → Task 4; §HW(mic→peak) → Task 5; memory/push → Task 6. All covered.

**Placeholder scan:** Task 1's PLL constants are a genuine derive-then-implement with a concrete procedure (read the 48 kHz template, scale to 11.2896 MHz, report the values) + a HW frame-sync check — the RT1176 audio-PLL fractional math is the implementer's to compute against the datasheet/core template, not a lazy TBD. The `<session-scratchpad>`/periodic-print in Task 5 are controller/HW-build specifics. Tasks 1-3 compile-verify at Task 4 (the first link point) — flagged explicitly, not hidden.

**Type consistency:** `config_i2s()` / `AudioInputI2S` (begin/isr/update, `i2s_rx_buffer`, `block_left`/`block_right`) / `AudioControlWM8962::enable()` / `AudioAnalyzePeak` (`available`/`read`) / `AudioConnection(in, ch, peak, 0)` / `AudioMemory` / `DMAMUX_SOURCE_SAI1_RX` / `IRQ_SOFTWARE` used consistently across the fork ports and the gate.
