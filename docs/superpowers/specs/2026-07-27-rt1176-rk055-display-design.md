# RT1176 → RK055HDMIPI4MA0 720×1280 MIPI-DSI panel (v1: "test pattern on glass") — Design

**Date:** 2026-07-27
**Status:** validated design, ready for an implementation plan
**Fulfils:** the "v4 — the generic LCDIFv2/MIPI-DSI core factored out for reuse by other DSI
panels" roadmap entry of `2026-07-23-rt1176-rpi-display-design.md` (§10), which explicitly
scoped out "any second panel". This is that second panel.

---

## 1. Goal

Light the **RK055HDMIPI4MA0** panel on the MIMXRT1170-EVKB from our Arduino-style core, and put
an **un-fakeable test pattern** on the glass — verified in QEMU *and* on real silicon.

"Lit" for v1 means: the LCDIFv2 scans a 720×1280 RGB565 framebuffer out of SDRAM, through the
MIPI-DSI host over 2 HS data lanes, into an HX8394 initialised over DSI, and a colour-bar +
diagonal pattern is visible on the panel — where every plausible failure (blank output, wrong
stride, wrong pixel format, wrong orientation) is a *visually distinct* failure rather than
"uniform something".

**Panel identity (verified):** RK055HDMIPI4MA0 == RK055MHD091A0-CTG == `DEMO_PANEL_RK055MHD091`
(value 2) in `mcuxsdk/examples/_boards/evkbmimxrt1170/display_support.h`. **720 × 1280**, driver
IC **HX8394**, **2 MIPI-DSI lanes**. It is the SDK's default panel for this board.

**Panel is physically in hand and connected to `J48`** (confirmed 2026-07-27). A hardware gate is
therefore part of v1, not a deferred aspiration — this spec does not repeat the LVGL work's
mistake of planning a HW gate against an assumed connection.

---

## 2. Scope

**In scope (v1):**

- LCDIFv2 → MIPI-DSI (2 lanes, **burst** video mode) → HX8394 → 720×1280 RGB565 in SDRAM.
- Panel power / reset / backlight via three GPIOs.
- A static test pattern (colour bars + diagonal), painted into the framebuffer.
- A **permanent** LCDIFv2 output-underrun assertion in the gate.
- Raising `BUS_CLK_ROOT` to 240 MHz core-wide (M0), and restructuring `RPiDisplay` into a
  panel-neutral `MipiDisplay` (M0).

**Out of scope — named follow-ons, not silently dropped:**

- **v2** — capacitive touch (controller on `J48` pins 26–29 via LPI2C5/Wire2).
- **v3** — LVGL `lv_indev` binding on top of v2, joining the existing `lvgl_rpi_panel_test` shape.
- **v4** — double buffering / page flip on vsync, PXP-accelerated drawing, XRGB8888.
- **Any hardware re-verification of the Raspberry Pi 7" panel.** That panel remains blocked on the
  `J84` DSI lane-order mismatch (see [[rt1176-rpi-display-fpc-remap]]); its QEMU gate stays green
  and its hardware status stays "blocked, documented". Nothing in this work changes that, and
  nothing in this work claims otherwise.

### 2.1 Why this is materially lower-risk than the RPi panel

| | RPi 7" v1.1 | RK055HDMIPI4MA0 |
|---|---|---|
| Connector | `J84`, 15-pin — lane order mismatched, **hard-blocked in hardware** | `J48`, 40-pin — laid out for this panel |
| Panel bring-up dependencies | ATtiny88 over LPI2C5 **and** an external 5 V supply | 3 plain GPIOs; **no I²C in the path** |
| Init-sequence source | GPL Linux drivers, transcribed as facts | **BSD-3 SDK** (`fsl_hx8394.c`) — permissive, directly portable |
| Pixel clock | VIDEO_PLL + the AI-protocol PLL bring-up | `PLL_528 / 9` — **no PLL bring-up at all** |
| Ever shown an image | **No** | Unknown, but nothing is known-broken |

The RPi bring-up's three hardest problems (ATtiny power, VIDEO_PLL AI protocol, the GPL-sourced
bridge sequence) are all *absent* here. What remains is the SoC-side chain — which that work
already built and debugged — plus a new, permissively-licensed panel init.

---

## 3. Hardware facts

### 3.1 `J48` — the RK055 connector (from the RevC3 netlist)

`~/Development/rt1170/MIMXRT1170-EVKB-DESIGNFILES_RevC3/pst2kicad/board.net`, component `J48`
(`CON_1x40`, PN 211-80552, footprint `con_fpc_40f_0p5_smt_ra`):

| `J48` pin | Net | Role |
|---|---|---|
| 5 / 6 | `MIPI_DSI_DN0` / `MIPI_DSI_DP0` | data lane 0 |
| 8 / 9 | `MIPI_DSI_DN1` / `MIPI_DSI_DP1` | data lane 1 |
| 11 / 12 | `MIPI_DSI_CLKN` / `MIPI_DSI_CLKP` | HS clock lane |
| 22 | `LCD_LPTE_1V8` | tearing effect (unused in v1) |
| 26 / 27 | `GPIO_LPSR_04` / `GPIO_LPSR_05` | LPI2C5 SDA/SCL — touch (v2) |
| 28 / 29 | `CTP_RST_B_C` / `CTP_INT_C` | touch reset / interrupt (v2) |
| 34 | `BACKLIGHT_CTL_C` | backlight enable |
| 4,7,10,13,16,19,20,31,33,35,36,37,S1,S2 | `GND` | |

**The `J84` lane-order mismatch that hard-blocks the RPi panel does not apply.** `J48` is a
distinct connector, laid out by NXP for this panel, and both DSI data lanes plus the clock lane
land on the pins the panel expects.

> Note for v2: `J48` pins 26/27 and `J84` pins 11/12 are the **same nets** (`GPIO_LPSR_04/05`).
> If both panels remain plugged in, they share the LPI2C5 bus. Harmless for v1 (no I²C in the
> path); a real constraint for touch.

### 3.2 Panel control GPIOs

From `mcuxsdk/examples/_boards/evkbmimxrt1170/board.h`:

| Signal | GPIO | Notes |
|---|---|---|
| Panel power | `GPIO11_IO16` | `BOARD_MIPI_PANEL_POWER_{GPIO,PIN}` |
| Panel reset | `GPIO9_IO01` | `BOARD_MIPI_PANEL_RST_{GPIO,PIN}`, active low pulse |
| Backlight | `GPIO9_IO29` | `BOARD_MIPI_PANEL_BL_{GPIO,PIN}`, driven high after init succeeds |
| Touch reset (v2) | `GPIO9_IO00` | |
| Touch interrupt (v2) | `GPIO8_IO31` | |

All three v1 pins are plain digital outputs. No I²C, no external supply, no ATtiny.

### 3.3 Blocks (already in `cores/imxrt1176/imxrt1176.h` from the RPi work)

| Block | Base | IRQ |
|---|---|---|
| LCDIFv2 | `0x40808000` | 55 |
| MIPI DSI host (4 sub-blocks) | `0x4080C000` | 59 |
| SDRAM (framebuffer) | `0x80000000` | — |
| PXP | `0x40814000` | 57 |

---

## 4. Timing and clock plan

All values pinned against the local BSD-3 SDK; none guessed.

### 4.1 Modeline

From `display_support.c`, `DEMO_PANEL_RK055MHD091`:

| | H | V |
|---|---|---|
| active | 720 | 1280 |
| sync width | 6 | 2 |
| front porch | 12 | 16 |
| back porch | 24 | 14 |
| **total** | **762** | **1312** |

762 × 1312 = **999,744** clocks per frame.

### 4.2 Clock roots

| Root | Mux | Divide | Result | Source of fact |
|---|---|---|---|---|
| `ROOT70` LCDIFv2 pixel | **4** = `SysPll2Out` | 9 | **58.667 MHz** | `BOARD_InitLcdifClock()` |
| `ROOT71` MIPI_REF (D-PHY ref) | 1 = `Osc24MOut` | 1 | 24.000 MHz | `BOARD_InitMipiDsiClock()` |
| `ROOT72` MIPI_ESC | **4** = `SysPll2Out` | 11 | 48.000 MHz | `BOARD_InitMipiDsiClock()` |
| MIPI clock **group** divider | — | 3 | TxClkEsc = **16 MHz** | `kCLOCK_Group_MipiDsi`, `div0 = 2` |
| `ROOT2` BUS — **core-wide, M0** | **4** = `SysPll3Out` | 2 | **240 MHz** | `clock_config.c` `BOARD_BootClockRUN` |

Mux encodings verified in `devices/RT/RT1170/MIMXRT1176/drivers/fsl_clock.h`
(`kCLOCK_LCDIFV2_ClockRoot_MuxSysPll2Out = 4`, `kCLOCK_MIPI_ESC_ClockRoot_MuxSysPll2Out = 4`,
`kCLOCK_MIPI_REF_ClockRoot_MuxOsc24MOut = 1`, `kCLOCK_BUS_ClockRoot_MuxSysPll3Out = 4`). Root
indices: Bus = 2, Lcdifv2 = 70, Mipi_Ref = 71, Mipi_Esc = 72. NXP's `.div` is a *divide value*;
the `CONTROL` register's `DIV` **field** holds divide−1 — `display_clock.cpp`'s existing
`root_ctrl()` helper already encodes this and its `static_assert`s already check it.

**VIDEO_PLL is not used by this panel at all.** NXP sources both the pixel and escape clocks from
`PLL_528`. `displayClockInit()` therefore runs the AI-protocol VIDEO_PLL bring-up only when the
selected panel's config actually muxes a root onto VIDEO_PLL — which the RPi panel does and this
one does not. That removes the single most fragile subsystem of the RPi bring-up from this path,
while keeping it working for the panel that needs it.

### 4.3 Derived figures

- **Frame rate:** 58.667 MHz ÷ 999,744 = **58.7 Hz**.
- **D-PHY bit clock:** `dpiClkFreq × (24 / laneCount) × 9/8` = **791,999,991 Hz** over 2 lanes.
  (Corrected 2026-07-27 from a nominal "792 MHz": the arithmetic is integer and truncates twice —
  528,000,000/9 = 58,666,666 rather than 58,666,666.67, then 703,999,992/8 = 87,999,999. The
  9 Hz shortfall is irrelevant to the link but the exact value is what the firmware asserts, so
  the round number is not written anywhere it could be mistaken for the computed one. Elsewhere
  in this document "792 MHz" is used as the informal name for this rate.)
  (The RPi panel locked at 744 MHz over 1 lane on real silicon, so this rate is in a
  demonstrated range for this D-PHY — but see §8.)
- **Framebuffer:** 720 × 1280 × 2 = **1,843,200 bytes**, pitch **1440 bytes** (already 32-aligned,
  and within `CTRLDESCL3[PITCH]`'s 16-bit field). Single buffer in SDRAM for v1.
- **Sustained scanout bandwidth:** ~**108 MB/s**, versus ~46 MB/s for the RPi panel — **2.4×**.

### 4.4 Register-field headroom

Every value fits, and this is asserted statically in `panel_config.h` the way `display_timing.h`
already does for the RPi panel:

- `DISP_SIZE` `DELTA_X`/`DELTA_Y` are 12-bit → 720 (`0x2D0`) and 1280 (`0x500`) fit.
- DSI DPI `VACTIVE` is 14-bit → 1280 fits.
- LCDIFv2 blanking fields are 9-bit → HFP 12 / HSW 6 / HBP 24 fit.
- DSI DPI `VBP`/`VFP` are 8-bit → 16 and 14 fit.

### 4.5 Divergences from the RPi configuration

All three are compile-time panel config, and all three come from NXP's own board code for this
panel. The lane count is the obvious one; the other two are easy to miss and would produce a
blank panel with no diagnostic:

| | RPi 7" | RK055 |
|---|---|---|
| `videoMode` | `kDSI_DpiNonBurstWithSyncPulse` (0) | **`kDSI_DpiBurst` (2)** |
| `autoInsertEoTp` | `false` | **`true`** |
| lanes | 1 | **2** |

---

## 5. Data path

```
   PXP / CPU paints the test pattern
                 │
                 ▼
   SDRAM framebuffer (720×1280 RGB565, 1,843,200 B)
                 │  ~108 MB/s sustained, fetched on BUS_CLK_ROOT = 240 MHz
                 ▼
   LCDIFv2  (layer 0 scanout, 762×1312 video timing @ 58.7 Hz)
                 │
                 ▼
   MIPI-DSI host:  DPI_INTFC (burst video mode, 24-bit)
                 +  D-PHY (2 HS lanes @ 792 MHz bit clock)
                 │
                 ├─ APB packet IF ──▶ HX8394 DCS/generic init writes
                 ▼
        [ FPC / J48 ] ──▶ HX8394 ──▶ 720×1280 glass

   GPIO11_IO16 (power) ─▶ GPIO9_IO01 (reset pulse) ─▶ … ─▶ GPIO9_IO29 (backlight)
```

**Why video mode, not command mode:** unchanged from the RPi rationale. The HX8394 as configured
here is driven as a continuously-refreshed video-mode panel; the LCDIFv2 supplies the DPI stream.
Burst mode (rather than non-burst-with-sync-pulse) is what NXP uses for this panel, and it relaxes
the HS-link timing by allowing the line to be sent faster than pixel rate and idle in LP.

---

## 6. Architecture

### 6.1 Library: `RPiDisplay` → `newdigate/MipiDisplay`

The existing library's SoC layer is already cleanly separable — `lcdifv2.cpp`, `mipi_dsi.cpp` and
`display_clock.cpp` touch the panel only through `display_timing.h` constants plus three
compile-time values (`DSI_LANES`, the DPI video mode, `DSI_DPI_PIXEL_CLK_HZ`). The restructure
formalises that seam rather than inventing one.

```
MipiDisplay/
  soc/
    lcdifv2.{h,cpp}         layer scanout; geometry from panel_config.h
    mipi_dsi.{h,cpp}        host + D-PHY + DPI; lanes / video mode / EoTp now parameters
    display_clock.{h,cpp}   roots from panel_config.h; VIDEO_PLL bring-up only if requested
  panels/
    rpi7/   panel_config.h   tc358762.{h,cpp}   rpi_attiny.{h,cpp}
    rk055/  panel_config.h   hx8394.{h,cpp}     panel_gpio.{h,cpp}
  Display.{h,cpp}           one facade; the panel is chosen at compile time
```

Panel selected with `-DEVKB_DSI_PANEL=RK055` (default) or `RPI7`.

**Compile-time selection is correct here, not a compromise:** the RT1176 has exactly **one**
MIPI-DSI host, so two DSI panels can never be driven simultaneously. Runtime polymorphism would
buy nothing and cost indirection in the scanout path.

**Rename is free:** `RPiDisplay` has no origin remote and is pinned `HEAD` (local-first) in
`evkb.cmake` — nothing external depends on the name.

**Rename blast radius (all landed in M0):**

- `evkb.cmake` — the `_evkb_lib()` entry and the RPiDisplay comment in `import_evkb_lvgl()`.
- `~/Development/LVGL/port/lvgl_rpi_panel.{h,cpp}` → `lvgl_mipi_panel.{h,cpp}`.
- `examples/display/rpi_panel_test/` — `CMakeLists.txt`, the sketch, `run_qemu.sh`.
- `examples/display/lvgl_rpi_panel_test/` — same three.
- `tools/license-audit.sh` — the repo path it sweeps.
- `CLAUDE.md` — the peripheral-library list.

### 6.2 `panels/rk055/panel_config.h`

The one home for everything panel-specific: geometry, modeline, `static_assert`s on register-field
widths, lane count, video mode, EoTp, and the clock-root plan (mux + divide per root, plus a flag
saying VIDEO_PLL is not required). `soc/` reads it and nothing else.

### 6.3 `panels/rk055/hx8394.{h,cpp}`

Our own Teensy-style transcription of the BSD-3 `fsl_hx8394.c` sequence:

1. power pin high, 1 ms
2. reset low, 1 ms, reset high, 50 ms
3. generic write `B9 FF 83 94` — the EXTC unlock; **nothing else is accepted before it**
4. generic write `BA 60|(lanes-1) 03 68 6B B2 C0` — `SETMIPI`, carrying the lane count
5. the **21-entry** command table, in this exact order — `36`, `B1`, `B2`, `B4`, `D3`, `D5`, `D6`,
   `B6`, `E0`, `C0`, `CC`, `D4`, `BD`, `D8`, `BD`, `BD`, `B1`, `BD`, `BF`, `C6`, `35`. Note `BD`
   appears four times and `B1` twice: `BD` is a page/bank select, so the repeats are **not**
   redundant and the order is load-bearing. Longest payload is the 59-byte `E0` gamma table, well
   inside `dsiWrite()`'s 256-byte TX FIFO limit
6. DCS exit-sleep, 120 ms, DCS set-display-on
7. backlight GPIO high **only if every step above succeeded**

### 6.4 Public API (unchanged shape, YAGNI)

```cpp
class DisplayClass {
public:
  bool begin();                       // clocks → LCDIFv2 → DSI → panel init → backlight
  void fillScreen(uint16_t rgb565);
  void drawTestPattern();             // v1: colour bars + diagonal
  uint16_t width()  const;            // 720
  uint16_t height() const;            // 1280
  uint16_t *framebuffer() const;
  // stage status for the gate
  bool clkOk() const; bool lcdifOk() const; bool dsiOk() const;
  bool panelOk() const; bool frameOk() const;
  uint32_t underrunCount() const;     // sampled LCDIFV2_INT_STATUS_D0[1]
};
extern DisplayClass Display;
```

`displayRgb565To888()` stays exported for the same reason as before: the gate must model the
identical RGB565↔PXP round trip the driver applies, so there is one home for it, not a copy on
each side.

### 6.5 QEMU model: `hw/display/imxrt_hx8394.c`

A new device mirroring `imxrt_tc358762.c`'s structure and its documented reasoning style. It
decodes DSI generic long writes forwarded by `imxrt_mipi_dsi.c` and asserts `downstream_ready`
(which `imxrt_lcdifv2.c` already checks before scanning out) only when **all** of:

1. `B9 FF 83 94` arrived **first** — any command before the EXTC unlock is a contract violation;
2. `SETMIPI` arrived, and its `byte1 & 3` **equals the DSI host's `CFG_NUM_LANES`**;
3. the full command table arrived;
4. DCS exit-sleep preceded DCS set-display-on;
5. the DSI host reports ready (existing `imxrt_mipi_dsi.c` gating).

It also tallies an FNV-1a checksum of the received pixel stream, exposed on a debug MMIO register
— the `PANEL_SUM` oracle, which has no counterpart on silicon.

The existing `imxrt_tc358762.c` is **left untouched**. Two panel models with parallel structure is
the honest outcome; a shared "generic DSI sink" abstraction would fit neither contract well and
would put a green gate at risk for no gain.

---

## 7. Milestones

### M0 — Foundation *(repo-wide, panel-independent; must fully land before M1)*

- `cores/imxrt1176/startup.c` step 5: `BUS_CLK_ROOT` ← mux 4 (`SysPll3Out`) / divide 2 =
  **240 MHz**, guarded on `SYS_PLL3_CTRL` (POWERUP && STABLE && !BYPASS && !GATE), falling back to
  the current `SysPll2Out`/3 = 176 MHz, then `OSC`/2. The guard chain means a boot that leaves
  PLL3 down degrades rather than hanging.
- **Delete the stale claim** in that comment that "SYS_PLL3 is left BYPASSED on this board's
  boot". It was never measured; the camera bring-up read `SYS_PLL3_CTRL = 0x2020201B` on silicon
  (POWERUP=1, STABLE=1, **BYPASS=0**) on 2026-07-25. Record the measured value in its place.
- Rename and restructure `RPiDisplay` → `MipiDisplay` per §6.1, introducing `panel_config.h`.

**Exit criteria:** `./tools/run-all-qemu-gates.sh` fully green — re-running *every* gate is the
real work of this milestone and the accepted cost of the up-front clock change;
`tools/license-audit.sh` PASS; `rpi_panel_test` and `lvgl_rpi_panel_test` still green. **Zero
functional change** beyond the bus clock.

### M1 — SoC chain at 720×1280 *(no panel driver yet)*

- `panels/rk055/panel_config.h`; `mipi_dsi.cpp` parameterised on lane count / video mode / EoTp;
  `display_clock.cpp` sourcing ROOT70/72 from `SysPll2Out` and skipping VIDEO_PLL for this panel.
- Gate `examples/display/rk055_panel_test/` asserting `CLK_OK`, `LCDIFV2_OK`, `DSI_OK`.

**Why this is its own milestone:** on silicon, `DSI_OK` is the genuinely new fact — **D-PHY lock
at 792 MHz across 2 lanes**, the tall pole of the whole project. Isolating it means a failure here
cannot be confused with a panel-init failure, which is exactly the ambiguity that cost the RPi
bring-up several sessions.

### M2 — HX8394 driver

- `panels/rk055/hx8394.{h,cpp}` and `panel_gpio.{h,cpp}` per §6.3.
- New QEMU device `hw/display/imxrt_hx8394.c` per §6.5, wired into `hw/arm/mimxrt1170-evk.c`.
- Gate gains `PANEL_OK`.

### M3 — Pixels on glass

- Test-pattern painter (colour bars + a diagonal), `FB_SUM`, `PANEL_SUM`, `FRAME_OK`.
- **`UNDERRUN=n/N`** promoted from a temp diagnostic to a permanent gate token, sampling
  `LCDIFV2_INT_STATUS_D0[1]` over N frames.
- Hardware: by-eye confirmation plus a photo into `transcript_hw_evkb.txt`.

---

## 8. Verification — what each gate can honestly prove

| Claim | QEMU | Hardware |
|---|---|---|
| Bring-up sequence well-formed and correctly ordered (video mux, PGMC power, GPR62 resets straddling D-PHY config) | ✅ enforced — D-PHY LOCK reads 0 unless PCLK+ESC resets are deasserted | implied by later stages |
| HX8394 init contract: EXTC unlock first, exit-sleep before display-on, full table sent | ✅ modelled (§6.5) | ✗ silent |
| **Panel lane count agrees with DSI host lane count** | ✅ `SETMIPI[1] & 3` vs `CFG_NUM_LANES` | ✗ silent — a mismatch just gives a blank screen with no diagnostic |
| Framebuffer holds the expected pattern (`FB_SUM`) | ✅ | ✅ — **must agree** |
| Pixel stream actually reaching the panel (`PANEL_SUM`) | ✅ virtual tap | ✗ **`TAP_ABSENT`** — no such register exists on silicon |
| D-PHY actually locks at 792 MHz; HS timing legal | ✗ **no analog model** | ✅ **only oracle** |
| **No FIFO underrun at 108 MB/s** (`UNDERRUN`) | ✗ **vacuous — QEMU has no timing model** | ✅ **only oracle** |
| HX8394 silicon accepts the command table | ✗ | ✅ |
| Correct colour, orientation, geometry | ✗ | ✅ by eye |

Two tokens are deliberately asymmetric, and the gate output says so in these words:

- **`PANEL_SUM` is QEMU-only by construction** — it prints `TAP_ABSENT` on hardware.
- **`UNDERRUN` is hardware-only by construction** — a QEMU run reporting `UNDERRUN=0/10` proves
  **nothing** about bandwidth, because the model has no timing. The gate prints
  `UNDERRUN=0/10 (QEMU: vacuous)` so the transcript can never be misread as bandwidth evidence.

That second point is the RPi bring-up's most expensive lesson, encoded: there, the underrun
sampler was a temporary diagnostic that got stripped at wrap. For a panel with 2.4× the bandwidth
it is a permanent assertion.

Checksums are computed in software and never hard-coded. Gate run as `./run_qemu.sh` (never
`sh run_qemu.sh`). Hardware flashing per [[rt1170-evkb-flashing]] — `LinkServer flash … load` then
`verify` for this image, since at 1.8 MB of framebuffer-touching firmware the "`LinkServer run`
stayed attached" signal is not proof that programming finished.

---

## 9. License firewall

The HX8394 sequence comes from **BSD-3-Clause** `mcuxsdk/components/video/display/hx8394/fsl_hx8394.c`
in the local SDK. Register values and orderings are *facts*, transcribed into our own Teensy-style
code with a fact-source attribution comment naming the file — the same discipline used for PXP,
SPI and Wire. **No GPL/LGPL source is consulted for this panel at all**, which is a strict
improvement on the RPi path (§185 of that spec records the TC358762 sequence having to come from
Linux). `tools/license-audit.sh` gains the renamed repo path in M0; the QEMU model stays in
`qemu2` (GPL) and never links into firmware.

---

## 10. Risks, with pre-designed responses

1. **`UNDERRUN` non-zero at 240 MHz.** The escalation is *not* another bus bump — 240 MHz is
   NXP's own `BOARD_BootClockRUN` value and the practical ceiling for this root. The responses, in
   order: (a) confirm the framebuffer is in the non-cacheable SDRAM region and 32-byte aligned;
   (b) drop to `kDSI_DpiNonBurstWithSyncEvent`; (c) accept a lower frame rate via a larger ROOT70
   divide (div 10 → 52.8 MHz → 52.8 Hz, ~97 MB/s). Never weaken the gate to hide it.
2. **`DSI_OK` fails on silicon at 792 MHz.** Isolated in M1. Attack via the D-PHY divider search
   and the HS timing derivation; the RPi work's 744 MHz/1-lane lock is the known-good reference
   point to bisect from.
3. **The 240 MHz bus change regresses an unrelated gate.** Caught in M0's full gate re-run, before
   any panel work exists to confuse the diagnosis. PIT/IntervalTimer derive their divisors at
   runtime and adapt; anything bus-timing-sensitive surfaces there.
4. **`J48` and `J84` share LPI2C5** (`GPIO_LPSR_04/05`). Harmless for v1; a real constraint for v2
   touch if both panels stay plugged in. Also note the stuck-BBF trap: a wedged LPI2C5 bus needs a
   **physical power-cycle**, not a debugger reset.
5. **The HX8394 table is right but the panel still shows nothing.** Unlike the RPi case there is a
   usable reference: NXP's own `lcdifv2/rgb565` demo defaults to this exact panel
   (`USE_MIPI_PANEL = MIPI_PANEL_RK055MHD091`), so building and flashing it is a **direct
   known-good comparison** — the thing that was unavailable for the RPi panel. Named here so it is
   reached for early rather than after the register-diff avenue is exhausted.

---

## 11. Roadmap after v1

- **v2** — capacitive touch over LPI2C5/Wire2, with `CTP_RST_B`/`CTP_INT`.
- **v3** — LVGL `lv_indev` binding; `lvgl_rk055_panel_test` alongside the existing examples.
- **v4** — double buffering and page flip on vsync; PXP-accelerated drawing; XRGB8888.
- **Possible** — `LV_USE_DRAW_PXP` (NXP ships an LVGL PXP draw backend that pairs with our
  HW-verified `newdigate/PXP`); CM4-owned display.

---

## Appendix A — summary for an engineer with zero context

The board is an NXP MIMXRT1170-EVKB. The RK055HDMIPI4MA0 is a 720×1280 portrait LCD that speaks
MIPI-DSI natively and is driven by an HX8394 controller chip on the panel itself. It plugs into
`J48`, a 40-pin FPC connector NXP put on the board specifically for it.

To show a picture: put pixels in SDRAM; the **LCDIFv2** block reads them out continuously at
video timing; the **MIPI-DSI host** serialises that stream over two high-speed differential pairs;
the HX8394 turns it back into panel drive. Before any of that works, the HX8394 must be told what
kind of signal to expect — a fixed sequence of 25 command packets sent over the same DSI link.

Three clocks matter: the **pixel clock** (58.7 MHz, how fast pixels leave the LCDIFv2), the
**D-PHY bit clock** (792 MHz, how fast bits leave the SoC), and the **bus clock** (240 MHz, how
fast the LCDIFv2 can *fetch* from SDRAM). The last one is why this panel is harder than the
previous one: it needs 2.4× the memory bandwidth, and if the fetch can't keep up the output FIFO
starves and the panel shows garbage or nothing.

Everything is verified twice: once in a QEMU model of this board (which proves the *sequence* is
right but knows nothing about analog timing), and once on the real board (which is the only thing
that can prove the high-speed link actually locks and the memory keeps up). Where a check is only
meaningful on one of the two, the spec says so explicitly rather than letting a green QEMU run
imply more than it proved.
