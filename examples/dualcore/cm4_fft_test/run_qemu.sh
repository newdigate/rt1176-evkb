#!/bin/sh
# QEMU gate for cm4_fft_test: CMSIS-DSP known-answer stages running ON the M4
# (FFT bin-8, FIR impulse echo, arm_sin_q31 spot check). Pure-CPU math, so a
# green QEMU run is meaningful; the EVKB run confirms on silicon.
set -e
QEMU=~/Development/rt1170/evkb/tools/qrun
DIR=$(cd "$(dirname "$0")" && pwd)
. ~/Development/rt1170/evkb/tools/gate-lib.sh
gate_init
ELF="$DIR/build/cm4_fft_test.elf"
OUT="$DIR/cm4_fft.uart"
rm -f "$OUT"
"$QEMU" -M mimxrt1170-evk -global fsl-imxrt1170.boot-xip=on -kernel "$ELF" \
    -display none -serial file:"$OUT" -d guest_errors -D "$DIR/cm4_fft.dbg" &
P=$!; gate_pid $P
for _ in $(seq 1 40); do
    [ -f "$OUT" ] && grep -q "CM4FFT-DONE" "$OUT" 2>/dev/null && break
    sleep 0.25
done
kill $P 2>/dev/null; wait $P 2>/dev/null || true

echo "==== captured ===="; [ -f "$OUT" ] && cat "$OUT" || echo "(no UART output)"
grep -q "CM4FFT-DONE" "$OUT" || { echo "FAIL: no done"; exit 1; }
grep -q "fft_bin=00000008" "$OUT" || { echo "FAIL: FFT energy not in bin 8"; exit 1; }
grep -q "fir_ok=00000001" "$OUT" || { echo "FAIL: FIR echo"; exit 1; }
grep -q "sin_ok=00000001" "$OUT" || { echo "FAIL: arm_sin_q31 known answers"; exit 1; }
grep -q "done=D0DE0004" "$OUT" || { echo "FAIL: wrong done marker"; exit 1; }
grep -q "FFT_CM4=PASS" "$OUT" || { echo "FAIL: no FFT_CM4=PASS"; exit 1; }
echo "PASS: FFT_CM4"
