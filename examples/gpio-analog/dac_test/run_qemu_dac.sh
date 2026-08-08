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
ELF="$DIR/$(gate_build_dir)/dac_test.elf"; OUT="$DIR/dac.uart"
rm -f "$OUT"
"$QEMU" $(gate_qemu_machine) -kernel "$ELF" \
    -display none -serial file:"$OUT" -d guest_errors -D "$DIR/dac.dbg" &
P=$!; gate_pid $P
# Poll for the run's terminal token instead of guessing a duration. The fixed
# `sleep 3` this replaces made the gate LOAD-SENSITIVE: on a busy machine QEMU
# had not written a byte before the reap, and the run failed with "no UART
# capture" -- a red that says nothing about the firmware and passes on retry.
# Observed repeatedly during the 2026-07-29 sweep. Same idiom as the dualcore
# gates. 40 x 0.25s bounds it at 10s; a healthy run breaks out well inside 3s.
#
# "[dacloop-exp] done" closes the value sweep, so it lands after all nine "X OK"
# config tokens (capture lines 2-10 vs 33) AND after the 0xfff/0x800 conversions
# the .dbg assertions below look for -- both values are swept before it. The
# dac= lines that follow are the idle loop.
for _ in $(seq 1 40); do
    [ -f "$OUT" ] && grep -q "\[dacloop-exp\] done" "$OUT" 2>/dev/null && break
    sleep 0.25
done
gate_reap $P
gate_require_capture "$OUT"
echo "==== captured ===="; cat "$OUT"
for k in cr_dacen=1 cr_dacrfs=1 cr_fifoen=0 wfp=4 rfp0=0 rfp1=1 irq=1 wmf=1 nemptf=1; do
  grep -q "$k OK" "$OUT" || { echo "FAIL: $k"; exit 1; }
done
grep -q "imxrt_dac: convert 0xfff" "$DIR/dac.dbg" || { echo "FAIL: 0xfff conversion trace"; exit 1; }
grep -q "imxrt_dac: convert 0x800" "$DIR/dac.dbg" || { echo "FAIL: 0x800 conversion trace"; exit 1; }
echo "PASS: DAC12 verified (config readback, FIFO pointers, watermark IRQ, conversion trace)"
