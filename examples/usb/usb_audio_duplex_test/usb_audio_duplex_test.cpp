#include "Arduino.h"
#include "HardwareSerial.h"   // Serial1 (LPUART1) -- not pulled in by USBHost_t36.h
#include "USBHost_t36.h"
#include "utility/imxrt_usbhs.h"   // USBHS_PORTSC1, for port=

// Stage C: FULL DUPLEX against ONE device.
//
// Both directions on the same device is the tractable case and the one worth
// having: IN and OUT share the device's converter, so one rate estimate serves
// both -- and, more usefully, the two directions become two INDEPENDENT
// instruments reading the same crystal at the same instant.
//
// That simultaneity is the point of this sketch. The project has measured this
// MC200's converter five times:
//
//     UAC1 locked-bias sweep     2026-08-02   -85.7 ppm
//     UAC1 glitch investigation  2026-08-02   ~-86   ppm
//     IN byte counter (Stage B)  2026-08-06   -86.7 ppm
//     feedback endpoint, live    2026-08-06   -85.4 ppm
//     UAC2 graph-driven sweep    2026-08-06   -85.8 ppm
//
// Every one of those came from a different session, so time -- temperature,
// really -- was always a confound between any two of them. Duplex removes it:
// fb= and derived= below are read from the same device in the same heartbeat,
// one from the device's own feedback report and one from counting bytes that
// arrived. Agreement is a strong correctness signal for both. Disagreement
// means one of them is wrong, which is worth knowing (design spec section 4).
//
// Non-goal, and deliberately: duplex across two DIFFERENT devices. That needs
// asynchronous sample-rate conversion between unrelated crystals and is a
// different project.

// The device's OUT geometry. UAC2 ignores these (usb_audio.cpp hardcodes the
// witness's 8ch/24 alt until P2's broader negotiation is wired to a chooser),
// but the rate IS honoured and is what the clock gets set to.
#ifndef DUPLEX_RATE_HZ
#define DUPLEX_RATE_HZ 44100
#endif
// The device's INPUT alt must be matched EXACTLY -- the driver refuses to
// approximate, because a format the device never offered is a negotiation
// error and not a rounding one. The MC200 captures 8ch/24 only.
#ifndef DUPLEX_IN_CHANNELS
#define DUPLEX_IN_CHANNELS 8
#endif
#ifndef DUPLEX_IN_BITS
#define DUPLEX_IN_BITS 24
#endif
// Channels actually unpacked into the capture FIFO. Two keeps the consumer's
// deadline comfortably longer than the ring's 32 ms revolution.
#ifndef DUPLEX_TAKE_CHANNELS
#define DUPLEX_TAKE_CHANNELS 2
#endif

// ECHO mode: route what arrives on IN straight back out on OUT, instead of
// playing the driver's tone generator. A monitor path, and the first thing in
// this project where the audio leaving the host is audio that entered it.
//
// The channel counts do NOT have to match and generally do not: the UAC1
// dongle captures 1ch/16 and plays 2ch/16, so mono must be fanned to both
// sides on the way through. The driver's FIFO is stereo int16 regardless of
// what the wire carries -- uac_pack16 handles the wire geometry underneath --
// so this only has to get from `capture channels` to two.
#ifndef DUPLEX_ECHO
#define DUPLEX_ECHO 0
#endif

USBHost myusb;
DMAMEM USBHub hub1(myusb);
DMAMEM USBAudioOut audio(myusb);

static const char *driver_names[] = { "Hub", "Audio" };
static const unsigned NDRIVERS = 2;
static USBDriver *drivers[] = { &hub1, &audio };
static bool driver_active[NDRIVERS] = { false, false };

static bool duplex_started;
static uint32_t seq, started_at, last_beat_ms;
static uint32_t last_out_packets, last_in_packets, last_in_bytes, last_out_errs;
static uint8_t  dev_subslot;          // bytes per device sample, from the descriptor
static int16_t  sink[256];            // where drained capture goes

// ---- CPU budget accounting ------------------------------------------------
// Where does the CM7 actually GO while USB audio streams, and how much is left
// for anything else? Off by default so the gate and the Stage C transcripts
// keep describing the image they were taken from.
//
// EXCLUSIVE is the load-bearing word. The EHCI ISR can land inside any of the
// polled windows, so each window subtracts whatever the ISR accumulated while
// it was open. Without that, ISR cycles are counted twice, the buckets sum to
// more than the wall clock, and idle is understated -- an error in exactly the
// direction that would make this measurement useless.
//
// The reporting printf is its OWN bucket rather than being left to fall into
// idle. It is not free: the heartbeat is ~250 bytes at 115200 baud, and if it
// blocks that is ~20 ms of a 2 s window. Silently calling that "idle" would
// inflate the one number this whole exercise exists to produce.
#ifndef CPU_BUDGET
#define CPU_BUDGET 0
#endif

#if CPU_BUDGET
static volatile uint32_t cpu_isr_cycles;    // cycles inside the EHCI ISR
static volatile uint32_t cpu_isr_entries;
static void (*cpu_isr_chain)(void);         // the driver's own ISR, called through

// Wrapping via _VectorsRam instead of editing the library: USBHost::isr is
// private, and this keeps the measurement entirely inside the sketch.
static void cpu_usb_isr(void)
{
    uint32_t t0 = ARM_DWT_CYCCNT;
    cpu_isr_chain();
    cpu_isr_cycles += ARM_DWT_CYCCNT - t0;
    cpu_isr_entries++;
}

static uint32_t cpu_task_cycles, cpu_svc_cycles, cpu_echo_cycles, cpu_rpt_cycles;
static uint32_t cpu_window_t0, cpu_loops;

// CYCCNT wraps every ~4.31 s at 996 MHz; uint32 subtraction handles that for
// any window shorter than one wrap, and the heartbeat is 2 s.
#define CPU_TIME(acc, call) do {                                    \
        uint32_t _i0 = cpu_isr_cycles, _t0 = ARM_DWT_CYCCNT;        \
        call;                                                       \
        acc += (ARM_DWT_CYCCNT - _t0) - (cpu_isr_cycles - _i0);     \
    } while (0)

static uint32_t cpu_bp(uint32_t cycles, uint32_t window)   // basis points
{
    if (!window) return 0;
    return (uint32_t)(((uint64_t)cycles * 10000u) / window);
}

// ---- headroom by theft ----------------------------------------------------
// The percentages above answer "where did the CM7 go", NOT "how much is
// left". They cannot answer the second question, because this sketch polls in
// a free-running loop: it is ~100% busy by construction, and its `idle` is
// just the share of spin that missed the instrumented calls. Turning that into
// an availability figure would be a fabrication.
//
// So measure availability the only way that cannot be faked: TAKE the CPU away
// and see when the audio breaks. A rising staircase of stolen time, with the
// health counters (under/over/drop/err, and pkts/s) printed beside the level,
// puts the answer in the transcript rather than in an estimate. One flash, one
// capture, and the number is whatever the last clean step was.
// CPU_STEAL_FIXED holds a level indefinitely instead of climbing, which is how
// a staircase step that merely "looked clean for 10 s" becomes a soak result.
#ifndef CPU_STEAL_FIXED
#define CPU_STEAL_FIXED 0
#endif
static uint32_t cpu_steal_pct = CPU_STEAL_FIXED;   // current staircase level
static uint32_t cpu_steal_cycles;   // actually burned this window -- the check
                                    // that the thief took what it claimed
#ifndef CPU_STEAL_STEP
#define CPU_STEAL_STEP 5            // percent added per step
#endif
#ifndef CPU_STEAL_PERIOD
#define CPU_STEAL_PERIOD 5          // heartbeats per step (5 x 2 s = 10 s)
#endif
#ifndef CPU_STEAL_MAX
#define CPU_STEAL_MAX 95
#endif

// Burn `cpu_steal_pct` percent of every millisecond. Pegged to WALL TIME, not
// to loop iterations: the loop rate changes by more than an order of magnitude
// between idle and streaming, so a per-iteration burn would steal a wildly
// different share in the two cases and measure nothing.
static void cpu_steal(void)
{
    if (!cpu_steal_pct) return;
    const uint32_t per_ms = (uint32_t)(F_CPU_ACTUAL / 1000u);
    static uint32_t next_slot;
    static bool armed;
    uint32_t now = ARM_DWT_CYCCNT;
    if (!armed) { next_slot = now + per_ms; armed = true; return; }
    if ((int32_t)(now - next_slot) < 0) return;          // not this millisecond
    // If the loop fell behind by more than a slot, resync rather than trying to
    // repay the debt -- catching up would burn far more than the stated share.
    next_slot = ((int32_t)(now - next_slot) > (int32_t)per_ms) ? now + per_ms
                                                              : next_slot + per_ms;
    uint32_t burn = (uint32_t)(((uint64_t)per_ms * cpu_steal_pct) / 100u);
    uint32_t t0 = ARM_DWT_CYCCNT;
    while ((uint32_t)(ARM_DWT_CYCCNT - t0) < burn) { __asm__ volatile("nop"); }
    cpu_steal_cycles += ARM_DWT_CYCCNT - t0;
}
#else
#define CPU_TIME(acc, call) call
#endif

void setup()
{
    Serial1.begin(115200);
    while (!Serial1) {}
    Serial1.println("DUPLEX-GATE v1");
    Serial1.println("DUPLEX-TEST: start");
    // Announced so the gate can assert what was ASKED FOR, independently of
    // what any device answers -- in QEMU nothing answers at all.
    Serial1.printf("DUPLEX-TEST: requesting OUT %u Hz, IN %u Hz %u ch %u bit, taking %u ch\n",
                   (unsigned)DUPLEX_RATE_HZ, (unsigned)DUPLEX_RATE_HZ,
                   (unsigned)DUPLEX_IN_CHANNELS, (unsigned)DUPLEX_IN_BITS,
                   (unsigned)DUPLEX_TAKE_CHANNELS);
    audio.format(DUPLEX_RATE_HZ, 2, 16);
    // Requesting an input format is what turns the input path on at all.
    audio.formatIn(DUPLEX_RATE_HZ, DUPLEX_IN_CHANNELS, DUPLEX_IN_BITS);
    audio.captureChannels(DUPLEX_TAKE_CHANNELS);
    audio.followFeedback(true);
    myusb.begin();
#if CPU_BUDGET
    // AFTER begin() -- ehci.cpp attaches the driver's ISR there, and this
    // chains onto whatever it installed rather than replacing it.
    cpu_isr_chain = _VectorsRam[16 + IRQ_USBHS];
    if (cpu_isr_chain) _VectorsRam[16 + IRQ_USBHS] = cpu_usb_isr;
    Serial1.printf("DUPLEX-TEST: CPU budget instrumentation on (isr chain %s)\n",
                   cpu_isr_chain ? "hooked" : "MISSING -- isr%% will read 0");
    cpu_window_t0 = ARM_DWT_CYCCNT;
#endif
    last_beat_ms = millis();
}

// Drain the capture FIFO. Unlike the OUT side there is no servo here: the
// device sends what its converter produces and the host must keep up, so this
// runs every loop() and the only defence against falling behind is being fast.
static uint32_t echo_frames, echo_dropped;

static uint32_t drainCapture(void)
{
    uint32_t total = 0, n;
#if DUPLEX_ECHO
    static int16_t out[sizeof(sink) / sizeof(sink[0]) * 2];
    const uint8_t ch = audio.captureChannels() ? audio.captureChannels() : 1;
    while (true) {
        // Read only whole frames, and only as many as the OUT side has room
        // for in STEREO. Reading more than can be written would mean throwing
        // captured audio away, which is a click; leaving it in the capture
        // FIFO instead just delays it one pass.
        uint32_t room_frames = audio.available() / 2;
        if (room_frames == 0) break;
        uint32_t want = room_frames * ch;
        if (want > sizeof(sink) / sizeof(sink[0])) want = (sizeof(sink) / sizeof(sink[0]) / ch) * ch;
        if (want == 0) break;
        n = audio.read(sink, want);
        if (n == 0) break;
        uint32_t frames = n / ch;
        total += n;
        // capture -> stereo. One channel is fanned to both (the dongle's
        // case); two or more are taken as L,R and the rest discarded, which
        // is what captureChannels() already limited us to anyway.
        for (uint32_t f = 0; f < frames; f++) {
            int16_t l = sink[f * ch];
            int16_t r = (ch >= 2) ? sink[f * ch + 1] : l;
            out[f * 2]     = l;
            out[f * 2 + 1] = r;
        }
        uint32_t took = audio.write(out, frames * 2);
        echo_frames += frames;
        if (took < frames * 2) echo_dropped += (frames * 2 - took) / 2;
        if (frames * ch < want) break;      // capture FIFO drained
    }
#else
    while ((n = audio.read(sink, sizeof(sink) / sizeof(sink[0]))) > 0) {
        total += n;
        if (n < sizeof(sink) / sizeof(sink[0])) break;
    }
#endif
    return total;
}

void loop()
{
#if CPU_BUDGET
    cpu_loops++;
#endif
    CPU_TIME(cpu_task_cycles, myusb.Task());

    for (unsigned i = 0; i < NDRIVERS; i++) {
        if (*drivers[i] != driver_active[i]) {
            driver_active[i] = !driver_active[i];
            if (driver_active[i]) {
                Serial1.printf("DUPLEX-TEST: + %s vid=%04X pid=%04X\n", driver_names[i],
                               drivers[i]->idVendor(), drivers[i]->idProduct());
            } else {
                Serial1.printf("DUPLEX-TEST: - %s detached\n", driver_names[i]);
                duplex_started = false;
            }
        }
    }

    // BOTH directions must be configured before either streams. ready() and
    // readyIn() are separate because the input interface is selected by
    // control requests that run AFTER the output ones complete, so there is a
    // window where the first is true and the second is not -- starting OUT in
    // that window would be a simplex stream wearing a duplex sketch's name.
    if (!duplex_started && audio.ready() && audio.readyIn()) {
        Serial1.printf("DUPLEX-TEST: both ready, out alt=%d in alt=%d uac2=%d rate=%lu\n",
                       audio.alternateSetting(), audio.inAlternateSetting(),
                       (int)audio.isUAC2(), (unsigned long)audio.rate());
        const UAC1Topology &t = audio.topology();
        for (uint8_t i = 0; i < t.in_alt_count; i++) {
            if (t.in_alts[i].alternate_setting != (uint8_t)audio.inAlternateSetting())
                continue;
            dev_subslot = t.in_alts[i].subframe_size;
            Serial1.printf("DUPLEX-TEST: in ep=0x%02X %uch x %uB (%u bit) clock=%u/%u\n",
                           t.in_alts[i].endpoint_address, t.in_alts[i].channels,
                           t.in_alts[i].subframe_size, t.in_alts[i].bit_resolution,
                           t.clock_source_id, t.in_clock_source_id);
        }
        // Order matters only in that both must succeed; the descriptor pool
        // now holds 96 iTDs = 32 OUT + 32 feedback + 32 IN, which is exactly
        // what a duplex arm needs and is why Stage C had to grow it.
#if DUPLEX_ECHO
        Serial1.printf("DUPLEX-TEST: ECHO mode -- IN %u ch fanned to stereo OUT\n",
                       (unsigned)audio.captureChannels());
#else
        audio.tone(440);
#endif
        bool o = audio.beginStreaming();
        bool r = audio.beginRecording();
        Serial1.printf("DUPLEX-TEST: out=%s in=%s\n",
                       o ? "streaming" : "FAILED", r ? "recording" : "FAILED");
        duplex_started = true;
        started_at = millis();
        last_beat_ms = started_at;
        last_out_packets = last_in_packets = last_in_bytes = last_out_errs = 0;
    }

    CPU_TIME(cpu_svc_cycles, audio.service());
    CPU_TIME(cpu_echo_cycles, drainCapture());
#if CPU_BUDGET
    cpu_steal();          // its own bucket, so idle stays the honest remainder
#endif

    uint32_t now = millis();
    if ((uint32_t)(now - last_beat_ms) >= 2000u) {
        uint32_t dt = now - last_beat_ms;
        last_beat_ms = now;
        uint32_t portsc = USBHS_PORTSC1;
#if CPU_BUDGET
        uint32_t rpt_t0 = ARM_DWT_CYCCNT;
#endif

        Serial1.printf("DUPLEX-TEST: HEARTBEAT seq=%lu up=%lus out=%d in=%d ctrl=%u/%lu/%lu",
                       (unsigned long)++seq,
                       (unsigned long)((now - started_at) / 1000),
                       (int)audio.streaming(), (int)audio.recording(),
                       (unsigned)audio.controlState(),
                       (unsigned long)audio.controlTimeouts(),
                       (unsigned long)audio.controlQueueFails());
        Serial1.printf(" port=%08lX(ccs=%u pe=%u pp=%u spd=%u)",
                       (unsigned long)portsc, (unsigned)(portsc & 1u),
                       (unsigned)((portsc >> 2) & 1u), (unsigned)((portsc >> 12) & 1u),
                       (unsigned)((portsc >> 26) & 3u));

        if (audio.streaming() && dt) {
            uint32_t p = audio.packetsSent(), e = audio.transportErrors();
            Serial1.printf(" OUT[pkts/s=%lu err/s=%lu fifo=%lu under=%lu]",
                           (unsigned long)((p - last_out_packets) * 1000u / dt),
                           (unsigned long)(e - last_out_errs),
                           (unsigned long)audio.queued(),
                           (unsigned long)audio.underruns());
            last_out_packets = p; last_out_errs = e;
        }
        if (audio.recording() && dt) {
            uint32_t ip = audio.packetsReceived(), ib = audio.inBytes();
            uint32_t bps = (uint32_t)(((uint64_t)(ib - last_in_bytes) * 1000u) / dt);
            uint32_t per_frame = (uint32_t)DUPLEX_IN_CHANNELS * (dev_subslot ? dev_subslot : 4);
            Serial1.printf(" IN[pkts/s=%lu B/s=%lu derived=%luHz over=%lu empty=%lu err=%lu fifo=%lu]",
                           (unsigned long)((ip - last_in_packets) * 1000u / dt),
                           (unsigned long)bps,
                           (unsigned long)(per_frame ? bps / per_frame : 0),
                           (unsigned long)audio.inOverruns(),
                           (unsigned long)audio.inEmptyMicroframes(),
                           (unsigned long)audio.inTransportErrors(),
                           (unsigned long)audio.recorded());
            last_in_packets = ip; last_in_bytes = ib;
        }
#if DUPLEX_ECHO
        // echo= is frames that made the round trip; drop= is frames the OUT
        // FIFO could not take. drop climbing means the two directions are not
        // actually sharing a clock, or the consumer is too slow -- either way
        // it is the number that turns "it sounds odd" into a measurement.
        Serial1.printf(" ECHO[frames=%lu drop=%lu]",
                       (unsigned long)echo_frames, (unsigned long)echo_dropped);
#endif
        // The two instruments, side by side, same device, same instant.
        Serial1.printf(" fb=%lu sizing=%lu fresh=%d\n",
                       (unsigned long)audio.feedbackRateMilliHz(),
                       (unsigned long)audio.sizingRateMilliHz(),
                       (int)audio.feedbackFresh());

#if CPU_BUDGET
        // The window is [previous rpt_t0, this rpt_t0], so it CONTAINS the
        // previous heartbeat's printing -- which is what cpu_rpt_cycles holds.
        // Buckets are reset after the print, so they cover the same span
        // exclusive of it. Everything therefore sums to the wall clock, and
        // idle is what is genuinely left over.
        uint32_t win = rpt_t0 - cpu_window_t0;
        uint32_t isr_bp  = cpu_bp(cpu_isr_cycles,  win);
        uint32_t task_bp = cpu_bp(cpu_task_cycles, win);
        uint32_t svc_bp  = cpu_bp(cpu_svc_cycles,  win);
        uint32_t echo_bp = cpu_bp(cpu_echo_cycles, win);
        uint32_t rpt_bp  = cpu_bp(cpu_rpt_cycles,  win);
        uint32_t stl_bp  = cpu_bp(cpu_steal_cycles, win);
        uint32_t busy_bp = isr_bp + task_bp + svc_bp + echo_bp + rpt_bp + stl_bp;
        uint32_t idle_bp = (busy_bp < 10000u) ? (10000u - busy_bp) : 0u;

        // Split across two calls: Print::printf truncates at 128 bytes PER
        // CALL, silently, and one long line has already cost this project a
        // debugging session.
        Serial1.printf("DUPLEX-TEST: CPU isr=%lu.%02lu%% task=%lu.%02lu%% svc=%lu.%02lu%% echo=%lu.%02lu%%",
                       (unsigned long)(isr_bp / 100),  (unsigned long)(isr_bp % 100),
                       (unsigned long)(task_bp / 100), (unsigned long)(task_bp % 100),
                       (unsigned long)(svc_bp / 100),  (unsigned long)(svc_bp % 100),
                       (unsigned long)(echo_bp / 100), (unsigned long)(echo_bp % 100));
        Serial1.printf(" rpt=%lu.%02lu%% idle=%lu.%02lu%% loops/s=%lu isr/s=%lu win=%lums\n",
                       (unsigned long)(rpt_bp / 100),  (unsigned long)(rpt_bp % 100),
                       (unsigned long)(idle_bp / 100), (unsigned long)(idle_bp % 100),
                       (unsigned long)(dt ? (uint32_t)((uint64_t)cpu_loops * 1000u / dt) : 0u),
                       (unsigned long)(dt ? (uint32_t)((uint64_t)cpu_isr_entries * 1000u / dt) : 0u),
                       (unsigned long)dt);
        // asked= is the staircase level, took= what the thief actually burned.
        // They must agree; if took lags asked, the loop could not keep up with
        // its own theft and the level is not the one being tested.
        Serial1.printf("DUPLEX-TEST: STEAL asked=%lu%% took=%lu.%02lu%% streaming=%d\n",
                       (unsigned long)cpu_steal_pct,
                       (unsigned long)(stl_bp / 100), (unsigned long)(stl_bp % 100),
                       (int)(audio.streaming() && audio.recording()));

        // Climb only while BOTH directions are actually streaming -- ramping
        // during enumeration would test the wrong thing and could break the
        // control sequence rather than the audio.
#if CPU_STEAL_RAMP
        static uint32_t steal_beats;
        if (audio.streaming() && audio.recording()) {
            if (++steal_beats >= (uint32_t)CPU_STEAL_PERIOD) {
                steal_beats = 0;
                if (cpu_steal_pct < (uint32_t)CPU_STEAL_MAX)
                    cpu_steal_pct += (uint32_t)CPU_STEAL_STEP;
            }
        }
#endif

        cpu_window_t0 = rpt_t0;                       // next window starts here
        cpu_rpt_cycles = ARM_DWT_CYCCNT - rpt_t0;     // ...and this print is in it
        cpu_isr_cycles = 0; cpu_isr_entries = 0;
        cpu_task_cycles = cpu_svc_cycles = cpu_echo_cycles = 0;
        cpu_steal_cycles = 0;
        cpu_loops = 0;
#endif
    }
}
