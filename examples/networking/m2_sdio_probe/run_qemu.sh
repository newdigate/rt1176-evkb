#!/bin/sh
set -e
DIR=$(cd "$(dirname "$0")" && pwd)
EVKB=$(cd "$DIR/../../.." && pwd)
QEMU="$EVKB/tools/qrun"
. "$EVKB/tools/gate-lib.sh"
gate_init
ELF="$DIR/$(gate_build_dir)/m2_sdio_probe.elf"
OUT=$(gate_capture_path "$DIR" serial.uart)
rm -f "$OUT"
"$QEMU" $(gate_qemu_machine) -kernel "$ELF" \
    -display none $(gate_console "$OUT") \
    -d guest_errors -D "$(gate_capture_path "$DIR" serial.dbg)" &
P=$!; gate_pid $P
for _ in $(seq 1 60); do
    [ -f "$OUT" ] && grep -q "alive=2" "$OUT" 2>/dev/null && break
    sleep 0.25
done
gate_reap $P
gate_require_capture "$OUT"
echo "==== captured UART ===="; cat "$OUT"
grep -q "RT1176 M.2 SDIO probe up" "$OUT" || { echo "FAIL: banner missing"; exit 1; }
# This gate asserts the DEVICE-ABSENT path.  QEMU attaches an SD *memory* card,
# which by spec ignores CMD5, so the correct outcome is a clean no-io-function
# verdict -- NOT success.  A green run here says nothing about the real IW416;
# see transcript_hw_evkb.txt for that.
grep -q "^sdio_begin=cmd5-no-response" "$OUT" || {
    echo "FAIL: expected the cmd5-no-response fallback"
    echo "      (if this now says ok=, QEMU gained an SDIO model -- update the gate deliberately)"
    exit 1; }
# Tightened 2026-08-17.  This asserted the broader 'no-io-function' until the
# first hardware run, where that one code covered both "nothing answered" and
# "answered with zero IO functions" -- the only distinction that mattered.
# Asserting the narrower token is what makes the QEMU result mean something
# specific: an SD memory card is present and ignoring CMD5.
grep -q "^int_status=0x" "$OUT" || { echo "FAIL: no raw evidence line"; exit 1; }
# A reason code alone is not enough: "nothing found" is also what a wedged or
# dead image looks like.  probe_done proves we left begin(), and alive= proves
# the image is still running afterwards.
grep -q "^probe_done" "$OUT" || { echo "FAIL: probe never completed"; exit 1; }
grep -q "^alive=2" "$OUT" || { echo "FAIL: no heartbeat after the probe"; exit 1; }
# B0 (2026-08-23): the two bracketed HCI_Reset probes must be PRESENT in their
# card-absent form.  LPUART2 has no chardev here, so nothing can be received:
# quiet=0 and n=0 are what "nothing answered" looks like, and match=0 says the
# firmware did not mistake silence for the Command Complete.  A missing line
# would mean the probe never reached that bracket.
grep -q "^hci_pre_download: quiet=0 reply=none n=0 match=0[[:space:]]*$" "$OUT" || {
    echo "FAIL: B0 pre-download bracket missing or not in its card-absent form"; exit 1; }
grep -q "^hci_post_download: quiet=0 reply=none n=0 match=0[[:space:]]*$" "$OUT" || {
    echo "FAIL: B0 post-download bracket missing or not in its card-absent form"; exit 1; }
grep -q "^after_pdn_cycle: rom_bytes=0 hex=none[[:space:]]*$" "$OUT" || {
    echo "FAIL: after_pdn_cycle line missing or not in its card-absent form"; exit 1; }
# The fallback must not claim identity it cannot have read.
if grep -q "^manfid=" "$OUT"; then
    echo "FAIL: reported a manufacturer ID with no IO function present"; exit 1
fi
echo "PASS: SDIO enumerate reached the cmd5-no-response fallback cleanly"
