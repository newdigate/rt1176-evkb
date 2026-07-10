#!/bin/sh
set -e
QEMU=~/Development/rt1170/evkb/tools/qrun
DIR=$(cd "$(dirname "$0")" && pwd)
. ~/Development/rt1170/evkb/tools/gate-lib.sh
gate_init
ELF="$DIR/build/rtc_test.elf"; OUT="$DIR/rtc.uart"
rm -f "$OUT"
"$QEMU" -M mimxrt1170-evk -global fsl-imxrt1170.boot-xip=on -kernel "$ELF" \
    -icount shift=auto \
    -display none -serial file:"$OUT" -d guest_errors -D "$DIR/rtc.dbg" &
P=$!; gate_pid $P; sleep 25; kill $P 2>/dev/null; wait $P 2>/dev/null || true
echo "==== captured ===="; cat "$OUT"
grep -q "RTC_ALL=PASS" "$OUT" || { echo "FAIL: rtc"; exit 1; }
echo "PASS: rtc verified"
