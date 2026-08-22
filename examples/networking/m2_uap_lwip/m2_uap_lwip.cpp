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
//   * A MINIMAL DHCP SERVER, so a client needs no prior knowledge of this
//     network.  lwip ships a DHCP client only, never a server, so this is
//     written here rather than imported.  It is deliberately the smallest thing
//     that works: one /24, a small pool, DISCOVER/REQUEST only, no persistence,
//     no conflict detection, no RENEW handling beyond answering it like a fresh
//     REQUEST.  It lives in the EXAMPLE and not in the library on purpose --
//     it has not soaked, and promoting an unsoaked API is how a bad interface
//     becomes permanent.  Promote it once it has.
//
// NOT here, deliberately: no routing or NAT between the STA and uAP sides,
// which is explicitly out of scope until the rest soaks.
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
#include "lwip/etharp.h"
#include "netif/ethernet.h"
}

#if defined(HAVE_UAP_PSK)
#include "uap_creds.h"          // generated, gitignored -- see CMakeLists.txt
#endif

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
static uint16_t s_staCount = 0xFFFF;      // 0xFFFF = never successfully read
static uint32_t s_joins = 0, s_leaves = 0;

// --- ARP loss probe (opt-in) -------------------------------------------------
// W17 left ~17% of AP->client ARP requests unanswered with client power save
// OFF, unexplained.  This measures it properly.  Two things were wrong with the
// earlier measurement and both are fixed here:
//
//  1. IT AGGREGATED.  "25 replies to 30 requests" cannot say whether the misses
//     were spread evenly or arrived in one burst, and those imply different
//     causes.  This records a per-request HIT or MISS.
//  2. IT HAD A BUILT-IN LAG.  A request was sent at the end of a 5 s window and
//     its reply counted in the NEXT one, so the last request could never be
//     counted and the ratio was structurally pessimistic by one.  Here each
//     request gets its own bounded wait.
//
// The variable under test is the DESTINATION.  A broadcast frame from an AP is
// buffered for DTIM delivery and sent at a basic rate; a unicast frame is not.
// If the loss is in that path, unicast removes it -- and if it does not, the
// broadcast path is exonerated and the next suspect is the client.
#ifndef ARP_PROBE_COUNT
#define ARP_PROBE_COUNT 40
#endif
#ifndef ARP_PROBE_WAIT_MS
#define ARP_PROBE_WAIT_MS 400
#endif
static uint8_t  s_clientMac[6] = {0};
static uint8_t  s_clientLast = 0;         // host byte of the lease we handed out
static bool     s_haveClient = false;
static uint32_t s_arpSent = 0, s_arpHit = 0, s_arpMiss = 0;
static bool     s_arpSeen = false;        // set by the sink for THIS request
static bool     s_arpDone = false;

// The AP's own address.  Static on both sides: there is no DHCP server here, so
// a client must be configured to match (192.168.44.50 in the bench sketch).
#define AP_ADDR0 192
#define AP_ADDR1 168
#define AP_ADDR2 44
#define AP_ADDR3 1
#define UDP_PORT 5001

// --- minimal DHCP server ----------------------------------------------------
// RFC 2131 wire layout: a 236-byte fixed header, the magic cookie, then
// options.  Only what a client needs to take an address is implemented.
#define DHCP_SERVER_PORT 67
#define DHCP_CLIENT_PORT 68
#define DHCP_OP_REQUEST  1
#define DHCP_OP_REPLY    2
#define DHCP_MSG_DISCOVER 1
#define DHCP_MSG_OFFER    2
#define DHCP_MSG_REQUEST  3
#define DHCP_MSG_ACK      5
#define DHCP_OPT_MSGTYPE  53
#define DHCP_OPT_SERVERID 54
#define DHCP_OPT_LEASE    51
#define DHCP_OPT_MASK      1
#define DHCP_OPT_ROUTER    3
#define DHCP_OPT_DNS       6
#define DHCP_OPT_END     255
#define DHCP_HDR_LEN     236
// Pool: .100 .. .109.  Small on purpose -- the card's own limit is eight
// stations, so a large pool would only paper over a lease leak.
#define POOL_FIRST 100
#define POOL_COUNT 10

struct Lease { uint8_t mac[6]; uint8_t last; bool used; };
static Lease s_leases[POOL_COUNT];
static struct udp_pcb *s_dhcpPcb = nullptr;
static uint32_t s_dhcpDiscover = 0, s_dhcpRequest = 0, s_dhcpAck = 0, s_dhcpNoPool = 0;
static uint32_t s_dhcpBcastReplies = 0;   // how many had to go out broadcast

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
// The netif poll delivers into lwip, so an ARP reply is consumed by etharp and
// never reaches an application callback.  Watching for it therefore has to
// happen at the FRAME level, which is what iw416NetifPollDual's sink does --
// but that sink is inside the library.  Rather than reach into it, the probe
// asks a simpler question of lwip itself: after a request, does the client's
// address appear in the ARP CACHE?  A cache entry can only come from a reply.
static bool clientInArpCache(void) {
    ip4_addr_t want;
    IP4_ADDR(&want, AP_ADDR0, AP_ADDR1, AP_ADDR2, s_clientLast);
    for (int i = 0; i < ARP_TABLE_SIZE; i++) {
        ip4_addr_t *ip = nullptr;
        struct netif *nif = nullptr;
        struct eth_addr *eth = nullptr;
        if (etharp_get_entry(i, &ip, &nif, &eth) &&
            ip && ip4_addr_cmp(ip, &want)) return true;
    }
    return false;
}

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

// One address per MAC, and the SAME address on a repeat request -- a client
// that DISCOVERs, then REQUESTs, must be offered and acked the same thing or it
// will refuse the lease.  Returns the host byte, or 0 when the pool is full.
static uint8_t leaseFor(const uint8_t *mac) {
    for (int i = 0; i < POOL_COUNT; i++)
        if (s_leases[i].used && memcmp(s_leases[i].mac, mac, 6) == 0)
            return s_leases[i].last;
    for (int i = 0; i < POOL_COUNT; i++)
        if (!s_leases[i].used) {
            memcpy(s_leases[i].mac, mac, 6);
            s_leases[i].last = (uint8_t)(POOL_FIRST + i);
            s_leases[i].used = true;
            return s_leases[i].last;
        }
    return 0;
}

static uint8_t dhcpMsgType(const uint8_t *o, uint16_t n) {
    for (uint16_t i = 0; i + 1 < n;) {
        uint8_t t = o[i];
        if (t == DHCP_OPT_END) break;
        if (t == 0) { i++; continue; }              // pad
        uint8_t l = o[i + 1];
        if (t == DHCP_OPT_MSGTYPE && l == 1 && i + 2 < n) return o[i + 2];
        i += 2 + l;
    }
    return 0;
}

static void dhcpReply(const uint8_t *req, uint16_t reqLen, uint8_t msgType, uint8_t last) {
    struct pbuf *p = pbuf_alloc(PBUF_TRANSPORT, 300, PBUF_RAM);
    if (p == nullptr) return;
    uint8_t *b = (uint8_t *)p->payload;
    memset(b, 0, 300);
    b[0] = DHCP_OP_REPLY;
    b[1] = 1; b[2] = 6;                             // Ethernet, 6-byte MAC
    memcpy(&b[4], &req[4], 4);                      // xid -- echoed, or ignored
    memcpy(&b[10], &req[10], 2);                    // flags (broadcast bit)
    b[16] = AP_ADDR0; b[17] = AP_ADDR1; b[18] = AP_ADDR2; b[19] = last;   // yiaddr
    b[20] = AP_ADDR0; b[21] = AP_ADDR1; b[22] = AP_ADDR2; b[23] = AP_ADDR3; // siaddr
    memcpy(&b[28], &req[28], 16);                   // chaddr
    uint16_t o = DHCP_HDR_LEN;
    b[o++] = 0x63; b[o++] = 0x82; b[o++] = 0x53; b[o++] = 0x63;   // magic cookie
    b[o++] = DHCP_OPT_MSGTYPE; b[o++] = 1; b[o++] = msgType;
    b[o++] = DHCP_OPT_SERVERID; b[o++] = 4;
    b[o++] = AP_ADDR0; b[o++] = AP_ADDR1; b[o++] = AP_ADDR2; b[o++] = AP_ADDR3;
    b[o++] = DHCP_OPT_LEASE; b[o++] = 4;
    b[o++] = 0x00; b[o++] = 0x00; b[o++] = 0x0E; b[o++] = 0x10;   // 3600 s
    b[o++] = DHCP_OPT_MASK; b[o++] = 4;
    b[o++] = 255; b[o++] = 255; b[o++] = 255; b[o++] = 0;
    b[o++] = DHCP_OPT_ROUTER; b[o++] = 4;
    b[o++] = AP_ADDR0; b[o++] = AP_ADDR1; b[o++] = AP_ADDR2; b[o++] = AP_ADDR3;
    b[o++] = DHCP_OPT_DNS; b[o++] = 4;
    b[o++] = AP_ADDR0; b[o++] = AP_ADDR1; b[o++] = AP_ADDR2; b[o++] = AP_ADDR3;
    b[o++] = DHCP_OPT_END;
    pbuf_realloc(p, o);
    // ★ UNICAST WHEN THE CLIENT ALLOWS IT, and this is a measured decision
    // rather than a preference.  802.11 acknowledges and RETRIES unicast frames
    // and does neither for broadcast, so a broadcast from an AP is lost
    // outright when it collides.  Measured here, same client, same channel,
    // one variable: 40 ARP requests broadcast -> 34 answered; 40 unicast -> 40.
    // A DHCP reply sent to the broadcast address inherits that ~15%, and a lost
    // OFFER makes the client start over -- which is exactly the dhcp_disc=4 /
    // dhcp_req=3 asymmetry seen over three rejoins before this change.
    //
    // RFC 2131 s4.1: if the client set the BROADCAST flag it cannot receive a
    // unicast before it is configured, and the reply MUST be broadcast.  Most
    // clients do not set it.  When it is clear, the reply goes to the address
    // being offered -- which needs an ARP entry the client cannot yet answer
    // for, so one is installed directly from the chaddr we are replying to.
    const bool mustBroadcast = (req[10] & 0x80) != 0;
    if (mustBroadcast) {
        s_dhcpBcastReplies++;
        (void)udp_sendto_if(s_dhcpPcb, p, IP_ADDR_BROADCAST, DHCP_CLIENT_PORT, &s_uapNif);
    } else {
        ip4_addr_t cli;
        IP4_ADDR(&cli, AP_ADDR0, AP_ADDR1, AP_ADDR2, last);
        struct eth_addr eth;
        memcpy(eth.addr, &req[28], 6);          // chaddr
        (void)etharp_add_static_entry(&cli, &eth);
        (void)udp_sendto_if(s_dhcpPcb, p, &cli, DHCP_CLIENT_PORT, &s_uapNif);
    }
    pbuf_free(p);
}

static void onDhcp(void *, struct udp_pcb *, struct pbuf *p,
                   const ip_addr_t *, u16_t) {
    if (p == nullptr) return;
    if (p->tot_len >= DHCP_HDR_LEN + 4) {
        static uint8_t buf[576];
        uint16_t n = p->tot_len > sizeof(buf) ? (uint16_t)sizeof(buf) : p->tot_len;
        pbuf_copy_partial(p, buf, n, 0);
        if (buf[0] == DHCP_OP_REQUEST) {
            const uint8_t t = dhcpMsgType(&buf[DHCP_HDR_LEN + 4],
                                          (uint16_t)(n - DHCP_HDR_LEN - 4));
            const uint8_t *mac = &buf[28];
            const uint8_t last = leaseFor(mac);
            if (last == 0) {
                s_dhcpNoPool++;
            } else if (t == DHCP_MSG_DISCOVER) {
                s_dhcpDiscover++;
                dhcpReply(buf, n, DHCP_MSG_OFFER, last);
            } else if (t == DHCP_MSG_REQUEST) {
                s_dhcpRequest++;
                dhcpReply(buf, n, DHCP_MSG_ACK, last);
                s_dhcpAck++;
                memcpy(s_clientMac, mac, 6);
                s_clientLast = last;
                s_haveClient = true;
                Serial1.print("uap_dhcp_ack mac=");  dumpBytes(mac, 6);
                Serial1.print(" ip=");
                Serial1.print(AP_ADDR0); Serial1.print('.'); Serial1.print(AP_ADDR1);
                Serial1.print('.'); Serial1.print(AP_ADDR2); Serial1.print('.');
                Serial1.println(last);
            }
        }
    }
    pbuf_free(p);
}

#if defined(ARP_LOSS_PROBE)
static void arpLossProbe(void) {
    if (!s_haveClient || s_arpDone) return;
    ip4_addr_t target;
    IP4_ADDR(&target, AP_ADDR0, AP_ADDR1, AP_ADDR2, s_clientLast);
    Serial1.print("arp_probe begin dst=");
#if defined(ARP_PROBE_UNICAST)
    Serial1.print("unicast");
#else
    Serial1.print("broadcast");
#endif
    Serial1.print(" target=");   Serial1.print(ip4addr_ntoa(&target));
    Serial1.print(" n=");        Serial1.print(ARP_PROBE_COUNT);
    Serial1.print(" wait_ms=");  Serial1.println(ARP_PROBE_WAIT_MS);

    for (int i = 0; i < ARP_PROBE_COUNT; i++) {
        // Clear the cache entry first, so each iteration measures a FRESH
        // exchange rather than reading last round's answer.  Without this the
        // probe would report 100% from the second request onward and prove
        // nothing at all.
        etharp_cleanup_netif(&s_uapNif);
#if defined(ARP_PROBE_UNICAST)
        (void)etharp_query(&s_uapNif, &target, NULL);   // lwip unicasts once cached
        struct pbuf *q = pbuf_alloc(PBUF_RAW, 42, PBUF_RAM);
        if (q) {
            uint8_t *f = (uint8_t *)q->payload;
            memcpy(&f[0], s_clientMac, 6);              // dst: the CLIENT, not FF..
            memcpy(&f[6], g_mac, 6);
            f[12] = 0x08; f[13] = 0x06;
            f[14] = 0x00; f[15] = 0x01; f[16] = 0x08; f[17] = 0x00;
            f[18] = 6;    f[19] = 4;    f[20] = 0x00; f[21] = 0x01;
            memcpy(&f[22], g_mac, 6);
            f[28] = AP_ADDR0; f[29] = AP_ADDR1; f[30] = AP_ADDR2; f[31] = AP_ADDR3;
            memset(&f[32], 0, 6);
            f[38] = AP_ADDR0; f[39] = AP_ADDR1; f[40] = AP_ADDR2; f[41] = s_clientLast;
            (void)s_uapNif.linkoutput(&s_uapNif, q);
            pbuf_free(q);
        }
#else
        (void)etharp_request(&s_uapNif, &target);       // lwip broadcasts this
#endif
        s_arpSent++;
        const uint32_t t0 = millis();
        bool hit = false;
        while (millis() - t0 < ARP_PROBE_WAIT_MS) {
            (void)iw416NetifPollDual(nullptr, &s_uapNif);
            sys_check_timeouts();
            if (clientInArpCache()) { hit = true; break; }
        }
        if (hit) s_arpHit++; else s_arpMiss++;
        Serial1.print(hit ? "." : "X");
        if ((i % 40) == 39) Serial1.println();
    }
    Serial1.println();
    Serial1.print("arp_probe done sent="); Serial1.print(s_arpSent);
    Serial1.print(" hit=");                Serial1.print(s_arpHit);
    Serial1.print(" miss=");               Serial1.print(s_arpMiss);
    Serial1.print(" pct=");
    Serial1.println((int)((s_arpHit * 100) / (s_arpSent ? s_arpSent : 1)));
    s_arpDone = true;
}
#endif

#if defined(UAP_TX_PROBE)
/*
 * Send one ARP request on the uAP netif, periodically.
 *
 * Its only job is to make the driver TRANSMIT on bss_type=1 so something can
 * check that it did.  With the model's tx-loopback on, the frame comes back
 * tagged with the interface it was SENT on -- so a driver that used the plain
 * sendDataFrame() (bss 0) sees its own frame arrive on the station interface
 * and be refused, while a correct one sees it arrive on the uAP interface.
 * That round trip is the only thing a host can observe that distinguishes
 * "addressed the uAP" from "addressed the station and got lucky".
 *
 * ARP rather than UDP because it needs no peer, no address and no checksum:
 * a frame that fails to come back must have failed for the reason under test.
 */
static uint32_t s_txProbes = 0;

static void uapTxProbe(void) {
    static uint32_t last = 0;
    if (millis() - last < 500) return;
    last = millis();
    ip4_addr_t target;
    IP4_ADDR(&target, AP_ADDR0, AP_ADDR1, AP_ADDR2, 200);   /* nobody holds it */
    if (etharp_request(&s_uapNif, &target) == ERR_OK) s_txProbes++;
}
#endif

// [2026-08-21] This preamble ALSO lives in M2Radio arduino/WiFi.cpp now:
// WiFi.begin() runs it by default, so a sketch using the Arduino facade
// needs no copy of this.  This copy stays because this example drives the
// driver directly, below the facade.  Keep the two in step.
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
#if defined(HAVE_UAP_PSK)
    cfg.psk          = M2_UAP_PSK_STR;
    cfg.tlvMask      = Iw416::UAP_TLV_ALL_WPA2;
#else
    cfg.tlvMask      = Iw416::UAP_TLV_ALL_OPEN;
#endif
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
    Serial1.print(" chan=");            Serial1.print(M2_UAP_CHANNEL);
    // The SSID is broadcast so printing it costs nothing.  The passphrase is
    // never printed, not even its length beyond this yes/no -- a console log is
    // the easiest place in this whole chain for a secret to end up.
#if defined(HAVE_UAP_PSK)
    Serial1.println(" security=wpa2-psk");
#else
    Serial1.println(" security=open");
#endif

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

    s_dhcpPcb = udp_new();
    if (s_dhcpPcb && udp_bind(s_dhcpPcb, IP_ANY_TYPE, DHCP_SERVER_PORT) == ERR_OK) {
        // ★ BOUND TO THE uAP NETIF, not to every interface.  A DHCP server
        // listening on IP_ANY_TYPE answers DISCOVERs arriving on ANY netif --
        // so the moment this is paired with the station path (the datasheet
        // supports STA+uAP simultaneously, and that is a later phase), it would
        // start handing out addresses on somebody else's network.  Harmless
        // today because there is no STA netif here, which is exactly why it
        // would have shipped unnoticed.
        udp_bind_netif(s_dhcpPcb, &s_uapNif);
        udp_recv(s_dhcpPcb, onDhcp, nullptr);
        Serial1.print("uap_dhcp_up pool=");
        Serial1.print(AP_ADDR0); Serial1.print('.'); Serial1.print(AP_ADDR1);
        Serial1.print('.'); Serial1.print(AP_ADDR2); Serial1.print('.');
        Serial1.print(POOL_FIRST); Serial1.print("-");
        Serial1.println(POOL_FIRST + POOL_COUNT - 1);
    } else {
        Serial1.println("uap_dhcp_up=FAILED");
    }
    Serial1.println("uap_lwip_done");
}

// The card's OWN list of who is associated.  Polled rather than inferred from
// events: an event can be missed (a service pass that ran late, a full ring),
// and a membership count that is only ever adjusted by deltas drifts silently
// once one is lost.  The count is authoritative; the events say WHEN.
static void pollStaList() {
    static uint8_t rx[Iw416::SDIO_BLOCK_SIZE];
    uint16_t rxLen = 0;
    if (iw416.sendHostCmdBss(Iw416::CMD_APCMD_STA_LIST, nullptr, 0,
                             Iw416::BSS_TYPE_UAP, 0) != SdioHost::OK) return;
    if (iw416.waitCmdResp(Iw416::CMD_APCMD_STA_LIST, rx, sizeof(rx), &rxLen, 2000) != SdioHost::OK)
        return;
    if (rxLen < 14) return;
    const uint16_t n = (uint16_t)(rx[12] | ((uint16_t)rx[13] << 8));
    if (s_staCount != 0xFFFF && n != s_staCount) {
        if (n > s_staCount) s_joins++; else s_leaves++;
        Serial1.print("uap_membership from="); Serial1.print((int)s_staCount);
        Serial1.print(" to=");                 Serial1.print((int)n);
        Serial1.print(" lastevent=0x");        Serial1.print(iw416.lastEvent(), HEX);
        Serial1.print(" joins=");              Serial1.print(s_joins);
        Serial1.print(" leaves=");             Serial1.println(s_leaves);
    }
    s_staCount = n;
}

void loop() {
    static uint32_t last = 0;
    static uint32_t lastSta = 0;
    if (s_haveCard && s_bssUp) {
        (void)iw416NetifPollDual(nullptr, &s_uapNif);
        sys_check_timeouts();
    }
#if defined(ARP_LOSS_PROBE)
    if (s_haveCard && s_bssUp) arpLossProbe();
#endif
#if defined(UAP_TX_PROBE)
    if (s_haveCard && s_bssUp) uapTxProbe();
#endif
    if (s_haveCard && s_bssUp && millis() - lastSta >= 2000) {
        lastSta = millis();
        pollStaList();
    }
    if (millis() - last >= 2000) {
        last = millis();
        Serial1.print("hb card=");   Serial1.print(s_haveCard ? 1 : 0);
        Serial1.print(" bss=");      Serial1.print(s_bssUp ? 1 : 0);
        Serial1.print(" udp_rx=");   Serial1.print(s_udpRx);
        Serial1.print(" udp_bytes=");Serial1.print(s_udpBytes);
        Serial1.print(" rx_bss0=");  Serial1.print(iw416.rxFramesByBss(0));
        Serial1.print(" rx_bss1=");  Serial1.print(iw416.rxFramesByBss(1));
        Serial1.print(" unrouted="); Serial1.print(iw416NetifUnroutedFrames());
        Serial1.print(" dhcp_disc=");Serial1.print(s_dhcpDiscover);
        Serial1.print(" dhcp_req="); Serial1.print(s_dhcpRequest);
        Serial1.print(" dhcp_ack="); Serial1.print(s_dhcpAck);
        Serial1.print(" dhcp_full=");Serial1.print(s_dhcpNoPool);
        Serial1.print(" dhcp_bcast=");Serial1.print(s_dhcpBcastReplies);
        Serial1.print(" sta=");
        if (s_staCount == 0xFFFF) Serial1.print("?"); else Serial1.print((int)s_staCount);
        Serial1.print(" joins=");    Serial1.print(s_joins);
        Serial1.print(" leaves=");   Serial1.print(s_leaves);
        Serial1.print(" lastevent=0x"); Serial1.println(iw416.lastEvent(), HEX);
        // ★ THE HEALTH LINE, and every counter on it was earned.  These are the
        // ones that caught real faults in this driver's history, so a soak that
        // does not print them is a soak that proves the AP kept talking and
        // nothing more:
        //   stranded  -- W12 fault #5: uploads the host never collected because
        //                a clear-on-read interrupt bit was discarded.  Cost days.
        //   desync    -- a set ring bitmap bit with a zero length behind it.
        //   split     -- a packet whose declared size disagreed with its slot.
        //   dropped   -- RX frames the driver could not hand up.
        //   seqmm     -- command replies whose sequence did not match; W17 made
        //                this load-bearing, since the card echoes bss nibbles.
        //   pswake    -- power-save wakes, the W10 erratum's tell.
        // rx_bss0 belongs here too: on a uAP-only build ANY frame tagged for the
        // station interface is a mis-tag, and it must stay 0 for the whole soak.
        Serial1.print("health stranded=");  Serial1.print(iw416.rxStrandedRecovered());
        Serial1.print(" desync=");          Serial1.print(iw416.rxDesyncRecovered());
        Serial1.print(" split=");           Serial1.print(iw416.rxSplitMismatch());
        Serial1.print(" dropped=");         Serial1.print(iw416.rxDropped());
        Serial1.print(" seqmm=");           Serial1.print(iw416.seqMismatches());
        Serial1.print(" pswake=");          Serial1.print(iw416.psWakes());
        Serial1.print(" rx_bss0=");         Serial1.print(iw416.rxFramesByBss(0));
        Serial1.print(" unrouted=");        Serial1.print(iw416NetifUnroutedFrames());
        Serial1.print(" uptime_s=");        Serial1.print(millis() / 1000);
#if defined(UAP_TX_PROBE)
        Serial1.print(" tx_probes=");       Serial1.print(s_txProbes);
#endif
        Serial1.println();
    }
}
