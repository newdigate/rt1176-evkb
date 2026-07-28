#!/bin/sh
set -e
QEMU=~/Development/rt1170/evkb/tools/qrun
DIR=$(cd "$(dirname "$0")" && pwd)
. ~/Development/rt1170/evkb/tools/gate-lib.sh
gate_init
ELF="$DIR/build/rk055_touch_test.elf"; OUT="$DIR/rk055_touch.uart"
rm -f "$OUT" "$DIR/rk055_touch.dbg"
"$QEMU" -M mimxrt1170-evk -global fsl-imxrt1170.boot-xip=on -kernel "$ELF" \
    -display none -serial file:"$OUT" -d guest_errors -D "$DIR/rk055_touch.dbg" &
P=$!; gate_pid $P
# 10s: the 8s the sibling display gates use for a cold binary (SEMC init + a
# 1.8 MB extmem framebuffer + LCDIFv2 config), plus margin.  The three phases
# consume 22 of the model's 25 scripted instants at 20 ms of guest time each,
# which is 440 ms on top of that -- measured end to end at 1.09 s of guest time
# and 2.2 s of wall, so this window carries a 4.5x margin.  (The three instants
# never reached are the SECOND and THIRD two-contact holds and the closing
# release -- phase 3 stops at the first hold that qualifies.  The drag's last
# two samples and its release are NOT left over: phase 3 opens by consuming
# them, because it discards any leading instant carrying fewer than two
# contacts.)
#
# WHAT THIS WINDOW CANNOT DO, and it is a deliberate trade, not an oversight:
# guest time runs at roughly half wall here, so a phase that genuinely times out
# burns its PHASE_TIMEOUT_MS (60 s guest, ~2 minutes of wall) and is killed
# here long before its named *_FAIL line reaches the UART.  The gate still goes
# red -- on the missing token, from the assertions below -- and the partial
# transcript still shows which phase was reached and what the basic read path
# was doing.  The alternative is a phase timeout short enough for QEMU, which
# would mean a bench operator losing a target because they took too long
# reading the prompt.  See PHASE_TIMEOUT_MS in the .cpp.
# `|| true` on the kill, and it is load-bearing under `set -e`: if QEMU has
# already exited (bad binary, instant crash) the kill fails, and without this
# the script dies HERE -- exit 1 with no message at all, before any assertion
# runs.  That is the second shape of "the run never happened", and it has to
# reach the named FAIL below like the first does.
sleep 10; kill $P 2>/dev/null || true; wait $P 2>/dev/null || true
# The capture has to EXIST before any grep of it means anything.  Without this,
# a QEMU that died before opening the serial file failed as `cat: no such file`
# -- a non-zero exit, so never a false green, but a message that points at
# nothing.  Two runs in five died this way during bring-up.
[ -s "$OUT" ] || { echo "FAIL: no UART capture at $OUT -- QEMU produced no serial output (did it start?)"; exit 1; }
echo "==== captured ===="; cat "$OUT"
# The panel has to be up before touch means anything -- targets have to be
# visible.  Re-asserted here rather than assumed from rk055_panel_test: a
# regression there would otherwise surface as a baffling touch failure.
grep -q "PANEL_OK" "$OUT" || { echo "FAIL: panel bring-up"; exit 1; }
# I2C_OK -- the GT911 ACKed at the address it LATCHED.  In QEMU this proves the
# reset/INT sequence is well-formed: the virtual part has no address at all
# until a genuine reset pulse, and takes 0x14 rather than 0x5D if INT was not
# driven low first.  On silicon it proves the real part answers.
grep -q "I2C_OK"   "$OUT" || { echo "FAIL: no ACK from the touch controller"; exit 1; }
# CORROBORATING, not an independent check.  address() returns 0 unless begin()
# SUCCEEDED, so this line can only disagree with I2C_OK/GT911_OK by printing
# 0x00 -- it cannot catch a wrong latch that those two missed.  I2C_OK carries
# the real load: the model has no address at all until a well-formed reset
# pulse, and answers at 0x14 rather than 0x5D unless INT was driven low, so
# anything ACKing at all means the latch went the way we intended.  The line is
# kept because a human reading a hardware transcript wants the address in it.
grep -q "ADDR=0x5D" "$OUT" || { echo "FAIL: no successful bring-up at 0x5D"; exit 1; }
# GT911_OK -- the product ID reads "911\0".  QEMU returns a constant here, so
# this proves our 16-bit big-endian sub-address encoding; only silicon proves
# the real part's identity.  It does NOT exercise the multi-chunk read path:
# the ID is 4 bytes, a single loop iteration.  The config read below is what
# covers chunking.
grep -q "GT911_OK" "$OUT" || { echo "FAIL: product ID"; exit 1; }
# Design 6.1 specifies the line as "GT911_OK  ID=911".  Asserted separately so
# the ID cannot quietly stop being echoed.  Note what this does NOT add: the
# driver's own byte assembly is already covered, because a byte-order slip there
# would fail the DEVICE_ID equality check and never reach GT911_OK.  What it
# covers is the EXAMPLE's rendering of those bytes back to text, and the token's
# continued presence in the transcript.
grep -q "ID=911" "$OUT" || { echo "FAIL: product ID not echoed as ID=911"; exit 1; }
# CFG_OK -- the 186-byte configuration blob read back with a valid checksum.
# The checksum matters beyond "did the read work": the resolution we are about
# to scale every coordinate by lives inside this blob, so a blob we cannot
# trust is a scale factor we cannot trust.
#
# It is ALSO this gate's only proof that the chunked read path works.  186 bytes
# at 32 per chunk is five full chunks plus a 26-byte remainder, and every chunk
# restates its own sub-address -- so a dropped chunk, a short read, or a chunk
# fetched from the wrong sub-address lands wrong bytes in the buffer and the
# 184-byte sum stops matching byte 184.  Nothing before this read more than 4
# bytes at a time.
#
# What it does NOT catch, so nobody reads more into a green run than is there:
# two whole chunks TRANSPOSED.  The checksum is a sum and addition commutes, so
# exchanging two equal-length blocks inside bytes 0..183 leaves it identical --
# unfixable at this layer, by any model.  Enumerated: 17 of 298 injected
# chunking faults survive, 10 of them transpositions.
#
# The rest of the claim is only true because the MODEL cooperates: it fills the
# blob's unspecified bytes with a position-dependent pattern, verified so that
# no aligned chunk sums to zero and no two chunk sums collide.  While those
# bytes were zeros this assertion was decorative -- a dropped middle chunk
# contributed exactly what it should have (nothing) and the gate stayed green.
# That property is now pinned by an abort() in the model itself, so flattening
# the filler fails loudly instead of silently retiring this line.
grep -q "CFG_OK" "$OUT" || { echo "FAIL: config blob"; exit 1; }
# RES/POINTS are RECORDED, never asserted.  A real GT911 reports whatever it
# reports and it is NOT guaranteed to equal the panel's 720x1280; asserting
# equality would fail on a legitimate panel variant.  A wrong resolution
# already has a natural detector -- the drawn targets become unhittable -- so
# these lines exist to put the truth in the transcript.
grep -q "RES=" "$OUT"    || { echo "FAIL: resolution not reported"; exit 1; }
grep -q "POINTS=" "$OUT" || { echo "FAIL: contact count not reported"; exit 1; }
# --- the coordinate layer --------------------------------------------------
# FIRST_TOUCH -- read() returned at least one contact.  The trailing SPACE in
# the pattern is load-bearing: the failure token is FIRST_TOUCH_NONE, and
# without the space it would match this assertion and turn a driver that never
# saw a contact into a pass.
#
# What it proves is modest and deliberately so: that the status read, the
# contact-array read and the point decode all work end to end.  The COORDINATES
# in it are recorded, never asserted -- the model's scripted path and any
# on-screen target are both percentages of the same resolution, so model and
# firmware share an assumption about which corner is (0,0), and no QEMU run can
# falsify a shared assumption.  Orientation is a hardware finding.
grep -q "FIRST_TOUCH " "$OUT" || { echo "FAIL: read() never reported a contact"; exit 1; }
# TOUCH_ADVANCED -- and THIS is the assertion this stage exists for.
#
# The GT911 republishes a coordinate buffer only after the host acknowledges the
# previous one by writing 0 to 0x814E.  A read() that omits that write sees the
# same contact for ever, on silicon and in the model alike: imxrt_gt911_tick()
# refuses to advance the script while the ready bit is still set, so the second
# instant never comes into existence.  FIRST_TOUCH above passes perfectly well
# against a driver in that state; this line is what catches it.
#
# The firmware emits this token only on GT911::Poll::Released -- a buffer the
# part PUBLISHED carrying zero contacts, the release after the tap.  That state
# cannot be reached without the acknowledgement, because an unacknowledged part
# keeps re-serving the same ONE-contact buffer.
#
# NEGATIVE-TESTED TWICE, and it needed both.  Two earlier versions of this
# assertion stayed GREEN against a driver that never cleared the status
# register:
#
#   1. "a fresh buffer seen twice" -- a wedged part reports the ready bit set on
#      every poll for ever, so this was true of a completely stuck part.
#   2. "fresh AND zero contacts" -- closed case 1 for a clean bus, but with the
#      clear deleted AND a contact-read fault injected, the driver's freshness
#      flag was set before the contact read and so was inherited by the fault
#      path, whose zero count then looked exactly like a release.  QEMU never
#      faults I2C, so nothing here could see it; a bus glitch on silicon
#      produces precisely that state.
#
# Both are now unrepresentable rather than merely fixed: read() returns a state,
# and Failed is not Released.  Verified in both directions -- the compound
# injection goes red against the current driver and green against the previous
# one.  Do not reduce this to a test on a contact count.
grep -q "TOUCH_ADVANCED" "$OUT" || { echo "FAIL: the part never published a second buffer -- read() did not clear the status register at 0x814E"; exit 1; }
# --- the three phases ------------------------------------------------------
# Phase 1 -- five drawn targets, hit IN ORDER.  This is the ONLY assertion in
# the whole gate that proves the coordinate-to-display MAPPING: a flipped,
# swapped or mis-scaled axis makes the sequence impossible to complete, because
# the operator is touching where the graphic actually is.  A stuck or
# fabricated coordinate satisfies at most one target -- the firmware carries a
# static_assert that the five acceptance boxes are pairwise disjoint, so that
# claim is enforced at compile time rather than eyeballed off the geometry.
#
# ORIENTATION IS HARDWARE-ONLY BY CONSTRUCTION, AND THIS RUN PROVES NONE OF IT.
# The QEMU model places its scripted contacts at the same percentages the
# firmware places its targets at (qemu2 hw/i2c/imxrt_gt911.c, gt911_script[]),
# so model and firmware SHARE an assumption about which corner is (0,0) and
# which way each axis runs.  A shared assumption cannot be falsified by the two
# parties that share it: this sequence would complete just as happily with both
# axes mirrored.  What a green run here does prove is the DECODE and the phase
# state machine -- that the contacts came back in the order the part published
# them, distinct and correctly scaled by the resolution the part reported.
# Only a finger on real glass settles the mapping; that is Task 7.
grep -q "TARGETS_OK" "$OUT" || { echo "FAIL: target sequence"; exit 1; }
# All five, individually.  TARGETS_OK alone would still be true of a firmware
# that had quietly dropped a target from the list, and the per-target lines are
# what a human reads on the bench.
for t in 1 2 3 4 5; do
    grep -q "TARGET $t/5 HIT" "$OUT" || { echo "FAIL: target $t of 5 never hit"; exit 1; }
done
# The per-target RELEASE WAIT, which nothing else can speak for.  TOUCH_ADVANCED
# is a once-ever token that ANY release in the 25-instant script satisfies,
# including the drag's, and the release instant is consumed either way -- by the
# wait, or by the next target's hit loop discarding non-Contacts polls.  So with
# the wait deleted, every other line of this transcript was byte-identical:
# five HITs, BUFFERS=22, INT_EDGES=22, PASS.  Measured, not theorised.
#
# The clause is worth an assertion of its own because on hardware it forces a
# LIFT between targets: an operator cannot drag one finger from target to
# target without it ever leaving the glass, which would turn five independent
# hits into one continuous stroke.
grep -q "^TARGET_RELEASES=5$" "$OUT" || { echo "FAIL: phase 1 saw $(sed -n 's/^TARGET_RELEASES=//p' "$OUT" | tail -1) published releases from its per-target wait, expected 5"; exit 1; }
# Phase 2 -- a directional drag. Catches a stuck value and a mirrored axis
# independently of phase 1, which a single tap cannot: EIGHT samples that each
# STRICTLY ADVANCE in +x, all carrying ONE track id, across >= 60% of the axis.
#
# Both clauses were added after they were shown to be needed.  Without the
# strict advance, a repeated x was not a backwards step and so counted as a
# sample with the span untouched: {72,72,72,72,72,72,72,648} scored
# "span=80% samples=8" out of TWO distinct coordinates.  Without the track-id
# clause, points[0] is the LOWEST track id currently down, so a contact parked
# at the left edge plus a second at the right edge lets points[0] jump 80% of
# the axis the moment the first is lifted -- two taps, no drag, on the one
# phase whose job is to prove directionality independently of phase 1.
grep -q "SWIPE_OK"   "$OUT" || { echo "FAIL: swipe"; exit 1; }
# WHERE PHASE 2 STOPS IN THE SCRIPT, pinned -- and this line, not BUFFERS below,
# is what pins it.  BUFFERS counts where phase 3 ENDS, which is scripted instant
# 21 no matter how many instants phase 2 left behind, so it is identical for
# every SWIPE_MIN_SAMPLES in 2..10: measured, 8 and 10 both gave BUFFERS=22 and
# a PASS.  The span and sample count in SWIPE_OK are the only observable
# statement of where the drag was satisfied.
#
# 62% / 8 samples is a CONSEQUENCE of the thresholds meeting the model's fixed
# 10-sample drag (10%..90% in 9 steps): the span test is checked per sample, so
# it passes at sample 8, at 73% of the axis, 62% along from the 10% start.
# Pinning it means a threshold change cannot quietly slide the phase boundary --
# it has to come here and be re-reasoned.  id=0 is the model's track id for the
# drag; a different one would mean the run followed a different contact.
grep -q "^SWIPE_OK dir=+x span=62% samples=8 id=0$" "$OUT" || { echo "FAIL: phase 2 stopped somewhere unexpected in the script: $(grep '^SWIPE_OK' "$OUT" | tail -1)"; exit 1; }
# Phase 3 -- two contacts with DISTINCT track ids. Exercises the contact array
# and the count field, which phases 1 and 2 only ever touch for points[0].
grep -q "MULTI_OK"   "$OUT" || { echo "FAIL: two-finger"; exit 1; }
# --- the INT line ----------------------------------------------------------
# INT_EDGES counts rising edges seen by an attachInterrupt() on D6 (GPIO2_IO31);
# BUFFERS counts the fresh buffers read() consumed while that interrupt was
# attached.  Both are printed, and the RELATION between them is what is
# asserted -- a bare "INT_EDGES is present" would pass against a counter wired
# to nothing.
#
# The relation, and why it is defensible on both sides: the part pulses INT
# once per published instant, and read() acknowledges each published instant
# exactly once, so over any interval the two counts are the same number.  Two
# documented slacks, and no more:
#
#   +1            an instant published after the last poll and before the print.
#   +POLL_FAILS   a Failed poll MAY have consumed a buffer without BUFFERS
#                 counting it -- read() acknowledges the part even when the
#                 contact read failed, so a point-read failure whose status
#                 clear succeeded consumes its buffer and has its edge counted,
#                 while a status-read failure consumes nothing, and the
#                 returned state does not distinguish them.  Hardware-only.
#
# Hence BUFFERS <= INT_EDGES <= BUFFERS + POLL_FAILS + 1.  POLL_FAILS is
# asserted 0 above, so under QEMU this collapses to the tight relation rather
# than granting slack nothing has bounded.
#
# What each direction catches.  INT_EDGES < BUFFERS means the pin is not
# delivering edges at all (wrong pin, wrong port, no CM7 interrupt line on a
# fast-GPIO alias) -- and this is this project's FIRST exercise of a GPIO2
# interrupt; irq_attach_test only ever proved GPIO3.  INT_EDGES > BUFFERS+1
# means the line moved when nothing was published, which on silicon is the
# chatter predicted by HW-TO-CONFIRM item 2 in gt911.h: begin() leaves D6 a
# plain INPUT with no pull, and nobody has confirmed a board pull-up on the
# RevC3 CTP_INT net.  QEMU cannot show that -- the model drives the line both
# ways -- so a green run here does NOT retire that item.
#
# The read path stays POLLED.  This counter is corroboration, never control
# flow; the ISR does nothing but increment.
# HOW MUCH OF THE SCRIPT THE RUN CONSUMED IN TOTAL, pinned.
#
# BE EXACT ABOUT WHAT THIS DOES AND DOES NOT CATCH, because the obvious reading
# is wrong and was measured to be wrong: it does NOT pin the phase-2/3 boundary.
# Phase 3 silently skips any number of leading n<2 instants and always stops at
# the FIRST two-contact instant, number 21, so BUFFERS is 22 for every
# SWIPE_MIN_SAMPLES in 2..10 alike.  The SWIPE_OK line above is what pins that
# boundary.  What this catches is the total: a phase 1 that consumed a different
# number of instants, or a phase 3 that needed more than one qualifying buffer
# and so ate into the two spare two-contact instants -- past which phase 3 runs
# off the end of the 25-instant script and fails as a bare 10-second harness
# kill with no MULTI_FAIL line at all.
#
# 22 is 10 for phase 1 (five taps and five releases), 8 for phase 2 (the span
# test is checked per sample, so it is satisfied at drag sample 8 of 10), and 4
# for phase 3 -- and note WHAT those four are: the drag's last two samples, the
# drag's release, and then the first two-contact instant.  Phase 3 opens by
# swallowing three instants belonging to phase 2's gesture.  The three the run
# never reaches are the SECOND and THIRD two-contact holds and the closing
# release.
grep -q "^BUFFERS=22$" "$OUT" || { echo "FAIL: the phases consumed $(sed -n 's/^BUFFERS=//p' "$OUT" | tail -1) of the model's 25 scripted instants, expected 22 -- a threshold moved and the phase boundaries have shifted"; exit 1; }
# POLL_FAILS bounds the slack the INT relation below is allowed to grant: a
# Failed poll MAY have consumed a buffer BUFFERS never counted (read()
# acknowledges the part even when the contact read failed), so the relation has
# to widen by one per failure.  QEMU cannot fault I2C at all, so anything but 0
# here means something unmodelled happened and the INT band is no longer tight.
grep -q "^POLL_FAILS=0$" "$OUT" || { echo "FAIL: $(sed -n 's/^POLL_FAILS=//p' "$OUT" | tail -1) failed poll(s) -- QEMU cannot fault I2C, so this is unmodelled behaviour"; exit 1; }
# SWITCH1 is RECORDED, never asserted: the model synthesises this byte with the
# rest of the blob, so only silicon says what this panel was programmed with.
# It is required to be present because Task 7 reads the transcript with no shell
# gate beside it, and bits 1:0 (the INT trigger mode) are the most likely cause
# of a hardware INT_MISMATCH against the RISING edge the firmware asks for.
grep -q "^SWITCH1=0x" "$OUT" || { echo "FAIL: INT trigger mode not reported"; exit 1; }
EDGES=$(sed -n 's/^INT_EDGES=\([0-9][0-9]*\).*$/\1/p' "$OUT" | tail -1)
BUFS=$(sed -n 's/^BUFFERS=\([0-9][0-9]*\).*$/\1/p' "$OUT" | tail -1)
[ -n "$EDGES" ] && [ -n "$BUFS" ] || { echo "FAIL: INT_EDGES/BUFFERS not reported"; exit 1; }
[ "$EDGES" -gt 0 ] || { echo "FAIL: INT_EDGES=0 -- the INT pin delivered no rising edge for any of the $BUFS buffers the poll path consumed"; exit 1; }
FAILS=$(sed -n 's/^POLL_FAILS=\([0-9][0-9]*\).*$/\1/p' "$OUT" | tail -1)
[ -n "$FAILS" ] || { echo "FAIL: POLL_FAILS not reported"; exit 1; }
[ "$EDGES" -ge "$BUFS" ] && [ "$EDGES" -le "$((BUFS + FAILS + 1))" ] || { echo "FAIL: INT_EDGES=$EDGES against BUFFERS=$BUFS POLL_FAILS=$FAILS -- expected $BUFS..$((BUFS + FAILS + 1)); fewer means edges are being missed, more means more edges than published buffers and failed polls can account for"; exit 1; }
# TOUCH_OK is the firmware's own verdict on everything above -- all three
# phases AND the INT relation, which it recomputes rather than leaving to this
# script.  That overlap is deliberate and is documented in reportInt(): Task 7
# reads the transcript off real glass with no shell gate near it, so a run that
# printed TOUCH_OK above an INT_EDGES=0 would be read as a pass.  Keep both.
grep -q "TOUCH_OK"   "$OUT" || { echo "FAIL: gate did not complete"; exit 1; }
# The driver must NEVER write the configuration space.  NXP's own driver
# rewrites all 186 bytes when the stored point count or trigger mode differ,
# and warns that a wrong write breaks the part.  The QEMU model logs any such
# write as a guest error; this turns the design decision into an assertion.
#
# The log must EXIST first.  `grep -q ... 2>/dev/null` inside an `if` scores a
# missing or empty file as "no write happened", so the assertion would pass
# most loudly in the one case where it knows nothing -- absent evidence read as
# evidence of absence.
#
# Belt and braces only, and be honest about why: tools/qrun creates this file
# with `: > "$LOG"` BEFORE it execs QEMU, so the file existing proves qrun ran,
# not that QEMU did -- a run that died at startup leaves a populated .dbg beside
# an empty .uart.  The `-s "$OUT"` check above is what actually catches that
# shape; this one catches a .dbg that never appeared at all.
[ -f "$DIR/rk055_touch.dbg" ] || { echo "FAIL: no guest-error log -- the config-write assertion cannot be checked"; exit 1; }
if grep -q "gt911: guest wrote config" "$DIR/rk055_touch.dbg"; then
    echo "FAIL: firmware wrote the GT911 configuration space"; exit 1
fi
echo "PASS: RK055 touch T1+T2+T3 (GT911 reset + INT-level address latch at 0x5D"
echo "      + product ID + the 186-byte config blob read and checksummed, with"
echo "      the config space untouched; a polled contact read whose mandatory"
echo "      status clear let the part go on to publish the release; and all"
echo "      three phases -- five targets in order each with its published"
echo "      release, a single-track +x drag of strictly advancing samples,"
echo "      and two contacts with distinct ids -- with INT edges matching the"
echo "      buffers consumed."
echo "      NOT proved here: the coordinate-to-display mapping.  See TARGETS_OK.)"
