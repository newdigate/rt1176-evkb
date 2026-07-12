#!/bin/sh
set -e
QEMU=~/Development/rt1170/evkb/tools/qrun
DIR=$(cd "$(dirname "$0")" && pwd)
. ~/Development/rt1170/evkb/tools/gate-lib.sh
gate_init
ELF="$DIR/build/lwip_test.elf"; VCOM="$DIR/vcom.uart"; DBG="$DIR/lwip.dbg"; RES="$DIR/lwip.result"
gate_tmp "$RES"; PORT=15600; PHASE="${1:-boot}"
rm -f "$VCOM" "$DBG" "$RES"
case "$PHASE" in
  ping)  NIC="-nic socket,listen=127.0.0.1:$PORT,model=imx.enet" ;;
  dhcp)  NIC="-nic user,model=imx.enet" ;;
  udp)   NIC="-nic user,model=imx.enet,hostfwd=udp::5556-:7" ;;
  tcp)   NIC="-nic user,model=imx.enet,hostfwd=tcp::5555-:7" ;;
  *)     NIC="-nic user,model=imx.enet" ;;
esac
"$QEMU" -M mimxrt1170-evk -global fsl-imxrt1170.boot-xip=on -kernel "$ELF" \
    -display none -serial file:"$VCOM" $NIC -d guest_errors -D "$DBG" &
P=$!; gate_pid $P
if [ "$PHASE" = ping ] || [ "$PHASE" = udp ] || [ "$PHASE" = tcp ]; then
    python3 "$DIR/lwip_peer.py" "$PHASE" 127.0.0.1 "$PORT" > "$RES" 2>&1; RC=$?
else
    sleep 8; RC=0
fi
sleep 1; kill $P 2>/dev/null; wait $P 2>/dev/null || true
echo "==== VCOM ===="; cat "$VCOM" 2>/dev/null; echo "==== peer ===="; cat "$RES" 2>/dev/null || true
grep -q "LWIP_NETIF_UP" "$VCOM" || { echo "FAIL: netif did not come up"; exit 1; }
[ $RC -eq 0 ] || { echo "FAIL: peer rc=$RC"; exit 1; }
echo "PASS: lwip_test $PHASE"
