#!/bin/sh
set -e
QEMU=~/Development/rt1170/evkb/tools/qrun
DIR=$(cd "$(dirname "$0")" && pwd)
# Capture the phase arg BEFORE gate_init: its gtimeout re-exec drops "$@", so
# forward the phase across the exec via the environment (gate-lib.sh documents
# this pattern for arg-taking runners). ENET_PHASE survives exec; $1 does not.
PHASE="${1:-${ENET_PHASE:-boot}}"
export ENET_PHASE="$PHASE"
. ~/Development/rt1170/evkb/tools/gate-lib.sh
gate_init
ELF="$DIR/build/enet_test.elf"
VCOM="$DIR/vcom.uart"; DBG="$DIR/enet.dbg"; RES="$DIR/enet.result"
gate_tmp "$RES"
PORT=15556
rm -f "$VCOM" "$DBG" "$RES"
"$QEMU" -M mimxrt1170-evk -global fsl-imxrt1170.boot-xip=on -kernel "$ELF" \
    -display none \
    -serial file:"$VCOM" \
    -nic socket,listen=127.0.0.1:$PORT,model=imx.enet \
    -d guest_errors -D "$DBG" &
P=$!; gate_pid $P
python3 "$DIR/enet_peer.py" 127.0.0.1 $PORT "$PHASE" > "$RES" 2>&1
RC=$?
sleep 1; kill $P 2>/dev/null; wait $P 2>/dev/null || true
echo "==== VCOM ===="; cat "$VCOM" 2>/dev/null || true
echo "==== peer ===="; cat "$RES"
[ $RC -eq 0 ] || { echo "FAIL: peer rc=$RC"; exit 1; }
grep -q "ENET_BOOT" "$VCOM" || { echo "FAIL: no ENET_BOOT"; exit 1; }
if [ "$PHASE" = "mac" ]; then
    grep -q "ENET_INIT_DONE" "$VCOM" || { echo "FAIL: no ENET_INIT_DONE"; exit 1; }
    grep -q "ENET_TX=PASS" "$VCOM" || { echo "FAIL: no ENET_TX=PASS"; exit 1; }
    grep -q "ENET_RX=PASS" "$VCOM" || { echo "FAIL: no ENET_RX=PASS"; exit 1; }
    echo "PASS: enet_test MAC round-trip (boot + TX=PASS + RX=PASS)"
else
    echo "PASS: enet_test harness live (boot + socket peer)"
fi
