#!/bin/bash
# dj_deck auto-next gate (Nosh only).  /music on the card holds, in list order,
# a_one.wav (a 2 s clip), b_bad.wav (not audio at all) and c_two.wav (the clip
# again), plus a hidden file and a text file that must never be picked.  The
# script loads a_one.wav, switches AUTO on and presses PLAY.  From there the
# deck must go on by itself.  The clip is shorter than the 5 s lead, so the
# next file is loaded at once, beside the one that is playing: b_bad is tried
# and skipped, c_two is loaded, and when a_one has played out c_two goes on
# the deck and plays with no load in between.  After it the folder has no more.
set -e
DIR=$(cd "$(dirname "$0")" && pwd)
EVKB=$(cd "$DIR/../../.." && pwd)
QEMU="$EVKB/tools/qrun"
. "$EVKB/tools/gate-lib.sh"
gate_init
ELF="$DIR/$(gate_build_dir)/dj_deck.elf"
[ -f "$ELF" ] || { echo "FAIL: no ELF ($ELF) -- build first"; exit 1; }
SCRIPT="$DIR/touch_script_auto.txt"
[ -f "$SCRIPT" ] || { echo "FAIL: touch script missing at $SCRIPT"; exit 1; }
WAV=$(gate_capture_path "$DIR" auto_clip.wav); IMG=$(gate_capture_path "$DIR" auto_card.img)
DBG=$(gate_capture_path "$DIR" dj_deck_auto.dbg); OUT=$(gate_capture_path "$DIR" dj_deck_auto.uart)
rm -f "$WAV" "$IMG" "$DBG" "$OUT"

# 2 s of the main gate's click track.
ffmpeg -hide_banner -loglevel error -y \
    -f lavfi -i "anoisesrc=color=pink:amplitude=0.02:sample_rate=44100:duration=2:seed=7" \
    -f lavfi -i "sine=frequency=1000:sample_rate=44100:duration=2" \
    -filter_complex "[1:a]volume='if(lt(mod(t,0.5),0.02),0.8,0)':eval=frame[c];[0:a][c]amix=inputs=2:normalize=0,pan=stereo|c0=c0|c1=0.8*c0[o]" \
    -map "[o]" -c:a pcm_s16le -bitexact "$WAV"

mkfile -n 512m "$IMG"
DISK=$(hdiutil attach -nomount -imagekey diskimage-class=CRawDiskImage "$IMG" | head -1 | awk '{print $1}')
[ -n "$DISK" ] || { echo "FAIL: attach"; exit 1; }
diskutil partitionDisk "$DISK" 1 MBR "MS-DOS FAT32" DJAUTO 100% >/dev/null \
    || { hdiutil detach "$DISK" >/dev/null 2>&1 || true; echo "FAIL: partition"; exit 1; }
MNT=$(diskutil info "${DISK}s1" | sed -n 's/.*Mount Point: *//p')
mkdir -p "$MNT/music"
cp "$WAV" "$MNT/music/a_one.wav"; cp "$WAV" "$MNT/music/c_two.wav"; cp "$WAV" "$MNT/music/.hidden.wav"
echo "this is not a wave file, whatever its name says" > "$MNT/music/b_bad.wav"
echo "not audio" > "$MNT/music/notes.txt"
sync; diskutil eject "$DISK" >/dev/null

"$QEMU" $(gate_qemu_machine) -kernel "$ELF" \
    -display none $(gate_console "$OUT") \
    $(gate_sd_drive "$IMG") \
    -global driver=imxrt.gt911,property=touch-script,value="$SCRIPT" \
    -d guest_errors -D "$DBG" &
P=$!; gate_pid $P
for _ in $(seq 1 1440); do
    [ -f "$OUT" ] && grep -a -q "^AUTO_END\|LVGL_ASSERT" "$OUT" 2>/dev/null && break
    sleep 0.25
done
sleep 1; gate_reap $P
gate_require_capture "$OUT"
echo "==== UART ===="; cat "$OUT"

L=0
for p in "^SD_OK" "^DJ_READY" "^BROWSE dir=/ n=1" "^BROWSE dir=/music n=3" "^BROWSE_PICK /music/a_one.wav" \
         "^LOAD_START /music/a_one.wav" "^FORMAT rate=44100 ch=2 bits=16 frames=88200" "^LOAD_DONE" "^UI_AUTO=1" "^UI_PLAY=1" \
         "^AUTO_PRELOAD /music/b_bad.wav" "^LOAD_START /music/b_bad.wav" "^LOAD_FAIL" "^AUTO_SKIP /music/b_bad.wav" \
         "^AUTO_PRELOAD /music/c_two.wav" "^LOAD_START /music/c_two.wav" "^FORMAT rate=44100 ch=2 bits=16 frames=88200" \
         "^AUTO_NEXT /music/c_two.wav" "^AUTO_PLAY" "^AUTO_END"; do
    N=$(grep -a -nE "$p" "$OUT" | awk -F: -v l="$L" '$1 > l { print $1; exit }')
    [ -n "$N" ] || { echo "FAIL: '$p' missing or out of order"; exit 1; }
    L=$N
done
grep -a -q "LVGL_ASSERT" "$OUT" && { echo "FAIL: $(grep -a "LVGL_ASSERT" "$OUT" | head -1)"; exit 1; }
# c_two.wav must have loaded completely before the end of the folder was reported
N=$(grep -a -n "^LOAD_DONE" "$OUT" | tail -1 | cut -d: -f1)
[ "$N" -gt "$(grep -a -n "^AUTO_PRELOAD /music/c_two.wav" "$OUT" | cut -d: -f1)" ] || { echo "FAIL: c_two.wav never finished loading"; exit 1; }
# exactly two early loads, one change of track, one skip, one failure, three loads in all, and nothing hidden or non-audio
[ "$(grep -a -c "^AUTO_PRELOAD" "$OUT")" = 2 ] || { echo "FAIL: want 2 AUTO_PRELOAD, got $(grep -a -c "^AUTO_PRELOAD" "$OUT")"; exit 1; }
[ "$(grep -a -c "^AUTO_NEXT" "$OUT")" = 1 ] || { echo "FAIL: want 1 AUTO_NEXT, got $(grep -a -c "^AUTO_NEXT" "$OUT")"; exit 1; }
[ "$(grep -a -c "^LOAD_START" "$OUT")" = 3 ] || { echo "FAIL: want 3 LOAD_START, got $(grep -a -c "^LOAD_START" "$OUT")"; exit 1; }
grep -a -q "^AUTO_NOROOM" "$OUT" && { echo "FAIL: two 2 s clips did not fit in memory together"; exit 1; }
[ "$(grep -a -c "^AUTO_SKIP" "$OUT")" = 1 ] && [ "$(grep -a -c "^LOAD_FAIL" "$OUT")" = 1 ] || { echo "FAIL: want exactly one skip and one failed load"; exit 1; }
[ "$(grep -a -c "^AUTO_PLAY" "$OUT")" = 1 ] || { echo "FAIL: want exactly one AUTO_PLAY"; exit 1; }
grep -a -q "hidden\|notes.txt" "$OUT" && { echo "FAIL: a hidden or non-audio file was touched"; exit 1; }
echo "DJ_DECK_AUTO_GATE=PASS"
