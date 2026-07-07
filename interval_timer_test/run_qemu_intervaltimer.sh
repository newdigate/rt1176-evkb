#!/bin/sh
set -e
QEMU=~/Development/rt1170/evkb/tools/qrun
DIR=$(cd "$(dirname "$0")" && pwd)
. ~/Development/rt1170/evkb/tools/gate-lib.sh
gate_init
ELF="$DIR/build/interval_timer_test.elf"; OUT="$DIR/it.uart"
rm -f "$OUT"
"$QEMU" -M mimxrt1170-evk -global fsl-imxrt1170.boot-xip=on -kernel "$ELF" \
    -icount shift=auto \
    -display none -serial file:"$OUT" -d guest_errors -D "$DIR/it.dbg" &
P=$!; gate_pid $P; sleep 20; kill $P 2>/dev/null; wait $P 2>/dev/null || true   # -icount couples delay() (DWT) and the PIT (QEMU_CLOCK_VIRTUAL) so counts are deterministic
echo "==== captured ===="; cat "$OUT"
grep -q "IT=PASS" "$OUT" || { echo "FAIL: IntervalTimer"; exit 1; }
echo "PASS: IntervalTimer verified"
