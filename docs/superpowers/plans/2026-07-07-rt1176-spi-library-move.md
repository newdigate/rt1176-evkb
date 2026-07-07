# RT1176 SPI → newdigate/SPI library move Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Re-implement RT1176 SPI to the full Teensy SPI API as a `__IMXRT1176__` branch in the `newdigate/SPI` library (LPSPI1 only, full API + DMA/async), delete the core SPI, and move the SPI tests into the SPI repo so evkb no longer depends on it.

**Architecture:** A **hybrid** port — the public API surface + `SPI_Hardware_t` structure + async framework are adapted from the library's `__IMXRT1062__` (Teensy 4) branch; the RT1176 register/clock/pin **logic** is taken from our HW-verified core `cores/imxrt1176/SPI.{h,cpp}` (the 1062 `begin()`/clock/pin path is Teensy-4-specific and does NOT map to RT1176). NOT byte-identical → hardware is the arbiter.

**Tech Stack:** C++ (Teensy SPI library structure), MIMXRT1176-EVKB LPSPI1, `DMAChannel`/`EventResponder` (core), QEMU `mimxrt1170-evk` gates via `qrun`, `teensy-cmake-macros` build.

---

## Repos & conventions

- **Core** `~/Development/rt1170/evkb/cores/imxrt1176` (teensy-cores) — gains `IMXRT_LPSPI_t`+`IMXRT_LPSPI1_ADDRESS`; loses `SPI.{h,cpp}`+`SPI_instances.cpp`.
- **Library** `/Users/nicholasnewdigate/Development/SPI` (newdigate/SPI) — gains the `__IMXRT1176__` branch + `tests/`.
- **evkb** (local) — loses the three `spi_*` gate dirs.
- Commit to `master` each repo; **push only when the user asks** (Task 6). QEMU via `~/Development/rt1170/evkb/tools/qrun`.

## The two source files to merge (READ both)

- **API-shape template:** the library's `__IMXRT1062__` branch — `SPI.h` ~lines 1075-1435 (`class SPIClass // Teensy 4`, `SPI_Hardware_t`, the full public API), `SPI.cpp` ~lines 1269-2105 (method bodies, `initDMAChannels`, async `transfer(...,EventResponderRef)`, `dma_rxisr`, the `spiclass_lpspi*_hardware` tables, the `SPI`/`SPI1`/`SPI2` instances). **KEEP only the `SPI`=LPSPI1 instance.**
- **RT1176 logic (HW-verified):** the core `cores/imxrt1176/SPI.{h,cpp}` + `SPI_instances.cpp`. This has the CORRECT RT1176 bring-up: `begin()` = LPCG ungate (`CCM_LPCG104_DIRECT=1`) + clock root (`CCM_CLOCK_ROOT43_CONTROL=0`) + direct IOMUXC mux/pad/select writes; `setClockDivider` (CCR + TCR prescale); `transfer`/`transfer16` (TCR FRAMESZ + TDR/RSR/RDR polling); the DMA (`startDMA`/`rxisr` — RX-completion-as-done, `DER=TDDE|RDDE`, 8-bit FRAMESZ(7), `triggerAtHardwareEvent(DMAMUX_SOURCE_LPSPI1_{RX,TX})`).

**Rule for the merge:** take method *signatures* + the `SPI_Hardware_t`/async *structure* from the 1062 template; take every *register/clock/pin operation* from the core RT1176 source. Where the 1062 body does Teensy-4 clock/pin work (`CCM_CBCMR`, `portControlRegister`, `IOMUXC_PAD_DSE`), replace it with the core RT1176 equivalent. Where a Teensy-4 feature has no RT1176 analog (hardware PCS chip-select, `IRQ_GPIO6789` usingInterrupt), provide the minimal manual-CS behavior our lean driver already used (manual CS = plain GPIO; `usingInterrupt` a safe near-no-op).

## The RT1176 retarget data (source of truth = `SPI_instances.cpp` `lpspi1_hw`)

```
base       IMXRT_LPSPI1_ADDRESS = 0x40114000
clock      lpcg = CCM_LPCG104_DIRECT (write 1 to ungate);
           clock_root = CCM_CLOCK_ROOT43_CONTROL (write 0 => mux 0, 24 MHz); func_clock = 24000000
pins(ALT0) SCK  = GPIO_AD_28 (mux IOMUXC_SW_MUX_CTL_PAD_GPIO_AD_28=0x0, pad IOMUXC_SW_PAD_CTL_PAD_GPIO_AD_28)
           MOSI = GPIO_AD_30 (mux ..._AD_30=0x0, pad ..._AD_30)
           MISO = GPIO_AD_31 (mux ..._AD_31=0x0, pad ..._AD_31)
           pad_ctl_val = 0x0000000C  (push-pull, DSE; no SION, no pull)
daisy      IOMUXC_LPSPI1_SCK_SELECT_INPUT = 0x1, ..._SDO_SELECT_INPUT = 0x1, ..._SDI_SELECT_INPUT = 0x1
DMA        DMAMUX_SOURCE_LPSPI1_RX, DMAMUX_SOURCE_LPSPI1_TX; DER bits TDDE(1<<0)|RDDE(1<<1)
CS         PCS0/CS = GPIO_AD_29, left as plain GPIO (manual CS)
```

---

## Task 1: Core — `IMXRT_LPSPI_t` port struct + address

**Files:** Modify `cores/imxrt1176/imxrt1176.h` (additive).

- [ ] **Step 1: Add the port struct + address.** Near the existing flat `LPSPI1_*` defs in `imxrt1176.h`, add a register-block struct laid out to the LPSPI map (offsets confirmed against the flat defs: `CR@0x10,SR@0x14,IER@0x18,DER@0x1C,CFGR0@0x20,CFGR1@0x24,DMR0@0x2C,DMR1@0x30,CCR@0x40,FCR@0x58,FSR@0x5C,TCR@0x60,TDR@0x64,RSR@0x70,RDR@0x74`) and the base address:

```c
typedef struct {
	volatile uint32_t VERID;    // 0x00
	volatile uint32_t PARAM;    // 0x04
	volatile uint32_t unused08; // 0x08
	volatile uint32_t unused0C; // 0x0C
	volatile uint32_t CR;       // 0x10
	volatile uint32_t SR;       // 0x14
	volatile uint32_t IER;      // 0x18
	volatile uint32_t DER;      // 0x1C
	volatile uint32_t CFGR0;    // 0x20
	volatile uint32_t CFGR1;    // 0x24
	volatile uint32_t unused28; // 0x28
	volatile uint32_t DMR0;     // 0x2C
	volatile uint32_t DMR1;     // 0x30
	volatile uint32_t unused34; // 0x34
	volatile uint32_t unused38; // 0x38
	volatile uint32_t unused3C; // 0x3C
	volatile uint32_t CCR;      // 0x40
	volatile uint32_t unused44[5]; // 0x44..0x54
	volatile uint32_t FCR;      // 0x58
	volatile uint32_t FSR;      // 0x5C
	volatile uint32_t TCR;      // 0x60
	volatile uint32_t TDR;      // 0x64
	volatile uint32_t unused68; // 0x68
	volatile uint32_t unused6C; // 0x6C
	volatile uint32_t RSR;      // 0x70
	volatile uint32_t RDR;      // 0x74
} IMXRT_LPSPI_t;
#define IMXRT_LPSPI1_ADDRESS 0x40114000
```

- [ ] **Step 2: Sanity-check the offsets.** Confirm the struct member addresses equal the flat defs:

Run: `grep -nE '#define LPSPI1_(CR|SR|DER|CFGR1|CCR|FCR|TCR|TDR|RSR|RDR)\b' cores/imxrt1176/imxrt1176.h`
Expected: `CR=0x40114010, SR=0x14, DER=0x1C, CFGR1=0x24, CCR=0x40, FCR=0x58, TCR=0x60, TDR=0x64, RSR=0x70, RDR=0x74` — matching the struct offsets above (base 0x40114000).

- [ ] **Step 3: Build-check the core still compiles** (additive change; the core SPI still present + unaffected). Rebuild any existing gate that globs the core:

Run: `cd ~/Development/rt1170/evkb/spi_loopback_test && rm -rf build && cmake -B build -DCMAKE_TOOLCHAIN_FILE=toolchain/rt1170-evkb.toolchain.cmake . && cmake --build build 2>&1 | tail -2`
Expected: builds clean (the new struct is unused for now).

- [ ] **Step 4: Commit.**
```bash
cd ~/Development/rt1170/evkb/cores/imxrt1176
git add imxrt1176.h && git commit -m "imxrt1176: add IMXRT_LPSPI_t port struct + IMXRT_LPSPI1_ADDRESS (for the SPI library move)"
```

---

## Task 2: Library — `SPI.h` `__IMXRT1176__` declaration branch

**Files:** Modify `/Users/nicholasnewdigate/Development/SPI/SPI.h`.

- [ ] **Step 1: Widen the top include gate.** Change the gate that pulls in DMA/EventResponder + `SPI_HAS_TRANSFER_ASYNC` so RT1176 qualifies:
```cpp
#if (defined(__arm__) && defined(TEENSYDUINO)) || defined(__IMXRT1176__)
#if defined(__has_include) && __has_include(<EventResponder.h>)
#define SPI_HAS_TRANSFER_ASYNC 1
#include <DMAChannel.h>
#include <EventResponder.h>
#endif
#endif
```

- [ ] **Step 2: Add the `#elif defined(__IMXRT1176__)` SPIClass branch.** In the platform `SPIClass` chain (after the `__IMXRT1062__`/`__IMXRT1052__` branch's `#endif`-region, before the trailing `#endif`), add the RT1176 branch. It mirrors the 1062 public API but with a single-bus `SPI_Hardware_t` retargeted for RT1176 (LPCG+clock_root instead of `clock_gate_register/mask`; our IOMUXC select-input registers). Full declaration:

```cpp
#elif defined(__IMXRT1176__)
class SPIClass { // MIMXRT1176-EVKB — LPSPI1, full Teensy API over the RT1176 core driver
public:
	static const uint8_t CNT_MISO_PINS = 1;
	static const uint8_t CNT_MOSI_PINS = 1;
	static const uint8_t CNT_SCK_PINS = 1;
	static const uint8_t CNT_CS_PINS = 1;
	typedef struct {
		volatile uint32_t &lpcg;                 // LPSPI clock gate (write 1 to ungate)
		volatile uint32_t &clock_root;           // CCM clock root
		uint32_t clock_root_val;                 // value => mux/div (0 => 24 MHz)
		uint32_t func_clock;                     // resulting functional clock (Hz)
		void (*dma_rxisr)();
		const uint8_t  miso_pin[CNT_MISO_PINS];
		volatile uint32_t &miso_mux; uint32_t miso_mux_val; volatile uint32_t &miso_pad;
		volatile uint32_t &miso_select_input_register; uint32_t miso_select_val;
		const uint8_t  mosi_pin[CNT_MOSI_PINS];
		volatile uint32_t &mosi_mux; uint32_t mosi_mux_val; volatile uint32_t &mosi_pad;
		volatile uint32_t &mosi_select_input_register; uint32_t mosi_select_val;
		const uint8_t  sck_pin[CNT_SCK_PINS];
		volatile uint32_t &sck_mux; uint32_t sck_mux_val; volatile uint32_t &sck_pad;
		volatile uint32_t &sck_select_input_register; uint32_t sck_select_val;
		const uint8_t  cs_pin[CNT_CS_PINS];
		uint32_t pad_ctl_val;
	} SPI_Hardware_t;
	static const SPI_Hardware_t spiclass_lpspi1_hardware;

	SPIClass(uintptr_t myport, const SPI_Hardware_t &myhardware)
		: port_addr(myport), hardware(myhardware) {}

	void begin();
	void end();
	void usingInterrupt(uint8_t n) {}            // manual-CS core; no shared-IRQ guard needed
	void usingInterrupt(IRQ_NUMBER_t interruptName) {}
	void notUsingInterrupt(IRQ_NUMBER_t interruptName) {}
	void beginTransaction(SPISettings settings);
	void endTransaction();
	uint8_t  transfer(uint8_t data);
	uint16_t transfer16(uint16_t data);
	void     transfer(void *buf, size_t count);
	void     transfer(const void *buf, void *retbuf, size_t count);
	bool     transfer(const void *buf, void *retbuf, size_t count, EventResponderRef event_responder);
	void     setBitOrder(uint8_t bitOrder);
	void     setDataMode(uint8_t dataMode);
	void     setClockDivider(uint8_t clockDiv) {}   // legacy AVR API; unused (use SPISettings clock)
	uint8_t  setCS(uint8_t pin) { return 0; }        // manual CS (Arduino convention); no HW PCS
	void     setMOSI(uint8_t pin) {}                 // fixed LPSPI1 pins on the EVKB header
	void     setMISO(uint8_t pin) {}
	void     setSCK(uint8_t pin) {}
	bool     pinIsChipSelect(uint8_t pin) { return false; }
	bool     pinIsMOSI(uint8_t pin) { return pin == hardware.mosi_pin[0]; }
	bool     pinIsMISO(uint8_t pin) { return pin == hardware.miso_pin[0]; }
	bool     pinIsSCK(uint8_t pin)  { return pin == hardware.sck_pin[0]; }

	IMXRT_LPSPI_t & port() { return *(IMXRT_LPSPI_t *)port_addr; }
private:
	uintptr_t port_addr;
	const SPI_Hardware_t &hardware;
	uint32_t tcr_base = 0;
	DMAChannel *_dmaTX = nullptr;
	DMAChannel *_dmaRX = nullptr;
	EventResponder *_dma_event_responder = nullptr;
	volatile bool _transfer_done = true;
	void setClockDividerHz(uint32_t clockHz);
	void startDMA(const void *txbuf, void *rxbuf, size_t count);
	static void dma_rxisr();
};
extern SPIClass SPI;
```
(NOTE the API-vs-logic split: `setMOSI/MISO/SCK`, `setCS`, `usingInterrupt`, `pinIsChipSelect` are minimal because the EVKB uses fixed LPSPI1 pins + manual CS — they exist for source-compatibility with SPI libraries. The real work is `begin`/`beginTransaction`/`transfer`×forms/the DMA, taken from the core driver.)

- [ ] **Step 3: Confirm `SPISettings` + the `SPI_MODE*`/`MSBFIRST` defines are visible to the RT1176 branch.** The library defines these in its common (pre-branch) section; verify the RT1176 branch sees a `SPISettings(clock,bitOrder,dataMode)` with `.clock/.bitOrder/.dataMode` (matching what `beginTransaction` will read). If the library's shared `SPISettings` differs, adapt `beginTransaction` in Task 3 to its accessors.

- [ ] **Step 4: Header-compile check.**

Run: `cd /Users/nicholasnewdigate/Development/SPI && arm-none-eabi-g++ -std=gnu++17 -mcpu=cortex-m7 -mfpu=fpv5-d16 -mfloat-abi=hard -D__IMXRT1176__ -I. -I$HOME/Development/rt1170/evkb/cores/imxrt1176 -fsyntax-only SPI.h 2>&1 | head -20`
Expected: no errors from the `__IMXRT1176__` branch (Arduino.h/type resolution warnings from the host are fine; look for real syntax errors in the new branch). Fix any until clean.

- [ ] **Step 5: Commit** (with Task 3 — the branch isn't functional until the .cpp lands; but committing the header alone is fine since it's guarded).
```bash
cd /Users/nicholasnewdigate/Development/SPI && git add SPI.h && git commit -m "SPI.h: add __IMXRT1176__ SPIClass declaration branch (full API, LPSPI1)"
```

---

## Task 3: Library — `SPI.cpp` `__IMXRT1176__` implementation branch

**Files:** Modify `/Users/nicholasnewdigate/Development/SPI/SPI.cpp`.

Add a `#elif defined(__IMXRT1176__)` branch implementing every method declared in Task 2, taking the register/clock/pin logic **verbatim from the core `cores/imxrt1176/SPI.cpp`** (HW-verified) and wrapping it in the library's method shapes. The `port()` accessor replaces the core's `hw->` field refs (e.g. core `hw->tcr` → `port().TCR`; core `hw->rdr` → `port().RDR`).

- [ ] **Step 1: `begin()` + `end()`** — from the core `SPIClass::begin()`/`end()`: ungate `hardware.lpcg=1`; `hardware.clock_root=hardware.clock_root_val`; write the three pin muxes/pads (`hardware.sck_mux=hardware.sck_mux_val; hardware.sck_pad=hardware.pad_ctl_val;` etc.) + the three `*_select_input_register=*_select_val`; `port().CR=CR_RST; port().CR=0; port().CFGR1=CFGR1_MASTER; tcr_base=0; setClockDividerHz(4000000); port().CR=CR_MEN;`. `end()`: `port().CR=0; hardware.lpcg=0;`.

- [ ] **Step 2: `setClockDividerHz`/`beginTransaction`/`endTransaction`/`setBitOrder`/`setDataMode`** — from the core's `setClockDivider`/`beginTransaction` (CCR SCKDIV + TCR prescale search; `beginTransaction` sets CPOL/CPHA/LSBF into `tcr_base` from `SPISettings` then calls `setClockDividerHz(s.clock)`). `endTransaction()` = no-op (manual CS). `setBitOrder`/`setDataMode` update `tcr_base` bits.

- [ ] **Step 3: `transfer`/`transfer16`/`transfer(buf,count)`** — from the core: `port().TCR = tcr_base | TCR_FRAMESZ(7/15); port().TDR = data; poll !(port().RSR & RSR_RXEMPTY) → return port().RDR;` (with the `SPI_TIMEOUT` guard). `transfer(buf,count)` loops byte-wise.

- [ ] **Step 4: The DMA — `transfer(buf,retbuf,count)` [blocking] + `transfer(...,EventResponderRef)` [async] + `startDMA` + `dma_rxisr`** — from the core's `transfer(tx,rx,count)`/`transfer(tx,rx,count,EventResponder&)`/`startDMA`/`rxisr` verbatim (RX-completion-as-done; `port().DER=DER_TDDE|DER_RDDE`; 8-bit FRAMESZ(7); `_dmaRX->source(*(volatile uint8_t*)&port().RDR)`, `_dmaTX->destination(*(volatile uint8_t*)&port().TDR)`; `triggerAtHardwareEvent(DMAMUX_SOURCE_LPSPI1_{RX,TX})`; `count==0||count>32767` guard). Keep the `static SPIClass *dma_active_spi` for the ISR. The async form takes `EventResponderRef` (the library's typedef) — store `&event_responder`, `triggerEvent()` on completion.

- [ ] **Step 5: The hardware table + instance** (copy the values from `SPI_instances.cpp` `lpspi1_hw`, mapped to the new struct field names):
```cpp
const SPIClass::SPI_Hardware_t SPIClass::spiclass_lpspi1_hardware = {
	CCM_LPCG104_DIRECT, CCM_CLOCK_ROOT43_CONTROL, 0u, 24000000u, SPIClass::dma_rxisr,
	{ /*MISO pin*/ 0 }, IOMUXC_SW_MUX_CTL_PAD_GPIO_AD_31, 0x0u, IOMUXC_SW_PAD_CTL_PAD_GPIO_AD_31,
		IOMUXC_LPSPI1_SDI_SELECT_INPUT, 0x1u,
	{ /*MOSI pin*/ 0 }, IOMUXC_SW_MUX_CTL_PAD_GPIO_AD_30, 0x0u, IOMUXC_SW_PAD_CTL_PAD_GPIO_AD_30,
		IOMUXC_LPSPI1_SDO_SELECT_INPUT, 0x1u,
	{ /*SCK pin*/ 0 }, IOMUXC_SW_MUX_CTL_PAD_GPIO_AD_28, 0x0u, IOMUXC_SW_PAD_CTL_PAD_GPIO_AD_28,
		IOMUXC_LPSPI1_SCK_SELECT_INPUT, 0x1u,
	{ /*CS pin*/ 0 },
	0x0000000Cu,
};
SPIClass SPI(IMXRT_LPSPI1_ADDRESS, SPIClass::spiclass_lpspi1_hardware);
```
(The `*_pin[]` values are the EVKB Arduino pin numbers for AD_28/30/31 — set them to the correct digital-pin numbers per the core's pin table, or 0 if unused by the fixed-pin `begin()`; `begin()` writes the muxes directly from the register refs, so the pin numbers only matter to `pinIsMOSI/…`. Use the real pin numbers if the pin table defines them; otherwise document them as nominal.)

- [ ] **Step 6: Register the CR/CFGR1/TCR/RSR/DER field macros** the branch uses (`CR_MEN/CR_RST`, `CFGR1_MASTER`, `TCR_FRAMESZ/PRESCALE/CPHA/CPOL/LSBF`, `RSR_RXEMPTY`, `DER_TDDE/RDDE`, `SPI_TIMEOUT`) — define them file-locally at the top of the `__IMXRT1176__` branch (copy from the core `SPI.cpp` top). Do not rely on Teensy `LPSPI_*` macros.

- [ ] **Step 7: Standalone `-c` compile of the whole branch.**

Run: `cd /Users/nicholasnewdigate/Development/SPI && arm-none-eabi-g++ -std=gnu++17 -mcpu=cortex-m7 -mfpu=fpv5-d16 -mfloat-abi=hard -D__IMXRT1176__ -I. -I$HOME/Development/rt1170/evkb/cores/imxrt1176 -c SPI.cpp -o /tmp/spi_1176.o 2>&1 | head -30`
Expected: compiles to an object with no errors from the `__IMXRT1176__` branch. (It won't LINK standalone — `DMAChannel`/`EventResponder`/register symbols resolve at the gate link in Task 4. `-c` catches syntax + type + macro-spelling errors, which is the goal here.)

- [ ] **Step 8: Commit.**
```bash
cd /Users/nicholasnewdigate/Development/SPI && git add SPI.cpp && git commit -m "SPI.cpp: add __IMXRT1176__ impl branch (LPSPI1 begin/transfer/DMA over the RT1176 core logic)"
```

---

## Task 4: Move the tests into the SPI repo + delete the core SPI + delete evkb gates (integration)

The first real compile+link+run of the library branch. Atomic: after this, no gate references the core SPI, and evkb has no SPI gate.

**Files:** Create `newdigate/SPI/tests/{spi_loopback_test,spi_dma_test,st7735_test}/*`; delete `cores/imxrt1176/SPI.{h,cpp}`+`SPI_instances.cpp`; delete `evkb/{spi_loopback_test,spi_dma_test,st7735_test}/`.

- [ ] **Step 1: Move the three gate dirs into the SPI repo.**
```bash
mkdir -p /Users/nicholasnewdigate/Development/SPI/tests
for g in spi_loopback_test spi_dma_test st7735_test; do
  cp -R ~/Development/rt1170/evkb/$g /Users/nicholasnewdigate/Development/SPI/tests/$g
  rm -rf /Users/nicholasnewdigate/Development/SPI/tests/$g/build
done
```

- [ ] **Step 2: Repoint each moved gate's `CMakeLists.txt`.** They currently `import_arduino_library(cores ${CMAKE_CURRENT_LIST_DIR}/../cores/imxrt1176)` and FetchContent `../teensy-cmake-macros`. Update the relative paths to the evkb checkout locations and add the SPI library import so `#include <SPI.h>` resolves to THIS repo (not the deleted core). Example for each:
```cmake
set(EVKB $ENV{HOME}/Development/rt1170/evkb)
FetchContent_Declare(teensy_cmake_macros SOURCE_DIR ${EVKB}/teensy-cmake-macros)
# ... FetchContent_MakeAvailable + toolchain as before ...
import_arduino_library(cores ${EVKB}/cores/imxrt1176)
import_arduino_library(SPI ${CMAKE_CURRENT_LIST_DIR}/../..)   # this SPI repo
teensy_add_executable(<name> <name>.cpp)
```
Keep each `toolchain/` dir + `run_qemu_*.sh` (update the `DIR`/ELF paths if they were absolute). The firmware `.cpp` files are unchanged (they already `#include "SPI.h"`/`<SPI.h>`, now resolving to the library).

- [ ] **Step 3: Delete the core SPI + the evkb gate dirs.**
```bash
cd ~/Development/rt1170/evkb/cores/imxrt1176 && git rm SPI.h SPI.cpp SPI_instances.cpp
rm -rf ~/Development/rt1170/evkb/{spi_loopback_test,spi_dma_test,st7735_test}
```

- [ ] **Step 4: Build + run all three gates from their new home.**
```bash
for g in spi_loopback_test spi_dma_test st7735_test; do
  echo "== $g =="
  cd /Users/nicholasnewdigate/Development/SPI/tests/$g && rm -rf build && \
    cmake -B build -DCMAKE_TOOLCHAIN_FILE=toolchain/rt1170-evkb.toolchain.cmake . && \
    cmake --build build 2>&1 | tail -2 && ./run_qemu_*.sh 2>&1 | tail -6
done
```
Expected: all three build (linking the library's `__IMXRT1176__` SPI + the core) and their QEMU gates PASS as they did in evkb — `spi_loopback_test` loopback echo, `spi_dma_test` DMA + async `EventResponder` completion, `st7735_test` the command/data stream. **A build error points back to Task 2/3** (a wrong retarget value, a missing macro, an API mismatch) — fix there, do not weaken the gate. Do NOT alter the assertions.

- [ ] **Step 5: Commit (two repos).**
```bash
cd /Users/nicholasnewdigate/Development/SPI && git add tests && git commit -m "tests: move SPI QEMU gates into the SPI repo (self-testing; no evkb->SPI dep)"
cd ~/Development/rt1170/evkb/cores/imxrt1176 && git commit -m "SPI: remove core SPI driver (moved to newdigate/SPI library)"
# evkb gate-dir deletions are local (evkb is not a tracked-push repo for these); note them.
```

---

## Task 5: Hardware verification (PAUSE for the user)

Re-implementation → HW is the arbiter. Controller drives scriptable flash+VCOM; the user sets the jumper / watches the display.

**Files:** none (uses the Task 4 ELFs).

- [ ] **Step 1: `spi_loopback_test` on HW** — flash `tests/spi_loopback_test/build/*.elf`; with the SDO→SDI jumper (GPIO_AD_30→AD_31) the transferred bytes echo back. Capture VCOM; expect the loopback PASS (per [[rt1176-lpspi-spi]]). PAUSE: user confirms/sets the jumper.
- [ ] **Step 2: `spi_dma_test` on HW** — flash; DMA full-duplex transfer completes + the async `EventResponder` callback fires (per [[rt1176-spi-dma]], the SDO→SDI jumper).
- [ ] **Step 3: `st7735_test` on HW** — flash; a real ST7735 display renders (the full-API + transfer16 exercise). PAUSE: user watches the display.
- [ ] **Step 4: Record HW results** for the memory note. If anything fails, the retarget surface (clock root/LPCG, pin mux/pad/select, DMAMUX, IRQ) is the prime suspect — cross-check against the (now-deleted, in git history) `SPI_instances.cpp` values.

---

## Task 6: Memory notes + push

**Files:** memory notes.

- [ ] **Step 1: Update notes.** `rt1176-lpspi-spi.md` + `rt1176-spi-dma.md` → SPI now lives in the `newdigate/SPI` library (`__IMXRT1176__` branch, full Teensy API), not the core; the gates moved into `newdigate/SPI/tests/`; the RT1176 register/clock/pin values are unchanged (re-expressed via the library `SPI_Hardware_t`). Add `rt1176-spi-library-move.md` (+ a `MEMORY.md` pointer): the hybrid port (1062 API shape + our RT1176 logic), `IMXRT_LPSPI_t` added to the core, core SPI deleted, tests-with-library pattern (no evkb→SPI), HW-verified via loopback+DMA+st7735.

- [ ] **Step 2: Push when the user asks.** Confirm the unpushed sets, then push both:
```bash
cd ~/Development/rt1170/evkb/cores/imxrt1176 && git log --oneline origin/master..master   # struct add + SPI deletion
cd /Users/nicholasnewdigate/Development/SPI && git log --oneline origin/master..master     # the __IMXRT1176__ branch + tests
```
On go: `git push origin master` in the cores tree (newdigate/teensy-cores) + `/Users/nicholasnewdigate/Development/SPI` (newdigate/SPI). evkb is local (gate-dir deletions). Report the pushed ranges.

---

## Self-review notes (author)

- **Spec coverage:** `IMXRT_LPSPI_t`+address → Task 1; full-API `__IMXRT1176__` branch (decl+impl) → Tasks 2-3; include-gate widen → Task 2 Step 1; core SPI deletion + tests-move + evkb-gate deletion → Task 4; HW loopback/DMA/st7735 → Task 5; memory+push → Task 6. SPI1/SPI2 correctly absent (deferred).
- **Hybrid split is explicit:** API shape/`SPI_Hardware_t`/async from the 1062 template; every register/clock/pin op from the core `SPI.{h,cpp}` (the 1062 `begin()` is Teensy-4-specific and must NOT be copied). Called out in the architecture + Task 3.
- **Ordering / green builds:** Task 1 additive (core SPI still present); Task 2-3 add a dormant guarded branch (verified by `-c` compile only — it can't link until Task 4); Task 4 atomically deletes the core SPI + repoints the (moved) tests so no build ever sees two `SPIClass` definitions.
- **Type consistency:** `IMXRT_LPSPI_t`/`port()`, `SPI_Hardware_t` field names used identically in Task 2 (decl) + Task 3 (table/impl); `spiclass_lpspi1_hardware`, `EventResponderRef`, the `CR_MEN/CFGR1_MASTER/TCR_*/RSR_RXEMPTY/DER_*` macros consistent across Tasks 2-3.
- **Verification honesty:** not byte-identical, so no diff check — the QEMU gates (Task 4) + HW (Task 5) are the sole regression check; every retarget value is cross-checked against the HW-verified `SPI_instances.cpp` (still in git history after deletion).
