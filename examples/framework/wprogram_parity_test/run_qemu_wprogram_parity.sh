#!/bin/sh
set -e
QEMU=~/Development/rt1170/evkb/tools/qrun
DIR=$(cd "$(dirname "$0")" && pwd)
. ~/Development/rt1170/evkb/tools/gate-lib.sh
gate_init
ELF="$DIR/build/wprogram_parity_test.elf"; OUT="$DIR/wp.uart"
rm -f "$OUT"
"$QEMU" -M mimxrt1170-evk -global fsl-imxrt1170.boot-xip=on -kernel "$ELF" \
    -icount shift=auto \
    -display none -serial file:"$OUT" -d guest_errors -D "$DIR/wp.dbg" &
P=$!; gate_pid $P; sleep 20; kill $P 2>/dev/null; wait $P 2>/dev/null || true
echo "==== captured ===="; cat "$OUT"
for k in WCHAR STRING WORD RAND EMILLIS ITIMER PULSE_TIMEOUT CRASHREPORT_BOOL BOUNCE USBSERIAL; do
  grep -q "$k=OK" "$OUT" || { echo "FAIL: $k"; exit 1; }
done
grep -q "not yet supported on IMXRT1176" "$OUT" || { echo "FAIL: CrashReport stub print"; exit 1; }
grep -q "GATE=DONE" "$OUT" || { echo "FAIL: completion"; exit 1; }
echo "PASS: WProgram.h include-parity gate"
