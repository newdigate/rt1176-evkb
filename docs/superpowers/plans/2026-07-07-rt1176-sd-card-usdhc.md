# RT1176 SD card (USDHC/SDIO) Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Bring up Arduino `SD` + `SdFat` over the USDHC1 SDIO controller on the RT1176 core so a real µSD in the EVKB J15 slot can be mounted, and files written/read back — both PIO and SDMA block paths, plus the FAT filesystem layer.

**Architecture:** Port `SdFat/src/SdCard/SdioTeensy.cpp` (the Teensy-4 USDHC backend) by adding an in-file `__IMXRT1176__` branch that retargets only base address, clock (CCM→CLOCK_ROOT58/LPCG117), and pin-mux (GPIO_SD_B1_00..05). The core gains USDHC register defs, `IRQ_USDHC1`, and `FS.h`. Verification is TDD via QEMU "gates": core-infra + driver are prerequisites, and two gates (`sd_block_test` raw SDIO, `sd_fs_test` filesystem) are where red→green happens — a gate is the executable test. Hardware is the final arbiter (Task 7).

**Tech Stack:** C/C++ (Arduino/Teensyduino core idiom), Python (register-header generator), CMake + `arm-none-eabi-g++` (teensy-cmake-macros), QEMU `mimxrt1170-evk` machine (`qrun`), `mtools`/`mkfile` (card images), LinkServer + VCOM (hardware).

**Spec:** `docs/superpowers/specs/2026-07-07-rt1176-sd-card-usdhc-design.md`

**Repos + git roots (commit to `master` in each; push only when the user asks):**
- **core** (`teensy-cores`): source at `~/Development/rt1170/evkb/cores/imxrt1176`, **git root `~/Development/rt1170/evkb/cores`** → `git -C ~/Development/rt1170/evkb/cores …`
- **SdFat** (`newdigate/SdFat`): `~/Development/SdFat` → `git -C ~/Development/SdFat …`
- **SD** (`newdigate/SD`, aka PaulS_SD): `~/Development/PaulS_SD` → `git -C ~/Development/PaulS_SD …`

**TDD note for embedded bring-up:** Tasks 1–3 (core defs, `FS.h`, driver branch) are prerequisites verified transitively by the Task 4 gate *build* — a wrong register/type fails that compile. Task 4 is the first red→green cycle (gate fails until the driver is ported, then passes). Task 6 is the Phase-B red→green cycle. This is the faithful hardware-gate adaptation of TDD used by every prior peripheral on this port.

---

## Task 1: Core — USDHC register defs (generator + regenerate)

`imxrt1176.h` is auto-generated — edit the generator and regenerate; never hand-edit the header.

**Files:**
- Modify: `~/Development/rt1170/evkb/cores/imxrt1176/tools/gen_imxrt1176_h.py`
- Regenerate: `~/Development/rt1170/evkb/cores/imxrt1176/imxrt1176.h`

- [ ] **Step 1: Add the USDHC block to the generator**

In `gen_imxrt1176_h.py`, immediately **after** the `IMXRT_LPI2C_t` heredoc block (the one ending with `#define IMXRT_LPI2C5_ADDRESS 0x40C34000` followed by `'''.rstrip("\n")]`, around line 382), insert this new block:

```python
    # --- USDHC (SDIO / SD-card) — for the SdFat SdioTeensy __IMXRT1176__ port --
    # microSD slot on the MIMXRT1170-EVKB (socket J15) = USDHC1 @ 0x40418000.
    # Pins GPIO_SD_B1_00..05 (ALT0); clock root 58 (SYS_PLL2_PFD2/2 ~198 MHz);
    # gate LPCG117; IRQ 133.  Addresses verified against the generator's own
    # IOMUXC scheme (AD_35 mux ends 0x198 / pad 0x3DC, so SD_B1_00 follows).
    L += ["",
          "/* USDHC1 microSD pins (EVKB J15): CMD/CLK/DATA0-3 = GPIO_SD_B1_00..05, ALT0 */",
          "#define IOMUXC_SW_MUX_CTL_PAD_GPIO_SD_B1_00 (*(volatile uint32_t *)0x400E819Cu)",
          "#define IOMUXC_SW_MUX_CTL_PAD_GPIO_SD_B1_01 (*(volatile uint32_t *)0x400E81A0u)",
          "#define IOMUXC_SW_MUX_CTL_PAD_GPIO_SD_B1_02 (*(volatile uint32_t *)0x400E81A4u)",
          "#define IOMUXC_SW_MUX_CTL_PAD_GPIO_SD_B1_03 (*(volatile uint32_t *)0x400E81A8u)",
          "#define IOMUXC_SW_MUX_CTL_PAD_GPIO_SD_B1_04 (*(volatile uint32_t *)0x400E81ACu)",
          "#define IOMUXC_SW_MUX_CTL_PAD_GPIO_SD_B1_05 (*(volatile uint32_t *)0x400E81B0u)",
          "#define IOMUXC_SW_PAD_CTL_PAD_GPIO_SD_B1_00 (*(volatile uint32_t *)0x400E83E0u)",
          "#define IOMUXC_SW_PAD_CTL_PAD_GPIO_SD_B1_01 (*(volatile uint32_t *)0x400E83E4u)",
          "#define IOMUXC_SW_PAD_CTL_PAD_GPIO_SD_B1_02 (*(volatile uint32_t *)0x400E83E8u)",
          "#define IOMUXC_SW_PAD_CTL_PAD_GPIO_SD_B1_03 (*(volatile uint32_t *)0x400E83ECu)",
          "#define IOMUXC_SW_PAD_CTL_PAD_GPIO_SD_B1_04 (*(volatile uint32_t *)0x400E83F0u)",
          "#define IOMUXC_SW_PAD_CTL_PAD_GPIO_SD_B1_05 (*(volatile uint32_t *)0x400E83F4u)",
          "/* USDHC1 clock root 58 (SYS_PLL2_PFD2 / 2) + gate LPCG117 */",
          "#define CCM_CLOCK_ROOT58_CONTROL (*(volatile uint32_t *)0x40CC1D00u)",
          "#define CCM_LPCG117_DIRECT       (*(volatile uint32_t *)0x40CC6EA0u)"]
    L += ['''
/* USDHC register-block overlay (for the SdFat SdioTeensy __IMXRT1176__ port).
 * Layout matches cores/teensy4/imxrt.h IMXRT_USDHC_t; only the base addresses
 * differ from the 1062 (0x40418000/0x4041C000 vs 0x402C0000/0x402C4000). */
typedef struct {
	volatile uint32_t DS_ADDR;              // 0x00
	volatile uint32_t BLK_ATT;              // 0x04
	volatile uint32_t CMD_ARG;              // 0x08
	volatile uint32_t CMD_XFR_TYP;          // 0x0C
	volatile uint32_t CMD_RSP0;             // 0x10
	volatile uint32_t CMD_RSP1;             // 0x14
	volatile uint32_t CMD_RSP2;             // 0x18
	volatile uint32_t CMD_RSP3;             // 0x1C
	volatile uint32_t DATA_BUFF_ACC_PORT;   // 0x20
	volatile uint32_t PRES_STATE;           // 0x24
	volatile uint32_t PROT_CTRL;            // 0x28
	volatile uint32_t SYS_CTRL;             // 0x2C
	volatile uint32_t INT_STATUS;           // 0x30
	volatile uint32_t INT_STATUS_EN;        // 0x34
	volatile uint32_t INT_SIGNAL_EN;        // 0x38
	volatile uint32_t AUTOCMD12_ERR_STATUS; // 0x3C
	volatile uint32_t HOST_CTRL_CAP;        // 0x40
	volatile uint32_t WTMK_LVL;             // 0x44
	volatile uint32_t MIX_CTRL;             // 0x48
	uint32_t unused1;                       // 0x4C
	volatile uint32_t FORCE_EVENT;          // 0x50
	volatile uint32_t ADMA_ERR_STATUS;      // 0x54
	volatile uint32_t ADMA_SYS_ADDR;        // 0x58
	uint32_t unused2;                       // 0x5C
	volatile uint32_t DLL_CTRL;             // 0x60
	volatile uint32_t DLL_STATUS;           // 0x64
	volatile uint32_t CLK_TUNE_CTRL_STATUS; // 0x68
	uint32_t unused3[21];                   // 0x6C..0xBC
	volatile uint32_t VEND_SPEC;            // 0xC0
	volatile uint32_t MMC_BOOT;             // 0xC4
	volatile uint32_t VEND_SPEC2;           // 0xC8
	volatile uint32_t TUNING_CTRL;          // 0xCC
} IMXRT_USDHC_t;
#define IMXRT_USDHC1_ADDRESS 0x40418000
#define IMXRT_USDHC2_ADDRESS 0x4041C000
#define IMXRT_USDHC1 (*(IMXRT_USDHC_t *)IMXRT_USDHC1_ADDRESS)
#define IMXRT_USDHC2 (*(IMXRT_USDHC_t *)IMXRT_USDHC2_ADDRESS)
'''.rstrip("\n")]
    for _n in (1, 2):
        for _reg in ("DS_ADDR","BLK_ATT","CMD_ARG","CMD_XFR_TYP","CMD_RSP0","CMD_RSP1",
                     "CMD_RSP2","CMD_RSP3","DATA_BUFF_ACC_PORT","PRES_STATE","PROT_CTRL",
                     "SYS_CTRL","INT_STATUS","INT_STATUS_EN","INT_SIGNAL_EN",
                     "AUTOCMD12_ERR_STATUS","HOST_CTRL_CAP","WTMK_LVL","MIX_CTRL",
                     "FORCE_EVENT","ADMA_ERR_STATUS","ADMA_SYS_ADDR","DLL_CTRL",
                     "DLL_STATUS","CLK_TUNE_CTRL_STATUS","VEND_SPEC","MMC_BOOT",
                     "VEND_SPEC2","TUNING_CTRL"):
            L.append(f"#define USDHC{_n}_{_reg} (IMXRT_USDHC{_n}.{_reg})")
```

- [ ] **Step 2: Regenerate the header**

Run: `cd ~/Development/rt1170/evkb/cores/imxrt1176 && python3 tools/gen_imxrt1176_h.py`
Expected: exits 0, no `warn:` about missing bases beyond any that already print on a clean run.

- [ ] **Step 3: Verify the diff is only USDHC additions and the symbols are present**

Run:
```bash
cd ~/Development/rt1170/evkb/cores
git diff --stat imxrt1176/imxrt1176.h
for s in USDHC1_BLK_ATT USDHC1_CMD_XFR_TYP USDHC1_MIX_CTRL USDHC1_DS_ADDR \
         USDHC1_PRES_STATE IMXRT_USDHC1_ADDRESS USDHC2_VEND_SPEC2 \
         CCM_CLOCK_ROOT58_CONTROL CCM_LPCG117_DIRECT \
         IOMUXC_SW_MUX_CTL_PAD_GPIO_SD_B1_00 IOMUXC_SW_PAD_CTL_PAD_GPIO_SD_B1_05; do
  grep -q "$s" imxrt1176/imxrt1176.h || echo "MISSING: $s"
done; echo "check done"
```
Expected: the diff touches only `imxrt1176.h` (added USDHC lines); the loop prints only `check done` (no `MISSING:` lines).

- [ ] **Step 4: Commit**

```bash
git -C ~/Development/rt1170/evkb/cores add imxrt1176/tools/gen_imxrt1176_h.py imxrt1176/imxrt1176.h
git -C ~/Development/rt1170/evkb/cores commit -m "$(cat <<'EOF'
imxrt1176: add USDHC1/2 register defs, CLOCK_ROOT58/LPCG117, GPIO_SD_B1 pins

For the SdFat SdioTeensy __IMXRT1176__ SD-card port. IMXRT_USDHC_t overlay
mirrors cores/teensy4/imxrt.h with the 1176 base 0x40418000; adds the six
GPIO_SD_B1_00..05 mux/pad regs, USDHC1 clock root 58 + gate LPCG117.

Co-Authored-By: Claude Opus 4.8 <noreply@anthropic.com>
EOF
)"
```

---

## Task 2: Core — `IRQ_USDHC1`, `DateTimeFields`, `FS.h`

**Files:**
- Modify: `~/Development/rt1170/evkb/cores/imxrt1176/core_pins.h:62-63` (IRQ enum)
- Create: `~/Development/rt1170/evkb/cores/imxrt1176/FS.h`

- [ ] **Step 1: Add the USDHC IRQ numbers to the enum**

In `core_pins.h`, find (lines 62-63):
```c
    IRQ_PIT1 = 155, IRQ_PIT2 = 156,
    IRQ_USB_OTG1 = 136
} IRQ_NUMBER_t;
```
Replace with:
```c
    IRQ_PIT1 = 155, IRQ_PIT2 = 156,
    IRQ_USDHC1 = 133, IRQ_USDHC2 = 134,
    IRQ_USB_OTG1 = 136
} IRQ_NUMBER_t;
```

- [ ] **Step 2: Copy `FS.h` from teensy4 into the core**

Run: `cp ~/Development/rt1170/evkb/cores/teensy4/FS.h ~/Development/rt1170/evkb/cores/imxrt1176/FS.h`

- [ ] **Step 3: Add the `DateTimeFields` typedef `FS.h` depends on**

The 1176 core does not define `DateTimeFields` (teensy4 has it in `core_pins.h`, but `FS.h` references it in `getCreateTime`/`setModifyTime` signatures). Add it to the copied `FS.h`. Change:
```c
#include <Arduino.h>

#define FILE_READ  0
```
to:
```c
#include <Arduino.h>

#ifndef __DateTimeFields_defined
#define __DateTimeFields_defined
// From cores/teensy4/core_pins.h — the 1176 core doesn't define it yet.
typedef struct  {
	uint8_t sec;   // 0-59
	uint8_t min;   // 0-59
	uint8_t hour;  // 0-23
	uint8_t wday;  // 0-6, 0=sunday
	uint8_t mday;  // 1-31
	uint8_t mon;   // 0-11
	uint8_t year;  // 70-206, 70=1970, 206=2106
} DateTimeFields;
#endif

#define FILE_READ  0
```

- [ ] **Step 4: Verify FS.h parses standalone against the core**

Run:
```bash
cd ~/Development/rt1170/evkb/cores/imxrt1176
grep -q "DateTimeFields" FS.h && grep -q "class File final : public Stream" FS.h && echo "FS.h OK"
grep -q "IRQ_USDHC1 = 133" core_pins.h && echo "IRQ OK"
```
Expected: prints `FS.h OK` and `IRQ OK`. (Full compilation of `FS.h` + the IRQ enum is exercised by the Task 4 gate build, which includes both via `SD.h`/the driver.)

- [ ] **Step 5: Commit**

```bash
git -C ~/Development/rt1170/evkb/cores add imxrt1176/core_pins.h imxrt1176/FS.h
git -C ~/Development/rt1170/evkb/cores commit -m "$(cat <<'EOF'
imxrt1176: add IRQ_USDHC1/2 (133/134), FS.h (File/FileImpl/FS), DateTimeFields

FS.h ported verbatim from cores/teensy4 (shared filesystem infra the SD/SdFat
stack builds on); DateTimeFields typedef added since the 1176 core lacks it.

Co-Authored-By: Claude Opus 4.8 <noreply@anthropic.com>
EOF
)"
```

---

## Task 3: SdFat — `SdioTeensy` `__IMXRT1176__` branch + `SdFatConfig`

Port the driver by adding parallel `__IMXRT1176__` branches. The register-access layer (`SDHC_*`→`USDHC1_*`) and all command/transfer logic are reused as-is; only base/clock/pins differ.

**Files:**
- Modify: `~/Development/SdFat/src/SdCard/SdioTeensy.h` (guard, `IRQ_SDHC` alias)
- Modify: `~/Development/SdFat/src/SdCard/SdioTeensy.cpp` (guard + i.MX-shared sites + new gpio/clock block)
- Modify: `~/Development/SdFat/src/SdFatConfig.h` (`HAS_SDIO_CLASS`)

- [ ] **Step 1: `SdioTeensy.h` — widen the platform guard (line 6)**

Change:
```c
#if defined(__IMXRT1062__)
#define MAKE_REG_MASK(m,s) (((uint32_t)(((uint32_t)(m) << s))))
```
to:
```c
#if defined(__IMXRT1062__) || defined(__IMXRT1176__)
#define MAKE_REG_MASK(m,s) (((uint32_t)(((uint32_t)(m) << s))))
```
And the matching close at line 276 — change `#endif  // defined(__IMXRT1062__)` to `#endif  // defined(__IMXRT1062__) || defined(__IMXRT1176__)`.

- [ ] **Step 2: `SdioTeensy.h` — make `IRQ_SDHC` resolve to `IRQ_USDHC1` on 1176 (line 257)**

Change:
```c
#define IRQ_SDHC    IRQ_SDHC1
```
to:
```c
#if defined(__IMXRT1176__)
#define IRQ_SDHC    IRQ_USDHC1
#else
#define IRQ_SDHC    IRQ_SDHC1
#endif
```

- [ ] **Step 3: `SdioTeensy.cpp` — widen the top-of-file guard (line 25)**

Change:
```c
#if defined(__MK64FX512__) || defined(__MK66FX1M0__) || defined(__IMXRT1062__)
```
to:
```c
#if defined(__MK64FX512__) || defined(__MK66FX1M0__) || defined(__IMXRT1062__) || defined(__IMXRT1176__)
```
And the matching close at line 1133 — change to:
```c
#endif  // defined(__MK64FX512__)  defined(__MK66FX1M0__) defined(__IMXRT1062__) defined(__IMXRT1176__)
```

- [ ] **Step 4: `SdioTeensy.cpp` — share the i.MX MIX_CTRL data-constant block (line 100)**

Change:
```c
#elif defined(__IMXRT1062__)
// Use low bits for SDHC_MIX_CTRL since bits 15-0 of SDHC_XFERTYP are reserved.
const uint32_t SDHC_MIX_CTRL_MASK = SDHC_MIX_CTRL_DMAEN | SDHC_MIX_CTRL_BCEN |
```
to:
```c
#elif defined(__IMXRT1062__) || defined(__IMXRT1176__)
// Use low bits for SDHC_MIX_CTRL since bits 15-0 of SDHC_XFERTYP are reserved.
const uint32_t SDHC_MIX_CTRL_MASK = SDHC_MIX_CTRL_DMAEN | SDHC_MIX_CTRL_BCEN |
```

- [ ] **Step 5: `SdioTeensy.cpp` — share the ISR MIX_CTRL clear (line 286)**

Change:
```c
#if defined(__IMXRT1062__)
  SDHC_MIX_CTRL &= ~(SDHC_MIX_CTRL_AC23EN | SDHC_MIX_CTRL_DMAEN);
#endif
  m_dmaBusy = false;
```
to:
```c
#if defined(__IMXRT1062__) || defined(__IMXRT1176__)
  SDHC_MIX_CTRL &= ~(SDHC_MIX_CTRL_AC23EN | SDHC_MIX_CTRL_DMAEN);
#endif
  m_dmaBusy = false;
```

- [ ] **Step 6: `SdioTeensy.cpp` — add the `__IMXRT1176__` gpio/clock branch (before line 373)**

Find the end of the 1062 gpio/clock block:
```c
static uint32_t baseClock() {
  uint32_t divider = ((CCM_CSCDR1 >> 11) & 0x7) + 1;
  return (528000000U * 3)/((CCM_ANALOG_PFD_528 & 0x3F)/6)/divider;
}
#endif  // defined(__MK64FX512__) || defined(__MK66FX1M0__)
```
Replace with (insert the `#elif` branch before the `#endif`):
```c
static uint32_t baseClock() {
  uint32_t divider = ((CCM_CSCDR1 >> 11) & 0x7) + 1;
  return (528000000U * 3)/((CCM_ANALOG_PFD_528 & 0x3F)/6)/divider;
}

#elif defined(__IMXRT1176__)
//------------------------------------------------------------------------------
static void gpioMux(uint8_t mode) {
  IOMUXC_SW_MUX_CTL_PAD_GPIO_SD_B1_00 = mode;  // USDHC1_CMD
  IOMUXC_SW_MUX_CTL_PAD_GPIO_SD_B1_01 = mode;  // USDHC1_CLK
  IOMUXC_SW_MUX_CTL_PAD_GPIO_SD_B1_02 = mode;  // USDHC1_DATA0
  IOMUXC_SW_MUX_CTL_PAD_GPIO_SD_B1_03 = mode;  // USDHC1_DATA1
  IOMUXC_SW_MUX_CTL_PAD_GPIO_SD_B1_04 = mode;  // USDHC1_DATA2
  IOMUXC_SW_MUX_CTL_PAD_GPIO_SD_B1_05 = mode;  // USDHC1_DATA3
}
//------------------------------------------------------------------------------
static void enableGPIO(bool enable) {
  // RT1176 SW_PAD_CTL fields: bit4 ODE, bits[3:2] PULL (01=up,10=down,11=none),
  // bit1 PDRV (0=high drive).  CMD/DATA = pull-up + high drive (0x04);
  // CLK = pull-down + high drive (0x08).  (The 1062 PKE/DSE/SPEED/PUE encoding
  // does not exist on the 1176.)
  const uint32_t DATA_PAD = 0x04;
  const uint32_t CLK_PAD  = 0x08;
  if (enable) {
    gpioMux(0);  // ALT0 = USDHC1
    IOMUXC_SW_PAD_CTL_PAD_GPIO_SD_B1_00 = DATA_PAD;  // CMD
    IOMUXC_SW_PAD_CTL_PAD_GPIO_SD_B1_01 = CLK_PAD;   // CLK
    IOMUXC_SW_PAD_CTL_PAD_GPIO_SD_B1_02 = DATA_PAD;  // DATA0
    IOMUXC_SW_PAD_CTL_PAD_GPIO_SD_B1_03 = DATA_PAD;  // DATA1
    IOMUXC_SW_PAD_CTL_PAD_GPIO_SD_B1_04 = DATA_PAD;  // DATA2
    IOMUXC_SW_PAD_CTL_PAD_GPIO_SD_B1_05 = DATA_PAD;  // DATA3
  } else {
    gpioMux(5);  // ALT5 = GPIO (idle during the clock switch)
  }
}
//------------------------------------------------------------------------------
static void initClock() {
  // USDHC1 clock root (CLOCK_ROOT58) <- SYS_PLL2_PFD2 (mux 4) / 2 (DIV field 1)
  // ~= 198 MHz.  Then ungate USDHC1 (LPCG117).
  CCM_CLOCK_ROOT58_CONTROL = CCM_CLOCK_ROOT_CONTROL_MUX(4) | CCM_CLOCK_ROOT_CONTROL_DIV(1);
  CCM_LPCG117_DIRECT = 1;
}
//------------------------------------------------------------------------------
static uint32_t baseClock() { return 198000000U; }
#endif  // defined(__MK64FX512__) || defined(__MK66FX1M0__)
```

- [ ] **Step 7: `SdioTeensy.cpp` — share the cardCommand MIX_CTRL handling (line 386)**

Change:
```c
  SDHC_CMDARG = arg;
#if defined(__IMXRT1062__)
  // Set MIX_CTRL if data transfer.
```
to:
```c
  SDHC_CMDARG = arg;
#if defined(__IMXRT1062__) || defined(__IMXRT1176__)
  // Set MIX_CTRL if data transfer.
```
And the matching close a few lines down — change `#endif  // defined(__IMXRT1062__)` to `#endif  // defined(__IMXRT1062__) || defined(__IMXRT1176__)`.

- [ ] **Step 8: `SdioTeensy.cpp` — share the initSDHC MIX_CTRL init (line 434)**

Change:
```c
#if defined (__IMXRT1062__)
  SDHC_MIX_CTRL |= 0x80000000;
#endif  //  (__IMXRT1062__)
```
to:
```c
#if defined (__IMXRT1062__) || defined(__IMXRT1176__)
  SDHC_MIX_CTRL |= 0x80000000;
#endif  //  (__IMXRT1062__) || (__IMXRT1176__)
```

- [ ] **Step 9: `SdioTeensy.cpp` — share the version-1 card workaround (line 680)**

Change:
```c
#if defined(__IMXRT1062__)
  // Old version 1 cards have trouble with Teensy 4.1 after CMD8.
  // For reasons unknown, SDIO stops working after the cards does
  // not reply.  Simply restarting and CMD0 is a crude workaround.
  if (!m_version2) {
    initSDHC();
    cardCommand(CMD0_XFERTYP, 0);
  }
#endif
```
to:
```c
#if defined(__IMXRT1062__) || defined(__IMXRT1176__)
  // Old version 1 cards have trouble with Teensy 4.1 after CMD8.
  // For reasons unknown, SDIO stops working after the cards does
  // not reply.  Simply restarting and CMD0 is a crude workaround.
  if (!m_version2) {
    initSDHC();
    cardCommand(CMD0_XFERTYP, 0);
  }
#endif
```

- [ ] **Step 10: `SdioTeensy.cpp` — share infinite-transfer in readStart (line 962) and writeStart (line 1117)**

In `readStart` change:
```c
  SDHC_PROCTL |= SDHC_PROCTL_SABGREQ;
#if defined(__IMXRT1062__)
  // Infinite transfer.
  SDHC_BLKATTR = SDHC_BLKATTR_BLKSIZE(512);
#else  // defined(__IMXRT1062__)
```
to:
```c
  SDHC_PROCTL |= SDHC_PROCTL_SABGREQ;
#if defined(__IMXRT1062__) || defined(__IMXRT1176__)
  // Infinite transfer.
  SDHC_BLKATTR = SDHC_BLKATTR_BLKSIZE(512);
#else  // defined(__IMXRT1062__)
```
In `writeStart` change:
```c
  SDHC_PROCTL &= ~SDHC_PROCTL_SABGREQ;

#if defined(__IMXRT1062__)
  // Infinite transfer.
  SDHC_BLKATTR = SDHC_BLKATTR_BLKSIZE(512);
#else  // defined(__IMXRT1062__)
```
to:
```c
  SDHC_PROCTL &= ~SDHC_PROCTL_SABGREQ;

#if defined(__IMXRT1062__) || defined(__IMXRT1176__)
  // Infinite transfer.
  SDHC_BLKATTR = SDHC_BLKATTR_BLKSIZE(512);
#else  // defined(__IMXRT1062__)
```

- [ ] **Step 11: `SdFatConfig.h` — enable the SDIO class for 1176 (line 418)**

Change:
```c
#if defined(__IMXRT1062__)
#define HAS_SDIO_CLASS 1
#ifndef BUILTIN_SDCARD
#define BUILTIN_SDCARD 254
#endif  // BUILTIN_SDCARD
#endif  // defined(__IMXRT1062__)
```
to:
```c
#if defined(__IMXRT1062__) || defined(__IMXRT1176__)
#define HAS_SDIO_CLASS 1
#ifndef BUILTIN_SDCARD
#define BUILTIN_SDCARD 254
#endif  // BUILTIN_SDCARD
#endif  // defined(__IMXRT1062__) || defined(__IMXRT1176__)
```

- [ ] **Step 12: Sanity-check the branches exist (full compile is Task 4)**

Run:
```bash
cd ~/Development/SdFat/src/SdCard
grep -q "#elif defined(__IMXRT1176__)" SdioTeensy.cpp && echo "branch OK"
grep -q "return 198000000U" SdioTeensy.cpp && echo "clock OK"
grep -q "GPIO_SD_B1_01 = CLK_PAD" SdioTeensy.cpp && echo "pins OK"
grep -q "IOMUXC_SW_MUX_CTL_PAD_GPIO_SD_B1_00 = mode" SdioTeensy.cpp && echo "mux OK"
grep -q "IRQ_USDHC1" SdioTeensy.h && echo "irq OK"
grep -q "defined(__IMXRT1176__)" SdFatConfig.h && echo "config OK"
```
Expected: `branch OK`, `clock OK`, `pins OK`, `mux OK`, `irq OK`, `config OK` (all six).

- [ ] **Step 13: Commit (SdFat repo)**

```bash
git -C ~/Development/SdFat add src/SdCard/SdioTeensy.h src/SdCard/SdioTeensy.cpp src/SdFatConfig.h
git -C ~/Development/SdFat commit -m "$(cat <<'EOF'
SdioTeensy: add __IMXRT1176__ branch (USDHC1 on the MIMXRT1170-EVKB)

In-file port mirroring the 1062: base 0x40418000 (via core USDHC1_* macros),
clock root 58 (SYS_PLL2_PFD2/2 ~198 MHz) + LPCG117, pins GPIO_SD_B1_00..05
(ALT0, RT1176 ODE/PULL/PDRV pad encoding). IRQ_SDHC -> IRQ_USDHC1. Shares the
i.MX MIX_CTRL / infinite-transfer / v1-card paths. HAS_SDIO_CLASS for 1176.

Co-Authored-By: Claude Opus 4.8 <noreply@anthropic.com>
EOF
)"
```

---

## Task 4: Gate — `sd_block_test` (Phase A: raw SDIO, PIO + SDMA)

The first red→green cycle. Exercises Tasks 1–3 by building + running a raw block read/write on a QEMU SD-card image, under both `FIFO_SDIO` (PIO) and `DMA_SDIO` (SDMA, buffer in `DMAMEM`).

**Files:**
- Create: `~/Development/SdFat/tests/sd_block_test/CMakeLists.txt`
- Create: `~/Development/SdFat/tests/sd_block_test/sd_block_test.cpp`
- Create: `~/Development/SdFat/tests/sd_block_test/run_qemu_sd_block.sh`
- Create (build artifact, gitignored): `~/Development/SdFat/tests/sd_block_test/card.img`

- [ ] **Step 1: Write the gate test (`sd_block_test.cpp`)**

```cpp
#include "Arduino.h"
#include "HardwareSerial.h"
#include <SdFat.h>

// SdioCard is SdFat's raw SDIO backend (Phase A driver).
static SdioCard card;
static uint8_t pattern[512];
static uint8_t pio_buf[512];                 // any RAM is fine for the PIO/FIFO path
DMAMEM static uint8_t dma_buf[512] __attribute__((aligned(32)));  // OCRAM: DMA-reachable, 4-byte-aligned

// Write a known pattern to a sector, read it back, compare byte-exact.
static bool blockRoundTrip(uint8_t opt, uint8_t* buf) {
  if (!card.begin(SdioConfig(opt))) return false;
  if (card.sectorCount() == 0) return false;
  for (int i = 0; i < 512; i++) pattern[i] = (uint8_t)(i * 7 + (opt ? 0x5A : 0xA5));
  const uint32_t lba = 12345;
  memcpy(buf, pattern, 512);
  if (!card.writeSector(lba, buf)) return false;
  memset(buf, 0, 512);
  if (!card.readSector(lba, buf)) return false;
  return memcmp(buf, pattern, 512) == 0;
}

void setup() {
  Serial1.begin(115200);
  while (!Serial1) {}

  bool init_ok = card.begin(SdioConfig(FIFO_SDIO));
  Serial1.print("SD_INIT=");       Serial1.println(init_ok ? "PASS" : "FAIL");

  bool pio = blockRoundTrip(FIFO_SDIO, pio_buf);
  Serial1.print("SD_BLOCK_PIO=");  Serial1.println(pio ? "PASS" : "FAIL");

  bool dma = blockRoundTrip(DMA_SDIO, dma_buf);
  Serial1.print("SD_BLOCK_DMA=");  Serial1.println(dma ? "PASS" : "FAIL");
}
void loop() {}
```

- [ ] **Step 2: Write the CMakeLists (`CMakeLists.txt`)**

```cmake
cmake_minimum_required(VERSION 3.24)
project(sd_block_test)

set(TEENSY_VERSION 117 CACHE STRING "")
set(EVKB $ENV{HOME}/Development/rt1170/evkb)

include(FetchContent)
FetchContent_Declare(teensy_cmake_macros SOURCE_DIR ${EVKB}/teensy-cmake-macros)
FetchContent_MakeAvailable(teensy_cmake_macros)
include(${teensy_cmake_macros_SOURCE_DIR}/CMakeLists.include.txt)

import_arduino_library(cores ${EVKB}/cores/imxrt1176)
import_arduino_library(SdFat ${CMAKE_CURRENT_LIST_DIR}/../..)
import_arduino_library(SPI   $ENV{HOME}/Development/SPI)

teensy_add_executable(sd_block_test sd_block_test.cpp)
teensy_target_link_libraries(sd_block_test cores SdFat SPI)

target_link_libraries(sd_block_test.elf stdc++)
```

- [ ] **Step 3: Write the runner (`run_qemu_sd_block.sh`), executable**

```sh
#!/bin/sh
set -e
QEMU=~/Development/rt1170/evkb/tools/qrun
DIR=$(cd "$(dirname "$0")" && pwd)
. ~/Development/rt1170/evkb/tools/gate-lib.sh
gate_init
ELF="$DIR/build/sd_block_test.elf"; OUT="$DIR/sd_block.uart"; IMG="$DIR/card.img"
rm -f "$OUT"
# 4 GB sparse raw image -> QEMU presents an SDHC (block-addressed) card, matching
# a real microSD.  Sparse: costs no disk until written.
[ -f "$IMG" ] || mkfile -n 4g "$IMG"
"$QEMU" -M mimxrt1170-evk -global fsl-imxrt1170.boot-xip=on -kernel "$ELF" \
    -display none -serial file:"$OUT" \
    -drive if=sd,format=raw,file="$IMG" \
    -d guest_errors,unimp -D "$DIR/sd_block.dbg" &
P=$!; gate_pid $P; sleep 5; kill $P 2>/dev/null; wait $P 2>/dev/null || true
echo "==== captured ===="; cat "$OUT"
grep -q "SD_INIT=PASS"      "$OUT" || { echo "FAIL: init";      exit 1; }
grep -q "SD_BLOCK_PIO=PASS" "$OUT" || { echo "FAIL: PIO block"; exit 1; }
grep -q "SD_BLOCK_DMA=PASS" "$OUT" || { echo "FAIL: DMA block"; exit 1; }
echo "PASS: SD raw block RW verified (init + PIO + SDMA)"
```
Then: `chmod +x ~/Development/SdFat/tests/sd_block_test/run_qemu_sd_block.sh`

- [ ] **Step 4: Build the gate**

Run:
```bash
cd ~/Development/SdFat/tests/sd_block_test
rm -rf build && cmake -B build -S . >/dev/null && cmake --build build 2>&1 | tail -20
```
Expected: `build/sd_block_test.elf` is produced. (If a USDHC register/type is wrong from Tasks 1–3, this is where it fails — fix the offending def and rebuild. `DMAMEM` must be defined by the core; if the linker reports `.dmabuffers` unplaced, confirm the core's `DMAMEM` macro/linker section as used by the eDMA/SerialUSB drivers.)

- [ ] **Step 5: Run the gate**

Run: `~/Development/SdFat/tests/sd_block_test/run_qemu_sd_block.sh`
Expected final line: `PASS: SD raw block RW verified (init + PIO + SDMA)`.

If it fails, inspect `sd_block.uart` (which marker failed) and `sd_block.dbg`. A `LOG_UNIMP` line names any USDHC register the model doesn't implement (the `-d ...,unimp` was added for exactly this) — if hit, add that register to the i.MX intercept in `~/Development/qemu2/hw/sd/sdhci.c` (`esdhc_read`/`esdhc_write`), rebuild qemu2, and re-run. Debug with systematic-debugging; refine the QEMU model to mirror silicon.

- [ ] **Step 6: Add a `.gitignore` for build artifacts, then commit (SdFat repo)**

Create `~/Development/SdFat/tests/sd_block_test/.gitignore`:
```
build/
card.img
*.uart
*.dbg
```
Commit:
```bash
git -C ~/Development/SdFat add tests/sd_block_test/CMakeLists.txt tests/sd_block_test/sd_block_test.cpp tests/sd_block_test/run_qemu_sd_block.sh tests/sd_block_test/.gitignore
git -C ~/Development/SdFat commit -m "$(cat <<'EOF'
tests: add sd_block_test QEMU gate (raw SDIO, PIO + SDMA)

SdioCard begin + byte-exact sector write/read under FIFO_SDIO and DMA_SDIO
(SDMA buffer in DMAMEM). Attaches a 4 GB sparse SD image (SDHC path). Markers
SD_INIT / SD_BLOCK_PIO / SD_BLOCK_DMA = PASS.

Co-Authored-By: Claude Opus 4.8 <noreply@anthropic.com>
EOF
)"
```

---

## Task 5: SD wrapper — `newdigate/SD` (PaulS_SD) for 1176

**Files:**
- Modify: `~/Development/PaulS_SD/src/SD.h:40`

- [ ] **Step 1: Add 1176 to the `BUILTIN_SDCARD` guard**

Change:
```c
#if defined(__MK64FX512__) || defined(__MK66FX1M0__) || defined(__IMXRT1062__)
#define BUILTIN_SDCARD 254
#endif
```
to:
```c
#if defined(__MK64FX512__) || defined(__MK66FX1M0__) || defined(__IMXRT1062__) || defined(__IMXRT1176__)
#define BUILTIN_SDCARD 254
#endif
```
(No change needed for `_SD_DAT3`: SD.cpp's `_SD_DAT3` uses are all `#ifdef _SD_DAT3`-guarded and it is intentionally left undefined for boards without a dedicated DAT3 pin macro — see SD.cpp's own comment. The `__arm__` base-class block at SD.h:44 already resolves for 1176.)

- [ ] **Step 2: Sanity-check**

Run: `grep -A1 "defined(__IMXRT1176__)" ~/Development/PaulS_SD/src/SD.h | grep -q "BUILTIN_SDCARD 254" && echo "wrapper OK"`
Expected: `wrapper OK`. (Full compile is exercised by the Task 6 gate build.)

- [ ] **Step 3: Commit (SD repo)**

```bash
git -C ~/Development/PaulS_SD add src/SD.h
git -C ~/Development/PaulS_SD commit -m "$(cat <<'EOF'
SD: define BUILTIN_SDCARD for __IMXRT1176__

Enables the SDIO path (SD.begin(BUILTIN_SDCARD) -> SdioConfig(FIFO_SDIO)) on
the MIMXRT1170-EVKB. Base-class selection already resolves via __arm__.

Co-Authored-By: Claude Opus 4.8 <noreply@anthropic.com>
EOF
)"
```

---

## Task 6: Gate — `sd_fs_test` (Phase B: FAT filesystem)

The Phase-B red→green cycle: mount a FAT image, create/write/reopen/read-back a file byte-exact, and list the root directory — proving the `SD.h` → SdFat → SdioCard → USDHC1 stack and host↔firmware FAT interop.

**Files:**
- Create: `~/Development/SdFat/tests/sd_fs_test/CMakeLists.txt`
- Create: `~/Development/SdFat/tests/sd_fs_test/sd_fs_test.cpp`
- Create: `~/Development/SdFat/tests/sd_fs_test/run_qemu_sd_fs.sh`
- Create: `~/Development/SdFat/tests/sd_fs_test/.gitignore`

- [ ] **Step 1: Write the gate test (`sd_fs_test.cpp`)**

```cpp
#include "Arduino.h"
#include "HardwareSerial.h"
#include <SD.h>

static const char* kName = "rttest.txt";
static const char* kMsg  = "RT1176 SD FAT works 0123456789";

void setup() {
  Serial1.begin(115200);
  while (!Serial1) {}

  if (!SD.begin(BUILTIN_SDCARD)) {
    Serial1.println("SD_FS_WRITE=FAIL");
    Serial1.println("SD_FS_READ=FAIL");
    Serial1.println("SD_FS_DIR=FAIL");
    return;
  }

  // --- write ---
  SD.remove(kName);
  File f = SD.open(kName, FILE_WRITE);
  bool wrote = false;
  if (f) {
    size_t n = f.write((const uint8_t*)kMsg, strlen(kMsg));
    f.close();
    wrote = (n == strlen(kMsg));
  }
  Serial1.print("SD_FS_WRITE="); Serial1.println(wrote ? "PASS" : "FAIL");

  // --- reopen + read back byte-exact ---
  File g = SD.open(kName, FILE_READ);
  bool readback = false;
  if (g) {
    char buf[64] = {0};
    int n = g.read(buf, sizeof(buf) - 1);
    g.close();
    readback = (n == (int)strlen(kMsg)) && memcmp(buf, kMsg, strlen(kMsg)) == 0;
  }
  Serial1.print("SD_FS_READ="); Serial1.println(readback ? "PASS" : "FAIL");

  // --- list root directory (expect at least our file) ---
  int count = 0;
  File root = SD.open("/");
  if (root) {
    for (File e = root.openNextFile(); e; e = root.openNextFile()) {
      count++;
      e.close();
    }
    root.close();
  }
  Serial1.print("SD_FS_DIR="); Serial1.println(count > 0 ? "PASS" : "FAIL");
}
void loop() {}
```

- [ ] **Step 2: Write the CMakeLists (`CMakeLists.txt`)**

```cmake
cmake_minimum_required(VERSION 3.24)
project(sd_fs_test)

set(TEENSY_VERSION 117 CACHE STRING "")
set(EVKB $ENV{HOME}/Development/rt1170/evkb)

include(FetchContent)
FetchContent_Declare(teensy_cmake_macros SOURCE_DIR ${EVKB}/teensy-cmake-macros)
FetchContent_MakeAvailable(teensy_cmake_macros)
include(${teensy_cmake_macros_SOURCE_DIR}/CMakeLists.include.txt)

import_arduino_library(cores ${EVKB}/cores/imxrt1176)
import_arduino_library(SdFat ${CMAKE_CURRENT_LIST_DIR}/../..)
import_arduino_library(SD    $ENV{HOME}/Development/PaulS_SD)
import_arduino_library(SPI   $ENV{HOME}/Development/SPI)

teensy_add_executable(sd_fs_test sd_fs_test.cpp)
teensy_target_link_libraries(sd_fs_test cores SdFat SD SPI)

target_link_libraries(sd_fs_test.elf stdc++)
```

- [ ] **Step 3: Write the runner (`run_qemu_sd_fs.sh`), executable**

```sh
#!/bin/sh
set -e
QEMU=~/Development/rt1170/evkb/tools/qrun
DIR=$(cd "$(dirname "$0")" && pwd)
. ~/Development/rt1170/evkb/tools/gate-lib.sh
gate_init
ELF="$DIR/build/sd_fs_test.elf"; OUT="$DIR/sd_fs.uart"; IMG="$DIR/card.img"

# Build a fresh FAT card image each run.  Prefer mtools (mformat) for a
# controlled FAT; the firmware then creates/reads its own file.
rm -f "$OUT" "$IMG"
mkfile -n 512m "$IMG"                 # 512 MB sparse
if command -v mformat >/dev/null 2>&1; then
    mformat -i "$IMG" -F ::           # FAT (superfloppy, no partition table)
else
    # macOS fallback: FAT16 superfloppy
    newfs_msdos -F 16 -S 512 "$IMG"
fi

"$QEMU" -M mimxrt1170-evk -global fsl-imxrt1170.boot-xip=on -kernel "$ELF" \
    -display none -serial file:"$OUT" \
    -drive if=sd,format=raw,file="$IMG" \
    -d guest_errors,unimp -D "$DIR/sd_fs.dbg" &
P=$!; gate_pid $P; sleep 6; kill $P 2>/dev/null; wait $P 2>/dev/null || true
echo "==== captured ===="; cat "$OUT"
grep -q "SD_FS_WRITE=PASS" "$OUT" || { echo "FAIL: fs write"; exit 1; }
grep -q "SD_FS_READ=PASS"  "$OUT" || { echo "FAIL: fs read";  exit 1; }
grep -q "SD_FS_DIR=PASS"   "$OUT" || { echo "FAIL: fs dir";   exit 1; }
# Host-side interop: the file the firmware wrote must be readable back on the host.
if command -v mdir >/dev/null 2>&1; then
    echo "---- host mdir ----"; mdir -i "$IMG" :: || true
fi
echo "PASS: SD FAT filesystem verified (write + read-back + dir)"
```
Then: `chmod +x ~/Development/SdFat/tests/sd_fs_test/run_qemu_sd_fs.sh`

- [ ] **Step 4: Build the gate**

Run:
```bash
cd ~/Development/SdFat/tests/sd_fs_test
rm -rf build && cmake -B build -S . >/dev/null && cmake --build build 2>&1 | tail -20
```
Expected: `build/sd_fs_test.elf` produced. (Compiles `FS.h`, PaulS_SD's `SD.h`/`SD.cpp`, and SdFat's FAT layer against the core — first full validation of Tasks 2 & 5.)

- [ ] **Step 5: Run the gate**

Run: `~/Development/SdFat/tests/sd_fs_test/run_qemu_sd_fs.sh`
Expected final line: `PASS: SD FAT filesystem verified (write + read-back + dir)`.

If `SD.begin` fails to mount the `mformat` superfloppy, create an MBR-partitioned FAT instead (one primary FAT32 partition starting at LBA 2048) and re-run — SdFat prefers an MBR partition and falls back to superfloppy. Use systematic-debugging; the `sd_fs.dbg` `LOG_UNIMP` lines and the `SD_FS_*` markers localize the failure (mount vs. write vs. read).

- [ ] **Step 6: Add `.gitignore` and commit (SdFat repo)**

Create `~/Development/SdFat/tests/sd_fs_test/.gitignore`:
```
build/
card.img
*.uart
*.dbg
```
Commit:
```bash
git -C ~/Development/SdFat add tests/sd_fs_test/CMakeLists.txt tests/sd_fs_test/sd_fs_test.cpp tests/sd_fs_test/run_qemu_sd_fs.sh tests/sd_fs_test/.gitignore
git -C ~/Development/SdFat commit -m "$(cat <<'EOF'
tests: add sd_fs_test QEMU gate (FAT filesystem over SDIO)

SD.begin(BUILTIN_SDCARD) on an mformat'd FAT image: create/write/reopen/
read-back byte-exact + root dir list. Markers SD_FS_WRITE/READ/DIR = PASS;
optional host-side mdir interop check.

Co-Authored-By: Claude Opus 4.8 <noreply@anthropic.com>
EOF
)"
```

---

## Task 7: Hardware verification (final arbiter)

QEMU cannot vouch for two things: SDMA DMA-reachability of the buffer, and card power. Verify on the real EVKB. Controller drives flash + VCOM; the user inserts the card and confirms.

**Files:**
- Modify (if needed): `~/Development/SdFat/src/SdCard/SdioTeensy.cpp` (card power-enable hook in `SdioCard::begin`)

- [ ] **Step 1: Identify the 1170-EVKB card power-enable GPIO**

The µSD `VSD_3V3` rail is switched by P-FET Q11 (netlist). Trace Q11's gate control net to the MCU pin (search `~/Development/rt1170/.../pstxnet.dat` for the Q11 gate net and the driving GPIO pad; cross-ref the EVKB schematic PDF). Determine the pad, the core Arduino pin number, and the active level. If the slot is powered by a fixed rail (no MCU control), no hook is needed — note that and skip Step 2.

- [ ] **Step 2: Add the power-enable hook (if a GPIO controls it)**

In `SdioCard::begin` (`SdioTeensy.cpp`, after `m_version2 = false;`, alongside the existing `ARDUINO_MIMXRT1060_EVKB` block ~line 651), add a parallel `__IMXRT1176__` block modeled on it — `pinMode(<pin>, OUTPUT)`, drive to the SDK "enable" level, `delay(10)` for the rail. Use the pin/polarity found in Step 1. Commit to the SdFat repo.

- [ ] **Step 3: Flash + run the block gate on hardware**

```bash
cd ~/Development/SdFat/tests/sd_block_test && cmake --build build
LinkServer flash MIMXRT1176:MIMXRT1170-EVKB load build/sd_block_test.elf
```
Insert a real µSD. Capture VCOM (`/dev/cu.usbmodem5DQ2DDHVWO5EI3` @115200, pyserial + gtimeout per the serial-capture note). Expected: `SD_INIT=PASS`, `SD_BLOCK_PIO=PASS`, and critically `SD_BLOCK_DMA=PASS` (the DMA-reachability proof QEMU can't give). If DMA fails on HW but PIO passes, the `dma_buf` region isn't reachable by the USDHC master — move it to a confirmed DMA-reachable region (OCRAM), not DTCM.

- [ ] **Step 4: Flash + run the FS gate on hardware**

```bash
cd ~/Development/SdFat/tests/sd_fs_test && cmake --build build
LinkServer flash MIMXRT1176:MIMXRT1170-EVKB load build/sd_fs_test.elf
```
With a **FAT-formatted** µSD inserted. Expected: `SD_FS_WRITE/READ/DIR=PASS`.

- [ ] **Step 5: Persistence across power cycle**

After Step 4 wrote `rttest.txt`: power-cycle the board, then flash a read-only variant (or re-run the FS gate with the write section skipped) and confirm `rttest.txt` still reads back `kMsg`. Persistence across power loss is the real SD proof. Remove the card, mount on a host, confirm the file is present and correct.

- [ ] **Step 6: Record the outcome**

Update the spec's status and note HW-verified results. If any QEMU-vs-silicon divergence was found and fixed (a qemu2 model tweak, or the DMA-region placement), capture it for the memory note. Do not push unless the user asks.

---

## Notes for the implementer

- **YAGNI already applied:** no ADMA2, no 50 MHz high-speed as a requirement (25 MHz default; the CMD6 switch runs opportunistically), no 1.8 V UHS, no card-detect IRQ. `AudioPlaySdWav` is a separate spec (C).
- **Commit boundaries:** core changes → `git -C ~/Development/rt1170/evkb/cores`; driver/gates → `git -C ~/Development/SdFat`; wrapper → `git -C ~/Development/PaulS_SD`. The evkb working tree is shared across sessions — check `git status` before assuming a file vanished.
- **If the QEMU gate needs a model fix**, it will be a missing i.MX register in `~/Development/qemu2/hw/sd/sdhci.c` (`esdhc_read`/`esdhc_write`) surfaced by `-d unimp`. The USDHC model has no `min_access_size` bug (byte access is allowed), so that class of trap does not apply here.
- **DMAMEM:** the SDMA buffer (`dma_buf`) must land in a DMA-reachable region. Confirm the core's `DMAMEM` macro places it in OCRAM (as the eDMA/SerialUSB drivers rely on); DTCM is not reachable by the USDHC master and would pass QEMU but fail hardware.
