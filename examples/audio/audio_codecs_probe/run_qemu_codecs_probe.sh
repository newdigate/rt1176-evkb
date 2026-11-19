#!/bin/bash
# audio_codecs_probe gate: the four decoders bit-exact against the host, the
# corrupt-input cases, spectrum and tempo (120 BPM click), on the RT1176.
set -e
DIR=$(cd "$(dirname "$0")" && pwd)
EVKB=$(cd "$DIR/../../.." && pwd)
QEMU="$EVKB/tools/qrun"
. "$EVKB/tools/gate-lib.sh"
gate_init
ELF="$DIR/$(gate_build_dir)/audio_codecs_probe.elf"
OUT=$(gate_capture_path "$DIR" codecs_probe.uart)
DBG=$(gate_capture_path "$DIR" codecs_probe.dbg)
rm -f "$OUT" "$DBG"
QRUN_TIMEOUT=${QRUN_TIMEOUT:-600}; export QRUN_TIMEOUT
"$QEMU" $(gate_qemu_machine) -kernel "$ELF" \
    -display none $(gate_console "$OUT") -d guest_errors -D "$DBG" &
P=$!; gate_pid $P
for _ in $(seq 1 2200); do
    [ -f "$OUT" ] && grep -q "CODECS_PROBE=" "$OUT" 2>/dev/null && break
    sleep 0.25
done
gate_reap $P
gate_require_capture "$OUT"
echo "==== captured ===="; cat "$OUT"
for f in "wav short.wav" "aiff short.aiff" "flac click.flac" "mp3 click.mp3" "wav short24.wav"; do
    grep -a -qE "DEC $f .* PASS" "$OUT" || { echo "FAIL: decode $f"; exit 1; }
done
grep -a -qE "DEC wav short24.wav .* bits=24 " "$OUT" || { echo "FAIL: the 24-bit clip was not seen as 24-bit"; exit 1; }
[ "$(grep -a -c "^DEC_BAD .* PASS" "$OUT")" = 7 ] || { echo "FAIL: corrupt-input cases (want 7 PASS)"; exit 1; }
grep -a -q "FAIL" "$OUT" && { echo "FAIL: a stage failed: $(grep -a FAIL "$OUT" | head -1)"; exit 1; }
true
grep -a -qE "SPECTRUM bytes=[1-9][0-9]* .* PASS" "$OUT" || { echo "FAIL: spectrum"; exit 1; }
grep -a -qE "^MP3_MATCH .* PASS" "$OUT" || { echo "FAIL: MP3 decode differs from the host beyond tolerance"; exit 1; }
# Tempo must equal the host's result for the same PCM (clips.h); the line carries both.
grep -a -qE "^TEMPO bpm_x100=([0-9]+) beats=([0-9]+) host_bpm_x100=\1 host_beats=\2 ms=[0-9]+ PASS" "$OUT" || { echo "FAIL: tempo differs from the host ($(grep -a '^TEMPO ' "$OUT" | cut -c1-90))"; exit 1; }
[ "$(grep -a -c "^TEMPO_EDGE .* PASS" "$OUT")" = 2 ] || { echo "FAIL: tempo edge cases"; exit 1; }
grep -a -qE "^TEMPO_INFO asv_same=1 att_same=1 " "$OUT" || { echo "FAIL: the analysis files are not byte-identical to the host's"; exit 1; }
grep -a -q "STACK_PEAK=" "$OUT" || { echo "FAIL: no stack figure"; exit 1; }
grep -q "CODECS_PROBE=PASS" "$OUT" || { echo "FAIL: overall"; exit 1; }
echo "PASS: CODECS_PROBE"
