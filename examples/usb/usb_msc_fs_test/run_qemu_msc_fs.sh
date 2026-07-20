#!/bin/sh
# RT1176 USB Mass Storage host gate -- FAT mount + file write/read-back/dir.
#
# Builds a fresh MBR-partitioned FAT16 image each run: SdFat's FsVolume::begin
# mounts only MBR partition 1 (no superfloppy fallback), so a bare FAT boot sector
# at LBA 0 will NOT mount.  The firmware auto-mounts partition 1 via myusb.Task(),
# writes /RTTEST.TXT, reads it back byte-exact, and lists the root, printing markers
# over Serial1 (LPUART1):
#   MSC_FS_BEGIN / MSC_MOUNT=FATxx / MSC_FS_WRITE=PASS / MSC_FS_READ=PASS /
#   MSC_FS_DIR=PASS / MSC_FS_DONE
set -e
QEMU=~/Development/rt1170/evkb/tools/qrun
DIR=$(cd "$(dirname "$0")" && pwd)
. ~/Development/rt1170/evkb/tools/gate-lib.sh
gate_init
ELF="$DIR/build/usb_msc_fs_test.elf"
OUT="$DIR/msc_fs.uart"; DBG="$DIR/msc_fs.dbg"; IMG="$DIR/usb.img"
rm -f "$OUT" "$DBG" "$IMG"

# 512 MB sparse image, MBR + one primary FAT16 partition (macOS-native tools).
mkfile -n 512m "$IMG"
DISK=$(hdiutil attach -nomount -imagekey diskimage-class=CRawDiskImage "$IMG" | head -1 | awk '{print $1}')
[ -n "$DISK" ] || { echo "FAIL: could not attach $IMG"; exit 1; }
diskutil partitionDisk "$DISK" 1 MBR "MS-DOS FAT16" RTTEST 100% >/dev/null \
    || { hdiutil detach "$DISK" >/dev/null 2>&1 || true; echo "FAIL: partition/format"; exit 1; }
hdiutil detach "$DISK" >/dev/null

"$QEMU" -M mimxrt1170-evk -global fsl-imxrt1170.boot-xip=on -kernel "$ELF" \
    -icount shift=auto -display none \
    -serial file:"$OUT" -d guest_errors -D "$DBG" \
    -drive if=none,id=stick,file="$IMG",format=raw \
    -device usb-storage,drive=stick,bus=usbhost.0,port=1,removable=on &
P=$!; gate_pid $P

python3 - "$OUT" 30 <<'PYWAIT'
import sys, time
out, secs = sys.argv[1], int(sys.argv[2]); dl = time.time() + secs
def rd():
    try:
        with open(out, errors="replace") as f: return f.read()
    except OSError: return ""
while time.time() < dl:
    t = rd()
    if "MSC_FS_DONE" in t or "MSC_FS_FAIL" in t: break
    time.sleep(0.5)
PYWAIT

kill $P 2>/dev/null; wait $P 2>/dev/null || true
echo "==== msc_fs UART (LPUART1) ===="; cat "$OUT"

# Optional non-fatal host interop: re-mount the image and show the written file.
HOSTATTACH=$(hdiutil attach -imagekey diskimage-class=CRawDiskImage "$IMG" 2>/dev/null || true)
HOSTDISK=$(echo "$HOSTATTACH" | head -1 | awk '{print $1}')
HOSTMNT=$(echo "$HOSTATTACH" | grep -o '/Volumes/[^[:space:]]*' | head -1)
if [ -n "$HOSTMNT" ]; then
    echo "---- host FAT listing ($HOSTMNT) ----"; ls -la "$HOSTMNT" 2>/dev/null || true
    echo "---- RTTEST.TXT ----"; cat "$HOSTMNT/RTTEST.TXT" 2>/dev/null || true; echo
fi
[ -n "$HOSTDISK" ] && { hdiutil detach "$HOSTDISK" >/dev/null 2>&1 || true; }

python3 "$DIR/check_msc_fs.py" "$OUT"
