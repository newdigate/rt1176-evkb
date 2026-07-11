#include "Arduino.h"
#include "HardwareSerial.h"
#include <string.h>
#include "enet.h"
static const uint8_t ENET_MAC[6] = {0x02,0x00,0x00,0x00,0x00,0x01};
static const uint8_t PROBE_TX[] = {
    0x02,0x00,0x00,0x00,0x00,0x02, 0x02,0x00,0x00,0x00,0x00,0x01, 0x88,0xB5,
    'E','N','E','T','-','T','X','-','P','R','O','B','E' };

static const uint8_t ENET_IP[4] = {192,168,100,50};

static uint16_t inet_cksum(const uint8_t *p, int n) {
    uint32_t s = 0; int i;
    for (i = 0; i + 1 < n; i += 2) s += (uint32_t)((p[i] << 8) | p[i+1]);
    if (n & 1) s += (uint32_t)(p[n-1] << 8);
    while (s >> 16) s = (s & 0xFFFF) + (s >> 16);
    return (uint16_t)~s;
}
/* ARP request for our IP -> reply. Ethernet[0:6]=dst,[6:12]=src,[12:14]=ethertype.
   ARP payload @14: htype[14:16] ptype[16:18] hlen[18] plen[19] oper[20:22]
   sndMAC[22:28] sndIP[28:32] tgtMAC[32:38] tgtIP[38:42]. */
static void handle_arp(uint8_t *f, uint16_t len) {
    if (len < 42) return;
    if (!(f[20] == 0x00 && f[21] == 0x01)) return;           /* oper == request */
    if (memcmp(&f[38], ENET_IP, 4) != 0) return;             /* target IP == us */
    uint8_t r[60]; memset(r, 0, sizeof(r));
    memcpy(&r[0], &f[6], 6);                                 /* dst = requester's MAC */
    memcpy(&r[6], ENET_MAC, 6);                              /* src = us */
    r[12] = 0x08; r[13] = 0x06;
    memcpy(&r[14], &f[14], 6);                               /* htype/ptype/hlen/plen */
    r[20] = 0x00; r[21] = 0x02;                              /* oper = reply */
    memcpy(&r[22], ENET_MAC, 6); memcpy(&r[28], ENET_IP, 4); /* sender = us */
    memcpy(&r[32], &f[22], 6);   memcpy(&r[38], &f[28], 4);  /* target = requester */
    enet_send_frame(r, 60);
    Serial1.println("ENET_ARP=PASS");
}
/* ICMP echo request to our IP -> echo reply. IP header @14; proto @23; dstIP @30;
   ICMP @ 14+ihl; ICMP type @+0, code @+1, cksum @+2. */
static void handle_ipv4(uint8_t *f, uint16_t len) {
    if (len < 34) return;
    uint16_t ihl = (uint16_t)((f[14] & 0x0F) * 4);
    if (f[23] != 1) return;                                  /* proto == ICMP */
    if (memcmp(&f[30], ENET_IP, 4) != 0) return;             /* dst IP == us */
    uint16_t icmp = (uint16_t)(14 + ihl);
    if (icmp + 8 > len) return;
    if (f[icmp] != 8) return;                                /* type 8 = echo request */
    /* enet_read_frame can hand back `len` padded to the 60-byte Ethernet
       minimum (QEMU/HW pad short RX frames, and the pad bytes are leftover
       buffer content, not zero) -- so `len` can run past the real datagram.
       Use the IP header's own Total Length to find the true ICMP boundary
       instead of trusting `len`, or the checksum ends up computed over
       trailing pad garbage. */
    uint16_t ip_total = (uint16_t)((f[16] << 8) | f[17]);
    uint16_t icmp_len = (uint16_t)(len - icmp);
    if (ip_total >= ihl && (uint16_t)(14 + ip_total) <= len)
        icmp_len = (uint16_t)(ip_total - ihl);
    uint8_t tmp[6];
    memcpy(tmp, &f[0], 6); memcpy(&f[0], &f[6], 6); memcpy(&f[6], tmp, 6);   /* swap MAC */
    { int i; for (i = 0; i < 4; i++) { uint8_t t = f[26+i]; f[26+i] = f[30+i]; f[30+i] = t; } } /* swap IP */
    f[icmp] = 0;                                             /* echo reply */
    f[16] = 0; f[17] = 0;                                    /* IP header cksum field */
    { uint16_t c = inet_cksum(&f[14], ihl); f[16] = (uint8_t)(c >> 8); f[17] = (uint8_t)c; }
    f[icmp+2] = 0; f[icmp+3] = 0;                            /* ICMP cksum field */
    { uint16_t c = inet_cksum(&f[icmp], icmp_len); f[icmp+2] = (uint8_t)(c >> 8); f[icmp+3] = (uint8_t)c; }
    enet_send_frame(f, (uint16_t)(icmp + icmp_len));
    Serial1.println("ENET_PING=PASS");
}
static void enet_poll(void) {
    uint8_t buf[1536]; uint16_t len = 0;
    while (enet_read_frame(buf, &len) == 1) {
        if (len < 14) continue;
        uint16_t et = (uint16_t)((buf[12] << 8) | buf[13]);
        if (et == 0x0806) handle_arp(buf, len);
        else if (et == 0x0800) handle_ipv4(buf, len);
        else if (et == 0x88B5) {   /* Gate-1 mac RX probe */
            if (len >= 27 && memcmp(&buf[14], "ENET-RX-PROBE", 13) == 0)
                Serial1.println("ENET_RX=PASS");
        }
    }
}

void setup() {
    Serial1.begin(115200); delay(50);
    Serial1.println("ENET_BOOT");
    enet_init(ENET_MAC);
    Serial1.println("ENET_INIT_DONE");
    uint16_t id1 = enet_mdio_read(3, 2), id2 = enet_mdio_read(3, 3);
    Serial1.print("ENET_PHYID="); Serial1.print(id1, HEX); Serial1.print(":"); Serial1.println(id2, HEX);
    int link = enet_phy_link_up(3000);
    Serial1.print("ENET_LINK="); Serial1.println(link ? "PASS" : "FAIL");
    bool idok = (id1 != 0xFFFF && id1 != 0x0000);   /* real MDIO round-trip, not 0xffff */
    Serial1.print("ENET_PHYID_OK="); Serial1.println(idok ? "PASS" : "FAIL");
}
void loop() {
    static uint32_t t0 = millis(); static bool sent = false;
    if (!sent && (millis() - t0) > 500) {
        int r = enet_send_frame(PROBE_TX, sizeof(PROBE_TX));
        Serial1.print("ENET_TX="); Serial1.println(r == 0 ? "PASS" : "FAIL"); sent = true;
    }
    enet_poll();
}
