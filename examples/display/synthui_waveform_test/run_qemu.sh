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
SCRIPT="$DIR/touch_script.txt"
[ -f "$SCRIPT" ] || { echo "FAIL: touch script missing at $SCRIPT"; exit 1; }
"$QEMU" $(gate_qemu_machine) -kernel "$ELF" \
    -display none -serial file:"$OUT" \
    -global driver=imxrt.gt911,property=touch-script,value="$SCRIPT" \
    -d guest_errors -D "$DBG" &
P=$!; gate_pid $P
for _ in $(seq 1 1200); do
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
# --- touch phase (expected values: gen_touch_script.py prints them) ---
grep -q "I2C_OK" "$OUT" || { echo "FAIL: touch bring-up"; exit 1; }
grep -q "wf_timeout" "$OUT" && { echo "FAIL: a scripted gesture never arrived: $(grep -a wf_timeout "$OUT" | head -1)"; exit 1; }
[ "$(grep -a -c "wf_ev=GRAB_BEGIN frame=7938000" "$OUT")" = 2 ] || { echo "FAIL: GRAB_BEGIN (want two, at the head frame)"; exit 1; }
LASTMOVE=$(tr -d '\r' < "$OUT" | awk '/wf_ev=GRAB_END/{exit} /wf_ev=GRAB_MOVE/{sub(/.*frame=/,""); m=$0} END{print m}')
[ "$LASTMOVE" = 7729104 ] || { echo "FAIL: last GRAB_MOVE of the first drag = $LASTMOVE, want 7729104"; exit 1; }
grep -a -q "wf_ev=GRAB_END frame=7729104" "$OUT" || { echo "FAIL: GRAB_END of the first drag"; exit 1; }
grep -a -q "wf_ev=SEEK frame=3940650" "$OUT" || { echo "FAIL: SEEK frame"; exit 1; }
grep -a -qE "wf_scroll_delta=-208896\r?$" "$OUT" || { echo "FAIL: free-view scroll ($(grep -a wf_scroll_delta "$OUT"))"; exit 1; }
[ "$(tr -d '\r' < "$OUT" | awk '/wf_ev=SEEK/{s=1} s&&/wf_scroll_delta/{exit} s&&/wf_ev=GRAB/{n++} END{print n+0}')" = 0 ] || { echo "FAIL: a grab event during the tracking-off drag"; exit 1; }
[ "$(grep -a -c "wf_ev=TRACK_REQUEST" "$OUT")" = 1 ] || { echo "FAIL: TRACK_REQUEST count"; exit 1; }
grep -a -qE "wf_press_lost begins=2 ends=2\r?$" "$OUT" || { echo "FAIL: PRESS_LOST must end the grab exactly once ($(grep -a wf_press_lost "$OUT"))"; exit 1; }
[ "$(tr -d '\r' < "$OUT" | awk '/wf_cleared_begin/{s=1} /wf_cleared_end/{s=0} s&&/wf_ev=/{n++} END{print n+0}')" = 0 ] || { echo "FAIL: events from a cleared widget"; exit 1; }
grep -q "wf_cleared_end" "$OUT" || { echo "FAIL: touch phase did not finish"; exit 1; }
grep -q "crc_done" "$OUT" || { echo "FAIL: no completion token"; exit 1; }
echo "PASS: SynthUI waveform render verified"
