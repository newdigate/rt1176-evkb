#!/bin/sh
# QEMU gate for Phase-2A: the CM7 boots a REAL COMPILED CM4 image and asserts
# the startup canaries (.data copy, .bss zero, DTCM stack) over the MU.
set -e
QEMU=~/Development/rt1170/evkb/tools/qrun
DIR=$(cd "$(dirname "$0")" && pwd)
. ~/Development/rt1170/evkb/tools/gate-lib.sh
gate_init
ELF="$DIR/build/cm4_image_test.elf"
OUT="$DIR/cm4_image.uart"
rm -f "$OUT"
"$QEMU" -M mimxrt1170-evk -global fsl-imxrt1170.boot-xip=on -kernel "$ELF" \
    -display none -serial file:"$OUT" -d guest_errors -D "$DIR/cm4_image.dbg" &
P=$!; gate_pid $P
for _ in $(seq 1 40); do
    [ -f "$OUT" ] && grep -q "CM4IMG-DONE" "$OUT" 2>/dev/null && break
    sleep 0.25
done
kill $P 2>/dev/null; wait $P 2>/dev/null || true

echo "==== captured UART ===="; [ -f "$OUT" ] && cat "$OUT" || echo "(no UART output)"

fail=0
check() {
    if grep -q "^$1" "$OUT"; then echo "PASS: $1"; else echo "FAIL: expected $1"; fail=1; fi
}
grep -q "CM4IMG-GATE v1" "$OUT" || { echo "FAIL: banner missing"; exit 1; }
check "boot=00000001"      # CM4 left reset
check "run=00000001"       # Multicore.running()
check "data=DA7A0001"      # .data copied ITCM-LMA -> DTCM-VMA
check "bss=00000B55"       # .bss zero-initialised
check "stack=0000008C"     # DTCM stack works (0+1+4+9+16+25+36+49)
check "echo=12345679"      # real image keeps running + MU round-trip
check "data2=DA7A0001"     # restart re-ran the whole CM4 startup
check "bss2=00000B55"
check "stack2=0000008C"
grep -q "CM4IMG-DONE" "$OUT" || { echo "FAIL: DONE missing"; fail=1; }

if [ $fail -eq 0 ]; then
    echo "PASS: real compiled CM4 image verified in QEMU"
else
    echo "GATE FAILED"; exit 1
fi
