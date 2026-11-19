#!/bin/bash
# dj_deck gate (Nosh only): the whole path a user takes, driven by scripted
# touches.  A FAT32 card image holds /music/click.wav (8 s, 120 BPM click),
# a hidden file, an empty folder and a non-audio file.  Run 1 browses to the
# clip, loads it (peaks, spectrum and tempo are built and saved beside it),
# plays, zooms, pauses and sets a cue.  Run 2 repeats on the same image and
# must reuse the three sidecar files untouched.
set -e
DIR=$(cd "$(dirname "$0")" && pwd)
EVKB=$(cd "$DIR/../../.." && pwd)
QEMU="$EVKB/tools/qrun"
. "$EVKB/tools/gate-lib.sh"
gate_init
ELF="$DIR/$(gate_build_dir)/dj_deck.elf"
[ -f "$ELF" ] || { echo "FAIL: no ELF ($ELF) -- build first"; exit 1; }
SCRIPT="$DIR/touch_script.txt"
[ -f "$SCRIPT" ] || { echo "FAIL: touch script missing at $SCRIPT"; exit 1; }
WAV=$(gate_capture_path "$DIR" click.wav); IMG=$(gate_capture_path "$DIR" card.img)
DBG=$(gate_capture_path "$DIR" dj_deck.dbg)
rm -f "$WAV" "$IMG" "$DBG"

# The clip: audio_codecs_probe/clips/make_clips.sh's 8 s click, as 16-bit WAV.
ffmpeg -hide_banner -loglevel error -y \
    -f lavfi -i "anoisesrc=color=pink:amplitude=0.02:sample_rate=44100:duration=8:seed=7" \
    -f lavfi -i "sine=frequency=1000:sample_rate=44100:duration=8" \
    -filter_complex "[1:a]volume='if(lt(mod(t,0.5),0.02),0.8,0)':eval=frame[c];[0:a][c]amix=inputs=2:normalize=0,pan=stereo|c0=c0|c1=0.8*c0[o]" \
    -map "[o]" -c:a pcm_s16le -bitexact "$WAV"

with_card() {   # with_card CMD...: run CMD with the image mounted, $MNT set
    DISK=$(hdiutil attach -nomount -imagekey diskimage-class=CRawDiskImage "$IMG" | head -1 | awk '{print $1}')
    [ -n "$DISK" ] || { echo "FAIL: attach"; exit 1; }
    diskutil mount "${DISK}s1" >/dev/null || { hdiutil detach "$DISK" >/dev/null 2>&1; echo "FAIL: mount"; exit 1; }
    MNT=$(diskutil info "${DISK}s1" | sed -n 's/.*Mount Point: *//p')
    "$@"; RC=$?
    sync; diskutil eject "$DISK" >/dev/null
    return $RC
}
mkfile -n 512m "$IMG"
DISK=$(hdiutil attach -nomount -imagekey diskimage-class=CRawDiskImage "$IMG" | head -1 | awk '{print $1}')
[ -n "$DISK" ] || { echo "FAIL: attach"; exit 1; }
diskutil partitionDisk "$DISK" 1 MBR "MS-DOS FAT32" DJDECK 100% >/dev/null \
    || { hdiutil detach "$DISK" >/dev/null 2>&1 || true; echo "FAIL: partition"; exit 1; }
MNT=$(diskutil info "${DISK}s1" | sed -n 's/.*Mount Point: *//p')
mkdir -p "$MNT/music/sub"
cp "$WAV" "$MNT/music/click.wav"; cp "$WAV" "$MNT/music/.hidden.wav"; echo "not audio" > "$MNT/notes.txt"
sync; diskutil eject "$DISK" >/dev/null

run() {   # run N -> capture in $OUT
    OUT=$(gate_capture_path "$DIR" "dj_deck_run$1.uart"); rm -f "$OUT"
    "$QEMU" $(gate_qemu_machine) -kernel "$ELF" \
        -display none $(gate_console "$OUT") \
        $(gate_sd_drive "$IMG") \
        -global driver=imxrt.gt911,property=touch-script,value="$SCRIPT" \
        -d guest_errors -D "$DBG" &
    P=$!; gate_pid $P
    for _ in $(seq 1 1440); do
        [ -f "$OUT" ] && grep -a -q "UI_CUE=\|LOAD_FAIL\|LVGL_ASSERT" "$OUT" 2>/dev/null && break
        sleep 0.25
    done
    sleep 1; gate_reap $P
    gate_require_capture "$OUT"
    echo "==== run $1 UART ===="; cat "$OUT"
}
need() { grep -a -qE "$1" "$OUT" || { echo "FAIL(run $RUN): missing $1"; exit 1; }; }
in_order() {   # every pattern must appear, each after the one before
    L=0
    for p in "$@"; do
        N=$(grep -a -nE "$p" "$OUT" | awk -F: -v l="$L" '$1 > l { print $1; exit }')
        [ -n "$N" ] || { echo "FAIL(run $RUN): '$p' missing or out of order"; exit 1; }
        L=$N
    done
}
common() {
    in_order "^SD_OK" "^DJ_READY" "^BROWSE dir=/ n=1" "^BROWSE dir=/music n=2" "^BROWSE_PICK /music/click.wav" \
        "^LOAD_START /music/click.wav" "^FORMAT rate=44100 ch=2 bits=16 frames=352800" "^PEAKS_SRC=" "^DECODE_DONE" \
        "^TEMPO bpm_x100=" "^SIDECARS=" "^LOAD_DONE" "^UI_PLAY=1" "^UI_ZOOM=" "^UI_ZOOM=" "^UI_PLAY=0" "^UI_CUE=SetCue"
    grep -a -q "LOAD_FAIL\|LVGL_ASSERT" "$OUT" && { echo "FAIL(run $RUN): $(grep -a "LOAD_FAIL\|LVGL_ASSERT" "$OUT" | head -1)"; exit 1; }
    BPM=$(grep -a -oE "^TEMPO bpm_x100=[0-9]+" "$OUT" | head -1 | cut -d= -f2)
    [ "$BPM" -ge 11950 ] && [ "$BPM" -le 12050 ] || { echo "FAIL(run $RUN): tempo $BPM, want 11950..12050"; exit 1; }
    Z1=$(grep -a -oE "^UI_ZOOM=[0-9]+" "$OUT" | sed -n 1p | cut -d= -f2); Z2=$(grep -a -oE "^UI_ZOOM=[0-9]+" "$OUT" | sed -n 2p | cut -d= -f2)
    # 352800 frames fit the 1120 px lane at 315 frames per pixel: one step in
    # halves that (158), the next stops at the widget's floor of 128.
    [ "$Z1" = 158 ] && [ "$Z2" = 128 ] || { echo "FAIL(run $RUN): zoom steps $Z1 -> $Z2, want 158 -> 128"; exit 1; }
}
sums() { ( cd "$MNT/music" && ls click.wav.apv click.wav.asv click.wav.att >/dev/null && cat click.wav.apv click.wav.asv click.wav.att | md5 ); }

RUN=1; run 1; common
need "^PEAKS_SRC=built"; need "^TEMPO bpm_x100=[0-9]+ beats=[0-9]+ src=built"; need "^SIDECARS=SAVED"
SUM1=$(with_card sums) || { echo "FAIL(run 1): the card does not hold click.wav.apv/.asv/.att"; exit 1; }
RUN=2; run 2; common
need "^PEAKS_SRC=file"; need "^TEMPO bpm_x100=[0-9]+ beats=[0-9]+ src=file"; need "^SIDECARS=REUSED"
SUM2=$(with_card sums) || { echo "FAIL(run 2): sidecars gone"; exit 1; }
[ "$SUM1" = "$SUM2" ] || { echo "FAIL: run 2 rewrote the sidecars ($SUM1 -> $SUM2)"; exit 1; }
echo "DJ_DECK_GATE=PASS"
