#include "Arduino.h"
#include "HardwareSerial.h"
#include "USBHost_t36.h"
#include "utility/imxrt_usbhs.h"   // USBHS_PORTSC1, for port= in the heartbeat
#include "AudioStream.h"
#include "synth_sine.h"
#include "analyze_peak.h"
#include "output_i2s.h"
#include "output_usbhost.h"
#include "input_usbhost.h"
#include "control_wm8960.h"

// THE CAPSTONE. Both audio directions, in one graph, on one board:
//
//   sine 1 kHz ---> AudioOutputUSBHost ---> USB OUT ---> adapter DAC
//                                                            |
//                                                    [loopback cable]
//                                                            v
//   WM8960 <--- AudioOutputI2S <--- AudioInputUSBHost <--- adapter ADC
//
// The MIMXRT1060-EVKB can do what the RT1176 capstone could not: it has a
// working on-board codec on the SAME board as the USB host port. So the tone
// this firmware plays out over USB comes back in through the adapter's own
// microphone input and leaves through the board's own line out. Hearing it is
// end-to-end proof of both directions plus the graph in between.
//
// rt1062-only (see the `boards` sidecar): the RT1176 already has its own
// capstone in dualcore/cm4_graph_usb_capstone.
//
// ★ QEMU CANNOT PROVE THE ROUND TRIP, AND REACHES LESS OF THIS SKETCH THAN IT
// LOOKS LIKE. Two separate limits, measured rather than assumed:
//
//  1. Its usb-audio model is playback-only and isochronous data does not flow
//     against it at all, so `usbIn` finds an empty FIFO forever and feeds the
//     codec real silence. The SAI tap is therefore hundreds of KB of zeroes --
//     which is a LIVE graph, not a dead one, and the distinction is the whole
//     value of the tap.
//  2. That model advertises ONE rate, 48000 (dev-audio.c USBAUDIO_SAMPLE_RATE),
//     and this graph runs at 44100. AudioOutputUSBHost's constructor pins the
//     OUT side to the graph rate by construction -- deliberately, because a
//     mismatch there is a permanent 8.8% pitch error, not a caveat -- so
//     uac1_find_alt() finds no matching alt and claim() returns FALSE. In QEMU
//     this device is never claimed at all: no "+ Audio" line, out=none forever.
//     The 44100 is correct and stays; it is what the J47 bench adapter and the
//     WM8960 both run at. It just means QEMU sees the graph and the SAI, and
//     sees the USB host stack decline a device it cannot play to.
//
// Silicon is the sole proof audio moves. See transcript_hw_evkb.txt.

// LPUART1 -- the EVKB's DAPLink VCOM. cores/teensy4 follows the Teensy
// pin-0/1 convention and gives the name Serial1 to LPUART6, which on this
// board only reaches Arduino header pins D0/D1. There is no rt1176 arm here
// because this example is rt1062-only.
#define CONSOLE Serial6

// The bench adapter (GeneralPlus 1B3F:2008) captures 1ch/16 at 44100 or 48000
// -- read off the wire by usb/usb_descriptor_survey on this very board. The
// driver refuses to approximate a format the device never offered, so these
// must match an alt exactly. 44100 is what the graph runs at
// (AUDIO_SAMPLE_RATE_EXACT), which is also what AudioOutputUSBHost pins the
// OUT side to in its constructor, so both directions use the device's one
// converter clock.
#ifndef CAPSTONE_IN_RATE_HZ
#define CAPSTONE_IN_RATE_HZ 44100
#endif
#ifndef CAPSTONE_IN_CHANNELS
#define CAPSTONE_IN_CHANNELS 1
#endif
#ifndef CAPSTONE_IN_BITS
#define CAPSTONE_IN_BITS 16
#endif

USBHost              myusb;
DMAMEM USBHub        hub1(myusb);
DMAMEM USBAudioOut   audioOut(myusb);

// ★ DECLARATION ORDER IS LOAD-BEARING, AND THIS IS THE WHOLE CLOCKING DESIGN.
//
// AudioStream::update_setup() is first-caller-wins (`if (update_scheduled)
// return false;`), and globals in one translation unit are constructed in
// declaration order. So AudioOutputI2S, declared FIRST, owns the graph's
// clock -- its SAI TX DMA interrupt paces update_all(), which is the same
// arrangement audio/audiooutput_i2s_test proved on this board in Phase 5a.
//
// AudioOutputUSBHost, declared after, gets `false` back from update_setup()
// and its frame_consumed() callback returns immediately on that -- so its
// FIFO-occupancy pacing self-disables and it degrades to a plain graph-paced
// FIFO writer. No change to that driver was needed to make this work; the
// guard was already there. Its dropped() counter then reports the drift: the
// USB FIFO gains about 7.6 samples/s against roughly 3300 samples of headroom
// above its target (USB_AUDIO_FIFO_SAMPLES 4096 - FIFO_TARGET_SAMPLES 768),
// so expect one counted drop every seven minutes or so, not zero and not many.
//
// SWAP THESE TWO LINES AND THE GRAPH RUNS ON THE WRONG CLOCK: USB would pace
// it, the SAI would starve, and the symptom would be at the codec, three nodes
// away from the cause.
AudioOutputI2S         i2sOut;
AudioSynthWaveformSine sine;
AudioOutputUSBHost     usbOut(audioOut);
AudioInputUSBHost      usbIn(audioOut);
AudioAnalyzePeak       inPeak;
AudioControlWM8960     codec;

AudioConnection pcOutL(sine,  0, usbOut, 0);   // OUT leg: tone to the device
AudioConnection pcOutR(sine,  0, usbOut, 1);
AudioConnection pcInL (usbIn, 0, i2sOut, 0);   // IN leg: what came back
AudioConnection pcInR (usbIn, 1, i2sOut, 1);
AudioConnection pcPeak(usbIn, 0, inPeak, 0);   // ...and how loud it was

static const char *driver_names[] = { "Hub", "Audio" };
static const unsigned NDRIVERS = 2;
static USBDriver *drivers[] = { &hub1, &audioOut };
static bool driver_active[NDRIVERS] = { false, false };

static uint32_t seq, last_beat_ms;
static bool     announced_ready;

void setup() {
    CONSOLE.begin(115200);
    while (!CONSOLE) {}
    CONSOLE.println("CAPSTONE: start");

    // Both legs plus the peak analyser; the IN leg allocates two blocks per
    // update and the I2S sink holds its own, so this is deliberately generous.
    AudioMemory(40);

    codec.enable();
    codec.volume(0.8f);
    sine.frequency(1000.0f);
    sine.amplitude(0.5f);

    // Turning the input path on at all. req_in_channels stays 0 until this is
    // called, and every input step -- the alt search in claim(), the extra
    // control requests, the IN ring -- is gated on it.
    audioOut.formatIn(CAPSTONE_IN_RATE_HZ, CAPSTONE_IN_CHANNELS, CAPSTONE_IN_BITS);

    myusb.begin();
    CONSOLE.println("CAPSTONE: host started, waiting for device");
    last_beat_ms = millis();
}

void loop() {
    myusb.Task();

    for (unsigned i = 0; i < NDRIVERS; i++) {
        bool now_active = (bool)*drivers[i];
        if (now_active == driver_active[i]) continue;
        driver_active[i] = now_active;
        if (now_active) {
            CONSOLE.printf("CAPSTONE: + %s vid=%04X pid=%04X\n",
                           driver_names[i], drivers[i]->idVendor(),
                           drivers[i]->idProduct());
        } else {
            CONSOLE.printf("CAPSTONE: - %s detached\n", driver_names[i]);
        }
    }

    if (!announced_ready && audioOut.ready()) {
        announced_ready = true;
        // alt= is a NEGOTIATION RESULT -- the driver matched the device's
        // declared alternate settings and selected one. rate= and ch= are NOT:
        // USBAudioOut::rate()/channels() return req_rate/req_channels, the
        // values AudioOutputUSBHost's constructor asked for, so printing them
        // is the firmware quoting itself. They are here to make the console
        // readable, and the gate does not assert them for that reason.
        // (There is no bits() getter; the request is 16 by construction.)
        CONSOLE.printf("CAPSTONE: OUT READY alt=%d rate=%lu ch=%d\n",
                       audioOut.alternateSetting(),
                       (unsigned long)audioOut.rate(),
                       audioOut.channels());
        // Input is selected by control requests that run AFTER the output
        // ones, so readyIn() can still be false here. The heartbeat's in=
        // field is what reports whether it ever became true.
        CONSOLE.println(audioOut.readyIn() ? "CAPSTONE: IN READY"
                                           : "CAPSTONE: IN not ready yet");
    }

    if (millis() - last_beat_ms >= 1000) {
        last_beat_ms += 1000;
        // in_peak is the loudness of what came BACK from the device. On
        // silicon with the loopback cable it tracks the tone; in QEMU, where
        // no capture data can exist, it is 0.0000 and underruns climbs at the
        // graph's update rate. Those two together are the emulated signature.
        float pk = inPeak.available() ? inPeak.read() : 0.0f;
        // ★ THE PEAK GOES THROUGH Print::print(float, digits), NOT printf.
        // MEASURED, not assumed: "%.4f" through Print::printf -> vdprintf
        // prints NOTHING for the conversion and then emits `precision` NUL
        // bytes as padding -- so `in_peak=%.4f` put "in_peak=" followed by
        // four 0x00 into the capture. Two harms, and the second is worse than
        // the first: the value a reader wants is simply absent, and the UART
        // capture stops being a text file, so grep treats it as binary.
        // Newlib's float formatter is not linked here (no -u _printf_float),
        // and no other example in this tree prints a float via printf --
        // audio/audiooutput_i2s_test, the Phase 5a sibling on this same board,
        // uses Print's own formatter, which is self-contained. Follow it.
        CONSOLE.printf("CAPSTONE: HEARTBEAT seq=%lu up=%lus out=%s in=%s in_peak=",
                       (unsigned long)++seq,
                       (unsigned long)(millis() / 1000),
                       audioOut.ready()   ? "ready" : "none",
                       audioOut.readyIn() ? "ready" : "none");
        CONSOLE.print(pk, 4);
        CONSOLE.printf(" out_drop=%lu in_under=%lu port=%08lX\n",
                       (unsigned long)usbOut.dropped(),
                       (unsigned long)usbIn.underruns(),
                       (unsigned long)USBHS_PORTSC1);
    }
}
