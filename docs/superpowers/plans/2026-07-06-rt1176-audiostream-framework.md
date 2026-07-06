# RT1176 AudioStream framework Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Port the Teensy4 audio-graph engine `AudioStream` into the RT1176 core so audio nodes have their base class, block pool, connections, and update dispatch.

**Architecture:** Verbatim port of `cores/teensy4/AudioStream.{h,cpp}` into `cores/imxrt1176/`, with four retargets (`IRQ_SOFTWARE` spare vector, `MAX_AUDIO_MEMORY` arch branch, `F_CPU_ACTUAL`/`FLASHMEM` if absent). Verified by a pure-firmware QEMU gate driving two synthetic nodes through the software-IRQ dispatch.

**Tech Stack:** C++ imxrt1176 core, Teensy `AudioStream`, `attachInterruptVector`/NVIC software IRQ, `DMAMEM`, QEMU `mimxrt1170-evk`, LinkServer.

**Reference (verbatim source):** `~/.platformio/packages/framework-arduinoteensy/cores/teensy4/AudioStream.{h,cpp}`.

**Spec:** `evkb/docs/superpowers/specs/2026-07-06-rt1176-audiostream-framework-design.md`. Sub-project A of the AudioInputI2S effort (B = `AudioInputI2S` in `newdigate/Audio` fork, later).

**Repos:** `cores/imxrt1176` = nested **teensy-cores** (github `origin/master`); `evkb` = local-only. Commit to `master`; push only when asked. No QEMU changes.

**CMake note:** gate builds glob the core via `file(GLOB)` without `CONFIGURE_DEPENDS`, so after adding `AudioStream.{h,cpp}` to the core, gate builds must `rm -rf build` to re-glob.

---

## Task 1: Port `AudioStream.{h,cpp}` + retargets

**Files:**
- Create: `cores/imxrt1176/AudioStream.h` (copy of the teensy4 reference + retargets)
- Create: `cores/imxrt1176/AudioStream.cpp` (copy + retargets)

- [ ] **Step 1: Copy the reference verbatim**

```bash
cp ~/.platformio/packages/framework-arduinoteensy/cores/teensy4/AudioStream.h  ~/Development/rt1170/evkb/cores/imxrt1176/AudioStream.h
cp ~/.platformio/packages/framework-arduinoteensy/cores/teensy4/AudioStream.cpp ~/Development/rt1170/evkb/cores/imxrt1176/AudioStream.cpp
```

- [ ] **Step 2: Discover what the core already provides (informs the retargets)**

Read/grep the core to confirm which symbols exist vs. need defining:
```bash
cd ~/Development/rt1170/evkb/cores/imxrt1176
echo "target define:"; grep -rnoE '__IMXRT1176__|CPU_MIMXRT1176|__IMXRT1062__' *.h *.c* 2>/dev/null | head; grep -rn 'IMXRT' ../../spi_loopback_test/toolchain/*.cmake 2>/dev/null | head
echo "NVIC macros:"; grep -nE 'NVIC_SET_PENDING|NVIC_SET_PRIORITY|NVIC_ENABLE_IRQ|NVIC_DISABLE_IRQ' imxrt1176.h
echo "F_CPU_ACTUAL / FLASHMEM / DMAMEM / ARM_DWT_CYCCNT:"; grep -rnE 'F_CPU_ACTUAL|FLASHMEM|define DMAMEM|ARM_DWT_CYCCNT' *.h | head
echo "IRQ enum + count (for a spare IRQ_SOFTWARE):"; grep -nE 'IRQ_NUMBER_t|NVIC_NUM_INTERRUPTS|IRQ_SOFTWARE' core_pins.h imxrt1176.h | head -20
```
Note the findings; you'll define exactly what's missing in Steps 3-6. (Expected from prior exploration: `NVIC_SET_PENDING` exists (`imxrt1176.h`); `ARM_DWT_CYCCNT` exists; `DMAMEM` exists. Likely missing: `IRQ_SOFTWARE`, `MAX_AUDIO_MEMORY` for our arch, possibly `F_CPU_ACTUAL`, `FLASHMEM`, and some `NVIC_*` spellings.)

- [ ] **Step 3: Retarget `MAX_AUDIO_MEMORY`** (`AudioStream.cpp`)

The reference has (near line 35):
```cpp
#if defined(__IMXRT1062__)
  #define MAX_AUDIO_MEMORY 229376
#endif
```
Our target is not `__IMXRT1062__`, so `MAX_AUDIO_MEMORY` (and thus `NUM_MASKS`) would be undefined → compile error. Replace with a branch that also covers our core; simplest robust form is an `#else` fallback so it can never be undefined:
```cpp
#if defined(__IMXRT1062__)
  #define MAX_AUDIO_MEMORY 229376
#else
  // RT1176: cap the audio block pool. 229376 bytes / (128 samples * 2 ch * 2 bytes)
  // ~= 448 blocks max; the app sizes the actual pool via AudioMemory(n) well below this.
  #define MAX_AUDIO_MEMORY 229376
#endif
```
(If Step 2 shows a specific target macro like `__IMXRT1176__`, prefer `#elif defined(__IMXRT1176__)` with the same value, keeping the `#else` as a safety net.)

- [ ] **Step 4: Define `IRQ_SOFTWARE`** as a spare NVIC vector

The reference uses `IRQ_SOFTWARE` in `update_all()` (`NVIC_SET_PENDING`), `update_setup()` (`attachInterruptVector`+priority+enable), and `update_stop()`. This core has no `IRQ_SOFTWARE`. From Step 2's IRQ enum + `NVIC_NUM_INTERRUPTS`, pick a **confirmed-unused** IRQ number (one no peripheral in this core attaches — check that no `attachInterruptVector(IRQ_x,...)` or `IRQ_x` in the enum collides) that is `< NVIC_NUM_INTERRUPTS` (so it has a RAM-vector slot). Define it near the top of `AudioStream.h` (after the includes, before it's first used), e.g.:
```cpp
// Spare NVIC vector repurposed as the audio-graph software interrupt. <N> is
// unused by any peripheral in this core and < NVIC_NUM_INTERRUPTS (has a RAM
// vector slot). The audio clock owner pends it (NVIC_SET_PENDING) so software_isr
// runs update_all() at low priority.
#ifndef IRQ_SOFTWARE
#define IRQ_SOFTWARE ((IRQ_NUMBER_t)<N>)
#endif
```
Report the exact `<N>` you chose and why it's free. Verify `attachInterruptVector((IRQ_NUMBER_t)<N>, software_isr)` compiles (the RAM vector table is sized `16 + NVIC_NUM_INTERRUPTS`).

- [ ] **Step 5: Define `F_CPU_ACTUAL` and `FLASHMEM` only if Step 2 shows them missing**

- `F_CPU_ACTUAL` is used by the `CYCLE_COUNTER_APPROX_PERCENT` macro (CPU-usage). If absent, add to `AudioStream.h` (before that macro):
```cpp
#ifndef F_CPU_ACTUAL
#define F_CPU_ACTUAL 996000000   // RT1176 CM7 core clock (see rt1176-996mhz-overdrive)
#endif
```
- `FLASHMEM` annotates `initialize_memory` in the `.cpp`. If absent, add to `AudioStream.h`:
```cpp
#ifndef FLASHMEM
#define FLASHMEM   // this core has no separate flash-exec section attr; run from .text
#endif
```
If Step 2 shows either already defined by the core, do NOT redefine — skip it.

- [ ] **Step 6: Confirm the `NVIC_*` spellings**

`update_setup`/`update_stop` use `NVIC_SET_PRIORITY`, `NVIC_ENABLE_IRQ`, `NVIC_DISABLE_IRQ`; `update_all` uses `NVIC_SET_PENDING`. If Step 2 shows any missing or spelled differently in this core, add the missing macro(s) to `AudioStream.h` using the CMSIS/NVIC registers already defined in `imxrt1176.h` (mirror the existing `NVIC_SET_PENDING(n)` definition's style). Report which you had to add.

- [ ] **Step 7: Build-verify (the core compiles with `AudioStream` added)**

Adding `AudioStream.{h,cpp}` to the core means every gate now globs+compiles them. Rebuild an existing gate to confirm the `.cpp` (always compiled) resolves all symbols:
```bash
cd ~/Development/rt1170/evkb/spi_loopback_test && rm -rf build \
  && cmake -B build -DCMAKE_TOOLCHAIN_FILE=toolchain/rt1170-evkb.toolchain.cmake . >/dev/null \
  && cmake --build build 2>&1 | tail -6
```
Expected: clean build. Any `undefined`/`not declared` for `IRQ_SOFTWARE`, `MAX_AUDIO_MEMORY`, `NUM_MASKS`, `F_CPU_ACTUAL`, `FLASHMEM`, `attachInterruptVector`, `NVIC_*`, `ARM_DWT_CYCCNT` means a retarget is missing — fix and rebuild. Host-analysis LSP warnings are irrelevant.

- [ ] **Step 8: Commit (cores)**

```bash
cd ~/Development/rt1170/evkb/cores
git add imxrt1176/AudioStream.h imxrt1176/AudioStream.cpp
git status --short   # only those two files
git commit -m "AudioStream: port Teensy4 audio-graph engine (base + block pool + update dispatch)

Verbatim port with retargets: IRQ_SOFTWARE spare NVIC vector, MAX_AUDIO_MEMORY
arch branch, F_CPU_ACTUAL/FLASHMEM as needed. 44100/128, pool in DMAMEM.

Co-Authored-By: Claude Opus 4.8 <noreply@anthropic.com>"
```

---

## Task 2: QEMU gate (`evkb/audiostream_test/`)

Pure firmware — two synthetic nodes prove the engine via the software-IRQ dispatch; no QEMU device.

**Files:**
- Create: `evkb/audiostream_test/{audiostream_test.cpp, CMakeLists.txt, run_qemu_audiostream.sh, toolchain/}`

- [ ] **Step 1: Copy the gate scaffolding**

```bash
mkdir -p ~/Development/rt1170/evkb/audiostream_test
cp -R ~/Development/rt1170/evkb/spi_loopback_test/toolchain ~/Development/rt1170/evkb/audiostream_test/
cp ~/Development/rt1170/evkb/spi_loopback_test/CMakeLists.txt ~/Development/rt1170/evkb/audiostream_test/
cp ~/Development/rt1170/evkb/spi_loopback_test/run_qemu_spi.sh ~/Development/rt1170/evkb/audiostream_test/run_qemu_audiostream.sh
```
Edit `audiostream_test/CMakeLists.txt`: rename `spi_loopback_test`→`audiostream_test` in `project(...)`/`add_executable(...)` (ELF `audiostream_test.elf`); leave the core `file(GLOB)` as-is. Read the copied CMakeLists.txt first.

- [ ] **Step 2: Write the firmware test**

Create `evkb/audiostream_test/audiostream_test.cpp`:
```cpp
#include "Arduino.h"
#include "HardwareSerial.h"
#include "AudioStream.h"

// Synthetic source: each update() emits a block filled with an incrementing
// per-block value (block k -> all samples = k), so the sink can verify order.
class TestSource : public AudioStream {
public:
    TestSource() : AudioStream(0, NULL) { update_setup(); }  // take the software-IRQ dispatch
    volatile uint16_t produced = 0;
    void update(void) override {
        audio_block_t *b = allocate();
        if (!b) return;                       // pool exhausted -> skip (sink sees a gap)
        for (int i = 0; i < AUDIO_BLOCK_SAMPLES; i++) b->data[i] = (int16_t)produced;
        transmit(b, 0);
        release(b);                           // we transmitted; drop our ownership
        produced++;
    }
};

// Synthetic sink: 1 input; records the first sample of each received block.
class TestSink : public AudioStream {
public:
    TestSink() : AudioStream(1, inputQueueArray) {}
    volatile uint16_t received = 0;
    volatile int16_t last = -1;
    volatile int16_t values[64];
    void update(void) override {
        audio_block_t *b = receiveReadOnly(0);
        if (!b) return;
        last = b->data[0];
        if (received < 64) values[received] = b->data[0];
        received++;
        release(b);
    }
private:
    audio_block_t *inputQueueArray[1];
};

static TestSource src;
static TestSink   sink;
static AudioConnection conn(src, 0, sink, 0);

void setup() {
    Serial1.begin(115200);
    while (!Serial1) {}
    AudioMemory(20);                          // size the pool

    const int N = 8;
    for (int k = 0; k < N; k++) {
        NVIC_SET_PENDING(IRQ_SOFTWARE);       // stand in for the audio clock's trigger
        delayMicroseconds(200);               // let software_isr run one graph pass
    }

    // STAGE_FLOW: sink saw N blocks, values 0..N-1 in order
    bool flow = (sink.received >= (uint16_t)N);
    for (int k = 0; k < N && flow; k++) if (sink.values[k] != (int16_t)k) flow = false;
    Serial1.println(flow ? "STAGE_FLOW=PASS" : "STAGE_FLOW=FAIL");

    // STAGE_NOLEAK: every allocated block was released -> memory_used back to 0
    bool noleak = (AudioMemoryUsage() == 0);
    Serial1.println(noleak ? "STAGE_NOLEAK=PASS" : "STAGE_NOLEAK=FAIL");

    Serial1.print("info received="); Serial1.print(sink.received);
    Serial1.print(" mem_used="); Serial1.print(AudioMemoryUsage());
    Serial1.print(" mem_max="); Serial1.println(AudioMemoryUsageMax());
    Serial1.println((flow && noleak) ? "AUDIOSTREAM_ALL=PASS" : "AUDIOSTREAM_ALL=FAIL");
}
void loop() {}
```
Notes: `update_setup()`/`allocate`/`release`/`transmit`/`receiveReadOnly` are `protected` in `AudioStream`, so they're only reachable from within the subclasses (as used above) — the gate itself only calls public `AudioMemory`/`AudioMemoryUsage*` + `NVIC_SET_PENDING(IRQ_SOFTWARE)`. If `AudioMemoryUsage()`/`AudioMemoryUsageMax()` (macros reading `AudioStream::memory_used`) don't compile from the sketch, use them anyway — they're public macros in `AudioStream.h`.

- [ ] **Step 3: Adapt the run script**

In `run_qemu_audiostream.sh`: point at `audiostream_test.elf`; keep the QEMU/`qrun` invocation + machine flags + the output-file variable exactly as the copied script (match its real name, e.g. `$OUT`); rename output files (e.g. `audiostream.uart`/`.dbg`); `sleep 3`→`sleep 5`; replace the grep/check block with:
```sh
grep -q "STAGE_FLOW=PASS"    "$OUT" || { echo "FAIL: flow";   exit 1; }
grep -q "STAGE_NOLEAK=PASS"  "$OUT" || { echo "FAIL: noleak"; exit 1; }
grep -q "AUDIOSTREAM_ALL=PASS" "$OUT" || { echo "FAIL: overall"; exit 1; }
echo "PASS: AUDIOSTREAM_ALL"
```
Read the copied `run_qemu_spi.sh` first to match its actual variable names.

- [ ] **Step 4: Build + run**

```bash
cd ~/Development/rt1170/evkb/audiostream_test && rm -rf build \
  && cmake -B build -DCMAKE_TOOLCHAIN_FILE=toolchain/rt1170-evkb.toolchain.cmake . >/dev/null \
  && cmake --build build 2>&1 | tail -2 && ./run_qemu_audiostream.sh 2>&1 | tail -10
```
Expected: `STAGE_FLOW=PASS`, `STAGE_NOLEAK=PASS`, `AUDIOSTREAM_ALL=PASS`, `PASS: AUDIOSTREAM_ALL`. Run twice for stability. If `STAGE_FLOW=FAIL` with `received=0`, the software-IRQ dispatch isn't firing — check `IRQ_SOFTWARE` (Task 1 Step 4): the vector must be attached (`update_setup` ran in `TestSource`'s ctor), enabled, and `< NVIC_NUM_INTERRUPTS`. If `received>0` but values wrong, the connection/transmit/receive path is off. Do NOT weaken the test — fix the framework.

- [ ] **Step 5: Commit (evkb)**

```bash
cd ~/Development/rt1170/evkb
git add audiostream_test/audiostream_test.cpp audiostream_test/CMakeLists.txt audiostream_test/run_qemu_audiostream.sh audiostream_test/toolchain
git status --short   # only audiostream_test/* (evkb has unrelated pre-existing changes — do not stage them)
git commit -m "audiostream_test: QEMU gate (graph flow + no-leak via software-IRQ dispatch) green

Co-Authored-By: Claude Opus 4.8 <noreply@anthropic.com>"
```

---

## Task 3: Hardware smoke test

Light — the framework has no peripheral. Controller-driven (board connected; flash + VCOM scriptable).

- [ ] **Step 1: Flash + capture**

```bash
LS=/Applications/LinkServer_26.6.137/LinkServer
PORT=/dev/cu.usbmodem5DQ2DDHVWO5EI3
OUT=<session-scratchpad>/audiostream_vcom.txt
( gtimeout 20 python3 -c "
import serial,sys
s=serial.Serial('$PORT',115200,timeout=1)
while True:
    l=s.readline()
    if l: sys.stdout.write(l.decode('utf-8','replace')); sys.stdout.flush()
" > \"$OUT\" 2>&1 ) &
CAP=$!
sleep 2
gtimeout 90 "$LS" flash MIMXRT1176:MIMXRT1170-EVKB load ~/Development/rt1170/evkb/audiostream_test/build/audiostream_test.elf 2>&1 | grep -iE 'finish|loaded|error' | tail -2
wait $CAP
cat "$OUT"
```

- [ ] **Step 2: Verify on silicon**

Expected VCOM: `STAGE_FLOW=PASS`, `STAGE_NOLEAK=PASS`, `AUDIOSTREAM_ALL=PASS`. Proves the software-IRQ dispatch + block pool + graph walk run on real hardware. If it passes in QEMU but fails on silicon, investigate the IRQ vector / priority on hardware before declaring done. No commit (verification only).

---

## Task 4: Memory note + push

**Files:**
- Create: `~/.claude/projects/-Users-nicholasnewdigate-Development-rt1170/memory/rt1176-audiostream.md`
- Modify: memory `MEMORY.md`

- [ ] **Step 1: Write the memory note**

Capture: Teensy4 `AudioStream` ported into the core (base + `audio_block_t` + `DMAMEM` block pool + `AudioConnection` + `update_all`); dispatch = **software IRQ** `IRQ_SOFTWARE` (the chosen spare vector `<N>`) attached to `software_isr` via `attachInterruptVector`, pended by `NVIC_SET_PENDING` (the audio clock owner pends it; sub-project B's I2S DMA will); retargets: `MAX_AUDIO_MEMORY` arch branch, `IRQ_SOFTWARE`, `F_CPU_ACTUAL`/`FLASHMEM` if they were missing; `AUDIO_SAMPLE_RATE=44100`/`AUDIO_BLOCK_SAMPLES=128`; `AudioMemory(n)` pool is `DMAMEM` by the macro; `ARM_DWT_CYCCNT` CPU-usage works. Gate `evkb/audiostream_test` = pure firmware (synthetic source→sink, pend `IRQ_SOFTWARE`, verify flow + no-leak). Sub-project A for **AudioInputI2S** (B, in the `newdigate/Audio` fork; will reconcile SAI 48k→44.1k). Record the exact `IRQ_SOFTWARE` number chosen. Link `[[rt1176-i2s-sai]]`, `[[rt1176-sai-rx]]`, `[[rt1176-edma-dmachannel]]`, `[[rt1176-eventresponder]]`. Add a one-line `MEMORY.md` pointer.

- [ ] **Step 2: Push (only when the user asks)**

```bash
cd ~/Development/rt1170/evkb/cores && git push origin master 2>&1 | tail -2   # teensy-cores
# evkb is local-only — nothing to push
```

---

## Self-Review

**Spec coverage:** §components (AudioStream base, block pool, AudioConnection, update_all) → Task 1 (verbatim port). §dispatch (software IRQ) → Task 1 Steps 4/6 (IRQ_SOFTWARE + NVIC). §constants (44100/128) → already in the reference (Task 1 Step 1, confirmed). §pool in DMAMEM → the `AudioMemory` macro (Task 1). §gate (synthetic source→sink, flow + no-leak) → Task 2. §HW smoke → Task 3. Memory/push → Task 4. All covered. The retargets the spec named (IRQ_SOFTWARE, DMAMEM pool, sample rate) are each addressed; the two the code review surfaced (`MAX_AUDIO_MEMORY`, `F_CPU_ACTUAL`/`FLASHMEM`) are Task 1 Steps 3/5.

**Placeholder scan:** The `<N>` for `IRQ_SOFTWARE` and the arch macro are genuine discover-then-pick steps with a concrete procedure (grep the enum, pick unused, verify `< NVIC_NUM_INTERRUPTS`), not lazy TBDs — the implementer reports the value. `<session-scratchpad>` in Task 3 is a controller substitution. No other placeholders; the gate code is complete.

**Type consistency:** `AudioStream(ninput, iqueue)` ctor, `update()`/`allocate`/`release`/`transmit`/`receiveReadOnly`, `AudioConnection(src, srcout, dst, dstin)`, `AudioMemory`/`AudioMemoryUsage`, `IRQ_SOFTWARE`, `NVIC_SET_PENDING` used consistently between the ported framework (Task 1) and the gate's synthetic nodes (Task 2). The gate's `TestSource(0,NULL)` / `TestSink(1,inputQueueArray)` match the reference `AudioStream` ctor signature.
