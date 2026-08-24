#!/bin/sh
# run_qemu_hci.sh -- the [hci] gate for m2_hci_probe (BT-1): the HCI transport
# is BIDIRECTIONAL, and its failure paths are named.
#
# WHAT THIS PROVES
#   qemu2 binds the second -serial to LPUART2 (hw/arm/fsl-imxrt1170.c:1110)
#   and its LPUART model receives from its chardev, so a UNIX socket puts
#   hci_peer.py -- a fake controller in this directory -- on the card's HCI
#   port with NO change to qemu2.  Four QEMU runs against four peer phases:
#     full        every B1 field and every B2 inquiry line carries the peer's
#                 value (manufacturer 0x1234, bd 11:22:33:44:55:66, two
#                 FAKE-HEADSET-* devices) -- values this firmware cannot invent;
#     drop-reset  no reply: the firmware times out BY NAME after 10 attempts;
#     garbage     3 x 0xFF before the first reply: attempt 1 fails as FRAMING
#                 (not timeout), attempt 2 succeeds after the 50 ms resync;
#     starve      Num_HCI_Command_Packets=0: every later command fails as
#                 ncmd_starved, by name, and the probe still completes.
#   The socket lives in /tmp: macOS caps sun_path at 104 bytes and this repo's
#   path alone can exceed it (the four mon.sock gates need /tmp/ev for that).
#   `server` WITHOUT `nowait`: QEMU holds the guest until the peer connects,
#   so the firmware's first Reset cannot be lost to an empty chardev and every
#   count below is strict -- attempts=2 in [garbage] means the driver, not
#   the timing.
#
# WHAT THIS DOES NOT PROVE
#   Anything about the IW416, or about baud (a chardev has none).  Silicon's
#   answers live in transcript_hw_evkb.txt.  ★ As of 2026-08-23 that file
#   records a NEGATIVE: a real IW416 has never answered an HCI command, so
#   NOTHING here says the card works -- only that the driver does what the
#   specification says against a peer that follows it.  That is precisely why
#   this gate is worth having: it is the only place the driver is exercised
#   against a controller that ANSWERS.
#
# DEMONSTRATED RED (2026-08-23), twice, and the two are deliberately different
# kinds of lie:
#   * Changing what the FAKE says -- hci_peer.py's MANUFACTURER 0x1234 -> 0x1235,
#     nothing else touched -- failed by name:
#       FAIL: [full] hci_version does not carry the peer's values
#     which is what makes the [full] field assertions a round-trip proof rather
#     than a check that the firmware can print.
#   * Breaking the DRIVER -- M2Radio hci/Hci.cpp, onPacket's Command Complete
#     branch, `opcode == m_inflightOpcode` -> `opcode == (m_inflightOpcode ^ 1)`
#     -- failed by name:
#       FAIL: [full] Reset timed out although replies ARRIVED (late>0) -- they
#             matched no in-flight command, so this is the driver, not the socket
#     (capture: `hci_reset=timeout reason=no_response attempts=10 timeouts=10
#     framing=0 starved=0 qfull=0 late=10`), while run_qemu.sh in this same
#     directory STAYED GREEN on the same ELF -- it has no replies to match, so
#     it cannot see this bug at all.  That asymmetry is the whole argument for
#     this variant existing.
#   The second demonstration also earned the late=0/late>0 split in run_phase
#   below: before it, this gate reported the broken driver as "the peer never
#   reached LPUART2" and sent the reader to the socket.
set -e
DIR=$(cd "$(dirname "$0")" && pwd)
EVKB=$(cd "$DIR/../../.." && pwd)
QEMU="$EVKB/tools/qrun"
. "$EVKB/tools/gate-lib.sh"
gate_init
[ "$(gate_board)" = rt1176 ] || {
    echo "FAIL: this gate is rt1176-only (EVKB_BOARD=$(gate_board)) -- the M.2 socket is on the MIMXRT1170-EVKB"; exit 1; }
ELF="$DIR/$(gate_build_dir)/m2_hci_probe.elf"

fail() { echo "FAIL: $*"; exit 1; }

# run_phase PHASE WAIT_REGEX -> capture in $OUT, peer output in $RES, peer rc in $PEER_RC
run_phase() {
    PHASE=$1; WAIT=$2
    OUT=$(gate_capture_path "$DIR" "hci_$PHASE.uart")
    DBG=$(gate_capture_path "$DIR" "hci_$PHASE.dbg")
    RES=$(gate_capture_path "$DIR" "hci_$PHASE.peer")
    rm -f "$OUT" "$DBG" "$RES"
    SOCK="/tmp/m2hci_$$_$PHASE.sock"; rm -f "$SOCK"; gate_tmp "$SOCK"
    "$QEMU" $(gate_qemu_machine) -kernel "$ELF" -display none \
        $(gate_console "$OUT") -serial unix:"$SOCK",server \
        -d guest_errors -D "$DBG" &
    P=$!; gate_pid $P
    PEER_RC=0
    python3 "$DIR/hci_peer.py" "$PHASE" "$SOCK" > "$RES" 2>&1 || PEER_RC=$?
    # Wait for the LAST line this phase parses, never the first interesting one.
    for _ in $(seq 1 120); do
        [ -f "$OUT" ] && grep -q "$WAIT" "$OUT" 2>/dev/null && break
        sleep 0.25
    done
    gate_reap $P
    gate_require_capture "$OUT" "phase $PHASE"
    echo "==== captured UART ($PHASE) ===="; cat "$OUT"
    echo "==== peer ($PHASE) ===="; cat "$RES"
    grep -q "RT1176 M.2 HCI probe up" "$OUT" || fail "[$PHASE] banner missing"
    if grep -q "^hci_reset=timeout" "$OUT" && [ "$PHASE" != drop-reset ]; then
        # Two very different faults reach this line, and `late=` separates them:
        # it counts replies that ARRIVED and matched no in-flight opcode.  So
        # late=0 means nothing came back at all (the peer never reached
        # LPUART2 -- look at the socket), and late>0 means the bytes made the
        # round trip and the DRIVER did not recognise them (look at Hci.cpp).
        # Naming the first when the second happened sends the reader to the
        # wrong half of the system.  Measured: the Step 6 opcode-match mutation
        # lands here with late=10, having been reported as a socket fault until
        # this branch existed.
        if grep -q "^hci_reset=timeout.*late=0[[:space:]]*$" "$OUT"; then
            fail "[$PHASE] the card-absent fallback ran and NOTHING came back (late=0) -- the peer never reached LPUART2"
        fi
        fail "[$PHASE] Reset timed out although replies ARRIVED (late>0) -- they matched no in-flight command, so this is the driver, not the socket"
    fi
}

# --- full ---------------------------------------------------------------------
run_phase full '^hb card=0 btfw=no_start_indication hci=ok n=1 '
# No bootloader on this peer, so the download must find nothing -- and say so.
# Asserting it here is what stops [fwdnld]'s success being attributed to the
# firmware rather than to the peer that answered.
grep -q "^bt_fw_download=no_start_indication chip_id=0x0000 loader_ver=0 start_inds=0 chunks=0 sent=0/1024 " "$OUT" \
    || fail "[full] the download should have found no bootloader"
grep -q "^hci_reset=ok attempts=" "$OUT" || fail "[full] no hci_reset=ok"
grep -q "^hci_version: hci_ver=11 hci_rev=0xBEEF lmp_ver=11 manufacturer=0x1234 lmp_subver=0xCAFE[[:space:]]*$" "$OUT" \
    || fail "[full] hci_version does not carry the peer's values"
grep -q "^bd_addr=11:22:33:44:55:66[[:space:]]*$" "$OUT" || fail "[full] bd_addr does not carry the peer's value"
grep -q "^hci_buffer: acl_len=1021 acl_num=8 sco_len=64 sco_num=0[[:space:]]*$" "$OUT" || fail "[full] hci_buffer wrong"
grep -q "^inquiry=started[[:space:]]*$" "$OUT" || fail "[full] inquiry did not start"
grep -q "^inq: bd=AA:BB:CC:DD:EE:01 cod=0x240404 psrm=1 clk=0x1234[[:space:]]*$" "$OUT" || fail "[full] inquiry result 1 wrong (field-major parse?)"
grep -q "^inq: bd=AA:BB:CC:DD:EE:02 cod=0x240404 psrm=1 clk=0x1234[[:space:]]*$" "$OUT" || fail "[full] inquiry result 2 wrong (field-major parse?)"
grep -q "^inquiry_complete: status=0x00 n=2[[:space:]]*$" "$OUT" || fail "[full] inquiry_complete wrong"
grep -q '^inq_name: bd=AA:BB:CC:DD:EE:01 status=0x00 name="FAKE-HEADSET-01"[[:space:]]*$' "$OUT" || fail "[full] remote name 1 wrong"
grep -q '^inq_name: bd=AA:BB:CC:DD:EE:02 status=0x00 name="FAKE-HEADSET-02"[[:space:]]*$' "$OUT" || fail "[full] remote name 2 wrong"
grep -q "^hci_probe_done[[:space:]]*$" "$OUT" || fail "[full] probe never completed"
grep -q "^hb card=0 btfw=no_start_indication hci=ok n=1 pump=[0-9]* timeouts=0 framing=0 starved=0 qfull=0 late=0[[:space:]]*$" "$OUT" \
    || fail "[full] heartbeat counters not all zero"
[ "$PEER_RC" -eq 0 ] || fail "[full] peer exited $PEER_RC"
grep -q "^PEER-DONE phase=full cmds=7 " "$RES" || fail "[full] peer did not see exactly the seven commands (Reset, 3 identity, Inquiry, 2 names)"

# --- drop-reset ---------------------------------------------------------------
run_phase drop-reset '^hb card=0 btfw=no_start_indication hci=no_response n=1 '
grep -q "^hci_reset=timeout reason=no_response attempts=10 timeouts=10 framing=0 starved=0 qfull=0 late=0[[:space:]]*$" "$OUT" \
    || fail "[drop-reset] Reset must time out BY NAME after ten counted attempts"
if grep -q "^hci_version" "$OUT"; then fail "[drop-reset] identity printed with no Reset"; fi
grep -q "^hci_probe_done[[:space:]]*$" "$OUT" || fail "[drop-reset] probe never completed"
grep -q "^PEER-DONE phase=drop-reset cmds=10 resets=10 " "$RES" || fail "[drop-reset] peer did not see ten Resets"

# --- garbage ------------------------------------------------------------------
run_phase garbage '^hb card=0 btfw=no_start_indication hci=ok n=1 '
grep -q "^hci_reset=ok attempts=2 timeouts=0 framing=1 starved=0 qfull=0 late=0[[:space:]]*$" "$OUT" \
    || fail "[garbage] attempt 1 must fail as FRAMING (not timeout) and attempt 2 must succeed"
grep -q "^hci_version: .*manufacturer=0x1234 " "$OUT" || fail "[garbage] the run did not recover to a full identity"
grep -q '^inq_name: bd=AA:BB:CC:DD:EE:02 status=0x00 name="FAKE-HEADSET-02"[[:space:]]*$' "$OUT" || fail "[garbage] inquiry did not complete after recovery"
grep -q "^hb card=0 btfw=no_start_indication hci=ok n=1 pump=[0-9]* timeouts=0 framing=1 starved=0 qfull=0 late=0[[:space:]]*$" "$OUT" \
    || fail "[garbage] heartbeat counters wrong"
[ "$PEER_RC" -eq 0 ] || fail "[garbage] peer exited $PEER_RC"

# --- starve -------------------------------------------------------------------
run_phase starve '^hb card=0 btfw=no_start_indication hci=ok n=1 '
grep -q "^hci_reset=ok attempts=1 " "$OUT" || fail "[starve] Reset itself must succeed"
for W in hci_version bd_addr hci_buffer inquiry; do
    grep -q "^$W=fail reason=ncmd_starved " "$OUT" || fail "[starve] $W must fail as ncmd_starved, by name"
done
grep -q "^hci_probe_done[[:space:]]*$" "$OUT" || fail "[starve] probe never completed"
grep -q "^hb card=0 btfw=no_start_indication hci=ok n=1 pump=[0-9]* timeouts=0 framing=0 starved=4 qfull=0 late=0[[:space:]]*$" "$OUT" \
    || fail "[starve] expected starved=4 and nothing else"
grep -q "^PEER-DONE phase=starve cmds=1 " "$RES" || fail "[starve] the firmware must not have SENT anything after Reset"

# --- fwdnld -------------------------------------------------------------------
# The V3 UART firmware download, end to end, against a peer PLAYING THE CARD'S
# BOOTLOADER -- it replays the exact five-byte start indication a real
# M2-MAYA-W161 sent on the bench (AB 01 72 00 47), serves four chunk requests,
# and CHECKS THE BYTES COMING BACK against the image it knows the gate build
# compiled in.  A downloader that acknowledged correctly but served the wrong
# offsets would pass every other assertion here and fail this one.
#
# ★ No NXP blob is involved.  The gate build compiles a 1 KB SYNTHETIC image
# (the example's CMakeLists) precisely so this protocol is gateable without a
# licensed file; the firmware prints bt_fw_source=synthetic and this gate
# ASSERTS that, so a build carrying the real blob can never be mistaken for it.
# DEMONSTRATED RED (2026-08-24), twice, and the FIRST one is the argument for
# checking bytes rather than counters:
#   * BtFwLoader served every chunk from offset 0 instead of the requested
#     offset.  EVERY OTHER ASSERTION STAYED GREEN -- acks correct, chunks=4,
#     sent=1024/1024, retx=0, crc_err=0, HCI came up afterwards.  Only the
#     peer's byte check caught it:
#       PEER-BOOT ok=1 acks=11 chunks=4 err=wrong image bytes at offset 768 (+255)
#       FAIL: [fwdnld] peer exited 1 -- it verifies the served bytes, so this is the driver
#     A downloader that hands a card the wrong 129 KB fails far from here, at
#     firmware authentication, with nothing to point at.
#   * crc8()'s init changed from 0xFF to 0x00: every frame rejected,
#     `crc_err=21 ... no_start_indication`, gate failed by name.
run_phase fwdnld '^hb card=0 btfw=ok hci=ok n=1 '
grep -q "^bt_fw_source=synthetic[[:space:]]*$" "$OUT" \
    || fail "[fwdnld] this build must carry the SYNTHETIC image, not a real blob"
# start_inds= is deliberately NOT pinned to a number: QEMU holds the guest until
# the peer connects, so the peer greets while the firmware is still in its board
# preamble and repeats until heard -- exactly as the real card repeats.  How
# many land before the loader drains its ring is a property of that race, not of
# the driver.  Everything the DRIVER controls is pinned exactly.
grep -qE "^bt_fw_download=ok chip_id=0x7201 loader_ver=0 start_inds=[1-9][0-9]* chunks=4 sent=1024/1024 max_off=1024 retx=0 crc_err=0 card_err=0x0000[[:space:]]*$" "$OUT" \
    || fail "[fwdnld] the download did not complete cleanly with the expected accounting"
# ★ The RAW reset fired immediately after the download, before any other code
# runs, must be ANSWERED here.  This is the exact assertion silicon fails: on
# the bench the same build, after an equally complete download, gets n=0.
# Pinning it here is what makes that silence attributable to the CARD rather
# than to this firmware -- same code, same path, different answer.
grep -q "^bt_raw_reset\[0\]: n=7 hex=040E0401030C00[[:space:]]*$" "$OUT" \
    || fail "[fwdnld] the raw post-download reset was not answered"
grep -q "^hci_reset=ok attempts=1 " "$OUT" || fail "[fwdnld] HCI did not start after the download"
grep -q "^bd_addr=11:22:33:44:55:66[[:space:]]*$" "$OUT" || fail "[fwdnld] HCI did not work after the download"
[ "$PEER_RC" -eq 0 ] || fail "[fwdnld] peer exited $PEER_RC -- it verifies the served bytes, so this is the driver"
grep -qE "^PEER-BOOT ok=1 acks=[0-9]+ chunks=4 err=none[[:space:]]*$" "$RES" \
    || fail "[fwdnld] the peer did not see a clean download (it checks every byte it was served)"
grep -q "^PEER-BOOT-COMPLETE chunks=4 bytes=1024[[:space:]]*$" "$RES" || fail "[fwdnld] peer never saw the image complete"

echo "PASS: HCI transport is bidirectional against the fake controller, the V3 firmware download serves the right bytes, and timeout/framing/starvation fail by name"
