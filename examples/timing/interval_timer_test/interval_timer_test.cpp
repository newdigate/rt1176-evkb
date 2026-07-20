#include "Arduino.h"
#include "HardwareSerial.h"
#include "IntervalTimer.h"
#include "imxrt1176.h"     // CCM_CLOCK_ROOT2_CONTROL, CCM_CLOCK_ROOT_CONTROL_MUX/DIV

volatile uint32_t g_count = 0;
static void onTick() { g_count++; }

void setup() {
    Serial1.begin(115200);
    while (!Serial1) {}
    bool ok = true;

    // Check 1: fires (~100 ticks over 100 ms at 1000 us)
    IntervalTimer t1;
    bool b1 = t1.begin(onTick, 1000);
    g_count = 0; delay(100);
    uint32_t c1 = g_count;
    if (!b1 || c1 < 90 || c1 > 110) ok = false;

    // Check 4: frequency ratio (500 us ~ 2x)
    t1.update(500);
    g_count = 0; delay(100);
    uint32_t c2 = g_count;                     // expect ~200
    if (c2 < 180 || c2 > 220) ok = false;

    // Check 3: end() stops
    t1.end();
    g_count = 0; delay(50);
    uint32_t c3 = g_count;                     // expect 0
    if (c3 != 0) ok = false;

    // Check 2: channel exhaustion (4 succeed, 5th fails)
    IntervalTimer a, b, c, d, e;
    bool ba = a.begin(onTick, 1000), bb = b.begin(onTick, 1000);
    bool bc = c.begin(onTick, 1000), bd = d.begin(onTick, 1000);
    bool be = e.begin(onTick, 1000);           // no channel left
    if (!(ba && bb && bc && bd) || be) ok = false;
    a.end(); b.end(); c.end(); d.end();

    // Check 5: clock faithfulness — period preserved across a BUS-clock change.
    IntervalTimer t5;
    t5.begin(onTick, 1000);
    g_count = 0; delay(100);
    uint32_t cA = g_count; t5.end();           // ~100 at 24 MHz
    // Re-route BUS root (CLOCK_ROOT2) to DIV=1 (/2 -> 12 MHz), mux 0.
    CCM_CLOCK_ROOT2_CONTROL = CCM_CLOCK_ROOT_CONTROL_MUX(0) | CCM_CLOCK_ROOT_CONTROL_DIV(1);
    t5.begin(onTick, 1000);                    // re-arm: core reads 12 MHz; PIT should too
    g_count = 0; delay(100);
    uint32_t cB = g_count; t5.end();           // faithful: ~100; broken (fixed 24 MHz): ~200
    CCM_CLOCK_ROOT2_CONTROL = CCM_CLOCK_ROOT_CONTROL_MUX(0) | CCM_CLOCK_ROOT_CONTROL_DIV(0);
    if (cB < 80 || cB > 120) ok = false;       // period preserved

    Serial1.print("c1="); Serial1.print(c1);
    Serial1.print(" c2="); Serial1.print(c2);
    Serial1.print(" c3="); Serial1.print(c3);
    Serial1.print(" exhaust="); Serial1.print(be ? 1 : 0);
    Serial1.print(" cA="); Serial1.print(cA);
    Serial1.print(" cB="); Serial1.println(cB);
    Serial1.println(ok ? "IT=PASS" : "IT=FAIL");
}
void loop() {}
