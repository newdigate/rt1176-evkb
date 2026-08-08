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
ELF="$DIR/$(gate_build_dir)/serial_test_rx.elf"
PORT=45455
"$QEMU" $(gate_qemu_machine) -kernel "$ELF" \
    -display none -chardev socket,id=u1,host=127.0.0.1,port=$PORT,server=on,wait=off \
    -serial chardev:u1 -d guest_errors -D "$DIR/rx.dbg" &
P=$!; gate_pid $P
sleep 1
RC=0
python3 "$DIR/qemu_rx_driver.py" $PORT || RC=1
gate_reap $P
exit $RC
