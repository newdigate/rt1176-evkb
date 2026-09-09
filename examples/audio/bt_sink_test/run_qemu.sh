#!/bin/sh
# run_qemu.sh -- THE gate for bt_sink_test (NEW-41): the EVKB as an A2DP SINK, against a fake
# A2DP SOURCE that pages it and streams a real tone at it.
#
# WHAT THIS PROVES
#   hci_peer.py's new `source` phase is the exact mirror of the `media` phase that gates
#   bt_tone_test: there the firmware is the source and the peer validates what it sends; here the
#   PEER is the source and the firmware must receive, decode and play.  In one run it asserts,
#   end to end:
#     * the sink makes itself DISCOVERABLE and CONNECTABLE with the identity a phone would list
#       (name EVKB-SINK, class 0x240414) -- the peer pages only once it has seen the inquiry-scan
#       bit go up, so a sink that never announced itself is never paged at all;
#     * an INCOMING page from an UNKNOWN address is ACCEPTED as slave (a source rejects strangers;
#       a sink must not), and the peer -- as SSP INITIATOR and master -- pairs it Just Works and
#       encrypts;
#     * the sink PUBLISHES an AudioSink record: the peer reads attribute 0x0004 of {0x110B} and
#       requires the ProtocolDescriptorList to carry L2CAP/PSM 0x0019 and AVDTP 1.3.  That record
#       exists only because A2dpSink calls Sdp::setRole(SINK); a source-role image answers the
#       same query with an EMPTY attribute list;
#     * the sink is an AVDTP ACCEPTOR whose one SEP is audio SNK (TSEP bit 0x08), whose
#       GET_ALL_CAPABILITIES is exactly the set its decoder can honour (44.1 kHz, all modes, 16
#       blocks / 8 subbands / LOUDNESS, bitpool 2..53, delay reporting), and which accepts
#       SET_CONFIGURATION at 44.1k joint / 16 / 8 / loudness / bitpool 53, then OPEN, then a
#       second L2CAP channel for media, then START;
#     * it sends the source ONE DelayReport once streaming (AVDTP 1.3 s8.19 makes that the SINK's
#       command, and only when the source configured delay reporting);
#     * and then the real claim: 150 RTP packets carrying 750 119-byte SBC frames of a 1 kHz
#       tone -- the committed sine.sbc, the same file M2Radio's own decoder tests use -- reach
#       AudioInputBluetooth, are decoded with NO refused frame and NO sequence gap, and land in
#       the PCM ring with the RIGHT AUDIO IN THEM: mean |L| per block in a band around the
#       encoded amplitude, and a CRC32 golden over the first 200 decoded blocks.  A decoder that
#       returns silence, or the wrong scale, or the wrong samples, cannot produce that number.
#
# ★ The level and the golden are what make this more than a plumbing test.  Every other
# assertion here is satisfiable by a sink that counts packets and throws them away; `rms=` and
# `crc200=` are satisfiable only by one that really decoded this tone.
#
# WHAT THIS DOES NOT PROVE -- two recorded vacuities, deliberately NOT asserted:
#   1. UNDERRUNS.  QEMU has no real audio clock: the SAI DMA ISR that walks the graph runs on
#      QEMU's own schedule, not at 44100/128 Hz, so the ring's fill is a fiction and `under=`
#      climbs (or does not) for reasons that say nothing about the firmware.  On silicon an
#      underrun count is the whole point; here it is noise.  The gate asserts what IS meaningful
#      under a fake clock -- every frame decoded, none refused, no gap, right samples.
#   2. THE SERVO'S TRIM.  `trim_ppm` is the PLL correction the servo computed from that same
#      fictional fill, and audioPllTrimPpm() reaches no modelled PLL in QEMU.  Only its CLAMP is
#      checked (the number must stay inside the servo's own +/-200 ppm bound, which is a pure
#      arithmetic property of servo.h and holds under any input); whether the trim actually
#      tracks a source's clock is a SILICON claim, measured on the bench against a real phone.
#
# ★ AND THE PACING IS DELIBERATELY SLOWER THAN REAL TIME -- 100 ms per packet, not the 14.512 ms
# of audio each carries (see SOURCE_PERIOD in hci_peer.py).  A real source at this configuration
# needs ~43 kB/s, nearly four times what 115200 baud can carry; the link runs at 3 Mbaud on
# silicon, and qemu2 models no baud at all, so wall-clock pacing against an emulated guest is the
# only thing bounding the rate.  At the audio rate the guest's 1 KB HCI RX ring (barely 1.7 media
# packets) was overrun by ordinary TCG scheduling jitter and H4 desynchronised for good: MEASURED,
# 2 runs in 8 stalled at exactly pkts=2 immediately after START.  At 100 ms/packet, 8 consecutive
# runs were green with byte-identical rms and golden.  Nothing asserted here depends on the rate.
#
# DEMONSTRATED RED (2026-09-09), three ways.  Each mutation was made in the named COMMITTED
# source, build/ rebuilt, this gate run, confirmed to fail BY THE NAMED ASSERTION, then reverted
# (`git -C ~/Development/M2Radio checkout -- <file>`), rebuilt and confirmed green again;
# `git -C ~/Development/M2Radio status` clean after all three:
#   (a) bt/A2dpSink.h, the constructor: `Sdp::setRole(Sdp::SINK)` removed, so the image publishes
#       the AudioSource record instead --
#         FAIL: [sink] the AudioSink SDP record is wrong: PEER-SINK-RECORD-BAD hex=07000100050002350000
#       (the query matches no live record, so the sink answers a well-formed but EMPTY attribute
#       list -- which is exactly how this fails on a phone: it sees no A2DP sink and offers no
#       audio route.)
#   (b) bt/SbcDecoder.cpp: SYNTH_SCALE 8.0f -> 16.0f (the synthesis-window scale doubled) --
#         FAIL: [sink] decoded level wrong (want mean|L| ~10430): bt_sink fill=0 trim_ppm=-200 rms=20826 rms_blocks=750 crc200=0x9FE9EBF4
#       (every structural assertion above still passes: the frames arrive, decode, and fill the
#       ring.  Only the level says the samples are wrong.)
#   (c) hci_peer.py's own SET_CONFIGURATION rate byte 0x21 -> 0x11 (48 kHz), a peer asking for a
#       rate the sink's graph does not run --
#         FAIL: [sink] the sink rejected a source command: PEER-SOURCE-REJECT sig=3 hex=23030729
#       (the sink REJECTS it 0x07/0x29 -- badCat = media codec, INVALID_CODEC_PARAMETER -- as it
#       must; PEER-SOURCE-REJECT sig=3 records the refusal from the other side.  This is the arm
#       that proves the configuration is really validated rather than adopted verbatim.)
#
# ★ The GOLDEN (SINK_GOLDEN below) is the CRC32 of the first 200 decoded left-channel blocks.  It
# is reproducible because both halves are: the peer streams the committed sine.sbc's frames in
# order, and SbcDecoder is integer arithmetic.  It moves only if the decoder's OUTPUT moves --
# which is a change to be justified, never re-goldened away.
set -e
DIR=$(cd "$(dirname "$0")" && pwd)
EVKB=$(cd "$DIR/../../.." && pwd)
QEMU="$EVKB/tools/qrun"
. "$EVKB/tools/gate-lib.sh"
gate_init
[ "$(gate_board)" = rt1176 ] || {
    echo "FAIL: this gate is rt1176-only (EVKB_BOARD=$(gate_board)) -- the M.2 socket is on the MIMXRT1170-EVKB"; exit 1; }

fail() { echo "FAIL: $*"; exit 1; }

SINK_GOLDEN=954D2C41      # crc200 over the first 200 decoded blocks of sine.sbc (measured 2026-09-09; two runs identical)

# The DEFAULT build -- no -D of any kind.  Unlike bt_tone_test (which needs a target name to go
# looking for a peer) a sink configures nothing: it listens.  Under the vacuity harness
# (GATE_VACUITY=1, no ARM toolchain guaranteed) skip the (re)build once the ELF exists, the same
# convention every other bt gate uses.
BUILD_DIR="$DIR/build"
ELF="$BUILD_DIR/bt_sink_test.elf"

if [ "${GATE_VACUITY:-}" = "1" ] && [ -x "$ELF" ]; then
    :
else
    mkdir -p "$BUILD_DIR"
    CONFIGURE_RC=0
    cmake -S "$DIR" -B "$BUILD_DIR" -DCMAKE_TOOLCHAIN_FILE="$EVKB/toolchain/rt1170-evkb.toolchain.cmake" \
          >"$BUILD_DIR/configure.log" 2>&1 || CONFIGURE_RC=$?
    BUILD_RC=0
    if [ "$CONFIGURE_RC" -eq 0 ]; then
        cmake --build "$BUILD_DIR" >"$BUILD_DIR/build.log" 2>&1 || BUILD_RC=$?
    fi
    if [ "$CONFIGURE_RC" -ne 0 ] || [ "$BUILD_RC" -ne 0 ]; then
        fail "build/ did not build -- see build/configure.log / build.log"
    fi
fi

OUT="$BUILD_DIR/sink.uart"; DBG="$BUILD_DIR/sink.dbg"; RES="$BUILD_DIR/sink.peer"
rm -f "$OUT" "$DBG" "$RES"

# ★ VACUITY-ONLY peer stand-in.  This gate reads BOTH the UART capture and the peer's tally, and
# the vacuity harness replaces QEMU with a script that opens no socket -- so a replay can never
# reach the peer at all, and every PEER-* assertion below would fail on a perfectly good fixture.
# GATE_PEER_FIXTURE names a committed peer transcript to use INSTEAD of running the peer, and is
# honoured ONLY under GATE_VACUITY=1, so it is strictly no weaker than the GATE_VACUITY build
# skip above: a real run (the sweep included) sets neither and always runs the real peer.
if [ "${GATE_VACUITY:-}" = "1" ] && [ -n "${GATE_PEER_FIXTURE:-}" ]; then
    SOCK=""
    "$QEMU" $(gate_qemu_machine) -kernel "$ELF" -display none $(gate_console "$OUT") \
        -d guest_errors -D "$DBG" &
    P=$!; gate_pid $P
    cp "$GATE_PEER_FIXTURE" "$RES"
    PEER_RC=0
    sleep 1
else
    SOCK="/tmp/m2sink_$$.sock"; rm -f "$SOCK"; gate_tmp "$SOCK"
    "$QEMU" $(gate_qemu_machine) -kernel "$ELF" -display none $(gate_console "$OUT") \
        -serial unix:"$SOCK",server -d guest_errors -D "$DBG" &
    P=$!; gate_pid $P
    PEER_RC=0
    python3 "$EVKB/examples/networking/m2_hci_probe/hci_peer.py" source "$SOCK" "$DIR/sine.sbc" > "$RES" 2>&1 || PEER_RC=$?
fi

# The peer runs in the FOREGROUND and returns once it has streamed every packet (or given up), so
# $RES is settled here.  Then wait for the LAST line this gate parses, never an earlier one (the
# m2_rx_demo[irq] mid-line reap is the standing lesson): the heartbeat prints hb / bt_sink /
# bt_link / bt_hci in that order, so a bt_hci line for every bt_sink line means the freshest
# heartbeat block is COMPLETE and the bt_sink line below cannot be read torn.  A failed peer means
# a failed run: poll briefly, not the full deadline.
LOOP=60; [ "$PEER_RC" -eq 0 ] || LOOP=20
for _ in $(seq 1 $LOOP); do
    if [ -f "$OUT" ] && grep -q "rms_blocks=750" "$OUT" 2>/dev/null; then
        NB=$(grep -c "^bt_sink " "$OUT" 2>/dev/null || true); NH=$(grep -c "^bt_hci " "$OUT" 2>/dev/null || true)
        # `cmd && cmd` as the LAST statement of this body would abort the whole gate under `set -e`
        # whenever it is false (the not-yet-complete heartbeat case) -- exit 1 with no message at
        # all, the silent-death shape gate-lib.sh exists to stop.  Keep it an if/then.
        if [ "${NB:-0}" -gt 0 ] && [ "${NH:-0}" -ge "${NB:-0}" ]; then break; fi
    fi
    sleep 0.25
done
gate_reap $P
gate_require_capture "$OUT" "source phase"
# CONSOLE.println() emits "\r\n" (Print::println); strip the \r so end-of-line ($) anchors below
# match the true end of the token rather than a trailing carriage return.
tr -d '\r' < "$OUT" > "$OUT.nocr" && mv "$OUT.nocr" "$OUT"
echo "==== captured UART ===="; cat "$OUT"
echo "==== peer ===="; cat "$RES"

# --- the sink's own account, from the UART -----------------------------------------------------
grep -q "RT1176 BT sink test up" "$OUT"          || fail "[sink] banner missing"
grep -q '^sink=listening name="EVKB-SINK" cod=0x240414' "$OUT" \
    || fail "[sink] never became listening"
grep -q '^conn_req: bd=AA:BB:CC:DD:EE:01 -> accept(slave, unbonded)' "$OUT" \
    || fail "[sink] the source's page was not accepted as an unbonded slave"
# --- the source's account of what went wrong, FIRST (un-fakeable: every value is the PEER's).
# These are the CAUSES; the UART assertions below are their downstream effects, and a run that
# died of a bad SDP record or a refused configuration must fail by cause, not as "never reached
# STREAMING" -- measured: RED demo (a) reports exactly this line and (c) reports the reject.
if grep -q "PEER-SINK-RECORD-BAD" "$RES";  then fail "[sink] the AudioSink SDP record is wrong: $(grep -m1 PEER-SINK-RECORD-BAD "$RES")"; fi
if grep -q "PEER-SINK-NOT-SNK" "$RES";     then fail "[sink] DISCOVER did not advertise a SNK endpoint: $(grep -m1 PEER-SINK-NOT-SNK "$RES")"; fi
if grep -q "PEER-SINK-CAPS-BAD" "$RES";    then fail "[sink] the sink's capabilities are not the 44.1/16/8/loudness set: $(grep -m1 PEER-SINK-CAPS-BAD "$RES")"; fi
if grep -q "PEER-SINK-DELAY-BAD" "$RES";   then fail "[sink] the DelayReport value is out of range: $(grep -m1 PEER-SINK-DELAY-BAD "$RES")"; fi
if grep -q "PEER-SOURCE-REJECT" "$RES";    then fail "[sink] the sink rejected a source command: $(grep -m1 PEER-SOURCE-REJECT "$RES")"; fi
if grep -q "PEER-ACCEPT-BAD-ROLE" "$RES";  then fail "[sink] the sink accepted the page in the wrong role: $(grep -m1 PEER-ACCEPT-BAD-ROLE "$RES")"; fi
if grep -q "PEER-SOURCE-MEDIA-FROM-SINK" "$RES"; then fail "[sink] the sink sent media back at the source: $(grep -m1 PEER-SOURCE-MEDIA-FROM-SINK "$RES")"; fi

grep -q '^a2dp_sink=ok bonds=1 paired_by=peer' "$OUT" \
    || fail "[sink] bring-up did not reach STREAMING as acceptor"
grep -q '^sink: adopted sbc bitpool=53 mode=3 blocks=16 sub=8' "$OUT" \
    || fail "[sink] the source's configuration was not adopted"
grep -q '^streaming by=incoming bitpool=53' "$OUT" || fail "[sink] node never began"
grep -q '^sink: delay_report=' "$OUT" \
    || fail "[sink] no DelayReport was sent to a source that configured it"

# --- the source's positive tally (it really streamed) -------------------------------------------
grep -q "^PEER-SINK-RECORD ok" "$RES"      || fail "[sink] the source never read our AudioSink record"
grep -q "^PEER-SOURCE-STARTED" "$RES"      || fail "[sink] the source never reached START"
grep -qE "^PEER-SOURCE pkts=150 frames=750 delay_reports=1 sink_record=1 started=1 errors=0" "$RES" \
    || fail "[sink] source tally wrong: $(grep -m1 '^PEER-SOURCE ' "$RES")"

# --- what actually arrived in the audio graph --------------------------------------------------
LASTHB=$(grep -E "^hb streaming=1 " "$OUT" | tail -1)
[ -n "$LASTHB" ] || fail "[sink] no streaming heartbeat"
echo "$LASTHB" | grep -qE "pkts=150 frames=750 seqgaps=0 " || fail "[sink] the node did not receive every packet/frame with no gap: $LASTHB"
echo "$LASTHB" | grep -qE " bad=0$"                         || fail "[sink] the decoder refused frames: $LASTHB"
# over= is the ring's OVERRUN count, and crc200 depends on it as much as on bad=: an overrun drops the NEW
# frame, so a run with over>0 has decoded a DIFFERENT set of blocks from the one the golden was pressed over,
# and would either move the golden or (worse) hit it by accident with 750 frames of which some were never
# played.  bad=0 alone does not say that -- a dropped frame is not a refused one.
echo "$LASTHB" | grep -qE " over=0 "                        || fail "[sink] the PCM ring overran -- frames were dropped, so crc200 is over a different block set: $LASTHB"
LASTSINK=$(grep -E "^bt_sink " "$OUT" | tail -1)
[ -n "$LASTSINK" ] || fail "[sink] no bt_sink ring/servo line"
echo "$LASTSINK" | awk '{for(i=1;i<=NF;i++) if ($i ~ /^rms=/) { split($i,a,"="); v=a[2]+0 } } END{exit !(v >= 9900 && v <= 10950)}' \
    || fail "[sink] decoded level wrong (want mean|L| ~10430): $LASTSINK"
echo "$LASTSINK" | grep -q "rms_blocks=750"                || fail "[sink] not every frame was decoded into a block: $LASTSINK"
echo "$LASTSINK" | grep -q "crc200=0x$SINK_GOLDEN"          || fail "[sink] decoded-PCM golden moved (was 0x$SINK_GOLDEN): $LASTSINK"
echo "$LASTSINK" | awk '{for(i=1;i<=NF;i++) if ($i ~ /^trim_ppm=/) { split($i,a,"="); v=a[2]+0 } } END{exit !(v >= -200 && v <= 200)}' \
    || fail "[sink] servo trim outside its clamp: $LASTSINK"

echo "PASS: A2DP SINK -- a paging source is accepted as an unbonded slave, paired and encrypted by the peer, reads our AudioSink record, configures 44.1/joint/16/8/loudness/53 with delay reporting and reaches START; 150 RTP packets / 750 SBC frames of the 1 kHz tone decoded into the graph (no gap, no refusal, level and PCM golden exact, one DelayReport); underruns and the servo's trim are SILICON claims -- QEMU has no audio clock"
