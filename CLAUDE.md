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
`docs/KNOWN-BROKEN-GATES.md`.** The sweep covers **111 gates**: 108 from the
merge of two independent lines that both branched from 94 — the display
capstone line added THREE (`display/vglite_lvgl_test`, then
`display/synthui_step_test` and `display/acid_box` from the Acid Box capstone,
reaching 97 on its own), and the M.2 Wi-Fi line added ELEVEN, reaching 105 on
its own (94 + 3 + 11 = 108) — plus W17's TWO on the new
`networking/m2_uap_probe` and ONE on `networking/m2_uap_lwip`. That arithmetic is
CHECKED against the runner rather than trusted: `-l` reports 111.

The M.2 line's own chain, kept because each step says what the gate is for:

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
`usb/usb_descriptor_survey`). The target is **111 passed, 0 failed, 0 SKIP**, or
**110 passed, 1 failed, 0 SKIP** when the nondeterministic dual-core gate is red.

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
is silicon-only with its OWN golden (`0xC3C6171A`) — two golden sets, never
reconciled (hardware AA ≠ LVGL mask arithmetic, and the GPU build carries
`LV_USE_FLOAT=1`). The Phase-2 fps criterion was measured and NOT met
(software 2.83 fps, GPU 2.45 fps, CPU-bound in the backend's per-task path
construction) — the GPU path is pixel-correct but not an optimisation as it
stands; `docs/superpowers/specs/2026-08-17-vglite-phase2-design.md` has the
verdict and the follow-up shape.

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
`networking/m2_sdio_probe[wifi]`, all SEVEN `networking/m2_rx_demo` gates and
`networking/m2_uap_probe[wifi]` need the **IW416 SDIO card model**
(`hw/sd/iw416-sdio.c`, enabled by `-machine mimxrt1170-evk,m2-wifi=on`), plus —
for `[irq]` — the SDIO card-interrupt plumbing W15 added to the SD bus and
SDHCI.
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
