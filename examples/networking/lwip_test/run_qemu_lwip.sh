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
# gate_init's guard re-exec ("exec ... "$0"" with no argv, see gate-lib.sh) drops
# CLI args -- capture+export the phase first so it survives via the environment
# instead (gate-lib.sh's own comment flags this as the expected fix for a
# runner that takes CLI args).
export PHASE="${1:-${PHASE:-boot}}"
gate_init
ELF="$DIR/build/lwip_test.elf"; VCOM="$DIR/vcom.uart"; DBG="$DIR/lwip.dbg"; RES="$DIR/lwip.result"
gate_tmp "$RES"; PORT=15600
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
    RC=0; python3 "$DIR/lwip_peer.py" "$PHASE" 127.0.0.1 "$PORT" > "$RES" 2>&1 || RC=$?
else
    sleep 8; RC=0
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
grep -q "LWIP_NETIF_UP" "$VCOM" || { echo "FAIL: netif did not come up"; exit 1; }
[ $RC -eq 0 ] || { echo "FAIL: peer rc=$RC"; exit 1; }
if [ "$PHASE" = dhcp ]; then
    grep -q "DHCP_OK ip=10.0.2.15" "$VCOM" || { echo "FAIL: no DHCP lease"; exit 1; }
fi
echo "PASS: lwip_test $PHASE"
