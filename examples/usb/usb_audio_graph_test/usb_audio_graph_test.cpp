#include "Arduino.h"
#include "HardwareSerial.h"   // Serial1 (LPUART1) -- not pulled in by USBHost_t36.h
#include "USBHost_t36.h"
#include "AudioStream.h"
#include "synth_sine.h"
#include "output_usbhost.h"

// Teensy Audio Library graph feeding a USB Audio Class 1.0 device on the
// MIMXRT1170-EVKB's host port.
//
// Where usb_audio_uac1_test drives USBAudioOut's built-in tone generator
// directly, this runs a real audio graph into it: a sine node connected to
// AudioOutputUSBHost, which is the graph's clock owner.
//
// The thing to watch on the console is `fifo=`. AudioOutputUSBHost holds FIFO
// occupancy near FIFO_TARGET_SAMPLES by choosing when to run the graph, so a
// working setup parks that number near the target and stays there. Drift in
// either direction is the interesting failure: the graph and the device
// disagree about the sample rate, which is exactly what the feedback endpoint
// exists to fix and what this firmware does not yet read.

// --- bisection switch ---------------------------------------------------
//
// 1 = drive the FIFO from USBAudioOut's own tone generator, with the Audio
//     library graph, AudioOutputUSBHost and its occupancy pacing removed from
//     the build entirely. Same USB transport, same device, same rate.
// 0 = the normal graph path.
//
// (Historical note: the analogue A/B that seemed to rule rate mismatch out
// measured 3-9 events/s against a capture chain whose own floor was that
// size. Rate mismatch WAS the cause -- see the bias sweep note below -- but
// its real event rate was ~0.2/s, invisible under that floor. The bisection
// stands for what it did prove: the glitch needs neither the graph nor the
// adapter.)
#define DRIVE_FROM_TONE 1

USBHost myusb;
DMAMEM USBHub hub1(myusb);
DMAMEM USBAudioOut audioOut(myusb);

// Declaration order matters. AudioOutputUSBHost's constructor calls
// audioOut.format() to force the USB rate to match the graph rate, and
// format() only takes effect before the device attaches -- so audioOut must
// already exist here.
#if !DRIVE_FROM_TONE
AudioOutputUSBHost usbSink(audioOut);

AudioSynthWaveformSine sine;

// Mono source fanned out to both channels. receiveReadOnly() in the sink
// handles the shared block correctly.
AudioConnection patchLeft(sine, 0, usbSink, 0);
AudioConnection patchRight(sine, 0, usbSink, 1);
#endif

static const char *driver_names[] = { "Hub", "AudioOut" };
static const unsigned NDRIVERS = 2;
static USBDriver *drivers[] = { &hub1, &audioOut };
static bool driver_active[NDRIVERS] = { false, false };

static uint32_t last_beat, beat_seq, last_packets, last_errs;
static bool stream_started;

// --- rate trim / drift measurement --------------------------------------
//
// The device is asynchronous: its converter runs on its own crystal, so the
// rate we size packets for is only nominally right. The host-side FIFO cannot
// reveal the error -- the pacing loop holds it flat by construction -- so the
// difference accumulates in the DEVICE's buffer until it block-corrects:
// under-feed and lib_xua's decoupler runs dry mid-stream, then inserts
// OUT_BUFFER_PREFILL bytes of silence before resuming; over-feed and it
// drops packets until the FIFO drains to half. Either correction is the
// periodic click. Both paths were finally seen directly on 2026-08-02 with
// xscope probes in the device's decoupler (the patch is committed alongside
// this example): a locked-bias sweep measured drift-vs-trim with slope 0.99
// and 0.06 ppm rms residuals, putting THIS unit's converter at -85.7 ppm
// relative to host nominal, glitch periods matching quantum/drift at every
// point tested.
//
// (An earlier analogue A/B seemed to rule rate mismatch out; its capture
// chain had a 17 events/s floor of its own, and the real glitch rate at
// these offsets is ~0.005-0.06/s. tools/driftrun.sh + tools/vcdfill.py are
// the trustworthy instrument.)
//
// The number wanders with temperature and differs per unit, which is why the
// permanent fix is the feedback endpoint: the device reports its measured
// rate every 2^bRefresh ms and USBAudioOut sizes packets from it
// (followFeedback). The trim below remains as the open-loop fallback and as
// the knob driftrun.sh sweeps to validate the whole chain.
//
// BIAS_MODE_LOCKED comes from the build (tools/driftrun.sh passes
// -DBIAS_LOCKED_PPM=<ppm> via CMake): it locks the trim for one drift
// measurement point, overriding the A/B machinery below, and opens the loop
// (a drift measurement is meaningless with feedback steering the rate).
#define BIAS_SWEEP 0
#ifndef BIAS_LOCKED_PPM
#define BIAS_LOCKED_PPM (0)
#endif
static const int32_t  BIAS_LOCKED   = BIAS_LOCKED_PPM;

// A/B mode. Absolute anomaly rates CANNOT be compared between recordings: the
// capture interface is itself asynchronous, so CoreAudio's resampler adds a
// per-session noise floor of its own that has nothing to do with this device.
// Alternating two bias values inside ONE recording removes that entirely --
// both halves share the same capture state, so any difference is the device.
// Off by default now that feedback closes the loop; -DBIAS_AB=1 revives the
// experiment (open the loop with BIAS_MODE_LOCKED or followFeedback(false)
// if the point is to hear the difference).
#ifndef BIAS_AB
#define BIAS_AB 0
#endif
static const int32_t  BIAS_A        = -86;    // sweep-measured device rate
static const int32_t  BIAS_B        = 250;    // known bad, for contrast
static const uint32_t AB_DWELL_MS   = 30000;
static bool ab_on_a = true;

static const int32_t  BIAS_MIN      = -250;
static const int32_t  BIAS_MAX      =  250;
static const int32_t  BIAS_STEP     =   25;
static const uint32_t BIAS_DWELL_MS = 20000;

static int32_t  bias_ppm = BIAS_MIN;
static uint32_t bias_changed_at;

static void announce_bias(void)
{
    uint32_t mhz = audioOut.effectiveRateMilliHz();
    Serial1.printf("GRAPH-TEST: BIAS %+ld ppm -> sizing packets for %lu.%03lu Hz\n",
                   (long)bias_ppm, (unsigned long)(mhz / 1000),
                   (unsigned long)(mhz % 1000));
}

void setup() {
    Serial1.begin(115200);
    while (!Serial1) {}
    Serial1.println("GRAPH-TEST: start");
#ifdef BIAS_MODE_LOCKED
    // Locked-bias drift measurement: open the loop, or the feedback servo
    // would null out the very drift being measured.
    audioOut.followFeedback(false);
#else
    audioOut.followFeedback(true);
#endif
#if DRIVE_FROM_TONE
    Serial1.println("GRAPH-TEST: MODE = driver tone generator (graph bypassed)");
    // AudioOutputUSBHost would normally do this; without it the driver would
    // stay at its 48 kHz default and the test would not be comparable.
    audioOut.format((uint32_t)AUDIO_SAMPLE_RATE_EXACT, 2, 16);
#else
    Serial1.printf("GRAPH-TEST: MODE = audio graph; rate=%u Hz, block=%d frames, fifo target=%lu\n",
                   (unsigned)AUDIO_SAMPLE_RATE_EXACT, AUDIO_BLOCK_SAMPLES,
                   (unsigned long)AudioOutputUSBHost::FIFO_TARGET_SAMPLES);
    AudioMemory(24);
    sine.frequency(440.0f);
    sine.amplitude(0.5f);
#endif

    myusb.begin();
    last_beat = millis();
}

void loop() {
    myusb.Task();

    for (unsigned i = 0; i < NDRIVERS; i++) {
        if (*drivers[i] != driver_active[i]) {
            driver_active[i] = !driver_active[i];
            if (driver_active[i]) {
                Serial1.printf("GRAPH-TEST: + %s vid=%04X pid=%04X\n", driver_names[i],
                               drivers[i]->idVendor(), drivers[i]->idProduct());
            } else {
                Serial1.printf("GRAPH-TEST: - %s detached\n", driver_names[i]);
                stream_started = false;
            }
        }
    }

    if (!stream_started && audioOut.ready()) {
        // No tone() here on purpose: the samples must come from the graph, so
        // if anything is audible it proves the whole path, not the driver's
        // internal generator.
        Serial1.printf("GRAPH-TEST: device ready, alt=%d, usb rate=%lu Hz\n",
                       audioOut.alternateSetting(), (unsigned long)audioOut.rate());
        // The async feedback pairing, from bSynchAddress in the descriptors.
        // EP 0 here means the device advertises none and the loop stays open.
        {
            const UAC1Topology &t = audioOut.topology();
            for (uint8_t i = 0; i < t.alt_count; i++) {
                if (t.alts[i].alternate_setting != (uint8_t)audioOut.alternateSetting())
                    continue;
                Serial1.printf("GRAPH-TEST: feedback ep=0x%02X refresh=2^%u ms, follow=%d\n",
                               t.alts[i].feedback_endpoint, t.alts[i].feedback_refresh,
                               (int)audioOut.followingFeedback());
            }
        }
        if (audioOut.rate() != (uint32_t)AUDIO_SAMPLE_RATE_EXACT) {
            Serial1.println("GRAPH-TEST: WARNING rate mismatch -- playback will be off pitch");
        }
#if DRIVE_FROM_TONE
        audioOut.tone(440);
#endif
        Serial1.println(audioOut.beginStreaming() ? "GRAPH-TEST: streaming started, 440 Hz"
                                                  : "GRAPH-TEST: STREAM START FAILED");
        stream_started = true;
        last_packets = 0;
        bias_ppm = BIAS_SWEEP ? BIAS_MIN : (BIAS_AB ? BIAS_A : BIAS_LOCKED);
        audioOut.setRateBias(bias_ppm);
        bias_changed_at = millis();
        announce_bias();
    }

    audioOut.service();

    uint32_t now = millis();

    if (BIAS_SWEEP && stream_started
        && (uint32_t)(now - bias_changed_at) >= BIAS_DWELL_MS) {
        bias_ppm += BIAS_STEP;
        if (bias_ppm > BIAS_MAX) bias_ppm = BIAS_MIN;   // wrap and sweep again
        audioOut.setRateBias(bias_ppm);
        bias_changed_at = now;
        announce_bias();
    }

    if (BIAS_AB && !BIAS_SWEEP && stream_started
        && (uint32_t)(now - bias_changed_at) >= AB_DWELL_MS) {
        ab_on_a = !ab_on_a;
        bias_ppm = ab_on_a ? BIAS_A : BIAS_B;
        audioOut.setRateBias(bias_ppm);
        bias_changed_at = now;
        announce_bias();
    }

#ifdef DUMP_CONFIG
    // Fixture capture: print the raw configuration descriptors of whatever
    // device claim() last saw, once, from loop() context. The driver only
    // memcpys during enumeration -- printing from there kills the
    // enumeration itself (measured 2026-08-03, cost half a bench night).
    {
        static uint16_t dumped_len;
        uint16_t dlen;
        const uint8_t *d = audioOut.lastConfig(&dlen);
        // Deliberately late: the console attaches a second or two after
        // reset, and a dump printed at first enumeration lands in that gap.
        // Re-arms when a different-length capture appears, so swapping the
        // attached device mid-run prints the new config too.
        if (dlen && dlen != dumped_len && now > 15000) {
            dumped_len = dlen;
            Serial1.printf("\nCONFIG-DUMP len=%u truncated=%d\n",
                           (unsigned)dlen, (int)audioOut.configWasTruncated());
            for (uint16_t i = 0; i < dlen; i++) {
                Serial1.printf("%02x", d[i]);
                if ((i & 31) == 31) Serial1.println();
            }
            Serial1.println("\nCONFIG-DUMP-END");
        }
    }
#endif

    if ((uint32_t)(now - last_beat) >= 1000u) {
        last_beat = now;
        // The rate is in the heartbeat, not just the one-shot startup line:
        // the flash script's console attaches a second or two after reset, so
        // anything printed once at boot is routinely lost.
        Serial1.printf("GRAPH-TEST: HEARTBEAT seq=%lu up=%lus audio=%s alt=%d graph=%uHz usb=%luHz%s",
                       (unsigned long)++beat_seq, (unsigned long)(now / 1000u),
                       audioOut.ready() ? "ready" : "none", audioOut.alternateSetting(),
                       (unsigned)AUDIO_SAMPLE_RATE_EXACT, (unsigned long)audioOut.rate(),
                       (audioOut.ready() && audioOut.rate() != (uint32_t)AUDIO_SAMPLE_RATE_EXACT)
                           ? " RATE-MISMATCH" : "");
        if (audioOut.streaming()) {
            uint32_t p = audioOut.packetsSent();
            // fifo= is the clock. It should hover near the target; a trend
            // means the graph and the device disagree on rate.
            // Transport errors per second is the number to watch: the audio
            // measurement says packets are going missing at about 4.4/s, and
            // a failed split transaction leaves the descriptor inactive just
            // like a good one, so pkts/s cannot show it.
            uint32_t e = audioOut.transportErrors();
            Serial1.printf(" err/s=%lu (xact=%lu babble=%lu buf=%lu short=%lu)",
                           (unsigned long)(e - last_errs),
                           (unsigned long)audioOut.xactErrors(),
                           (unsigned long)audioOut.babbleErrors(),
                           (unsigned long)audioOut.bufferErrors(),
                           (unsigned long)audioOut.shortSends());
            last_errs = e;
            // fb= is the device's own report of its converter rate in mHz;
            // sizing= is what packets are actually cut to after the slew.
            // With the loop closed the two converge and the device-side
            // buffer stays flat -- the drift sweep's -85.7 ppm should read
            // back here as fb=44096218-ish.
            Serial1.printf(" fb=%lu sizing=%lu fbpkts=%lu fbrej=%lu fberr=%lu fresh=%d",
                           (unsigned long)audioOut.feedbackRateMilliHz(),
                           (unsigned long)audioOut.sizingRateMilliHz(),
                           (unsigned long)audioOut.feedbackPackets(),
                           (unsigned long)audioOut.feedbackRejects(),
                           (unsigned long)audioOut.feedbackErrors(),
                           (int)audioOut.feedbackFresh());
            Serial1.printf(" bias=%+ldppm pkts/s=%lu fifo=%lu/%lu dropped=%lu underruns=%lu",
                           (long)audioOut.rateBiasPpm(),
                           (unsigned long)(p - last_packets),
                           (unsigned long)audioOut.queued(),
                           (unsigned long)AudioOutputUSBHost::FIFO_TARGET_SAMPLES,
#if DRIVE_FROM_TONE
                           0UL,
#else
                           (unsigned long)usbSink.dropped(),
#endif
                           (unsigned long)audioOut.underruns());
            last_packets = p;
        }
#if DRIVE_FROM_TONE
        Serial1.println(" mode=tone");
#else
        Serial1.printf(" cpu=%u%% mem=%u\n", (unsigned)AudioProcessorUsage(),
                       (unsigned)AudioMemoryUsage());
#endif
    }
}
