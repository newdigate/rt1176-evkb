#!/bin/sh
# QEMU gate for Plan-2 Task 3 capstone (cm4_audio_test): the CM4 owns the whole
# audio pipeline (WM8962 over LPI2C5, interrupt-driven SAI1 I/O, AudioStream
# graph, CMSIS analyze_fft256); the CM7 boots it and parks in WFI.
#
# World split (documented, not a weakening -- "silicon wins"):
#  - QEMU asserts the DETERMINISTIC side (AUDIO_CM4_DET): codec write-ACKs
#    (wm8962 stub), the SAI1 ISR clocks the graph (dispatch/isr over threshold),
#    and -- the key trick -- the FFT taps the SINE (TX path), so fft_peak_bin==6
#    (1033.59 Hz = 6*44100/256) is exact in BOTH worlds even with no mic. The
#    CM7 NVIC read-back proves it did zero audio (cm7_audio_isers==0).
#  - FIFO health (underruns/fef/rx_overflows) is HW-ONLY: the QEMU SAI model
#    does not enforce FIFO drain/fill timing, so these are ANTI-correlated with
#    silicon. EVKB 2026-07-22: at SAI IRQ prio 224 (graph outranks SAI) QEMU was
#    clean but the HW RX FIFO overflowed (rx_overflows=0x3FF) because the fft256
#    graph preempted RX service; at the CORRECT prio 192 (SAI outranks the
#    graph, cm4/main_cm4.cpp) the CM4 is clean on HW but QEMU shows
#    graph-starvation underruns. So the full AUDIO_CM4=PASS verdict (which adds
#    FIFO health) is the HW oracle, not a QEMU assertion.
#  - mic_peak is HW-ONLY too (no acoustic source in QEMU). The HW transcript
#    must show a mic level above ambient + a human hears ~1 kHz on J101.
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
check "fft_peak_bin=00000006"   # 1033.59 Hz sine tap -> bin 6 (deterministic)
check "cm7_audio_isers=00000000" # CM7 enabled NO audio interrupt
check "AUDIO_CM4_DET=PASS"      # deterministic verdict: codec + sai_isr>1000 +
                                # disp>500 + fft_bin==6 + cm7_isers==0
grep -q "CM4AUDIO-DONE" "$OUT" || { echo "FAIL: DONE missing"; fail=1; }
# HW-ONLY -- deliberately NOT asserted in QEMU (see header): underruns / fef /
# rx_overflows are anti-correlated with silicon (the QEMU SAI model does not
# enforce FIFO timing), and mic_peak has no acoustic source. The full
# AUDIO_CM4=PASS verdict (adds FIFO health) is the HW oracle; on the EVKB with
# SAI IRQ prio 192 (SAI outranks the graph) all three read 0.

if [ $fail -eq 0 ]; then
    echo "PASS: CM4-owned audio pipeline verified in QEMU"
else
    echo "GATE FAILED"; exit 1
fi
