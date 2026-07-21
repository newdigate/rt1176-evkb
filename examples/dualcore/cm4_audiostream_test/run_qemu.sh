#!/bin/sh
# QEMU gate for cm4_audiostream_test: the AudioStream graph engine on the CM4
# (real core AudioStream.cpp + cm4_shim, IRQ_SOFTWARE=44 pended on the CM4
# NVIC). Pure-CPU graph — a green QEMU run is meaningful and expected to be
# token-identical to the EVKB run; the EVKB run confirms on silicon.
set -e
QEMU=~/Development/rt1170/evkb/tools/qrun
DIR=$(cd "$(dirname "$0")" && pwd)
. ~/Development/rt1170/evkb/tools/gate-lib.sh
gate_init
ELF="$DIR/build/cm4_audiostream_test.elf"
OUT="$DIR/cm4_audiostream.uart"
rm -f "$OUT"
"$QEMU" -M mimxrt1170-evk -global fsl-imxrt1170.boot-xip=on -kernel "$ELF" \
    -display none -serial file:"$OUT" -d guest_errors -D "$DIR/cm4_audiostream.dbg" &
P=$!; gate_pid $P
for _ in $(seq 1 40); do
    [ -f "$OUT" ] && grep -q "CM4AUDIOSTREAM-DONE" "$OUT" 2>/dev/null && break
    sleep 0.25
done
kill $P 2>/dev/null; wait $P 2>/dev/null || true

echo "==== captured ===="; [ -f "$OUT" ] && cat "$OUT" || echo "(no UART output)"
grep -q "CM4AUDIOSTREAM-DONE" "$OUT" || { echo "FAIL: no done"; exit 1; }
grep -q "flow=00000001" "$OUT" || { echo "FAIL: blocks did not flow in order"; exit 1; }
grep -q "noleak=00000001" "$OUT" || { echo "FAIL: audio pool leaked"; exit 1; }
grep -q "recv=00000008" "$OUT" || { echo "FAIL: sink did not receive 8 blocks"; exit 1; }
grep -q "done=D0DE0003" "$OUT" || { echo "FAIL: wrong done marker"; exit 1; }
grep -q "AUDIOSTREAM_CM4=PASS" "$OUT" || { echo "FAIL: no AUDIOSTREAM_CM4=PASS"; exit 1; }
echo "PASS: AUDIOSTREAM_CM4"
