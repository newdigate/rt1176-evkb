#!/bin/sh
# run_qemu_reconnect.sh -- the [reconnect] gate for m2_hci_probe (NEW-34 piece 1): a
# bonded device is reconnected with NO inquiry and NO pairing, from a bond that made a
# round trip through the EEPROM store; a bond the peer rejects is erased and replaced.
#
# WHAT THIS PROVES
#   Built with -DM2_BT_CONNECT=ON -DM2_BT_TARGET_NAME=FAKE-HEADSET-01 -DM2_BT_RECONNECT=ON,
#   probeReconnect() drives the SHIPPED stack (A2dpSource + BondTable + BondStoreEeprom)
#   FOUR times in one boot against hci_peer.py's `reconnect` phase:
#     1. fresh pairing: inquiry -> SSP -> key #1 notified -> bond saved -> AVDTP STREAMING;
#     2. cold reload from the EEPROM emulation (RAM wiped, the journal index rebuilt from
#        the flash, loaded), a DECOY bond planted at the front and reloaded again, then a
#        page of the bonded address with NO inquiry, Link_Key_Request_Reply carrying key #1,
#        no IO-capability dance, STREAMING again -- the name filter must skip the decoy;
#     3. the peer REJECTS the offered key (Authentication_Complete 0x06): the bond is erased,
#        a fresh pairing yields key #2, the store holds it, STREAMING a third time.
#     4. a third cold reload, then a stored-key connect on key #2 -- verified by the PEER
#        against the key it notified, the only corroboration of key_changed (the probe's own
#        comparison is against its own memory, and key #2 has now made key #1's round trip).
#   Every tally value is counted by the PEER (inquiries, pages, key replies and whether each
#   matched the key it notified, per-link handles), so none can be satisfied by printing.
#   The peer allocates a FRESH handle per link and refuses ACL or Disconnect on a stale one,
#   and each link is torn down before the next page: four clean L2cap/Avdtp re-inits.
#
# ★ OWNS ITS OWN BUILD DIRECTORY, build-reconnect/ (same convention as build-avdtp/).
#
# WHAT THIS DOES NOT PROVE
#   Persistence across a POWER CYCLE (QEMU has no backing store behind the FlexSPI window --
#   the cold reload is a within-boot sector rescan, the same instrument
#   storage-memory/eeprom_test uses); a real headset's willingness to accept a stored key;
#   and the page(name) seam on the rejection path (the probe's inquiry hits persist for the
#   whole boot, so the name never has to come from page() here -- a2dpsource_test scenario 8
#   pins that on the host).  The bench claims live in audio/bt_tone_test/transcript_hw_evkb.txt.
#
# TIMING
#   Measured ~17 s of QEMU+peer wall (18 s for the gate steady-state, ~26 s when build-reconnect/
#   has to rebuild), so the four links leave the peer's budget more than half unused.
#   hci_peer.py gives up at 50 s (DEADLINE), BELOW
#   tools/qrun's 60 s QRUN_TIMEOUT, so on a hung run the peer announces (PEER-DEADLINE)
#   before QEMU is killed; if QEMU dies first the peer prints PEER-EOF.  Do NOT raise
#   QRUN_TIMEOUT here without raising the peer's deadline with it.
#
# DEMONSTRATED RED (2026-09-06), four mutations of M2Radio/bt run against this gate, each failing
# BY THE NAMED ASSERTION, each reverted (git -C M2Radio checkout -- bt/) and the gate confirmed GREEN
# again before the next; plus one host-suite arm that this gate cannot reach:
#   (a) BtLink answers every Link_Key_Request negatively  -> "FAIL: [reconnect] phase 2 did not
#       authenticate with the stored key"  (peer tally corroborates: key_replies=0 neg_replies=4
#       iocap_dances=4 -- every link re-paired by SSP)
#   (b) BondStoreEeprom::save() is a no-op                -> "FAIL: [reconnect] bond did not survive
#       the cold reload (expected 2 = the paired device + the decoy, decoy in front):
#       bonds_reload=0 front=\"\""
#   (c) the rejection rung keeps the bond                 -> "FAIL: [reconnect] the rejected key was
#       offered again -- the bond was not erased: PEER-KEY-STALE-REPLAY link=3"
#   (d) A2dpSource ignores the target-name filter         -> "FAIL: [reconnect] a bond whose name does
#       not match the target was paged -- the name filter is gone: PEER-DECOY-PAGED"
#   (e) BondTable::load() skips the CRC                   -> bondtable_test's flipped-key-byte arm (host
#       suite), not this gate.  It needs a (void)want; beside it or -Werror rejects the mutant unused.
# ★ Only (c) waits out the peer's 50 s deadline (52 s wall).  (a), (b) and (d) fail in ~20 s: the
# peer's EXIT CODE does not notice them -- all four links still STREAM, so phase_done is satisfied
# and the peer exits 0 -- it is the gate's own UART checks and its peer-derived checks (the
# tripwires and the tally) that catch them.  A failing run here is not reliably a slow one, so
# never read a fast red as "it cannot have got that far".
set -e
DIR=$(cd "$(dirname "$0")" && pwd)
EVKB=$(cd "$DIR/../../.." && pwd)
QEMU="$EVKB/tools/qrun"
. "$EVKB/tools/gate-lib.sh"
gate_init
[ "$(gate_board)" = rt1176 ] || {
    echo "FAIL: this gate is rt1176-only (EVKB_BOARD=$(gate_board)) -- the M.2 socket is on the MIMXRT1170-EVKB"; exit 1; }

fail() { echo "FAIL: $*"; exit 1; }

BUILD_DIR="$DIR/build-reconnect"
ELF="$BUILD_DIR/m2_hci_probe.elf"

if [ "${GATE_VACUITY:-}" = "1" ] && [ -x "$ELF" ]; then
    :
else
    mkdir -p "$BUILD_DIR"
    CONFIGURE_RC=0
    cmake -S "$DIR" -B "$BUILD_DIR" -DCMAKE_TOOLCHAIN_FILE="$EVKB/toolchain/rt1170-evkb.toolchain.cmake" \
          -DM2_BT_CONNECT=ON -DM2_BT_TARGET_NAME=FAKE-HEADSET-01 -DM2_BT_RECONNECT=ON >"$BUILD_DIR/configure.log" 2>&1 || CONFIGURE_RC=$?
    BUILD_RC=0
    if [ "$CONFIGURE_RC" -eq 0 ]; then
        cmake --build "$BUILD_DIR" >"$BUILD_DIR/build.log" 2>&1 || BUILD_RC=$?
    fi
    if [ "$CONFIGURE_RC" -ne 0 ] || [ "$BUILD_RC" -ne 0 ]; then
        fail "build-reconnect/ did not build -- see build-reconnect/configure.log / build.log"
    fi
fi

OUT="$BUILD_DIR/reconnect.uart"; DBG="$BUILD_DIR/reconnect.dbg"; RES="$BUILD_DIR/reconnect.peer"
rm -f "$OUT" "$DBG" "$RES"
SOCK="/tmp/m2recon_$$.sock"; rm -f "$SOCK"; gate_tmp "$SOCK"
"$QEMU" $(gate_qemu_machine) -kernel "$ELF" -display none $(gate_console "$OUT") \
    -serial unix:"$SOCK",server -d guest_errors -D "$DBG" &
P=$!; gate_pid $P
PEER_RC=0
python3 "$DIR/hci_peer.py" reconnect "$SOCK" > "$RES" 2>&1 || PEER_RC=$?
# The peer runs in the FOREGROUND and returns only when the phase is done or it gave up, so
# a failed peer means a failed run: poll briefly for whatever the firmware still prints, not
# the full 50 s.  Wait for the LAST line the probe prints, or its named failure -- never an
# earlier one (the m2_rx_demo[irq] mid-line reap is the standing lesson).
LOOP=200; [ "$PEER_RC" -eq 0 ] || LOOP=20
for _ in $(seq 1 $LOOP); do
    [ -f "$OUT" ] && grep -q "^reconnect=done\|^reconnect=fail" "$OUT" 2>/dev/null && break
    sleep 0.25
done
gate_reap $P
gate_require_capture "$OUT" "reconnect phase"
echo "==== captured UART ===="; cat "$OUT"
echo "==== peer ===="; cat "$RES"

grep -q "RT1176 M.2 HCI probe up" "$OUT" || fail "[reconnect] banner missing"

# ★ Tripwires first (the [avdtp] convention): each names a specific violation the PEER itself
# detected.  Every positive check below is downstream of STREAMING, so without these a failure
# at any stage collapses onto the same generic message.
# Anchored with ^ so a token quoted INSIDE another line (a PEER-EXCEPTION repr, PEER-DONE's
# opcode list) cannot trip a wire; the full tokens keep STARTED / START-TWICE /
# START-BEFORE-OPEN apart.
while IFS='|' read -r pat msg; do
    [ -n "$pat" ] || continue
    if grep -q "$pat" "$RES"; then fail "[reconnect] $msg: $(grep -m1 "$pat" "$RES")"; fi
done <<'TRIPWIRES'
^PEER-DECOY-PAGED|a bond whose name does not match the target was paged -- the name filter is gone
^PEER-KEY-MISMATCH|the host offered a key the peer never notified
^PEER-KEY-STALE-REPLAY|the rejected key was offered again -- the bond was not erased
^PEER-ACL-BAD-HANDLE|an ACL packet was sent on a handle that is not this link's -- a cached or stale handle
^PEER-DISCONNECT-BAD-HANDLE|a Disconnect named a handle that is not this link's
^PEER-AUTH-NO-LINK|an authentication was requested with no link up
^PEER-ACL-UNKNOWN-CID|data was sent on a CID the peer does not hold -- a stale channel from a torn-down link
^PEER-AVDTP-START-TWICE|START was sent on a stream that was already started
^PEER-AVDTP-START-BEFORE-OPEN|START was sent before OPEN was acknowledged
^PEER-L2CAP-CFGRSP-BAD-SCID|our Config Response names the wrong CID
^PEER-L2CAP-CFGREQ-NO-MTU|our L2CAP Config Request carries no MTU option
^PEER-L2CAP-CFG-UNKNOWN-DCID|a Config Request named a channel the peer does not hold
^PEER-L2CAP-CONNRSP-UNKNOWN-SCID|a Connection Response named a channel the peer did not open
^PEER-REV-CONN-REFUSED|the peer's reverse SDP channel was refused
^PEER-SDP-QUERY-UNANSWERED|the peer's SDP query of our AudioSource record was never answered
^PEER-SDP-SOURCE-RECORD-BAD|our AudioSource SDP record does not match the reference
^PEER-AVDTP-DISCOVER-HELD|DISCOVER was never answered because the reverse SDP did not complete
^PEER-AVDTP-SETCONFIG-WRONG-SEID|SET_CONFIGURATION targeted the MPEG SEP
^PEER-AVDTP-DELAYREPORT-UNANSWERED|the peer's DelayReport command was never accepted
^PEER-AVDTP-DELAYREPORT-REJECTED|the peer's DelayReport command was rejected
^PEER-AVDTP-UNEXPECTED-RSP|an AVDTP response arrived for a command the peer never sent
^PEER-UNKNOWN-OPCODE|the host sent a command the peer does not model
^PEER-BAD-TYPE|the host sent an H4 packet type the peer does not model
^PEER-SETBAUD-BAD-LEN|a vendor set-baud command had the wrong length
^PEER-EXCEPTION|the peer hit a malformed frame
TRIPWIRES

# The probe's own lines, in boot order.  (If phase 1 fails and the peer file shows no
# PEER-CONNECTED line, the controller never attached -- see the peer file above.)
grep -q "^bonds_boot=0[[:space:]]*$" "$OUT"                                       || fail "[reconnect] no bonds_boot line -- the reconnect probe never ran (or the store was not empty at boot)"
grep -q "^reconnect_phase=1 result=ok paired_by=ssp[[:space:]]*$" "$OUT"          || fail "[reconnect] phase 1 (fresh pairing) did not reach STREAMING by SSP -- if the peer file shows PEER-EOF, QEMU died under it (infrastructure); if it shows no PEER-CONNECTED at all, the controller never attached"
grep -q '^bonds_reload=2 front="DECOY"[[:space:]]*$' "$OUT"                       || fail "[reconnect] bond did not survive the cold reload (expected 2 = the paired device + the decoy, decoy in front): $(grep '^bonds_reload=' "$OUT" || echo none)"
grep -q "^reconnect_phase=2 result=ok paired_by=stored[[:space:]]*$" "$OUT"       || fail "[reconnect] phase 2 did not authenticate with the stored key"
grep -q "^bond_rejected: status=0x06 -> erased" "$OUT"                            || fail "[reconnect] the rejected bond was not erased by name"
grep -q "^reconnect_phase=3 result=ok paired_by=ssp[[:space:]]*$" "$OUT"          || fail "[reconnect] phase 3 (rejection -> fresh pairing) did not reach STREAMING by SSP"
grep -q '^bonds_reload2=2 front="FAKE-HEADSET-01"[[:space:]]*$' "$OUT"            || fail "[reconnect] key #2's bond did not survive the third cold reload in front: $(grep '^bonds_reload2=' "$OUT" || echo none)"
grep -q "^reconnect_phase=4 result=ok paired_by=stored[[:space:]]*$" "$OUT"       || fail "[reconnect] phase 4 did not authenticate with key #2 from the store"
grep -q '^bonds_final=2 front="FAKE-HEADSET-01" key_changed=1[[:space:]]*$' "$OUT" || fail "[reconnect] the re-pairing did not replace the key (or the re-created bond is not in front): $(grep '^bonds_final=' "$OUT" || echo none)"
grep -q "^reconnect=done" "$OUT"                                                  || fail "[reconnect] probe did not reach reconnect=done"
NINQ=$(grep -c "^inquiry=started" "$OUT" || true)
[ "$NINQ" -eq 1 ] || fail "[reconnect] expected exactly ONE inquiry (phase 1); saw $NINQ -- a bonded page was replaced by an inquiry"
NTRY=$(grep -c '^bond_try: bd=AA:BB:CC:DD:EE:01 name="FAKE-HEADSET-01" attempts=3' "$OUT" || true)
[ "$NTRY" -eq 3 ] || fail "[reconnect] expected the bonded device paged FIRST (attempts=3) in phases 2, 3 and 4 -- the decoy was not filtered, or the walk changed; saw $NTRY"
if grep -q "^bond_page=none" "$OUT"; then fail "[reconnect] a bonded page fell through to inquiry"; fi
NKR=$(grep -c "reply(stored type=4)" "$OUT" || true)
[ "$NKR" -eq 3 ] || fail "[reconnect] expected exactly three stored-key replies (phases 2, 3 and 4 -- phase 3's is the rejected one); saw $NKR"
NNEG=$(grep -c "neg_reply (no stored key)" "$OUT" || true)
[ "$NNEG" -eq 2 ] || fail "[reconnect] expected exactly two negative replies (phase 1, and phase 3 after the erase); saw $NNEG"

# ★ Infrastructure verdicts: placed AFTER the UART checks on purpose -- the peer waits out its
# deadline on any firmware failure, so putting these first would mask every named failure above.
# A run whose UART lines are all right but whose peer died here is infrastructure, not firmware.
if grep -q "^PEER-EOF" "$RES";      then fail "[reconnect] QEMU closed the socket under the peer -- infrastructure, not firmware: $(grep -m1 '^PEER-EOF' "$RES")"; fi
if grep -q "^PEER-DEADLINE" "$RES"; then fail "[reconnect] the peer gave up at its deadline (sweep load? raise DEADLINE with QRUN_TIMEOUT): $(grep -m1 '^PEER-DEADLINE' "$RES")"; fi
if grep -q "^Traceback (most recent call last)" "$RES"; then fail "[reconnect] the fake controller crashed -- peer bug, not firmware: $(grep -A3 '^Traceback' "$RES" | tail -1)"; fi

# The peer's tally, last: every number here was counted on the other side of the socket.
grep -q "^PEER-CONNECTED phase=reconnect" "$RES" || fail "[reconnect] the fake controller never attached to LPUART2"
[ "$PEER_RC" -eq 0 ] || fail "[reconnect] peer exited $PEER_RC"
grep -q "^PEER-RECONNECT inquiries=1 create_conns=4 key_replies=3 key_ok=2 key_rejected=1 neg_replies=2 iocap_dances=2 notified=2 started_links=4[[:space:]]*$" "$RES" \
    || fail "[reconnect] peer tally mismatch: $(grep '^PEER-RECONNECT' "$RES" || echo none)"
grep -q "^PEER-KEY-REJECTED link=3" "$RES" || fail "[reconnect] the modelled rejection did not happen on link 3"

echo "PASS: a bonded device is paged with no inquiry and authenticates with the stored key after an EEPROM cold reload past a decoy; a rejected bond is erased and re-paired with a new key; four links on four fresh handles"
