#!/bin/sh
# run_qemu_lifecycle.sh -- the [lifecycle] gate for bt_tone_test (NEW-34 piece 2):
# A2DP streaming that SURVIVES the link -- a drop is detected, the media path is torn
# down, and the link is reconnected in EITHER direction (the headset pages us; we page
# the headset) without a reboot, adopting the peer's config when the peer drives AVDTP.
#
# WHAT THIS PROVES
#   Built with -DM2_BT_TARGET_NAME=FAKE-HEADSET-01 -DM2_BT_RETRY_MS=3000, bt_tone_test's
#   BtSession runs against hci_peer.py's `lifecycle` phase across THREE legs on one link
#   socket:
#     LEG 1  empty bond store -> inquiry -> SSP pair (key #1) -> A2DP STREAMING BY INQUIRY
#            at bitpool 53; then the peer INJECTS Disconnection_Complete reason 0x08.  The
#            session must tear the media node down (btout.end()) and go WAITING.
#     LEG 2  one second later the PEER pages us (Connection_Request from the bonded
#            address).  BtLink Accepts as SLAVE (role 0x01, a FRESH handle), authenticates
#            with the STORED key from leg 1 (no re-pair), and BtSession starts an INBOUND
#            attempt -- so the PEER drives AVDTP AS INITIATOR at bitpool 35.  A2dpSource
#            ADOPTS that config, so AudioOutputBluetooth streams 83-byte SBC frames
#            (bitpool 35 = 4 + 8 + ceil((8 + 16*35)/8)); the peer validates each frame at
#            83 bytes -- a length the host's own initiator (always bitpool 53 = 119) can
#            NOT produce, so it is un-fakeable proof of adoption.  Then the peer drops 0x13.
#     LEG 3  half a second later a Connection_Request from an UNKNOWN address must be
#            REJECTED 0x0F (never pair a stranger from an incoming page); then the host's
#            own 3 s (M2_BT_RETRY_MS) retry re-pages the lost peer and streams a THIRD time
#            BY PAGED, at bitpool 53.
#   Every value the peer asserts is one the firmware cannot invent: the Accept role byte,
#   the adopted 83-byte media frame length, page-scan on/off counted from Write_Scan_Enable,
#   and -- the load-bearing negative -- ACL on a handle the link no longer owns.
#
# ★ OWNS ITS OWN BUILD DIRECTORY, build-lifecycle/, configured -DM2_BT_RETRY_MS=3000 so
# leg 2's incoming page (peer +1 s) WINS the race against the lost-peer retry, and leg 3's
# re-page fires 3 s after the 0x13 drop.  build/ and build-media/ are never touched here.
#
# WHAT THIS DOES NOT PROVE
#   Audible reconnection on a real sink, or a real headset's willingness to page a source
#   and accept a stored key out of pairing mode -- that is the silicon claim
#   (transcript_hw_evkb.txt).  QEMU has no baud/ACL-scheduling pacing, so like [media] this
#   gate asserts framing + continuity, not a zero-drop link.
#
# TIMING
#   Three legs with a 3 s gap before leg 3's re-page; the peer breaks as soon as all three
#   have streamed (phase_done), and this script then waits out the host's final heartbeat.
#   hci_peer.py gives up at 55 s (DEADLINE), BELOW tools/qrun's 60 s QRUN_TIMEOUT, so on a
#   hung run the peer announces (PEER-DEADLINE) before QEMU is killed; if QEMU dies first
#   the peer prints PEER-EOF.  Do NOT raise QRUN_TIMEOUT without raising the peer's deadline.
#
# DEMONSTRATED RED (2026-09-06): each mutation made in the named COMMITTED source file,
# build-lifecycle/ rebuilt, this gate run against it, then reverted
# (`git -C <repo> checkout -- <file>`) and rebuilt from clean source -- confirmed GREEN
# again after each revert, both repos clean between demos.  FIVE of the six fail BY THE
# NAMED ASSERTION; the sixth is a documented GATE GAP (recorded here honestly, not papered
# over -- the app-level omission it makes is invisible because the library covers it):
#   (1) GATE GAP -- evkb bt_tone_test.cpp onStreamCb() else: `btout.end();` commented out.
#       The gate PASSES BIT-IDENTICALLY (peer tally unchanged, no tripwire).  A2dpSource::tick()
#       (M2Radio bt/A2dpSource.cpp) already does `m_avdtp.reset(); m_l2.reset();` on link loss
#       BEFORE onStreamCb fires, and leg 2's streaming callback re-begins btout on a fresh
#       channel, so the app-level end() is redundant for the ACL-observable behaviour and its
#       removal changes nothing the peer sees.  The PEER-ACL-BAD-HANDLE tripwire IS LIVE: a
#       stacked diagnostic that ALSO removes A2dpSource's `m_avdtp.reset(); m_l2.reset();` makes
#       the host write ACL on the dead handle -- `PEER-ACL-BAD-HANDLE 0x0001 (current 0x0002)`
#       -- so the gate catches the LIBRARY-level teardown failure, just not the app-level one
#       that nothing observable here depends on.
#   (2) M2Radio bt/A2dpSource.cpp start(): the outbound `if (!m_inbound) m_params = {...,53};`
#       reset removed (leg-2 adopted config leaks into leg 3) --
#         FAIL: [lifecycle] leg 3 re-page did not stream at the initiator default bitpool 53 (adopted config leaked from leg 2?)
#       (UART: `streaming by=paged bitpool=35 ...` -- leg 2's adopted 35 leaked into leg 3.)
#   (3) M2Radio bt/BtLink.cpp Connection_Request accept branch: Accept role byte `r[6] = 0x01`
#       changed to `0x00` --
#         FAIL: [lifecycle] Accept_Connection_Request used the wrong role (must remain slave 0x01): PEER-ACCEPT-BAD-ROLE 0x00
#   (4) M2Radio bt/BtSession.cpp tick(): the `inboundUp()` branch disabled, so an incoming link
#       neither cancels the retry nor starts an INBOUND attempt --
#         FAIL: [lifecycle] leg 2 did not stream by incoming at the ADOPTED bitpool 35
#       (leg 2 never streams; peer tally collapses to
#        `PEER-LIFECYCLE links=1 drops=1 accepts=1 rejects=0 scan_on=2 scan_off=2 bitpool2=0`.)
#   (5) M2Radio bt/BtLink.cpp Connection_Request: `bool bonded = m_bonds && m_bonds->find(p);`
#       forced to `true` (accept any address) --
#         FAIL: [lifecycle] unknown-address page was not rejected 0x0F
#       (the stranger DE:05:04:03:02:01 is `-> accept(slave)`; peer tally `accepts=2 rejects=0`.)
#   (6) M2Radio bt/BtLink.cpp reconcileScan(): `uint8_t s = m_wantScan ? 0x02 : 0x00;` forced to
#       `0x00` (Write_Scan_Enable 0x02 never sent) --
#         FAIL: [lifecycle] peer tally wrong: PEER-LIFECYCLE links=3 drops=2 accepts=1 rejects=1 scan_on=0 scan_off=4 bitpool2=35
#       (the UART still logs `page_scan=on`, which is exactly why the peer-COUNTED number, not
#        the firmware's own print, is the load-bearing check.)
#   Confirmed green again after each revert; `git -C M2Radio status` clean after all six.
set -e
DIR=$(cd "$(dirname "$0")" && pwd)
EVKB=$(cd "$DIR/../../.." && pwd)
QEMU="$EVKB/tools/qrun"
. "$EVKB/tools/gate-lib.sh"
gate_init
[ "$(gate_board)" = rt1176 ] || {
    echo "FAIL: this gate is rt1176-only (EVKB_BOARD=$(gate_board)) -- the M.2 socket is on the MIMXRT1170-EVKB"; exit 1; }

fail() { echo "FAIL: $*"; exit 1; }

# This gate owns its build: M2_BT_TARGET_NAME + M2_BT_RETRY_MS must be set here and match no
# other bt_tone_test build. Under the vacuity harness (GATE_VACUITY=1, no ARM toolchain
# guaranteed) skip the (re)build once the ELF already exists -- same convention as
# run_qemu_media.sh / run_qemu_avdtp.sh.
BUILD_DIR="$DIR/build-lifecycle"
ELF="$BUILD_DIR/bt_tone_test.elf"

if [ "${GATE_VACUITY:-}" = "1" ] && [ -x "$ELF" ]; then
    :
else
    mkdir -p "$BUILD_DIR"
    CONFIGURE_RC=0
    cmake -S "$DIR" -B "$BUILD_DIR" -DCMAKE_TOOLCHAIN_FILE="$EVKB/toolchain/rt1170-evkb.toolchain.cmake" \
          -DM2_BT_TARGET_NAME=FAKE-HEADSET-01 -DM2_BT_RETRY_MS=3000 >"$BUILD_DIR/configure.log" 2>&1 || CONFIGURE_RC=$?
    BUILD_RC=0
    if [ "$CONFIGURE_RC" -eq 0 ]; then
        cmake --build "$BUILD_DIR" >"$BUILD_DIR/build.log" 2>&1 || BUILD_RC=$?
    fi
    if [ "$CONFIGURE_RC" -ne 0 ] || [ "$BUILD_RC" -ne 0 ]; then
        fail "build-lifecycle/ did not build -- see build-lifecycle/configure.log / build.log"
    fi
fi

OUT="$BUILD_DIR/lifecycle.uart"; DBG="$BUILD_DIR/lifecycle.dbg"; RES="$BUILD_DIR/lifecycle.peer"
rm -f "$OUT" "$DBG" "$RES"
SOCK="/tmp/m2lifec_$$.sock"; rm -f "$SOCK"; gate_tmp "$SOCK"
"$QEMU" $(gate_qemu_machine) -kernel "$ELF" -display none $(gate_console "$OUT") \
    -serial unix:"$SOCK",server -d guest_errors -D "$DBG" &
P=$!; gate_pid $P
PEER_RC=0
python3 "$EVKB/examples/networking/m2_hci_probe/hci_peer.py" lifecycle "$SOCK" > "$RES" 2>&1 || PEER_RC=$?
# The peer runs in the FOREGROUND and returns when all three legs have streamed (or it gave
# up).  Then wait for the LAST line the gate parses -- the host's heartbeat once links=3 --
# never an earlier one (the m2_rx_demo[irq] mid-line reap is the standing lesson).  A failed
# peer means a failed run: poll briefly, not the full deadline.
LOOP=240; [ "$PEER_RC" -eq 0 ] || LOOP=40
for _ in $(seq 1 $LOOP); do
    [ -f "$OUT" ] && grep -qE "^bt_link links=3 " "$OUT" 2>/dev/null \
        && [ -f "$RES" ] && grep -q "^PEER-LIFECYCLE " "$RES" 2>/dev/null && break
    sleep 0.25
done
gate_reap $P
gate_require_capture "$OUT" "lifecycle phase"
echo "==== captured UART ===="; cat "$OUT"
echo "==== peer ===="; cat "$RES"

grep -q "RT1176 BT tone test up" "$OUT"            || fail "[lifecycle] banner missing"

# --- UART positives (the firmware's own view), in leg order ---
grep -q "^bonds_boot=0" "$OUT"                              || fail "[lifecycle] store not empty at boot"
grep -q "^streaming by=inquiry " "$OUT"                     || fail "[lifecycle] leg 1 never streamed by inquiry"
grep -q "^bt_dropped reason=0x08 " "$OUT"                   || fail "[lifecycle] leg 1 drop (0x08) not seen"
grep -q "^streaming by=incoming bitpool=35 " "$OUT"         || fail "[lifecycle] leg 2 did not stream by incoming at the ADOPTED bitpool 35"
grep -q "^bt_dropped reason=0x13 " "$OUT"                   || fail "[lifecycle] leg 2 drop (0x13) not seen"
grep -q "conn_req: .* -> reject(0x0F unknown)" "$OUT"       || fail "[lifecycle] unknown-address page was not rejected 0x0F"
# Leg 3 is an OUTBOUND (initiator) attempt, so it must stream at the initiator DEFAULT bitpool 53 -- NOT
# the bitpool 35 it adopted as the acceptor in leg 2.  Asserting bitpool=53 (not just "by=paged") is what
# makes this gate catch the A2dpSource config-leak bug (adopted config must not persist into a later
# outbound attempt -- see the plan's Task 5 note "otherwise m_params stays the initiator default").
grep -q "^streaming by=paged bitpool=53 " "$OUT"            || fail "[lifecycle] leg 3 re-page did not stream at the initiator default bitpool 53 (adopted config leaked from leg 2?)"
grep -qE "^bt_link links=3 lost=2 " "$OUT"                  || fail "[lifecycle] final health line wrong (expected links=3 lost=2)"
grep -qE "^bt_link .* reconnect_ms=[1-9]" "$OUT"            || fail "[lifecycle] reconnect_ms never populated"

# --- peer tripwires (values the firmware cannot invent).  ACL/dead-handle first: every
# positive above is downstream of STREAMING, so without these a stale-media failure would
# collapse onto a generic message.  Anchored with ^ so a token quoted inside PEER-DONE cannot trip. ---
if grep -q "^PEER-ACL-BAD-HANDLE" "$RES";   then fail "[lifecycle] the host wrote ACL on a dead handle (media not torn down on drop?): $(grep -m1 PEER-ACL-BAD-HANDLE "$RES")"; fi
if grep -q "^PEER-ACL-UNKNOWN-CID" "$RES";  then fail "[lifecycle] the host wrote ACL on a stale channel from a torn-down link: $(grep -m1 PEER-ACL-UNKNOWN-CID "$RES")"; fi
if grep -q "^PEER-ACCEPT-BAD-ROLE" "$RES";  then fail "[lifecycle] Accept_Connection_Request used the wrong role (must remain slave 0x01): $(grep -m1 PEER-ACCEPT-BAD-ROLE "$RES")"; fi
if grep -q "^PEER-LC-KEY-MISMATCH" "$RES";  then fail "[lifecycle] the stored key the host offered on the re-page did not match leg 1's: $(grep -m1 PEER-LC-KEY-MISMATCH "$RES")"; fi
if grep -q "^PEER-LC-AVDTP-REJECT" "$RES";  then fail "[lifecycle] the host's acceptor rejected the peer's AVDTP command: $(grep -m1 PEER-LC-AVDTP-REJECT "$RES")"; fi
if grep -q "^PEER-LC-AVDTP-UNEXPECTED" "$RES"; then fail "[lifecycle] the host's AVDTP acceptor answered out of sequence: $(grep -m1 PEER-LC-AVDTP-UNEXPECTED "$RES")"; fi
if grep -q "^PEER-EXCEPTION" "$RES";        then fail "[lifecycle] the peer hit a malformed frame: $(grep -m1 PEER-EXCEPTION "$RES")"; fi

# --- the peer tally, last of the peer-derived checks: every number counted on the other
# side of the socket (see run_qemu_media.sh -- the NUMBER is what proves it, not a print). ---
grep -q "^PEER-CONNECTED phase=lifecycle" "$RES" || fail "[lifecycle] the fake controller never attached to LPUART2"
# scan_on=2: page-scan enabled after each of the two drops (never at boot -- no bond yet).  scan_off=2:
# disabled at each link-up (leg 2 incoming, leg 3 re-page).  Pinned to the first green run; a mutant moves it.
grep -q "^PEER-LIFECYCLE links=3 drops=2 accepts=1 rejects=1 scan_on=2 scan_off=2 bitpool2=35" "$RES" \
    || fail "[lifecycle] peer tally wrong: $(grep -m1 PEER-LIFECYCLE "$RES")"

# --- infrastructure LAST (the peer waits out its deadline on any firmware failure, so these
# would mask every named failure above if placed first). ---
if grep -q "^PEER-EOF" "$RES";      then fail "[lifecycle] QEMU closed the socket under the peer -- infrastructure, not firmware: $(grep -m1 '^PEER-EOF' "$RES")"; fi
if grep -q "^PEER-DEADLINE" "$RES"; then fail "[lifecycle] the peer gave up at its deadline (sweep load? raise DEADLINE with QRUN_TIMEOUT): $(grep -m1 '^PEER-DEADLINE' "$RES")"; fi
if grep -q "^Traceback (most recent call last)" "$RES"; then fail "[lifecycle] the fake controller crashed -- peer bug, not firmware: $(grep -A3 '^Traceback' "$RES" | tail -1)"; fi
[ "$PEER_RC" -eq 0 ] || fail "[lifecycle] peer exited $PEER_RC"

echo "PASS: A2DP link lifecycle -- drop, re-page in both directions, config adoption (bitpool 35 adopted, 83-byte frames), unknown-address reject; all three links streamed"
