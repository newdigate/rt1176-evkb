/* lvgl_smoke_test - LVGL 9.4 comes up on the RT1176: lv_init() succeeds, the
 * millis() tick callback is actually driving LVGL's clock at the right RATE,
 * the FNV-1a checksum oracle that Tasks 3/4 gate on is correct, and
 * lv_timer_handler() survives being pumped with no display registered.
 * No panel involved.
 * Copyright (c) 2026 Nicholas Newdigate
 * SPDX-License-Identifier: MIT */
#include <Arduino.h>
#include "lvgl_rt1176.h"

void setup()
{
    Serial1.begin(115200);
    delay(200);
    Serial1.println("LVGL_SMOKE_BEGIN");

    Serial1.printf("LVGL_VERSION=%d.%d.%d\n",
                   LVGL_VERSION_MAJOR, LVGL_VERSION_MINOR, LVGL_VERSION_PATCH);

    lvgl_rt1176_begin();
    Serial1.printf("LVGL_INIT=%s\n", lv_is_initialized() ? "PASS" : "FAIL");

    /* lv_tick_get() must advance because of our callback, not by itself, AND at
     * roughly 1 kHz. A bare t1 > t0 would also pass with the callback wired to
     * micros(), which would silently divide every LVGL timeout by 1000. Band is
     * deliberately loose (10..500 across a 50 ms delay) because QEMU timing is. */
    uint32_t t0 = lv_tick_get();
    delay(50);
    uint32_t t1 = lv_tick_get();
    uint32_t dt = t1 - t0;
    Serial1.printf("LVGL_TICK=%s (t0=%lu t1=%lu dt=%lu)\n",
                   (t1 > t0 && dt >= 10 && dt <= 500) ? "PASS" : "FAIL",
                   (unsigned long)t0, (unsigned long)t1, (unsigned long)dt);

    /* The checksum accumulator is the ONLY oracle Tasks 3/4 have (QEMU models
     * no ILI9341), so it must be proven correct HERE, while a wrong answer is
     * still visible. Once a binding feeds it real pixels, any defect would just
     * become the golden value and the gate would be green and meaningless
     * forever. Vector: FNV-1a-32 of the single byte "a" == 0xE40C292C
     * (published test vector; independently recomputed for this port). */
    lvgl_sum_reset();
    lvgl_sum_feed("a", 1);
    uint32_t sum = lvgl_sum_value();
    Serial1.printf("LVGL_SUM_SELFTEST=%s (got=0x%08lX want=0xE40C292C bytes=%lu)\n",
                   (sum == 0xE40C292Cu && lvgl_sum_bytes() == 1) ? "PASS" : "FAIL",
                   (unsigned long)sum, (unsigned long)lvgl_sum_bytes());

    Serial1.println("LVGL_SMOKE_DONE");
}

void loop()
{
    /* Everything above is setup() output; without this nothing would prove the
     * loop path runs at all. Note lv_timer_handler() is being pumped with NO
     * display registered — an unusual state, and proving it is survivable
     * rather than a fault or a hang is part of the point.
     * Capped at three reports: the UART capture is a size-capped file and a
     * free-running loop() would otherwise emit these forever. */
    static uint32_t n = 0;
    lvgl_rt1176_loop();
    if (++n % 100 == 0 && n <= 300) Serial1.printf("LVGL_LOOP=%lu\n", (unsigned long)n);
}
