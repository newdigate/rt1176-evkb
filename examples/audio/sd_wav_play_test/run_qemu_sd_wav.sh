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
ELF="$DIR/$(gate_build_dir)/sd_wav_play_test.elf"
[ -f "$ELF" ] || { echo "FAIL: no ELF ($ELF) — build first"; exit 1; }

run_one() {   # $1 = mono|stereo
    KIND="$1"
    WAV=$(gate_capture_path "$DIR" "$KIND.wav"); IMG=$(gate_capture_path "$DIR" "card_$KIND.img")
    VCOM=$(gate_capture_path "$DIR" "$KIND.uart"); TAP=$(gate_capture_path "$DIR" "$KIND.raw")
    DBG=$(gate_capture_path "$DIR" "$KIND.dbg")
    rm -f "$WAV" "$IMG" "$VCOM" "$TAP" "$DBG"
    python3 "$DIR/gen_wav.py" "$KIND" "$WAV"
    # MBR/FAT16 card image with TEST.WAV seeded (the sd_fs_test recipe + a copy).
    mkfile -n 512m "$IMG"
    DISK=$(hdiutil attach -nomount -imagekey diskimage-class=CRawDiskImage "$IMG" | head -1 | awk '{print $1}')
    [ -n "$DISK" ] || { echo "FAIL($KIND): attach"; return 1; }
    diskutil partitionDisk "$DISK" 1 MBR "MS-DOS FAT16" RTWAV 100% >/dev/null \
        || { hdiutil detach "$DISK" >/dev/null 2>&1 || true; echo "FAIL($KIND): partition"; return 1; }
    MNT=$(diskutil info "${DISK}s1" | sed -n 's/.*Mount Point: *//p')
    cp "$WAV" "$MNT/TEST.WAV"; sync
    diskutil eject "$DISK" >/dev/null
    # run: SD image (USDHC1 on the EVKB, USDHC2 on nosh: gate_sd_drive picks the
    # -drive index) + SAI1 TX tap. -icount couples the guest CPU to
    # QEMU_CLOCK_VIRTUAL so the SAI TX frame-clock (which paces the TDR0-drain DMA)
    # and the blocking USDHC wavfile.read() in the graph-update ISR advance in
    # lock-step. Without it the SD read burns virtual time while the SAI timer
    # keeps draining half-buffers the guest hasn't refilled -> the captured TDR0
    # stream is scrambled (mid-block phase jumps). Same determinism lever the
    # interval_timer / tone gates use; here it makes the play->TDR0 path exact.
    #
    # The shift is FIXED, not `auto`. Under shift=auto QEMU re-fits the
    # instructions-per-virtual-ns ratio at run time, and that re-fit lands a
    # jump in virtual time on the SAI frame timer: several DMA half-buffer ticks
    # fire back to back before the software update runs, one 128-frame block
    # goes out as zeros and the stream resumes 64 frames late. Which gate run
    # trips it depends only on WHEN playback starts after boot: with play ~50 ms
    # after boot (the WM8962 path) it never showed; with play ~450 ms after boot
    # it reproduced on BOTH boards, codec or no codec (2026-11-07: body_mism=4734
    # of 8704 every run, mono, nosh and rt1176 alike). The Nosh codec exposed it
    # because AudioControlSGTL5000::enable() carries the Teensy driver's 400 ms
    # analog-settle delay. A fixed shift has no re-fit; 3, 5 and 7 all pass on
    # both boards, 5 (32 ns per instruction) is the one pinned here.
    "$QEMU" $(gate_qemu_machine) -kernel "$ELF" \
        -display none $(gate_console "$VCOM") \
        $(gate_sd_drive "$IMG") \
        -chardev file,id=sai1-tap,path="$TAP" \
        -icount shift=5 \
        -d guest_errors,unimp -D "$DBG" &
    P=$!; gate_pid $P; sleep 20; gate_reap $P
    gate_require_capture "$VCOM"
    echo "==== $KIND VCOM ===="; cat "$VCOM"
    grep -q "SD_WAV_MOUNT=PASS" "$VCOM" || { echo "FAIL($KIND): mount"; return 1; }
    grep -q "SD_WAV_PLAY=PASS"  "$VCOM" || { echo "FAIL($KIND): play";  return 1; }
    grep -q "SD_WAV_DONE=PASS"  "$VCOM" || { echo "FAIL($KIND): done";  return 1; }
    echo "==== $KIND TAP (sample-exact) ===="
    python3 "$DIR/check_tap.py" "$TAP" "$WAV" "$KIND" || { echo "FAIL($KIND): sample-exact"; return 1; }
}

run_one mono   || exit 1
run_one stereo || exit 1
echo "SD_WAV_ALL=PASS"
