# RT1176 Pin Table + FlexPWM analogWrite Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax.

**Goal:** Complete the digital pin table for the whole Arduino header (D0–D15, A0–A5) and implement `analogWrite`/`analogWriteFrequency`/`analogWriteResolution` on FlexPWM.

**Architecture:** Phase A fills `digital_pin_to_info[]` (header numbering: pin N = DN, A0–A5 = 16–21, `LED_BUILTIN=3`). Phase B adds a stateless FlexPWM engine (`pwm.c`) with a `pin → {module,submodule,channel,alt}` table. HW-verified with a Saleae logic analyzer (no QEMU FlexPWM model).

**Tech Stack:** C (Arduino core), ARM GCC 10.2.1 (`ARMGCC_DIR=/Applications/ARM_10`), CMake+Ninja, LinkServer, Saleae Logic 2 + `logic2-automation` (server on 127.0.0.1:10430).

**Reference:** `cores/imxrt1176/digital.c` (existing pin13 entry to mirror), `cores/imxrt1176/tools/gen_imxrt1176_h.py`, `cores/teensy4/pwm.c` (FlexPWM sequence), `cores/teensy4/imxrt.h` (FlexPWM struct).

**Confirmed constants**
- GPIO bases: GPIO8=`0x40C60000`, GPIO9=`0x40C64000`, GPIO11=`0x40C6C000`, GPIO12=`0x40C70000`. GPIO reg offsets: GDIR +0x04, DR_SET +0x84, DR_CLEAR +0x88, PSR +0x08. **GPIO ALT for all header pins = `0xA`.**
- FlexPWM bases: PWM1=`0x4018C000`, PWM2=`0x40190000`, PWM3=`0x40194000`, PWM4=`0x40198000`. Submodule stride `0x60`. Per-SM offsets: CTRL=+0x06, VAL0=+0x0A, VAL1=+0x0E, VAL3=+0x16, VAL5=+0x1E, CTRL2=+0x04, INIT=+0x02. Module: OUTEN=+0x180, MCTRL=+0x188. FlexPWM1 clock gate LPCG79=`0x40CC69E0`.
- FlexPWM bit fields: `MCTRL_LDOK(m)=(m&0xF)<<0`, `MCTRL_CLDOK(m)=(m&0xF)<<4`, `MCTRL_RUN(m)=(m&0xF)<<8`; `OUTEN_PWMX_EN(m)=(m&0xF)<<0`, `OUTEN_PWMB_EN(m)=(m&0xF)<<4`, `OUTEN_PWMA_EN(m)=(m&0xF)<<8`; `SMCTRL_PRSC(n)=(n&0xF)<<4`, `SMCTRL_FULL=1<<10`; `SMCTRL2_INDEP=1<<13`, `SMCTRL2_CLK_SEL=0` (IPG bus clock).

**Header pin map** (pin = DN; each: pad, GPIO base/bit, mux reg, pad reg; ALT 0xA):

| pin | pad | GPIO.bit | MUX | PAD |
|--|--|--|--|--|
| 0 (D0) | DISP_B2_11 | 11.12 | 0x400E8240 | 0x400E8484 |
| 1 (D1) | DISP_B2_10 | 11.11 | 0x400E823C | 0x400E8480 |
| 2 (D2) | DISP_B2_12 | 11.13 | 0x400E8244 | 0x400E8488 |
| 3 (D3) | AD_04 | 9.3 | 0x400E811C | 0x400E8360 |
| 4 (D4) | AD_06 | 9.5 | 0x400E8124 | 0x400E8368 |
| 5 (D5) | AD_05 | 9.4 | 0x400E8120 | 0x400E8364 |
| 6 (D6) | AD_00 | 8.31 | 0x400E810C | 0x400E8350 |
| 7 (D7) | AD_14 | 9.13 | 0x400E8144 | 0x400E8388 |
| 8 (D8) | AD_07 | 9.6 | 0x400E8128 | 0x400E836C |
| 9 (D9) | AD_01 | 9.0 | 0x400E8110 | 0x400E8354 |
| 10 (D10) | AD_29 | 9.28 | 0x400E8180 | 0x400E83C4 |
| 11 (D11) | AD_30 | 9.29 | 0x400E8184 | 0x400E83C8 |
| 12 (D12) | AD_31 | 9.30 | 0x400E8188 | 0x400E83CC |
| 13 (D13) | AD_28 | 9.27 | 0x400E817C | 0x400E83C0 |
| 14 (D14) | LPSR_04 | 12.4 | 0x40C08010 | 0x40C08050 |
| 15 (D15) | LPSR_05 | 12.5 | 0x40C08014 | 0x40C08054 |
| 16 (A0) | AD_10 | 9.9 | 0x400E8134 | 0x400E8378 |
| 17 (A1) | AD_11 | 9.10 | 0x400E8138 | 0x400E837C |
| 18 (A2) | AD_12 | 9.11 | 0x400E813C | 0x400E8380 |
| 19 (A3) | AD_13 | 9.12 | 0x400E8140 | 0x400E8384 |
| 20 (A4) | AD_09 | 9.8 | 0x400E8130 | 0x400E8374 |
| 21 (A5) | AD_08 | 9.7 | 0x400E812C | 0x400E8370 |

**PWM pin table** (subset with FlexPWM routing; ALT differs from GPIO):

| pin | pad | module | submodule | chan | ALT |
|--|--|--|--|--|--|
| 9 (D9) | AD_01 | PWM1 | 0 | B | 4 |
| 3 (D3) | AD_04 | PWM1 | 2 | A | 4 |
| 5 (D5) | AD_05 | PWM1 | 2 | B | 4 |
| 4 (D4) | AD_06 | PWM1 | 0 | X | 0xB |
| 8 (D8) | AD_07 | PWM1 | 1 | X | 0xB |
| 7 (D7) | AD_14 | PWM3 | 0 | X | 0xB |

---

## Phase A — digital pin table

### Task A1: Generate header pad/GPIO defs into imxrt1176.h

**Files:** Modify `cores/imxrt1176/tools/gen_imxrt1176_h.py`; regenerate `cores/imxrt1176/imxrt1176.h`.

- [ ] **Step 1:** In the generator, after the LPSPI1 block, append a "digital header pins" block. Add the GPIO bases + every MUX/PAD def from the Header-pin-map table **that is not already defined** (grep first: AD_08/09/28/30/31 muxes/pads and GPIO9_BASE already exist from the I2C/SPI/LED work — do NOT redefine). Concretely append these lines (Python list-append style, matching the existing blocks):

```python
          "/* Digital header GPIO bases */",
          "#define GPIO8_BASE  0x40C60000u",
          "#define GPIO11_BASE 0x40C6C000u",
          "#define GPIO12_BASE 0x40C70000u",
          "/* Arduino-header pin pads (GPIO ALT = 0xA) */",
          "#define IOMUXC_SW_MUX_CTL_PAD_GPIO_DISP_B2_10 (*(volatile uint32_t *)0x400E823Cu)",
          "#define IOMUXC_SW_MUX_CTL_PAD_GPIO_DISP_B2_11 (*(volatile uint32_t *)0x400E8240u)",
          "#define IOMUXC_SW_MUX_CTL_PAD_GPIO_DISP_B2_12 (*(volatile uint32_t *)0x400E8244u)",
          "#define IOMUXC_SW_PAD_CTL_PAD_GPIO_DISP_B2_10 (*(volatile uint32_t *)0x400E8480u)",
          "#define IOMUXC_SW_PAD_CTL_PAD_GPIO_DISP_B2_11 (*(volatile uint32_t *)0x400E8484u)",
          "#define IOMUXC_SW_PAD_CTL_PAD_GPIO_DISP_B2_12 (*(volatile uint32_t *)0x400E8488u)",
          "#define IOMUXC_SW_MUX_CTL_PAD_GPIO_LPSR_04 (*(volatile uint32_t *)0x40C08010u)",
          "#define IOMUXC_SW_MUX_CTL_PAD_GPIO_LPSR_05 (*(volatile uint32_t *)0x40C08014u)",
          "#define IOMUXC_SW_PAD_CTL_PAD_GPIO_LPSR_04 (*(volatile uint32_t *)0x40C08050u)",
          "#define IOMUXC_SW_PAD_CTL_PAD_GPIO_LPSR_05 (*(volatile uint32_t *)0x40C08054u)",
          "#define IOMUXC_SW_MUX_CTL_PAD_GPIO_AD_00 (*(volatile uint32_t *)0x400E810Cu)",
          "#define IOMUXC_SW_MUX_CTL_PAD_GPIO_AD_01 (*(volatile uint32_t *)0x400E8110u)",
          "#define IOMUXC_SW_MUX_CTL_PAD_GPIO_AD_04 (*(volatile uint32_t *)0x400E811Cu)",
          "#define IOMUXC_SW_MUX_CTL_PAD_GPIO_AD_05 (*(volatile uint32_t *)0x400E8120u)",
          "#define IOMUXC_SW_MUX_CTL_PAD_GPIO_AD_06 (*(volatile uint32_t *)0x400E8124u)",
          "#define IOMUXC_SW_MUX_CTL_PAD_GPIO_AD_10 (*(volatile uint32_t *)0x400E8134u)",
          "#define IOMUXC_SW_MUX_CTL_PAD_GPIO_AD_11 (*(volatile uint32_t *)0x400E8138u)",
          "#define IOMUXC_SW_MUX_CTL_PAD_GPIO_AD_12 (*(volatile uint32_t *)0x400E813Cu)",
          "#define IOMUXC_SW_MUX_CTL_PAD_GPIO_AD_13 (*(volatile uint32_t *)0x400E8140u)",
          "#define IOMUXC_SW_MUX_CTL_PAD_GPIO_AD_14 (*(volatile uint32_t *)0x400E8144u)",
          "#define IOMUXC_SW_PAD_CTL_PAD_GPIO_AD_00 (*(volatile uint32_t *)0x400E8350u)",
          "#define IOMUXC_SW_PAD_CTL_PAD_GPIO_AD_01 (*(volatile uint32_t *)0x400E8354u)",
          "#define IOMUXC_SW_PAD_CTL_PAD_GPIO_AD_04 (*(volatile uint32_t *)0x400E8360u)",
          "#define IOMUXC_SW_PAD_CTL_PAD_GPIO_AD_05 (*(volatile uint32_t *)0x400E8364u)",
          "#define IOMUXC_SW_PAD_CTL_PAD_GPIO_AD_06 (*(volatile uint32_t *)0x400E8368u)",
          "#define IOMUXC_SW_PAD_CTL_PAD_GPIO_AD_10 (*(volatile uint32_t *)0x400E8378u)",
          "#define IOMUXC_SW_PAD_CTL_PAD_GPIO_AD_11 (*(volatile uint32_t *)0x400E837Cu)",
          "#define IOMUXC_SW_PAD_CTL_PAD_GPIO_AD_12 (*(volatile uint32_t *)0x400E8380u)",
          "#define IOMUXC_SW_PAD_CTL_PAD_GPIO_AD_13 (*(volatile uint32_t *)0x400E8384u)",
          "#define IOMUXC_SW_PAD_CTL_PAD_GPIO_AD_14 (*(volatile uint32_t *)0x400E8388u)"]
```

(Note: GPIO_AD_07 mux/pad, AD_28/29/30/31, AD_08/09 already exist from SPI/I2C — grep `imxrt1176.h` and skip any duplicate.)

- [ ] **Step 2:** Regenerate: `cd cores/imxrt1176 && python3 tools/gen_imxrt1176_h.py > imxrt1176.h`. Verify no duplicate defines: `grep -oE "^#define [A-Za-z0-9_]+" imxrt1176.h | sort | uniq -d` → empty.
- [ ] **Step 3:** Commit: `cd cores && git add imxrt1176/tools/gen_imxrt1176_h.py imxrt1176/imxrt1176.h && git commit -m "imxrt1176: generate Arduino-header pad/GPIO defs for the pin table"`

### Task A2: Populate digital_pin_to_info[] + header numbering

**Files:** Modify `cores/imxrt1176/digital.c`, `cores/imxrt1176/core_pins.h` (or `pins_arduino.h` for `LED_BUILTIN`/`CORE_NUM_DIGITAL`).

- [ ] **Step 1:** In `core_pins.h`/`pins_arduino.h` set `#define CORE_NUM_DIGITAL 22` and `#define LED_BUILTIN 3`. Grep for existing `CORE_NUM_DIGITAL`/`LED_BUILTIN` and update in place (the old value was 13-based).

- [ ] **Step 2:** Replace the `digital_pin_to_info[]` initializer in `digital.c` with all 22 entries. Each entry uses `.mux_mode = 0xA` (GPIO ALT), the GPIO base, bit, and the mux/pad regs from the Header-pin-map. Example for the first few (follow this exact shape for all 22; the LED is now pin 3):

```c
const struct digital_pin_info_struct digital_pin_to_info[CORE_NUM_DIGITAL] = {
	[0]  = { .gpio=GPIO11_BASE, .bit=12, .mux_mode=0xAu, .mux=&IOMUXC_SW_MUX_CTL_PAD_GPIO_DISP_B2_11, .pad=&IOMUXC_SW_PAD_CTL_PAD_GPIO_DISP_B2_11 },
	[1]  = { .gpio=GPIO11_BASE, .bit=11, .mux_mode=0xAu, .mux=&IOMUXC_SW_MUX_CTL_PAD_GPIO_DISP_B2_10, .pad=&IOMUXC_SW_PAD_CTL_PAD_GPIO_DISP_B2_10 },
	[2]  = { .gpio=GPIO11_BASE, .bit=13, .mux_mode=0xAu, .mux=&IOMUXC_SW_MUX_CTL_PAD_GPIO_DISP_B2_12, .pad=&IOMUXC_SW_PAD_CTL_PAD_GPIO_DISP_B2_12 },
	[3]  = { .gpio=GPIO9_BASE,  .bit=3,  .mux_mode=0xAu, .mux=&IOMUXC_SW_MUX_CTL_PAD_GPIO_AD_04, .pad=&IOMUXC_SW_PAD_CTL_PAD_GPIO_AD_04 }, /* LED_BUILTIN */
	[4]  = { .gpio=GPIO9_BASE,  .bit=5,  .mux_mode=0xAu, .mux=&IOMUXC_SW_MUX_CTL_PAD_GPIO_AD_06, .pad=&IOMUXC_SW_PAD_CTL_PAD_GPIO_AD_06 },
	[5]  = { .gpio=GPIO9_BASE,  .bit=4,  .mux_mode=0xAu, .mux=&IOMUXC_SW_MUX_CTL_PAD_GPIO_AD_05, .pad=&IOMUXC_SW_PAD_CTL_PAD_GPIO_AD_05 },
	[6]  = { .gpio=GPIO8_BASE,  .bit=31, .mux_mode=0xAu, .mux=&IOMUXC_SW_MUX_CTL_PAD_GPIO_AD_00, .pad=&IOMUXC_SW_PAD_CTL_PAD_GPIO_AD_00 },
	[7]  = { .gpio=GPIO9_BASE,  .bit=13, .mux_mode=0xAu, .mux=&IOMUXC_SW_MUX_CTL_PAD_GPIO_AD_14, .pad=&IOMUXC_SW_PAD_CTL_PAD_GPIO_AD_14 },
	[8]  = { .gpio=GPIO9_BASE,  .bit=6,  .mux_mode=0xAu, .mux=&IOMUXC_SW_MUX_CTL_PAD_GPIO_AD_07, .pad=&IOMUXC_SW_PAD_CTL_PAD_GPIO_AD_07 },
	[9]  = { .gpio=GPIO9_BASE,  .bit=0,  .mux_mode=0xAu, .mux=&IOMUXC_SW_MUX_CTL_PAD_GPIO_AD_01, .pad=&IOMUXC_SW_PAD_CTL_PAD_GPIO_AD_01 },
	[10] = { .gpio=GPIO9_BASE,  .bit=28, .mux_mode=0xAu, .mux=&IOMUXC_SW_MUX_CTL_PAD_GPIO_AD_29, .pad=&IOMUXC_SW_PAD_CTL_PAD_GPIO_AD_29 },
	[11] = { .gpio=GPIO9_BASE,  .bit=29, .mux_mode=0xAu, .mux=&IOMUXC_SW_MUX_CTL_PAD_GPIO_AD_30, .pad=&IOMUXC_SW_PAD_CTL_PAD_GPIO_AD_30 },
	[12] = { .gpio=GPIO9_BASE,  .bit=30, .mux_mode=0xAu, .mux=&IOMUXC_SW_MUX_CTL_PAD_GPIO_AD_31, .pad=&IOMUXC_SW_PAD_CTL_PAD_GPIO_AD_31 },
	[13] = { .gpio=GPIO9_BASE,  .bit=27, .mux_mode=0xAu, .mux=&IOMUXC_SW_MUX_CTL_PAD_GPIO_AD_28, .pad=&IOMUXC_SW_PAD_CTL_PAD_GPIO_AD_28 },
	[14] = { .gpio=GPIO12_BASE, .bit=4,  .mux_mode=0xAu, .mux=&IOMUXC_SW_MUX_CTL_PAD_GPIO_LPSR_04, .pad=&IOMUXC_SW_PAD_CTL_PAD_GPIO_LPSR_04 },
	[15] = { .gpio=GPIO12_BASE, .bit=5,  .mux_mode=0xAu, .mux=&IOMUXC_SW_MUX_CTL_PAD_GPIO_LPSR_05, .pad=&IOMUXC_SW_PAD_CTL_PAD_GPIO_LPSR_05 },
	[16] = { .gpio=GPIO9_BASE,  .bit=9,  .mux_mode=0xAu, .mux=&IOMUXC_SW_MUX_CTL_PAD_GPIO_AD_10, .pad=&IOMUXC_SW_PAD_CTL_PAD_GPIO_AD_10 },
	[17] = { .gpio=GPIO9_BASE,  .bit=10, .mux_mode=0xAu, .mux=&IOMUXC_SW_MUX_CTL_PAD_GPIO_AD_11, .pad=&IOMUXC_SW_PAD_CTL_PAD_GPIO_AD_11 },
	[18] = { .gpio=GPIO9_BASE,  .bit=11, .mux_mode=0xAu, .mux=&IOMUXC_SW_MUX_CTL_PAD_GPIO_AD_12, .pad=&IOMUXC_SW_PAD_CTL_PAD_GPIO_AD_12 },
	[19] = { .gpio=GPIO9_BASE,  .bit=12, .mux_mode=0xAu, .mux=&IOMUXC_SW_MUX_CTL_PAD_GPIO_AD_13, .pad=&IOMUXC_SW_PAD_CTL_PAD_GPIO_AD_13 },
	[20] = { .gpio=GPIO9_BASE,  .bit=8,  .mux_mode=0xAu, .mux=&IOMUXC_SW_MUX_CTL_PAD_GPIO_AD_09, .pad=&IOMUXC_SW_PAD_CTL_PAD_GPIO_AD_09 },
	[21] = { .gpio=GPIO9_BASE,  .bit=7,  .mux_mode=0xAu, .mux=&IOMUXC_SW_MUX_CTL_PAD_GPIO_AD_08, .pad=&IOMUXC_SW_PAD_CTL_PAD_GPIO_AD_08 },
};
```

Confirm the `digital_pin_info_struct` field names match the existing struct (`gpio`, `bit`, `mux_mode`, `mux`, `pad`). If the existing pin-13 entry used a macro like `IOMUXC_PAD_GPIO_AD_04_GPIO9_IO03_ALT` for `mux_mode`, the literal `0xAu` is the same value — use `0xAu` for uniformity.

- [ ] **Step 3:** Build any existing sketch that links the core (e.g. the blink or spi test build dir) to confirm the core compiles: `cd evkb/spi_loopback_test && export ARMGCC_DIR=/Applications/ARM_10 && cmake --build build 2>&1 | tail -3` → links OK.
- [ ] **Step 4:** Commit: `cd cores && git add imxrt1176/digital.c imxrt1176/core_pins.h imxrt1176/pins_arduino.h && git commit -m "imxrt1176: full Arduino-header digital pin table (D0-D15/A0-A5), header numbering, LED_BUILTIN=3"`

### Task A3: Hardware smoke test (pin table)

**Files:** Create `evkb/pintable_test/pintable_test.cpp` (+ CMakeLists/toolchain copied from `evkb/spi_loopback_test`).

- [ ] **Step 1:** Sketch: blink `LED_BUILTIN` (pin 3) and toggle pins 8 + 10 (the ST7735 DC/CS pads) so a scope/LED confirms them; print over Serial1.

```cpp
#include "Arduino.h"
#include "HardwareSerial.h"
void setup() {
	Serial1.begin(115200); while(!Serial1){}
	pinMode(LED_BUILTIN, OUTPUT); pinMode(8, OUTPUT); pinMode(10, OUTPUT);
	Serial1.println("pintable: blinking LED(3) + toggling D8/D10");
}
void loop() {
	static uint8_t s=0; s^=1;
	digitalWrite(LED_BUILTIN, s); digitalWrite(8, s); digitalWrite(10, s^1);
	delay(300);
}
```

- [ ] **Step 2:** Build + flash: `cd evkb/pintable_test && export ARMGCC_DIR=/Applications/ARM_10 && cmake -B build -G Ninja -DCMAKE_TOOLCHAIN_FILE=toolchain/rt1170-evkb.toolchain.cmake . && cmake --build build && pkill -f LinkServer; sleep 2; /Applications/LinkServer_26.6.137/LinkServer run MIMXRT1176:MIMXRT1170-EVKB build/pintable_test.elf` (background). Confirm the green LED blinks (~1.6 Hz). Ask the user to confirm the LED blink (D8/D10 are verified more rigorously by the PWM Saleae step later).
- [ ] **Step 3:** Commit the test: `cd evkb && git add pintable_test && git commit -m "pintable: HW smoke test (LED blink + D8/D10 toggle)"`

---

## Phase B — FlexPWM analogWrite

### Task B1: FlexPWM register block + pin_to_pwm table

**Files:** Modify `cores/imxrt1176/tools/gen_imxrt1176_h.py` → regen `imxrt1176.h`; create `cores/imxrt1176/pwm.h`.

- [ ] **Step 1:** Append FlexPWM defs to the generator (module bases + clock gate; the SM/module offsets are applied in `pwm.c` via pointer math, so only the bases + clock are needed here):

```python
          "/* FlexPWM (analogWrite) */",
          "#define FLEXPWM1_BASE 0x4018C000u",
          "#define FLEXPWM2_BASE 0x40190000u",
          "#define FLEXPWM3_BASE 0x40194000u",
          "#define FLEXPWM4_BASE 0x40198000u",
          "#define CCM_LPCG79_DIRECT (*(volatile uint32_t *)0x40CC69E0u)"]
```
Regenerate + dup-check as in Task A1 Step 2.

- [ ] **Step 2:** Create `cores/imxrt1176/pwm.h`:

```c
#ifndef pwm_h
#define pwm_h
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
void analogWrite(uint8_t pin, int value);
void analogWriteFrequency(uint8_t pin, float frequency);
void analogWriteResolution(int bits);
#ifdef __cplusplus
}
#endif
#endif
```
Ensure `Arduino.h` includes `pwm.h` (grep `Arduino.h` for where `analogRead` is declared and add the include there if not pulled in transitively).

- [ ] **Step 3:** Commit: `cd cores && git add imxrt1176/tools/gen_imxrt1176_h.py imxrt1176/imxrt1176.h imxrt1176/pwm.h && git commit -m "imxrt1176: FlexPWM base/clock defs + pwm.h prototypes"`

### Task B2: pwm.c FlexPWM engine

**Files:** Create `cores/imxrt1176/pwm.c`.

- [ ] **Step 1:** Write `pwm.c`. Uses 16-bit register access at computed offsets; a `pin_to_pwm[]` table; per-submodule "running" tracking; and `digitalWrite` fallback for non-PWM pins.

```c
#include "pwm.h"
#include "imxrt1176.h"
#include "core_pins.h"

extern void pinMode(uint8_t, uint8_t);
extern void digitalWrite(uint8_t, uint8_t);
#ifndef OUTPUT
#define OUTPUT 1
#endif

// --- FlexPWM register access (16-bit) ---
#define PWM_SM(base, sm, off) (*(volatile uint16_t *)((base) + (sm)*0x60u + (off)))
#define SM_CTRL   0x06u
#define SM_CTRL2  0x04u
#define SM_VAL0   0x0Au
#define SM_VAL1   0x0Eu
#define SM_VAL3   0x16u
#define SM_VAL5   0x1Eu
#define SM_INIT   0x02u
#define PWM_OUTEN(base) (*(volatile uint16_t *)((base) + 0x180u))
#define PWM_MCTRL(base) (*(volatile uint16_t *)((base) + 0x188u))
#define SMCTRL_PRSC(n)  ((uint16_t)(((n)&0xF)<<4))
#define SMCTRL_FULL     ((uint16_t)(1u<<10))
#define SMCTRL2_INDEP   ((uint16_t)(1u<<13))
#define MCTRL_LDOK(m)   ((uint16_t)(((m)&0xF)<<0))
#define MCTRL_CLDOK(m)  ((uint16_t)(((m)&0xF)<<4))
#define MCTRL_RUN(m)    ((uint16_t)(((m)&0xF)<<8))
#define OUTEN_X(m)      ((uint16_t)(((m)&0xF)<<0))
#define OUTEN_B(m)      ((uint16_t)(((m)&0xF)<<4))
#define OUTEN_A(m)      ((uint16_t)(((m)&0xF)<<8))

// FlexPWM peripheral clock (IPG/bus). Nominal; the Saleae step calibrates the
// absolute frequency and this constant is trimmed to match if needed.
#define PWM_CLOCK_HZ 24000000u   // start conservative; adjust in Task B3 from measured freq

enum { CH_A, CH_B, CH_X };
struct pwm_pin { uint8_t pin; uint32_t base; uint8_t sm; uint8_t chan; uint8_t alt;
                 volatile uint32_t *mux; volatile uint32_t *pad; };

static const struct pwm_pin pwm_pins[] = {
	{ 9, FLEXPWM1_BASE, 0, CH_B, 4,   &IOMUXC_SW_MUX_CTL_PAD_GPIO_AD_01, &IOMUXC_SW_PAD_CTL_PAD_GPIO_AD_01 },
	{ 3, FLEXPWM1_BASE, 2, CH_A, 4,   &IOMUXC_SW_MUX_CTL_PAD_GPIO_AD_04, &IOMUXC_SW_PAD_CTL_PAD_GPIO_AD_04 },
	{ 5, FLEXPWM1_BASE, 2, CH_B, 4,   &IOMUXC_SW_MUX_CTL_PAD_GPIO_AD_05, &IOMUXC_SW_PAD_CTL_PAD_GPIO_AD_05 },
	{ 4, FLEXPWM1_BASE, 0, CH_X, 0xB, &IOMUXC_SW_MUX_CTL_PAD_GPIO_AD_06, &IOMUXC_SW_PAD_CTL_PAD_GPIO_AD_06 },
	{ 8, FLEXPWM1_BASE, 1, CH_X, 0xB, &IOMUXC_SW_MUX_CTL_PAD_GPIO_AD_07, &IOMUXC_SW_PAD_CTL_PAD_GPIO_AD_07 },
	{ 7, FLEXPWM3_BASE, 0, CH_X, 0xB, &IOMUXC_SW_MUX_CTL_PAD_GPIO_AD_14, &IOMUXC_SW_PAD_CTL_PAD_GPIO_AD_14 },
};
#define NPWM (sizeof(pwm_pins)/sizeof(pwm_pins[0]))

static uint32_t pwm_res_max = 255;      // (1<<bits)-1
static uint32_t pwm_freq   = 1000;      // default Hz
// per (base,sm) modulo cache; small fixed set. Key by base+sm index.
static uint16_t sm_modulo[4*4];         // 4 modules x 4 submodules, 0 = not configured
static uint8_t  sm_prsc[4*4];

static int pwm_slot(uint32_t base) {    // module index 0..3 for cache
	if (base==FLEXPWM1_BASE) return 0; if (base==FLEXPWM2_BASE) return 1;
	if (base==FLEXPWM3_BASE) return 2; return 3;
}

static const struct pwm_pin *find_pwm(uint8_t pin) {
	for (unsigned i=0;i<NPWM;i++) if (pwm_pins[i].pin==pin) return &pwm_pins[i];
	return 0;
}

// Program a submodule for pwm_freq; returns modulo (VAL1+1). Enables clocks.
static uint16_t configure_sm(uint32_t base, uint8_t sm) {
	CCM_LPCG79_DIRECT = 1u;                          // ungate FlexPWM1 clock (all modules share? gate per module)
	uint8_t prsc = 0; uint32_t modulo = 0;
	for (prsc=0; prsc<8; prsc++) {                   // pick prescale so modulo fits 16-bit
		modulo = PWM_CLOCK_HZ / ((1u<<prsc) * pwm_freq);
		if (modulo <= 65535u) break;
	}
	if (modulo < 2u) modulo = 2u; if (modulo > 65535u) modulo = 65535u;
	uint8_t mask = (uint8_t)(1u<<sm);
	PWM_MCTRL(base) |= MCTRL_CLDOK(mask);
	PWM_SM(base,sm,SM_CTRL2) = SMCTRL2_INDEP;        // independent, IPG clk (CLK_SEL=0)
	PWM_SM(base,sm,SM_CTRL)  = SMCTRL_FULL | SMCTRL_PRSC(prsc);
	PWM_SM(base,sm,SM_INIT)  = 0;
	PWM_SM(base,sm,SM_VAL1)  = (uint16_t)(modulo - 1u);
	PWM_MCTRL(base) |= MCTRL_LDOK(mask);
	PWM_MCTRL(base) |= MCTRL_RUN(mask);
	int slot = pwm_slot(base)*4 + sm;
	sm_modulo[slot] = (uint16_t)modulo; sm_prsc[slot] = prsc;
	return (uint16_t)modulo;
}

void analogWriteResolution(int bits) {
	if (bits < 1) bits = 1; if (bits > 16) bits = 16;
	pwm_res_max = (1u<<bits) - 1u;
}

void analogWrite(uint8_t pin, int value) {
	const struct pwm_pin *p = find_pwm(pin);
	if (value < 0) value = 0; if ((uint32_t)value > pwm_res_max) value = pwm_res_max;
	if (!p) {                                        // non-PWM pin: digital fallback
		pinMode(pin, OUTPUT);
		digitalWrite(pin, ((uint32_t)value >= (pwm_res_max+1)/2) ? 1 : 0);
		return;
	}
	int slot = pwm_slot(p->base)*4 + p->sm;
	uint16_t modulo = sm_modulo[slot] ? sm_modulo[slot] : configure_sm(p->base, p->sm);
	uint32_t duty = (uint32_t)value * modulo / pwm_res_max;   // 0..modulo counts
	*p->mux = p->alt;                                // route pad to FlexPWM
	*p->pad = 0x0008u;                               // DSE drive, push-pull
	uint8_t mask = (uint8_t)(1u<<p->sm);
	PWM_MCTRL(p->base) |= MCTRL_CLDOK(mask);
	if (p->chan==CH_A) { PWM_SM(p->base,p->sm,SM_VAL3)=(uint16_t)duty; PWM_OUTEN(p->base)|=OUTEN_A(mask); }
	else if (p->chan==CH_B) { PWM_SM(p->base,p->sm,SM_VAL5)=(uint16_t)duty; PWM_OUTEN(p->base)|=OUTEN_B(mask); }
	else { PWM_SM(p->base,p->sm,SM_VAL0)=(uint16_t)(modulo-duty); PWM_OUTEN(p->base)|=OUTEN_X(mask); }
	PWM_MCTRL(p->base) |= MCTRL_LDOK(mask);
}

void analogWriteFrequency(uint8_t pin, float frequency) {
	const struct pwm_pin *p = find_pwm(pin);
	if (frequency < 1.0f) frequency = 1.0f;
	pwm_freq = (uint32_t)frequency;
	if (p) { int slot=pwm_slot(p->base)*4+p->sm; sm_modulo[slot]=0; configure_sm(p->base,p->sm); }
}
```

Notes for the implementer: FlexPWM modules 1–4 have separate LPCG gates (79=PWM1, 80=PWM2, 81=PWM3, 82=PWM4). This code ungates only PWM1 (`CCM_LPCG79_DIRECT`). For the D7 pin on PWM3, also ungate LPCG81 (`0x40CC69E0 + 2*0x20 = 0x40CC6A20`); add `CCM_LPCG81_DIRECT` to the generator and ungate it in `configure_sm` when `base==FLEXPWM3_BASE`. (D3/D4/D5/D8/D9 are all PWM1, so PWM1-only works for the primary verification pin D9.)

- [ ] **Step 2:** Add `pwm.c` to the core source glob if the CMake uses an explicit list (grep the macros CMake for the source list; the `import_arduino_library` glob picks up new `.c` automatically — reconfigure the test build dir). Build the PWM test build dir (Task B3) to confirm it compiles.
- [ ] **Step 3:** Commit: `cd cores && git add imxrt1176/pwm.c && git commit -m "imxrt1176: FlexPWM analogWrite engine (analogWrite/Frequency/Resolution)"`

### Task B3: Hardware verification via Saleae

**Files:** Create `evkb/pwm_test/pwm_test.cpp` (+ CMake/toolchain), `evkb/pwm_test/measure_pwm.py`.

- [ ] **Step 1:** Sketch — sweep duty on **D9** (FLEXPWM1_PWM0_B) so the Saleae can measure each level; hold each 1.5 s; print over Serial1.

```cpp
#include "Arduino.h"
#include "HardwareSerial.h"
void setup(){ Serial1.begin(115200); while(!Serial1){} analogWriteFrequency(9,1000); Serial1.println("pwm D9 sweep"); }
void loop(){
	static const int duties[]={0,64,128,192,255}; static int i=0;
	analogWrite(9, duties[i]); Serial1.print("duty="); Serial1.println(duties[i]);
	i=(i+1)%5; delay(1500);
}
```

- [ ] **Step 2:** Build + flash (LinkServer `run`). Wire a **Saleae channel to header D9**, GND to board GND.
- [ ] **Step 3:** `measure_pwm.py` uses `logic2-automation` (server on 127.0.0.1:10430) to capture D9 for ~2 s, then computes duty% and frequency from edge timestamps, printing measured vs expected. (Model the capture/export on the Saleae automation API; export digital CSV and compute high-time/period.) Run it while each duty is held; expect duty ≈ {0, 25, 50, 75, 100}% and freq ≈ 1 kHz (±10%).
- [ ] **Step 4:** If the measured **frequency** is off by a constant factor, set `PWM_CLOCK_HZ` in `pwm.c` to `measured_freq_ratio * 24_000_000` (calibrate the bus-clock constant), rebuild, re-measure. Duty% must be correct regardless (it's a ratio).
- [ ] **Step 5:** Servo check: temporarily `analogWriteFrequency(9,50)`; measure ≈ 50 Hz. Non-PWM fallback: `analogWrite(16, 200)` (A0) → Saleae/DMM reads steady HIGH.
- [ ] **Step 6:** Commit test + any `PWM_CLOCK_HZ` calibration: `cd evkb && git add pwm_test && git commit -m "pwm: Saleae HW verification (duty + frequency)"` and (if calibrated) commit `pwm.c` in cores.

### Task B4: Finish

- [ ] **Step 1:** Regression — re-run Wire/SPI QEMU gates (pin-table/PWM changes must not break them): `evkb/wire_master_test/run_qemu_wire.sh`, `evkb/wire_slave_test/run_qemu_wire_slave.sh`, `evkb/spi_loopback_test/run_qemu_spi.sh` → all PASS.
- [ ] **Step 2:** Push cores: `cd cores && git push`.
- [ ] **Step 3:** Memory: add `rt1176-pintable-pwm.md` (header numbering pin=DN, LED_BUILTIN=3, AD_nn↔gpio9.(nn-1), GPIO ALT 0xA; FlexPWM1 base/offsets, analogWrite maps D3/D5/D9=PWM1 A/B, Saleae-verified; PWM_CLOCK_HZ value found). One-line pointer in MEMORY.md.
- [ ] **Step 4:** superpowers:finishing-a-development-branch.

---

## Self-review (author checklist — done)
- **Spec coverage:** pin table D0–D15/A0–A5 (A1/A2), header numbering + LED_BUILTIN=3 (A2), analogWrite/Frequency/Resolution (B2), non-PWM fallback (B2 analogWrite), Saleae duty+freq+servo verification (B3), regression (B4). Covered.
- **Type consistency:** `pin_to_pwm`→`pwm_pins`, `configure_sm`, `pwm_slot`, `find_pwm`, `sm_modulo`/`sm_prsc`, `pwm_res_max`/`pwm_freq` used consistently across B2. `digital_pin_info_struct` fields match existing `digital.c`.
- **Known deferrals with method:** PWM3 (D7) needs LPCG81 ungate (noted inline in B2); `PWM_CLOCK_HZ` calibrated from the Saleae measurement (B3 Step 4) — legitimate empirical step, not a placeholder.
