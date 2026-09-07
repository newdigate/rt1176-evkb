#!/bin/sh
# run_qemu_soak.sh -- the [soak] gate for bt_tone_test (NEW-34 piece 5): the A2DP link
# SURVIVES REPETITION -- ten forced drops, ten auto-reconnects with the stored key on
# fresh handles, structural leak invariants held on every cycle.
#
# WHAT THIS PROVES
#   Built with -DM2_BT_SOAK=ON -DM2_BT_SOAK_PERIOD_MS=2000 -DM2_BT_SOAK_CYCLES=10
#   -DM2_BT_SOAK_RECONNECT_BOUND_MS=8000 (M2_BT_SOAK_RETRY_NOW stays default ON, so the
#   bounded gate does not wait out BtSession's own field retry timer), bt_tone_test forces
#   a RAW HCI_Disconnect every M2_BT_SOAK_PERIOD_MS while STREAMING and lets BtSession
#   auto-reconnect, against hci_peer.py's `soak` phase driving the SAME reconnect flow as
#   [reconnect]/[lifecycle] ten times over on one link socket: page (or inquiry the first
#   time), SSP/stored-key auth, AVDTP to STREAMING, then the peer answers the host's own
#   forced Disconnect and pages again on a FRESH handle.
#
#   This is not a re-run of [lifecycle]'s three legs -- it asserts what only holds up under
#   REPETITION, structural invariants [lifecycle] cannot see because it only ever tears the
#   link down twice:
#     * every one of the ten cycles reconnects -- both the firmware's own tally
#       (soak_done cycles=10 reconnects=10 fails=0) and the peer's independent count
#       (PEER-SOAK links=11 disconnects=10 streamed=11) must agree;
#     * the LOSS-TIME teardown witness l2_free_loss_min=5 (== L2cap::MAX_CHANNELS): inside
#       A2dpSource::tick(), a link loss runs m_l2.reset() BEFORE BtSession's onStream(false)
#       callback fires, so sampling L2cap::freeSlots() at that instant is the one place a
#       skipped reset is visible -- a skipped reset reads 2 (the two channels the address-of
#       constant test below explains), not 5.  ★ l2_free_base and l2_leak, sampled at
#       STREAMING entry instead, CANNOT see this: A2dpSource calls m_l2.begin() (a hard
#       memset of the channel table) unconditionally on every attempt, so the table is
#       always fresh by the time onStream(true) fires regardless of whether the PRIOR
#       teardown ran. That is why this driver samples freeSlots() a second time, at loss,
#       rather than trusting the STREAMING-entry sample alone.
#     * l2_free_base=2: of L2cap::MAX_CHANNELS=5, the slots non-reusable at STREAMING entry
#       are the AVDTP signalling channel, the AVDTP media channel, and the outbound SDP
#       channel A2dpSource opens on every outbound attempt and never itself disconnects
#       (cleared only by the next L2cap reset) -- 3 held, 2 free. The peer's own reverse-SDP
#       channel is CLOSED (reusable) by the time we sample.
#     * no ACL/CID handle leak across ten fresh handles (PEER-ACL-BAD-HANDLE,
#       PEER-DISCONNECT-BAD-HANDLE never fire) -- the same class of tripwire [lifecycle] and
#       [reconnect] use, run through ten cycles instead of one or two;
#     * no bond-table churn (bonds=1 after ten stored-key re-pages: key_ok=10 notified=1,
#       and PEER-SOAK's links == key_ok + notified == 11);
#     * per-cycle credit-stat RESET: L2cap::resetCreditStats() runs at every STREAMING
#       entry (onStreamCb). btout's own `packets=` ALSO resets on every begin() (a
#       reconnect), so it cannot serve as a "cumulative" reference to diff the final
#       bt_cred `sent=` against -- both restart at the same instant. What DOES distinguish
#       "reset every cycle" from "never reset" is a CEILING across the run: each ~2 s cycle
#       sends a few dozen to ~120 media packets (measured), so a max `sent=` over all
#       eleven links staying under a few hundred is only possible if the counter is really
#       being zeroed; a driver that forgot the reset accumulates past a thousand by the
#       last cycle (measured 1297 with the call removed -- see DEMONSTRATED RED (4));
#     * no HCI command-credit starvation (bt_hci starved=0) and no NCP over-return clamp
#       (bt_cred clamp=0) against this deterministic peer;
#     * clean media (RTP/SBC framing, no PEER-EXCEPTION/PEER-BAD-ARG) on all eleven links;
#     * STRAGGLERS bounded: the peer defers tearing down its own media reception to the
#       NEXT page rather than at the moment it answers our Disconnect (a real controller
#       keeps the ACL alive until it reports Disconnection_Complete back to us), so a few
#       media packets legitimately arrive in that ~50 ms window after the host has already
#       moved on -- stale_max<=8 (measured 4: this is the ~3-4 packets/cycle the host
#       legitimately sends before Disconnection_Complete lands, NOT a bound on total
#       stragglers, which vary cycle to cycle and are NOT pinned exactly here). Bound 8
#       (measured 4, three runs; stale_acl 33-37 across runs, ~12% spread): stale_run is the
#       number of media packets the guest emits before it PARSES Disconnection_Complete, i.e.
#       host-Python scheduling latency measured in guest time -- the same wall-clock/guest-time
#       skew behind the tree's load-sensitivity class, so one packet of margin (5) would flake;
#       the defect it guards against (a host that ignores Disconnection_Complete streams the
#       whole ~2 s cycle, 100+ packets) is still caught with ~12x margin at 8.
#
#   Every peer-side number is one the firmware cannot invent (see [lifecycle]/[reconnect]).
#
# ★ OWNS ITS OWN BUILD DIRECTORY, build-soak/, configured with M2_BT_RETRY_MS=3000 (same as
# [lifecycle]) so a cycle whose M2_BT_SOAK_RETRY_NOW path is somehow skipped still falls
# back to the session's own bounded retry inside this gate's timing rather than the field
# default. build/, build-media/ and build-lifecycle/ are never touched here.
#
# WHAT THIS DOES NOT PROVE
#   `credmin`/`starves`/`drops` DYNAMICS -- QEMU has no baud/ACL-scheduling pacing, so those
#   counters are timing noise here (piece 4's silicon-only claim); heap growth over hours,
#   real reconnect latency, or RF behaviour -- all silicon claims (transcript_hw_evkb.txt).
#   heap=0 on this vehicle is a "newlib's arena was never touched" tripwire, not proof
#   against a leak that would show as heap>0 -- asserted EXACTLY for that reason.
#
# TIMING
#   Ten 2 s periods plus reconnects, ~40-50 s wall. hci_peer.py's soak DEADLINE is 55 s,
#   below tools/qrun's 60 s QRUN_TIMEOUT -- on a hung run the peer announces PEER-DEADLINE
#   before QEMU is killed. Do NOT raise QRUN_TIMEOUT without raising the peer's deadline.
#   The gate runs ~49 s idle against the sweep runner's 120 s per-gate GATE_TIMEOUT; with
#   [media] (~50 s, same example) it is in the documented LOAD-SENSITIVITY class -- a red
#   under sweep load is re-run idle before it is believed. The post-peer wait below is capped
#   at 15 s (LOOP=60 at 0.25 s polls) so a stalled UART cannot push the script past ~70 s.
#
# DEMONSTRATED RED (2026-09-07): each mutation made in the named COMMITTED source file,
# build-soak/ rebuilt, this gate run against it, then reverted (`git -C <repo> checkout --
# <file>`) and rebuilt from clean source -- confirmed GREEN again after each revert, both
# repos clean between demos. The four demonstrated are (1) the skipped L2cap reset,
# (2) the btout.end() gap, (3) the stale handle, (4) resetCreditStats removed -- THREE of
# the four fail BY THE NAMED ASSERTION; (2) is a documented GATE GAP for the SAME reason
# [lifecycle]'s own demo (1) is one:
#   (1) M2Radio bt/A2dpSource.cpp tick(): the link-lost branch's `m_l2.reset();` removed --
#         FAIL: [soak] loss-time teardown witness wrong (expected l2_free_loss_min=5 == MAX_CHANNELS; a skipped L2cap reset reads 2): soak_done cycles=10 reconnects=10 fails=0 reconnect_ms_max=993 l2_free_base=2 l2_free_restream_min=2 l2_free_loss_min=2 l2_leak=0 submit_fails=0 bonds=1
#   (2) GATE GAP -- evkb bt_tone_test.cpp onStreamCb() else branch: `btout.end();`
#       commented out. The gate PASSES BIT-IDENTICALLY (peer tally unchanged at stale_max=4,
#       no PEER-ACL-BAD-HANDLE) -- exactly [lifecycle] demo (1)'s finding, for the same
#       reason: A2dpSource::tick() already runs `m_avdtp.reset(); m_l2.reset();` on link
#       loss BEFORE onStreamCb fires, and the next STREAMING entry re-begins btout on a
#       fresh channel regardless, so the app-level end() is redundant for every
#       ACL-observable behaviour this gate can see. The LIBRARY-level teardown IS covered --
#       see demo (1) above, which removes exactly that call and fails immediately.
#   (3) evkb bt_tone_test.cpp soakTick() SOAK_STREAM case: `uint16_t h = src.link().handle();`
#       changed to `uint16_t h = 0x0001;` (a stale, wrong handle on every forced disconnect) --
#         FAIL: [soak] not every cycle reconnected: soak_done cycles=10 reconnects=1 fails=9 reconnect_ms_max=986 l2_free_base=2 l2_free_restream_min=2 l2_free_loss_min=5 l2_leak=0 submit_fails=0 bonds=1
#       (the peer's own PEER-DISCONNECT-BAD-HANDLE tripwire ALSO fires here, nine times --
#       once per cycle after the first, since the handle only happens to be right on cycle
#       1 -- but this assertion is checked earlier in the script and reports first.)
#   (4) evkb bt_tone_test.cpp onStreamCb() streaming branch:
#       `src.l2().resetCreditStats();` commented out --
#         FAIL: [soak] credit stats not reset per cycle (max sent=1297 over the run -- expected each ~2 s cycle to stay under a few hundred)
#   Confirmed green again after each revert; `git -C M2Radio status` and `git status`
#   clean after all four.
#   The plan's demo (2) (BondTable upsert() re-broken to always insert -> bonds=4) was DROPPED
#   as unreachable: upsert() has ONE call site (M2Radio bt/BtLink.cpp, the
#   Link_Key_Notification handler) and the peer counts notified=1 for the whole run (every
#   re-page authenticates with the STORED key, so no new key is ever notified), so no mutation
#   there can move bonds. The live bond-churn pin is therefore the peer-counted notified=1
#   equality in the PEER-SOAK tally (links == key_ok + notified), not ` bonds=1$` alone.
set -e
DIR=$(cd "$(dirname "$0")" && pwd)
EVKB=$(cd "$DIR/../../.." && pwd)
QEMU="$EVKB/tools/qrun"
. "$EVKB/tools/gate-lib.sh"
gate_init
[ "$(gate_board)" = rt1176 ] || {
    echo "FAIL: this gate is rt1176-only (EVKB_BOARD=$(gate_board)) -- the M.2 socket is on the MIMXRT1170-EVKB"; exit 1; }

fail() { echo "FAIL: $*"; exit 1; }

# N > ~12 does not fit hci_peer.py's soak DEADLINE (55 s, see its own comment) under tools/qrun's
# 60 s QRUN_TIMEOUT -- do not raise N without raising both.
N=10; NP1=$((N + 1))

# This gate owns its build: M2_BT_TARGET_NAME + M2_BT_SOAK(_*) must be set here and match no
# other bt_tone_test build. Under the vacuity harness (GATE_VACUITY=1, no ARM toolchain
# guaranteed) skip the (re)build once the ELF already exists -- same convention as
# run_qemu_media.sh / run_qemu_lifecycle.sh.
BUILD_DIR="$DIR/build-soak"
ELF="$BUILD_DIR/bt_tone_test.elf"

if [ "${GATE_VACUITY:-}" = "1" ] && [ -x "$ELF" ]; then
    :
else
    mkdir -p "$BUILD_DIR"
    CONFIGURE_RC=0
    cmake -S "$DIR" -B "$BUILD_DIR" -DCMAKE_TOOLCHAIN_FILE="$EVKB/toolchain/rt1170-evkb.toolchain.cmake" \
          -DM2_BT_TARGET_NAME=FAKE-HEADSET-01 -DM2_BT_RETRY_MS=3000 -DM2_BT_SOAK=ON \
          -DM2_BT_SOAK_PERIOD_MS=2000 -DM2_BT_SOAK_CYCLES=$N -DM2_BT_SOAK_RECONNECT_BOUND_MS=8000 \
          >"$BUILD_DIR/configure.log" 2>&1 || CONFIGURE_RC=$?
    BUILD_RC=0
    if [ "$CONFIGURE_RC" -eq 0 ]; then
        cmake --build "$BUILD_DIR" >"$BUILD_DIR/build.log" 2>&1 || BUILD_RC=$?
    fi
    if [ "$CONFIGURE_RC" -ne 0 ] || [ "$BUILD_RC" -ne 0 ]; then
        fail "build-soak/ did not build -- see build-soak/configure.log / build.log"
    fi
fi

OUT="$BUILD_DIR/soak.uart"; DBG="$BUILD_DIR/soak.dbg"; RES="$BUILD_DIR/soak.peer"
rm -f "$OUT" "$DBG" "$RES"
SOCK="/tmp/m2soak_$$.sock"; rm -f "$SOCK"; gate_tmp "$SOCK"
"$QEMU" $(gate_qemu_machine) -kernel "$ELF" -display none $(gate_console "$OUT") \
    -serial unix:"$SOCK",server -d guest_errors -D "$DBG" &
P=$!; gate_pid $P
PEER_RC=0
python3 "$EVKB/examples/networking/m2_hci_probe/hci_peer.py" soak "$SOCK" "$N" > "$RES" 2>&1 || PEER_RC=$?
# The peer runs in the FOREGROUND and returns once all ten cycles have streamed (or it gave
# up). Then wait for the LAST line the gate parses -- never an earlier one (the m2_rx_demo[irq]
# mid-line reap is the standing lesson). soak_done fires the instant the tenth reconnect's
# handleAttemptEnd() runs, in the SAME tick that increments BtSession::Stats::links -- but the
# per-second "bt_link" heartbeat is on its own independent 1 s cadence, so the freshest one on
# disk at that instant can still read the PRIOR count (links=10) until its own next tick. Wait
# for that heartbeat to catch up too (it will, within ~1 s, and then holds steady -- no further
# drops are forced once cycles=10), or a reap here would read a real but stale snapshot. A
# failed peer means a failed run: poll briefly, not the full deadline.
LOOP=60; [ "$PEER_RC" -eq 0 ] || LOOP=40
for _ in $(seq 1 $LOOP); do
    [ -f "$OUT" ] && grep -qE "^soak_done cycles=$N " "$OUT" 2>/dev/null \
        && grep -qE "^bt_link links=$NP1 lost=$N " "$OUT" 2>/dev/null \
        && [ -f "$RES" ] && grep -q "^PEER-SOAK " "$RES" 2>/dev/null && break
    sleep 0.25
done
gate_reap $P
gate_require_capture "$OUT" "soak phase"
# CONSOLE.println() emits "\r\n" (Print::println); strip the \r so end-of-line ($) anchors
# below match the true end of the token rather than a trailing carriage return.
tr -d '\r' < "$OUT" > "$OUT.nocr" && mv "$OUT.nocr" "$OUT"
echo "==== captured UART ===="; cat "$OUT"
echo "==== peer ===="; cat "$RES"

grep -q "RT1176 BT tone test up" "$OUT" || fail "[soak] banner missing"

grep -qE "^soak period_ms=2000 cycles=$N bound_ms=8000 retry_now=1" "$OUT" \
    || fail "[soak] soak config line missing (not a soak build?)"

grep -q "^streaming by=inquiry " "$OUT" || fail "[soak] never streamed (first link by inquiry missing)"

grep -q "^soak_done cycles=$N " "$OUT" || fail "[soak] soak_done never printed with cycles=$N"
DONE=$(grep -m1 "^soak_done " "$OUT")
echo "$DONE" | grep -q " reconnects=$N fails=0 " \
    || fail "[soak] not every cycle reconnected: $DONE"
echo "$DONE" | grep -q " l2_free_loss_min=5 " \
    || fail "[soak] loss-time teardown witness wrong (expected l2_free_loss_min=5 == MAX_CHANNELS; a skipped L2cap reset reads 2): $DONE"
echo "$DONE" | grep -q " l2_leak=0 " \
    || fail "[soak] L2CAP slot leak: $DONE"
echo "$DONE" | grep -q " l2_free_base=2 " \
    || fail "[soak] L2CAP baseline moved (expected 2 of MAX_CHANNELS=5 free at STREAMING entry -- AVDTP signalling + media + the outbound SDP channel held; l2_leak is RELATIVE so a channel held from the first stream onward would hide in it): $DONE"
echo "$DONE" | grep -q " submit_fails=0 " \
    || fail "[soak] HCI submit failures: $DONE"
echo "$DONE" | grep -q " bonds=1$" \
    || fail "[soak] bond churn (expected bonds=1 after ten stored-key re-pages): $DONE"
echo "$DONE" | grep -qE " reconnect_ms_max=[1-9]" \
    || fail "[soak] reconnect_ms_max never populated: $DONE"

if grep -q "^soak_fail " "$OUT"; then
    fail "[soak] a cycle failed: $(grep -m1 '^soak_fail ' "$OUT")"
fi
if grep -q "^soak_drop_status=" "$OUT"; then
    fail "[soak] a forced disconnect was refused by the controller: $(grep -m1 '^soak_drop_status=' "$OUT")"
fi

grep -qE "^bt_link links=$NP1 lost=$N " "$OUT" \
    || fail "[soak] final bt_link wrong (expected links=$NP1 lost=$N)"

# --- credit stats reset per cycle. btout's own "packets=" ALSO resets at every begin() (a
# link reconnect), so it cannot serve as a "cumulative" reference to compare the final
# bt_cred sent= against -- both counters restart at the same instant. What DOES distinguish
# "reset every cycle" from "never reset" is the CEILING across the whole run: each ~2 s cycle
# sends on the order of a few dozen to ~120 media packets (measured), so if resetCreditStats()
# were never called, sent= would keep climbing across all eleven links and comfortably clear
# several hundred by the last cycle. Take the MAX sent= seen over the whole run, not the final
# line alone -- the very last sample can land mid-reconnect with a transient credit stall
# (this run's own tail shows starves=1 at that instant), which is exactly the "QEMU credit
# DYNAMICS are timing noise" case this gate does NOT assert on; the ceiling is unaffected by
# that noise. ---
MAXSENT=$(grep "^bt_cred " "$OUT" | sed -E 's/.*sent=([0-9]+).*/\1/' | sort -n | tail -1)
[ -n "$MAXSENT" ] || fail "[soak] no bt_cred line found"
[ "$MAXSENT" -lt 300 ] \
    || fail "[soak] credit stats not reset per cycle (max sent=$MAXSENT over the run -- expected each ~2 s cycle to stay under a few hundred)"
# Structural companion: sent= must FALL once per reconnect (the per-attempt reset) -- immune to period/packet-rate changes.
DROPS=$(grep "^bt_cred " "$OUT" | sed -E 's/.*sent=([0-9]+).*/\1/' | awk 'NR>1 && $1<p {n++} {p=$1} END{print n+0}')
[ "${DROPS:-0}" -ge 9 ] \
    || fail "[soak] credit stats never reset (sent= fell $DROPS times over 10 reconnects; expected >= 9)"

# Require the line-final field on each extraction, rejecting a line torn by a reap that lands
# mid-print (the m2_rx_demo[irq] lesson): bt_hci and bt_cred both print AFTER the bt_link line
# the wait loop keys on, so a reap timed between "bt_link ..." and the next heartbeat's tail can
# catch either mid-write.
LASTHCI=$(grep -E "^bt_hci .* credmin=[0-9]+\$" "$OUT" | tail -1)
[ -n "$LASTHCI" ] || fail "[soak] no complete bt_hci line captured (torn at reap?)"
echo "$LASTHCI" | grep -q " starved=0 " || fail "[soak] HCI command-credit starvation: $LASTHCI"
LASTCRED=$(grep -E "^bt_cred .* clamp=[0-9]+\$" "$OUT" | tail -1)
[ -n "$LASTCRED" ] || fail "[soak] no complete bt_cred line captured (torn at reap?)"
echo "$LASTCRED" | grep -q " clamp=0$" || fail "[soak] NCP clamp hit: $LASTCRED"

LASTSOAK=$(grep "^bt_soak " "$OUT" | tail -1)
echo "$LASTSOAK" | grep -q " heap=0 " \
    || fail "[soak] heap allocation appeared (this vehicle never allocates; heap=0 is a nothing-ever-allocated tripwire): $LASTSOAK"
echo "$LASTSOAK" | grep -q " l2_free_loss_min=5 " \
    || fail "[soak] loss-time witness wrong on the last bt_soak line: $LASTSOAK"

# --- peer tripwires (values the firmware cannot invent) ---
for T in PEER-ACL-BAD-HANDLE PEER-ACL-UNKNOWN-CID PEER-DISCONNECT-BAD-HANDLE PEER-KEY-MISMATCH PEER-AUTH-NO-LINK PEER-EXCEPTION PEER-BAD-ARG; do
    if grep -q "^$T" "$RES"; then fail "[soak] peer tripwire: $(grep -m1 "^$T" "$RES")"; fi
done

grep -q "^PEER-CONNECTED phase=soak" "$RES" || fail "[soak] the fake controller never attached to LPUART2"

# --- the peer tally: every number counted on the other side of the socket. ---
PEER_SOAK_PAT="^PEER-SOAK links=$NP1 disconnects=$N streamed=$NP1 key_ok=$N notified=1 badmedia=0 stale_acl=[0-9]+ stale_max=[0-9]+\$"
grep -qE "$PEER_SOAK_PAT" "$RES" \
    || fail "[soak] peer tally wrong: $(grep -m1 '^PEER-SOAK' "$RES")"
SM=$(grep -m1 "^PEER-SOAK" "$RES" | sed -E 's/.* stale_max=([0-9]+).*/\1/')
[ "$SM" -le 8 ] \
    || fail "[soak] stragglers exceeded the bound (stale_max=$SM > 8; media kept flowing after the host should have torn down): $(grep -m1 '^PEER-SOAK' "$RES")"

# --- infrastructure LAST (the peer waits out its deadline on any firmware failure, so
# these would mask every named failure above if placed first). ---
if grep -q "^PEER-EOF" "$RES";      then fail "[soak] QEMU closed the socket under the peer -- infrastructure, not firmware: $(grep -m1 '^PEER-EOF' "$RES")"; fi
if grep -q "^PEER-DEADLINE" "$RES"; then fail "[soak] the peer gave up at its deadline (sweep load? raise DEADLINE with QRUN_TIMEOUT): $(grep -m1 '^PEER-DEADLINE' "$RES")"; fi
if grep -q "^Traceback (most recent call last)" "$RES"; then fail "[soak] the fake controller crashed -- peer bug, not firmware: $(grep -A3 '^Traceback' "$RES" | tail -1)"; fi
[ "$PEER_RC" -eq 0 ] || fail "[soak] peer exited $PEER_RC"

echo "PASS: A2DP link survives repetition -- 10 forced drops, 10 auto-reconnects on fresh handles with the stored key, teardown witnessed at every loss (l2_free_loss_min=5), no slot/handle/bond leak, clean media on all 11 links, credit stats reset per cycle, stragglers bounded"
