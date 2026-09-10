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
#     * it OPENS AVCTP ITSELF (PSM 0x0017) once STREAMING, and absolute volume then works over that
#       channel: the peer registers for VOLUME_CHANGED (answered INTERIM with the target's current
#       volume, 100) and writes SetAbsoluteVolume(0x40), which is ACCEPTED and reaches the codec
#       (`volume=64` on the UART).  ★ THE PEER OPENS NOTHING -- deliberately, because an iPhone does
#       not: bench 2026-09-09, four connections over ~15 minutes with `bt_avrcp avctp=0` throughout,
#       even after the AVRCP Target record moved to Category 2.  iOS waits for the sink to initiate,
#       as real speakers do.  So a sink that only ever ACCEPTS an AVCTP channel has a dead volume
#       slider on every phone, and passes every other assertion in this gate;
#     * and then the real claim: 150 RTP packets carrying 750 119-byte SBC frames of a 1 kHz
#       tone -- the committed sine.sbc, the same file M2Radio's own decoder tests use -- reach
#       AudioInputBluetooth, are decoded with NO refused frame and NO sequence gap, and land in
#       the PCM ring with the RIGHT AUDIO IN THEM: mean |L| per block in a band around the
#       encoded amplitude, and a CRC32 golden over the first 200 decoded blocks.  A decoder that
#       returns silence, or the wrong scale, or the wrong samples, cannot produce that number.
#     * and then, once the tone is streamed (NEW-46), the RUNTIME PAIRING WINDOW.  The sink is
#       discoverable for a window at boot and closes it `paired` when the link comes up; a `pair`
#       typed WHILE STREAMING is REFUSED (`reason=link_up`); the peer then DROPS the link and the
#       sink opens a window BY ITSELF, which the peer SEES as a Write_Scan_Enable of 0x03 -- a
#       controller write the firmware cannot invent -- with PREPARE (Write_Simple_Pairing_Mode)
#       re-issued; `pair` and `forget` each open a commanded window; and after `forget` the peer's
#       re-page finds NO stored key and pairs Just Works from scratch (a second
#       accept(slave, unbonded), a second neg_reply, a second pairing_complete).  That last is the
#       NEW-43 recovery WITHOUT A REBOOT, which is the whole of NEW-46.
#     * ★ THE COMMANDS ARE REALLY TYPED, and only the driver can say so.  sink_console.py connects to
#       the console chardev's socket and sends them; its log (`$CON`) is the gate's evidence that
#       they were sent at all, and it is read BEFORE the firmware assertions so a driver that died
#       cannot present as a firmware defect.  A capture showing `pairing=on reason=cmd` with nothing
#       typed would be a sink opening windows by itself -- ruled out by the DRIVER-SENT lines for all
#       three post-drop commands together with the ORDER those windows appear in the capture
#       (a commanded window, then the wipe, then a second commanded window).  ★ The driver also holds
#       the one ORDERING the 0x03 above depends on: reconcileScan() writes only on a CHANGE, so the
#       drop window's write and the two commanded ones coalesce into a single controller write and
#       whichever window opened first owns it.  The driver waits for the sink's own `scan_enable=0x03`
#       after the drop and logs DRIVER-SAW BEFORE it types, and the gate asserts that order -- which
#       is what makes the peer's 0x03 the DROP window's rather than merely some window's.
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
#   3. THE POST-`forget` MEDIA PATH -- that the sink is re-USABLE, not merely re-PAIRABLE.  The
#      peer's re-page is deliberately MINIMAL: it pairs, injects Encryption_Change and stops.  It
#      does NOT redo SDP, AVDTP or media (hci_peer.py says why at that branch: a second A2DP
#      bring-up would call AudioInputBluetooth::begin(), clearing m_primed with no media left to
#      send, and the [jit] `primed=1` assertion would read 0 on a healthy run).  So nothing here
#      would notice if a re-paired sink could no longer play.  Widening the peer was considered and
#      REJECTED: the run is already ~39 s against tools/qrun's 60 s cap, and CLAUDE.md records that
#      QEMU cannot carry A2DP at the audio rate anyway.  Task 6's bench is where a real phone
#      re-pairs and streams again -- and NEW-43, the defect this feature answers, is a PAIRING
#      defect, not a streaming one.
#   4. `pairing=off reason=paired` ON THE SECOND WINDOW.  Following from 3: the re-page never
#      reaches STREAMING, so the window `forget` opened is still open when the run ends and the
#      close-as-paired transition is exercised EXACTLY ONCE here, on the boot window.  Per-window,
#      it is pinned by BtSinkSession's host suite; on the wire, by the bench's every reconnect.
#      The PASS: line below overclaims neither -- read it as the boot window's close, which is what
#      it says.
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
# DEMONSTRATED RED (2026-09-09), four ways.  Each mutation was made in the named COMMITTED
# source, build/ rebuilt, this gate run, confirmed to fail BY THE NAMED ASSERTION, then reverted
# (`git -C ~/Development/M2Radio checkout -- <file>`), rebuilt and confirmed green again;
# `git -C ~/Development/M2Radio status` clean after all four:
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
#   (d) bt/A2dpSink.cpp: the `openAvctp()` call on the transition to STREAMING commented out (the
#       state the bench measured) --
#         FAIL: [sink] the sink did not open AVCTP itself
#       (the peer opens none either, so the channel never exists and no volume command can arrive:
#       every OTHER assertion in this gate still passes, which is precisely why this one is here.)
#
# DEMONSTRATED RED AGAIN (2026-09-09) for the NEW-42 assertions this gate grew -- the jitter
# instrument's counts, the pre-fill, and the AVRCP counter partition.  Same method: mutate the named
# COMMITTED source, rebuild, run, confirm the NAMED assertion fires, revert, rebuild, confirm green;
# both repos `git status` clean afterwards:
#   (e) AudioInputBluetooth.cpp onMedia(): `m_gap[gapBucketOf(d)]++` -> `m_gap[0]++` (the histogram
#       stubbed -- it still COUNTS every interval, it just stops reading the duration) --
#         FAIL: [jit] the 50-120 ms band does not carry the bulk of the peer's 100 ms pacing (want g80+g120 >= 120): bt_jit gapmax_ms=3142 g30=149 g50=0 g80=0 g120=0 gbig=0 ...
#       (note g30=149: the SUM assertion is still satisfied, which is what makes the two separate
#       assertions rather than one.  Nothing else in the gate moves -- every packet still arrived,
#       decoded and hit the golden, because the histogram sits beside the audio path, not in it.)
#   (f) the same line -> `m_gap[gapBucketOf(d)] += 2;` (each interval counted twice) --
#         FAIL: [jit] the gap buckets do not account for every interval (want 149 = pkts-1): bt_jit gapmax_ms=3142 g30=0 g50=4 g80=10 g120=280 gbig=4 ...
#       (and here the BAND assertion is still satisfied at 290, the mirror image of (e).)
#   (g) M2Radio bt/Avrcp.cpp service(): KIND_ANSWERED folded back into `m_unsupported++`, which is
#       exactly the pre-NEW-42 code --
#         FAIL: [sink] the target counted an answered AV/C command as UNSUPPORTED (NEW-42 item B): bt_avrcp avctp=1 notif=1 ans=0 unsup=1 drop=0 vol=64
#       (`vol=64` is UNCHANGED on that line -- the volume really did reach the codec.  The defect is
#       only ever visible in the counters, which is why item B needed an assertion of its own.)
#   (h) a scratch `-DBT_SINK_PREFILL=OFF` build dir (the bench's control arm), `build` symlinked to
#       it for the run and restored afterwards --
#         FAIL: [jit] the START pre-fill never completed (want primed=1): bt_jit ... fillmax=5 trimlo=-200 trimhi=0 overev=0 reprimes=0 primed=0 prime_ms=0
#       (the ring runs shallow and the servo pins at its clamp, but neither of those is assertable
#       here -- both are consume-side fictions under QEMU's clock.  `primed=` is not.)
#
# DEMONSTRATED RED (2026-09-10) for the NEW-46 PAIRING-WINDOW assertions in the [pair] block below --
# five mutations, one more than the plan called for.  Same method throughout: mutate the named
# COMMITTED source, rebuild, run this gate, confirm the NAMED assertion fires, revert, rebuild,
# confirm green; `git -C ~/Development/M2Radio status --short` empty at b90d52b afterwards.
#   (i) M2Radio bt/BtSinkSession.cpp, `openWindow()`: `m_sink.link().startPrepare();` removed, so no
#       window re-enables Simple Pairing --
#         FAIL: [pair] PREPARE was not re-issued after the drop (want a 2nd 'ssp_mode: st=ok status=0x00 mode=1'): 1
#       (this is the NEW-43 half of the feature: after one failed SSP the legacy-PIN fallback has
#       written Write_Simple_Pairing_Mode=0, and a window that does not re-issue PREPARE leaves the
#       sink passcode-only until a reboot -- exactly the defect the window exists to heal.  Every
#       other assertion in this gate passes: the window still opens, the peer still re-pages, and
#       this peer pairs Just Works regardless of what mode the sink is in.)
#   (ii) the sketch's `runCommand()`, `forget`: the `wipe()` call and its report both removed --
#         FAIL: [pair] forget did not wipe the one bond
#   (iii) the same, but the report KEPT and only the wipe removed -- the sharpest of the five, and
#       the reason (ii) is not enough on its own:
#         FAIL: [pair] the re-page after forget was not accepted as UNBONDED (the bond survived, or no re-page)
#       ★ `bonds_forgotten=1` STILL PRINTS on that build, because the count is read BEFORE the wipe
#       -- so the one line a reader would trust is the one that lies.  What actually changes is on
#       the far side of the link: the peer's re-page degrades from `accept(slave, unbonded)` +
#       `neg_reply (no stored key)` to `accept(slave)` + `reply(stored type=4)`, and the sink
#       authenticates with the old key having run no SSP at all.  Only an assertion on the RE-PAGE's
#       shape can see that; the wipe's own report cannot.
#   (iv) hci_peer.py's source run-loop: `s["dropped"] = True` latched WITHOUT sending the
#       Disconnection_Complete (a peer that believes it dropped the link and did not) --
#         FAIL: [pair] no drop window after the peer's disconnect
#       (the sink's link is still up, so it opens nothing; this is the arm that proves the drop
#       window is driven by the WIRE and not by the sink's own timers.)
#   (v) M2Radio bt/BtSinkSession.cpp, `openWindow()`: the `canPair()` guard dropped, so a window
#       opens with a link up --
#         FAIL: [pair] pair while streaming was not REFUSED
#       (the refusal is also the whole command path's only positive proof -- socket, LPUART1 RX,
#       serialEvent1(), the parser, runCommand() -- so this arm reddens if the console dies too.)
# ★ ONE MESSAGE WAS REWORDED AFTER ITS DEMONSTRATION: (i)'s said "for the drop window", which
# `grep -c ... -ge 2` cannot localise (in the real capture the second `ssp_mode` lands after the
# FORGET window, not the drop one).  It now says what it counts.  (i) and (iii) were RE-RUN against
# the reworded file, so both lines above are verbatim; (ii), (iv) and (v) quote messages this review
# did not touch.
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
# NEW-46: the CONSOLE DRIVER's own log.  It holds what only the driver knows -- that it typed the
# commands at all, and when -- which no amount of firmware output can establish: a capture showing
# `pairing=on reason=cmd` with nothing having been typed would be a firmware that opens windows by
# itself, and that is the shape this file exists to rule out.
CON="$BUILD_DIR/sink.console"
rm -f "$OUT" "$DBG" "$RES" "$CON"

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
    # ... and, for the same reason, a stand-in for the CONSOLE DRIVER's log: no driver runs against a
    # replay either, so without this every DRIVER-* assertion would fail on a perfectly good fixture.
    # Same rule as the peer fixture -- honoured ONLY under GATE_VACUITY=1, and an unset variable leaves
    # an EMPTY file rather than a missing one, so the assertions still fail by name rather than on
    # `grep: no such file`.
    if [ -n "${GATE_CONSOLE_FIXTURE:-}" ]; then cp "$GATE_CONSOLE_FIXTURE" "$CON"; else : > "$CON"; fi
    PEER_RC=0
    CON_RC=0                       # no driver ran, so there is no exit code; the fixture is the whole
                                   # of what the DRIVER-* assertions have to read here.
    sleep 1
else
    SOCK="/tmp/m2sink_$$.sock"; rm -f "$SOCK"; gate_tmp "$SOCK"
    # NEW-46: the console is a SOCKET so the driver can type at it, with logfile= so every byte the
    # sink prints still lands in $OUT for the assertions above and below (smoke-tested: the banner
    # lands in the file).  The vacuity harness's fake QEMU recognises only `-serial file:`, which is
    # why the GATE_VACUITY branch above keeps gate_console -- do not collapse the two.
    # ★ The console is the FIRST -serial (LPUART1); the HCI socket stays the SECOND (LPUART2).  Swap
    # them and the firmware runs perfectly with an empty capture, which is indistinguishable from
    # firmware that never started (CLAUDE.md records that exact trap).
    # ★ THE SLOT IS SPELLED OUT HERE rather than taken from gate-lib.sh, because gate_console() emits
    # a `-serial file:` and this branch needs a socket chardev.  That is deliberate and single-caller:
    # a `gate_console_socket()` helper was weighed in review and judged over-engineering for one use.
    # gate_console()'s own warning applies unchanged -- it is the SLOT that matters, not the form --
    # so if a second example ever wants a writable console, move BOTH there rather than copying this.
    # ★ THE PORT IS PROBED, NOT ASSUMED.  A PID-derived constant alone collides with whatever else on
    # this machine holds that port, and a QEMU that cannot bind exits at once -- surfacing as "no UART
    # capture", the same text dead firmware produces.  The base is still PID-derived (so two runs
    # started together differ); the probe walks up from it and the walk's EXHAUSTION is reported by
    # name.  ★ It is a TOCTOU probe and cannot be otherwise: it binds, closes, and QEMU binds a moment
    # later, so a port taken in that gap is NOT reported by name -- it surfaces as "no UART capture",
    # the very text the probe exists to avoid.  What the probe buys is the common case (a port held
    # for the run's duration by something else); the racing case is left, knowingly, because closing
    # it needs a chardev fd handed to QEMU and qemu2 takes none.
    CPORT=$(python3 -c '
import socket, sys
base = int(sys.argv[1])
for p in range(base, base + 64):
    s = socket.socket()
    try:
        s.bind(("127.0.0.1", p)); s.close(); print(p); break
    except OSError:
        s.close()
else:
    sys.exit(1)
' $((45500 + ($$ % 400)))) || fail "no free loopback port for the console chardev"
    "$QEMU" $(gate_qemu_machine) -kernel "$ELF" -display none \
        -chardev socket,id=con,host=127.0.0.1,port="$CPORT",server=on,wait=off,logfile="$OUT" \
        -serial chardev:con \
        -serial unix:"$SOCK",server -d guest_errors -D "$DBG" &
    P=$!; gate_pid $P
    # The driver runs in the BACKGROUND, beside the peer: it waits on UART milestones the peer's own
    # traffic produces, so the two must be live at the same time.  Its budget is below tools/qrun's
    # 60 s cap on QEMU, so a driver that hangs still leaves a capture to fail against.
    python3 "$DIR/sink_console.py" "$CPORT" 45 > "$CON" 2>&1 &
    CPID=$!; gate_pid $CPID
    PEER_RC=0
    python3 "$EVKB/examples/networking/m2_hci_probe/hci_peer.py" source "$SOCK" "$DIR/sine.sbc" > "$RES" 2>&1 || PEER_RC=$?
    # ★ The driver's EXIT CODE is kept, and read at the top of the [pair] block.  Discarding it is how
    # a harness fault gets reported as a firmware defect: kill the driver after its second command and
    # the first thing that fails is `[pair] the commanded windows never opened`, which blames the sink
    # for a run in which nothing was ever typed at it.
    if [ "$PEER_RC" -eq 0 ]; then
        CON_RC=0; wait $CPID 2>/dev/null || CON_RC=$?
    else
        # A peer that failed FAST (it exits 2 at t~0 when it cannot reach the socket) leaves the driver
        # waiting on milestones that will never arrive, and a plain `wait` would sit out its whole 45 s
        # budget for a run that was over in a second.  Reap it -- and force CON_RC=0, because a driver
        # WE killed is not a driver fault: the peer's own failure is diagnosed by the PEER-* assertions
        # further down, which run before the [pair] block.
        kill $CPID 2>/dev/null || true; wait $CPID 2>/dev/null || true; CON_RC=0
    fi
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
# ★ AND DROP A TORN FINAL LINE, because gate_reap kills QEMU an ARBITRARY moment after the poll loop broke.
# The loop's `NH >= NB` guarantees a complete heartbeat block existed AT THE BREAK; it guarantees nothing
# about the instant of the kill, and the guest keeps printing in between.  Every `tail -1` selector below
# ($LASTHB, $LASTSINK, $LASTJIT, $LASTAVRCP) would then read a HALF-WRITTEN line and fail on a healthy run --
# a spurious red under load, and a new hazard as of NEW-46: those selectors used to be `^hb streaming=1 `
# and friends, which a torn line could not satisfy, and they are now the freshest line of each kind.
# A torn line has exactly one signature -- the file does not end in a newline -- so removing it fixes all
# four at the source, which is strictly more than re-checking any one of them.  (Command substitution
# strips a trailing newline, so a non-empty result here means the last byte was NOT one.)
if [ -n "$(tail -c 1 "$OUT")" ]; then sed -e '$d' "$OUT" > "$OUT.whole" && mv "$OUT.whole" "$OUT"; fi
echo "==== captured UART ===="; cat "$OUT"
echo "==== peer ===="; cat "$RES"
echo "==== console driver ===="; cat "$CON" 2>/dev/null || true

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
if grep -q "PEER-SINK-AVRCP-BAD" "$RES";   then fail "[sink] the AVRCP target answered wrongly: $(grep -m1 PEER-SINK-AVRCP-BAD "$RES")"; fi
if grep -q "PEER-SINK-AVCTP-SECOND" "$RES"; then fail "[sink] the sink opened a SECOND AVCTP channel: $(grep -m1 PEER-SINK-AVCTP-SECOND "$RES")"; fi

grep -q '^a2dp_sink=ok bonds=1 paired_by=peer' "$OUT" \
    || fail "[sink] bring-up did not reach STREAMING as acceptor"
grep -q '^sink: adopted sbc bitpool=53 mode=3 blocks=16 sub=8' "$OUT" \
    || fail "[sink] the source's configuration was not adopted"
grep -q '^streaming by=incoming bitpool=53' "$OUT" || fail "[sink] node never began"
grep -q '^sink: delay_report=' "$OUT" \
    || fail "[sink] no DelayReport was sent to a source that configured it"
# ★ SINK-INITIATED AVCTP.  The peer models an iPhone and opens NOTHING; if the sink does not ask, no
# AVRCP channel exists at all, and the four assertions that follow -- two on the UART here, two on the
# peer's tally below -- are unreachable together.
grep -q '^sink: avctp connect' "$OUT" || fail "[sink] the sink did not open AVCTP itself"
grep -q '^volume=64' "$OUT" \
    || fail "[sink] SetAbsoluteVolume(0x40) never reached the codec (no ^volume=64 line)"

# --- the source's positive tally (it really streamed) -------------------------------------------
grep -q "^PEER-SINK-RECORD ok" "$RES"      || fail "[sink] the source never read our AudioSink record"
grep -q "^PEER-SOURCE-STARTED" "$RES"      || fail "[sink] the source never reached START"
grep -qE "^PEER-SOURCE pkts=150 frames=750 delay_reports=1 sink_record=1 started=1 errors=0" "$RES" \
    || fail "[sink] source tally wrong: $(grep -m1 '^PEER-SOURCE ' "$RES")"
grep -q "^PEER-SINK-AVCTP opened" "$RES" \
    || fail "[sink] the peer never saw an AVCTP Connection Request from the sink"
grep -q "^PEER-SINK-AVRCP interim_vol=100 set_ok=1" "$RES" \
    || fail "[sink] absolute volume did not complete: $(grep -m1 '^PEER-SOURCE-STATE ' "$RES")"

# --- what actually arrived in the audio graph --------------------------------------------------
# ★ THE COUNTERS ARE READ FROM THE FINAL HEARTBEAT, not from the last STREAMING one, and that changed
# with NEW-46 rather than being loosened.  Every field on this line is a LIFETIME tally --
# AudioInputBluetooth::begin() deliberately does not reset them, and its own comment says why -- so the
# last reading of the run is the completest one.  Since the peer now DROPS the link one second after
# its 150th packet, the last STREAMING heartbeat can land a few packets short of the total (measured:
# `pkts=147` on a run that received all 150 and whose final line reads 150).  Reading the final line is
# strictly STRONGER, not weaker: it also covers the post-drop window, where an `over` or a `bad` was
# invisible before.  That the sink really streamed is a separate assertion, immediately below, and
# `streaming by=incoming` above.
# ★ Every `tail -1` selector below is safe against a HALF-WRITTEN final line only because the capture had its
# torn last line dropped above -- see the note at the CR strip.  The poll loop's completeness check is about
# the break, not about the kill that follows it.
LASTHB=$(grep -E "^hb " "$OUT" | tail -1)
[ -n "$LASTHB" ] || fail "[sink] no heartbeat at all"
grep -qE "^hb streaming=1 " "$OUT" || fail "[sink] no streaming heartbeat -- the node never played"
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
# The sink's OWN account of the control channel, read from the freshest heartbeat: the channel is up and
# the volume the phone wrote is the volume the target holds.
LASTAVRCP=$(grep -E "^bt_avrcp " "$OUT" | tail -1)
[ -n "$LASTAVRCP" ] || fail "[sink] no bt_avrcp line"
# ★ avctp= is LIVE state -- "is there an L2CAP channel on PSM 0x0017 right now" -- and NEW-46's peer
# drops the link before the run ends, so it is read from the block belonging to the last STREAMING
# heartbeat.  MEASURED: that selector returns byte-for-byte the SAME line `tail -1` returned on the
# pre-NEW-46 committed transcript, so nothing here is relaxed; every OTHER field on the line is a
# lifetime counter and stays on the final line, where it is completest.
STREAMAVRCP=$(awk '/^hb /{s=($0 ~ /^hb streaming=1 /)} s && /^bt_avrcp /{l=$0} END{print l}' "$OUT")
[ -n "$STREAMAVRCP" ] || fail "[sink] no bt_avrcp line inside a streaming heartbeat"
echo "$STREAMAVRCP" | grep -q "avctp=1" || fail "[sink] the sink does not report its AVCTP channel up: $STREAMAVRCP"
echo "$LASTAVRCP" | grep -q "vol=64"  || fail "[sink] the target did not keep the volume the phone set: $LASTAVRCP"
# ★ notif=1 ans=1 unsup=0 pins NEW-42 item B ON THE WIRE.  The source phase sends exactly TWO AV/C
# commands -- one RegisterNotification(VOLUME_CHANGED) and one SetAbsoluteVolume -- and both must be
# counted as ANSWERED.  The old firmware answered SetAbsoluteVolume perfectly and still counted it
# `unsupported`, so `unsup=0` is the one reading that separates the two builds: every other assertion
# in this gate, `vol=64` included, passes either way.  `ans=1` and not 2 because GetCapabilities
# belongs to the [media] phase, which is bt_tone_test's peer, not this one.
echo "$LASTAVRCP" | grep -qE " notif=1( |$)" || fail "[sink] the target did not answer the peer's RegisterNotification exactly once: $LASTAVRCP"
echo "$LASTAVRCP" | grep -qE " unsup=0( |$)" || fail "[sink] the target counted an answered AV/C command as UNSUPPORTED (NEW-42 item B): $LASTAVRCP"
echo "$LASTAVRCP" | grep -qE " ans=1( |$)"   || fail "[sink] the target did not count SetAbsoluteVolume as ANSWERED (NEW-42 item B): $LASTAVRCP"

# --- the NEW-42 arrival instrument: its COUNTS, the only half QEMU can honestly speak to -----------
# ★ THE MAGNITUDES ARE DELIBERATELY NOT ASSERTED, and a later reader must not "strengthen" this back
# into a flake.  MEASURED 2026-09-09 over three runs: the guest reports one ~3.14 s inter-arrival
# EVERY run -- gapmax_ms 3142 / 3143 / 3142 -- on a peer whose own log proves it never paused
# (PEER-SOURCE-PROGRESS elapsed=5.2 / 10.2 / 15.2 for packets 50 / 100 / 150, exactly linear at its
# 100 ms pace).  The guest's millis() runs slow against wall time and micros() then takes a single
# step of the accumulated lag.  So `gbig` carries two to four entries with NO pause injected and
# varies run to run, and `gapmax_ms` is a clock artefact rather than a measurement.  That is also why
# nothing is scripted in hci_peer.py: a 400 ms pause could not be told apart from a 3.14 s artefact,
# so it would have bought nothing and would have changed a peer file three other gates share.  The
# bucket EDGES are pinned exactly where they can be pinned honestly -- tests/node_test case 11, which
# drives boundary values through the real onMedia() with an injectable micros().
# ★ And every CONSUME-side field on this line is exactly as fictional here as `under=` already is --
# reprimes, fillmin, fillmax, trimlo, trimhi, prime_ms -- because QEMU walks update() on its own
# schedule rather than at 44100/128 Hz.  None of them is asserted; they are SILICON claims.
LASTJIT=$(grep -E "^bt_jit " "$OUT" | tail -1)
[ -n "$LASTJIT" ] || fail "[jit] no bt_jit line -- the NEW-42 arrival instrument never printed"
# The five buckets must sum to exactly pkts-1: every ACCEPTED packet after the first records exactly
# one inter-arrival interval.  `pkts` is read from the same heartbeat the assertions above already
# pinned at 150, so the 149 is DERIVED rather than a bare literal that could drift apart from it.  A
# dead histogram sums to 0; a double-count sums to 298; a bucketing that drops the out-of-range case
# sums to less.  None of that is visible in any other field on the line.
GAPWANT=$(echo "$LASTHB" | awk '{for(i=1;i<=NF;i++) if ($i ~ /^pkts=/) { split($i,a,"="); print a[2]-1 } }')
[ -n "$GAPWANT" ] || fail "[jit] could not read pkts= from the final heartbeat: $LASTHB"
echo "$LASTJIT" | awk -v want="$GAPWANT" \
    '{s=0; for(i=1;i<=NF;i++) if ($i ~ /^g(30|50|80|120|big)=/) { split($i,a,"="); s+=a[2] } } END{exit !(s == want+0)}' \
    || fail "[jit] the gap buckets do not account for every interval (want $GAPWANT = pkts-1): $LASTJIT"
# The peer paces media at 100 ms -- a number the firmware has no way to know and cannot MEASURE its
# way to by accident -- so the bulk of the 149 intervals must land in the band that straddles that
# pace: g80 (50-80 ms) plus g120 (80-120 ms).  A stubbed m_gap[0]++ still sums to 149 and fails
# HERE, which is what makes this assertion separate from the one above rather than a duplicate; so
# does any bucketing that does not read the interval's DURATION at all.
# ★ WHAT THIS DOES *NOT* CATCH, stated so nobody reads it as stronger than it is: a constant
# m_gap[3]++ scores 149 in the band and passes both assertions.  The band cannot tell 50-80 ms from
# 80-120 ms -- that is the price of widening it below -- and the BUCKET EDGES are pinned instead on
# the host, in node_test case 11, where micros() is injectable and all four inclusive tops are
# driven on the boundary and demonstrated RED.
# ★ THE BAND RATHER THAN g120 ALONE, AND THE FLOOR AT 90 RATHER THAN 120: BOTH ARE MEASUREMENTS.
# Spec s4.2 first asked for `g120 >= 120` on the strength of three IDLE runs (138 / 140 / 142).
# Under eight CPU spinners g120 reads 122 / 124 / 125 -- a margin of two -- because the same
# guest-clock lag that invents the 3.14 s outlier also makes an ordinary 100 ms interval MEASURE
# SHORT.  Widening to the band was the first fix and it was NOT ENOUGH: a review re-ran it under the
# same eight spinners and it went RED 2 of 5, the band reading 118 / 119 / 124 / 125 / 135 against
# 144-145 idle.  The migration's dominant sink under load is gbig (4 idle -> 14..26), which is
# deliberately outside the band because the artefact lives there -- so the band absorbs the smaller
# half of the drift and not the larger.  A floor of 120 would therefore have joined this tree's
# documented load-sensitivity class (m2_rx_demo[txaggr], m2_uap_lwip[uap], bt_tone_test[media]) by
# construction, on a gate that has never been in it.  90 (60% of 149) leaves 28 of headroom under
# the worst measured reading and is still exactly 0 against the stubbed histogram, which is the only
# discrimination this assertion was ever buying.
echo "$LASTJIT" \
    | awk '{s=0; for(i=1;i<=NF;i++) if ($i ~ /^g(80|120)=/) { split($i,a,"="); s+=a[2] } } END{exit !(s >= 90)}' \
    || fail "[jit] the 50-120 ms band does not carry the bulk of the peer's 100 ms pacing (want g80+g120 >= 90): $LASTJIT"
# The START pre-fill ran to completion -- the node buffered TARGET blocks of audio before it played
# the first one, instead of playing from an empty ring and counting the whole start-up as underruns.
echo "$LASTJIT" | grep -qE " primed=1( |$)" \
    || fail "[jit] the START pre-fill never completed (want primed=1): $LASTJIT"

# --- the pairing window (NEW-46), judged against what the PEER and the DRIVER know -----------------------------
# The console driver sends `pair` while STREAMING (must be refused: a window is never open with a link up), and
# after the peer's injected drop sends `pair`, `forget` and `status`.  What only the peer can see: its own
# Write_Scan_Enable log -- the sink turning INQUIRY scan on after the drop is the window, and the firmware
# cannot invent a controller write.  What only the driver can see: that it sent the commands at all.
# ★ `forget` is exercised here ONLY because QEMU has no NVM behind the FlexSPI window (CLAUDE.md): the bond
# store this wipes is a within-boot table.  On silicon it is destructive, by design.
# ★ THE DRIVER IS JUDGED FIRST, and that ordering is the point rather than tidiness.  Every assertion below it
# is firmware-shaped, and a driver that died mid-run makes the ones after its death UNREACHABLE -- so without
# these two lines a harness fault is reported as a firmware defect.  MEASURED: kill the driver after its second
# command and the first failure reads `[pair] the commanded windows never opened`, blaming a sink that was
# never typed at.  That is the shape this file's header says the gate exists to prevent.
! grep -q "^DRIVER-FAIL" "$CON" || fail "[pair] the console driver failed: $(grep -m1 '^DRIVER-FAIL' "$CON")"
[ "$CON_RC" -eq 0 ] || fail "[pair] the console driver exited $CON_RC: $(tail -3 "$CON")"
# ★ secs=1[0-9][0-9] here and at the two windows below means "100..199 seconds remaining", i.e. a window that
# really is the ~120 s one and not a stub printing 0.  The exact number is the guest's own countdown and is not
# pinned: QEMU's clock makes any time MAGNITUDE a fiction (CLAUDE.md), so the range is as far as this may go.
grep -q "^pairing=on reason=boot secs=1[0-9][0-9]$" "$OUT"           || fail "[pair] no boot window: $(grep -m1 '^pairing=' "$OUT")"
grep -q "^pairing=off reason=paired$" "$OUT"                          || fail "[pair] the boot window did not close as PAIRED when the link came up"
grep -q "^DRIVER-SENT pair while-streaming" "$CON"                    || fail "[pair] the driver never sent pair while streaming: $(tail -3 "$CON")"
# `link_up` and not `not_listening`: Task 3's review split the refusal in two (spec 5), and THIS one is a
# genuine link up, so the text is unchanged.  Anchored, so the other reason can never satisfy it.
grep -q "^pairing=refused reason=link_up$" "$OUT"                     || fail "[pair] pair while streaming was not REFUSED"
grep -q "^PEER-SOURCE-DROP " "$RES"                                   || fail "[pair] the peer did not inject its drop: $(tail -3 "$RES")"
grep -q "^pairing=on reason=drop secs=1[0-9][0-9]$" "$OUT"            || fail "[pair] no drop window after the peer's disconnect"
# The un-fakeable half: after the drop, the peer must see inquiry scan turned ON (bit 0 of Write_Scan_Enable).
# ★ The comparison is against 0x00, not 0x02: with a link up BtLink::reconcileScan() composes page|inquiry and
# BOTH go off, which is the `PEER-SCAN-ENABLE 0x00` the baseline capture shows right after the page is accepted.
# So a windowless sink writes nothing at all after its drop, and this line is the whole difference.  Counted
# AFTER the PEER-SOURCE-DROP line, not anywhere in the log -- the BOOT window already wrote 0x03 once.
# ★ AND IT IS THE *DROP* WINDOW'S WRITE, which took a driver change to be able to say.  reconcileScan() writes
# only on a CHANGE, so the drop window's 0x03 and the two commanded windows' would coalesce into the single
# `PEER-SCAN-ENABLE 0x03` the peer logs -- leaving this assertion able to prove only that SOME window opened.
# The driver therefore waits for the sink's own `scan_enable=0x03` after the drop and logs DRIVER-SAW BEFORE it
# types anything; the two lines below pin that order in its log, so the write the peer saw provably happened
# while no command had yet been sent.
grep -q "^DRIVER-SAW scan_enable=0x03 after-drop$" "$CON" \
    || fail "[pair] the driver never saw the drop window's own scan write: $(tail -3 "$CON")"
# (the `while-streaming` send is deliberately not counted: it is long before the drop, and it is the
# post-drop commands -- the ones that open windows of their own -- that could steal the attribution.)
awk '/^DRIVER-SAW scan_enable=0x03 after-drop$/{saw=1}
     /^DRIVER-SENT .* after-(drop|forget)$/{ if (!saw) early=1 }
     END{exit !(saw && !early)}' "$CON" \
    || fail "[pair] a command was typed before the drop window's scan write, so the peer's 0x03 is not attributable to the DROP: $(grep '^DRIVER-S' "$CON" | tail -4)"
awk '/^PEER-SOURCE-DROP /{d=1} d && /^PEER-SCAN-ENABLE 0x03/{n++} END{exit !(n>=1)}' "$RES" \
    || fail "[pair] the peer never saw Write_Scan_Enable 0x03 (page|inquiry) after its drop: $(grep PEER-SCAN-ENABLE "$RES" | tail -3)"
# ★ THIS COUNTS RUN-WIDE AND CANNOT LOCALISE.  A second `ssp_mode` line proves SOME window after the boot one
# re-issued PREPARE, not WHICH -- in the real capture it lands after the FORGET window, not the drop.  That is
# enough for the class the assertion must catch (mutation (i) strips startPrepare() from openWindow(), so NO
# window re-issues and the count stays 1); a per-window claim would need a marker the firmware does not print.
[ "$(grep -c '^ssp_mode: st=ok status=0x00 mode=1' "$OUT")" -ge 2 ] \
    || fail "[pair] PREPARE was not re-issued after the drop (want a 2nd 'ssp_mode: st=ok status=0x00 mode=1'): $(grep -c '^ssp_mode' "$OUT")"
# ★ ALL THREE post-drop commands are read from the driver's log, not one of four.  A `pair` line alone leaves
# `DRIVER-SENT forget after-drop` unread, and the "reason=cmd twice" count below cannot then tell "pair opened
# one and forget opened one" from "forget opened two" -- which is exactly what that count claims.
grep -q "^DRIVER-SENT pair after-drop$" "$CON"                        || fail "[pair] the driver never sent pair after the drop: $(tail -3 "$CON")"
grep -q "^DRIVER-SENT forget after-drop$" "$CON"                      || fail "[pair] the driver never sent forget after the drop: $(tail -3 "$CON")"
grep -q "^bonds_forgotten=1$" "$OUT"                                  || fail "[pair] forget did not wipe the one bond"
# TWO commanded windows, not one, AND IN ORDER: `pair` opens one, then `forget` wipes and re-opens.  The
# ORDER is what makes the pair of them distinguishable from `forget` alone opening two -- a bare count cannot,
# and neither can a bare grep (with one, `forget`'s own line satisfies it and a driver that never sent `pair`
# passes).  Same idiom as networking/m2_uap_lwip[uap]'s configure-before-BSS_START check: assert the sequence,
# because a gate that only greps for the lines accepts a firmware that emits them the wrong way round.
awk '/^pairing=on reason=cmd secs=1[0-9][0-9]$/{ if (st==0) st=1; else if (st==2) st=3 }
     /^bonds_forgotten=1$/{ if (st==1) st=2 }
     END{exit !(st==3)}' "$OUT" \
    || fail "[pair] the commanded windows are not pair -> wipe -> forget in that order (want reason=cmd, bonds_forgotten=1, reason=cmd): $(grep -cE '^pairing=on reason=cmd' "$OUT") cmd window(s)"
grep -q "^PEER-SOURCE-REPAGE " "$RES"                                 || fail "[pair] the peer did not re-page after forget"
[ "$(grep -c '^conn_req: bd=AA:BB:CC:DD:EE:01 -> accept(slave, unbonded)' "$OUT")" -ge 2 ] \
    || fail "[pair] the re-page after forget was not accepted as UNBONDED (the bond survived, or no re-page)"
[ "$(grep -c '^link_key_req: .* -> neg_reply (no stored key)' "$OUT")" -ge 2 ] \
    || fail "[pair] the re-page after forget did not restart pairing from scratch"
[ "$(grep -c '^pairing_complete: status=0x00' "$OUT")" -ge 2 ]        || fail "[pair] the re-page after forget did not pair Just Works again"
# `status` prints a heartbeat NOW rather than at the next tick of the 1 s timer.  The driver judges it two
# ways and reports OK only if both hold: STRUCTURALLY, the `cmd=status` token -- which the timer never emits --
# must be followed IMMEDIATELY by a COMPLETE block, the `hb ` line and EXACTLY the four that close it
# (loop() prints all six back to back from one context, and nothing else writes the console, so nothing can
# interleave); and by the HOST's clock, inside 0.9 s of the send.  That second half is measured OUTSIDE the
# guest, on the driver's own wall clock, which is why it is allowed at all -- CLAUDE.md's rule is that no gate
# may assert a duration the GUEST reports.  ★ The "exactly four" is a 2026-09-10 correction, not decoration:
# with the lazy pattern the driver carried before it, a block truncated after its `hb ` line still matched by
# borrowing the NEXT block's closing line, and the docstring's "complete block" was simply not what the regex
# said.  ★ And the 0.9 s was measured IDLE only (0.03-0.04 s, ~22x headroom); sink_console.py says so, and
# says which way to read a SLOW verdict in a loaded sweep.
# ★ tail -3 and not `grep DRIVER-STATUS`: in the commonest failure the driver never reached the verdict at
# all, so that grep prints NOTHING and the message reads as if the tool had no opinion.  The last three lines
# always say where it stopped -- the same reason every DRIVER-* failure above quotes `tail -3 "$CON"`.
grep -q "^DRIVER-STATUS-OK" "$CON"                                    || fail "[pair] status did not produce an immediate heartbeat: $(tail -3 "$CON")"

echo "PASS: A2DP SINK -- a paging source is accepted as an unbonded slave, paired and encrypted by the peer, reads our AudioSink record, configures 44.1/joint/16/8/loudness/53 with delay reporting and reaches START; 150 RTP packets / 750 SBC frames of the 1 kHz tone decoded into the graph (no gap, no refusal, level and PCM golden exact, one DelayReport); the SINK opens AVCTP itself at a peer that opens none (as an iPhone does not) and absolute volume completes over it -- VOLUME_CHANGED answered INTERIM vol=100, SetAbsoluteVolume(0x40) ACCEPTED and on the codec (volume=64, bt_avrcp avctp=1 vol=64) and BOTH AV/C commands counted as answered rather than unsupported (notif=1 ans=1 unsup=0 -- NEW-42 item B on the wire); the NEW-42 arrival instrument ran and bucketed at a pace only the peer knows -- the five gap buckets account for exactly pkts-1 intervals and the 50-120 ms band that straddles the peer's 100 ms pacing carries the bulk -- and the START pre-fill completed (primed=1); underruns, the servo's trim, every CONSUME-side field of bt_jit and every gap MAGNITUDE are SILICON claims -- QEMU has no audio clock, and its own clock invents a ~3.14 s inter-arrival on a peer that never pauses; the pairing window opens at boot and closes PAIRED on link-up, pair is REFUSED while streaming, the peer's injected drop opens a drop window it can SEE (inquiry scan 0x03 re-enabled) with PREPARE re-issued, and after forget the peer's re-page is accepted UNBONDED and pairs Just Works again -- the NEW-43 recovery without a reboot"
