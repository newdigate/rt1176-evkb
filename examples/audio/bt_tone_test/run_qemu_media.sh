#!/bin/sh
# run_qemu_media.sh -- the [media] gate for bt_tone_test (BT-3 phase 4, task 6):
# AudioOutputBluetooth actually RTP/SBC-frames the tone graph's audio and gets
# it onto the AVDTP media channel, against a fake acceptor that validates
# every packet.
#
# WHAT THIS PROVES
#   Built with -DM2_BT_TARGET_NAME=FAKE-HEADSET-01 (SSP -- no
#   M2_BT_LEGACY_PIN), bt_tone_test's whole chain runs: BT firmware download
#   -> HCI Reset/identity -> A2dpSource::connect() (inquiry-by-name -> SSP
#   pairing -> encryption -> L2CAP -> SDP -> AVDTP DISCOVER..START) -> the
#   SAME fake acceptor hci_peer.py's `avdtp` phase already proves (this gate
#   reuses that acceptor unchanged) -- then, once STREAMING, the tone graph
#   (AudioSynthWaveformSine -> AudioOutputBluetooth) actually runs: the audio
#   ISR encodes SBC and pushes frames into MediaPacketizer's ring, and
#   AudioOutputBluetooth::poll() drains them as RTP packets over the SECOND
#   L2CAP channel AVDTP opened for the media transport (PSM 0x0019, distinct
#   from the signalling channel). hci_peer.py's new `media` phase receives on
#   that channel and validates each packet: RTP V2/PT96, a strictly
#   incrementing sequence number, and -- per the frameCount the SBC media
#   payload header names -- each frame's own sync byte (0x9C) and the fixed
#   119-byte length the negotiated bitpool-53 config produces. None of those
#   values can be invented by a driver that isn't actually doing the framing;
#   a matching PEER-MEDIA line is a round-trip proof, the same discipline as
#   every other hci_peer.py phase.
#
# ★ OWNS ITS OWN BUILD DIRECTORY, build-media/, with M2_BT_TARGET_NAME=
# FAKE-HEADSET-01 (no M2_BT_LEGACY_PIN -- the fake peer pairs by SSP, same as
# run_qemu_avdtp.sh). build/ (the card-absent gate's directory) is never
# reconfigured or written to by this script.
#
# WHAT THIS DOES NOT PROVE
#   That the SBC bytes are AUDIBLE on a real sink, or anything about a real
#   A2DP sink's media-channel quirks -- that is the deferred silicon task
#   (ESP32 sink). QEMU has no notion of ACL scheduling latency either.
#
# ★ HISTORY -- two real bugs in bt_tone_test.cpp were found DIAGNOSING this
# gate, before AudioOutputBluetooth/bt_tone_test.cpp carried the fixes below.
# Kept because both are non-obvious and easy to reintroduce:
#   1. The graph (AudioSynthWaveformSine -> AudioOutputBluetooth) has no I2S
#      (or other hardware-interrupt-driven) AudioStream node, so nothing ever
#      called AudioStream::update_all() -- every other Audio example in this
#      tree gets that for free from an I2S/DAC/etc. DMA ISR. Measured against
#      the pre-fix build: `blocks=0` through 38 consecutive 1 Hz heartbeats.
#      FIXED: AudioOutputBluetooth now owns a static IntervalTimer, started
#      once in begin() (so the card-absent path -- begin() is only reached
#      after a2dp=ok -- never starts it, and run_qemu.sh stays vacuous), that
#      calls AudioStream::update_all() every audio block period. It is its
#      own I2S-equivalent clock, the same role that node plays elsewhere.
#   2. Even with a clock, loop() drained the packetizer only ONCE per pass,
#      and each pass was dominated by `delay(1000)` -- the 64-frame
#      (~190 ms at 344 fps) ring can't hold a second's worth of ISR output,
#      so it dropped for ~810 ms of every second. FIXED: loop() now calls
#      l2().service()/avdtp().service()/btout.poll() every pass with no
#      delay, and throttles only the heartbeat PRINT via millis().
#
# ★ EVEN AFTER BOTH FIXES, SUSTAINED DROPS ARE NOT ZERO IN QEMU -- which is why
# this gate does NOT assert drops=0. It asserts the self-clock actually ran (a
# heartbeat with blocks>0) and that every packet that arrived is valid framing,
# and leaves the drop / flow-control-at-rate measurement to SILICON (Task 8,
# the ESP32 sink). An earlier draft grepped `^hb streaming=1 .* drops=0 `, and
# it passed only VACUOUSLY: the FIRST heartbeat (n=0, printed right after
# `streaming`, before any block is produced) is trivially `blocks=0 packets=0
# drops=0`. Every heartbeat after that shows real, growing drops: measured over
# a full run, `blocks=12529 packets=1162 drops=6703` by n=34 -- roughly 53%
# loss, sustained. hci_peer.py's RTP/SBC validation is unaffected (the packets
# that DO arrive are perfectly formed: pkts=1162 frames=5803 seqgaps=0 badsbc=0
# badrtp=0), so asserting the framing is the right QEMU-scope claim; a zero-drop
# LINK is a silicon claim, not a QEMU one. Root cause of the QEMU loss: qemu2's
# LPUART model (hw/char/imxrt_lpuart.c) has no baud-rate-based pacing, so it
# is not simply "115200 baud is too slow for SBC" -- more likely L2cap's
# 8-slot TXQ / ACL-credit round trip (service() drains while credits>0;
# credit is only returned when hci_peer.py's Number_Of_Completed_Packets
# reply is read back) is throttled by real host-wall-clock scheduling
# latency between the QEMU and python processes, since this harness runs
# QEMU without -icount (guest I/O timing tracks host wall time). Left as a
# finding -- see the BT-3 phase 4 task 6 report for the full writeup.
#
# DEMONSTRATED RED (2026-09-03), twice, each mutation made in M2Radio, this
# gate run against it, confirmed to fail BY THE NAMED ASSERTION, then
# reverted (`git -C M2Radio checkout -- <file>`) and build-media/ rebuilt from
# the clean source before either the PASS above or the next demonstration:
#   (a) bt/MediaPacketizer.cpp, drain(): `m_seq++` commented out --
#         FAIL: [media] RTP/SBC framing invalid or sequence gapped
#       (peer.py: PEER-MEDIA pkts=1191 frames=5945 seqgaps=1190 badsbc=0
#       badrtp=0 -- every packet after the first repeats the same RTP
#       sequence number, so hci_peer.py's media handler counts a seqgap on
#       nearly every packet.)
#   (b) bt/Rtp.cpp, header(): `o[12] = (uint8_t)(frameCount & 0x0F);` swapped
#       for `o[12] = 0;` -- every packet claims zero SBC frames regardless of
#       how many were actually batched --
#         FAIL: [media] RTP/SBC framing invalid or sequence gapped
#       (peer.py: PEER-MEDIA pkts=1195 frames=0 seqgaps=0 badsbc=0 badrtp=0 --
#       pkts>0 alone does not catch this, since the RTP header and sequence
#       are otherwise fine and frameCount=0 means the peer's frame walk never
#       finds a bad sync byte either. Caught ONLY because the awk check below
#       also requires frames=[1-9] on the final PEER-MEDIA line.)
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

# This gate owns its build: M2_BT_TARGET_NAME must be set here and matches no
# other bt_tone_test build. Under the vacuity harness (GATE_VACUITY=1, no ARM
# toolchain guaranteed) skip the (re)build once the ELF already exists -- same
# convention as run_qemu_avdtp.sh / run_qemu_baud.sh.
BUILD_DIR="$DIR/build-media"
ELF="$BUILD_DIR/bt_tone_test.elf"

if [ "${GATE_VACUITY:-}" = "1" ] && [ -x "$ELF" ]; then
    :
else
    mkdir -p "$BUILD_DIR"
    CONFIGURE_RC=0
    cmake -S "$DIR" -B "$BUILD_DIR" -DCMAKE_TOOLCHAIN_FILE="$EVKB/toolchain/rt1170-evkb.toolchain.cmake" \
          -DM2_BT_TARGET_NAME=FAKE-HEADSET-01 >"$BUILD_DIR/configure.log" 2>&1 || CONFIGURE_RC=$?
    BUILD_RC=0
    if [ "$CONFIGURE_RC" -eq 0 ]; then
        cmake --build "$BUILD_DIR" >"$BUILD_DIR/build.log" 2>&1 || BUILD_RC=$?
    fi
    if [ "$CONFIGURE_RC" -ne 0 ] || [ "$BUILD_RC" -ne 0 ]; then
        fail "build-media/ did not build -- see build-media/configure.log / build.log"
    fi
fi

OUT="$BUILD_DIR/media.uart"; DBG="$BUILD_DIR/media.dbg"; RES="$BUILD_DIR/media.peer"
rm -f "$OUT" "$DBG" "$RES"
SOCK="/tmp/m2media_$$.sock"; rm -f "$SOCK"; gate_tmp "$SOCK"
"$QEMU" $(gate_qemu_machine) -kernel "$ELF" -display none $(gate_console "$OUT") \
    -serial unix:"$SOCK",server -d guest_errors -D "$DBG" &
P=$!; gate_pid $P
PEER_RC=0
python3 "$EVKB/examples/networking/m2_hci_probe/hci_peer.py" media "$SOCK" > "$RES" 2>&1 || PEER_RC=$?
# Wait for the LAST lines each side produces -- never the first interesting
# one (the m2_rx_demo[irq] race is the standing lesson for why). The peer
# process above already blocked until ITS terminal state, so $RES is settled
# by the time we get here; still confirm both files carry their last line
# before reading them, in case QEMU's own capture lags the peer's exit.
for _ in $(seq 1 240); do
    [ -f "$OUT" ] && grep -q "^hb streaming=1 " "$OUT" 2>/dev/null \
        && [ -f "$RES" ] && grep -q "^PEER-MEDIA " "$RES" 2>/dev/null && break
    sleep 0.25
done
gate_reap $P
gate_require_capture "$OUT" "media phase"
echo "==== captured UART ===="; cat "$OUT"
echo "==== peer ===="; cat "$RES"

grep -q "RT1176 BT tone test up" "$OUT" || fail "[media] banner missing"

grep -q '^a2dp=ok' "$OUT"                          || fail "[media] bring-up did not reach AVDTP START"
# The fake acceptor models the Shokz since 2026-09-04 (see run_qemu_avdtp.sh's header): it SDP-queries our
# AudioSource record before answering DISCOVER, lists its MPEG SEP first, and sends a DelayReport after OPEN.
# A2dpSource must get through all of it -- named here so a regression fails by cause, not as "no media".
if grep -q "PEER-L2CAP-CFGREQ-NO-MTU" "$RES";           then fail "[media] our L2CAP Config Request carries no MTU option"; fi
if grep -q "PEER-SDP-QUERY-UNANSWERED" "$RES";          then fail "[media] the peer's SDP query of our AudioSource record was never answered"; fi
if grep -q "PEER-SDP-SOURCE-RECORD-BAD" "$RES";         then fail "[media] our AudioSource SDP record does not match the Mac's reply"; fi
if grep -q "PEER-AVDTP-SETCONFIG-WRONG-SEID" "$RES";    then fail "[media] SET_CONFIGURATION targeted the MPEG SEP"; fi
if grep -q "PEER-AVDTP-DELAYREPORT-UNANSWERED" "$RES";  then fail "[media] the peer's DelayReport command was never accepted"; fi
grep -q "^PEER-SDP-SOURCE-RECORD ok" "$RES"        || fail "[media] the peer never read our AudioSource record"
grep -q "^PEER-AVDTP-DELAYREPORT-ACCEPTED" "$RES"  || fail "[media] the peer's DelayReport was not accepted"
grep -q "^PEER-SET-CONFIG cie=21150235 acp_seid=1 delay_reporting=1" "$RES" || fail "[media] SET_CONFIGURATION must pick SEID 1 with delay reporting"
grep -q '^streaming' "$OUT"                        || fail "[media] node never began streaming"
grep -qE '^hb streaming=1 blocks=[1-9]' "$OUT"      || fail "[media] reached STREAMING but the audio clock produced no blocks (self-clock not running)"
grep -q '^PEER-MEDIA ' "$RES"                      || fail "[media] peer received no media"
awk '/^PEER-MEDIA/{p=$0} END{exit !(p ~ /pkts=[1-9]/ && p ~ /frames=[1-9]/ && p ~ /seqgaps=0/ && p ~ /badsbc=0/ && p ~ /badrtp=0/)}' "$RES" \
                                                   || fail "[media] RTP/SBC framing invalid or sequence gapped"

echo "PASS: AudioOutputBluetooth self-clocks the tone graph and RTP-frames valid SBC onto the media channel (blocks produced, sequence continuous, framing valid); drops / flow-control-at-rate is a SILICON question -- QEMU has no baud pacing"
