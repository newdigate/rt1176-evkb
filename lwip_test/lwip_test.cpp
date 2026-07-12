#include "Arduino.h"
#include "HardwareSerial.h"
#include "lwip/init.h"
#include "lwip/netif.h"
#include "lwip/timeouts.h"
#include "lwip/dhcp.h"
#include "netif/ethernet.h"
#include "ethernetif.h"

extern "C" { uint8_t g_mac[6] = {0x02,0x00,0x00,0x00,0x00,0x01}; }   /* C linkage: ethernetif.c extern-refs this */
static struct netif s_netif;

void setup() {
    Serial1.begin(115200); delay(50);
    Serial1.println("LWIP_BOOT");
    lwip_init();
    netif_add(&s_netif, IP4_ADDR_ANY4, IP4_ADDR_ANY4, IP4_ADDR_ANY4, NULL,
              ethernetif_init, ethernet_input);   /* address via DHCP */
    netif_set_default(&s_netif);
    netif_set_up(&s_netif);
    dhcp_start(&s_netif);
    Serial1.println("LWIP_NETIF_UP");
}
void loop() {
    ethernetif_poll(&s_netif);
    sys_check_timeouts();
    static bool leased = false, fell_back = false; static uint32_t t0 = millis();
    if (!leased && dhcp_supplied_address(&s_netif)) {
        leased = true;
        Serial1.print("DHCP_OK ip="); Serial1.println(ip4addr_ntoa(netif_ip4_addr(&s_netif)));
    }
    if (!leased && !fell_back && (millis() - t0) > 5000) {   /* no DHCP server (ping phase) -> static */
        fell_back = true; dhcp_stop(&s_netif);
        ip4_addr_t ip, mask, gw;
        IP4_ADDR(&ip, 192,168,1,50); IP4_ADDR(&mask, 255,255,255,0); IP4_ADDR(&gw, 192,168,1,1);
        netif_set_addr(&s_netif, &ip, &mask, &gw);
        Serial1.println("LWIP_STATIC_FALLBACK ip=192.168.1.50");
    }
}
