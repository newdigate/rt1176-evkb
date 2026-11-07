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
ELF="$DIR/$(gate_build_dir)/tone_test.elf"
OUT=$(gate_capture_path "$DIR" tone.uart)
DBG=$(gate_capture_path "$DIR" tone.dbg)
rm -f "$OUT" "$DBG"
# -icount couples delay()/PIT to the guest instruction count so the run is
# deterministic. The shift is FIXED (5 = 32 ns per instruction), not `auto`:
# shift=auto re-fits the ratio at run time and the re-fit can land a jump in
# virtual time on the SAI frame timer, bunching DMA half-buffer ticks and
# dropping a block. Root-caused 2026-11-07 in sd_wav_play_test/run_qemu_sd_wav.sh
# (see the comment there); the same tap-then-compare pattern runs here.
"$QEMU" $(gate_qemu_machine) -kernel "$ELF" \
    -icount shift=5 \
    -display none $(gate_console "$OUT") -d guest_errors -D "$DBG" &
P=$!; gate_pid $P; sleep 20; gate_reap $P
gate_require_capture "$OUT"
echo "==== captured ===="; cat "$OUT"
grep -q "TONE=PASS" "$OUT" || { echo "FAIL: tone"; exit 1; }
echo "PASS: tone verified"
