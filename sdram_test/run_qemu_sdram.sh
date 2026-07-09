#!/bin/sh
set -e
QEMU=~/Development/rt1170/evkb/tools/qrun
DIR=$(cd "$(dirname "$0")" && pwd)
. ~/Development/rt1170/evkb/tools/gate-lib.sh
gate_init
ELF="$DIR/build/sdram_test.elf"; OUT="$DIR/sdram.uart"
rm -f "$OUT"
# Faithful window: the SDRAM at 0x80000000 is created DISABLED in the SEMC model
# and only enabled when the guest's semc_sdram_init() issues the SDRAM Mode-Set
# IP command during startup. A normal boot therefore lights the window up before
# setup() runs the memory test. No -icount: a plain memory test isn't timing-sensitive.
"$QEMU" -M mimxrt1170-evk -global fsl-imxrt1170.boot-xip=on -kernel "$ELF" \
    -display none -serial file:"$OUT" -d guest_errors -D "$DIR/sdram.dbg" &
P=$!; gate_pid $P; sleep 6; kill $P 2>/dev/null; wait $P 2>/dev/null || true
echo "==== captured ===="; cat "$OUT"
grep -q "SDRAM_DATA=PASS" "$OUT" || { echo "FAIL: data-line test"; exit 1; }
grep -q "SDRAM_ADDR=PASS" "$OUT" || { echo "FAIL: address-line test"; exit 1; }
grep -q "SDRAM_HOLD=PASS" "$OUT" || { echo "FAIL: refresh-retention test"; exit 1; }
grep -q "SDRAM_TEST=PASS" "$OUT" || { echo "FAIL: overall"; exit 1; }
echo "PASS: SEMC SDRAM verified (data-line + address-line over full 64 MB, faithful window)"
