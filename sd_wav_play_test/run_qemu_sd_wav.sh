#!/bin/sh
set -e
QEMU=~/Development/rt1170/evkb/tools/qrun
DIR=$(cd "$(dirname "$0")" && pwd)
. ~/Development/rt1170/evkb/tools/gate-lib.sh
gate_init
ELF="$DIR/build/sd_wav_play_test.elf"
[ -f "$ELF" ] || { echo "FAIL: no ELF ($ELF) — build first"; exit 1; }

run_one() {   # $1 = mono|stereo
    KIND="$1"
    WAV="$DIR/$KIND.wav"; IMG="$DIR/card_$KIND.img"
    VCOM="$DIR/$KIND.uart"; TAP="$DIR/$KIND.raw"; DBG="$DIR/$KIND.dbg"
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
    # run: SD image (USDHC1) + SAI1 TX tap. -icount couples the guest CPU to
    # QEMU_CLOCK_VIRTUAL so the SAI TX frame-clock (which paces the TDR0-drain DMA)
    # and the blocking USDHC wavfile.read() in the graph-update ISR advance in
    # lock-step. Without it the SD read burns virtual time while the SAI timer
    # keeps draining half-buffers the guest hasn't refilled -> the captured TDR0
    # stream is scrambled (mid-block phase jumps). Same determinism lever the
    # interval_timer / tone gates use; here it makes the play->TDR0 path exact.
    "$QEMU" -M mimxrt1170-evk -global fsl-imxrt1170.boot-xip=on -kernel "$ELF" \
        -display none -serial file:"$VCOM" \
        -drive if=sd,format=raw,file="$IMG" \
        -chardev file,id=sai1-tap,path="$TAP" \
        -icount shift=auto \
        -d guest_errors,unimp -D "$DBG" &
    P=$!; gate_pid $P; sleep 20; kill $P 2>/dev/null; wait $P 2>/dev/null || true
    echo "==== $KIND VCOM ===="; cat "$VCOM" 2>/dev/null || true
    grep -q "SD_WAV_MOUNT=PASS" "$VCOM" || { echo "FAIL($KIND): mount"; return 1; }
    grep -q "SD_WAV_PLAY=PASS"  "$VCOM" || { echo "FAIL($KIND): play";  return 1; }
    grep -q "SD_WAV_DONE=PASS"  "$VCOM" || { echo "FAIL($KIND): done";  return 1; }
    echo "==== $KIND TAP (sample-exact) ===="
    python3 "$DIR/check_tap.py" "$TAP" "$WAV" "$KIND" || { echo "FAIL($KIND): sample-exact"; return 1; }
}

run_one mono   || exit 1
run_one stereo || exit 1
echo "SD_WAV_ALL=PASS"
