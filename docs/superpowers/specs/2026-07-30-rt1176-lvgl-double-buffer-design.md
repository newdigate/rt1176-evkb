# RT1176 → LVGL double buffering with page flip on vsync (v4: "no tearing") — Design

**Date:** 2026-07-30
**Status:** validated design, ready for an implementation plan
**Fulfils:** the double-buffer/vsync third of the "**v4** — double buffering / page flip on
vsync, PXP-accelerated drawing, XRGB8888" roadmap entry of
`2026-07-27-rt1176-rk055-display-design.md` (§2). The other two thirds are **deliberately
split out** (§2). Builds on v3 (`2026-07-29-rt1176-lvgl-touch-indev-design.md`, complete and
hardware-verified).

---

## 1. Goal

Remove tearing from the LVGL MIPI direct-render path: LVGL renders into a back buffer while
the LCDIFv2 scans the front one, and the two swap **at vsync** via the controller's shadow-load
mechanism. v1 accepted tearing explicitly (`lvgl_mipi_panel.h`: "no back buffer, no vsync
fence"); v3 dragged a widget across live scanout — the worst case — and the operator saw
"very minor flickering". This milestone is the fix v1's header promised.

**Three facts this design stands on, all verified in source:**

1. **LVGL v9 owns cross-buffer coherency.** In double-buffered DIRECT mode,
   `refr_sync_areas()` (`lv_refr.c:647`) copies the previous frame's dirty regions from the
   on-screen buffer into the off-screen one before rendering. The binding supplies two
   buffers and a flip; LVGL does the rest.
2. **The flip mechanism half-exists.** `lcdifv2.cpp` already programs
   `CTRLDESCL5_SHADOW_LOAD_EN` as a one-shot: on silicon the hardware latches the shadowed
   ADDR into the active registers **at the next vsync** and self-clears the bit (RM ch. 48).
   The same file documents that **QEMU stores the bit as plain RW** — a divergence this
   milestone closes (§5) rather than works around.
3. **The wait has first-class LVGL support.** `lv_display_set_flush_wait_cb`
   (`lv_display.h:324`) is invoked in LVGL's own wait loop (`lv_refr.c:1435`) only when the
   next refresh actually needs the buffer — so the flip wait costs nothing in steady state
   (33 ms refresh period vs ≤17 ms flip latency at 58.7 Hz).

---

## 2. Scope

**In scope (v4):**

- Two `MipiDisplay` `soc/lcdifv2` primitives + a second-framebuffer allocator (§3.1).
- **`lvgl_mipi_panel_create_db(DisplayClass&)`** — the double-buffered sibling of the v1
  create, in the same port file (§3.2). The v1 function is **byte-untouched**.
- **`examples/display/lvgl_rk055_flip_test/`** — an animated scene, a QEMU gate that proves
  the panel scanned each buffer in turn (§6), and a hardware pass whose point is an eye on
  fast motion.
- **QEMU LCDIFv2 model: faithful shadow-load** — latch shadow→active at vsync, self-clear
  `SHADOW_LOAD_EN` (§5). Stricter and more faithful; the four existing display gates are
  re-run green against it *before* anything builds on it.

**Out of scope — named, not silently dropped:**

- **PXP-accelerated drawing** and **XRGB8888** — each is its own future milestone with its
  own driving measurement; XRGB8888 additionally re-records every render golden in the tree.
- **The vsync ISR** (`lv_display_flush_ready()` from the LCDIFv2 interrupt) and **migrating
  `lvgl_rk055_touch_test` to double buffering** — both recorded in
  `docs/superpowers/next-session-lvgl-vsync-isr-brainstorm.md`, written alongside this spec.
  v4 polls `INT_STATUS` from thread context, honouring the v2/v3 staging rule: no interrupt
  in a load-bearing path on first silicon contact.
- Any change to the three existing LVGL examples or their goldens.

---

## 3. Architecture

### 3.1 MipiDisplay primitives (panel API untouched)

`soc/lcdifv2` gains:

```cpp
// Allocate a second framebuffer identical in size/alignment to the one
// lcdifv2Begin() allocated (extmem_malloc, 64-byte aligned). nullptr on failure.
uint16_t *lcdifv2AllocAltFramebuffer();

// Program the shadowed CTRLDESCL4 ADDR and pulse SHADOW_LOAD_EN.  The flip
// TAKES EFFECT AT THE NEXT VSYNC, not at the call.
void lcdifv2FlipTo(const uint16_t *fb);

// Vsync event over INT_STATUS (write-1-clear), polled from thread context:
// arm() clears the latched bit; seen() reports whether one has arrived since.
void lcdifv2VsyncArm();
bool lcdifv2VsyncSeen();
```

**Why `INT_STATUS` and not the SHADOW_LOAD_EN self-clear:** the self-clear is the QEMU
divergence §5 fixes — but a wait built on `INT_STATUS` is correct on both silicon and every
model version, while a wait on the self-clear is correct only after the model change. The
robust choice costs nothing.

### 3.2 The binding: `lvgl_mipi_panel_create_db`

Same file as the v1 create (`port/lvgl_mipi_panel.{h,cpp}`), sharing its static_asserts and
diagnostics conventions:

```cpp
lv_display_t *lvgl_mipi_panel_create_db(DisplayClass &display);
uint32_t lvgl_mipi_panel_flips();    // shadow-load pulses issued since create
uint32_t lvgl_mipi_panel_vsyncs();   // vsync events consumed by the wait path
```

- Allocates the alt buffer (asserting success), hands **both** buffers to
  `lv_display_set_buffers(..., LV_DISPLAY_RENDER_MODE_DIRECT)`.
- `flush_cb` (last flush only): `lcdifv2VsyncArm()`, `lcdifv2FlipTo(just_rendered)`,
  mark flip pending, count the flip, `lv_display_flush_ready()`.
- `flush_wait_cb`: if a flip is pending, poll `lcdifv2VsyncSeen()` until it lands (counting
  the vsync), then clear pending. LVGL calls this only when the next refresh needs the
  buffer — the deferred-wait shape of the approved polling design.
- The v1 accessors (`frame_done`, `flushed_px`) keep working for the db path; the rotation
  and geometry preconditions are identical.

**The invariant, stated once:** LVGL must never render into a buffer the panel is still
scanning. It holds because rendering begins only after `flush_wait_cb` returns, and
`flush_wait_cb` returns only after the vsync at which the flip latched.

### 3.3 The flip protocol

```
refresh N:   LVGL syncs dirty areas front→back (refr_sync_areas, LVGL-internal)
             LVGL renders dirty areas into BACK
flush (last): arm vsync · FlipTo(BACK) · pending=1 · flush_ready
             [panel still scanning FRONT until vsync]
vsync:       hardware latches ADDR; BACK becomes the scanned buffer
refresh N+1: flush_wait_cb sees pending, waits VsyncSeen() → roles swapped, repeat
```

---

## 4. The example: `lvgl_rk055_flip_test`

An autonomous animation — a filled box sweeping the full panel width, position advanced one
step per refresh by an `lv_timer` — deliberately the worst case for tearing and deliberately
**golden-free**: the scene is time-driven, so the gate asserts flip *discipline* (§6), never
pixel checksums. Runs a fixed number of refreshes (enough for ≥ 2 full sweeps), prints its
counters, then holds the animation running for the bench eye.

Imports mirror `lvgl_rk055_panel_test` (`import_evkb_lvgl()`, MipiDisplay `soc panels/rk055`,
PXP); no Wire, no TouchPanel — the flip is a display concern and carries no touch dependency.

---

## 5. The QEMU model change: shadow-load becomes faithful

`hw/display/imxrt_lcdifv2.c` already runs a ~60 Hz vsync timer that raises `INT_VSYNC` in
both interrupt domains (`imxrt_lcdifv2.c:180–207`), and `INT_STATUS` is already
write-1-to-clear. The change:

- `SHADOW_LOAD_EN` written 1 → the model records the *pending* shadow ADDR;
- at the next vsync-timer tick, the pending ADDR is latched into the address the scanout/
  panel-tap path reads, and `SHADOW_LOAD_EN` reads back 0 (self-cleared);
- writes to CTRLDESCL4 never reach the scanned address **before** a vsync: the latch copies
  the shadow registers **as they stand at that vsync** — exactly the shadowed behaviour the
  RM describes and `lcdifv2.cpp:211–220` documents.

**What this makes provable:** the virtual HX8394's pixel-checksum tap now sums *whichever
buffer the panel actually scanned at that vsync*. A gate can therefore prove the panel showed
buffer A's frame and then buffer B's — the flip **happening**, not merely being requested.
Without the latch semantics, that assertion is unwritable.

**Regression duty (the v3 Task-4 pattern):** the four existing display gates
(`rk055_panel_test`, `rpi_panel_test`, `lvgl_rk055_panel_test`, `lvgl_rpi_panel_test`) pulse
`SHADOW_LOAD_EN` once at init and rely on the descriptor landing. They are re-run green
against the changed model **before** the binding or example exist. Their init-time pulse now
latches at the first vsync tick (≤ ~17 ms of model time later) — well inside every gate's
settle window, but that is *measured*, not assumed.

---

## 6. Verification

### 6.1 Tokens and assertions (QEMU gate)

```
PANEL_OK
FLIP_A_SUM=0x…  PANEL_A_SUM=0x…  FLIP_AB=MATCH     (frame in buffer A, tap agrees)
FLIP_B_SUM=0x…  PANEL_B_SUM=0x…  FLIP_BA=MATCH     (next frame in buffer B, tap agrees)
REFRESHES=<n>  FLIPS=<n>  VSYNCS=<m>
FLIP_OK
```

- **The pair of MATCH lines is the core claim:** firmware computes the FNV-1a of the buffer
  it just flipped in; the model's panel tap reports what it scanned; they must agree — for
  **two consecutive, different frames** (A's sum ≠ B's sum is itself asserted, or
  alternation would be unfalsifiable — the v3 HOLD/TRAP vacuity discipline).
- `FLIPS == REFRESHES` exactly (one shadow-load pulse per full refresh — a double pulse or a
  missed frame both break it), `VSYNCS ≥ FLIPS` and `VSYNCS > 0`.
- Failure tokens name the frame and both sums.

### 6.2 What each side is allowed to prove

| Claim | QEMU | Hardware |
|---|---|---|
| the panel scanned buffer A, then B | **the tap-sum pairs, via §5's latch** | not directly observable |
| flip discipline (1 flip/refresh, wait correctness) | counters above | same counters on silicon |
| shadow-load latches at real vsync timing | model fiction (60 Hz timer) | **the real 58.7 Hz cadence; first silicon exercise of `INT_STATUS` polling** |
| **tearing is gone** | invisible — no partial-scanout model | **an eye on the sweeping box**; the run's whole purpose |
| LVGL's cross-buffer sync-copy correctness | indirectly (the B-frame tap sum covers regions synced from A) | the same, plus the eye |

Same stated-asymmetry pattern as v1/v2/v3: what QEMU cannot see is written down, not papered
over.

---

## 7. Decomposition

| | Milestone | Gate |
|---|---|---|
| **F1** | **QEMU model: vsync latch + self-clear.** | All four existing display gates re-run green against the new model, before anything depends on it. |
| **F2** | **MipiDisplay primitives.** | Compile + the F3 gate exercises them; no standalone gate (they are three registers deep). |
| **F3** | **Binding + `lvgl_rk055_flip_test` + gate.** | Every §6.1 token green; a deliberate-break run (skip the wait, or flip to the wrong buffer) measured red before green is trusted. |
| **F4** | **Hardware.** | Counters green on silicon; the operator watches the sweep — before/after comparison against v1's single-buffer tearing is the honest demonstration. |
| **F5** | **Wrap.** | `GATES` entry (audit red-first), MipiDisplay + LVGL pushes and pin bumps, sweep expectation **70 → 71**, docs, memory, and the vsync-ISR brief committed. |

---

## 8. Risks

1. **The model change regresses an existing gate** (highest). Mitigated by F1's ordering and
   by the latch landing at the first tick; if a gate's init races the first vsync in a way
   silicon never showed, that is a real finding to document, not to patch over.
2. **`refr_sync_areas` cost.** The CPU dirty-copy runs each frame; on a 1.8 MB buffer a
   full-screen invalidation copies 1.8 MB/frame. Accepted unmeasured for v4 (the animation
   invalidates far less); if it ever hurts, that is the PXP milestone's driving number.
3. **Two buffers of extmem** (~3.7 MB total) — trivial against SDRAM, but the allocator must
   fail loudly, not fall back to single-buffer silently.
4. **`flush_wait_cb` semantics drift** across LVGL bumps — the call site is pinned by
   citation (`lv_refr.c:1435`) and the gate's `FLIPS == REFRESHES` catches a behavioural
   change immediately.

---

## 9. Licence firewall

All new code MIT in its home repos (MipiDisplay, LVGL port, evkb example); the QEMU model
change is GPL-2.0-or-later inside qemu2, as all qemu2 work is — nothing from it is compiled
into firmware. RM facts cited as facts. F5's `GATES` entry keeps the audit's drift check
honest.

---

## 10. Open questions for the plan stage

1. Exact refresh count and box geometry in the flip test (enough refreshes for ≥ 2 sweeps;
   box sized so consecutive frames always differ — the §6.1 vacuity guard depends on it).
2. Whether `lcdifv2VsyncSeen()` should time out and report, or spin forever and rely on the
   gate's harness timeout (leaning: bounded spin with a loud token — a dead vsync should
   name itself).
3. Whether the panel tap needs a per-frame sum-latch interface in the model (sum-at-vsync
   snapshots) or the existing continuous tap suffices once scanout follows the latched
   address — settle by reading `imxrt_hx8394.c` before writing the gate.
