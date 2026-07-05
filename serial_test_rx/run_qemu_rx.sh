#!/bin/sh
set -e
QEMU=~/Development/rt1170/evkb/tools/qrun
DIR=$(cd "$(dirname "$0")" && pwd)
ELF="$DIR/build/serial_test_rx.elf"
PORT=45455
"$QEMU" -M mimxrt1170-evk -global fsl-imxrt1170.boot-xip=on -kernel "$ELF" \
    -display none -chardev socket,id=u1,host=127.0.0.1,port=$PORT,server=on,wait=off \
    -serial chardev:u1 -d guest_errors -D "$DIR/rx.dbg" &
P=$!
sleep 1
RC=0
python3 "$DIR/qemu_rx_driver.py" $PORT || RC=1
kill $P 2>/dev/null; wait $P 2>/dev/null || true
exit $RC
