# RT1176 USB Host — Sub-project A: controller + HID (keyboard + mouse) — Design Spec

**Date:** 2026-07-08
**Status:** Approved (design)
**Library:** `newdigate/USBHost_t36` (port) · `newdigate/teensy-cores` (core regs) · `qemu2` (model) · `evkb` (gate)

## Goal

The RT1176 MIMXRT1170-EVKB acts as a **USB host**: a real USB keyboard and mouse
plugged into **J47 (USB_OTG2)** enumerate and deliver input, verified in QEMU and on
hardware.

## Decomposition (this spec = Sub-project A)

The user-approved scope is **keyboard + mouse + mass storage**, delivered as two
focused specs built back-to-back:

- **Sub-project A (this spec):** USB host controller bring-up (OTG2/PHY2, host mode)
  + HID (keyboard + mouse). All the chip-specific risk lives here; once the
  controller enumerates one device, the class-driver model is generic.
- **Sub-project B (separate spec, later):** USB mass storage — the MSC BOT/SCSI
  block driver (`USBDrive`) mounted as a `USBFilesystem`, which rides the **existing
  SdFat + `FS.h` stack** we built and HW-verified for the SD card. Gets its own
  spec→plan→implement cycle once A lands on hardware.

Rationale for the split: B stacks a filesystem layer that only makes sense once the
controller works, and reuses a different subsystem (SD/FS). Two focused specs → two
tight plans; A de-risks B entirely.

## Architecture

One hardware file, the rest generic — the same shape the Audio library decomposed into.

```
  firmware: KeyboardController / MouseController + Serial1 (LPUART) markers
     │
  USBHIDParser + keyboard.cpp + mouse.cpp     ← HID class drivers (generic)
  enumeration.cpp + hub.cpp + memory.cpp      ← USB core (generic)
     │
  ehci.cpp  USBHost::begin()                  ← THE hardware file (new __IMXRT1176__ branch)
     │
  core imxrt1176.h : USB_OTG2 + USBPHY2 + USBHS_* aliases + IRQ_USB_OTG2
     │
  silicon: USB_OTG2 @0x4042C000 (host)  ·  USBPHY2 @0x40438000
```

**Coexistence with the device controller:** OTG1 stays the USB *device* controller
(SerialUSB); OTG2 becomes host. Independent controllers, independent PHYs; the only
shared resource is the 480 MHz USB PLL. The host `begin()` does a **self-contained
PHY2 + PLL bring-up** so it works standalone with the device side uninitialized (and
is idempotent if the PLL turns out to be shared). Gate markers go over **Serial1
(LPUART1 VCOM)** — so the host gate has no dependency on USB-device/CDC.

## Hardware configuration

| Item | Value | Source |
|---|---|---|
| Host controller | **USB_OTG2 @ 0x4042C000** | qemu2 `fsl-imxrt1170.c:187`; matches OTG1 device layout |
| Host IRQ | **IRQ_USB_OTG2 = 135** | qemu2 `fsl-imxrt1170.c:190` |
| Host PHY | **USBPHY2 @ 0x40438000** | qemu2 `fsl-imxrt1170.c:193` |
| Device controller (existing) | USB_OTG1 @ 0x40430000, USBPHY1 @ 0x40434000, IRQ 136 | core `imxrt1176.h`, `usb.c` |
| USB PLL | 480 MHz via `USBPHYn_PLL_SIC` (`DIV_SEL(3)`, 24→480) | device `usb.c:usb_pll_phy_init` |
| USB clock gate | `CCM_LPCG115_DIRECT = 1` (confirm whether OTG2/PHY2 share LPCG115) | device `usb.c:105` |
| Host port | **J47** on the EVKB | board |
| VBUS | assumed dedicated hardware rail, no software switch — **⚠ verify for the 1170-EVKB** (the existing note is for the *1060*-EVKB) | risk #2 |

## Component design

### C1. `ehci.cpp` — new `__IMXRT1176__` branch in `USBHost::begin()`

Inserted alongside the existing `#elif defined(__IMXRT1052__) || defined(__IMXRT1062__)`
branch. Two steps, then falls through to the **unchanged** generic EHCI setup
(ehci.cpp:223–287). **No MPU code is needed** — the RT1176 core `startup.c` already
configures the M7 MPU to grant DMA masters OCRAM access (proven by eDMA/SAI/USDHC); the
`MPU_RGDAAC0 |= 0x30000000` in upstream `begin()` is the *Teensy-3.6 (Kinetis)* branch
only, not the 1062 path (O3 resolved).

1. **PHY2 + PLL power-up** — a `usbphy2_pll_init()` mirroring the device side's proven
   `usb_pll_phy_init()`, retargeted to PHY2:
   - ungate the USB LPCG (`CCM_LPCG115_DIRECT = 1`)
   - `USBPHY2_PLL_SIC` bring-up: `PLL_REG_ENABLE` (0x200000), `PLL_POWER` (0x1000),
     `PLL_DIV_SEL(3)` (24→480 MHz), clear `PLL_BYPASS` (0x10000), `PLL_EN_USB_CLKS`
     (0x40), clear `CLKGATE`
   - bounded lock-wait (≤~100 iterations, "no infinite spin"; QEMU doesn't model the
     PHY-PLL lock — HW arbiter)
   - `USBPHY2_CTRL_CLR = CLKGATE`, `USBPHY2_PWD = 0`
2. **Fall through to the generic sequence** (unchanged): `USBHS_USBCMD` reset,
   `USBHS_USB_SBUSCFG = 1`, **`USBHS_USBMODE = USBHS_USBMODE_CM(3)`** (host — vs the
   device path's `CM(2)`), `USBHS_PERIODICLISTBASE`/`FRINDEX`/`ASYNCLISTADDR`, start
   (`USBHS_USBCMD` RS + schedule enables), `USBHS_PORTSC1 |= USBHS_PORTSC_PP` (port
   power), `attachInterruptVector(IRQ_USBHS, isr)` + `NVIC_ENABLE_IRQ`, `USBHS_USBINTR`.

The entire trick is that the `USBHS_*` macros in steps 1–3 must resolve to OTG2 — a
core-header job (C2). No other change to `ehci.cpp`'s logic.

### C2. Core `imxrt1176.h` additions (generator `gen_imxrt1176_h.py` + regen)

`imxrt1176.h` is auto-generated — edit the generator AND regenerate; never hand-edit.

- `USB_OTG2_BASE 0x4042C000` + the **`USBHS_*` alias set** the generic driver expects,
  at the standard i.MX USB operational offsets (same offsets as the existing OTG1
  `USB1_*` set, based at OTG2):
  `USBHS_USBCMD` @0x140, `USBHS_USBSTS` @0x144, `USBHS_USBINTR` @0x148,
  `USBHS_FRINDEX` @0x14C, `USBHS_PERIODICLISTBASE` @0x154, `USBHS_ASYNCLISTADDR` @0x158,
  `USBHS_BURSTSIZE` @0x160, `USBHS_USB_SBUSCFG` @0x90, `USBHS_PORTSC1` @0x184,
  `USBHS_USBMODE` @0x1A8, `USBHS_GPTIMERnCTL/LD` as referenced. Include the
  `USBHS_USBCMD_*`, `USBHS_USBMODE_*`, `USBHS_PORTSC_*`, `USBHS_USBINTR_*`,
  `USBHS_USBSTS_*` bit macros the driver uses (port from the 1062 `imxrt.h`).
- `USBPHY2_BASE 0x40438000` + `USBPHY2_CTRL/_SET/_CLR`, `USBPHY2_PWD`,
  `USBPHY2_PLL_SIC/_SET/_CLR` — a straight mirror of the existing `USBPHY1_*` block;
  the `USBPHY_*` and `USBPHY_PLL_SIC_*` bit macros are already shared/present.
- `IRQ_USB_OTG2 = 135`.
- `MPU_RGDAAC0` if not already present.

### C3. DMA memory placement — the trap QEMU will not catch

`periodictable` (`ehci.cpp:64`, `aligned(4096)`) and the seed pools
`memory_Device/Pipe/Transfer` (`memory.cpp:60–62`) are plain `static` → `.bss`. On
Teensy 4 `.bss` is OCRAM (DMA-reachable, so upstream never noticed); on **our core
`.bss` is DTCM**, which the USB DMA master **cannot reach**.

- Add **`DMAMEM`** to `periodictable` and the seed pools.
- **Audit every DMA-touched static buffer** across the compiled drivers
  (`enumeration.cpp`, `hid.cpp`, `keyboard.cpp`, `mouse.cpp` each contribute their own
  `Pipe_t`/`Transfer_t`/report buffers via `contribute_*`) — all must land in OCRAM.
- **QEMU models DTCM as DMA-reachable → it will false-pass.** Hardware is the arbiter,
  same lesson as the SerialUSB dTDs, eDMA, and SAI buffers. This is the single
  highest-risk correctness item in A (risk #1); called out explicitly in the plan and
  the HW-verification step.

### C4. Driver layer (compiled, generic — unchanged)

Compiled: `ehci.cpp` (+ our branch), `enumeration.cpp`, `hub.cpp`, `memory.cpp`,
`hid.cpp`, `keyboard.cpp`, `mouse.cpp`, `USBHost_t36.h`. The other ~12 drivers stay
unlinked.

- `USBHost_t36.h` *declares* the storage classes and `#include`s `<FS.h>`, but those
  are declaration-only — not compiling `MassStorageDriver.cpp` keeps the storage/SdFat
  stack out of A, and `<FS.h>` resolves from the core (present from the SD work).
- Claim model (platform-independent): enumeration offers each device/interface to every
  registered driver's `claim()`; `USBHIDParser` claims HID interfaces, parses the report
  descriptor, and feeds parsed fields to `KeyboardController`/`MouseController`.

### C5. Gate/demo firmware (`evkb/usb_host_hid_test/`)

Canonical Teensy USBHost wiring, with one deliberate divergence: **markers over
`Serial1` (LPUART1 VCOM), not USB CDC**.

```cpp
#include "USBHost_t36.h"
USBHost myusb;
USBHIDParser  hid1(myusb);
KeyboardController keyboard1(myusb);
MouseController    mouse1(myusb);

void onPress(int u) { Serial1.printf("KEY=%d\n", u); }

void setup() {
  Serial1.begin(115200);                 // VCOM markers, NOT Serial/USB-CDC
  myusb.begin();                         // <- our __IMXRT1176__ host bring-up
  keyboard1.attachPress(onPress);
  Serial1.println("USB_HOST_BEGIN");
}
void loop() {
  myusb.Task();
  if (mouse1.available()) {
    Serial1.printf("MOUSE=%d,%d,%02x\n",
                   mouse1.getMouseX(), mouse1.getMouseY(), mouse1.getButtons());
    mouse1.mouseDataClear();
  }
}
```

Markers the runner greps (PASS = all four):
- `USB_HOST_BEGIN` — controller init returned (no hang in PHY/PLL bring-up)
- `KBD_CONNECT=vid:pid` / `MOUSE_CONNECT=vid:pid` — both devices enumerated (VID/PID
  printed from a connect hook — exact API is open question O2)
- `KEY=…` — an injected keystroke reached the HID→keyboard path
- `MOUSE=dx,dy,btn` — an injected motion reached the HID→mouse path

## Test strategy

### QEMU gate (`evkb/usb_host_hid_test/`) — first gate to drive qemu2's USB *host* path

- **Attach virtual devices to OTG2's bus:** `-device usb-kbd,bus=…` + `-device
  usb-mouse,bus=…`. `chipidea` inherits `TYPE_SYS_BUS_EHCI` (`chipidea.c:285`), so
  realizing it creates a USBBus; give OTG2's bus a stable id in the machine model so
  `-device` can target it (small machine tweak).
- **Two-phase gate:** (1) *enumeration* — assert `USB_HOST_BEGIN` +
  `KBD_CONNECT`/`MOUSE_CONNECT` (proves controller + schedule + enumeration + HID
  parse — the hard part); (2) *input* — inject a keystroke + mouse motion, assert
  `KEY=`/`MOUSE=`.
- **Input injection is new tooling:** prior gates were passive `-chardev` taps; this
  needs a control channel (QEMU monitor `sendkey` / QMP `input-send-event`). Modest,
  flagged as the one novel harness piece. Enumeration markers remain the primary QEMU
  proof even if injection proves fiddly.
- **Expect host-mode model fixes:** host mode is coded but never exercised
  (`chipidea.c:114–123` delegates host-mode MMIO to EHCI). Likely 1–3 mirror-silicon
  fixes in the chipidea↔EHCI glue (e.g. the device-op overlay at `0x140–0x1DC` added at
  priority 1 in `chipidea_realize` must fully yield to EHCI when `USBMODE.CM=host`;
  PORTSC connect/reset routing). The generic EHCI core underneath is mature.
- **Likely needs `-icount shift=auto`** for deterministic enumeration timing (reset
  debounce, control-transfer pacing) — same lever as the PIT/tone/SD-WAV gates.
- Reuse `qrun` + `gate-lib.sh` lifecycle safety net; markers over Serial1 (LPUART1) as
  every gate.

### Hardware verification (the arbiter)

Flash via LinkServer, VCOM @115200; plug a **real USB keyboard + mouse into J47**;
confirm `USB_HOST_BEGIN`, both `*_CONNECT` lines with the real VID/PIDs, live `KEY=` on
typing and `MOUSE=` on movement. This is where the DMAMEM audit (risk #1) and the VBUS
question (risk #2) are truly settled.

## Risk register

| # | Risk | Sev | Mitigation |
|---|---|---|---|
| 1 | **DMAMEM audit miss** — a DMA-touched USB struct left in DTCM | **High** | Audit + `DMAMEM` all EHCI/pool/driver statics; QEMU false-passes → HW arbiter (SerialUSB/eDMA lesson) |
| 2 | **J47 VBUS on the 1170-EVKB** — the "no software switch" note was for the *1060*-EVKB | **Med** | Verify 1170-EVKB schematic; add a VBUS-enable GPIO hook (SD-PWREN pattern) if J47 needs one |
| 3 | **QEMU host-mode gaps** — never driven before | **Med** | Drive-and-fix, mirror silicon; confined to the chipidea/EHCI glue |
| 4 | **USB PLL sharing / PHY2 specifics** — shared 480 MHz PLL vs per-PHY? | Low–Med | Self-contained PHY2+PLL bring-up (idempotent if shared); confirm vs RM |
| 5 | **Input-injection harness** — new control channel | Low | Small monitor/QMP script; enumeration is the core proof regardless |

## Open questions (resolve during planning/implementation)

- **O1.** USB LPCG index for OTG2/PHY2 — shared `LPCG115` with the device side, or a
  separate gate?
- **O2.** Exact connect-hook API to print a device's VID/PID (iterate `myusb` drivers /
  a claim hook / `USBDriver` device pointer).
- **O3. RESOLVED** — no MPU code needed: the RT1176 core `startup.c` already grants DMA
  masters OCRAM access (`MPU_RGDAAC0` in upstream `begin()` is Kinetis-only).

## Repos & files touched

| Repo | Change |
|---|---|
| `newdigate/teensy-cores` (`evkb/cores`) | `tools/gen_imxrt1176_h.py` + regen `imxrt1176.h`: `USB2_*` (OTG2) + `USBPHY2_*` + generic `USB_*` bit macros + `IRQ_USB_OTG2=135` |
| `newdigate/USBHost_t36` | `utility/imxrt_usbhs.h`: widen `USBHS_*` aliases to `__IMXRT1176__`; `ehci.cpp`: new `__IMXRT1176__` branch in `begin()`; `DMAMEM` on `periodictable` + `memory.cpp` pools + driver-buffer audit |
| `qemu2` (gitlab) | machine tweak to name OTG2's USB bus; expected host-mode fixes to the chipidea/EHCI glue as the gate surfaces them |
| `evkb` (local) | new gate `usb_host_hid_test/` (firmware + CMakeLists + runner + checker + input-injection script) |

## References

- Device-side USB bring-up (PHY1/PLL, the mirror source): memory `rt1176-serialusb`
- DMAMEM/OCRAM reachability lessons: memory `rt1176-edma-dmachannel`, `rt1176-serialusb`
- Gate infra: memory `rt1170-qemu` (qrun), `rt1170-gate-lib`
- SD/FS foundation reused by Sub-project B: memory `rt1176-sd-usdhc`
- Repo boundaries / shared tree: memory `rt1170-evkb-git-repo`
- Flashing + serial capture: memory `rt1170-evkb-flashing`, `macos-serial-capture`
- qemu2 model anchors: `hw/arm/fsl-imxrt1170.c` (USB base/IRQ/PHY tables),
  `hw/usb/chipidea.c` (`TYPE_SYS_BUS_EHCI` parent, host-mode delegation)
