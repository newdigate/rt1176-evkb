// QEMU gate for the String API. ★ = harvested must-pass consumer surface
// (the calls actually made by the core + libraries + gates). Assertions are
// computed independently from the documented Arduino semantics — never
// captured from old-String output.
#include <Arduino.h>
#include <string.h>
#include <math.h>

static void check(bool ok, const char *tag) {
	Serial1.print(tag); Serial1.println(ok ? "=OK" : "=FAIL");
}
static String make_rvo() { String r("rv"); r += "o"; return r; }

void setup() {
	Serial1.begin(115200);
	Serial1.println("STRING GATE");
	bool all = true; bool ok;

	// ★ default ctor, ★ c-string ctor, ★ length, ★ c_str
	{ String e; ok = (e.length() == 0 && e.c_str() != NULL && e.c_str()[0] == 0);
	  String h("hello"); ok &= (h.length() == 5 && strcmp(h.c_str(), "hello") == 0);
	  check(ok, "CTOR_BASIC"); all &= ok; }
	// numeric ctors (documented API, no in-tree consumers)
	{ ok = (String('A') == "A") && (String(42) == "42") && (String(-42) == "-42")
	     && (String((unsigned long)1000000UL) == "1000000")
	     && (String(255, HEX) == "ff") && (String(255, BIN) == "11111111")
	     && (String(8, OCT) == "10") && (String((unsigned int)0) == "0")
	     && (String(3.14159f, 2) == "3.14") && (String(-2.5f, 1) == "-2.5");
	  check(ok, "CTOR_NUM"); all &= ok; }
	// ★ copy + RVO
	{ String a("x"); String b(a); a += "y";
	  ok = (b == "x" && a == "xy" && make_rvo() == "rvo");
	  check(ok, "COPY_RVO"); all &= ok; }
	// assignment incl. self
	{ String a("abc"); a = a; ok = (a == "abc");
	  a = "def"; ok &= (a == "def"); a = F("ghi"); ok &= (a == "ghi");
	  check(ok, "ASSIGN"); all &= ok; }
	// ★ += char, ★ += c-string; += String/int; self-append
	{ String a; for (char c = 'a'; c <= 'e'; c++) a += c;   // readString pattern
	  ok = (a == "abcde");
	  String b("ab"); b += b; ok &= (b == "abab");          // s += s
	  String d("n="); d += 42; ok &= (d == "n=42");
	  check(ok, "CONCAT"); all &= ok; }
	// operator+ chains (StringSumHelper path)
	{ String r = String("a") + "b" + 'c' + 42;
	  ok = (r == "abc42"); check(ok, "PLUS_CHAIN"); all &= ok; }
	// ★ == literal; comparisons
	{ String s("HELLO");
	  ok = (s == "HELLO") && (s != "hello") && s.equals("HELLO")
	     && s.equalsIgnoreCase("hello") && (s.compareTo("HELLO") == 0)
	     && (String("abc").compareTo("abd") < 0) && (String("b") > String("a"))
	     && (String("a") < String("b")) && (String("a") <= String("a"));
	  check(ok, "COMPARE"); all &= ok; }
	// indexOf / lastIndexOf
	{ String s("abcabc");
	  ok = (s.indexOf('b') == 1) && (s.indexOf('b', 2) == 4) && (s.indexOf("ca") == 2)
	    && (s.lastIndexOf('b') == 4) && (s.indexOf('z') == -1) && (s.lastIndexOf("zz") == -1);
	  check(ok, "INDEXOF"); all &= ok; }
	// substring incl. clamp + swapped args (documented: from > to are switched)
	{ String s("hamburger");
	  ok = (s.substring(3) == "burger") && (s.substring(3, 6) == "bur")
	    && (s.substring(3, 999) == "burger") && (s.substring(6, 3) == "bur")
	    && (s.substring(99) == "");
	  check(ok, "SUBSTRING"); all &= ok; }
	// ★ toUpperCase; toLowerCase
	{ String s("Hello World"); s.toUpperCase(); ok = (s == "HELLO WORLD");
	  s.toLowerCase(); ok &= (s == "hello world");
	  check(ok, "CASE"); all &= ok; }
	// trim
	{ String s("  x  "); s.trim(); ok = (s == "x");
	  String w("   "); w.trim(); ok &= (w == "" && w.length() == 0);
	  check(ok, "TRIM"); all &= ok; }
	// replace: char, grow, shrink
	{ String s("banana"); s.replace('a', 'o'); ok = (s == "bonono");
	  String g("ab-ab"); g.replace("ab", "xyz"); ok &= (g == "xyz-xyz");
	  String h("xyz-xyz"); h.replace("xyz", "a"); ok &= (h == "a-a");
	  check(ok, "REPLACE"); all &= ok; }
	// remove with clamp
	{ String s("hello"); s.remove(3); ok = (s == "hel");
	  String t("hello"); t.remove(1, 2); ok &= (t == "hlo");
	  String u("hi"); u.remove(1, 99); ok &= (u == "h");
	  check(ok, "REMOVE"); all &= ok; }
	// charAt/setCharAt/[]
	{ String s("cat");
	  ok = (s.charAt(1) == 'a') && (s[2] == 't');
	  s.setCharAt(0, 'b'); ok &= (s == "bat");
	  s[0] = 'r'; ok &= (s == "rat");
	  check(ok, "CHARAT"); all &= ok; }
	// toInt / toFloat incl. garbage → 0
	{ ok = (String("42").toInt() == 42) && (String("-7x").toInt() == -7)
	    && (String("abc").toInt() == 0) && (fabsf(String("3.5").toFloat() - 3.5f) < 0.001f)
	    && (String("nope").toFloat() == 0.0f);
	  check(ok, "TONUM"); all &= ok; }
	// startsWith / endsWith
	{ String s("filename.wav");
	  ok = s.startsWith("file") && !s.startsWith("x") && s.endsWith(".wav") && !s.endsWith(".mp3");
	  check(ok, "STARTEND"); all &= ok; }
	// ★ getBytes — the Print::print(String&) 32-byte chunk protocol; toCharArray
	{ String s("0123456789"); uint8_t b[8] = {0};
	  s.getBytes(b, 5, 2);                  // up to 4 chars from index 2 + NUL
	  ok = (memcmp(b, "2345", 4) == 0 && b[4] == 0);
	  char cb[16] = {0}; s.toCharArray(cb, sizeof(cb));
	  ok &= (strcmp(cb, "0123456789") == 0);
	  check(ok, "GETBYTES"); all &= ok; }
	// reserve keeps content; growth across reallocation
	{ String s("seed"); ok = (s.reserve(200) != 0);
	  for (int i = 0; i < 50; i++) s += "xy";
	  ok &= (s.length() == 4 + 100) && s.startsWith("seedxy") && s.endsWith("xy");
	  check(ok, "RESERVE_GROW"); all &= ok; }
	// OOM invariants: absurd reserve fails cleanly; string stays valid
	{ String s("keep");
	  ok = (s.reserve(100u * 1024u * 1024u) == 0);   // 100MB on a ~1MB-RAM part
	  ok &= (s == "keep") && (s.c_str() != NULL);
	  check(ok, "OOM"); all &= ok; }
	// F() / __FlashStringHelper overloads (flash == RAM on this core)
	{ String s(F("abc")); ok = (s == "abc"); s += F("def"); ok &= (s == "abcdef");
	  ok &= (s == F("abcdef"));
	  check(ok, "FLASH"); all &= ok; }
	// Print::print(String&) end-to-end (chunked getBytes path) — the runner
	// greps the uart for this exact line.
	{ String s("print-me-via-Print-chunks-print-me-via-Print-chunks-END");
	  Serial1.print("PRINTSTR:"); Serial1.println(s); }

	Serial1.println(all ? "STRING_ALL=PASS" : "STRING_ALL=FAIL");
	Serial1.println("GATE=DONE");
}

void loop() {}
