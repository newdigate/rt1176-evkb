#!/bin/sh
# run_qemu.sh — the CARD-ABSENT gate for m2_uap_probe.
#
# WHAT THIS PROVES
#   With a plain SD memory card on USDHC1 (the default machine — an SD card by
#   spec ignores CMD5), the image takes the no-card fallback cleanly, says so,
#   reaches uap_probe_done and keeps running. It must NOT claim a card, must
#   NOT emit a single uap_probe cell, and must NOT print a verdict: a verdict
#   with no firmware behind it is exactly the kind of confident-and-empty
#   answer this whole example exists to avoid.
#
# WHAT THIS DOES *NOT* PROVE
#   Nothing at all about AP mode. The uAP question can only be answered by the
#   card's own firmware; the sibling gate run_qemu_wifi.sh exercises the
#   probe's machinery against the IW416 model (whose answer is always
#   NOT-SUPPORTED, because it models no AP commands), and the real answer is a
#   silicon answer and lives in transcript_hw_evkb.txt.
set -e
DIR=$(cd "$(dirname "$0")" && pwd)
EVKB=$(cd "$DIR/../../.." && pwd)
QEMU="$EVKB/tools/qrun"
. "$EVKB/tools/gate-lib.sh"
gate_init
ELF="$DIR/$(gate_build_dir)/m2_uap_probe.elf"
OUT=$(gate_capture_path "$DIR" serial.uart)
DBG=$(gate_capture_path "$DIR" serial.dbg)
rm -f "$OUT" "$DBG"
"$QEMU" $(gate_qemu_machine) -kernel "$ELF" \
    -display none $(gate_console "$OUT") \
    -d guest_errors -D "$DBG" &
P=$!; gate_pid $P
# Wait for the heartbeat, which is the LAST thing printed — not for
# uap_probe_done, which is followed by more output. A gate that reaps on the
# first interesting line can land mid-line on the next one and then blame the
# firmware for its own timing (W16 cost a day to that on m2_rx_demo[irq]).
for _ in $(seq 1 60); do
    [ -f "$OUT" ] && grep -q "^hb card=" "$OUT" 2>/dev/null && break
    sleep 0.25
done
gate_reap $P
gate_require_capture "$OUT"
echo "==== captured UART ===="; cat "$OUT"
grep -q "RT1176 M.2 uAP probe up" "$OUT" || { echo "FAIL: banner missing"; exit 1; }
grep -q "^sdio_begin=cmd5-no-response" "$OUT" || {
    echo "FAIL: expected the cmd5-no-response fallback"; exit 1; }
# The new W17 FAULT 1 readings are subject to the same vacuity rule as the
# probe cells: with no card there is nothing to read, so a card-state dump or a
# recovery verdict appearing here would be invented rather than measured.
if grep -q "^uap_cardreg " "$OUT"; then
    echo "FAIL: dumped card registers with no card present"; exit 1
fi
if grep -q "^uap_recover=" "$OUT"; then
    echo "FAIL: reported a command-port recovery verdict with no card present"; exit 1
fi
grep -q "^uap_probe_done[[:space:]]*$" "$OUT" || { echo "FAIL: probe never completed"; exit 1; }
grep -q "^hb card=0" "$OUT" || { echo "FAIL: no heartbeat, or it claimed a card"; exit 1; }
# THE VACUITY GUARDS. With no card there is nothing to ask, so any probe cell
# or verdict on this capture is a fabricated answer.
if grep -q "^uap_probe " "$OUT"; then
    echo "FAIL: emitted a probe cell with no card present"; exit 1
fi
if grep -q "^uap_verdict=" "$OUT"; then
    echo "FAIL: printed a uAP verdict with no firmware to ask"; exit 1
fi
echo "PASS: card-absent fallback; no probe cells and no verdict were invented"
