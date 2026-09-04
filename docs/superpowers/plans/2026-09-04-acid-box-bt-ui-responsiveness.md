# acid_box BT UI Responsiveness (NEW-33) Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Measure where acid_box's BT-streaming main loop spends its time, then fix what the measurement names, so the BT build's UI is within 15 % of the default build on frames/s and p95 touch-to-frame latency with `pcmdrops=0` / `drops=0` held.

**Architecture:** An opt-in `ACIDBOX_LOOPSTAT` instrument (per-slot `micros()` laps in `loop()`, LVGL display events for frames, a wrapped indev read callback for touch, a title-tap knob wiggle for synthetic load) prints three once-per-second lines; both the BT-off and BT-on builds carry it, and a bench A/B on silicon attributes the cost. Fix 1 (predicted) gives `HciTransport` a 5 KB TX ring so the credit-paced ACL write never spins on Serial2's 64-byte buffer; fix 2 (conditional) moves the SBC encode into a pended priority-240 IRQ.

**Tech Stack:** C/C++ on the imxrt1176 core (CMake, ARM GCC 10), LVGL 9.4, SynthUI rotary widget, M2Radio `hci/` + `bt/`, LinkServer + `tools/rt1170-console.py` for the bench, host `cc` for the one pure helper's test.

**Spec:** `docs/superpowers/specs/2026-09-04-acid-box-bt-ui-responsiveness-design.md`

**Branch:** `nicnewdigate/new-33-acid_box-bt-streaming-degrades-ui-responsiveness-investigate` in the main checkout (decided in brainstorming; build dirs are reused).

**Scratchpad:** `SCRATCH=/private/tmp/claude-501/-Users-nicholasnewdigate-Development-rt1170-evkb/93944375-9596-4dcc-9da4-fc113d32b8a1/scratchpad`. `$SCRATCH/acid_box.pre.bin` (295,936 bytes) is the default build's LOADABLE IMAGE (`arm-none-eabi-objcopy -O binary`) built from the current source BEFORE any change in this plan, with `acid_box.pre.elf` beside it; Task 4 compares against the `.bin`.

★ **Compare the loadable image, never the raw ELF.** Found while snapshotting: an untouched rebuild of the default build differed from the 17:26 ELF at an offset inside `.debug_info`, because the 20:18 commit had added one line to `acid_box.cpp` under `M2_BT_OUT` — every edited line shifts the DWARF line tables even when the code it adds is compiled out. The `objcopy -O binary` images were identical. So "byte-identical" here means the loadable image, which is what the gate boots and the golden hashes; the ELF's debug sections move with every edit.

---

## File structure

| file | responsibility |
|---|---|
| `examples/display/acid_box/loopstat_pct.h` (new) | pure C ring + nearest-rank percentile; host-tested |
| `examples/display/acid_box/tests/loopstat_pct_test.c`, `tests/run.sh` (new) | the helper's host suite (PASS:/FAIL: lines, count at the end) |
| `examples/audio/bt_tone_test/AudioOutputBluetooth.{h,cpp}` | three accumulators: `encodeUs()`, `drainUs()`, `txBytes()` (Task 2); fix 2's `setEncodeIrq()` (Task 8, conditional) |
| `examples/display/acid_box/CMakeLists.txt` | `option(ACIDBOX_LOOPSTAT)` |
| `examples/display/acid_box/acid_box.cpp` | the instrument (flash-resident), `loop()` laps, wiggle, `hci_txring` witness + `static_assert` |
| `~/Development/M2Radio/hci/HciTransport.{h,cpp}` | fix 1: `TX_EXTRA` + `addMemoryForWrite` |
| `evkb.cmake` | M2Radio pin bump |
| `examples/display/acid_box/transcript_hw_evkb_bt.txt` | the bench numbers, before and after |
| `CLAUDE.md`, memory | close-out |

---

### Task 1: `loopstat_pct.h` — ring + nearest-rank percentile, host-tested

**Files:**
- Create: `examples/display/acid_box/loopstat_pct.h`
- Create: `examples/display/acid_box/tests/loopstat_pct_test.c`
- Create: `examples/display/acid_box/tests/run.sh`

- [ ] **Step 1: Write the failing test**

`examples/display/acid_box/tests/loopstat_pct_test.c`:

```c
/* loopstat_pct_test.c -- host test for loopstat_pct.h (NEW-33).
 * PASS:/FAIL: per check, count at the end (the tree's convention:
 * `grep -c "^PASS:"` on a live run is the case count). */
#include <stdio.h>
#include <stdint.h>
#include "../loopstat_pct.h"

static int pass = 0, fail = 0;
static void check(const char *name, uint32_t got, uint32_t want)
{
    if (got == want) { printf("PASS: %s (%lu)\n", name, (unsigned long)got); pass++; }
    else { printf("FAIL: %s got %lu want %lu\n", name, (unsigned long)got, (unsigned long)want); fail++; }
}

int main(void)
{
    uint32_t buf[4], sorted[256];
    loopstat_ring_t r;

    /* empty ring: every percentile is 0, count 0 */
    loopstat_ring_init(&r, buf, 4);
    check("empty sorted count", loopstat_ring_sorted(&r, sorted), 0);
    check("empty p50", loopstat_pct_sorted(sorted, 0, 50), 0);

    /* one sample: every percentile is that sample */
    loopstat_ring_push(&r, 7);
    uint32_t n = loopstat_ring_sorted(&r, sorted);
    check("n=1 count", n, 1);
    check("n=1 p50", loopstat_pct_sorted(sorted, n, 50), 7);
    check("n=1 p95", loopstat_pct_sorted(sorted, n, 95), 7);
    check("n=1 p100", loopstat_pct_sorted(sorted, n, 100), 7);

    /* {4,1,3,2} -> sorted {1,2,3,4}: p50 = rank ceil(2) = 2nd = 2, p95 = rank 4 = 4 */
    loopstat_ring_reset(&r);
    loopstat_ring_push(&r, 4); loopstat_ring_push(&r, 1);
    loopstat_ring_push(&r, 3); loopstat_ring_push(&r, 2);
    n = loopstat_ring_sorted(&r, sorted);
    check("n=4 count", n, 4);
    check("n=4 sorted[0]", sorted[0], 1);
    check("n=4 sorted[3]", sorted[3], 4);
    check("n=4 p50", loopstat_pct_sorted(sorted, n, 50), 2);
    check("n=4 p95", loopstat_pct_sorted(sorted, n, 95), 4);
    check("n=4 p0 clamps to rank 1", loopstat_pct_sorted(sorted, n, 0), 1);

    /* wrap: cap 4, push 1..6 -> the ring keeps {3,4,5,6} */
    loopstat_ring_push(&r, 5); loopstat_ring_push(&r, 6);
    loopstat_ring_reset(&r);
    for (uint32_t v = 1; v <= 6; v++) loopstat_ring_push(&r, v);
    n = loopstat_ring_sorted(&r, sorted);
    check("wrap count", n, 4);
    check("wrap min", sorted[0], 3);
    check("wrap max", sorted[3], 6);

    /* 10 samples 10,9,...,1 -> p95 is the 10th smallest (rank ceil(9.5)=10), p50 the 5th */
    uint32_t big[16]; loopstat_ring_t rb; loopstat_ring_init(&rb, big, 16);
    for (uint32_t v = 10; v >= 1; v--) loopstat_ring_push(&rb, v);
    n = loopstat_ring_sorted(&rb, sorted);
    check("n=10 count", n, 10);
    check("n=10 p50", loopstat_pct_sorted(sorted, n, 50), 5);
    check("n=10 p95", loopstat_pct_sorted(sorted, n, 95), 10);
    check("n=10 p90", loopstat_pct_sorted(sorted, n, 90), 9);

    /* 100 samples 100..1 -> p50 = 50, p95 = 95, p100 = 100 */
    uint32_t hb[256]; loopstat_ring_t rh; loopstat_ring_init(&rh, hb, 256);
    for (uint32_t v = 100; v >= 1; v--) loopstat_ring_push(&rh, v);
    n = loopstat_ring_sorted(&rh, sorted);
    check("n=100 p50", loopstat_pct_sorted(sorted, n, 50), 50);
    check("n=100 p95", loopstat_pct_sorted(sorted, n, 95), 95);
    check("n=100 p100", loopstat_pct_sorted(sorted, n, 100), 100);

    printf("loopstat_pct_test: %d passed, %d failed\n", pass, fail);
    return fail ? 1 : 0;
}
```

`examples/display/acid_box/tests/run.sh`:

```sh
#!/bin/sh
# Host test for acid_box's ACIDBOX_LOOPSTAT percentile helper (NEW-33). No
# toolchain, no board, no QEMU: a p95 index is exactly where an off-by-one
# hides, and nothing on the bench or in the QEMU gate can see it.
# Demonstrated RED (2026-09-04): flooring the rank ((p*n)/100 instead of
# ceil) fails "n=10 p95" by name -- the 9th smallest instead of the 10th.
set -e
DIR=$(cd "$(dirname "$0")" && pwd); OUT=$(mktemp -d); trap 'rm -rf "$OUT"' EXIT
CC=${CC:-cc}
$CC -std=c99 -Wall -Wextra -Werror -I"$DIR/.." "$DIR/loopstat_pct_test.c" -o "$OUT/loopstat_pct_test"
"$OUT/loopstat_pct_test"
echo "ACIDBOX-HOST-TESTS: PASS"
```

- [ ] **Step 2: Run it to verify it fails (header missing)**

```bash
chmod +x examples/display/acid_box/tests/run.sh
examples/display/acid_box/tests/run.sh
```
Expected: compile error `loopstat_pct.h: No such file or directory`, exit non-zero.

- [ ] **Step 3: Write the header**

`examples/display/acid_box/loopstat_pct.h`:

```c
/* loopstat_pct.h -- a ring of uint32 samples and a nearest-rank percentile
 * over it.  Pure C, header-only, no allocation: the caller owns the storage.
 * Used by acid_box's ACIDBOX_LOOPSTAT instrument (NEW-33) for the frame
 * interval and touch-to-frame latency distributions; host-tested in tests/.
 * Copyright (c) 2026 Nicholas Newdigate.  SPDX-License-Identifier: MIT */
#pragma once
#include <stdint.h>

typedef struct {
    uint32_t *buf;      /* caller-owned storage, `cap` entries */
    uint32_t  cap;
    uint32_t  head;     /* next write index */
    uint32_t  count;    /* valid entries, <= cap */
} loopstat_ring_t;

static inline void loopstat_ring_init(loopstat_ring_t *r, uint32_t *buf, uint32_t cap)
{
    r->buf = buf; r->cap = cap; r->head = 0; r->count = 0;
}

static inline void loopstat_ring_reset(loopstat_ring_t *r) { r->head = 0; r->count = 0; }

/* Overwrites the oldest entry once full: the ring holds the `cap` most recent. */
static inline void loopstat_ring_push(loopstat_ring_t *r, uint32_t v)
{
    r->buf[r->head] = v;
    r->head = (r->head + 1 == r->cap) ? 0 : r->head + 1;
    if (r->count < r->cap) r->count++;
}

/* Copy the ring's valid entries into out[] (>= cap entries) sorted ascending
 * and return how many.  Every valid entry lives in buf[0..count): before the
 * first wrap head == count, and after it all cap entries are valid -- so the
 * write order is irrelevant to a sort.  Insertion sort: n <= 256, once/sec. */
static inline uint32_t loopstat_ring_sorted(const loopstat_ring_t *r, uint32_t *out)
{
    const uint32_t n = r->count;
    for (uint32_t i = 0; i < n; i++) out[i] = r->buf[i];
    for (uint32_t i = 1; i < n; i++) {
        const uint32_t v = out[i];
        int32_t j = (int32_t)i - 1;
        while (j >= 0 && out[j] > v) { out[j + 1] = out[j]; j--; }
        out[j + 1] = v;
    }
    return n;
}

/* Nearest-rank percentile of a sorted array: the value at 1-based rank
 * ceil(p * n / 100), clamped to [1, n].  n == 0 returns 0.
 * p50 of {1,2,3,4} is 2; p95 of 100 samples is the 95th smallest; p100 is
 * the max.  Integer ceil: (p*n + 99) / 100. */
static inline uint32_t loopstat_pct_sorted(const uint32_t *sorted, uint32_t n, uint32_t p)
{
    if (n == 0) return 0;
    uint32_t rank = (p * n + 99u) / 100u;
    if (rank < 1) rank = 1;
    if (rank > n) rank = n;
    return sorted[rank - 1];
}
```

- [ ] **Step 4: Run the test, verify it passes**

```bash
examples/display/acid_box/tests/run.sh
```
Expected: 22 `PASS:` lines, `loopstat_pct_test: 22 passed, 0 failed`, `ACIDBOX-HOST-TESTS: PASS`. (The plan first said 24; a run says 22 -- the run is the count.)

- [ ] **Step 5: Demonstrate RED, then restore**

Edit `loopstat_pct_sorted` to floor: `uint32_t rank = (p * n) / 100u;` → run → expected `FAIL: n=4 p95 got 3 want 4` and `FAIL: n=10 p95 got 9 want 10` (measured). Restore the `+ 99u`. Run again → all PASS. Record the demonstrated-RED line in `run.sh`'s header (already written above).

- [ ] **Step 6: Commit**

```bash
git add examples/display/acid_box/loopstat_pct.h examples/display/acid_box/tests/
git commit -m "feat(acid_box): loopstat_pct.h -- ring + nearest-rank percentile, host-tested (NEW-33)

The ACIDBOX_LOOPSTAT instrument's one pure piece: the frame-interval and
touch-to-frame latency distributions need p50/p95/max over a ring, and a
percentile index is where an off-by-one hides. 22 checks; demonstrated RED
by flooring the rank (n=4 p95 and n=10 p95 fail by name).

Co-Authored-By: Claude Fable 5.1 <noreply@anthropic.com>"
```

---

### Task 2: `AudioOutputBluetooth` accumulators (`encodeUs`, `drainUs`, `txBytes`)

**Files:**
- Modify: `examples/audio/bt_tone_test/AudioOutputBluetooth.h` (public accessors near `pcmDrops()`, three members near `m_pcmDrops`)
- Modify: `examples/audio/bt_tone_test/AudioOutputBluetooth.cpp` (`sendThunk`, `poll`)

No host test exists for this Arduino-bound node; the check is that `bt_tone_test`'s two QEMU gates stay green (the counters are additive) and that acid_box's `loopstat` line reads them (Task 4).

- [ ] **Step 1: Header — accessors and members**

In `AudioOutputBluetooth.h`, after the `pcmDrops()` line add:

```cpp
    // NEW-33 attribution: cumulative us spent in poll()'s SBC encode loop and in
    // its drain, and bytes handed to L2cap::send (each + the 9-byte ACL header the
    // transport prepends).  Deltas per second let a consumer split the BT cost by
    // stage and test "is the send wire-bound" (bytes x 3.33 us at 3 Mbaud).
    uint32_t encodeUs() const { return m_encodeUs; }
    uint32_t drainUs()  const { return m_drainUs; }
    uint32_t txBytes()  const { return m_txBytes; }
```

After the `uint32_t m_pcmDrops = 0;` member add:

```cpp
    uint32_t m_encodeUs = 0, m_drainUs = 0, m_txBytes = 0;   // NEW-33 attribution
```

- [ ] **Step 2: Implementation — stamp the encode loop and the drain, count bytes**

In `AudioOutputBluetooth.cpp`, replace `sendThunk` with:

```cpp
bool AudioOutputBluetooth::sendThunk(void *ctx, const uint8_t *pkt, uint16_t len) {
    AudioOutputBluetooth *o = (AudioOutputBluetooth *)ctx;
    const bool ok = o->m_l2->send(o->m_cid, pkt, len);   // L2cap::send returns false when out of credit/txq
    if (ok) o->m_txBytes += 9u + len;                    // + the ACL header L2cap::service() prepends
    return ok;
}
```

In `poll()`, replace the encode loop and the drain block with:

```cpp
    uint16_t head = m_pcmHead;
    if (m_pcmTail != head) {
        const uint32_t t0 = micros();
        while (m_pcmTail != head) {
            uint16_t tail = m_pcmTail;
            uint8_t frame[128];
            uint16_t n = m_sbc.encode(m_pcm[tail].l, m_pcm[tail].r, frame);
            m_pk.push(frame, n); m_blocks++;
            uint16_t next = tail + 1; if (next >= PCM_RING) next = 0;
            m_pcmTail = next;                              // release the slot AFTER the encode (SPSC)
        }
        m_encodeUs += micros() - t0;
    }
    // Drain when a full packet's worth of frames is ready (so packets are fuller and use
    // fewer ACL credits), or when the flush deadline passes (bounds latency and empties a
    // backlog).  drain() itself still batches up to framesPerPacket and, once triggered,
    // sends every full packet it can -- so a credit-stall backlog drains fast on recovery.
    if (m_pk.pending() >= m_pk.framesPerPacket() || (int32_t)(now - (m_lastDrainUs + m_flushUs)) >= 0) {
        const uint32_t t0 = micros();
        m_pk.drain(sendThunk, this);
        m_drainUs += micros() - t0;
        m_lastDrainUs = now;
    }
```

(The comment block above the encode loop stays as it is.)

- [ ] **Step 3: Build bt_tone_test and run its two gates**

```bash
cd examples/audio/bt_tone_test && cmake --build build 2>&1 | tail -2 && ./run_qemu.sh | tail -3 && ./run_qemu_media.sh | tail -3
```
Expected: both end in `PASS`. (`[media]` is attach-timing sensitive; re-run idle if it flakes, per CLAUDE.md.)

- [ ] **Step 4: Commit**

```bash
git add examples/audio/bt_tone_test/AudioOutputBluetooth.h examples/audio/bt_tone_test/AudioOutputBluetooth.cpp
git commit -m "feat(bt_tone_test): AudioOutputBluetooth encodeUs/drainUs/txBytes accumulators (NEW-33)

Two micros() reads per poll() and a byte count per accepted packet, so a
consumer can split the per-loop BT cost into encode vs drain and test whether
the send is wire-bound. Additive; both bt_tone_test gates green.

Co-Authored-By: Claude Fable 5.1 <noreply@anthropic.com>"
```

---

### Task 3: `ACIDBOX_LOOPSTAT` — CMake option and the instrument in acid_box

**Files:**
- Modify: `examples/display/acid_box/CMakeLists.txt` (after the `import_evkb_library(TouchPanel gt911)` line, before the `# --- Bluetooth A2DP audio output` block)
- Modify: `examples/display/acid_box/acid_box.cpp`: includes (after `#include "synthui_step.h"`), a new instrument block (after the `#if defined(M2_BT_OUT) … #endif` graph block that ends with `static bool s_btBegun = false;`), `build_ui()` (title + eight `mkknob` lines), `setup()` (after `lvgl_mipi_panel_create_db`, and the `lvgl_gt911_indev_create` call), `loop()`.

- [ ] **Step 1: CMake option**

Insert after `import_evkb_library(TouchPanel gt911) # controller chosen by the importer`:

```cmake
# --- Loop / frame / touch instrumentation (opt-in, bench) ------------------
# OFF by default: nothing compiles in and the loadable image is byte-identical
# (Task 4 of the NEW-33 plan diffs the objcopy -O binary; the ELF's DWARF moves
# with every edited line), so the gate, both goldens and the vacuity fixture
# are untouched.  ON prints loopstat/framestat/touchstat once a second and makes
# the "ACID BOX" title a tap-toggle for a synthetic knob wiggle -- see
# docs/superpowers/specs/2026-09-04-acid-box-bt-ui-responsiveness-design.md §1.
# Independent of M2_BT_OUT on purpose: the same instrument measures both builds.
option(ACIDBOX_LOOPSTAT "Per-slot loop timing, frame and touch latency lines (bench)" OFF)
if(ACIDBOX_LOOPSTAT)
    add_definitions(-DACIDBOX_LOOPSTAT=1)
endif()
```

- [ ] **Step 2: Includes**

After `#include "synthui_step.h"` add:

```cpp
#if defined(ACIDBOX_LOOPSTAT)
#include "loopstat_pct.h"
#endif
```

- [ ] **Step 3: The instrument block**

Insert immediately after the line `static bool s_btBegun = false;` and its closing `#endif` (i.e. after the `M2_BT_OUT` graph block):

```cpp
#if defined(ACIDBOX_LOOPSTAT)
/* --- ACIDBOX_LOOPSTAT: where does the main loop spend its time? ---------- *
 * Bench instrument for NEW-33 (spec 2026-09-04-acid-box-bt-ui-responsiveness-
 * design.md §1).  Never in the gated build: -DACIDBOX_LOOPSTAT=1 is a CMake
 * option that defaults OFF, and OFF leaves the loadable image byte-identical.
 *
 * Three lines, once a second, beside bt_hb:
 *   loopstat  loops= max_us= yield= svc= poll= enc= drain= txb= print= lvgl= probe= wiggle=
 *   framestat frames= med_us= max_us= flips=+ wait_us=+
 *   touchstat n= p50_us= p95_us= max_us=          (only when samples arrived)
 *
 * ★ EVERYTHING HERE LIVES IN FLASH.  The M2_BT_OUT bench build has ~1 KB of
 * ITCM left (.text.itcm 0x3FBD0 of 0x40000), so every function below carries
 * LOOPSTAT_FN: section .progmem.loopstat, collected by the core's *(.progmem*)
 * rule into .text.progmem, which is XIP and already AX.  Only the micros()
 * laps in loop() are inline.  A print that runs once a second does not need
 * ITCM; a lap that runs per iteration is a handful of instructions.
 *
 * ★ The slots are laps, not nested timers: each LS_LAP charges the time since
 * the previous lap to one slot, so the sum of the slots IS the iteration.  The
 * summary's own print time lands in the NEXT window's `print` (the counters
 * are reset inside the summary, before its lap) -- honest, one window late. */
#define LOOPSTAT_FN __attribute__((section(".progmem.loopstat"), noinline))

enum { LS_YIELD, LS_SVC, LS_POLL, LS_PRINT, LS_LVGL, LS_PROBE, LS_SLOTS };
static uint32_t ls_slotUs[LS_SLOTS];        /* cumulative us per slot, this window */
static uint32_t ls_loops = 0, ls_maxUs = 0;
static uint32_t ls_windowMs = 0;            /* millis() at the last summary */
static inline __attribute__((always_inline)) uint32_t ls_lap(int slot, uint32_t t0)
{
    const uint32_t t = micros();
    ls_slotUs[slot] += t - t0;
    return t;
}
#define LS_LAP(slot) (ls_t = ls_lap(slot, ls_t))

/* frames: LVGL display events.  A frame counts at REFR_READY only if
 * RENDER_READY fired since REFR_START (an empty refresh cycle is not a frame). */
static uint32_t        ls_frameBuf[64];
static loopstat_ring_t ls_frameRing;
static uint32_t ls_frames = 0;              /* rendered frames this window */
static uint32_t ls_lastFrameUs = 0;         /* REFR_READY of the previous rendered frame, 0 = none */
static uint32_t ls_refrStartUs = 0;
static bool     ls_rendered = false;
static uint32_t ls_flips0 = 0, ls_wait0 = 0;

/* touch: the age, at presentation, of the OLDEST input change a frame carries.
 * The stamp is taken when LVGL's indev read returns a changed (state, x, y)
 * and no stamp is pending; it closes at the first rendered REFR_READY whose
 * REFR_START came AFTER the stamp (so the frame's render began after the
 * input was processed and its invalidation queued).  Ring of the 256 most
 * recent samples; the line reports over the ring, so the last touchstat of a
 * drag is the drag's distribution. */
static uint32_t        ls_touchBuf[256];
static loopstat_ring_t ls_touchRing;
static uint32_t ls_touchN = 0, ls_touchNew = 0;
static uint32_t ls_touchStampUs = 0;
static bool     ls_touchPending = false;
static lv_indev_read_cb_t ls_origRead = nullptr;
static lv_indev_state_t   ls_prevState = LV_INDEV_STATE_RELEASED;
static lv_point_t         ls_prevPoint = {0, 0};

LOOPSTAT_FN static void ls_refr_cb(lv_event_t *e)
{
    switch (lv_event_get_code(e)) {
    case LV_EVENT_REFR_START:   ls_refrStartUs = micros(); ls_rendered = false; break;
    case LV_EVENT_RENDER_READY: ls_rendered = true; break;
    case LV_EVENT_REFR_READY: {
        if (!ls_rendered) break;
        const uint32_t now = micros();
        ls_frames++;
        if (ls_lastFrameUs) loopstat_ring_push(&ls_frameRing, now - ls_lastFrameUs);
        ls_lastFrameUs = now;
        if (ls_touchPending && (int32_t)(ls_refrStartUs - ls_touchStampUs) >= 0) {
            loopstat_ring_push(&ls_touchRing, now - ls_touchStampUs);
            ls_touchPending = false;
            ls_touchN++; ls_touchNew++;
        }
        break;
    }
    default: break;
    }
}

LOOPSTAT_FN static void ls_read_cb(lv_indev_t *indev, lv_indev_data_t *data)
{
    ls_origRead(indev, data);
    if (data->state != ls_prevState ||
        data->point.x != ls_prevPoint.x || data->point.y != ls_prevPoint.y) {
        ls_prevState = data->state; ls_prevPoint = data->point;
        if (!ls_touchPending) { ls_touchStampUs = micros(); ls_touchPending = true; }
    }
}

/* wiggle: a 15 ms LVGL timer sweeping all eight sound knobs through a triangle
 * over the full ±140° bounded range -- 100 steps per half-sweep (1.5 s), knob k
 * offset by 12 steps so the rotors are never in phase.  set_angle ONLY: the
 * widget sends VALUE_CHANGED from its input path (synthui_rotary_knob.cpp),
 * never from set_angle, so the synth parameters do not move -- the panel
 * animates flat-out while the sound is unchanged.  Boot/current angles are
 * saved at wiggle-on and restored at wiggle-off so the picture matches the
 * engine again afterwards. */
static lv_obj_t   *ls_knob[8];
static int         ls_nKnob = 0;
static float       ls_saved[8];
static lv_timer_t *ls_wiggleTimer = nullptr;
static uint32_t    ls_wiggleStep = 0;
static bool        ls_wiggle = false;
#define LS_KNOB(x) (ls_knob[ls_nKnob++] = (x))

LOOPSTAT_FN static float ls_tri(uint32_t step)
{
    const uint32_t s = step % 200u;
    const float u = (s < 100u) ? (float)s / 100.0f : (float)(200u - s) / 100.0f;
    return -140.0f + 280.0f * u;
}
LOOPSTAT_FN static void ls_wiggle_cb(lv_timer_t *t)
{
    (void)t;
    ls_wiggleStep++;
    for (int k = 0; k < ls_nKnob; k++)
        synthui_rotary_knob_set_angle(ls_knob[k], ls_tri(ls_wiggleStep + 12u * (uint32_t)k));
}
LOOPSTAT_FN static void ls_title_cb(lv_event_t *e)
{
    (void)e;
    if (!ls_wiggle) {
        for (int k = 0; k < ls_nKnob; k++) ls_saved[k] = synthui_rotary_knob_get_angle(ls_knob[k]);
        ls_wiggleTimer = lv_timer_create(ls_wiggle_cb, 15, NULL);
        ls_wiggle = true;
    } else {
        lv_timer_delete(ls_wiggleTimer); ls_wiggleTimer = nullptr;
        for (int k = 0; k < ls_nKnob; k++) synthui_rotary_knob_set_angle(ls_knob[k], ls_saved[k]);
        ls_wiggle = false;
    }
    CONSOLE.printf("wiggle=%d\n", ls_wiggle ? 1 : 0);
}
LOOPSTAT_FN static void ls_attach_title(lv_obj_t *title)
{
    lv_obj_add_flag(title, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_ext_click_area(title, 24);          /* a finger-sized target around the small label */
    lv_obj_add_event_cb(title, ls_title_cb, LV_EVENT_CLICKED, NULL);
}
LOOPSTAT_FN static void ls_attach_display(lv_display_t *disp)
{
    loopstat_ring_init(&ls_frameRing, ls_frameBuf, 64);
    loopstat_ring_init(&ls_touchRing, ls_touchBuf, 256);
    lv_display_add_event_cb(disp, ls_refr_cb, LV_EVENT_REFR_START, NULL);
    lv_display_add_event_cb(disp, ls_refr_cb, LV_EVENT_RENDER_READY, NULL);
    lv_display_add_event_cb(disp, ls_refr_cb, LV_EVENT_REFR_READY, NULL);
    ls_flips0 = lvgl_mipi_panel_flips();
    ls_wait0  = lvgl_mipi_panel_wait_us();
    ls_windowMs = millis();
}
LOOPSTAT_FN static void ls_attach_touch(lv_indev_t *indev)
{
    ls_origRead = lv_indev_get_read_cb(indev);
    lv_indev_set_read_cb(indev, ls_read_cb);
}

LOOPSTAT_FN static void ls_summary(void)
{
    const uint32_t now = millis();
    if (now - ls_windowMs < 1000u) return;
    ls_windowMs = now;

    uint32_t encUs = 0, drainUs = 0, txb = 0;
#if defined(M2_BT_OUT)
    static uint32_t enc0 = 0, drn0 = 0, txb0 = 0;
    const uint32_t enc1 = btout.encodeUs(), drn1 = btout.drainUs(), txb1 = btout.txBytes();
    encUs = enc1 - enc0; drainUs = drn1 - drn0; txb = txb1 - txb0;
    enc0 = enc1; drn0 = drn1; txb0 = txb1;
#endif
    CONSOLE.printf("loopstat loops=%lu max_us=%lu yield=%lu svc=%lu poll=%lu enc=%lu drain=%lu"
                   " txb=%lu print=%lu lvgl=%lu probe=%lu wiggle=%d\n",
                   (unsigned long)ls_loops, (unsigned long)ls_maxUs,
                   (unsigned long)ls_slotUs[LS_YIELD], (unsigned long)ls_slotUs[LS_SVC],
                   (unsigned long)ls_slotUs[LS_POLL], (unsigned long)encUs, (unsigned long)drainUs,
                   (unsigned long)txb, (unsigned long)ls_slotUs[LS_PRINT],
                   (unsigned long)ls_slotUs[LS_LVGL], (unsigned long)ls_slotUs[LS_PROBE],
                   ls_wiggle ? 1 : 0);

    uint32_t sorted[256];
    uint32_t n = loopstat_ring_sorted(&ls_frameRing, sorted);
    const uint32_t flips = lvgl_mipi_panel_flips(), wait = lvgl_mipi_panel_wait_us();
    CONSOLE.printf("framestat frames=%lu med_us=%lu max_us=%lu flips=+%lu wait_us=+%lu\n",
                   (unsigned long)ls_frames,
                   (unsigned long)loopstat_pct_sorted(sorted, n, 50),
                   (unsigned long)loopstat_pct_sorted(sorted, n, 100),
                   (unsigned long)(flips - ls_flips0), (unsigned long)(wait - ls_wait0));
    ls_flips0 = flips; ls_wait0 = wait;
    ls_frames = 0; loopstat_ring_reset(&ls_frameRing);

    if (ls_touchNew) {
        n = loopstat_ring_sorted(&ls_touchRing, sorted);
        CONSOLE.printf("touchstat n=%lu p50_us=%lu p95_us=%lu max_us=%lu\n",
                       (unsigned long)ls_touchN,
                       (unsigned long)loopstat_pct_sorted(sorted, n, 50),
                       (unsigned long)loopstat_pct_sorted(sorted, n, 95),
                       (unsigned long)loopstat_pct_sorted(sorted, n, 100));
        ls_touchNew = 0;
    }
    memset(ls_slotUs, 0, sizeof ls_slotUs);
    ls_loops = 0; ls_maxUs = 0;
}
#else
#define LS_LAP(slot) ((void)0)
#define LS_KNOB(x)   (x)
#endif /* ACIDBOX_LOOPSTAT */
```

- [ ] **Step 4: `build_ui()` — the title and the eight knobs**

Replace the title creation lines

```cpp
    lv_obj_t *title = lv_label_create(scr);
    lv_label_set_text(title, "ACID BOX");
    lv_obj_set_style_text_color(title, lv_color_hex(0xFFFFFF), LV_PART_MAIN);
    lv_obj_set_pos(title, 15, 24);
```

with the same four lines followed by:

```cpp
#if defined(ACIDBOX_LOOPSTAT)
    ls_attach_title(title);        /* tap = synthetic knob wiggle on/off (bench) */
#endif
```

Wrap each of the eight `mkknob(...)` statements in `LS_KNOB(...)`, e.g.:

```cpp
    LS_KNOB(mkknob(scr, 0, 0, "CUTOFF",  logf(800.0f / 20.0f) / logf(12000.0f / 20.0f), cbCut));
    LS_KNOB(mkknob(scr, 1, 0, "RESO",    0.55f, cbRes));
    LS_KNOB(mkknob(scr, 2, 0, "ENV MOD", 0.60f, cbEnv));
    LS_KNOB(mkknob(scr, 3, 0, "DECAY",   logf(0.28f / 0.03f) / logf(2.0f / 0.03f), cbDec));
    LS_KNOB(mkknob(scr, 0, 1, "ACCENT",  0.70f, cbAcc));
    LS_KNOB(mkknob(scr, 1, 1, "DIST",    0.15f, cbDst));
    LS_KNOB(mkknob(scr, 2, 1, "SUB",     0.20f, cbSub));
    LS_KNOB(mkknob(scr, 3, 1, "SLIDE T", logf(0.06f / 0.01f) / logf(0.3f / 0.01f), cbSld));
```

(With LOOPSTAT off `LS_KNOB(x)` is `(x)`: the same expression statement, so the default ELF does not move.)

- [ ] **Step 5: `setup()` — attach display events and the touch wrapper**

After `lv_display_t *disp = lvgl_mipi_panel_create_db(Display);` and its `diag_mark();` line add:

```cpp
#if defined(ACIDBOX_LOOPSTAT)
    ls_attach_display(disp);
#endif
```

Replace `        lvgl_gt911_indev_create(disp, touch);` with:

```cpp
        lv_indev_t *indev = lvgl_gt911_indev_create(disp, touch);
        (void)indev;
#if defined(ACIDBOX_LOOPSTAT)
        ls_attach_touch(indev);
#endif
```

- [ ] **Step 6: `loop()` — the laps**

Replace the whole `loop()` with:

```cpp
void loop()
{
#if defined(ACIDBOX_LOOPSTAT)
    const uint32_t ls_iter0 = micros();
    uint32_t ls_t = ls_iter0;
#endif
#if defined(M2_BT_OUT)
    yield();                                   // drives the yield-attached HciPump (parses NCP/credits)
    LS_LAP(LS_YIELD);
    src.service();                             // SdpServer + L2cap::service() (the ACL UART write) + Avdtp
    LS_LAP(LS_SVC);
    if (s_btBegun) btout.poll();               // SBC encode of the buffered PCM + drain into L2cap's queue
    LS_LAP(LS_POLL);
    {
        static uint32_t lastTry = 0;
        if (!s_btBegun && (lastTry == 0 || millis() - lastTry >= 5000)) {
            lastTry = millis();
#if defined(M2_BT_TARGET_NAME)
            A2dpSource::Result rr = src.connect(M2_BT_TARGET_NAME, s_aclNum, nowMs, idleMs);
#else
            A2dpSource::Result rr = src.connect(nullptr, s_aclNum, nowMs, idleMs);
#endif
            CONSOLE.print("a2dp_try="); CONSOLE.println(A2dpSource::resultName(rr));
            if (rr == A2dpSource::OK) {
                btout.setSelfClock(false);     // the I2S SAI ISR clocks the graph; poll() only drains
                btout.begin(src);
                s_btBegun = true;
                CONSOLE.print("bt_streaming frames_per_pkt="); CONSOLE.print(btout.framesPerPacket());
                CONSOLE.print(" media_mtu="); CONSOLE.println(src.mediaMtu());
            }
        }
    }
    {
        static uint32_t last = 0;
        if (s_btBegun && millis() - last >= 1000) {
            last = millis();
            CONSOLE.print("bt_hb blocks="); CONSOLE.print(btout.blocks());
            CONSOLE.print(" packets="); CONSOLE.print(btout.packets());
            CONSOLE.print(" drops="); CONSOLE.print(btout.drops());
            CONSOLE.print(" pcmdrops="); CONSOLE.print(btout.pcmDrops());  // PCM-ring overflow = loop too slow to encode
            CONSOLE.print(" hw="); CONSOLE.print(btout.queueHighWater());
            CONSOLE.print(" audiomax="); CONSOLE.println(AudioMemoryUsageMax());
        }
    }
    LS_LAP(LS_PRINT);                          // the connect attempt (once, ~30 s) and bt_hb land here
#endif
#if defined(ACIDBOX_LOOPSTAT)
    ls_summary();
    LS_LAP(LS_PRINT);
#endif
#ifdef ACIDBOX_DIAG
    /* Synthetic play, diagnostic build only.  The shipped contract is BOOT
     * SILENT and this must not be allowed to soften that claim -- so it lives
     * behind the definition build/ never sets, and it starts the transport
     * FOUR SECONDS IN, purely so the audio path can be MEASURED over SWD
     * (g_diag_rms) while the VCOM carries nothing. */
    if (!g_diag_played && millis() >= 4000) {
        g_diag_played = millis();
        transport.play();
    }
#endif
    lvgl_rt1176_loop();
    LS_LAP(LS_LVGL);
    audio_probe_poll();
    LS_LAP(LS_PROBE);
#if defined(ACIDBOX_LOOPSTAT)
    {
        const uint32_t it = micros() - ls_iter0;
        if (it > ls_maxUs) ls_maxUs = it;
        ls_loops++;
    }
#endif
}
```

- [ ] **Step 7: Configure and build the two instrumented images**

```bash
cd examples/display/acid_box
cmake -B build-loopstat -DCMAKE_TOOLCHAIN_FILE=../../../toolchain/rt1170-evkb.toolchain.cmake -DACIDBOX_LOOPSTAT=ON 2>&1 | tail -2
cmake --build build-loopstat 2>&1 | grep -E "error|warning: unused|ITCM|overflow|Linking|acid_box.elf" | tail -5
cmake -B build-bench -DACIDBOX_LOOPSTAT=ON 2>&1 | tail -2      # keeps its cached bench config (blob, 3 Mbaud, Shokz, RTS)
cmake --build build-bench 2>&1 | grep -E "error|ITCM|overflow|Linking|acid_box.elf" | tail -5
/Applications/ARM_10/bin/arm-none-eabi-readelf -S build-bench/acid_box.elf | grep -E "\.text\.itcm|\.text\.progmem"
```
Expected: both link. `.text.itcm` of `build-bench` ≤ 0x40000. If the bench link fails with `region ITCM overflowed`, add `.text.loop` to the flash list in the BT linker-script derivation: in `CMakeLists.txt` change the line `*acid_box.cpp.obj(.text.setup .text._GLOBAL__sub*)` to `*acid_box.cpp.obj(.text.setup .text.loop .text._GLOBAL__sub*)` and rebuild.

Check that the instrument really landed in flash:
```bash
/Applications/ARM_10/bin/arm-none-eabi-nm -S --size-sort build-bench/acid_box.elf | grep -E "ls_summary|ls_refr_cb|ls_wiggle_cb|ls_read_cb"
```
Expected: every address starts with `3000…` (flash), none with `0000…` (ITCM).

- [ ] **Step 8: Commit**

```bash
git add examples/display/acid_box/CMakeLists.txt examples/display/acid_box/acid_box.cpp
git commit -m "feat(acid_box): ACIDBOX_LOOPSTAT -- per-slot loop timing, frame + touch latency, title-tap wiggle (NEW-33)

Opt-in (CMake option, default OFF, loadable image byte-identical when off). Prints
loopstat/framestat/touchstat once a second beside bt_hb: micros() laps per
loop() slot (yield / src.service / btout.poll / prints / lvgl / probe), frames
per second + median interval from the LVGL display events, and p50/p95/max
touch-sample-to-presented-frame latency from a wrapped indev read cb. Tapping
the ACID BOX title toggles a 15 ms synthetic sweep of all eight knobs
(set_angle only -- no VALUE_CHANGED, the synth is untouched). Everything but
the laps lives in .progmem.loopstat (flash): the bench build has ~1 KB of ITCM
left.

Co-Authored-By: Claude Fable 5.1 <noreply@anthropic.com>"
```

---

### Task 4: Prove the default build did not move; QEMU checks

**Files:** none modified.

- [ ] **Step 1: Byte-identity of the default (gated) build's LOADABLE IMAGE**

```bash
cd examples/display/acid_box && cmake -B build 2>&1 | tail -1 && cmake --build build 2>&1 | tail -1
/Applications/ARM_10/bin/arm-none-eabi-objcopy -O binary build/acid_box.elf $SCRATCH/acid_box.post.bin
cmp $SCRATCH/acid_box.post.bin $SCRATCH/acid_box.pre.bin && echo "LOADABLE IMAGE IDENTICAL"
```
Expected: `LOADABLE IMAGE IDENTICAL` (295,936 bytes both). The raw ELFs WILL differ in `.debug_*` (the edits shift line numbers) and that is expected; the loadable image is what the gate boots and the golden hashes. The pre-change rebuild was already shown reproducible at the image level, so a difference here can only be this plan's change — find it with `arm-none-eabi-nm -S --size-sort` diffs of the two ELFs before going on.

- [ ] **Step 2: The gate**

```bash
./run_qemu.sh 2>&1 | tail -4
```
Expected: `PASS` with the golden `ACIDBOX_UI_SUM=0x25B30A96` unchanged.

- [ ] **Step 3: The LOOPSTAT image boots in QEMU and prints the lines (not a gate)**

Write `$SCRATCH/ls_qemu.sh`:

```bash
#!/usr/bin/env bash
# One-off: boot the ACIDBOX_LOOPSTAT image in QEMU for ~40 s and show the
# instrument lines.  Timing there is meaningless; this only proves the lines
# print with every field present.  Uses the gate's exact machine/console chain.
set -euo pipefail
EVKB=$HOME/Development/rt1170/evkb
DIR=$EVKB/examples/display/acid_box
. "$EVKB/tools/gate-lib.sh"
OUT=$SCRATCH/ls_qemu.uart; DBG=$SCRATCH/ls_qemu.dbg; rm -f "$OUT" "$DBG"
QRUN_TIMEOUT=40 "$EVKB/tools/qrun" $(gate_qemu_machine) -kernel "$DIR/build-loopstat/acid_box.elf" \
    -display none $(gate_console "$OUT") \
    -global driver=imxrt.gt911,property=touch-script,value="$DIR/touch_script.txt" \
    -d guest_errors -D "$DBG" || true
grep -E "^(loopstat|framestat|touchstat|wiggle)" "$OUT" | head -12
```

```bash
SCRATCH=$SCRATCH bash $SCRATCH/ls_qemu.sh
```
Expected: several `loopstat loops=… wiggle=0` lines with all twelve fields, `framestat frames=…` lines; `touchstat` may appear (the injected gesture) — its presence is not required. `yield=0 svc=0 poll=0 enc=0 drain=0 txb=0` (BT off).

- [ ] **Step 4: Nothing to commit** (Task 3's commit covers it). Note the QEMU line sample in `$SCRATCH/ls_qemu.uart` for the transcript's "instrument sanity" paragraph.

---

### Task 5: Bench baseline A/B (silicon; you at the bench with the Shokz)

**Files:**
- Modify: `examples/display/acid_box/transcript_hw_evkb_bt.txt` (append the NEW-33 baseline section)

Pre-flight (CLAUDE.md flash lessons): no console reader on the VCOM while LinkServer programs; `flash load` → `verify` → detach → reader → SW4. If `load` refuses the `.elf` (`exited with code -11`), load `build-*/acid_box.hex` instead. If `LinkServer` connect hangs, power-cycle the board; if the DAP wedges, replug the DEBUG USB.

- [ ] **Step 1: Build A (BT off + LOOPSTAT) — flash, boot, capture**

```bash
pkill LinkServer; pkill redlinkserv; pkill crt_emu_cm_redlink; sleep 1
cd examples/display/acid_box
LinkServer flash MIMXRT1176:MIMXRT1170-EVKB load build-loopstat/acid_box.elf
LinkServer flash MIMXRT1176:MIMXRT1170-EVKB verify build-loopstat/acid_box.elf
cd ~/Development/rt1170/evkb && python3 tools/rt1170-console.py /dev/cu.usbmodem5DQ2DDHVWO5EI3 115200 | tee $SCRATCH/A_loopstat.txt
``` Press SW4. Expected boot: `ACIDBOX_ENGINE=gpu`, `ACIDBOX_UI_SUM=0x1479CEE8`, `ACIDBOX_DONE`, then `loopstat`/`framestat` lines every second.

Protocol on the panel: tap ▶ (playing), tap the "ACID BOX" title → console `wiggle=1` → wait 20 s → tap the title → `wiggle=0` → drag CUTOFF back and forth for ~15 s (keep the finger on the knob) → stop. Then Ctrl-C the reader.

- [ ] **Step 2: Build B (bench + LOOPSTAT) — flash, boot, capture**

Same commands with `build-bench/acid_box.elf` and `$SCRATCH/B_loopstat.txt`. Wait for `bt_streaming frames_per_pkt=5 media_mtu=895` (~35 s: firmware download, 3 Mbaud, inquiry, SSP, AVDTP), then the same panel protocol, then keep streaming ≥ 2 min reading `bt_hb`. Confirm by ear the Shokz plays the acid line clean throughout.

- [ ] **Step 3: Extract the readings**

```bash
for f in A B; do echo "== $f =="; grep -E "^(loopstat|framestat|touchstat|wiggle|bt_hb|bt_streaming)" $SCRATCH/${f}_loopstat.txt > $SCRATCH/${f}_lines.txt; wc -l $SCRATCH/${f}_lines.txt; done
# wiggle window: framestat lines between wiggle=1 and wiggle=0
awk '/^wiggle=1/{w=1} /^wiggle=0/{w=0} w && /^framestat/' $SCRATCH/A_lines.txt | tail -15
awk '/^wiggle=1/{w=1} /^wiggle=0/{w=0} w && /^framestat/' $SCRATCH/B_lines.txt | tail -15
# the drag: the LAST touchstat line of each capture
grep '^touchstat' $SCRATCH/A_lines.txt | tail -1; grep '^touchstat' $SCRATCH/B_lines.txt | tail -1
# attribution during streaming (B): loopstat with wiggle=1 and with wiggle=0
grep '^loopstat' $SCRATCH/B_lines.txt | tail -20
```

Fill the table (median over the wiggle window's `framestat` lines; `p95_us` from the last `touchstat`):

| number | A | B | B/A | bound |
|---|---|---|---|---|
| `frames`/s under wiggle (median of the window) | | | | ≥ 0.85 |
| `med_us` under wiggle (median of the window) | | | | ≤ 1.15 |
| `touchstat p95_us` (last line of the drag) | | | | ≤ 1.15 |
| B `bt_hb` over ≥ 2 min: `pcmdrops` delta, `drops` | — | | | 0 / 0 |

Attribution (B, streaming): `svc` vs `txb × 3.33 µs − packets × 213 µs` (64 B of ring per packet at 3.33 µs/B); `enc`; `lvgl`; `max_us`. Decision per spec §3: `svc` tracking the wire time → Task 6 (fix 1). `svc` small and `lvgl + enc` explaining the gap → skip to Task 8 with the evidence written down first.

- [ ] **Step 4: Append the baseline to the transcript and commit**

Append to `examples/display/acid_box/transcript_hw_evkb_bt.txt` a section headed
`NEW-33 BASELINE A/B, <date> -- ACIDBOX_LOOPSTAT before any fix` containing: the build lines for A and B, the boot lines, ~10 `loopstat`/`framestat` lines from each wiggle window, the last `touchstat` of each drag, the B `bt_hb` lines bracketing the 2 minutes, the filled table, and two sentences of attribution ("svc=… against a wire time of … → wire-bound / not").

```bash
git add examples/display/acid_box/transcript_hw_evkb_bt.txt
git commit -m "docs(acid_box): NEW-33 baseline A/B on silicon -- loop attribution before any fix

Co-Authored-By: Claude Fable 5.1 <noreply@anthropic.com>"
```

**STOP here and report the table and the attribution to the user before implementing any fix** (the spec's decision rule, and the debugging skill's Iron Law).

---

### Task 6: Fix 1 — credit-bounded TX ring in `HciTransport` (M2Radio) + witness

Only after Task 5's attribution shows the send is wire-bound.

**Files:**
- Modify: `~/Development/M2Radio/hci/HciTransport.h`
- Modify: `~/Development/M2Radio/hci/HciTransport.cpp`
- Modify: `examples/display/acid_box/acid_box.cpp` (witness + `static_assert`, under `M2_BT_OUT`)
- Modify: `evkb.cmake` (M2Radio pin)

- [ ] **Step 1: `HciTransport.h` — `TX_EXTRA` and the buffer**

Replace the class body's `static const size_t RX_EXTRA = 1024;` line with:

```cpp
    static const size_t RX_EXTRA = 1024;
    // TX extension (NEW-33, 2026-09-04).  The core's Serial2 TX ring is 64 B and
    // HardwareSerialIMXRT::write() spins in yield() when it is full, so every
    // ~620 B ACL media packet blocked the caller ~1.9 ms at 3 Mbaud -- measured
    // in acid_box's main loop as `svc` tracking `txb x 3.33 us` (see the NEW-33
    // transcript).  Sized PAST THE ACL CREDIT WINDOW: L2cap writes a packet only
    // while it holds a controller credit, and credits return only after the air
    // transmission, so bytes resident in this ring can never exceed
    // aclNum x (9 + L2cap::MAX_PAYLOAD) = 7 x 709 = 4963 B on the IW416
    // (hci_buffer acl_num=7).  The UART drains at 3 Mbaud, far faster than the
    // air link, so the credits -- not this ring -- are the bottleneck, and
    // write() never blocks.  hci/ cannot see bt/'s MAX_PAYLOAD, so the consumer
    // that knows both pins the bound with a static_assert (acid_box does) and
    // prints the measured ring against the real aclNum at connect.
    static const size_t TX_EXTRA = 5120;
```

and add the member after `uint8_t m_rxExtra[RX_EXTRA];`:

```cpp
    uint8_t m_txExtra[TX_EXTRA];
```

- [ ] **Step 2: `HciTransport.cpp` — attach in `begin()`, detach AFTER the drain in `end()`**

Replace `begin()` and `end()` with:

```cpp
void HciTransport::begin(uint32_t baud) {
    m_port.begin(baud);
    m_port.addMemoryForRead(m_rxExtra, sizeof m_rxExtra);
    m_port.addMemoryForWrite(m_txExtra, sizeof m_txExtra);   // credit-bounded: never fills (header)
}
// Hand the port back its built-in rings BEFORE dropping the extensions: the
// LPUART ISR writes/reads through those pointers, and both extensions die with
// this object.  Harmless for the static instances this library expects, fatal
// for a stack or heap one -- the ISR would keep using freed memory after
// destruction.  ORDER MATTERS FOR TX: addMemoryForWrite() resets head/tail,
// which would DROP bytes still queued (rebaud()'s vendor set-baud command
// sits in this ring when end() runs), so the port is end()ed FIRST -- the
// core's end() waits for the transmitter to drain -- and only then is the TX
// extension released.  RX is released first, as before: nothing is lost
// there, the bytes merely land in the built-in ring.
void HciTransport::end() {
    m_port.addMemoryForRead(nullptr, 0);
    m_port.end();                            // drains TX (waits for transmitting_ to clear)
    m_port.addMemoryForWrite(nullptr, 0);    // AFTER the drain
}
```

- [ ] **Step 3: M2Radio host suites still green, commit M2Radio**

```bash
cd ~/Development/M2Radio && hci/test/run.sh | tail -2 && bt/test/run.sh | tail -2
git add hci/HciTransport.h hci/HciTransport.cpp
git commit -m "feat(hci): HciTransport TX extension (5 KB) -- the ACL write never blocks on Serial2's 64-byte ring

Sized past the ACL credit window (7 x 709 B on the IW416): L2cap only writes
while it holds a credit, so the ring provably cannot fill and write() never
spins. end() releases the TX extension only AFTER the core's drain, so
rebaud()'s in-flight set-baud command is not dropped. Measured cause: acid_box
(NEW-33) main loop, svc tracking txb x 3.33 us at 3 Mbaud.

Co-Authored-By: Claude Fable 5.1 <noreply@anthropic.com>"
git log --oneline -1
```
Expected: `HCI-HOST-TESTS: PASS`-style tails for both suites (HciTransport is not host-compiled; nothing there can regress), one new SHA.

- [ ] **Step 4: acid_box witness + `static_assert`**

In `acid_box.cpp`, inside the `#if defined(M2_BT_OUT)` graph block (right after `static bool s_btBegun = false;`) add:

```cpp
// NEW-33 fix 1: the transport's TX ring must cover the IW416's 7-credit ACL
// window (hci_buffer acl_num=7) so L2cap::service()'s write never spins on the
// core's 64-byte Serial2 ring.  64 is the core's built-in ring (HardwareSerial2.cpp
// tx_buffer2[64]); the runtime line below reads the real total instead.
static_assert(HciTransport::TX_EXTRA + 64u >= 7u * (9u + L2cap::MAX_PAYLOAD),
              "HciTransport::TX_EXTRA must cover 7 x (9 + L2cap::MAX_PAYLOAD) (NEW-33)");
```

In `loop()`'s connect-success branch, after the `bt_streaming` print, add:

```cpp
                {
                    const uint32_t ring = (uint32_t)Serial2.availableForWrite() + 1u;   // idle ring == total capacity
                    const uint32_t need = (uint32_t)s_aclNum * (9u + L2cap::MAX_PAYLOAD);
                    CONSOLE.printf("hci_txring=%lu need=%lu%s\n", (unsigned long)ring, (unsigned long)need,
                                   need > ring ? " WARN" : "");
                }
```

- [ ] **Step 5: Consumer footprint and the six regression gates (against the LOCAL M2Radio)**

```bash
cd examples/audio/bt_tone_test && cmake --build build 2>&1 | tail -1 && /Applications/ARM_10/bin/arm-none-eabi-size build/bt_tone_test.elf
./run_qemu.sh | tail -2 && ./run_qemu_media.sh | tail -2
cd ../../networking/m2_hci_probe && cmake --build build 2>&1 | tail -1 && /Applications/ARM_10/bin/arm-none-eabi-size build/m2_hci_probe.elf
./run_qemu.sh | tail -2 && ./run_qemu_hci.sh | tail -2 && ./run_qemu_baud.sh | tail -2 && ./run_qemu_avdtp.sh | tail -2
cd ../../display/acid_box && cmake --build build-bench 2>&1 | tail -1
```
Expected: `bss` grows by 5120 on both consumers (67840 → 72960 and 35840 → 40960, DTCM has room); all six gates `PASS`; the bench image links.

- [ ] **Step 6: Push M2Radio, bump the pin, fresh-user verify**

```bash
cd ~/Development/M2Radio && git push origin master && git rev-parse HEAD
```
In `evkb.cmake` line 119 replace the M2Radio SHA `a7cbe03733feed5f90205e9f3b338ddcc24957e9` with the new full SHA and append to that line's comment: ` 2026-09-0X: HciTransport TX extension (5 KB, credit-bounded) -- the ACL write no longer spins on Serial2's 64-byte ring (NEW-33 fix 1; acid_box main-loop measurement). Additive; every hci/ consumer gains 5 KB of .bss.`

```bash
cd ~/Development/rt1170/evkb/examples/audio/bt_tone_test
rm -rf $SCRATCH/ff && cmake -B $SCRATCH/ff -DCMAKE_TOOLCHAIN_FILE=../../../toolchain/rt1170-evkb.toolchain.cmake -DEVKB_FORCE_FETCH=ON 2>&1 | grep -iE "clone|M2Radio|Already at" | head -4
cmake --build $SCRATCH/ff 2>&1 | tail -1
mv build build.keep && ln -s $SCRATCH/ff build && ./run_qemu.sh | tail -1; ./run_qemu_media.sh | tail -1; rm build && mv build.keep build
```
Expected: the configure log shows M2Radio cloned at the new SHA; both gates PASS against the fetched-source ELF; `build` restored.

- [ ] **Step 7: Commit evkb**

```bash
cd ~/Development/rt1170/evkb
git add evkb.cmake examples/display/acid_box/acid_box.cpp
git commit -m "build: bump M2Radio pin to <sha> (HciTransport TX extension) + acid_box hci_txring witness (NEW-33 fix 1)

Fresh-user verified: -DEVKB_FORCE_FETCH=ON clones M2Radio @ <sha>, builds
bt_tone_test clean, both its gates pass against the fetched-source ELF. All
four m2_hci_probe gates green locally. acid_box static_asserts the ring
covers 7 x (9 + MAX_PAYLOAD) and prints hci_txring= need= at connect.

Co-Authored-By: Claude Fable 5.1 <noreply@anthropic.com>"
```

---

### Task 7: Re-measure B with fix 1 (silicon)

**Files:**
- Modify: `examples/display/acid_box/transcript_hw_evkb_bt.txt` (append the post-fix section)

- [ ] **Step 1: Rebuild build-bench, flash, run the Task 5 Step 2 protocol** into `$SCRATCH/B2_loopstat.txt`. Expected new boot line: `hci_txring=5184 need=4963` (no `WARN`).

- [ ] **Step 2: Extract as in Task 5 Step 3** (B2 against A). The prediction to check: `svc` collapses to ~memcpy time (tens of µs per packet), `loops`/s rises, `frames`/s and `p95_us` move toward A. `pcmdrops`/`drops` still 0 over ≥ 2 min.

- [ ] **Step 3: Decide**

- All three numbers inside the bound → append the post-fix section to the transcript (same shape as Task 5 Step 4, headed `NEW-33 AFTER FIX 1`), commit, go to Task 9. Task 8 is NOT built.
- A number still outside the bound AND `enc` / `max_us` show the encode burst pushing iterations past vsync slots → append the section, commit, go to Task 8.
- A number outside the bound and NEITHER `svc` nor `enc` explains it (the gap is `lvgl` itself) → append the section, commit, STOP and report: the spec's CM4 escape hatch (new Linear issue), not more fixes here.

```bash
git add examples/display/acid_box/transcript_hw_evkb_bt.txt
git commit -m "docs(acid_box): NEW-33 after fix 1 -- silicon re-measurement

Co-Authored-By: Claude Fable 5.1 <noreply@anthropic.com>"
```

---

### Task 8 (CONDITIONAL — only on Task 7's second outcome): Fix 2 — SBC encode in a pended priority-240 IRQ

**Files:**
- Modify: `examples/audio/bt_tone_test/AudioOutputBluetooth.h`
- Modify: `examples/audio/bt_tone_test/AudioOutputBluetooth.cpp`
- Modify: `examples/display/acid_box/acid_box.cpp` (one call before `btout.begin(src)`)

- [ ] **Step 0: Confirm the spare vector**

```bash
grep -n "Reserved52\|= 52" ~/Development/mcuxsdk-ws/mcuxsdk/devices/*/MIMXRT1176/MIMXRT1176_cm7.h 2>/dev/null | head -3
```
Expected: `Reserved52_IRQn = 52`. If 52 is NOT reserved on this part, pick the first `ReservedNN_IRQn` the header lists and use that number below. The core names nothing at 52 (`core_pins.h`'s enum), and `IRQ_SOFTWARE` (44, really CAN1) is the Audio library's precedent for commandeering an unused vector.

- [ ] **Step 1: Header**

After `void setSelfClock(bool on) { m_selfClock = on; }` add:

```cpp
    // NEW-33 fix 2: encode in a PENDED LOW-PRIORITY IRQ instead of poll().  update()
    // (the audio clock) still only copies the block into the PCM ring; it then pends
    // `irq`, whose handler encodes the backlog and pushes into the packetiser (whose
    // push/drain contract already assumes an ISR producer and a main-loop consumer).
    // poll() then only drains.  Priority must sit BELOW the audio software ISR (208)
    // and any audio-adjacent timer (acid_box's note pump, 224) and above the main
    // loop: 240.  Total CPU is unchanged; the encode preempts the compositor in
    // ~0.5 ms slices instead of landing as one burst before the render.  Not an
    // IntervalTimer (the imxrt1176 PIT period runs ~20x fast, silicon 2026-09-03).
    // Call before begin().  Default (-1) keeps the main-loop encode: bt_tone_test
    // never calls this and is unchanged.
    void setEncodeIrq(int irq, uint8_t priority);
```

After the private `static bool sendThunk(...)` line add:

```cpp
    static AudioOutputBluetooth *s_encIrqSelf;
    static void encodeIsr();
    void encodeBacklog();                    // the SBC encode loop (poll() or the IRQ)
    int m_encIrq = -1;
```

- [ ] **Step 2: Implementation**

In `AudioOutputBluetooth.cpp` add after `bool AudioOutputBluetooth::s_setupDone = false;`:

```cpp
AudioOutputBluetooth *AudioOutputBluetooth::s_encIrqSelf = nullptr;

void AudioOutputBluetooth::setEncodeIrq(int irq, uint8_t priority) {
    s_encIrqSelf = this; m_encIrq = irq;
    attachInterruptVector((IRQ_NUMBER_t)irq, encodeIsr);
    NVIC_SET_PRIORITY(irq, priority);
    NVIC_ENABLE_IRQ(irq);
}
void AudioOutputBluetooth::encodeIsr() { if (s_encIrqSelf) s_encIrqSelf->encodeBacklog(); }

void AudioOutputBluetooth::encodeBacklog() {
    // A snapshot of head bounds the loop so it cannot spin against a producer that
    // keeps filling the ring during a long encode.
    uint16_t head = m_pcmHead;
    if (m_pcmTail == head) return;
    const uint32_t t0 = micros();
    while (m_pcmTail != head) {
        uint16_t tail = m_pcmTail;
        uint8_t frame[128];
        uint16_t n = m_sbc.encode(m_pcm[tail].l, m_pcm[tail].r, frame);
        m_pk.push(frame, n); m_blocks++;
        uint16_t next = tail + 1; if (next >= PCM_RING) next = 0;
        m_pcmTail = next;                              // release the slot AFTER the encode (SPSC)
    }
    m_encodeUs += micros() - t0;
}
```

In `update()`, after `m_pcmHead = next;` add:

```cpp
        if (m_encIrq >= 0) NVIC_SET_PENDING(m_encIrq);   // encode this block at priority 240, not in poll()
```

In `poll()`, replace the `uint16_t head = m_pcmHead; if (m_pcmTail != head) { … }` encode block (Task 2's version) with:

```cpp
    if (m_encIrq < 0) encodeBacklog();
    // else: the pended IRQ encoded each block as it arrived; only the drain is left.
```

- [ ] **Step 3: acid_box opts in**

In `loop()`'s connect-success branch, before `btout.begin(src);`:

```cpp
                btout.setEncodeIrq(52, 240);   // Reserved52_IRQn on the RT1176 CM7 table (checked against the NXP header); below audio (208) and the note pump (224)
```

- [ ] **Step 4: bt_tone_test unchanged, bench image links**

```bash
cd examples/audio/bt_tone_test && cmake --build build 2>&1 | tail -1 && ./run_qemu.sh | tail -1 && ./run_qemu_media.sh | tail -1
cd ../../display/acid_box && cmake --build build-bench 2>&1 | tail -1
```
Expected: both gates PASS (bt_tone_test never calls `setEncodeIrq`); bench links.

- [ ] **Step 5: Re-measure B (Task 7's protocol) into `$SCRATCH/B3_loopstat.txt`**, extract, decide against the bound. Expected shape: `enc` ≈ 0 in `loopstat` (it now runs in the IRQ; `encodeUs()` still accumulates and is printed as `enc` — read it as "IRQ encode time", it is no longer part of `poll`), `max_us` drops, `pcmdrops` stays 0.

- [ ] **Step 6: Commit** (transcript section `NEW-33 AFTER FIX 2` included)

```bash
git add examples/audio/bt_tone_test/AudioOutputBluetooth.h examples/audio/bt_tone_test/AudioOutputBluetooth.cpp examples/display/acid_box/acid_box.cpp examples/display/acid_box/transcript_hw_evkb_bt.txt
git commit -m "feat(bt): AudioOutputBluetooth::setEncodeIrq -- SBC encode in a pended priority-240 IRQ (NEW-33 fix 2)

Co-Authored-By: Claude Fable 5.1 <noreply@anthropic.com>"
```

---

### Task 9: Close-out

**Files:**
- Modify: `CLAUDE.md` (a new `✅ Measured <date>` block at the top of the sweep-history list, plus the NEW-33 lessons)
- Modify: `examples/display/acid_box/transcript_hw_evkb_bt.txt` (header line naming the final state)
- Memory: a new memory file + `MEMORY.md` line

- [ ] **Step 1: Full sweep, audit, vacuity — sequentially, never concurrently** (CLAUDE.md: read `docs/KNOWN-BROKEN-GATES.md` first; one sweep at a time, output captured)

```bash
cd ~/Development/rt1170/evkb
./tools/run-all-qemu-gates.sh -l | tail -1              # expect "(128 gate(s))"
./tools/run-all-qemu-gates.sh 2>&1 | tee $SCRATCH/sweep.txt | tail -5
LICENSE_AUDIT_EVKB=$(pwd) ./tools/license-audit.sh 2>&1 | tail -2
./tools/gate-vacuity.test.sh 2>&1 | tail -2
```
Expected: `gates: 128 passed`, exit 0 (or `127 passed, 1 failed` with the ONE red being `cm4_audio_test`, re-run idle); `LICENSE-AUDIT: PASS`; vacuity all PASS. Any other red is a regression to fix before going on.

- [ ] **Step 2: CLAUDE.md measurement block**

Add, above the current top `✅ **Measured 2026-09-04 (later the same day): 128 gates…` block, a new block in the same voice recording: the sweep result, that NO gate was added (LOOPSTAT is opt-in, the default loadable image diffed byte-identical), the baseline A/B numbers, the attribution that named the fix, fix 1's effect (the `svc` collapse and the final A/B), whether fix 2 was built, and the lessons: the core's `write()` spins in `yield()` on a full 64-byte ring and a per-byte `Print::write` makes a 620 B packet a ~1.9 ms stall; the credit-window argument that sizes the ring; the touch-to-frame stamp closing only on a render that STARTED after the input; and that the instrument had to live in flash because the bench ELF had 1 KB of ITCM left.

- [ ] **Step 3: Memory**

Write `~/.claude/projects/-Users-nicholasnewdigate-Development-rt1170-evkb/memory/new33-acid-box-bt-ui-responsiveness.md` (type: project) with the outcome, the numbers, and the two transferable lessons (the blocking-UART-write class; the flash-resident instrument), and add one line to `MEMORY.md`.

- [ ] **Step 4: Linear + merge**

```bash
git checkout master && git merge --ff-only nicnewdigate/new-33-acid_box-bt-streaming-degrades-ui-responsiveness-investigate && git log --oneline -8
git push origin master
```
Then, via the Linear MCP, set NEW-33 to Done with a closing comment carrying the final A/B table and the fix(es) landed. Commit CLAUDE.md before the merge:

```bash
git add CLAUDE.md
git commit -m "docs: NEW-33 close-out -- acid_box BT UI responsiveness measured and fixed (sweep 128/0/0)

Co-Authored-By: Claude Fable 5.1 <noreply@anthropic.com>"
```

---

## Self-review (done while writing)

- **Spec coverage:** §1 instrument → Tasks 1–4 (lines, wiggle, flash placement, byte-identity, host test); §2 protocol → Task 5; §3 rule + fix 1 → Tasks 5–7 (rule at Task 5 Step 3, fix at Task 6, re-measure at Task 7); §4 fix 2 → Task 8 (conditional, with the CM4 escape at Task 7 Step 3); §5 close-out → Task 9. Testing section → Task 1 (host), Task 4 (byte-identity, gate, QEMU boot), Task 6 Step 5 (six gates), Task 9 (sweep/audit/vacuity).
- **Placeholders:** none; every code step carries the code. The only "if" branches are the spec's own decision rule and the ITCM-overflow fallback, both with the exact edit.
- **Type consistency:** `loopstat_ring_t` / `loopstat_ring_init` / `loopstat_ring_push` / `loopstat_ring_reset` / `loopstat_ring_sorted` / `loopstat_pct_sorted` match between Task 1 and Task 3; `encodeUs()` / `drainUs()` / `txBytes()` match between Task 2 and Task 3; `TX_EXTRA` between Task 6 Steps 1 and 4; `encodeBacklog()` / `setEncodeIrq(int, uint8_t)` between Task 8 Steps 1–3.
