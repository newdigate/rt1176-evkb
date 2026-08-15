#include <Arduino.h>
#include "Display.h"
#include "lvgl_rt1176.h"
#include "lvgl_mipi_panel.h"
#include "synthui_knob.h"

void setup()
{
    Serial1.begin(115200);
    delay(200);
    Serial1.println("SYNTHUI_KNOB_BEGIN");
    const bool ok = Display.begin();
    Serial1.println(ok ? "PANEL_OK" : "PANEL_FAIL");
    if (!ok) { Serial1.println("KNOB_TEST_DONE"); return; }
    Display.fillScreen(0x0000);
    lvgl_rt1176_begin();
    lvgl_mipi_panel_create(Display);
    lv_obj_t *k = synthui_knob_create(lv_screen_active());
    lv_obj_center(k);
    uint32_t t0 = millis();
    while (!lvgl_mipi_panel_frame_done() && (millis() - t0) < 5000) lvgl_rt1176_loop();
    Serial1.printf("LVGL_FLUSHED=%s\n", lvgl_mipi_panel_frame_done() ? "PASS" : "FAIL");
    Serial1.println("KNOB_TEST_DONE");
}
void loop() { lvgl_rt1176_loop(); }
