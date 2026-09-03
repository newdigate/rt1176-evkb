#!/bin/sh
# run_qemu_avdtp.sh -- the [avdtp] gate for m2_hci_probe (BT-3 Task 10): the
# A2DP INITIATOR reaches STREAMING against a fake acceptor, in the right
# order, honouring the L2CAP receiver-side SCID rule.
#
# WHAT THIS PROVES
#   Built with -DM2_BT_CONNECT=ON -DM2_BT_TARGET_NAME=FAKE-HEADSET-01,
#   probeConnect() runs inquiry-by-name -> Create_Connection -> SSP pairing
#   -> encryption -> L2CAP -> SDP (AVDTP version) -> AVDTP
#   DISCOVER/GET_CAPABILITIES/SET_CONFIGURATION/OPEN/START, all over the same
#   fake-controller socket run_qemu_hci.sh uses (hci_peer.py, this time its
#   `avdtp` phase). That phase plays the whole peer: SSP Just Works, an L2CAP
#   acceptor (recording the receiver-side Config Response SCID, which must
#   name the PEER's channel, not ours), an SDP responder (AVDTP 1.3) and an
#   AVDTP acceptor that only accepts START after OPEN. Every value asserted
#   below is one the firmware cannot invent (the SBC calibration config
#   0x21150235 comes from Avdtp::sbcCie() encoding {44100, JOINT_STEREO, 16,
#   8, LOUDNESS, 2, 53}; the peer's own GET_CAPABILITIES reply -- bitpool
#   2..53 -- is what proves it was READ off the wire, not just satisfied
#   locally), so a matching line is a round-trip proof.
#
# ★ OWNS ITS OWN BUILD DIRECTORY, build-avdtp/, with M2_BT_CONNECT=ON and
# M2_BT_TARGET_NAME=FAKE-HEADSET-01 (no firmware blobs -- the gate build
# synthesises its BT image, same as every other m2_hci_probe gate). build/
# (the bench directory) is never reconfigured or written to by this script,
# same convention as build-baud/ in run_qemu_baud.sh.
#
# WHAT THIS DOES NOT PROVE
#   Anything about the IW416, or about a real headset's AVDTP quirks --
#   those live in transcript_hw_evkb.txt when a bench run exists. QEMU's
#   chardev has no notion of ACL scheduling latency either; the 0.02-0.5 s
#   delays hci_peer.py uses are its own pacing, not a timing proof.
#
# DEMONSTRATED RED (2026-09-03), twice, each mutation made in M2Radio, this
# gate run against it, confirmed to fail BY THE NAMED ASSERTION, then
# reverted (`git -C M2Radio checkout -- <file>`) and build-avdtp/ rebuilt from
# the clean source before either the PASS above or the next demonstration:
#   (a) bt/Avdtp.cpp, service()'s OPENING case: `buildOpen` swapped for
#       `buildStart` (skipping the media channel entirely and asking to START
#       a stream that was never opened) --
#         FAIL: [avdtp] START was sent before OPEN was acknowledged
#       hci_peer.py's own AVDTP acceptor caught it: sig==0x07 arrived with
#       avdtp["opened"] still false, so it logged PEER-AVDTP-START-BEFORE-OPEN
#       and replied BAD_STATE (0x31) rather than accepting.
#   (b) bt/L2cap.cpp, the Config Response builder (handleSig, code 0x05):
#       `ch.remoteCid` swapped for `ch.localCid` in the SCID field --
#         FAIL: [avdtp] our Config Response names the wrong CID
#       the peer's receiver-side check (hci_peer.py code 0x05 handling) saw a
#       SCID that was never one of ITS assigned CIDs and logged
#       PEER-L2CAP-CFGRSP-BAD-SCID -- this is the same W17-class hazard the
#       tree already names for RX demux ("the wrong half of a bidirectional
#       exchange silently swapped"), here on the L2CAP config path instead of
#       the SDIO RX path.
#   Confirmed green again after each revert; `git -C M2Radio status` clean.
set -e
DIR=$(cd "$(dirname "$0")" && pwd)
EVKB=$(cd "$DIR/../../.." && pwd)
QEMU="$EVKB/tools/qrun"
. "$EVKB/tools/gate-lib.sh"
gate_init
[ "$(gate_board)" = rt1176 ] || {
    echo "FAIL: this gate is rt1176-only (EVKB_BOARD=$(gate_board)) -- the M.2 socket is on the MIMXRT1170-EVKB"; exit 1; }

fail() { echo "FAIL: $*"; exit 1; }

# This gate owns its build: the CONNECT knobs must be ON here and OFF
# everywhere else. Under the vacuity harness (GATE_VACUITY=1, no ARM
# toolchain guaranteed) skip the (re)build once the ELF already exists -- it
# was built once by hand and every fixture run after that reuses it, same
# convention run_qemu_baud.sh already uses.
BUILD_DIR="$DIR/build-avdtp"
ELF="$BUILD_DIR/m2_hci_probe.elf"

if [ "${GATE_VACUITY:-}" = "1" ] && [ -x "$ELF" ]; then
    :
else
    mkdir -p "$BUILD_DIR"
    CONFIGURE_RC=0
    cmake -S "$DIR" -B "$BUILD_DIR" -DCMAKE_TOOLCHAIN_FILE="$EVKB/toolchain/rt1170-evkb.toolchain.cmake" \
          -DM2_BT_CONNECT=ON -DM2_BT_TARGET_NAME=FAKE-HEADSET-01 >"$BUILD_DIR/configure.log" 2>&1 || CONFIGURE_RC=$?
    BUILD_RC=0
    if [ "$CONFIGURE_RC" -eq 0 ]; then
        cmake --build "$BUILD_DIR" >"$BUILD_DIR/build.log" 2>&1 || BUILD_RC=$?
    fi
    if [ "$CONFIGURE_RC" -ne 0 ] || [ "$BUILD_RC" -ne 0 ]; then
        fail "build-avdtp/ did not build -- see build-avdtp/configure.log / build.log"
    fi
fi

OUT="$BUILD_DIR/avdtp.uart"; DBG="$BUILD_DIR/avdtp.dbg"; RES="$BUILD_DIR/avdtp.peer"
rm -f "$OUT" "$DBG" "$RES"
SOCK="/tmp/m2avdtp_$$.sock"; rm -f "$SOCK"; gate_tmp "$SOCK"
"$QEMU" $(gate_qemu_machine) -kernel "$ELF" -display none $(gate_console "$OUT") \
    -serial unix:"$SOCK",server -d guest_errors -D "$DBG" &
P=$!; gate_pid $P
PEER_RC=0
python3 "$DIR/hci_peer.py" avdtp "$SOCK" > "$RES" 2>&1 || PEER_RC=$?
# Wait for the LAST line this phase parses: either the successful terminal
# state or the named failure -- never the first interesting one (the
# m2_rx_demo[irq] race is the standing lesson for why).
for _ in $(seq 1 180); do
    [ -f "$OUT" ] && grep -q "^B7 DONE\|^avdtp=fail" "$OUT" 2>/dev/null && break
    sleep 0.25
done
gate_reap $P
gate_require_capture "$OUT" "avdtp phase"
echo "==== captured UART ===="; cat "$OUT"
echo "==== peer ===="; cat "$RES"

grep -q "RT1176 M.2 HCI probe up" "$OUT" || fail "[avdtp] banner missing"

# ★ Tripwires FIRST, ahead of every positive assertion below.  Both name a
# SPECIFIC protocol violation the peer itself detected; the positive checks
# further down (avdtp_caps, avdtp_start, B7 DONE, ...) are gated behind the
# probe reaching STREAMING, so ANY failure -- whichever stage caused it --
# makes every one of them fail with the SAME generic "did not reach" message.
# Checking the named tripwires first is what lets each of the two
# demonstrate-RED mutations below be identified by its own distinct assertion
# rather than both collapsing onto whichever positive check happens to sit
# first in the file. Measured, not assumed: with these checks placed AFTER
# the positive block (the first draft of this gate), mutation (a) failed as
# "[avdtp] capabilities not read off the peer" instead of naming the real
# fault -- true, but useless for diagnosis, since caps WERE read; the probe
# failed two stages later.
if grep -q "PEER-AVDTP-START-BEFORE-OPEN" "$RES"; then fail "[avdtp] START was sent before OPEN was acknowledged"; fi
if grep -q "PEER-L2CAP-CFGRSP-BAD-SCID" "$RES"; then fail "[avdtp] our Config Response names the wrong CID"; fi

grep -q '^inq_name: bd=AA:BB:CC:DD:EE:01 status=0x00 name="FAKE-HEADSET-01"' "$OUT" || fail "[avdtp] target not found by name"
grep -q "^secure=ok paired_by=ssp[[:space:]]*$" "$OUT"            || fail "[avdtp] SSP pairing + encryption did not complete"
grep -q "^sdp_avdtp_version=0x0103 (B6 DONE)" "$OUT"              || fail "[avdtp] SDP did not return the peer's AVDTP 1.3"
grep -q "^avdtp_caps: rates=0x0F modes=0x0F bitpool=2..53" "$OUT" || fail "[avdtp] capabilities not read off the peer"
grep -q "^avdtp_start=ok media_mtu=672" "$OUT"                    || fail "[avdtp] START not accepted / media MTU not negotiated"
grep -q "^B7 DONE" "$OUT"                                         || fail "[avdtp] probe did not reach B7 DONE"

[ "$PEER_RC" -eq 0 ] || fail "[avdtp] peer exited $PEER_RC"
grep -q "^PEER-SET-CONFIG cie=21150235" "$RES"                    || fail "[avdtp] our SET_CONFIGURATION bytes are not the calibration config"
grep -q "^PEER-AVDTP-STARTED" "$RES"                              || fail "[avdtp] the peer never saw an accepted START"
grep "^PEER-AVDTP" "$RES" | grep -q "order=1,2,3,6,7" || fail "[avdtp] signalling order must be DISCOVER, GET_CAPABILITIES, SET_CONFIGURATION, OPEN, START"

echo "PASS: AVDTP initiator negotiates the calibration SBC config and reaches START against the fake acceptor, in order, with the SCID rule honoured"
