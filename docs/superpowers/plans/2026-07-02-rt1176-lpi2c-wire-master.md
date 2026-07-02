# RT1176 `Wire` I²C Master (Stage A) Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Implement the Arduino `Wire` (TwoWire) I²C **master** on the RT1176 LPI2C peripheral, verified by an automated QEMU gate (AT24C EEPROM round-trip) and on the EVKB against an SSD1306 OLED.

**Architecture:** A blocking, polled master engine modelled on the existing `analog.c` idiom — write LPI2C command words to `MTDR`, poll `MSR` flags with a bounded guard-loop, return Arduino status codes. Register defs come from the generator (`gen_imxrt1176_h.py` → `imxrt1176.h`); clock/pin bring-up follows the `Serial1` `hardware_t` pattern (LPCG gate + CCM clock root + IOMUXC mux). The QEMU LPI2C master model already exists and is wired; we only attach an AT24C EEPROM to its bus in the machine.

**Tech stack:** C/C++ (register-poke, no NXP SDK at runtime), QEMU device model + machine, `eeprom_at24c` QEMU slave, SSD1306 OLED (hardware).

**Spec:** `docs/superpowers/specs/2026-07-02-rt1176-lpi2c-wire-design.md` (Stage A).

---

## Ground-truth constants (verified from headers/model)

**Primary target — LPI2C1** (Arduino-first convention; `GPIO_AD` pads, main power domain). LPI2C5 alternate table is in Task 8 if the schematic says otherwise.

| Constant | Value | Source |
|---|---|---|
| LPI2C1 base | `0x40104000` | fsl-imxrt1170.c `lpi2c_base[0]` |
| IRQ_LPI2C1 | `32` | fsl-imxrt1170.c `lpi2c_irq[0]` / CMSIS `LPI2C1_IRQn` |
| MCR / MSR / MIER | `+0x10` / `+0x14` / `+0x18` | imxrt_lpi2c.c |
| MCFGR1 / MCCR0 | `+0x24` / `+0x48` | imxrt_lpi2c.c |
| MFCR / MTDR / MRDR | `+0x58` / `+0x60` / `+0x70` | imxrt_lpi2c.c |
| MCR bits | MEN=`1<<0`, RST=`1<<1`, RTF=`1<<8`, RRF=`1<<9` | imxrt_lpi2c.c |
| MSR bits | TDF=`1<<0`, RDF=`1<<1`, EPF=`1<<8`, SDF=`1<<9`, NDF=`1<<10`, ALF=`1<<11`, FEF=`1<<12`, MBF=`1<<24`, BBF=`1<<25` | imxrt_lpi2c.c |
| MTDR encoding | data=`[7:0]`, cmd=`[10:8]` | imxrt_lpi2c.c |
| MTDR commands | TXD=`0`, RXD=`1`, STOP=`2`, START=`4` | imxrt_lpi2c.c |
| MRDR | data=`[7:0]`, RXEMPTY=`1<<14` | imxrt_lpi2c.c |
| MCCR0 fields | CLKLO`[5:0]`, CLKHI`[13:8]`, SETHOLD`[21:16]`, DATAVD`[29:24]` | fsl_lpi2c.h |
| MCFGR1.PRESCALE | `[2:0]` | fsl_lpi2c.h |
| CCM_CLOCK_ROOT37 (LPI2C1) | `0x40CC1280` (=`0x40CC0000 + 37*0x80`) | root idx 37, stride 0x80 |
| CCM_LPCG98 (LPI2C1) | `0x40CC6C40` (=`0x40CC6000 + 98*0x20`) | LPCG idx 98, stride 0x20 |
| clock_root value | `0` → OscRC48MDiv2 = **24 MHz** (same mux-0 as LPUART/ADC roots) | existing core |
| SCL pad = GPIO_AD_08 | mux `0x400E812C`=`1`, daisy `0x400E85AC`=`0`, pad `0x400E8370` | fsl_iomuxc.h |
| SDA pad = GPIO_AD_09 | mux `0x400E8130`=`1`, daisy `0x400E85B0`=`0`, pad `0x400E8374` | fsl_iomuxc.h |
| AT24C QEMU type | `at24c-eeprom`, `CONFIG_AT24C=1` (enabled) | eeprom_at24c.c |
| LPI2C bus accessor | `soc->lpi2c[0].bus` (public `I2CBus*`) | imxrt_lpi2c.h |

**Two facts that require the physical board / RM (resolved in Task 8, HW-only — QEMU ignores both):**
1. Whether the Arduino header is LPI2C1 (`GPIO_AD_08/09`) or LPI2C5 (`GPIO_LPSR_04/05`).
2. The exact `GPIO_AD` pad-ctl **ODE** (open-drain) bit value for `IOMUXC_SW_PAD_CTL_PAD_GPIO_AD_08/09`.

---

## File structure

- `cores/imxrt1176/tools/gen_imxrt1176_h.py` — **modify**: emit LPI2C1 master regs + clock/pin defs.
- `cores/imxrt1176/imxrt1176.h` — **regenerate** (never hand-edit).
- `cores/imxrt1176/core_pins.h` (or wherever `IRQ_NUMBER_t` lives) — **modify**: add `IRQ_LPI2C1 = 32`.
- `cores/imxrt1176/Wire.h` — **create**: `TwoWire` class + Arduino compat.
- `cores/imxrt1176/Wire.cpp` — **create**: master engine.
- `cores/imxrt1176/Wire_instances.cpp` — **create**: `Wire` `hardware_t` table + object.
- `wire_master_test/wire_master_test.cpp` — **create**: gate sketch.
- `wire_master_test/CMakeLists.txt` — **create**.
- `wire_master_test/run_qemu_wire.sh` — **create**: QEMU gate.
- `qemu2/hw/arm/mimxrt1170-evk.c` — **modify**: attach AT24C EEPROM to `lpi2c[0].bus`.

---

## Task 1: Attach AT24C EEPROM to LPI2C1 bus in the QEMU machine

**Files:**
- Modify: `~/Development/qemu2/hw/arm/mimxrt1170-evk.c`

- [ ] **Step 1: Read the machine init to find the SoC handle and include site**

Run: `grep -n "FSL_IMXRT1170\|lpi2c\|sysbus_realize\|#include" ~/Development/qemu2/hw/arm/mimxrt1170-evk.c | head -30`
Expected: shows the `FslIMXRT1170State *` / `FSL_IMXRT1170(...)` handle after SoC realize, and the include block.

- [ ] **Step 2: Add the EEPROM include and attach call**

At the top include block add:
```c
#include "hw/i2c/i2c.h"
#include "hw/nvram/eeprom_at24c.h"
```
After the SoC device is realized (immediately following the line that yields the `FslIMXRT1170State *soc` — if the code uses a different local name, match it), add:
```c
    /* Test fixture: an AT24C02 EEPROM at 0x50 on the LPI2C1 (Arduino-header) bus,
     * so the Wire master QEMU gate can round-trip real I2C traffic. */
    at24c_eeprom_init(soc->lpi2c[0].bus, 0x50, 256);
```
If a `FslIMXRT1170State *soc` local does not already exist, add `FslIMXRT1170State *soc = FSL_IMXRT1170(dev);` where `dev` is the SoC `DeviceState*` used in the realize call.

- [ ] **Step 3: Verify the AT24C header exposes `at24c_eeprom_init`**

Run: `grep -n "at24c_eeprom_init" ~/Development/qemu2/include/hw/nvram/eeprom_at24c.h`
Expected: prototype `I2CSlave *at24c_eeprom_init(I2CBus *bus, uint8_t address, uint32_t rom_size);`
If the header path differs, run `grep -rn "at24c_eeprom_init" ~/Development/qemu2/include` and use that include path in Step 2.

- [ ] **Step 4: Rebuild QEMU**

Run: `ninja -C ~/Development/qemu2/build qemu-system-arm`
Expected: links `qemu-system-arm` with no errors.

- [ ] **Step 5: Regression — existing gates still green**

Run:
```bash
cd ~/Development/rt1170/evkb
(cd serial_test && sh run_qemu.sh 2>&1 | grep -iE "Serial1 up")
(cd analog_test && sh run_qemu_adc.sh 2>&1 | grep -iE "PASS|FAIL")
```
Expected: `RT1176 Serial1 up` and `PASS: LPADC ...` (attaching a slave must not perturb existing peripherals).

- [ ] **Step 6: Commit**

```bash
cd ~/Development/qemu2
git add hw/arm/mimxrt1170-evk.c
git commit -m "hw/arm/mimxrt1170-evk: attach AT24C EEPROM at 0x50 on LPI2C1 for Wire gate

Co-Authored-By: Claude Opus 4.8 <noreply@anthropic.com>"
```

---

## Task 2: Generate LPI2C1 register + clock + pin defs

**Files:**
- Modify: `~/Development/rt1170/evkb/cores/imxrt1176/tools/gen_imxrt1176_h.py`
- Regenerate: `~/Development/rt1170/evkb/cores/imxrt1176/imxrt1176.h`

- [ ] **Step 1: Find the append site in the generator**

Run: `grep -n 'L += \["", "#endif"\]' ~/Development/rt1170/evkb/cores/imxrt1176/tools/gen_imxrt1176_h.py`
Expected: the final `L += ["", "#endif"]` line (the SRC block precedes it from the CM4 guard work).

- [ ] **Step 2: Insert the LPI2C1 block before the final `#endif` append**

Immediately before `L += ["", "#endif"]`, add:
```python
    L += ["",
          "/* LPI2C1 (Arduino-header I2C master), base 0x40104000 */",
          "#define LPI2C1_MCR    (*(volatile uint32_t *)0x40104010u)",
          "#define LPI2C1_MSR    (*(volatile uint32_t *)0x40104014u)",
          "#define LPI2C1_MIER   (*(volatile uint32_t *)0x40104018u)",
          "#define LPI2C1_MCFGR1 (*(volatile uint32_t *)0x40104024u)",
          "#define LPI2C1_MCCR0  (*(volatile uint32_t *)0x40104048u)",
          "#define LPI2C1_MFCR   (*(volatile uint32_t *)0x40104058u)",
          "#define LPI2C1_MTDR   (*(volatile uint32_t *)0x40104060u)",
          "#define LPI2C1_MRDR   (*(volatile uint32_t *)0x40104070u)",
          "/* LPI2C1 clock: CCM_CLOCK_ROOT37 + LPCG98 */",
          "#define CCM_CLOCK_ROOT37_CONTROL (*(volatile uint32_t *)0x40CC1280u)",
          "#define CCM_LPCG98_DIRECT        (*(volatile uint32_t *)0x40CC6C40u)",
          "/* LPI2C1 pins: SCL=GPIO_AD_08, SDA=GPIO_AD_09 (ALT1) */",
          "#define IOMUXC_SW_MUX_CTL_PAD_GPIO_AD_08 (*(volatile uint32_t *)0x400E812Cu)",
          "#define IOMUXC_SW_MUX_CTL_PAD_GPIO_AD_09 (*(volatile uint32_t *)0x400E8130u)",
          "#define IOMUXC_SW_PAD_CTL_PAD_GPIO_AD_08 (*(volatile uint32_t *)0x400E8370u)",
          "#define IOMUXC_SW_PAD_CTL_PAD_GPIO_AD_09 (*(volatile uint32_t *)0x400E8374u)",
          "#define IOMUXC_LPI2C1_SCL_SELECT_INPUT   (*(volatile uint32_t *)0x400E85ACu)",
          "#define IOMUXC_LPI2C1_SDA_SELECT_INPUT   (*(volatile uint32_t *)0x400E85B0u)"]
```

- [ ] **Step 3: Regenerate the header**

Run: `cd ~/Development/rt1170/evkb/cores/imxrt1176 && python3 tools/gen_imxrt1176_h.py`
Expected: `wrote .../imxrt1176.h`

- [ ] **Step 4: Verify the macros are present and the header still compiles**

Run:
```bash
grep -cE "LPI2C1_MCR|LPI2C1_MTDR|CCM_LPCG98_DIRECT|IOMUXC_SW_MUX_CTL_PAD_GPIO_AD_08" ~/Development/rt1170/evkb/cores/imxrt1176/imxrt1176.h
cd ~/Development/rt1170/evkb/blink && cmake --build build 2>&1 | tail -2
```
Expected: count `4`; blink still builds OK (header syntactically valid).

- [ ] **Step 5: Commit**

```bash
cd ~/Development/rt1170/evkb/cores
git add imxrt1176/tools/gen_imxrt1176_h.py imxrt1176/imxrt1176.h
git commit -m "imxrt1176: generate LPI2C1 master register + clock + pin defs

Co-Authored-By: Claude Opus 4.8 <noreply@anthropic.com>"
```

---

## Task 3: Add `IRQ_LPI2C1` to the IRQ enum

**Files:**
- Modify: the header defining `IRQ_NUMBER_t` (find in Step 1)

- [ ] **Step 1: Locate the IRQ enum**

Run: `grep -rn "IRQ_LPUART1" ~/Development/rt1170/evkb/cores/imxrt1176/*.h`
Expected: a line like `IRQ_LPUART1 = 20,` inside an `enum IRQ_NUMBER_t { ... }` (note the file).

- [ ] **Step 2: Add the LPI2C1 IRQ**

In that enum, add (keep numeric ordering tidy; value must be exactly 32):
```c
	IRQ_LPI2C1 = 32,
```

- [ ] **Step 3: Verify it compiles**

Run: `cd ~/Development/rt1170/evkb/blink && cmake --build build 2>&1 | tail -2`
Expected: builds OK.

- [ ] **Step 4: Commit**

```bash
cd ~/Development/rt1170/evkb/cores
git add -A imxrt1176
git commit -m "imxrt1176: add IRQ_LPI2C1 (32) to IRQ_NUMBER_t

Co-Authored-By: Claude Opus 4.8 <noreply@anthropic.com>"
```

---

## Task 4: `Wire.h` — TwoWire class declaration

**Files:**
- Create: `~/Development/rt1170/evkb/cores/imxrt1176/Wire.h`

- [ ] **Step 1: Write the header**

```cpp
#ifndef Wire_h
#define Wire_h

#include <stdint.h>
#include <stddef.h>
#include "imxrt1176.h"
#include "core_pins.h"   // IRQ_NUMBER_t, attachInterruptVector (path per Task 3 Step 1)

#define BUFFER_LENGTH 32
#define WIRE_HAS_END  1

class TwoWire {
public:
	struct hardware_t {
		uint8_t  instance;                       // 0-based LPI2C index (LPI2C1 -> 0)
		IRQ_NUMBER_t irq;
		volatile uint32_t &lpcg;                 // CCM->LPCG[n].DIRECT
		volatile uint32_t &clock_root;           // CCM->CLOCK_ROOT[n].CONTROL
		uint32_t  clock_root_val;                // 0 => 24 MHz (OscRC48MDiv2)
		volatile uint32_t &scl_mux;  uint32_t scl_mux_val;  volatile uint32_t &scl_pad;
		volatile uint32_t &sda_mux;  uint32_t sda_mux_val;  volatile uint32_t &sda_pad;
		volatile uint32_t &scl_select_input; uint32_t scl_select_val;
		volatile uint32_t &sda_select_input; uint32_t sda_select_val;
		uint32_t  pad_ctl_val;                   // open-drain pad config (Task 8 resolves ODE bit)
		volatile uint32_t &mcr;
		volatile uint32_t &msr;
		volatile uint32_t &mcfgr1;
		volatile uint32_t &mccr0;
		volatile uint32_t &mtdr;
		volatile uint32_t &mrdr;
	};

	TwoWire(const hardware_t *hw) : hw(hw) {}

	void begin();
	void end();
	void setClock(uint32_t freq);

	void beginTransmission(uint8_t address) { tx_addr = address; tx_len = 0; }
	size_t write(uint8_t data);
	size_t write(const uint8_t *data, size_t len);
	uint8_t endTransmission(bool sendStop = true);

	uint8_t requestFrom(uint8_t address, uint8_t quantity, bool sendStop = true);
	int available() { return rx_len - rx_idx; }
	int read();
	int peek();

private:
	const hardware_t *hw;
	uint8_t tx_addr = 0;
	uint8_t tx_buf[BUFFER_LENGTH];
	uint8_t tx_len = 0;
	uint8_t rx_buf[BUFFER_LENGTH];
	uint8_t rx_len = 0;
	uint8_t rx_idx = 0;
	uint32_t clock_hz = 100000;

	bool wait_flag(uint32_t mask, uint32_t error_mask, uint32_t &err);
};

extern TwoWire Wire;

#endif
```

- [ ] **Step 2: Verify it parses (compile a throwaway TU)**

Run:
```bash
cd ~/Development/rt1170/evkb/cores/imxrt1176
arm-none-eabi-g++ -std=gnu++17 -fsyntax-only -I. Wire.h 2>&1 | head
```
Expected: no output beyond possible `core_pins.h` include resolution; if `core_pins.h` isn't the right include for `IRQ_NUMBER_t`/`attachInterruptVector`, adjust the `#include` to match Task 3 Step 1's file. (No errors = pass.)

- [ ] **Step 3: Commit**

```bash
cd ~/Development/rt1170/evkb/cores
git add imxrt1176/Wire.h
git commit -m "imxrt1176: add TwoWire (Wire) class declaration

Co-Authored-By: Claude Opus 4.8 <noreply@anthropic.com>"
```

---

## Task 5: Write the failing QEMU gate (sketch + CMake + runner)

**Files:**
- Create: `~/Development/rt1170/evkb/wire_master_test/wire_master_test.cpp`
- Create: `~/Development/rt1170/evkb/wire_master_test/CMakeLists.txt`
- Create: `~/Development/rt1170/evkb/wire_master_test/run_qemu_wire.sh`

The gate asserts, against the AT24C at 0x50: a bus scan finds 0x50, a write+readback round-trips, a present-address `endTransmission` returns 0, and an absent address returns 2. **AT24C protocol:** write = `[addr_byte][data...]` (first byte is the internal address pointer); read = write the 1-byte address pointer (no stop), then `requestFrom` N bytes.

- [ ] **Step 1: Write the gate sketch**

```cpp
#include "Arduino.h"
#include "Wire.h"

static void print_hex(uint8_t v) {
	const char *h = "0123456789ABCDEF";
	Serial1.print(h[v >> 4]); Serial1.print(h[v & 0xF]);
}

void setup() {
	Serial1.begin(115200);
	while (!Serial1) { }
	Wire.begin();
	Wire.setClock(400000);

	// 1. Bus scan: find the EEPROM.
	int found = -1;
	for (uint8_t a = 1; a < 0x7F; a++) {
		Wire.beginTransmission(a);
		if (Wire.endTransmission() == 0) { found = a; break; }
	}
	Serial1.print("scan_found=0x"); if (found >= 0) print_hex((uint8_t)found); else Serial1.print("NONE");
	Serial1.println();

	// 2. Write 4 bytes to EEPROM offset 0x00.
	Wire.beginTransmission(0x50);
	Wire.write((uint8_t)0x00);          // internal address pointer
	Wire.write((uint8_t)0xDE); Wire.write((uint8_t)0xAD);
	Wire.write((uint8_t)0xBE); Wire.write((uint8_t)0xEF);
	uint8_t wr = Wire.endTransmission();
	Serial1.print("wr_status="); Serial1.println(wr);

	// 3. Read them back: set pointer to 0x00 (no stop), then read 4.
	Wire.beginTransmission(0x50);
	Wire.write((uint8_t)0x00);
	Wire.endTransmission(false);
	Wire.requestFrom((uint8_t)0x50, (uint8_t)4, true);
	Serial1.print("readback=");
	while (Wire.available()) { print_hex((uint8_t)Wire.read()); Serial1.print(' '); }
	Serial1.println();

	// 4. Absent address must NACK -> status 2.
	Wire.beginTransmission(0x33);
	Serial1.print("absent_status="); Serial1.println(Wire.endTransmission());
}

void loop() { }
```

- [ ] **Step 2: Write CMakeLists.txt** (mirrors `analog_test`)

```cmake
cmake_minimum_required(VERSION 3.24)
project(wire_master_test)

set(TEENSY_VERSION 117 CACHE STRING "")

include(FetchContent)
FetchContent_Declare(teensy_cmake_macros SOURCE_DIR ${CMAKE_CURRENT_LIST_DIR}/../teensy-cmake-macros)
FetchContent_MakeAvailable(teensy_cmake_macros)
include(${teensy_cmake_macros_SOURCE_DIR}/CMakeLists.include.txt)

import_arduino_library(cores ${CMAKE_CURRENT_LIST_DIR}/../cores/imxrt1176)

teensy_add_executable(wire_master_test wire_master_test.cpp)
teensy_target_link_libraries(wire_master_test cores)

target_link_libraries(wire_master_test.elf stdc++)
```

- [ ] **Step 3: Write run_qemu_wire.sh** (mirrors `run_qemu_adc.sh`)

```bash
#!/bin/sh
set -e
QEMU=~/Development/qemu2/build/qemu-system-arm
DIR=$(cd "$(dirname "$0")" && pwd)
ELF="$DIR/build/wire_master_test.elf"; OUT="$DIR/wire.uart"
rm -f "$OUT"
"$QEMU" -M mimxrt1170-evk -global fsl-imxrt1170.boot-xip=on -kernel "$ELF" \
    -display none -serial file:"$OUT" -d guest_errors -D "$DIR/wire.dbg" &
P=$!; sleep 3; kill $P 2>/dev/null; wait $P 2>/dev/null || true
echo "==== captured ===="; cat "$OUT"
grep -q "scan_found=0x50"          "$OUT" || { echo "FAIL: scan"; exit 1; }
grep -q "wr_status=0"              "$OUT" || { echo "FAIL: write status"; exit 1; }
grep -q "readback=DE AD BE EF "    "$OUT" || { echo "FAIL: readback"; exit 1; }
grep -q "absent_status=2"          "$OUT" || { echo "FAIL: absent NACK"; exit 1; }
echo "PASS: Wire I2C master verified (scan, write, readback, NACK)"
```

- [ ] **Step 4: Configure the build**

Run:
```bash
cd ~/Development/rt1170/evkb/wire_master_test
cmake -B build -G Ninja . 2>&1 | tail -3
```
Expected: CMake configures (it will fail to *link* later until `Wire.cpp`/`Wire_instances.cpp` exist — that's the point). If configure itself errors, fix paths before proceeding.

- [ ] **Step 5: Confirm it fails to build (no Wire implementation yet)**

Run: `cd ~/Development/rt1170/evkb/wire_master_test && cmake --build build 2>&1 | tail -5`
Expected: **link error** — undefined reference to `Wire`, `TwoWire::begin()`, etc. This is the failing state.

- [ ] **Step 6: Commit the gate**

```bash
cd ~/Development/rt1170/evkb
chmod +x wire_master_test/run_qemu_wire.sh
git -C cores add -A 2>/dev/null || true
git add wire_master_test 2>/dev/null || true   # if evkb root is a repo; else this sketch dir tracks with cores' sibling convention
cd ~/Development/rt1170/evkb/cores && git add -A ../wire_master_test 2>/dev/null || true
# Commit in whichever repo tracks the sketches (mirror how analog_test is tracked):
```
Run first: `git -C ~/Development/rt1170/evkb rev-parse --show-toplevel` and commit the new `wire_master_test/` there with message `test: wire_master_test QEMU gate (failing until Wire implemented)`.

---

## Task 6: Implement master bring-up + TX (`begin`, `write`, `endTransmission`)

**Files:**
- Create: `~/Development/rt1170/evkb/cores/imxrt1176/Wire.cpp`
- Create: `~/Development/rt1170/evkb/cores/imxrt1176/Wire_instances.cpp`

- [ ] **Step 1: Write `Wire_instances.cpp` (hardware table + object)**

```cpp
#include "Wire.h"

static const TwoWire::hardware_t lpi2c1_hw = {
	/* instance */ 0,
	/* irq */ IRQ_LPI2C1,
	/* lpcg */ CCM_LPCG98_DIRECT,
	/* clock_root */ CCM_CLOCK_ROOT37_CONTROL,
	/* clock_root_val */ 0u,                 // OscRC48MDiv2 = 24 MHz
	/* scl_mux */ IOMUXC_SW_MUX_CTL_PAD_GPIO_AD_08, /* scl_mux_val */ 1u,
	/* scl_pad */ IOMUXC_SW_PAD_CTL_PAD_GPIO_AD_08,
	/* sda_mux */ IOMUXC_SW_MUX_CTL_PAD_GPIO_AD_09, /* sda_mux_val */ 1u,
	/* sda_pad */ IOMUXC_SW_PAD_CTL_PAD_GPIO_AD_09,
	/* scl_select_input */ IOMUXC_LPI2C1_SCL_SELECT_INPUT, /* scl_select_val */ 0u,
	/* sda_select_input */ IOMUXC_LPI2C1_SDA_SELECT_INPUT, /* sda_select_val */ 0u,
	/* pad_ctl_val */ 0x0000000Cu,           // provisional open-drain; Task 8 resolves ODE
	/* mcr */ LPI2C1_MCR, /* msr */ LPI2C1_MSR, /* mcfgr1 */ LPI2C1_MCFGR1,
	/* mccr0 */ LPI2C1_MCCR0, /* mtdr */ LPI2C1_MTDR, /* mrdr */ LPI2C1_MRDR,
};

TwoWire Wire(&lpi2c1_hw);
```

- [ ] **Step 2: Write `Wire.cpp` — begin/setClock/write/endTransmission**

```cpp
#include "Wire.h"

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

#define WIRE_TIMEOUT 100000u   // bounded guard-loop (like analogRead)

void TwoWire::begin() {
	hw->lpcg = 1u;                                     // ungate LPI2C clock
	hw->clock_root = hw->clock_root_val;              // 24 MHz functional clock
	hw->scl_mux = hw->scl_mux_val;  hw->scl_pad = hw->pad_ctl_val;
	hw->sda_mux = hw->sda_mux_val;  hw->sda_pad = hw->pad_ctl_val;
	hw->scl_select_input = hw->scl_select_val;
	hw->sda_select_input = hw->sda_select_val;
	hw->mcr = MCR_RST;                                 // reset the master block
	hw->mcr = 0u;
	setClock(clock_hz);                                // program MCCR0/MCFGR1
	hw->mcr = MCR_MEN;                                 // enable
}

void TwoWire::end() { hw->mcr = 0u; hw->lpcg = 0u; }

void TwoWire::setClock(uint32_t freq) {
	clock_hz = freq;
	const uint32_t src = 24000000u;
	uint32_t pre = 0, div = 0;
	for (pre = 0; pre < 8u; pre++) { div = (src >> pre) / freq; if (div <= 120u) break; }
	uint32_t clklo = (div * 6u) / 10u;                // ~60% low time (I2C tLOW>tHIGH)
	uint32_t clkhi = (div > clklo) ? (div - clklo) : 1u;
	if (clklo > 63u) clklo = 63u;
	if (clkhi > 63u) clkhi = 63u; if (clkhi < 1u) clkhi = 1u;
	uint32_t men = hw->mcr & MCR_MEN;
	hw->mcr = men & ~MCR_MEN;                          // MCCR/MCFGR need MEN=0
	hw->mcfgr1 = (hw->mcfgr1 & ~0x7u) | (pre & 0x7u);
	hw->mccr0 = (clklo) | (clkhi << 8) | ((clkhi/2u) << 16) | ((clkhi/2u) << 24);
	if (men) hw->mcr = MCR_MEN;
}

// Wait until any bit in `mask` is set, or an error bit appears / timeout.
// Returns true on success (mask seen, no error). On error/timeout sets err to a
// nonzero Arduino status.
bool TwoWire::wait_flag(uint32_t mask, uint32_t error_mask, uint32_t &err) {
	for (uint32_t g = 0; g < WIRE_TIMEOUT; g++) {
		uint32_t s = hw->msr;
		if (s & error_mask) {
			if (s & MSR_NDF) err = (err == 0xFFu) ? 2u : 3u; // addr vs data NACK
			else err = 4u;                                    // ALF/FEF/other
			hw->msr = s;                                      // W1C the flags
			return false;
		}
		if (s & mask) return true;
	}
	err = 5u;                                                 // timeout
	return false;
}

size_t TwoWire::write(uint8_t data) {
	if (tx_len >= BUFFER_LENGTH) return 0;
	tx_buf[tx_len++] = data; return 1;
}
size_t TwoWire::write(const uint8_t *data, size_t len) {
	size_t n = 0; while (n < len && write(data[n])) n++; return n;
}

uint8_t TwoWire::endTransmission(bool sendStop) {
	uint32_t err = 0xFFu;                                     // 0xFF => next NACK is address NACK
	hw->msr = hw->msr;                                        // clear stale flags
	hw->mtdr = TX_CMD(CMD_START, (tx_addr << 1) | 0u);        // START + addr(W)
	if (!wait_flag(MSR_TDF, MSR_NDF | MSR_ALF | MSR_FEF, err)) { if (sendStop) hw->mtdr = TX_CMD(CMD_STOP,0); return (uint8_t)err; }
	err = 0u;                                                 // address ACKed; further NACK = data NACK (3)
	for (uint8_t i = 0; i < tx_len; i++) {
		hw->mtdr = TX_CMD(CMD_TXD, tx_buf[i]);
		if (!wait_flag(MSR_TDF, MSR_NDF | MSR_ALF | MSR_FEF, err)) { if (sendStop) hw->mtdr = TX_CMD(CMD_STOP,0); return (uint8_t)err; }
	}
	if (sendStop) {
		hw->mtdr = TX_CMD(CMD_STOP, 0);
		wait_flag(MSR_SDF, MSR_ALF | MSR_FEF, err);
		hw->msr = MSR_SDF | MSR_EPF;
	}
	tx_len = 0;
	return 0u;
}
```

- [ ] **Step 3: Add both files to the cores library build**

Run: `grep -rn "HardwareSerial1.cpp\|analog.c\|GLOB\|\.cpp" ~/Development/rt1170/evkb/cores/imxrt1176/CMakeLists.txt 2>/dev/null | head`
Expected: reveals whether sources are globbed or listed. If listed explicitly, add `Wire.cpp` and `Wire_instances.cpp`. If globbed (`*.cpp`), nothing to do. (The `import_arduino_library` macro typically globs — verify.)

- [ ] **Step 4: Build the gate sketch (should now link; RX not yet implemented)**

Run: `cd ~/Development/rt1170/evkb/wire_master_test && cmake --build build 2>&1 | tail -5`
Expected: links OK (`requestFrom`/`read`/`peek` are still undefined — add temporary stubs only if the linker complains; Task 7 implements them). If link fails on `requestFrom`, proceed to Task 7 before running the gate.

- [ ] **Step 5: Commit**

```bash
cd ~/Development/rt1170/evkb/cores
git add imxrt1176/Wire.cpp imxrt1176/Wire_instances.cpp
git commit -m "imxrt1176: Wire I2C master bring-up + TX engine (begin/setClock/write/endTransmission)

Co-Authored-By: Claude Opus 4.8 <noreply@anthropic.com>"
```

---

## Task 7: Implement RX (`requestFrom`, `read`, `peek`) and pass the gate

**Files:**
- Modify: `~/Development/rt1170/evkb/cores/imxrt1176/Wire.cpp`

- [ ] **Step 1: Append the RX methods to `Wire.cpp`**

```cpp
uint8_t TwoWire::requestFrom(uint8_t address, uint8_t quantity, bool sendStop) {
	if (quantity > BUFFER_LENGTH) quantity = BUFFER_LENGTH;
	rx_len = 0; rx_idx = 0;
	uint32_t err = 0xFFu;
	hw->msr = hw->msr;
	hw->mtdr = TX_CMD(CMD_START, (address << 1) | 1u);        // START + addr(R)
	if (!wait_flag(MSR_TDF, MSR_NDF | MSR_ALF | MSR_FEF, err)) { if (sendStop) hw->mtdr = TX_CMD(CMD_STOP,0); return 0; }
	hw->mtdr = TX_CMD(CMD_RXD, (uint8_t)(quantity - 1));      // receive `quantity` bytes (N-1 encoding)
	for (uint8_t i = 0; i < quantity; i++) {
		err = 0u;
		if (!wait_flag(MSR_RDF, MSR_ALF | MSR_FEF, err)) break;
		uint32_t r = hw->mrdr;
		if (r & MRDR_RXEMPTY) break;
		rx_buf[rx_len++] = (uint8_t)(r & 0xFF);
	}
	if (sendStop) {
		hw->mtdr = TX_CMD(CMD_STOP, 0);
		wait_flag(MSR_SDF, MSR_ALF | MSR_FEF, err);
		hw->msr = MSR_SDF | MSR_EPF;
	}
	return rx_len;
}

int TwoWire::read()  { return (rx_idx < rx_len) ? rx_buf[rx_idx++] : -1; }
int TwoWire::peek()  { return (rx_idx < rx_len) ? rx_buf[rx_idx]   : -1; }
```

- [ ] **Step 2: Build**

Run: `cd ~/Development/rt1170/evkb/wire_master_test && cmake --build build 2>&1 | tail -3`
Expected: builds OK, produces `build/wire_master_test.elf`.

- [ ] **Step 3: Run the QEMU gate — expect PASS**

Run: `cd ~/Development/rt1170/evkb/wire_master_test && sh run_qemu_wire.sh`
Expected output includes:
```
scan_found=0x50
wr_status=0
readback=DE AD BE EF
absent_status=2
PASS: Wire I2C master verified (scan, write, readback, NACK)
```
If `scan_found=NONE`: the EEPROM isn't on the bus — recheck Task 1 (`soc->lpi2c[0].bus`, rebuild QEMU). If `readback` mismatches: check the AT24C address-pointer write (first `write` byte) and the `CMD_RXD` N-1 encoding.

- [ ] **Step 4: Regression — all gates green**

Run:
```bash
cd ~/Development/rt1170/evkb
(cd serial_test && sh run_qemu.sh 2>&1 | grep -iE "Serial1 up")
(cd analog_test && sh run_qemu_adc.sh 2>&1 | grep -iE "PASS|FAIL")
(cd serial_test_rx && sh run_qemu_rx.sh 2>&1 | grep -iE "PASS|FAIL" | head -1)
```
Expected: all PASS/up.

- [ ] **Step 5: Commit**

```bash
cd ~/Development/rt1170/evkb/cores
git add imxrt1176/Wire.cpp
git commit -m "imxrt1176: Wire I2C master RX (requestFrom/read/peek); QEMU gate passes

Co-Authored-By: Claude Opus 4.8 <noreply@anthropic.com>"
```

---

## Task 8: Hardware bring-up — resolve instance + ODE, SSD1306 acceptance

**Files:**
- Create: `~/Development/rt1170/evkb/wire_oled_test/wire_oled_test.cpp`
- Create: `~/Development/rt1170/evkb/wire_oled_test/CMakeLists.txt` (copy Task 5's, rename target `wire_oled_test`)
- Possibly modify: `~/Development/rt1170/evkb/cores/imxrt1176/Wire_instances.cpp` (pad value / instance)

- [ ] **Step 1: Resolve the Arduino-header instance**

Determine whether the EVKB Arduino I²C header (SDA/SCL, silkscreen near D14/D15) is LPI2C1 (`GPIO_AD_08/09`) or LPI2C5 (`GPIO_LPSR_04/05`). Method: consult the MIMXRT1170-EVKB schematic / board user guide, or continuity-check the header SDA/SCL pins to the SoC balls. **If LPI2C5**, regenerate with the LPI2C5 constants instead (base `0x40C34000`, IRQ `36`, CLOCK_ROOT41 `0x40CC1480`, LPCG102 `0x40CC6CC0`, pads `GPIO_LPSR_05` SCL / `GPIO_LPSR_04` SDA — grep `fsl_iomuxc.h` for `IOMUXC_GPIO_LPSR_05_LPI2C5_SCL` / `IOMUXC_GPIO_LPSR_04_LPI2C5_SDA` five-tuples) and update `lpi2c1_hw` accordingly. The engine code is instance-agnostic; only the `hardware_t` table changes.

- [ ] **Step 2: Resolve the pad ODE value**

From the RT1176 Reference Manual IOMUXC chapter (register `SW_PAD_CTL_PAD_GPIO_AD_08`), set `pad_ctl_val` to enable **open-drain (ODE)** plus keeper/pull-up. Update `pad_ctl_val` in `Wire_instances.cpp` with the correct ODE-bit-set value. External SSD1306 pull-ups back the bus, so the bus will function; ODE prevents the master from actively driving SCL/SDA high (correct I²C behaviour). Verify SDA/SCL idle high on a scope/meter after `Wire.begin()`.

- [ ] **Step 3: Write the SSD1306 acceptance sketch**

```cpp
#include "Arduino.h"
#include "Wire.h"
#define OLED 0x3C

static void cmd(uint8_t c) { Wire.beginTransmission(OLED); Wire.write((uint8_t)0x00); Wire.write(c); Wire.endTransmission(); }
static const uint8_t init_seq[] = {
	0xAE,0xD5,0x80,0xA8,0x3F,0xD3,0x00,0x40,0x8D,0x14,0x20,0x00,
	0xA1,0xC8,0xDA,0x12,0x81,0xCF,0xD9,0xF1,0xDB,0x40,0xA4,0xA6,0xAF
};

void setup() {
	Serial1.begin(115200); while (!Serial1) {}
	Wire.begin(); Wire.setClock(400000);

	Wire.beginTransmission(OLED);
	uint8_t ack = Wire.endTransmission();
	Serial1.print("oled_ack="); Serial1.println(ack);   // expect 0

	for (unsigned i = 0; i < sizeof(init_seq); i++) cmd(init_seq[i]);
	cmd(0x21); cmd(0); cmd(127);        // column range 0..127
	cmd(0x22); cmd(0); cmd(7);          // page range 0..7

	// Blit a checkerboard: 1024 data bytes, chunked to fit the 32-byte buffer.
	for (int page = 0; page < 8; page++) {
		for (int col = 0; col < 128; ) {
			Wire.beginTransmission(OLED);
			Wire.write((uint8_t)0x40);  // data stream control byte
			int n = 0;
			while (n < 16 && col < 128) { Wire.write((uint8_t)((col & 1) ? 0x55 : 0xAA)); col++; n++; }
			Wire.endTransmission();
		}
	}
	Serial1.println("oled_done");
}
void loop() {}
```

- [ ] **Step 4: Build + flash**

Run:
```bash
cd ~/Development/rt1170/evkb/wire_oled_test && cmake -B build -G Ninja . && cmake --build build
/Applications/LinkServer_26.6.137/LinkServer flash MIMXRT1176xxxxx:MIMXRT1170-EVKB load build/wire_oled_test.hex
```
Then power-cycle the EVKB (per the established flash→POR procedure).

- [ ] **Step 5: Verify on hardware**

Capture serial (pyserial @115200) and observe the OLED:
```bash
python3 - <<'PY'
import serial,glob,time
p=glob.glob('/dev/tty.usbmodem*')[0]; s=serial.Serial(p,115200,timeout=1)
end=time.time()+5
while time.time()<end:
    l=s.readline().decode(errors='replace').strip()
    if l: print(l)
PY
```
Expected: `oled_ack=0` and `oled_done` over serial, **and** a visible checkerboard on the SSD1306. `oled_ack=0` alone proves addressing/ACK; the rendered pattern proves ~1 KB of master writes landed correctly.

- [ ] **Step 6: Commit**

```bash
cd ~/Development/rt1170/evkb
# commit wire_oled_test/ and any Wire_instances.cpp pad/instance fix in the repos that track them
git -C cores add -A imxrt1176 2>/dev/null || true
cd ~/Development/rt1170/evkb/cores && git commit -m "imxrt1176: finalize LPI2C pad/instance for EVKB Arduino header (HW-verified)" 2>/dev/null || true
```
Commit `wire_oled_test/` in whichever repo tracks the sketches (same as `analog_test`).

---

## Task 9: Finish the branch

- [ ] **Step 1:** Run all QEMU gates once more (serial, adc, rx, wire) — confirm green.
- [ ] **Step 2:** Use **superpowers:finishing-a-development-branch** to merge/push both repos (cores + qemu2), mirroring the LPADC/voltage workflow.
- [ ] **Step 3:** Save a memory note (RT1176 LPI2C/Wire: instance, pads, ODE value, MCCR0 baud math) and update the task list.

---

## Self-review

**Spec coverage (Stage A):** master API (`begin`/`setClock`/`beginTransmission`/`write`/`endTransmission`/`requestFrom`/`read`/`available`/`peek`) — Tasks 4/6/7. Register defs via generator — Task 2. Clock/LPCG/pin bring-up — Tasks 2/6. 32-byte buffers — Task 4. Open-drain pads — Tasks 6/8. Status codes 0/2/3/4/5 + bounded timeout — Task 6 (`wait_flag`). Bus-busy/arb-loss/FIFO recovery — Task 6 (`MCR_RST` in `begin`, error_mask in `wait_flag`). QEMU AT24C gate — Tasks 1/5/7. SSD1306 HW (ACK + render) — Task 8. Single `Wire` instance (YAGNI) — Task 6. ✔ All Stage-A spec items map to a task.

**Placeholder scan:** The two board/RM-dependent values (instance choice, ODE bit) are explicit resolution *steps* (Task 8 Steps 1–2) with the exact register named and a concrete method — not silent gaps. `pad_ctl_val=0x0000000C` is a provisional that Task 8 finalizes; it does not affect the QEMU gate (model ignores pad regs). All firmware code is complete and inline.

**Type consistency:** `hardware_t` field names match between `Wire.h`, `Wire_instances.cpp`, and `Wire.cpp` (`lpcg`, `clock_root`, `mcr`, `msr`, `mcfgr1`, `mccr0`, `mtdr`, `mrdr`, `pad_ctl_val`). MSR/MCR/MTDR macros match the imxrt_lpi2c.c contract. `wait_flag` signature identical at declaration (Wire.h) and definition (Wire.cpp). Status-code convention (`0xFF` sentinel → address vs data NACK) is consistent between `endTransmission` and `wait_flag`.
