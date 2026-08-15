#!/bin/sh
set -e
DIR=$(cd "$(dirname "$0")" && pwd)
# Tools come from THIS checkout, derived from the gate's own location.
EVKB=$(cd "$DIR/../../.." && pwd)
QEMU="$EVKB/tools/qrun"
# The script waits on AUDIO blocks rather than wall-clock delays, which is what
# makes its numbers host-speed-independent -- but it also means a loaded host
# stretches the run in wall time. 120 s and 300 poll iterations give that room.
# No assertion was relaxed to fit. Five measurement runs of 1500 blocks each is
# 21.8 s of audio, ~27 s of wall at the 0.81x rate measured under QEMU.
QRUN_TIMEOUT="${QRUN_TIMEOUT:-120}"; export QRUN_TIMEOUT
. "$EVKB/tools/gate-lib.sh"
gate_init
ELF="$DIR/$(gate_build_dir)/step_seq_test.elf"
OUT=$(gate_capture_path "$DIR" step_seq.uart)
DBG=$(gate_capture_path "$DIR" step_seq.dbg)
rm -f "$OUT" "$DBG"
"$QEMU" $(gate_qemu_machine) -kernel "$ELF" \
    -display none $(gate_console "$OUT") -d guest_errors -D "$DBG" &
P=$!; gate_pid $P
for _ in $(seq 1 300); do
    kill -0 "$P" 2>/dev/null || break
    n=$(grep -c 'SQ_ALIVE' "$OUT" 2>/dev/null || true)
    [ "${n:-0}" -ge 3 ] && break
    sleep 0.25
done
gate_reap $P
gate_require_capture "$OUT"
echo "==== captured ===="; cat "$OUT"
grep -q "STEPSEQ-GATE v1" "$OUT" || { echo "FAIL: banner"; exit 1; }
grep -q "STEPSEQ-DONE"    "$OUT" || { echo "FAIL: script never completed (truncated run)"; exit 1; }
grep -q "STEPS=PASS"      "$OUT" || { echo "FAIL: step count"; exit 1; }
grep -q "ACCENT=PASS"     "$OUT" || { echo "FAIL: accent velocity"; exit 1; }
grep -q "ORDER=PASS"      "$OUT" || { echo "FAIL: slide event order"; exit 1; }
grep -q "REST=PASS"       "$OUT" || { echo "FAIL: rest handling"; exit 1; }
grep -q "WRAP=PASS"       "$OUT" || { echo "FAIL: loop wrap"; exit 1; }
grep -q "SHORTLOOP=PASS"  "$OUT" || { echo "FAIL: short loop tail"; exit 1; }
# The pattern player is the audible half; assert it ran AND produced audio, and
# that the count is real rather than a wait that merely expired.
[ "$(grep -c 'SQ_ALIVE' "$OUT")" -ge 3 ] \
    || { echo "FAIL: fewer than 3 heartbeats -- loop() stopped or QEMU died early"; exit 1; }
grep -qE "SQ_ALIVE step=[0-9]+ beat=[0-9]+ peak=(0\.[0-9]*[1-9]|1\.)" "$OUT" \
    || { echo "FAIL: pattern player silent or not running"; exit 1; }
echo "PASS: STEPS ACCENT ORDER REST WRAP SHORTLOOP"
