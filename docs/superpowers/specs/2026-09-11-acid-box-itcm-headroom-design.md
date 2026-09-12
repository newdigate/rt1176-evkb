# acid_box: ITCM headroom for the `M2_BT_OUT` bench builds (NEW-45, folding in NEW-37)

Design, brainstormed and approved 2026-09-11.  NEW-45 was filed as "the build is broken"; NEW-37 as "this
could be optimised".  They are the same file and the same decision, and this spec takes them together: the
overflow is repaired by answering NEW-37's question — what still belongs in ITCM now the CM7 has an
instruction cache — rather than by shaving bytes until it fits again.

★ **Session findings and decision record:** `2026-09-12-acid-box-itcm-headroom-findings.md` — what was
decided and why, the two predictions this spec's own reasoning got wrong, and the bench traps. Read that
for the narrative; this file is the design.

## 1. The problem, measured

Four build directories do not link.  Reproduced 2026-09-11 against `master`:

| build dir | configuration | headroom 2026-09-08 | today |
| -- | -- | -- | -- |
| `build-bt` | `M2_BT_OUT=ON` | 272 B | **−140 B** |
| `build-bench`, `build-bench-pre`, `build-bench-post` | `M2_BT_OUT=ON` + `ACIDBOX_LOOPSTAT=ON` | 96 B | **−316 B** |

```
ld: acid_box.elf section `.text.itcm' will not fit in region `ITCM'
ld: region `ITCM' overflowed by 140 bytes
```

**Both configurations moved by exactly +412 B**, which is the whole diagnosis: an identical delta across two
builds that share only their libraries is library growth, not anything acid_box or M2Radio did.  The only
pins that moved since 2026-09-08 are `cores` → `a9b0de5` and `Audio` → `ff610a2`, both from NEW-41, and both
archives are ITCM-resident here.  The net-new symbols account for it — `audioPllTrimPpm` 112 B, its global
constructor 44 B, `AudioControlWM8962::headphoneVolume` 52 B, plus growth inside `audioPllConfigure` (now
240 B), `ai_write` and `output_i2s`.

★ **The 2026-09-08 wildcard held.**  `libM2Radio` contributes 2,260 B — `Sbc.cpp` alone, exactly as designed.
This is not a fourth instance of the drifting-object-list bug; it is the first instance of the failure mode
that bug was masking, which is that the build has no headroom at all.

★ **The recorded `272 B` is the only reason this was attributable.**  Without a headroom number written down
on 2026-09-08, today's reading is "it overflows" and the +412 B is invisible.  §7 makes recording it
automatic, and §8 makes re-recording it an acceptance item.

## 2. Where the 256 KB goes

Measured by relinking the failing configuration against a 1 MB ITCM region to obtain a map — the section
does not fit, so no ELF exists to `nm`.  The recipe (a `sed` on the derived script's `MEMORY` length plus
`-Wl,-Map` on the link line from `CMakeFiles/acid_box.elf.dir/link.txt`) is the instrument for every later
re-measurement in this spec, and it needs no bench.

```
203050  libLVGL.a          ← 77.4 %
  9984  libVGLite.a          9300  libcores.o.a       6856  libm.a
  5926  libSynthUI.a         5470  libc_nano.a        5242  libAudio.o.a
  4928  acid_box.cpp         2424  libgcc.a           2260  libM2Radio (Sbc.cpp only)
  2226  libMipiDisplay       1306  libWire             906  libTouchPanel
   814  AudioOutputBluetooth.cpp                      1052  LVGL port objects
```

The budget is therefore **203 KB of LVGL that measurement says belongs there, plus 53 KB of everything else
that is there by default rather than by evidence.**  NEW-37's revisit is about the 53 KB.

Two archives in that 53 KB are not wholesale candidates, checked rather than assumed:

* `libc_nano` holds `memcpy`, `memset`, `memmove`, `strlen` alongside the printf family.  Routing it whole
  would put the framebuffer and audio-block copies in flash.
* `libm` is pulled by `acid_box.cpp` itself (`powf`) and by `tanhf`/`tanf` — almost certainly the acid-bass
  waveshaper, i.e. the per-block audio path.

Neither is excluded forever; both need an `EXCLUDE_FILE` and a measurement, which is more than this change
should spend.  What is left is a tier of archives cold or coarse-grained **by construction**:

| archive | ITCM | why it is coarse |
| -- | -- | -- |
| `libVGLite` | 9,984 | called per *draw call*; the GC355 does the pixels |
| `libMipiDisplay` | 2,226 | DSI bring-up plus the flip path, not a per-pixel loop |
| `libWire` | 1,306 | I²C to the codec and touch controller, at UI rate |
| `libTouchPanel` | 906 | polled at UI rate |
| **total** | **14,422** | ≈ 53× the headroom that existed on 2026-09-08 |

## 3. What is not available, and why

* **ITCM cannot grow.**  The linker script's FlexRAM split is `0xFFFFAAAA` — 8 DTCM banks (256 K) + 8 ITCM
  banks (256 K) of the 16 × 32 K array — and its own comment records that a non-power-of-2 bank count leaves
  the window unbacked (IBUSERR on fetch).  384 K ITCM is not a power of two; 512 K leaves no DTCM for the
  stack.  256 K is the ceiling.
* **LVGL is predicted unavailable.**  From the NEW-36 A/B transcript, the `lvgl` slot costs **844 ms per
  second of wall time** in the `wiggle=1` window at 30 fps — ≈ 28 ms of a 33.3 ms frame budget.  At the
  I-cache's best observed ratio (1.5×, measured on a *3 KB* working set; LVGL's is 200 KB against a 32 KB
  cache) that is ≈ 42 ms, past the next vsync multiple.  Prediction: **30 fps → 20 fps.**  §6 arm (d) tests
  it anyway, because it is one linker line and the payoff if the prediction is wrong is 203 KB.
* **Making the bench build GPU-rendered is declined.**  `import_evkb_lvgl(VGLITE)` already archives as
  `libLVGL_flash.a`, which the core script routes to flash — 203 KB by a mechanism that exists.  But it
  changes the renderer, not the placement: the goldens move, and `display/vglite_lvgl_test` measured
  GPU-mode LVGL *slower* than software (2.45 vs 2.83 fps).  Named here so a later reader sees it was
  considered, not missed.

## 4. The fix, in phases

**Phase 0 — unbreak, provisionally.**  One archive-granular rule routing `libVGLite` to flash.  It is the
largest non-LVGL consumer and the most clearly coarse-grained; it restores all four directories and gives
Phase 2 a baseline ELF to measure against.  Provisional by declaration: arm (a) either confirms it or it is
reverted and the tier is rebuilt from the remaining 4.4 KB plus `Sbc.cpp`.

**Phase 1 — guardrails (§7).**  No silicon.  The headroom instrument and the bench sidecar plus its builder.
Independently useful and independently shippable: if the bench session slips, Phase 1 still turns the next
occurrence from "found months later by an unrelated workstream" into "found at close-out".

**Phase 2 — one silicon session, four arms (§6).**

**Phase 3 — commit what measurement supports**, record the refuted arms with their numbers, update
`CLAUDE.md` with the per-directory headroom, and resolve NEW-37: closed if arms (c) and (d) both settle, or
narrowed in writing to whatever they leave open.

## 5. Two rules for the linker script

Both are earned by this tree's own history and both go into the script's comment, not only into this file.

1. **Route whole archives, with `EXCLUDE_FILE` for named hot exceptions — never an inclusion list of
   objects.**  An exclusion rule makes a newly added library file default to *flash*; an inclusion list makes
   it default to ITCM, which is exactly how `Avrcp.cpp` overflowed ITCM for a day on 2026-09-07.  The
   direction a rule drifts when a library grows is the whole property being bought.
2. **New rules must not capture `.fastrun`.**  The existing M2Radio rule does — `EXCLUDE_FILE(*Sbc.cpp.obj)
   .text* EXCLUDE_FILE(*Sbc.cpp.obj) .fastrun` — which quietly defeats `FASTRUN` for that library; its
   comment says so and calls it inert because M2Radio marks nothing.  Keeping `.fastrun` out of the new
   rules leaves every routed library a way to pin one hot function back into ITCM without a linker-script
   change.

`EXCLUDE_FILE` matches the archive **member** name under ld 2.35 — as the 2026-09-08 swap established.

## 6. The measurement

**Instrument.**  `ACIDBOX_LOOPSTAT`, unchanged: per-slot µs/s and µs/iter (`svc`, `poll`, `enc`, `drain`,
`print`, `lvgl`, `yield`, `max_us`), `framestat` (fps, median frame interval), `touchstat` p95, and
`pcmdrops` / air-link `drops` from the BT report.  `examples/display/acid_box/transcript_hw_evkb_bt.txt`
(NEW-36 I-CACHE A/B) is the baseline, and the arms follow that section's protocol exactly: one variable per
arm, flash → verify → reader → SW4 → `bt_streaming` → PLAY → title tap (`wiggle=1`) ~20 s → tap
(`wiggle=0`) → stream ≥ 60 s → CUTOFF drag; last four loopstat lines averaged in each window.

**Every arm carries one un-fakeable check: `ACIDBOX_UI_SUM` must remain `0x1479CEE8`.**  Placement changes
no pixels.  A moved checksum means the arm changed behaviour and voids it however good its timing looked.

**Pre-registered predictions.**  Written before the boot; each may be refuted in writing, and a refutation is
recorded with its number rather than quietly re-fitted.

| arm | change | prediction | **measured 2026-09-11/12** | verdict |
| -- | -- | -- | -- | -- |
| (a) | `libVGLite` → flash | no change | 29 fps @ 33,321 µs; `enc` 125.6 µs/block; `timeouts=0` | **HELD** |
| (b) | + `libMipiDisplay`, `libWire`, `libTouchPanel` | `timeouts=0`; touch p95 ≤ 78 ms | `timeouts=0` on 801 fence lines; touch p95 **71.5 ms** (n=273) | **HELD** |
| (c) | + `Sbc.cpp` | `enc` 125 → ~190 µs/block | **704 µs/block — 5.6×, not 1.5×** | **REFUTED** |
| (d) | LVGL → flash, placement only | 30 → 20 fps at ~50,000 µs | **27 fps at 39,275 µs**, a *mix* of 2- and 3-vsync frames | **PART** |

Every arm held `ACIDBOX_UI_SUM = 0x1479CEE8` and `gpu_err=0`, so all four are valid: placement changed no
pixels anywhere. **Shipped: arm (b)** — headroom 9,728 → 13,776 B. **Declined: (c) on cost, (d) on frame
rate.**

★★ **Arm (c) is the result worth keeping, and it refutes this spec's own reasoning.** §6 predicted ~190 µs
by extrapolating NEW-36's `icache_bench_hw` (flash+I-cache 187 µs vs ITCM 125). The real cost is 704 µs —
the extra is ~9× what that benchmark implied. **The reason was already written down in NEW-36's README and
this spec did not weight it**: *"that is a best case — acid_box's `M2_BT_OUT` build routes 17 M2Radio
objects (tens of KB) to FLASH, where set conflicts are real, so the application number must be MEASURED, not
extrapolated."* The benchmark's working set was ~3 KB inside a 32 KB I-cache; this build has VGLite,
MipiDisplay, Wire, TouchPanel and all of M2Radio competing for it. **The general lesson: a micro-benchmark
measures the cache, not the application.**

★ **Arm (c) passed its acceptance anyway** — 8.8 hours, 31,667 streaming heartbeats, `pcmdrops=0` and
`drops=0` throughout, 345 blocks/s, 4.1× real-time headroom — so it is declined on **cost** (200 ms/s of CPU
for 2,240 B) and not on failure. That is NEW-37 option 1, answered.

★★ **Arm (d)'s number is a LOWER BOUND, which is why no re-run is needed.** It never connected
(`blocks=0`), so 27 fps was measured with no audio encode and no BT service competing, while arms (a)–(c)
were all measured *while streaming*. Its LVGL slot was already 951,346 µs/s — 95 % of wall time — and its
loop ran at 352 it/s against arm (a)'s 2,689. Adding the load it did not carry cannot improve it.

★★ **Two limitations of this bench, recorded rather than left to be discovered.**
**(1) A control for arm (a) is not buildable.** "Same source, VGLite in ITCM" *is* the image that overflows
by 140 B — the reason arm (a) exists. The nearest substitutes (MipiDisplay+Wire+TouchPanel+Sbc = 6,698 B)
cannot free the 10,124 B required, so arm (a) is judged on internal evidence: the frame interval is
vsync-locked at 33,321 µs = 2 × 16.67 ms exactly, and the fence never timed out.
**(2) §6's stated baseline was invalid and this was a defect in the plan.** The NEW-36 POST column was
measured on a firmware whose heartbeat has since grown three lines (`bt_cred`, `bt_link`, `bt_mem`), so
`print` costs 495 ms/s here against that column's 72 — a 6.8× observer effect that halves the loop rate on
its own. **Every comparison above is therefore within-session, arm against arm.** The per-iteration figures
do survive the change (`svc` 0.44–0.46 µs/iter across NEW-36 POST and arms (a)/(b)), which is what a
print-insensitive metric looks like.

★ **Arm (c) is not judged by NEW-37's stated criterion.**  That issue asked for `enc` "within 50 % of the
ITCM figure", which lands at 187 µs against a predicted ~190 — a coin toss dressed as a threshold.  It is
judged instead on `pcmdrops = 0` and `drops` no worse than the baseline, which is what the ITCM placement
was bought for in NEW-9.

★ **Arms are cumulative and ordered by confidence**, so a regression names its own cause: (b) is (a) plus
three archives, (c) is (b) plus one object.  Arm (d) is run last and standalone against (a), since a 20 fps
result makes anything stacked on it unreadable.

## 7. Guardrails

### 7.1 The `bench` sidecar

One file per example, the same idiom as the existing `boards` sidecar — a declared name plus the cmake flags
that define the configuration:

```
# examples/display/acid_box/bench
bt        -DM2_BT_OUT=ON
loopstat  -DM2_BT_OUT=ON -DACIDBOX_LOOPSTAT=ON
```

Two configurations cover all four broken directories: `build-bench-pre` and `-post` differ from
`build-bench` only by the **core pin** (`TEENSY_LIB_ROOT` state, not a cmake flag), so they are historical
A/B directories, not configurations to declare.

### 7.2 The builder, and the directory it must not touch

`tools/build-bench-configs.sh` configures and builds every declared configuration into directories **it
owns** — `build-benchcheck-<name>` — and never touches a human's bench directory.

★ **That is a correctness requirement, not tidiness.**  `build-bt` carries `M2RADIO_IW416_BT_FW` pointing at
a real 131,840-byte blob; a tool that reconfigured it from the declared flags alone would silently strip the
bench's firmware.  It is the 2026-08-27 red inverted — there, a bench-configured directory made its own gate
fail; here, a gate-minded tool would break the bench.  Configure-time cache variables are a build-directory
state axis `git status` cannot see, and a tool that writes into that axis has to own the directory.

★ **The proxy is sound and checkably so.**  The firmware blob lands in `.progmem`, not ITCM — with the
synthetic 1 KB fallback the ITCM footprint is identical — so `build-benchcheck-bt` has the same ITCM symbol
set as `build-bt`.  The tool `nm`-diffs the two when both exist and reports a difference rather than assuming
equivalence.

Run at close-out beside `LICENSE-AUDIT`, **never concurrently with the QEMU sweep** — CLAUDE.md already
records a sweep invalidated by a concurrent audit, and this tool is heavier.

### 7.3 The headroom instrument, two halves

* `-Wl,--print-memory-usage` on the `M2_BT_OUT` target, so every link prints region used/size/percent and a
  shrinking margin is visible in ordinary build output rather than only at the cliff.
* `ASSERT(SIZEOF(.text.itcm) <= LENGTH(ITCM) - N)` in the derived script, with `N` injected at configure
  time from a cache variable — the script is already generated by `string(REPLACE)`, so this costs nothing
  structurally.  `N = 2048`: low enough not to nag against the ~14 KB §2's tier buys, high enough that the
  next NEW-41-sized growth reports `ITCM headroom below 2048` instead of `overflowed by 140`.

Both are scoped to acid_box's derived script and its `M2_BT_OUT` target.  Putting either in the core
`imxrt1176.ld` would change every example in the tree, including ones that legitimately run close; that is a
separate change with its own blast radius.

## 8. Acceptance

* **The default `display/acid_box` ELF's loadable image is byte-identical** (`objcopy -O binary`, diffed).
  The routing is scoped to `M2_BT_OUT`, so this holds *by construction* — which is exactly why it gets
  checked rather than argued.
* **`nm`-diffed ITCM symbol set per arm**: the symbols that left ITCM are exactly those the rule names, and
  nothing else moved.  Same acceptance as the 2026-09-08 wildcard swap.
* All four bench directories link, and `build-benchcheck-*` links from the declared flags alone.
* QEMU gate `display/acid_box` green with golden `0x25B30A96` unmoved; full sweep at **139 discovered**, with
  any member of the documented load-sensitivity class dispositioned by re-running it idle, not explained away.
* `LICENSE-AUDIT: PASS`, run before or after the sweep and never during.
* Silicon: `ACIDBOX_UI_SUM = 0x1479CEE8`, `ACIDBOX_VSYNC timeouts=0`, 30 fps, `pcmdrops = 0`, `drops` no
  worse than the baseline, on the committed arm.
* **Per-directory headroom recorded in `CLAUDE.md`**, as the 2026-09-08 entry did.  A number nobody writes
  down cannot be diffed later, and §1 is what that costs.

## 9. Out of scope

* **The gate runner is untouched.**  Its hard-coded `pxp_draw_bench` / `build-32` special case could be
  absorbed by the `bench` sidecar, but it works today, it is a gate concern rather than a bench one, and
  folding it in would turn a contained fix into a change to the script every sweep depends on.
* **No new `GATES` manifest entry.**  These are not gates, nothing runs them in QEMU, and they link the same
  libraries already audited through `audio/bt_tone_test`.  Stated so the omission does not later read as
  drift.
* **`libc_nano` and `libm` are not routed** (§2).  Both need an `EXCLUDE_FILE` and a measurement; the tier in
  §2 makes them unnecessary for this change.
* **Inverting the rule — flash by default, ITCM by allow-list — is not taken here.**  It yields the most
  headroom and makes the safe default automatic for future library growth, but it inverts the failure mode
  from loud (a link error) to quiet (a performance regression nobody notices), and the `_stext` / `_etext` /
  `_itcm_block_count` machinery assumes ITCM holds the bulk.  It stays on the table for a later issue, and
  §6's arms are exactly the evidence that would justify it: if (a)–(d) all come back free, the inversion is
  the next step rather than a gamble.

## 10. Risks

* **Arm (a) fails and Phase 0's unbreak has to be reverted.**  Then the tier is `libMipiDisplay` + `libWire`
  + `libTouchPanel` + `Sbc.cpp` ≈ 6.7 KB — still 24× the 2026-09-08 headroom, so the change survives; only
  the margin shrinks.
* **A routed archive holds an ISR.**  Verified, not supposed: `libVGLite`'s `vg_lite_hal.c` / `vg_lite_os.c`
  carry the GPU2D handler (`vg_lite_IRQHandler`, attached at `vg_lite_hal.c:248`) and `libMipiDisplay`'s
  `lcdifv2.cpp` the LCDIFv2 vsync ISR — the one the tear-free pipeline's fence counts.  With the I-cache
  this costs a first-use miss, not a steady-state penalty
  — but it is the most likely source of a surprise in arm (a) or (b), so `ACIDBOX_VSYNC timeouts=0` and
  `max_us` are watched specifically, not only fps.
* **The bench session does not happen.**  Phase 1 is independent and ships regardless; Phase 0 alone
  restores the four builds.  The spec degrades to "unbroken with ~10 KB headroom and a guardrail", which is
  still a better state than today.
* **A fifth instance arrives from `cores` or `Audio` anyway.**  Neither can be routed to flash — they hold
  the ISRs and the audio graph — so headroom is the only defence against exactly the libraries that caused
  this one.  That is the argument for taking the whole tier rather than the minimum that fits.
