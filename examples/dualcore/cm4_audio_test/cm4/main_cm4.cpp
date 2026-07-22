/* cm4_audio_test CM4 firmware: the CM4 OWNS THE ENTIRE AUDIO PIPELINE.
 *
 * The M4 runs the real cores/imxrt1176/AudioStream.cpp engine (against the
 * cm4_shim Arduino-lite header), the interrupt-driven SAI1 I/O nodes
 * (AudioOutputI2SInt/AudioInputI2SInt over the shared sai1176 C core, NO DMA --
 * the main eDMA cannot interrupt the CM4), CMSIS-DSP analyze_fft256, and it
 * brings up the WM8962 codec over LPI2C5 itself (codec_wm8962.c). The CM7 only
 * boots this image and relays the MU observations; it parks in WFI and never
 * enables an audio interrupt.
 *
 * Graph:
 *   sine(1033.59 Hz) --+--> AudioOutputI2SInt (SAI1 TX FIFO-request IRQ 76,
 *                      |      audible 1 kHz on J101)
 *                      +--> AudioAnalyzeFFT256   (TX-path tap: DETERMINISTIC
 *                             in both worlds -- 1033.59 Hz = bin 6 exactly,
 *                             fs/256 = 172.266 Hz/bin, like filter_fir_test)
 *   AudioInputI2SInt(mic, RIGHT ch) --> AudioAnalyzePeak  (HW-only mic level)
 *
 * The SAI1 ISR is the graph clock: every 128 TX frames the output node pends
 * IRQ_SOFTWARE (44), whose Software_IRQHandler runs update_all over the graph
 * (~344 Hz at 44.1 kHz). One SAI1 line services both TX fill and RX drain
 * (sai1176_isr_dispatch: rx hook first, tx hook last).
 *
 * Ordering (silicon-driven, matches the HW-verified i2s_int_test): out.begin()
 * starts MCLK/BCLK/LRCLK FIRST, THEN codec init runs (the WM8962 write-sequencer
 * needs SYSCLK=MCLK present -- see codec_wm8962.c), THEN in.begin() arms RX,
 * THEN the CM4 NVIC is armed (SAI1 IRQ 76 = NVIC_ISER2 bit 12; IRQ_SOFTWARE 44
 * was enabled by update_setup inside out.begin()) and `cpsie i` unmasks. The
 * whole codec I2C runs with interrupts masked (polled), so it is never starved
 * by the free-running QEMU TX FIFO-request level.
 *
 * QEMU world-split (documented, not a weakening): the graph is TX-clocked, so
 * fft_peak_bin==6 and dispatch/isr counts are deterministic; the mic path has
 * no acoustic source in QEMU so mic_peak reads ~0 (asserted >=0 only). On the
 * EVKB a human hears 1 kHz on J101, mic_peak rises above ambient, and the CM7's
 * NVIC audio bits stay 0.
 *
 * Reported over MU channel 0, in order:
 *   codec_ack     1 = every WM8962 I2C transaction ACKed
 *   sai_isr_count SAI1 ISR entries
 *   dispatch_count graph dispatches (128-frame TX blocks completed)
 *   underruns     output graph-starve events (expect 0)
 *   fef           TCSR.FEF sticky TX FIFO-error (expect 0)
 *   rx_overflows  RX FIFO overflows recovered (L/R-desync hazard; expect 0)
 *   mic_peak_q15  (uint32)(peak*32767) -- HW only, ~0 in QEMU
 *   fft_peak_bin  argmax of |FFT| over bins 1..127 (expect 6)
 *   fft_peak_mag  output[fft_peak_bin] (raw q15-ish magnitude)
 *   done          0xD0DE0005
 * Public domain (N. Newdigate); node/register logic is MIT (see each source). */
#include <stdint.h>
#include <stdlib.h>          // abs() for the peak read path (also via the shim)
#include "Arduino.h"         // cores/imxrt1176/cm4_shim/Arduino.h
#include "AudioStream.h"     // the real core engine header
#include "synth_sine.h"
#include "analyze_peak.h"
#include "analyze_fft256.h"
#include "output_i2s_int.h"
#include "input_i2s_int.h"
#include "sai1176.h"
extern "C" {
#include "mu_report.h"
#include "codec_wm8962.h"
void cm4_run_ctors(void);
}

// AudioStream.cpp's software_isr has C++ linkage (mangled _Z12software_isrv),
// so the static vector table (startup_cm4.S index 60) carries this extern "C"
// wrapper. sai1176_isr_dispatch is already extern "C" (sai1176.h), but the
// vector table needs the SAI1_IRQHandler name at index 92 -- wrap it too.
void software_isr(void);
extern "C" void Software_IRQHandler(void) { software_isr(); }
extern "C" void SAI1_IRQHandler(void)     { sai1176_isr_dispatch(); }

// --- the CM4-owned audio graph -------------------------------------------
// 1033.59375 Hz = 6 * 44100/256 -> lands exactly on FFT bin 6 (deterministic
// in QEMU and on HW); still an audible "~1 kHz" tone on J101.
#define SINE_HZ         1033.59f
#define SINE_BIN        6u
#define DISPATCH_TARGET 1024u   // wait for >=1024 dispatches: dispatch_count>500
                                // AND sai_isr_count(>=dispatch_count)>1000 in
                                // BOTH worlds, independent of the per-world
                                // ISR/dispatch ratio. ~3 s of audio on HW.

static AudioSynthWaveformSine sine;
static AudioOutputI2SInt      out;
static AudioInputI2SInt       in;
static AudioAnalyzePeak       micpeak;   // mic = RIGHT channel (WM8962 Input3)
static AudioAnalyzeFFT256     fft;       // taps the sine (TX path) -> bin 6
static AudioConnection c1(sine, 0, out, 0);       // left  = sine
static AudioConnection c2(sine, 0, out, 1);       // right = sine
static AudioConnection c3(sine, 0, fft, 0);       // deterministic FFT tap
static AudioConnection c4(in, 1, micpeak, 0);     // mic (right ch) -> peak

// AudioMemory(40), hand-expanded (the AudioMemory macro places its pool in the
// CM7-world DMAMEM section; here it is plain DTCM .bss).
#define AUDIO_POOL_N 40
static audio_block_t audio_pool[AUDIO_POOL_N];

// DWT cycle counter: software_isr reads ARM_DWT_CYCCNT; enable it on the CM4.
#define DEMCR      (*(volatile uint32_t *)0xE000EDFCu)
#define DWT_CTRL   (*(volatile uint32_t *)0xE0001000u)
#define DWT_CYCCNT (*(volatile uint32_t *)0xE0001004u)

// CM4 NVIC set-enable register 2 (IRQs 64..95): SAI1 = 76 -> bit 12.
#define NVIC_ISER2 (*(volatile uint32_t *)0xE000E108u)
// SAI1 (IRQ 76) priority must be NUMERICALLY ABOVE software_isr's 208 (i.e. a
// LOWER urgency) so a pended update_all (IRQ_SOFTWARE=44 @208, set by
// update_setup) PREEMPTS the level-held SAI ISR and actually clocks the graph.
// The CM7 node sets this in sai1176_evkb_nvic_hookup(); on the CM4 that hookup
// is a deliberate no-op (consumer sequences the NVIC), so the image sets it.
// Without it the SAI ISR (default priority 0 = highest) starves the graph and
// every TX block is fed silence (underruns) -- the exact symptom this fixes.
#define SAI1_IRQ_PRIORITY 224u

extern "C" int main(void) {
    cm4_run_ctors();                 // construct the graph nodes + connections

    DEMCR |= (1u << 24);             // TRCENA: unlock the DWT
    DWT_CYCCNT = 0u;
    DWT_CTRL |= 1u;                  // CYCCNTENA

    AudioStream::initialize_memory(audio_pool, AUDIO_POOL_N);

    sine.frequency(SINE_HZ);
    sine.amplitude(0.5f);

    // Phase A: arm the measurement window. The output ISR self-disarms FRIE
    // after DISPATCH_TARGET graph dispatches (the QEMU world-split escape:
    // its TX FIFO-request level never deasserts without an audio backend, so a
    // free-running FRIE would starve main forever; on HW this just pauses the
    // tone, resumed below).
    AudioOutputI2SInt::setPauseAfter(DISPATCH_TARGET);

    // Start the SAI TX clock FIRST (MCLK/BCLK/LRCLK now run; FCONT holds them
    // through the pre-codec underrun). Interrupts are still masked (PRIMASK set
    // from reset; NVIC IRQ 76 not yet enabled), so no ISR fires yet.
    out.begin();

    // Codec bring-up over LPI2C5 -- polled, MCLK present. Interrupts masked, so
    // the I2C is never preempted/starved by the SAI ISR.
    uint32_t codec_ack = codec_wm8962_init();

    // Arm the mic capture path (RX synchronous to TX).
    in.begin();

    // CM4 NVIC hookup (the sai1176 consumer contract -- output_i2s_int.h):
    // update_setup() in out.begin() already NVIC-enabled IRQ_SOFTWARE(44); here
    // we set the SAI1 priority BELOW software_isr (so the graph clock preempts),
    // enable SAI1 (IRQ 76), and unmask. From this point the SAI1 ISR clocks the
    // whole graph on the CM4 with zero CM7 involvement.
    NVIC_SET_PRIORITY(76, SAI1_IRQ_PRIORITY);
    NVIC_ISER2 |= (1u << 12);
    __asm volatile ("cpsie i" ::: "memory");

    // Run the window: wait for the ISR-clocked graph to reach the dispatch
    // target. In QEMU main is starved until FRIE self-disarms at the target;
    // on HW main runs concurrently and sees the count climb at ~344/s.
    while (AudioOutputI2SInt::dispatchCount() < DISPATCH_TARGET) {
        __asm volatile ("nop");
    }

    // Gather the deterministic measurables (the ISR is now disarmed: the graph
    // is frozen, so these reads are race-free).
    uint32_t sai_isr = sai1176_isr_count;
    uint32_t disp    = AudioOutputI2SInt::dispatchCount();
    uint32_t under   = AudioOutputI2SInt::underrunCount();
    uint32_t fef     = AudioOutputI2SInt::fef() ? 1u : 0u;
    uint32_t rx_ovf  = AudioInputI2SInt::overflows();

    // Mic level (HW-only signal; ~0 in QEMU).
    float mp = micpeak.available() ? micpeak.read() : 0.0f;
    uint32_t mic_q15 = (uint32_t)(mp * 32767.0f);

    // FFT peak bin over the sine tap: argmax of the (frozen) magnitude output.
    uint32_t fft_bin = 0u, fft_mag = 0u;
    for (uint32_t k = 1u; k < 128u; k++) {
        uint32_t m = fft.output[k];
        if (m > fft_mag) { fft_mag = m; fft_bin = k; }
    }

    // Stream the observations to the CM7 (MU TR0, fixed order).
    mu_send(codec_ack);
    mu_send(sai_isr);
    mu_send(disp);
    mu_send(under);
    mu_send(fef);
    mu_send(rx_ovf);
    mu_send(mic_q15);
    mu_send(fft_bin);
    mu_send(fft_mag);
    mu_send(0xD0DE0005u);

    // Phase D: continuous audible tone on HW (re-arm FRIE). In QEMU the TX
    // FIFO-request level free-runs the ISR again and parks main here forever --
    // fine, every token is already on the MU wire.
    AudioOutputI2SInt::setPauseAfter(0u);
    AudioOutputI2SInt::resume();
    for (;;) { __asm volatile ("wfi"); }
}
