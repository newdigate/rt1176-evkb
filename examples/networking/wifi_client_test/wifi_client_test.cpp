// Arduino WiFi facade proof: WiFi.begin() + WiFiClient echo against the ESP
// bench oracle (192.168.4.1:4712), plus a WiFiServer echo session the bench
// drives the other way (this board listening on :4713).
//
// QEMU proof (run_qemu.sh): the CARD-ABSENT path -- WL_NO_SHIELD, no IP, alive.
// QEMU proof (run_qemu_wifi.sh): enumeration + a REAL scan against the IW416
// model, which returns zero BSS by design -> WL_NO_SSID_AVAIL, honestly.
// Association/DHCP/TCP are silicon-only (transcript_hw_evkb.txt).
#include "Arduino.h"
#include "HardwareSerial.h"
#include "WiFi.h"
#include "WiFiClient.h"
#include "WiFiServer.h"
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
// WHICH failure, not just "a failure".  connect() folds six distinguishable
// causes into 0 and the echo can fail two more ways, so a silicon transcript
// that only carried s_fail could say nothing useful.  0 = none; 1..7 =
// WiFiClient::ConnectError (NO_LINK, NO_SLOT, NO_PCB, NO_ROUTE, TIMED_OUT,
// REFUSED, DNS_FAILED); 200 = short write; 201 = echo timeout or mismatch.
static uint8_t s_lastFail = 0;
static const uint8_t FAIL_SHORT_WRITE = 200, FAIL_ECHO = 201;

// --- the WiFiServer side ----------------------------------------------------
// This board LISTENING, which is the other half of the facade and the only
// thing in the tree that links WiFiServer at all.  The bench oracle is the
// same ESP: it opens :4713, sends a line, and compares what comes back, so
// srv=<sessions>/<bytes> below is un-fakeable the same way tcp= is.
static WiFiServer s_server(4713);
// ONE persistent session, held across loop() passes.  This is the idiom
// accept() exists for -- available() only ever surfaces a connection that has
// bytes staged this instant, so a sketch built on it cannot hold an idle
// session open (the pool's evictor and stall valve reap unclaimed conns).
static WiFiClient s_session;
static uint32_t s_srvSess = 0, s_srvBytes = 0;

// Takes the handle BY VALUE, deliberately: a WiFiClient is a refcounted handle
// onto a pool slot, and handing one to a request handler is what that design
// buys.  It is also, as of this task, the first genuine COPY of a WiFiClient
// anywhere in the tree -- `s_session = s_server.accept()` below is the first
// genuine copy-ASSIGN.  Before them --gc-sections dropped both operations from
// every image, so their correctness rested on review alone.
static uint32_t serveEcho(WiFiClient c) {
    // SNAPSHOT THE COUNT FIRST, and note that the obvious spelling is the bug:
    // `while (c.available() > 0)` LOOKS bounded and is not.  Since Task 7,
    // available() runs a servicePass() and re-checks whenever the staged chain
    // empties, so a peer that streams continuously keeps this loop fed and
    // loop() never returns -- the heartbeat stops and the sketch reads as hung.
    // One pass echoes what was staged when it started, capped, and goes home.
    int n = c.available();
    if (n <= 0) return 0;
    uint8_t buf[64];
    if (n > (int)sizeof(buf)) n = (int)sizeof(buf);
    int got = c.read(buf, (size_t)n);
    if (got <= 0) return 0;
    // ONE tcp_write for the pass, not one per byte: byte-at-a-time echoing
    // emits a TCP segment per byte, which works but makes a silicon capture
    // enormous and leans on a path the bench oracle is not measuring.
    return (uint32_t)c.write(buf, (size_t)got);
}

// --- INSTRUMENT (2026-08-21) -------------------------------------------------
// A silicon run reached alive=2359 with tcp=0/0/0 while the AP still listed a
// station.  status() alone cannot say which boundary broke: WL_NO_SHIELD is
// the shared exit of five bring-up failures and WL_CONNECT_FAILED of three,
// and the two that matter here -- "the associate was refused" and "we
// associated but no lease arrived" -- look identical from it.  This prints
// WiFi.lastError() by NAME, the raw driver status behind it, and the firmware
// event id, so the next run says which rather than leaving it inferred from
// the AP's station count.
static void reportBegin(int st, uint32_t attempt) {
    Serial1.print("wifi_status="); Serial1.print(st);
    Serial1.print(" attempt=");    Serial1.print(attempt);
    Serial1.print(" err=");        Serial1.print(WiFi.lastError());
    Serial1.print('(');            Serial1.print(WiFiClass::beginErrorName(WiFi.lastError()));
    Serial1.print(") drv=");       Serial1.print(WiFi.lastDriverStatus());
    Serial1.print('(');            Serial1.print(WiFiClass::driverStatusName(WiFi.lastDriverStatus()));
    Serial1.print(')');
    Serial1.print(" last_event=0x");
    Serial1.print(WiFi.radio().lastEvent(), HEX);
    Serial1.print(" info=0x");
    Serial1.println(WiFi.radio().lastEventInfo(), HEX);
    if (st == WL_CONNECTED) {
        Serial1.print("wifi_ip=");   Serial1.println(WiFi.localIP());
        Serial1.print("wifi_rssi="); Serial1.println(WiFi.RSSI());
        return;
    }
    // SSID_NOT_FOUND is the one failure whose cause is entirely OUTSIDE the
    // board, so make the failure carry the evidence: dump what the card
    // actually saw.  setsSeen > count means the firmware found more BSSes
    // than fit -- connectStation() keeps only 12, in SCAN ORDER not by
    // strength, so a weak AP on a busy bench can be crowded out of its own
    // connect attempt.  That is a different bug from "the AP is not on air".
    if (WiFi.lastError() == WiFiClass::SSID_NOT_FOUND) {
        static Iw416::ScanResult aps[24];
        uint8_t sets = 0;
        int n = WiFi.scanNetworks(aps, 24, &sets);
        Serial1.print("scan_dump n="); Serial1.print(n);
        Serial1.print(" sets_seen="); Serial1.print(sets);
        Serial1.print(" looking_for='"); Serial1.print(M2_WIFI_SSID);
        Serial1.println("'");
        for (int i = 0; i < n; i++) {
            Serial1.print("  ap["); Serial1.print(i);
            Serial1.print("] ch=");  Serial1.print(aps[i].channel);
            Serial1.print(" rssi=-"); Serial1.print(aps[i].rssi);
            Serial1.print(" sec=");  Serial1.print(aps[i].security);
            Serial1.print(" ssid='"); Serial1.print(aps[i].ssid);
            Serial1.println("'");
        }
    }
}

// Does a SECOND begin() succeed?  m2_lwip_test retries wifiConnect() every 5 s
// forever and so never shows a transient association failure; this facade
// attempts once, and setAutoReconnect() would not help because
// maybeReconnect() gates on m_wantReconnect, which only linkLost() raises --
// so a link that was never UP has no recovery path.  Retrying HERE answers
// "is it transient?" without changing the library.
static uint32_t s_beginAttempts = 1;
static uint32_t s_lastBeginMs   = 0;

void setup() {
    Serial1.begin(115200);
    delay(50);
    Serial1.println("RT1176 WiFi client test up");
#if defined(HAVE_IW416_FW)
    WiFi.setFirmware(iw416_fw, iw416_fw_len);
#endif
    int st = WiFi.begin(M2_WIFI_SSID, M2_WIFI_PSK);
    reportBegin(st, 1);
    if (st == WL_CONNECTED) {
        Serial1.print("wifi_ip=");   Serial1.println(WiFi.localIP());
        Serial1.print("wifi_rssi="); Serial1.println(WiFi.RSSI());
        s_server.begin();
        Serial1.print("srv_listen="); Serial1.print(s_server ? 1 : 0);
        // WHICH of begin()'s five FAILURE exits (ListenError has six values,
        // five of them failures), not just "it did not listen" -- a
        // transcript showing srv_listen=0 with no cause cannot be debugged.
        // 0 = OK; 1..5 = WiFiServer::ListenError (BAD_PORT, NO_LINK, NO_PCB,
        // BIND_FAILED, LISTEN_FAILED).
        Serial1.print(" err="); Serial1.println(s_server.lastError());
    }
}

void loop() {
    const bool up = (WiFi.status() == WL_CONNECTED);

    // INSTRUMENT: retry begin() every 15 s while down, reporting each attempt.
    if (!up && millis() - s_lastBeginMs >= 15000) {
        s_lastBeginMs = millis();
        int rst = WiFi.begin(M2_WIFI_SSID, M2_WIFI_PSK);
        reportBegin(rst, ++s_beginAttempts);
    }


    // --- the WiFiServer echo session, serviced every pass --------------------
    // SILICON-ONLY, same reason as the client block below: the QEMU IW416 model
    // returns zero scan results by design, so `up` is never true under either
    // gate and not one line of the accept path runs there.  accept() rather
    // than available() because this session is held open between requests --
    // see s_session.
    // Retryable, and it has to be retried here: begin() in setup() only runs
    // if the FIRST WiFi.begin() reached WL_CONNECTED, so a link that comes up
    // late (auto-reconnect, or an AP that appeared afterwards) would otherwise
    // leave the server falsy for ever with nothing to say why.
    if (up && !s_server) s_server.begin();
    if (up && s_server) {
        if (!s_session) {
            s_session = s_server.accept();       // copy-ASSIGN from a prvalue
            if (s_session) s_srvSess++;
        }
        if (s_session) {
            s_srvBytes += serveEcho(s_session);  // copy-CONSTRUCT (by value)
            // connected() stays true while unread bytes remain, so this drops
            // the session only once its tail has been echoed.
            if (!s_session.connected()) s_session.stop();
        }
    }

    // --- the WiFiClient echo round-trip, once every 2 s -----------------------
    // SILICON-ONLY, and not by omission: the QEMU IW416 model returns zero scan
    // results by design, so status() is never WL_CONNECTED under either gate and
    // this whole block is unreachable there.  Neither gate executes a single
    // line of the WiFiClient data path -- they prove no regression, nothing
    // more.  The oracle is the ESP bench echo server on the AP itself
    // (192.168.4.1:4712), which returns exactly the bytes it was sent, so
    // tcp=<sent>/<ok>/<fail> in the heartbeat below is un-fakeable by the
    // firmware: an ok can only come from bytes that made a full round trip.
    static uint32_t lastKick = 0;
    if (up && millis() - lastKick >= 2000) {
        lastKick = millis();
        WiFiClient c;
        char msg[48];
        int n = snprintf(msg, sizeof(msg), "WIFI hello %lu", (unsigned long)s_tx);
        // snprintf returns what it WOULD have written, so a truncation would
        // make n index past msg[] in the memcmp below.  Clamp it.
        if (n < 0) n = 0;
        if (n > (int)sizeof(msg) - 1) n = (int)sizeof(msg) - 1;
        s_tx++;
        if (c.connect(IPAddress(192, 168, 4, 1), 4712)) {
            size_t w = c.write((const uint8_t *)msg, (size_t)n);
            if (w != (size_t)n) {
                // A short write is a DIFFERENT fault from a bad echo, and
                // ignoring write()'s return would report it as the latter.
                s_fail++; s_lastFail = FAIL_SHORT_WRITE;
            } else {
                char echo[48];
                int got = 0;
                uint32_t t0 = millis();
                // Incremental, NOT `if (c.available() >= n)`: the pool stages a
                // capped number of pbufs at a time (WiFiConnPool.h obligation
                // 5), so waiting for a total is how you wait forever.
                while (got < n && millis() - t0 < 5000) {
                    int r = c.read((uint8_t *)echo + got, (size_t)(n - got));
                    if (r > 0) got += r;
                    else delay(1);      // yield -> the auto-service pump runs
                }
                if (got == n && memcmp(echo, msg, (size_t)n) == 0) { s_ok++; s_lastFail = 0; }
                else { s_fail++; s_lastFail = FAIL_ECHO; }
            }
            c.stop();
        } else {
            s_fail++;
            s_lastFail = c.lastError();   // which of the six, not just "it
        }                                 // failed"
    }

    static uint32_t lastBeat = 0, beats = 0;
    if (millis() - lastBeat >= 1000) {
        lastBeat = millis();
        Serial1.print("alive="); Serial1.print(++beats);
        Serial1.print(" tcp="); Serial1.print(s_tx);
        Serial1.print('/');     Serial1.print(s_ok);
        Serial1.print('/');     Serial1.print(s_fail);
        Serial1.print(" lastfail="); Serial1.print(s_lastFail);
        Serial1.print(" srv="); Serial1.print(s_srvSess);
        Serial1.print('/');     Serial1.println(s_srvBytes);
    }
}
