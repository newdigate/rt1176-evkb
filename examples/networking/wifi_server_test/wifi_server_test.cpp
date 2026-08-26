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
// For the soak health line: tcp_active_pcbs / tcp_tw_pcbs.  lwip's internal
// header, deliberately -- MEMP_NUM_TCP_PCB is 5 against WIFI_MAX_CONNS = 4,
// and this one-shot server is the ACTIVE closer, so the board holds every
// TIME_WAIT (~120 s each).  Walking the two lists is the only way to see that
// pressure; without it the pcb-starvation hypothesis stays a hypothesis.
#include "lwip/priv/tcp_priv.h"

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

// ListenError -> name.  Six values: LISTEN_OK and five distinct failures.
static const char *listenErrName(uint8_t e) {
    switch (e) {
    case WiFiServer::LISTEN_OK:     return "LISTEN_OK";
    case WiFiServer::BAD_PORT:      return "BAD_PORT";
    case WiFiServer::NO_LINK:       return "NO_LINK";
    case WiFiServer::NO_PCB:        return "NO_PCB";
    case WiFiServer::BIND_FAILED:   return "BIND_FAILED";
    case WiFiServer::LISTEN_FAILED: return "LISTEN_FAILED";
    default:                        return "UNKNOWN";
    }
}

// --- INSTRUMENT (2026-08-21), at parity with wifi_client_test ---------------
// status() alone cannot say which of begin()'s exits fired: WL_NO_SHIELD is
// the shared exit of five bring-up failures and WL_CONNECT_FAILED of three.
// A silicon run here printed a bare `wifi_status=1` and the bench had no way
// to tell "the scan missed the AP" from "we associated and got no lease".
// NOTE last_event is STICKY across attempts -- on an SSID_NOT_FOUND the card
// never associated, so any event shown is residue from an earlier attempt.
static void reportBegin(int st, uint32_t attempt) {
    Serial1.print("wifi_status="); Serial1.print(st);
    Serial1.print(" attempt=");    Serial1.print(attempt);
    Serial1.print(" err=");        Serial1.print(WiFi.lastError());
    Serial1.print('(');            Serial1.print(WiFiClass::beginErrorName(WiFi.lastError()));
    Serial1.print(") drv=");       Serial1.print(WiFi.lastDriverStatus());
    Serial1.print('(');            Serial1.print(WiFiClass::driverStatusName(WiFi.lastDriverStatus()));
    Serial1.print(") last_event=0x");
    Serial1.print(WiFi.radio().lastEvent(), HEX);
    Serial1.print(" info=0x");
    Serial1.println(WiFi.radio().lastEventInfo(), HEX);
    if (st == WL_CONNECTED) {
        Serial1.print("wifi_ip="); Serial1.println(WiFi.localIP());
        return;
    }
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
            Serial1.print("] ch=");   Serial1.print(aps[i].channel);
            Serial1.print(" rssi=-"); Serial1.print(aps[i].rssi);
            Serial1.print(" ssid='"); Serial1.print(aps[i].ssid);
            Serial1.println("'");
        }
    }
}

static uint32_t s_beginAttempts = 1;
static uint32_t s_lastBeginMs   = 0;

void setup() {
    Serial1.begin(115200);
    delay(50);
    Serial1.println("RT1176 WiFi server test up");
#if defined(HAVE_IW416_FW)
    WiFi.setFirmware(iw416_fw, iw416_fw_len);
#endif
    int st = WiFi.begin(M2_WIFI_SSID, M2_WIFI_PSK);
    reportBegin(st, 1);
    // NOTE: this can print "listening" while wifi_status is a FAILURE, and
    // that is honest rather than a bug -- WiFi.begin() brings lwip up before
    // it attempts the association, so the TCP listener binds fine on a netif
    // that has no address yet.  It just cannot be reached until there is one.
    server.begin();     // with no link this must be a clean, falsy no-op
    Serial1.print("server_begin=");
    Serial1.println(server ? "listening" : "ok_nolink");
    // WHICH of ListenError's SIX values -- LISTEN_OK plus FIVE distinct
    // failures -- on its own line so a gate and a bench can both grep it.
    // "It did not listen" with no cause is not a diagnosable transcript.
    // The NAME is printed beside the ordinal deliberately: the gate greps the
    // name, so renumbering the enum cannot red a gate for a non-semantic
    // reason, and a bench reading a transcript needs no header to hand.
    Serial1.print("server_err=");  Serial1.print(server.lastError());
    Serial1.print(" (");           Serial1.print(listenErrName(server.lastError()));
    Serial1.println(")");
}

void loop() {
    // INSTRUMENT: retry begin() every 15 s while down, reporting each attempt.
    // A first begin() that failed used to be permanent (reconnect intent was
    // raised only by link LOSS, and a link that was never up cannot be lost);
    // the library now arms it, but this reports each attempt so a marginal
    // link is visible as a sequence rather than as one silent failure.
    if (WiFi.status() != WL_CONNECTED && millis() - s_lastBeginMs >= 15000) {
        s_lastBeginMs = millis();
        int rst = WiFi.begin(M2_WIFI_SSID, M2_WIFI_PSK);
        reportBegin(rst, ++s_beginAttempts);
        if (rst == WL_CONNECTED && !server) {
            server.begin();           // listener may have been bound already
            Serial1.print("server_relisten="); Serial1.print(server ? 1 : 0);
            Serial1.print(" err="); Serial1.println(server.lastError());
        }
    }
    const bool up = (WiFi.status() == WL_CONNECTED);
    // Retryable, and it has to be retried here: begin() in setup() runs on a
    // stack that may not be up yet, and a link that arrives late would
    // otherwise leave the server falsy for ever with nothing to say why.
    if (up && !server) {
        server.begin();
        Serial1.print("server_relisten="); Serial1.print(server ? 1 : 0);
        Serial1.print(" err="); Serial1.print(server.lastError());
        Serial1.print(" ("); Serial1.print(listenErrName(server.lastError()));
        Serial1.println(")");
    }
    if (server) {
        WiFiClient c = server.available();
        if (c) {
            s_sessions++;
            // ★ BOUNDED ON BOTH SIDES, and each bound is separate.
            //
            // RX -- BOUNDED BY A SNAPSHOT.  The obvious
            // `while ((n = c.read(buf, sizeof buf)) > 0)` is NOT bounded:
            // since Task 7, read() runs a service pass and re-checks when the
            // staged chain empties, so a peer that keeps sending keeps the
            // loop fed and loop() never returns -- the heartbeat this gate
            // asserts would stop and the board would read as hung.
            // wifi_peer.py is exactly such a peer, so that is the primary
            // case here, not a corner one.  One pass echoes what was staged
            // when it started, and goes home.
            //
            // TX -- BOUNDED BY BAILING ON A SHORT WRITE, which is a different
            // hazard with the same symptom.  WiFiClient::write blocks up to
            // WIFI_TX_TIMEOUT_MS (5 s) waiting for room in lwip's send buffer
            // and then SHORT-WRITES.  Against a peer that stops reading, two
            // things follow and neither is visible without this check:
            //   - the un-written bytes are LOST.  read() already consumed them
            //     from the rx chain, nothing re-queues them, and the peer sees
            //     a truncated echo that reads as packet loss on the link.
            //   - each call can burn 5 s, and that is a FLOOR, not a cap:
            //     the budget restarts on every accepted tcp_write, so a peer
            //     trickling ACKs stretches it (WiFiClient.h).  With
            //     TCP_WND = 11680 a single snapshot is up to ~46 calls of this
            //     size => ~230 s with no heartbeat -- exactly the wedge
            //     `alive=` exists to catch, and it would be this sketch's own
            //     doing.
            // So a short write ends the pass and SAYS SO.  A starved server
            // must be a number, not silence.  (wifi_peer.py always reads, so
            // it cannot reach this; the line is for a bench with a real peer.)
            uint8_t buf[256];
            int budget = c.available();          // snapshot ONCE
            while (budget > 0) {
                size_t want = (budget < (int)sizeof(buf)) ? (size_t)budget
                                                          : sizeof(buf);
                int n = c.read(buf, want);
                if (n <= 0) break;
                int w = (int)c.write(buf, (size_t)n);
                s_bytes += (uint32_t)w;
                budget -= n;
                if (w < n) {                     // tx stalled 5 s; n-w bytes
                    Serial1.print("echo_short=");// are gone for good
                    Serial1.print(n - w);
                    Serial1.println(" (tx stalled -- peer not reading)");
                    break;
                }
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
    // --- SOAK HEALTH (NEW-8), every 2 s, at parity with m2_uap_lwip ---------
    // Two lines, deliberately separate, because they are read differently:
    //
    //   iw416 ...  carries ONLY counters that must be INVARIANT (zero) for
    //              the whole run.  The soak verdict is
    //              `grep '^iw416 ' log | sort -u` producing ONE line -- which
    //              proves no counter was EVER non-zero at ANY sample, where
    //              checking the last sample would only prove clean-at-the-end.
    //              Mixing a varying quantity into this line would destroy
    //              that technique, which is why the pcb pressure is NOT here.
    //
    //   pcb ...    is EXPECTED to vary: live pcbs and TIME_WAIT count, walked
    //              from lwip's own lists.  Exchange rate versus this line is
    //              the number the soak exists to produce (tcp_kill_timewait
    //              self-heals starvation SILENTLY -- no counter, no error --
    //              so watching tw= saturate is the only way to see it work).
    //
    // Safe to walk here: NO_SYS=1, so lwip runs in this loop()'s context and
    // nothing mutates the lists concurrently.  Card-absent both lists are
    // never registered into, so the gate asserts `pcb act=0 tw=0`.
    static uint32_t lastHealth = 0;
    if (millis() - lastHealth >= 2000) {
        lastHealth = millis();
        Iw416 &r = WiFi.radio();
        Serial1.print("iw416 stranded="); Serial1.print(r.rxStrandedRecovered());
        Serial1.print(" desync=");        Serial1.print(r.rxDesyncRecovered());
        Serial1.print(" split=");         Serial1.print(r.rxSplitMismatch());
        Serial1.print(" drop=");          Serial1.print(r.rxDropped());
        Serial1.print(" seqmis=");        Serial1.print(r.seqMismatches());
        Serial1.print(" pswake=");        Serial1.println(r.psWakes());
        uint32_t act = 0, tw = 0;
        for (struct tcp_pcb *p = tcp_active_pcbs; p; p = p->next) act++;
        for (struct tcp_pcb *p = tcp_tw_pcbs;     p; p = p->next) tw++;
        Serial1.print("pcb act="); Serial1.print(act);
        Serial1.print(" tw=");     Serial1.println(tw);
    }
}
