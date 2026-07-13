#include "Arduino.h"
#include "HardwareSerial.h"
#include "Ethernet.h"
#include "utility/socket_lwip.h"

static uint8_t mac[6] = {0x02,0x00,0x00,0x00,0x00,0x01};
EthernetServer server(7);
EthernetUDP    udp;
static bool did_client = false, did_dns = false;

/* IPAddress unit checks (license clean-room oracle): storage is
 * network-order-in-memory; operator uint32_t returns that memory as-is. */
class CapturePrint : public Print {
public:
    char buf[64]; size_t n = 0;
    virtual size_t write(uint8_t c) { if (n < sizeof(buf)-1) buf[n++] = c; buf[n] = 0; return 1; }
};

static bool ipaddr_checks() {
    IPAddress a(192, 168, 1, 101);
    if (a[0] != 192 || a[1] != 168 || a[2] != 1 || a[3] != 101) return false;
    uint32_t v = a;
    const uint8_t *vb = (const uint8_t *)&v;
    if (vb[0] != 192 || vb[1] != 168 || vb[2] != 1 || vb[3] != 101) return false;
    IPAddress b(v);
    if (!(b == a)) return false;
    const uint8_t raw[4] = {192, 168, 1, 101};
    if (!(a == raw)) return false;
    IPAddress c;
    if (!(c == IPAddress(0, 0, 0, 0))) return false;   /* default = 0.0.0.0 */
    if (!c.fromString("10.0.2.15")) return false;
    if (c[0] != 10 || c[1] != 0 || c[2] != 2 || c[3] != 15) return false;
    if (c.fromString("999.1.2.3")) return false;       /* octet out of range */
    if (c.fromString("1.2.3")) return false;           /* too few octets */
    if (c.fromString("banana")) return false;
    c = raw;                                           /* operator=(const uint8_t*) */
    if (c[3] != 101) return false;
    c[3] = 7;                                          /* operator[] write */
    if (c[3] != 7) return false;
    CapturePrint cp;
    a.printTo(cp);
    if (strcmp(cp.buf, "192.168.1.101") != 0) return false;
    return true;
}

void setup() {
    Serial1.begin(115200); delay(50);
    Serial1.println("ETH_BOOT");
    Serial1.println(ipaddr_checks() ? "IPADDR=OK" : "IPADDR=FAIL");
    int ok = Ethernet.begin(mac, 15000);          /* DHCP */
    IPAddress ip = Ethernet.localIP();
    Serial1.print("ETH_DHCP ok="); Serial1.print(ok);
    Serial1.print(" ip="); Serial1.println(ip);
    server.begin();
    udp.begin(7);
    Serial1.println("ETH_NETIF_UP");
}

static void serve_tcp() {
    EthernetClient c = server.available();
    if (c) { while (c.available()) { uint8_t b = c.read(); c.write(b); } }
}
static void serve_udp() {
    int n = udp.parsePacket();
    if (n > 0) {
        static uint8_t buf[600]; int m = udp.read(buf, sizeof(buf));
        udp.beginPacket(udp.remoteIP(), udp.remotePort());
        udp.write(buf, m); udp.endPacket();
    }
}
static void try_client_once() {              /* fires ~immediately after DHCP, before the 6s peer */
    did_client = true;
    EthernetClient c;
    if (c.connect(IPAddress(10,0,2,100), 7)) {
        const char *tok = "ETHCLI-PROBE\n"; c.write((const uint8_t*)tok, 13);
        uint32_t t0 = millis(); char in[16]; int got = 0;
        while (got < 13 && millis()-t0 < 3000) { Ethernet.loop(); while (c.available() && got<13) in[got++]=c.read(); }
        Serial1.print("CLIENT_ECHO="); Serial1.println((got==13 && memcmp(in,tok,13)==0) ? "PASS":"FAIL");
        c.stop();
    } else Serial1.println("CLIENT_ECHO=FAIL");
}
static void try_dns_once() {
    did_dns = true;
    ip_addr_t a;
    if (eth_resolve("example.com", &a, 6000)) {
        uint32_t v = ip_2_ip4(&a)->addr;
        Serial1.print("DNS_OK ip=");
        Serial1.print(v&0xff); Serial1.print('.'); Serial1.print((v>>8)&0xff); Serial1.print('.');
        Serial1.print((v>>16)&0xff); Serial1.print('.'); Serial1.println((v>>24)&0xff);
    } else Serial1.println("DNS_FAIL");
}

void loop() {
    Ethernet.loop();
    serve_tcp();
    serve_udp();
    if (Ethernet.localIP() != IPAddress(0,0,0,0)) {
        if (!did_client) try_client_once();
        if (!did_dns) try_dns_once();
    }
}
