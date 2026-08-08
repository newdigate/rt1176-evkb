# Phase 2 — RT1062 USB host in QEMU

**Date:** 2026-08-08
**Status:** design approved, not yet implemented
**Parent:** `docs/superpowers/specs/2026-08-07-rt1060-board-axis-design.md` §5
**Predecessor:** Phase 1 complete — 82 gates, both boards green, evkb `835cd87`

---

## 1. Goal

Make the `mimxrt1060-evk` QEMU machine able to host an emulated USB device, and
prove it with a gate: `usb_descriptor_survey` enumerates QEMU's `usb-audio` on
the RT1062 and reads its identity off the wire.

## 2. What is already there, and what is missing

The EHCI host controller is **not** missing. `TYPE_CHIPIDEA` derives from
`TYPE_SYS_BUS_EHCI` and is shared by `fsl-imxrt1062.c` and `fsl-imxrt1170.c`;
host support landed underneath both when the 1170 work was done
(`hw/usb/chipidea.c:114–116` — device mode owns the registers, host mode
delegates to the EHCI core).

What the 1062 SoC lacks is the wiring the 1170 added *on top*. For `i == 1`
(OTG2, the host-role controller) the 1170 does two things the 1062 does
neither of (`fsl-imxrt1170.c:1414–1419`):

```c
DEVICE(&s->usb[i])->id = g_strdup("usbhost");
fsl_imxrt1170_init_usb_host_dma(s);
s->usb[i].parent_obj.ehci.as = &s->usb_dma_as;
```

**The stale README claim is corrected as part of this phase.**
`USBHost_t36/README.md` states the RT1062 QEMU model is "device-mode only (no
EHCI host emulation)". That was true when written and is not true now.

## 3. Change 1 — the bus id

`DEVICE(&s->usb[1])->id = g_strdup("usbhost")`.

Without an id, qbus names a nameless child bus `usb-bus.<N>` from a global
counter whose value depends on realize order — fragile, and not something a
gate can name. With it, the EHCI host bus (the controller's *first* child bus,
created in the parent realize before any gadget bus) becomes `usbhost.0`, which
is what `-device usb-audio,bus=usbhost.0` binds to.

**OTG1 (`i == 0`) is deliberately left id-less**, exactly as on the 1170, so the
existing `-chardev ...,id=usb0` CDC-ACM console path on this board
(`mimxrt1060-evk.c:77–80`) is completely unchanged.

## 4. Change 2 — the DMA view with TCM holes

Port `fsl_imxrt1170_init_usb_host_dma` (`fsl-imxrt1170.c:78–100`): a memory
region aliasing system memory, with ITCM and DTCM punched out as error-returning
holes, wrapped in its own `AddressSpace` and installed as the EHCI's `as`.

**Why this board needs it more than the 1176 does, not less.** The i.MX RT1062's
TCMs are CPU-private; the USB controller is a separate AHB master and cannot
reach them. That is the same architectural constraint the 1176 models. But on
*this* board the constraint is easier to trip: `imxrt1060_evkb.ld` places
`_estack` in **DTCM**, so every stack-allocated buffer is TCM-resident. A driver
that hands a stack buffer to a transfer would work in an unmodelled QEMU and
fail on silicon.

That is exactly the divergence class the two-gate rule exists to prevent, and
Phase 4 (the UAC driver, which the RT1062 has never compiled) is where it is
most likely to appear. Modelling it now means QEMU cannot lie about DMA
reachability for the rest of this arc.

Note the asymmetry with Phase 1's finding, because it is easy to misread: §1.2
of the parent spec established that USBHost_t36's `__IMXRT1176__`-only `DMAMEM`
guards are correctly inert here, since `.bss` lands in OCRAM on this board.
That is about the library's **static** pools and remains true. It says nothing
about **stack** buffers, which are in DTCM. The two facts coexist.

## 5. Change 3 — `usb_descriptor_survey` gains rt1062

- `toolchain/rt1062-evkb.toolchain.cmake` — a **verbatim copy** of the one
  Phase 1 wrote for `serial_test`. Both examples sit at the same depth
  (`../../../evkb.cmake`), so the `get_filename_component` walk to the repo root
  is identical; no adjustment needed.
- `boards` sidecar declaring `rt1176` and `rt1062`.
- **The `TEENSY_VERSION` guard is required here — the trap is present.**
  `CMakeLists.txt:4` reads `set(TEENSY_VERSION 117 CACHE STRING "")`,
  unconditionally, exactly as `serial_test`'s did. Left as-is it silently builds
  an RT1176 image into `build-rt1062/`, which then boots the wrong QEMU machine
  and fails in a way that reads as a board or model problem rather than a build
  misconfiguration. Apply the same `if(NOT DEFINED TEENSY_VERSION)` guard.

**No source changes are expected.** The library's `__IMXRT1062__` path is
upstream Teensy's own and this is its home silicon.

### The oracle

The gate asserts `idVendor=46F4` / `idProduct=0002` — QEMU's `usb-audio`
identity (`hw/usb/dev-audio.c:43-44`). The firmware has no knowledge of those
numbers, so this is a genuine off-the-wire oracle rather than firmware agreeing
with itself. The firmware asserts only `vid != 0`, so the identical image serves
a silicon run in Phase 3 with whatever device is in J47.

## 6. Red-first, and the vacuity trap

`transcript_qemu_red.txt` is committed **before** the model change, showing the
gate failing because `bus=usbhost.0` does not resolve.

**The device must be HOTPLUGGED, not present at reset.** ChipIdea defers attach
to the guest's port-power write (`chipidea.c:230` → `hcd-ehci.c:1066`), so a
device present from reset raises PCD before the firmware is listening. Phase 7.1
produced two vacuous reds this way before it was understood — the first with
`irqcnt=0, stsraw=0`, which said nothing about routing at all. The gate hotplugs
via `-monitor unix:` after polling the UART for a marker proving the firmware
has powered the port.

A red that would have happened anyway is not evidence.

## 7. Risks

| Risk | Status |
|---|---|
| `CCM_ANALOG_PLL_USB2` poll never completes | **Checked** — `imxrt1060_anatop.c:51` forces the LOCK bit on reads |
| USBPHY2 unmodelled | `TYPE_IMX_USBPHY` is instantiated (`fsl-imxrt1062.c:292`); the `__IMXRT1062__` branch does no PHY polling, unlike the 1176's `PLL_SIC` |
| Another SEMC-class stub blocks boot | Unknown until run. **Diagnose with `-d unimp`**, which is what localised SEMC (1,000,000 reads of `semc-ctrl` offset `0x3c`); `-d guest_errors` alone showed only 3 lines and would have misled |
| Fresh clone sees this gate red | Accepted and documented, exactly as Phase 7.1's IRQ-135 split is |

## 8. The GPL firewall

The qemu2 change **stays local** — `~/Development/qemu2` is GPL-2.0 and its code
must never flow into this MIT/BSD firmware tree. Firmware→qemu2 is fine; the
reverse is not.

Consequence, stated plainly because it is a real cost: a fresh clone of this
repo gets a **red** `rt1062:usb/usb_descriptor_survey`, for the same reason
`dualcore/cm4_usb_irq_probe` is red on a fresh clone. This goes in
`docs/KNOWN-BROKEN-GATES.md` when the phase lands.

## 9. Definition of done

- `transcript_qemu_red.txt` committed, showing the gate red for the stated cause
- `rt1062:usb/usb_descriptor_survey` PASSES, asserting `46F4:0002` off the wire
- `rt1176:usb/usb_descriptor_survey` unchanged and still green
- Sweep 82 → 83, zero SKIP
- Licence audit PASS with the new `build-rt1062` entry in `GATES`
- `USBHost_t36/README.md`'s "device-mode only" paragraph corrected
- `CLAUDE.md` and `KNOWN-BROKEN-GATES.md` record the count and the fresh-clone red

## 10. Not in this phase

No silicon run (Phase 3, on **J47** — not J48, which is the device port). No
audio driver, no `usb_audio_*` example for rt1062 (Phase 4). No `String` gating
gap work (tracked separately).
