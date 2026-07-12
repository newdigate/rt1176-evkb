#include "Arduino.h"
#include "HardwareSerial.h"
#include "lwip/init.h"
#include "lwip/netif.h"
#include "lwip/timeouts.h"
#include "netif/ethernet.h"
#include "ethernetif.h"

extern "C" { uint8_t g_mac[6] = {0x02,0x00,0x00,0x00,0x00,0x01}; }   /* C linkage: ethernetif.c extern-refs this */
static struct netif s_netif;

void setup() {
    Serial1.begin(115200); delay(50);
    Serial1.println("LWIP_BOOT");
    lwip_init();
    ip4_addr_t ip, mask, gw;
    IP4_ADDR(&ip, 192,168,1,50); IP4_ADDR(&mask, 255,255,255,0); IP4_ADDR(&gw, 192,168,1,1);
    netif_add(&s_netif, &ip, &mask, &gw, NULL, ethernetif_init, ethernet_input);
    netif_set_default(&s_netif);
    netif_set_up(&s_netif);
    Serial1.println("LWIP_NETIF_UP");
}
void loop() {
    ethernetif_poll(&s_netif);
    sys_check_timeouts();
}
