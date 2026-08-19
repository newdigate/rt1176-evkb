// W9: lwip over the IW416 M.2 Wi-Fi link.
//
// Hardware proof: DHCP via lwip's own client, then a raw-API TCP echo client
// against the ESP8266 oracle (192.168.4.1:4712) -- send "M2LWIP hello <n>",
// require the echo byte-exact.  Console: `lwip: ip=... tcp=tx/ok/fail ...`.
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
#include "lwip/init.h"
#include "lwip/netif.h"
#include "lwip/timeouts.h"
#include "lwip/dhcp.h"
#include "lwip/tcp.h"
#include "netif/ethernet.h"

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

// --- TCP echo client (raw API) ----------------------------------------------
static struct tcp_pcb *s_pcb = nullptr;
static uint32_t s_tcpTx = 0, s_tcpOk = 0, s_tcpFail = 0;
static char     s_expect[48];
static uint16_t s_expectLen = 0;
static bool     s_busy = false;
static uint32_t s_started = 0, s_lastKick = 0;

static err_t echoRecv(void *, struct tcp_pcb *pcb, struct pbuf *p, err_t) {
    if (p == nullptr) {                        // remote closed before data
        tcp_close(pcb); s_pcb = nullptr; s_busy = false; s_tcpFail++;
        return ERR_OK;
    }
    bool ok = (p->tot_len == s_expectLen) &&
              (pbuf_memcmp(p, 0, s_expect, s_expectLen) == 0);
    tcp_recved(pcb, p->tot_len);
    pbuf_free(p);
    if (ok) s_tcpOk++; else s_tcpFail++;
    tcp_close(pcb); s_pcb = nullptr; s_busy = false;
    return ERR_OK;
}
static err_t echoConnected(void *, struct tcp_pcb *pcb, err_t) {
    tcp_write(pcb, s_expect, s_expectLen, TCP_WRITE_FLAG_COPY);
    tcp_output(pcb);
    return ERR_OK;
}
static void echoErr(void *, err_t) {           // pcb already freed by lwip
    s_pcb = nullptr; s_busy = false; s_tcpFail++;
}
static void tcpKick() {
    if (s_busy) {
        if (millis() - s_started > 5000 && s_pcb) tcp_abort(s_pcb);  // -> echoErr
        return;
    }
    if (millis() - s_lastKick < 2000) return;
    s_lastKick = millis();
    ip4_addr_t dst; IP4_ADDR(&dst, 192, 168, 4, 1);
    s_pcb = tcp_new();
    if (s_pcb == nullptr) return;
    s_expectLen = (uint16_t)snprintf(s_expect, sizeof(s_expect),
                                     "M2LWIP hello %lu", (unsigned long)s_tcpTx);
    tcp_arg(s_pcb, nullptr);
    tcp_err(s_pcb, echoErr);
    tcp_recv(s_pcb, echoRecv);
    s_busy = true; s_started = millis(); s_tcpTx++;
    if (tcp_connect(s_pcb, &dst, 4712, echoConnected) != ERR_OK)
        tcp_abort(s_pcb);                      // -> echoErr does the bookkeeping
}

// --- bring-up ----------------------------------------------------------------
static SdioHost::Status s_sdioSt = SdioHost::CMD_TIMEOUT;
static SdioHost::Status s_iwSt   = SdioHost::CMD_TIMEOUT;
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

void setup() {
    Serial1.begin(115200);
    delay(50);
    Serial1.println("RT1176 M.2 lwip test up");

    s_sdioSt = sdio.begin();
    Serial1.print("sdio_begin="); Serial1.print(statusName(s_sdioSt));
    Serial1.print(" rc="); Serial1.println((int)s_sdioSt);
    Serial1.print("int_status=0x"); Serial1.println(sdio.lastIntStatus(), HEX);

#if defined(HAVE_IW416_FW)
    if (s_sdioSt == SdioHost::OK) {
        s_iwSt = iw416.begin();
        if (s_iwSt == SdioHost::OK &&
            iw416.downloadFirmware(iw416_fw, iw416_fw_len) == SdioHost::OK) {
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
#endif

    if (s_haveCard && wifiConnect()) {
        s_linkUp = true;
        lwip_init();
        netif_add(&s_netif, IP4_ADDR_ANY4, IP4_ADDR_ANY4, IP4_ADDR_ANY4,
                  &iw416, iw416NetifInit, ethernet_input);
        netif_set_default(&s_netif);
        netif_set_up(&s_netif);
        dhcp_start(&s_netif);
        s_lwipUp = true;
        Serial1.println("lwip_netif_up");
    }
    Serial1.println("lwip_probe_done");
}

void loop() {
    static uint32_t lastBeat = 0, lastStat = 0, beats = 0;

    if (s_lwipUp) {
        if (s_linkUp && !iw416NetifPoll(&s_netif)) {
            s_linkUp = false;
            dhcp_stop(&s_netif);
            Serial1.println("link_down");
        }
        if (!s_linkUp && wifiConnect()) {
            s_linkUp = true;
            s_reconnects++;
            netif_set_link_up(&s_netif);
            dhcp_start(&s_netif);
            Serial1.println("link_reup");
        }
        sys_check_timeouts();
        if (s_linkUp && dhcp_supplied_address(&s_netif)) tcpKick();

        if (millis() - lastStat >= 3000) {
            lastStat = millis();
            Serial1.print("lwip: ip=");
            Serial1.print(ip4addr_ntoa(netif_ip4_addr(&s_netif)));
            Serial1.print(" tcp="); Serial1.print(s_tcpTx);
            Serial1.print('/');     Serial1.print(s_tcpOk);
            Serial1.print('/');     Serial1.print(s_tcpFail);
            Serial1.print(" rx=");  Serial1.print(iw416.rxDataCount());
            Serial1.print(" tx=");  Serial1.print(iw416.dataTxCount());
            Serial1.print(" ring="); Serial1.print(iw416.txPort());
            Serial1.print('/');      Serial1.print(iw416.rxPort());
            Serial1.print('/');      Serial1.print(iw416.rxRingResyncs());
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
