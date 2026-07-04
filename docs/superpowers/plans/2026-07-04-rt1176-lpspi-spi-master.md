# RT1176 Arduino SPI (LPSPI1 master) Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Implement the modern Arduino `SPI` library (master only) on the RT1176 LPSPI1 (EVKB Arduino-header SPI), verified by loopback in QEMU and on hardware.

**Architecture:** Blocking polled master engine (no ISR — `transfer()` is synchronous), mirroring the existing `Wire`/`analog` cores. New `SPI.{h,cpp}` + `SPI_instances.cpp` over an `LPSPI1` register block generated into `imxrt1176.h`. QEMU gate uses the existing `imxrt_lpspi` model plus an `ssi-loopback` peripheral on the bus (no model change); HW uses an external SDO→SDI jumper.

**Tech Stack:** C++ (Arduino core), ARM GCC 10.2.1 (`ARMGCC_DIR=/Applications/ARM_10`), CMake+Ninja, QEMU (`mimxrt1170-evk`), LinkServer flashing, pyserial VCOM capture.

**Reference (read before starting):**
- Existing peripheral to mirror: `cores/imxrt1176/Wire.h`, `Wire.cpp`, `Wire_instances.cpp`.
- Header generator: `cores/imxrt1176/tools/gen_imxrt1176_h.py` (see the `LPI2C1` block it appends).
- SDK A/B reference: `~/Development/mcuxsdk-ws/mcuxsdk/examples/driver_examples/lpspi/loopback/lpspi_loopback.c`.
- QEMU model: `~/Development/qemu2/hw/ssi/imxrt_lpspi.c`; loopback device: `~/Development/qemu2/hw/ssi/ssi_loopback.c` (`TYPE_SSI_LOOPBACK`).

**Confirmed hardware constants** (verified against `fsl_iomuxc.h` / `fsl_clock.h` / QEMU SoC):
- LPSPI1 base `0x40114000`; `IRQ_LPSPI1 = 38`.
- Clock: `kCLOCK_Root_Lpspi1 = 43` → `CCM_CLOCK_ROOT43_CONTROL = 0x40CC1580` (mux 0 = OscRC48MDiv2 = 24 MHz). `kCLOCK_Lpspi1 = 104` → `CCM_LPCG104_DIRECT = 0x40CC6D00`.
- Pins (all ALT **mode 0**, select-input **daisy 1**):

  | signal | pin | MUX_CTL | SELECT_INPUT | PAD_CTL |
  |---|---|---|---|---|
  | SCK | GPIO_AD_28 | 0x400E817C | 0x400E85D0 | 0x400E83C0 |
  | SOUT (MOSI) | GPIO_AD_30 | 0x400E8184 | 0x400E85D8 | 0x400E83C8 |
  | SIN (MISO) | GPIO_AD_31 | 0x400E8188 | 0x400E85D4 | 0x400E83CC |
  | PCS0 (CS) | GPIO_AD_29 | 0x400E8180 | 0x400E85CC | 0x400E83C4 |

  CS is **manual** (sketch drives GPIO_AD_29 via `digitalWrite`); we do **not** mux PCS0.

**LPSPI register map / bits** (offsets from base; used throughout):
- `CR 0x10`: MEN=1<<0, RST=1<<1, RTF=1<<8, RRF=1<<9
- `SR 0x14`: TDF=1<<0, RDF=1<<1, TCF=1<<10
- `CFGR1 0x24`: MASTER=1<<0
- `CCR 0x40`: SCKDIV[7:0], DBT[15:8], PCSSCK[23:16], SCKPCS[31:24]
- `TCR 0x60`: FRAMESZ[11:0], RXMSK=1<<19, PCS[25:24], PRESCALE[29:27], CPHA=1<<30, CPOL=1<<31, LSBF=1<<23
- `TDR 0x64` (write data); `RSR 0x70`: RXEMPTY=1<<1; `RDR 0x74` (read data)

**Clock formula:** `SCK = 24MHz / (prescale_div × (SCKDIV + 2))`, prescale_div ∈ {1,2,4,…,128} (TCR.PRESCALE = 0..7), SCKDIV ∈ 0..255. Choose smallest prescale whose SCKDIV lands in range and yields `SCK ≤ requested`. (4 MHz default → prescale 0, SCKDIV 4 → exactly 4 MHz.)

---

### Task 1: LPSPI1 register block in `imxrt1176.h`

**Files:**
- Modify: `cores/imxrt1176/tools/gen_imxrt1176_h.py` (append an LPSPI1 block, mirroring the existing LPI2C1 block)
- Regenerate: `cores/imxrt1176/imxrt1176.h`

- [ ] **Step 1: Append the LPSPI1 lines to the generator.** Find the list where the LPI2C1 block is appended (search `"/* LPI2C1 (Arduino-header I2C master)`). After that block's list, append these lines:

```python
          "/* LPSPI1 (Arduino-header SPI master), base 0x40114000 */",
          "#define LPSPI1_CR     (*(volatile uint32_t *)0x40114010u)",
          "#define LPSPI1_SR     (*(volatile uint32_t *)0x40114014u)",
          "#define LPSPI1_CFGR1  (*(volatile uint32_t *)0x40114024u)",
          "#define LPSPI1_CCR    (*(volatile uint32_t *)0x40114040u)",
          "#define LPSPI1_TCR    (*(volatile uint32_t *)0x40114060u)",
          "#define LPSPI1_TDR    (*(volatile uint32_t *)0x40114064u)",
          "#define LPSPI1_RSR    (*(volatile uint32_t *)0x40114070u)",
          "#define LPSPI1_RDR    (*(volatile uint32_t *)0x40114074u)",
          "/* LPSPI1 clock: CCM_CLOCK_ROOT43 + LPCG104 (mux 0 = 24 MHz) */",
          "#define CCM_CLOCK_ROOT43_CONTROL (*(volatile uint32_t *)0x40CC1580u)",
          "#define CCM_LPCG104_DIRECT       (*(volatile uint32_t *)0x40CC6D00u)",
          "/* LPSPI1 pins: SCK=GPIO_AD_28, SOUT=GPIO_AD_30, SIN=GPIO_AD_31 (ALT0) */",
          "#define IOMUXC_SW_MUX_CTL_PAD_GPIO_AD_28 (*(volatile uint32_t *)0x400E817Cu)",
          "#define IOMUXC_SW_MUX_CTL_PAD_GPIO_AD_30 (*(volatile uint32_t *)0x400E8184u)",
          "#define IOMUXC_SW_MUX_CTL_PAD_GPIO_AD_31 (*(volatile uint32_t *)0x400E8188u)",
          "#define IOMUXC_SW_PAD_CTL_PAD_GPIO_AD_28 (*(volatile uint32_t *)0x400E83C0u)",
          "#define IOMUXC_SW_PAD_CTL_PAD_GPIO_AD_30 (*(volatile uint32_t *)0x400E83C8u)",
          "#define IOMUXC_SW_PAD_CTL_PAD_GPIO_AD_31 (*(volatile uint32_t *)0x400E83CCu)",
          "#define IOMUXC_LPSPI1_SCK_SELECT_INPUT   (*(volatile uint32_t *)0x400E85D0u)",
          "#define IOMUXC_LPSPI1_SDO_SELECT_INPUT   (*(volatile uint32_t *)0x400E85D8u)",
          "#define IOMUXC_LPSPI1_SDI_SELECT_INPUT   (*(volatile uint32_t *)0x400E85D4u)"]
```

(If `IOMUXC_SW_MUX_CTL_PAD_GPIO_AD_29` / pad already exist from another block, do not duplicate — CS is a plain GPIO, and the GPIO pin table may already define AD_29. Grep first.)

- [ ] **Step 2: Regenerate the header.**

Run: `cd cores/imxrt1176 && python3 tools/gen_imxrt1176_h.py > imxrt1176.h`
Expected: file regenerates with no error.

- [ ] **Step 3: Verify the new symbols exist.**

Run: `grep -c "LPSPI1_CR\|CCM_CLOCK_ROOT43_CONTROL\|CCM_LPCG104_DIRECT\|IOMUXC_LPSPI1_SCK_SELECT_INPUT" cores/imxrt1176/imxrt1176.h`
Expected: `4`

- [ ] **Step 4: Commit.**

```bash
cd cores && git add imxrt1176/tools/gen_imxrt1176_h.py imxrt1176/imxrt1176.h
git commit -m "imxrt1176: generate LPSPI1 register block, clock root 43/LPCG104, header pins"
```

---

### Task 2: Add `IRQ_LPSPI1` to the interrupt enum

**Files:**
- Modify: `cores/imxrt1176/core_pins.h` (the `IRQ_NUMBER_t` enum; `IRQ_LPI2C1 = 32` is already there)

- [ ] **Step 1: Add the enumerator.** Locate `IRQ_LPI2C1 = 32,` in the `IRQ_NUMBER_t` enum. Add on the following line:

```c
	IRQ_LPSPI1 = 38, IRQ_LPSPI2, IRQ_LPSPI3, IRQ_LPSPI4, IRQ_LPSPI5, IRQ_LPSPI6, /* = 43 */
```

- [ ] **Step 2: Verify it compiles into the enum.**

Run: `grep -n "IRQ_LPSPI1 = 38" cores/imxrt1176/core_pins.h`
Expected: one match.

- [ ] **Step 3: Commit.**

```bash
cd cores && git add imxrt1176/core_pins.h
git commit -m "imxrt1176: add IRQ_LPSPI1..6 (38..43) to IRQ_NUMBER_t"
```

---

### Task 3: `SPI.h` — class, settings, hardware struct

**Files:**
- Create: `cores/imxrt1176/SPI.h`

- [ ] **Step 1: Write `SPI.h`.**

```cpp
#ifndef SPI_h
#define SPI_h

#include <stdint.h>
#include <stddef.h>
#include "imxrt1176.h"
#include "core_pins.h"

#define MSBFIRST 1
#define LSBFIRST 0
#define SPI_MODE0 0x00   // CPOL=0, CPHA=0
#define SPI_MODE1 0x01   // CPOL=0, CPHA=1
#define SPI_MODE2 0x02   // CPOL=1, CPHA=0
#define SPI_MODE3 0x03   // CPOL=1, CPHA=1

class SPISettings {
public:
	SPISettings(uint32_t clock, uint8_t bitOrder, uint8_t dataMode)
		: clock(clock), bitOrder(bitOrder), dataMode(dataMode) {}
	SPISettings() : clock(4000000), bitOrder(MSBFIRST), dataMode(SPI_MODE0) {}
	uint32_t clock;
	uint8_t  bitOrder;
	uint8_t  dataMode;
};

class SPIClass {
public:
	struct hardware_t {
		volatile uint32_t &cr;
		volatile uint32_t &sr;
		volatile uint32_t &cfgr1;
		volatile uint32_t &ccr;
		volatile uint32_t &tcr;
		volatile uint32_t &tdr;
		volatile uint32_t &rsr;
		volatile uint32_t &rdr;
		volatile uint32_t &lpcg;
		volatile uint32_t &clock_root;
		uint32_t clock_root_val;                 // 0 => mux0 = 24 MHz
		volatile uint32_t &sck_mux;  uint32_t sck_mux_val;  volatile uint32_t &sck_pad;
		volatile uint32_t &sdo_mux;  uint32_t sdo_mux_val;  volatile uint32_t &sdo_pad;
		volatile uint32_t &sdi_mux;  uint32_t sdi_mux_val;  volatile uint32_t &sdi_pad;
		volatile uint32_t &sck_select_input; uint32_t sck_select_val;
		volatile uint32_t &sdo_select_input; uint32_t sdo_select_val;
		volatile uint32_t &sdi_select_input; uint32_t sdi_select_val;
		uint32_t pad_ctl_val;
	};

	SPIClass(const hardware_t *hw) : hw(hw) {}

	void begin();
	void end();
	void beginTransaction(SPISettings settings);
	void endTransaction();
	uint8_t  transfer(uint8_t data);
	uint16_t transfer16(uint16_t data);
	void     transfer(void *buf, size_t count);

private:
	const hardware_t *hw;
	uint32_t tcr_base = 0;      // CPOL/CPHA/LSBF/PRESCALE/PCS, minus FRAMESZ
	uint32_t func_clock = 24000000;
	void setClockDivider(uint32_t clockHz);  // programs CCR, sets prescale in tcr_base
};

extern SPIClass SPI;

#endif
```

- [ ] **Step 2: Verify it parses (host syntax check is noisy; just confirm the ARM build later). Commit.**

```bash
cd cores && git add imxrt1176/SPI.h
git commit -m "imxrt1176: SPI.h (SPIClass + SPISettings + hardware_t)"
```

---

### Task 4: `SPI_instances.cpp` — the LPSPI1 instance

**Files:**
- Create: `cores/imxrt1176/SPI_instances.cpp`

- [ ] **Step 1: Write the instance literal.** Field order MUST match the `hardware_t` declaration in Task 3.

```cpp
#include "SPI.h"

/* EVKB Arduino-header SPI is LPSPI1: SCK=GPIO_AD_28, SOUT/MOSI=GPIO_AD_30,
 * SIN/MISO=GPIO_AD_31, all ALT mode 0. PCS0/CS=GPIO_AD_29 is left as a plain
 * GPIO (manual chip-select, Arduino convention). Push-pull pads (no open-drain,
 * no pull-ups): DSE drive enabled. */
static const SPIClass::hardware_t lpspi1_hw = {
	/* cr */ LPSPI1_CR, /* sr */ LPSPI1_SR, /* cfgr1 */ LPSPI1_CFGR1, /* ccr */ LPSPI1_CCR,
	/* tcr */ LPSPI1_TCR, /* tdr */ LPSPI1_TDR, /* rsr */ LPSPI1_RSR, /* rdr */ LPSPI1_RDR,
	/* lpcg */ CCM_LPCG104_DIRECT, /* clock_root */ CCM_CLOCK_ROOT43_CONTROL, /* clock_root_val */ 0u,
	/* sck */ IOMUXC_SW_MUX_CTL_PAD_GPIO_AD_28, 0x0u, IOMUXC_SW_PAD_CTL_PAD_GPIO_AD_28,
	/* sdo */ IOMUXC_SW_MUX_CTL_PAD_GPIO_AD_30, 0x0u, IOMUXC_SW_PAD_CTL_PAD_GPIO_AD_30,
	/* sdi */ IOMUXC_SW_MUX_CTL_PAD_GPIO_AD_31, 0x0u, IOMUXC_SW_PAD_CTL_PAD_GPIO_AD_31,
	/* sck_select */ IOMUXC_LPSPI1_SCK_SELECT_INPUT, 0x1u,
	/* sdo_select */ IOMUXC_LPSPI1_SDO_SELECT_INPUT, 0x1u,
	/* sdi_select */ IOMUXC_LPSPI1_SDI_SELECT_INPUT, 0x1u,
	/* pad_ctl_val */ 0x0000000Cu,   // DSE (bits[2:1]) drive; push-pull, no pull, no ODE
};

SPIClass SPI(&lpspi1_hw);
```

- [ ] **Step 2: Commit.**

```bash
cd cores && git add imxrt1176/SPI_instances.cpp
git commit -m "imxrt1176: LPSPI1 SPI instance (GPIO_AD_28/30/31, ALT0)"
```

---

### Task 5: Attach `ssi-loopback` to LPSPI1 in the QEMU machine

**Files:**
- Modify: `~/Development/qemu2/hw/arm/mimxrt1170-evk.c` (near the AT24C attach on `lpi2c[0].bus`)
- Possibly: `~/Development/qemu2/hw/arm/Kconfig` (ensure `SSI_LOOPBACK` is selected so the device links)

- [ ] **Step 1: Confirm the LPSPI SSI bus is reachable.** In `~/Development/qemu2/include/hw/ssi/imxrt_lpspi.h`, confirm the state struct exposes `SSIBus *bus;` (the model does `s->bus = ssi_create_bus(dev, "spi")`). The machine reaches it as `FSL_IMXRT1170(dev)->lpspi[0].bus`.

Run: `grep -n "SSIBus\|bus;" ~/Development/qemu2/include/hw/ssi/imxrt_lpspi.h`
Expected: a `SSIBus *bus;` field.

- [ ] **Step 2: Add the loopback device.** After the AT24C line (`at24c_eeprom_init(...->lpi2c[0].bus, 0x50, 256);`), add:

```c
    /* Test fixture: SSI loopback on LPSPI1 (Arduino-header SPI) -- echoes
     * MOSI->MISO so a master transfer reads back what it sent (mirrors an
     * external SDO->SDI jumper on the board). */
    {
        DeviceState *lb = qdev_new(TYPE_SSI_LOOPBACK);
        qdev_realize_and_unref(lb, BUS(FSL_IMXRT1170(dev)->lpspi[0].bus),
                               &error_fatal);
    }
```

Add the include near the top: `#include "hw/ssi/ssi.h"` and ensure `TYPE_SSI_LOOPBACK` is declared (it's in `hw/ssi/ssi_loopback.c`; if there's no public header, add `#define TYPE_SSI_LOOPBACK "ssi-loopback"` locally with a comment, or include its header if one exists — grep `hw/ssi` for a header first).

- [ ] **Step 3: Ensure the device is compiled in.** Check `~/Development/qemu2/hw/ssi/meson.build` includes `ssi_loopback.c` unconditionally or under a config the machine selects. If it's behind `CONFIG_SSI_LOOPBACK`, add `select SSI_LOOPBACK` to the `FSL_IMXRT1170`/machine entry in `hw/arm/Kconfig`.

Run: `grep -rn "ssi_loopback\|SSI_LOOPBACK" ~/Development/qemu2/hw/ssi/meson.build ~/Development/qemu2/hw/ssi/Kconfig`
Expected: the source is listed; note whether a Kconfig symbol gates it.

- [ ] **Step 4: Rebuild QEMU.**

Run: `cd ~/Development/qemu2/build && ninja qemu-system-arm 2>&1 | tail -3`
Expected: links to `qemu-system-arm` with no error.

- [ ] **Step 5: Commit (qemu2 repo).**

```bash
cd ~/Development/qemu2 && git add hw/arm/mimxrt1170-evk.c hw/arm/Kconfig
git commit -m "hw/arm/mimxrt1170-evk: attach ssi-loopback to LPSPI1 bus for SPI gate"
```

---

### Task 6: Failing QEMU gate — `spi_loopback_test`

**Files:**
- Create: `evkb/spi_loopback_test/spi_loopback_test.cpp`
- Create: `evkb/spi_loopback_test/CMakeLists.txt` (copy of `evkb/wire_master_test/CMakeLists.txt`, rename target `spi_loopback_test`, source `spi_loopback_test.cpp`)
- Create: `evkb/spi_loopback_test/toolchain/rt1170-evkb.toolchain.cmake` (copy from `evkb/wire_master_test/toolchain/`)
- Create: `evkb/spi_loopback_test/run_qemu_spi.sh` (copy of `evkb/wire_master_test/run_qemu_wire.sh`, adjust names + asserts)

- [ ] **Step 1: Write the test sketch.**

```cpp
#include "Arduino.h"
#include "HardwareSerial.h"
#include "SPI.h"

static void hex2(uint8_t v) { const char* h="0123456789ABCDEF"; Serial1.print(h[v>>4]); Serial1.print(h[v&0xF]); }

void setup() {
	Serial1.begin(115200);
	while (!Serial1) {}
	SPI.begin();

	bool ok = true;
	// Loopback (MOSI tied to MISO): transfer returns what it sent.
	SPI.beginTransaction(SPISettings(4000000, MSBFIRST, SPI_MODE0));
	uint8_t a = SPI.transfer(0xA5);
	uint8_t b = SPI.transfer(0x3C);
	if (a != 0xA5 || b != 0x3C) ok = false;

	uint8_t buf[4] = {0xDE, 0xAD, 0xBE, 0xEF};
	SPI.transfer(buf, 4);
	if (buf[0]!=0xDE || buf[1]!=0xAD || buf[2]!=0xBE || buf[3]!=0xEF) ok = false;

	uint16_t w = SPI.transfer16(0xBEEF);
	if (w != 0xBEEF) ok = false;
	SPI.endTransaction();

	// All four modes + LSB first still echo correctly through loopback.
	const uint8_t modes[4] = {SPI_MODE0, SPI_MODE1, SPI_MODE2, SPI_MODE3};
	for (int m = 0; m < 4; m++) {
		SPI.beginTransaction(SPISettings(1000000, LSBFIRST, modes[m]));
		if (SPI.transfer(0x5A) != 0x5A) ok = false;
		SPI.endTransaction();
	}

	Serial1.print("spi a=0x"); hex2(a); Serial1.print(" b=0x"); hex2(b);
	Serial1.print(" w=0x"); hex2(w>>8); hex2(w&0xFF);
	Serial1.println();
	Serial1.println(ok ? "SPI_LOOPBACK=PASS" : "SPI_LOOPBACK=FAIL");
}
void loop() {}
```

- [ ] **Step 2: Write the runner `run_qemu_spi.sh`.**

```sh
#!/bin/sh
set -e
QEMU=~/Development/qemu2/build/qemu-system-arm
DIR=$(cd "$(dirname "$0")" && pwd)
ELF="$DIR/build/spi_loopback_test.elf"; OUT="$DIR/spi.uart"
rm -f "$OUT"
"$QEMU" -M mimxrt1170-evk -global fsl-imxrt1170.boot-xip=on -kernel "$ELF" \
    -display none -serial file:"$OUT" -d guest_errors -D "$DIR/spi.dbg" &
P=$!; sleep 3; kill $P 2>/dev/null; wait $P 2>/dev/null || true
echo "==== captured ===="; cat "$OUT"
grep -q "SPI_LOOPBACK=PASS" "$OUT" || { echo "FAIL: SPI loopback"; exit 1; }
echo "PASS: SPI master loopback verified"
```

- [ ] **Step 3: Copy CMakeLists + toolchain, adjusting names.** In the copied `CMakeLists.txt` replace every `wire_master_test` with `spi_loopback_test`. It compiles the whole core `cores` library (which now includes `SPI.cpp`/`SPI_instances.cpp` once Task 7 lands).

- [ ] **Step 4: Configure + build — expect a LINK failure (no `SPI.cpp` yet).**

Run: `cd evkb/spi_loopback_test && export ARMGCC_DIR=/Applications/ARM_10 && cmake -B build -G Ninja -DCMAKE_TOOLCHAIN_FILE=toolchain/rt1170-evkb.toolchain.cmake . && cmake --build build 2>&1 | tail -5`
Expected: **FAIL** — undefined references to `SPIClass::begin()`, `SPIClass::transfer(...)`, `SPI`. This confirms the gate exercises the not-yet-written engine.

- [ ] **Step 5: Commit the failing gate.**

```bash
cd evkb && git add spi_loopback_test
git commit -m "spi: failing QEMU loopback gate (sketch + runner + cmake)"
```

---

### Task 7: `SPI.cpp` — the master engine (make the gate pass)

**Files:**
- Create: `cores/imxrt1176/SPI.cpp`

- [ ] **Step 1: Write `SPI.cpp`.**

```cpp
#include "SPI.h"

// CR
#define CR_MEN   (1u<<0)
#define CR_RST   (1u<<1)
// CFGR1
#define CFGR1_MASTER (1u<<0)
// TCR fields
#define TCR_FRAMESZ(n) ((uint32_t)((n) & 0xFFFu))   // n = bits-1
#define TCR_PRESCALE(p) (((uint32_t)(p) & 0x7u) << 27)
#define TCR_CPHA (1u<<30)
#define TCR_CPOL (1u<<31)
#define TCR_LSBF (1u<<23)
// RSR
#define RSR_RXEMPTY (1u<<1)

#define SPI_TIMEOUT 100000u

void SPIClass::begin() {
	hw->lpcg = 1u;                              // ungate LPSPI clock
	hw->clock_root = hw->clock_root_val;        // mux 0 => 24 MHz
	hw->sck_mux = hw->sck_mux_val;  hw->sck_pad = hw->pad_ctl_val;
	hw->sdo_mux = hw->sdo_mux_val;  hw->sdo_pad = hw->pad_ctl_val;
	hw->sdi_mux = hw->sdi_mux_val;  hw->sdi_pad = hw->pad_ctl_val;
	hw->sck_select_input = hw->sck_select_val;
	hw->sdo_select_input = hw->sdo_select_val;
	hw->sdi_select_input = hw->sdi_select_val;
	hw->cr = CR_RST;  hw->cr = 0u;              // reset the block (MEN=0)
	hw->cfgr1 = CFGR1_MASTER;                   // master mode (write while MEN=0)
	tcr_base = 0u;                              // MODE0, MSB first (prescale added next)
	setClockDivider(4000000);                   // default 4 MHz: writes CCR, ORs prescale into tcr_base
	hw->cr = CR_MEN;                            // enable
}

void SPIClass::end() { hw->cr = 0u; hw->lpcg = 0u; }

// Program CCR.SCKDIV and the PRESCALE bits of tcr_base for the requested SCK.
// SCK = func_clock / (prescale_div * (SCKDIV + 2)); pick smallest prescale with
// SCKDIV in [0,255] giving SCK <= clockHz.
void SPIClass::setClockDivider(uint32_t clockHz) {
	uint32_t prescale = 0, sckdiv = 0;
	for (prescale = 0; prescale < 8u; prescale++) {
		uint32_t pdiv = 1u << prescale;
		// smallest SCKDIV such that func/(pdiv*(SCKDIV+2)) <= clockHz
		uint32_t denom = pdiv * clockHz;
		uint32_t div = (func_clock + denom - 1u) / denom;   // ceil(func/(pdiv*clk))
		if (div < 2u) div = 2u;
		sckdiv = div - 2u;
		if (sckdiv <= 255u) break;
	}
	if (prescale > 7u) { prescale = 7u; sckdiv = 255u; }
	uint32_t men = hw->cr & CR_MEN;
	hw->cr = 0u;                                 // CCR is writable only with MEN=0
	hw->ccr = (hw->ccr & ~0xFFu) | (sckdiv & 0xFFu);
	if (men) hw->cr = CR_MEN;
	tcr_base = (tcr_base & ~(0x7u << 27)) | TCR_PRESCALE(prescale);
}

void SPIClass::beginTransaction(SPISettings s) {
	tcr_base = 0u;
	if (s.dataMode & 0x2) tcr_base |= TCR_CPOL;
	if (s.dataMode & 0x1) tcr_base |= TCR_CPHA;
	if (s.bitOrder == LSBFIRST) tcr_base |= TCR_LSBF;
	setClockDivider(s.clock);                    // adds PRESCALE bits to tcr_base
}

void SPIClass::endTransaction() { /* manual CS; nothing to release */ }

uint8_t SPIClass::transfer(uint8_t data) {
	hw->tcr = tcr_base | TCR_FRAMESZ(7);         // 8-bit frame
	hw->tdr = data;
	for (uint32_t g = 0; g < SPI_TIMEOUT; g++) {
		if (!(hw->rsr & RSR_RXEMPTY)) return (uint8_t)hw->rdr;
	}
	return 0xFFu;
}

uint16_t SPIClass::transfer16(uint16_t data) {
	hw->tcr = tcr_base | TCR_FRAMESZ(15);        // 16-bit frame
	hw->tdr = data;
	for (uint32_t g = 0; g < SPI_TIMEOUT; g++) {
		if (!(hw->rsr & RSR_RXEMPTY)) return (uint16_t)hw->rdr;
	}
	return 0xFFFFu;
}

void SPIClass::transfer(void *buf, size_t count) {
	uint8_t *p = (uint8_t *)buf;
	for (size_t i = 0; i < count; i++) p[i] = transfer(p[i]);
}
```

- [ ] **Step 2: Build the gate — expect PASS at link and in QEMU.**

Run: `cd evkb/spi_loopback_test && export ARMGCC_DIR=/Applications/ARM_10 && cmake --build build 2>&1 | tail -3 && ./run_qemu_spi.sh 2>&1 | tail -6`
Expected: builds; runner prints `SPI_LOOPBACK=PASS` and `PASS: SPI master loopback verified`. The value line should read `spi a=0xA5 b=0x3C w=0xBEEF`.

- [ ] **Step 3: Commit.**

```bash
cd cores && git add imxrt1176/SPI.cpp
git commit -m "imxrt1176: SPI master engine (begin/transaction/transfer); QEMU gate passes"
```

---

### Task 8: Hardware verification (SDO→SDI jumper + SDK A/B)

**Files:** none (uses `evkb/spi_loopback_test` from Task 6/7)

- [ ] **Step 1: Build the HW image.**

Run: `cd evkb/spi_loopback_test && export ARMGCC_DIR=/Applications/ARM_10 && cmake --build build 2>&1 | tail -2`
Expected: `spi_loopback_test.elf` built.

- [ ] **Step 2: Wire the loopback jumper.** On the EVKB Arduino header, connect **SDO (GPIO_AD_30) to SDI (GPIO_AD_31)** with one jumper. No pull-ups needed (push-pull).

- [ ] **Step 3: Flash + run (LinkServer `run` boots without a POR).**

Run: `pkill -f LinkServer; sleep 2; /Applications/LinkServer_26.6.137/LinkServer run MIMXRT1176:MIMXRT1170-EVKB evkb/spi_loopback_test/build/spi_loopback_test.elf` (in background)

- [ ] **Step 4: Capture VCOM and confirm PASS.**

Run a pyserial reader on `/dev/cu.usbmodem5DQ2DDHVWO5EI3` @115200 for ~6 s.
Expected: `spi a=0xA5 b=0x3C w=0xBEEF` and `SPI_LOOPBACK=PASS`. If FAIL/0xFF: the jumper isn't seated or SDO/SDI muxing is wrong — re-check Task 1 pin addresses and the jumper.

- [ ] **Step 5: SDK A/B cross-check.** Build and flash the SDK loopback example on the same jumper:

```bash
cd ~/Development/mcuxsdk-ws/mcuxsdk
ARMGCC_DIR=/Applications/ARM_10 west build -b evkbmimxrt1170 \
  examples/driver_examples/lpspi/loopback --toolchain armgcc -Dcore_id=cm7 -p always \
  -d /tmp/sdk_lpspi_loopback
/Applications/LinkServer_26.6.137/LinkServer run MIMXRT1176:MIMXRT1170-EVKB \
  /tmp/sdk_lpspi_loopback/*.elf
```
Expected: SDK prints `LPSPI loopback test pass!!!` on the VCOM. If ours passes and the SDK passes on the same jumper, the driver is HW-confirmed; if both fail, it's the jumper/bench (per the I²C lesson).

- [ ] **Step 6: Record the result** (no commit; HW verification is observational). Note pass/fail in the finish-branch summary.

---

### Task 9: Finish the branch

- [ ] **Step 1: Run all QEMU gates (regression).**

Run each and confirm PASS: `evkb/spi_loopback_test/run_qemu_spi.sh`, `evkb/wire_master_test/run_qemu_wire.sh`, `evkb/wire_slave_test/run_qemu_wire_slave.sh`.
Expected: all print their PASS lines.

- [ ] **Step 2: Push both repos.**

```bash
cd cores && git push
cd ~/Development/qemu2 && git push
cd evkb && git push   # if the evkb repo tracks the test sketches
```

- [ ] **Step 3: Update memory.** Add `~/.claude/projects/-Users-nicholasnewdigate-Development-rt1170/memory/rt1176-lpspi-spi.md` capturing: LPSPI1 = header SPI (SCK AD_28 / SOUT AD_30 / SIN AD_31, ALT0; clock root 43, LPCG 104); blocking polled master; loopback = external SDO→SDI jumper (no internal loopback bit); QEMU gate uses ssi-loopback on lpspi[0].bus. Add a one-line pointer in `MEMORY.md`.

- [ ] **Step 4: Use superpowers:finishing-a-development-branch** to complete (tests already verified in Step 1).

---

## Self-review notes (author checklist — completed)

- **Spec coverage:** master-only modern API (T3/T7), SPISettings→register mapping (T7 `beginTransaction`/`setClockDivider`), transfer/transfer16/transfer(buf,len) (T7), manual CS (T4 — PCS0 not muxed), LPSPI1 pins/clock (T1), QEMU ssi-loopback gate (T5/T6), HW jumper + SDK A/B (T8). All covered.
- **Type consistency:** `hardware_t` field order in T4 matches T3; `tcr_base`/`func_clock`/`setClockDivider` used consistently in T7 as declared in T3.
- **Known unknowns flagged in-task:** T5 Step 1 verifies the `lpspi[0].bus` field name and `TYPE_SSI_LOOPBACK` visibility before use; T1 Step 1 says to grep for pre-existing AD_29 macros to avoid duplicate defs.
</content>
