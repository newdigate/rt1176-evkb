#!/bin/sh
# QEMU gate for Plan-2 Task 1: interrupt-driven SAI nodes on the CM7.
#
# World split (documented, not a weakening):
#  - QEMU asserts the DETERMINISTIC side: the SAI FIFO-request interrupt
#    clocks the whole graph (dispatches >= 600), zero underruns, TCSR.FEF
#    stays 0, WM8962 write-side ACK (stub), synth peak in range.
#  - Mic-level assertions are HW-ONLY: the QEMU model has no acoustic
#    source, and the graph clock is TX-driven -- with the TX FIFO-request
#    level held (no audio backend, imxrt_sai.c fidelity note) the main
#    thread is parked after I2SINT-DONE, so an rx-inject peak could never
#    be polled/printed anyway. The HW transcript must show MIC=PASS
#    (right-channel onboard mic) and a human hears 1 kHz on J101.
#
# Poll-loop runner (NOT a fixed sleep -- the audiooutput flake lesson):
# wait for I2SINT-DONE up to 40 x 0.25 s.
set -e
QEMU=~/Development/rt1170/evkb/tools/qrun
DIR=$(cd "$(dirname "$0")" && pwd)
. ~/Development/rt1170/evkb/tools/gate-lib.sh
gate_init
ELF="$DIR/build/i2s_int_test.elf"
OUT="$DIR/vcom.uart"
rm -f "$OUT" "$DIR/i2s_int.dbg"
"$QEMU" -M mimxrt1170-evk -global fsl-imxrt1170.boot-xip=on -kernel "$ELF" \
    -display none -serial file:"$OUT" -d guest_errors -D "$DIR/i2s_int.dbg" &
P=$!; gate_pid $P
for _ in $(seq 1 40); do
    [ -f "$OUT" ] && grep -q "I2SINT-DONE" "$OUT" 2>/dev/null && break
    sleep 0.25
done
kill $P 2>/dev/null; wait $P 2>/dev/null || true

echo "==== captured UART ===="; [ -f "$OUT" ] && cat "$OUT" || echo "(no UART output)"

fail=0
check() {
    if grep -q "^$1" "$OUT"; then echo "PASS: $1"; else echo "FAIL: expected $1"; fail=1; fi
}
grep -q "I2SINT-GATE v1" "$OUT" || { echo "FAIL: banner missing"; exit 1; }

# The ISR-clocked measurement window must have hit its dispatch target.
DISP=$(sed -n 's/^info dispatches=\([0-9][0-9]*\).*/\1/p' "$OUT" | head -1)
if [ -n "$DISP" ] && [ "$DISP" -ge 600 ]; then
    echo "PASS: dispatches=$DISP (>=600)"
else
    echo "FAIL: dispatches='$DISP' (need >=600)"; fail=1
fi
check "info underruns=0"     # the graph never failed to feed the ISR
check "info fef=0"           # no TX FIFO error (sticky TCSR.FEF)
check "info codec_ack=1"     # WM8962 write-side ACK (QEMU stub / real codec)
check "I2SINT=PASS"          # firmware verdict (incl. synth peak in range)
grep -q "I2SINT-DONE" "$OUT" || { echo "FAIL: DONE missing"; fail=1; }
# MIC lines are deliberately NOT asserted here (HW-only, see header).

if [ $fail -eq 0 ]; then
    echo "PASS: interrupt-driven SAI nodes verified in QEMU"
else
    echo "GATE FAILED"; exit 1
fi
