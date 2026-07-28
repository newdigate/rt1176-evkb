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
# 1.8 MB extmem framebuffer + LCDIFv2 config), plus margin.  Barely any of it is
# for the touch script -- the firmware polls only until the part has published
# its second instant, which is 20 ms of guest time after the first, and then
# stops.  The remaining 23 steps are left unread on purpose; consuming the whole
# scripted path is a later stage's job.
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
echo "PASS: RK055 touch T1+T2+T3a (GT911 reset + INT-level address latch at 0x5D"
echo "      + product ID + the 186-byte config blob read and checksummed,"
echo "      with the config space untouched; and a polled contact read whose"
echo "      mandatory status clear let the part go on to publish the release)"
