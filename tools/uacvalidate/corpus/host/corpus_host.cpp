// Minimal UAC host firmware for the validator's verification corpus.
//
// Every corpus case builds THIS, not examples/usb/usb_audio_graph_test. The
// example calls feedbackRateMilliHz(), isUAC2(), controlState(), topology(),
// lastConfig() and patternMode(), none of which existed at the oldest corpus
// commit -- building it at 99cd466~1 yields a compile error rather than a
// historical host, and a corpus case that cannot be built is not a case.
//
// So this sketch uses only the intersection of the driver API across every
// corpus commit: format, ready, tone, beginStreaming, service, packetsSent,
// alternateSetting, queued, underruns, streaming, and the transport error
// counters. If a future case reaches further back and one of those is also
// missing, drop it from the heartbeat rather than reaching forward -- the
// heartbeat is a convenience, and the actual measurement is on the device.
//
// Deliberately dull. The validator judges the DEVICE-side capture; all this
// firmware owes it is a stream and an honest console line about whether the
// host believes it is streaming. That belief is itself evidence in case E,
// where the host says audio=ready alt=1 while transmitting nothing.
#include "Arduino.h"
#include "HardwareSerial.h"
#include "USBHost_t36.h"

// The negotiated format is deliberately the same request for both classes:
// 44100/2/16 is what the UAC1 witness advertises directly, and the UAC2
// driver maps it onto the device's native 8ch/4B subslots from the alt
// descriptor. Asking for the device's native geometry instead would make
// the UAC1 cases unclaimable.
#ifndef CORPUS_RATE_HZ
#define CORPUS_RATE_HZ 44100
#endif
#ifndef CORPUS_TONE_HZ
#define CORPUS_TONE_HZ 440
#endif

USBHost myusb;
DMAMEM USBHub hub1(myusb);
DMAMEM USBAudioOut audioOut(myusb);

static uint32_t last_beat, beat_seq, last_packets;
static bool stream_started;

void setup() {
    Serial1.begin(115200);
    while (!Serial1) {}
    Serial1.println("CORPUS-HOST: start");
    audioOut.format((uint32_t)CORPUS_RATE_HZ, 2, 16);
    myusb.begin();
    last_beat = millis();
}

void loop() {
    myusb.Task();

    if (!stream_started && audioOut.ready()) {
        Serial1.printf("CORPUS-HOST: device ready, alt=%d, rate=%lu\n",
                       audioOut.alternateSetting(),
                       (unsigned long)audioOut.rate());
        // R3 needs signal: a silent stream makes the justification test pass
        // for the wrong reason, which is why the judge refuses to rule on it
        // without non-silent frames. The tone is the corpus's vacuity guard.
        audioOut.tone(CORPUS_TONE_HZ);
        Serial1.println(audioOut.beginStreaming()
                        ? "CORPUS-HOST: streaming started"
                        : "CORPUS-HOST: STREAM START FAILED");
        stream_started = true;
        last_packets = 0;
    }

    audioOut.service();

    uint32_t now = millis();
    if ((uint32_t)(now - last_beat) >= 1000u) {
        last_beat = now;
        uint32_t p = audioOut.packetsSent();
        // pkts/s is the line that matters for case E: the wedge reports
        // audio=ready with a valid alt while this reads 0.
        Serial1.printf("CORPUS-HOST: HEARTBEAT seq=%lu up=%lus audio=%s alt=%d "
                       "pkts/s=%lu fifo=%lu underruns=%lu err=%lu\n",
                       (unsigned long)++beat_seq,
                       (unsigned long)(now / 1000u),
                       audioOut.ready() ? "ready" : "none",
                       audioOut.alternateSetting(),
                       (unsigned long)(p - last_packets),
                       (unsigned long)audioOut.queued(),
                       (unsigned long)audioOut.underruns(),
                       (unsigned long)audioOut.transportErrors());
        last_packets = p;
    }
}
