#!/bin/sh
# QEMU gate for Phase-1 D7 (CM4 hot-swap): the CM7 boots the CM4 with image A
# (identity 0xA1A1A1A1), then a SECOND Multicore.begin(imageB) re-pulses
# SRC_CTRL_M4CORE.SW_RESET on the RUNNING CM4, rebooting it into image B
# (identity 0xB2B2B2B2). Each image streams a ready handshake (CAFE0001) + its
# identity over the MU. Seeing idA=A1A1A1A1 THEN idB=B2B2B2B2 proves the running
# CM4 was hot-swapped into a different program at runtime. No qemu2 or library
# change: qemu2's fsl_imxrt1170_cm4_boot() already re-reads GPR0/1 + cpu_reset()s
# a running CM4, and Multicore.begin() re-pulses SW_RESET (skipping BT_RELEASE
# after the first call).
set -e
QEMU=~/Development/rt1170/evkb/tools/qrun
DIR=$(cd "$(dirname "$0")" && pwd)
. ~/Development/rt1170/evkb/tools/gate-lib.sh
gate_init
ELF="$DIR/build/cm4_hotswap_test.elf"
OUT="$DIR/cm4_hotswap.uart"
rm -f "$OUT"
"$QEMU" -M mimxrt1170-evk -global fsl-imxrt1170.boot-xip=on -kernel "$ELF" \
    -display none -serial file:"$OUT" -d guest_errors -D "$DIR/cm4_hotswap.dbg" &
P=$!; gate_pid $P
for _ in $(seq 1 40); do
    [ -f "$OUT" ] && grep -q "CM4HOTSWAP-DONE" "$OUT" 2>/dev/null && break
    sleep 0.25
done
kill $P 2>/dev/null; wait $P 2>/dev/null || true

echo "==== captured UART ===="; [ -f "$OUT" ] && cat "$OUT" || echo "(no UART output)"

fail=0
check() {
    if grep -q "^$1" "$OUT"; then echo "PASS: $1"; else echo "FAIL: expected $1"; fail=1; fi
}
grep -q "CM4HOTSWAP-GATE v1" "$OUT" || { echo "FAIL: banner missing"; exit 1; }
check "readyA=CAFE0001"              # image A ready handshake
check "idA=A1A1A1A1"                 # image A identity
check "readyB=CAFE0001"              # image B ready handshake (post hot-swap reset)
check "idB=B2B2B2B2"                 # image B identity -> the CM4 rebooted into B
check "HOTSWAP=PASS"
grep -q "CM4HOTSWAP-DONE" "$OUT" || { echo "FAIL: DONE missing"; fail=1; }

if [ $fail -eq 0 ]; then
    echo "PASS: CM4 hot-swap (boot A -> begin(B) re-pulses SW_RESET -> reboot into B) verified in QEMU"
else
    echo "GATE FAILED"; exit 1
fi
