/* cm4_usb_audio_probe -- Phase 7.3 of the CM4 USB arc: the Cortex-M4 drives an
 * ISOCHRONOUS OUT STREAM.
 *
 * 7.1 proved the CM4 can own the port and take IRQ 135 on its own NVIC.
 * 7.2 proved the real USBHost_t36 transport core runs -nostdlib on the M4 and
 * enumerates a device.  This adds the class driver: the CM4 claims a USB audio
 * device, runs the post-claim control sequence, arms 32 siTDs and streams a
 * tone at it, then reports what the CONTROLLER says it did.
 *
 * The audio source is the driver's own tone generator (usb_audio.cpp
 * topUpFromTone), deliberately.  It is a producer into the same FIFO an
 * AudioStream adapter writes to, so the path under test is the real streaming
 * path -- but no audio graph is linked, so a red run cannot be the graph's
 * fault.  Porting AudioStream to this image is Phase 7.4.
 *
 * The CM7 boots this image and relays the MU stream.  It touches no USB
 * register: a CM7 that configured the block would make a CM4 failure invisible
 * (the circular-pass shape 3.1 warned about).
 *
 * ★★ WHAT THE QEMU GATE CAN AND CANNOT SEE.  Isochronous data does NOT flow
 * against QEMU's emulated usb-audio device -- measured 2026-08-06 and recorded
 * in docs/KNOWN-BROKEN-GATES.md: an OUT sketch enumerates, claims, selects alt
 * 1, completes the whole control sequence and then sits at pkts/s = 0 forever
 * (cause unestablished; a split-transaction/TT mismatch is suspected, since an
 * siTD is for a full-speed device behind a high-speed hub and the model has
 * this one on the root port).  So `pkts` is a WORLD-SPLIT token here, exactly
 * as PHY_PLL_CM4 is in 7.1: the firmware prints STREAM_PACKETS=PASS/FAIL, the
 * QEMU gate asserts only that the token is PRESENT, and silicon asserts that it
 * is PASS.  AUDIO_CM4 is the CONTROL-PLANE verdict and excludes it on purpose.
 * Do not "fix" that by asserting packet flow in QEMU -- the gate would be
 * unpassable for a reason with nothing to do with the CM4.
 *
 * ★ THE SERVICE LOOP IS THE REAL RISK, and `lps` is why it is measured.
 * Instrumented on the CM7 at 996 MHz (usb_audio_duplex_test/
 * transcript_hw_cpu_budget.txt): the USB path's genuine work is 1.16% of the
 * core, but the binding constraint is how long you can go WITHOUT calling
 * service() -- 600 us stalls soak clean, 850 us fail.  The CM4 is 2.5x slower.
 * So this loop does nothing but Task() + service(), and reports its own rate:
 * lps is the margin made visible instead of assumed.  1/lps is the mean gap;
 * anything approaching 1000-2000 loops/s deserves suspicion before the FIFO
 * numbers are believed.
 *
 * Public domain (N. Newdigate). */
#include <Arduino.h>          /* cm4_shim/Arduino.h -- resolved ahead of the
                               * core header by INCLUDE_DIRS ordering */
#include "USBHost_t36.h"

/* ---- the format the probe asks for -----------------------------------------
 *
 * 48000/2/16 is not a preference, it is the only thing QEMU's usb-audio model
 * offers: USBAUDIO_SAMPLE_RATE is a compile-time #define baked into its
 * descriptor tables (hw/usb/dev-audio.c:118) with no property to change it, and
 * uac1_find_alt() correctly refuses a rate the device never advertised.  Every
 * other streaming example in this tree asks for 44100 to match the Audio
 * library, which is exactly why none of them can be gated against that model.
 *
 * A silicon run against a device that does not offer 48 kHz rebuilds with
 * -DPROBE_RATE_HZ=44100; the alt search is what decides, and a refusal shows up
 * as FORMAT_NEGOTIATED=FAIL with alt=FFFFFFFF rather than as a stream that
 * quietly plays at the wrong speed. */
#ifndef PROBE_RATE_HZ
#define PROBE_RATE_HZ   48000u
#endif
#ifndef PROBE_CHANNELS
#define PROBE_CHANNELS  2u
#endif
#ifndef PROBE_BITS
#define PROBE_BITS      16u
#endif
/* Audible on silicon, and obviously wrong to the ear if the frame sizing
 * breaks. The generator is a triangle, not a sine -- see topUpFromTone(). */
#ifndef PROBE_TONE_HZ
#define PROBE_TONE_HZ   1000u
#endif

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

/* ---- the C++ image world's prologue (Phase 5: cm4_cpp_test) ---- */
extern "C" void cm4_run_ctors(void);

/* ★★ DWT FIRST, AND ITS FAILURE IS SILENT.
 *
 * The shim derives millis()/micros()/delay()/delayMicroseconds() from
 * ARM_DWT_CYCCNT.  If DEMCR.TRCENA and DWT_CTRL.CYCCNTENA are not set, CYCCNT
 * reads 0 forever, so every one of those clocks stands still -- and
 * USBHost_t36 is full of timeouts measured against them.  None would ever fire.
 * Enumeration would not fail; it would simply never finish and never give up.
 *
 * It costs this image MORE than it cost 7.2, because the audio driver has a
 * clock of its own: the control watchdog (usb_audio.cpp serviceControl,
 * CTRL_TIMEOUT_MS 500) is the only thing that can rescue a stalled
 * configuration request, since a driver is told about CLEAN completions only.
 * A frozen millis() disarms it, and the sequence then hangs at whatever step
 * the device did not answer, forever, reporting nothing.
 *
 * So it is the first thing main() does, and the FIRST observation on the wire
 * is a measured cycle delta rather than an assertion that the register was
 * written. */
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
 * BOTH objects are DMAMEM, and for the same reason: they CONTAIN structures the
 * EHCI bus master walks, and the CM4's plain .bss is DTCM, which that master
 * cannot reach at all (see cm4.ld and the shim's DMAMEM note).
 *
 * USBHub carries Device_t/Pipe_t/Transfer_t arrays whose constructor donates
 * them to the pools enumeration allocates from.  USBAudioOut carries its
 * `setup`, five Transfer_t, and -- new in this phase, and the reason .bss.dma
 * grows from 20 KB to ~190 KB -- the payload rings the controller DMAs out of:
 * 32 x 256 B of siTD buffers, the HS iTD buffers, and the two 4096-sample
 * FIFOs.  myusb itself has no instance data and correctly stays in DTCM. */
USBHost              myusb;
DMAMEM USBHub        hub1(myusb);
DMAMEM USBAudioOut   audioOut(myusb);

/* Observation windows.
 *
 * READY: long enough for a hand-plugged device on silicon and for QEMU's
 * monitor round-trip plus debounce, reset and enumeration in the emulator. It
 * exits the moment ready() goes true, so a healthy run does not pay for it.
 *
 * STREAM: fixed, because unlike enumeration there is no completion event to
 * wait for -- the answer IS the rate, and a rate needs a known interval. Four
 * seconds is ~4000 frames at 1 packet/ms, enough that a ring servicing even one
 * slot per revolution would be unmistakable, and short enough to stay inside
 * qrun's 60 s cap with the enumeration time in front of it. */
#define READY_WINDOW_MS   10000u
#define STREAM_MS          4000u
/* Hard iteration caps, belt-and-braces: if CYCCNT were dead, millis() would
 * never advance and the millis() bound alone would spin forever -- turning the
 * one failure this probe most wants to REPORT into a hang.  s1 would already
 * have shown dwt=0, but the transcript should still reach its done marker. */
#define WINDOW_LOOPS  200000000u

int main(void) {
    dwt_start();                          /* ★ before ANY clock call -- see above */
    uint32_t dwt = dwt_measure();

    /* Static constructors: myusb/hub1/audioOut are file-scope objects whose
     * constructors call driver_ready_for_device() and contribute the DMA pools.
     * USBAudioOut::init() also calls sitd_pool_init()/itd_pool_init() -- skip
     * this walk and the isochronous pools are never threaded, so every
     * sitd_alloc() in beginStreaming() returns null and the stream fails to arm
     * for a reason that looks nothing like its cause. */
    cm4_run_ctors();

    mu_send(0xA5B00001u);                 /* stage 1: image alive, clocks up */
    mu_send(dwt);                         /* observation: 0 => CYCCNT is dead */

    /* ★ PER-IMAGE OBLIGATION.  The copied startup_cm4.S leaves PRIMASK set
     * (Reset_Handler opens with `cpsid i`).  QEMU dispatches the ISR anyway in
     * some paths, so a missing cpsie i PASSES in the emulator and false-FAILs
     * on silicon.  Phase 5 hit this; 7.1 hit it; 7.2 hit it; it lives in each
     * image's main because the startup file is shared and copied.
     *
     * Before begin(), not after: begin() arms the NVIC and the interrupt mask
     * itself, and a device already present on the port raises PCI inside it. */
    __enable_irq();

    /* Requested format, BEFORE begin(): claim() reads req_rate/req_channels/
     * req_bits to pick the alternate setting, and on silicon the device is
     * already in J47, so the claim can happen inside begin(). Setting it
     * afterwards would race enumeration and select whatever the defaults are.
     *
     * tone() likewise: it only sets tone_hz, but beginStreaming() primes all 32
     * ring buffers from the FIFO before the first packet goes out, and a zero
     * tone at that moment primes the ring with 32 frames of silence and an
     * underrun count to match. */
    audioOut.format(PROBE_RATE_HZ, PROBE_CHANNELS, PROBE_BITS);
    audioOut.tone(PROBE_TONE_HZ);

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
     * ready() is not "a device appeared".  It is active_alt >= 0, which the
     * driver sets only when the LAST step of the post-claim sequence completes
     * -- SET_INTERFACE, and then SET_CUR SAMPLING_FREQ on devices whose alt
     * does not by itself determine the rate.  So this waits for a negotiated
     * format, not for an attachment.
     *
     * service() is in here as well as in the streaming loop, and that is not
     * symmetry for its own sake: serviceControl() -- the watchdog that is the
     * only way back from a control request the device never answers -- runs
     * from service() and nowhere else. A loop that called only Task() would
     * wait out the whole window on a device that stalled one request. */
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

    /* The control sequence's own health, packed so a red run says WHICH of the
     * three things went wrong.  All three zero is the assertion that matters:
     * state 0 means nothing is outstanding, timeouts 0 means the watchdog never
     * had to abandon a request, queue-fails 0 means no retry outran the shared
     * Transfer_t pool.  A device that stalls SET_INTERFACE leaves alt at -1 AND
     * timeouts climbing, which is a different fault from an alt the device
     * never advertised (alt -1, timeouts 0).
     *
     * SATURATED, not truncated. Both counters are lifetime totals with no
     * intrinsic bound -- a device re-enumerating in a loop can run either up --
     * so a raw shift would let a big timeout count spill into the queue-fails
     * field and read as a completely different fault. Clamping keeps a
     * pathological run legible as "lots", which is all the token needs to say. */
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

    /* --- phase C: stream ---------------------------------------------------
     *
     * NOTHING but Task() and service().  See the ★ note in the header: the
     * budget that matters is contiguous stall length, so anything else added to
     * this loop invalidates the lps figure it exists to produce. */
    uint32_t t1 = millis();
    uint32_t sloops = 0;
    while ((uint32_t)(millis() - t1) < STREAM_MS && sloops < WINDOW_LOOPS) {
        myusb.Task();
        audioOut.service();
        sloops++;
    }
    uint32_t elapsed = (uint32_t)(millis() - t1);

    /* loops per second, computed WITHOUT 64-bit division: this image links
     * -nostdlib, so no libgcc, so __aeabi_uldivmod is a link error rather than
     * a slow path.  Both divisions below are 32-bit and lower to the M4's own
     * UDIV.  Split into quotient and remainder so sloops*1000 can never
     * overflow uint32 (it would at ~4.3 M loops, which a 4 s window on a
     * 400 MHz core is not far from). */
    uint32_t lps = 0;
    if (elapsed) {
        lps = (sloops / elapsed) * 1000u + ((sloops % elapsed) * 1000u) / elapsed;
    }

    mu_send(audioOut.packetsSent());      /* ★ THE WORLD-SPLIT TOKEN: 0 in QEMU */
    mu_send(audioOut.transportErrors());  /* xact + babble + buffer + short */
    mu_send(audioOut.underruns());        /* the producer failing to keep up */
    mu_send(audioOut.queued());           /* FIFO level, in samples */
    mu_send(sloops);
    mu_send(lps);
    mu_send(elapsed);
    mu_send(USB2_PORTSC1);                /* portsc2: CCS expected 1 */
    mu_send(0xD0DE0009u);                 /* done marker */

    for (;;) { __asm volatile ("wfi"); }
}
