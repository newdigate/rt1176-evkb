#!/bin/sh
# run_qemu_wifi.sh — the PROBE-MACHINERY gate for m2_uap_probe.
#
# WHAT THIS PROVES
#   Against QEMU's IW416/SD8978 card model (qemu2 hw/sd/iw416-sdio.c, opt-in
#   via `-machine mimxrt1170-evk,m2-wifi=on`), the whole Phase-0 probe runs end
#   to end and REACHES A CORRECT NEGATIVE:
#     * the positive control (GET_HW_SPEC 0x0003) is answered on both
#       interfaces — so the command port, the seq correlation and the new
#       BSS-addressed send path all work;
#     * every AP command comes back indistinguishable from the reserved id
#       0x7FFE, because the model implements no AP commands at all and answers
#       everything unmodelled with HostCmd_RESULT_ERROR (its NOTE 13);
#     * therefore the verdict is NOT the supported one.
#
#   That is worth gating for one reason: it is the only automated proof that a
#   NEGATIVE Phase-0 answer is reachable and readable. If the probe could only
#   ever print SUPPORTED, its silicon answer would be worthless.
#
#   ★ WHAT THE bss=1 ROWS DO AND DO NOT SAY. W17 made seq_num carry bss_type
#   in bits 15:12, and every command this driver had ever sent left that at 0.
#   THE MODEL DOES NOT ROUTE ON THAT FIELD — it has no uAP interface to route
#   to. So these rows do NOT prove a command reaches the uAP interface; only
#   silicon can say that. What they DO prove is that a BSS-addressed send
#   produces a well-formed packet the card answers and the driver correlates —
#   that the new field did not corrupt the header, or the seq the reply is
#   matched against.
#   ★ UPDATED 2026-08-22: the model now ECHOES THE WHOLE seq_num, bss nibbles
#   included, because that is what silicon does (a bss=1 reply carries 0x1004,
#   not 0x0004 — qemu2 721fb09146, anchored to this example's own silicon
#   capture). It used to zero the high byte, and that HID A BUG CLASS: a driver
#   comparing the full 16-bit seq instead of masking to the low byte passed
#   here and rejected every uAP reply on the bench. It now fails this gate.
#   So these rows say more than they used to — but still not that the uAP
#   interface exists.
#   DEMONSTRATED RED, not asserted on faith (2026-08-20): sendHostCmdBss() was
#   temporarily changed to OR bssType into seq_num's LOW nibble instead of
#   shifting it to bits 15:12 — a plausible transcription slip — and every
#   bss=1 cell went `st=cmd-timeout` (the card answered with a seq the driver
#   no longer recognised) while every bss=0 cell stayed green. This gate went
#   red at the positive control, by name. The driver was then restored and it
#   went green again.
#   ★ NOTE WHAT DID NOT CATCH IT: `uap_verdict=` was UNCHANGED
#   (INDISTINGUISHABLE_FROM_UNKNOWN_CMD both ways), because half the matrix
#   silently vanishing does not move a verdict computed from what is left.
#   The tally (answered=6 -> answered=3) and the per-cell assertions caught it.
#   That is why the AP rows are checked ONE AT A TIME rather than by count, and
#   why the tally is asserted as well as the verdict.
#
# WHAT THIS DOES *NOT* PROVE — read before trusting a green
#   * NOTHING about real AP support. The model has no uAP layer and never
#     will pretend to have one before it is designed against
#     mlan_uap_cmdevent.c. A green here is a green for the PROBE, not an
#     answer about the card. The answer is silicon's and lives in
#     transcript_hw_evkb.txt.
#   * NOT firmware download. The model runs with `fw-preboot=on`, an admitted
#     fiction (its NOTE 7) standing in for a download that needs the
#     NXP-licensed blob no gate may depend on.
#   * NOT that HostCmd_RESULT_ERROR is what silicon returns for an unknown
#     command. It is what the MODEL returns. Which is precisely why the probe
#     carries its own negative control and compares against it at runtime
#     instead of hardcoding an expected code.
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
ELF="$DIR/$(gate_build_dir)/m2_uap_probe.elf"
# Distinct basenames from run_qemu.sh's serial.uart/serial.dbg ON PURPOSE: both
# gates run from this one directory and each starts with `rm -f`, so a shared
# name means one gate deleting the other's LIVE capture under `-j`.
OUT=$(gate_capture_path "$DIR" wifi.uart)
DBG=$(gate_capture_path "$DIR" wifi.dbg)
rm -f "$OUT" "$DBG"
"$QEMU" $(gate_qemu_machine) -machine m2-wifi=on \
    -global iw416-sdio.fw-preboot=on \
    -kernel "$ELF" -display none $(gate_console "$OUT") \
    -d guest_errors -D "$DBG" &
P=$!; gate_pid $P
# Wait for the heartbeat, the LAST line printed — not for uap_probe_done or
# the verdict, both of which have output after them. See run_qemu.sh.
for _ in $(seq 1 120); do
    [ -f "$OUT" ] && grep -q "^hb card=1" "$OUT" 2>/dev/null && break
    sleep 0.25
done
gate_reap $P
gate_require_capture "$OUT"
echo "==== captured UART ===="; cat "$OUT"
grep -q "RT1176 M.2 uAP probe up" "$OUT" || { echo "FAIL: banner missing"; exit 1; }

# ★ THE GATED BUILD MUST NOT TRANSMIT, asserted directly rather than by proxy.
# This used to check that the BSS_START/BSS_STOP matrix rows printed SKIPPED.
# Once those rows moved into a real start/stop SEQUENCE that only exists under
# -DM2_UAP_PROBE_BSS_START, the SKIPPED lines stopped being emitted at all and
# the old check would have had to be deleted -- taking the only "does not
# transmit" assertion with it.  This is the stronger replacement: the sequence
# announces itself, and its announcement must be absent.
if grep -q "^uap_bss_seq=begin" "$OUT"; then
    echo "FAIL: this build RUNS THE TRANSMITTING SEQUENCE and must not be gated"
    echo "      (M2_UAP_PROBE_BSS_START is ON in the gated build directory)"; exit 1
fi
for T in "^uap_bss cmd=0x00B1" "^uap_bss=beaconing"; do
    if grep -q "$T" "$OUT"; then
        echo "FAIL: found evidence of BSS_START in a gated run ($T)"; exit 1
    fi
done
# ★ Checked HERE, before anything else, and that position is deliberate. A
# transmitting build also compiles OUT the trailing positive controls, so the
# control loop below would fail FIRST and report "positive control f did not
# answer" -- blaming a missing control for what is actually a build that
# transmits. Measured, not imagined: that is exactly what it printed before this
# moved. A safety check has to be the first thing to speak, or it gets
# pre-empted by its own side effects.

# THE NEGATIVE THAT MAKES THE REST MEAN SOMETHING. Without the model attached
# the board gets a plain SD memory card, which ignores CMD5 — the outcome
# run_qemu.sh asserts. Every assertion below would then be vacuous.
if grep -q "^sdio_begin=cmd5-no-response" "$OUT"; then
    echo "FAIL: the card-absent fallback ran — the iw416-sdio model did not attach"
    echo "      (a QEMU without the model REJECTS -machine m2-wifi=on outright, which"
    echo "       shows up as an empty capture; reaching this line instead means the"
    echo "       property was accepted and a plain SD card was attached anyway)"
    exit 1
fi
grep -q "^sdio_begin=ok " "$OUT" || { echo "FAIL: SDIO enumeration did not succeed"; exit 1; }
# `[[:space:]]*$` rather than a bare `$` on end-anchored patterns: Print's
# println() emits CRLF, so a capture line ends "...\r" and a plain `$` never
# matches.
grep -q "^fw_download=skipped (no blob supplied) preboot=1[[:space:]]*$" "$OUT" || {
    echo "FAIL: this gate needs the no-blob build running against fw-preboot=on"
    echo "      (a build configured with -DM2RADIO_IW416_FW downloads instead, which"
    echo "       this model does not implement)"; exit 1; }
grep -q "^hb card=1" "$OUT" || { echo "FAIL: never reached a serviced card"; exit 1; }

# --- THE POSITIVE CONTROLS -------------------------------------------------
# GET_HW_SPEC is answered by the model. EVERY positive control in the matrix is
# required, on both interfaces: the verdict's bracketing rule only means
# something if the controls themselves are read, and a control that quietly
# stopped being emitted would silently turn every AP row unbracketed while the
# verdict happily reported on whatever was left.
for N in a b c d e e2 e3 f g; do
    for B in 0 1; do
        grep -q "^uap_probe cmd=0x0003 name=HWSPEC.ctl+.$N bss=$B st=ok " "$OUT" || {
            echo "FAIL: positive control $N did not answer on bss=$B"
            echo "      (bss=0 failing too means the command port itself is broken; ONLY"
            echo "       bss=1 failing points at W17's seq_num bss-addressing — either"
            echo "       sendHostCmdBss packing the bss nibbles where they collide with"
            echo "       the seq, or waitCmdResp comparing the whole 16-bit seq instead"
            echo "       of masking to the low byte)"; exit 1; }
    done
done
# Both negative controls, at both ends of the matrix.
for N in a b; do
    for B in 0 1; do
        grep -q "^uap_probe cmd=0x7FFE name=RSVD.ctl-.$N bss=$B st=ok .* result=0x0001 " "$OUT" || {
            echo "FAIL: the reserved-id control $N did not answer as the model documents on bss=$B"
            echo "      (expected st=ok result=0x0001 — HostCmd_RESULT_ERROR, iw416-sdio NOTE 13)"
            exit 1; }
    done
done

# --- W17 FAULT 1: the body / uAP-interface controls -------------------------
# These separate "SYS_CONFIGURE is the port-killer" from "a bodied command on
# this port is the port-killer" — a distinction the first silicon runs could
# not make, because every row that answered there was empty-bodied and both
# rows that wedged carried a body. They are asserted here so the controls
# cannot quietly stop being emitted between now and the next silicon run; the
# model does not reproduce the wedge, so this gate proves the rows RUN and are
# READ, not what silicon answers.
for B in 0 1; do
    grep -q "^uap_probe cmd=0x7FFE name=RSVD.body bss=$B st=ok .* result=0x0001 " "$OUT" || {
        echo "FAIL: the bodied reserved-id control did not answer on bss=$B"
        echo "      (expected st=ok result=0x0001 — this row is what says a 10-byte"
        echo "       bodied command reaches the parser at all)"; exit 1; }
    grep -q "^uap_probe cmd=0x004D name=MACADDR.uap bss=$B st=ok " "$OUT" || {
        echo "FAIL: the uAP-interface control (MAC_ADDRESS GET) did not answer on bss=$B"
        exit 1; }
done
grep -q "^uap_bsscheck MACADDR.uap .* bracketed=1[[:space:]]*$" "$OUT" || {
    echo "FAIL: the uAP-interface control was not reported, or was not bracketed"; exit 1; }

# --- THE AP ROWS -------------------------------------------------------------
# The model answers unmodelled commands with HostCmd_RESULT_ERROR, so each AP
# command must come back answered-but-erroring, exactly like the reserved id.
# Asserted per cell rather than via the tally so a single row going quietly
# missing cannot be absorbed by a count.
for C in 00AE:SYS_INFO 00B3:STA_LIST 00B0:SYSCFG.fullopen 00B0:SYSCFG.chantlv 00B0:SYSCFG.bare; do
    ID=${C%%:*}; NAME=${C#*:}
    for B in 0 1; do
        grep -q "^uap_probe cmd=0x$ID name=$NAME bss=$B st=ok .* result=0x0001 " "$OUT" || {
            echo "FAIL: $NAME on bss=$B did not come back as the model's unknown-command error"
            echo "      (expected st=ok result=0x0001 — HostCmd_RESULT_ERROR, iw416-sdio NOTE 13)"
            exit 1; }
    done
done


# --- Iw416::uapConfigure(), the one SYS_CONFIGURE shape silicon ACCEPTS ------
# Silicon (2026-08-21) answers this RESULT_OK on both interfaces while every
# MINIMAL SYS_CONFIGURE kills the command port; this model answers its usual
# unknown-command ERROR and wedges nothing, so what this gate proves is that the
# driver still BUILDS AND SENDS the request, byte for byte.  That is the part
# that can silently rot -- a wrong TLV length would still compile, still send,
# and would only be caught on the bench.
#
# 82 bytes for the default open configuration, and the length is asserted
# because it is the cheapest check that catches a mis-sized TLV:
#   action 2 + MAC 10 + SSID 19 + beacon 6 + DTIM 5 + rates 16 + bcast 5
#   + chan/band 6 + auth 7 + protocol 6 = 82
grep -q "^uap_cfg_bytes len=82 01002B0106" "$OUT" || {
    echo "FAIL: uapConfigure() built the wrong request — expected an 82-byte body"
    echo "      opening action=SET(0100) then TLV 0x012B len 6 (the MAC)."
    echo "      A changed length means a TLV payload length moved; those are"
    echo "      PAYLOAD lengths, not whole-TLV lengths (DTIM and BCAST are 1,"
    echo "      AUTH is 3), and getting one wrong mis-parses the whole soup."
    exit 1; }
grep -q "^uap_cfg_req mask=0x01FF ssid=" "$OUT" || {
    echo "FAIL: the full open-AP TLV set was not requested (mask should be ALL_OPEN)"
    exit 1; }

# --- THE VERDICT -------------------------------------------------------------
# The negative control's own signature, echoed so a reader can check the
# comparison the firmware-independent way. bracketed=1 is the load-bearing
# field: it says the control was itself measured on a port proven healthy on
# both sides, which is what makes it a yardstick rather than a reading.
grep -q "^uap_control neg_sta_st=ok neg_sta_result=0x0001 neg_uap_st=ok neg_uap_result=0x0001 bracketed=1 seq_mismatches=0[[:space:]]*$" "$OUT" || {
    echo "FAIL: the reserved-id control did not behave as the model documents,"
    echo "      or was not itself bracketed by healthy positive controls"; exit 1; }
# ★ unbracketed=0 is asserted, not just tolerated. On this model nothing wedges
# the command port, so every AP cell MUST be bracketed; a non-zero count here
# means the model started dropping replies and the run has quietly measured
# less than it claims — the same class of defect as a SKIP hiding in a sweep.
grep -q "^uap_tally bracketed=10 distinct_from_neg=0 unbracketed=0[[:space:]]*$" "$OUT" || {
    echo "FAIL: wrong tally — all TEN AP cells must be bracketed and none may"
    echo "      differ from the reserved-id control"; exit 1; }
grep -q "^uap_verdict=INDISTINGUISHABLE_FROM_UNKNOWN_CMD[[:space:]]*$" "$OUT" || {
    echo "FAIL: wrong verdict for a model with no uAP layer"; exit 1; }
# ★ The one that must never pass here. If the model ever grows a uAP surface
# this line is the tripwire that says so, and this gate must be rewritten
# rather than relaxed.
if grep -q "^uap_verdict=SUPPORTED" "$OUT"; then
    echo "FAIL: claimed uAP support against a model that has no AP commands"; exit 1
fi
# --- W17 FAULT 1: the card-state autopsy and the recovery probe -------------
# Nothing wedges on this model, so both readings have a KNOWN value here and
# asserting them is what stops the autopsy from rotting into dead code between
# silicon runs — the failure mode being that it silently stops reading the card
# and the next wedge is diagnosed with no state again.
for T in init final; do
    grep -q "^uap_cardreg tag=$T cmd52=ok .* fwstatus=0xFEDC " "$OUT" || {
        echo "FAIL: the '$T' card-state dump is missing, could not read the card over"
        echo "      CMD52, or did not see FIRMWARE_READY — on this model the firmware"
        echo "      is always up, so anything else means the dump itself is broken"
        exit 1; }
done
# The port is alive at the end here, so the recovery loop must NOT run: if this
# ever reads a number or 'never', the model started dropping command replies and
# every reading above is suspect.
grep -q "^uap_recover=n/a reason=command_port_alive_at_end[[:space:]]*$" "$OUT" || {
    echo "FAIL: the command port did not survive the matrix on a model where"
    echo "      nothing wedges it"; exit 1; }
grep -q "^uap_probe_done[[:space:]]*$" "$OUT" || { echo "FAIL: probe never completed"; exit 1; }
echo "PASS: the probe reached a correct NEGATIVE — every AP command indistinguishable from the reserved id, on both bss values"
