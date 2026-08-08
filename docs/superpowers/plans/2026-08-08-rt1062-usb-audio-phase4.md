# RT1062 USB Audio (Phase 4) Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** `usb_audio_uac1_test` builds and gates on both boards, and plays an
audible 1 kHz tone from the MIMXRT1060-EVKB's J47 host port.

**Architecture:** Four firmware edits to one example (console alias, TEENSY_VERSION
guard, sample rate, toolchain + `boards` sidecar), then a new gate script that
serves both boards through `gate-lib.sh`. The gate asserts the USB **control
plane** only; silicon is the sole proof audio moves.

**Tech Stack:** CMake, ARM GCC 10, POSIX sh gates, QEMU `mimxrt1060-evk` /
`mimxrt1170-evk`, LinkServer + DAPLink for silicon.

**Spec:** `docs/superpowers/specs/2026-08-08-rt1062-usb-audio-phase4-design.md`

---

## Critical context for the implementer

- **Run gates as `./run_qemu.sh`, never `sh run_qemu.sh`** — `gate_init` does
  `exec gtimeout ... "$0"` and needs the shebang honoured.
- **Ask the runner for gate counts** (`./tools/run-all-qemu-gates.sh -l`), never
  a bare `find`. Its trailing `(N gate(s))` line means `wc -l` is one more.
- **Zero SKIP is load-bearing.**
- **A gate never names a QEMU machine, build dir or `-serial` chain.** Ask
  `gate-lib.sh` (`gate_qemu_machine`, `gate_build_dir`, `gate_console`).
- `cores/` and `teensy-cmake-macros/` showing untracked is NORMAL.
- **Commit messages go in a scratch file and use `git commit -F`.** Inline
  heredocs with apostrophes have caused shell parse errors in this repo.
- **★ Hardware safety.** Never leave a serial reader attached across ANY
  LinkServer operation — it can kernel-panic the host Mac. Kill readers → flash
  → kill probe daemons → attach reader. **The MIMXRT1060-EVKB has no SW4**;
  Task 4 uses a debug-build countdown instead of a reset button.
- **`rt1176:dualcore/cm4_audio_test` is a nondeterministic permitted red.** Load
  does not predict it. It is the ONLY permitted red.

---

### Task 0: Baseline

- [ ] **Step 1: Confirm tree and current counts**

```bash
cd ~/Development/rt1170/evkb && git branch --show-current && git status --short
./tools/run-all-qemu-gates.sh -l | tail -1
```

Expected: branch `rt1060-board-axis`; only `?? cores/` and
`?? teensy-cmake-macros/`; `(84 gate(s))`.

- [ ] **Step 2: Confirm the example builds for rt1176 today**

```bash
cd examples/usb/usb_audio_uac1_test && cmake --build build 2>&1 | tail -3
```

Expected: builds clean. This example has no gate yet, so a broken build here
would otherwise surface later as a confusing gate failure.

---

### Task 1: Firmware — make the example two-board

**Files:**
- Modify: `examples/usb/usb_audio_uac1_test/usb_audio_uac1_test.cpp`
- Modify: `examples/usb/usb_audio_uac1_test/CMakeLists.txt:4`
- Create: `examples/usb/usb_audio_uac1_test/toolchain/rt1062-evkb.toolchain.cmake`
- Create: `examples/usb/usb_audio_uac1_test/boards`

- [ ] **Step 1: Add the CONSOLE alias and the rate define**

In `usb_audio_uac1_test.cpp`, immediately **before** the line `USBHost myusb;`,
insert:

```cpp
// The console is LPUART1 on BOTH boards. The two cores just name it
// differently: cores/imxrt1176 calls LPUART1 `Serial1`, while cores/teensy4
// follows the Teensy pin-0/1 convention and calls LPUART6 `Serial1` and LPUART1
// `Serial6`. Naming it once here is what keeps QEMU and silicon reading the same
// wire -- on the MIMXRT1060-EVKB, LPUART1 (GPIO_AD_B0_12/13) is the DAPLink VCOM,
// whereas LPUART6 only reaches Arduino header pins D0/D1.
#if defined(ARDUINO_MIMXRT1060_EVKB)
#define CONSOLE Serial6
#else
#define CONSOLE Serial1
#endif

// Requested sample rate. 48000 and not the Audio library's 44100 on purpose:
// QEMU's usb-audio model offers 48000 ONLY (USBAUDIO_SAMPLE_RATE is a
// compile-time #define in hw/usb/dev-audio.c with no property to change it), and
// gate builds share build/ and build-rt1062/ with silicon builds -- so there is
// no gate-only override to hide a difference in. The J47 device supports both
// rates, so one binary claims in QEMU and plays on the bench.
//
// Phase 5's capstone will want 44100 back, to match the Audio library. This is
// the knob it turns.
#define UAC1_RATE_HZ 48000u
```

- [ ] **Step 2: Point every console call at the alias**

```bash
cd ~/Development/rt1170/evkb/examples/usb/usb_audio_uac1_test
sed -i '' 's/\bSerial1\./CONSOLE./g' usb_audio_uac1_test.cpp
grep -n 'Serial1' usb_audio_uac1_test.cpp
```

Expected: the only remaining `Serial1` hits are inside the comment block from
Step 1 and the `#define CONSOLE Serial1` line. **If any bare `Serial1` remains
in code** (e.g. `while (!Serial1)`), change it by hand — `sed` only catches
`Serial1.` with a dot.

- [ ] **Step 3: Use the rate define**

Replace the line:

```cpp
    audioOut.format(44100, 2, 16);   // shipping target: 44.1k, matches the Audio library
```

with:

```cpp
    audioOut.format(UAC1_RATE_HZ, 2, 16);   // see UAC1_RATE_HZ above
```

- [ ] **Step 4: Guard TEENSY_VERSION**

In `CMakeLists.txt`, replace line 4 (`set(TEENSY_VERSION 117 CACHE STRING "")`)
with:

```cmake
# Fallback only: the toolchain file sets this FORCE-fully, so a toolchain-driven
# build wins. A bare `cmake -B build .` with no toolchain still selects 117.
# Left unguarded this caches 117 and silently builds an RT1176 image into
# build-rt1062/, which boots the wrong QEMU machine and fails looking like a
# board or model problem rather than a build misconfiguration.
if(NOT DEFINED TEENSY_VERSION)
    set(TEENSY_VERSION 117 CACHE STRING "")
endif()
```

- [ ] **Step 5: Add the rt1062 toolchain file**

It is a verbatim copy — this example sits at the same depth as `serial_test`, so
the `get_filename_component` walk to the repo root is identical.

```bash
cd ~/Development/rt1170/evkb/examples
cp serial/serial_test/toolchain/rt1062-evkb.toolchain.cmake \
   usb/usb_audio_uac1_test/toolchain/rt1062-evkb.toolchain.cmake
```

- [ ] **Step 6: Declare the boards**

Create `examples/usb/usb_audio_uac1_test/boards`:

```
# Boards this example is built and gated for. See
# docs/superpowers/specs/2026-08-08-rt1062-usb-audio-phase4-design.md
#
# rt1062 is the point of Phase 4: this is the first time usb_audio*.cpp compiles
# for __IMXRT1062__ at all.
rt1176
rt1062
```

- [ ] **Step 7: Build both boards**

```bash
cd ~/Development/rt1170/evkb/examples/usb/usb_audio_uac1_test
cmake --build build 2>&1 | tail -3
cmake -B build-rt1062 -DEVKB_BOARD=rt1062 \
      -DCMAKE_TOOLCHAIN_FILE=toolchain/rt1062-evkb.toolchain.cmake
cmake --build build-rt1062 2>&1 | tail -3
```

Expected: both produce a `.elf`. **This is the first compile of `usb_audio*.cpp`
for `__IMXRT1062__`.** If it fails on a chip-specific assumption, that is the
risk the spec flagged (§6): fix it in the USBHost_t36 transport layer where the
`__IMXRT1176__` / `__IMXRT1062__` pattern already exists — **not** in this
example, and do not add a guard that merely hides it.

- [ ] **Step 8: Prove the rt1062 image is genuinely RT1062**

```bash
/Applications/ARM_10/bin/arm-none-eabi-readelf -h build-rt1062/usb_audio_uac1_test.elf | grep Entry
/Applications/ARM_10/bin/arm-none-eabi-readelf -h build/usb_audio_uac1_test.elf | grep Entry
```

Expected: `0x60001000` for rt1062, `0x30001000` for rt1176. If they match, the
toolchain file was ignored — recheck Step 4.

- [ ] **Step 9: Prove the DMA structures are in OCRAM, not DTCM**

This is the Phase 3 failure mode and it is silent until the controller halts, so
check it here rather than discovering it on the bench.

```bash
cd ~/Development/rt1170/evkb/examples/usb/usb_audio_uac1_test
/Applications/ARM_10/bin/arm-none-eabi-nm build-rt1062/usb_audio_uac1_test.elf \
  | grep -iE "periodictable|enumbuf|memory_Pipe|memory_Transfer|audioOut|hub1"
```

Expected: every address begins `0x2020` (OCRAM). **Any `0x2000xxxx` address is
DTCM and will fail on silicon** — the OTG2 DMA master cannot reach it, and the
controller raises `USBSTS.SEI` and halts. If one appears, the `DMAMEM` guard for
that object is missing; fix it in USBHost_t36 or on the object's declaration in
this example, not by ignoring it.

- [ ] **Step 10: Commit**

Write the message to a scratch file, then `git commit -F`:

```
usb_audio_uac1_test: build for rt1062, console on LPUART1

First compile of usb_audio*.cpp for __IMXRT1062__. Adds the rt1062 toolchain
file, a boards sidecar, and the CONSOLE alias every two-board example in this
tree uses -- Serial1 on imxrt1176, Serial6 on teensy4, LPUART1 on both.

Requested rate moves to 48000 via UAC1_RATE_HZ. QEMU's usb-audio is 48000-only
and gate builds share build dirs with silicon builds, so there is no gate-only
override; the J47 device supports both rates, so one binary serves both. Stays a
define because Phase 5 wants 44100 back for the Audio library.

TEENSY_VERSION is now guarded: unguarded it caches 117 and silently builds an
RT1176 image into build-rt1062/.

No gate yet -- that is the next commit.
```

Stage: the `.cpp`, `CMakeLists.txt`, `boards`, and the toolchain file.

---

### Task 2: Observe what QEMU actually reaches

The gate's assertions must come from measured output, not from this plan's
guesses. The spec commits to `SITD PASS` and `streaming started` being
reachable, but that evidence is second-hand (from `usb_descriptor_survey`'s gate
comments, a different example). Verify it here **before** writing assertions.

**Files:** none — this task only observes.

- [ ] **Step 1: Run rt1176 by hand and capture**

```bash
cd ~/Development/rt1170/evkb/examples/usb/usb_audio_uac1_test
gtimeout 25 ~/Development/qemu2/build/qemu-system-arm \
  -M mimxrt1170-evk -global fsl-imxrt1170.boot-xip=on \
  -kernel build/usb_audio_uac1_test.elf \
  -display none -serial file:/tmp/uac1-1176.txt -d guest_errors -D /tmp/uac1-1176.dbg \
  -audiodev none,id=snd0 -device usb-audio,bus=usbhost.0,port=1,audiodev=snd0 \
  >/dev/null 2>&1
cat /tmp/uac1-1176.txt
```

- [ ] **Step 2: Run rt1062 by hand and capture**

Note `boot-ivt`, not `boot-xip`, and the plain single `-serial` (the rt1062
console is now LPUART1 = slot 0, same as rt1176):

```bash
cd ~/Development/rt1170/evkb/examples/usb/usb_audio_uac1_test
gtimeout 25 ~/Development/qemu2/build/qemu-system-arm \
  -M mimxrt1060-evk -global fsl-imxrt1062.boot-ivt=on \
  -kernel build-rt1062/usb_audio_uac1_test.elf \
  -display none -serial file:/tmp/uac1-1062.txt -d guest_errors -D /tmp/uac1-1062.dbg \
  -audiodev none,id=snd0 -device usb-audio,bus=usbhost.0,port=1,audiodev=snd0 \
  >/dev/null 2>&1
cat /tmp/uac1-1062.txt
```

- [ ] **Step 3: Record which tokens are actually present**

```bash
for f in /tmp/uac1-1176.txt /tmp/uac1-1062.txt; do
  echo "=== $f"
  for t in "UAC1-TEST: start" "DEVICE READY" "selected alt=" "UAC1-TEST: PASS" \
           "siTD posted" "SITD PASS" "streaming started" "HEARTBEAT seq=2" \
           "siTD POST FAILED" "SITD FAIL" "STREAM START FAILED"; do
    printf "  %-24s %s\n" "$t" "$(grep -c "$t" $f)"
  done
  grep -o "selected alt=[0-9]*" $f | sort -u
done
```

**Both boards should reach the same point** — the model is shared. Write down
the exact `selected alt=N` value; Task 3 asserts it literally.

**If either board stops earlier than `streaming started`:** that is a real
finding, not a reason to fudge. Assert only what is genuinely reached, and
record in the gate header *what it stops at and why*. Do not weaken silently and
do not assert something the capture does not contain.

**Expect `pkts/s=0` or no `pkts/s` at all.** Isochronous data does not flow
against this model. That is not a failure.

---

### Task 3: The gate

**Files:**
- Create: `examples/usb/usb_audio_uac1_test/run_qemu.sh` (mode `755`)

- [ ] **Step 1: Write the gate**

Create `run_qemu.sh` with the content below. **Adjust the `selected alt=1`
assertion to the value observed in Task 2 Step 3**, and delete any assertion
whose token Task 2 showed absent (recording why in the header).

```sh
#!/bin/sh
# QEMU gate for usb_audio_uac1_test -- the UAC1 OUT control plane, on both boards.
#
# WHAT THIS PROVES: that the host stack enumerates QEMU's emulated usb-audio
# device, CLAIMS it, selects the streaming alternate setting, completes the
# control sequence, posts an isochronous descriptor the controller accepts, and
# starts streaming. On rt1062 that is the first time usb_audio*.cpp has run at
# all -- the point of Phase 4.
#
# ★ WHAT IT CANNOT PROVE: that audio moves. Isochronous data does NOT flow
# against QEMU's model -- measured, and recorded in usb_descriptor_survey's gate:
# an OUT sketch built for 48000 "enumerates, claims, selects alt 1, completes the
# whole control sequence with ctrl=0/0/0 and reports streaming started, and then
# sits at pkts/s=0". So this gate MUST NOT assert pkts/s > 0. Silicon is the sole
# proof of the tone; see transcript_hw_evkb.txt.
#
# The example requests 48000 (UAC1_RATE_HZ) because QEMU's model offers only that
# rate. The J47 bench device supports 44100 and 48000, so the same binary serves
# both worlds.
set -e
DIR=$(cd "$(dirname "$0")" && pwd)
EVKB=$(cd "$DIR/../../.." && pwd)
QEMU="$EVKB/tools/qrun"
. "$EVKB/tools/gate-lib.sh"
gate_init
ELF="$DIR/$(gate_build_dir)/usb_audio_uac1_test.elf"; OUT="$DIR/uac1.uart"
rm -f "$OUT"
"$QEMU" $(gate_qemu_machine) -kernel "$ELF" \
    -display none $(gate_console "$OUT") -d guest_errors -D "$DIR/uac1.dbg" \
    -audiodev none,id=snd0 \
    -device usb-audio,bus=usbhost.0,port=1,audiodev=snd0 &
P=$!; gate_pid $P
# Poll for the last token asserted rather than guessing a duration -- a fixed
# sleep makes the gate load-sensitive, which cost this tree real time before.
for _ in $(seq 1 80); do
    [ -f "$OUT" ] && grep -q "HEARTBEAT seq=2 " "$OUT" 2>/dev/null && break
    sleep 0.25
done
gate_reap $P
gate_require_capture "$OUT"
echo "==== captured ===="; head -30 "$OUT"

grep -q "UAC1-TEST: start" "$OUT" || { echo "FAIL: banner"; exit 1; }
grep -q "UAC1-TEST: DEVICE READY" "$OUT" \
    || { echo "FAIL: AudioOut never claimed the emulated device"; exit 1; }
grep -q "UAC1-TEST: selected alt=1" "$OUT" \
    || { echo "FAIL: streaming alternate setting not selected"; exit 1; }
grep -q "UAC1-TEST: PASS" "$OUT" || { echo "FAIL: topology report"; exit 1; }
grep -q "UAC1-TEST: siTD posted, 180 bytes" "$OUT" \
    || { echo "FAIL: isochronous descriptor not posted"; exit 1; }
grep -q "UAC1-TEST: SITD PASS - controller sent the packet" "$OUT" \
    || { echo "FAIL: controller did not accept the siTD"; exit 1; }
grep -q "UAC1-TEST: streaming started, 1 kHz tone" "$OUT" \
    || { echo "FAIL: streaming did not start"; exit 1; }
grep -q "HEARTBEAT seq=2 " "$OUT" \
    || { echo "FAIL: loop did not survive to a second heartbeat"; exit 1; }

# Vacuity guards. Each of these is a state the firmware prints INSTEAD of the
# success token above it, so a loose grep could pass while the path failed.
grep -q "UAC1-TEST: siTD POST FAILED" "$OUT" && { echo "FAIL: siTD post failed"; exit 1; }
grep -q "UAC1-TEST: SITD FAIL" "$OUT" && { echo "FAIL: siTD reported error flags"; exit 1; }
grep -q "UAC1-TEST: STREAM START FAILED" "$OUT" && { echo "FAIL: stream start failed"; exit 1; }

echo "PASS: UAC1_OUT_CONTROL_PLANE"
```

- [ ] **Step 2: Make it executable**

```bash
chmod 755 ~/Development/rt1170/evkb/examples/usb/usb_audio_uac1_test/run_qemu.sh
```

- [ ] **Step 3: Run it on both boards**

```bash
cd ~/Development/rt1170/evkb && ./tools/run-all-qemu-gates.sh usb_audio_uac1_test
```

Expected: `PASS  rt1176:usb/usb_audio_uac1_test` and
`PASS  rt1062:usb/usb_audio_uac1_test`.

- [ ] **Step 4: Confirm the count moved 84 → 86**

```bash
cd ~/Development/rt1170/evkb && ./tools/run-all-qemu-gates.sh -l | tail -1
```

Expected: `(86 gate(s))` — one script, two board ids.

- [ ] **Step 5: Commit**

Scratch file + `git commit -F`:

```
usb_audio_uac1_test: gate the UAC1 OUT control plane on both boards

This example had no gate at all -- it was built only by hand, because QEMU had
no UAC device to enumerate until the emulated usb-audio device came into use.
One script, two board ids: sweep 84 -> 86.

Asserts claim, alternate-setting selection, topology, the posted siTD and the
controller accepting it, streaming start, and loop survival -- with vacuity
guards on the three FAILED/FAIL states the firmware prints instead.

It deliberately does NOT assert pkts/s > 0. Isochronous data does not flow
against QEMU's model; that is measured and recorded, and silicon is the sole
proof the tone plays. The gate header says so, so the limitation travels with
the script.
```

---

### Task 4: Silicon — the tone

**Files:**
- Create: `examples/usb/usb_audio_uac1_test/transcript_hw_evkb.txt`

★ **Read the hardware-safety rules in "Critical context" before this task.**

- [ ] **Step 1: Free the VCOM and clear stale probe daemons**

```bash
pkill -9 -f "rt1170-console" 2>/dev/null
pkill LinkServer; pkill redlinkserv; pkill crt_emu_cm_redlink
sleep 1; ls /dev/cu.usbmodem*; lsof /dev/cu.usbmodem* 2>/dev/null || echo "VCOM free"
```

Expected: `VCOM free`. A reader still holding a port that has disappeared is the
documented kernel-panic precondition — kill it before going further.

- [ ] **Step 2: Confirm the audio device is in J47**

J47 is the **host** port. J48 is the device port and will not work. The Phase 3
device was GeneralPlus `1B3F:2008`, which offers 2 ch / 16-bit at 44100 **and**
48000 — the rate this build requests.

- [ ] **Step 3: Flash**

```bash
cd ~/Development/rt1170/evkb/examples/usb/usb_audio_uac1_test
timeout 180 /Applications/LinkServer_26.6.137/LinkServer flash \
  MIMXRT1062:MIMXRT1060-EVKB load build-rt1062/usb_audio_uac1_test.elf 2>&1 | tail -3
```

Expected: `Finished writing Flash successfully` then `Starting execution using
system reset`.

- [ ] **Step 4: Drop the probe and read the console**

The flash resets the target, so the boot output is already gone by the time a
reader attaches — and **this board has no SW4 to press**. Capture the steady
state instead; the example prints a heartbeat every second and re-reports on
attach.

```bash
pkill LinkServer; pkill redlinkserv; pkill crt_emu_cm_redlink; sleep 2
cd ~/Development/rt1170/evkb
timeout 40 python3 tools/rt1170-console.py /dev/cu.usbmodem145302 115200 | tee /tmp/uac1-hw.txt
```

If the boot banner is needed, rebuild with
`-DCMAKE_CXX_FLAGS="-DUSBHOST_PRINT_DEBUG -DUSBHDBGSerial=Serial6"` into a
**separate** build dir (`build-rt1062-dbg`, so the gate's build is untouched) —
that build holds a 20 s countdown at startup for exactly this.

- [ ] **Step 5: Confirm the bar**

The spec's silicon bar (A) is: **audible 1 kHz tone** plus
`UAC1-TEST: SITD PASS`. Listen to the adapter's output, and check:

```bash
grep -E "DEVICE READY|selected alt=|SITD PASS|streaming started" /tmp/uac1-hw.txt
```

Record `pkts/s` if it appears — it is not part of bar A, but it is free, and
follow-up B wants it.

- [ ] **Step 6: Write the transcript**

Create `transcript_hw_evkb.txt` on this skeleton, pasting the real capture in
place of the bracketed parts and deleting any line that did not happen:

```
Hardware transcript -- usb_audio_uac1_test on the MIMXRT1060-EVKB (rt1062).

Recorded 2026-08-08, branch rt1060-board-axis, evkb <SHA>, cores <SHA>.
Board: MIMXRT1060-EVKB, DAPLink VCOM, console on Serial6 (LPUART1).
Device: <vid:pid> <product string>, in J47 (the HOST port; J48 is the device port).
Requested format: UAC1_RATE_HZ 48000, 2 ch, 16 bit.

★ THIS FILE IS THE ONLY EVIDENCE THAT AUDIO MOVES. The QEMU gate proves the
control plane and stops there -- isochronous data does not flow against QEMU's
usb-audio model. If this transcript and the gate ever disagree, silicon wins.

TONE: <audible / not audible> -- 1 kHz from the adapter's output.

==== console ====
<paste the capture>

Notes:
- pkts/s observed: <value>. Not part of this phase's bar; recorded for the
  follow-up that asks what the 600 MHz stall headroom actually is (spec 5-B).
- siTD flags: <SITD PASS / the failing flags>.
```

- [ ] **Step 7: Commit**

Scratch file + `git commit -F`, message naming the device, the rate, and whether
the tone was heard.

---

### Task 5: Close the phase

**Files:**
- Modify: `tools/license-audit.sh` (GATES list)
- Modify: `CLAUDE.md`, `docs/KNOWN-BROKEN-GATES.md`

- [ ] **Step 1: Add the rt1062 build to the licence audit**

In `tools/license-audit.sh`, find the line containing
`examples/usb/usb_audio_uac1_test:usb_audio_uac1_test` and add beneath it:

```
examples/usb/usb_audio_uac1_test/build-rt1062:usb_audio_uac1_test \
```

If that example is not in `GATES` at all, add both lines — it needs the rt1176
entry too.

- [ ] **Step 2: Run the audit, capturing FULL output**

```bash
cd ~/Development/rt1170/evkb && ./tools/license-audit.sh > /tmp/p4-audit.txt 2>&1; echo "exit=$?"
tail -2 /tmp/p4-audit.txt
grep "usb_audio_uac1_test" /tmp/p4-audit.txt
```

Expected: `LICENSE-AUDIT: PASS`, and both entries walked with non-zero dep-path
counts. **Do not `| tail` the audit itself** — a truncated log cannot tell you
what it covered.

- [ ] **Step 3: Full sweep**

```bash
uptime && cd ~/Development/rt1170/evkb && ./tools/run-all-qemu-gates.sh -j 2
```

Expected: `86 passed, 0 failed, 0 SKIP`, or `85 passed, 1 failed, 0 SKIP` with
only `rt1176:dualcore/cm4_audio_test` red. Any other failure is a real
regression.

- [ ] **Step 4: Update `CLAUDE.md`**

Change the sweep count 84 → 86 and extend the leading parenthetical with
`85 before Phase 4 gated usb/usb_audio_uac1_test on two boards;`. Update the
expectation numbers in the same paragraph (`84 passed, 0 failed` → `86 passed,
0 failed`; `83 passed, 1 failed` → `85 passed, 1 failed`).

- [ ] **Step 5: Update `docs/KNOWN-BROKEN-GATES.md`**

Append a dated entry to the sweep-result section recording: 84 → 86 from one new
script on two boards; that the gate proves the control plane only and why; that
silicon carried the tone; and the two follow-ups (the `pkts/s` stall-headroom
number, and the valid cache A/B via a dedicated non-cached `USBHOST_DMAMEM`
section).

- [ ] **Step 6: Commit**

Scratch file + `git commit -F`.

---

## Definition of done

- [ ] `usb_audio_uac1_test` builds for both boards; rt1062 entry `0x60001000`
- [ ] Gate green on both boards, asserting the control plane
- [ ] Audible 1 kHz tone from J47, `SITD PASS`
- [ ] `transcript_hw_evkb.txt` committed
- [ ] Sweep 86 gates, zero SKIP
- [ ] `LICENSE-AUDIT: PASS` with `build-rt1062` walked
- [ ] `CLAUDE.md` and `KNOWN-BROKEN-GATES.md` updated

## Not in this phase

The IN direction, `usb_audio_capture_test` (8 ch / 24-bit — the J47 device
offers 1 ch / 16-bit IN, so it would never claim), `usb_audio_duplex_test`,
`usb_audio_graph_test`, and the Phase 5 capstone. Follow-ups B and C from the
spec §5 are recorded there, not done here.
