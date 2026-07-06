# RT1176 SPI full-duplex DMA transfer (blocking + async) Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Add DMA-backed full-duplex SPI `transfer` — a blocking `transfer(tx,rx,count)` and an async `transfer(tx,rx,count,EventResponder&)` — to the RT1176 LPSPI driver.

**Architecture:** Two `DMAChannel`s (TX feeds `TDR`, RX drains `RDR`→rxbuf); `DER=TDDE|RDDE` gates the requests; the RX-completion ISR marks done and fires the EventResponder. Blocking `yield()`-spins on the done flag; async returns immediately (`false` if busy). Verbatim port of the Teensy4 IMXRT LPSPI DMA transfer.

**Tech Stack:** C++ imxrt1176 core, Teensy `DMAChannel` (eDMA), `EventResponder`, QEMU `mimxrt1170-evk` + SSI loopback, LinkServer flash.

**Reference:** `~/.platformio/packages/framework-arduinoteensy/libraries/SPI/SPI.cpp` — IMXRT section (`#elif … __IMXRT1062__`).

**Spec:** `evkb/docs/superpowers/specs/2026-07-06-rt1176-spi-dma-async-design.md`.

**Repos:** `cores/imxrt1176` = nested **teensy-cores** (github `origin/master`); `qemu2` = gitlab `origin/master`; `evkb` = local-only. Commit to `master`; push only when the user asks.

**KEY memory rule:** DMA-accessed buffers must be in `DMAMEM` (OCRAM) — DTCM (default `.bss`) is DMA-unreachable on this silicon. All gate buffers use `DMAMEM`.

---

## Task 1: LPSPI DMA register defs (generator) + `hardware_t` der/fcr

**Files:**
- Modify: `cores/imxrt1176/tools/gen_imxrt1176_h.py` (LPSPI1 block ~line 272-280)
- Regenerate: `cores/imxrt1176/imxrt1176.h`
- Modify: `cores/imxrt1176/SPI.h` (hardware_t)
- Modify: `cores/imxrt1176/SPI_instances.cpp` (literal)

- [ ] **Step 1: Add the LPSPI1 DER/FCR register macros to the generator**

In `tools/gen_imxrt1176_h.py`, the LPSPI1 block emits register `#define`s (`LPSPI1_CR`@0x40114010 … `LPSPI1_RDR`@0x40114074). Add `LPSPI1_DER` (offset 0x1C) after the `LPSPI1_SR` line and `LPSPI1_FCR` (offset 0x58) after the `LPSPI1_CCR` line, so the block reads (in offset order):
```python
          "#define LPSPI1_CR     (*(volatile uint32_t *)0x40114010u)",
          "#define LPSPI1_SR     (*(volatile uint32_t *)0x40114014u)",
          "#define LPSPI1_DER    (*(volatile uint32_t *)0x4011401Cu)",
          "#define LPSPI1_CFGR1  (*(volatile uint32_t *)0x40114024u)",
          "#define LPSPI1_CCR    (*(volatile uint32_t *)0x40114040u)",
          "#define LPSPI1_FCR    (*(volatile uint32_t *)0x40114058u)",
          "#define LPSPI1_TCR    (*(volatile uint32_t *)0x40114060u)",
          "#define LPSPI1_TDR    (*(volatile uint32_t *)0x40114064u)",
          "#define LPSPI1_RSR    (*(volatile uint32_t *)0x40114070u)",
          "#define LPSPI1_RDR    (*(volatile uint32_t *)0x40114074u)",
```

- [ ] **Step 2: Regenerate the header and reconcile to an empty diff**

```bash
cd ~/Development/rt1170/evkb/cores/imxrt1176
python3 tools/gen_imxrt1176_h.py > /tmp/imxrt1176.new.h && diff imxrt1176.h /tmp/imxrt1176.new.h
```
Expected diff: ONLY the two new lines (`LPSPI1_DER`, `LPSPI1_FCR`). If so, copy it in:
```bash
cp /tmp/imxrt1176.new.h imxrt1176.h
grep -nE 'LPSPI1_(DER|FCR)' imxrt1176.h   # confirm both present at 0x4011401C / 0x40114058
```

- [ ] **Step 3: Add `der`/`fcr` refs to `hardware_t`**

In `cores/imxrt1176/SPI.h`, the `hardware_t` struct begins `volatile uint32_t &cr; … &rsr; &rdr; &lpcg; …`. Insert `der`/`fcr` right after `rdr` (grouping the core LPSPI registers before `lpcg`):
```cpp
		volatile uint32_t &rsr;
		volatile uint32_t &rdr;
		volatile uint32_t &der;
		volatile uint32_t &fcr;
		volatile uint32_t &lpcg;
```

- [ ] **Step 4: Add `der`/`fcr` to the instance literal**

In `cores/imxrt1176/SPI_instances.cpp`, the `lpspi1_hw` literal has `/* rdr */ LPSPI1_RDR,` immediately before the `/* lpcg */` line. Insert the two new refs positionally (matching the struct order from Step 3):
```cpp
	/* tcr */ LPSPI1_TCR, /* tdr */ LPSPI1_TDR, /* rsr */ LPSPI1_RSR, /* rdr */ LPSPI1_RDR,
	/* der */ LPSPI1_DER, /* fcr */ LPSPI1_FCR,
	/* lpcg */ CCM_LPCG104_DIRECT, /* clock_root */ CCM_CLOCK_ROOT43_CONTROL, /* clock_root_val */ 0u,
```

- [ ] **Step 5: Verify compile (rebuild the existing SPI gate)**

```bash
cd ~/Development/rt1170/evkb/spi_loopback_test && rm -rf build \
  && cmake -B build -DCMAKE_TOOLCHAIN_FILE=toolchain/rt1170-evkb.toolchain.cmake . >/dev/null \
  && cmake --build build 2>&1 | tail -3
```
Expected: builds `spi_loopback_test.elf` cleanly (the enlarged `hardware_t` + literal compile). LSP host-analysis warnings are irrelevant noise.

- [ ] **Step 6: Commit (cores)**

```bash
cd ~/Development/rt1170/evkb/cores
git add imxrt1176/tools/gen_imxrt1176_h.py imxrt1176/imxrt1176.h imxrt1176/SPI.h imxrt1176/SPI_instances.cpp
git commit -m "SPI/LPSPI: DER+FCR register defs + hardware_t refs (DMA prep)

Co-Authored-By: Claude Opus 4.8 <noreply@anthropic.com>"
```

---

## Task 2: SPI DMA transfers (`startDMA` + `rxisr` + blocking + async)

**Files:**
- Modify: `cores/imxrt1176/SPI.h` (declarations + private members)
- Modify: `cores/imxrt1176/SPI.cpp` (implementation)

- [ ] **Step 1: Declare the API + private DMA state in `SPI.h`**

Add includes near the top of `SPI.h` (after the existing includes):
```cpp
#include "DMAChannel.h"
#include "EventResponder.h"
```
In `class SPIClass`, add the two public overloads (after the existing `void transfer(void *buf, size_t count);`):
```cpp
	void transfer(const void *txbuf, void *rxbuf, size_t count);                     // blocking DMA
	bool transfer(const void *txbuf, void *rxbuf, size_t count, EventResponder &event); // async DMA
```
And in the `private:` section (after `void setClockDivider(uint32_t);`):
```cpp
	DMAChannel *_dmaTX = nullptr;
	DMAChannel *_dmaRX = nullptr;
	EventResponder *_dma_event_responder = nullptr;
	volatile bool _transfer_done = true;   // idle
	void startDMA(const void *txbuf, void *rxbuf, size_t count);
	static void rxisr();
```

- [ ] **Step 2: Implement `startDMA`, `rxisr`, and the two transfers in `SPI.cpp`**

Add the DER bit defines near the top of `SPI.cpp` (with the existing `#define`s):
```cpp
// DER (DMA enable)
#define DER_TDDE (1u<<0)
#define DER_RDDE (1u<<1)
```
Append the implementation at the end of `SPI.cpp`:
```cpp
// Single SPI instance on this core: the RX-completion ISR reaches the active
// instance's DMA state through this pointer, set in startDMA().
static SPIClass *dma_active_spi = nullptr;

void SPIClass::startDMA(const void *txbuf, void *rxbuf, size_t count) {
	if (_dmaTX == nullptr) _dmaTX = new DMAChannel();
	if (_dmaRX == nullptr) _dmaRX = new DMAChannel();
	dma_active_spi = this;

	// RX drains RDR -> rxbuf; its completion is the transfer's completion.
	_dmaRX->disable();
	_dmaRX->source(*(volatile uint8_t *)&hw->rdr);
	_dmaRX->destinationBuffer((uint8_t *)rxbuf, count);
	_dmaRX->disableOnCompletion();
	_dmaRX->triggerAtHardwareEvent(DMAMUX_SOURCE_LPSPI1_RX);
	_dmaRX->attachInterrupt(rxisr);
	_dmaRX->interruptAtCompletion();

	// TX feeds txbuf -> TDR.
	_dmaTX->disable();
	_dmaTX->destination(*(volatile uint8_t *)&hw->tdr);
	_dmaTX->sourceBuffer((const uint8_t *)txbuf, count);
	_dmaTX->disableOnCompletion();
	_dmaTX->triggerAtHardwareEvent(DMAMUX_SOURCE_LPSPI1_TX);

	hw->tcr = (tcr_base & ~TCR_FRAMESZ(0xFFF)) | TCR_FRAMESZ(7);  // 8-bit frames
	hw->fcr = 0;                                                  // watermark 0
	_transfer_done = false;
	hw->der = DER_TDDE | DER_RDDE;                                // both DMA requests
	_dmaRX->enable();                                             // arm RX before TX
	_dmaTX->enable();
}

void SPIClass::rxisr() {
	SPIClass *spi = dma_active_spi;
	spi->_dmaRX->clearInterrupt();
	asm volatile ("dsb" ::: "memory");     // ensure the interrupt clears before return
	spi->hw->der = 0;                      // stop DMA requests
	EventResponder *e = spi->_dma_event_responder;
	spi->_dma_event_responder = nullptr;
	spi->_transfer_done = true;
	if (e) e->triggerEvent();
}

void SPIClass::transfer(const void *txbuf, void *rxbuf, size_t count) {
	if (count == 0) return;
	_dma_event_responder = nullptr;
	startDMA(txbuf, rxbuf, count);
	while (!_transfer_done) yield();        // cooperative wait; RX ISR sets the flag
}

bool SPIClass::transfer(const void *txbuf, void *rxbuf, size_t count, EventResponder &event) {
	if (count == 0) return false;
	if (!_transfer_done) return false;      // a transfer is already in progress
	_dma_event_responder = &event;
	startDMA(txbuf, rxbuf, count);
	return true;
}
```
Note: `tcr_base` and `TCR_FRAMESZ` already exist in `SPI.cpp` (used by the polled path); `DMAMUX_SOURCE_LPSPI1_RX`/`_TX` come from `imxrt1176.h`. The DMA does 8-bit accesses to the low byte of RDR/TDR (`(volatile uint8_t *)&hw->rdr`), matching the reference.

- [ ] **Step 3: Verify compile (rebuild the SPI gate)**

```bash
cd ~/Development/rt1170/evkb/spi_loopback_test && rm -rf build \
  && cmake -B build -DCMAKE_TOOLCHAIN_FILE=toolchain/rt1170-evkb.toolchain.cmake . >/dev/null \
  && cmake --build build 2>&1 | tail -3
```
Expected: clean build (the new methods compile + link; `DMAChannel`/`EventResponder` resolve). No undefined references.

- [ ] **Step 4: Commit (cores)**

```bash
cd ~/Development/rt1170/evkb/cores
git add imxrt1176/SPI.h imxrt1176/SPI.cpp
git commit -m "SPI: full-duplex DMA transfer (blocking yield-spin + async EventResponder)

Co-Authored-By: Claude Opus 4.8 <noreply@anthropic.com>"
```

---

## Task 3: QEMU gate (`evkb/spi_dma_test/`) + resolve the §5 QEMU risk

**Files:**
- Create: `evkb/spi_dma_test/{spi_dma_test.cpp, CMakeLists.txt, run_qemu_spidma.sh, toolchain/}`
- Possibly modify: `qemu2/hw/ssi/imxrt_lpspi.c` (contingency, Step 5)

- [ ] **Step 1: Copy the gate scaffolding from `spi_loopback_test`**

```bash
mkdir -p ~/Development/rt1170/evkb/spi_dma_test
cp -R ~/Development/rt1170/evkb/spi_loopback_test/toolchain ~/Development/rt1170/evkb/spi_dma_test/
cp ~/Development/rt1170/evkb/spi_loopback_test/CMakeLists.txt ~/Development/rt1170/evkb/spi_dma_test/
cp ~/Development/rt1170/evkb/spi_loopback_test/run_qemu_spi.sh ~/Development/rt1170/evkb/spi_dma_test/run_qemu_spidma.sh
```
Edit `spi_dma_test/CMakeLists.txt`: rename `spi_loopback_test`→`spi_dma_test` in `project(...)`/`add_executable(...)` (built ELF `spi_dma_test.elf`); leave the core `file(GLOB)` as-is.

- [ ] **Step 2: Write the firmware test (DMAMEM buffers)**

Create `evkb/spi_dma_test/spi_dma_test.cpp`:
```cpp
#include "Arduino.h"
#include "HardwareSerial.h"
#include "SPI.h"
#include "EventResponder.h"

static const int N = 16;
static DMAMEM uint8_t txbuf[N];    // DMA-accessed -> OCRAM (DTCM is DMA-unreachable)
static DMAMEM uint8_t rxbuf[N];
static DMAMEM uint8_t rxbuf2[N];
static EventResponder er;
static volatile bool async_cb_fired = false;
static void cb(EventResponderRef e) { async_cb_fired = true; }

void setup() {
	Serial1.begin(115200);
	while (!Serial1) {}
	for (int i = 0; i < N; i++) txbuf[i] = (uint8_t)(0xA0 ^ (i * 7));
	SPI.begin();
	SPI.beginTransaction(SPISettings(4000000, MSBFIRST, SPI_MODE0));
	bool ok = true;

	// STAGE_BLOCKING: full-duplex DMA; SDO->SDI loopback echoes tx into rx.
	for (int i = 0; i < N; i++) rxbuf[i] = 0;
	SPI.transfer(txbuf, rxbuf, N);
	bool b = true;
	for (int i = 0; i < N; i++) if (rxbuf[i] != txbuf[i]) b = false;
	Serial1.println(b ? "STAGE_BLOCKING=PASS" : "STAGE_BLOCKING=FAIL");
	if (!b) ok = false;

	// STAGE_ASYNC: full-duplex DMA; EventResponder fires on RX completion.
	for (int i = 0; i < N; i++) rxbuf2[i] = 0;
	er.attach(cb);
	async_cb_fired = false;
	bool started = SPI.transfer(txbuf, rxbuf2, N, er);
	uint32_t guard = 0;
	while (!async_cb_fired && ++guard < 2000000) yield();
	bool a = started && async_cb_fired;
	for (int i = 0; i < N; i++) if (rxbuf2[i] != txbuf[i]) a = false;
	Serial1.println(a ? "STAGE_ASYNC=PASS" : "STAGE_ASYNC=FAIL");
	if (!a) ok = false;

	SPI.endTransaction();
	Serial1.println(ok ? "SPI_DMA_ALL=PASS" : "SPI_DMA_ALL=FAIL");
}
void loop() {}
```

- [ ] **Step 3: Adapt the run script**

In `spi_dma_test/run_qemu_spidma.sh`: point it at `spi_dma_test.elf`; keep the QEMU/`qrun` invocation, machine flags, and output-file variable exactly as the copied script (match its actual variable name, e.g. `$OUT`); rename its output files (e.g. `spidma.uart`/`spidma.dbg`); change `sleep 3`→`sleep 5` (cold-start robustness, per the EventResponder gate); replace the grep/check block with:
```sh
grep -q "STAGE_BLOCKING=PASS" "$OUT" || { echo "FAIL: blocking"; exit 1; }
grep -q "STAGE_ASYNC=PASS"    "$OUT" || { echo "FAIL: async";    exit 1; }
grep -q "SPI_DMA_ALL=PASS"    "$OUT" || { echo "FAIL: overall";  exit 1; }
echo "PASS: SPI_DMA_ALL"
```

- [ ] **Step 4: Build + run the gate**

```bash
cd ~/Development/rt1170/evkb/spi_dma_test && rm -rf build \
  && cmake -B build -DCMAKE_TOOLCHAIN_FILE=toolchain/rt1170-evkb.toolchain.cmake . >/dev/null \
  && cmake --build build 2>&1 | tail -2 && ./run_qemu_spidma.sh 2>&1 | tail -10
```
Expected: `STAGE_BLOCKING=PASS`, `STAGE_ASYNC=PASS`, `SPI_DMA_ALL=PASS`, `PASS: SPI_DMA_ALL`. Run twice to confirm stability.

- [ ] **Step 5: CONTINGENCY — if the transfer doesn't drive correctly in QEMU**

If `STAGE_BLOCKING=FAIL` (rxbuf ≠ txbuf) or `STAGE_ASYNC` never completes (no RX-completion IRQ), the QEMU LPSPI model's *level* `dma_tx`/`dma_rx` don't correctly drive a finite full-duplex transfer through the eDMA hardware-request path. Diagnose and fix minimally in `~/Development/qemu2/hw/ssi/imxrt_lpspi.c`:
- Read `imxrt_lpspi_update_dma` (asserts `dma_tx` while `MEN && TDDE`; `dma_rx` while `RDDE && rx_fifo` non-empty) and the transfer/loopback path (a `TDR` write shifts SDO→SDI and pushes to `rx_fifo` unless `RXMSK`).
- Check whether the eDMA hardware-request bottom-half drains exactly `count` bytes per channel, and whether `dma_rx` re-asserts as each loopback byte lands so the RX channel drains all `count`. The likely gap mirrors the **SAI `tx_tick`** (level-vs-edge) / **eDMA `single_minor`** precedents ([[rt1176-edma-dmachannel]], [[rt1176-sai-rx]]): a level request that either over-drives (drains the whole major loop at once — usually fine for a finite transfer) or fails to pace TX vs the loopback→RX coupling.
- Apply the minimal fix, then rebuild QEMU with Homebrew clang:
  ```bash
  cd ~/Development/qemu2 && PATH=/usr/local/opt/llvm/bin:$PATH ninja -C build qemu-system-arm 2>&1 | tail -5
  ```
- If `imxrt_lpspi.c` is shared with the RT1062 machine, re-run the RT1062 functional suite and keep it all-green:
  ```bash
  cd ~/Development/qemu2 && QEMU_TEST_QEMU_BINARY=$PWD/build/qemu-system-arm \
    QEMU_TEST_ARM_GCC=/Applications/ARM_10/bin/arm-none-eabi-gcc MESON_BUILD_ROOT=$PWD/build \
    PYTHONPATH="$PWD/python:$PWD/tests/functional" build/pyvenv/bin/python3 \
    tests/functional/arm/test_imxrt1062.py 2>&1 | tail -20
  ```
- Re-run the gate (Step 4) until green. Commit the QEMU fix separately (see Step 6b). If NO QEMU change was needed, note that explicitly.

- [ ] **Step 6a: Commit the gate (evkb)**

```bash
cd ~/Development/rt1170/evkb
git add spi_dma_test/spi_dma_test.cpp spi_dma_test/CMakeLists.txt spi_dma_test/run_qemu_spidma.sh spi_dma_test/toolchain
git commit -m "spi_dma_test: QEMU gate (blocking + async full-duplex DMA) green

Co-Authored-By: Claude Opus 4.8 <noreply@anthropic.com>"
```

- [ ] **Step 6b: Commit any QEMU fix (qemu2), only if Step 5 changed it**

```bash
cd ~/Development/qemu2
git add hw/ssi/imxrt_lpspi.c
git commit -m "hw/ssi/imxrt_lpspi: <describe the finite full-duplex DMA fix>

Co-Authored-By: Claude Opus 4.8 <noreply@anthropic.com>"
```

---

## Task 4: Hardware verification (SDO→SDI jumper)

Requires the board + an external **SDO→SDI jumper** (GPIO_AD_30 → GPIO_AD_31), exactly as `spi_loopback_test` was HW-verified. **PAUSE for the human** to place the jumper; the controller then drives the scriptable flash + VCOM capture.

- [ ] **Step 1: Confirm the jumper is in place** (human: connect SDO `GPIO_AD_30` to SDI `GPIO_AD_31`).

- [ ] **Step 2: Flash + capture (controller-driven)**

```bash
LS=/Applications/LinkServer_26.6.137/LinkServer
PORT=/dev/cu.usbmodem5DQ2DDHVWO5EI3
OUT=<session-scratchpad>/spidma_vcom.txt
gtimeout 22 python3 -c "
import serial,sys
s=serial.Serial('$PORT',115200,timeout=1)
while True:
    l=s.readline()
    if l: sys.stdout.write(l.decode('utf-8','replace')); sys.stdout.flush()
" > \"$OUT\" 2>&1 &
CAP=$!
sleep 2
gtimeout 90 "$LS" flash MIMXRT1176:MIMXRT1170-EVKB load ~/Development/rt1170/evkb/spi_dma_test/build/spi_dma_test.elf 2>&1 | grep -iE 'finish|loaded|error' | tail -2
wait $CAP
cat "$OUT"
```

- [ ] **Step 3: Verify on silicon**

Expected VCOM: `STAGE_BLOCKING=PASS`, `STAGE_ASYNC=PASS`, `SPI_DMA_ALL=PASS`. This proves blocking and async full-duplex DMA (with the loopback echo `rxbuf==txbuf` and the async EventResponder callback firing) on real hardware. If a stage FAILs on silicon but passed in QEMU, investigate (DMAMEM/OCRAM reachability, the jumper, the RX-completion IRQ on hardware) before declaring done. No commit (verification only).

---

## Task 5: Memory note + push

**Files:**
- Create: `~/.claude/projects/-Users-nicholasnewdigate-Development-rt1170/memory/rt1176-spi-dma.md`
- Modify: memory `MEMORY.md`

- [ ] **Step 1: Write the memory note**

Capture: full-duplex DMA `transfer` (blocking `yield()`-spin + async `bool`/EventResponder) via 2 `DMAChannel`s (TX→TDR src37, RX RDR→rxbuf src36); `DER=TDDE|RDDE`, `TCR` FRAMESZ(7)=8-bit, `FCR=0`; RX-completion ISR = done (full-duplex RX-done==TX-done) → `_transfer_done`+`triggerEvent`; not re-entrant (async returns false if busy); **DMA buffers MUST be `DMAMEM`** (DTCM unreachable); `der`/`fcr` added to `hardware_t` (generator LPSPI1_DER@0x4011401C / FCR@0x40114058). Record whether the QEMU LPSPI model needed a fix (and what), or worked as-is. Link `[[rt1176-lpspi-spi]]`, `[[rt1176-edma-dmachannel]]`, `[[rt1176-eventresponder]]`. Add a one-line `MEMORY.md` pointer.

- [ ] **Step 2: Push (only when the user asks)**

```bash
cd ~/Development/rt1170/evkb/cores && git push origin master 2>&1 | tail -2   # teensy-cores
cd ~/Development/qemu2 && git push origin master 2>&1 | tail -2               # gitlab, only if Task 3 touched it
# evkb is local-only — nothing to push
```

---

## Self-Review

**Spec coverage:** §files→Tasks 1-3; §API (blocking + async signatures)→Task 2 Step 1-2; §data-flow (startDMA/rxisr/blocking-yield/async-bool)→Task 2 Step 2; §LPSPI config (FRAMESZ7/FCR0/DER)→Task 2 startDMA; §QEMU risk→Task 3 Step 5 contingency; §gate (blocking+async stages)→Task 3 Step 2; §HW (SDO→SDI jumper)→Task 4; memory/push→Task 5. All covered. The `DMAMEM` rule (implied by §HW correctness) is explicit in Task 3 Step 2.

**Placeholder scan:** No TBD/TODO; every code step has complete code; the only intentionally-open piece is the QEMU fix *body* (Task 3 Step 5), which is a genuine diagnose-then-fix contingency with concrete guidance + precedents, not a lazy placeholder — the commit message there has a `<describe…>` slot to fill with the actual fix. The `<session-scratchpad>` in Task 4 is a controller substitution, annotated.

**Type consistency:** `transfer(const void*,void*,size_t)` / `transfer(const void*,void*,size_t,EventResponder&)` / `startDMA(const void*,void*,size_t)` / `rxisr` / `_dmaTX`/`_dmaRX` / `_dma_event_responder` / `_transfer_done` / `der`/`fcr` / `DER_TDDE`/`DER_RDDE` / `DMAMUX_SOURCE_LPSPI1_RX`/`_TX` used consistently across SPI.h, SPI.cpp, SPI_instances.cpp, and the gate. `hardware_t` field order (der/fcr after rdr) matches between SPI.h (Task 1 Step 3) and the literal (Task 1 Step 4).
