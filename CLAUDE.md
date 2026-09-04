# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## What this repo is

An Arduino/Teensyduino-style core for the NXP i.MX RT1176 (dual Cortex-M7 @
996 MHz + Cortex-M4) on the MIMXRT1170-EVKB board, derived from the Teensy 4.x
core. `README.md` and `examples/README.md` are accurate and detailed — read
them for anything not covered here.

The parent directory (`~/Development/rt1170/`) is a non-git workspace holding
NXP reference material only (reference-manual and board PDFs, `rm_full.txt` —
the reference manual as searchable text — and the EVKB RevC3 design files).
It is kept out of the repo deliberately: those files are NXP-copyrighted and
large.

### Git layout (important)

- This repo is `github.com/newdigate/rt1176-evkb`. The parent `rt1170/`
  directory is **not** a repo — run git from here (or `git -C evkb` from the
  parent).
- **Nothing else lives inside this repo.** The core (`teensy-cores`), the
  build macros (`teensy-cmake-macros`) and all peripheral libraries (Wire,
  SPI, Audio, SdFat, SD, Ethernet, NativeEthernet, FNET, lwip, USBHost_t36,
  …) are sibling checkouts under `$TEENSY_LIB_ROOT/<lib>` (default
  `~/Development/<lib>`), each its own repo. A reappearing `?? cores/` or
  `?? teensy-cmake-macros/` in git status is a STALE in-repo copy from before
  2026-08-14 — it is dead (nothing resolves there); delete it.

## Build (CMake only — no Arduino IDE)

Firmware lives in `examples/<category>/<name>/` (11 categories: dualcore, usb,
audio, camera, networking, storage-memory, gpio-analog, timing, serial,
display, framework). Each example is self-contained; build from its own
directory:

```sh
cd examples/gpio-analog/blink
cmake -B build -DCMAKE_TOOLCHAIN_FILE=../../../toolchain/rt1170-evkb.toolchain.cmake
cmake --build build          # produces <name>.elf (+ .hex)
```

The two toolchain files (`rt1170-evkb.toolchain.cmake`, `rt1062-evkb.toolchain.cmake`)
live once at the repo root, in `toolchain/`, and are shared by every example —
a new example needs no `toolchain/` directory of its own.

Build dirs configured before 2026-08-14 cached an absolute toolchain path that
no longer exists; their elfs remain valid (gates run them unchanged), but the
first reconfigure fails with "toolchain file not found" — `rm -rf` the build
dir and configure fresh with the command above.
★ **The same staleness also presents as `COMPILERPATH is UNDEFINED`** (from
`teensy-cmake-macros/CMakeLists.include.txt:72`), which does not mention a
toolchain at all — met 2026-08-16 in seven display build dirs when an
`evkb.cmake` pin bump triggered their first reconfigure since. Same cause, same
fix. Note what makes it easy to misread: these dirs had been passing gates all
along, because a gate runs the cached `.elf` and never reconfigures. **A green
sweep is not evidence that a build dir can still configure** — the two are
tested by different actions.

(`storage-memory/sd_test`, `audio/sd_wav_play_test`, `display/pxp_composite_test`
and `display/pxp_draw_bench` inline their toolchain, so none of the four need
the flag: `sd_test`/`sd_wav_play_test` `include()` the shared root file
directly, and the two PXP examples `set(CMAKE_TOOLCHAIN_FILE ...)` to it behind
an `if(NOT ...)` guard, so an explicit `-D` still wins.)

- Compiler: ARM GCC 10 at `/Applications/ARM_10/bin/` (override with
  `ARM_TOOLCHAIN_BIN`).
- Every example bootstraps via `../../../evkb.cmake`, which provides the
  `cores` library, the `teensy-cmake-macros` build macros, and
  `import_evkb_library(<name>)` for peripheral libraries.
- **Library resolution is local-first**: a `$TEENSY_LIB_ROOT/<lib>` checkout
  wins (default `~/Development`; env var to override — including uncommitted
  edits); if absent, the library is fetched from GitHub at a SHA pinned in
  `evkb.cmake`. This covers the core (`teensy-cores`) and the build macros
  (`teensy-cmake-macros`) too — the macros are the one repo fetched with plain
  FetchContent rather than CPM (the single CPM pin lives in the macros;
  `CPM_SOURCE_CACHE` covers everything else). `-DEVKB_FORCE_FETCH=ON` forces
  the pinned fetch ("fresh user" mode). After pushing new library work, the
  pin in `evkb.cmake` must be updated by hand.
- CM4 (second-core) images are built by the same macros
  (`teensy_add_cm4_image` / `teensy_add_cm4_slot_image`) and embedded into the
  CM7 ELF as C arrays.

## Test / verify — the two-gate rule

Every capability is verified twice, and both matter:

1. **QEMU gate** — each example has a `./run_qemu.sh` that boots the image on
   the custom `mimxrt1170-evk` QEMU machine (`~/Development/qemu2/`, from
   gitlab.com/Newdigate/qemu-rt1170) and asserts expected UART tokens.
   Run it as `./run_qemu.sh`, **never `sh run_qemu.sh`** — it re-execs itself
   under `gtimeout` (via `tools/gate-lib.sh`). QEMU runs through the
   `tools/qrun` wrapper (hard timeout + log cap) so a stuck gate can't burn CPU
   or fill the disk.
2. **Hardware verification** on the real EVKB with un-fakeable assertions
   (loopback jumpers, audible tones, real network traffic). Many examples keep
   `transcript_qemu.txt` / `transcript_hw_evkb.txt` as evidence.

**Silicon wins.** The QEMU model doesn't enforce clock gating/pin muxing
everywhere and stubs some devices; several real bugs only ever reproduced on
the board. Treat a QEMU pass as necessary but not sufficient — never weaken a
gate or the QEMU model to make a divergence disappear; document it instead.

There is a dedicated **`cm4-bringup` skill** — use it for any dual-core/CM4
work in this tree.

**★ Before running `./tools/run-all-qemu-gates.sh`, read
`docs/KNOWN-BROKEN-GATES.md`.** The sweep covers **128 gates** — the merge of
THREE independent lines, plus the first two Bluetooth gates, NEW-20's one new
gate, NEW-23's one, NEW-32's one, BT-3 phase 4's TWO (`audio/bt_tone_test`
card-absent + `[media]` — the A2DP media path), and BT-3's TWO more Bluetooth gates
(`networking/m2_hci_probe[baud]` and `[avdtp]`, taking that example from two
gates to four). The Arduino WiFi facade added THREE
(`networking/wifi_client_test` and its `[wifi]` variant — enumeration plus a
REAL 802.11 scan against the model's deliberate zero-BSS reply, asserting an
honest WL_NO_SSID_AVAIL — and `networking/wifi_server_test`); the uAP line added
EIGHT (below); and 108 came from the earlier merge of two lines that both
branched from 94 — the display
capstone line added THREE (`display/vglite_lvgl_test`, then
`display/synthui_step_test` and `display/acid_box` from the Acid Box capstone,
reaching 97 on its own), and the M.2 Wi-Fi line added ELEVEN, reaching 105 on
its own (94 + 3 + 11 = 108) — plus W17's TWO on the new
`networking/m2_uap_probe` and ONE on `networking/m2_uap_lwip`, then W18's FIVE
more once the QEMU model grew a uAP surface, a station and a readable TxPD tag.
That arithmetic is CHECKED against the runner rather than trusted: `-l` reports
128.

NEW-20 added ONE — `display/rotary_knob_bench`, the RotaryKnob render-strategy
bench: 12 cells ({vector,bitmap,strip} × {sw,gpu} × {notch,facet}) in ONE ELF,
LVGL kept software-only and the GC355 reached by DIRECT vg_lite calls behind
the chip-ID probe — which is what makes a single image safe everywhere where
vglite_lvgl_test needed a build split. Its gate pins the six SOFTWARE cells'
Phase A goldens and asserts the six GPU cells report an honest `gpu-absent`
(two tripwires: no `/gpu/` line may carry a result in QEMU, and `gpu_err=`
may never appear there). Phase B (the fps measurement) runs after `crc_done`
and is deliberately NOT gated — QEMU timing is meaningless; silicon is where
the bench's question is answered. 121 before it.
★ **NEW-20 Phase 2 (2026-08-27) shipped the production widget on the bench's
winner**: `synthui_rotary_knob` in SynthUI (notch variant, vector/gpu —
3 cached vg_lite paths + a per-frame matrix — behind an opt-in compositor,
LVGL-sw fallback everywhere else), old `synthui_knob` DELETED from SynthUI
(`synthui_knob_math.h` stays — the input layer reuses it), and THREE
consumers re-goldened: `synthui_knob_test` (now the rotary widget's
two-engine test, `import_evkb_synthui(VGLITE)`, engine tripwires),
`acid_box` (bounded ±140 stated explicitly — the widget's DC default range
is ±150), `vglite_lvgl_test` (mode × theme rows). `synthui_step_test` was
NOT touched — the issue listed it in error (it has no knobs) and its
unchanged golden is the cross-widget control. Gate count unchanged at 122.
★ **Wedge-delta damage followed the same evening** (spec+findings docs
2026-08-27): `set_angle` invalidates only the old∪new index-wedge boxes
(the notch discs are rotationally invariant), and the GPU compositor
scissors to the DISPLAY's actual rendered areas (`disp->inv_areas` — never
to widget-stored rects: LVGL joins areas into supersets, and a
stored-rect scissor leaves well-coloured holes; found in review before it
shipped). Measured on silicon before implementing: 180→45 ms/frame
(5.5→22.3 fps gpu), 310→71 ms (sw), on the all-16-knob worst case — the
LVGL sw floor was 96% of the frame and `vg_lite_finish` only 6.6 ms, so
pipelining is a dead lever for the vector strategy. Guarded in
`synthui_knob_test` by an EQUALITY check (delta-sequence CRC must equal a
fresh full render — gate-compared, never re-goldened) and an ENGAGEMENT
check (per-step damage ≤8000 px), both demonstrated RED. No other golden
moved — consumers gate only fresh full renders.
★ **The GPU well followed (2026-08-28) and MET the ≥30 fps criterion:
42.4 fps** on the all-16-knob worst case (the compositor draws well+rotor;
sw side is ground fill only). Three silicon defects on the way, ALL caught
by the delta equality guard run per boot, none visible in QEMU: the VGLite
port's wait consumed stale IRQ flags (fixed: flag AND `AQHiIdle` idle,
VGLite 2e17773); a winding-2 multi-subpath track path was the machine's one
source of per-boot render nondeterminism (ten boots, ten checksums — fixed:
single-contour track, SynthUI 7856f35); the apparent scissor-window fill
inversion was fallout of that track, so wedge-delta damage ships on both
engines. ★ GPU goldens in `synthui_knob_test` now require REPEATED-boot
stability — one defect hid behind exactly one boot. ★ Bench reality: the
MCU-Link DAP wedges after repeated flash/run/kill cycles (fix: replug the
DEBUG USB — a board power cycle does NOT clear it, and once it took BOTH
cold together); pyocd and gdb `monitor reset` cannot reboot this target, so
unattended reset loops do not exist — SW4-press loops with a persistent
console reader are the wedge-free procedure.
★ **The scanout flash and the tear-free pipeline (2026-08-28 evening)**: a
compositor drawing into LIVE scanout flashes damage-box squares (~1/s beat
between refresh and 60 Hz scan) that NO CHECKSUM can see — CRCs read after
finish; only a camera/eye catches it (60 fps video + frame extraction is
the instrument). Fixed structurally: `synthui_knob_test` now runs
`lvgl_mipi_panel_create_db()` with the port's new PRE-FLIP compose hook —
the compositor's deferred mode draws into the off-screen back buffer before
the flip, so scanout only ever presents complete frames. Pixel-neutral
(every golden held, QEMU and silicon), `rk_vsync … timeouts=0` is gated,
and `rk_fps` now measures the vsync-locked pipeline (32.1 fps; unfenced
compute ~42). Checksums in db mode must read the PRESENTED buffer
(`flip_sync()` + `scanned_fb()`), never `Display.framebuffer()`.
★ **`display/acid_box` joined the same pipeline the next day (2026-08-28)**:
`import_evkb_synthui(VGLITE)`, `create_db` + deferred compositor behind the
chip-ID probe. Its QEMU golden HELD (0x25B30A96 — sw path untouched, gate
grew engine/GPU tripwires and per-bar `ACIDBOX_VSYNC timeouts=0` witnesses,
all demonstrated RED), but its famous QEMU↔silicon bit-identity is GONE BY
DESIGN: silicon now composites knobs on the GC355 and owns a separate gpu
golden (0x1479CEE8, four boots bit-identical). Gate count unchanged.

NEW-23 added ONE — `display/synthui_fader_test`, the SynthUI Fader's
sw-delta gate (spec 2026-08-29): ONE bank golden with every config axis
inside it (three states, center, four panel greys, three tick counts), the
delta-equality guard (a 66-step LCG sequence's checksum must EQUAL a fresh
full render — gate-compared, never re-goldened), the engagement bound
(max single invalidated area ≤6000 px; measured 3234, a full-invalidate
revert measures 16380 and the equality guard stays GREEN on it — only the
engagement check sees that defect), and the vsync witness. The widget is
LVGL-sw ONLY by design — no GPU TU unless the silicon checkpoint misses
30 fps. All three guards demonstrated RED by name; the RED probes also
measured the cap-extent's real sensitivity floor (2 < floor ≤ 6 units —
the +2 px damage inflation absorbs cuts under 2 px). 122 before it.
★ **The silicon checkpoint MISSED, and the widget gained the GPU TU it was
designed to avoid.** The sw path measured 11 fps against a ~76 ms fixed
per-refresh LVGL cost (uncached-SDRAM draw-task churn, ~90 µs/task —
diagnosed with the Phase C probes and filed as a separate platform issue),
so the fader gained a GC355 compositor (`import_evkb_synthui(VGLITE)`,
deferred pre-flip compose) which MET the criterion at `mfps_med=30448`
against the `>= 30000` bound, with `fd_delta_eq=PASS` and goldens
bit-identical across four SW4 boots (then `fd_crc=0x141D0A41`,
`delta==fresh=0xE929A5E4`). QEMU still asserts `fd_engine=sw` and the sw
goldens are unmoved; the GPU set is recorded only in
`transcript_hw_evkb.txt` (two golden sets, never reconciled — same
discipline as `vglite_lvgl_test` and `acid_box`).
★ **THOSE TWO GPU GOLDENS MOVED ON 2026-09-02** — deliberately, by NEW-32
Phase 4's premultiply fix: `fd_crc=0x814F4047`, `delta==fresh=0xE9A9A2B5`,
measured over TWO boots × FOUR passes, all eight bit-identical, with
`fd_delta_eq=PASS` throughout. `fd_damage max=3234` and `mfps_med=30448` are
UNCHANGED — the cost is three multiplies per DRAW CALL, not per pixel, and
the median is vsync-quantised anyway. The sw goldens and the QEMU gate did
not move at all.

NEW-32 Phase 1 added ONE — `display/vglite_conformance`, the GC355/VGLite
conformance harness (spec 2026-08-30): a case table of `{id, run, check}`
triples rendering one at a time into a 128×128 BGRA8888 EXTMEM scratch, each
case printing TWO INDEPENDENT VERDICTS — `api=` (what the driver said) and
`pixel=` (what a structural CPU-side predicate found) — because every GC355
defect this tree has hit reported success while producing the wrong picture.
Phase 1 is thirteen paths/contours/winding cases; Phase 1b (2026-09-01) added two more and answered the mechanism question, and Phase 2 (2026-09-01) added FIVE COLOUR/BLEND cases, taking the matrix to TWENTY. Its gate asserts the HONEST
NEGATIVE (`vgc_engine=absent`, all thirteen `pixel=skip`) with three tripwires
— no case may report `pixel=ok`, none may report `pixel=broken`, and no
`api=`/`api2=` may say `success` with no GPU — plus a case-line COUNT check and
a `case_begin`-vs-`case` equality check, since every tripwire above is
satisfied VACUOUSLY by an empty matrix and an unfinished case is how a GPU hang
would present. 123 before it.
★ **No panel.** The core's startup brings up the SEMC SDRAM before `setup()`,
so EXTMEM is live without `Display.begin()`. It is the only **VGLite** example
that links neither MipiDisplay nor LVGL — `vglite_probe` and `vglite_lvgl_test`
both do. (Seven non-VGLite display examples also link neither: the four `pxp_*`
ones, both `camera_preview_*` and `ssd1306_display`.)
★ **The tessellation buffer is 64×64 against a 128×128 target, deliberately.**
A tess buffer ≥ the target puts the driver in its `ts_is_fullscreen == 1`
regime, where scissor left/top clamping is silently disabled — a different
machine from the one the shipping compositors run on (720×1280 target, 256×256
tess). Phase 3's `scissor/tess-fullscreen` case probes the other regime on
purpose.
★ **QEMU cannot reach one line of the pixel logic** (every case reports
`pixel=skip`), so the example carries FIVE HOST SUITES run by
`examples/display/vglite_conformance/tests/run.sh` — **440 checks** over the pure
predicates, the colour predicates, the path arena, the path case geometry and
the colour case geometry. The geometry suite
compiles the REAL case functions against model rasterisers — the path suite
against four (correct / first-contour-only / draws-nothing / stray-ink) and the
colour suite against seven, including one per admissible blend reading. A
correct GPU (all fifteen path cases `ok`), a first-contour-only GPU modelling this
GC355's known defect (the probe cases must go BROKEN **by name**, every control
must stay `ok`), and a GPU that draws nothing. **The negative arms are the
point** — a positive-only suite is equally consistent with a matrix that cannot
detect anything, demonstrated: a case hard-wired to `VGC_OK` leaves arm 1 green
and is caught only by arm 3.
★ It says NOTHING about what the silicon does. The silicon matrix is ONE boot,
diffed against the PRE-REGISTERED `expected_silicon.txt` by
`tools/vglite-conformance-check.sh`, which fails on drift in EITHER direction:
a quirk that silently DISAPPEARS after an SDK bump matters as much as a new
one, because it means the driver changed under us.
`docs/gc355-vglite-quirks.md` is the reference the matrix feeds.

BT-1 added the tree's FIRST TWO BLUETOOTH GATES, both on the new
`networking/m2_hci_probe`. `run_qemu.sh` is the card-ABSENT fallback — with no
second `-serial`, LPUART2 has no chardev, so `HCI_Reset` times out BY NAME after
ten counted attempts and the image heartbeats afterwards (the positive tokens:
"no identity printed" is also what a dead image produces). `[hci]` attaches a
Python fake controller to LPUART2 through `-serial unix:…,server` and runs FOUR
phases in four QEMU boots: `full` (every field the firmware prints carries the
PEER's value — manufacturer 0x1234, bd 11:22:33:44:55:66, two field-major
inquiry results and their names — values this firmware cannot invent),
`drop-reset` (times out by name, peer sees all ten Resets), `garbage` (attempt 1
fails as FRAMING, attempt 2 succeeds after the 50 ms resync) and `starve`
(`ncmd=0` starves every later command, by name).
★ **It needs NO qemu2 change.** qemu2 binds the second `-serial` to LPUART2
(`hw/arm/fsl-imxrt1170.c`) and `imxrt_lpuart.c` implements chardev RX, so the
whole Bluetooth transport is gateable against stock qemu2 — unlike every other
m2_* gate that proves the device WORKS, all of which need the IW416 model. (The
card-ABSENT m2_* gates pass on stock QEMU too; it is the working-device half
that the model exists for.) `server` WITHOUT `nowait` holds the
guest until the peer connects, which is what makes the counter assertions
strict rather than racy.
★ **The socket lives in `/tmp`, deliberately** — same `sun_path` 104-byte cap
that forces the four `mon.sock` gates through a short-path symlink; putting it
under the example directory would reintroduce that hazard for no gain.
★ **BOTH GATES ONCE SKIPPED ON A FRESH CLONE — RESOLVED 2026-08-24, and the
resolution is worth keeping because the class recurs.** `m2_hci_probe` is the
first example to link `M2Radio/hci/`, and it calls `addMemoryForRead()` on the
core; neither existed at the SHAs `evkb.cmake` used to pin (`M2Radio` 300d32b
had no `hci/`; `cores` fcd22b0 had no `addMemoryForRead`), so a fresh clone
could not build the example and the sweep reported **2 SKIP** — and a SKIP
hides in a count, which is why the line was written. Both libraries are now
PUSHED (`M2Radio` **6ff9ade**, 13 commits; `cores` **36e480d**, 2) and both
pins bumped.
★ **VERIFIED THE ONLY WAY THAT COUNTS — by running the fresh-user path, not by
reading the SHAs.** `-DEVKB_FORCE_FETCH=ON` in a scratch build directory cloned
both repos from GitHub at the new pins, compiled clean, and BOTH GATES WERE THEN
RUN AGAINST THAT FETCHED-SOURCE ELF (`build` symlinked to it, then restored):
`run_qemu.sh` PASS and `run_qemu_hci.sh` PASS. A configure that succeeds proves
the subdirectory resolves; only running the gate proves the fetched code
behaves. `0 SKIP` is achievable on a clean machine again.
DEMONSTRATED RED twice: changing the fake's manufacturer failed `[full]` by
name, and breaking the driver's opcode match failed `[hci]` while
**`run_qemu.sh` stayed GREEN** — it has no replies to match, so it cannot see
that bug. That asymmetry is the whole argument for the variant existing.
★ **THE BAUD SWEEP (2026-08-25).** When Reset fails at 115200 the probe now
escalates through 3000000/921600/460800/115200 and, on the first rate that
answers, re-runs identity and inquiry there. This exists because u-blox's own
bring-up for this module attaches the controller at **3 Mbaud**
(MAYA-W1 SIM UBX-21010495 R09 §4.4.6, after §4.4.3's combo image goes over
SDIO) and **every probe in this tree before that date used 115200 only** — a
controller at 3 Mbaud decodes nothing sent at 115200, which is exactly the
`n=0 framing=0` silence on record.
★ **It is an ESCALATION, not an unconditional sweep, and that is load-bearing
for the gates rather than tidiness.** `[garbage]` scripts its corruption
against the FIRST command the peer sees, so a sweep in front of the main
sequence would absorb it and that phase would silently stop testing the resync
it exists for. Measured: unconditional, `[full]` went from `cmds=7` to
`cmds=8` immediately. `[full]` now asserts the sweep does NOT run.
★ **QEMU'S CHARDEV HAS NO BAUD**, so no gate here can show a RATE is correct —
only silicon can. What is gated is that all four are attempted (`[drop-reset]`
asserts `cmds=14 resets=14`: ten counted attempts plus one per rate, counted by
the PEER, so it cannot be satisfied by a sweep that merely prints cells), that
none is claimed without a reply, and that the counters survive.
★ Counters are **cumulative across `Hci::begin()`** because the sweep calls it
per rate. Without that the heartbeat printed `timeouts=0` on a run with
fourteen real timeouts — a counter reading zero where the failures were real is
worse than no counter, since it reads as a healthy idle link.
DEMONSTRATED RED three ways (sweep claiming every rate; bases removed; sweep
made unconditional), each failing by name — quoted in the gate headers.
★ **RUN ON SILICON 2026-08-25 AND REFUTED**, in three bench runs (u-blox's
combo-over-SDIO path; the BT-only UART download; that download with CTS
asserted in the corrected order): `bt_baud=none tried=4` every time.
★ **The negative is worth more than the hypothesis was.** A controller talking
at an unmatched rate gives FRAMING faults — garbage sampled at the wrong
phase. Four rates gave `framing=0` and zero bytes, so the card transmits
NOTHING after a fully successful download, rather than "nothing we could
decode". Keep the sweep anyway: it is cheap, gated, and it is what made that
distinction visible.
★ **WHICH FIRMWARE, AND WHERE FROM** (recorded 2026-08-25 — it had never been
written down): MCUXpresso SDK **v26.06.00-LTS**
(`~/Development/mcuxsdk-ws`, `components/conn_fwloader/fw_bin/inc/IW416/`),
version **`16.92.21.p155.2`** FP92, built 2026-03-12, read out of the images'
own ID strings rather than inferred from filenames. The combo
(`sduartIW416_wlan_bt.bin`, 411,064 B) carries BOTH build IDs — `w8978o-V0`
WLAN and `w8978d-V0 … BT_UART` — with timestamps identical to the standalone
`sdIW416_wlan.bin` (279,164 B) and `uartIW416_bt.bin` (131,840 B). It is
therefore both radios' firmware, **LZMA-compressed, not concatenated** — which
is why byte prefix/suffix checks found nothing and why an earlier note here
saying the combo does not "contain" the BT image was wrong.
★ **The RT1060 SDK 26.03 on this machine ships the IDENTICAL firmware**, so
"try another build" is not a local experiment. Never commit these blobs; they
are supplied at configure time only.

★★ **NXP'S OWN SHELL EXAMPLE HANGS AT `bt init` ON THIS BOARD** (2026-08-25),
and it is the cleanest artefact this investigation has produced. Built from
`middleware/edgefast_open/examples/shell` with `--config flexspi_nor_debug`,
IW416 + `board_murata_1xk_m2`. It boots and reaches `uart:~$`, then `bt init`
hangs forever and the shell dies — **with no debug output at all**, despite
`CONFIG_LOG=y` and `CONFIG_BT_HCI_DRIVER_LOG_LEVEL=4` (DBG). SWD says
`DHCSR 0x01010001` (healthy-running, `S_HALT`/`S_LOCKUP` clear) and
`CFSR`/`HFSR` zero — alive and blocked, not crashed.
★ It also closes the "HCI passthrough" idea: the shell offers exactly that
diagnostic (`bt hci-cmd <ogf> <ocf>`, arbitrary vendor commands incl. OGF
0x3F) and it is UNREACHABLE, because it needs the transport that `bt init`
fails to produce. Our probe already runs the other half — a raw `01 03 0C 00`
at the bootstrap rate right after the download, three times per run,
`bt_raw_reset[0..2]: n=0` every time.
★ Use the shell, not `a2dp_source`, for any further vendor-stack work: it is
the smallest example, it reaches a prompt, and its BT stack starts on command
so the failure can be provoked deliberately.

★★ **THE COMBO IMAGE OVER SDIO DOES NOT BRING UP BLUETOOTH ON THIS CARD —
measured 2026-08-25 in BYTES, not inferred.** With the combo image downloaded
and the WLAN side running (`fw_download=ok card=1`), the BT UART emits a FOURTH
`AB 01 72 00 47` — a fresh ROM start indication. The BT core is still in its
bootloader, still asking for a UART download. That contradicts NXP's
`controller_wifi_nxp.c` (whose premise is that BT is up after the combo
download) and u-blox's own SIM §4.4.3/§4.4.6 procedure, and it VINDICATES
BT-1's pivot to the `CONFIG_BT_IND_DNLD` UART path.
★ **The contrast between the paths is the diagnostic**, and it tells us what
rejection looks like on this card: combo → the ROM keeps announcing; our UART
download → the ROM goes SILENT and never re-greets (`bt_post_dnld[0..3]` all
`n=0`, four 500 ms windows). A ROM that rejected an image restarts and
announces — so ours is ACCEPTED and the bootloader is exited. **The failure is
therefore after the jump**, not in transport, delivery, or arrival.
★ Every claim about the combo path before this was based on the Hci driver's
`framing` counter read long afterwards — which can say "something unparseable
arrived" but not what. `m2DumpSerial2()` dumps the raw UART at power-up and
after the SDIO download, on both paths, which is what turned an inference into
a reading.

★ **`networking/m2_hci_probe` ALSO BUILDS FOR rt1062 — as a BENCH build, not a
gate.** `-DEVKB_BOARD=rt1062` links `M2Radio/hci` only (SdioHost is RT1176-only,
so the Wi-Fi half compiles out and says `sdio_begin=not_built` rather than being
silently absent). The BT UART is `Serial2` on both boards — LPUART2 on the 1176,
**LPUART3 on the 1062** (`GPIO_AD_B1_06/07`, core pins 17/16) — so all transport,
loader and HCI code is board-independent.
★ `rt1062` is deliberately NOT in the example's `boards` sidecar: it would create
a gate whose script asserts rt1176-specific lines and go red for reasons
unrelated to Bluetooth.
★ **The MIMXRT1060-EVKB needs THREE hand reworks and STILL does not greet.**
`R345` (GPIO_AD_B1_03 → WL_RST# → J8.56 PDn) and `R96` (the card→MCU RX leg,
between the level shifter and `R200`) are both DNP from the factory and were
bridged. `R343`/`R344` were also removed — **unnecessarily, on a wrong diagnosis
of mine** (see the GPIO6 trap above). With all of that, the continuity probe
says the RX line is held high externally (an idle UART) and PDn swings when read
back at the pad — and the card is **silent for 3 s** after every reset.
★ **That is not evidence about the module**: the same physical card greets
reliably on the 1170 (`start_inds=2` or 3, five sessions). The 1060 is simply not
equivalent yet, in a way no software instrument here has surfaced. Further work
there needs a scope on J8.22/J8.56, not more firmware.
★ `M2_CONTINUITY_PROBE` (default ON for rt1062, OFF for rt1176 so the two gates
are undisturbed) is the instrument that settles "is this line actually driven" —
the same technique that proved the 1170's R1901 bridge conducted.

★ **`M2_BT_WAKE_PULSE` (default ON) is NXP's boot-sleep wake**, found 2026-08-25
by reading their loader's CALL ORDER: `uart_fw_download()` calls
`wakeUpControllerFromBootSleep()` BEFORE the image, and for the RT1170 that is
a 10 ms LOW pulse on **GPIO_DISP_B2_13** with the pad then returned to
`LPUART2_RTS_B`. That pad is the one this tree had been calling "the card's CTS
input" — NXP do not treat it as flow control here at all.
★ **It is the only thing that has ever changed the card's behaviour**: with the
pulse the card greets an extra time (`start_inds=3` vs `2` in two controls run
the same session, one variable). So the pin is connected and the card listens
on it. **And it changes nothing that matters** — the image still lands and the
controller still never answers, so "we were missing NXP's wake step" is
refuted too. n=1 per arm; repeat before leaning on the delta.

★ `M2_BT_UART_DNLD` (default ON) selects the firmware path — OFF takes
u-blox's combo-over-SDIO route instead of the BT-only UART download. The two
are **NOT** alternatives — corrected 2026-08-25. A BT UART download leaves the
later WLAN SDIO download at `fw_download=cmd-timeout` **only when the COMBO
image is used**; with the WLAN-ONLY `sdIW416_wlan.bin` it succeeds
(`fw_download=ok card=1`). That is precisely NXP's `CONFIG_BT_IND_DNLD` mode,
whose `wlan_bt_fw.h` includes BOTH `sdIW416_wlan.h` AND `uartIW416_bt.h` — one
image per bus. **Pair `M2_BT_UART_DNLD=ON` with the WLAN-ONLY image**, never
the combo.
★ With that pairing both radios are correctly provisioned — Wi-Fi running
(`card=1`), BT firmware accepted and the loader exited (`bt_uart_postsdio n=0`,
no re-greet) — and Bluetooth **still answers nothing** at any of four rates.
That is the vendor's own configuration, reproduced exactly, still failing.

★ **Two real driver bugs were found by these gates, not by review** — both the
same disease, and worth knowing because the symptom is silence rather than a
crash. `Num_HCI_Command_Packets` is assigned ABSOLUTELY from each reply, so any
reply that is lost or discarded leaves the credit stuck at zero with nothing
able to raise it: no credit means no command, and no command means no reply.
A framing fault hit it first (found in review); a TIMEOUT hit it too, and that
one turned the example's ten-attempt retry loop into a single attempt —
measured `timeouts=1 starved=9`, one command ever reaching the wire. On the
bench that would have reported a perfectly good card as dead. 119 before them;

The M.2 line's own chain, kept because each step says what the gate is for:

W18 finally added `networking/m2_uap_lwip[tx]`, which closes the last part of the
uAP driver that was silicon-only. The model now READS the TxPD's `bss_type` and
its `tx-loopback` echoes each frame back ON THE INTERFACE IT WAS SENT ON, so the
tag that returns is the tag that went out. That round trip is the only thing a
host can observe that distinguishes "addressed the uAP" from "addressed the
station and got lucky" — a status code proves the card took a buffer and nothing
more. Discrimination is total and was measured before the gate was written:
correct → `rx_bss0=0 rx_bss1=71 unrouted=0`; the uAP netif calling
`sendDataFrame()` instead → `rx_bss0=71 rx_bss1=0 unrouted=71`. 115 before it;
★ `UAP_TX_PROBE` is ON by default and that is a SKIP-rule decision, not a
preference: a gate whose example was configured without it would report SKIP
rather than FAIL, and a SKIP hides in a count. 

W18 later added TWO more on `networking/m2_uap_lwip` once the model could TAG an
injected frame (`-global iw416-sdio.inject-bss=`), and they are a PAIR that must
be read together. `[data]` injects frames tagged bss_type=1 and asserts they
reach the uAP netif. `[mistag]` injects them tagged bss_type=0 while only a uAP
netif exists, and asserts they are REFUSED and COUNTED — that is the W17
handoff's named hazard ("RX frames from AP clients would be silently
mis-delivered to the STA netif"), and this driver did exactly that before W17.
★ ONLY `[mistag]` CAN CATCH IT. Measured, not assumed: against a demux re-broken
to ignore the tag, `[mistag]` fails by name and `[data]` still PASSES — because
there every frame belongs where it lands. A gate that only tests the happy
routing proves nothing about routing. `[data]` earns its place separately, by
failing when the tag is read from the wrong RxPD offset. 113 before them;

W18 added TWO gates once the QEMU IW416 model grew an AP surface
(`-global iw416-sdio.uap=on`, qemu2 after `721fb09146`), which is what finally
made the uAP work gateable rather than silicon-only:
`networking/m2_uap_probe[uap]` is the **FAULT 1 REGRESSION** — a POPULATED
SYS_CONFIGURE is accepted and a MINIMAL one WEDGES the command port, exactly as
silicon does, so a driver that reintroduces a config GET fails here instead of
on a bench (there is no harmless probe of that command). Its tally matches the
silicon run byte for byte: `bracketed=6 distinct_from_neg=6 unbracketed=4`.
`networking/m2_uap_lwip[uap]` asserts the bring-up SEQUENCE — configure →
BSS_START → netif → socket → DHCP — and asserts the ORDER, since configuring
after starting would be a different and broken driver these greps would
otherwise accept. Both DEMONSTRATED RED against a `uapConfigure()` re-broken to
drop its SSID TLV. 111 before them;
★ The `uap` property is OFF by default, so `m2_uap_probe[wifi]` keeps asserting
its correct NEGATIVE and its "SUPPORTED must never appear" tripwire stays valid.
★ `m2_uap_probe[uap]` waits for `^uap_verdict=`, NOT for the heartbeat: after the
wedge the probe spends ~90 s retrying before it reaches `loop()`, and qrun caps
QEMU at 60 s (`QRUN_TIMEOUT`), so waiting for `hb` means waiting out the poll
loop for a run QEMU was already killed under. Measured — the first version of
that gate did exactly that.

W17 Phase 1 added `networking/m2_uap_lwip` and ONE gate — the card-ABSENT
fallback, whose VACUITY GUARDS are its substance rather than a formality. That
example's purpose is to TRANSMIT (it hosts an open AP indefinitely), so the gate
asserts that every AP line — configure, start, hosting, netif, socket — is
ABSENT with no card, which a happy-path check would not have said. DEMONSTRATED
RED by appending a fake `uap_hosting` line to the capture. 110 before it;

W17 Phase 0 added `networking/m2_uap_probe` and its TWO gates — `run_qemu.sh`
(card-absent, and it asserts the VACUITY guards: no probe cell and no verdict
may be invented with no card to ask) and `run_qemu_wifi.sh`, which runs the
whole probe against the IW416 model and requires it to reach a correct
NEGATIVE. That second gate exists for one reason: it is the only automated
proof that a negative Phase-0 answer is REACHABLE and readable. A probe that
could only ever print SUPPORTED would make its silicon answer worthless.
DEMONSTRATED RED against a deliberately re-broken `sendHostCmdBss()` (bss
nibble OR'd into seq_num's low nibble instead of shifted to 15:12): every
`bss=1` cell went `st=cmd-timeout` and the gate failed by name.
★ And note what did NOT catch it — `uap_verdict=` was UNCHANGED, because half
the matrix vanishing does not move a verdict computed from what is left. The
per-cell assertions and the tally did. 108 before that;

W16 added THREE more to `networking/m2_rx_demo` — `run_qemu_rxaggr.sh` and
`run_qemu_txaggr.sh` for multiport aggregation in each direction, and
`run_qemu_regfallback.sh` for the driver's detection of the one thing W16
assumes about the card and cannot verify; 101 before W15
phase 2 added a FOURTH gate to `networking/m2_rx_demo` — `run_qemu_irq.sh`, the
interrupt-driven-SDIO-service win, which divides service CMD52s by the frames
the same window delivered and so cannot be satisfied by a driver that merely
polls faster; 98 before W14
phase 2 added `networking/m2_rx_demo` and its THREE gates at once —
`run_qemu.sh` (card-absent fallback), `run_qemu_ring.sh` (the W8 32-slot-ring
regression) and `run_qemu_stranded.sh` (the W12 stranded-upload regression);
97 before W14's
QEMU IW416 card model added a SECOND gate to `networking/m2_sdio_probe` —
`run_qemu_wifi.sh`, the first example in the tree to own two; 96 before W11's
throughput example added `networking/m2_throughput_test`; 95 before W9's
lwip-over-Wi-Fi bridge added `networking/m2_lwip_test`; 94 before the
M.2 SDIO probe added `networking/m2_sdio_probe`; 93 before
VGLite Phase 1 added `display/vglite_probe`; 92 before the
SynthUI Knob pilot added `display/synthui_knob_test`; 91 before the
step sequencer added `audio/step_seq_test`; 90 before the
transport added `audio/transport_test`; 89 before the
acid-bass voice added `audio/acid_bass_test`; 87 before Phase 5b
gated `usb/usb_audio_capstone_test` and `audio/audioinput_i2s_test`'s second
board; 86 before Phase 5a gated
`audio/audiooutput_i2s_test` on two boards; 84 before Phase 4 gated
`usb/usb_audio_uac1_test` on two boards; 83 before
`framework/string_test` was gated on a second board; 82 before Phase 2
gated `usb/usb_descriptor_survey` on a second board; 81 before the
RT1060 board axis gated `serial/serial_test` on a second board; 80 before Phase
7.4 added `dualcore/cm4_graph_usb_capstone`; 79 before Phase
7.3 added `dualcore/cm4_usb_audio_probe`; 78 before Phase
7.2c added `dualcore/cm4_usb_enum_probe`; 77 before Phase 7.1 added
`dualcore/cm4_usb_irq_probe`; 75 before Stage C added
`usb/usb_audio_duplex_test` and the emulated-device gate on
`usb/usb_descriptor_survey`). The target is **128 passed, 0 failed, 0 SKIP**, or
**127 passed, 1 failed, 0 SKIP** when the nondeterministic dual-core gate
(`cm4_audio_test`) is red.

★ **That target is for THIS machine.** `display/acid_box` joins the standing
fresh-clone-red set: its injected gestures come from the `touch-script`
property on qemu2's `imxrt.gt911`, a LOCAL-ONLY change kept off this repo by
the GPL firewall, exactly like `sai1-rxinject` and the rt1062 halves of
`usb/usb_descriptor_survey` and `dualcore/cm4_usb_irq_probe`. A clean clone
sees it red for that reason and not as a regression.

★ **A gate id now carries a `[variant]` suffix when — and only when — its
example directory holds more than one `run_qemu*.sh`.** W14 made
`networking/m2_sdio_probe` the first such example (`run_qemu.sh` asserts the
card-ABSENT fallback, `run_qemu_wifi.sh` asserts real enumeration against the
opt-in IW416 model), and it exposed a masking bug in the sweep runner: the id is
the slug, the slug names the `.result` file, so two gates in one directory wrote
the SAME file and the summary read it twice. Measured before the fix, with one
of the two deliberately failing: `gates: 2 passed`, **exit 0**, with the FAIL
line printed right there above the summary. A failing gate reported as a pass is
the worst outcome this runner can have, so discovery now suffixes the variants —
`rt1176:networking/m2_sdio_probe` and `rt1176:networking/m2_sdio_probe[wifi]`.
Directories with exactly one script are untouched, so all 97 pre-existing ids are
byte-identical (diffed, not assumed), including the ~38 already named for what
they test (`run_qemu_lwip.sh`, `run_qemu_usb.sh`, …).
W14 phase 2 exercised that suffixing further: `networking/m2_rx_demo` owns
**SEVEN** scripts (W15 phase 2 added the fourth, W16 the last three), and lists
as `rt1176:networking/m2_rx_demo`, `…[ring]`, `…[stranded]`, `…[irq]`,
`…[rxaggr]`, `…[txaggr]` and `…[regfallback]`.

✅ **Measured 2026-09-04 (later the same day): 128 gates discovered, 128 passed,
0 failed, 0 SKIP** (`gates: 128 passed`, exit 0; `-l` reports 128), on the
**acid_box Bluetooth dual-output capstone** close-out (`display/acid_box
-DM2_BT_OUT=ON` -- the acid synth streamed to a real Shokz OpenMove A2DP headset
WHILE the SynthUI renders). **No new gate**: `M2_BT_OUT` is a BENCH build (the
`display/acid_box` gate builds the default, BT-OFF image -- byte-identical, its
golden unchanged), and the `bt_tone_test` decouple + the M2Radio `BtLink` change
add none. `LICENSE-AUDIT: PASS`. All four `m2_hci_probe` gates and both
`bt_tone_test` gates green against the changed M2Radio.
★ **The acceptance is SILICON-ONLY and un-fakeable** -- a clean acid-bass line
audible on the Shokz (by ear) WHILE the panel renders (`ACIDBOX_VSYNC timeouts=0`),
the encoder at real time (`blocks` +345/s == 44100/128), ongoing `pcmdrops=0`,
media `drops=0`. THREE silicon-only fixes, none visible to any QEMU gate (the gates
run the SW audio path, and acid_box's gate has no BT):
  1. **update() must skip all work until begin()** -- acid_box clocks the graph
     from the SAI DMA ISR, so an eager `AudioOutputBluetooth::update()` encoded SBC
     from power-on and starved `setup()` (black screen; SWD: `m_blocks`=77k,
     `m_l2`=null).
  2. **DECOUPLE the SBC encode out of the audio ISR** -- `update()` now only copies
     PCM into a 32-slot SPSC ring; `poll()` (main loop) encodes + drains. A
     flash-resident encode IN the SAI ISR overran the 2.9 ms block period and
     LIVELOCKED the loop (ring full, ~99% dropped, loop frozen right after
     `begin()` -- `s_btBegun` never set). `bt_tone_test` is behaviourally unchanged
     (its self-clocked `poll()` fills then drains one block per call); both its
     gates pass, media framing byte-valid.
  3. **Keep the SBC encode in ITCM** -- route ONLY `Sbc.cpp` (the one
     per-audio-block-hot object, 344/s) to ITCM; evict the example's own `setup()`
     (2564 B, runs once) to flash for the ~1.6 KB of room. THE dominant lever: a
     flash-resident encode is ~2-3 ms/block vs ~0.5 ms in ITCM, and at 344/s that
     alone saturates the CM7 before the GC355 compositor runs (`pcmdrops` 1739 -> 0
     ongoing). The M2Radio `BtLink` Number_Of_Completed_Packets(0x13)-flood
     suppression was MEASURED SECONDARY -- removing it alone changed the encode
     ratio not at all.
★ **The plan's stated fallback was WRONG and is recorded so**: "drop the local I2S
(BT-only) if the CM7 can't keep up" does not help -- muting the WM8962 does not
reduce the SBC encode, which is the actual cost. The levers were encode PLACEMENT
(ITCM) and DECOUPLING it from the ISR. M2Radio `a7cbe03` / evkb (decouple, acid_box
perf, pin) pushed, fresh-user `-DEVKB_FORCE_FETCH=ON` verified.

✅ **Measured 2026-09-04: 128 gates discovered, 128 passed, 0 failed, 0 SKIP**
(`gates: 128 passed`, exit 0; `-l` reports 128), on the **BT-3/NEW-9 headset
AVDTP DISCOVER fix** close-out. No new gate — the `[avdtp]` fake peer
(`hci_peer.py`) was TIGHTENED to model the Shokz OpenMove (from a Mac→Shokz
Apple PacketLogger reference): it SDP-queries our AudioSource record and answers
DISCOVER only after that completes, lists its MPEG SEP BEFORE its SBC one, sends
GET_ALL_CAPABILITIES with delay reporting, and holds the OPEN accept until we
accept its DelayReport — so the gate now asserts `order=1,12,12,3,6,7`,
`sdp_served=1 delay_report=2000`, the SET_CONFIG SEID + delay-reporting bytes,
and a Config-Request-MTU tripwire. **DEMONSTRATED RED five ways** (SDP server
silent = the bench symptom; option-less CONFIG_REQ; wrong-SEP config; 0x02 vs
0x0C; DelayReport ignored). `[media]` carries the same tripwires. `LICENSE-AUDIT:
PASS`. The five M2Radio fixes (L2CAP MTU option + 5 channels; Avdtp
GetAllCapabilities + SEP-walk + delay reporting + DelayReport-accept; Sdp server;
BtLink page-timeout/cancel-a-silent-page/credit-reclaim + disconnect) are
**VERIFIED ON SILICON**: a real Shokz OpenMove reaches AVDTP STREAMING and plays a
**clean, audible 1 kHz tone** at the real-time rate (344 SBC frames/s = 44100/128),
every fix decoded from the `M2_BT_ACL_TRACE` capture, and the ESP32 sink shows
no-regression (reaches STREAMING through the same new negotiation; its throughput
ceiling is its own, unchanged).
★★ **THE FIRST BENCH RUN CRACKLED, AND IT WAS THE TRACE, NOT THE STACK — an
observer effect worth keeping.** With `M2_BT_ACL_TRACE=ON`, every 132-byte media
packet became a ~439-char console line; at 115200 baud that print takes ~38 ms
and `CONSOLE.print` BLOCKS when its TX buffer fills, INSIDE L2cap's send loop —
pacing media to `11520/439 = ~26 packets/s`, a ~13× underrun → a ~26 Hz crackle.
The measured media cadence was **38.95 ms**, an exact match to the print time,
and the SBC content was always a clean 1 kHz sine (ffmpeg-decoded from the trace).
The `drops=0`/`hw=1`/26-fps figures first recorded here were that THROTTLED run —
`drops=0` only means the packetiser did not drop, NOT that the audio was clean.
Fix: the trace now SKIPS RTP media (payload `0x80 0x60`); the default build never
traced. Trace-free: 344 fps, drops=0 for the first ~130 s (minor bursts after),
audible clean tone. **Never per-packet-console-trace a high-rate BT stream.**
★ **RESIDUAL-DROPS FIX (batch to full packets).** Trace-free, the tone held
drops=0 for ~130 s then shed ~1.7%. A diagnostic build (per-second rates + min
ACL credits) showed the cause: at ONE SBC frame per RTP packet we sent ~344
packets/s against a 7-credit ACL pool, so `credmin` ran to 0 and a brief link
stall (RF/sink backpressure) overflowed the ring. Fix (`AudioOutputBluetooth::poll`
+ `MediaPacketizer::pending()`): drain only once `framesPerPacket` frames are
ready (or a short flush deadline), so packets carry ~5 frames and the rate falls
to ~69 packets/s — 5× less credit churn. **Silicon: 344→69 packets/s, ring
high-water 7→5 steady, drops=0 over 6+ minutes, clean tone (user-confirmed).**
M2Radio `d982236`, evkb pin bumped. Host suites gained `bt/test/sdp_test`
(37) and `bt/test/btlink_test` (23); l2cap 45 / avdtp 55.
★ **Bench note added to the flash lessons**: on this session the `LinkServer run`
CONNECT hung repeatedly while `flash load`/`verify`/`gdbserver` connected fine —
a full board POWER-CYCLE (not a debug-USB replug) cleared it, since the replug
only resets the MCU-Link probe, not the RT1176 target. The wire-level "Wire not
connected" wedge returned after killing crt_emu mid-connect; a power-cycle cleared
that too. Working recipe once connect is healthy: `flash load` → `flash verify`
(both quick, VECTRESET-based) → detach all debuggers → **SW4 to free-run** the
flashed image (a `gdb continue` in batch mode vKills-and-halts the core on exit).
Do NOT run the 64 MB full-chip `flash erase` — `run`/`load` erase only the
image's sectors.

✅ **Measured 2026-09-03: 128 gates discovered, 128 passed, 0 failed, 0 SKIP**
(`gates: 128 passed`, exit 0; `-l` reports 128), on the **BT-3 phase 4** close-out
(`AudioOutputBluetooth` — first sound over A2DP). Two new gates on the new
`audio/bt_tone_test` (M2Radio pin `ea52aac`, fresh-user verified): `run_qemu.sh`
is the card-ABSENT fallback (streaming stays vacuous with no peer — the self-clock
only starts after `a2dp=ok`); **`[media]`** builds `-DM2_BT_TARGET_NAME=FAKE-HEADSET-01`,
runs the whole `A2dpSource` bring-up to STREAMING, then the tone graph
(`AudioSynthWaveformSine → AudioOutputBluetooth`) really encodes SBC + RTP-frames
it onto the AVDTP media channel, and the fake acceptor's new `media` phase
validates every packet (RTP V2/PT96, strict sequence continuity, each frame's
`0x9C` sync + 119-byte length). DEMONSTRATED RED twice (seq-freeze in
`MediaPacketizer::drain`; `frameCount=0` in `Rtp::header`).
★ **`[media]` asserts blocks>0 + valid framing, NOT drops=0.** Even after two real
bugs were fixed (the graph had no audio clock — `AudioOutputBluetooth` now
self-clocks via an IntervalTimer, its I2S-equivalent role; and `loop()` drained
once/sec), QEMU sustains ~53% drops — a timing artifact (qemu2's LPUART has no baud
pacing; the ACL-credit round-trip is throttled by host-wall-clock scheduling, no
`-icount`). A `drops=0` grep passed only VACUOUSLY (the pre-stream `n=0` heartbeat)
and printed a false claim, so it was replaced: QEMU cannot model the link's flow
control, and the drop / flow-control-at-rate measurement is a SILICON claim (the
deferred Task 8, ESP32 sink). `LICENSE-AUDIT: PASS`; host suites include the new
`rtp_test` + `mediapacketizer_test` (credit-starvation + drop-oldest). Phase 4's
silicon acceptance (audible tone, `drops=0` at some bitpool) is bench-only.
★ **The `[media]` peer/socket attach is timing-sensitive** (like `[hci]`/`[avdtp]`):
reliable in the sequential sweep and in isolation, but rapid back-to-back manual
runs can race the `-serial unix:…,server` bind — re-run idle if it flakes, it is
not a firmware regression.

✅ **Measured 2026-09-03: 126 gates discovered, 126 passed, 0 failed, 0 SKIP**
(`gates: 126 passed`, exit 0; `-l` reports 126), on the **BT-3** (A2DP source)
software close-out. Two new gates on `networking/m2_hci_probe`, taking it from
two to four: **`[baud]`** asserts the vendor set-baud SEQUENCE (0xFC09 uint32 LE
after identity, reply awaited, Reset + identity re-run) — the RATE itself is
silicon-only (0xFC09 does not answer at 921600/3000000 on the real IW416, a Task
4 finding, so phase 0 is deferred to the bench); **`[avdtp]`** runs the A2DP
initiator on `M2Radio/bt` (L2cap/BtLink/Sdp/Avdtp) against the Python fake
acceptor to STREAMING and asserts the calibration SBC config
(`cie=21150235`), the signalling ORDER (DISCOVER/GET_CAP/SET_CONFIG/OPEN/START),
the two negative tripwires (START-before-OPEN, wrong-CID Config Response) and the
L2CAP SCID rule — DEMONSTRATED RED twice before trusted.
★ **This close-out CLEARED the long-standing `m2_hci_probe[hci]` red** — that
gate had been the one standing failure since 2026-08-27 because its `build/`
carried a BENCH config (real `M2RADIO_IW416_*_FW` blobs + `M2_BT_UART_DNLD` +
`M2_BT_ASSERT_CTS`, dated Aug 29); the probe-on-library refactor reconfigured
`build/` gate-clean, so `[hci]` now passes and `cm4_audio_test` did not hit its
nondeterministic red this run — hence a fully clean 126/0/0, the first since the
`[hci]` class appeared. `LICENSE-AUDIT: PASS`. Host suites: `M2Radio/bt/test`
adds the clean-room **SBC ENCODER** (A2DP v1.3 §12; 228 checks) with an ffmpeg
SNR oracle — unity round-trip **63.9 dB, 0 railed** (a +6 dB analysis-scaling
gain that clipped −6 dBFS input was found by the SNR tool and fixed; the
structural host tests could not see it). The SBC encoder is HOST-tested only and
adds NO QEMU gate — it is not linked by any firmware example yet (phase 4's
`AudioOutputBluetooth` will), so the pin bump to `M2Radio` 8920b8d changes no
built ELF. Phases 0 (silicon baud) and 2 (silicon SET_CONFIG/START against the
ESP32 sink + both headsets) remain for a bench session.

✅ **Measured 2026-09-02 (NEW-32 Phase 3, blits & scissor): 124 gates
discovered, **123 passed, 1 failed, 0 SKIP**, `display/vglite_conformance`
green in 11 s with its matrix at **32**. `LICENSE-AUDIT: PASS` (after the
sweep; 133 dep paths). Host suites seven, 800 checks. The one red is
`m2_hci_probe[hci]`, the standing bench-config class. Silicon: THREE boots
in one capture, six Phase 3 lines byte-identical, checker PASS on each,
every prediction held. **Every section of the NEW-32 design is now
measured.**

✅ **Measured 2026-09-02 (NEW-32 gradients): 124 gates discovered,
**123 passed, 1 failed, 0 SKIP**, `display/vglite_conformance` green in 10 s
with its matrix at 26. `LICENSE-AUDIT: PASS` (run AFTER the sweep, never
during), `vglite_conformance` manifest at 132 dep paths. Host suites 631
checks over six. The one red is `m2_hci_probe[hci]`, the standing bench-config
class. Silicon: two boots, six gradient lines byte-identical, checker PASS on
both committed transcripts — and one pre-registered prediction REFUTED (the
legacy gradient API works; see the VGLite note in Architecture).

✅ **Measured 2026-09-02 (NEW-32 Phase 4 guard layer): 124 gates discovered,
**123 passed, 1 failed, 0 SKIP**. `LICENSE-AUDIT: PASS`. The one red is
`m2_hci_probe[hci]`, the standing bench-config class.
★ **The raw sweep read 2 failed, and the SECOND red was MY fault, not the
tree's** — `m2_rx_demo[irq]` failed under load and PASSES idle, because the
licence audit was run CONCURRENTLY with the sweep. That is the exact condition
this file already warns about ("one sweep at a time, output captured, or it did
not happen"); the audit is cheap enough to feel harmless and is not. Run it
before or after, never during.
★ Guard-layer acceptance is separate from the sweep and stronger: six consumer
gates green with goldens VERIFIED UNMOVED (`fd_crc=0xAB66DE0D`,
`KNOB_GRID_SUM_SW=0x579E5810`), plus both widgets re-measured on SILICON
bit-identical to their pre-guard transcripts.

✅ **Measured 2026-09-02 (later the same day): 124 gates discovered,
**123 passed, 1 failed, 0 SKIP**, on the NEW-32 **Phase 4** close-out — the
fader's `SRC_OVER` premultiply fix (SynthUI `d995e63`, pin bumped).
`LICENSE-AUDIT: PASS`. The ONE red is `m2_hci_probe[hci]`, the standing
bench-configured-build-dir class, dispositioned by reading its
`CMakeCache.txt` directly rather than assuming: it still carries
`M2RADIO_IW416_BT_FW`, `M2_BT_UART_DNLD=ON` and `M2_BT_ASSERT_CTS=ON`, dated
Aug 29, and nothing in this work touches that example.
★ **`m2_uap_lwip[uap]` PASSED in this sweep**, having been the load-sensitive
red in the Phase 2 sweep hours earlier. That is corroboration for the
load-sensitivity diagnosis rather than a fix: same tree, same gate, different
machine load.
★ The fader gate PASSES with its SOFTWARE goldens UNMOVED (`fd_crc=0xAB66DE0D`)
even though the GPU goldens moved — which is the whole point of the two-golden-
set discipline, and also why that gate could not have caught the defect the
Phase 4 fix corrects. `synthui_knob_test` and `acid_box` were run as controls
and are unaffected.

✅ **Measured 2026-09-02: 124 gates discovered, 122 passed, 2 failed, 0 SKIP**,
on the NEW-32 Phase 2 close-out (colour & blend; matrix 15 → 20,
`display/vglite_conformance` green in 10 s). `LICENSE-AUDIT: PASS`, vacuity
29/29, host suites **440 checks over five suites** (39 predicates + 25 colour
predicates + 42 arena + 200 path geometry + 134 colour geometry).
Both reds dispositioned WITH EVIDENCE, neither a regression:
  * `m2_hci_probe[hci]` — the SAME bench-configured-build-dir class as
    2026-08-27/29/30, unchanged all session.
  * `m2_uap_lwip[uap]` — failed at 3 s under sweep load and **PASSES idle**
    (re-run: "configure -> BSS_START -> netif -> socket -> DHCP, in order,
    health clean"). This is the documented load-sensitivity class landing on a
    gate NOT previously suspect — the THIRD time that has happened
    (`cm4_wire_int_slave_test` 2026-08-06, `m2_rx_demo[txaggr]` 2026-08-27).
    ★ **So the susceptible set is not a fixed list**, and the branch cannot be
    the cause: its only touch under `examples/networking/` or `tools/` is one
    message string in `gate-vacuity.test.sh`, which that gate never reads.
★ **Phase 2 exists because of a gap found while scoping it**: all fifteen
Phase 1 cases render with `VG_LITE_BLEND_NONE`, while BOTH shipping compositors
use `VG_LITE_BLEND_SRC_OVER` exclusively (twelve call sites). The matrix had
never tested the blend mode production uses.
★ **The colour cases were PINNED to the measured reading after the boot**, and
that closed a real hole rather than a cosmetic one: the drift checker compares
only `<id> <pixel> <repeat>`, so a case returning `ok` under either of two
readings is INVISIBLE to it. Admitting both was right before the boot and wrong
to leave standing after. Two host-suite arms now prove the pin — a reading-A
model reddens the two SRC_OVER cases by name, a modulating-`BLEND_NONE` model
reddens the fifth.

✅ **Measured 2026-08-30: 124 gates discovered, 123 passed, 1 failed, 0 SKIP**,
on the NEW-32 Phase 1 close-out (`display/vglite_conformance`, the GC355/VGLite
conformance harness — green in 10 s on its first sweep). The ONE red is
`m2_hci_probe[hci]`, the SAME bench-configured-build-dir class as 2026-08-27/29
and dispositioned the same way — checked directly rather than assumed: its
`CMakeCache.txt` still carries `M2RADIO_IW416_BT_FW`, `M2_BT_UART_DNLD=ON` and
`M2_BT_ASSERT_CTS=ON` from the concurrent BT bench workstream (dated Aug 29), so
it runs a bench-configured ELF against fake-controller assertions. Not a
regression. `LICENSE-AUDIT: PASS` the same day with the new
`examples/display/vglite_conformance` manifest walked (**129 dep paths**).
Vacuity suite **29/29**, re-derived from a live run — four new cases
(`green_still_passes_vglite_conformance`, the `pixel=ok` tripwire, the
`api2=success` tripwire, the truncated matrix), each demonstrated RED by name
first.
★ **QEMU cannot reach ONE LINE of this example's pixel logic** — every case
reports `pixel=skip` with no GC355 — so it carries THREE HOST SUITES
(`tests/run.sh`, **209 checks**) over the pure predicates, the path arena, and
the case geometry itself. The geometry suite compiles the REAL case functions
against three model rasterisers: a correct GPU, a first-contour-only GPU, and a
GPU that draws nothing. **The negative arms are the point** — demonstrated: a
case hard-wired to `VGC_OK` leaves arm 1 GREEN and is caught only by arm 3, so a
positive-only suite is equally consistent with a matrix that cannot detect
anything.
★ Silicon: `cases=13 ok=12 broken=1 repeat_differs=1`, `vgc_timeouts=0`, TWO
BOOTS BYTE-IDENTICAL, diffed against the PRE-REGISTERED `expected_silicon.txt`
by `tools/vglite-conformance-check.sh`. Three predictions were REFUTED — two VERDICT
predictions (`two-contour-ring-nonzero`, `evenodd-vs-nonzero`: both predicted
`broken`, measured `ok`) and one REPEAT prediction (`evenodd-vs-nonzero`:
`same` → `differs`). Two verdict lines changed in `expected_silicon.txt`, each
with a written reason; `multi-contour-close-padded` was never a prediction, it
was pre-registered as a `pair:` with both outcomes admissible. The transcript
was never pasted over the expectation.
See the VGLite note in Architecture for what that measurement changed.
★ **`LinkServer flash … load` REFUSED THIS EXAMPLE'S `.elf` and accepted its
`.hex`** — `Flash operation exited with code -11`, 0 of 38060 bytes written, 4/4
reproducible, while `vglite_probe` (32 KB) and `synthui_fader_test` (302 KB, one
write) flashed clean immediately before and after. So it is neither size nor a
wedged probe: it is LinkServer's ELF program-header path choking on this image.
`flash … load build/<name>.hex` writes the same bytes and `verify` reports
"File matches flash" on both sections. Reach for the `.hex` before suspecting
the board.

✅ **Measured 2026-08-30: 123 gates discovered, 122 passed, 1 failed**, on
the NEW-23 GPU-compositor close-out (`display/synthui_fader_test` gained the
GC355 compositor; `fd_fps mfps_med=30448` accepted on silicon against the
`>= 30000` criterion, goldens bit-identical across four SW4 boots). The ONE
red is `m2_hci_probe[hci]`, confirmed the SAME bench-configured-build-dir
class as 2026-08-27/29 — its `CMakeCache.txt` still carries
`M2RADIO_IW416_BT_FW` and `M2_BT_UART_DNLD=ON` from the concurrent BT bench
workstream, checked directly rather than assumed. `dualcore/cm4_audio_test`
and `m2_rx_demo[txaggr]` both passed clean on this run (the latter re-run
idle is unnecessary when it already passed in the sweep). Both SynthUI
(`3401ab1`) and LVGL (`9951934`) were pushed and their `evkb.cmake` pins
bumped; a `-DEVKB_FORCE_FETCH=ON` build of `display/synthui_fader_test`
CLONED both from GitHub at the new pins (confirmed in the configure log's
`git clone`/`Already at requested ref` lines, not assumed) and its gate
PASSED against that fetched-source ELF. `LICENSE-AUDIT: PASS` the same day,
`display/synthui_fader_test` manifest walked at 24703 dep paths. Vacuity
suite ran GREEN at **25/25** — and that exposed a wrong number in the
2026-08-29 entry below, which claimed 28/28. Re-counted two ways (a live
run's `PASS:` lines, and the suite's own `report` call sites): 25 both
times. No case was lost; the 28 was a miscount carried in from a summary,
and it has been CORRECTED in place rather than left standing, because a
stale pass-count is exactly the kind of number a later sweep gets diffed
against.

✅ **Measured 2026-08-29: 123 gates discovered, 121 passed, 2 failed** on
the NEW-23 fader close-out — `rt1176:display/synthui_fader_test` green in
21 s on its first sweep, and the two reds are the SAME two as 2026-08-27,
each re-dispositioned with fresh evidence: `m2_hci_probe[hci]` still runs
a BENCH-configured ELF (`M2_BT_UART_DNLD=ON` + the real firmware blob in
its CMakeCache — the build-dir-state class `git status` cannot see), and
`m2_rx_demo[txaggr]` passed idle the same day (the documented
load-sensitivity class). `LICENSE-AUDIT: PASS` the same day with the new
`examples/display/synthui_fader_test` manifest entry walked (24672 dep
paths). Vacuity suite 25/25 the same day, the three new fader cases
included (corrupted golden and missing damage counter both fail by name).
★ That figure read "28/28" until 2026-08-30; it was WRONG — a miscount
carried into this entry from a subagent's summary, not a suite that shrank.
Counted twice since: `grep -c "^PASS:"` on a live run gives 25, and the
suite's own `report` call sites agree. A pass COUNT in this file is a claim
like any other; re-derive it from a run rather than from a report about a
run.
★ The sweep ran via a FRESH `/tmp/fd23` symlink: `/tmp/ev` still points at
the `rt1176-evkb-m2-maya-w161` checkout, and the wrong-tree sweep it would
have produced is the exact trap the 2026-08-23 note warns about — read the
symlink before trusting a sweep taken through one.

✅ **Measured 2026-08-27: 122 gates discovered, 120 passed, 2 failed** on the
first CLEAN single-run sweep of the NEW-20 branch — and both reds were
dispositioned WITH EVIDENCE, neither a regression.
`rt1176:display/rotary_knob_bench` green in 3 s on its first sweep;
`rt1176:dualcore/cm4_audio_test` green in 4 s. The two reds:
  * `m2_rx_demo[txaggr]` failed under sweep load ("7 of 6 frames") and PASSES
    idle — the documented load-sensitivity class, on a gate not previously
    suspect (the second time that has happened; `cm4_wire_int_slave_test` was
    the first).
  * `m2_hci_probe[hci]` fails idle too, and the cause is a NEW CLASS worth
    naming: its build dir had been RECONFIGURED the same morning (09:08) by
    the concurrent BT bench workstream with the real firmware blob
    (`M2RADIO_IW416_BT_FW` set, `M2_BT_UART_DNLD=ON`), so the gate ran a
    BENCH-configured ELF against fake-controller assertions. PROVEN, not
    inferred: the bench build was set aside, the committed configuration
    built fresh, both gates run — PASS and PASS — and the bench build
    restored untouched. A bench-configured build makes its own gate red BY
    DESIGN; this machine's sweep stays red on that one gate until the BT
    workstream's build dir returns to the gate configuration. qemu2 was ruled
    out first (binary Aug 22, tree clean), and every source repo is clean at
    its pin — configure-time CACHE VARIABLES are a build-dir state axis that
    `git status` cannot see.
`LICENSE-AUDIT: PASS` the same day with the new
`examples/display/rotary_knob_bench` manifest entry walked (24682 dep
paths). The vacuity suite
runs 22/22 the same day, including the three new rotary_knob_bench cases
(green fixture replays, gpu tripwire fires by name, corrupted golden fails by
name).
★ An earlier sweep attempt that same afternoon was VOID and is deliberately
not the measurement: three overlapping runner instances (two killed by a
command timeout, one detached with its output lost) — the shared-capture-path
hazard in its purest form. One sweep at a time, output captured, or it did
not happen.

The previous count's measurement, kept per convention:
✅ **Measured 2026-08-24: 121 passed, 0 failed, 0 SKIP** (`gates: 121 passed`,
exit 0), re-measured in the MAIN CHECKOUT on master after the two sibling repos
were pushed and both `evkb.cmake` pins bumped — `rt1176:dualcore/cm4_audio_test`
green in 3 s, `display/acid_box` green, `networking/m2_hci_probe` 15 s and its
`[hci]` variant 61 s. `LICENSE-AUDIT: PASS` the same day.
★ **`0 SKIP` is now true of a FRESH CLONE too, and that was verified rather
than reasoned.** A `-DEVKB_FORCE_FETCH=ON` build of `networking/m2_hci_probe`
cloned `M2Radio` **6ff9ade** and `cores` **36e480d** from GitHub, compiled
clean, and **both gates were then run against that fetched-source ELF** and
passed. A configure proves the subdir resolves; only a gate run proves the
fetched code behaves.
★ **The FIRST attempt at this sweep read `120 passed, 1 failed`, and the
failure was STALENESS, not a regression — worth knowing because it is the
"gates do not build" trap wearing a new face.** `networking/m2_sdio_probe`
failed with "B0 pre-download bracket missing or not in its card-absent form".
Its ELF in this checkout dated 2026-08-22; the BT-1 merge changed that
example's source on 2026-08-24. The gate was reading firmware older than the
assertion. `cmake --build build` then a re-run turned it green, and the number
above is a genuine single-run re-measurement, not two runs spliced together.
★ Two things follow. **The gate did its job** — it went RED rather than passing
vacuously against stale firmware, which is the outcome a gate exists for. And
**mtimes cannot be used to find the other stale dirs**: git rewrites them on
checkout, so ~24 build dirs *looked* stale here. Checked by content history
instead — every one of those ELFs was built the same day as the last commit
touching its example, and the merge touched only two example directories.

The previous count's measurement, kept per convention:
✅ **Measured 2026-08-23: 121 passed, 0 failed, 0 SKIP** (`gates: 121 passed`,
exit 0), on the BT-1 HCI transport close-out, `rt1176:dualcore/cm4_audio_test`
included and green in 3 s, `display/acid_box` green, both new
`networking/m2_hci_probe` gates green (11 s and 33 s).
`LICENSE-AUDIT: PASS` the same day, with the new manifest walked:
`examples/networking/m2_hci_probe` at 120 dep paths.
★ **Its `GATES` entry had to be added by hand and the plan never mentioned the
audit at all** — the drift check caught it, which is what that check is for.
★ **Pass `LICENSE_AUDIT_EVKB=$(pwd)`** when running the audit from anywhere but
`~/Development/rt1170/evkb`: the variable is `LICENSE_AUDIT_EVKB`, not `EVKB`,
and without it the script audits that ONE checkout and reports this tree's new
gate as `MISSING BUILD` while never looking at it. Same class of trap as the
symlink below — a tool that silently measures a different tree than the one you
are in.
★ Run from a SHORT-PATH SYMLINK, and this time it was `/tmp/bt` rather than
`/tmp/ev` — the existing `/tmp/ev` points at a DIFFERENT checkout
(`rt1176-evkb-m2-maya-w161`), so it would have swept the wrong tree entirely and
reported a perfectly plausible number for work that was not there. Check where
the symlink points before trusting a sweep taken through one.
★ Two things about that measurement are worth keeping, because both cost a
whole sweep to learn and neither is a firmware fault:
  * **A fresh worktree has almost nothing built**, and a gate does not build.
    116 rt1176 + 7 rt1062 builds had to be made first. The one build that
    "failed" is `usb/usb_audio_capstone_test`, which is rt1062-ONLY by its
    `boards` sidecar — expected, not a regression.
  * **`display/pxp_draw_bench` needs a SECOND build directory** that no generic
    build-everything loop will make: `cmake -B build-32 -DDRAW_BENCH_32=ON`.
    Without it the gate fails as "no UART capture ... (DEPTH=32 build)", which
    reads exactly like firmware that never started. It is the only example in
    the tree with a second same-board build dir, so it is the only one a
    build-all loop silently misses.

The previous count's measurement, kept per convention:
✅ **Measured 2026-08-21: 111 passed, 0 failed, 0 SKIP** (`gates: 111 passed`,
exit 0), on the Arduino WiFi facade close-out, run via `/tmp/ev`,
`rt1176:dualcore/cm4_audio_test` included and green (3 s).

The previous count's measurement, kept per convention:
✅ **Measured 2026-08-20: 108 passed, 0 failed, 0 SKIP** (`gates: 108 passed`,
exit 0) on the MERGE of the M.2 Wi-Fi line into master, run via `/tmp/ev`,
`rt1176:dualcore/cm4_audio_test` green and `display/acid_box` green (so this
machine does carry the local-only `touch-script` qemu2 change its gate needs).
Nothing had to be re-run. That merge reconciled two lines that had both moved
`evkb.cmake` pins: `cores` and `lwip` kept the M.2 line's SHAs and `fnet` took
master's, each decided by asking the library's own history which SHA was the
DESCENDANT rather than by picking a side. The lwip one mattered — master's pin
is an ancestor of the M.2 line's, and what sits between them is the W11
lwipopts bump (TCP_WND / TCP_SND_BUF = 8*MSS) the throughput work depends on.

The two pre-merge baselines, both kept:
✅ Measured 2026-08-18: 97 passed, 0 failed, 0 SKIP — a fully clean serial
sweep on the Acid Box capstone branch (`display/synthui_step_test` and
`display/acid_box` added), `rt1176:dualcore/cm4_audio_test` green in 3 s on the
first run. `LICENSE-AUDIT: PASS` the same day with both new manifests walked:
`display/acid_box` at 25691 dep paths, `display/synthui_step_test` at 24667.
(Earlier: 2026-08-17 95/0/0 on the VGLite Phase 2 merge; 2026-08-16 94/0/0 on
Phase 1.)

✅ **Measured 2026-08-20: 105 passed, 0 failed, 0 SKIP**, on the W16
aggregation work, run via `/tmp/ev`, `rt1176:dualcore/cm4_audio_test` included
and green. All seven `networking/m2_rx_demo` gates green; `[txaggr]` is now the
longest in the example because its burst runs after BOTH service windows.

★ **The sweep before that one found a REAL race in `[irq]`, and it is worth
knowing because it presents as a firmware failure.** That gate broke its wait
loop on `^irq_done ` and then reaped QEMU immediately — but the two `phase=`
lines it computes its A/B from are printed right after that line, so under
sweep load the reap landed between them and the gate failed with "the run did
not print both phase= summary lines" against a run that had worked perfectly
(the capture was torn mid-line at `irq_done frames=70 … stranded=0/0`). The
race was latent from W15; W16 made it likelier by lengthening that line. **Every
gate on this example now waits for the LAST line it parses, not the first
interesting one** — `[irq]` on `^phase=irq `, the `demo_done`-based gates on
the `^irq_mode=` line that follows it. A gate that reaps mid-line blames the
firmware for its own timing.

The previous count's measurement, kept per convention:
✅ Measured 2026-08-20: 102 passed, 0 failed, 0 SKIP (`gates: 102 passed`,
exit 0), on the W15 phase-2 interrupt-driven-service gate, run via `/tmp/ev`,
`rt1176:dualcore/cm4_audio_test` included and green. All four
`networking/m2_rx_demo` gates green in that sweep, `[irq]` at 19 s — it is the
longest gate in the example because it runs two 6 s service windows back to
back in one image, which is what makes its polled/interrupt comparison a
controlled A/B rather than two runs.

The previous count's measurement, kept per convention:
✅ Measured 2026-08-19: 101 passed, 0 failed, 0 SKIP (`gates: 101 passed`,
exit 0), on the W14 phase-2b regression gates, run via `/tmp/ev`,
`rt1176:dualcore/cm4_audio_test` included and green.

The previous count's measurement, kept per convention:
✅ Measured 2026-08-19: 98 passed, 0 failed, 0 SKIP (`gates: 98 passed`,
exit 0), on the W14 IW416-model gate, run via `/tmp/ev`,
`rt1176:dualcore/cm4_audio_test` included and green.

The previous count's measurement, kept per convention:
✅ Measured 2026-08-19: 97 passed, 0 failed, 0 SKIP (`gates: 97 passed`,
exit 0), on the W11 throughput close-out, run via `/tmp/ev`,
`rt1176:dualcore/cm4_audio_test` included and green.

The previous count's measurement, kept per convention:
✅ Measured 2026-08-19 (earlier same week): 96 passed, 0 failed, 0 SKIP
(`gates: 96 passed`, exit 0), on the W9 lwip-bridge close-out, run via
`/tmp/ev`, `rt1176:dualcore/cm4_audio_test` included and green.

The previous count's measurement, kept for its notes:
✅ **Measured 2026-08-17: 95 passed, 0 failed, 0 SKIP.** A fully clean sweep on
the M2Radio/`networking/m2_sdio_probe` merge, `rt1176:dualcore/cm4_audio_test`
included. Note the runner prints only non-zero categories, so `gates: 95 passed`
with exit 0 IS `95 / 0 / 0` — don't go looking for the zeros.

★ **That sweep was run through a SHORT PATH SYMLINK, and it had to be.** Four
gates open a QEMU monitor UNIX socket at `$DIR/mon.sock`
(`cm4_usb_irq_probe`, `cm4_usb_enum_probe`, `cm4_usb_audio_probe`,
`cm4_graph_usb_capstone`). macOS caps `sun_path` at **104 bytes**, and in a
checkout named `rt1176-evkb-m2-maya-w161` those paths are 106–111 bytes, so all
four die with `OSError: AF_UNIX path too long` — before QEMU is even contacted.
They pass unchanged via `ln -s <repo> /tmp/ev` and running the sweep from
`/tmp/ev` (52 bytes). **This is a property of where the clone lives, not of the
firmware**: the same ELF and the same gate script pass or fail purely on the
length of the directory name above them. `~/Development/rt1170/evkb` is 93 bytes
and fits, which is why this has never been seen before. Diagnose it by
`echo -n "$DIR/mon.sock" | wc -c` rather than by reading the Python traceback,
which names neither the path nor the limit.

★ **`display/vglite_lvgl_test` gates the SOFTWARE build of a two-build
example.** The GPU build (`build-vglite/`, LVGL's VG_LITE unit on the GC355)
is silicon-only with its OWN golden — two golden sets, never
reconciled (hardware AA ≠ LVGL mask arithmetic, and the GPU build carries
`LV_USE_FLOAT=1`). Since NEW-20 Phase 2 the scene is a
`synthui_rotary_knob` grid (sw golden `0x579E5810`; the pre-swap pair was
sw `0x513C4DB8` / gpu `0xC3C6171A`). The VGLite-Phase-2 fps criterion was
measured on the OLD knob and NOT met
(software 2.83 fps, GPU 2.45 fps, CPU-bound in the backend's per-task path
construction) — that per-task path rebuild diagnosis is what NEW-20's bench
confirmed and what the rotary widget's cached-path compositor answers;
`docs/superpowers/specs/2026-08-17-vglite-phase2-design.md` has the original
verdict, `2026-08-27-rotary-knob-bench-design.md` §13 the measurement that
superseded it.

★ **No gate is SKIP-class any more.** For one day `display/synthui_knob_test`
and `display/vglite_probe` were: SynthUI and VGLite were unpushed, their import
macros raised `FATAL_ERROR`, and a fresh clone reported `2 SKIP` — invisible in
the pass/fail columns. **Both repos were pushed public on 2026-08-17** and both
now build from the pin, verified with `-DEVKB_FORCE_FETCH=ON` (the knob's five
goldens came back bit-for-bit from GitHub-fetched sources). `0 SKIP` is
therefore achievable on a clean machine again, which is what makes it worth
asserting. `docs/KNOWN-BROKEN-GATES.md` keeps the resolved entry, because the
next unfetchable library re-creates the class — and its lesson: a SKIP hides in
a count, while every other exception in that file shows up RED and by name.
★ **SynthUI's history was REWRITTEN before it went public** (`reference/rebirth/`
dropped, rights unclear), so every SynthUI SHA before `e132012` is unreachable.
A pin recovered from this repo's own git history will not fetch.

★ **A green `display/vglite_probe` does NOT mean the GPU works.** QEMU has no
GC355 model, so that gate asserts the GPU-ABSENT fallback — the same ELF
detecting no GPU and taking the software path rather than spinning in
`vg_lite_init()`. The GC355 rendering is verified on silicon only, in the
example's `transcript_hw_evkb.txt`. This is the sharpest current instance of
"QEMU pass is necessary but not sufficient".

The previous baseline, kept because its account is still the reference for the
WM8962 lesson: **measured 2026-08-15: 92 passed, 0 failed, 0 SKIP**, a fully
clean sweep including the two dual-core Wire gates that were red earlier the
same day —
`cm4_wire_test` and `cm4_wire_int_master_test` had been asserting
`rdv=00000000`, a value produced only by QEMU's old WM8962 *stub*. That stub is
now a real model returning the true 0x6243 device ID, so both gates assert what
silicon asserts. Full account in `docs/KNOWN-BROKEN-GATES.md`.

★ **A QEMU model can change under you with no commit to show for it.** That
change was uncommitted in the qemu2 working tree and already compiled into the
binary, so `git log` on the model looked a month stale while the behaviour had
already moved. When a gate goes red and nothing in the firmware's history
explains it, check the model's WORKING TREE and the binary's mtime, not just
its log. Related: a concurrent session rebuilt that binary three times in an
hour and then removed it entirely with an ASan `configure`, which is enough to
invalidate a sweep taken across it and enough to make every gate report "no
UART capture". **Confirm with an untouched control gate before believing a
broad red.**

Beyond those there is **one** permitted red:

- `rt1176:dualcore/cm4_audio_test` — **nondeterministic.** Long called a load
  artefact, and load does not predict it: on 2026-08-08 it failed a sweep
  starting at load 6.8 and passed one starting at 8.6. Re-run before believing a
  red, and note that **consecutive readings are not a trend** — four in a row
  that day looked like a clean threshold near load 4 and the next measurement
  refuted it. Don't infer a load number; `docs/KNOWN-BROKEN-GATES.md` has all
  six readings.

Both the count and the pass/fail were measured that day; nothing here is
carried forward.
**0 SKIP is the load-bearing number in every case**: it is what says the
sweep actually covered everything rather than quietly measuring less.
Note `-l` prints a trailing "(N gate(s))" summary line, so `wc -l` on its
output is one more than the gate count. Any failure that is **not** that one is
a real regression from what you are doing — read the gate NAMES in the summary
rather than trusting the count alone.

**The tree is multi-board.** `EVKB_BOARD` selects `rt1176` (MIMXRT1170-EVKB,
the default) or `rt1062` (MIMXRT1060-EVKB). An example declares the boards it
supports in a `boards` sidecar file; absent means `rt1176` only, which is why
most examples have none. Gate ids are `<board>:<category>/<name>` and no gate
names a QEMU machine — `tools/gate-lib.sh` derives `-M`, `-global`, the build
directory and the `-serial` chain from the board. Build a non-default board with
`cmake -B build-rt1062 -DEVKB_BOARD=rt1062 -DCMAKE_TOOLCHAIN_FILE=../../../toolchain/rt1062-evkb.toolchain.cmake`.

`usb/usb_descriptor_survey` is gated on both boards and is the RT1062's USB
host proof: it enumerates QEMU's emulated `usb-audio` and reads `46F4:0002` off
the wire — an oracle the firmware has no knowledge of. Like
`dualcore/cm4_usb_irq_probe` and `audio/audioinput_i2s_test` (whose rt1062 half
needs the `sai1-rxinject` binding, qemu2 `2141a5d781`), its rt1062 half depends
on LOCAL-ONLY qemu2 changes, so **a fresh clone sees it red for that reason
too**; that is the GPL firewall working, not a regression.

★ **NINE gates need the IW416 card model, and unlike the rt1062 group above
they are NOT local-only — the model is PUSHED.**
`networking/m2_sdio_probe[wifi]`, `networking/wifi_client_test[wifi]` (added
with the Arduino facade — it drives a real scan through `WiFi.begin()` and
asserts the model's zero-BSS reply is reported honestly), all SEVEN
`networking/m2_rx_demo` gates and `networking/m2_uap_probe[wifi]` — TEN in all —
need the **IW416 SDIO card model**
(`hw/sd/iw416-sdio.c`, enabled by `-machine mimxrt1170-evk,m2-wifi=on`), plus —
for `[irq]` — the SDIO card-interrupt plumbing W15 added to the SD bus and
SDHCI.
★ **FIVE MORE need a qemu2 with the uAP SURFACE on top of that** —
`m2_uap_probe[uap]` and all four `m2_uap_lwip` variants beyond the card-absent
one (`[uap]`, `[data]`, `[mistag]`, `[tx]`). The floor is qemu2
**`c3b816be51`** (uAP command family + modelled station + `inject-bss` + the
TxPD `bss_type` read). An older model rejects `-global iw416-sdio.uap=on`
outright, so these go RED and
not SKIP, and it is not a firmware regression — diagnose by asking the device
whether it takes the property (`qemu-system-arm -device help | grep iw416`),
not by reading firmware.
★ **The IW416 model now echoes the WHOLE seq_num** (qemu2 `721fb09146`,
2026-08-22): bss_num/bss_type included, because that is what silicon does. It
used to zero the high byte, which HID A BUG CLASS — a driver comparing the full
16-bit seq instead of masking to the low byte passed in emulation and rejected
every uAP reply on the bench. A qemu2 older than that revision does not catch
it; nothing else changes, and all eight model-using gates pass either way.
★ The floor is NOT uniform across the nine. `m2_uap_probe[wifi]` uses only
`m2-wifi=on` + `fw-preboot=on` and the command port, so an older model
satisfies it; the W16 floor below is the m2_rx_demo family's. Do not read one
of the nine going red as evidence about the others.
★ **W16 MOVED THE FLOOR, and it moved it for gates that used to be satisfied
by an older model.** The driver's service path no longer polls registers with
CMD52 at all: it reads the multiport REGISTER PORT (a byte-mode CMD53 at fn1
address 0) and issues AGGREGATED CMD53s spanning several ring slots. A qemu2
that models neither returns zeros for the first and refuses the second, so
**every** m2_rx_demo gate except the card-absent one goes red — not just the
new three. The requirement is now `gitlab.com/Newdigate/qemu-rt1170` **master
at or after `7e17eff5d3`**, which adds both plus the `reg-port-literal`
property `[regfallback]` needs. `2ed9314631` (the W15 floor) is no longer
enough.
That exact revision matters for `[irq]`: `2ed9314631` gates the model's
DAT1 line on CCCR 0x04 (IENM|IEN1) -- the card-side enable silicon proved
mandatory in W15 phase 3 -- so against the older `8d81ed3fc1` model a
driver that FORGOT the CCCR write would still pass `[irq]` while being
dead on hardware.  The newer model fails it, as silicon does.
Stock upstream QEMU has no such device — and worse, no SDIO card interrupt
at all (`SDHC_NIS_CARDINT` exists only in the clear path) — so against a
stock build these go RED, not SKIP, and it is not a firmware regression.
Diagnose by asking the machine whether it takes the property
(`qemu-system-arm -machine mimxrt1170-evk,help`), not by reading firmware.
Note `m2_rx_demo`'s plain `run_qemu.sh` is exempt: it asserts the
card-ABSENT fallback and passes on stock QEMU like every other m2_* gate.
★ Do NOT confuse a qemu2 that is merely too old with the DRIVER's own
register-port fallback. `[regfallback]` deliberately runs a card that answers
the register port the other plausible way, and the driver detects it and keeps
going on CMD52 (`mpregs=0` in the demo's counter line). An old qemu2 does not
answer the register port at all — the reads simply return zeros with no
guest-error — and the driver's CARD_STATUS check catches that too, so a red
sweep on an old model tends to show `mpregs=0` everywhere rather than a hang.
Diagnose by asking the machine whether it takes the property
(`qemu-system-arm -device help | grep iw416`, or start it with
`-global iw416-sdio.reg-port-literal=on` and see whether it is rejected).
★ **Why those gates exist at all**: until W14 the entire SDIO ring/interrupt
layer had ZERO automated coverage — every m2_* gate asserted the card was
absent, so both of this subsystem's serious bugs (the W8 32-port ring, the
W12 stranded uploads) would pass a fully green sweep, and both cost days of
hand-run silicon soaks to find. `m2_rx_demo[ring]` and `[stranded]` are
regression gates for exactly those two, and **each was DEMONSTRATED to fail
against a deliberately re-broken driver** before being trusted (the
demonstrations are quoted in each gate's header). A regression gate never
shown to fail is decoration.
★ **`[stranded]` covers the ring SAFETY NET, not the sticky accumulator** —
measured, not assumed: reverting the accumulator alone leaves the gate GREEN,
because `suppress-updl` means the interrupt is never raised and there is
nothing to accumulate. Removing the safety net reds it. Do not read a green
`[stranded]` as "the W12 layer-1 fix is fine"; nothing in this tree can
express that variant, because nothing can force one of the driver's five
HOST_INT_STATUS readers to run at the deciding instant.

★ **The RT1062 USB host needs the CCM_ANALOG SET/CLR/TOG aliases modelled.**
`hw/misc/imxrt1060_anatop.c` treated the `base+0x4/+0x8/+0xC` words as ordinary
storage rather than alias ports onto the base register, so every alias write
vanished. USBHost_t36's `PLL_USB2` powerup (`ehci.cpp:182-214`) drives that PLL
*only* through `_SET`, so it spun forever and `USBHost::begin()` never
returned. Worth knowing because of how it presents: **not** "USB does not
enumerate" but a hard hang in `setup()`, with the banner printed and the
2-second heartbeat absent. If an rt1062 image goes quiet after one line, check
whether it is looping on a register whose only writes go through an alias —
`-d unimp` will NOT show it, because the registers are all implemented.

Three per-board divergences are already known, all handled in `gate-lib.sh`,
and any new two-board example must go through it rather than spelling them out:
- **QEMU machine**: `mimxrt1170-evk` vs `mimxrt1060-evk`.
- **Boot property**: the RT1170 model's `boot-xip` is a boot-ROM stub that
  parses the real IVT; the RT1062 model has `boot-xip` AND `boot-ivt` as
  *different* properties selecting different reset vectors, and a Teensy-core
  image is the `boot-ivt` kind. Using the wrong one double-faults into Lockup
  before a line of firmware runs.
- **Which object is the console**: **LPUART1 on both boards**, but the cores
  name it differently — `Serial1` on the `imxrt1176` core, `Serial6` on
  the `teensy4` core (which follows the Teensy pin-0/1 convention and gives the
  name `Serial1` to LPUART6). Every two-board sketch therefore defines a
  `CONSOLE` alias rather than naming a `SerialN` directly, and every gate takes
  its chain from `gate_console` — which now emits a plain `-serial file:` for
  both boards, because both land in slot 0.
  ★ **This was got wrong first time and the mistake is worth knowing.** The
  rt1062 sketches originally printed to `Serial1` = LPUART6, and `gate-lib`
  emitted five `-serial null` to reach it. That passed in QEMU and was **useless
  on silicon**: on the MIMXRT1060-EVKB, LPUART6 only reaches Arduino header pins
  D0/D1, while the DAPLink/OpenSDA VCOM is wired to LPUART1
  (`GPIO_AD_B0_12/13` — `core_pins.h` pins 21/22). The gate and the bench were
  reading different wires, and nothing caught it until a hardware run was
  attempted. Get the slot wrong in either direction and the firmware runs
  perfectly while the capture stays empty — indistinguishable from firmware that
  never started.

★ **`rt1062` links the `teensy4` core, which is LGPL** — see the licence-audit
note under `tools/` below. That core is upstream Teensy, not the clean-room
`imxrt1176` core, and building it is what put copyleft source into a link
manifest for the first time in this tree.

Three things that number depends on:

- **Gates do not build.** The runner assumes `build/<name>.elf` exists and
  reports a missing one as SKIP, not as a failure. A non-zero SKIP count means
  you measured less than you think — build every gate-owning example first, or
  the sweep quietly under-reports.
- **Do not count gates with a bare `find`.** Until 2026-08-06 discovery
  descended into `build*` directories, where `-DEVKB_FORCE_FETCH` clones
  peripheral libraries that carry gates of their OWN — four, in the SPI and
  SdFat trees. A raw find therefore returns 79, and the four extras become
  permanent SKIPs for images this repo never builds. That is worse than a
  wrong count: it destroys the SKIP signal above, which is the only thing
  that tells you a sweep under-reported. Discovery now prunes `build*`; ask
  the runner (`-l`) rather than `find`.
- **Dual-core gates are load-sensitive, not deterministic — and it moves
  between them.** `docs/KNOWN-BROKEN-GATES.md` records `cm4_audio_test` both
  ways on consecutive days. On 2026-08-06 a `-j 4` sweep run *while the
  machine was simultaneously building, flashing and driving hardware* failed
  `cm4_wire_int_slave_test` instead — a gate not previously suspect — while
  `cm4_audio_test` passed in that same run. The failing gate passed in 1 s
  when re-run idle and compiles none of the code that sweep was testing. So
  do not treat one red dual-core gate as a regression without re-running it
  idle first, and do not assume the susceptible gate is the documented one.
  Equally: do not delete, weaken, or drop any of them to make a sweep green.
  That file is the record; keep this line agreeing with it.

The baseline before that was `28 passed, 1 failed, 0 SKIP`, which had gone
stale by more than half the tree. A baseline that understates the gate count
that badly means a sweep can silently lose dozens of gates and still look
right — the same class of defect the audit's `GATES` drift check exists to
catch. Re-measure this line when you add gates.

Re-measuring means running the sweep, not counting files. On 2026-08-06 the
`74` above was judged stale on the strength of a bare `find` returning 79, and
it was not stale — the four extras were another repo's gates inside a build
directory, and 74 was exactly right. The number moved to 75 only because a
gate was genuinely added.

Repo-wide gates in `tools/`:
- `license-audit.sh` — proves no copyleft source is compiled into firmware
  (header sweep + binary-provenance check + non-UTF-8-source check +
  link-manifest depfile audit). The
  tree is permissive-only — MIT/BSD; every
  inherited LGPL file has a clean-room rewrite. (The former Apache-2.0
  exception, `VGLite/vg_lite_flat.{c,h}`, no longer exists: the v7 SDK
  re-vendor of 2026-08-17 has no such files and VGLite is MIT throughout —
  its `NOTICE` records the history.) Don't
  introduce GPL/LGPL/MPL code or dependencies, and don't vendor a prebuilt
  binary without licence text beside it.
  ★ **Part 1 also fails on any tracked non-UTF-8 C/C++ source** (added
  2026-08-17): `grep -I` classifies such files as binary and silently skips
  them, so a source file in another encoding is a file the copyleft sweep
  never reads — the same hole as an unlicensed binary, through a different
  door. The SDK v7 VGLite drop shipped `vg_lite_stroke.c` in ISO-8859-1 and
  FNET carried 16 cp1252 files; all are transcoded and the check now keeps
  it that way. When vendoring, transcode with
  `iconv -f WINDOWS-1252 -t UTF-8` (not ISO-8859-1 — that mapping turns
  cp1252 smart quotes into INVISIBLE C1 controls that pass the audit while
  silently corrupting comments; measured, then fixed, in FNET).
  ★ **Green does NOT mean "this tree is MIT".** The audit greps for COPYLEFT,
  and Apache-2.0 is not copyleft — so it passed, correctly, while VGLite's own
  README claimed "MIT throughout" and was wrong for a day. The audit answers
  *is there copyleft here*, not *is each repo the licence it advertises*. The
  second question needs a per-file survey; VGLite's `VENDORING.md` carries the
  one-liner that does it, and that is the check to run when vendoring anything.
  An entry may name a **build directory** (`examples/…/build-rt1062:target`)
  rather than an example directory, so a second board's build of the same
  example gets its own depfile walk. It needs one: `EVKB_BOARD=rt1062` links a
  different core.
  ★ **The GATES drift check accepts `<rel>/build*:` as well as `<rel>:`** (fixed
  Phase 5b). An example gated on a NON-default board only has no plain `build/`,
  so its build-directory entry is its *only* entry — and the old `"$rel:"`
  substring test false-positived on it. `usb/usb_audio_capstone_test` (rt1062
  only) is the first such example; before it every two-board example also built
  for rt1176, so a plain entry always existed and the narrower test was
  sufficient by accident. Left unfixed, the only ways to green the audit were
  deleting a real entry or writing a bogus `GATES_EXEMPT` — the check meant to
  keep GATES honest applying pressure to weaken it.
  ✅ **CLOSED 2026-08-08 — the audit PASSES, rt1062 builds included.** It was
  open for most of that day: `cores/teensy4/` sits in the audit's `ALLOW` list
  on the condition that its objects define **no** symbols, which held while
  that directory was "an uncompiled upstream reference copy — never built".
  The board axis builds it, and five of its files were LGPL-2.1 (`WString.cpp`,
  `IPAddress.cpp`, `Stream.cpp`, `WMath.cpp`, `Time.cpp` — all
  Arduino-inherited), compiling to real symbols in `libcores.o.a`.
  Resolved the only acceptable way — the five were **replaced** with the MIT
  clean-room versions (`cores` `99f7657`, pinned in evkb.cmake's manifest and
  pushed to `origin/master`), not excused by relaxing `ALLOW` or dropping the
  `build-rt1062` GATES entry. Two headers went the same way, `Printable.h` and
  `WCharacter.h`, because they were in the rt1062 link manifest and the
  EMPTY-object rule cannot see headers — they define no symbols. `Client.h` and
  `Server.h` still carry LGPL text and are deliberately left: no link manifest
  includes them. **Check the manifest, not this note, before adding a header.**
  Measured: `LICENSE-AUDIT: PASS` with both rt1062 entries walked —
  `serial_test/build-rt1062` 136 dep paths, `usb_descriptor_survey/build-rt1062`
  203.
- `license-audit.test.sh` — negative tests proving the audit's part-1 checks
  actually fire (unlicensed binary, MPL header) rather than passing vacuously.
- `gate-lib.test.sh` — tests for the gate runner lifecycle library.
★ **A COMMITTED FIXTURE GOES STALE SILENTLY, and the vacuity suite is where
that shows up — found 2026-08-25.** `green_still_passes_m2_hci_probe` was RED
at `master` before any of that day's work: the committed
`m2_hci_probe/transcript_qemu.txt` had been captured before the example grew
its `btfw=` and `bt_cts=` lines, so the gate's own assertions no longer matched
its own fixture. Nothing else caught it — **the QEMU sweep cannot**, because
the sweep runs the firmware live and the fixture is only replayed by
`gate-vacuity.test.sh`. Two consequences worth keeping: a green 121-gate sweep
says nothing about fixture freshness, and BT-1's recorded "vacuity 19/19" had
gone false without anyone touching the tests. **Re-capture the fixture whenever
an example's output changes** (`cp build/serial.uart transcript_qemu.txt` after
the gate runs), and run the vacuity suite as well as the sweep before believing
a close-out.

- `gate-vacuity.test.sh` — negative tests proving the *gates themselves* fail
  when they should: a run that produced no UART must fail by name rather than
  die silently or blame the firmware, a missing counter token must not read
  as proof the good outcome happened, and — added with W14 — a gate asserting a
  device WORKS must fail on the device-ABSENT capture, or it would pass whether
  or not the model was enabled. Drives real runners against a fake QEMU
  (via `qrun`'s `REAL_QEMU` hook) using each gate's committed
  `transcript_qemu*.txt` as the fixture, so it needs no prior gate run — but it
  does need the examples it covers built.

## Flash / run on hardware

```sh
pkill LinkServer; pkill redlinkserv; pkill crt_emu_cm_redlink   # clear stale probe daemons
LinkServer run MIMXRT1176:MIMXRT1170-EVKB build/<name>.elf      # load + reset + free-run
```

**★ Do NOT hold the VCOM while programming.** With `tools/rt1170-console.py`
attached, `LinkServer flash … load` dies with `request to clear DAP error failed
- status 131` / `LOAD_EXIT=255` and the port re-enumerates mid-attempt. The
identical command succeeds the moment nothing holds the port. Working order:
`flash … load` → `flash … verify` (both VCOM-free) → **then** attach the reader
→ reset. There is no standalone `LinkServer reset` subcommand in 26.6.137;
backgrounding `LinkServer run` is how you trigger one. Note `pkill LinkServer`
alone leaves `redlinkserv`/`crt_emu_cm_redlink` resident, which silently kills
the next few runs.

Use **LinkServer** (`/Applications/LinkServer_26.6.137/`), not pyOCD — pyOCD is
unreliable programming this board's FlexSPI NOR. Console is the MCU-Link VCOM
(`/dev/cu.usbmodem…`) at 115200; read it with pyserial
(`tools/rt1170-console.py <port> 115200`) — macOS `cat` silently resets the
port to 9600. Start the serial reader *before* triggering a reset if you need
boot output. `tools/rt1170-flash.sh` wraps flash + console;
`tools/rt1170-qemu.sh` boots an arbitrary image in QEMU outside the gate
harness.

★ **A DEBUGGER-HALTED CORE IS INDISTINGUISHABLE FROM DEAD FIRMWARE, AND THE
EVIDENCE LOOKS DAMNING.** This cost a full session on `display/acid_box`, which
was written up as a CM7 lockup at 1.739 s and merged as such; the firmware was
healthy the whole time and now runs 15 minutes on the bench. Three separate
LinkServer behaviours conspire, and none of them announces itself:

- `gdbserver --attach` reports a **fake stop** and returns **0 for every core
  register** while the target runs on — so `$sp = $lr = 0` is the normal
  reading of a RUNNING core, not a corrupted one. Memory reads in that mode are
  live and trustworthy; **writes are silently dropped**, debug registers
  included.
- A probe-latched `C_HALT` **survives `wiretimedreset`**. The core never
  re-runs startup, so `systick_millis_count` keeps its old DTCM value and every
  later read returns the SAME number — which reads exactly like a reproducible
  freeze at that millisecond. Watch systick **restart from ~0** or you are
  re-reading a corpse that was never a corpse.
- Every connect script arms all seven **DEMCR vector catches** (`0x010007F0`),
  and vector catch halts the core *instead of* running the handler. So "a
  breakpoint on `fault_isr` was never hit" is the EXPECTED result with or
  without a fault.

**Read `DHCSR` (0xE000EDF0) FIRST, every time** — bit 17 `S_HALT`, bit 19
`S_LOCKUP`, bit 24 `S_RETIRE_ST`; `0x01010001` is healthy-running,
`0x00030003` is halted-by-debugger. Then `CFSR`/`HFSR` (0xE000ED28/2C): both
zero means no fault was ever taken, and that is readable even when registers
are not. `tools/rt1170-swdprobe.py --health` prints the whole block; its
docstring carries the full account. Corroborate a frozen counter with a
SECOND clock (an audio graph's sample counter tracking systick to
milliseconds over minutes is far stronger than either alone) before believing
any freeze.

★ **Never `pkill -9` a LinkServer session mid-flash-program.** Doing so left the
target unreachable at the wire level — `Wire not connected` and
`Hardware interface transfer error` on every transfer, `dapinfo` included —
while the MCU-Link itself still enumerated perfectly over USB. No software
recovery worked; a full board **power cycle** was required. Note also that
`LinkServer run` is silent for a minute or more while it programs: silent is
not hung.

## Architecture

- **`imxrt1176/` (in the `teensy-cores` sibling repo)** — the core: startup
  (FlexRAM config, 996 MHz
  OverDrive voltage), linker script `imxrt1176.ld` (XIP image at 0x30002000),
  Teensy-compatible API surface (GPIO, LPADC, FlexPWM, DAC, PIT/IntervalTimer,
  LPUART Serial, USB device stack, DMAChannel/eDMA, EventResponder,
  AudioStream), and the dual-core layer (`Multicore`, `MessagingUnit`,
  `Cm4ImageBank`). **`teensy4/` (same repo) used to be an uncompiled upstream
  reference copy; since the RT1060 board axis it is the core that
  `EVKB_BOARD=rt1062` actually builds.** That is what broke the licence audit
  for a day (see `tools/license-audit.sh` above) — it is upstream
  Teensy/Arduino code, and the five LGPL files it carried have since been
  replaced with the clean-room versions. Its linker scripts still differ from
  `imxrt1176.ld` in a way that matters for DMA: `.bss` goes to **DTCM** in
  `imxrt1060_evkb.ld`, `imxrt1062.ld` and `imxrt1062_t41.ld` alike, and only
  `.bss.dma` (`DMAMEM`) reaches OCRAM. So a buffer that is DMA-reachable by
  default on one board is not on the other.
  ★★ **THE TWO CORES DIFFER ON WHICH GPIO INSTANCE OWNS A PAD, and getting it
  wrong manufactures PLAUSIBLE FALSE READINGS rather than failing.** `teensy4`
  sets `IOMUXC_GPR_GPR26..29 = 0xFFFFFFFF` in `startup.c`, which hands every pad
  to the **FAST GPIO aliases — GPIO6..GPIO9**. The IOMUX ALT still selects the
  "GPIO1" function and NXP's own pin naming still says `GPIO1_IO19`, so code
  written from a schematic looks right and does nothing: **writes to `GPIO1_*`
  never reach the pin, and `GPIO1_PSR` returns a value unrelated to it.**
  `imxrt1176` has no such remap — GPIO9 there is the real instance.
  ★ Met 2026-08-25 in `networking/m2_hci_probe`'s rt1062 port. A pin-swing test
  that drove `GPIO1` and read `GPIO1_PSR` reported `drives_low=1 drives_high=0`
  — a confident "PIN STUCK (loaded or shorted)" verdict — because read-low twice
  inverts to exactly that. **A disconnected register and a clamped pin are
  indistinguishable through such a test**, and the false verdict cost two
  resistors off a board before the cause was found. If you write direct GPIO
  register code for rt1062, use GPIO6..GPIO9, and prove a pin moves by reading
  it back through the SAME instance you drove.

  ★ **The two cores also differ on the D-cache, and DMA correctness depends on
  it.** `teensy4` enables it (`startup.c`, `SCB_CCR_IC | SCB_CCR_DC`);
  `imxrt1176` never writes `SCB_CCR` at all, so OCRAM is coherent there
  for free. On rt1062 it is not: a DMAMEM buffer is cached write-back unless the
  MPU says otherwise, so a CPU write can sit in cache while a bus master reads
  stale memory. That cost a full silicon debug session — the EHCI walked a
  periodic list of stale garbage and halted with **no error bit set**, because
  the port-connect ISR had already acked the fatal status. OCRAM is now mapped
  `MEM_NOCACHE` under `ARDUINO_MIMXRT1060_EVKB`. **Two facts follow: DMA buffers
  on rt1062 need OCRAM *and* that OCRAM must be uncached, and `SEI`/`UEI`
  reading 0 never proves no error occurred when an ISR is attached.**
- **Peripheral libraries are sibling repos**, not in-core: Wire (LPI2C),
  SPI (LPSPI), Audio (graph nodes + WM8962 codec driver), MipiDisplay
  (MIPI-DSI panels), Ethernet stacks, etc. Core-vs-library boundary follows
  Teensy convention; several subsystems were deliberately moved out of the core
  into `newdigate/<lib>` forks. MipiDisplay is split panel-independent `soc/`
  vs. per-panel `panels/<name>/`, so it is imported as
  `import_evkb_library(MipiDisplay soc panels/<name>)` — the panel is chosen by
  which directory the example imports (the RT1176 has one MIPI-DSI host, so
  only one panel can ever be live).
  **`VGLite` is the newest and the odd one out**: not a newdigate fork but NXP's
  MIT VGLite driver vendored verbatim (`VENDORING.md` records provenance), plus
  this tree's own `port/baremetal/` replacing the FreeRTOS port layer. It drives
  the **Vivante GC355 GPU2D** — which the RT1176 does have, contrary to what
  `docs/superpowers/specs/2026-07-27-rt1176-lvgl-design.md` claimed until that
  claim was corrected. Imported with `import_evkb_vglite()`;
  `VG_DRIVER_SINGLE_THREAD` is load-bearing, not decoration.
  ★ **Vivante wants 64-byte-aligned command buffers and a misaligned one does
  not fail — it hangs the front end while every API call returns
  `VG_LITE_SUCCESS`.** That cost most of Phase 1. When a device API insists
  everything worked, read the device's own status register (`AQHiIdle`, 0x004,
  bit 0 = front end).
  ★★ **This GC355/driver renders ONLY THE FIRST CONTOUR of a vg_lite path —
  every subpath after the first `VLC_OP_MOVE` is silently dropped, and every
  API call still returns `VG_LITE_SUCCESS`.** Found 2026-08-29/30 building the
  fader's GPU compositor (NEW-23), from a static SWD framebuffer capture: the
  software golden draws 12 visible ticks per fader (y=143,157,170,…,291) while
  the GPU drew exactly two — tick i=0 (the first rect of the bright pass's
  path) and i=1 (the first of the dim pass's), and nothing else, on every
  boot. The same rule explains a cap that rendered SOLID DARK: its border was
  a two-contour ring, and the first contour — the outer rounded rect — filled
  solid in the border colour (`0x20262A`). It also explains per-boot-varying
  whole-framebuffer checksums (7 boots, 7 values) that vanished once every
  path became single-contour. **This SUPERSEDES the earlier belief that
  disjoint same-winding subpaths are fine** — the tick evidence disproves it —
  and it is why the sibling rotary compositor always worked: every path it
  builds is already a single contour. The fix is one `vg_lite_draw()` per
  contour, plus culling shapes whose screen bbox misses the clip (which pays
  for the extra draws: during animation the clip is a narrow strip, so only
  2-3 tick runs survive per frame). **Rule for any future vg_lite path in this
  tree: build and submit one contour per draw call — never rely on multiple
  `VLC_OP_MOVE`s inside one path to render more than the first.**
  ★★ **THAT ONE-CONTOUR RULE IS REFUTED AS STATED — measured 2026-08-30, two
  boots byte-identical.** `display/vglite_conformance` ran it as a controlled
  experiment and the wide rule did not survive. What DID: the **CLOSE
  ENCODING** matters, decisively. Four DISJOINT bars in one path render as ONE
  contour with the ordinary `01 00 00 00` CLOSE slot (`runs=1`, `fill=1393` —
  one bar plus antialiasing) and as ALL FOUR with the slot padded to
  `01 01 01 01` (`runs=4`, `fill=5120`). Identical geometry, identical
  predicate; the encoding is the only variable. That padding is exactly NXP's
  own `CHIPID == 0x355` workaround (`vg_lite_path.c:556-570`, inside
  `vg_lite_append_path` — which nothing here calls, the only hits being LVGL's
  ThorVG PC shim), and `VLC_OP_END` being `0x00` is why: every contour boundary
  this tree has ever emitted carries three END bytes inside its CLOSE slot.
  ★★ **But the wide rule is refuted, not merely narrowed, and the mechanism is
  NOT identified.** Two NESTED-contour paths using the ORDINARY encoding
  rendered BOTH contours correctly — the non-zero ring cut its hole
  (`rim=1 centre=0`) and the same-winding nest honoured both fill rules
  (`eoc=0 nzc=1`). A truncate-at-the-first-CLOSE story explains the
  four bars and explains NEITHER of those, which carry the same CLOSE-then-MOVE
  boundary. Three Phase-1 predictions were wrong in exactly this direction
  (predicted `broken`, measured `ok`); each is recorded with its reason in
  `expected_silicon.txt` rather than re-goldened away. What the matrix does not
  separate is DISJOINT-vs-NESTED from FOUR-contours-vs-TWO. **Phase 1b
  (2026-09-01, two boots) ANSWERED IT: DISJOINTNESS is the variable.**
  ★★★ **AND PHASE 2 (2026-09-02) WENT FURTHER: HOLE CUTTING is the variable,
  not nesting.** Closing the `evenodd` pass-1 coverage gap — that pass IS an
  EVEN_ODD hole and had never been checked — gave `eocover=short:308`,
  identical on both boots, while pass 2 (same winding, NO hole) is exact. Every
  hole-cutting render in the matrix mis-covers (ring `short:769`, evenodd pass 1
  `short:308`, four-nested-rings `stray:1171`); the one that cuts no hole does
  not. The two boots even disagreed on `nzfill` while agreeing EXACTLY on
  `eofill` — which read as "the nondeterminism lives in the no-hole pass"
  until a FOURTH boot (2026-09-02, the gradients run) read `eocover=short:459`
  after three boots of `short:308`. RETRACTED as a general claim: both passes
  vary boot to boot; the hole-cutting one had merely not varied yet. Two
  disjoint contours fail exactly as four do (`path/two-disjoint-bars`,
  `runs=1` of 2, byte-identical on both boots); four nested contours work
  exactly as two do (`path/four-nested-rings`, `runs=4`). So the rule is
  neither "only the first contour renders" nor "more than two breaks" — it is
  DISJOINT contours in one path under the ordinary zero-padded CLOSE encoding.
  ★★ **AND THE SECOND BOOT CHANGED THE CONCLUSION.** `four-nested-rings` read
  `repeat=same` on boot 1 and `repeat=differs` on boot 2. One boot would have
  recorded that nested contours are clean. They render CORRECTLY every time and
  NOT DETERMINISTICALLY, and two of the three nested cases now show it. **So
  this does NOT license nested multi-contour paths** — correct-but-
  nondeterministic is unsafe for a delta-rendering compositor, which is exactly
  how NEW-20's winding-2 defect presented. `expected_silicon.txt` records that
  cell's repeat as `unstable`, a third state the checker takes ONLY on
  `repeat`, ONLY with a written reason, and always while printing which way the
  run landed.
  ★★★ **WITH COVERAGE CHECKED, ALL FOUR CELLS OF THE 2x2 ARE BROKEN** (measured
  2026-09-01, two boots, every verdict identical). `two-contour-ring-nonzero`
  had reported `ok` since Phase 1 on two sample points and NO fill check —
  with one it reads `fill=4607` vs analytic 5376, `cover=short:769`, 14% of the
  ring MISSING. So nested multi-contour paths are wrong in BOTH directions:
  two nested draw 769 px too FEW, four draw ~1150 too MANY. "Nested is OK" was
  an artefact of checking only structure.
  ★ **`path/two-draws-ring` is now the load-bearing control of the whole
  matrix**: the SAME ring built as two single-contour paths and two draws
  measures `fill=5376` EXACTLY, beside a single-path version of the identical
  ring that is 769 px short. One-contour-per-path is therefore DIRECTLY
  MEASURED against its own counterexample rather than a conservative guess —
  and it is what both compositors already do.
  ★★ **STRAY COVERAGE, and it reframes all of the above.** Measured fill vs
  exact analytic area: the two axis-aligned CONTROLS land EXACTLY
  (`single-contour-rect` 6400, `multi-contour-close-padded` 5120 — zero AA cost
  on this pipeline) while every unpadded multi-contour path draws EXTRA —
  disjoint-4 +113, disjoint-2 +42, nested-4 **+1171** — and the excess scales
  with how much path data FOLLOWS the first CLOSE. **That is not truncation and
  not antialiasing: it is the parser continuing and MISREADING**, which also
  explains the nondeterminism (a desynchronised parse reads whatever is in
  memory). `display/vglite_conformance`'s `pixel=` verdict now requires the
  structural predicate AND fill within tolerance, so it means the picture is
  RIGHT rather than merely structurally right; `four-nested-rings` is therefore
  `broken`, not `ok`. Tolerance is `k*perimeter`, NEVER a percentage of area —
  a 5% band would have false-`broken`ed `self-intersecting`, a CONTROL, since a
  pentagram carries 474 px of all-diagonal boundary on 2792 px of area.
  ★★ **PHASE 2 (2026-09-01) FOUND THAT THE MATRIX HAD NEVER TESTED THE BLEND
  MODE PRODUCTION USES.** All fifteen Phase 1 cases render with
  `VG_LITE_BLEND_NONE`; both shipping compositors use `VG_LITE_BLEND_SRC_OVER`
  exclusively, twelve call sites. Five colour/blend cases MEASURED 2026-09-02, two boots,
  every verdict identical (matrix 15 → 20, sweep unchanged at 124; host suites
  305 → 410 checks). `cases=20 ok=15 broken=5 repeat_differs=2`, timeouts 0.
  ★ **The driver's own header is internally inconsistent about whether
  `SRC_OVER` is premultiplied** — `inc/vg_lite.h:452`/`:458` file mode 1 as
  non-premultiplied while `:461` gives it `S + D*(1-Sa)`, the PREmultiplied
  operator, and `:481` gives mode 11 the non-premultiplied `S*Sa + D*(1-Sa)`
  which `:137` aliases `PREMULTIPLY_SRC_OVER`. Names and formulas inverted. The
  cases admit BOTH readings and report which, rather than pre-judging.
  ★★★ **MEASURED: THIS SILICON IMPLEMENTS READING B — `SRC_OVER` IS THE
  PREMULTIPLIED OPERATOR** (`v=255 a=255 model=B` in both colour cases, and they
  AGREE). And **the FADER feeds it NON-premultiplied colour**:
  `synthui_fader_gpu.cpp`'s `abgr_a()` packs unscaled RGB with a separate alpha
  and passes it to `SRC_OVER` at alpha 115 (a shadow), `pal->gloss_opa` (a
  highlight) and a variable `opa` (the tick runs). Under `S + D*(1-Sa)` the
  source contributes at FULL intensity whatever its alpha, so a white gloss at
  partial opacity SATURATES instead of reading as a sheen. **The fix is one line
  in `abgr_a`** — premultiply RGB by `a/255`. **DONE 2026-09-02** (SynthUI
  `d995e63`): it moved the fader's two GPU goldens and nothing else, over two
  boots × four passes. The eight `a=255` call sites are bit-identical because
  `(v*a + 127)/255` is exact at both rails — checked exhaustively, not argued.
  ★ **No gate in this tree can see that code**, so the fix ships with a HOST
  test (SynthUI `tests/fader_color_test.c`, 69017 checks) rather than a gate:
  the QEMU gate runs the SW engine and the GPU goldens live only in a
  hand-pressed transcript. DEMONSTRATED RED against three mutants; the
  load-bearing arm is the one asserting the packing DIFFERS from the unscaled
  one, since without it every other check in the file is equally satisfied by
  the defect.
  ★ **`synthui_rotary_knob_gpu.cpp` is NOT affected**, structurally rather than
  by luck: it has no `abgr_a`, and every colour it draws goes through
  `abgr(hex)` which forces `0xFF000000` — always opaque. At α=255
  `S + D*(1-Sa)` reduces to `S`, correct under either reading.
  ★ **Sensitivity limit:** under reading B a saturated white source clamps to
  255 in cases 2-4, so their colour tolerances do nothing and the ALPHA ROW is
  what discriminates. A non-saturated source is the obvious next case.
  ★ `BLEND_NONE` measured `v=255 a=128 read=raw` — source written raw, alpha row
  `A: Sa`, exactly as documented. All fifteen Phase 1 cases used it with an
  opaque colour and are unaffected.
  ★ **The alpha row is the one unambiguous part and it is what catches a GPU
  that discards alpha**: `:462` gives `A: Sa + Da*(1-Sa)` = 255 over an opaque
  backdrop under BOTH readings, while alpha-ignoring leaves 128. Without it,
  a saturated white source makes reading B observationally identical to writing
  the source raw. `blend/none-honours-alpha` deliberately does NOT judge alpha
  — `BLEND_NONE`'s row is `A: Sa`, so 128 is correct there.
  ★ **The "SRC_OVER of AA paths is not idempotent" quirk is RETIRED, not
  confirmed**: that drift is correct alpha compositing, true of every
  conforming implementation. `blend/srcover-double` instead asserts the second
  composite lands where the formula predicts FROM THE MEASURED FIRST.
  ★ **KEEP FOLLOWING ONE-CONTOUR-PER-PATH** — now for two independent reasons.
  The mechanism is still unidentified: "disjoint" describes the geometry, not
  why the tessellator drops it. One clue: bar 0 renders 1393 px inside the
  four-bar path and 1322 px inside the two-bar path — same bar, different
  bounding box, and the driver derives its tessellation window from that box.
  ★ `path/evenodd-vs-nonzero` is one of TWO cases reporting `repeat=differs` (with `path/four-nested-rings`): its two
  IDENTICAL back-to-back renders produce different pixels, REPRODUCIBLY
  (byte-identical on both boots). Not boot-to-boot noise but a repeatable
  in-boot difference between two identical draw sequences — the class that hid
  NEW-20's winding-2 track defect, and the reason both compositors keep a
  per-boot delta-equality check.
  ★★ **THE GUARD LAYER IS BUILT (2026-09-02)** — `VGLite/port/vglite_guard.h`
  (VGLite `4b75168`), and both compositors are retrofitted onto it (SynthUI
  `44a1c58`). It refuses, BEFORE the driver sees them, any path with more than
  one `VLC_OP_MOVE` (the measured defect) or without a trailing `VLC_OP_END`
  (the Phase 1 hang, which no return-code check can see). `GPU_TRY` in both
  files is now the shared `VGLITE_GUARD_TRY`.
  ★ **The acceptance test is that NOTHING MOVED** — a guard that alters a
  rendered pixel has changed behaviour rather than constrained it. Verified on
  silicon for BOTH widgets: fader `fd_crc=0x814F4047` /
  `delta==fresh=0xE9A9A2B5`, knob all six `KNOB_SUM_*` plus
  `KNOB_DELTA_SEQ=FULL=0x7C9EC8DB` and `irqs=64`, every value bit-identical to
  its pre-guard transcript, with **`gpu_err=0` on both** — the guard validated
  every path either compositor builds and refused none.
  ★ **The validator is PURE and host-tested** (`VGLite/tests/run.sh`, 58
  checks, an arm per status, RED against four mutants) because **no gate in
  this tree can see GPU code**: every QEMU gate runs the software engine. That
  split is the whole reason the layer has automated coverage at all.
  ★★★ **PHASE 3 WAS PROBED (2026-09-02, six blit/scissor cases, THREE boots,
  every line byte-identical; matrix 32, host suites seven).** All six
  predictions held. **The scissor is TWO mechanisms**: right/bottom go to
  register `0x0A13` in `set_render_target` in every regime; left/top exist
  ONLY as the tess-window clamp inside `vg_lite_draw`, which is skipped in the
  fullscreen regime. `scissor/tess-fullscreen` (a second 64×64 target under
  the 64×64 tess buffer) read **`L=0,T=0,R=1,B=1`** — the fader header's
  warning measured and sharpened: `vg_lite_init()` with the panel's own size
  loses the LEFT and TOP scissor edges and keeps the other two, which is half
  a clip. `scissor/basic` in the shipping multi-tile regime clips all four.
  **The 64-B blit stride rule is a DRIVER check** (`_check_source_aligned`,
  `vg_lite.c:1383`): an 80-B stride is refused with `INVALID_ARGUMENT` before
  any command is built (`blit/stride-unaligned`, `rc=1`, nothing drawn), the
  rotary bench's padded layout blits unsheared (`blit/stride-64`), and RGB565
  reads red from the LOW five bits with 5-bit channels expanded by
  replication (`blit/formats`, `order=low`, pinned in code with an arm that
  proves the pin). A8/L8 and `scissor_rects` deliberately unprobed.
  ★★★ **THE GRADIENTS WERE PROBED (2026-09-02, six linear cases, two boots,
  every line byte-identical).** Matrix 20 → 26; host suites 631 checks over
  six. The driver fact that decides it, read then confirmed in pixels:
  `vg_lite_draw_linear_grad` takes the paint parameter from `grad->matrix`
  ALONE and applies `path_matrix` only to the geometry — an EXT gradient lives
  in SCREEN space. `ext-linear-moved` BROKEN (`l=189`), `ext-linear-reupdate`
  BROKEN with the IDENTICAL profile — a second update is idempotent and leaks
  one ramp (that prediction was changed from "double transform" BEFORE the
  boot, by algebra, and held) — `ext-linear-rebuilt` ok. **Re-specify the
  gradient line in screen space per placement; never cache a ramp across
  moves.** `ramp-word-order` ok: the ABGR8888 sampler reads the ramp as the
  driver packed it (A,B,G,R).
  ★★ **THE "GC255-ONLY, RENDERS BLACK" LEGACY CLAIM IS REFUTED AND RETIRED.**
  `grad/legacy-linear` was pre-registered `broken`/`unstable` on the strength
  of one sighting and measured **ok, `repeat=same`, two boots identical** — a
  textbook ramp within one unit of the host model. With a CORRECT matrix
  (`identity; translate(x,y); scale(w/1024)` — the helpers post-multiply) the
  legacy API works and is deterministic; the earlier black was that caller's
  identity matrix sampling ~6% of a 1024-px ramp. Its colours are the driver's
  own `0xAARRGGBB` (`vg_lite_context.h:95-99`), NOT `vg_lite_color_t` ABGR.
  ★ The host model reproduces the driver's gradient entry points FROM SOURCE
  (`tests/model.h`), so the predictions were executable before the press; the
  arm that earns the suite is "paint FOLLOWS the path", which inverts the
  moved/reupdate/rebuilt row by name. Building it exposed a latent model
  defect — `vgc_ident()` returned an all-zero matrix, invisible to 334 checks
  because the flat rasteriser never read it.
  ★ **Two prior cells moved on the repeat axis**: `path/two-disjoint-bars` is
  now `unstable` (first `differs` in six boots) and `evenodd`'s hole-cutting
  pass varied for the first time. A misparse's determinism depends on the
  bytes after the path, and these boots ran a different image.
  ★ **Gradient helpers were DELIBERATELY NOT BUILT.** The spec conditioned them
  on the probe confirming the API unusable, and **the probe never tested
  gradients** — Phase 2 was redirected to colour and blend. Those claims come
  from reading NXP's source, not a boot. A recorded gap, not an omission.
  ★ `docs/gc355-vglite-quirks.md` is the reference for all of this — one row
  per feature (verdict · safe usage · evidence), every row citing the
  `display/vglite_conformance` case id that establishes it, so a claim without
  a probe case is visibly a claim without evidence.
- **Dual-core model**: the CM7 stages/boots/hot-swaps CM4 images and talks over
  the MU mailbox. Key constraints: main-eDMA completion IRQs reach the CM7
  only (CM4 interrupt-driven DMA needs eDMA_LPSR + an LPSR peripheral);
  the CM4 has no interrupt on fast-GPIO ports; DMA can't reach CM4 private
  TCM; each peripheral instance is assigned to one core.
- **Board traps**: header pin A5 (`GPIO_AD_08`) doubles as `USB_OTG2_ID` — an
  OTG adapter in the second USB port clamps A5 to 0 V and kills header I²C.
  FlexCAN RX mailboxes lock when their C/S word is read.
- `docs/` holds the QEMU peripheral status table, the RevC3 Arduino-header
  pin audit, and `docs/superpowers/{specs,plans}/` — timestamped design docs
  for past bring-up phases (historical record; old flat example paths in them
  are pre-2026-07-20).
