#!/bin/sh
# run_qemu_wifi.sh — the ENUMERATION + SCAN gate for wifi_client_test.
#
# WHAT THIS PROVES
#   Against the qemu2 IW416 card model (m2-wifi=on, fw-preboot=on so no NXP
#   blob is needed), WiFi.begin() walks real SDIO enumeration, real fn1 init,
#   GET_HW_SPEC, and issues a REAL 802_11_SCAN -- and when that scan returns
#   zero BSSes (the model returns none BY DESIGN: hw/sd/iw416-sdio.c refuses
#   to invent an AP), begin() reports WL_NO_SSID_AVAIL (1) honestly instead
#   of wedging or claiming a link.  The heartbeat after begin() proves the
#   blocking call returned and the image stays alive.
#
# WHAT THIS DOES *NOT* PROVE — read before trusting a green
#   * NO association, NO 4-way handshake, NO DHCP, NO TCP, NO WiFiClient/
#     WiFiServer data path.  Zero scan results means the connect path ends at
#     the scan; everything past it is silicon-only (transcript_hw_evkb.txt).
#   * NO firmware download (fw-preboot skips it; m2_lwip_test's silicon
#     transcript covers the download).
#   * The model is not silicon; see run_qemu_wifi.sh in m2_sdio_probe for the
#     full statement of where it could be lying.
# Requires qemu2 >= 7e17eff5d3 (gitlab.com/Newdigate/qemu-rt1170) -- the W16
# floor, not W15's 2ed9314631: this example links the W16 driver, which reads
# the multiport REGISTER PORT and issues AGGREGATED CMD53s, and a model with
# neither returns zeros for the first and refuses the second.  7e17eff5d3 is
# what this gate was actually measured green on.  That floor is RESTATED from
# CLAUDE.md's "W16 MOVED THE FLOOR" paragraph, NOT independently measured here:
# the driver does self-detect and fall back to CMD52 (Iw416.cpp mpRegsUsable()),
# so an older model MIGHT still satisfy this particular gate -- nobody has run
# it, and rebuilding the shared qemu2 tree to find out would invalidate other
# sessions' sweeps.  Claim the floor you measured on.
# On stock QEMU this gate goes RED, not SKIP.  (CLAUDE.md's model-dependent-gate
# paragraph does not name this gate yet -- adding it is Task 13's job.)
set -e
DIR=$(cd "$(dirname "$0")" && pwd)
EVKB=$(cd "$DIR/../../.." && pwd)
QEMU="$EVKB/tools/qrun"
. "$EVKB/tools/gate-lib.sh"
gate_init
[ "$(gate_board)" = rt1176 ] || {
    echo "FAIL: this gate is rt1176-only (EVKB_BOARD=$(gate_board)) — the M.2 socket and the"
    echo "      iw416-sdio model both live on the MIMXRT1170-EVKB machine"; exit 1; }
ELF="$DIR/$(gate_build_dir)/wifi_client_test.elf"
# Distinct basenames from run_qemu.sh's serial.uart ON PURPOSE (both gates run
# from this directory; each starts with rm -f).
OUT=$(gate_capture_path "$DIR" wifi.uart)
DBG=$(gate_capture_path "$DIR" wifi.dbg)
rm -f "$OUT" "$DBG"
# `-M ...` from gate-lib (no gate names a machine) plus a SECOND, additive
# `-machine` fragment for the opt-in property; QEMU merges the two into one
# machine opts group.  Writing `-machine mimxrt1170-evk,m2-wifi=on` instead
# DROPS gate_qemu_machine's `-global fsl-imxrt1170.boot-xip=on` (boot-xip is a
# -global on the SoC object, not a machine property) and aborts with an EMPTY
# capture -- indistinguishable from firmware that never started.
"$QEMU" $(gate_qemu_machine) -machine m2-wifi=on \
    -global iw416-sdio.fw-preboot=on \
    -kernel "$ELF" -display none $(gate_console "$OUT") \
    -d guest_errors -D "$DBG" &
P=$!; gate_pid $P
for _ in $(seq 1 240); do
    [ -f "$OUT" ] && grep -q "alive=3" "$OUT" 2>/dev/null && break
    sleep 0.25
done
gate_reap $P
gate_require_capture "$OUT"
echo "==== captured UART ===="; cat "$OUT"
grep -q "RT1176 WiFi client test up" "$OUT" || { echo "FAIL: banner missing"; exit 1; }
# THE NEGATIVE THAT MAKES THE REST MEAN SOMETHING: status 255 here would mean
# the model was not attached and we are silently re-running the absent gate.
if grep -q "^wifi_status=255" "$OUT"; then
    echo "FAIL: WL_NO_SHIELD with the model attached — m2-wifi=on did not take"; exit 1
fi
grep -q "^wifi_status=1" "$OUT" || {
    echo "FAIL: expected WL_NO_SSID_AVAIL (1) from a real scan finding zero BSS"; exit 1; }
grep -q "^alive=3" "$OUT" || { echo "FAIL: no heartbeat after the failed connect"; exit 1; }
if grep -q "^wifi_ip=" "$OUT"; then
    echo "FAIL: claimed an IP the model cannot have granted"; exit 1
fi
echo "PASS: enumeration + real scan -> honest WL_NO_SSID_AVAIL; image alive"
