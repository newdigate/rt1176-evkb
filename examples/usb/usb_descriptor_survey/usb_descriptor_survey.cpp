#include "Arduino.h"
#include "HardwareSerial.h"   // Serial1 (LPUART1) -- not pulled in by USBHost_t36.h
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
	Serial1.printf("SURVEY: device %04X:%04X, %lu descriptor bytes%s\n",
	               desc_vid, desc_pid, (unsigned long)desc_len,
	               repeat ? " (repeat of last capture)" : "");

	bool audio_seen = false, async_seen = false, feedback_seen = false;
	uint8_t cur_class = 0, cur_sub = 0;
	uint32_t i = 0;

	while (i + 1 < desc_len && desc_buf[i] >= 2 && i + desc_buf[i] <= desc_len) {
		const uint8_t *b = desc_buf + i;
		uint8_t len = b[0], t = b[1];

		if (t == 0x04 && len >= 9) {                 // INTERFACE
			cur_class = b[5];
			cur_sub   = b[6];
			Serial1.printf("SURVEY:  if %d alt %d  class=0x%02X(%s) sub=0x%02X eps=%d\n",
			               b[2], b[3], cur_class, class_name(cur_class), cur_sub, b[4]);
			if (cur_class == 0x01) audio_seen = true;

		} else if (t == 0x24 && len >= 3 && cur_class == 0x01) {  // CS_INTERFACE
			if (cur_sub == 0x01 && b[2] == 0x01 && len >= 8) {    // AC HEADER
				uint16_t bcd = (uint16_t)b[3] | ((uint16_t)b[4] << 8);
				Serial1.printf("SURVEY:    *** UAC %d.%02d ***\n", bcd >> 8, bcd & 0xFF);
			} else if (cur_sub == 0x02 && b[2] == 0x02 && len >= 11) {  // FORMAT_TYPE
				uint8_t nfreq = b[7];
				Serial1.printf("SURVEY:    format: %d ch, %d-bit, %d rate(s):",
				               b[4], b[6], nfreq);
				for (uint8_t k = 0; k < nfreq && (8u + 3u * k + 2u) < len; k++) {
					uint32_t o = 8 + 3 * k;
					Serial1.printf(" %lu", (unsigned long)(b[o] | (b[o+1] << 8) | (b[o+2] << 16)));
				}
				if (nfreq == 0) Serial1.print(" (continuous range)");
				Serial1.println();
			}

		} else if (t == 0x05 && len >= 7) {          // ENDPOINT
			uint8_t attr = b[3];
			Serial1.printf("SURVEY:    ep 0x%02X %-3s %-9s sync=%-12s usage=%-11s mps=%d\n",
			               b[2], (b[2] & 0x80) ? "IN" : "OUT",
			               xfer_name(attr), sync_name(attr), usage_name(attr),
			               (uint16_t)b[4] | ((uint16_t)b[5] << 8));
			if (cur_class == 0x01 && (attr & 0x03) == 0x01) {
				if (((attr >> 2) & 0x03) == 1) async_seen = true;
				if (((attr >> 4) & 0x03) == 1) feedback_seen = true;
			}
		}
		i += len;
	}

	// The headline, so the answer does not have to be read out of the table.
	if (!audio_seen) {
		Serial1.println("SURVEY: VERDICT not an audio device");
	} else if (async_seen && feedback_seen) {
		Serial1.println("SURVEY: VERDICT ASYNCHRONOUS with a feedback endpoint -- usable for async work");
	} else if (async_seen) {
		Serial1.println("SURVEY: VERDICT asynchronous but no feedback endpoint found");
	} else {
		Serial1.println("SURVEY: VERDICT adaptive/synchronous only -- cannot exercise async");
	}

	// Raw bytes, for tools/usbcap.py decode to parse in full on the host.
	Serial1.print("SURVEY-HEX:");
	for (uint32_t k = 0; k < desc_len; k++) Serial1.printf(" %02X", desc_buf[k]);
	Serial1.println();
	Serial1.println("SURVEY: end");
}

// --- sketch ------------------------------------------------------------

static uint32_t last_beat;
static uint32_t beat_seq;

void setup() {
	Serial1.begin(115200);
	while (!Serial1) {}
	Serial1.println("SURVEY: start -- plug in any USB device");
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
		Serial1.printf("SURVEY: waiting seq=%lu up=%lus\n",
		               (unsigned long)++beat_seq, (unsigned long)(now / 1000u));
	}
}
