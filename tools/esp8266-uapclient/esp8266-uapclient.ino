// Bench STATION for the RT1176 uAP work (W17).
//
// Joins the OPEN access point hosted by the M.2 IW416 on the MIMXRT1170-EVKB
// and does nothing else.  Association alone is the measurement: the card's own
// STA_LIST is the oracle on the other side, and whether the firmware raises
// EVENT_MICRO_AP_STA_ASSOC (0x2D) when this connects is the open question this
// exists to answer.
//
// ★ NO PASSWORD ANYWHERE, and that is deliberate rather than lazy.  The AP
// under test is open, so there is nothing to store, nothing to generate and
// nothing that could reach a commit.  This repo has leaked live Wi-Fi
// passwords into pushed history once already; the cheapest way not to do it
// again is to run the first join test against a network that has no secret.
// When WPA2 comes, credentials go through tools/esp32c6-benchap/make-creds.sh's
// refuse-to-write-untracked pattern -- never inline here.
//
// ★ NO DHCP IS EXPECTED.  The uAP has no upstack yet (no server, no netif), so
// this will associate and never get an address.  That is the CORRECT outcome
// for this phase: a static IP is configured below so the sketch does not sit
// in a DHCP retry loop and misreport association as failure.  "Associated" and
// "addressed" are different claims and this prints them separately.
//
// Build/flash:
//   arduino-cli compile -b esp8266:esp8266:nodemcuv2 tools/esp8266-uapclient
//   arduino-cli upload  -b esp8266:esp8266:nodemcuv2 -p /dev/cu.usbserial-0001 \
//                       tools/esp8266-uapclient
#include <ESP8266WiFi.h>

static const char *AP_SSID = "RT1176-UAP-TEST";

// Static, because there is no DHCP server on the far side yet.  The addresses
// are private and arbitrary; nothing routes through them.
static IPAddress kIp(192, 168, 44, 50);
static IPAddress kGw(192, 168, 44, 1);
static IPAddress kMask(255, 255, 255, 0);

static uint32_t s_lastPrint = 0;
static bool     s_wasConnected = false;

void setup() {
    Serial.begin(115200);
    delay(300);
    Serial.println();
    Serial.println("esp8266_uapclient up");
    Serial.print("client_mac=");
    Serial.println(WiFi.macAddress());

    // STATION ONLY.  The ESP8266 ships its own SoftAP on by default and this
    // board has been broadcasting ESP8266TEST; leaving that up during a uAP
    // test would put a second beacon on the air next to the one under test.
    WiFi.softAPdisconnect(true);
    WiFi.mode(WIFI_STA);
    WiFi.persistent(false);
    WiFi.setAutoReconnect(true);
    WiFi.config(kIp, kGw, kMask);
    Serial.print("joining ssid="); Serial.println(AP_SSID);
    WiFi.begin(AP_SSID);          // open network: no passphrase argument
}

void loop() {
    const bool up = (WiFi.status() == WL_CONNECTED);
    if (up != s_wasConnected) {
        s_wasConnected = up;
        Serial.print("client_state=");
        Serial.print(up ? "ASSOCIATED" : "disconnected");
        if (up) {
            Serial.print(" bssid=");   Serial.print(WiFi.BSSIDstr());
            Serial.print(" ch=");      Serial.print(WiFi.channel());
            Serial.print(" rssi=");    Serial.print(WiFi.RSSI());
            Serial.print(" ip=");      Serial.print(WiFi.localIP().toString());
        }
        Serial.println();
    }
    if (millis() - s_lastPrint >= 5000) {
        s_lastPrint = millis();
        // status() and localIP() are separate claims -- see the header note.
        Serial.print("hb status="); Serial.print((int)WiFi.status());
        Serial.print(" assoc=");    Serial.print(up ? 1 : 0);
        Serial.print(" ip=");       Serial.println(WiFi.localIP().toString());
    }
}
