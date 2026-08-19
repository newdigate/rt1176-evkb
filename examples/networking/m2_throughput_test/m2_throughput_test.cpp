// W11: Wi-Fi TCP/UDP throughput services over the IW416 M.2 link.
//
// The board runs four PASSIVE services; the Mac-side peer (tput_peer.py)
// sequences the tests and is the authoritative measurement -- the board's
// `tput:` result lines are the cross-check, not the measurement:
//   :5001 TCP RX sink   -- peer connects + blasts; board counts and discards.
//   :5002 TCP TX source -- peer connects; board blasts a 1 KiB pattern for
//                          TX_BLAST_MS, then reports once the last byte acks.
//   :5003 UDP           -- data datagrams (BE32 seq + 0xA5 fill) are counted;
//                          "TPUT RESET" / "TPUT STATS?" / "TPUT GO <secs>"
//                          control datagrams drive the udp-rx / udp-tx runs.
// All services stay armed between tests; each test re-runs without a reflash.
//
// QEMU proof (run_qemu.sh): the DEVICE-ABSENT path -- QEMU's SD memory card
// ignores CMD5, so the correct outcome is the cmd5-no-response fallback,
// lwip_probe_done, and a heartbeat.  Green in QEMU says nothing about Wi-Fi.
//
// The W5 monitor phase is deliberately absent: NET_MONITOR before ASSOCIATE
// kills this firmware's managed RX-to-host delivery (W8 erratum).
#include "Arduino.h"
#include "HardwareSerial.h"
#include "SdioHost.h"
#include "SdioFunc.h"
#include "Iw416.h"
#include "Iw416Netif.h"
#if defined(HAVE_WIFI_CREDS)
// Generated at configure time from -DM2RADIO_WIFI_SSID/-DM2RADIO_WIFI_PSK.
// Lives only in the (gitignored) build dir -- the PSK is never committed.
#include "wifi_creds.h"
#endif
#include "lwip/init.h"
#include "lwip/netif.h"
#include "lwip/timeouts.h"
#include "lwip/dhcp.h"
#include "lwip/tcp.h"
#include "lwip/udp.h"
#include "netif/ethernet.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

extern "C" { unsigned char g_mac[6] = {0}; }   // Iw416Netif.cpp + ethernetif.c extern this

#if defined(HAVE_IW416_FW)
extern const uint8_t  iw416_fw[];
extern const uint32_t iw416_fw_len;
#endif

static SdioHost sdio;
static SdioFunc func(sdio);
static Iw416 iw416(sdio, func);
static struct netif s_netif;

static bool     s_linkUp = false, s_lwipUp = false;
static uint32_t s_reconnects = 0;

enum { TPUT_TCP_RX_PORT = 5001, TPUT_TCP_TX_PORT = 5002, TPUT_UDP_PORT = 5003 };
enum { UDP_PAYLOAD = 1400, TX_BLAST_MS = 10000 };

static uint32_t tputKbps(uint32_t nbytes, uint32_t ms) {
    return ms ? (uint32_t)(((uint64_t)nbytes * 8u) / ms) : 0;
}

// Clear EVERY callback, then close; on a failed close fall back to tcp_abort.
// Same discipline as m2_lwip_test's closeEcho and for the same reason: once
// the callbacks are cleared the abort fallback cannot re-enter our handlers,
// and the returned err_t is what an IN-CALLBACK caller must hand back to lwip
// -- after tcp_abort() the pcb is gone and only ERR_ABRT tells tcp_input so;
// returning ERR_OK there would leave tcp_input touching a freed pcb.  A
// caller outside a raw-API callback (loop()'s drained-TX path) has no
// tcp_input to signal and ignores the return value.
static void tputClearCallbacks(struct tcp_pcb *pcb) {
    tcp_arg(pcb, nullptr);
    tcp_recv(pcb, nullptr);
    tcp_sent(pcb, nullptr);
    tcp_err(pcb, nullptr);
    tcp_poll(pcb, nullptr, 0);
}
static err_t tputClose(struct tcp_pcb *pcb) {
    tputClearCallbacks(pcb);
    if (tcp_close(pcb) != ERR_OK) {
        tcp_abort(pcb);
        return ERR_ABRT;
    }
    return ERR_OK;
}

// --- TCP RX sink (:5001) -----------------------------------------------------
// Peer connects and blasts; we count bytes and discard.  The first data byte
// starts the clock; the peer's FIN stops it and triggers the report.  The
// listen pcb stays armed, and counters reset on the next accept.
static struct tcp_pcb *s_rxListen = nullptr;
static struct tcp_pcb *s_rxPcb = nullptr;
static uint32_t s_rxBytes = 0, s_rxStartMs = 0, s_rxLastMs = 0;
static bool     s_rxTiming = false;

static err_t rxRecv(void *, struct tcp_pcb *pcb, struct pbuf *p, err_t) {
    if (p == nullptr) {                    // peer FIN: report + free the slot
        uint32_t ms = s_rxTiming ? (millis() - s_rxStartMs) : 0;
        Serial1.print("tput: tcp_rx bytes="); Serial1.print(s_rxBytes);
        Serial1.print(" ms=");   Serial1.print(ms);
        Serial1.print(" kbps="); Serial1.println(tputKbps(s_rxBytes, ms));
        s_rxPcb = nullptr;
        return tputClose(pcb);
    }
    if (!s_rxTiming) { s_rxTiming = true; s_rxStartMs = millis(); }
    s_rxLastMs = millis();
    s_rxBytes += p->tot_len;
    tcp_recved(pcb, p->tot_len);
    pbuf_free(p);
    return ERR_OK;
}
static void rxErr(void *, err_t) {         // pcb already freed by lwip
    s_rxPcb = nullptr;
    Serial1.println("tput: tcp_rx aborted");
}
// Safety valve: one connection at a time means a peer that vanishes without
// FIN or RST would wedge the service forever.  30 s with no data -> abort.
static err_t rxPoll(void *, struct tcp_pcb *pcb) {
    if (millis() - s_rxLastMs > 30000u) {
        Serial1.println("tput: tcp_rx stalled -- abort");
        s_rxPcb = nullptr;
        tputClearCallbacks(pcb);           // else tcp_abort re-enters rxErr
        tcp_abort(pcb);
        return ERR_ABRT;                   // pcb gone; tcp_input must know
    }
    return ERR_OK;
}
static err_t rxAccept(void *, struct tcp_pcb *np, err_t err) {
    if (err != ERR_OK || np == nullptr) return ERR_VAL;
    if (s_rxPcb != nullptr) { tcp_abort(np); return ERR_ABRT; }  // one at a time
    s_rxPcb = np;
    s_rxBytes = 0; s_rxTiming = false;     // counters reset per run
    s_rxLastMs = millis();
    tcp_arg(np, nullptr);
    tcp_recv(np, rxRecv);
    tcp_err(np, rxErr);
    tcp_poll(np, rxPoll, 10);              // ~5 s cadence
    return ERR_OK;
}

// --- TCP TX source (:5002) ---------------------------------------------------
// On accept: blast from a static 1 KiB pattern buffer for TX_BLAST_MS, then
// stop enqueueing and report once the last enqueued byte is acked.  Enqueueing
// happens in bounded bursts (<= 8 chunks per call) from loop() and from the
// tcp_sent callback -- NO_SYS: nothing may monopolize a pass, the main loop
// must keep servicing the netif.
static struct tcp_pcb *s_txPcb = nullptr;
static uint8_t  s_txPat[1024];             // immortal: filled once in setup()
static uint32_t s_txBytes = 0, s_txUnacked = 0, s_txStartMs = 0;
static bool     s_txBlasting = false;

static err_t txFinish(struct tcp_pcb *pcb) {
    uint32_t ms = millis() - s_txStartMs;  // includes the ack-drain tail
    Serial1.print("tput: tcp_tx bytes="); Serial1.print(s_txBytes);
    Serial1.print(" ms=");   Serial1.print(ms);
    Serial1.print(" kbps="); Serial1.println(tputKbps(s_txBytes, ms));
    s_txPcb = nullptr; s_txBlasting = false;
    return tputClose(pcb);
}
// flags 0 (no TCP_WRITE_FLAG_COPY) is deliberate: s_txPat is a static,
// never-mutated buffer, so lwip may reference it in place for the life of any
// segment -- the no-copy contract holds because the buffer is immortal.
static void txPump(struct tcp_pcb *pcb, int maxChunks) {
    int n = 0;
    while (n < maxChunks && tcp_sndbuf(pcb) >= sizeof(s_txPat)) {
        if (tcp_write(pcb, s_txPat, sizeof(s_txPat), 0) != ERR_OK) break;
        s_txBytes   += sizeof(s_txPat);
        s_txUnacked += sizeof(s_txPat);
        n++;
    }
    if (n) tcp_output(pcb);
}
static err_t txSent(void *, struct tcp_pcb *pcb, u16_t len) {
    s_txUnacked = (len >= s_txUnacked) ? 0 : (s_txUnacked - len);
    if (s_txBlasting) {
        txPump(pcb, 8);
        return ERR_OK;
    }
    if (s_txUnacked == 0) return txFinish(pcb);    // last byte acked
    return ERR_OK;
}
static err_t txRecv(void *, struct tcp_pcb *pcb, struct pbuf *p, err_t) {
    if (p == nullptr) {                    // peer bailed mid-blast: wrap up
        Serial1.println("tput: tcp_tx peer_fin");
        s_txPcb = nullptr; s_txBlasting = false;
        return tputClose(pcb);
    }
    tcp_recved(pcb, p->tot_len);           // unexpected data: ack + discard
    pbuf_free(p);
    return ERR_OK;
}
static void txErr(void *, err_t) {         // pcb already freed by lwip
    s_txPcb = nullptr; s_txBlasting = false;
    Serial1.println("tput: tcp_tx aborted");
}
// Safety valve: a peer that stops acking never drains s_txUnacked; cap the
// whole run at 3x the blast window so the single slot cannot wedge.
static err_t txPoll(void *, struct tcp_pcb *pcb) {
    if (millis() - s_txStartMs > (uint32_t)TX_BLAST_MS * 3u) {
        Serial1.println("tput: tcp_tx stalled -- abort");
        s_txPcb = nullptr; s_txBlasting = false;
        tputClearCallbacks(pcb);           // else tcp_abort re-enters txErr
        tcp_abort(pcb);
        return ERR_ABRT;                   // pcb gone; tcp_input must know
    }
    return ERR_OK;
}
static err_t txAccept(void *, struct tcp_pcb *np, err_t err) {
    if (err != ERR_OK || np == nullptr) return ERR_VAL;
    if (s_txPcb != nullptr) { tcp_abort(np); return ERR_ABRT; }  // one at a time
    s_txPcb = np;
    s_txBytes = 0; s_txUnacked = 0;        // counters reset per run
    s_txStartMs = millis(); s_txBlasting = true;
    tcp_arg(np, nullptr);
    tcp_recv(np, txRecv);
    tcp_sent(np, txSent);
    tcp_err(np, txErr);
    tcp_poll(np, txPoll, 10);              // ~5 s cadence
    return ERR_OK;                          // loop() starts the pumping
}

// --- UDP service (:5003) -----------------------------------------------------
// One socket, one udp_recv callback.  Datagrams beginning with ASCII "TPUT "
// are control; everything else is data (BE32 seq at offset 0).  A data seq
// can never read as control: byte 4 of a data datagram is 0xA5, never ' '.
static struct udp_pcb *s_udp = nullptr;
// udp-rx run state.  Start/last stamp the first->last data-datagram span,
// reported as ms= in the STATS reply (the board-side cross-check on the
// peer's delivered-rate figure).  Counters are zeroed ONLY by "TPUT RESET".
static uint32_t s_udpRxCount = 0, s_udpRxHi = 0;
static uint32_t s_udpRxStartMs = 0, s_udpRxLastMs = 0;
static bool     s_udpRxTiming = false;
// udp-tx (GO) run state.  The callback only ARMS this; the blast itself runs
// in loop() -- a 30 s send loop inside a udp_recv callback would starve the
// netif poll (NO_SYS).
static bool      s_udpTxActive = false;
static ip_addr_t s_udpTxDst;
static u16_t     s_udpTxPort = 0;
static uint32_t  s_udpTxDeadline = 0, s_udpTxStartMs = 0;
static uint32_t  s_udpTxCount = 0, s_udpTxDrops = 0, s_udpTxSeq = 0;

static void udpSendText(const ip_addr_t *addr, u16_t port, const char *msg) {
    u16_t len = (u16_t)strlen(msg);
    struct pbuf *p = pbuf_alloc(PBUF_TRANSPORT, len, PBUF_RAM);
    if (p == nullptr) return;
    memcpy(p->payload, msg, len);
    (void)udp_sendto(s_udp, p, addr, port);
    pbuf_free(p);                          // udp_sendto never takes ownership
}

static void udpRecv(void *, struct udp_pcb *, struct pbuf *p,
                    const ip_addr_t *addr, u16_t port) {
    char head[32];
    u16_t got = pbuf_copy_partial(p, head, sizeof(head) - 1, 0);
    head[got] = '\0';
    if (got >= 5 && memcmp(head, "TPUT ", 5) == 0) {         // control
        if (strcmp(head, "TPUT STATS?") == 0) {
            // PURELY idempotent: report only, never reset.  A dropped reply
            // (or a pbuf_alloc failure in udpSendText) must let the peer's
            // retry read the SAME counters -- resetting here would turn one
            // lost reply into a phantom 100%-loss run.
            uint32_t ms = s_udpRxTiming ? (s_udpRxLastMs - s_udpRxStartMs) : 0;
            char msg[64];
            snprintf(msg, sizeof(msg), "TPUT STATS rx=%lu hi=%lu ms=%lu",
                     (unsigned long)s_udpRxCount, (unsigned long)s_udpRxHi,
                     (unsigned long)ms);
            udpSendText(addr, port, msg);
            Serial1.println(msg);
        } else if (strcmp(head, "TPUT RESET") == 0) {
            // Explicit, acked reset.  The peer sends this BEFORE it starts
            // blasting -- no data is in flight yet, so there is no race
            // between the reset and a late-arriving tail from the same run.
            // Re-sending RESET after a lost RESETOK is harmless (idempotent).
            s_udpRxCount = 0; s_udpRxHi = 0; s_udpRxTiming = false;
            udpSendText(addr, port, "TPUT RESETOK");
            Serial1.println("tput: udp_rx reset");
        } else if (strncmp(head, "TPUT GO ", 8) == 0) {
            long secs = strtol(head + 8, nullptr, 10);
            if (secs < 1)  secs = 1;
            if (secs > 30) secs = 30;
            ip_addr_copy(s_udpTxDst, *addr);
            s_udpTxPort     = port;
            s_udpTxStartMs  = millis();
            s_udpTxDeadline = s_udpTxStartMs + (uint32_t)secs * 1000u;
            s_udpTxCount = 0; s_udpTxDrops = 0; s_udpTxSeq = 0;
            s_udpTxActive = true;          // loop() does the blasting
        }
        pbuf_free(p);
        return;
    }
    // data datagram: count it, track the highest BE32 seq; the first one
    // starts the span clock and every one advances it (feeds STATS ms= --
    // the cross-check; the peer's own wall clock stays the measurement).
    if (!s_udpRxTiming) { s_udpRxTiming = true; s_udpRxStartMs = millis(); }
    s_udpRxLastMs = millis();
    s_udpRxCount++;
    if (p->tot_len >= 4) {
        uint8_t s[4];
        pbuf_copy_partial(p, s, 4, 0);
        uint32_t seq = ((uint32_t)s[0] << 24) | ((uint32_t)s[1] << 16) |
                       ((uint32_t)s[2] << 8)  |  (uint32_t)s[3];
        if (seq > s_udpRxHi) s_udpRxHi = seq;
    }
    pbuf_free(p);
}

// One loop() pass of the udp-tx blast: up to 4 datagrams, then yield so the
// netif keeps getting serviced.  Alloc/send failure counts a drop and ends
// the pass (back-pressure, no spin).
static void udpTxPump() {
    if (!s_udpTxActive) return;
    if ((int32_t)(millis() - s_udpTxDeadline) < 0) {
        for (int i = 0; i < 4; i++) {
            struct pbuf *p = pbuf_alloc(PBUF_TRANSPORT, UDP_PAYLOAD, PBUF_RAM);
            if (p == nullptr) { s_udpTxDrops++; break; }
            uint8_t *d = (uint8_t *)p->payload;
            memset(d, 0xA5, UDP_PAYLOAD);
            d[0] = (uint8_t)(s_udpTxSeq >> 24); d[1] = (uint8_t)(s_udpTxSeq >> 16);
            d[2] = (uint8_t)(s_udpTxSeq >> 8);  d[3] = (uint8_t)(s_udpTxSeq);
            err_t e = udp_sendto(s_udp, p, &s_udpTxDst, s_udpTxPort);
            pbuf_free(p);
            if (e != ERR_OK) { s_udpTxDrops++; break; }
            s_udpTxCount++; s_udpTxSeq++;
        }
    } else {
        s_udpTxActive = false;
        char msg[32];
        snprintf(msg, sizeof(msg), "TPUT DONE tx=%lu", (unsigned long)s_udpTxCount);
        udpSendText(&s_udpTxDst, s_udpTxPort, msg);
        uint32_t ms = millis() - s_udpTxStartMs;
        uint32_t nbytes = s_udpTxCount * (uint32_t)UDP_PAYLOAD;
        Serial1.print("tput: udp_tx tx="); Serial1.print(s_udpTxCount);
        Serial1.print(" drops="); Serial1.print(s_udpTxDrops);
        Serial1.print(" ms=");    Serial1.print(ms);
        Serial1.print(" kbps=");  Serial1.println(tputKbps(nbytes, ms));
    }
}

// --- service bring-up --------------------------------------------------------
static struct tcp_pcb *tputListen(u16_t port, tcp_accept_fn fn) {
    struct tcp_pcb *pcb = tcp_new();
    if (pcb == nullptr) return nullptr;
    if (tcp_bind(pcb, IP4_ADDR_ANY, port) != ERR_OK) { tcp_close(pcb); return nullptr; }
    struct tcp_pcb *l = tcp_listen(pcb);   // frees pcb on success only
    if (l == nullptr) { tcp_close(pcb); return nullptr; }
    tcp_accept(l, fn);
    return l;
}
static void tputArmServices() {
    s_rxListen = tputListen(TPUT_TCP_RX_PORT, rxAccept);
    struct tcp_pcb *txl = tputListen(TPUT_TCP_TX_PORT, txAccept);
    s_udp = udp_new();
    if (s_udp != nullptr) {
        if (udp_bind(s_udp, IP4_ADDR_ANY, TPUT_UDP_PORT) == ERR_OK) {
            udp_recv(s_udp, udpRecv, nullptr);
        } else {
            udp_remove(s_udp);
            s_udp = nullptr;
        }
    }
    Serial1.print("tput_services=");
    Serial1.print(s_rxListen ? "rx5001 " : "rx-FAIL ");
    Serial1.print(txl        ? "tx5002 " : "tx-FAIL ");
    Serial1.println(s_udp    ? "udp5003" : "udp-FAIL");
}

// --- bring-up ----------------------------------------------------------------
static SdioHost::Status s_sdioSt = SdioHost::CMD_TIMEOUT;
static SdioHost::Status s_iwSt   = SdioHost::CMD_TIMEOUT;
static SdioHost::Status s_fwSt   = SdioHost::CMD_TIMEOUT;
static bool s_haveCard = false;

static const char *statusName(SdioHost::Status s) {
    switch (s) {
        case SdioHost::OK:               return "ok";
        case SdioHost::NO_IO_FUNCTION:   return "no-io-function";
        case SdioHost::CMD_TIMEOUT:      return "cmd-timeout";
        case SdioHost::CMD_CRC:          return "cmd-crc";
        case SdioHost::CLOCK_UNSTABLE:   return "clock-unstable";
        case SdioHost::BAD_CIS:          return "bad-cis";
        case SdioHost::CMD5_NO_RESPONSE: return "cmd5-no-response";
        case SdioHost::INIT_CLK_STUCK:   return "init-clk-stuck";
    }
    return "unknown";
}

static bool wifiConnect() {
#if defined(HAVE_WIFI_CREDS)
    SdioHost::Status c = iw416.connectStation(M2_WIFI_SSID, M2_WIFI_PSK);
    Serial1.print("connect="); Serial1.print(statusName(c));
    Serial1.print(" last_event=0x"); Serial1.println(iw416.lastEvent(), HEX);
    return c == SdioHost::OK;
#else
    return false;
#endif
}

// M.2 board bring-up preamble, from m2_sdio_probe (W2/W3 evidence there):
// release SDIO_RST (GPIO_AD_16 = GPIO9.15) then WL_RST/PDn (GPIO_AD_31 =
// GPIO9.30, reaching PDn via the hand-bridged R404), with the 1 s ROM-boot
// wait PDn requires, and switch the SDIO pads to 1.8 V.  Without this the
// card either stays in full power-down or is left in the PREVIOUS image's
// state and never answers CMD5 -- measured on silicon during W9 bring-up:
// this exact preamble was missing and the example fell to the fallback path
// until it was added.
#define M2_SDIO_RST_MUX (*(volatile uint32_t *)0x400E814Cu)   // GPIO_AD_16
#define M2_WL_RST_MUX   (*(volatile uint32_t *)0x400E8188u)   // GPIO_AD_31
#define M2_SDIO_RST_BIT 15
#define M2_WL_RST_BIT   30

static void m2ReleaseWifiReset() {
    M2_SDIO_RST_MUX = 0x10u | 0xAu;                 // SION | ALT10 = GPIO9_IO15
    M2_WL_RST_MUX   = 0x10u | 0xAu;                 // SION | ALT10 = GPIO9_IO30
    GPIO9_GDIR |= (1u << M2_SDIO_RST_BIT) | (1u << M2_WL_RST_BIT);
    GPIO9_DR_CLEAR = (1u << M2_SDIO_RST_BIT) | (1u << M2_WL_RST_BIT);
    delay(10);
    GPIO9_DR_SET = (1u << M2_SDIO_RST_BIT);         // SDIO_RST high
    delay(100);
    GPIO9_DR_SET = (1u << M2_WL_RST_BIT);           // then WL_RST / PDn high
    delay(1000);                                    // PDn exit needs ROM boot time
}

void setup() {
    Serial1.begin(115200);
    delay(50);
    Serial1.println("RT1176 M.2 throughput test up");

    for (unsigned i = 0; i < sizeof(s_txPat); i++) s_txPat[i] = (uint8_t)i;

    m2ReleaseWifiReset();
    Serial1.println("m2_wifi_reset=released");
    sdio.useIoVoltage1V8(true);
    Serial1.println("sdio_io_voltage=1v8_requested");

    s_sdioSt = sdio.begin();
    Serial1.print("sdio_begin="); Serial1.print(statusName(s_sdioSt));
    Serial1.print(" rc="); Serial1.println((int)s_sdioSt);
    Serial1.print("int_status=0x"); Serial1.println(sdio.lastIntStatus(), HEX);

#if defined(HAVE_IW416_FW)
    if (s_sdioSt == SdioHost::OK) {
        s_iwSt = iw416.begin();
        if (s_iwSt == SdioHost::OK) {
            s_fwSt = iw416.downloadFirmware(iw416_fw, iw416_fw_len);
            if (s_fwSt == SdioHost::OK) {
                (void)iw416.refreshIoPort();
                delay(50);
                (void)iw416.enableHostInt();
                uint32_t fwRel = 0; uint16_t hwVer = 0;
                if (iw416.getHwSpec(g_mac, &fwRel, &hwVer) == SdioHost::OK) {
                    (void)iw416.reconfigureTxBuffers(2048);
                    (void)iw416.macControl(Iw416::MAC_RX_ON | Iw416::MAC_TX_ON |
                                           Iw416::MAC_ETHERNETII | Iw416::MAC_RTS_CTS);
                    (void)iw416.set11nCfg();
                    (void)iw416.amsduAggrCtrl();
                    s_haveCard = true;
                }
            }
        }
    }
    // Visible bench-failure checkpoint between sdio_begin= and
    // lwip_probe_done: a stall past this line with s_haveCard still false
    // pinpoints init vs. firmware-download vs. GET_HW_SPEC without a debugger.
    if (!s_haveCard) {
        Serial1.print("iw416_init="); Serial1.print(statusName(s_iwSt));
        Serial1.print(" fw="); Serial1.println(statusName(s_fwSt));
    }
#endif

    if (s_haveCard) {
        // Bring the netif up unconditionally: even if the FIRST connect
        // attempt below fails, the stack stays primed so loop()'s !s_linkUp
        // branch can retry wifiConnect() and call netif_set_link_up() +
        // dhcp_start() itself once a later attempt succeeds.
        lwip_init();
        netif_add(&s_netif, IP4_ADDR_ANY4, IP4_ADDR_ANY4, IP4_ADDR_ANY4,
                  &iw416, iw416NetifInit, ethernet_input);
        netif_set_default(&s_netif);
        netif_set_up(&s_netif);
        s_lwipUp = true;
        tputArmServices();     // passive listeners: safe to arm before DHCP
        if (wifiConnect()) {
            s_linkUp = true;
            dhcp_start(&s_netif);
            Serial1.println("lwip_netif_up");
        }
    }
    Serial1.println("lwip_probe_done");
}

void loop() {
    static uint32_t lastBeat = 0, lastStat = 0, beats = 0;
    static uint32_t lastReconnectAttempt = 0;

    if (s_lwipUp) {
        if (s_linkUp && !iw416NetifPoll(&s_netif)) {
            s_linkUp = false;
            dhcp_stop(&s_netif);
            Serial1.println("link_down");
        }
        // Throttle reconnect attempts: a scan+associate attempt takes
        // seconds on its own, so retry at most every 5 s rather than letting
        // a dead AP turn every loop() pass into a fresh 15 s scan.
        if (!s_linkUp && millis() - lastReconnectAttempt >= 5000) {
            lastReconnectAttempt = millis();
            if (wifiConnect()) {
                s_linkUp = true;
                // Also reached when the BOOT-TIME connect in setup() failed
                // and this is the first success afterward: on such a run
                // reconnects=1 means "first successful association", not a
                // link that dropped and came back.
                s_reconnects++;
                netif_set_link_up(&s_netif);
                dhcp_start(&s_netif);
                Serial1.println("link_reup");
            }
        }
        sys_check_timeouts();

        // Bounded service pumps -- each does a small fixed amount of work per
        // pass so the netif poll above never starves.
        if (s_txPcb != nullptr) {
            if (s_txBlasting && millis() - s_txStartMs >= (uint32_t)TX_BLAST_MS)
                s_txBlasting = false;      // stop enqueueing; drain the acks
            if (s_txBlasting) {
                txPump(s_txPcb, 8);
            } else if (s_txUnacked == 0) {
                // Everything acked between tcp_sent callbacks: finish from
                // the main loop.  Not a raw-API callback context, so
                // tputClose's ERR_ABRT return has no tcp_input to go to.
                (void)txFinish(s_txPcb);
            }
        }
        udpTxPump();

        if (millis() - lastStat >= 3000) {
            lastStat = millis();
            const char *st = (s_rxPcb != nullptr) ? "tcprx"
                           : (s_txPcb != nullptr) ? "tcptx"
                           : s_udpTxActive        ? "udptx"
                           : "idle";
            Serial1.print("tput: ip=");
            Serial1.print(ip4addr_ntoa(netif_ip4_addr(&s_netif)));
            Serial1.print(" ps="); Serial1.print(iw416.ieeePsEnabled() ? 1 : 0);
            Serial1.print('/');    Serial1.print(iw416.psSleeps());
            Serial1.print('/');    Serial1.print(iw416.psWakes());
            Serial1.print('/');    Serial1.print(iw416.psHostWakes());
            Serial1.print('/');    Serial1.print(iw416.psConfirmFails());
            // Bus-cost instrument (M2Radio 60280af): CMD52 polls split by
            // producer, CMD53 count/bytes both directions.  cmd53ByteMode is
            // deliberately not printed -- constant 0 by construction.
            Serial1.print(" bus=c52tx:"); Serial1.print(iw416.cmd52PollsTx());
            Serial1.print(",c52svc:");    Serial1.print(iw416.cmd52PollsSvc());
            Serial1.print(",c53:");       Serial1.print(iw416.cmd53Count());
            Serial1.print('/');           Serial1.print(iw416.cmd53Bytes());
            Serial1.print(" state=");      Serial1.print(st);
            Serial1.print(" reconnects="); Serial1.println(s_reconnects);
        }
    } else {
        // Fallback (QEMU / no card / no fw): prove the image stays alive.
        if (millis() - lastBeat >= 1000) {
            lastBeat = millis();
            Serial1.print("alive="); Serial1.println(++beats);
        }
    }
}
