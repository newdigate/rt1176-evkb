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
# 1.8 MB extmem framebuffer + LCDIFv2 config), plus margin.  NOT for the touch
# script: nothing reads contacts at this stage, so the model publishes step 0
# and then stalls for ever waiting for the mandatory status clear that never
# comes.  (The full script is 25 steps at 20 ms = 500 ms, and only Task 5's
# read() will ever let it advance past the first.)
sleep 10; kill $P 2>/dev/null; wait $P 2>/dev/null || true
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
# the ID is 4 bytes, a single loop iteration.  Nothing reads more than 32 bytes
# until Task 4's 186-byte config blob.
grep -q "GT911_OK" "$OUT" || { echo "FAIL: product ID"; exit 1; }
# Design 6.1 specifies the line as "GT911_OK  ID=911".  Asserted separately so
# the ID cannot quietly stop being echoed.  Note what this does NOT add: the
# driver's own byte assembly is already covered, because a byte-order slip there
# would fail the DEVICE_ID equality check and never reach GT911_OK.  What it
# covers is the EXAMPLE's rendering of those bytes back to text, and the token's
# continued presence in the transcript.
grep -q "ID=911" "$OUT" || { echo "FAIL: product ID not echoed as ID=911"; exit 1; }
# The driver must NEVER write the configuration space.  NXP's own driver
# rewrites all 186 bytes when the stored point count or trigger mode differ,
# and warns that a wrong write breaks the part.  The QEMU model logs any such
# write as a guest error; this turns the design decision into an assertion.
if grep -q "gt911: guest wrote config" "$DIR/rk055_touch.dbg" 2>/dev/null; then
    echo "FAIL: firmware wrote the GT911 configuration space"; exit 1
fi
echo "PASS: RK055 touch T1 (GT911 reset + INT-level address latch at 0x5D"
echo "      + product ID, with the config space untouched)"
