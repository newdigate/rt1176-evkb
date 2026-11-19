#!/bin/bash
# synthui_waveform_test gate: SynthUI's waveform widget on the landscape
# (rotated present) pipeline.  Nine fixed states, each a golden FNV-1a of the
# scanned-out frame; the grid-thinning bound; and the cleared widget must
# return to the empty state's frame.  Goldens: see transcript_qemu.txt.
set -e
DIR=$(cd "$(dirname "$0")" && pwd)
EVKB=$(cd "$DIR/../../.." && pwd)
QEMU="$EVKB/tools/qrun"
. "$EVKB/tools/gate-lib.sh"
gate_init
ELF="$DIR/$(gate_build_dir)/synthui_waveform_test.elf"
OUT=$(gate_capture_path "$DIR" synthui_waveform.uart)
DBG=$(gate_capture_path "$DIR" synthui_waveform.dbg)
rm -f "$OUT" "$DBG"
"$QEMU" $(gate_qemu_machine) -kernel "$ELF" \
    -display none -serial file:"$OUT" -d guest_errors -D "$DBG" &
P=$!; gate_pid $P
for _ in $(seq 1 720); do
    [ -f "$OUT" ] && grep -q "crc_done" "$OUT" 2>/dev/null && break
    sleep 0.25
done
gate_reap $P
gate_require_capture "$OUT"
echo "==== captured UART ===="; cat "$OUT"
grep -q "PANEL_OK" "$OUT" || { echo "FAIL: panel bring-up"; exit 1; }
grep -q "wf_blit_err" "$OUT" && { echo "FAIL: a PXP blit failed"; exit 1; }
crc() { grep -a -oE "wf_crc\[$1\]=0x[0-9A-F]{8}" "$OUT" | head -1 | cut -d= -f2; }
EMPTY=$(crc empty)
[ -n "$EMPTY" ] || { echo "FAIL: no empty-state checksum"; exit 1; }
for s in fit mid close loading free cue end grid; do
    C=$(crc $s)
    [ -n "$C" ] || { echo "FAIL: state $s missing"; exit 1; }
    [ "$C" != "$EMPTY" ] || { echo "FAIL: state $s drew nothing (equals the empty frame)"; exit 1; }
done
# Goldens (QEMU 2026-11-19; the Nosh board reads the same values).
grep -qE "wf_crc\\[empty\\]=0x4B74D5C5\r?$" "$OUT" || { echo "FAIL: golden empty (want 0x4B74D5C5, got $(crc empty))"; exit 1; }
grep -qE "wf_crc\\[fit\\]=0x7E10C925\r?$" "$OUT" || { echo "FAIL: golden fit (want 0x7E10C925, got $(crc fit))"; exit 1; }
grep -qE "wf_crc\\[mid\\]=0x654AD956\r?$" "$OUT" || { echo "FAIL: golden mid (want 0x654AD956, got $(crc mid))"; exit 1; }
grep -qE "wf_crc\\[close\\]=0x3F0E643C\r?$" "$OUT" || { echo "FAIL: golden close (want 0x3F0E643C, got $(crc close))"; exit 1; }
grep -qE "wf_crc\\[loading\\]=0x2A12C371\r?$" "$OUT" || { echo "FAIL: golden loading (want 0x2A12C371, got $(crc loading))"; exit 1; }
grep -qE "wf_crc\\[free\\]=0xB89D3536\r?$" "$OUT" || { echo "FAIL: golden free (want 0xB89D3536, got $(crc free))"; exit 1; }
grep -qE "wf_crc\\[cue\\]=0x00E972FF\r?$" "$OUT" || { echo "FAIL: golden cue (want 0x00E972FF, got $(crc cue))"; exit 1; }
grep -qE "wf_crc\\[end\\]=0x843DE617\r?$" "$OUT" || { echo "FAIL: golden end (want 0x843DE617, got $(crc end))"; exit 1; }
grep -qE "wf_crc\\[grid\\]=0x28A0FC77\r?$" "$OUT" || { echo "FAIL: golden grid (want 0x28A0FC77, got $(crc grid))"; exit 1; }
GL=$(grep -a -oE "wf_grid_lines=[0-9]+" "$OUT" | head -1 | cut -d= -f2)
[ -n "$GL" ] && [ "$GL" -gt 0 ] && [ "$GL" -le 184 ] || { echo "FAIL: grid thinning (lines=$GL, want 1..184)"; exit 1; }
CL=$(grep -a -oE "wf_crc_cleared=0x[0-9A-F]{8}" "$OUT" | head -1 | cut -d= -f2)
[ "$CL" = "$EMPTY" ] || { echo "FAIL: clear() does not return to the empty frame ($CL vs $EMPTY)"; exit 1; }
grep -q "crc_done" "$OUT" || { echo "FAIL: no completion token"; exit 1; }
echo "PASS: SynthUI waveform render verified"
