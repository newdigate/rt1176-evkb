#include "Arduino.h"
#include "HardwareSerial.h"
#include <string.h>
#include "enet.h"
static const uint8_t ENET_MAC[6] = {0x02,0x00,0x00,0x00,0x00,0x01};
static const uint8_t PROBE_TX[] = {
    0x02,0x00,0x00,0x00,0x00,0x02, 0x02,0x00,0x00,0x00,0x00,0x01, 0x88,0xB5,
    'E','N','E','T','-','T','X','-','P','R','O','B','E' };
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
    uint8_t buf[1536]; uint16_t len = 0;   // >= ENET_BUF_SZ (see enet.h)
    if (enet_read_frame(buf, &len) == 1) {
        bool ok = (len >= 27) && buf[12]==0x88 && buf[13]==0xB5 && memcmp(&buf[14],"ENET-RX-PROBE",13)==0;
        Serial1.print("ENET_RX="); Serial1.println(ok ? "PASS" : "FAIL");
    }
}
