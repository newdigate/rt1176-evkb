#!/bin/sh
# QEMU gate for Plan-2 Task 3 capstone (cm4_audio_test): the CM4 owns the whole
# audio pipeline (WM8962 over LPI2C5, interrupt-driven SAI1 I/O, AudioStream
# graph, CMSIS analyze_fft256); the CM7 boots it and parks in WFI.
#
# World split (documented, not a weakening):
#  - QEMU asserts the DETERMINISTIC side: codec write-ACKs (wm8962 stub),
#    the SAI1 ISR clocks the graph (dispatch/isr counts over threshold), no
#    underrun/FEF/RX-overflow, and -- the key trick -- the FFT taps the SINE
#    (TX path), so fft_peak_bin==6 (1033.59 Hz = 6*44100/256) is exact in BOTH
#    worlds even though there is no mic. The CM7 NVIC read-back proves it did
#    zero audio (cm7_audio_isers==0).
#  - mic_peak is HW-ONLY (no acoustic source in QEMU): asserted >=0 only, i.e.
#    reported but not thresholded here. The HW transcript must show a mic level
#    above ambient and a human hears ~1 kHz on J101 from a CM4-owned pipeline.
#
# Poll-loop runner (NOT a fixed sleep): wait for CM4AUDIO-DONE up to 60 x 0.25s
# (the CM4 measures ~1024 dispatches before reporting).
set -e
QEMU=~/Development/rt1170/evkb/tools/qrun
DIR=$(cd "$(dirname "$0")" && pwd)
. ~/Development/rt1170/evkb/tools/gate-lib.sh
gate_init
ELF="$DIR/build/cm4_audio_test.elf"
OUT="$DIR/cm4_audio.uart"
rm -f "$OUT" "$DIR/cm4_audio.dbg"
"$QEMU" -M mimxrt1170-evk -global fsl-imxrt1170.boot-xip=on -kernel "$ELF" \
    -display none -serial file:"$OUT" -d guest_errors -D "$DIR/cm4_audio.dbg" &
P=$!; gate_pid $P
for _ in $(seq 1 60); do
    [ -f "$OUT" ] && grep -q "CM4AUDIO-DONE" "$OUT" 2>/dev/null && break
    sleep 0.25
done
kill $P 2>/dev/null; wait $P 2>/dev/null || true

echo "==== captured UART ===="; [ -f "$OUT" ] && cat "$OUT" || echo "(no UART output)"

fail=0
check() {
    if grep -q "^$1" "$OUT"; then echo "PASS: $1"; else echo "FAIL: expected $1"; fail=1; fi
}
grep -q "CM4AUDIO-GATE v1" "$OUT" || { echo "FAIL: banner missing"; exit 1; }

check "codec_ack=00000001"      # WM8962 brought up by the CM4 over LPI2C5
check "underruns=00000000"      # output graph never starved the ISR
check "fef=00000000"            # no TX FIFO error (sticky TCSR.FEF)
check "rx_overflows=00000000"   # no RX FIFO overflow (L/R-desync hazard)
check "fft_peak_bin=00000006"   # 1033.59 Hz sine tap -> bin 6 (deterministic)
check "cm7_audio_isers=00000000" # CM7 enabled NO audio interrupt
check "AUDIO_CM4=PASS"          # firmware verdict (incl. sai_isr>1000, disp>500)
grep -q "CM4AUDIO-DONE" "$OUT" || { echo "FAIL: DONE missing"; fail=1; }
# mic_peak is deliberately NOT thresholded here (HW-only, see header).

if [ $fail -eq 0 ]; then
    echo "PASS: CM4-owned audio pipeline verified in QEMU"
else
    echo "GATE FAILED"; exit 1
fi
