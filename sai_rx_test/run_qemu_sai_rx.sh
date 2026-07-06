#!/bin/sh
set -e
QEMU=~/Development/rt1170/evkb/tools/qrun
DIR=$(cd "$(dirname "$0")" && pwd)
ELF="$DIR/build/sai_rx_test.elf"
VCOM="$DIR/vcom.uart"; DBG="$DIR/sai_rx.dbg"; INJ="$DIR/inject.raw"; TAP="$DIR/tap.raw"
python3 "$DIR/gen_inject.py" "$INJ"
rm -f "$VCOM" "$DBG" "$TAP"
# NOTE: this QEMU build's "file" chardev backend requires path= (the write/out
# side) even when only input-path= is given ("chardev: file: no filename
# given"), so plain input-path=$INJ does not work here. Fall back to a fifo:
# pump the injector file into a named pipe and point the chardev at that.
rm -f "$INJ.fifo"; mkfifo "$INJ.fifo"
(cat "$INJ" > "$INJ.fifo" 2>/dev/null &)
"$QEMU" -M mimxrt1170-evk -global fsl-imxrt1170.boot-xip=on -kernel "$ELF" \
    -display none -serial file:"$VCOM" \
    -chardev pipe,id=sai1-rxinject,path="$INJ.fifo" \
    -chardev file,id=sai1-tap,path="$TAP" \
    -d guest_errors -D "$DBG" &
P=$!
sleep 4; kill $P 2>/dev/null; wait $P 2>/dev/null || true
rm -f "$INJ.fifo.in" "$INJ.fifo.out" "$INJ.fifo"
echo "==== VCOM ===="; cat "$VCOM" 2>/dev/null || true
grep -q "STAGE_A_PASS" "$VCOM" || { echo "FAIL: stage A polled read"; exit 1; }
echo "PASS: stage A"
grep -q "STAGE_B_DONE" "$VCOM" || { echo "FAIL: stage B not reached"; exit 1; }
grep -q "STAGE_B_PASS" "$VCOM" || { echo "FAIL: stage B DMA capture"; exit 1; }
echo "PASS: stage B"
