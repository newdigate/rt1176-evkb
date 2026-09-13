---
name: flashing-rt1170-evkb
description: Use when preparing to flash, debug, or monitor firmware on the RT1170-EVKB hardware bench, or troubleshooting board connectivity, power states, or serial lockups
---

# Flashing & Debugging the MIMXRT1170-EVKB

## Overview
Safe hardware deployment and diagnostics for the NXP MIMXRT1170-EVKB. Enforces host protection against macOS `IOSerialFamily` kernel panics, cooperative multi-agent locking, non-invasive SWD pre-flights, and live log tailing.

## When to Use
- Flashing any firmware image (`.elf` or `.hex`) to the physical MIMXRT1170-EVKB board.
- Capturing boot banners and serial telemetry via the MCU-Link VCOM.
- Troubleshooting target connection failures (`Wire not connected`, `Ep(03). Invalid ID for processor`, or ELF code -11).
- Diagnosing apparent firmware hangs without inserting extra printfs.
- When NOT to use: Pure QEMU test runs (`qrun`, `run-all-qemu-gates.sh`).

---

## 1. Pre-Flash Discipline: "Probe Before Flash"

Flashing external NOR flash takes time and wears silicon. Never flash to investigate a bug that can be checked non-invasively:

### 1. Gate in QEMU First
Both RT1176 and RT1062 have comprehensive QEMU support in this repository.
```bash
tools/rt1170-qemu.sh build/<app>.elf
# or run the example's test harness:
./run_qemu.sh
```
If a logic error, missing initialization, or syntax defect reproduces in QEMU, fix it there first before touching silicon.

### 2. Versioned Boot Banners (Firmware Provenance)
Every firmware example must emit a structured, versioned boot banner on its debug serial port (`Serial1` on RT1170 at 115200 baud) in `setup()`:
```cpp
Serial1.begin(115200);
while (!Serial1 && millis() < 2000) {}

// [APP:<name>] [VER:<gate_version>] [BUILD:<timestamp>]
Serial1.println("=== BOOT ===");
Serial1.printf("[APP: %s] [VER: v%u] [BUILD: %s %s]\n",
               "my_example", 1, __DATE__, __TIME__);
```
**Verification Rule**: When reading `/tmp/rt1170/serial.log`, always verify that `[BUILD: ...]` matches the timestamp of the binary just built. If the timestamp does not advance, the target did not boot the new image.

### 3. Non-Invasive Live State Inspection (`tools/rt1170-swdprobe.py`)
Before modifying code to add debug `printf`s and reflashing, inspect variables and CPU state live over SWD (`LinkServer gdbserver --attach`):
```bash
# Read variables live without stopping the core:
tools/rt1170-swdprobe.py --elf build/my_app.elf -c "p my_counter"

# Check core health and fault registers:
tools/rt1170-swdprobe.py --health
```

### 4. Non-Invasive Framebuffer Inspection (`tools/rt1170-screenshot.py`)
For display and UI testing, dump the SDRAM framebuffer directly over SWD without disturbing the running scene or adding serial logging:
```bash
tools/rt1170-screenshot.py /tmp/display.png
```

### 5. Corroborate Liveness with Dual Clocks
A quiet serial port or frozen counter does not necessarily mean dead silicon. Compare `systick_millis_count` with an audio sample counter or hardware timer before assuming a crash.

### 6. Avoid Mass Erase (Sector Erase Only)
Do **NOT** run a standalone full-chip mass erase (`LinkServer flash erase`). The EVKB carries a 64 MB external Octal/FlexSPI NOR flash; a full-chip mass erase takes minutes of complete silence with no progress telemetry. This silence frequently confuses developers and AI agents into assuming the MCU or probe has hung, tempting an abort or `kill -9` (which mid-flash wedges the wire interface and requires a physical board power cycle). `tools/rt1170-flash.sh` uses `flash load`, which automatically erases only the sectors occupied by the target image.

---

## 2. Flashing & Execution Workflow

### Step 1: Environment Pre-flight
Verify that the bench environment variables are configured:
```bash
tools/rt1170-flash.sh --env-check
```
If any variables are missing or custom paths are needed, persist them in the project-root `.env` file (which is gitignored):
```bash
LINKSERVER="/Applications/LinkServer_26.6.137/LinkServer"
RT1170_PORT="/dev/cu.usbmodem5DQ2DDHVWO5EI3"
EVKB_DEVICE="MIMXRT1176:MIMXRT1170-EVKB"
PY="/usr/local/Caskroom/miniconda/base/bin/python3"
```

### Step 2: Execute Flash
Run the hardened flash script with your built image:
```bash
tools/rt1170-flash.sh build/my_app.elf
```

The script will automatically perform:
1. **Lock Check**: Verifies `/tmp/rt1170/evkb.lock`. If busy or an active reader holds the port, it aborts (Option C) to protect against kernel panics.
2. **Board Power Ping**: Runs a non-invasive `LinkServer probe '#0' dapinfo`.
   - *If target is unpowered*: Prompts the operator to switch ON board power switch (SW1) or 5V DC barrel jack, and press `[Enter]` to retry.
3. **Flashing**: Directs LinkServer output to `/tmp/rt1170/flash.log`.
   - If LinkServer rejects the `.elf` with exit code -11, it automatically retries with a sibling `.hex` file.
4. **Console Handover**: Detaches all debuggers, starts background `rt1170-console.py` writing to `/tmp/rt1170/serial.log`, records its PID in `/tmp/rt1170/evkb.lock`, and prints instructions.

### Step 3: Monitor & Boot
1. Tell the user they can tail output in another terminal:
   ```bash
   tail -f /tmp/rt1170/serial.log
   ```
2. Prompt the operator to press **SW4 (RESET)** on the EVKB board to release the core and execute the new firmware.
3. Check `/tmp/rt1170/serial.log` to confirm the boot banner appeared and the build timestamp matches.

### Step 4: Clean Release
When testing is complete or before flashing another image, release the hardware lock and stop the background serial reader:
```bash
tools/rt1170-flash.sh --unlock
```

---

## 3. Diagnostics & Quick Reference

### Core State Interpretation (`tools/rt1170-swdprobe.py --health`)

| Register / Field | Value | Meaning | Action |
|---|---|---|---|
| **DHCSR** (0xE000EDF0) | `0x01010001` | Core is healthy and instructions are retiring (`S_RETIRE_ST=1`, `S_HALT=0`) | Target is running; counter reads are trustworthy |
| **DHCSR** (0xE000EDF0) | `0x00030003` | Core is HALTED by debugger (`S_HALT=1`) | Firmware has not crashed; tap SW4 to free-run |
| **DHCSR** (0xE000EDF0) | Bit 19 set (`S_LOCKUP`) | Genuine ARM core lockup | Check fault registers (`CFSR`/`HFSR`) |
| **CFSR** (0xE000ED28) | `0x00000000` | No Configurable Fault taken | No MemManage, BusFault, or UsageFault |
| **HFSR** (0xE000ED2C) | `0x00000000` | No HardFault taken | Core did not escalate a fault |

### Common Bench Traps & Solutions

| Symptom | Root Cause | Solution |
|---|---|---|
| **Host Kernel Panic (`IOSerialFamily`)** | A process held `/dev/cu.usbmodem*` open while the probe re-enumerated during flash. | Never read or open the serial port during a flash. Run `tools/rt1170-flash.sh --unlock` before flashing. |
| **`Wire not connected`** | MCU-Link has USB power, but the EVKB board power switch (SW1) is OFF. | Switch on SW1 (or connect the 5V barrel power supply) and re-check. |
| **`Flash operation exited with code -11`** | LinkServer's ELF loader choked on image program headers. | Flash the `.hex` artifact instead (`tools/rt1170-flash.sh build/app.hex`). |
| **`Ep(03). Invalid ID for processor`** | CM7 core parked in WFI (e.g. during dual-core CM4 tests) and cannot be halted by SWD. | Loop `flash` while tapping SW4 every ~3 seconds, or boot into SDP mode (SW1-3 OFF / SW1-4 ON). |
| **Probe daemons wedged** | Stale `redlinkserv` or `crt_emu_cm_redlink` left behind. | Run `pkill LinkServer; pkill redlinkserv; pkill crt_emu_cm_redlink` (never `kill -9` mid-flash!). |
| **Silent console after flash** | Probe-latched debug halt survived reset. | Tap SW4 on the EVKB board to trigger clean reset and free-run. |
| **LinkServer silent / perceived hang during flash** | Standalone mass erase (`flash erase`) of the 64 MB NOR flash takes minutes of silent execution. | Avoid mass erase. Use `tools/rt1170-flash.sh` which uses sector-based `flash load`. Remember: LinkServer is silent while programming; silent is not hung. |
