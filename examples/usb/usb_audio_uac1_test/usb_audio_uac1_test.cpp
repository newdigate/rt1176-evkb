#include "Arduino.h"
#include "HardwareSerial.h"   // the console UART -- not pulled in by USBHost_t36.h
#include "USBHost_t36.h"

// USB Host gate for the MIMXRT1170-EVKB and the MIMXRT1060-EVKB: UAC1 (USB
// Audio Class 1.0) enumeration over USBHost_t36 on USB_OTG2 / USBPHY2.  Markers
// go out over the console UART via the CONSOLE alias below -- Serial1 on
// imxrt1176, Serial6 on teensy4, LPUART1 (the board VCOM) on both.
//
// This is the M0/M1 gate for the USB audio work: it answers whether a
// FULL-SPEED device enumerates when directly attached to the root port.
// Per RT1176 RM 62.5.4.1 the controller has an embedded Transaction Translator;
// the quoted phrase "supports direct connection of a HS/FS/LS device" is from
// 62.3.1.1 (Host Mode), not 62.5.4.1 -- two sections, one claim. The actual
// speed is reported by PORTSC.PSPD, which ehci.cpp already reads.
//
// It prints a once-per-second HEARTBEAT whether or not a device is attached, so
// silence on the console always means "firmware stopped", never "nothing
// plugged in".  The heartbeat also gives captures a timestamp reference to
// correlate against.
//
// DMAMEM: on RT1176 plain .bss is DTCM, which the EHCI DMA master cannot reach.
// USBAudioOut carries DMA-visible members (setup_t setup, Transfer_t
// mytransfers[2]), so the whole object must live in OCRAM -- same trap as
// hid1/keyboard1 in usb_host_hid_test.  USBHub likewise carries member DMA
// buffers.  myusb has no instance data members, so it correctly stays in DTCM.
// The console is LPUART1 on BOTH boards. The two cores just name it
// differently: cores/imxrt1176 calls LPUART1 `Serial1`, while cores/teensy4
// follows the Teensy pin-0/1 convention and calls LPUART6 `Serial1` and LPUART1
// `Serial6`. Naming it once here is what keeps QEMU and silicon reading the same
// wire -- on the MIMXRT1060-EVKB, LPUART1 (GPIO_AD_B0_12/13) is the DAPLink VCOM,
// whereas LPUART6 only reaches Arduino header pins D0/D1.
#if defined(ARDUINO_MIMXRT1060_EVKB)
#define CONSOLE Serial6
#else
#define CONSOLE Serial1
#endif

// Requested sample rate. 48000 and not the Audio library's 44100 on purpose:
// QEMU's usb-audio model offers 48000 ONLY (USBAUDIO_SAMPLE_RATE is a
// compile-time #define in hw/usb/dev-audio.c with no property to change it), and
// gate builds share build/ and build-rt1062/ with silicon builds -- so there is
// no gate-only override to hide a difference in. The J47 device supports both
// rates, so one binary claims in QEMU and plays on the bench.
//
// Phase 5's capstone will want 44100 back, to match the Audio library. This is
// the knob it turns.
#define UAC1_RATE_HZ 48000u

USBHost              myusb;
DMAMEM USBHub        hub1(myusb);
DMAMEM USBAudioOut   audioOut(myusb);

USBDriver *drivers[] = { &hub1, &audioOut };
const char *driver_names[] = { "Hub", "AudioOut" };
bool driver_active[] = { false, false };
#define NDRIVERS (sizeof(drivers) / sizeof(drivers[0]))

static bool     topology_reported = false;
static bool     packet_posted     = false;
static bool     status_reported   = false;
static uint32_t post_ms           = 0;
static bool     stream_started    = false;
static uint32_t last_packets      = 0;
static uint32_t attach_count = 0;
static uint32_t last_beat = 0;
static uint32_t beat_seq = 0;

// String descriptors are optional; the accessors return nullptr when absent.
static const char *str_or(const uint8_t *s, const char *fallback) {
    return (s && *s) ? (const char *)s : fallback;
}

static void report_topology(void) {
    const UAC1Topology &t = audioOut.topology();
    CONSOLE.printf("UAC1-TEST: bcdADC=%x.%02x control_if=%d streaming_if=%d feature_unit=%d\n",
                   t.bcd_adc >> 8, t.bcd_adc & 0xFF, t.control_interface,
                   t.streaming_interface, t.feature_unit_id);
    for (uint8_t i = 0; i < t.alt_count; i++) {
        const UAC1AltSetting &a = t.alts[i];
        // Rates: a discrete list, or a continuous range when rate_count is 0.
        // Both idioms occur -- this device lists one rate per alt setting, the
        // Jabra 0B0E:2301 lists five in a single one.
        CONSOLE.printf("UAC1-TEST:   alt %d ep=0x%02X attr=0x%02X mps=%d ch=%d bits=%d rates=",
                       a.alternate_setting, a.endpoint_address, a.endpoint_attributes,
                       a.max_packet_size, a.channels, a.bit_resolution);
        if (a.rate_count == 0) {
            CONSOLE.printf("%lu..%lu (continuous)",
                           (unsigned long)a.rate_min, (unsigned long)a.rate_max);
        } else {
            for (uint8_t r = 0; r < a.rate_count; r++)
                CONSOLE.printf("%s%lu", r ? "," : "", (unsigned long)a.rates[r]);
        }
        CONSOLE.println();
    }
    CONSOLE.printf("UAC1-TEST: selected alt=%d\n", audioOut.alternateSetting());
    CONSOLE.println("UAC1-TEST: PASS");
}

void setup() {
    CONSOLE.begin(115200);
    while (!CONSOLE) {}
    CONSOLE.println("UAC1-TEST: start");
    audioOut.format(UAC1_RATE_HZ, 2, 16);   // see UAC1_RATE_HZ above
    myusb.begin();
    CONSOLE.println("UAC1-TEST: host started, waiting for device");
    last_beat = millis();
}

void loop() {
    myusb.Task();

    // Per-driver connect/disconnect edges, with identity.  operator bool() on
    // USBDriver reports whether it is currently bound to a device.
    for (unsigned i = 0; i < NDRIVERS; i++) {
        bool now_active = (bool)(*drivers[i]);
        if (now_active == driver_active[i]) continue;
        driver_active[i] = now_active;

        if (now_active) {
            attach_count++;
            CONSOLE.printf("UAC1-TEST: + %s vid=%04X pid=%04X mfg=\"%s\" prod=\"%s\" serial=\"%s\"\n",
                           driver_names[i], drivers[i]->idVendor(), drivers[i]->idProduct(),
                           str_or(drivers[i]->manufacturer(), "?"),
                           str_or(drivers[i]->product(), "?"),
                           str_or(drivers[i]->serialNumber(), "?"));
        } else {
            CONSOLE.printf("UAC1-TEST: - %s detached\n", driver_names[i]);
            if (i == 1) {
                audioOut.stopStreaming();
                topology_reported = false;
                packet_posted = false;
                status_reported = false;
                stream_started = false;
            }
        }
    }

    if (audioOut.ready() && !topology_reported) {
        CONSOLE.println("UAC1-TEST: DEVICE READY");
        report_topology();
        topology_reported = true;
    }

    // Task 4: post ONE isochronous OUT packet and let the controller run it.
    // One 1 ms frame of stereo 16-bit at UAC1_RATE_HZ, so the payload scales
    // with the rate: 4 bytes per sample x ceil(UAC1_RATE_HZ/1000) samples. That
    // is 192 bytes at the 48 kHz set above, and 180 at 44.1 kHz -- 45 samples,
    // NOT 44.1: a 44.1 kHz stream sends 45-sample frames with a 44 every tenth,
    // so the max packet is the rounded-up one. Drop the round-up and you get
    // 176.4, which is why this spells it out.
    // 0xA5 fill so the payload is recognisable if it is ever captured.
    if (audioOut.ready() && topology_reported && !packet_posted) {
        audioOut.fillTestBuffer(0xA5);
        if (audioOut.postTestPacket(180)) {
            CONSOLE.println("UAC1-TEST: siTD posted, 180 bytes");
            post_ms = millis();
        } else {
            CONSOLE.println("UAC1-TEST: siTD POST FAILED");
            status_reported = true;   // nothing to read back
        }
        packet_posted = true;
    }

    // Read the controller's writeback. This is the verification that matters:
    // the hardware reporting what it actually did, rather than us inferring it
    // from a logic capture. 50 ms is ~50 frames, far longer than needed.
    if (packet_posted && !status_reported && (uint32_t)(millis() - post_ms) > 50u) {
        sitd_status_t st;
        if (audioOut.testPacketStatus(&st)) {
            CONSOLE.printf("UAC1-TEST: siTD active=%d xact_err=%d babble=%d buf_err=%d bytes_left=%u\n",
                           st.active ? 1 : 0, st.err_transaction ? 1 : 0,
                           st.err_babble ? 1 : 0, st.err_buffer ? 1 : 0,
                           (unsigned)st.bytes_left);
            bool ok = !st.active && !st.err_transaction && !st.err_babble
                   && !st.err_buffer && st.bytes_left == 0;
            CONSOLE.println(ok ? "UAC1-TEST: SITD PASS - controller sent the packet"
                               : "UAC1-TEST: SITD FAIL - see flags above");
        }
        status_reported = true;
    }

    // Task 5: once the single packet has proven the path, stream continuously.
    // One siTD per periodic slot -- a slot is only revisited every 32 frames,
    // so a shorter ring would transmit in just that fraction of them.
    if (status_reported && !stream_started && audioOut.ready()) {
        audioOut.tone(1000);                 // 1 kHz, audible
        if (audioOut.beginStreaming()) {
            CONSOLE.println("UAC1-TEST: streaming started, 1 kHz tone");
        } else {
            CONSOLE.println("UAC1-TEST: STREAM START FAILED");
        }
        stream_started = true;
        last_packets = 0;
    }

    audioOut.service();

    // Heartbeat: unconditional, so silence means the firmware stopped.
    uint32_t now = millis();
    if ((uint32_t)(now - last_beat) >= 1000u) {
        last_beat = now;
        CONSOLE.printf("UAC1-TEST: HEARTBEAT seq=%lu up=%lus attaches=%lu audio=%s alt=%d",
                       (unsigned long)++beat_seq, (unsigned long)(now / 1000u),
                       (unsigned long)attach_count,
                       audioOut.ready() ? "ready" : "none",
                       audioOut.alternateSetting());
        for (unsigned i = 0; i < NDRIVERS; i++) {
            CONSOLE.printf(" %s=%c", driver_names[i], driver_active[i] ? 'Y' : 'n');
        }
        if (audioOut.streaming()) {
            // Packets per second is the correctness measure for the ring: with
            // one frame per packet it must sit near 1000, which follows from
            // the 1 ms USB frame alone and does NOT depend on UAC1_RATE_HZ --
            // changing the rate changes the bytes per packet, not the count.
            // Lower means frames went out empty because service() did not get
            // round the ring in time.
            uint32_t p = audioOut.packetsSent();
            CONSOLE.printf(" pkts/s=%lu total=%lu", (unsigned long)(p - last_packets),
                           (unsigned long)p);
            last_packets = p;
        }
        CONSOLE.println();
    }
}
