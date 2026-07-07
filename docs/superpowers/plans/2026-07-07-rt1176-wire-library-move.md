# RT1176 Wire → library move (full Teensy API) Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Move Wire (I²C) out of the core into the `newdigate/Wire` library, re-implemented to the full Teensy `TwoWire : public Stream` API in a dedicated `WireIMXRT1176.{h,cpp}`, keeping our HW-verified polled-master / interrupt-slave register logic; relocate the Wire test gates into the Wire repo; leave the codec/display consumers in evkb with an added Wire import.

**Architecture:** Hybrid port (identical strategy to the just-completed SPI move): the **API shape + `Stream` derivation + hardware-table structure** come from the library's `__IMXRT1062__` `WireIMXRT` branch; **every register/clock/pin/slave-ISR operation** comes verbatim-in-behavior from our HW-verified core `cores/imxrt1176/Wire.{h,cpp}` + `Wire_instances.cpp` (flat `hw->mcr` refs → `port().MCR` overlay fields via a new core `IMXRT_LPI2C_t`). Not byte-identical → the QEMU gates + hardware are the regression check (no diff). Ordering is load-bearing so no compiled test ever sees two `TwoWire` definitions.

**Tech Stack:** C++ (arm-none-eabi-g++), i.MX RT1176 LPI2C (same IP family as Teensy-4), `teensy-cmake-macros` build, QEMU `mimxrt1170-evk` machine via `evkb/tools/qrun`, three repos — core = `teensy-cores` (`cores/imxrt1176`), Wire = `newdigate/Wire` (`/Users/nicholasnewdigate/Development/Wire`), evkb (local).

**Source-of-truth files to read before starting (all still present until Task 4 deletes the core Wire):**
- `cores/imxrt1176/Wire.h`, `Wire.cpp`, `Wire_instances.cpp` — the RT1176 logic + exact `lpi2c{1,2,5}_hw` values being re-expressed.
- `/Users/nicholasnewdigate/Development/Wire/Wire.h` (platform dispatcher), `WireIMXRT.h` (the `__IMXRT1062__` full-API `TwoWire : public Stream` decl to adapt in shape).
- `/Users/nicholasnewdigate/Development/SPI/SPI.h` (lines 1426–1529, the `__IMXRT1176__` decl branch) + `SPI.cpp` (lines 2109+, the `__IMXRT1176__` impl branch) + `SPI/tests/spi_loopback_test/` (CMake + toolchain + run-script) — the precedent template this plan mirrors.
- Memory notes `rt1176-lpi2c-wire`, `rt1176-spi-library-move`.

**Repo conventions:** commit to `master` in each repo; **push only when the user asks** (Task 6). qemu2 is UNCHANGED (the LPI2C model incl. the `addr_nacked` timing fix is already in). Cross-check every retarget value against `Wire_instances.cpp`.

---

## Task 1: Core — add `IMXRT_LPI2C_t` struct + `IMXRT_LPI2C1/2/5_ADDRESS`

Additive: the core Wire still builds against the flat `LPI2C*_*` defines; this only adds the port-struct overlay the library will use. `imxrt1176.h` is **auto-generated**, so the struct must be added to **both** the generator and the header (the SPI move did exactly this for `IMXRT_LPSPI_t`).

**Files:**
- Modify: `cores/imxrt1176/tools/gen_imxrt1176_h.py` (emit `IMXRT_LPI2C_t` + the 3 ADDRESS defines, next to the existing `IMXRT_LPSPI_t` block around line 296–329)
- Modify: `cores/imxrt1176/imxrt1176.h` (the generated output — regenerate, or hand-mirror the same block)

- [ ] **Step 1: Locate the `IMXRT_LPSPI_t` emission block in the generator**

Run: `grep -n 'IMXRT_LPSPI_t\|IMXRT_LPSPI1_ADDRESS' cores/imxrt1176/tools/gen_imxrt1176_h.py`
Expected: the `L += ['''...''']` triple-quoted block ending `} IMXRT_LPSPI_t;` / `#define IMXRT_LPSPI1_ADDRESS 0x40114000` (around lines 296–329).

- [ ] **Step 2: Add the `IMXRT_LPI2C_t` block in the generator**

Immediately after the `IMXRT_LPSPI_t` block's `L += [...]`, append a new `L += ['''...''']` block with this exact content. Offsets are cross-checked against the flat `LPI2C1_*` defines (MCR@+0x10 … MRDR@+0x70, slave SCR@+0x110 … SRDR@+0x170) and match the master/slave gap in the RT1176 LPI2C map. `sizeof` = `0x174`.

```python
    L += ['''
/* LPI2C register-block overlay (for the newdigate/Wire library's port()
 * accessor).  Layout matches cores/teensy4/imxrt.h IMXRT_LPI2C_t; verified
 * against the flat LPI2C1_* offsets above.  Master block 0x10..0x70, then a
 * gap, slave block 0x110..0x170.  Same layout at all 3 bases (LPI2C1
 * 0x40104000, LPI2C2 0x40108000, LPI2C5 0x40C34000). */
typedef struct {
	volatile uint32_t VERID;        // 0x00
	volatile uint32_t PARAM;        // 0x04
	volatile uint32_t unused08;     // 0x08
	volatile uint32_t unused0C;     // 0x0C
	volatile uint32_t MCR;          // 0x10
	volatile uint32_t MSR;          // 0x14
	volatile uint32_t MIER;         // 0x18
	volatile uint32_t MDER;         // 0x1C
	volatile uint32_t MCFGR0;       // 0x20
	volatile uint32_t MCFGR1;       // 0x24
	volatile uint32_t MCFGR2;       // 0x28
	volatile uint32_t MCFGR3;       // 0x2C
	volatile uint32_t unused30[4];  // 0x30..0x3C
	volatile uint32_t MDMR;         // 0x40
	volatile uint32_t unused44;     // 0x44
	volatile uint32_t MCCR0;        // 0x48
	volatile uint32_t unused4C;     // 0x4C
	volatile uint32_t MCCR1;        // 0x50
	volatile uint32_t unused54;     // 0x54
	volatile uint32_t MFCR;         // 0x58
	volatile uint32_t MFSR;         // 0x5C
	volatile uint32_t MTDR;         // 0x60
	volatile uint32_t unused64[3];  // 0x64..0x6C
	volatile uint32_t MRDR;         // 0x70
	volatile uint32_t unused74[39]; // 0x74..0x10C
	volatile uint32_t SCR;          // 0x110
	volatile uint32_t SSR;          // 0x114
	volatile uint32_t SIER;         // 0x118
	volatile uint32_t SDER;         // 0x11C
	volatile uint32_t unused120;    // 0x120
	volatile uint32_t SCFGR1;       // 0x124
	volatile uint32_t SCFGR2;       // 0x128
	volatile uint32_t unused12C[5]; // 0x12C..0x13C
	volatile uint32_t SAMR;         // 0x140
	volatile uint32_t unused144[3]; // 0x144..0x14C
	volatile uint32_t SASR;         // 0x150
	volatile uint32_t unused154[3]; // 0x154..0x15C
	volatile uint32_t STDR;         // 0x160
	volatile uint32_t unused164[3]; // 0x164..0x16C
	volatile uint32_t SRDR;         // 0x170
} IMXRT_LPI2C_t;
#define IMXRT_LPI2C1_ADDRESS 0x40104000
#define IMXRT_LPI2C2_ADDRESS 0x40108000
#define IMXRT_LPI2C5_ADDRESS 0x40C34000
'''.rstrip("\n")]
```

- [ ] **Step 3: Regenerate `imxrt1176.h`**

The generator writes the header **in place** (`OUT = <repo>/imxrt1176.h`) and prints `wrote …` — do **not** redirect stdout (that would clobber the header with the log line).
Run: `cd cores/imxrt1176 && python3 tools/gen_imxrt1176_h.py`
Expected: prints `wrote /…/cores/imxrt1176/imxrt1176.h`.
Then: `grep -n 'IMXRT_LPI2C_t\|IMXRT_LPI2C1_ADDRESS\|IMXRT_LPI2C5_ADDRESS' imxrt1176.h`
Expected: the struct + all three `#define`s present, `IMXRT_LPI2C1_ADDRESS 0x40104000`, `IMXRT_LPI2C5_ADDRESS 0x40C34000`.

> The generator reads the NXP SDK header at `~/Development/nxp/mcux-devices-rt/RT1170/MIMXRT1176/MIMXRT1176_cm7_COMMON.h`. If that path is absent (generator errors out), fall back to hand-editing `imxrt1176.h`: insert the identical `IMXRT_LPI2C_t` block + the three `#define`s right after `#define IMXRT_LPSPI1_ADDRESS 0x40114000`, keeping the generator's `L += ['''…''']` block in sync so the next regeneration reproduces it.

- [ ] **Step 4: Verify the struct offsets by static assertion (throwaway check)**

Create `/tmp/lpi2c_offsets.c`:
```c
#include <stddef.h>
#include <stdint.h>
#include "imxrt1176.h"
_Static_assert(offsetof(IMXRT_LPI2C_t, MCR)    == 0x10,  "MCR");
_Static_assert(offsetof(IMXRT_LPI2C_t, MSR)    == 0x14,  "MSR");
_Static_assert(offsetof(IMXRT_LPI2C_t, MIER)   == 0x18,  "MIER");
_Static_assert(offsetof(IMXRT_LPI2C_t, MCFGR1) == 0x24,  "MCFGR1");
_Static_assert(offsetof(IMXRT_LPI2C_t, MCCR0)  == 0x48,  "MCCR0");
_Static_assert(offsetof(IMXRT_LPI2C_t, MFCR)   == 0x58,  "MFCR");
_Static_assert(offsetof(IMXRT_LPI2C_t, MTDR)   == 0x60,  "MTDR");
_Static_assert(offsetof(IMXRT_LPI2C_t, MRDR)   == 0x70,  "MRDR");
_Static_assert(offsetof(IMXRT_LPI2C_t, SCR)    == 0x110, "SCR");
_Static_assert(offsetof(IMXRT_LPI2C_t, SSR)    == 0x114, "SSR");
_Static_assert(offsetof(IMXRT_LPI2C_t, SIER)   == 0x118, "SIER");
_Static_assert(offsetof(IMXRT_LPI2C_t, SCFGR1) == 0x124, "SCFGR1");
_Static_assert(offsetof(IMXRT_LPI2C_t, SCFGR2) == 0x128, "SCFGR2");
_Static_assert(offsetof(IMXRT_LPI2C_t, SAMR)   == 0x140, "SAMR");
_Static_assert(offsetof(IMXRT_LPI2C_t, SASR)   == 0x150, "SASR");
_Static_assert(offsetof(IMXRT_LPI2C_t, STDR)   == 0x160, "STDR");
_Static_assert(offsetof(IMXRT_LPI2C_t, SRDR)   == 0x170, "SRDR");
int main(void){return 0;}
```
Run: `arm-none-eabi-gcc -c -I cores/imxrt1176 /tmp/lpi2c_offsets.c -o /tmp/lpi2c_offsets.o` (use `/Applications/ARM_10/bin/arm-none-eabi-gcc`).
Expected: compiles clean (no `_Static_assert` failure). Delete `/tmp/lpi2c_offsets.*` after.

- [ ] **Step 5: Confirm the core still builds (additive change)**

Rebuild an existing non-Wire gate that pulls the core, e.g. the blink gate:
Run: `cd /Users/nicholasnewdigate/Development/rt1170/evkb/wire_master_test && cmake --build build` (this gate still uses the core Wire, so it re-exercises the whole core incl. the regenerated header).
Expected: builds clean — the new struct is additive and unused by the core Wire.

- [ ] **Step 6: Commit (teensy-cores)**

```bash
cd /Users/nicholasnewdigate/Development/rt1170/evkb/cores/imxrt1176
git add imxrt1176.h tools/gen_imxrt1176_h.py
git commit -m "feat(imxrt1176): add IMXRT_LPI2C_t + LPI2C1/2/5 base addresses

Register-block overlay for the newdigate/Wire library's port() accessor
(same pattern as IMXRT_LPSPI_t for the SPI move). Master block 0x10..0x70,
slave block 0x110..0x170, uniform at LPI2C1 0x40104000 / LPI2C2 0x40108000
/ LPI2C5 0x40C34000. Additive; core Wire unaffected. Added to both the
generator and the generated header.

Co-Authored-By: Claude Opus 4.8 <noreply@anthropic.com>"
```

---

## Task 2: Library — `Wire.h` dispatch arm + `WireIMXRT1176.h` (declaration, dormant)

Create the new platform file's **header** and wire it into the dispatcher. The branch is dormant (evkb still uses the core Wire until Task 4), so verify by a standalone compile with `-D__IMXRT1176__` + the core include path.

**Files:**
- Modify: `/Users/nicholasnewdigate/Development/Wire/Wire.h` (add the dispatch arm)
- Create: `/Users/nicholasnewdigate/Development/Wire/WireIMXRT1176.h`

- [ ] **Step 1: Add the RT1176 dispatch arm to `Wire.h`**

In `/Users/nicholasnewdigate/Development/Wire/Wire.h`, the current top chain is:
```cpp
#if defined(__IMXRT1052__) || defined(__IMXRT1062__)
#include "WireIMXRT.h"

#elif defined(__arm__) && defined(TEENSYDUINO)
#include "WireKinetis.h"

#elif defined(__AVR__)
...
```
Insert a new arm **before** the `__arm__ && TEENSYDUINO` arm (RT1176 is not `TEENSYDUINO`, so it must be caught first):
```cpp
#if defined(__IMXRT1052__) || defined(__IMXRT1062__)
#include "WireIMXRT.h"

#elif defined(__IMXRT1176__)
#include "WireIMXRT1176.h"

#elif defined(__arm__) && defined(TEENSYDUINO)
#include "WireKinetis.h"

#elif defined(__AVR__)
...
```

- [ ] **Step 2: Create `WireIMXRT1176.h`**

Full declaration — full Teensy API surface (shape adapted from `WireIMXRT.h`) over our engine's state. Register access is via `port()` over `IMXRT_LPI2C_t`; pins/clock/irq via a lean `I2C_Hardware_t` (only the fields our engine's `begin()` touches — the per-register refs are gone, replaced by `port()`).

```cpp
#ifndef TwoWireIMXRT1176_h
#define TwoWireIMXRT1176_h

#if defined(__IMXRT1176__)

#include <stdint.h>
#include <stddef.h>
#include "imxrt1176.h"
#include "core_pins.h"   // IRQ_NUMBER_t, attachInterruptVector, NVIC_*
#include "Stream.h"

#define BUFFER_LENGTH 32
#define WIRE_HAS_END 1
#define WIRE_IMPLEMENT_WIRE
#define WIRE_IMPLEMENT_WIRE1
#define WIRE_IMPLEMENT_WIRE2

class TwoWire : public Stream {
public:
	// Hardware description: clock gate + clock root + the SCL/SDA pad config
	// and the IRQ. Registers themselves are reached through port() over the
	// IMXRT_LPI2C_t overlay, so no per-register refs live here.
	typedef struct {
		volatile uint32_t &lpcg;                 // CCM->LPCG[n].DIRECT (write 1 to ungate)
		volatile uint32_t &clock_root;           // CCM->CLOCK_ROOT[n].CONTROL
		uint32_t clock_root_val;                 // 0 => 24 MHz (OscRC48MDiv2)
		volatile uint32_t &scl_mux;  uint32_t scl_mux_val;  volatile uint32_t &scl_pad;
		volatile uint32_t &sda_mux;  uint32_t sda_mux_val;  volatile uint32_t &sda_pad;
		volatile uint32_t &scl_select_input; uint32_t scl_select_val;
		volatile uint32_t &sda_select_input; uint32_t sda_select_val;
		uint32_t pad_ctl_val;                    // open-drain pad config
		IRQ_NUMBER_t irq;
		void (*irq_function)(void);
		uint16_t irq_priority;
	} I2C_Hardware_t;
	static const I2C_Hardware_t lpi2c1_hardware;
	static const I2C_Hardware_t lpi2c2_hardware;
	static const I2C_Hardware_t lpi2c5_hardware;

	TwoWire(uintptr_t myport, const I2C_Hardware_t &myhardware)
		: port_addr(myport), hardware(myhardware) {}

	void begin();
	void begin(uint8_t address);           // slave mode
	void begin(int address) { begin((uint8_t)address); }
	void end();
	void setClock(uint32_t frequency);
	void setSDA(uint8_t pin) {}             // fixed pin per bus; no-op (parity)
	void setSCL(uint8_t pin) {}

	void beginTransmission(uint8_t address) { tx_addr = address; tx_len = 0; }
	void beginTransmission(int address) { beginTransmission((uint8_t)address); }
	uint8_t endTransmission(uint8_t sendStop);
	uint8_t endTransmission(void) { return endTransmission((uint8_t)1); }

	uint8_t requestFrom(uint8_t address, uint8_t quantity, uint8_t sendStop);
	uint8_t requestFrom(uint8_t address, uint8_t quantity, bool sendStop) {
		return requestFrom(address, quantity, (uint8_t)(sendStop ? 1 : 0));
	}
	uint8_t requestFrom(uint8_t address, uint8_t quantity) {
		return requestFrom(address, quantity, (uint8_t)1);
	}
	uint8_t requestFrom(int address, int quantity, int sendStop) {
		return requestFrom((uint8_t)address, (uint8_t)quantity, (uint8_t)(sendStop ? 1 : 0));
	}
	uint8_t requestFrom(int address, int quantity) {
		return requestFrom((uint8_t)address, (uint8_t)quantity, (uint8_t)1);
	}
	uint8_t requestFrom(uint8_t addr, uint8_t qty, uint32_t iaddr, uint8_t n, uint8_t stop) {
		if (n > 0) {
			beginTransmission(addr);
			if (n > 3) n = 3;
			while (n-- > 0) write((uint8_t)(iaddr >> (n * 8)));   // internal addr, MSB first
			endTransmission((uint8_t)0);
		}
		return requestFrom(addr, qty, stop);
	}

	virtual size_t write(uint8_t data);
	virtual size_t write(const uint8_t *data, size_t len);
	virtual int available(void);
	virtual int read(void);
	virtual int peek(void);
	virtual void flush(void) {}
	void onReceive(void (*function)(int numBytes)) { on_receive = function; }
	void onRequest(void (*function)(void)) { on_request = function; }
	void handle_slave_isr();               // called by the fastrun trampolines

	// pre-1.0 compatibility shims (parity with WireIMXRT)
	void send(uint8_t b)             { write(b); }
	void send(uint8_t *s, uint8_t n) { write(s, n); }
	void send(int n)                 { write((uint8_t)n); }
	void send(char *s)               { write((uint8_t *)s, strlen(s)); }
	uint8_t receive(void) { int c = read(); return (c < 0) ? 0 : (uint8_t)c; }

	size_t write(unsigned long n) { return write((uint8_t)n); }
	size_t write(long n)          { return write((uint8_t)n); }
	size_t write(unsigned int n)  { return write((uint8_t)n); }
	size_t write(int n)           { return write((uint8_t)n); }
	using Print::write;

	IMXRT_LPI2C_t & port() { return *(IMXRT_LPI2C_t *)port_addr; }

private:
	uintptr_t port_addr;
	const I2C_Hardware_t &hardware;

	uint8_t tx_addr = 0;
	uint8_t tx_buf[BUFFER_LENGTH];
	uint8_t tx_len = 0;
	uint8_t rx_buf[BUFFER_LENGTH];
	uint8_t rx_len = 0;
	uint8_t rx_idx = 0;
	uint32_t clock_hz = 100000;

	bool is_slave = false;
	bool in_slave_request = false;
	uint8_t slave_addr = 0;
	void (*on_receive)(int) = nullptr;
	void (*on_request)(void) = nullptr;
	uint8_t s_rx_buf[BUFFER_LENGTH];
	uint8_t s_rx_len = 0;
	uint8_t s_rx_idx = 0;
	uint8_t s_tx_buf[BUFFER_LENGTH];
	uint8_t s_tx_len = 0;
	uint8_t s_tx_idx = 0;

	bool wait_flag(uint32_t mask, uint32_t error_mask, uint32_t &err);
	void bus_recover();
};

extern TwoWire Wire;
extern TwoWire Wire1;
extern TwoWire Wire2;

#endif
#endif
```

> Note `#include <string.h>` is needed for `strlen` in `send(char*)`; add it to the includes if the core headers don't transitively provide it. If `core_pins.h` already pulls `Stream.h`, the explicit include is harmless.

- [ ] **Step 3: Verify the header parses (dormant compile)**

Create `/tmp/wire1176_hdr.cpp`:
```cpp
#define __IMXRT1176__ 1
#include "Wire.h"
TwoWire *probe = &Wire;      // forces the class to be complete
```
Run:
```bash
/Applications/ARM_10/bin/arm-none-eabi-g++ -std=gnu++17 -fsyntax-only \
  -I /Users/nicholasnewdigate/Development/Wire \
  -I /Users/nicholasnewdigate/Development/rt1170/evkb/cores/imxrt1176 \
  /tmp/wire1176_hdr.cpp
```
Expected: no output (clean parse). If it complains about `Stream`/`Print`/`IRQ_NUMBER_t`, adjust the includes in `WireIMXRT1176.h` to match what the core headers expose (they are the same headers the core Wire.h used). Delete `/tmp/wire1176_hdr.cpp` after.

- [ ] **Step 4: Commit (Wire repo)**

```bash
cd /Users/nicholasnewdigate/Development/Wire
git add Wire.h WireIMXRT1176.h
git commit -m "feat: add __IMXRT1176__ dispatch arm + WireIMXRT1176.h decl

Full Teensy TwoWire : public Stream API for the MIMXRT1170-EVKB, dispatched
from Wire.h before the TEENSYDUINO arm (RT1176 is not TEENSYDUINO). Lean
I2C_Hardware_t (clock/pin/irq only) + port() over IMXRT_LPI2C_t; slave state
retained. Declaration only; impl follows. LPSPI move is the precedent.

Co-Authored-By: Claude Opus 4.8 <noreply@anthropic.com>"
```

---

## Task 3: Library — `WireIMXRT1176.cpp` (implementation, dormant)

The intricate task (**use a capable model**). Port the core `Wire.cpp` engine verbatim-in-behavior into the library's method signatures, add the full-API methods the core lacks, and define the three hardware tables + instances + fastrun ISR trampolines. Verify with a standalone `-c` compile.

**Files:**
- Create: `/Users/nicholasnewdigate/Development/Wire/WireIMXRT1176.cpp`
- Read (source of truth): `cores/imxrt1176/Wire.cpp` + `Wire_instances.cpp`

**The mechanical mapping (core `Wire.cpp` → this file):**
- `hw->mcr` → `port().MCR`, `hw->msr` → `port().MSR`, `hw->mcfgr1` → `port().MCFGR1`, `hw->mccr0` → `port().MCCR0`, `hw->mtdr` → `port().MTDR`, `hw->mrdr` → `port().MRDR`.
- `hw->scr` → `port().SCR`, `hw->ssr` → `port().SSR`, `hw->sier` → `port().SIER`, `hw->samr` → `port().SAMR`, `hw->sasr` → `port().SASR`, `hw->scfgr1` → `port().SCFGR1`, `hw->scfgr2` → `port().SCFGR2`, `hw->stdr` → `port().STDR`, `hw->srdr` → `port().SRDR`.
- `hw->lpcg / clock_root / clock_root_val / scl_mux / scl_mux_val / scl_pad / sda_mux / sda_mux_val / sda_pad / scl_select_input / scl_select_val / sda_select_input / sda_select_val / pad_ctl_val / irq / irq_handler / irq_priority` → `hardware.<same field>` (note: `irq_handler` is renamed `irq_function` to match the decl).
- Every private member (`tx_addr`, `tx_len`, `rx_*`, `s_*`, `clock_hz`, `is_slave`, `in_slave_request`, etc.) is unchanged.

**Invariants that MUST be preserved verbatim (do not "improve" them):**
1. `endTransmission` judges ACK/NACK at **STOP via `MSR.NDF`**, never treats early `TDF` as an address-ACK (the HW-verified fix for bus scanning). Keep the `err == 0xFF ? 2 : 3` address-vs-data NACK encoding and the `bus_recover()` on failure.
2. `setClock`: the 24 MHz source, the `for (pre...) div = (src>>pre)/freq; if (div<=120) break;` prescale search, `clklo = div*6/10`, the 63-clamp, and writing MCCR only with `MEN=0`.
3. Slave `begin(addr)`: `SCFGR1 = SAEN|TXDSTALL|RXSTALL` (`(1<<9)|(1<<2)|(1<<1)`), `SCFGR2 = 0x0000000F` (CLKHOLD max), `SIER = TDIE|RDIE|AVIE|SDIE|BEIE|FEIE`, `SCR = SEN|FILTEN` (`SCR_SEN|(1<<4)`), and the `attachInterruptVector`/`NVIC_SET_PRIORITY`/`NVIC_ENABLE_IRQ` wiring.
4. `handle_slave_isr()` stays in **`.fastrun` (ITCM)** — keep the `__attribute__((section(".fastrun")))`. Preserve the AVF→reset-rx, RDF→append, TDF→(fire `on_request` on first byte then feed `STDR`), SDF→(fire `on_receive`, reset), and BEF/FEF→W1C-and-reset recovery, exactly as in the core.
5. `requestFrom`: START+addr(R), `MTDR = RXD | (quantity-1)`, drain `MRDR` while `!RXEMPTY`, STOP.
6. `read`/`peek`/`available` keep the `is_slave ? s_rx… : rx…` branch.
7. `BUFFER_LENGTH` = 32.

- [ ] **Step 1: File preamble — includes + file-local macros (copy verbatim from core `Wire.cpp`)**

```cpp
#include "WireIMXRT1176.h"

#if defined(__IMXRT1176__)

#include <string.h>

// MSR flags (imxrt_lpi2c.c contract)
#define MSR_TDF  (1u<<0)
#define MSR_RDF  (1u<<1)
#define MSR_EPF  (1u<<8)
#define MSR_SDF  (1u<<9)
#define MSR_NDF  (1u<<10)
#define MSR_ALF  (1u<<11)
#define MSR_FEF  (1u<<12)
// MCR flags
#define MCR_MEN  (1u<<0)
#define MCR_RST  (1u<<1)
#define MCR_RTF  (1u<<8)
#define MCR_RRF  (1u<<9)
// MTDR commands (data in [7:0], cmd in [10:8])
#define TX_CMD(cmd, data)  (((uint32_t)(cmd) << 8) | ((data) & 0xFF))
#define CMD_TXD    0u
#define CMD_RXD    1u
#define CMD_STOP   2u
#define CMD_START  4u
#define MRDR_RXEMPTY (1u<<14)
// Slave register bits
#define SCR_SEN  (1u<<0)
#define SCR_RST  (1u<<1)
#define SSR_TDF  (1u<<0)
#define SSR_RDF  (1u<<1)
#define SSR_AVF  (1u<<2)
#define SSR_SDF  (1u<<9)
#define SSR_BEF  (1u<<10)
#define SSR_FEF  (1u<<11)
#define SIER_TDIE (1u<<0)
#define SIER_RDIE (1u<<1)
#define SIER_AVIE (1u<<2)
#define SIER_SDIE (1u<<9)
#define SIER_BEIE (1u<<10)
#define SIER_FEIE (1u<<11)

#define WIRE_TIMEOUT 100000u
```

- [ ] **Step 2: Port the engine methods from core `Wire.cpp`**

Port each of these methods, applying the mapping table above (`hw->X` → `port().X` / `hardware.X`), keeping bodies behaviorally identical to `cores/imxrt1176/Wire.cpp`:
- `void TwoWire::begin()` — LPCG ungate, clock root, SCL/SDA mux+pad, select-inputs, `MCR` reset, `setClock(clock_hz)`, `MCR = MEN`.
- `void TwoWire::begin(uint8_t address)` — slave setup per invariant 3.
- `void TwoWire::end()` — `port().MCR = 0; hardware.lpcg = 0;`.
- `void TwoWire::setClock(uint32_t freq)` — invariant 2.
- `bool TwoWire::wait_flag(uint32_t mask, uint32_t error_mask, uint32_t &err)`.
- `void TwoWire::bus_recover()`.
- `uint8_t TwoWire::endTransmission(uint8_t sendStop)` — invariant 1. (Core signature is `bool sendStop`; here it is `uint8_t` — treat nonzero as stop. The body is otherwise identical: `if (sendStop) {…}`.)
- `uint8_t TwoWire::requestFrom(uint8_t address, uint8_t quantity, uint8_t sendStop)` — invariant 5. (Core signature `bool sendStop`; `uint8_t` here, nonzero = stop.)
- `size_t TwoWire::write(uint8_t data)` and `size_t TwoWire::write(const uint8_t *data, size_t len)` — the `in_slave_request` branch that appends to `s_tx_buf`, else `tx_buf`.
- `int TwoWire::available()`, `int TwoWire::read()`, `int TwoWire::peek()` — invariant 6.
- `__attribute__((section(".fastrun"))) void TwoWire::handle_slave_isr()` — invariant 4.

> These are ~180 lines that already exist, verified, in `cores/imxrt1176/Wire.cpp`. Read that file and transcribe with the mapping. Do not restructure. The only signature deltas are `endTransmission`/`requestFrom` taking `uint8_t sendStop` instead of `bool` (behaviorally identical). `available()` is `virtual` here (was non-virtual); the body is unchanged.

- [ ] **Step 3: Add the hardware tables + instances + fastrun trampolines**

Values copied exactly from `Wire_instances.cpp` (the register-ref fields are dropped — `port()` supplies them). Append to `WireIMXRT1176.cpp`:

```cpp
static void wire_isr();    // forward decls
static void wire1_isr();
static void wire2_isr();

// EVKB Arduino-header I2C = LPI2C1 on GPIO_AD_08 (SCL) / GPIO_AD_09 (SDA),
// ALT1|SION (0x11). Pad 0x1E = ODE|DSE|PUE|PUS (internal pull-up). CLOCK_ROOT37
// (24 MHz) / LPCG98. Values verbatim from the HW-verified Wire_instances.cpp.
const TwoWire::I2C_Hardware_t TwoWire::lpi2c1_hardware = {
	/* lpcg */ CCM_LPCG98_DIRECT,
	/* clock_root */ CCM_CLOCK_ROOT37_CONTROL, /* clock_root_val */ 0u,
	/* scl */ IOMUXC_SW_MUX_CTL_PAD_GPIO_AD_08, 0x11u, IOMUXC_SW_PAD_CTL_PAD_GPIO_AD_08,
	/* sda */ IOMUXC_SW_MUX_CTL_PAD_GPIO_AD_09, 0x11u, IOMUXC_SW_PAD_CTL_PAD_GPIO_AD_09,
	/* scl_select */ IOMUXC_LPI2C1_SCL_SELECT_INPUT, 0u,
	/* sda_select */ IOMUXC_LPI2C1_SDA_SELECT_INPUT, 0u,
	/* pad_ctl_val */ 0x0000001Eu,
	/* irq */ IRQ_LPI2C1, /* irq_function */ wire_isr, /* irq_priority */ 16u,
};

// LPI2C2: QEMU-loopback slave persona only (no physical EVKB pins). Pin refs
// bind to LPI2C1's IOMUXC regs (inert in QEMU). CLOCK_ROOT38 / LPCG99.
const TwoWire::I2C_Hardware_t TwoWire::lpi2c2_hardware = {
	/* lpcg */ CCM_LPCG99_DIRECT,
	/* clock_root */ CCM_CLOCK_ROOT38_CONTROL, /* clock_root_val */ 0u,
	/* scl */ IOMUXC_SW_MUX_CTL_PAD_GPIO_AD_08, 0x11u, IOMUXC_SW_PAD_CTL_PAD_GPIO_AD_08,
	/* sda */ IOMUXC_SW_MUX_CTL_PAD_GPIO_AD_09, 0x11u, IOMUXC_SW_PAD_CTL_PAD_GPIO_AD_09,
	/* scl_select */ IOMUXC_LPI2C1_SCL_SELECT_INPUT, 0u,
	/* sda_select */ IOMUXC_LPI2C1_SDA_SELECT_INPUT, 0u,
	/* pad_ctl_val */ 0x0000001Eu,
	/* irq */ IRQ_LPI2C2, /* irq_function */ wire1_isr, /* irq_priority */ 16u,
};

// LPI2C5: onboard eCompass/WM8962 codec bus on GPIO_LPSR_05 (SCL) /
// GPIO_LPSR_04 (SDA), ALT0|SION (0x10). LPSR-domain pad 0x0A. CLOCK_ROOT41
// (mux 1) / LPCG102.
const TwoWire::I2C_Hardware_t TwoWire::lpi2c5_hardware = {
	/* lpcg */ CCM_LPCG102_DIRECT,
	/* clock_root */ CCM_CLOCK_ROOT41_CONTROL, /* clock_root_val */ (1u << 8),
	/* scl */ IOMUXC_SW_MUX_CTL_PAD_GPIO_LPSR_05, 0x10u, IOMUXC_SW_PAD_CTL_PAD_GPIO_LPSR_05,
	/* sda */ IOMUXC_SW_MUX_CTL_PAD_GPIO_LPSR_04, 0x10u, IOMUXC_SW_PAD_CTL_PAD_GPIO_LPSR_04,
	/* scl_select */ IOMUXC_LPI2C5_SCL_SELECT_INPUT, 0u,
	/* sda_select */ IOMUXC_LPI2C5_SDA_SELECT_INPUT, 0u,
	/* pad_ctl_val */ 0x0000000Au,
	/* irq */ IRQ_LPI2C5, /* irq_function */ wire2_isr, /* irq_priority */ 16u,
};

TwoWire Wire(IMXRT_LPI2C1_ADDRESS, TwoWire::lpi2c1_hardware);
TwoWire Wire1(IMXRT_LPI2C2_ADDRESS, TwoWire::lpi2c2_hardware);
TwoWire Wire2(IMXRT_LPI2C5_ADDRESS, TwoWire::lpi2c5_hardware);

__attribute__((section(".fastrun"))) static void wire_isr()  { Wire.handle_slave_isr(); }
__attribute__((section(".fastrun"))) static void wire1_isr() { Wire1.handle_slave_isr(); }
__attribute__((section(".fastrun"))) static void wire2_isr() { Wire2.handle_slave_isr(); }

#endif // __IMXRT1176__
```

- [ ] **Step 4: Standalone compile of the branch**

```bash
/Applications/ARM_10/bin/arm-none-eabi-g++ -std=gnu++17 -c \
  -mcpu=cortex-m7 -mthumb -D__IMXRT1176__ \
  -I /Users/nicholasnewdigate/Development/Wire \
  -I /Users/nicholasnewdigate/Development/rt1170/evkb/cores/imxrt1176 \
  /Users/nicholasnewdigate/Development/Wire/WireIMXRT1176.cpp -o /tmp/wire1176.o
```
Expected: compiles clean. Fix any symbol mismatches (e.g. a mistyped `CCM_*`/`IOMUXC_*`/`IRQ_LPI2C*` name — cross-check against `Wire_instances.cpp`). Delete `/tmp/wire1176.o` after.

- [ ] **Step 5: Commit (Wire repo)**

```bash
cd /Users/nicholasnewdigate/Development/Wire
git add WireIMXRT1176.cpp
git commit -m "feat: WireIMXRT1176.cpp — RT1176 LPI2C engine under the Teensy API

Polled master + fastrun-ITCM interrupt slave ported verbatim-in-behavior
from the HW-verified core Wire.cpp (hw->X -> port().X over IMXRT_LPI2C_t;
hardware_t register refs dropped). Preserves the NACK-at-STOP-via-NDF fix,
the 24MHz MCCR math, and the slave CLKHOLD/TXDSTALL + BEF/FEF recovery.
lpi2c{1,2,5}_hardware values copied from Wire_instances.cpp. Dormant until
evkb repoints. Compiles standalone with -D__IMXRT1176__.

Co-Authored-By: Claude Opus 4.8 <noreply@anthropic.com>"
```

---

## Task 4: Atomic integration — move gates, repoint consumers, delete core Wire

The first real link+run of the library branch. Move the 3 Wire gates into the Wire repo, add the Wire import to the staying evkb consumers, delete the core Wire + the 3 evkb gate dirs — **atomically**, so no compiled test ever sees two `TwoWire`. Build errors point back to Task 2/3.

Gate reality (confirmed): `wire_master_test` + `wire_slave_test` have QEMU run scripts; `wire_oled_test` is a **slave HW demo** (RT1170 slave @0x42, MKR Zero master) with **no QEMU run script** — build-only in CI. The run scripts already reference `~/Development/rt1170/evkb/tools/qrun` by absolute path, so they need no edits.

**Files:**
- Move: `evkb/{wire_master_test,wire_slave_test,wire_oled_test}` → `/Users/nicholasnewdigate/Development/Wire/tests/<name>/`
- Modify: each moved gate's `CMakeLists.txt` + `toolchain/rt1170-evkb.toolchain.cmake`
- Create: `/Users/nicholasnewdigate/Development/Wire/tests/.gitignore`
- Modify: `evkb/{i2s_audio_test,audioinput_i2s_test,audiooutput_i2s_test,ssd1306_display}/CMakeLists.txt` (add Wire import + link)
- Delete: `cores/imxrt1176/Wire.h`, `Wire.cpp`, `Wire_instances.cpp`
- Delete: `evkb/{wire_master_test,wire_slave_test,wire_oled_test}`

- [ ] **Step 1: Copy the 3 Wire gates into the Wire repo**

```bash
mkdir -p /Users/nicholasnewdigate/Development/Wire/tests
cd /Users/nicholasnewdigate/Development/rt1170/evkb
for g in wire_master_test wire_slave_test wire_oled_test; do
  rm -rf "$g/build"
  cp -R "$g" /Users/nicholasnewdigate/Development/Wire/tests/"$g"
done
rm -rf /Users/nicholasnewdigate/Development/Wire/tests/*/build
```

- [ ] **Step 2: Create `tests/.gitignore`**

Write `/Users/nicholasnewdigate/Development/Wire/tests/.gitignore`:
```
build/
*.uart
*.dbg
```

- [ ] **Step 3: Repoint each moved gate's `CMakeLists.txt`**

For each of `wire_master_test`, `wire_slave_test`, `wire_oled_test`, replace `CMakeLists.txt` with this (change the two `<name>` occurrences per gate — for `wire_oled_test` there is no run script but the CMake is identical):
```cmake
cmake_minimum_required(VERSION 3.24)
project(<name>)

set(TEENSY_VERSION 117 CACHE STRING "")

# This gate lives in the Wire library repo (Wire/tests/<name>/): it compiles
# Wire.h + WireIMXRT1176.* from THIS repo (the core's own Wire driver was
# removed) while borrowing the evkb checkout for teensy-cmake-macros + the rest
# of the imxrt1176 core.
if(NOT DEFINED EVKB)
    set(EVKB $ENV{HOME}/Development/rt1170/evkb)
endif()

include(FetchContent)
FetchContent_Declare(teensy_cmake_macros SOURCE_DIR ${EVKB}/teensy-cmake-macros)
FetchContent_MakeAvailable(teensy_cmake_macros)
include(${teensy_cmake_macros_SOURCE_DIR}/CMakeLists.include.txt)

# Core without its Wire driver (removed in the move).
import_arduino_library(cores ${EVKB}/cores/imxrt1176)
# Wire from THIS repo (adds the include dir so #include "Wire.h" resolves here,
# and compiles WireIMXRT1176.cpp).
import_arduino_library(Wire ${CMAKE_CURRENT_LIST_DIR}/../..)

teensy_add_executable(<name> <name>.cpp)
teensy_target_link_libraries(<name> cores Wire)

target_link_libraries(<name>.elf stdc++)
```

- [ ] **Step 4: Repoint each moved gate's toolchain to use `EVKB_ROOT`**

Replace `tests/<name>/toolchain/rt1170-evkb.toolchain.cmake` in all three with the SPI-tests toolchain (which resolves `COREPATH` from `EVKB_ROOT` rather than a relative walk). Copy it verbatim:
```bash
SPI_TC=/Users/nicholasnewdigate/Development/SPI/tests/spi_loopback_test/toolchain/rt1170-evkb.toolchain.cmake
for g in wire_master_test wire_slave_test wire_oled_test; do
  cp "$SPI_TC" /Users/nicholasnewdigate/Development/Wire/tests/"$g"/toolchain/rt1170-evkb.toolchain.cmake
done
```
Confirm it sets `COREPATH` from `EVKB_ROOT` (`$ENV{HOME}/Development/rt1170/evkb`):
Run: `grep -n 'EVKB_ROOT\|COREPATH' /Users/nicholasnewdigate/Development/Wire/tests/wire_master_test/toolchain/rt1170-evkb.toolchain.cmake`
Expected: `set(EVKB_ROOT "$ENV{HOME}/Development/rt1170/evkb")` + `COREPATH ... /cores/imxrt1176/`.

- [ ] **Step 5: Delete the core Wire driver**

```bash
cd /Users/nicholasnewdigate/Development/rt1170/evkb/cores/imxrt1176
git rm Wire.h Wire.cpp Wire_instances.cpp
```

- [ ] **Step 6: Delete the 3 evkb Wire gate dirs**

```bash
cd /Users/nicholasnewdigate/Development/rt1170/evkb
rm -rf wire_master_test wire_slave_test wire_oled_test
```

- [ ] **Step 7: Add the Wire import to the staying evkb consumers**

For each of `i2s_audio_test`, `audioinput_i2s_test`, `audiooutput_i2s_test`, `ssd1306_display`: in its `CMakeLists.txt`, add one line right after the existing `import_arduino_library(cores …)` line:
```cmake
import_arduino_library(Wire $ENV{HOME}/Development/Wire)
```
and add `Wire` to its `teensy_target_link_libraries(<gate> cores …)` line, e.g. for `i2s_audio_test`:
```cmake
# before: teensy_target_link_libraries(i2s_audio_test cores)
teensy_target_link_libraries(i2s_audio_test cores Wire)
```
Do the same edit for the other three (their target names are `audioinput_i2s_test`, `audiooutput_i2s_test`, `ssd1306_display`). Read each file first to place the two edits correctly (their bodies differ slightly — the audio gates also have `target_sources(... Development/Audio/...)` lines that stay untouched). `mkr_ssd1306_test.ino` is a bare sketch with no CMake — leave it.

- [ ] **Step 8: Build + run the two QEMU Wire gates from their new home**

```bash
cd /Users/nicholasnewdigate/Development/Wire/tests/wire_master_test
cmake -S . -B build -DCMAKE_TOOLCHAIN_FILE=$PWD/toolchain/rt1170-evkb.toolchain.cmake
cmake --build build
./run_qemu_wire.sh
```
Expected tail: `PASS: Wire I2C master verified (scan, write, readback, NACK)` (greps `scan_found=0x50`, `scan_count=1`, `wr_status=0`, `readback=DE AD BE EF `, `absent_status=2`).

```bash
cd /Users/nicholasnewdigate/Development/Wire/tests/wire_slave_test
cmake -S . -B build -DCMAKE_TOOLCHAIN_FILE=$PWD/toolchain/rt1170-evkb.toolchain.cmake
cmake --build build
./run_qemu_wire_slave.sh
```
Expected tail: `PASS: Wire I2C slave verified (onReceive + onRequest via loopback; multi-byte read HW-verified)` (greps `rx_count=3`, `rx=0xAA 0xBB 0xCC`, `wr_status=0`, `rd(1)=0x11`).

> If either fails at link with `undefined reference to Wire`/`TwoWire::…`, the library branch didn't compile into the Wire lib — re-check Task 3's `#if defined(__IMXRT1176__)` guards and that `import_arduino_library(Wire …)` globs `WireIMXRT1176.cpp`. If `import_arduino_library` chokes on `utility/twi.c` (AVR-only), confirm it compiles to an empty TU for ARM; if not, that is a real finding — report it. **Do not weaken the gate assertions.**

- [ ] **Step 9: Build the third moved gate (build-only)**

```bash
cd /Users/nicholasnewdigate/Development/Wire/tests/wire_oled_test
cmake -S . -B build -DCMAKE_TOOLCHAIN_FILE=$PWD/toolchain/rt1170-evkb.toolchain.cmake
cmake --build build
ls build/wire_oled_test.elf
```
Expected: `wire_oled_test.elf` exists (compiles+links; no QEMU run — it is a HW slave demo).

- [ ] **Step 10: Rebuild the staying evkb consumers + re-run their QEMU gates**

```bash
cd /Users/nicholasnewdigate/Development/rt1170/evkb/i2s_audio_test
rm -rf build && cmake -S . -B build -DCMAKE_TOOLCHAIN_FILE=$PWD/toolchain/rt1170-evkb.toolchain.cmake && cmake --build build
```
Repeat the `rm -rf build && cmake … && cmake --build build` for `audioinput_i2s_test`, `audiooutput_i2s_test`, `ssd1306_display`.
Then re-run each gate's existing `run_qemu_*.sh` where present (i2s_audio_test, audioinput_i2s_test, audiooutput_i2s_test).
Expected: all compile+link against the library `Wire.h` (the `control_wm8962.cpp` `#include "Wire.h"` now resolves to the library) and their existing QEMU gates still PASS (the WM8962 codec still enumerates on `Wire2`). `rm -rf build` is required because the CMake glob for the Audio fork sources has no `CONFIGURE_DEPENDS`.

- [ ] **Step 11: Commit (three repos)**

```bash
# Wire repo: the moved tests
cd /Users/nicholasnewdigate/Development/Wire
git add tests
git commit -m "test: move wire_master/wire_slave/wire_oled gates into tests/

Self-contained under Wire/tests/ (SPI-tests precedent): import cores from the
evkb checkout, Wire from this repo, teensy-cmake-macros via FetchContent,
toolchain COREPATH from EVKB_ROOT. wire_master + wire_slave QEMU-gated green;
wire_oled is a build-only slave HW demo. No evkb->Wire dependency for these.

Co-Authored-By: Claude Opus 4.8 <noreply@anthropic.com>"

# teensy-cores: the core Wire deletion
cd /Users/nicholasnewdigate/Development/rt1170/evkb/cores/imxrt1176
git commit -m "refactor: remove core Wire driver (moved to newdigate/Wire)

Wire.{h,cpp}+Wire_instances.cpp deleted; the RT1176 I2C driver now lives in
the newdigate/Wire library (WireIMXRT1176.*). Core keeps IMXRT_LPI2C_t +
the LPI2C base addresses. Exactly one TwoWire in any build now.

Co-Authored-By: Claude Opus 4.8 <noreply@anthropic.com>"

# evkb (local): staying-consumer imports + gate-dir removals
cd /Users/nicholasnewdigate/Development/rt1170/evkb
git add -A
git commit -m "build: repoint Wire consumers to newdigate/Wire; drop moved gates

Audio + ssd1306 gates gain import_arduino_library(Wire ~/Development/Wire)
+ link Wire (WM8962 codec uses Wire2). wire_{master,slave,oled}_test dirs
removed (moved into the Wire repo).

Co-Authored-By: Claude Opus 4.8 <noreply@anthropic.com>"
```

---

## Task 5: Hardware verification (PAUSE for user)

Re-implementation → hardware is the arbiter. Clock root/LPCG, pad SION/ODE/DSE, select-input, and IRQ retargets are the prime suspects for any regression. The controller drives flash + VCOM capture; the user wires the slave/OLED and confirms.

**Flashing (per `rt1170-evkb-flashing` memory):** LinkServer (not pyOCD); VCOM console @115200. **Capture across the flash+reset window** (background the capture, `sleep 3`, then flash-blocking) so the one-shot `setup()` prints aren't missed.

- [ ] **Step 1: `wire_master_test` — master to a real slave**

Flash `Wire/tests/wire_master_test/build/wire_master_test.elf`. It scans the bus, writes, reads back, and probes an absent address. With a real I²C slave on the LPI2C1 header (`GPIO_AD_08` SCL / `GPIO_AD_09` SDA, external pull-ups), confirm over VCOM: `wr_status=0`, a non-empty `readback=…`, `scan_count` matching the devices present, `absent_status=2` for an unpopulated address. **PAUSE** — ask the user to wire the slave + report the VCOM lines.

- [ ] **Step 2: `wire_slave_test` / `wire_oled_test` — RT1170 as slave**

Flash `wire_oled_test.elf` (RT1170 slave @0x42; the firmware mirrors master writes + answers reads `0x11 0x22 0x33 0x44`). With a MKR Zero (or any 3.3 V master) driving it, confirm the `event … rxn=… data:` lines and correct read-back. (This is the physical-master slave test; `wire_slave_test`'s QEMU loopback already proved the ISR logic.) **PAUSE** — ask the user to wire the master + report.

- [ ] **Step 3: (Optional) `ssd1306_display` — OLED visual**

If the user has an SSD1306 wired to the LPI2C1 header, flash `evkb/ssd1306_display/build/ssd1306_display.elf` and confirm the panel renders (the full write path end-to-end). Optional — skip if no OLED on the bench.

- [ ] **Step 4: If a retarget bug surfaces**

Symptoms → suspects (per `rt1176-lpi2c-wire`): BBF-stuck/timeout = SION missing; ALF on idle-high bus = no pull-up / wrong pins; SCL never toggles = wrong pad/clock. Fix the offending `lpi2c*_hardware` value in `WireIMXRT1176.cpp` (cross-check `Wire_instances.cpp`), rebuild, re-flash. Commit the fix to the Wire repo.

- [ ] **Step 5: Record the HW result** — capture the passing VCOM output for the memory note in Task 6.

---

## Task 6: Memory notes + push (when the user asks)

- [ ] **Step 1: Update `rt1176-lpi2c-wire.md`**

Add a top banner (like the SPI notes got): Wire re-implemented to the full Teensy API and moved core→`newdigate/Wire` (`WireIMXRT1176.{h,cpp}`, `__IMXRT1176__` arm in `Wire.h`); core `Wire.{h,cpp}`+`Wire_instances.cpp` deleted; core keeps `IMXRT_LPI2C_t`; the `wire_*` gates now live in `newdigate/Wire/tests/`; the register/clock/pin/slave logic is unchanged (re-expressed via `port()`), re-HW-verified. Link `[[rt1176-wire-library-move]]`.

- [ ] **Step 2: Create `rt1176-wire-library-move.md`**

New `project` memory: the hybrid port (Teensy API shape over our HW-verified engine, dedicated `WireIMXRT1176` file dispatched from `Wire.h`), `IMXRT_LPI2C_t` (master/slave gap layout), the 3 gates moved + the audio/ssd1306 consumers that stayed and gained a Wire import (the codec `Wire2` dependency), and the HW result. Commits + links (`[[rt1176-lpi2c-wire]]`, `[[rt1176-spi-library-move]]`, `[[rt1176-wm8962-consolidation]]`). Add the `MEMORY.md` pointer line.

- [ ] **Step 3: Confirm unpushed state**

```bash
git -C /Users/nicholasnewdigate/Development/rt1170/evkb/cores/imxrt1176 log --oneline origin/master..master
git -C /Users/nicholasnewdigate/Development/Wire log --oneline origin/master..master
```
Expected: teensy-cores has the struct-add + Wire-deletion commits; Wire has the dispatch/decl + impl + tests commits.

- [ ] **Step 4: Push (ONLY after the user confirms)**

```bash
git -C /Users/nicholasnewdigate/Development/rt1170/evkb/cores/imxrt1176 push origin master
git -C /Users/nicholasnewdigate/Development/Wire push origin master
```
evkb is local (not pushed).

---

## Notes on ordering (why it's load-bearing)

- **Task 1 is additive** — the core Wire keeps compiling against the flat defines; nothing sees the new struct yet.
- **Tasks 2–3 are a dormant library branch** — verified by `-fsyntax-only`/`-c` only, because evkb still links the core Wire. No build has two `TwoWire` yet.
- **Task 4 is atomic**: the core Wire deletion, the gate moves, and the consumer repoints happen together, so the first build that links the library branch is also the first build without the core Wire. If Task 4 is split, an intermediate state either has two `TwoWire` (link error) or none (also a link error) — either way it won't build, which is the safety check working.
