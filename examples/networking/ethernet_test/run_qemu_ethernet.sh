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
export PHASE="${1:-${PHASE:-boot}}"
gate_init
ELF="$DIR/build/ethernet_test.elf"; VCOM="$DIR/vcom.uart"; DBG="$DIR/eth.dbg"; RES="$DIR/eth.result"
gate_tmp "$RES"; PORT=15600
rm -f "$VCOM" "$DBG" "$RES"
# Carry the -nic VALUE (no flag) so it can be passed as a single quoted arg;
# the client phase's guestfwd -cmd contains a space that must NOT word-split.
case "$PHASE" in
  server) NICVAL="user,model=imx.enet,hostfwd=tcp::5555-:7" ;;
  udp)    NICVAL="user,model=imx.enet,hostfwd=udp::5556-:7" ;;
  client) NICVAL="user,model=imx.enet,guestfwd=tcp:10.0.2.100:7-cmd:python3 $DIR/guestfwd_echo.py" ;;
  dns)    NICVAL="user,model=imx.enet" ;;
  *)      NICVAL="user,model=imx.enet" ;;
esac
"$QEMU" -M mimxrt1170-evk -global fsl-imxrt1170.boot-xip=on -kernel "$ELF" \
    -display none -serial file:"$VCOM" -nic "$NICVAL" -d guest_errors -D "$DBG" &
P=$!; gate_pid $P
if [ "$PHASE" = server ] || [ "$PHASE" = udp ]; then
    RC=0; python3 "$DIR/ethernet_peer.py" "$PHASE" 127.0.0.1 "$PORT" > "$RES" 2>&1 || RC=$?
else
    sleep 12; RC=0
fi
sleep 1; gate_reap $P
gate_require_capture "$VCOM"
echo "==== VCOM ===="; cat "$VCOM"
# The peer log only exists in the phases that actually run a peer; in the
# DEFAULT `boot` phase there is none. Displaying it unconditionally aborted the
# gate under `set -e` (`cat: no such file`) before a single assertion ran --
# the same "the run reports nothing" failure gate_reap exists to stop. The
# capture above is asserted; this one is genuinely optional, so it is guarded.
[ -f "$RES" ] && { echo "==== peer ===="; cat "$RES"; }
grep -q "ETH_NETIF_UP" "$VCOM" || { echo "FAIL: netif did not come up"; exit 1; }
grep -q "IPADDR=OK" "$VCOM" || { echo "FAIL: ipaddr"; exit 1; }
[ $RC -eq 0 ] || { echo "FAIL: peer rc=$RC"; exit 1; }
case "$PHASE" in
  client) grep -q "CLIENT_ECHO=PASS" "$VCOM" || { echo "FAIL: no client echo"; exit 1; } ;;
  dns)    grep -q "DNS_OK ip=" "$VCOM" || { echo "FAIL: no DNS resolve"; exit 1; } ;;
esac
echo "PASS: ethernet_test $PHASE"
