#!/bin/sh
# QEMU gate for blink example.
# Verifies GPIO LED toggle via QMP memory read.
set -e
DIR=$(cd "$(dirname "$0")" && pwd)
EVKB=$(cd "$DIR/../../.." && pwd)
. "$EVKB/tools/gate-lib.sh"
gate_init 30

ELF="$DIR/$(gate_build_dir)/blinky.elf"
if [ ! -f "$ELF" ]; then
    echo "no $(gate_build_dir)/blinky.elf in $DIR — build the example first:"
    echo "  cd $DIR && cmake -B $(gate_build_dir) -DCMAKE_TOOLCHAIN_FILE=$EVKB/toolchain/rt1170-evkb.toolchain.cmake && cmake --build $(gate_build_dir)"
    exit 1
fi

python3 "$EVKB/tools/qemu_check_blink.py" "$ELF" --addr 0x40134000 --bit 3 --toggles --seconds 3
