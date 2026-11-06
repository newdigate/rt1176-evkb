#!/bin/sh
# QEMU gate for nosh_bringup: the Nosh board's first-boot image, run on the EVKB
# machine dressed as Nosh -- gate-lib's nosh arm grows the SoC's SDRAM window to
# 128 MB, gate_sd_drive puts the card image on USDHC2 (-drive index 1), and
# m2-wifi=on puts the IW416 model on USDHC1 so the M.2 enumeration has something
# to answer CMD5.  Every subsystem verdict the sketch prints is asserted here;
# the I2C scan is asserted to complete (what it finds is the EVKB machine's
# fixtures, not Nosh's codec), and the M.2 verdict is asserted to be "ok".
set -e
DIR=$(cd "$(dirname "$0")" && pwd)
EVKB=$(cd "$DIR/../../.." && pwd)
QEMU="$EVKB/tools/qrun"
. "$EVKB/tools/gate-lib.sh"
gate_init 120

[ "$(gate_board)" = nosh ] || {
    echo "FAIL: nosh_bringup is a nosh-only gate (EVKB_BOARD=$(gate_board)); the boards sidecar lists nosh alone"
    exit 1; }

ELF="$DIR/$(gate_build_dir)/nosh_bringup.elf"
[ -f "$ELF" ] || {
    echo "FAIL: no ELF ($ELF) -- build first:"
    echo "  cd $DIR && cmake -B $(gate_build_dir) -DEVKB_BOARD=nosh -DCMAKE_TOOLCHAIN_FILE=$EVKB/toolchain/nosh.toolchain.cmake && cmake --build $(gate_build_dir)"
    exit 1; }

OUT=$(gate_capture_path "$DIR" bringup.uart)
DBG=$(gate_capture_path "$DIR" bringup.dbg)
IMG=$(gate_capture_path "$DIR" card.img)
rm -f "$OUT" "$DBG" "$IMG"
gate_tmp "$IMG"

# MBR + one FAT16 partition, the sd_fs_test recipe: SdFat mounts MBR partition 1
# only, so a bare FAT boot sector would not mount.  512 MB sparse -> SDSC.
mkfile -n 512m "$IMG"
DISK=$(hdiutil attach -nomount -imagekey diskimage-class=CRawDiskImage "$IMG" | head -1 | awk '{print $1}')
[ -n "$DISK" ] || { echo "FAIL: could not attach $IMG"; exit 1; }
diskutil partitionDisk "$DISK" 1 MBR "MS-DOS FAT16" NOSH 100% >/dev/null \
    || { hdiutil detach "$DISK" >/dev/null 2>&1 || true; echo "FAIL: partition/format"; exit 1; }
hdiutil detach "$DISK" >/dev/null

"$QEMU" $(gate_qemu_machine) -machine m2-wifi=on -kernel "$ELF" \
    -display none $(gate_console "$OUT") \
    $(gate_sd_drive "$IMG") \
    -d guest_errors -D "$DBG" &
P=$!; gate_pid $P
for _ in $(seq 1 240); do
    [ -f "$OUT" ] && grep -q "^alive=2" "$OUT" 2>/dev/null && break
    sleep 0.25
done
gate_reap $P
gate_require_capture "$OUT"
echo "==== captured UART ===="; cat "$OUT"

grep -q "^NOSH_BRINGUP board=nosh"   "$OUT" || { echo "FAIL: banner missing (not a Nosh build?)"; exit 1; }
grep -q "sdram_mb=128"               "$OUT" || { echo "FAIL: core does not report 128 MB SDRAM"; exit 1; }
grep -q "^SDRAM_DATA=PASS"           "$OUT" || { echo "FAIL: SDRAM data lines"; exit 1; }
grep -q "^SDRAM_ADDR=PASS"           "$OUT" || { echo "FAIL: SDRAM address lines (aliasing at the 64 MB line?)"; exit 1; }
grep -q "^SDRAM_FILL=PASS mb=12[0-9] errors=0" "$OUT" || { echo "FAIL: SDRAM full fill"; exit 1; }
grep -q "^SDRAM=PASS"                "$OUT" || { echo "FAIL: SDRAM verdict"; exit 1; }
grep -q "^sd: usdhc2 up"             "$OUT" || { echo "FAIL: microSD on USDHC2 did not come up"; exit 1; }
grep -q "^SD=PASS"                   "$OUT" || { echo "FAIL: microSD write/read-back"; exit 1; }
grep -q "^I2C5_SCAN=DONE n="         "$OUT" || { echo "FAIL: I2C5 scan did not complete (bus hung?)"; exit 1; }
# The IW416 model answers CMD5 with one IO function; anything else means the
# USDHC1 SDIO path regressed in the Nosh build (or m2-wifi=on was lost).
grep -q "^M2_SDIO=ok funcs=[1-9]"    "$OUT" || { echo "FAIL: M.2 SDIO enumeration against the IW416 model"; exit 1; }
grep -q "^BRINGUP=PASS"              "$OUT" || { echo "FAIL: overall verdict"; exit 1; }
grep -q "^alive=2"                   "$OUT" || { echo "FAIL: no heartbeat after setup()"; exit 1; }
echo "PASS: nosh_bringup -- SDRAM 128 MB, microSD on USDHC2, I2C5 scan, M.2 SDIO enumeration"
