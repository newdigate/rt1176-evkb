/* cm4_graph_usb_capstone -- Phase 7.4, the capstone of the CM4 USB arc: an
 * AudioStream GRAPH running on the Cortex-M4 feeds the isochronous USB stream
 * THE SAME CORE drives.  The CM7 boots the image and relays MU words; it
 * touches no USB register and runs no audio code.
 *
 *   AudioSynthWaveformSine -> AudioOutputUSBHost -> USBAudioOut -> the device
 *                          `-> AudioAnalyzePeak (the "is it actually audio?" tap)
 *
 * 7.1 proved the CM4 owns the port and takes IRQ 135 on its own NVIC.  7.2
 * proved the real USBHost_t36 transport core enumerates -nostdlib on the M4.
 * 7.3 proved the CM4 arms an isochronous ring and streams AUDIBLY, fed by the
 * driver's own tone generator (`lps=142332`, `pkts=8109`, `xerr=0`,
 * `unders=0`, EVKB 2026-08-07).  Phase 5 proved AudioStream.cpp itself runs
 * unmodified on the M4.  This joins them, which is the first time the graph and
 * the transport have had to share one core.
 *
 * ★★ THE ARCHITECTURAL RISK, AND IT IS THE INVERSE OF PHASE 6's.
 *
 * Phase 6 finding 2: on the CM4 the SAI I/O ISR must OUTRANK the AudioStream
 * graph, or RX overflows (`rx_overflows=0x3FF` at priority 224 vs the graph's
 * 208, EVKB 2026-07-22).  On the CM7 that question is hidden, because DMA
 * services the SAI FIFO and no software deadline exists.
 *
 * Here the polarity is REVERSED, and it is worse.  USB service is not an
 * interrupt at all: it is `myusb.Task()` + `audioOut.service()` called from the
 * main loop, and the graph runs from `software_isr` on IRQ_SOFTWARE = 44.  An
 * ISR always outranks thread mode, so THE GRAPH UNCONDITIONALLY PREEMPTS USB
 * SERVICE and every graph pass is a contiguous stall in the service loop.  No
 * priority number can change that; only the graph's cost can.
 *
 * The budget, measured on the CM7 at 996 MHz (usb_audio_duplex_test/
 * transcript_hw_cpu_budget.txt): the binding constraint is contiguous stall
 * LENGTH, not throughput -- 600 us stalls soak clean over 4.3 min, 850 us fail
 * (FIFO drains monotonically, underruns accelerate).  So this image measures
 * the stall directly rather than inferring it:
 *
 *   gcycmax = AudioStream::cpu_cycles_total_max, the worst FULL graph pass in
 *             units of 16 cycles.  x16 / 400 = microseconds on this core.
 *   lps     = service-loop iterations per second, directly comparable with
 *             7.3's 142332 measured with the graph ABSENT.  1/lps is the mean
 *             gap between service() calls.
 *   unders  = the producer failing to keep the FIFO fed, and gpasses = graph
 *             passes the USB frame clock actually drove.
 *
 * If unders climbs or lps collapses, THAT IS THE FINDING and it belongs in the
 * transcript, not in a smaller graph.
 *
 * ★ THE CLOCK IS THE FIFO, NOT THE FRAME COUNTER.  AudioOutputUSBHost registers
 * frame_consumed() via onFrameConsumed(); USBAudioOut::service() calls it each
 * time it arms a ring slot, and it pends IRQ_SOFTWARE only while FIFO occupancy
 * is below FIFO_TARGET_SAMPLES.  The FIFO drains at the DEVICE's real rate, so
 * holding its level constant makes the graph run at the device's rate with no
 * resampling.  ★ That also means the graph's clock is a CONSEQUENCE of packets
 * moving -- and packets do not move in QEMU (see below).
 *
 * ★★ WHAT THE QEMU GATE CAN AND CANNOT SEE.  Isochronous data does NOT flow
 * against QEMU's emulated usb-audio device: measured 2026-08-06, recorded in
 * docs/KNOWN-BROKEN-GATES.md, and re-confirmed by 7.3, whose passing gate
 * carries `pkts=0` / `STREAM_PACKETS=FAIL`.  Two consequences here:
 *
 *   - `pkts`/`xerr`/`unders` stay WORLD-SPLIT tokens, printed and
 *     presence-checked in QEMU, value-asserted only on silicon;
 *   - and so does `gpasses`, because the graph's clock is packet flow.  A graph
 *     that never gets clocked is not a graph that cannot run.
 *
 * So the graph is proved SEPARATELY, in a way that works in both worlds: after
 * beginStreaming() the image pends IRQ_SOFTWARE itself until the FIFO reaches
 * the adapter's setpoint (phase B2 below).  That is pure CPU, it is exactly the
 * gating frame_consumed() applies, and it is what the QEMU gate asserts --
 * blocks flowed, the FIFO filled, the peak tap saw real audio, nothing leaked.
 * Do NOT "fix" this by asserting packet flow, or graph clocking, in QEMU: that
 * is 7.1's PHY_PLL_CM4 mistake, which has cost this phase twice.
 *
 * Public domain (N. Newdigate). */
#include <Arduino.h>          /* cm4_shim/Arduino.h -- resolved ahead of the
                               * core header by INCLUDE_DIRS ordering */
#include "USBHost_t36.h"
#include "AudioStream.h"      /* the real core engine header */
#include "synth_sine.h"
#include "analyze_peak.h"
#include "output_usbhost.h"

/* ---- the format ------------------------------------------------------------
 *
 * ONE number, two places it has to land, and they are not allowed to disagree.
 *
 * AudioOutputUSBHost's constructor calls audio.format(GRAPH_RATE, 2, 16) where
 * GRAPH_RATE is (uint32_t)AUDIO_SAMPLE_RATE_EXACT -- the adapter enforces the
 * rate match by construction rather than trusting the sketch, because a graph
 * producing 44100 into a stream sized for 48000 is 8.8% sharp and drains the
 * FIFO for good.  So the USB rate here is NOT set by this file at all; it is
 * set by the graph's rate, and the build sets that (CMakeLists defines
 * AUDIO_SAMPLE_RATE_EXACT from PROBE_RATE_HZ).
 *
 * PROBE_RATE_HZ therefore exists here only to be CHECKED against what the
 * adapter actually configured -- the `rate` token below.  Default 48000
 * because that is the ONLY rate QEMU's usb-audio offers (USBAUDIO_SAMPLE_RATE
 * is a compile-time #define in its descriptor tables, hw/usb/dev-audio.c:118)
 * and uac1_find_alt() correctly refuses a rate the device never advertised.  A
 * silicon run against the 44.1 kHz UAC1 dongle rebuilds with
 * -DPROBE_RATE_HZ=44100, which moves BOTH numbers together. */
#ifndef PROBE_RATE_HZ
#define PROBE_RATE_HZ   48000u
#endif

/* Audible on silicon, and obviously wrong to the ear if the graph or the frame
 * sizing breaks.  440 Hz matches the CM7's usb_audio_graph_test so the two
 * transcripts describe the same sound. */
#ifndef GRAPH_TONE_HZ
#define GRAPH_TONE_HZ   440.0f
#endif
#ifndef GRAPH_AMPLITUDE
#define GRAPH_AMPLITUDE 0.5f
#endif

/* Sentinel for "the peak tap analysed no block in this window".  A real q15
 * peak cannot reach 0xFFFFFFFF (the largest possible is 32768), so the value is
 * unambiguous, and it is deliberately NOT 0: zero means "blocks arrived and
 * they were silent", which is a completely different fault from "no blocks
 * arrived". */
#define PEAK_NONE       0xFFFFFFFFu

/* AudioMemory(24) worth of blocks, matching usb_audio_graph_test.  Steady-state
 * occupancy is 1 (see the memuse note at the report site), so this is ~24x
 * headroom; it is not sized to be tight, it is sized so pool exhaustion cannot
 * be the explanation for anything measured here. */
#define AUDIO_POOL_N    24

/* ---- MU B side (the CM4's), TR channel 0 ----------------------------------
 *
 * MUB_BASE, MUB_SR and MU_SR_TE() come from imxrt1176.h, which the shim pulls
 * in for the USB register map.  Only MUB_TR is missing there: the core header
 * declares the A-side MUA_TR(n) because the CM7 library uses it, and no CM7
 * code writes the B side. */
#define REG32(a)   (*(volatile uint32_t *)(a))
#define MUB_TR(n)  REG32(MUB_BASE + 0x00u + ((unsigned)(n) << 2))

static void mu_send(uint32_t v) {
    while (!(MUB_SR & MU_SR_TE(0))) {}
    MUB_TR(0) = v;
}

/* NVIC interrupt-priority registers, one byte per IRQ (ARMv7-M B3.4.7).  Read,
 * not written: see the PRIORITY POLARITY note below. */
#define NVIC_IPR(n)  (*(volatile uint8_t *)(0xE000E400u + (unsigned)(n)))

/* ---- the C++ image world's prologue (Phase 5: cm4_cpp_test) ---- */
extern "C" void cm4_run_ctors(void);

/* ★★ F_CPU_ACTUAL, DEFINED HERE ON PURPOSE, AND ONLY HERE.
 *
 * cm4_shim/Arduino.h DECLARES it and deliberately never defines it, so that an
 * image which odr-uses AudioStream's CPU-usage-percent API fails at LINK rather
 * than silently computing a percentage against the CM7's 996 MHz.  That guard
 * did its job; this image now opts in, because "what fraction of the block
 * period does a graph pass cost" is one of the two numbers Phase 7.4 exists to
 * produce.
 *
 * Opting in means supplying the CM4's OWN clock -- 400 MHz, the same constant
 * the shim's millis()/micros() are derived from (F_CPU_CM4) and the same rate
 * qemu2 clocks the modelled CM4 at (fsl-imxrt1170.c:365).  Written as F_CPU_CM4
 * rather than a fresh literal so there is exactly one place the number lives.
 *
 * The percentage is still the WEAKER of the two measurements.  A percentage is
 * work per block period; the thing that actually breaks USB is the CONTIGUOUS
 * STALL, and that is gcycmax below.  Both are reported; do not read the
 * percentage as the safety margin. */
volatile uint32_t F_CPU_ACTUAL = F_CPU_CM4;

/* ★ software_isr has C++ LINKAGE (`_Z12software_isrv`): AudioStream.cpp
 * declares it as a plain free function (AudioStream.cpp:54/316, no extern "C"),
 * so the flat vector table in startup_cm4.S cannot name it.  Phase 5 solved
 * this with a thin extern "C" wrapper at index 16 + 44 = 60; this is that
 * wrapper, plus a pass counter.
 *
 * The counter is the graph's block count and it is the ONLY thing that
 * distinguishes "the graph is running" from "the FIFO happens to have samples
 * in it".  Counting here rather than inside a node also means it counts PASSES,
 * not blocks a particular node saw, so a node that silently stops updating
 * still shows up -- as the peak tap going quiet while this keeps climbing. */
void software_isr(void);
static volatile uint32_t graph_passes;
extern "C" void Software_IRQHandler(void) { graph_passes++; software_isr(); }

/* ★★ DWT FIRST, AND ITS FAILURE IS SILENT -- and this image needs it THREE
 * times over.
 *
 * The shim derives millis()/micros()/delay()/delayMicroseconds() from
 * ARM_DWT_CYCCNT.  If DEMCR.TRCENA and DWT_CTRL.CYCCNTENA are not set, CYCCNT
 * reads 0 forever, so every one of those clocks stands still.  Then:
 *   1. no USBHost_t36 timeout ever fires, so enumeration neither completes nor
 *      gives up (7.2's lesson);
 *   2. the audio driver's control watchdog (usb_audio.cpp serviceControl,
 *      CTRL_TIMEOUT_MS 500) -- the only way back from a configuration request
 *      the device never answers -- is disarmed (7.3's lesson);
 *   3. and NEW in 7.4: software_isr's own accounting reads CYCCNT, so every
 *      graph-cost number this phase exists to produce would be zero.  A dead
 *      clock would report a FREE graph, which is the most misleading possible
 *      failure for this measurement.
 *
 * So it is the first thing main() does, and the first observation on the wire
 * is a MEASURED cycle delta, not an assertion that a register was written. */
static void dwt_start(void) {
    ARM_DEMCR    |= ARM_DEMCR_TRCENA;
    ARM_DWT_CYCCNT = 0u;
    ARM_DWT_CTRL |= 1u;                          /* CYCCNTENA */
}

/* Bounded, and deliberately not delayMicroseconds(): if CYCCNT is dead, a
 * cycle-count wait never returns.  This one always returns, and returns 0. */
static uint32_t dwt_measure(void) {
    uint32_t t0 = ARM_DWT_CYCCNT;
    for (volatile uint32_t i = 0; i < 200u; i++) { }
    return ARM_DWT_CYCCNT - t0;
}

/* ---- the USB host ----------------------------------------------------------
 *
 * BOTH USB objects are DMAMEM, because they CONTAIN structures the EHCI bus
 * master walks and the CM4's plain .bss is DTCM, which that master cannot reach
 * at all (see cm4.ld and the shim's DMAMEM note).  USBHub carries the
 * Device_t/Pipe_t/Transfer_t arrays its constructor donates to the enumeration
 * pools; USBAudioOut carries `setup`, five Transfer_t, the 32 x 256 B siTD
 * payload ring, the HS iTD buffers and the two 4096-sample FIFOs.
 *
 * ★ DECLARATION ORDER IS LOAD-BEARING, and usb_audio_graph_test says so for the
 * same reason: AudioOutputUSBHost's constructor calls audioOut.format() to force
 * the USB rate to the graph rate, and format() only takes effect before the
 * device attaches.  audioOut must therefore already exist -- and on this image
 * "exist" means "have been constructed by cm4_run_ctors()", which walks
 * .init_array in declaration order within a translation unit.  Reorder these
 * two and the adapter configures an object that has not run its own init() yet.
 *
 * ★ usbSink, sine and peak are NOT DMAMEM, deliberately.  Nothing walks an
 * AudioStream node; the adapter COPIES each block into the FIFO, which is
 * already in OCRAM2 inside audioOut.  Marking them DMAMEM would move graph
 * working set out of tightly-coupled memory for no reason. */
USBHost              myusb;
DMAMEM USBHub        hub1(myusb);
DMAMEM USBAudioOut   audioOut(myusb);

AudioOutputUSBHost   usbSink(audioOut);
AudioSynthWaveformSine sine;
/* The audio oracle.  Every other number here would look identical if the sine
 * produced silence: blocks would still flow, the FIFO would still fill, packets
 * would still go out.  peak.read() > 0 is what says the samples on the wire are
 * AUDIO.  It is pure CPU, so unlike packet flow it is assertable in QEMU too. */
AudioAnalyzePeak     peak;

/* Mono source fanned out to both channels; receiveReadOnly() in the sink handles
 * the shared block correctly.  Third connection taps the same output into the
 * peak analyser. */
AudioConnection      patchLeft(sine, 0, usbSink, 0);
AudioConnection      patchRight(sine, 0, usbSink, 1);
AudioConnection      patchTap(sine, 0, peak, 0);

/* AudioMemory(AUDIO_POOL_N), hand-expanded (AudioStream.h:121-124), exactly as
 * Phase 5's cm4_audiostream_test does.  The macro wraps the array in DMAMEM,
 * which is a CM7-world placement; here it is plain .bss and therefore DTCM.
 * See the ★ note in cm4.ld for why that is the RIGHT place rather than merely a
 * workaround. */
static audio_block_t audio_pool[AUDIO_POOL_N];

/* Observation windows.
 *
 * READY: long enough for a hand-plugged device on silicon and for QEMU's
 * monitor round-trip plus debounce, reset and enumeration in the emulator.  It
 * exits the moment ready() goes true, so a healthy run does not pay for it.
 *
 * STREAM: fixed, because unlike enumeration there is no completion event to
 * wait for -- the answer IS a rate, and a rate needs a known interval.  Four
 * seconds matches 7.3 exactly, so `lps` here and `lps` there are the same
 * measurement with and without the graph. */
#define READY_WINDOW_MS   10000u
#define STREAM_MS          4000u
/* Hard iteration caps, belt-and-braces: if CYCCNT were dead, millis() would
 * never advance and the millis() bound alone would spin forever -- turning the
 * one failure this probe most wants to REPORT into a hang. */
#define WINDOW_LOOPS  200000000u

/* Phase B2's caps.  PRIME_PASSES is generous against the 3 passes it should
 * take (256 samples per pass, 768-sample setpoint); PRIME_SPIN bounds the wait
 * for each pass so a graph that never runs is REPORTED as gblocks=0 rather than
 * hanging the image. */
#define PRIME_PASSES  64u
#define PRIME_SPIN    200000u

int main(void) {
    dwt_start();                          /* ★ before ANY clock call -- see above */
    uint32_t dwt = dwt_measure();

    /* Static constructors.  On this image the walk builds BOTH worlds:
     * myusb/hub1/audioOut register drivers and thread the isochronous pools
     * (USBAudioOut::init() calls sitd_pool_init()/itd_pool_init(); skip the walk
     * and every sitd_alloc() in beginStreaming() returns null), and
     * usbSink/sine/peak/the three AudioConnections build the graph --
     * AudioOutputUSBHost's ctor also calls update_setup(), which NVIC-enables
     * IRQ_SOFTWARE and sets its priority to 208. */
    cm4_run_ctors();

    mu_send(0xA5B00001u);                 /* stage 1: image alive, clocks up */
    mu_send(dwt);                         /* observation: 0 => CYCCNT is dead */

    /* The pool BEFORE anything can pend the graph.  update_setup() has already
     * NVIC-enabled IRQ_SOFTWARE during the ctor walk, so once PRIMASK clears a
     * pend could dispatch at any time; allocate() against a null pool would
     * merely return NULL (every mask word is zero at reset), but ordering it
     * here means the graph can never observe a half-built pool.
     *
     * ★ AND NOTE WHAT THIS CALL ITSELF DOES: initialize_memory() wraps its work
     * in __disable_irq()/__enable_irq(), and the shim's __enable_irq() is an
     * unconditional `cpsie i` -- so PRIMASK is actually cleared HERE, several
     * lines before the explicit call below.  That is harmless only because of
     * what is armed at this instant: IRQ_SOFTWARE is enabled but nothing pends
     * it, USB 135 is not NVIC-enabled until begin(), and the MU interrupt is
     * never enabled on this image.  It is written down because "the cpsie i is
     * the line that unmasks" is the obvious reading and it is wrong; if a future
     * edit arms anything before this point, this is where it starts running. */
    AudioStream::initialize_memory(audio_pool, AUDIO_POOL_N);
    sine.frequency(GRAPH_TONE_HZ);
    sine.amplitude(GRAPH_AMPLITUDE);

    /* ★ PER-IMAGE OBLIGATION.  The copied startup_cm4.S leaves PRIMASK set
     * (Reset_Handler opens with `cpsid i`).  QEMU dispatches the ISR anyway in
     * some paths, so a missing cpsie i PASSES in the emulator and false-FAILs on
     * silicon.  Phase 5 hit this; 7.1, 7.2 and 7.3 each hit it again; it lives
     * in each image's main because the startup file is shared and copied.
     *
     * Before begin(), not after: begin() arms the NVIC and the interrupt mask
     * itself, and a device already present on the port raises PCI inside it. */
    __enable_irq();

    /* NO audioOut.format() and NO audioOut.tone() here, and both omissions are
     * the point of the phase.
     *
     * format(): the ADAPTER set it, in its constructor, from the graph's rate.
     * Setting it again here would make the token below unable to fail.
     *
     * tone(): USBAudioOut's built-in generator is a producer into the very same
     * FIFO the adapter writes to.  Leaving it at its default 0 means the ONLY
     * source of samples on the wire is the graph -- so a working stream cannot
     * be the driver quietly feeding itself, which is precisely the confound 7.3
     * left for 7.4 to remove. */

    /* Everything 7.1 did by hand, done by the library: LPCG115, the USBPHY2
     * PLL, the EHCI reset, the DMA pools, host mode + SDIS, the periodic list,
     * port power, RUN, the vector, NVIC 135, and USBINTR. */
    myusb.begin();

    mu_send(0xA5B00002u);                 /* stage 2: begin() RETURNED */
    mu_send(USB2_USBINTR);                /* observation: UAIE(18) must be set */

    /* Settle, then report the port BEFORE anything is plugged.  CCS is expected
     * 0 here: the QEMU gate device_adds on this marker and the operator plugs
     * J47 on it, so portsc -> portsc2 is a real 0->1 connect edge rather than a
     * device that was present all along.  Same un-fakeable shape as 7.1/7.2. */
    delay(200);
    mu_send(0xA5B00003u);                 /* stage 3: ARMED AND WAITING */
    mu_send(USB2_PORTSC1);                /* observation: CCS(bit0) expected 0 */

    /* --- phase A: claim + control sequence ---------------------------------
     *
     * ready() is active_alt >= 0, which the driver sets only when the LAST step
     * of the post-claim sequence completes -- SET_INTERFACE, then SET_CUR
     * SAMPLING_FREQ where the alt does not by itself determine the rate.  So
     * this waits for a NEGOTIATED FORMAT, not for an attachment.
     *
     * service() is in here as well as in the streaming loop: serviceControl()
     * -- the watchdog that is the only way back from a control request the
     * device never answers -- runs from service() and nowhere else. */
    uint32_t t0 = millis();
    uint32_t loops = 0;
    while ((uint32_t)(millis() - t0) < READY_WINDOW_MS && loops < WINDOW_LOOPS) {
        myusb.Task();
        audioOut.service();
        loops++;
        if (audioOut.ready()) break;
    }

    mu_send(0xA5B00004u);                 /* stage 4: claim window closed */
    mu_send((bool)audioOut ? 1u : 0u);    /* bound: USBDriver::operator bool */
    mu_send((uint32_t)audioOut.idVendor());
    mu_send((uint32_t)audioOut.idProduct());

    mu_send(0xA5B00005u);                 /* stage 5: format negotiation */
    mu_send((uint32_t)audioOut.alternateSetting());   /* -1 -> FFFFFFFF */
    /* ★ THE ADAPTER'S RATE, READ BACK.  Nothing in this file called format();
     * if this is not PROBE_RATE_HZ then AudioOutputUSBHost's constructor did not
     * run, or ran against a different AUDIO_SAMPLE_RATE_EXACT than the build
     * intended -- which is the exact defect that would put a 44100 graph into a
     * 48000 stream and drain the FIFO for good. */
    mu_send(audioOut.rate());

    /* The control sequence's own health, packed so a red run says WHICH of the
     * three things went wrong.  SATURATED, not truncated: both counters are
     * lifetime totals with no intrinsic bound, so a raw shift would let a big
     * timeout count spill into the queue-fails field and read as a different
     * fault entirely. */
    uint32_t c_tmo = audioOut.controlTimeouts();
    uint32_t c_qf  = audioOut.controlQueueFails();
    if (c_tmo > 0xFFu)   c_tmo = 0xFFu;
    if (c_qf  > 0xFFFFu) c_qf  = 0xFFFFu;
    mu_send(0xA5B00006u);                 /* stage 6: control-sequence health */
    mu_send((c_qf << 16) | (c_tmo << 8) | (uint32_t)audioOut.controlState());

    /* --- phase B: arm the ring --------------------------------------------
     *
     * Guarded on ready(): beginStreaming() would return false anyway with
     * active_alt < 0, but calling it regardless would make "the device never
     * negotiated" and "the ring would not arm" report as the same failure. */
    uint32_t armed = (audioOut.ready() && audioOut.beginStreaming()) ? 1u : 0u;
    mu_send(0xA5B00007u);                 /* stage 7: streaming armed */
    mu_send(armed);

    /* --- phase B2: PRIME THE FIFO, which is also the graph's proof ---------
     *
     * Two jobs, one loop, and neither is a workaround.
     *
     * 1. Priming.  beginStreaming() calls usb_audio_fifo_reset() and then fills
     *    all 32 ring slots from the now-empty FIFO, so the ring starts with
     *    32 ms of silence whatever was in the FIFO before (it also zeroes
     *    underrun_count afterwards, so those 32 do not show up).  The graph's
     *    own clock cannot pre-empt that: frame_consumed() only fires once
     *    service() starts arming slots.  Without this loop the first frames of
     *    the measured window underrun while the graph catches up, and `unders`
     *    -- the number that says whether the graph kept the stream fed -- would
     *    carry a startup artefact in every single run.
     *
     * 2. The graph's liveness proof, in BOTH worlds.  Packets do not move in
     *    QEMU, so frame_consumed() never fires there and a purely USB-clocked
     *    graph would be untestable in the gate.  Pending IRQ_SOFTWARE directly
     *    is precisely what AudioStream::update_all() does -- the only difference
     *    is who pends it -- and the `queued() < FIFO_TARGET_SAMPLES` condition
     *    is verbatim the gate frame_consumed() applies.  So this runs the real
     *    graph through the real adapter into the real FIFO; it just supplies the
     *    tick.  It is bounded twice (passes and spin) so a graph that never runs
     *    is REPORTED, not hung on.
     *
     * Expect 3 passes: 128 frames x 2 channels = 256 samples each, against a
     * 768-sample setpoint.  The FIRST pass writes silence, because update order
     * is construction order and usbSink was constructed before sine -- so the
     * sink consumes what the source produced on the PREVIOUS pass.  One block of
     * latency, 2.7 ms, and it is why memuse settles at 1 rather than 0. */
    uint32_t primed = 0;
    while (audioOut.queued() < AudioOutputUSBHost::FIFO_TARGET_SAMPLES
           && primed < PRIME_PASSES) {
        uint32_t before = graph_passes;
        NVIC_SET_PENDING(IRQ_SOFTWARE);
        for (volatile uint32_t i = 0; i < PRIME_SPIN && graph_passes == before; i++) { }
        primed++;
    }

    /* peak.read() RESETS the min/max accumulators, so this value covers the
     * priming passes only and the second read below covers the streaming window
     * only -- two independent samples rather than one cumulative one.  Scaled to
     * q15 because the MU carries uint32_t and a float would have to be punned.
     *
     * ★★ GUARDED ON available(), AND THAT IS NOT DEFENSIVE PROGRAMMING -- IT IS
     * A CORRECTNESS FIX.  AudioAnalyzePeak's reset state is min=+32767,
     * max=-32768, and read() returns max(|min|,|max|)/32767.  So a peak node
     * that has analysed NOTHING reads 1.0 -- FULL SCALE.  An unguarded read
     * therefore reports the loudest possible signal exactly when there was no
     * signal at all, which is the most dangerous shape a token can have: the
     * starved case and the healthy case are indistinguishable, and the starved
     * one looks better.  Caught in the first QEMU run, where the streaming
     * window analyses nothing (no packets, so no graph clock) and peak2 came
     * back 32768.
     *
     * available() also CLEARS the flag, which is what makes the two reads
     * independent: without the clear here the second read would see the
     * priming passes' flag still set and claim fresh data it never got. */
    uint32_t peak_prime = peak.available() ? (uint32_t)(peak.read() * 32767.0f)
                                           : PEAK_NONE;

    mu_send(0xA5B00008u);                 /* stage 8: graph primed the FIFO */
    mu_send(graph_passes);                /* gblocks: graph passes so far      */
    mu_send(audioOut.queued());           /* fifo: FIFO level, in samples      */
    /* The setpoint, SENT rather than duplicated on the CM7 side.  It is
     * AUDIO_BLOCK_SAMPLES * 2 * 3, i.e. a build-time function of the graph's
     * block size, and the relay has no Audio library to derive it from -- a
     * hand-copied 768 there would silently go stale the day anyone changes
     * AUDIO_BLOCK_SAMPLES, and the fifo assertion would quietly weaken. */
    mu_send(AudioOutputUSBHost::FIFO_TARGET_SAMPLES);
    mu_send(peak_prime);                  /* peak: q15 -- 0 means SILENCE      */
    mu_send(usbSink.dropped());           /* drop: blocks the FIFO refused     */
    mu_send(AudioMemoryUsage());          /* memuse: live blocks               */
    mu_send(AudioMemoryUsageMax());       /* memmax: pool high-water mark      */

    /* ★ THE PRIORITY POLARITY, MEASURED RATHER THAN ASSERTED.  Neither number
     * is set by this file: AudioStream::update_setup() chose 208 for
     * IRQ_SOFTWARE and USBHost::begin() NVIC-enabled IRQ 135 without touching
     * its priority, leaving the reset value 0.  Reading the registers back is
     * the difference between "the libraries are documented to do this" and "on
     * this core, in this image, they did".  Lower value = higher priority, so
     * usb < graph is the Phase-6 discipline: the interrupt-driven half of USB
     * (enumeration and every control transfer) preempts the graph.
     *
     * It says NOTHING about the half that matters most here -- service() runs in
     * thread mode and no priority can protect it from an ISR.  That is what
     * gcycmax and lps are for. */
    uint32_t prio_usb   = NVIC_IPR(IRQ_USB2);
    uint32_t prio_graph = NVIC_IPR(44);   /* IRQ_SOFTWARE */
    mu_send((prio_usb << 8) | prio_graph);

    /* --- phase C: stream ---------------------------------------------------
     *
     * NOTHING but Task() and service(), exactly as 7.3, so that `lps` here and
     * `lps` there differ ONLY by the presence of the graph.  Anything else added
     * to this loop invalidates the one comparison the phase is built around.
     *
     * The graph runs INSIDE this loop all the same -- from software_isr,
     * preempting it, driven by frame_consumed() from within service(). */
    uint32_t g0 = graph_passes;
    uint32_t t1 = millis();
    uint32_t sloops = 0;
    while ((uint32_t)(millis() - t1) < STREAM_MS && sloops < WINDOW_LOOPS) {
        myusb.Task();
        audioOut.service();
        sloops++;
    }
    uint32_t elapsed = (uint32_t)(millis() - t1);
    uint32_t gpasses = graph_passes - g0;

    /* loops per second, computed WITHOUT 64-bit division: split into quotient
     * and remainder so sloops*1000 cannot overflow uint32 (it would at ~4.3 M
     * loops, which a 4 s window on this core is not far from). */
    uint32_t lps = 0;
    if (elapsed) {
        lps = (sloops / elapsed) * 1000u + ((sloops % elapsed) * 1000u) / elapsed;
    }

    mu_send(0xA5B00009u);                 /* stage 9: streaming window closed */
    mu_send(audioOut.packetsSent());      /* ★ WORLD-SPLIT: 0 in QEMU          */
    mu_send(audioOut.transportErrors());  /* xact + babble + buffer + short     */
    mu_send(audioOut.underruns());        /* ★ the graph failing to keep up     */
    mu_send(audioOut.queued());           /* FIFO level -- see the note below   */
    mu_send(gpasses);                     /* ★ graph passes the USB clock drove */
    /* ★★ THE STALL, AND IT IS THE NUMBER THE PHASE IS ABOUT.  software_isr
     * stores (cycles >> 4), so x16 gives cycles and /400 gives microseconds on
     * this core; the CM7 does that arithmetic for the transcript.  gcycmax is
     * the WORST full pass, which is what has to be compared against the CM7's
     * measured 600 us clean / 850 us fail knee -- not the average, and not the
     * percentage. */
    mu_send(AudioStream::cpu_cycles_total);
    mu_send(AudioStream::cpu_cycles_total_max);
    /* The percentage form, for continuity with every other Audio-library
     * measurement in this tree.  Only meaningful because F_CPU_ACTUAL is
     * defined at the top of this file to the CM4's own clock. */
    mu_send((uint32_t)AudioProcessorUsage());
    mu_send((uint32_t)AudioProcessorUsageMax());
    mu_send(usbSink.dropped());           /* drop2: still 0 => no block refused */
    /* peak2: audio DURING the streaming window, or PEAK_NONE if the graph was
     * never clocked (which is the QEMU case, and which must NOT read as full
     * scale -- see the guard note at the first read). */
    mu_send(peak.available() ? (uint32_t)(peak.read() * 32767.0f) : PEAK_NONE);
    mu_send(AudioMemoryUsage());          /* memuse2: bounded => no leak        */
    mu_send(sloops);
    mu_send(lps);                         /* ★ compare directly with 7.3's 142332 */
    mu_send(elapsed);
    mu_send(USB2_PORTSC1);                /* portsc2: CCS expected 1 */
    mu_send(0xD0DE000Au);                 /* done marker */

    for (;;) { __asm volatile ("wfi"); }
}
