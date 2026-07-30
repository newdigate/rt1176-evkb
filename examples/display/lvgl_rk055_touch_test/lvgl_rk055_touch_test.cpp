/* lvgl_rk055_touch_test - a finger drives LVGL widgets on the RK055 (v3).
 * Copyright (c) 2026 Nicholas Newdigate
 * SPDX-License-Identifier: MIT
 *
 * v2 (rk055_touch_test) proved COORDINATES.  This gate proves LVGL REACTED --
 * a different claim, and one a render checksum cannot make: a checksum cannot
 * tell "the widget moved because of a touch" from "the scene changed".  So
 * every widget here exists to make one specific wrong binding fail:
 *
 *   BTN1..5 (checkable, under the five scripted tap points)  all must end
 *       CHECKED.  A stuck, mirrored, swapped or unscaled coordinate checks at
 *       most one.  This is real LVGL hit-testing and click delivery.
 *   THE DRAG HANDLE  must end at the band's right (>= DRAG_END_MIN) having
 *       moved >= DRAG_MIN_MOVES times.  THE STAR WITNESS for the Idle latch:
 *       at the binding's 10 ms read period, Idle polls occur MID-DRAG by
 *       construction; forward one as "released" and the drag shatters into
 *       taps -- the handle never leaves the left edge and this gate goes red.
 *   HOLD (checkable, at the phase-3 primary)  must end CHECKED: the
 *       two-contact press reached LVGL and released cleanly.
 *   TRAP (checkable, at the phase-3 secondary)  must end UNCHECKED: the
 *       pointer never re-adopted the surviving finger.  VACUOUS ALONE -- a
 *       dead touch path also leaves it unchecked -- so it is only asserted
 *       as a PAIR with HOLD=CHECKED, which proves the phase-3 press arrived.
 *
 * The scene checksum here is DEMOTED: taken before the indev exists (safe:
 * the QEMU model stalls its script until the first buffer is acknowledged,
 * and nobody polls until the indev is created), it asserts "the scene built
 * correctly" and NOTHING about touch.  No post-touch checksum is asserted.
 *
 * TEARING is expected and accepted (spec 8.1): direct render, no back buffer,
 * no vsync fence -- a dragged widget may show a sliced edge for a frame on
 * real glass.  That is v1's documented trade, fixed by v4's double-buffer
 * milestone, NOT a bug in this example.  Do not scope a "flicker bug" here.
 *
 * QEMU vs HARDWARE: in QEMU the virtual GT911 replays a model-owned script
 * (5 taps, a 10-sample drag, two-contact holds, then phase 3b: the primary
 * lifts while id 1 remains).  The model places contacts at the same
 * percentages this file places widgets, so a green QEMU run proves the
 * binding and LVGL's delivery, NEVER the orientation -- only the hardware
 * run's finger settles that (same asymmetry as v2, stated not papered over).
 *
 * Uses Serial1 (LPUART; QEMU captures it), like every sibling gate.
 */
#include <Arduino.h>
#include <Wire.h>
#include <Display.h>
#include "gt911.h"
#include "lvgl_rt1176.h"
#include "lvgl_mipi_panel.h"
#include "lvgl_gt911_indev.h"

// D9 = GPIO_AD_01 = touch reset; D6 = GPIO_AD_00 = touch interrupt (owned by
// begin(); v3 never attaches an interrupt to it -- the two v2 INT findings
// stay open and stay out of this gate's failure domain).
static constexpr uint8_t TOUCH_RST_PIN = 9;
static constexpr uint8_t TOUCH_INT_PIN = 6;
static GT911 touch(Wire2, TOUCH_RST_PIN, TOUCH_INT_PIN);

// --- Geometry (spec 6.2) -----------------------------------------------------
// Everything is placed against the QEMU script's percentages of 720x1280:
// taps at (15,10) (85,10) (15,90) (85,90) (50,50) percent; the drag along
// y=50% from x=10% to 90%; phase-3 contacts at x=25% and 75%.
struct Rect { int16_t x, y, w, h; };
static constexpr Rect BTN_RECT[5] = {
    {   8,   48, 200, 160 },   // centre (108,128)  = tap 1
    { 512,   48, 200, 160 },   // centre (612,128)  = tap 2
    {   8, 1072, 200, 160 },   // centre (108,1152) = tap 3
    { 512, 1072, 200, 160 },   // centre (612,1152) = tap 4
    { 290,  570, 140, 140 },   // centre (360,640)  = tap 5
};
static constexpr Rect HANDLE_RECT = {  22, 570, 100, 140 };  // drag start x=72 inside
static constexpr Rect HOLD_RECT   = { 125, 570, 120, 140 };  // centre (185,640); contact (180,640)
static constexpr Rect TRAP_RECT   = { 485, 570, 110, 140 };  // centre (540,640); contact (540,640)

// The handle follows the pointer x; these two are what the gate asserts.
// QEMU: final drag sample at 90% = x=648, handle x = 648-50 = 598; 9 of the
// 10 samples change x (the first lands where the handle already is).
static constexpr int16_t DRAG_END_MIN   = 560;   // handle x >= this = "arrived"
static constexpr int32_t DRAG_MIN_MOVES = 8;

// COMPILE-TIME PROOF the band widgets are disjoint and the drag cannot brush
// TRAP.  Same discipline as v2's target-overlap assert: these are properties
// of the geometry that an edit destroys silently while every gate stays green
// (the model taps dead-centre wherever the rects now are).
static constexpr bool rectsDisjointX(const Rect &a, const Rect &b) {
    return (a.x + a.w <= b.x) || (b.x + b.w <= a.x);
}
static_assert(rectsDisjointX(HANDLE_RECT, HOLD_RECT) &&
              rectsDisjointX(HOLD_RECT, BTN_RECT[4]) &&
              rectsDisjointX(BTN_RECT[4], TRAP_RECT),
              "band widgets overlap: a tap or drag sample could hit two at once");
// The handle's final position (pointer 648 -> x 598) must clear TRAP's right
// edge, or the drag itself would end resting on the trap it must not touch.
static_assert(TRAP_RECT.x + TRAP_RECT.w <= 598,
              "the drag's end position overlaps TRAP");

// Phase patience: sized for a bench operator reading prompts (v2's rationale);
// QEMU's script finishes each phase in well under a second.
static constexpr uint32_t PHASE_TIMEOUT_MS = 60000;
// The FIRST watch gets 10 minutes, because its window arms at RESET, not when
// the operator is ready.  On a remotely-driven bench (agent flashes, human
// walks over) the operator arrives on human latency; 60 s expired before the
// first tap TWICE, each time with the identical no-contact signature
// (IDLE_POLLS=6047, BUFFERS=3) while the operator's later taps played to
// watches that had already exited -- the widgets still react (loop() keeps
// LVGL live), which makes the failure look like success on the glass.  Once
// the operator is engaged, PHASE_TIMEOUT_MS per step is ample.  QEMU is
// indifferent: the scripted first tap arrives within milliseconds.
static constexpr uint32_t FIRST_TOUCH_TIMEOUT_MS = 600000;
// Quiet time with no fresh buffer before the final verdict is read: long
// enough that the tail of the script (2 x 20 ms steps) cannot still be in
// flight, short enough to cost nothing.
static constexpr uint32_t DRAIN_QUIET_MS = 500;

// --- Widgets -----------------------------------------------------------------
static lv_obj_t *s_btn[5];
static lv_obj_t *s_hold, *s_trap, *s_handle;
static int32_t   s_drag_moves = 0;
static int32_t   s_handle_x   = 0;   /* last COMMANDED x -- see the cb comment */

static void handle_pressing_cb(lv_event_t *e)
{
    lv_obj_t  *obj   = (lv_obj_t *)lv_event_get_target(e);
    lv_indev_t *indev = lv_event_get_indev(e);
    if (!indev) return;
    lv_point_t p;
    lv_indev_get_point(indev, &p);
    /* Centre the handle on the pointer x; y stays put.  Clamped to the panel
     * so a wild coordinate cannot park it off-screen. */
    int32_t nx = p.x - HANDLE_RECT.w / 2;
    if (nx < 0) nx = 0;
    if (nx > (int32_t)PANEL_WIDTH - HANDLE_RECT.w)
        nx = (int32_t)PANEL_WIDTH - HANDLE_RECT.w;
    /* Compare against the last COMMANDED x, never lv_obj_get_x(): the getter
     * returns the PRE-LAYOUT coordinate until LVGL's refresh timer
     * recalculates it, so a second PRESSING event for the same published
     * sample re-triggers this branch off the stale read and double-counts
     * the move.  Measured, not theorised: off the getter, moves oscillated
     * 12/13 across seven otherwise-identical QEMU runs (the 33 ms refresh
     * phase against the 10 ms read timer decides which); off the commanded
     * value it is exactly the 9 distinct scripted positions, every run. */
    if (nx != s_handle_x) {
        lv_obj_set_x(obj, nx);
        s_handle_x = nx;
        s_drag_moves++;
    }
}

static lv_obj_t *make_checkable(const Rect &r, const char *text,
                                const lv_font_t *font)
{
    lv_obj_t *btn = lv_button_create(lv_screen_active());
    lv_obj_set_pos(btn, r.x, r.y);
    lv_obj_set_size(btn, r.w, r.h);
    lv_obj_add_flag(btn, LV_OBJ_FLAG_CHECKABLE);
    /* Explicit colours in BOTH states: deterministic pixels for the pre-touch
     * golden, and a visible flip for the bench operator. */
    lv_obj_set_style_bg_color(btn, lv_color_hex(0x1C2B3E), LV_PART_MAIN);
    lv_obj_set_style_bg_color(btn, lv_color_hex(0x2EA043),
                              LV_PART_MAIN | LV_STATE_CHECKED);
    lv_obj_t *label = lv_label_create(btn);
    lv_label_set_text(label, text);
    lv_obj_set_style_text_font(label, font, LV_PART_MAIN);
    lv_obj_center(label);
    return btn;
}

static void build_scene()
{
    lv_obj_t *scr = lv_screen_active();
    /* The drag must move the HANDLE, not scroll the screen. */
    lv_obj_remove_flag(scr, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_color(scr, lv_color_hex(0x101820), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, LV_PART_MAIN);

    static const char *NUM[5] = { "1", "2", "3", "4", "5" };
    for (uint8_t i = 0; i < 5; i++)
        s_btn[i] = make_checkable(BTN_RECT[i], NUM[i], &lv_font_montserrat_28);

    s_hold = make_checkable(HOLD_RECT, "HOLD", &lv_font_montserrat_14);
    s_trap = make_checkable(TRAP_RECT, "TRAP", &lv_font_montserrat_14);

    s_handle = lv_obj_create(scr);
    s_handle_x = HANDLE_RECT.x;   /* the commanded-x tracker starts where the handle does */
    lv_obj_set_pos(s_handle, HANDLE_RECT.x, HANDLE_RECT.y);
    lv_obj_set_size(s_handle, HANDLE_RECT.w, HANDLE_RECT.h);
    lv_obj_remove_flag(s_handle, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_color(s_handle, lv_color_hex(0xB07020), LV_PART_MAIN);
    lv_obj_add_event_cb(s_handle, handle_pressing_cb, LV_EVENT_PRESSING, NULL);
}

// --- Phase driver ------------------------------------------------------------
// Watches are SEQUENTIAL and state-based: LVGL reacts whenever it reacts; the
// firmware only observes widget state and prints tokens on transitions.  The
// QEMU script fires the events in exactly this order, so the same watches
// serve both the model and a prompted bench operator.

// Pump LVGL until `obj` reports CHECKED or `timeout_ms` expires.
[[nodiscard]] static bool waitChecked(lv_obj_t *obj, const char *tok,
                                      uint32_t timeout_ms = PHASE_TIMEOUT_MS)
{
    const uint32_t deadline = millis() + timeout_ms;
    while ((int32_t)(millis() - deadline) < 0) {
        lvgl_rt1176_loop();
        if (lv_obj_has_state(obj, LV_STATE_CHECKED)) {
            Serial1.printf("%s=CHECKED\n", tok);
            return true;
        }
    }
    Serial1.printf("%s_FAIL reason=timeout\n", tok);
    return false;
}

[[nodiscard]] static bool phaseButtons()
{
    static const char *TOK[5] = { "BTN1", "BTN2", "BTN3", "BTN4", "BTN5" };
    for (uint8_t i = 0; i < 5; i++) {
        // Centre coordinates in the prompt, for the bench operator.
        Serial1.printf("TOUCH_PROMPT phase=A tap button %u at=(%d,%d)\n",
                       (unsigned)(i + 1),
                       (int)(BTN_RECT[i].x + BTN_RECT[i].w / 2),
                       (int)(BTN_RECT[i].y + BTN_RECT[i].h / 2));
        // Button 1's window is the operator-arrival window -- see
        // FIRST_TOUCH_TIMEOUT_MS.  The rest run on normal phase patience.
        if (!waitChecked(s_btn[i], TOK[i],
                         i == 0 ? FIRST_TOUCH_TIMEOUT_MS : PHASE_TIMEOUT_MS))
            return false;
    }
    return true;
}

[[nodiscard]] static bool phaseDrag()
{
    Serial1.println("TOUCH_PROMPT phase=B drag the amber handle to the right "
                    "edge of its band, one continuous stroke");
    const uint32_t deadline = millis() + PHASE_TIMEOUT_MS;
    while ((int32_t)(millis() - deadline) < 0) {
        lvgl_rt1176_loop();
        if (lv_obj_get_x(s_handle) >= DRAG_END_MIN && s_drag_moves >= DRAG_MIN_MOVES) {
            Serial1.printf("DRAG_END x=%ld moves=%ld\n",
                           (long)lv_obj_get_x(s_handle), (long)s_drag_moves);
            return true;
        }
    }
    Serial1.printf("DRAG_FAIL reason=timeout x=%ld moves=%ld\n",
                   (long)lv_obj_get_x(s_handle), (long)s_drag_moves);
    return false;
}

[[nodiscard]] static bool phaseHold()
{
    Serial1.println("TOUCH_PROMPT phase=C place a finger on HOLD, then a second "
                    "on TRAP; lift the HOLD finger FIRST, then the other");
    return waitChecked(s_hold, "HOLD");
}

// Pump LVGL until no fresh buffer has arrived for DRAIN_QUIET_MS, so the
// script's tail (or the operator's remaining finger) cannot still be in
// flight when TRAP's final state is read.  Bounded by the phase timeout.
static void drainQuiet()
{
    const uint32_t deadline = millis() + PHASE_TIMEOUT_MS;
    uint32_t last_buffers = lvgl_gt911_buffers();
    uint32_t quiet_since  = millis();
    while ((int32_t)(millis() - deadline) < 0) {
        lvgl_rt1176_loop();
        if (lvgl_gt911_buffers() != last_buffers) {
            last_buffers = lvgl_gt911_buffers();
            quiet_since  = millis();
        } else if (millis() - quiet_since >= DRAIN_QUIET_MS) {
            return;
        }
    }
}

void setup()
{
    Serial1.begin(115200);
    delay(200);
    Serial1.println("LVGL_RK055_TOUCH_BEGIN");

    const bool disp_ok = Display.begin();
    Serial1.println(disp_ok ? "PANEL_OK" : "PANEL_FAIL");
    if (!disp_ok) { Serial1.println("LVGL_RK055_TOUCH_DONE"); return; }

    Display.fillScreen(0x0000);
    lvgl_rt1176_begin();
    lv_display_t *disp = lvgl_mipi_panel_create(Display);
    build_scene();

    /* Render and checksum the initial frame BEFORE the indev exists.  Safe by
     * construction: the QEMU model stalls its script until the first buffer
     * is acknowledged, and nobody polls until the indev is created below.
     * This golden asserts "the scene built correctly" -- nothing about touch,
     * and no post-touch checksum is asserted anywhere. */
    uint32_t t0 = millis();
    while (!lvgl_mipi_panel_frame_done() && (millis() - t0) < 5000) {
        lvgl_rt1176_loop();
    }
    lvgl_sum_reset();
    lvgl_sum_feed(Display.framebuffer(), PANEL_FB_BYTES);
    Serial1.printf("LVGL_FLUSHED=%s\n", lvgl_mipi_panel_frame_done() ? "PASS" : "FAIL");
    Serial1.printf("LVGL_BYTES=%lu\n",
                   (unsigned long)(lvgl_mipi_panel_flushed_px() * PANEL_BYTES_PER_PIXEL));
    Serial1.printf("LVGL_SUM=0x%08lX\n", (unsigned long)lvgl_sum_value());

    /* Touch bring-up -- v2's proven sequence.  The caller owns Wire2 (it is
     * also the codec's bus). */
    Wire2.begin();
    Wire2.setClock(400000);
    if (!touch.begin()) {
        Serial1.printf("TOUCH_FAIL err=%s i2c=%u id=0x%08lX\n",
                       GT911::errorName(touch.lastError()),
                       (unsigned)touch.lastI2cStatus(),
                       (unsigned long)touch.lastDeviceId());
        Serial1.println("LVGL_RK055_TOUCH_DONE");
        return;
    }
    Serial1.println("I2C_OK");
    Serial1.printf("ADDR=0x%02X\n", (unsigned)touch.address());
    Serial1.println("GT911_OK");
    Serial1.printf("CFG_OK RES=%ux%u POINTS=%u\n",
                   (unsigned)touch.resolutionX(), (unsigned)touch.resolutionY(),
                   (unsigned)touch.configuredPoints());

    /* From here LVGL polls the part every 10 ms and the script (in QEMU) or
     * the operator (on the bench) drives the widgets. */
    lvgl_gt911_indev_create(disp, touch);

    const bool a = phaseButtons();
    const bool b = a && phaseDrag();
    const bool c = b && phaseHold();
    drainQuiet();

    /* TRAP is read only after the drain, because a broken re-adoption checks
     * it at the script's FINAL release -- after HOLD=CHECKED. */
    const bool trap_clear = !lv_obj_has_state(s_trap, LV_STATE_CHECKED);
    Serial1.printf("TRAP=%s\n", trap_clear ? "UNCHECKED" : "CHECKED");
    Serial1.printf("IDLE_POLLS=%lu\n", (unsigned long)lvgl_gt911_idle_polls());
    Serial1.printf("POLL_FAILS=%lu\n", (unsigned long)lvgl_gt911_poll_fails());
    Serial1.printf("BUFFERS=%lu\n", (unsigned long)lvgl_gt911_buffers());

    if (a && b && c && trap_clear &&
        lvgl_gt911_idle_polls() > 0 && lvgl_gt911_poll_fails() == 0) {
        Serial1.println("LVGL_TOUCH_OK");
    }
    Serial1.println("LVGL_RK055_TOUCH_DONE");
}

/* The scene stays live; widget state persists for the bench operator. */
void loop() { lvgl_rt1176_loop(); }
