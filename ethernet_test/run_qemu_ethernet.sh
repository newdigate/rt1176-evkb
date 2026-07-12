#!/bin/sh
set -e
QEMU=~/Development/rt1170/evkb/tools/qrun
DIR=$(cd "$(dirname "$0")" && pwd)
. ~/Development/rt1170/evkb/tools/gate-lib.sh
export PHASE="${1:-${PHASE:-boot}}"
gate_init
ELF="$DIR/build/ethernet_test.elf"; VCOM="$DIR/vcom.uart"; DBG="$DIR/eth.dbg"; RES="$DIR/eth.result"
gate_tmp "$RES"; PORT=15600
rm -f "$VCOM" "$DBG" "$RES"
case "$PHASE" in
  server) NIC="-nic user,model=imx.enet,hostfwd=tcp::5555-:7" ;;
  udp)    NIC="-nic user,model=imx.enet,hostfwd=udp::5556-:7" ;;
  client) NIC="-nic user,model=imx.enet,guestfwd=tcp:10.0.2.100:7-cmd:python3 $DIR/guestfwd_echo.py" ;;
  dns)    NIC="-nic user,model=imx.enet" ;;
  *)      NIC="-nic user,model=imx.enet" ;;
esac
"$QEMU" -M mimxrt1170-evk -global fsl-imxrt1170.boot-xip=on -kernel "$ELF" \
    -display none -serial file:"$VCOM" $NIC -d guest_errors -D "$DBG" &
P=$!; gate_pid $P
if [ "$PHASE" = server ] || [ "$PHASE" = udp ]; then
    RC=0; python3 "$DIR/ethernet_peer.py" "$PHASE" 127.0.0.1 "$PORT" > "$RES" 2>&1 || RC=$?
else
    sleep 12; RC=0
fi
sleep 1; kill $P 2>/dev/null; wait $P 2>/dev/null || true
echo "==== VCOM ===="; cat "$VCOM" 2>/dev/null; echo "==== peer ===="; cat "$RES" 2>/dev/null || true
grep -q "ETH_NETIF_UP" "$VCOM" || { echo "FAIL: netif did not come up"; exit 1; }
[ $RC -eq 0 ] || { echo "FAIL: peer rc=$RC"; exit 1; }
case "$PHASE" in
  client) grep -q "CLIENT_ECHO=PASS" "$VCOM" || { echo "FAIL: no client echo"; exit 1; } ;;
  dns)    grep -q "DNS_OK ip=" "$VCOM" || { echo "FAIL: no DNS resolve"; exit 1; } ;;
esac
echo "PASS: ethernet_test $PHASE"
