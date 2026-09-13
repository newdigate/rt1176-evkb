# Flashing the MIMXRT1170-EVKB: Safety Conventions & Skill — Design

**Status:** Approved (design), ready for implementation plan  
**Date:** 2026-09-13  

---

## 1. Goal

Create a hardened, panic-free flashing workflow and agent skill (`flashing-rt1170-evkb`) for the NXP MIMXRT1170-EVKB development board. The skill and its companion tooling enforce strict bench safety, prevent macOS `IOSerialFamily` kernel panics, manage multi-agent/multi-session hardware arbitration, decouple debugger/serial logging for live tailing, and codify microcontroller debugging practices to minimize unnecessary flash wear.

---

## 2. Background & Motivation

Hardware work on the MIMXRT1170-EVKB carries specific bench traps that have historically caused kernel panics and wedged probe hardware:

1. **macOS `IOSerialFamily` Kernel Panic (The Closed/Dangling TTY Trap)**:
   - When LinkServer reprograms the RT1176, the onboard MCU-Link debug probe resets and re-enumerates over USB.
   - If any process (e.g. Python with `pyserial`, `cat`, or a terminal emulator) holds the VCOM port (`/dev/cu.usbmodem...`) open across this re-enumeration, macOS's `IOSerialFamily` driver triggers a use-after-free, causing a host kernel panic.
   - Re-opening a closed port mid-reconnect or racing on the tty can likewise panic the machine.
2. **MCU-Link Bus Power vs. Target Silicon Power**:
   - The onboard MCU-Link debug chip is powered directly by the USB cable (J17). It enumerates on the host even when the RT1170-EVKB main power switch (SW1) is **OFF**.
   - Attempting to flash an unpowered board fails with cryptic `Wire not connected` or DAP transfer errors.
3. **Stale Daemons vs. Probe Brick Risk**:
   - `pkill LinkServer` leaves background daemons (`redlinkserv`, `crt_emu_cm_redlink`) resident, silently blocking subsequent sessions.
   - Conversely, sending `SIGKILL` (`kill -9`) to a LinkServer session *mid-flash-program* wedges the SWD wire interface (`Hardware interface transfer error`), requiring a physical power cycle.
4. **Multi-Agent / Multi-Session Collisions**:
   - Multiple agents or concurrent developer terminals can race to claim the debugger or serial port, corrupting flashes or triggering kernel panics.
5. **Firmware Provenance & Flash Churn**:
   - Reflashing without verifying if code actually changed or whether a bug reproduces in QEMU wastes time and NOR flash write cycles.
   - When inspecting serial output, missing version/build metadata makes it impossible to know whether the output is from the newly flashed firmware or a leftover image in flash.

---

## 3. Architecture & Key Decisions

We follow a **dual-layered approach** (Script Hardening + Agent Skill):

```
┌─────────────────────────────────────────────────────────────┐
│               Agent Skill: flashing-rt1170-evkb             │
│  - Triggers on flashing, hardware runs, serial lockups      │
│  - Enforces: QEMU first -> Banner check -> Safe execution   │
│  - Guides operator on SW4 taps, log tailing, power switches │
└──────────────────────────────┬──────────────────────────────┘
                               │ invokes
┌──────────────────────────────▼──────────────────────────────┐
│             Tooling Layer: tools/rt1170-flash.sh            │
│  - Subcommands / flags: [image.elf|.hex] [--unlock]         │
│  - Auto-discovers / manages .env persistence                │
│  - Cooperative lock: /tmp/rt1170/evkb.lock                  │
│  - Non-invasive SWD power ping (LinkServer dapinfo)         │
│  - Strict abort on active readers (Option C)                │
│  - Clean daemon cleanup (pkill without -9)                  │
│  - Tailed logs: /tmp/rt1170/flash.log & serial.log          │
│  - Background console spawn + SW4 prompt                    │
└─────────────────────────────────────────────────────────────┘
```

### Decision 1: Environment Discovery & Persistence (`.env`)
- Config lives in project-root `.env` (added to `.gitignore`).
- Managed variables:
  - `LINKSERVER`: Path to LinkServer executable (default: `/Applications/LinkServer_26.6.137/LinkServer`).
  - `RT1170_PORT`: Path to MCU-Link VCOM tty (default: `/dev/cu.usbmodem5DQ2DDHVWO5EI3`).
  - `EVKB_DEVICE`: Target device identifier (default: `MIMXRT1176:MIMXRT1170-EVKB`).
  - `PY`: Python 3 executable with `pyserial`.
  - `ARM_GDB`: (Optional, for SWD probe) ARM toolchain GDB binary.
- Script automatically sources `.env`. If a variable is missing, it inspects standard paths, prompts the user interactively if ambiguous, and saves the resolved choices into `.env`.

### Decision 2: Board Power Verification (SWD Ping + Guidance)
- Check USB device presence (`[ -c "$PORT" ]`).
- Run non-invasive SWD probe: `"$LINKSERVER" probe '#0' dapinfo`.
- If `Wire not connected` is detected while USB is present, the script does not crash; it prompts the operator:
  `"MCU-Link detected on USB, but RT1170 target is unpowered. Please switch ON the board power switch (SW1) or check the 5V DC barrel jack, then press [Enter] to retry..."`
- Retries up to 3 times before failing cleanly.

### Decision 3: Multi-Session Arbitration & Serial Safety (Strict Abort)
- Central lock directory: `/tmp/rt1170/`
- Central lock file: `/tmp/rt1170/evkb.lock` containing `PID`, `COMMAND`, `HOLDER`, `START_TIME`.
- Stale check: If lock file exists but PID is dead (`kill -0 "$PID"` fails), lock is safely purged.
- **Strict Abort Rule**:
  - Scan for active lock holders, processes holding `$PORT` (`lsof -t -- "$PORT"`), running console scripts (`pgrep -f 'rt1170-console\.py'`), or probe daemons (`pgrep -f 'LinkServer|redlinkserv|crt_emu_cm_redlink'`).
  - If ANY conflicting process is found, **do not kill it automatically**.
  - Abort immediately with the offending PID and command, instructing the user to stop the process or run `tools/rt1170-flash.sh --unlock`.

### Decision 4: Separated Logs in System Temp (`/tmp/rt1170/`)
- During flash: LinkServer output is streamed to `/tmp/rt1170/flash.log`. The script prints:
  `"==> Tail debugger output with: tail -f /tmp/rt1170/flash.log"`
- Post-flash: Once LinkServer exits and detaches, the script spawns `rt1170-console.py` in the background streaming to `/tmp/rt1170/serial.log`, updates the lock file with the console reader's PID, and prints:
  `"==> Background serial console started. Tail live output with: tail -f /tmp/rt1170/serial.log"`
  `"==> Press SW4 (RESET) on the EVKB board to boot the new image."`

### Decision 5: ELF Exit Code -11 Fallback to HEX
- LinkServer's ELF loader occasionally fails with exit code -11 due to program-header parsing quirks on specific images.
- If flashing a `.elf` fails with exit code -11 and a sibling `.hex` file exists, the script notifies the operator and retries flashing using the `.hex` file.

### Decision 6: Clean Unlock Command
- Running `tools/rt1170-flash.sh --unlock` (or `--stop-console`):
  1. Reads PID from `/tmp/rt1170/serial.pid` / `/tmp/rt1170/evkb.lock`.
  2. Sends `SIGTERM` to the background reader.
  3. Waits up to 2 seconds and verifies with `lsof` that the port is freed.
  4. Removes the lock file.

---

## 4. Microcontroller Debugging Conventions & Best Practices

The skill codifies the following repo-tested conventions:

### 1. Versioned Boot Banners & Build Provenance
Every firmware example must emit a structured banner on `Serial1` (115200 baud) in `setup()`:
```cpp
Serial1.begin(115200);
while (!Serial1 && millis() < 2000) {}

// [APP:<name>] [VER:<gate_version>] [BUILD:<timestamp>]
Serial1.println("=== BOOT ===");
Serial1.printf("[APP: %s] [VER: v%u] [BUILD: %s %s]\n",
               "my_example", 1, __DATE__, __TIME__);
```
When tailing `/tmp/rt1170/serial.log`, agents must check that `[BUILD: ...]` matches the newly compiled binary. If the timestamp did not advance, the core did not execute the new image.

### 2. "Probe Before Flash" Rules
To avoid unnecessary flash write cycles and debug latency:
1. **Gate in QEMU First**: Run `tools/rt1170-qemu.sh build/<app>.elf` or `./run_qemu.sh`. If it fails in QEMU, do not flash to silicon.
2. **SWD State Probe (`tools/rt1170-swdprobe.py`)**:
   - Inspect live memory and symbols without stopping the core or flashing printfs:
     `tools/rt1170-swdprobe.py --elf build/app.elf -c "p my_var"`
   - Distinguish real freeze from debugger halt via `tools/rt1170-swdprobe.py --health`:
     - `DHCSR` (0xE000EDF0): `0x01010001` = healthy running (`S_RETIRE_ST=1`), `0x00030003` = halted by debugger (`S_HALT=1`).
     - Fault registers: `CFSR`, `HFSR` (0xE000ED28/2C) = 0 means no fault taken.
3. **SWD Framebuffer Inspection (`tools/rt1170-screenshot.py`)**:
   - For graphical / LVGL / panel tests, dump the framebuffer over SWD directly into a PNG.
4. **Corroborate Liveness with Dual Clocks**:
   - Never declare firmware frozen based on one counter alone; cross-verify `systick_millis_count` with audio sample counts or peripheral timers.

---

## 5. File Layout & Deliverables

1. **`tools/rt1170-flash.sh`** [MODIFY]:
   - Add `.env` loading and interactive auto-discovery.
   - Add `/tmp/rt1170/evkb.lock` acquisition and stale detection.
   - Add non-invasive SWD board power pre-flight check with interactive prompt.
   - Add strict reader conflict detection (Option C) with diagnostic output.
   - Add logging to `/tmp/rt1170/flash.log` and post-flash background `rt1170-console.py` logging to `/tmp/rt1170/serial.log`.
   - Add `.hex` fallback on ELF exit code -11.
   - Add `--unlock` / `--stop-console` subcommands.
2. **`.gitignore`** [MODIFY]:
   - Add `.env` to ignored files.
3. **`.claude/skills/flashing-rt1170-evkb/SKILL.md`** [NEW]:
   - SDO description: `Use when preparing to flash, debug, or monitor firmware on the RT1170-EVKB hardware bench, or troubleshooting board connectivity, power states, or serial lockups.`
   - Checklist: QEMU gate -> Banner update -> Flash pre-flight -> Log tailing -> Banner verification -> Unlock.
   - Quick reference: SWD probe commands, DHCSR health interpretation, Mac kernel panic warning, common failure signatures.

---

## 6. Verification Plan

### Automated / Pre-Flight Tests
1. **Argument & Syntax Validation**: Run `bash -n tools/rt1170-flash.sh`.
2. **Conflict & Lock Test**:
   - Acquire a dummy lock in `/tmp/rt1170/evkb.lock`; verify `rt1170-flash.sh` aborts with the expected message.
   - Verify `--unlock` successfully cleans up the lock.
3. **Stale Lock Test**:
   - Create a lock file with a non-existent PID; verify `rt1170-flash.sh` cleans it up automatically.
4. **Reader Detection Test**:
   - Launch a mock reader process on the port; verify `rt1170-flash.sh` aborts and reports the reader PID without sending any signals.

### Hardware Verification (Interactive with User)
1. **Power-Off Pre-flight Test**:
   - With board power switch OFF and USB connected, run pre-flight check. Confirm it detects `Wire not connected` and prompts the user to turn on SW1.
2. **Full Flash & Logging Test**:
   - Turn on board power; execute flash on an example image.
   - Verify `/tmp/rt1170/flash.log` captures LinkServer programming output.
   - Verify background console starts writing to `/tmp/rt1170/serial.log`.
   - Verify tapping SW4 boots the image and the versioned banner appears in `serial.log`.
3. **Unlock Test**:
   - Run `tools/rt1170-flash.sh --unlock` and confirm the background console terminates and the port is clean.
