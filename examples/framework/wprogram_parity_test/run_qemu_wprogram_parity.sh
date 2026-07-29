#!/bin/sh
set -e
DIR=$(cd "$(dirname "$0")" && pwd)
# Tools come from THIS checkout, derived from the gate's own location. The old
# hardcoded ~/Development/rt1170/evkb/tools/... meant a worktree or a clone at
# any other path silently loaded a DIFFERENT tree's gate-lib.sh -- which surfaces
# as "gate_reap: command not found", or worse, as a gate quietly running against
# the wrong library.
EVKB=$(cd "$DIR/../../.." && pwd)
QEMU="$EVKB/tools/qrun"
. "$EVKB/tools/gate-lib.sh"
gate_init
ELF="$DIR/build/wprogram_parity_test.elf"; OUT="$DIR/wp.uart"
rm -f "$OUT"
"$QEMU" -M mimxrt1170-evk -global fsl-imxrt1170.boot-xip=on -kernel "$ELF" \
    -icount shift=auto \
    -display none -serial file:"$OUT" -d guest_errors -D "$DIR/wp.dbg" &
P=$!; gate_pid $P; sleep 20; gate_reap $P
gate_require_capture "$OUT"
echo "==== captured ===="; cat "$OUT"
for k in WCHAR STRING WORD RAND EMILLIS ITIMER PULSE_TIMEOUT CRASHREPORT_BOOL BOUNCE USBSERIAL; do
  grep -q "$k=OK" "$OUT" || { echo "FAIL: $k"; exit 1; }
done
grep -q "not yet supported on IMXRT1176" "$OUT" || { echo "FAIL: CrashReport stub print"; exit 1; }
grep -q "GATE=DONE" "$OUT" || { echo "FAIL: completion"; exit 1; }
echo "PASS: WProgram.h include-parity gate"
