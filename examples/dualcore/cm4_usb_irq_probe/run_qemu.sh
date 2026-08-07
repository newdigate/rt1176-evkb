#!/bin/sh
# QEMU gate for cm4_usb_irq_probe (Phase 7.1): can the CM4 bring up the USB
# host port itself and take IRQ 135 on its own NVIC?
#
# EXPECTED-FAIL until qemu2 fans USB OTG2's IRQ to both NVICs. The model wires
# both USB IRQs to the CM7 NVIC only (fsl-imxrt1170.c:1418
# "sysbus_connect_irq(sbd, 0, qdev_get_gpio_in(armv7m, usb_irq[i]))"), so
# irqcnt reads 0 and USB_IRQ_CM4=FAIL. transcript_qemu_red.txt is that run,
# checked in as evidence BEFORE the model changed -- without it, a later green
# gate could not distinguish "the split was needed and works" from "the split
# was never needed".
#
# THE DEVICE IS HOTPLUGGED, NOT PRESENT AT RESET, and that is load-bearing.
# ChipIdea defers the attach report to the guest's PP write (chipidea.c:230 ->
# hcd-ehci.c:1066), so a device present from reset raises PCD during the
# firmware's stage 5 -- and stage 6's "W1C any stale status" then clears it
# before PCE and the NVIC are armed. Measured 2026-08-06, that produced
# irqcnt=0 with a post-window USBSTS of 0: no interrupt condition ever occurred
# while anything was listening, so the red said nothing whatever about IRQ
# routing. This gate now waits for the CM4's stage-6 marker and only then
# device_adds, so the condition arises while the CM4 is armed -- the same
# physical event the silicon probe asserts (operator plugs J47 in the window).
set -e
DIR=$(cd "$(dirname "$0")" && pwd)
EVKB=$(cd "$DIR/../../.." && pwd)
QEMU="$EVKB/tools/qrun"
. "$EVKB/tools/gate-lib.sh"
gate_init
ELF="$DIR/$(gate_build_dir)/cm4_usb_irq_probe.elf"
OUT="$DIR/cm4_usb_irq.uart"
MON="$DIR/mon.sock"
rm -f "$OUT" "$MON"
gate_tmp "$MON"
"$QEMU" $(gate_qemu_machine) -kernel "$ELF" \
    -display none -serial file:"$OUT" -d guest_errors -D "$DIR/cm4_usb_irq.dbg" \
    -audiodev none,id=snd0 \
    -monitor unix:"$MON",server,nowait &
P=$!; gate_pid $P

# Wait for the CM4 to arm IRQ 135 (stage 6), THEN plug the device. Polling for
# the token rather than sleeping a fixed interval: the emulator does not run at
# wall-clock rate (the firmware's nominal 8 s observation window has been
# measured completing in ~7 s wall), so a fixed offset would be fragile.
for _ in $(seq 1 120); do
    [ -f "$OUT" ] && grep -q "s6=C5B00006" "$OUT" 2>/dev/null && break
    sleep 0.25
done
# python3, not nc: -U (unix socket) support varies across macOS versions, and
# python3 is already a dependency of this repo's tools.
printf 'device_add usb-audio,bus=usbhost.0,port=1,audiodev=snd0\n' \
    | python3 -c 'import socket,sys; s=socket.socket(socket.AF_UNIX); s.connect(sys.argv[1]); s.sendall(sys.stdin.buffer.read()); s.close()' "$MON"

for _ in $(seq 1 120); do
    [ -f "$OUT" ] && grep -q "CM4USBIRQ-DONE" "$OUT" 2>/dev/null && break
    sleep 0.25
done
gate_reap $P
gate_require_capture "$OUT"

echo "==== captured ===="; cat "$OUT"

grep -q "CM4USBIRQ-DONE" "$OUT" || { echo "FAIL: no done marker"; exit 1; }

# Stage assertions first: they localise a failure instead of just reporting one.
grep -q "STAGES=PASS" "$OUT" \
    || { echo "FAIL: the CM4 did not complete all six register stages"; exit 1; }
# PHY_PLL_CM4 is a WORLD-SPLIT token, deliberately NOT asserted here.
# QEMU's TYPE_IMX_USBPHY is an i.MX6-era model whose register file ends at
# USBPHY_VERSION (0x80); the RT1176's PLL_SIC quad at 0xA0/A4/A8/AC is
# unmodelled, so reads return 0 and the lock poll can never succeed -- for
# EITHER core. This is pre-existing and already documented in HW-verified
# firmware (USBHost_t36/ehci.cpp:248, "QEMU: PLL_SIC reads 0 -> times out,
# proceeds"), not a regression and not a CM4 finding. Asserting it here
# would make the gate unpassable for a reason unrelated to interrupts.
# Silicon asserts it instead (Task 7). Precedent for world-split tokens:
# 2C systick, 3.2 rdv, Phase 5 irqcnt.
grep -q "PHY_PLL_CM4=" "$OUT" \
    || { echo "FAIL: PHY_PLL_CM4 token missing entirely"; exit 1; }
grep -q "HOSTMODE_CM4=PASS" "$OUT" \
    || { echo "FAIL: USBMODE did not read back as host (CM=3)"; exit 1; }

# A 0->1 CONNECT TRANSITION during the observation window, not merely a device
# being present. This is what makes the gate un-fakeable in the same way the
# silicon probe is: a device already attached at reset, and one that never
# arrives, both fail here.
#
# Ordered BEFORE the USB_IRQ_CM4 assertion so a hotplug that did not land
# inside the window is reported as such, rather than as an interrupt-routing
# failure it says nothing about.
grep -q "PLUG_TRANSITION=PASS" "$OUT" \
    || { echo "FAIL: no 0->1 connect transition during the observation window"; exit 1; }

grep -q "USB_IRQ_CM4=PASS" "$OUT" \
    || { echo "FAIL: the CM4 did not take IRQ 135"; exit 1; }

# Vacuity guard: irqcnt is asserted >0 by the firmware, but a transcript that
# somehow carried the literal zero token alongside a PASS would mean the
# verdict logic is broken, not that the probe succeeded.
grep -q "irqcnt=00000000" "$OUT" \
    && { echo "FAIL: irqcnt is zero yet a PASS was printed -- verdict logic is wrong"; exit 1; }

echo "PASS: USB_IRQ_CM4"
