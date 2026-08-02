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
// Measured phase discontinuities of 3-9 per second do NOT track the rate bias
// (A/B across 306 ppm: 0.6 sigma), so the rate-mismatch explanation is out.
// This splits the remaining system in half: if the glitches survive with the
// graph gone, the cause is the transport or the device; if they vanish, it is
// the graph or the adapter.
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

static uint32_t last_beat, beat_seq, last_packets;
static bool stream_started;

// --- rate bias sweep ---------------------------------------------------
//
// The device is asynchronous: its converter runs on its own crystal, so the
// rate we size packets for is only nominally right. The host-side FIFO cannot
// reveal the error -- the pacing loop holds it flat by construction -- so the
// difference accumulates in the DEVICE's buffer until it drops or repeats
// samples, which is the periodic click.
//
// Sweep the trim and listen. The step where the clicks stop, or slow down
// most, is where the device's converter actually is. That measurement is
// worth having independently of the feedback endpoint: when feedback support
// lands, it gives a target to check the decoder against.
//
// 20 s dwell is chosen against an observed click roughly every 5 s, so a
// correct step is four missed clicks -- clearly audible, not a coin flip.
//
// -56 ppm came from fitting anomaly-rate against bias over a swept recording,
// and it looked convincing: a clean V, and the drift arithmetic agreed to the
// decimal (at -250 ppm the error from true is 194 ppm = 8.6 samples/s, and the
// measured excess over the floor was 8.6/s).
//
// IT DID NOT REPRODUCE. A properly controlled A/B -- alternating -56 and +250
// every 30 s inside ONE recording, so both arms share the capture chain's
// state -- found 17.36 vs 17.99 events/s, a difference of 0.63 +- 0.43 (1.5
// sigma) where the drift model predicts 13.5. No effect.
//
// The likely flaw in the swept measurement: the sweep is a sawtooth, so -250
// and +250 are ADJACENT IN TIME at the wrap. "Elevated at both extremes" is
// therefore indistinguishable from "elevated once per 420 s cycle", which any
// slow periodic disturbance in the capture chain would produce. The V was real
// in the data and still meant nothing about bias.
//
// So -56 is an unconfirmed guess, kept only as a placeholder. Do not treat it
// as a measurement. The capture chain's own noise floor also moved from ~7 to
// ~17 events/s between sessions, which is why absolute rates cannot be
// compared across recordings and why this instrument is not trustworthy at
// this resolution. Reading the feedback endpoint measures the device's buffer
// directly and sidesteps the analogue path entirely.
//
// With BIAS_SWEEP 0 the trim is locked there instead of swept.
#define BIAS_SWEEP 0
static const int32_t  BIAS_LOCKED   = -56;

// A/B mode. Absolute anomaly rates CANNOT be compared between recordings: the
// capture interface is itself asynchronous, so CoreAudio's resampler adds a
// per-session noise floor of its own that has nothing to do with this device.
// Alternating two bias values inside ONE recording removes that entirely --
// both halves share the same capture state, so any difference is the device.
#define BIAS_AB 1
static const int32_t  BIAS_A        = -56;    // measured optimum
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
