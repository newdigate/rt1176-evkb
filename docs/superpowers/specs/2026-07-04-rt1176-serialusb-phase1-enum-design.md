# RT1176 SerialUSB — Phase 1: USB device controller + CDC enumeration — Design

**Status:** approved (brainstorming) — ready for implementation plan
**Date:** 2026-07-04
**Target:** `cores/imxrt1176` Arduino core for the MIMXRT1176-EVKB. QEMU-gated (the ChipIdea device model + CDC virtual host enumerate the guest) **and** hardware-verified (the Mac enumerates a `/dev/cu.usbmodem…` CDC port).

**Decomposition note:** This is **Phase 1 of 2** for `SerialUSB`. Phase 1 = the device **enumerates** as CDC-ACM (reaches "configured"). Phase 2 (separate spec) = bulk RX/TX data + the `Serial : Stream` API (echo works). Porting the compact Teensy 4 stack (`cores/teensy4/usb.c` / `usb_desc.c`), ~95% portable (same ChipIdea/EHCI USB IP).

## Goal

Bring up **USB_OTG1** as a USB CDC-ACM device that completes enumeration against a host: PLL/PHY up, controller in device mode, EP0 control transfers answered (`GET_DESCRIPTOR`/`SET_ADDRESS`/`SET_CONFIGURATION` + CDC class requests stored), CDC descriptors presented. Success = `usb_configuration != 0`.

## Scope

**In:** PLL_USB1 (480 MHz) + USBPHY1 bring-up; USB clock gate; ChipIdea controller device-mode init; dQH/dTD scaffolding; `usb_isr` SETUP detection + EP0 control-transfer engine; `endpoint0_setup` handling `SET_ADDRESS`, `GET_DESCRIPTOR`, `SET_CONFIGURATION`, and CDC `SET_LINE_CODING`/`GET_LINE_CODING`/`SET_CONTROL_LINE_STATE` (**store only**); the full CDC descriptor set (device, config w/ IAD + comm iface + interrupt EP + data iface + 2 bulk EPs, strings). Configure (but don't yet service) the CDC bulk/interrupt endpoints so the config is complete.

**Out (YAGNI → Phase 2 or never):** bulk RX/TX data movement, ring buffers, `Serial` read/write/available/flush API; the `usb_serial_class`; USB host/OTG mode; USB_OTG2; suspend/resume/wakeup/remote-wakeup; composite devices; the Teensy HalfKay bootloader/reboot hooks.

## Hardware facts (RT1176 USB — cm7 header + Teensy reference + QEMU model, all agreed)

- **Controller:** USB_OTG1 @ `0x40430000`; non-core USBNC_OTG1 @ `0x40430200`; PHY USBPHY1 @ `0x40434000`. **IRQ `USB_OTG1_IRQn = 136`.** ChipIdea/EHCI device-mode IP (same as imxrt1062, so Teensy `usb.c` ports directly).
- **Registers** (offsets from USB_OTG1 base, per Teensy `usb.c`): `USBCMD`, `USBSTS`, `USBINTR`, `DEVICEADDR`, `ENDPTLISTADDR`, `USBMODE`, `ENDPTSETUPSTAT`, `ENDPTPRIME`, `ENDPTFLUSH`, `ENDPTSTATUS`, `ENDPTCOMPLETE`, `ENDPTCTRL0..n`. Device mode = `USBMODE.CM=2 | SLOM`; run = `USBCMD.RS`; reset = `USBCMD.RST`.
- **dQH / dTD:** `endpoint_queue_head[(NUM_ENDPOINTS+1)*2]` (RX then TX per EP), **4 KB-aligned**; each dQH holds config (max-packet, IOS), overlay, and the 8-byte SETUP buffer. dTDs 32-byte-aligned, linked list, `status` active bit `0x80`. EP0 dQH config = `(64<<16) | (1<<15)` (64 B, interrupt-on-setup); EP0-TX = `(64<<16)`.
- **Clock/PLL (new — not yet in startup):** PLL_USB1 → **480 MHz** via ANADIG (`ANADIG_PLL` block, same idiom as the ARM PLL at `cores/imxrt1176/startup.c:271`); PHY init: `USBPHY1_CTRL` set `ENUTMILEVEL2|ENUTMILEVEL3`, `USBPHY1_PWD = 0`; USB clock gate (CCM LPCG for USB — number resolved in plan).
- **Endpoints (Teensy CDC layout):** EP0 control 64 B; EP2 interrupt-IN `0x82` (ACM notify, 16 B); EP3 bulk-OUT `0x03`; EP4 bulk-IN `0x84`. Bulk 512 B (HS) / 64 B (FS).
- **Descriptors:** device `bcdUSB=0x0200`, class `0xEF`/sub `0x02`/proto `0x01` (misc/IAD), `bMaxPacketSize0=64`; config has IAD (CDC-ACM, 2 ifaces) + comm iface (`0x02`/`0x02`/`0x01`, 1 interrupt EP) + CDC functional descriptors (header/call-mgmt/ACM/union) + data iface (`0x0A`, 2 bulk EPs). **VID/PID = `0x1209`/`0x0001`** (pid.codes generic test IDs — placeholder, not PJRC/NXP; revisit before any distribution).

## Architecture & files

```
cores/imxrt1176/               (core repo → github teensy-cores)
  usb.c        — usb_init (PLL/PHY/clock/controller), usb_isr, endpoint0_setup,
                 EP0 control-transfer engine, dQH/dTD structs, endpoint config.
                 Ported+trimmed from cores/teensy4/usb.c (enumeration subset only).
  usb_dev.h    — transfer_t / endpoint_t structs, usb_init/config_rx/config_tx API,
                 usb_configuration + usb_cdc_line_coding/rtsdtr externs.
  usb_desc.c   — CDC device/config/string descriptors + descriptor_list.
  usb_desc.h   — VID/PID, NUM_ENDPOINTS=4, CDC endpoint numbers/sizes, config #defines.
  imxrt1176.h  — USB_OTG1/USBNC/USBPHY1 register block + ANADIG PLL_USB1 + USB clock gate.
  core_pins.h  — add IRQ_USB_OTG1 = 136 to IRQ_NUMBER_t.
qemu2/                         (only if the gate needs it — see Verification)
  hw/arm/mimxrt1170-evk.c — possibly wire a CDC chardev to USB_OTG1 for the gate.
evkb/
  usb_enum_test/ — QEMU gate: bring up USB, print USB=CONFIGURED over Serial1; runner
                   attaches the ChipIdea CDC host + greps Serial1.
```

- DMA structures (`endpoint_queue_head[]`, dTDs, EP0 rx/tx buffers) get a `DMAMEM` attribute → `.dmabuffers` section → OCRAM (`imxrt1176.ld:83`). Reuses the existing provision; safe whether or not the D-cache is later enabled.
- Reuses the runtime RAM-vector-table + NVIC machinery (`attachInterruptVector`, `NVIC_ENABLE_IRQ`) exactly as prior peripherals.

## Enumeration mechanism (EP0 control engine, ported from Teensy)

- **`usb_init()`**: bring up PLL_USB1 + PHY; gate USB clock; `USBCMD.RST` (wait clear); zero dQH array; EP0 dQH config (RX `(64<<16)|(1<<15)`, TX `(64<<16)`); `ENDPTLISTADDR = &endpoint_queue_head`; `USBMODE = CM(2)|SLOM`; `USBINTR = UE|UEE|URE|SLE`; `attachInterruptVector(IRQ_USB_OTG1, usb_isr)` + `NVIC_ENABLE_IRQ`; `USBCMD.RS`.
- **`usb_isr()`**: on `USBSTS.UI`: read `ENDPTSETUPSTAT`; for a setup on EP0, copy the 8-byte SETUP from `dQH[0].setup0/1`, `ENDPTFLUSH` EP0 in/out, call `endpoint0_setup(setup)`. Handle `ENDPTCOMPLETE` (EP0 status/data stages), `USBSTS.URI` (bus reset → `usb_configuration=0`, reset address/endpoints).
- **`endpoint0_setup(setup)`** switch on `wRequestAndType`: `0x0500` SET_ADDRESS (defer to status stage → `DEVICEADDR`); `0x0680/0x0681` GET_DESCRIPTOR (walk `descriptor_list`, transmit from a DMAMEM staging buffer); `0x0900` SET_CONFIGURATION (`usb_configuration=wValue`; `usb_config_rx/tx` the CDC endpoints via `ENDPTCTRLn`); CDC `0x2021` SET_LINE_CODING (rx 7 bytes → `usb_cdc_line_coding`), `0xA1..` GET_LINE_CODING (tx it), `0x2221` SET_CONTROL_LINE_STATE (`usb_cdc_line_rtsdtr=wValue`). Unknown → stall EP0.

## Verification

- **QEMU gate (`usb_enum_test`)** — greps `USB=CONFIGURED` on `Serial1`:
  - Sketch: `Serial1.begin(115200)`; `usb_init()`; wait (bounded) until `usb_configuration != 0`; `Serial1.println("USB=CONFIGURED")` (else `USB=TIMEOUT`).
  - Runner: `qemu-system-arm -M mimxrt1170-evk -global fsl-imxrt1170.boot-xip=on -kernel … -serial file:<uart>` **plus** a CDC chardev on USB_OTG1 to activate the enumerating host — via `-chardev … -global usb-chipidea.chardev=<id>` (there are 2 ChipIdea instances; if `-global` is ambiguous, wire the chardev to `usb[0]` in `mimxrt1170-evk.c` — resolved in plan). Cross-check the `CI-CDC:` lines in `-d guest_errors`.
- **Hardware (the real arbiter):** flash; connect the EVKB **native USB** (not the debug VCOM) to the Mac; confirm a new `/dev/cu.usbmodem…` CDC device appears (`ls /dev/cu.*`, `ioreg -p IOUSB`), and `Serial1` (debug VCOM) prints `USB=CONFIGURED`. A green QEMU gate proves the state machine; only the Mac enumerating proves correctness.

## Error handling

- Unknown/unsupported EP0 requests → stall EP0 (`ENDPTCTRL0` stall bits), per USB spec.
- Bus reset (`URI`) mid-enumeration resets address, `usb_configuration=0`, re-primes EP0.
- `usb_init` waits for PLL lock / controller reset-clear with bounded loops (no infinite spin).
- The gate's "wait for configured" is bounded (`USB=TIMEOUT` on failure) so the runner always terminates.

## Risks / open items (resolved in plan)

- **PLL_USB1 480 MHz bring-up + lock**: exact `ANADIG_PLL_USB1` register sequence + lock wait, and the USB clock gate (CCM LPCG number) — from SDK `usb_phy.c`/clock config + RM. QEMU models the PLL loosely, so **HW is the judge**.
- **D-cache state**: verify caches are off on HW (no `SCB_EnableDCache` in the core); DMAMEM/OCRAM placement covers us either way, but confirm no coherency corruption on silicon.
- **QEMU chardev→USB_OTG1 wiring**: whether `-global usb-chipidea.chardev=` targets one instance cleanly or needs a `mimxrt1170-evk.c` tweak; and whether the QEMU CDC host drives HS (512 B bulk) or FS (64 B) — descriptors/packet sizes must match what the host expects.
- **Teensy `usb.c` subset**: which functions to port for enumeration-only (init, isr, endpoint0_*, config_rx/tx, transfer scaffolding) vs defer (rx/tx buffering) to Phase 2; and the `DMAMEM` macro (add if absent → `__attribute__((section(".dmabuffers")))`).
- **USBNC (non-core) registers**: any required OTG/USBNC settings (e.g. `USBNC_USB_OTG1_CTRL`) for device mode — from SDK.
