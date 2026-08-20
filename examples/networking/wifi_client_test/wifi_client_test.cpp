// Arduino WiFi facade proof: WiFi.begin() + WiFiClient echo against the ESP
// bench oracle (192.168.4.1:4712).
//
// QEMU proof (run_qemu.sh): the CARD-ABSENT path -- WL_NO_SHIELD, no IP, alive.
// QEMU proof (run_qemu_wifi.sh): enumeration + a REAL scan against the IW416
// model, which returns zero BSS by design -> WL_NO_SSID_AVAIL, honestly.
// Association/DHCP/TCP are silicon-only (transcript_hw_evkb.txt).
#include "Arduino.h"
#include "HardwareSerial.h"
#include "WiFi.h"
#include "WiFiClient.h"
#include <string.h>
#include <stdio.h>

#if defined(HAVE_WIFI_CREDS)
#include "wifi_creds.h"          // generated, gitignored -- never committed
#else
// Deliberately nonexistent SSID: NOT a credential.  The [wifi] gate uses this
// to drive a real scan that must honestly find nothing.
#define M2_WIFI_SSID "WIFI-GATE-NO-SUCH-AP"
#define M2_WIFI_PSK  nullptr
#endif

#if defined(HAVE_IW416_FW)
extern const uint8_t  iw416_fw[];
extern const uint32_t iw416_fw_len;
#endif

static uint32_t s_tx = 0, s_ok = 0, s_fail = 0;

void setup() {
    Serial1.begin(115200);
    delay(50);
    Serial1.println("RT1176 WiFi client test up");
#if defined(HAVE_IW416_FW)
    WiFi.setFirmware(iw416_fw, iw416_fw_len);
#endif
    int st = WiFi.begin(M2_WIFI_SSID, M2_WIFI_PSK);
    Serial1.print("wifi_status="); Serial1.println(st);
    if (st == WL_CONNECTED) {
        Serial1.print("wifi_ip=");   Serial1.println(WiFi.localIP());
        Serial1.print("wifi_rssi="); Serial1.println(WiFi.RSSI());
    }
}

void loop() {
    static uint32_t lastBeat = 0, beats = 0;
    if (millis() - lastBeat >= 1000) {
        lastBeat = millis();
        Serial1.print("alive="); Serial1.print(++beats);
        Serial1.print(" tcp="); Serial1.print(s_tx);
        Serial1.print('/');     Serial1.print(s_ok);
        Serial1.print('/');     Serial1.println(s_fail);
    }
}
