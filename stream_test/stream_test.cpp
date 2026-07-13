// QEMU gate for the Stream helper API (parse/find/readBytes/timeout paths).
// Runs against the current Stream implementation; after the clean-room swap
// it re-runs unchanged — the old implementation is the behavioral oracle.
// Expected values follow the documented Arduino Stream semantics, computed
// independently (not captured from old-Stream output).
#include <Arduino.h>
#include <string.h>
#include <math.h>

class MemStream : public Stream {
public:
	const char *buf; size_t len, pos;
	MemStream(const char *s) : buf(s), len(strlen(s)), pos(0) {}
	virtual int available() { return (int)(len - pos); }
	virtual int read() { return pos < len ? (uint8_t)buf[pos++] : -1; }
	virtual int peek() { return pos < len ? (uint8_t)buf[pos] : -1; }
	virtual size_t write(uint8_t) { return 1; }
	void seterr() { setReadError(); }        // exercise protected setReadError
};

class EmptyStream : public Stream {          // timeout paths: never has data
public:
	virtual int available() { return 0; }
	virtual int read() { return -1; }
	virtual int peek() { return -1; }
	virtual size_t write(uint8_t) { return 1; }
};

static void check(bool ok, const char *tag) {
	Serial1.print(tag); Serial1.println(ok ? "=OK" : "=FAIL");
}

void setup() {
	Serial1.begin(115200);
	Serial1.println("STREAM GATE");
	bool all = true; bool ok;

	{ MemStream m("abc-123xyz"); m.setTimeout(100);
	  ok = (m.parseInt() == -123); check(ok, "PARSEINT_SKIP"); all &= ok; }
	{ MemStream m("  42"); m.setTimeout(100);
	  ok = (m.parseInt() == 42); check(ok, "PARSEINT_WS"); all &= ok; }
	{ EmptyStream e; e.setTimeout(200); uint32_t t0 = millis();
	  long r = e.parseInt(); uint32_t dt = millis() - t0;
	  ok = (r == 0 && dt >= 180 && dt < 2000); check(ok, "PARSEINT_TIMEOUT"); all &= ok; }
	{ MemStream m("t=12.50;"); m.setTimeout(100);
	  ok = (fabsf(m.parseFloat() - 12.5f) < 0.001f); check(ok, "PARSEFLOAT"); all &= ok; }
	{ MemStream m("-0.25"); m.setTimeout(100);
	  ok = (fabsf(m.parseFloat() + 0.25f) < 0.001f); check(ok, "PARSEFLOAT_NEG"); all &= ok; }
	{ MemStream m("hello"); m.setTimeout(100); char b[16] = {0};
	  size_t n = m.readBytes(b, sizeof(b));
	  ok = (n == 5 && memcmp(b, "hello", 5) == 0); check(ok, "READBYTES"); all &= ok; }
	{ EmptyStream e; e.setTimeout(150); char b[8]; uint32_t t0 = millis();
	  size_t n = e.readBytes(b, 4); uint32_t dt = millis() - t0;
	  ok = (n == 0 && dt >= 130); check(ok, "READBYTES_TIMEOUT"); all &= ok; }
	{ MemStream m("hello\nworld"); m.setTimeout(100); char b[16] = {0};
	  size_t n = m.readBytesUntil('\n', b, sizeof(b));
	  ok = (n == 5 && memcmp(b, "hello", 5) == 0 && m.read() == 'w');
	  check(ok, "READBYTESUNTIL"); all &= ok; }      // terminator consumed, not stored
	{ MemStream m("xxxNEEDLEyyy"); m.setTimeout(100);
	  ok = (m.find((char *)"NEEDLE") && m.read() == 'y'); check(ok, "FIND"); all &= ok; }
	{ MemStream m("xxxyyy"); m.setTimeout(100);
	  ok = (!m.find((char *)"NEEDLE")); check(ok, "FIND_MISS"); all &= ok; }
	{ MemStream m("aaSTOPbbNEEDLE"); m.setTimeout(100);
	  ok = (!m.findUntil((char *)"NEEDLE", (char *)"STOP")); check(ok, "FINDUNTIL"); all &= ok; }
	{ MemStream m("abc"); m.setTimeout(100);
	  String s = m.readString();
	  ok = (s == "abc"); check(ok, "READSTRING"); all &= ok; }
	{ MemStream m("foo,bar"); m.setTimeout(100);
	  String s = m.readStringUntil(',');
	  ok = (s == "foo" && m.read() == 'b'); check(ok, "READSTRINGUNTIL"); all &= ok; }
	{ MemStream m("x");
	  ok = (m.getTimeout() == 1000); m.setTimeout(250); ok &= (m.getTimeout() == 250);
	  check(ok, "TIMEOUT_API"); all &= ok; }
	{ MemStream m("x");
	  ok = (m.getReadError() == 0); m.seterr(); ok &= (m.getReadError() != 0);
	  m.clearReadError(); ok &= (m.getReadError() == 0);
	  check(ok, "READERROR"); all &= ok; }
	{ MemStream m("0001x"); m.setTimeout(100);
	  ok = (m.find((char *)"001") && m.read() == 'x');
	  check(ok, "FIND_OVERLAP"); all &= ok; }   // overlapping partial match
	{ MemStream m("99999999999"); m.setTimeout(100);
	  long r = m.parseInt(); ok = true; (void)r; // must not crash/UB-trap
	  check(ok, "PARSEINT_LONG"); all &= ok; }

	Serial1.println(all ? "STREAM_ALL=PASS" : "STREAM_ALL=FAIL");
	Serial1.println("GATE=DONE");
}

void loop() {}
