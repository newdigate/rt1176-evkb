// Standalone SD re-verify: SD.begin over USDHC (BUILTIN_SDCARD), list the root
// directory, then write + read-back a small test file. Prints results on Serial1
// (MCU-Link VCOM). loop() re-prints the RESULT line every 2 s so a late reader
// still catches it. Not a QEMU gate -- a real-hardware check with a card inserted.
#include "Arduino.h"
#include "HardwareSerial.h"
#include <SD.h>
#include <string.h>

static const char *g_result = "RESULT: (running)";

void setup() {
    Serial1.begin(115200);
    delay(300);
    Serial1.println();
    Serial1.println("==== SD re-verify (RT1176 / USDHC) ====");

    if (!SD.begin(BUILTIN_SDCARD)) {
        Serial1.println("SD.begin(BUILTIN_SDCARD) FAILED (card missing / not seated / bad slot?)");
        g_result = "RESULT: FAIL (SD.begin)";
        return;
    }
    Serial1.println("SD.begin OK");

    // Root directory listing.
    File root = SD.open("/");
    int count = 0;
    if (root) {
        Serial1.println("root listing:");
        for (;;) {
            File e = root.openNextFile();
            if (!e) break;
            Serial1.print("  ");
            Serial1.print(e.name());
            if (e.isDirectory()) {
                Serial1.println("/");
            } else {
                Serial1.print("  ");
                Serial1.print((uint32_t)e.size());
                Serial1.println(" B");
            }
            e.close();
            if (++count >= 40) { Serial1.println("  ...(truncated)"); break; }
        }
        root.close();
    }
    Serial1.print("root entries: ");
    Serial1.println(count);

    // Write + read-back a small test file.
    const char *fn = "SDVERIFY.TXT";
    const char *payload = "RT1176 SD OK 2026-07-14";
    if (SD.exists(fn)) SD.remove(fn);
    File w = SD.open(fn, FILE_WRITE);
    if (!w) { Serial1.println("open-for-write FAILED"); g_result = "RESULT: FAIL (write-open)"; return; }
    w.print(payload);
    w.close();

    File r = SD.open(fn);
    if (!r) { Serial1.println("open-for-read FAILED"); g_result = "RESULT: FAIL (read-open)"; return; }
    char buf[64];
    int n = 0;
    while (r.available() && n < 63) buf[n++] = (char)r.read();
    buf[n] = 0;
    r.close();
    Serial1.print("read back: '");
    Serial1.print(buf);
    Serial1.println("'");

    if (strcmp(buf, payload) == 0) {
        Serial1.println("write/read MATCH");
        g_result = "RESULT: PASS";
    } else {
        Serial1.println("write/read MISMATCH");
        g_result = "RESULT: FAIL (mismatch)";
    }
    Serial1.println(g_result);
}

void loop() {
    Serial1.println(g_result);   // re-print so a reader started after boot still sees it
    delay(2000);
}
