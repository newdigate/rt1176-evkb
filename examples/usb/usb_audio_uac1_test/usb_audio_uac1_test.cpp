#include "Arduino.h"
#include "HardwareSerial.h"   // Serial1 (LPUART1) -- not pulled in by USBHost_t36.h
#include "USBHost_t36.h"

// RT1176 EVKB USB Host gate: UAC1 (USB Audio Class 1.0) enumeration over
// USBHost_t36 on USB_OTG2 / USBPHY2.  Markers go out over Serial1 (LPUART1 /
// VCOM).
//
// This is the M0/M1 gate for the USB audio work: it answers whether a
// FULL-SPEED device enumerates when directly attached to the RT1176 root port.
// Per RM 62.5.4.1 the controller has an embedded Transaction Translator and
// "supports direct connection of a HS/FS/LS device", with the actual speed
// reported by PORTSC.PSPD -- which ehci.cpp already reads.
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
USBHost              myusb;
DMAMEM USBHub        hub1(myusb);
DMAMEM USBAudioOut   audioOut(myusb);

USBDriver *drivers[] = { &hub1, &audioOut };
const char *driver_names[] = { "Hub", "AudioOut" };
bool driver_active[] = { false, false };
#define NDRIVERS (sizeof(drivers) / sizeof(drivers[0]))

static bool     topology_reported = false;
static uint32_t attach_count = 0;
static uint32_t last_beat = 0;
static uint32_t beat_seq = 0;

// String descriptors are optional; the accessors return nullptr when absent.
static const char *str_or(const uint8_t *s, const char *fallback) {
    return (s && *s) ? (const char *)s : fallback;
}

static void report_topology(void) {
    const UAC1Topology &t = audioOut.topology();
    Serial1.printf("UAC1-TEST: bcdADC=%x.%02x control_if=%d streaming_if=%d feature_unit=%d\n",
                   t.bcd_adc >> 8, t.bcd_adc & 0xFF, t.control_interface,
                   t.streaming_interface, t.feature_unit_id);
    for (uint8_t i = 0; i < t.alt_count; i++) {
        const UAC1AltSetting &a = t.alts[i];
        Serial1.printf("UAC1-TEST:   alt %d ep=0x%02X attr=0x%02X mps=%d ch=%d bits=%d rate=%lu\n",
                       a.alternate_setting, a.endpoint_address, a.endpoint_attributes,
                       a.max_packet_size, a.channels, a.bit_resolution,
                       (unsigned long)a.sample_rate);
    }
    Serial1.printf("UAC1-TEST: selected alt=%d\n", audioOut.alternateSetting());
    Serial1.println("UAC1-TEST: PASS");
}

void setup() {
    Serial1.begin(115200);
    while (!Serial1) {}
    Serial1.println("UAC1-TEST: start");
    audioOut.format(48000, 2, 16);   // bring-up target: 48k stereo 16-bit
    myusb.begin();
    Serial1.println("UAC1-TEST: host started, waiting for device");
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
            Serial1.printf("UAC1-TEST: + %s vid=%04X pid=%04X mfg=\"%s\" prod=\"%s\" serial=\"%s\"\n",
                           driver_names[i], drivers[i]->idVendor(), drivers[i]->idProduct(),
                           str_or(drivers[i]->manufacturer(), "?"),
                           str_or(drivers[i]->product(), "?"),
                           str_or(drivers[i]->serialNumber(), "?"));
        } else {
            Serial1.printf("UAC1-TEST: - %s detached\n", driver_names[i]);
            if (i == 1) topology_reported = false;
        }
    }

    if (audioOut.ready() && !topology_reported) {
        Serial1.println("UAC1-TEST: DEVICE READY");
        report_topology();
        topology_reported = true;
    }

    // Heartbeat: unconditional, so silence means the firmware stopped.
    uint32_t now = millis();
    if ((uint32_t)(now - last_beat) >= 1000u) {
        last_beat = now;
        Serial1.printf("UAC1-TEST: HEARTBEAT seq=%lu up=%lus attaches=%lu audio=%s alt=%d",
                       (unsigned long)++beat_seq, (unsigned long)(now / 1000u),
                       (unsigned long)attach_count,
                       audioOut.ready() ? "ready" : "none",
                       audioOut.alternateSetting());
        for (unsigned i = 0; i < NDRIVERS; i++) {
            Serial1.printf(" %s=%c", driver_names[i], driver_active[i] ? 'Y' : 'n');
        }
        Serial1.println();
    }
}
