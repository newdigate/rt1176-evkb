// Bench access point for the M.2 Wi-Fi work, on a Seeed XIAO ESP32-C6.
//
// WHY THIS EXISTS
// ---------------
// The ESP8266 bench AP caps the link at ~2.7 Mbps, and that is the single
// reason two W16 questions cannot be answered:
//
//   * THROUGHPUT.  W11 measured the SDIO bus as the ceiling at 9.7 Mbps
//     against a real AP.  A bench that tops out at 2.7 is bounded somewhere
//     else entirely, so cutting bus cost 30x cannot show up as Mbps there --
//     and it didn't (2.66 -> 2.68, noise).
//   * AGGREGATION.  It needs frames to arrive FASTER THAN THE HOST LOOKS.
//     Measured on the ESP8266: 134 frames/s against 211 service looks/s, so
//     every ring run is length 1 and aggregation stays at ~1% of frames.  No
//     driver tuning can fix that; only more frames per second can.
//
// This AP is the instrument, not the subject.  It deliberately keeps the
// ESP8266's SSID, PSK and 192.168.4.x subnet so the RT1176 image, its
// compiled-in credentials and tput_peer.py all work UNCHANGED -- swapping the
// AP must change exactly one thing, or the A/B stops being an A/B.
//
// WHAT IT STILL CANNOT DO, said up front: the C6 is 2.4 GHz only, so it
// RELAYS both stations on one radio and every byte crosses the air twice.
// It will not match a dual-band router with the peer on 5 GHz (which is what
// the W11 baseline used).  It should clear 10 Mbps, which is the threshold
// that matters -- but if it does not, the honest reading is that the bench is
// still the ceiling, not that the driver is.
//
// BUILD + FLASH
//   tools/esp32c6-benchap/make-creds.sh          # once, copies SSID/PSK
//   arduino-cli compile -b esp32:esp32:XIAO_ESP32C6 tools/esp32c6-benchap
//   arduino-cli upload  -b esp32:esp32:XIAO_ESP32C6 -p /dev/cu.usbmodemXXXX \
//                       tools/esp32c6-benchap
#include <WiFi.h>
#include <esp_wifi.h>
#include "ap_creds.h"

// ── Seeed XIAO ESP32-C6 antenna control ──────────────────────────────────
// This board has an RF switch between the on-board antenna and the u.FL
// connector, and it is NOT passive: GPIO3 powers the switch (LOW = on) and
// GPIO14 selects (LOW = on-board, HIGH = external).  Leaving them at their
// reset state is a real RF fault on a throughput bench -- exactly the class
// of mistake that cost W8 days when the M.2 card's own antenna was loose.
static const int PIN_RF_SWITCH_PWR = 3;
static const int PIN_RF_ANT_SEL    = 14;

// 2.4 GHz channel.  6 by default rather than 1: 1/6/11 are the
// non-overlapping trio and 1 is the most crowded default in most homes.
// Change it if the bench sits next to a busy AP -- and if you do, say so in
// whatever transcript the run lands in, because it changes the air.
static const int AP_CHANNEL = 6;

// ★ HT40 (40 MHz) IS THE POINT OF THIS BOARD.  It doubles the PHY rate, and
// the IW416 supports it -- M2Radio's set11nCfg() already advertises SHORT_GI_40
// in its HT capability.  It is antisocial on 2.4 GHz (it occupies two of the
// three non-overlapping channels), which is fine for a bench and would not be
// on a shared network.  Set false to A/B the bandwidth itself.
static const bool AP_HT40 = true;

static void printApState(const char *tag) {
    Serial.print(tag);
    Serial.print(" ssid=");     Serial.print(AP_SSID);
    Serial.print(" ip=");       Serial.print(WiFi.softAPIP());
    Serial.print(" channel=");  Serial.print(AP_CHANNEL);
    wifi_bandwidth_t bw = WIFI_BW_HT20;
    esp_wifi_get_bandwidth(WIFI_IF_AP, &bw);
    Serial.print(" bw=");       Serial.print(bw == WIFI_BW_HT40 ? "HT40" : "HT20");
    int8_t pwr = 0;
    esp_wifi_get_max_tx_power(&pwr);
    Serial.print(" txpower=");  Serial.print(pwr * 0.25f, 2); Serial.print("dBm");
    Serial.print(" stations=");
    Serial.println(WiFi.softAPgetStationNum());
}

// Station join/leave, so the console says whether the RT1176 board is on the
// air rather than leaving it to be inferred from the board's own claims.
static void onWiFiEvent(WiFiEvent_t event, WiFiEventInfo_t info) {
    switch (event) {
    case ARDUINO_EVENT_WIFI_AP_STACONNECTED: {
        const uint8_t *m = info.wifi_ap_staconnected.mac;
        Serial.printf("sta_connected mac=%02X:%02X:%02X:%02X:%02X:%02X count=%d\n",
                      m[0], m[1], m[2], m[3], m[4], m[5],
                      WiFi.softAPgetStationNum());
        break;
    }
    case ARDUINO_EVENT_WIFI_AP_STADISCONNECTED: {
        const uint8_t *m = info.wifi_ap_stadisconnected.mac;
        Serial.printf("sta_disconnected mac=%02X:%02X:%02X:%02X:%02X:%02X count=%d\n",
                      m[0], m[1], m[2], m[3], m[4], m[5],
                      WiFi.softAPgetStationNum());
        break;
    }
    case ARDUINO_EVENT_WIFI_AP_STAIPASSIGNED:
        Serial.printf("sta_ip_assigned ip=%s\n",
                      IPAddress(info.wifi_ap_staipassigned.ip.addr).toString().c_str());
        break;
    default:
        break;
    }
}

void setup() {
    Serial.begin(115200);
    delay(300);
    Serial.println();
    Serial.println("XIAO ESP32-C6 bench AP for the RT1176 M.2 Wi-Fi work");

    pinMode(PIN_RF_SWITCH_PWR, OUTPUT);
    digitalWrite(PIN_RF_SWITCH_PWR, LOW);    // power the RF switch
    pinMode(PIN_RF_ANT_SEL, OUTPUT);
    digitalWrite(PIN_RF_ANT_SEL, LOW);       // on-board antenna
    Serial.println("antenna=onboard (GPIO3 low = switch powered, GPIO14 low = internal)");

    WiFi.onEvent(onWiFiEvent);
    WiFi.mode(WIFI_AP);

    // Subnet pinned to the ESP8266's so nothing downstream changes.
    // ★ AND SO THE ADDRESSES DO NOT SHUFFLE.  An ESP8266 SoftAP hands out
    // 192.168.4.100 first and a rebooted AP has an empty lease table, so
    // whichever station asks first takes the address the board had -- which
    // cost one confused bench session when the peer ended up talking to the
    // Mac itself.  Same pool here, same hazard: read the board's own
    // `tput: ip=` line, never assume .100.
    WiFi.softAPConfig(IPAddress(192, 168, 4, 1),
                      IPAddress(192, 168, 4, 1),
                      IPAddress(255, 255, 255, 0));

    // max_connection 4: the board and the Mac, with room to watch.
    if (!WiFi.softAP(AP_SSID, AP_PSK, AP_CHANNEL, /*hidden=*/0, /*max_conn=*/4)) {
        Serial.println("softAP=FAILED");
        return;
    }

    // Throughput knobs, all of them deliberate:
    //  - PS NONE: an AP that sleeps adds latency to every relayed frame, and
    //    this one is measuring somebody else's latency.
    //  - HT40: the actual reason for using a C6 rather than the ESP8266.
    //  - max TX power: the bench board sits inches away, but attenuating the
    //    link would make the measurement about the air rather than the bus.
    esp_wifi_set_ps(WIFI_PS_NONE);
    if (AP_HT40) {
        esp_err_t e = esp_wifi_set_bandwidth(WIFI_IF_AP, WIFI_BW_HT40);
        if (e != ESP_OK) {
            Serial.printf("ht40=REFUSED (%d) -- staying at HT20\n", (int)e);
        }
    }
    esp_wifi_set_max_tx_power(80);           // 80 * 0.25 = 20 dBm

    printApState("ap_up");
}

void loop() {
    // One line a second: station count is the only thing that changes, and a
    // bench operator needs to see the board join without reading the board.
    static uint32_t last = 0;
    static int lastCount = -1;
    if (millis() - last >= 1000) {
        last = millis();
        int n = WiFi.softAPgetStationNum();
        if (n != lastCount) {
            lastCount = n;
            printApState("ap");
        }
    }
}
