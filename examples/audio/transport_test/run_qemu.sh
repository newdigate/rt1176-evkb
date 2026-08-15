#!/bin/sh
set -e
DIR=$(cd "$(dirname "$0")" && pwd)
# Tools come from THIS checkout, derived from the gate's own location.
EVKB=$(cd "$DIR/../../.." && pwd)
QEMU="$EVKB/tools/qrun"
# The measurement script waits on AUDIO blocks rather than wall-clock delays,
# which is what makes its numbers host-speed-independent -- but it also means a
# loaded host stretches the run in wall time. 90 s and 200 poll iterations give
# that room. No assertion was relaxed to fit.
QRUN_TIMEOUT="${QRUN_TIMEOUT:-90}"; export QRUN_TIMEOUT
. "$EVKB/tools/gate-lib.sh"
gate_init
ELF="$DIR/$(gate_build_dir)/transport_test.elf"
OUT=$(gate_capture_path "$DIR" transport.uart)
DBG=$(gate_capture_path "$DIR" transport.dbg)
rm -f "$OUT" "$DBG"
"$QEMU" $(gate_qemu_machine) -kernel "$ELF" \
    -display none $(gate_console "$OUT") -d guest_errors -D "$DBG" &
P=$!; gate_pid $P
for _ in $(seq 1 200); do
    kill -0 "$P" 2>/dev/null || break
    n=$(grep -c 'TR_ALIVE' "$OUT" 2>/dev/null || true)
    [ "${n:-0}" -ge 3 ] && break
    sleep 0.25
done
gate_reap $P
gate_require_capture "$OUT"
echo "==== captured ===="; cat "$OUT"
grep -q "TRANSPORT-GATE v1" "$OUT" || { echo "FAIL: banner"; exit 1; }
grep -q "TRANSPORT-DONE"    "$OUT" || { echo "FAIL: script never completed (truncated run)"; exit 1; }
grep -q "TRANSPORT_ORDER=PASS" "$OUT" || { echo "FAIL: update order"; exit 1; }
grep -q "TEMPO=PASS"        "$OUT" || { echo "FAIL: tempo"; exit 1; }
grep -q "LOOP=PASS"         "$OUT" || { echo "FAIL: loop seam"; exit 1; }
grep -q "ELAPSED=PASS"      "$OUT" || { echo "FAIL: elapsed vs song position"; exit 1; }
grep -q "TEMPOCHANGE=PASS"  "$OUT" || { echo "FAIL: tempo change"; exit 1; }
grep -q "STATE=PASS"        "$OUT" || { echo "FAIL: play/pause/stop"; exit 1; }
# The pattern player is the audible half; assert it actually ran and produced
# audio, and that the count is real rather than a wait that merely expired.
[ "$(grep -c 'TR_ALIVE' "$OUT")" -ge 3 ] \
    || { echo "FAIL: fewer than 3 heartbeats -- loop() stopped or QEMU died early"; exit 1; }
grep -qE "TR_ALIVE bar=[0-9]+ beat=[0-9]+ peak=(0\.[0-9]*[1-9]|1\.)" "$OUT" \
    || { echo "FAIL: pattern player silent or not running"; exit 1; }
echo "PASS: TRANSPORT_ORDER TEMPO LOOP ELAPSED TEMPOCHANGE STATE"
