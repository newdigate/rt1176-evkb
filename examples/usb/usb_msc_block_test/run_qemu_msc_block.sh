#!/bin/sh
# RT1176 USB Mass Storage host gate -- raw block R/W (BOT+SCSI over USBDrive).
#
# The firmware brings up the OTG2 EHCI host, enumerates QEMU's usb-storage device,
# and does a non-destructive raw-sector round-trip (save/write/read-back/restore),
# printing markers over Serial1 (LPUART1 = serial_hd(0)):
#   MSC_BLOCK_BEGIN / MSC_CONNECT=vid:pid / MSC_CAP=blocksxbytes /
#   MSC_BLOCK_WRITE=PASS / MSC_BLOCK_READ=PASS / MSC_BLOCK_DONE
#
# usb-storage is pinned to the single OTG2 root port with port=1 (without it QEMU's
# convenience auto-hub splices a usb-hub ahead of the device).  -icount shift=auto
# keeps the EHCI frame timer deterministic against the firmware's polling.
set -e
QEMU=~/Development/rt1170/evkb/tools/qrun
DIR=$(cd "$(dirname "$0")" && pwd)
. ~/Development/rt1170/evkb/tools/gate-lib.sh
gate_init
ELF="$DIR/build/usb_msc_block_test.elf"
OUT="$DIR/msc_block.uart"; DBG="$DIR/msc_block.dbg"; IMG="$DIR/usb.img"
rm -f "$OUT" "$DBG"
# 64 MB bare raw image (no filesystem needed for raw block R/W); sparse.
[ -f "$IMG" ] || mkfile -n 64m "$IMG"

"$QEMU" -M mimxrt1170-evk -global fsl-imxrt1170.boot-xip=on -kernel "$ELF" \
    -icount shift=auto -display none \
    -serial file:"$OUT" -d guest_errors -D "$DBG" \
    -drive if=none,id=stick,file="$IMG",format=raw \
    -device usb-storage,drive=stick,bus=usbhost.0,port=1,removable=on &
P=$!; gate_pid $P

# Bounded poll for a terminal marker, then stop QEMU.
python3 - "$OUT" 25 <<'PYWAIT'
import sys, time
out, secs = sys.argv[1], int(sys.argv[2]); dl = time.time() + secs
def rd():
    try:
        with open(out, errors="replace") as f: return f.read()
    except OSError: return ""
while time.time() < dl:
    t = rd()
    if "MSC_BLOCK_DONE" in t or "MSC_BLOCK_FAIL" in t: break
    time.sleep(0.5)
PYWAIT

kill $P 2>/dev/null; wait $P 2>/dev/null || true
echo "==== msc_block UART (LPUART1) ===="; cat "$OUT"
echo "==== TCM-DMA guest_errors (should be empty until later) ===="; grep -i "TCM" "$DBG" 2>/dev/null || echo "(none)"
python3 "$DIR/check_msc_block.py" "$OUT"
