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
ELF="$DIR/$(gate_build_dir)/sai_rx_test.elf"
VCOM="$DIR/vcom.uart"; DBG="$DIR/sai_rx.dbg"; INJ="$DIR/inject.raw"; TAP="$DIR/tap.raw"
python3 "$DIR/gen_inject.py" "$INJ"
rm -f "$VCOM" "$DBG" "$TAP"
gate_tmp "$INJ" "$INJ.fifo" "$INJ.fifo.in" "$INJ.fifo.out"
# NOTE: this QEMU build's "file" chardev backend requires path= (the write/out
# side) even when only input-path= is given ("chardev: file: no filename
# given"), so plain input-path=$INJ does not work here. Fall back to a fifo:
# pump the injector file into a named pipe and point the chardev at that.
rm -f "$INJ.fifo"; mkfifo "$INJ.fifo"
( cat "$INJ" > "$INJ.fifo" 2>/dev/null ) & gate_pid $!
"$QEMU" $(gate_qemu_machine) -kernel "$ELF" \
    -display none -serial file:"$VCOM" \
    -chardev pipe,id=sai1-rxinject,path="$INJ.fifo" \
    -chardev file,id=sai1-tap,path="$TAP" \
    -d guest_errors -D "$DBG" &
P=$!; gate_pid $P
sleep 6; gate_reap $P
gate_require_capture "$VCOM"
rm -f "$INJ.fifo.in" "$INJ.fifo.out" "$INJ.fifo"
echo "==== VCOM ===="; cat "$VCOM"
grep -q "STAGE_A_PASS" "$VCOM" || { echo "FAIL: stage A polled read"; exit 1; }
echo "PASS: stage A"
grep -q "STAGE_B_DONE" "$VCOM" || { echo "FAIL: stage B not reached"; exit 1; }
grep -q "STAGE_B_PASS" "$VCOM" || { echo "FAIL: stage B DMA capture"; exit 1; }
echo "PASS: stage B"
grep -q "STAGE_C_DONE" "$VCOM" || { echo "FAIL: stage C not reached"; exit 1; }
python3 "$DIR/check_tap.py" "$TAP" || { echo "FAIL: stage C TX tap mismatch"; exit 1; }
grep -q "STAGE_FD_PASS" "$VCOM" || { echo "FAIL: full-duplex block counts"; exit 1; }
echo "PASS: SAI_RX_ALL (A+B+C)"
