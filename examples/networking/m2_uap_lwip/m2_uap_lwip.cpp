// M.2 IW416 as an ACCESS POINT, with an lwip netif on top (W17, Phase 1 step 4).
//
// ★ THIS EXAMPLE TRANSMITS.  Hosting a network is its entire purpose, so unlike
// networking/m2_uap_probe -- which asks questions of the firmware and keeps its
// RF-emitting rows compiled out -- there is no opt-in here.  Building this for
// hardware is a deliberate act.  It hosts an OPEN network with nothing routed
// behind it, and -- unlike m2_uap_probe, which starts a BSS and stops it again
// within one run -- it hosts INDEFINITELY: there is no BSS_STOP here, because an
// access point that switches itself off after a minute is not an access point.
// The board beacons until it is reset or reflashed.  Say so out loud rather
// than leaving someone to discover it from a scan.
//
// What it demonstrates, and why each part is the un-fakeable version:
//   * uapConfigure() + BSS_START bring a real BSS up.  Silicon-proven: a foreign
//     radio sees the SSID appear and disappear around START/STOP (W17 run 8).
//   * A SECOND lwip netif is bound to that BSS, and frames are routed to it BY
//     THE RxPD's bss tag -- not by assuming everything belongs to one stack.
//     The card runs both interfaces over ONE set of rings, tagged per packet;
//     that was verified on silicon rather than inherited (99/99 uAP frames
//     tagged bss_type=1, none mis-tagged).
//   * A UDP socket is bound on the AP's address.  Packets a CLIENT sends
//     arriving at that socket is the proof the upstack works end to end: they
//     have travelled air -> RxPD -> demux -> netif -> lwip -> socket, and the
//     payload is one this firmware never generates.
//
// NOT here, deliberately: no DHCP server (clients use static addresses -- the
// handoff's zero-dependency Phase 1), and no routing or NAT between the STA and
// uAP sides, which is explicitly out of scope until the rest soaks.
//
// Bench client: tools/esp8266-uapclient (static 192.168.44.50, broadcasts to
// 192.168.44.255:5001 once a second, power save off).
#include <Arduino.h>
#include "SdioHost.h"
#include "SdioFunc.h"
#include "Iw416.h"
#include "Iw416Netif.h"
extern "C" {
#include "lwip/init.h"
#include "lwip/netif.h"
#include "lwip/timeouts.h"
#include "lwip/udp.h"
#include "netif/ethernet.h"
}

#if defined(HAVE_IW416_FW)
extern const uint8_t iw416_fw[];
extern const uint32_t iw416_fw_len;
#endif

static SdioHost sdio;
static SdioFunc func(sdio);
static Iw416    iw416(sdio, func);
extern "C" { unsigned char g_mac[6] = {0}; }   // Iw416Netif.cpp externs this

static struct netif s_uapNif;
static struct udp_pcb *s_pcb = nullptr;
static uint32_t s_udpRx = 0, s_udpBytes = 0;
static bool     s_dumped = false;
static bool     s_haveCard = false, s_bssUp = false;

// The AP's own address.  Static on both sides: there is no DHCP server here, so
// a client must be configured to match (192.168.44.50 in the bench sketch).
#define AP_ADDR0 192
#define AP_ADDR1 168
#define AP_ADDR2 44
#define AP_ADDR3 1
#define UDP_PORT 5001

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
        default:                         return "unknown";
    }
}

static void dumpBytes(const uint8_t *p, uint16_t n) {
    for (uint16_t i = 0; i < n; i++) {
        if (p[i] < 0x10) Serial1.print('0');
        Serial1.print(p[i], HEX);
    }
}

// Every packet that reaches here has come off the air, through the RxPD demux,
// into the uAP netif and up through lwip to a bound socket.  The first one is
// dumped whole: a counter says "something arrived", the bytes say WHAT.
static void onUdp(void *, struct udp_pcb *, struct pbuf *p,
                  const ip_addr_t *addr, u16_t port) {
    if (p == nullptr) return;
    s_udpRx++;
    s_udpBytes += p->tot_len;
    if (!s_dumped && p->tot_len) {
        s_dumped = true;
        static uint8_t buf[64];
        uint16_t n = p->tot_len > sizeof(buf) ? (uint16_t)sizeof(buf) : p->tot_len;
        pbuf_copy_partial(p, buf, n, 0);
        Serial1.print("uap_udp_first from=");
        Serial1.print(ip4addr_ntoa(ip_2_ip4(addr)));
        Serial1.print(":");    Serial1.print(port);
        Serial1.print(" len="); Serial1.print(p->tot_len);
        Serial1.print(" ascii=");
        for (uint16_t i = 0; i < n; i++)
            Serial1.print((buf[i] >= 32 && buf[i] < 127) ? (char)buf[i] : '.');
        Serial1.print(" bytes="); dumpBytes(buf, n);
        Serial1.println();
    }
    pbuf_free(p);
}

// --- board preamble (identical to m2_lwip_test / m2_uap_probe) ---------------
#define M2_SDIO_RST_MUX (*(volatile uint32_t *)0x400E814Cu)
#define M2_WL_RST_MUX   (*(volatile uint32_t *)0x400E8188u)
#define M2_SDIO_RST_BIT 15
#define M2_WL_RST_BIT   30

static void m2ReleaseWifiReset() {
    M2_SDIO_RST_MUX = 0x10u | 0xAu;
    M2_WL_RST_MUX   = 0x10u | 0xAu;
    GPIO9_GDIR |= (1u << M2_SDIO_RST_BIT) | (1u << M2_WL_RST_BIT);
    GPIO9_DR_CLEAR = (1u << M2_SDIO_RST_BIT) | (1u << M2_WL_RST_BIT);
    delay(10);
    GPIO9_DR_SET = (1u << M2_SDIO_RST_BIT);
    delay(100);
    GPIO9_DR_SET = (1u << M2_WL_RST_BIT);
    delay(1000);
}

void setup() {
    Serial1.begin(115200);
    delay(50);
    Serial1.println("RT1176 M.2 uAP + lwip up");

    m2ReleaseWifiReset();
    sdio.useIoVoltage1V8(true);
    SdioHost::Status s = sdio.begin();
    Serial1.print("sdio_begin="); Serial1.println(statusName(s));
    if (s != SdioHost::OK) { Serial1.println("uap_lwip_done"); return; }

    s = iw416.begin();
    Serial1.print("iw416_begin="); Serial1.println(statusName(s));
    if (s != SdioHost::OK) { Serial1.println("uap_lwip_done"); return; }

#if defined(HAVE_IW416_FW)
    s = iw416.downloadFirmware(iw416_fw, iw416_fw_len);
    Serial1.print("fw_download="); Serial1.println(statusName(s));
#else
    s = (iw416.fwStatus() == Iw416::FIRMWARE_READY) ? SdioHost::OK : SdioHost::CMD_TIMEOUT;
    Serial1.print("fw_download=skipped preboot="); Serial1.println(s == SdioHost::OK ? 1 : 0);
#endif
    if (s != SdioHost::OK) { Serial1.println("uap_lwip_done"); return; }

    (void)iw416.refreshIoPort();
    delay(50);
    (void)iw416.enableHostInt();
    uint32_t rel = 0; uint16_t hw = 0;
    if (iw416.getHwSpec(g_mac, &rel, &hw) != SdioHost::OK) {
        Serial1.println("hwspec=FAILED"); Serial1.println("uap_lwip_done"); return;
    }
    Serial1.print("fw_release=0x"); Serial1.print(rel, HEX);
    Serial1.print(" mac=");         dumpBytes(g_mac, 6);
    Serial1.println();
    s_haveCard = true;

    // --- bring the BSS up ---------------------------------------------------
    // ★ A POPULATED configuration, always.  A minimal SYS_CONFIGURE kills this
    // firmware's command port outright (W17 FAULT 1, reproduced 5/5), so there
    // is no such thing as a harmless partial config or a config GET here.
    Iw416::UapConfig cfg;
    cfg.ssid         = M2_UAP_SSID;
    cfg.channel      = M2_UAP_CHANNEL;
    cfg.mac          = g_mac;
    cfg.beaconPeriod = 100;
    cfg.dtimPeriod   = 1;
    cfg.bcastSsidCtl = 1;
    cfg.tlvMask      = Iw416::UAP_TLV_ALL_OPEN;
    SdioHost::Status cs = iw416.uapConfigure(cfg);
    Serial1.print("uap_configure="); Serial1.print(statusName(cs));
    Serial1.print(" result=0x");     Serial1.println(iw416.lastRespResult(), HEX);
    if (cs != SdioHost::OK || iw416.lastRespResult() != Iw416::RESULT_OK) {
        Serial1.println("uap_lwip_done"); return;
    }

    uint8_t rx[Iw416::SDIO_BLOCK_SIZE]; uint16_t rxLen = 0;
    SdioHost::Status bs = iw416.sendHostCmdBss(Iw416::CMD_APCMD_BSS_START, nullptr, 0,
                                               Iw416::BSS_TYPE_UAP, 0);
    if (bs == SdioHost::OK)
        bs = iw416.waitCmdResp(Iw416::CMD_APCMD_BSS_START, rx, sizeof(rx), &rxLen, 5000);
    Serial1.print("uap_bss_start="); Serial1.print(statusName(bs));
    Serial1.print(" result=0x");     Serial1.println(iw416.lastRespResult(), HEX);
    if (bs != SdioHost::OK || iw416.lastRespResult() != Iw416::RESULT_OK) {
        Serial1.println("uap_lwip_done"); return;
    }
    s_bssUp = true;
    Serial1.print("uap_hosting ssid="); Serial1.print(M2_UAP_SSID);
    Serial1.print(" chan=");            Serial1.println(M2_UAP_CHANNEL);

    // --- the second netif ---------------------------------------------------
    lwip_init();
    ip4_addr_t ip, mask, gw;
    IP4_ADDR(&ip,   AP_ADDR0, AP_ADDR1, AP_ADDR2, AP_ADDR3);
    IP4_ADDR(&mask, 255, 255, 255, 0);
    IP4_ADDR(&gw,   AP_ADDR0, AP_ADDR1, AP_ADDR2, AP_ADDR3);
    netif_add(&s_uapNif, &ip, &mask, &gw, &iw416, iw416NetifInitUap, ethernet_input);
    netif_set_default(&s_uapNif);
    netif_set_up(&s_uapNif);
    netif_set_link_up(&s_uapNif);
    Serial1.print("uap_netif_up addr=");
    Serial1.print(AP_ADDR0); Serial1.print('.'); Serial1.print(AP_ADDR1); Serial1.print('.');
    Serial1.print(AP_ADDR2); Serial1.print('.'); Serial1.println(AP_ADDR3);

    s_pcb = udp_new();
    if (s_pcb && udp_bind(s_pcb, IP_ANY_TYPE, UDP_PORT) == ERR_OK) {
        udp_recv(s_pcb, onUdp, nullptr);
        Serial1.print("uap_udp_bound port="); Serial1.println(UDP_PORT);
    } else {
        Serial1.println("uap_udp_bound=FAILED");
    }
    Serial1.println("uap_lwip_done");
}

void loop() {
    static uint32_t last = 0;
    if (s_haveCard && s_bssUp) {
        (void)iw416NetifPollDual(nullptr, &s_uapNif);
        sys_check_timeouts();
    }
    if (millis() - last >= 2000) {
        last = millis();
        Serial1.print("hb card=");   Serial1.print(s_haveCard ? 1 : 0);
        Serial1.print(" bss=");      Serial1.print(s_bssUp ? 1 : 0);
        Serial1.print(" udp_rx=");   Serial1.print(s_udpRx);
        Serial1.print(" udp_bytes=");Serial1.print(s_udpBytes);
        Serial1.print(" rx_bss0=");  Serial1.print(iw416.rxFramesByBss(0));
        Serial1.print(" rx_bss1=");  Serial1.print(iw416.rxFramesByBss(1));
        Serial1.print(" unrouted="); Serial1.println(iw416NetifUnroutedFrames());
    }
}
