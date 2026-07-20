#!/usr/bin/env bash
# Run an RT1170 firmware image in the custom QEMU mimxrt1170-evk machine.
#
# Usage:  rt1170-qemu.sh [path/to/image.elf|.hex]
# Default image: the prebuilt Zephyr hello_world.
#
# Boot flow note: flexspi_nor / boot-header images (vector table reached via the
# FCB/IVT, not at 0x30000000) require the BootROM stub, enabled with
# `-global fsl-imxrt1170.boot-xip=on`. Without it QEMU resets straight to
# 0x30000000 (zeros in such images) and immediately HardFault-locks up (PC=0).
# boot-xip mimics NXP's ROM: reads the FlexSPI IVT @0x30001000 + image vector
# table @0x30002000, sets MSP+VTOR, branches to entry.
#
# Console: LPUART1 -> serial_hd(0) -> stdio. Output is instant (QEMU ignores baud).
# Quit QEMU: Ctrl-A then X.   (-serial mon:stdio also gives the monitor via Ctrl-A C)
set -euo pipefail

QEMU="${QEMU:-$HOME/Development/qemu2/build/qemu-system-arm}"
MACHINE="mimxrt1170-evk"
IMG="${1:-$HOME/Development/zephyr/projects/zepherproject/build-hello/zephyr/zephyr.elf}"

# boot-xip defaults on (correct for boot-header XIP images). Set BOOT_XIP=off for
# raw RAM-bring-up images whose vector table sits directly at 0x30000000.
BOOT_XIP="${BOOT_XIP:-on}"

[ -x "$QEMU" ] || { echo "QEMU not found at $QEMU (set \$QEMU)"; exit 1; }
[ -f "$IMG" ]  || { echo "Image not found: $IMG"; exit 1; }

echo "==> QEMU $MACHINE  boot-xip=$BOOT_XIP"
echo "==> Image: $IMG"
echo "==> Quit with Ctrl-A X."
echo
exec "$QEMU" -M "$MACHINE" -global "fsl-imxrt1170.boot-xip=$BOOT_XIP" \
    -nographic -serial mon:stdio -kernel "$IMG"
