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
ELF="$DIR/$(gate_build_dir)/audioinput_i2s_test.elf"
VCOM="$DIR/vcom.uart"; DBG="$DIR/audioinput.dbg"; INJ="$DIR/inject.raw"
python3 "$DIR/gen_inject.py" "$INJ"
rm -f "$VCOM" "$DBG"
gate_tmp "$INJ" "$INJ.fifo" "$INJ.fifo.in" "$INJ.fifo.out"
# NOTE: this QEMU build's "file" chardev backend requires path= (the write/out
# side) even when only input-path= is given ("chardev: file: no filename
# given"), so plain input-path=$INJ does not work here. Fall back to a fifo:
# pump the injector file into a named pipe and point the chardev at that.
#
# Unlike sai_rx_test's single cat (that gate only needed a handful of blocks
# total across its whole run), this gate polls for 500ms of *guest* time and
# the SAI model's rx_timer drains the ring continuously the whole time QEMU is
# up -- so loop the pump so the fifo never runs dry mid-poll (the reader side
# blocks between loop iterations, which is fine: a fifo write() only unblocks
# once the guest-side drain has room again).
rm -f "$INJ.fifo"; mkfifo "$INJ.fifo"
( while true; do cat "$INJ" > "$INJ.fifo" 2>/dev/null; done ) & PUMP_PID=$!
gate_pid $PUMP_PID
"$QEMU" $(gate_qemu_machine) -kernel "$ELF" \
    -display none -serial file:"$VCOM" \
    -chardev pipe,id=sai1-rxinject,path="$INJ.fifo" \
    -d guest_errors -D "$DBG" &
P=$!; gate_pid $P
sleep 5; gate_reap $P
gate_require_capture "$VCOM"
kill $PUMP_PID 2>/dev/null || true
rm -f "$INJ.fifo.in" "$INJ.fifo.out" "$INJ.fifo"
echo "==== VCOM ===="; cat "$VCOM"
grep -q "^info peak=" "$VCOM" || { echo "FAIL: no info peak= line"; exit 1; }
grep "^info peak=" "$VCOM"
grep -q "STAGE_PEAK=PASS" "$VCOM" || { echo "FAIL: STAGE_PEAK"; exit 1; }
grep -q "AUDIOINPUT_ALL=PASS" "$VCOM" || { echo "FAIL: AUDIOINPUT_ALL"; exit 1; }
echo "PASS: AUDIOINPUT_ALL"
