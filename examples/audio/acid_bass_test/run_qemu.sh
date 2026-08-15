#!/bin/sh
set -e
DIR=$(cd "$(dirname "$0")" && pwd)
# Tools come from THIS checkout, derived from the gate's own location.
EVKB=$(cd "$DIR/../../.." && pwd)
QEMU="$EVKB/tools/qrun"
# The measurement script waits on AUDIO blocks rather than wall-clock delays,
# which is what makes its numbers host-speed-independent -- but it also means a
# loaded host stretches the run in wall time. 90 s (vs qrun's 60 s default) and
# 200 poll iterations give that room. No assertion was relaxed to fit.
QRUN_TIMEOUT="${QRUN_TIMEOUT:-90}"; export QRUN_TIMEOUT
. "$EVKB/tools/gate-lib.sh"
gate_init
ELF="$DIR/$(gate_build_dir)/acid_bass_test.elf"
# Per-run artifacts go through gate_capture_path (gate-lib.sh:131), never
# "$DIR/<name>" -- a shared path is what let one board half of a two-board
# example `rm -f` the other half's LIVE capture under `-j`. This example is
# rt1176-only today, but the rule is not conditional on that.
OUT=$(gate_capture_path "$DIR" acid_bass.uart)
DBG=$(gate_capture_path "$DIR" acid_bass.dbg)
rm -f "$OUT" "$DBG"
# gate_console picks the console slot for the board rather than hardcoding
# `-serial file:` (gate-lib.sh:102-107) -- the failure it guards is silent, an
# empty capture being indistinguishable from firmware that never ran.
"$QEMU" $(gate_qemu_machine) -kernel "$ELF" \
    -display none $(gate_console "$OUT") -d guest_errors -D "$DBG" &
P=$!; gate_pid $P
# Wait for THREE heartbeats, not for ACID-DONE. Breaking on ACID-DONE reaps the
# instant setup() ends, so loop() -- the pattern player, i.e. the entire audible
# half -- never runs in QEMU, and the committed transcript ends with however
# many heartbeats happened to slip out before the kill (0-2, nondeterministic).
# Three costs ~1 s of guest time and makes the tail deterministic.
for _ in $(seq 1 200); do
    # `|| true` + ${n:-0}: grep exits 1 on zero matches and prints nothing at
    # all when the file does not exist yet, and a bare "$(...)" would then make
    # the test `[ "" -ge 3 ]` -- "integer expression expected" on stderr once
    # per iteration. Same defaulting idiom as run_qemu_audiooutput.sh.
    n=$(grep -c 'ACID_ALIVE' "$OUT" 2>/dev/null || true)
    [ "${n:-0}" -ge 3 ] && break
    # A dead QEMU ends the wait now instead of burning the full cap.
    kill -0 "$P" 2>/dev/null || break
    sleep 0.25
done
gate_reap $P
gate_require_capture "$OUT"
echo "==== captured ===="; cat "$OUT"
grep -q "ACID-GATE v1" "$OUT" || { echo "FAIL: banner"; exit 1; }
# Completion is checked BEFORE the verdicts so a run that hung mid-script is
# diagnosed as truncated rather than blamed on whichever check happens to be
# first ("FAIL: accent" for a firmware that never reached the accent test).
grep -q "ACID-DONE" "$OUT" || { echo "FAIL: script never completed (truncated run)"; exit 1; }
grep -q "ACID_ACCENT=PASS" "$OUT" || { echo "FAIL: accent"; exit 1; }
# Both accent checks, not just the first. ACID_ACCENT is dominated by the filter
# sweep and passes at 2.70 with accent's VCA term deleted outright; ACID_ACCENT_VCA
# is the half that covers. Dropping either leaves half of accent() unasserted.
grep -q "ACID_ACCENT_VCA=PASS" "$OUT" || { echo "FAIL: accent vca"; exit 1; }
grep -q "ACID_SLIDE=PASS"  "$OUT" || { echo "FAIL: slide"; exit 1; }
grep -q "ACID_DECAY=PASS"  "$OUT" || { echo "FAIL: decay"; exit 1; }
# The pattern player must be running AND producing signal. The nonzero-digit
# requirement is the point: `peak=0\.[0-9]` would match "peak=0.0000", so a
# silent loop() would satisfy it. This accepts 0.0001-0.9999 and 1.x, rejects
# an exact 0.0000 and rejects "peak=(no update)" (a graph that stopped
# clocking, which is what a broken SAI1 TX DMA looks like from here).
grep -qE "ACID_ALIVE peak=(0\.[0-9]*[1-9]|1\.)" "$OUT" \
    || { echo "FAIL: pattern player silent or not running"; exit 1; }
# The 3-heartbeat loop above is a WAIT CONDITION, not an assertion: it also
# exits when QEMU dies (kill -0) and when the 30 s cap expires. So arriving
# here does NOT mean three heartbeats were captured -- a loop() that hard-faults
# after the first one leaves a single nonzero ACID_ALIVE, caps out the wait, and
# satisfies every check above. Assert the count separately, or the coverage the
# wait was added for is satisfied by a run that died.
[ "$(grep -c 'ACID_ALIVE' "$OUT")" -ge 3 ] \
    || { echo "FAIL: fewer than 3 heartbeats -- loop() stopped or QEMU died early"; exit 1; }
echo "PASS: ACID_ACCENT ACID_ACCENT_VCA ACID_SLIDE ACID_DECAY ACID_ALIVE"
