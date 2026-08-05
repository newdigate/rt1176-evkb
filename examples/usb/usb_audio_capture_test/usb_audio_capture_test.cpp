#include "Arduino.h"
#include "HardwareSerial.h"   // Serial1 (LPUART1) -- not pulled in by USBHost_t36.h
#include "USBHost_t36.h"

// USB audio INPUT on the RT1176 host port: record from a class-compliant
// device instead of playing to one.
//
// This is Stage B of docs/superpowers/specs/2026-08-05-uac-host-input-and-
// duplex-design.md -- input alone, with nothing streaming out, which is what
// proves the transport, the FIFO discipline and the descriptor budget before
// clock ownership is touched. beginStreaming() is deliberately never called.
//
// WHAT MAKES THIS VERIFIABLE WITHOUT A SECOND INSTRUMENT
//
// Every earlier capability on this port was judged by a device that had been
// patched to report on itself (the lib_xua observer). Input needs no such
// thing, because the arithmetic is closed:
//
//   bytes/s = rate x channels x subslot
//
// At 44.1 kHz, 8 channels, 4-byte subslots that is 1,411,200 B/s, and the
// heartbeat below derives a sample rate back out of the byte counter. A ring
// that skipped slots, a harvest that lost microframes, or a device at the
// wrong rate all move that number, and none of them can move it back. The
// derived rate is therefore the primary check and is printed every beat.
//
// The second check is the recording itself. With a tone patched into the
// device's analogue input, PEAK/RMS and the zero-crossing frequency estimate
// say whether what arrived is that tone. With nothing patched in, they read
// the converter's noise floor -- which is still a fact about the ADC and not
// about this firmware, so a dead-silent zero everywhere means the samples
// never came from a converter at all.

USBHost myusb;
DMAMEM USBHub hub1(myusb);
DMAMEM USBAudioOut audio(myusb);

// The MC200 witness captures 8 channels of 24-in-4 and offers no other input
// format; the parser records exactly what the device advertises, and
// formatIn() must match it or no alt is found. Duplex is not symmetric --
// this same device PLAYS 16-bit as well.
#ifndef CAPTURE_RATE_HZ
#define CAPTURE_RATE_HZ 44100u
#endif
#ifndef CAPTURE_CHANNELS
#define CAPTURE_CHANNELS 8
#endif
#ifndef CAPTURE_BITS
#define CAPTURE_BITS 24
#endif
// How many of the device's channels reach the FIFO. Two is not a
// simplification: eight channels at 44.1 kHz fill the 4096-sample FIFO in
// 11.6 ms, inside the ring's own 32 ms revolution, so the consumer's deadline
// would be shorter than the producer's.
#ifndef CAPTURE_TAKE_CHANNELS
#define CAPTURE_TAKE_CHANNELS 2
#endif

// The claim still needs an OUTPUT alt to succeed -- uac1_parse_config and
// uac2_parse_config both return false for a device with no iso OUT endpoint,
// a limitation recorded in the design spec rather than worked around here.
// Selecting it costs nothing while beginStreaming() is never called.
#ifndef PLAYBACK_RATE_HZ
#define PLAYBACK_RATE_HZ 44100u
#endif

static USBDriver *drivers[] = {&hub1, &audio};
static const char *driver_names[] = {"Hub", "Audio"};
static bool driver_active[] = {false, false};
#define NDRIVERS (sizeof(drivers) / sizeof(drivers[0]))

static bool recording_started = false;
static uint32_t last_beat = 0, seq = 0, started_at = 0;
static uint32_t last_packets = 0, last_bytes = 0, last_beat_ms = 0;

// Analysis window. 1024 frames is 23 ms at 44.1 kHz -- ten cycles of a
// 440 Hz tone, enough for a stable zero-crossing estimate, and small enough
// to hex-dump over a 115200 console in about a second.
#define WINDOW_FRAMES 1024
static int16_t window[WINDOW_FRAMES * CAPTURE_TAKE_CHANNELS];
static uint32_t window_used = 0;
static bool dumped = false;

// Drain whatever has arrived. The FIFO is the only backstop between the ring
// and this loop, so it must be emptied every pass whether or not the window
// wants more -- a full FIFO is an overrun, and overruns are the sketch's
// fault, not the transport's.
static void drainCapture()
{
	int16_t scratch[256];
	for (;;) {
		uint32_t n = audio.read(scratch, sizeof(scratch) / sizeof(scratch[0]));
		if (n == 0) break;
		if (window_used < sizeof(window) / sizeof(window[0])) {
			uint32_t room = sizeof(window) / sizeof(window[0]) - window_used;
			uint32_t take = n < room ? n : room;
			memcpy(window + window_used, scratch, take * sizeof(int16_t));
			window_used += take;
		}
	}
}

// Peak, RMS and a zero-crossing frequency estimate for one channel of the
// window. Crude on purpose: it runs on the device, needs no host tooling, and
// is more than sharp enough to tell a 440 Hz tone from a noise floor.
static void analyseWindow(uint8_t ch)
{
	const uint32_t stride = CAPTURE_TAKE_CHANNELS;
	uint32_t frames = window_used / stride;
	if (frames < 64) {
		Serial1.printf("CAPTURE: ANALYSIS ch%u -- only %lu frames, skipped\n",
		               ch, (unsigned long)frames);
		return;
	}

	int32_t peak = 0;
	uint64_t sumsq = 0;
	uint32_t crossings = 0;
	int16_t prev = window[ch];
	for (uint32_t f = 0; f < frames; f++) {
		int16_t v = window[f * stride + ch];
		int32_t a = v < 0 ? -(int32_t)v : (int32_t)v;
		if (a > peak) peak = a;
		sumsq += (uint64_t)((int32_t)v * (int32_t)v);
		// Hysteresis of one LSB either side of zero would be better; at a
		// noise floor this over-counts, which is why the RMS is printed
		// beside it and neither is read alone.
		if ((prev < 0 && v >= 0) || (prev >= 0 && v < 0)) crossings++;
		prev = v;
	}
	uint32_t rms = (uint32_t)sqrtf((float)((double)sumsq / frames));
	// Two crossings per cycle.
	float hz = (float)crossings * (float)CAPTURE_RATE_HZ / (2.0f * (float)frames);

	Serial1.printf("CAPTURE: ANALYSIS ch%u frames=%lu peak=%ld rms=%lu "
	               "crossings=%lu est=%d.%02d Hz\n",
	               ch, (unsigned long)frames, (long)peak, (unsigned long)rms,
	               (unsigned long)crossings, (int)hz,
	               (int)((hz - (int)hz) * 100.0f));
}

// One-shot hex dump of the window, delimited so a script can lift it out of a
// console log. Little-endian int16 pairs, interleaved exactly as read().
static void dumpWindow()
{
	Serial1.printf("\nCAPTURE-DUMP samples=%lu channels=%u rate=%lu\n",
	               (unsigned long)window_used, (unsigned)CAPTURE_TAKE_CHANNELS,
	               (unsigned long)CAPTURE_RATE_HZ);
	for (uint32_t i = 0; i < window_used; i++) {
		Serial1.printf("%04x", (uint16_t)window[i]);
		if ((i & 31) == 31) Serial1.println();
	}
	Serial1.println("\nCAPTURE-DUMP-END");
}

void setup() {
	Serial1.begin(115200);
	while (!Serial1) {}
	// Versioned banner so a gate cannot pass against some other image that
	// happens to be on the board.
	Serial1.println("CAPTURE-GATE v1");
	Serial1.println("CAPTURE-TEST: start");
	Serial1.printf("CAPTURE-TEST: requesting IN %lu Hz %u ch %u bit, taking %u ch\n",
	               (unsigned long)CAPTURE_RATE_HZ, (unsigned)CAPTURE_CHANNELS,
	               (unsigned)CAPTURE_BITS, (unsigned)CAPTURE_TAKE_CHANNELS);

	audio.format(PLAYBACK_RATE_HZ, 2, 16);
	audio.formatIn(CAPTURE_RATE_HZ, CAPTURE_CHANNELS, CAPTURE_BITS);
	audio.captureChannels(CAPTURE_TAKE_CHANNELS);

	myusb.begin();
	last_beat = millis();
}

void loop() {
	myusb.Task();

	for (unsigned i = 0; i < NDRIVERS; i++) {
		if (*drivers[i] != driver_active[i]) {
			driver_active[i] = !driver_active[i];
			if (driver_active[i]) {
				Serial1.printf("CAPTURE-TEST: + %s vid=%04X pid=%04X\n", driver_names[i],
				               drivers[i]->idVendor(), drivers[i]->idProduct());
			} else {
				Serial1.printf("CAPTURE-TEST: - %s detached\n", driver_names[i]);
				recording_started = false;
			}
		}
	}

	// readyIn(), not ready(): the input interface is selected by control
	// requests that run after the output ones finish, so there is a window
	// where the device is playable and not yet recordable.
	if (!recording_started && audio.readyIn()) {
		const UAC1Topology &t = audio.topology();
		Serial1.printf("CAPTURE-TEST: input ready, iface=%u alt=%d uac2=%d\n",
		               t.input_streaming_interface, audio.inAlternateSetting(),
		               (int)audio.isUAC2());
		for (uint8_t i = 0; i < t.in_alt_count; i++) {
			if (t.in_alts[i].alternate_setting != (uint8_t)audio.inAlternateSetting())
				continue;
			Serial1.printf("CAPTURE-TEST: input ep=0x%02X attr=0x%02X mps=%u "
			               "%uch x %uB (%u bit) clock=%u\n",
			               t.in_alts[i].endpoint_address,
			               t.in_alts[i].endpoint_attributes,
			               t.in_alts[i].max_packet_size, t.in_alts[i].channels,
			               t.in_alts[i].subframe_size, t.in_alts[i].bit_resolution,
			               t.in_clock_source_id);
		}
		Serial1.println(audio.beginRecording() ? "CAPTURE-TEST: recording started"
		                                       : "CAPTURE-TEST: RECORD START FAILED");
		recording_started = true;
		started_at = millis();
		last_packets = 0;
		last_bytes = 0;
		last_beat_ms = millis();
		window_used = 0;
		dumped = false;
	}

	audio.service();
	drainCapture();

	uint32_t now = millis();
	if (now - last_beat >= 2000) {
		uint32_t dt = now - last_beat_ms;
		last_beat = now;
		last_beat_ms = now;
		seq++;

		uint32_t pkts = audio.packetsReceived();
		uint32_t bytes = audio.inBytes();
		uint32_t dp = pkts - last_packets;
		uint32_t db = bytes - last_bytes;
		last_packets = pkts;
		last_bytes = bytes;

		// cfg= is the diagnostic that separates two states this sketch could
		// not previously tell apart, and the ambiguity cost a bench session:
		// claim() memcpys every device's descriptors into lastConfig()
		// WHETHER OR NOT it claims them, so
		//   cfg=0    -> nothing has enumerated at all (cable, VBUS, device)
		//   cfg=N>0, ready=0/0 -> a device WAS offered and this driver
		//                        refused it (wrong class, or no alt matching
		//                        the requested format)
		// Without it, a refused claim is silent and looks exactly like an
		// empty port -- the driver's attach print only fires once a claim
		// has already succeeded.
		uint16_t cfglen = 0;
		audio.lastConfig(&cfglen);
		Serial1.printf("CAPTURE-TEST: HEARTBEAT seq=%lu up=%lus rec=%d ready=%d/%d "
		               "ctrl=%u/%lu/%lu cfg=%u",
		               (unsigned long)seq, (unsigned long)((now - started_at) / 1000),
		               (int)audio.recording(), (int)audio.ready(), (int)audio.readyIn(),
		               audio.controlState(), (unsigned long)audio.controlTimeouts(),
		               (unsigned long)audio.controlQueueFails(), cfglen);

		if (audio.recording() && dt) {
			// The load-bearing number. bytes/s divided by (channels x
			// subslot) is the device's own converter rate as measured by
			// what it actually delivered -- an independent estimate of the
			// same quantity the feedback endpoint reports on the OUT side.
			uint32_t bps = (uint32_t)(((uint64_t)db * 1000u) / dt);
			uint32_t per_frame = (uint32_t)CAPTURE_CHANNELS * ((CAPTURE_BITS + 7) / 8);
			uint32_t derived = per_frame ? bps / per_frame : 0;
			Serial1.printf(" pkts/s=%lu bytes/s=%lu derived=%luHz fifo=%lu "
			               "over=%lu empty=%lu err=%lu",
			               (unsigned long)((uint64_t)dp * 1000u / dt),
			               (unsigned long)bps, (unsigned long)derived,
			               (unsigned long)audio.recorded(),
			               (unsigned long)audio.inOverruns(),
			               (unsigned long)audio.inEmptyMicroframes(),
			               (unsigned long)audio.inTransportErrors());
		}
		Serial1.println();

		// One analysis pass per beat once the window has filled, and one
		// hex dump ever -- the dump exists so the samples can be checked
		// off-device, not so the console can be flooded.
		if (audio.recording() && window_used >= WINDOW_FRAMES * CAPTURE_TAKE_CHANNELS) {
			for (uint8_t c = 0; c < CAPTURE_TAKE_CHANNELS && c < 2; c++)
				analyseWindow(c);
			if (!dumped && seq >= 3) { dumpWindow(); dumped = true; }
			window_used = 0;   // next beat analyses fresh audio
		}
	}
}
