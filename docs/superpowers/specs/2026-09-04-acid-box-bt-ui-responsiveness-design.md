# acid_box BT streaming vs UI responsiveness (NEW-33) — Design

**Date:** 2026-09-04
**Issue:** NEW-33 — "acid_box: BT streaming degrades UI responsiveness —
investigate & improve". Follow-up to the NEW-9 capstone
(`display/acid_box -DM2_BT_OUT=ON`, spec `2026-09-04-acid-box-bluetooth-output-design.md`).
**Status:** Design, approved in brainstorming; pending spec review.

## Problem

With `M2_BT_OUT=ON`, acid_box streams the acid synth to a Shokz OpenMove over
A2DP with zero audio drops (`pcmdrops=0`, `drops=0`, encode at real time), but
the SynthUI is noticeably less responsive than the default (BT-off) build.
Nothing measures that today: the heartbeat carries audio counters only.

## Goal

1. **Measure** where the BT build's main loop spends its time, with the same
   instrument in the BT-off and BT-on builds, on silicon.
2. **Fix** what the attribution names, so that the BT build's UI is within
   **15 %** of the default build on two numbers (below), with `pcmdrops=0` /
   `drops=0` preserved over ≥ 2 minutes of streaming to the real headset.
   "Within 15 %" is one-sided, the direction that matters: B's frames/s
   ≥ 0.85 × A's, B's median frame interval ≤ 1.15 × A's, and B's p95
   touch-to-frame latency ≤ 1.15 × A's. A B that beats A on any number passes
   that number.

## Non-goals

- No change to the default build's ELF, its QEMU gate, golden or fixture.
- No new QEMU gate: every number here is timing, and QEMU timing is meaningless
  (the `rotary_knob_bench` Phase B precedent — measured on silicon, not gated).
- No CM4 offload in this issue. If both fixes below leave a gap it becomes a
  new Linear issue with these measurements as its input.
- No change to the SBC bitpool or the A2DP negotiation.

## What the code predicts (to be confirmed by measurement, not assumed)

The BT build's `loop()` has five slots per iteration:

| slot | what runs | predicted cost |
|---|---|---|
| `yield()` | HciPump → `Hci::service()` (drain RX, parse H4, NCP → credits) | negligible (~7 B × 69/s) |
| `src.service()` | SdpServer, **`L2cap::service()` — the credit-paced ACL write to Serial2**, Avdtp | **Serial2's TX ring is 64 B and the core's bulk `write()` is a per-byte loop that spins in `yield()` when the ring is full. A ~620 B media packet blocks ~1.9 ms at 3 Mbaud; at 69 pkt/s that is ~13 % of wall time, in bursts of 2–3 packets after each frame.** |
| `btout.poll()` | encode EVERY PCM block queued since the last iteration (ITCM, ~0.5 ms each), then drain into L2CAP's queue | a 31 ms frame queues ~11 blocks → ~5.5 ms burst |
| prints | `bt_hb` once/s | ~1 ms/s |
| `lvgl_rt1176_loop()` | LVGL timers (10 ms indev read, 33 ms refresh, 33 ms `ui_poll`), the GC355 compositor in the pre-flip hook, the vsync-fenced flip | the frame itself |
| `audio_probe_poll()` | RMS/bar bookkeeping | negligible |

Both bursts sit immediately before the render, and the pipeline is vsync-locked,
so an iteration that slips past a 16.7 ms slot loses the whole slot. That is the
expected mechanism. The instrument exists to confirm or refute it before any fix.

## Design

### 1. The instrument: `ACIDBOX_LOOPSTAT` (opt-in, default OFF)

`examples/display/acid_box/CMakeLists.txt` gains
`option(ACIDBOX_LOOPSTAT "Per-slot loop timing, frame and touch latency lines (bench)" OFF)`
→ `-DACIDBOX_LOOPSTAT=1`. Independent of `M2_BT_OUT`, so the SAME instrument
compiles into both builds. OFF leaves the default ELF **byte-identical**
(verified by `cmp` against the pre-change `build/acid_box.elf`), so the gate,
golden `0x25B30A96`, silicon golden `0x1479CEE8`, and the vacuity fixture are
untouched.

Three lines, each once per second, beside the existing `bt_hb`:

```
loopstat loops=N max_us=M yield=U svc=U poll=U enc=U drain=U txb=B print=U lvgl=U probe=U wiggle=W
framestat frames=F med_us=U max_us=U flips=+D wait_us=+W
touchstat n=N p50_us=U p95_us=U max_us=U
```

- **`loopstat`** — per-slot cumulative µs over the last second (`micros()`
  stamps around each slot in `loop()`), `loops` iterations, `max_us` the
  longest single iteration. `poll` is the whole `btout.poll()`; `enc` / `drain`
  / `txb` come from three new accumulators in `AudioOutputBluetooth`
  (`encodeUs()`, `drainUs()`, `txBytes()` — two extra `micros()` reads per
  poll, unconditional, negligible; `txb` counts bytes handed to `L2cap::send`
  incl. the 9-byte ACL header). With `M2_BT_OUT` off the BT slots are compiled
  out and print `0`; the line format never changes. `print` is the time spent
  in the heartbeat/loopstat prints themselves, so their console cost is visible
  and excluded from the other slots. `wiggle` is 0/1 (below).
  **The wire-bound test:** `svc ≈ txb × 3.33 µs` (10 bits/byte at 3 Mbaud, less
  64 B of ring per packet) confirms the blocking write; `svc ≪ txb × 3.33 µs`
  refutes it.
- **`framestat`** — LVGL display events (`LV_EVENT_REFR_START`,
  `LV_EVENT_RENDER_READY`, `LV_EVENT_REFR_READY`, as `synthui_knob_test`'s
  `rk_fps` uses them). A frame counts at `REFR_READY` only if `RENDER_READY`
  fired since `REFR_START` (an empty refresh cycle is not a frame). The
  interval is `REFR_READY`-to-`REFR_READY` between consecutive rendered frames
  inside the window; `med_us`/`max_us` over the window (ring of 64). `flips`
  and `wait_us` are deltas of the existing `lvgl_mipi_panel_flips()` /
  `lvgl_mipi_panel_wait_us()` — the fence's per-frame cost.
  **Read only with `wiggle=1`**: with the wiggle off the only periodic damage is
  the step cursor (~8.5 Hz at 128 BPM), so the interval reports the sequencer,
  not the pipeline.
- **`touchstat`** — touch-sample-to-presented-frame latency. The GT911 indev
  read callback is wrapped (`lv_indev_get_read_cb` / `lv_indev_set_read_cb`,
  LVGL 9.4; no LVGL port change): after calling the port's callback, if
  `(state, x, y)` differs from the previous read and no stamp is pending, stamp
  `micros()`. At a rendered `REFR_READY`, a pending stamp closes: sample =
  now − stamp. So each sample is the age, at presentation, of the OLDEST input
  change the frame carries. Samples go into a ring of the 256 most recent; the
  line prints p50/p95/max over the ring (and cumulative `n`) only in seconds
  where new samples arrived. **The acceptance reads the last `touchstat` line
  of a drag** (≥ 100 samples). Measured from LVGL's read of the panel, not the
  finger (the GT911's own 10 ms scan is the same in both builds).
- **Synthetic load (`wiggle`)** — the "ACID BOX" title label becomes clickable
  (`LV_OBJ_FLAG_CLICKABLE`); `LV_EVENT_CLICKED` toggles a 15 ms LVGL timer
  that steps all eight knobs' angles through a triangle sweep over ±140°
  (100 steps per sweep, knob k offset by k × 12 steps) via
  `synthui_rotary_knob_set_angle`. **`set_angle` does not send
  `LV_EVENT_VALUE_CHANGED`** (`synthui_rotary_knob.cpp:221-222` sends it
  separately from the input path), so the synth parameters never move — the
  panel animates flat-out while the sound is unchanged. The eight boot angles
  are captured at wiggle-on and restored at wiggle-off, so the picture matches
  the synth again afterwards. `build_ui()` keeps the eight knob handles in a
  static array under `ACIDBOX_LOOPSTAT` (today `mkknob`'s return value is
  discarded).
- **Placement.** The bench ELF has **1072 B of ITCM left** (`.text.itcm`
  0x3FBD0 of 0x40000). Every LOOPSTAT function (summary/print, the frame and
  touch callbacks, the wiggle timer, the title handler) carries
  `__attribute__((section(".progmem.loopstat"), noinline))` and lands in flash
  via the core's `*(.progmem*)` rule (`.text.progmem` is already `AX`). Only
  the per-iteration `micros()` stamps sit inline in `loop()`. If ITCM still
  overflows, `.text.loop` joins `.text.setup` in the derived BT linker
  script's flash list — one line in the existing `string(REPLACE …)`.
- The median/percentile over a ring is a pure header function
  (`loopstat_pct.h`) with a host test (`tests/run.sh`, the tree's pattern),
  because a p95 index is exactly where an off-by-one hides and nothing else
  can see it.

### 2. Bench protocol (A/B, silicon, Shokz OpenMove)

- **Build A** — `build-loopstat`: default + `-DACIDBOX_LOOPSTAT=ON` (BT off).
- **Build B** — `build-bench` reconfigured with `-DACIDBOX_LOOPSTAT=ON` on top
  of its existing bench config (firmware blob, 3 Mbaud, `M2_BT_TARGET_NAME=Shokz`,
  `M2_BT_RTS_FLOW=ON`, SSP).

Per build: `LinkServer flash … load` → `verify` → detach → console reader →
SW4 (the wedge-free procedure). Then: (B) wait for `bt_streaming`; tap ▶;
tap the title (wiggle on) for ~20 s; tap it again; drag CUTOFF back and forth
for ~15 s; (B) keep streaming ≥ 2 min and read `bt_hb`. Build B also yields a
free third reading BEFORE the A2DP connect lands (stack linked, idle), which
separates the stack's idle cost from streaming.

Readings that decide everything:

| number | A (BT off) | B (BT on, streaming) | criterion |
|---|---|---|---|
| `framestat frames`/s and `med_us` under `wiggle=1` | baseline | measured | B within 15 % of A |
| `touchstat p95_us` over the CUTOFF drag | baseline | measured | B within 15 % of A |
| `loopstat` attribution | (control) | `svc` vs `txb×3.33`, `enc`, `lvgl`, `max_us` | picks the fix |
| `bt_hb pcmdrops` / `drops` | — | over ≥ 2 min | 0 / 0 |

Everything is appended to `examples/display/acid_box/transcript_hw_evkb_bt.txt`.

### 3. Decision rule and fix 1 (predicted): a credit-bounded TX ring

**Rule:** if `svc` tracks `txb × 3.33 µs`, the write is wire-bound and fix 1
applies. If `svc` is small and `lvgl` + `enc` explain the gap, skip to §4.

**Fix 1 — `HciTransport` TX extension (M2Radio).** `HciTransport` gains
`static const size_t TX_EXTRA = 5120;` and a `uint8_t m_txExtra[TX_EXTRA]`,
attached in `begin()` with `m_port.addMemoryForWrite(m_txExtra, TX_EXTRA)`,
exactly as `RX_EXTRA` is attached for reads (and re-attached by `rebaud()`'s
`end(); begin()`). `write()` stays a pass-through.

Why it provably never blocks: `L2cap::service()` writes a packet only while it
holds an ACL credit, and credits return only after the controller has sent the
packet over the air. So bytes ever resident in the UART ring ≤
`aclNum × (9 + L2cap::MAX_PAYLOAD)` = 7 × 709 = **4963 B** on this IW416
(`hci_buffer acl_num=7`), below 5120 + 64. The UART drains at 3 Mbaud, far
faster than the air link, so the ring is never the bottleneck — the credits are.
HCI commands are tens of bytes and share the same bound.

Witness: acid_box prints `hci_txring=<TX_EXTRA+64> need=<aclNum×709>` once at
connect (and `WARN` if `need > ring`), and a `static_assert` in acid_box pins
`HciTransport::TX_EXTRA + 64 >= 7 * (9 + L2cap::MAX_PAYLOAD)` against the
documented credit count (the `hci/` layer cannot see `bt/`'s `MAX_PAYLOAD`).
Cost: 5 KB of `.bss` (DTCM: ~90 KB of 256 KB used today) in every consumer of
`HciTransport` — checked, not assumed, for `bt_tone_test` and `m2_hci_probe`.

Lands in M2Radio (commit, push, `evkb.cmake` pin bump, fresh-user
`-DEVKB_FORCE_FETCH=ON` verification — the standing procedure). Regression:
`audio/bt_tone_test` (2 gates) and `networking/m2_hci_probe` (4 gates) green;
QEMU's chardev has no baud so it cannot show the blocking, but it exercises the
ring plumbing. Then **re-measure B**. Inside 15 % on both numbers → done; §4 is
not built (YAGNI).

### 4. Fix 2 (conditional): encode off the loop

Built only if, after fix 1, a number is still > 15 % off AND `loopstat` shows
the encode burst (`enc`, `max_us`) is what pushes iterations past vsync slots.

Move the SBC encode into a **pended low-priority interrupt** (a reserved IRQ
vector, the mechanism `AudioStream` uses for `IRQ_SOFTWARE`), at priority
**240** — below the audio software ISR (208) and acid_box's note pump (224), so
neither can be delayed by an encode, and above the main loop so the encode
preempts the compositor in ~0.5 ms slices instead of a burst.
`AudioOutputBluetooth::update()` (the SAI-clocked audio ISR) copies the block
into the PCM ring and pends the IRQ; the handler encodes the backlog and
`push()`es into `MediaPacketizer` — whose push/drain contract already assumes an
ISR producer and a main-loop consumer (`mp_irq_save` around the `m_rd` commit);
`poll()` then only drains. Opt-in on the node (`setEncodeIrq(...)`; default
keeps today's main-loop encode so `bt_tone_test` is unchanged). NOT an
`IntervalTimer`: the imxrt1176 PIT period runs ~20× fast (silicon 2026-09-03).
`Sbc.cpp` stays in ITCM (`cd37050`). Total CPU is unchanged (~17 %); what
changes is burstiness — which only the re-measurement can value.

If both fixes leave a gap → CM4 offload, new Linear issue, out of scope here.

### 5. Close-out

Transcript (`transcript_hw_evkb_bt.txt`: baseline A/B, attribution, post-fix
A/B), CLAUDE.md measurement block, memory note, full sweep at **128/0/0**,
`LICENSE-AUDIT: PASS`, vacuity suite green, Linear NEW-33 → Done, merge the
branch to master.

## Testing

- **Default build byte-identical:** `cmp` of `build/acid_box.elf` before and
  after; `./run_qemu.sh` green (golden unchanged).
- **LOOPSTAT build boots in QEMU** (`tools/rt1170-qemu.sh`, not a gate): the
  three lines print with every field present. Numbers are meaningless there and
  nothing asserts them.
- **Host:** `examples/display/acid_box/tests/run.sh` — the percentile helper
  (p50/p95/max over a partially-filled and a wrapped ring; empty ring; n=1;
  demonstrated RED against an off-by-one). M2Radio's existing `bt/test` and
  `hci/test` suites stay green.
- **QEMU regression for fix 1:** the six gates named in §3; the full sweep.
- **Silicon:** the §2 table, before and after, in the transcript. The only
  acceptance is the two numbers within 15 % with drops held.

## Risks

- **Ceiling effect.** `LV_DEF_REFR_PERIOD` is 33 ms, so both builds cap near
  30 fps under wiggle; A and B could read 30 vs 30 while touch latency differs.
  That is why both numbers are required and why `lvgl` / `max_us` are reported
  beside them.
- **The touch number is human-driven.** Same knob, same motion, ≥ 100 samples
  per drag; p95 over a 256-sample ring smooths the rest.
- **The prints cost console time** (~2–4 ms/s at 115200); measured in `print`,
  never per-packet (the trace-throttled-crackle lesson of 2026-09-04).
- **ITCM** — the bench build is 1072 B from full; section placement handles it
  and the linker fails loudly if it does not.
- **A dangling touch stamp**: a change that renders nothing (a press on empty
  panel) closes on the next rendered frame and inflates one sample. The drag
  protocol keeps the finger on CUTOFF, where every move renders.

## File-by-file

- `examples/display/acid_box/CMakeLists.txt` — `option(ACIDBOX_LOOPSTAT)` →
  `-DACIDBOX_LOOPSTAT=1`; (if needed) `.text.loop` in the BT linker-script
  flash list.
- `examples/display/acid_box/acid_box.cpp` — under `ACIDBOX_LOOPSTAT`: slot
  stamps in `loop()`, the three summary lines, display event callbacks, the
  indev read wrapper, the wiggle timer + clickable title, knob handle array;
  the `hci_txring` witness + `static_assert` under `M2_BT_OUT`.
- `examples/display/acid_box/loopstat_pct.h` + `tests/` — pure percentile
  helper and its host test.
- `examples/audio/bt_tone_test/AudioOutputBluetooth.{h,cpp}` — `encodeUs()`,
  `drainUs()`, `txBytes()` accumulators. (Fix 2, if built: `setEncodeIrq()`.)
- `M2Radio/hci/HciTransport.{h,cpp}` — `TX_EXTRA` + `addMemoryForWrite` in
  `begin()` (fix 1). `evkb.cmake` pin bump.
- `examples/display/acid_box/transcript_hw_evkb_bt.txt`, `CLAUDE.md`, memory.
