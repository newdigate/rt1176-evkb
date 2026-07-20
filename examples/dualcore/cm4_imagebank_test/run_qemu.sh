#!/bin/sh
# QEMU gate for Cm4ImageBank over Multicore.switchImage(). Four CM4 images in
# uniform ITCM slots: A/B/C in distinct slots 0/1/2 (co-resident), D sharing
# A's slot 0 (pages). switchTo() flips (no copy) when resident, stages+evicts
# when not. Each boot streams ready(CAFE0001)+identity over the MU; the CM7
# also logs the isResident() nibble (A=8,B=4,C=2,D=1). No qemu2 change -- the
# machine maps the full 256K ocram_m4 backdoor and re-reads GPR0/1 per boot.
set -e
QEMU=~/Development/rt1170/evkb/tools/qrun
DIR=$(cd "$(dirname "$0")" && pwd)
. ~/Development/rt1170/evkb/tools/gate-lib.sh
gate_init
ELF="$DIR/build/cm4_imagebank_test.elf"
OUT="$DIR/cm4_imagebank.uart"
rm -f "$OUT"
"$QEMU" -M mimxrt1170-evk -global fsl-imxrt1170.boot-xip=on -kernel "$ELF" \
    -display none -serial file:"$OUT" -d guest_errors -D "$DIR/cm4_imagebank.dbg" &
P=$!; gate_pid $P
for _ in $(seq 1 40); do
    [ -f "$OUT" ] && grep -q "CM4IMAGEBANK-DONE" "$OUT" 2>/dev/null && break
    sleep 0.25
done
kill $P 2>/dev/null; wait $P 2>/dev/null || true

echo "==== captured UART ===="; [ -f "$OUT" ] && cat "$OUT" || echo "(no UART output)"

fail=0
check() {
    if grep -q "^$1" "$OUT"; then echo "PASS: $1"; else echo "FAIL: expected $1"; fail=1; fi
}
grep -q "CM4IMAGEBANK-GATE v1" "$OUT" || { echo "FAIL: banner missing"; exit 1; }
check "idA=A1A1A1A1"         # stage A (BT_RELEASE edge)
check "idB=B2B2B2B2"         # stage B (distinct slot; A resident)
check "idC=C3C3C3C3"         # stage C (distinct slot; A,B resident)
check "resABC=0000000E"      # isResident nibble: A,B,C set, D clear (co-residency)
check "idA2=A1A1A1A1"        # switchTo(A) -> FAST FLIP (A still resident)
check "idD=D4D4D4D4"         # switchTo(D) -> slot 0, evicts A
check "resD=00000007"        # B,C,D set, A clear (slot-scoped eviction)
check "idA3=A1A1A1A1"        # switchTo(A) -> RE-STAGE; ==A1 not D4 => re-copied
check "resA3=0000000E"       # A,B,C set, D clear (A re-staged, D evicted)
check "IMAGEBANK=PASS"
grep -q "CM4IMAGEBANK-DONE" "$OUT" || { echo "FAIL: DONE missing"; fail=1; }

if [ $fail -eq 0 ]; then
    echo "PASS: Cm4ImageBank co-residency + no-copy flip + slot-scoped eviction verified in QEMU"
else
    echo "GATE FAILED"; exit 1
fi
