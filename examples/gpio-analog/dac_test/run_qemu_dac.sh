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
ELF="$DIR/build/dac_test.elf"; OUT="$DIR/dac.uart"
rm -f "$OUT"
"$QEMU" -M mimxrt1170-evk -global fsl-imxrt1170.boot-xip=on -kernel "$ELF" \
    -display none -serial file:"$OUT" -d guest_errors -D "$DIR/dac.dbg" &
P=$!; gate_pid $P; sleep 3; gate_reap $P
gate_require_capture "$OUT"
echo "==== captured ===="; cat "$OUT"
for k in cr_dacen=1 cr_dacrfs=1 cr_fifoen=0 wfp=4 rfp0=0 rfp1=1 irq=1 wmf=1 nemptf=1; do
  grep -q "$k OK" "$OUT" || { echo "FAIL: $k"; exit 1; }
done
grep -q "imxrt_dac: convert 0xfff" "$DIR/dac.dbg" || { echo "FAIL: 0xfff conversion trace"; exit 1; }
grep -q "imxrt_dac: convert 0x800" "$DIR/dac.dbg" || { echo "FAIL: 0x800 conversion trace"; exit 1; }
echo "PASS: DAC12 verified (config readback, FIFO pointers, watermark IRQ, conversion trace)"
