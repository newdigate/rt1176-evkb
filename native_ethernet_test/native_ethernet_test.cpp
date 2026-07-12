#include "Arduino.h"
#include "HardwareSerial.h"
#include "NativeEthernet.h"
#include "NativeDns.h"
#include <string.h>

static uint8_t mac[6] = {0x02,0x00,0x00,0x00,0x00,0x01};
EthernetServer server(7);
EthernetUDP    udp;
static bool did_client = false, did_dns = false;

void setup() {
    Serial1.begin(115200); delay(50);
    Serial1.println("ETH_BOOT");
    int ok = Ethernet.begin(mac, 15000);      /* DHCP; FNET pumped by IntervalTimer */
    IPAddress ip = Ethernet.localIP();
    Serial1.print("ETH_DHCP ok="); Serial1.print(ok);
    Serial1.print(" ip="); Serial1.println(ip);
    server.begin();
    udp.begin(7);
    Serial1.println("ETH_NETIF_UP");
}

static bool on_slirp() {                      /* QEMU SLIRP lease = 10.0.2.15 */
    IPAddress ip = Ethernet.localIP();
    return ip[0] == 10 && ip[1] == 0 && ip[2] == 2;
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

static void try_client_once() {
    did_client = true;
    EthernetClient c;
    if (on_slirp()) {
        /* This probe targets 10.0.2.100, which is only guestfwd-routed in the
           "client" phase; in every other phase the connect can't succeed and
           EthernetClient::connect() blocks loop() for its full timeout
           (NativeEthernet default 10000 ms) before giving up. loop() also
           drives serve_tcp()/serve_udp(), so a 10 s stall here starves the
           echo server for the whole window the peer expects a reply in.
           Bound it to the same budget the read-loop below already assumes
           (3000 ms) so a one-shot miss here can't monopolize loop(). */
        c.setConnectionTimeout(3000);
        if (c.connect(IPAddress(10,0,2,100), 7)) {
            const char *tok = "ETHCLI-PROBE\n"; c.write((const uint8_t*)tok, 13);
            uint32_t t0 = millis(); char in[16]; int got = 0;
            while (got < 13 && millis()-t0 < 3000) { while (c.available() && got < 13) in[got++] = c.read(); }
            Serial1.print("CLIENT_ECHO="); Serial1.println((got==13 && memcmp(in,tok,13)==0) ? "PASS" : "FAIL");
            c.stop();
        } else Serial1.println("CLIENT_ECHO=FAIL");
    } else {
        /* Real network: outbound HTTP GET via router NAT (Mac firewall blocks
           inbound echo peers — same approach as milestone 3 HW verification). */
        if (c.connect("example.com", 80)) {
            c.print("GET / HTTP/1.1\r\nHost: example.com\r\nConnection: close\r\n\r\n");
            uint32_t t0 = millis(); char hdr[16]; int got = 0;
            while (got < 15 && millis()-t0 < 8000) { while (c.available() && got < 15) hdr[got++] = c.read(); }
            hdr[got] = 0;
            Serial1.print("HTTP_GET="); Serial1.println(strstr(hdr, "HTTP/1.1 200") ? "PASS" : "FAIL");
            c.stop();
        } else Serial1.println("HTTP_GET=FAIL");
    }
}

static void try_dns_once() {
    did_dns = true;
    DNSClient dns;
    IPAddress d = Ethernet.dnsServerIP();
    if (d == IPAddress(0,0,0,0)) d = IPAddress(10,0,2,3);   /* SLIRP DNS fallback */
    dns.begin(d);
    IPAddress rip;
    if (dns.getHostByName("example.com", rip, 6000) == 1) {
        Serial1.print("DNS_OK ip="); Serial1.println(rip);
    } else Serial1.println("DNS_FAIL");
}

void loop() {
    serve_tcp();
    serve_udp();
    if (Ethernet.localIP() != IPAddress(0,0,0,0)) {
        if (!did_client) try_client_once();
        if (!did_dns)    try_dns_once();
    }
}
