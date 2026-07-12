#include "Arduino.h"
#include "HardwareSerial.h"
#include "Ethernet.h"

static uint8_t mac[6] = {0x02,0x00,0x00,0x00,0x00,0x01};
EthernetServer server(7);
EthernetUDP    udp;
static bool did_client = false, did_dns = false;

void setup() {
    Serial1.begin(115200); delay(50);
    Serial1.println("ETH_BOOT");
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
    IPAddress r;
    /* connect() with a hostname exercises DNS; here we resolve via EthernetUDP::beginPacket(host) path.
       Task 6 wires real resolution; until then this prints DNS_FAIL harmlessly. */
    EthernetClient c;
    int ok = c.connect("example.com", 9);      /* discard port; DNS is the point */
    r = Ethernet.localIP();                     /* placeholder until Task 6 exposes resolved ip */
    Serial1.print("DNS_TRY ok="); Serial1.println(ok);
    if (ok) c.stop();
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
