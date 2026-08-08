#include "Arduino.h"
#include "HardwareSerial.h"   // the console UART -- not pulled in by USBHost_t36.h
#include "USBHost_t36.h"

// USB descriptor survey for the MIMXRT1170-EVKB.
//
// Answers "what kind of audio device is this?" for any device you plug in,
// without trying to stream to it. Written because USBAudioOut only claims UAC1
// devices that offer a 44.1 kHz stereo 16-bit alternate setting -- anything
// else (a UAC2 DAC, a 24-bit-only interface) is silently not claimed, which on
// the console is indistinguishable from nothing being plugged in.
//
// It reports, per audio streaming alternate setting, the endpoint's
// synchronisation type -- adaptive, asynchronous or synchronous -- which is
// what determines how much work supporting the device would be. It also dumps
// the raw configuration descriptor as hex so it can be decoded in full on the
// host with `tools/usbcap.py decode`.
//
// This driver deliberately never binds: claim() copies the descriptor and
// returns false, so the device stays available to any real driver and the
// survey keeps working for every device in turn.
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

USBHost myusb;
DMAMEM USBHub hub1(myusb);

// --- descriptor capture ------------------------------------------------

static const uint32_t DESC_MAX = 1024;
static uint8_t  desc_buf[DESC_MAX];
static uint32_t desc_len;
static uint16_t desc_vid, desc_pid;
static volatile bool desc_pending;

// The report prints once, at enumeration -- which is a second or two after
// reset, before the flash script's console has finished attaching to the VCOM.
// That loses exactly the output the firmware exists to produce. So keep the
// last capture and re-print it periodically; a repeat is labelled as one so it
// is never mistaken for a second device.
static bool     desc_valid;
static uint32_t last_report;
static const uint32_t REPORT_REPEAT_MS = 15000;

class USBDescriptorSurvey : public USBDriver {
public:
	USBDescriptorSurvey(USBHost &host) { init(); }
	USBDescriptorSurvey(USBHost *host) { init(); }
protected:
	virtual bool claim(Device_t *dev, int type, const uint8_t *descriptors, uint32_t len) {
		// Only device-level, and only when the previous capture has been
		// printed -- dropping one is better than tearing a report in half.
		if (type != 0 || desc_pending) return false;

		desc_len = (len > DESC_MAX) ? DESC_MAX : len;
		for (uint32_t i = 0; i < desc_len; i++) desc_buf[i] = descriptors[i];
		desc_vid = dev->idVendor;
		desc_pid = dev->idProduct;
		desc_pending = true;

		// Never bind. Printing here would stall enumeration, and staying
		// unbound leaves the device available to a real driver and keeps
		// this one eligible for the next device.
		return false;
	}
	virtual void disconnect() {}
private:
	void init() { driver_ready_for_device(this); }
};

USBDescriptorSurvey survey(myusb);

// --- decoding ----------------------------------------------------------

static const char *sync_name(uint8_t attr) {
	switch ((attr >> 2) & 0x03) {
		case 0: return "none";
		case 1: return "ASYNCHRONOUS";
		case 2: return "adaptive";
		default: return "synchronous";
	}
}

static const char *usage_name(uint8_t attr) {
	switch ((attr >> 4) & 0x03) {
		case 0: return "data";
		case 1: return "FEEDBACK";
		case 2: return "implicit-fb";
		default: return "?";
	}
}

static const char *xfer_name(uint8_t attr) {
	static const char *n[] = { "control", "iso", "bulk", "interrupt" };
	return n[attr & 0x03];
}

static const char *class_name(uint8_t c) {
	switch (c) {
		case 0x01: return "AUDIO";
		case 0x03: return "HID";
		case 0x08: return "mass-storage";
		case 0x0B: return "smart-card";
		case 0x0E: return "video";
		case 0xFE: return "app-specific";
		case 0xFF: return "vendor";
		default:   return "other";
	}
}

// Walks the descriptor set by bLength. Deliberately class-version agnostic:
// it reports what it finds rather than requiring UAC1, so a UAC2 device is
// identified as UAC2 instead of vanishing.
static void report(bool repeat)
{
	// Nothing calls disconnect() -- claim() never binds -- so a repeat only
	// means "this is what was last plugged in", not "it is still there".
	CONSOLE.printf("SURVEY: device %04X:%04X, %lu descriptor bytes%s\n",
	               desc_vid, desc_pid, (unsigned long)desc_len,
	               repeat ? " (repeat of last capture)" : "");

	bool audio_seen = false, async_seen = false, feedback_seen = false;
	bool implicit_fb_seen = false;
	uint8_t cur_class = 0, cur_sub = 0;
	uint32_t i = 0;

	while (i + 1 < desc_len && desc_buf[i] >= 2 && i + desc_buf[i] <= desc_len) {
		const uint8_t *b = desc_buf + i;
		uint8_t len = b[0], t = b[1];

		if (t == 0x04 && len >= 9) {                 // INTERFACE
			cur_class = b[5];
			cur_sub   = b[6];
			CONSOLE.printf("SURVEY:  if %d alt %d  class=0x%02X(%s) sub=0x%02X eps=%d\n",
			               b[2], b[3], cur_class, class_name(cur_class), cur_sub, b[4]);
			if (cur_class == 0x01) audio_seen = true;

		} else if (t == 0x24 && len >= 3 && cur_class == 0x01) {  // CS_INTERFACE
			if (cur_sub == 0x01 && b[2] == 0x01 && len >= 8) {    // AC HEADER
				uint16_t bcd = (uint16_t)b[3] | ((uint16_t)b[4] << 8);
				CONSOLE.printf("SURVEY:    *** UAC %d.%02d ***\n", bcd >> 8, bcd & 0xFF);
			} else if (cur_sub == 0x02 && b[2] == 0x02 && len >= 11) {  // FORMAT_TYPE
				uint8_t nfreq = b[7];
				CONSOLE.printf("SURVEY:    format: %d ch, %d-bit, %d rate(s):",
				               b[4], b[6], nfreq);
				for (uint8_t k = 0; k < nfreq && (8u + 3u * k + 2u) < len; k++) {
					uint32_t o = 8 + 3 * k;
					CONSOLE.printf(" %lu", (unsigned long)(b[o] | (b[o+1] << 8) | (b[o+2] << 16)));
				}
				if (nfreq == 0) CONSOLE.print(" (continuous range)");
				CONSOLE.println();
			}

		} else if (t == 0x05 && len >= 7) {          // ENDPOINT
			uint8_t attr = b[3];
			CONSOLE.printf("SURVEY:    ep 0x%02X %-3s %-9s sync=%-12s usage=%-11s mps=%d",
			               b[2], (b[2] & 0x80) ? "IN" : "OUT",
			               xfer_name(attr), sync_name(attr), usage_name(attr),
			               (uint16_t)b[4] | ((uint16_t)b[5] << 8));
			// bLength 9 is the audio endpoint descriptor: it adds bRefresh and
			// bSynchAddress, which the 7-byte standard descriptor lacks.
			if (len >= 9) {
				CONSOLE.printf(" refresh=%d synchAddr=0x%02X", b[7], b[8]);
			}
			CONSOLE.println();
			if (cur_class == 0x01 && (attr & 0x03) == 0x01) {
				if (((attr >> 2) & 0x03) == 1) async_seen = true;
				if (((attr >> 4) & 0x03) == 1) feedback_seen = true;
				// The authoritative way to find a feedback endpoint. lib_xua
				// leaves the feedback endpoint's own usage bits at 00 (data),
				// so testing usage type alone reports "no feedback" on a device
				// that plainly has one -- it is named right here instead.
				if (len >= 9 && b[8] != 0) feedback_seen = true;
				// Implicit feedback is a feedback mechanism too: the rate is
				// carried by the IN stream's packet sizes rather than by a
				// dedicated endpoint. Counting only usage==FEEDBACK reported
				// such devices as having no feedback at all.
				if (((attr >> 4) & 0x03) == 2) implicit_fb_seen = true;
			}
		}
		i += len;
	}

	// The headline, so the answer does not have to be read out of the table.
	if (!audio_seen) {
		CONSOLE.println("SURVEY: VERDICT not an audio device");
	} else if (async_seen && feedback_seen) {
		CONSOLE.println("SURVEY: VERDICT ASYNCHRONOUS with an explicit feedback endpoint -- usable for async work");
	} else if (async_seen && implicit_fb_seen) {
		CONSOLE.println("SURVEY: VERDICT ASYNCHRONOUS with implicit feedback -- usable for async work, but the rate must be recovered from IN packet sizes");
	} else if (async_seen) {
		CONSOLE.println("SURVEY: VERDICT asynchronous, no feedback mechanism of either kind found");
	} else {
		CONSOLE.println("SURVEY: VERDICT adaptive/synchronous only -- cannot exercise async");
	}

	// Raw bytes, for tools/usbcap.py decode to parse in full on the host.
	CONSOLE.print("SURVEY-HEX:");
	for (uint32_t k = 0; k < desc_len; k++) CONSOLE.printf(" %02X", desc_buf[k]);
	CONSOLE.println();
	CONSOLE.println("SURVEY: end");
}

// --- sketch ------------------------------------------------------------

static uint32_t last_beat;
static uint32_t beat_seq;

void setup() {
	CONSOLE.begin(115200);
	while (!CONSOLE) {}
#ifdef USBHOST_PRINT_DEBUG
	// Bench diagnosis only. USBHost_t36's debug narration happens almost
	// entirely INSIDE myusb.begin(), which on a flash-and-run lands seconds
	// before any console can attach -- so the interesting output scrolls past
	// and all you see is the heartbeat. Hold here long enough to attach a
	// reader after the flash tool has let go of the VCOM.
	//
	// This exists because resetting the board to catch boot output is awkward
	// here: on the MIMXRT1170-EVKB that is SW4 (POR), but the MIMXRT1060-EVKB
	// does not have an SW4, and resetting via LinkServer with a reader attached
	// is the documented route to a HOST KERNEL PANIC (see CLAUDE.md). A delay
	// needs no button and no probe.
	//
	// Guarded, so the gate build is byte-identical -- verified, not assumed.
	for (int i = 20; i > 0; i--) {
		CONSOLE.printf("SURVEY: debug build -- attach console, starting in %d\n", i);
		delay(1000);
	}
#endif
	CONSOLE.println("SURVEY: start -- plug in any USB device");
	myusb.begin();
	last_beat = millis();
}

void loop() {
	myusb.Task();

	uint32_t now = millis();

	if (desc_pending) {
		report(false);
		desc_pending = false;
		desc_valid   = true;
		last_report  = now;
	} else if (desc_valid && (uint32_t)(now - last_report) >= REPORT_REPEAT_MS) {
		report(true);
		last_report = now;
	}
	if ((uint32_t)(now - last_beat) >= 2000u) {
		last_beat = now;
		CONSOLE.printf("SURVEY: waiting seq=%lu up=%lus\n",
		               (unsigned long)++beat_seq, (unsigned long)(now / 1000u));
	}
}
