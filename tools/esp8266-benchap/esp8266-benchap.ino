// Bench access point for the M.2 Wi-Fi work, on the ESP8266 -- the ORIGINAL
// bench AP, reconstructed in-tree.
//
// WHY THIS EXISTS
// ---------------
// Every silicon transcript in examples/networking/ was taken against an
// ESP8266 softAP called ESP8266TEST, but the sketch that made it one was
// never committed anywhere -- when the physical module stopped coming up
// (2026-08-26, NEW-8), there was nothing to reflash it FROM.  This is that
// sketch, written to the same contract the ESP32-C6 port documents
// (tools/esp32c6-benchap/): same SSID, PSK, channel and 192.168.4.x subnet,
// so the RT1176 image, its compiled-in credentials, wifi_peer.py and
// tput_peer.py all work UNCHANGED against either AP.
//
// The two APs differ in exactly one axis, and it is deliberate: the C6 does
// HT40 and clears 10 Mbps (it is the THROUGHPUT instrument); the ESP8266 caps
// at ~2.7 Mbps and is fine for everything that is not a throughput test --
// correctness runs and the NEW-8 soak measure resource behaviour, not Mbps.
//
// BUILD + FLASH
//   tools/esp8266-benchap/make-creds.sh          # once, writes ap_creds.h
//   arduino-cli compile -b esp8266:esp8266:nodemcuv2 tools/esp8266-benchap
//   arduino-cli upload  -b esp8266:esp8266:nodemcuv2 -p /dev/cu.usbserial-XXXX \
//                       tools/esp8266-benchap
#include <ESP8266WiFi.h>
#include "ap_creds.h"

// 6, matching the C6 port and every transcript: 1/6/11 are the
// non-overlapping trio and 1 is the most crowded default in most homes.
static const int AP_CHANNEL = 6;

// ★ The ESP8266 core delivers softAP station events through handler OBJECTS,
// and the registration dies with the handle -- these three must be globals.
// A local in setup() compiles clean and silently never fires.
static WiFiEventHandler s_onJoin, s_onLeave, s_onProbe;

static void printApState(const char *tag) {
    Serial.print(tag);
    Serial.print(" ssid=");    Serial.print(AP_SSID);
    Serial.print(" ip=");      Serial.print(WiFi.softAPIP());
    Serial.print(" channel="); Serial.print(AP_CHANNEL);
    Serial.print(" stations=");
    Serial.println(WiFi.softAPgetStationNum());
}

void setup() {
    Serial.begin(115200);
    delay(300);
    Serial.println();
    Serial.println("ESP8266 bench AP for the RT1176 M.2 Wi-Fi work");

    WiFi.persistent(false);         // fresh config every boot, no flash wear
    WiFi.mode(WIFI_AP);

    // Same subnet as always; the DHCP pool starts at .100 and a rebooted AP
    // has an empty lease table, so whichever station asks first takes .100 --
    // read the board's own wifi_ip= line, never assume the address.
    WiFi.softAPConfig(IPAddress(192, 168, 4, 1),
                      IPAddress(192, 168, 4, 1),
                      IPAddress(255, 255, 255, 0));

    // Station join/leave on the console, so whether the RT1176 board is on
    // the air is a printed fact rather than an inference.
    s_onJoin = WiFi.onSoftAPModeStationConnected(
        [](const WiFiEventSoftAPModeStationConnected &e) {
            Serial.printf("sta_connected mac=%02X:%02X:%02X:%02X:%02X:%02X count=%d\n",
                          e.mac[0], e.mac[1], e.mac[2], e.mac[3], e.mac[4],
                          e.mac[5], WiFi.softAPgetStationNum());
        });
    s_onLeave = WiFi.onSoftAPModeStationDisconnected(
        [](const WiFiEventSoftAPModeStationDisconnected &e) {
            Serial.printf("sta_disconnected mac=%02X:%02X:%02X:%02X:%02X:%02X count=%d\n",
                          e.mac[0], e.mac[1], e.mac[2], e.mac[3], e.mac[4],
                          e.mac[5], WiFi.softAPgetStationNum());
        });

    if (!WiFi.softAP(AP_SSID, AP_PSK, AP_CHANNEL, /*hidden=*/0, /*max_conn=*/4)) {
        Serial.println("softAP=FAILED");
        return;
    }

    // An AP that naps adds latency to every relayed frame; this one is a
    // bench instrument measuring somebody else's behaviour.
    WiFi.setSleepMode(WIFI_NONE_SLEEP);
    WiFi.setOutputPower(20.5f);     // dBm, the ESP8266 maximum

    printApState("ap_up");
}

void loop() {
    // Report on CHANGE, so a join or drop cannot scroll past unseen...
    static uint32_t lastPoll = 0, lastBeat = 0;
    static int lastCount = -1;
    if (millis() - lastPoll >= 500) {
        lastPoll = millis();
        int n = WiFi.softAPgetStationNum();
        if (n != lastCount) {
            lastCount = n;
            printApState("ap");
        }
    }
    // ...and a heartbeat anyway: a healthy idle AP and a dead console are
    // otherwise the same thing -- silence.
    if (millis() - lastBeat >= 5000) {
        lastBeat = millis();
        Serial.print("alive up=");
        Serial.print(millis() / 1000);
        Serial.print("s stations=");
        Serial.println(WiFi.softAPgetStationNum());
    }
}
