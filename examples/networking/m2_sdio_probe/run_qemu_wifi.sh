#!/bin/sh
# run_qemu_wifi.sh — the SDIO Wi-Fi ENUMERATION gate for m2_sdio_probe.
#
# WHAT THIS PROVES
#   Booted against QEMU's IW416/SD8978 card model (qemu2 hw/sd/iw416-sdio.c,
#   opt-in via `-machine mimxrt1170-evk,m2-wifi=on`), the M.2 driver walks the
#   whole SDIO enumeration path and comes out with the right answers:
#     * CMD5 IO_SEND_OP_COND is answered  -> sdio_begin=ok, R4 = 0x90FF8000
#     * CMD3/CMD7 + the fn0 CCCR chain    -> io_functions=1, cccr_rev=0x3
#     * the CIS tuple walk                -> manfid=0x2DF, cardid=0x9158
#     * Iw416::begin()'s fn1 register file and the SD8978 "new mode" init block
#       -> ioport=0x10000, card_status=0xD, and every cfg write taking effect
#   Every one of those tokens is byte-identical to the silicon run recorded in
#   transcript_hw_evkb.txt (W1/W3 sections) — they are the card's values, not
#   the driver's wishes, and this firmware contains none of them.
#
#   This is the FIRST automated proof that any of the M.2 SDIO path works. The
#   sibling gate run_qemu.sh asserts the opposite (card-absent fallback) and
#   still must: it is what a default sweep, with a plain SD memory card on
#   USDHC1, is supposed to see.
#
# WHAT THIS DOES *NOT* PROVE — read before trusting a green
#   * NO firmware download. The IW416 blob is NXP-licensed and is never in this
#     repo, so the committed build reports `fw_download=skipped (no blob
#     supplied)` and the download handshake is not exercised here at all. (The
#     model's download termination is also unsafe unless the gate sets
#     `-global iw416-sdio.fw-image-size=<len>`, and a gate cannot know the
#     length of a blob it does not have — one more reason this gate refuses to
#     run against a build that has one.)
#   * NO data path. The command/event port, the 32-slot rings, HOST_INT_STATUS
#     clear-on-read and frame injection are Phase 2 (design doc:
#     docs/superpowers/specs/2026-08-19-w14-qemu-iw416-design.md). Only Phase 2
#     can catch a W8- or W12-class bug; this gate would not have caught either.
#     Phase 2 now EXISTS and lives next door: networking/m2_rx_demo's
#     run_qemu_ring.sh (W8, the 32-slot ring) and run_qemu_stranded.sh (W12,
#     the stranded upload), both demonstrated red against a deliberately
#     un-fixed driver. If you came here looking for data-path coverage, that is
#     where it is — do not add it to this gate, whose whole value is that it
#     asserts enumeration alone and needs no blob.
#   * The model is not silicon. Its request sizes are a uniform chunk (silicon's
#     are image-derived — it publishes 2049, which is why requested_len is
#     deliberately NOT asserted below), its CCCR I/O-ready is immediate where a
#     real card takes milliseconds, and its CARD_CAPS/FUNCE tuple bodies are
#     plausible rather than measured. Nothing flagged as a guess in that design
#     doc's "Where the model could be lying" section is asserted here.
set -e
DIR=$(cd "$(dirname "$0")" && pwd)
EVKB=$(cd "$DIR/../../.." && pwd)
QEMU="$EVKB/tools/qrun"
. "$EVKB/tools/gate-lib.sh"
gate_init
# The M.2 socket (J54 on USDHC1) is a MIMXRT1170-EVKB feature and the model is
# only attached by the mimxrt1170-evk machine. Fail by name rather than handing
# `m2-wifi=on` to a machine that has no such property and blaming the firmware.
[ "$(gate_board)" = rt1176 ] || {
    echo "FAIL: this gate is rt1176-only (EVKB_BOARD=$(gate_board)) — the M.2 socket and the"
    echo "      iw416-sdio model both live on the MIMXRT1170-EVKB machine"; exit 1; }
ELF="$DIR/$(gate_build_dir)/m2_sdio_probe.elf"
# Distinct basenames from run_qemu.sh's serial.uart/serial.dbg ON PURPOSE: both
# gates run from this one directory and each starts with `rm -f`, so a shared
# name means one gate deleting the other's LIVE capture under `-j`.
OUT=$(gate_capture_path "$DIR" wifi.uart)
DBG=$(gate_capture_path "$DIR" wifi.dbg)
rm -f "$OUT" "$DBG"
# `-M ...` from gate-lib (no gate names a machine) plus one more `-machine`
# fragment for the opt-in property; QEMU merges the two into one machine opts
# group. Default is off, which is what keeps every other gate byte-identical.
"$QEMU" $(gate_qemu_machine) -machine m2-wifi=on -kernel "$ELF" \
    -display none $(gate_console "$OUT") \
    -d guest_errors -D "$DBG" &
P=$!; gate_pid $P
for _ in $(seq 1 40); do
    [ -f "$OUT" ] && grep -q "alive=2" "$OUT" 2>/dev/null && break
    sleep 0.25
done
gate_reap $P
gate_require_capture "$OUT"
echo "==== captured UART ===="; cat "$OUT"
grep -q "RT1176 M.2 SDIO probe up" "$OUT" || { echo "FAIL: banner missing"; exit 1; }
# THE NEGATIVE THAT MAKES THE REST MEAN SOMETHING. Without `m2-wifi=on` the
# board attaches a plain SD memory card, which by spec ignores CMD5 — the exact
# outcome run_qemu.sh asserts. If this gate ever saw that, every assertion below
# would be testing the wrong device, so check it first and say why.
if grep -q "^sdio_begin=cmd5-no-response" "$OUT"; then
    echo "FAIL: the card-absent fallback ran — the iw416-sdio model did not attach"
    echo "      (a QEMU without the model REJECTS -machine m2-wifi=on outright, which"
    echo "       shows up as an empty capture; reaching this line instead means the"
    echo "       property was accepted and a plain SD card was attached anyway)"
    exit 1
fi
grep -q "^sdio_begin=ok rc=0" "$OUT" || { echo "FAIL: SDIO enumeration did not succeed"; exit 1; }
# R4 is the card's own IO_SEND_OP_COND response: C=1 (ready), 1 IO function,
# OCR window 0xFF8000. Silicon prints exactly this. int_status is the host
# controller's, so it is matched loosely — the assertion is about the card.
#
# `[[:space:]]*$` rather than a bare `$` on every end-anchored pattern here:
# Print::println() emits CRLF, so a capture line ends "...0x90FF8000\r" and a
# plain `$` never matches. Costs nothing and it is the difference between an
# exact-value assertion and a prefix one.
grep -q "^int_status=0x[0-9A-F]* r4=0x90FF8000[[:space:]]*$" "$OUT" || {
    echo "FAIL: no r4=0x90FF8000 (the card's OCR + function count)"; exit 1; }
# ioport 0x10000 is read from IO_PORT_0..2; card_status 0xD is
# DN_LD_CARD_RDY|CARD_IO_READY. fw_status and requested_len are deliberately
# NOT asserted: both are download-phase values and differ from silicon here.
grep -q "^iw416_begin=ok ioport=0x10000 .* card_status=0xD " "$OUT" || {
    echo "FAIL: Iw416::begin() did not reach a ready function-1 ioport"; exit 1; }
# The SD8978 "new mode" init block, whole, pre->post. A post value missing its
# requested bit is a register that did not take the write — the defect that cost
# W3 (see transcript_hw_evkb.txt). Every character of this line is
# silicon-measured, so it is asserted whole.
grep -q "^iw416_cfg: fw_pre=0x0 d9=0x0->0x1 c4=0x0->0x4 c5=0x0->0x1 r04=0x0->0xFF d8=0x0->0x10[[:space:]]*$" "$OUT" || {
    echo "FAIL: the SD8978 new-mode config block did not take"; exit 1; }
grep -q "^io_functions=1[[:space:]]*$" "$OUT" || { echo "FAIL: wrong IO function count"; exit 1; }
grep -q "^cccr_rev=0x3[[:space:]]*$" "$OUT" || { echo "FAIL: wrong CCCR revision"; exit 1; }
# Off the wire from the card's CIS tuple chain. This firmware contains neither
# value, which is what makes them an oracle rather than an echo.
grep -q "^manfid=0x2DF[[:space:]]*$" "$OUT" || { echo "FAIL: wrong/absent SDIO manufacturer ID"; exit 1; }
grep -q "^cardid=0x9158[[:space:]]*$" "$OUT" || { echo "FAIL: wrong/absent SDIO card ID"; exit 1; }
if grep -q "^cis_error=" "$OUT"; then echo "FAIL: the CIS walk errored"; exit 1; fi
# This gate covers the NO-BLOB build — the only build a fresh clone can make,
# and the only one whose scope matches the Phase-1 model. A build configured
# with -DM2RADIO_IW416_FW runs the download and then the Phase-2 command port,
# which this model does not implement; refusing it is enforcing the design
# doc's "gates MUST set fw-image-size" rule, not hiding a regression.
grep -qF "fw_download=skipped (no blob supplied)" "$OUT" || {
    echo "FAIL: this build supplied a firmware blob — out of scope for the Phase-1"
    echo "      enumeration gate (it asserts nothing about download, and cannot set"
    echo "      iw416-sdio.fw-image-size for a blob it does not have)"; exit 1; }
# A reason code alone is not enough: probe_done proves we left begin(), and
# alive= proves the image is still running afterwards rather than wedged.
grep -q "^probe_done[[:space:]]*$" "$OUT" || { echo "FAIL: probe never completed"; exit 1; }
grep -q "^alive=2[[:space:]]*$" "$OUT" || { echo "FAIL: no heartbeat after the probe"; exit 1; }
echo "PASS: SDIO enumerated the IW416 model — ioport 0x10000, manfid 0x2DF, cardid 0x9158"
