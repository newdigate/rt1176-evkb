/* rk055_touch_test - RT1176 -> RK055HDMIPI4MA0 GT911 capacitive touch gate
 * Copyright (c) 2026 Nicholas Newdigate
 * SPDX-License-Identifier: MIT
 *
 * Stage 1: the panel is up and the GT911 has been reset, has latched its I2C
 * address from the INT pin, and has identified itself.
 *
 * Stage 2: its 186-byte configuration blob reads back with a valid checksum,
 * and the resolution and contact count it was programmed with are recorded.
 * Recorded, not asserted -- see the RES/POINTS comment in run_qemu.sh.
 *
 * Stage 3: THREE PHASES, because a touch gate that prints a token because a
 * register read succeeded proves nothing.  Each phase asks the contact stream
 * for something a stuck or fabricated value cannot supply:
 *
 *   TARGETS_OK  five drawn targets, hit IN ORDER, each followed by the release
 *               the part PUBLISHES.  A fixed coordinate satisfies at most one
 *               of them -- enforced by the static_assert below, not eyeballed.
 *               This is also the ONLY assertion anywhere that proves the
 *               coordinate-to-display MAPPING, and only on hardware: a
 *               flipped, swapped or mis-scaled axis makes the sequence
 *               impossible because the operator touches where the graphic is.
 *   SWIPE_OK    a drag whose x advances monotonically across >= SWIPE_MIN_PCT
 *               of the axis over >= SWIPE_MIN_SAMPLES samples.  Catches a
 *               stuck value and a mirrored axis without depending on absolute
 *               registration, and no single coordinate contributes more than
 *               one sample to it.
 *   MULTI_OK    two simultaneous contacts with DISTINCT track ids, far apart.
 *               The only phase that exercises the contact array and the count
 *               field rather than just points[0].
 *
 * Carried forward from the minimal stage 3, and NOT subsumed by any of the
 * above: FIRST_TOUCH and TOUCH_ADVANCED.  Both are now fed by the same polls
 * phase 1 makes, so they cost nothing, and they are what separates "the basic
 * read path works but a phase's geometry was not satisfied" from "no contact
 * ever arrived" in a transcript where the phase tokens are all absent.
 * TOUCH_ADVANCED in particular remains the direct assertion on the mandatory
 * status clear: it is emitted on a published RELEASE, which an unacknowledged
 * part can never produce.
 *
 *   INT_EDGES   how many times the INT line actually moved, from an
 *   BUFFERS     attachInterrupt() on D6 -- against how many fresh buffers the
 *               poll path consumed while that interrupt was attached.  The
 *               PAIR is the evidence, and their agreement is part of TOUCH_OK;
 *               see reportInt() below for why one count without the other
 *               proves nothing.  Corroborating only: the read path is polled
 *               and the ISR does nothing but count.  It is also this project's
 *               first exercise of a GPIO2 interrupt (irq_attach_test only ever
 *               proved GPIO3).
 *
 * Every stage token is emitted UNCONDITIONALLY so the first FAIL pinpoints the
 * broken layer rather than the run simply stopping, and every failure names the
 * phase, the reason and the last coordinate seen.  FIRST_TOUCH and
 * TOUCH_ADVANCED are printed the instant their evidence exists rather than at
 * the end of a phase, so that they survive a run the harness kills mid-phase --
 * which is exactly the shape a wedged part produces.
 *
 * QEMU vs HARDWARE.  In QEMU the virtual GT911 replays a MODEL-OWNED scripted
 * path the firmware cannot influence, and the model places its contacts at the
 * same percentages this file places its targets at.  A green QEMU run therefore
 * proves the decode and this state machine, and NOT the mapping -- see the note
 * above the TARGETS_OK assertion in run_qemu.sh.
 *
 * Uses Serial1 (the LPUART console run_qemu.sh captures via `-serial file:`),
 * not Serial (native USB CDC, which QEMU would not capture here).
 */
#include <Arduino.h>
#include <Wire.h>
#include <Display.h>
#include "gt911.h"

// D9 = GPIO_AD_01 = touch reset; D6 = GPIO_AD_00 = touch interrupt.
// Both are RevC3 nets CTP_RST_B / CTP_INT via 0R to J48 pins 28 / 29.
//
// ** These are Arduino D6 and D9. **  Anything else on those pins fights the
// touch controller, and because the I2C address is latched from the INT level
// at reset release, it can do so silently.  irq_attach_test's D13->D9 jumper is
// the one to watch for on the bench.
static constexpr uint8_t TOUCH_RST_PIN = 9;
static constexpr uint8_t TOUCH_INT_PIN = 6;

static GT911 touch(Wire2, TOUCH_RST_PIN, TOUCH_INT_PIN);

// --- Phase 1 geometry ------------------------------------------------------
// Targets as PERCENTAGES of the display, so the same numbers work whatever the
// panel's size.  Integer arithmetic throughout: no floats in a gate.
struct Target { uint8_t x_pct, y_pct; };
// constexpr, not const: the static_assert below walks this table at compile
// time, and that is what keeps the acceptance boxes provably unambiguous.
static constexpr Target TARGETS[] = {
  {15, 10}, {85, 10}, {15, 90}, {85, 90}, {50, 50},
};
static constexpr uint8_t  NUM_TARGETS  = sizeof(TARGETS) / sizeof(TARGETS[0]);
static constexpr uint16_t TARGET_HALF  = 80;   // drawn square is 160x160
static constexpr uint16_t TARGET_TOL   = 110;  // accepted box, half-width

// --- Phase 2 / 3 thresholds ------------------------------------------------
static constexpr uint8_t  SWIPE_MIN_SAMPLES = 8;
static constexpr uint8_t  SWIPE_MIN_PCT     = 60;  // of the x axis
static constexpr uint16_t SWIPE_BACK_TOL    = 2;   // px of backwards jitter tolerated
static constexpr uint16_t MULTI_MIN_SEP     = 200; // px between the contacts

// Per-phase patience, in guest milliseconds.  Sized for the HARDWARE operator,
// who has to read a prompt and move a finger; QEMU's scripted path finishes
// every phase in well under a second and so never approaches it.
//
// THE COST OF THAT IS REAL AND IS ACCEPTED, not overlooked: a phase that
// genuinely times out under QEMU spends this long in guest time, and guest time
// runs at roughly half wall on the gate machine (1.09 s of guest measured in
// 2.2 s of wall for a whole green run), so 60 s here is about two minutes of
// wall -- far longer than run_qemu.sh is willing to wait.  The harness kills
// the run before the *_FAIL line is printed.  The gate still goes red -- on the missing
// token, with its own named message -- and the partial transcript still shows
// which phase was reached.  Shortening this to buy that one line back would
// mean a bench operator losing a target because they blinked, which is the
// worse trade.  run_qemu.sh says the same thing where it sizes its window.
static constexpr uint32_t PHASE_TIMEOUT_MS  = 60000;

// -------------------------------------------------------------------------
// COMPILE-TIME PROOF THAT NO TWO ACCEPTANCE BOXES OVERLAP.
// -------------------------------------------------------------------------
// The whole worth of phase 1 is that a stuck, fabricated or replayed
// coordinate can satisfy AT MOST ONE target.  That is a property of the
// geometry, not of the loop below, and it is the kind of property an edit to
// TARGETS[] or TARGET_TOL destroys silently -- with every gate still green,
// because the model's scripted taps would go on hitting whichever box they now
// land in.  So it is checked by the compiler.
//
// PANEL_WIDTH / PANEL_HEIGHT rather than Display.width() / Display.height():
// those accessors are not constexpr, and they return exactly these two
// constants (Display.h), so the check governs the same geometry the runtime
// uses.  panel_config.h reaches us through Display.h.
static constexpr uint16_t targetCxOf(uint8_t i) {
  return (uint16_t)((uint32_t)PANEL_WIDTH * TARGETS[i].x_pct / 100u);
}
static constexpr uint16_t targetCyOf(uint8_t i) {
  return (uint16_t)((uint32_t)PANEL_HEIGHT * TARGETS[i].y_pct / 100u);
}
static constexpr uint16_t absDiff(uint16_t a, uint16_t b) {
  return a > b ? (uint16_t)(a - b) : (uint16_t)(b - a);
}
static constexpr bool targetsUnambiguous() {
  for (uint8_t i = 0; i < NUM_TARGETS; i++) {
    for (uint8_t j = (uint8_t)(i + 1); j < NUM_TARGETS; j++) {
      // Boxes are Chebyshev: disjoint if they are separated on EITHER axis.
      if (absDiff(targetCxOf(i), targetCxOf(j)) <= 2u * TARGET_TOL &&
          absDiff(targetCyOf(i), targetCyOf(j)) <= 2u * TARGET_TOL) {
        return false;
      }
    }
  }
  return true;
}
static_assert(targetsUnambiguous(),
              "two phase-1 acceptance boxes overlap: one coordinate could "
              "satisfy both targets, and phase 1 would stop proving that the "
              "contacts moved.  Move the targets apart or shrink TARGET_TOL.");

// --- The INT line ----------------------------------------------------------
// The ISR counts and does nothing else.  read() is a multi-step I2C
// transaction over shared Wire state and is explicitly not reentrant, so
// touching the bus from here would corrupt any poll in flight.
static volatile uint32_t int_edges = 0;
static void touch_int_isr() { int_edges++; }

// --- Observations shared across the phases ---------------------------------
// Every poll in this file goes through pollTouch(), which is what keeps these
// counters honest: a read() called directly would consume a buffer that
// BUFFERS never counted, and INT_EDGES would then look like an excess.
static TouchPoint first_touch{};
static bool     have_first  = false;
static bool     saw_release = false;
static uint32_t buffers     = 0;   // fresh buffers consumed since the attach
static uint32_t poll_fails  = 0;

// Poll the part and fold the result into the observations above.
//
// [[nodiscard]] like the read() it wraps, and for the same reason: this
// example is built with -Werror=unused-result (CMakeLists.txt), and a poll
// whose result is dropped is a poll whose outcome nothing acted on.
[[nodiscard]] static GT911::Poll pollTouch(TouchPoint *pts, uint8_t maxPts,
                                           uint8_t &n) {
  n = 0;
  const GT911::Poll p = touch.read(pts, maxPts, &n);
  switch (p) {
    case GT911::Poll::Contacts:
      buffers++;
      // PRINTED THE MOMENT THE EVIDENCE EXISTS, not at the end of a phase.
      // These two tokens have to survive a run the harness kills mid-phase --
      // which is exactly the shape a wedged part produces, since the release
      // wait in phase 1 then runs to its full timeout.  Deferring them put
      // "FAIL: read() never reported a contact" in the transcript directly
      // under a TARGET 1/5 HIT line, which is a false diagnosis of the one
      // fault this pair exists to name.  Measured: that is what the
      // status-clear negative test printed before this was moved here.
      //
      // Once each, so this cannot flood a poll loop.
      if (!have_first && n > 0) {
        first_touch = pts[0];
        have_first = true;
        Serial1.printf("FIRST_TOUCH x=%u y=%u id=%u\n", (unsigned)first_touch.x,
                       (unsigned)first_touch.y, (unsigned)first_touch.id);
      }
      break;
    case GT911::Poll::Released:
      buffers++;
      // TOUCH_ADVANCED is the release, and the release is the whole assertion
      // on the mandatory write of 0 to 0x814E: an unacknowledged part re-serves
      // the same one-contact buffer for ever and can never publish a
      // zero-contact one.
      if (!saw_release) {
        saw_release = true;
        Serial1.println("TOUCH_ADVANCED");
      }
      break;
    case GT911::Poll::Failed:
      poll_fails++;
      break;
    case GT911::Poll::Idle:
      break;
  }
  return p;
}

// --- Drawing ---------------------------------------------------------------
// The CM7 D-cache is off (startup.c), so a plain CPU write into the SDRAM
// framebuffer is coherent with the LCDIFv2's fetch -- there is nothing to clean
// between writing here and the panel seeing it.
static void fillRect(int32_t x, int32_t y, int32_t w, int32_t h, uint16_t c) {
  uint16_t *fb = Display.framebuffer();
  if (!fb) return;
  const int32_t W = (int32_t)Display.width(), H = (int32_t)Display.height();
  if (x < 0) { w += x; x = 0; }
  if (y < 0) { h += y; y = 0; }
  if (x + w > W) w = W - x;
  if (y + h > H) h = H - y;
  if (w <= 0 || h <= 0) return;
  for (int32_t row = 0; row < h; row++) {
    uint16_t *p = fb + (y + row) * W + x;
    for (int32_t col = 0; col < w; col++) p[col] = c;
  }
}

static uint16_t targetCx(uint8_t i) {
  return (uint16_t)((uint32_t)Display.width() * TARGETS[i].x_pct / 100u);
}
static uint16_t targetCy(uint8_t i) {
  return (uint16_t)((uint32_t)Display.height() * TARGETS[i].y_pct / 100u);
}

// Scale a controller coordinate into display space using the resolution the
// part REPORTED.  Never assume it equals the panel's.  Safe to divide without
// a guard only because begin() succeeded: the driver validates both axes
// non-zero before it returns true, and nothing here runs otherwise.
static uint16_t toScreenX(uint16_t tx) {
  return (uint16_t)((uint32_t)tx * Display.width() / touch.resolutionX());
}
static uint16_t toScreenY(uint16_t ty) {
  return (uint16_t)((uint32_t)ty * Display.height() / touch.resolutionY());
}

// "last=(x,y)" for a failure line, or "last=none" when the phase never saw a
// contact at all.  Those two cases need telling apart and a bare 0,0 cannot do
// it -- (0,0) is a legitimate corner touch.
static void printLast(int32_t x, int32_t y) {
  if (x < 0) Serial1.print(" last=none");
  else       Serial1.printf(" last=(%ld,%ld)", (long)x, (long)y);
}

// --- Phases ----------------------------------------------------------------
static bool phaseTargets() {
  TouchPoint pts[2];
  for (uint8_t i = 0; i < NUM_TARGETS; i++) {
    const uint16_t cx = targetCx(i), cy = targetCy(i);

    Display.fillScreen(0x0000);
    fillRect((int32_t)cx - TARGET_HALF, (int32_t)cy - TARGET_HALF,
             TARGET_HALF * 2, TARGET_HALF * 2, 0xFFFF);
    Serial1.printf("TOUCH_PROMPT phase=1 target=%u/%u at=(%u,%u)\n",
                   (unsigned)(i + 1), (unsigned)NUM_TARGETS,
                   (unsigned)cx, (unsigned)cy);

    const uint32_t deadline = millis() + PHASE_TIMEOUT_MS;
    int32_t lastX = -1, lastY = -1;
    bool hit = false;
    while ((int32_t)(millis() - deadline) < 0 && !hit) {
      uint8_t n = 0;
      // Contacts is the only state carrying coordinates.  Idle is the common
      // case by far -- this loop runs far faster than the part publishes.
      if (pollTouch(pts, 2, n) != GT911::Poll::Contacts || n == 0) continue;
      lastX = (int32_t)toScreenX(pts[0].x);
      lastY = (int32_t)toScreenY(pts[0].y);
      const int32_t dx = lastX - (int32_t)cx;
      const int32_t dy = lastY - (int32_t)cy;
      if (dx <= (int32_t)TARGET_TOL && dx >= -(int32_t)TARGET_TOL &&
          dy <= (int32_t)TARGET_TOL && dy >= -(int32_t)TARGET_TOL) hit = true;
    }
    if (!hit) {
      Serial1.printf("TARGET_FAIL phase=1 target=%u reason=timeout want=(%u,%u)",
                     (unsigned)(i + 1), (unsigned)cx, (unsigned)cy);
      printLast(lastX, lastY);
      Serial1.println();
      return false;
    }
    Serial1.printf("TARGET %u/%u HIT x=%ld y=%ld\n", (unsigned)(i + 1),
                   (unsigned)NUM_TARGETS, (long)lastX, (long)lastY);

    // Wait for the RELEASE THE PART PUBLISHES, so the next target cannot be
    // satisfied by the finger that is still down on this one.  Not for a quiet
    // poll: a zero count is also what an idle poll gives, and idle polls
    // outnumber real samples by orders of magnitude, so waiting on one would
    // wait for nothing at all.
    //
    // A REQUIREMENT, not a courtesy pause.  A part whose status register is
    // never acknowledged re-serves the same one-contact buffer for ever and can
    // never reach Released, so this is where a missing status clear is caught
    // by name rather than three targets later as an unexplained timeout.
    const uint32_t rel = millis() + PHASE_TIMEOUT_MS;
    bool released = false;
    while ((int32_t)(millis() - rel) < 0 && !released) {
      uint8_t n = 0;
      if (pollTouch(pts, 2, n) == GT911::Poll::Released) released = true;
    }
    if (!released) {
      Serial1.printf("TARGET_FAIL phase=1 target=%u reason=no-release "
                     "want=(%u,%u)", (unsigned)(i + 1), (unsigned)cx,
                     (unsigned)cy);
      printLast(lastX, lastY);
      Serial1.println();
      return false;
    }
  }
  Serial1.println("TARGETS_OK");
  return true;
}

static bool phaseSwipe() {
  TouchPoint pts[2];
  Display.fillScreen(0x0000);
  fillRect(0, (int32_t)Display.height() / 2 - 40, (int32_t)Display.width(), 80,
           0x07E0);
  Serial1.println("TOUCH_PROMPT phase=2 swipe LEFT to RIGHT across the band");

  const uint32_t deadline = millis() + PHASE_TIMEOUT_MS;
  uint16_t minX = 0, maxX = 0, prevX = 0;
  int32_t  lastX = -1, lastY = -1;
  uint8_t  samples = 0;
  bool     down = false;
  const char *reason = "timeout";

  while ((int32_t)(millis() - deadline) < 0) {
    uint8_t n = 0;
    const GT911::Poll p = pollTouch(pts, 2, n);
    // IDLE IS NOT A LIFT.  The poll loop runs far faster than the part's
    // publish cadence, so idle polls vastly outnumber real samples; treating
    // one as a lift resets the run between every pair of samples and
    // SWIPE_MIN_SAMPLES is then never reached.  A count-based draft of this
    // loop did exactly that.
    if (p == GT911::Poll::Idle) continue;
    if (p == GT911::Poll::Failed) continue;   // counted in POLL_FAILS below
    if (p == GT911::Poll::Released) {
      // A genuine lift.  If the run was long enough to be interesting it had
      // its chance at the span test below and did not pass it, so say so
      // rather than blaming the clock.
      if (down && samples >= SWIPE_MIN_SAMPLES) { reason = "lifted-short"; break; }
      down = false; samples = 0;
      continue;
    }
    if (n == 0) continue;                     // Contacts, but no capacity given
    const uint16_t x = toScreenX(pts[0].x);
    lastX = (int32_t)x;
    lastY = (int32_t)toScreenY(pts[0].y);
    if (!down) {
      down = true; prevX = x; minX = x; maxX = x; samples = 1;
      continue;
    }
    // Monotonic in +x.  A step backwards by more than SWIPE_BACK_TOL restarts
    // the run rather than failing outright -- a real finger jitters -- and the
    // restart discards the span too, so a finger that wanders back and forth
    // accumulates nothing.  The SPAN requirement is what actually makes this
    // hard to fake: SWIPE_MIN_PCT of the axis has to be crossed inside ONE
    // uninterrupted forward run.
    if (x + SWIPE_BACK_TOL < prevX) {
      samples = 1; minX = x; maxX = x; prevX = x;
      continue;
    }
    if (x > maxX) maxX = x;
    if (x < minX) minX = x;
    prevX = x;
    if (samples < 255) samples++;

    const uint32_t span_pct = (uint32_t)(maxX - minX) * 100u / Display.width();
    if (samples >= SWIPE_MIN_SAMPLES && span_pct >= SWIPE_MIN_PCT) {
      Serial1.printf("SWIPE_OK dir=+x span=%lu%% samples=%u\n",
                     (unsigned long)span_pct, (unsigned)samples);
      return true;
    }
  }
  Serial1.printf("SWIPE_FAIL phase=2 reason=%s samples=%u span=%lu%%", reason,
                 (unsigned)samples,
                 (unsigned long)(down ? (uint32_t)(maxX - minX) * 100u /
                                            Display.width()
                                      : 0u));
  printLast(lastX, lastY);
  Serial1.println();
  return false;
}

static bool phaseMulti() {
  TouchPoint pts[2];
  Display.fillScreen(0x001F);
  Serial1.println("TOUCH_PROMPT phase=3 hold TWO fingers apart");

  const uint32_t deadline = millis() + PHASE_TIMEOUT_MS;
  int32_t lastX = -1, lastY = -1;
  uint8_t maxSeen = 0;   // the most contacts any one buffer carried
  int32_t bestSep = -1;  // the widest separation seen with two distinct ids

  while ((int32_t)(millis() - deadline) < 0) {
    uint8_t n = 0;
    if (pollTouch(pts, 2, n) != GT911::Poll::Contacts || n == 0) continue;
    lastX = (int32_t)toScreenX(pts[0].x);
    lastY = (int32_t)toScreenY(pts[0].y);
    if (n > maxSeen) maxSeen = n;
    if (n < 2) continue;
    // Distinct TRACK IDS, not distinct array slots.  The driver sorts contacts
    // by id, so equal ids here would mean one contact delivered twice.
    if (pts[0].id == pts[1].id) continue;
    const int32_t bx = (int32_t)toScreenX(pts[1].x);
    const int32_t by = (int32_t)toScreenY(pts[1].y);
    const int32_t dx = lastX > bx ? lastX - bx : bx - lastX;
    const int32_t dy = lastY > by ? lastY - by : by - lastY;
    const int32_t sep = dx > dy ? dx : dy;     // Chebyshev: no sqrt in a gate
    if (sep > bestSep) bestSep = sep;
    if (sep < (int32_t)MULTI_MIN_SEP) continue;
    Serial1.printf("MULTI_OK ids=%u,%u sep=%ld\n", (unsigned)pts[0].id,
                   (unsigned)pts[1].id, (long)sep);
    return true;
  }
  Serial1.printf("MULTI_FAIL phase=3 reason=timeout maxcontacts=%u bestsep=%ld",
                 (unsigned)maxSeen, (long)bestSep);
  printLast(lastX, lastY);
  Serial1.println();
  return false;
}

// The NEGATIVE halves of FIRST_TOUCH and TOUCH_ADVANCED.  Their positive halves
// are printed by pollTouch() the instant the evidence exists, so all this has
// to do is speak for the runs where the evidence never arrived -- and it can
// only do that from here, once the polling is over.
//
// Both carry diagnostics because the two ways of reaching either need telling
// apart in a transcript: a part wedged on one buffer (err=NONE -- nothing
// failed, it simply never moved on) versus one whose transfers are failing
// (err=PTREAD / STATCLR / STATREAD).
//
// The FIRST_TOUCH coordinates are RECORDED, never asserted: in QEMU the
// scripted path and the on-screen targets are both percentages of the same
// resolution, so model and firmware share an assumption about which corner is
// (0,0), and no QEMU run can falsify a shared assumption.
static void reportReadPathGaps() {
  if (!have_first) {
    Serial1.printf("FIRST_TOUCH_NONE err=%s I2C=%u\n",
                   GT911::errorName(touch.lastError()),
                   (unsigned)touch.lastI2cStatus());
  }
  if (!saw_release) {
    Serial1.printf("TOUCH_STALLED err=%s I2C=%u\n",
                   GT911::errorName(touch.lastError()),
                   (unsigned)touch.lastI2cStatus());
  }
}

// INT_EDGES AND BUFFERS ARE ONE PIECE OF EVIDENCE, NOT TWO, and neither half
// means anything alone: an edge count with nothing to compare it against would
// pass against a counter wired to a chattering pin, and a buffer count says
// nothing about the pin at all.  Together they state a relation that run_qemu.sh
// asserts -- the part pulses INT once per published instant and read()
// acknowledges each published instant exactly once, so the two counts are the
// same number, give or take one instant published after the last poll and
// before this print.
//
// THE WINDOW THIS DOES NOT COVER, stated rather than papered over: an instant
// published in the gap between begin() returning and the attachInterrupt()
// below it would be consumed later as a buffer whose edge was never counted,
// making INT_EDGES one SHORT.  The gap is a few instructions wide, and under
// QEMU it is not reachable at all -- the model arms its script 100 ms after the
// address latch and begin() returns roughly 45 ms before that.  On hardware it
// needs a finger already on the glass during bring-up, before phase 1 has
// prompted for one.
//
// THE VERDICT IS COMPUTED HERE AND NOT ONLY IN run_qemu.sh, and that
// duplication is deliberate.  Task 7 reads this transcript off real glass with
// no shell gate anywhere near it, and a run that printed TOUCH_OK above an
// INT_EDGES=0 would be read as a pass by exactly the person who most needs to
// see the failure.  Measured: deleting the attachInterrupt() call produced that
// transcript.  The gate keeps its own copy of the arithmetic because it is the
// half that names WHICH direction went wrong in the harness output.
static bool reportInt() {
  const uint32_t edges = int_edges;
  Serial1.printf("INT_EDGES=%lu\n", (unsigned long)edges);
  Serial1.printf("BUFFERS=%lu\n", (unsigned long)buffers);
  // The +1 is the one legitimate discrepancy: an instant published after the
  // last poll and before this print.  Anything else is a finding.
  const bool ok = edges >= buffers && edges <= buffers + 1u;
  if (!ok) {
    Serial1.printf("INT_MISMATCH edges=%lu buffers=%lu reason=%s\n",
                   (unsigned long)edges, (unsigned long)buffers,
                   edges < buffers ? "edges-missing" : "edges-unexplained");
  }
  // Recorded, never asserted.  A failing poll is not fatal on its own -- the
  // driver clears lastError() on the next good one and the phases simply keep
  // polling -- but a nonzero count beside a phase failure is the first thing
  // worth knowing, and on silicon it is the bus-quality reading this gate has.
  Serial1.printf("POLL_FAILS=%lu\n", (unsigned long)poll_fails);
  Serial1.printf("ELAPSED_MS=%lu\n", (unsigned long)millis());
  return ok;
}

void setup() {
    Serial1.begin(115200);
    delay(200);
    Serial1.println("RK055_TOUCH_BEGIN");

    Display.begin();
    Serial1.printf("PANEL_%s\n", Display.panelOk() ? "OK" : "FAIL");
    Display.fillScreen(0x0000);

    // LPI2C5 is shared with the WM8962 codec, so the EXAMPLE owns the bus, not
    // the driver.  400 kHz is the GT911's rated maximum; the 24 MHz functional
    // clock the Wire library configures for this instance is what setClock()'s
    // prescale arithmetic assumes.  NXP instead runs the root at 24 MHz / 12
    // and divides less -- we reach a comparable SCL rate from a root that is
    // already hardware-proven on this bus.
    Wire2.begin();
    Wire2.setClock(400000);

    const bool ok = touch.begin();

    // Two independent questions, answered separately.  I2C_ is "did anything
    // ACK at the address we latched"; GT911_ is "was it a GT911".  A part that
    // latched the OTHER address fails the first; a different part on the bus
    // would fail the second.
    //
    // busOk() rather than a test on lastError() here: classifying errors is the
    // driver's job, and an example that enumerated the driver's taxonomy would
    // quietly go wrong the first time an enumerator was added.
    Serial1.printf("I2C_%s\n", touch.busOk() ? "OK" : "FAIL");
    Serial1.printf("ADDR=0x%02X\n", touch.address());

    if (ok) {
        // Design 6.1 asks for "GT911_OK  ID=911".  The ID is RENDERED FROM THE
        // FOUR BYTES ACTUALLY READ at 0x8140, not printed as a literal -- a
        // literal would assert nothing.  Safe to treat as text only here:
        // begin() has already proved these bytes equal "911\0", so the NUL
        // terminates the string and every character is printable.  The failure
        // path keeps the raw hex instead, which is what you want when the
        // bytes are NOT "911".
        const uint32_t raw = touch.lastDeviceId();
        char id[5];
        for (uint8_t i = 0; i < 4; i++) id[i] = (char)((raw >> (8 * i)) & 0xFF);
        id[4] = '\0';
        Serial1.printf("GT911_OK  ID=%s\n", id);
    } else {
        Serial1.println("GT911_FAIL");
    }

    // The configuration layer.  begin() reads and verifies the 186-byte blob as
    // its last act, so `ok` IS "the blob arrived and verified" -- there is no
    // separate query to make, and CFG_FAIL after an earlier failure (a bad ID,
    // say) is honest: the blob was never verified.  TOUCH_ERR below names the
    // layer that actually broke.
    //
    // RES/POINTS are printed unconditionally and are 0 0 on any failure, which
    // is the driver's contract, not a special case here.  They are RECORDED,
    // not asserted: the real panel reports whatever it was programmed with.
    Serial1.printf("CFG_%s\n", ok ? "OK" : "FAIL");
    Serial1.printf("RES=%ux%u\n", (unsigned)touch.resolutionX(),
                                  (unsigned)touch.resolutionY());
    Serial1.printf("POINTS=%u\n", (unsigned)touch.configuredPoints());

    // begin()'s RETURN VALUE is the guard for everything below, checked once,
    // here.  Not lastError(), and not Poll::Failed: the driver's header is
    // explicit that "Failed beside lastError() == None" is a debugging aid for
    // a human reading a transcript and not a control-flow gate.
    //
    // Note what the combination above says on the way out: a config-CONTENT
    // failure (CFGVER / CFGSUM / CFGRES / CFGPTS) prints I2C_OK, because 186
    // bytes did arrive and only what they hold is wrong, while a config-READ
    // failure prints I2C_FAIL.  The driver draws that line in busOk(); this
    // example only reports it, and deliberately does not enumerate the taxonomy
    // itself -- the four content tokens were added to the driver without
    // touching a line of this file.
    if (!ok) {
        Serial1.printf("TOUCH_ERR=%s ID=0x%08lX I2C=%u\n",
                       GT911::errorName(touch.lastError()),
                       (unsigned long)touch.lastDeviceId(),
                       (unsigned)touch.lastI2cStatus());
        return;
    }

    // ONLY NOW, once begin() has released INT to an input, may anything else
    // touch that pin.  During bring-up it is an OUTPUT driving the address
    // latch, and an interrupt attached earlier would fight it.
    attachInterrupt(digitalPinToInterrupt(TOUCH_INT_PIN), touch_int_isr, RISING);

    // The phases run in order and stop at the first failure, but the reporting
    // below always runs: a transcript that stops mid-phase tells the next
    // reader far less than one that names the phase, the basic read path's
    // state, and the INT evidence.
    const bool p1 = phaseTargets();
    const bool p2 = p1 && phaseSwipe();
    const bool p3 = p2 && phaseMulti();
    reportReadPathGaps();
    const bool intOk = reportInt();

    // TOUCH_OK means EVERYTHING this gate checks passed, INT evidence included
    // -- see reportInt() for why that verdict is drawn here as well as in
    // run_qemu.sh.
    if (p3 && intOk) Serial1.println("TOUCH_OK");
}

void loop() {}
