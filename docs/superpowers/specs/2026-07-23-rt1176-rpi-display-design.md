# RT1176 → Raspberry Pi 7" Touch Display (v1: "solid color lit") — Design

**Date:** 2026-07-23
**Status:** design approved (forks resolved), pending user spec review
**Target board:** MIMXRT1170-EVKB (i.MX RT1176), CM7 only
**Predecessor:** builds directly on [PXP](2026-07-22-rt1176-pxp-design.md) (framebuffer paint), SDRAM/SEMC, Wire/LPI2C, and the RT1176 PLL bring-up pattern (Ethernet SysPll1).

---

## 1. Goal

Light the Raspberry Pi 7" Touchscreen Display (v1.1) with a **single solid color**,
end-to-end, on real EVKB silicon **and** in QEMU — the smallest vertical slice that
proves the entire MIPI-DSI display chain. One coherent milestone; everything richer
is deferred (§10).

"Lit" for v1 means: call `Display.begin()` then `Display.fillScreen(color)` and see
that color fill the 800×480 panel — and see the same color in the QEMU display window.

## 2. Scope

**In scope (v1):**
- ATtiny88 power-on + backlight over LPI2C1 (the gate that makes the panel live at all).
- VIDEO_PLL / display clock bring-up for the ~25.98 MHz pixel clock + DSI D-PHY bit clock.
- LCDIFv2 driver: configure one layer, scan a single RGB565 framebuffer out of SDRAM.
- MIPI DSI host bring-up: D-PHY + DPI **video mode** enable (continuous pixel stream).
- TC358762 DSI→DPI bridge init sequence (DCS/generic writes via the DSI APB packet interface).
- Paint the solid color into the SDRAM framebuffer by **reusing `PXP.fill()`** (already HW-verified).
- **Full DSI-transport QEMU model** (Option A, high-fidelity fork): LCDIFv2 scanout to a
  visible window, a MIPI DSI host model, a virtual TC358762 that only accepts the video
  stream after the correct power + init sequence, and a virtual ATtiny88 on LPI2C1.

**Explicitly out of scope (deferred to later specs — §10):**
Changing/animated content, double buffering, page flip, test patterns, FT5406 touch,
DSI command/DBI-mode pixel path, LVGL, any second panel. No new public API beyond
`begin()` + `fillScreen()`.

## 3. Hardware facts (authoritative, from the local RT1176 device header)

| Block | Base | IRQ | Notes |
|---|---|---|---|
| **LCDIFv2** | `0x40808000` | **55** (`LCDIFv2_IRQn`) | framebuffer scanout engine; drives the DSI DPI input |
| **MIPI DSI host** (`DSI_HOST`) | `0x4080C000` | **59** (`MIPI_DSI_IRQn`) | 4 sub-blocks (below) |
| eLCDIF (legacy, *not used*) | `0x40804000` | 54 | QEMU currently reuses `imx6ul_lcdif` here; unrelated to v1 |
| SDRAM (framebuffer) | `0x80000000` | — | 64 MB SEMC, HW-verified; `extmem_malloc` |
| LPI2C1 (ATtiny88) | (Wire lib) | — | ATtiny @ I2C **0x45** |
| PXP | `0x40814000` | 57 | reused to paint the framebuffer |

**MIPI DSI host is a 4-sub-block design** (NXP FDSOI28 MIPI DSI host, matching `fsl_mipi_dsi.c`):
- `DSI_HOST` — core host config (D-PHY interface enable, PLL, packet handling).
- `DSI_HOST_DPI_INTFC` — the DPI (video) interface: resolution, timing, color coding, polarity.
- `DSI_HOST_NXP_FDSOI28_DPHY_INTFC` — the D-PHY: PLL, lane count, HS timing.
- `DSI_HOST_APB_PKT_IF` — the APB packet interface: sends DCS/generic short+long packets
  (this is the channel the TC358762 init writes travel over).

**Panel timing** (RPi 7" v1.1, from the Linux modeline): 800×480, pixel clock ≈ 25.979 MHz,
htotal 849 (hsync 801–803), vtotal 510 (vsync 487–489). One DSI data lane suffices (RT1176 supports 2).

**Framebuffer size (v1, RGB565):** 800 × 480 × 2 = **768,000 bytes** per buffer, in SDRAM. Single buffer.

## 4. Data path

```
                      ┌──────────── PXP.fill(color) paints the buffer ────────────┐
                      ▼                                                            │
   SDRAM framebuffer (RGB565, 0x80000000+) ─▶ LCDIFv2 (layer scanout, video timing)
        ─▶ MIPI DSI host: DPI_INTFC (video mode) + D-PHY (HS lanes)
             ─▶[FPC / J84]▶ TC358762 (DSI→DPI bridge) ─▶ LCD panel (800×480)

   LPI2C1 ─▶ ATtiny88 @0x45 : power-on (REG_POWERON) then backlight (REG_BRIGHTNESS)
             (must happen FIRST — panel is black with no error otherwise)
```

**Why video mode, not command mode:** the TC358762 is a stateless DSI→DPI converter with
no frame memory, so it must be fed a **continuous DPI video stream** (DSI video mode —
`kDSI_DpiNonBurstWithSyncPulse`/`Burst`). DSI command/DBI mode is only for panels that hold
their own framebuffer; it is rejected for this bridge. Video mode also maps directly onto the
QEMU continuous-scanout model, so both gates use the same mental model.

## 5. Component decomposition

### 5.1 Core additions (`cores/imxrt1176/imxrt1176.h`) — register facts only
- LCDIFv2 register block (base `0x40808000`): `CTRL`/`DISP_PARA`/`DISP_SIZE`/`CTRLDESCLn`
  (layer control: buffer addr, pitch, format, enable), interrupt/status. SET/CLR/TOG aliases.
- MIPI `DSI_HOST` register block (base `0x4080C000`) across the 4 sub-interfaces.
- `IRQ_LCDIFV2 = 55`, `IRQ_MIPI_DSI = 59` in `core_pins.h` (mirror both CM7/CM4 tables).
- Display-clock (VIDEO_PLL / ANADIG) defines as needed for the pixel + D-PHY clocks.

  Register addresses and bit definitions are **facts** transcribed from the local NXP BSD SDK
  (`devices/RT/RT1170/periph/PERI_LCDIFV2.h`, `PERI_DSI_HOST*.h`) and the RM. No NXP code is copied.

### 5.2 New library `newdigate/RPiDisplay` (MIT) — mirrors the PXP pattern (registers in core, driver in sibling repo)
Internal files, each one responsibility:
- `lcdifv2.{h,cpp}` — LCDIFv2 driver: clock, one layer, RGB565 scanout of an SDRAM buffer, vsync.
- `mipi_dsi.{h,cpp}` — MIPI DSI host driver: D-PHY + PLL bring-up, DPI video-mode config, and a
  `dsiWrite()` (DCS/generic packet TX via the APB packet interface) used by the panel driver.
- `tc358762.{h,cpp}` — the bridge init sequence (a table of DSI register writes) + enable.
- `rpi_attiny.{h,cpp}` — ATtiny88 power-on + backlight over Wire (@0x45), ID read-back check.
- `Display.{h,cpp}` — thin facade: `begin()` runs the whole bring-up in order; `fillScreen(color)`
  paints via PXP and ensures a frame is scanned. `extern DisplayClass Display;`

**Driver sourcing (study-hybrid fork, like SPI/Wire):** register maps and bring-up sequences are
*studied* from the local NXP BSD-3-Clause SDK (`fsl_lcdifv2.c`, `fsl_mipi_dsi.c`, the SDK display
clock config) and, for the TC358762 + ATtiny only, from the RPi references (`tc358762.c`,
`panel-raspberrypi-touchscreen.c`, `rpi-panel-attiny-regulator.c` — GPL, read for **facts only**,
values reproduced, never code-copied — same discipline used studying `fsl_pxp.c` for PXP). The
resulting driver is our own Teensy-style code with our register logic. Attribution comment noting
the fact-source in each file. `tools/license-audit.sh` extended to cover the new repo + gate.

### 5.3 QEMU model (`qemu2`, GPL — firewalled, never into firmware) — full DSI transport (Option A)
- **LCDIFv2 model** @ `0x40808000`, IRQ 55: register state + real scanout. When the layer is enabled
  **and** the downstream (DSI→bridge→panel) reports ready, it runs `framebuffer_update_display` from
  the layer's SDRAM address to a QEMU graphic console window (RGB565→surface) — reusing the exact
  machinery `hw/display/imx6ul_lcdif.c` already uses. Vsync raises IRQ 55.
- **MIPI DSI host model** @ `0x4080C000`, IRQ 59: models the 4 sub-blocks enough to enforce ordering —
  D-PHY powered/PLL-locked, host powered, DPI video mode configured (resolution/format), and an APB
  packet interface that accepts DCS/generic writes and **forwards them to the virtual TC358762**.
- **Virtual TC358762**: a state machine consuming the forwarded init writes. Starts *not ready*;
  asserts `downstream_ready` (which LCDIFv2 checks before scanning out) only after (a) ATtiny power is on,
  (b) the required init register writes arrived, and (c) DPI video mode is enabled. It also computes an
  **FNV-1a checksum of the pixel stream it receives** and exposes it via a debug MMIO register — the
  self-validating gate's QEMU-side oracle (absent on real silicon).
- **Virtual ATtiny88** on the existing LPI2C1 model (`TYPE_IMXRT_LPI2C`, master+slave already done for Wire):
  an I2C device @0x45 that reports "powered" only after `REG_POWERON` is written, and returns the known
  `REG_ID` constant. The TC358762 model stays dark until ATtiny power is on — modeling the "#1 failure mode:
  black panel if ATtiny power isn't written first" as a **hard QEMU gate**, not a HW-only surprise.

This is the high-fidelity path you chose: a broken or mis-ordered bring-up sequence fails in QEMU
(window dark, checksum mismatch), not only on glass.

## 6. Public API (v1 — minimal, YAGNI)

```cpp
class DisplayClass {
public:
  bool begin();                       // ATtiny power → PLL → LCDIFv2 → DSI → TC358762; false on any failure
  void fillScreen(uint16_t rgb565);   // PXP.fill the SDRAM framebuffer, ensure one scanout frame
  uint16_t width()  const { return 800; }
  uint16_t height() const { return 480; }
  // v1 internal only: framebuffer pointer, brightness(uint8_t) may be exposed if trivial
};
extern DisplayClass Display;
```

`begin()` returns a bool (like `PXP.begin()`); a typed error enum is deferred unless bring-up
debugging needs it. Color is RGB565 to match the v1 framebuffer format.

**RGB565 ↔ PXP seam (explicit):** `PXP.fill()` takes a 24-bit RGB888 background color and converts
it to the output surface's format. So `fillScreen(uint16_t rgb565)` expands 565→888, then calls
`PXP.fill()` with the PXP **output surface = the RGB565 framebuffer** (PXP converts the 888 back to
565 on write). The expansion must be the standard bit-replication (`r5<<3|r5>>2`, etc.) so the
software-expected checksum in the gate matches PXP's on-chip conversion exactly — the plan pins this
conversion and the gate's reference uses the identical function.

## 7. Verification (two-gate, self-validating)

**Gate:** `evkb/examples/display/rpi_panel_test/` (`run_qemu.sh` via qrun/gate-lib.sh).

**Self-validating loop (QEMU):**
1. Software-compute the expected framebuffer for the chosen solid color (800×480 RGB565) and its FNV-1a checksum.
2. `Display.begin()` — full bring-up. Emit a token per stage (`ATTINY_OK`, `PLL_OK`, `LCDIFV2_OK`, `DSI_OK`, `TC358762_OK`) read unconditionally from status, so a failure localizes.
3. `Display.fillScreen(color)` (PXP paints SDRAM).
4. Checksum the SDRAM framebuffer directly → verifies PXP.fill (`FB_SUM`).
5. Read the **virtual TC358762 received-pixel checksum** debug register and compare to the expected checksum → `PANEL_SUM` PASS/FAIL. This closes the loop entirely inside QEMU: fill → LCDIFv2 scan → DSI → bridge tallies what it received → firmware compares.
6. The color is also **visibly** in the QEMU window (manual confirmation / screenshot).

**HW (EVKB):** the bridge checksum register reads a sentinel on real silicon (no such register), so the gate prints `HW: bridge tap absent — verify by eye` and the human confirms the **real 7" panel shows the color** + a photo into `transcript_hw_evkb.txt`. Silicon remains the oracle for the D-PHY/bridge handshake (as with the 1 kHz audio tone). Flash via LinkServer; pyserial console started before the run (per [[rt1170-evkb-flashing]], [[macos-serial-capture]]).

Checksums are computed in software, never hard-coded. QEMU and HW must agree on `FB_SUM`;
`PANEL_SUM` is QEMU-only by construction.

## 8. Clocking (the one genuinely new subsystem)

RT1176 uses the ANADIG fractional **PLL_VIDEO**. Two derived roots:
- **LCDIFv2 pixel clock** ≈ 25.98 MHz (849 × 510 × 60). Set the LCDIFv2 CLOCK_ROOT divider off VIDEO_PLL.
- **MIPI D-PHY bit clock** for the HS lane(s), sized for 800×480 × 24-bit DPI over the lane count, plus the DSI escape clock.

Exact PLL multipliers/dividers and D-PHY timing (`t_hs-prepare`, etc.) are transcribed from the SDK
display clock config + `fsl_mipi_dsi` D-PHY setup and **verified on HW** (a wrong D-PHY clock = no sync,
a HW-only symptom). This mirrors the Ethernet SysPll1 bring-up experience.

## 9. Risks / HW-only unknowns (silicon is the oracle)

1. **D-PHY bit clock + HS timing** — wrong values → the bridge never syncs; invisible in a thin model, so the QEMU model enforces *ordering/enablement*, not analog timing. HW confirms lock. (Tall pole.)
2. **Exact TC358762 init sequence** — not in the local BSD SDK (only RK055 panels are). Transcribed from the RPi Linux/appcodehub references (facts). Wrong order/values → dark panel; debugged on HW.
3. **ATtiny register map** — the reference doc gives `REG_POWERON`, `REG_BRIGHTNESS`, `REG_ID`; exact values/addresses confirmed against `rpi-panel-attiny-regulator.c` (facts) and the HW `REG_ID` read-back.
4. **v1.1 backlight PWM** — full 0–255 range on v1.1; confirmed on the actual board.

None of these block the *design*; each is an implementation fact to pin from a cited source and verify on silicon.

## 10. Roadmap (later specs — decomposed like Ethernet m1→m2→m3)

- **v2** — dynamic/animated content: double buffer, page flip on vsync, PXP blits, test patterns.
- **v3** — FT5406 capacitive touch via the ATtiny relay (polled/IRQ).
- **v4** — richer formats (XRGB8888 framebuffer), the generic LCDIFv2/MIPI-DSI core factored out for reuse by other DSI panels.
- **v5** — LVGL (or a minimal graphics API) on top.
- Possible: DSI command/DBI-mode path for framebuffer-holding panels; CM4-owned display.

## 11. License firewall (standing constraint)

- New library `newdigate/RPiDisplay`: **MIT**.
- Studied for facts: NXP MCUXpresso SDK (`fsl_lcdifv2.c`, `fsl_mipi_dsi.c`, clock config) — **BSD-3-Clause**, permissive; RPi Linux drivers — **GPL, facts-only** (register values reproduced, no code copied).
- qemu2 model — **GPL-2.0**, one-way firewall: never imported into any firmware repo.
- `tools/license-audit.sh` gains the new repo + gate path; must PASS before push.

---

### Appendix A — architecture summary for an engineer with zero context

You are lighting a specific 7" panel. It is **not** a native DSI panel — a Toshiba TC358762 chip on
the display board converts MIPI-DSI into the parallel RGB (DPI) bus the LCD actually wants, and an
ATtiny88 microcontroller controls panel power + backlight over I2C. So the RT1176 must: (1) turn the
panel on by writing the ATtiny over I2C, (2) generate a video-timed pixel stream from an SDRAM
framebuffer using the LCDIFv2 block, (3) push that stream over MIPI-DSI in *video mode* through its
D-PHY, and (4) send the TC358762 its init register writes over DSI so it starts converting. Get any
step wrong or out of order and the panel is simply black with no error — which is exactly why the
QEMU model for this milestone is built to reproduce that ordering as a hard, checksummed gate.

---

## Amendment 1 (2026-07-24) — the TC358762 register map is NOT settled

**Correction.** During Task 11 an implementer was told the bridge's key registers were listed in
§3 of this spec. **They are not** — §3 is the block/base/IRQ table and contains no TC358762
registers. The list in question (`0x0004` SYSCTL, `0x0006` SYSPMCTRL, `0x0110` PPICNT,
`0x0210` DSICMD, `0x0400` VSDLY, `0x0404` VFMTHI, `0x0408`) comes from the project's
**display reference document**, not from this spec. Recorded here so no later task re-derives a
false provenance.

**The substantive finding.** There are **two published TC358762 register maps and they disagree**:

| Source | Names |
|---|---|
| The reference doc | `0x0004` SYSCTL, `0x0006` SYSPMCTRL, `0x0110` PPICNT, `0x0210` DSICMD, `0x0400`/`0x0404`/`0x0408` video |
| The RPi/Linux-derived map | `0x0104` PPI_STARTPPI, `0x0114` LPTXTIMECNT, `0x0164` D0S_CLRSIPOCOUNT, `0x0204` DSI_STARTDSI, `0x0210` DSI_LANEENABLE, `0x0420` LCDCTRL, `0x0450` SPICMR, `0x0464` SYSCTRL |

Only two things are common to both: the **regions** (`0x0100–0x02FF` = PPI/DSI receive layer;
`0x0400–0x04FF` = video/DPI + system) and the **ordering** (receive layer configured before the
video path, then a final start).

**Consequence for the QEMU bridge (Task 11).** The virtual TC358762's required-init contract is
deliberately **region-based, not literal-address-based**: it requires ≥1 write in `0x0100–0x02FF`,
≥1 write in `0x0400–0x04FF`, the former before the latter, and ≥1 further write after. Pinning a
literal address that neither published map agrees on would have forced the firmware to emit a
**fabricated** register write to satisfy the emulator — precisely the failure mode the plan's
convention #2 exists to prevent. Region 2 is broader than its "video" label (it also spans SPI,
PLL/SYSPMCTRL, GPIO and chip-ID), so a sequence touching those *before* its first PPI write could
be wrongly rejected; the canonical sequence below does not, and the model — not the firmware — is
what changes if a genuine sequence ever trips it.

**Task 12's target — the canonical RPi bring-up order** (verified against the Task-11 FSM as
satisfying all four conditions with margin: lane-seq 1, video-seq 7, post-video writes 4):

```
DSI_LANEENABLE 0x0210 → PPI 0x0164, 0x0168, 0x0144, 0x0148, 0x0114
  → SPICMR 0x0450 → LCDCTRL 0x0420 → SYSCTRL 0x0464
  → PPI_STARTPPI 0x0104 → DSI_STARTDSI 0x0204
```

Task 12 transcribes the **real** sequence (values included) from the RPi references and sends it as
DSI generic long writes (`dsiWrite(0, 0x29, {addr_lo, addr_hi, data…})`). It must NOT be trimmed to
the model's minimum, nor padded to satisfy the model. **Silicon (Task 14) remains the oracle** for
both the register addresses and their data.
