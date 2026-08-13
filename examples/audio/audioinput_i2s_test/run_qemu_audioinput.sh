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
VCOM=$(gate_capture_path "$DIR" vcom.uart)
DBG=$(gate_capture_path "$DIR" audioinput.dbg)
INJ=$(gate_capture_path "$DIR" inject.raw)
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
# Redirect OUTSIDE the loop: one blocking open() at subshell start, so killing
# the pump cannot leave an orphaned `cat` blocked opening a fifo that is about
# to be unlinked. Any child that outlives the kill is then reading a plain file
# and exits immediately.
( while :; do cat "$INJ"; done ) > "$INJ.fifo" 2>/dev/null & PUMP_PID=$!
gate_pid $PUMP_PID
"$QEMU" $(gate_qemu_machine) -kernel "$ELF" \
    -display none $(gate_console "$VCOM") \
    -chardev pipe,id=sai1-rxinject,path="$INJ.fifo" \
    -d guest_errors -D "$DBG" &
P=$!; gate_pid $P
# Poll for the verdict token rather than guessing a duration -- a fixed sleep
# makes the gate load-sensitive, which has cost this tree real time. Every
# assertion below reads the console, so wait on the LAST line the firmware
# prints: AUDIOINPUT_ALL=, not the STAGE_PEAK= directly above it. Polling the
# penultimate token races the final println -- a tick landing between the two
# reaps QEMU before AUDIOINPUT_ALL= is written, and the gate then fails on an
# assertion the run actually satisfied, with STAGE_PEAK=PASS sitting right above
# it in the transcript. Match the `=` so a FAIL verdict ends the wait too and is
# reported by name below, instead of spinning to the cap and being blamed on the
# timeout. The cap is a diagnosis aid, not an assertion; a QEMU that died early
# ends the wait immediately rather than eating the full 20 s.
for _ in $(seq 1 80); do
    [ -f "$VCOM" ] && grep -q "AUDIOINPUT_ALL=" "$VCOM" 2>/dev/null && break
    kill -0 "$P" 2>/dev/null || break
    sleep 0.25
done
gate_reap $P
gate_require_capture "$VCOM"
kill $PUMP_PID 2>/dev/null || true
rm -f "$INJ.fifo.in" "$INJ.fifo.out" "$INJ.fifo"
echo "==== VCOM ===="; cat "$VCOM"
grep -q "^info peak=" "$VCOM" || { echo "FAIL: no info peak= line"; exit 1; }
grep "^info peak=" "$VCOM"
# Oracle on the INJECTED WAVEFORM, not just on "some signal arrived":
# gen_inject.py's crest is 0x6000 = 24576, and 24576/32767 = 0.7500. The
# firmware's own threshold is a loose > 0.02f because the same sketch prints a
# live mic peak on silicon; the gate can be exact because the injector is.
grep -q "^info peak=0\.7500" "$VCOM" \
    || { echo "FAIL: peak is not the injected waveform's 0.7500"; exit 1; }
grep -q "STAGE_PEAK=PASS" "$VCOM" || { echo "FAIL: STAGE_PEAK"; exit 1; }
grep -q "AUDIOINPUT_ALL=PASS" "$VCOM" || { echo "FAIL: AUDIOINPUT_ALL"; exit 1; }
echo "PASS: AUDIOINPUT_ALL"
