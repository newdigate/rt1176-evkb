#!/bin/sh
set -e
QEMU=~/Development/rt1170/evkb/tools/qrun
DIR=$(cd "$(dirname "$0")" && pwd)
. ~/Development/rt1170/evkb/tools/gate-lib.sh
gate_init
ELF="$DIR/build/audio_h_test.elf"; OUT="$DIR/audio_h.uart"
rm -f "$OUT"
"$QEMU" -M mimxrt1170-evk -global fsl-imxrt1170.boot-xip=on -kernel "$ELF" \
    -display none -serial file:"$OUT" -d guest_errors -D "$DIR/audio_h.dbg" &
P=$!; gate_pid $P
for _ in $(seq 1 40); do [ -f "$OUT" ] && grep -q "AUDIOH-DONE" "$OUT" 2>/dev/null && break; sleep 0.25; done
kill $P 2>/dev/null; wait $P 2>/dev/null || true
echo "==== captured ===="; cat "$OUT"
grep -q "AUDIOH-GATE v1"  "$OUT" || { echo "FAIL: banner"; exit 1; }
grep -q "AUDIOH_CHAIN=PASS" "$OUT" || { echo "FAIL: chain"; exit 1; }
echo "PASS: AUDIOH_CHAIN"
