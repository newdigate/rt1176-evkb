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
}
void loop() {
    static uint32_t t0 = millis(); static bool sent = false;
    if (!sent && (millis() - t0) > 500) {
        int r = enet_send_frame(PROBE_TX, sizeof(PROBE_TX));
        Serial1.print("ENET_TX="); Serial1.println(r == 0 ? "PASS" : "FAIL"); sent = true;
    }
    uint8_t buf[1522]; uint16_t len = 0;
    if (enet_read_frame(buf, &len) == 1) {
        bool ok = (len >= 27) && buf[12]==0x88 && buf[13]==0xB5 && memcmp(&buf[14],"ENET-RX-PROBE",13)==0;
        Serial1.print("ENET_RX="); Serial1.println(ok ? "PASS" : "FAIL");
    }
}
