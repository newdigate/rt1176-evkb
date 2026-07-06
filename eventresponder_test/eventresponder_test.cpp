#include "Arduino.h"
#include "HardwareSerial.h"
#include "EventResponder.h"

static EventResponder er;
static volatile int fired = 0;
static volatile int seen_status = -1;
static volatile void *seen_data = nullptr;

static void cb(EventResponderRef e) {
    fired++;
    seen_status = e.getStatus();
    seen_data = e.getData();
}

void setup() {
    Serial1.begin(115200);
    while (!Serial1) {}
    bool ok = true;

    // STAGE_IMMEDIATE: attachImmediate -> callback runs synchronously in triggerEvent()
    fired = 0;
    er.attachImmediate(cb);
    er.triggerEvent();
    bool s_imm = (fired == 1);                 // fired before any yield()
    Serial1.println(s_imm ? "STAGE_IMMEDIATE=PASS" : "STAGE_IMMEDIATE=FAIL");
    if (!s_imm) ok = false;
    er.detach();

    // STAGE_YIELD: attach -> deferred; not fired until yield()
    fired = 0;
    er.attach(cb);
    er.triggerEvent();
    bool before = (fired == 0);                // not yet
    yield();
    bool after = (fired == 1);                 // now
    bool s_yield = before && after;
    Serial1.println(s_yield ? "STAGE_YIELD=PASS" : "STAGE_YIELD=FAIL");
    if (!s_yield) ok = false;
    er.detach();

    // STAGE_CLEAR: attach + trigger + clearEvent + yield -> callback does NOT run
    fired = 0;
    er.attach(cb);
    er.triggerEvent();
    er.clearEvent();
    yield();
    bool s_clear = (fired == 0);
    Serial1.println(s_clear ? "STAGE_CLEAR=PASS" : "STAGE_CLEAR=FAIL");
    if (!s_clear) ok = false;
    er.detach();

    // STAGE_STATUS: triggerEvent(42, &marker) -> callback sees status + data
    static int marker = 7;
    fired = 0; seen_status = -1; seen_data = nullptr;
    er.attach(cb);
    er.triggerEvent(42, &marker);
    yield();
    bool s_status = (fired == 1) && (seen_status == 42) && (seen_data == &marker);
    Serial1.println(s_status ? "STAGE_STATUS=PASS" : "STAGE_STATUS=FAIL");
    if (!s_status) ok = false;
    er.detach();

    Serial1.println(ok ? "EVENTRESPONDER_ALL=PASS" : "EVENTRESPONDER_ALL=FAIL");
}
void loop() {}
