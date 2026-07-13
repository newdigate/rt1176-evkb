// QEMU/HW gate for the WProgram.h include-parity pass.
// The point: ONLY <Arduino.h> from the core — everything exercised below must
// arrive transitively, exactly as on a stock Teensy 4 build.
#include <Arduino.h>
#include <Bounce2.h>   // stock library smoke test (unmodified, MIT)

// Negative checks: CDC-only descriptors must keep every usb_* gate closed.
#ifdef KEYBOARD_INTERFACE
#error "KEYBOARD_INTERFACE leaked into a CDC-only build"
#endif
#ifdef MIDI_INTERFACE
#error "MIDI_INTERFACE leaked into a CDC-only build"
#endif
#ifdef MTP_INTERFACE
#error "MTP_INTERFACE leaked into a CDC-only build"
#endif

static volatile uint32_t itimer_ticks;
static IntervalTimer itimer;   // no #include "IntervalTimer.h" — the NativeEthernet regression
static elapsedMillis emillis;  // ditto elapsedMillis.h

static void itimer_isr() { itimer_ticks++; }

static void check(bool ok, const char *tag)
{
	Serial1.print(tag);
	Serial1.println(ok ? "=OK" : "=FAIL");
}

void setup()
{
	Serial1.begin(115200);
	Serial1.println("WPROGRAM PARITY GATE");

	// WCharacter.h
	check(isAlpha('A') && !isAlpha('1') && isDigit('7') && toUpperCase('a') == 'A', "WCHAR");

	// WString via Arduino.h (worked before this pass; kept as a regression check)
	String s("hello");
	s += " world";
	s.toUpperCase();
	check(s == "HELLO WORLD" && s.length() == 11, "STRING");

	// WMath.cpp: makeWord + reproducible PRNG (clean-room xorshift32)
	check(makeWord(0x12, 0x34) == 0x1234, "WORD");
	randomSeed(42);
	long r1 = random(100);
	randomSeed(42);
	long r2 = random(100);
	long r3 = random(50, 60);
	check(r1 == r2 && r1 >= 0 && r1 < 100 && r3 >= 50 && r3 < 60, "RAND");

	// elapsedMillis
	emillis = 0;
	delay(25);
	check(emillis >= 20 && emillis <= 100, "EMILLIS");

	// IntervalTimer via Arduino.h alone
	itimer_ticks = 0;
	itimer.begin(itimer_isr, 1000); // 1 kHz
	delay(50);
	itimer.end();
	check(itimer_ticks >= 40 && itimer_ticks <= 60, "ITIMER");

	// pulseIn: quiet-pin timeout path (valid in QEMU and on HW — D11 is
	// undriven at this point, so a jumpered D12 still reads a quiet LOW).
	// D11/D12 (AD_30/AD_31) are the SPI-loopback pair already HW-proven on
	// this board; the RevC3 schematic shows AD_06 ("D4") never reaches the
	// Arduino sockets, so the original D4<->D5 jumper shorted a rail.
	pinMode(12, INPUT_PULLDOWN);
	check(pulseIn(12, HIGH, 20000) == 0, "PULSE_TIMEOUT");

	// pulseIn: real measurement — meaningful on HW only (jumper D11 <-> D12).
	// In QEMU the unjumpered input reads 0, so this prints PULSE_HW=0; the
	// gate script does not assert on it. HW-RESULTS.md records ~500us
	// (half-period of the 1 kHz tone), accepted range 400..600.
	tone(11, 1000);
	delay(10);
	uint32_t width = pulseIn(12, HIGH, 50000);
	noTone(11);
	Serial1.print("PULSE_HW=");
	Serial1.println(width);

	// CrashReport stub
	check(!CrashReport, "CRASHREPORT_BOOL");
	Serial1.print(CrashReport);          // Print::print(const Printable&)
	CrashReport.clear();                 // no-op, must link
	CrashReportClass::breadcrumb(1, 0x1176); // no-op, must link

	// Stock library smoke: Bounce2, unmodified
	Bounce b;
	b.attach(6, INPUT_PULLUP);
	b.interval(5);
	b.update();
	check(true, "BOUNCE");

	// usb_serial.h surface via Arduino.h (Serial == USB CDC)
	Serial.begin(9600);
	check(true, "USBSERIAL");

	Serial1.println("GATE=DONE");
}

void loop() {}
