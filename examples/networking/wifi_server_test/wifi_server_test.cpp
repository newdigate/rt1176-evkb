// Arduino WiFiServer proof: accept + echo on :5010, driven by wifi_peer.py on
// the Mac -- which is the AUTHORITATIVE side (it counts its own bytes; the
// board's alive= line is the cross-check, not the measurement).
//
// QEMU (run_qemu.sh) proves ONLY the card-absent fallback: WL_NO_SHIELD, a
// server.begin() that no-ops cleanly with NO_LINK, and a heartbeat that keeps
// running.  The IW416 model returns zero scan results by design, so the board
// never associates and NOT ONE LINE of the accept/echo path below executes
// under any gate in this tree.  Accept, data flow and the pool are silicon-
// only: transcript_hw_evkb.txt + wifi_peer.py.
//
// ★ WHY THIS SKETCH USES available() AND wifi_client_test USES accept() -- the
// two are a deliberate pair, not a duplication:
//
//   - accept() surfaces any accepted conn the sketch has never seen, WITH OR
//     WITHOUT DATA, and claiming it makes it permanently exempt from the
//     pool's evictor and stall valve.  That is what lets wifi_client_test hold
//     ONE session open across quiet periods.
//   - available() surfaces only conns with BYTES STAGED RIGHT NOW.  A peer
//     that connects and says nothing is therefore never claimed here, which
//     leaves it exactly where the pool's two valves can reap it.  That is what
//     makes wifi_peer.py's `fill` test possible at all: 4 silent connections
//     stay unclaimed, so a 5th SYN evicts the least-recently-active of them
//     and evict= in the heartbeat below counts it.  An accept()-based server
//     would instead claim all four within a millisecond, and the 5th would be
//     REFUSED (refuse= climbing) -- a different, equally correct outcome, but
//     not the one this example exists to show.
//
// ★ AND THE CONSEQUENCE, which surprises people coming from upstream Arduino:
// this is a ONE-SHOT server.  `WiFiClient c = server.available();` is a
// refcounted handle, and WiFiConnPool's release() closes the connection when
// the LAST handle dies -- so the conn is closed at the end of the loop() pass
// that echoed it, and the peer must open a new connection per exchange.
// Upstream Ethernet/WiFiNINA keep the socket alive after the handle goes out
// of scope; this library deliberately does not ("Arduino clients don't linger
// after their last handle dies", WiFiConnPool.cpp).  To hold a session open
// across passes, keep the handle in a static -- see wifi_client_test.cpp's
// s_session.  wifi_peer.py is written to this contract.
#include "Arduino.h"
#include "HardwareSerial.h"
#include "WiFi.h"
#include "WiFiClient.h"
#include "WiFiServer.h"
#include "WiFiConnPool.h"

#if defined(HAVE_WIFI_CREDS)
#include "wifi_creds.h"          // generated, gitignored -- never committed
#else
// Deliberately nonexistent SSID: NOT a credential.  It is what the gate uses
// to reach the clean-failure path.
#define M2_WIFI_SSID "WIFI-GATE-NO-SUCH-AP"
#define M2_WIFI_PSK  nullptr
#endif

#if defined(HAVE_IW416_FW)
extern const uint8_t  iw416_fw[];
extern const uint32_t iw416_fw_len;
#endif

static WiFiServer server(5010);
// Un-fakeable by the firmware in the same sense tcp= is in wifi_client_test:
// a byte counted here made a full round trip through lwip and the radio, and
// wifi_peer.py compares its own bytes independently.
static uint32_t s_sessions = 0, s_bytes = 0;

void setup() {
    Serial1.begin(115200);
    delay(50);
    Serial1.println("RT1176 WiFi server test up");
#if defined(HAVE_IW416_FW)
    WiFi.setFirmware(iw416_fw, iw416_fw_len);
#endif
    int st = WiFi.begin(M2_WIFI_SSID, M2_WIFI_PSK);
    Serial1.print("wifi_status="); Serial1.println(st);
    if (st == WL_CONNECTED) {
        Serial1.print("wifi_ip="); Serial1.println(WiFi.localIP());
    }
    server.begin();     // with no link this must be a clean, falsy no-op
    Serial1.print("server_begin=");
    Serial1.println(server ? "listening" : "ok_nolink");
    // WHICH of begin()'s five exits, on its own line so a gate and a bench can
    // both grep it: 0 = LISTEN_OK, 1 = BAD_PORT, 2 = NO_LINK, 3 = NO_PCB,
    // 4 = BIND_FAILED, 5 = LISTEN_FAILED.  "It did not listen" with no cause
    // is not a diagnosable transcript.
    Serial1.print("server_err="); Serial1.println(server.lastError());
}

void loop() {
    const bool up = (WiFi.status() == WL_CONNECTED);
    // Retryable, and it has to be retried here: begin() in setup() runs on a
    // stack that may not be up yet, and a link that arrives late would
    // otherwise leave the server falsy for ever with nothing to say why.
    if (up && !server) {
        server.begin();
        Serial1.print("server_relisten="); Serial1.print(server ? 1 : 0);
        Serial1.print(" err="); Serial1.println(server.lastError());
    }
    if (server) {
        WiFiClient c = server.available();
        if (c) {
            s_sessions++;
            // ★ BOUNDED BY A SNAPSHOT, and it has to be.  The obvious
            // `while ((n = c.read(buf, sizeof buf)) > 0)` is NOT bounded:
            // since Task 7, read() runs a service pass and re-checks when the
            // staged chain empties, so a peer that keeps sending keeps the
            // loop fed and loop() never returns -- the heartbeat this gate
            // asserts would stop and the board would read as hung.
            // wifi_peer.py is exactly such a peer, so that is the primary
            // case here, not a corner one.  One pass echoes what was staged
            // when it started, and goes home.
            uint8_t buf[256];
            int budget = c.available();          // snapshot ONCE
            while (budget > 0) {
                size_t want = (budget < (int)sizeof(buf)) ? (size_t)budget
                                                          : sizeof(buf);
                int n = c.read(buf, want);
                if (n <= 0) break;
                s_bytes += (uint32_t)c.write(buf, (size_t)n);
                budget -= n;
            }
        }   // c dies here -> last handle -> the conn is CLOSED.  See the ★
    }       // block at the top: this is a one-shot server, on purpose.
    static uint32_t lastBeat = 0, beats = 0;
    if (millis() - lastBeat >= 1000) {
        lastBeat = millis();
        Serial1.print("alive=");  Serial1.print(++beats);
        // All three "a connection vanished / was refused" counters, plus the
        // listener's own failure code.  This example exists to be diagnosed
        // from its transcript: a starved server must be a number, not silence.
        // evict=  an unclaimed accept was dropped to make room for a newer one
        //         (the valve wifi_peer.py's `fill` test exists to fire).
        // stall=  connPoll reaped an unheld, unclaimed conn at 30-40 s idle.
        // refuse= no valve could fire because every slot was CLAIMED -- that
        //         one is a bug in the SKETCH, not in the pool, and cannot
        //         happen here (this sketch holds no handle across passes).
        Serial1.print(" srv=");    Serial1.print(server ? 1 : 0);
        Serial1.print('/');        Serial1.print(server.lastError());
        Serial1.print(" sess=");   Serial1.print(s_sessions);
        Serial1.print('/');        Serial1.print(s_bytes);
        Serial1.print(" evict=");  Serial1.print(WiFiPool::evictions());
        Serial1.print(" stall=");  Serial1.print(WiFiPool::stallAborts());
        Serial1.print(" refuse="); Serial1.println(WiFiPool::acceptRefusals());
    }
}
