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
# Capture the phase arg BEFORE gate_init: its gtimeout re-exec drops "$@", so
# forward the phase across the exec via the environment (gate-lib.sh documents
# this pattern for arg-taking runners). ENET_PHASE survives exec; $1 does not.
PHASE="${1:-${ENET_PHASE:-boot}}"
export ENET_PHASE="$PHASE"
. "$EVKB/tools/gate-lib.sh"
gate_init
ELF="$DIR/$(gate_build_dir)/enet_test.elf"
VCOM="$DIR/vcom.uart"; DBG="$DIR/enet.dbg"; RES="$DIR/enet.result"
gate_tmp "$RES"
PORT=15556
rm -f "$VCOM" "$DBG" "$RES"
"$QEMU" $(gate_qemu_machine) -kernel "$ELF" \
    -display none \
    -serial file:"$VCOM" \
    -nic socket,listen=127.0.0.1:$PORT,model=imx.enet \
    -d guest_errors -D "$DBG" &
P=$!; gate_pid $P
# `|| RC=$?` is load-bearing under `set -e`. Unguarded, a peer failure aborted
# the script HERE -- so the `FAIL: peer rc=` assertion below could never run, and
# the gate died silently on precisely the failure it was written to report.
# lwip_test's equivalent line already guards it; this one did not.
RC=0; python3 "$DIR/enet_peer.py" 127.0.0.1 $PORT "$PHASE" > "$RES" 2>&1 || RC=$?
# The peer has finished; wait for the guest's terminal token rather than a flat
# second. The `sleep 1` this replaces made the gate LOAD-SENSITIVE -- the
# thinnest budget in the tree -- so on a busy machine the reap landed before
# QEMU wrote a byte and the run failed with "no UART capture", a red that says
# nothing about the firmware and passes on retry. Observed during the 2026-07-29
# sweep. 40 x 0.25s bounds it at 10s; a healthy run breaks out in well under 1s.
case "$PHASE" in
    ping) _tok="ENET_PING=PASS" ;;
    *)    _tok="ENET_PHYID_OK=PASS" ;;   # last line the firmware emits in boot/mac
esac
for _ in $(seq 1 40); do
    [ -f "$VCOM" ] && grep -q "$_tok" "$VCOM" 2>/dev/null && break
    sleep 0.25
done
gate_reap $P
gate_require_capture "$VCOM"
echo "==== VCOM ===="; cat "$VCOM"
echo "==== peer ===="; cat "$RES"
[ $RC -eq 0 ] || { echo "FAIL: peer rc=$RC"; exit 1; }
grep -q "ENET_BOOT" "$VCOM" || { echo "FAIL: no ENET_BOOT"; exit 1; }
if [ "$PHASE" = "mac" ]; then
    grep -q "ENET_INIT_DONE" "$VCOM" || { echo "FAIL: no ENET_INIT_DONE"; exit 1; }
    grep -q "ENET_TX=PASS" "$VCOM" || { echo "FAIL: no ENET_TX=PASS"; exit 1; }
    grep -q "ENET_RX=PASS" "$VCOM" || { echo "FAIL: no ENET_RX=PASS"; exit 1; }
    grep -q "ENET_LINK=PASS" "$VCOM" || { echo "FAIL: no ENET_LINK=PASS"; exit 1; }
    grep -q "ENET_PHYID_OK=PASS" "$VCOM" || { echo "FAIL: no ENET_PHYID_OK=PASS"; exit 1; }
    echo "PASS: enet_test MAC round-trip (boot + TX=PASS + RX=PASS + LINK=PASS + PHYID_OK=PASS)"
elif [ "$PHASE" = "ping" ]; then
    grep -q "ENET_ARP=PASS" "$VCOM" || { echo "FAIL: no ENET_ARP=PASS"; exit 1; }
    grep -q "ENET_PING=PASS" "$VCOM" || { echo "FAIL: no ENET_PING=PASS"; exit 1; }
    echo "PASS: enet_test ping round-trip (boot + ARP=PASS + PING=PASS)"
else
    echo "PASS: enet_test harness live (boot + socket peer)"
fi
