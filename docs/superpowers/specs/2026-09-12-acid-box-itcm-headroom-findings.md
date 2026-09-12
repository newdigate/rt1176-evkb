# acid_box ITCM headroom — session findings (NEW-45, NEW-37)

Date: 2026-09-11/12
Status: findings and decision record for work that SHIPPED. The change itself is
in `examples/display/acid_box/CMakeLists.txt`, `tools/build-bench-configs.sh`,
`examples/display/acid_box/bench` and the root `CMakeLists.txt`; the design is
`2026-09-11-acid-box-itcm-headroom-design.md` and the plan is
`../plans/2026-09-11-acid-box-itcm-headroom.md`. This file is the narrative the
other two cannot carry: what was decided, why, what it cost, and — the reason it
is worth keeping — **which of its own predictions the bench refuted**.

Also published as a formatted record:
<https://claude.ai/code/artifact/e6b5e138-d17d-435b-970d-4f2dc93a9d7f>
(same content; this file is the version in the repo and wins on any difference.)

## 1. What broke, and the measurement that named the cause

Four bench directories stopped linking. `build-bt` was 140 B over ITCM and the
three `ACIDBOX_LOOPSTAT` dirs 316 B — **both exactly +412 B past the headroom
recorded on 2026-09-08** (272 B and 96 B).

That identical delta across two builds sharing only their libraries IS the
diagnosis: library growth, not acid_box and not M2Radio. The only pins that had
moved were `cores` `a9b0de5` and `Audio` `ff610a2` (NEW-41's `audioPllTrimPpm`,
its global ctor, `headphoneVolume`). The 2026-09-08 M2Radio wildcard held —
M2Radio still contributed `Sbc.cpp` alone.

★ **The recorded `272 B` is the only reason this was attributable.** Without a
headroom number written down, the reading is "it overflows" and the +412 B is
invisible. Hence §5's instrument.

## 2. Where the 256 KB goes

Measured by relinking the failing configuration against a **1 MB ITCM** to obtain
a map — the section does not fit, so no ELF exists to `nm`. The recipe (a `sed`
on the derived script's `MEMORY` length plus `-Wl,-Map` on the link line from
`CMakeFiles/acid_box.elf.dir/link.txt`) needs no bench and is the reusable
instrument for every later question here.

```
203050  libLVGL.a          <- 77.4 %
  9984  libVGLite.a          9300  libcores.o.a       6856  libm.a
  5926  libSynthUI.a         5470  libc_nano.a        5242  libAudio.o.a
  4928  acid_box.cpp         2424  libgcc.a           2260  libM2Radio (Sbc only)
  2226  libMipiDisplay       1306  libWire             906  libTouchPanel
```

So ITCM is **203 KB of LVGL that measurement says belongs there** (its slot costs
844 ms of every wall-clock second at 30 fps) **plus ~53 KB that is there by
default**. The revisit was only ever about the 53 KB.

Two archives that look like easy candidates are not, checked rather than assumed:
`libc_nano` holds `memcpy`/`memset`/`memmove`, and `libm` is pulled by `powf` and
`tanhf` — almost certainly the acid-bass waveshaper, i.e. the per-block audio
path. Neither is a wholesale candidate.

★ **ITCM cannot grow.** The FlexRAM split is 8 DTCM + 8 ITCM banks of the
16 x 32 K array, and the script's own comment records that a non-power-of-2 bank
count leaves the window unbacked (IBUSERR on fetch). 384 K is not a power of two;
512 K leaves no DTCM for the stack. **256 K is the ceiling** — the only lever is
what goes in it.

## 3. The two rules now in the linker script

Both are earned by this tree's own history and both live in the script's comment,
not only in a design document.

1. **Route WHOLE ARCHIVES with `EXCLUDE_FILE` for named hot exceptions — never an
   inclusion list of objects.** The direction a rule drifts when a library grows
   is the property being bought: an exclusion rule makes a newly added file
   default to FLASH, an inclusion list defaults it to ITCM — which is exactly how
   `Avrcp.cpp` overflowed ITCM for a day on 2026-09-07.
2. **Never capture `.fastrun` in a new rule**, so `FASTRUN` keeps meaning ITCM and
   a library can pin one hot function back without a linker-script change. The
   pre-existing M2Radio line captures it and is inert only because M2Radio marks
   nothing FASTRUN.

## 4. The bench: four cumulative arms

Predictions were written into `examples/display/acid_box/transcript_hw_evkb_bt.txt`
and **committed before any arm was built** (`d4b9014`). Board: MIMXRT1170-EVKB,
Shokz OpenMove (`C0:86:B3:31:29:2F`, `cod=0x240418`), real 131,840-byte
`uartIW416_bt.bin.inc` verified per arm by symbol size `00020300`.

| arm | routing added (cumulative) | itcm | headroom | prediction | measured | verdict |
| -- | -- | -- | -- | -- | -- | -- |
| (a) | `libVGLite` -> flash | 252,416 | 9,728 | no change | 29 fps @ 33,321 us; enc 125.6 us/block | HELD |
| (b) | + `MipiDisplay`, `Wire`, `TouchPanel` | 248,368 | 13,776 | timeouts=0; touch p95 <= 78 ms | 0 across 801 lines; 71.5 ms (n=273) | **HELD — SHIPPED** |
| (c) | + `Sbc.cpp` | 246,128 | 16,016 | enc 125 -> ~190 us/block | **704 us/block** | **REFUTED** |
| (d) | (a) + `libLVGL`, placement only | 49,744 | 212,400 | 30 -> 20 fps @ ~50,000 us | 27 fps @ 39,275 us | PART — REJECTED |

Every arm held `ACIDBOX_UI_SUM = 0x1479CEE8` and `gpu_err = 0`, so all four are
valid: **placement changed no pixels anywhere**. That checksum is what makes a
timing comparison meaningful rather than a comparison of two different pictures.

**Shipped arm (b).** Headroom 272 B -> 13,968 B (`build-bt`) and 96 B -> 13,776 B
(the loopstat dirs) — 33x the growth that broke it. The named risk was
`lcdifv2.cpp`'s **LCDIFv2 vsync ISR** now running from flash; answered with
`timeouts=0` across 801 fence lines, `flips == isrs`.

## 5. ★★ The result worth keeping: an arm that PASSED and was DECLINED

Arm (c) moved `Sbc.cpp` to flash — NEW-37 option 1 — and measured **704 us/block
against 125.6 in ITCM, 5.6x**. The design predicted ~190 us by extrapolating
`timing/icache_bench_hw`, which measured 187 vs 125 (1.5x) for the same
transition. **The extra is ~9x what that benchmark implied.**

★ **The reason was already written down and the spec did not weight it.**
NEW-36's own bench README: *"that is a best case — acid_box's `M2_BT_OUT` build
routes 17 M2Radio objects (tens of KB) to FLASH, where set conflicts are real, so
the application number must be MEASURED, not extrapolated."* The benchmark's
working set was ~3 KB inside a 32 KB I-cache; this build has VGLite, MipiDisplay,
Wire, TouchPanel and all of M2Radio competing for it.

**The transferable lesson: a micro-benchmark measures the cache, not the
application.** Same code, same transition, same silicon — 1.5x on a 3 KB working
set, 5.6x inside a real image.

The arm nonetheless PASSED its stated acceptance over **8.8 hours** of streaming:
31,667 streaming heartbeats, `pcmdrops=0` and `drops=0` throughout, 345 blocks/s,
4.1x real-time headroom against the 2.902 ms block period. It is therefore
**declined on COST** — 200 ms/s of CPU, a fifth of the core, for 2,240 B that the
shipped headroom makes unnecessary — **and not on failure**. NEW-37 option 1:
answered.

## 6. Arm (d), and why no re-run was needed

27 fps at 39,275 us, with the LVGL slot at **951,346 us/s — 95 % of wall time** —
and the loop at 352 it/s against arm (a)'s 2,689.

★ **The figure is a LOWER BOUND.** It never connected (`blocks=0`,
`a2dp=connect_failed`), so it carried no audio encode and no BT service while
arms (a)-(c) were all measured WHILE STREAMING. Adding the load it did not carry
cannot improve it, and the lower bound already misses 30 fps. 203 KB stays
unavailable without another bench run.

The prediction was directionally right and wrong on both magnitude and mechanism:
predicted a clean slip to three vsync periods (50,000 us), measured a **mix** of
2- and 3-period frames.

## 7. Two limitations of this bench, recorded rather than left to be found

1. **A control for arm (a) is not buildable.** "Same source, VGLite in ITCM" IS
   the image that overflows by 140 B — the reason arm (a) exists. The nearest
   substitutes (MipiDisplay+Wire+TouchPanel+Sbc = 6,698 B) cannot free the
   10,124 B required. Arm (a) is judged on internal evidence instead: the frame
   interval is vsync-locked at 33,321 us = 2 x 16.67 ms exactly, and the fence
   never timed out across 19,747 flips.
2. **The baseline the plan named was invalid, and that was a defect in the plan.**
   The NEW-36 POST column was measured on a firmware whose heartbeat has since
   grown three lines (`bt_cred`, `bt_link`, `bt_mem`), so `print` costs 495 ms/s
   here against that column's 72 — a 6.8x observer effect that halves the loop
   rate on its own. Every comparison was redone **within-session, arm against
   arm**. The per-iteration figures do survive (`svc` 0.44-0.46 us/iter across
   NEW-36 POST and arms (a)/(b)), which is what a print-insensitive metric looks
   like.

## 8. The discovery gap, and the tool that shipped its own disease

Nothing in this tree built a BENCH directory — gates never build — which is why
the break sat two days until an unrelated workstream rebuilt one.
`tools/build-bench-configs.sh` now builds every configuration declared in an
example's `bench` sidecar, and the root `bench_check` target runs it. It builds
into `build-benchcheck-*` directories IT OWNS: `build-bench`/`-pre`/`-post` carry
a real `M2RADIO_IW416_BT_FW`, and reconfiguring one from the declared flags alone
would silently strip it. `-n` nm-diffs the owned dir against the human's to PROVE
the proxy (the blob lands in `.progmem`, never ITCM) rather than asserting it.

★★ **The tool exists to catch a build that silently does nothing, and it shipped
that exact defect four times.**

1. `cmake -B` with no `-S` configured the ROOT project
   (`project(rt1170_evkb_root NONE)`, empty `all`), built nothing, exited 0 and
   printed `OK`. **The only symptom was SPEED** — seconds instead of ten minutes.
   Before `912c8d1` added that root CMakeLists the same omission was LOUD, so the
   failure mode is newer than the idiom.
2. The RED demo poisoned the owned cache, which retains every `-D` ever passed, so
   a flag REMOVED from a `bench` line would stay in effect forever and the tool
   would measure a configuration nobody declared. It now wipes before configuring;
   the cost is a full build every run, which is the honest price of the question.
3. `-n` could print `nm-diff OK` having read no symbols at all — `itcm_syms` is a
   pipeline so its status is `sort`'s, and two EMPTY files compare equal. Three
   siblings in the same path: zero pairs passed silently, the pattern argument was
   ignored, and it compared symbol NAMES without `.text.itcm` size.
4. `-not -path '*/build*'` matches the WHOLE path, so a checkout under
   `~/buildfarm` pruned everything and printed PASS — invisible to every arm,
   because they all build their throwaway tree under a path with no `build`
   component.

★ **Two mutation arms were themselves VACUOUS**, and only running the mutants
found them: one recipe went stale when the code was restructured and silently
mutated nothing (the harness now ASSERTS its anchors), and the other put both
sides of a comparison inside the ITCM address window, where `awk` prints the
symbol NAME and the sets matched either way. An arm that cannot fail is worse
than no arm, because it is counted. Final: 19 arms, 65 assertions, 11 mutants,
all reddening by name.

## 9. Bench traps met this session

* **A stale bond to a PHONE** (`cod=0x7A020C`, major device class 2) paged a
  device that can never be an A2DP sink — 20x `avdtp_failed`, 2x `connect_failed`,
  after a perfectly good `connect_secure=ok encryption=on paired_by=stored`.
  Decode the class of device before chasing a "missing" sink.
* **With our bond wiped the Shokz was NOT discoverable** until put into pairing
  mode, because it still remembered US — the NEW-46 asymmetry seen from the source
  side. `inquiry_complete: n=0` (zero devices, not "none matching") is the tell.
* **"Wire not connected" on every transfer was THE POWER SWITCH**, not the
  documented DAP wedge. The MCU-Link still enumerated its VCOM either way; what
  separates them is that the console was silent TOO.
* **An n=6 touch sample read p95 = 1.44 s.** The same arm's full boot reads
  71.5 ms on n=273. Split a multi-boot capture by boot and analyse the most
  complete one.
* **A truncated pass-count nearly entered the record**: the vacuity suite piped
  through `tail -4` read "4 cases". The real figure is 47.

## 10. Close-out, measured 2026-09-12

* QEMU sweep: **139 gates discovered, 139 passed, 0 failed, 0 SKIP**, exit 0,
  23m25s. No member of the load-sensitivity class went red in the sweep itself.
* `LICENSE-AUDIT: PASS`, run after the sweep, never during.
* Vacuity suite **47/47**.
* `build-bench-configs.test.sh` PASS; `build-bench-configs.sh -n` PASS, 2
  configurations, 2 nm-diff pairs matched (`.text.itcm` 248,368 and 248,176).
* Default `display/acid_box` ELF's loadable image **byte-identical at 295,936 B**;
  gate PASS, golden `0x25B30A96` unmoved.
* **No fresh-user `-DEVKB_FORCE_FETCH=ON` check**, stated rather than omitted: no
  library pin moved — every change is in the `evkb` repo itself.

## 11. The shape of it

Every real finding in this session came from RUNNING something, not from reading
it. The overflow's cause, the tool's four faults, both vacuous test arms, and both
refuted predictions were each surfaced by an execution that disagreed with a
written expectation — and in three of those cases the expectation was mine.
