#!/bin/sh
# QEMU gate for the CM4 two-resident-images hot-swap (D7, fast VTOR switch):
# two CM4 images are co-resident in ITCM at once -- A linked at 0x1FFE0000 and
# staged at backdoor 0x20200000, B linked at 0x1FFF0000 and staged at 0x20210000
# (= ITCM+0x10000). The CM7 boots A (begin,STAGE_A), boots B (begin,STAGE_B --
# A stays resident), then SWITCHES the boot VTOR back and forth with
# Multicore.switchImage(): reprogram IOMUXC_LPSR_GPR0/1 + re-pulse
# SRC_CTRL_M4CORE.SW_RESET, WITHOUT re-staging either image. Each boot streams a
# ready handshake (CAFE0001) + its identity over the MU. idA==idA2==A1A1A1A1 and
# idB==idB2==B2B2B2B2 prove both images stayed resident and the new-VTOR reboot
# works bidirectionally. No qemu2 or library change beyond switchImage():
# qemu2 maps the full 256K ocram_m4 backdoor and fsl_imxrt1170_cm4_boot()
# re-reads GPR0/1 fresh and accepts 0x20210000 (system backdoor, above the
# rejected CM4-private-TCM VTOR range [0x1FFE0000, 0x20020000)).
set -e
QEMU=~/Development/rt1170/evkb/tools/qrun
DIR=$(cd "$(dirname "$0")" && pwd)
. ~/Development/rt1170/evkb/tools/gate-lib.sh
gate_init
ELF="$DIR/build/cm4_hotswap2_test.elf"
OUT="$DIR/cm4_hotswap2.uart"
rm -f "$OUT"
"$QEMU" -M mimxrt1170-evk -global fsl-imxrt1170.boot-xip=on -kernel "$ELF" \
    -display none -serial file:"$OUT" -d guest_errors -D "$DIR/cm4_hotswap2.dbg" &
P=$!; gate_pid $P
for _ in $(seq 1 40); do
    [ -f "$OUT" ] && grep -q "CM4HOTSWAP2-DONE" "$OUT" 2>/dev/null && break
    sleep 0.25
done
kill $P 2>/dev/null; wait $P 2>/dev/null || true

echo "==== captured UART ===="; [ -f "$OUT" ] && cat "$OUT" || echo "(no UART output)"

fail=0
check() {
    if grep -q "^$1" "$OUT"; then echo "PASS: $1"; else echo "FAIL: expected $1"; fail=1; fi
}
grep -q "CM4HOTSWAP2-GATE v1" "$OUT" || { echo "FAIL: banner missing"; exit 1; }
check "idA=A1A1A1A1"                 # image A identity (boot A)
check "idB=B2B2B2B2"                 # image B identity (boot B; A stays resident)
check "idA2=A1A1A1A1"                # switchImage(STAGE_A) -> A rebooted, still resident
check "idB2=B2B2B2B2"                # switchImage(STAGE_B) -> B rebooted, still resident
check "HOTSWAP2=PASS"
grep -q "CM4HOTSWAP2-DONE" "$OUT" || { echo "FAIL: DONE missing"; fail=1; }

if [ $fail -eq 0 ]; then
    echo "PASS: two-resident CM4 hot-swap (boot A -> boot B -> switchImage(A) -> switchImage(B)) verified in QEMU"
else
    echo "GATE FAILED"; exit 1
fi
