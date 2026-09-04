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
#
# ★ 2026-09-04 -- THE FAKE PEER NOW MODELS THE SHOKZ OPENMOVE, from the Mac->Shokz
# Apple PacketLogger reference decoded for the headset AVDTP DISCOVER fix
# (docs/superpowers/handoffs/2026-09-03-headset-avdtp-discover-handoff.md):
#   * it opens an SDP channel BACK at the host on DISCOVER, configures it (MTU
#     48), asks the source's A2DP profile version with the Shokz's exact query,
#     requires the Mac's exact 25-byte reply, disconnects, and only THEN answers
#     DISCOVER -- a source with no SDP server hangs at DISCOVERING, as on the
#     bench (PEER-SDP-QUERY-UNANSWERED / PEER-AVDTP-DISCOVER-HELD);
#   * its DISCOVER reply lists the MPEG SEP (SEID 2) BEFORE the SBC one (SEID 1),
#     so a host that configures the first audio sink is refused
#     (PEER-AVDTP-SETCONFIG-WRONG-SEID) -- the host must read each SEP's
#     GET_ALL_CAPABILITIES (0x0C, order=1,12,12,...) and pick the SBC one;
#   * SEID 1 advertises Delay Reporting; once the host configures it the peer
#     sends its own DelayReport COMMAND after OPEN and holds the OPEN accept
#     until the host ACCEPTS it (PEER-AVDTP-DELAYREPORT-UNANSWERED);
#   * the host's L2CAP Config Request must carry an MTU option, like the Mac's
#     (PEER-L2CAP-CFGREQ-NO-MTU).
# DEMONSTRATED RED 2026-09-04, five mutations of M2Radio/bt, each run against
# this peer, each failing BY THE NAMED ASSERTION, each reverted (git checkout
# HEAD -- bt/) and the gate confirmed GREEN again afterwards:
#   (a) SdpServer never answers               -> "the peer's SDP query of our
#       AudioSource record was never answered" -- the exact bench symptom
#       (avdtp_state=1, no DISCOVER reply);
#   (b) the pre-fix L2cap.cpp (51e982b)        -> "our L2CAP Config Request
#       carries no MTU option";
#   (c) Avdtp configures the first SEP without checking its codec ->
#       "SET_CONFIGURATION targeted the MPEG SEP";
#   (d) Avdtp asks GET_CAPABILITIES (0x02)     -> "signalling order must be
#       DISCOVER, GET_ALL_CAPABILITIES ...";
#   (e) Avdtp ignores the peer's DelayReport   -> "the peer's DelayReport
#       command was never accepted".
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
# The Shokz-model tripwires (2026-09-04), each naming the stage a real headset left us stuck in:
if grep -q "PEER-L2CAP-CFGREQ-NO-MTU" "$RES";           then fail "[avdtp] our L2CAP Config Request carries no MTU option (the Mac's does)"; fi
if grep -q "PEER-SDP-QUERY-UNANSWERED" "$RES";          then fail "[avdtp] the peer's SDP query of our AudioSource record was never answered"; fi
if grep -q "PEER-SDP-SOURCE-RECORD-BAD" "$RES";         then fail "[avdtp] our AudioSource SDP record does not match the Mac's reply byte for byte"; fi
if grep -q "PEER-AVDTP-DISCOVER-HELD" "$RES";           then fail "[avdtp] DISCOVER was never answered because the reverse SDP did not complete"; fi
if grep -q "PEER-AVDTP-SETCONFIG-WRONG-SEID" "$RES";    then fail "[avdtp] SET_CONFIGURATION targeted the MPEG SEP -- the host did not walk the SEP list to the SBC one"; fi
if grep -q "PEER-AVDTP-DELAYREPORT-UNANSWERED" "$RES";  then fail "[avdtp] the peer's DelayReport command was never accepted (OPEN held back, as the Shokz does)"; fi
# The signalling ORDER next, before any positive UART check: a host that still asks GET_CAPABILITIES (0x02) is
# answered (a compliant sink must), gets no delay-reporting category back, and so fails several later checks
# too -- this one names the actual regression.
if grep -q "^PEER-AVDTP order=" "$RES" && ! grep "^PEER-AVDTP order=" "$RES" | grep -q "order=1,12,12,3,6,7"; then
    fail "[avdtp] signalling order must be DISCOVER, GET_ALL_CAPABILITIES (SEID 2, then SEID 1), SET_CONFIGURATION, OPEN, START -- got: $(grep '^PEER-AVDTP order=' "$RES")"; fi

grep -q '^inq_name: bd=AA:BB:CC:DD:EE:01 status=0x00 name="FAKE-HEADSET-01"' "$OUT" || fail "[avdtp] target not found by name"
grep -q "^secure=ok paired_by=ssp[[:space:]]*$" "$OUT"            || fail "[avdtp] SSP pairing + encryption did not complete"
grep -q "^sdp_avdtp_version=0x0103 (B6 DONE)" "$OUT"              || fail "[avdtp] SDP did not return the peer's AVDTP 1.3"
grep -q "^avdtp_caps: rates=0x0F modes=0x0F bitpool=2..53" "$OUT" || fail "[avdtp] capabilities not read off the peer"
grep -q "^avdtp_start=ok media_mtu=672" "$OUT"                    || fail "[avdtp] START not accepted / media MTU not negotiated"
grep -q "^B7 DONE" "$OUT"                                         || fail "[avdtp] probe did not reach B7 DONE"

grep -q "^sdp_served=1 delay_report=2000" "$OUT"                  || fail "[avdtp] the probe did not report serving the peer's SDP query and the 200.0 ms DelayReport"

[ "$PEER_RC" -eq 0 ] || fail "[avdtp] peer exited $PEER_RC"
grep -q "^PEER-SET-CONFIG cie=21150235 acp_seid=1 delay_reporting=1" "$RES" || fail "[avdtp] our SET_CONFIGURATION must carry the calibration config for SEID 1 WITH delay reporting (the SEP advertised it)"
grep -q "^PEER-SDP-SOURCE-RECORD ok" "$RES"                       || fail "[avdtp] the peer never read our AudioSource record"
grep -q "^PEER-AVDTP-DELAYREPORT-ACCEPTED" "$RES"                 || fail "[avdtp] the peer's DelayReport was not accepted"
grep -q "^PEER-PAGE-TIMEOUT slots=0x2000" "$RES"                  || fail "[avdtp] Write_Page_Timeout 0x2000 was not written before paging"
grep -q "^PEER-CREATE-CONN role_switch=0" "$RES"                  || fail "[avdtp] Create_Connection must not allow a role switch (the Mac's parameters to this headset)"
grep -q "^PEER-AVDTP-STARTED" "$RES"                              || fail "[avdtp] the peer never saw an accepted START"
grep "^PEER-AVDTP" "$RES" | grep -q "order=1,12,12,3,6,7" || fail "[avdtp] signalling order must be DISCOVER, GET_ALL_CAPABILITIES (SEID 2, then SEID 1), SET_CONFIGURATION, OPEN, START"

echo "PASS: AVDTP initiator answers the Shokz-shaped peer's SDP query, walks its SEP list to the SBC one with GET_ALL_CAPABILITIES, configures delay reporting, accepts its DelayReport, and reaches START in order with the SCID rule honoured"
