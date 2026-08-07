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
ELF="$DIR/$(gate_build_dir)/usb_enum_test.elf"; OUT="$DIR/usb.uart"
rm -f "$OUT"
"$QEMU" $(gate_qemu_machine) -kernel "$ELF" \
    -display none \
    -serial file:"$OUT" \
    -chardev null,id=usbcdc \
    -d guest_errors -D "$DIR/usb.dbg" &
P=$!; gate_pid $P; sleep 6; gate_reap $P
gate_require_capture "$OUT"
echo "==== VCOM ===="; cat "$OUT"
echo "==== CI-CDC (enumeration) ===="; grep "CI-CDC" "$DIR/usb.dbg" | head
grep -q "USB=CONFIGURED" "$OUT" || { echo "FAIL: USB enumeration"; exit 1; }
echo "PASS: USB CDC enumeration verified"
