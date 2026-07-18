#!/bin/sh
# QEMU gate for Phase-3.1: the CM4 self-configures LPSPI1 and runs a polled
# self-loopback; the CM7 reports the observations over the MU on LPUART1.
# NOTE: QEMU's ssi-loopback child echoes on CR.MEN alone (ignores clock/pins),
# so a green QEMU run proves the register/transfer SEQUENCE only — the HW jumper
# run is what proves the CM4's clock-gating + pin-mux (see README / spec).
set -e
QEMU=~/Development/rt1170/evkb/tools/qrun
DIR=$(cd "$(dirname "$0")" && pwd)
. ~/Development/rt1170/evkb/tools/gate-lib.sh
gate_init
ELF="$DIR/build/cm4_spi_test.elf"
OUT="$DIR/cm4_spi.uart"
rm -f "$OUT"
"$QEMU" -M mimxrt1170-evk -global fsl-imxrt1170.boot-xip=on -kernel "$ELF" \
    -display none -serial file:"$OUT" -d guest_errors -D "$DIR/cm4_spi.dbg" &
P=$!; gate_pid $P
for _ in $(seq 1 40); do
    [ -f "$OUT" ] && grep -q "CM4SPI-DONE" "$OUT" 2>/dev/null && break
    sleep 0.25
done
kill $P 2>/dev/null; wait $P 2>/dev/null || true

echo "==== captured UART ===="; [ -f "$OUT" ] && cat "$OUT" || echo "(no UART output)"

fail=0
check() {
    if grep -q "^$1" "$OUT"; then echo "PASS: $1"; else echo "FAIL: expected $1"; fail=1; fi
}
grep -q "CM4SPI-GATE v1" "$OUT" || { echo "FAIL: banner missing"; exit 1; }
check "cr=00000001"       # LPSPI CR.MEN set (block enabled)
check "cfgr1=00000001"    # CFGR1.MASTER
check "a=000000A5"        # loopback echoed 0xA5
check "b=0000003C"        # loopback echoed 0x3C
check "w=0000BEEF"        # transfer16 echoed 0xBEEF
check "buf=DEADBEEF"      # 4-byte buffer echoed
check "rxok=00000001"     # all loopback bytes matched
check "SPI_CM4=PASS"      # verdict
# lpcg= / croot= are printed for HW diagnosis but intentionally NOT asserted.
grep -q "CM4SPI-DONE" "$OUT" || { echo "FAIL: DONE missing"; fail=1; }

if [ $fail -eq 0 ]; then
    echo "PASS: CM4 self-configured polled SPI loopback verified in QEMU"
else
    echo "GATE FAILED"; exit 1
fi
