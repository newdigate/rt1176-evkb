# MIMXRT1170-EVKB Flashing Skill & Safety Tooling Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Implement a hardened, panic-free flashing workflow in `tools/rt1170-flash.sh` and author the comprehensive agent skill `.claude/skills/flashing-rt1170-evkb/SKILL.md` enforcing bench safety, multi-agent arbitration, power verification, log tailing, and microcontroller debugging practices.

**Architecture:** Dual-layer architecture:
1. Tool layer (`tools/rt1170-flash.sh`): Handles `.env` persistence, cooperative locking at `/tmp/rt1170/evkb.lock`, non-invasive SWD wire power detection, strict reader abort (Option C), LinkServer programming with `.hex` fallback, and background serial logging to `/tmp/rt1170/serial.log` with a clean `--unlock` command.
2. Skill layer (`.claude/skills/flashing-rt1170-evkb/SKILL.md`): Provides triggering conditions, pre-flight checklists (QEMU-first, boot banners), execution guidelines, SWD probe reference (`tools/rt1170-swdprobe.py`), and failure troubleshooting.

**Tech Stack:** Bash, Python 3 (`pyserial`), LinkServer CLI, CMSIS-DAP / SWD, NXP MIMXRT1170-EVKB hardware.

**Spec:** [docs/superpowers/specs/2026-09-13-rt1170-evkb-flashing-skill-design.md](file:///Users/nicholasnewdigate/Development/rt1170/evkb/docs/superpowers/specs/2026-09-13-rt1170-evkb-flashing-skill-design.md)

## Global Constraints

- Never hold open or re-open `/dev/cu.usbmodem*` during a LinkServer flash or probe operation to prevent macOS `IOSerialFamily` kernel panics.
- Never kill active processes automatically without strict user instruction; abort immediately when any conflicting reader or probe daemon is detected (Option C).
- Do not run full 64MB chip erase; only erase sectors needed by the image (`LinkServer flash load ... --erase-all`).
- Store all transient logs and locks in `/tmp/rt1170/` (`flash.log`, `serial.log`, `evkb.lock`, `serial.pid`).
- Keep `.env` strictly gitignored.

---

### Task 1: Environment Discovery, Persistence, and `.gitignore`

**Files:**
- Modify: `.gitignore:17-21`
- Modify: `tools/rt1170-flash.sh:1-25`
- Test: `tools/rt1170-flash.test.sh` (new test harness)

**Interfaces:**
- Consumes: Environment variables `$LINKSERVER`, `$RT1170_PORT`, `$DEVICE`, `$PY`, and `.env` file in repository root.
- Produces: `tools/rt1170-flash.sh --env-check` prints active resolved configuration in `KEY=VALUE` format and auto-generates `.env` if missing.

- [ ] **Step 1: Write failing test for environment discovery and `.env` loading**

Create `tools/rt1170-flash.test.sh`:
```bash
#!/usr/bin/env bash
set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
FLASH_SH="$HERE/rt1170-flash.sh"
TEST_TMP="$(mktemp -d /tmp/rt1170_test_env.XXXXXX)"
trap 'rm -rf "$TEST_TMP"' EXIT

# Test 1: --env-check loads from .env if present
cat <<'EOF' > "$TEST_TMP/.env"
LINKSERVER="/mock/LinkServer"
RT1170_PORT="/dev/cu.usbmodemMOCK123"
EVKB_DEVICE="MIMXRT1176:MIMXRT1170-EVKB"
PY="/mock/python3"
EOF

OUT="$(RT1170_ENV_FILE="$TEST_TMP/.env" "$FLASH_SH" --env-check)"
echo "$OUT" | grep -q 'LINKSERVER=/mock/LinkServer' || { echo "FAIL: LINKSERVER not loaded from .env"; exit 1; }
echo "$OUT" | grep -q 'RT1170_PORT=/dev/cu.usbmodemMOCK123' || { echo "FAIL: RT1170_PORT not loaded from .env"; exit 1; }
echo "PASS: test_env_discovery"
```

- [ ] **Step 2: Run test to verify it fails**

Run: `bash tools/rt1170-flash.test.sh`  
Expected: FAIL with unrecognized option or exit code 1.

- [ ] **Step 3: Implement `.env` discovery and `--env-check` in `tools/rt1170-flash.sh` and update `.gitignore`**

In `.gitignore`, add `.env` under project ignores:
```gitignore
# Local environment configuration
.env
```

In `tools/rt1170-flash.sh`, add environment loading and discovery:
```bash
ENV_FILE="${RT1170_ENV_FILE:-$HERE/../.env}"
if [ -f "$ENV_FILE" ]; then
  # shellcheck disable=SC1090
  set -a; source "$ENV_FILE"; set +a
fi

# Auto-discovery if variables are not set
if [ -z "${LINKSERVER:-}" ]; then
  for cand in /Applications/LinkServer_*/LinkServer; do
    if [ -x "$cand" ]; then LINKSERVER="$cand"; break; fi
  done
  LINKSERVER="${LINKSERVER:-$(command -v LinkServer 2>/dev/null || true)}"
fi

if [ -z "${RT1170_PORT:-}" ]; then
  for cand in /dev/cu.usbmodem*; do
    if [ -e "$cand" ]; then RT1170_PORT="$cand"; break; fi
  done
fi

DEVICE="${EVKB_DEVICE:-${DEVICE:-MIMXRT1176:MIMXRT1170-EVKB}}"
PORT="${RT1170_PORT:-/dev/cu.usbmodem5DQ2DDHVWO5EI3}"
PY="${PY:-/usr/local/Caskroom/miniconda/base/bin/python3}"
[ -x "$PY" ] || PY="$(command -v python3 || true)"

if [ "${1:-}" = "--env-check" ]; then
  echo "LINKSERVER=$LINKSERVER"
  echo "RT1170_PORT=$PORT"
  echo "EVKB_DEVICE=$DEVICE"
  echo "PY=$PY"
  exit 0
fi
```

- [ ] **Step 4: Run test to verify it passes**

Run: `bash tools/rt1170-flash.test.sh`  
Expected: `PASS: test_env_discovery`

- [ ] **Step 5: Commit**

```bash
git add .gitignore tools/rt1170-flash.sh tools/rt1170-flash.test.sh
git commit -m "feat(flash): add .env discovery, persistence support, and --env-check"
```

---

### Task 2: Cooperative Lock & Strict Abort Conflict Detection (Option C)

**Files:**
- Modify: `tools/rt1170-flash.sh`
- Test: `tools/rt1170-flash.test.sh`

**Interfaces:**
- Consumes: `/tmp/rt1170/evkb.lock`, `/tmp/rt1170/serial.pid`, `lsof -t -- "$PORT"`, `pgrep`.
- Produces:
  - `check_lock_and_conflicts`: Aborts if lock held by active PID or open serial reader or probe daemon.
  - `--unlock`: Terminates background console reader gracefully, verifies port release, and deletes lock files.

- [ ] **Step 1: Write failing tests for lock acquisition, stale lock purging, strict abort, and `--unlock`**

Extend `tools/rt1170-flash.test.sh`:
```bash
# Test 2: Stale lock is purged
mkdir -p /tmp/rt1170
echo "PID=99999999
COMMAND=fake
OWNER=test" > /tmp/rt1170/evkb.lock

OUT="$(RT1170_ENV_FILE="$TEST_TMP/.env" "$FLASH_SH" --check-lock)"
[ ! -f /tmp/rt1170/evkb.lock ] || { echo "FAIL: stale lock was not purged"; exit 1; }
echo "PASS: test_stale_lock_purged"

# Test 3: Active lock triggers strict abort
echo "PID=$$
COMMAND=active_shell
OWNER=test_suite" > /tmp/rt1170/evkb.lock

set +e
ERR="$("$FLASH_SH" --check-lock 2>&1)"
RC=$?
set -e
[ $RC -ne 0 ] || { echo "FAIL: active lock did not fail"; exit 1; }
echo "$ERR" | grep -q "Board or serial port is currently busy" || { echo "FAIL: expected abort message missing"; exit 1; }
echo "PASS: test_active_lock_aborts"

# Test 4: --unlock cleans up lock
"$FLASH_SH" --unlock
[ ! -f /tmp/rt1170/evkb.lock ] || { echo "FAIL: lock file still present after --unlock"; exit 1; }
echo "PASS: test_unlock"
```

- [ ] **Step 2: Run test to verify it fails**

Run: `bash tools/rt1170-flash.test.sh`  
Expected: FAIL on `--check-lock`.

- [ ] **Step 3: Implement lock management, strict abort, and `--unlock` in `tools/rt1170-flash.sh`**

Implement in `tools/rt1170-flash.sh`:
```bash
LOCK_DIR="/tmp/rt1170"
LOCK_FILE="$LOCK_DIR/evkb.lock"
SERIAL_PID_FILE="$LOCK_DIR/serial.pid"

unlock_board() {
  local spid=""
  if [ -f "$SERIAL_PID_FILE" ]; then
    spid="$(cat "$SERIAL_PID_FILE" 2>/dev/null || true)"
  fi
  if [ -n "$spid" ] && kill -0 "$spid" 2>/dev/null; then
    echo "==> Terminating background serial reader (PID: $spid)..."
    kill -TERM "$spid" 2>/dev/null || true
    for _ in {1..10}; do
      if ! kill -0 "$spid" 2>/dev/null; then break; fi
      sleep 0.2
    done
  fi
  rm -f "$LOCK_FILE" "$SERIAL_PID_FILE"
  echo "==> Board lock released and serial console stopped safely."
}

if [ "${1:-}" = "--unlock" ] || [ "${1:-}" = "--stop-console" ]; then
  unlock_board
  exit 0
fi

check_lock_and_conflicts() {
  mkdir -p "$LOCK_DIR"
  if [ -f "$LOCK_FILE" ]; then
    local lpid
    lpid="$(grep '^PID=' "$LOCK_FILE" 2>/dev/null | cut -d= -f2 || true)"
    if [ -n "$lpid" ] && kill -0 "$lpid" 2>/dev/null; then
      echo "ERROR: Board or serial port is currently busy!" >&2
      echo "Active lock held by PID $lpid:" >&2
      cat "$LOCK_FILE" >&2
      echo "To prevent a macOS IOSerialFamily kernel panic, aborting." >&2
      echo "Release the board with: tools/rt1170-flash.sh --unlock" >&2
      exit 1
    else
      rm -f "$LOCK_FILE"
    fi
  fi

  # Check active readers or daemons
  local conflicting_pids=""
  local readers=""
  readers="$(lsof -t -- "$PORT" 2>/dev/null || true)"
  local console_pids=""
  console_pids="$(pgrep -f 'rt1170-console\.py' 2>/dev/null || true)"
  local daemons=""
  daemons="$(pgrep -f 'LinkServer|redlinkserv|crt_emu_cm_redlink' 2>/dev/null || true)"

  conflicting_pids="$(echo "$readers $console_pids $daemons" | tr ' ' '\n' | grep -v "^$$$" | sort -un | tr '\n' ' ')"
  if [ -n "${conflicting_pids// /}" ]; then
    echo "ERROR: Board or serial port is currently busy!" >&2
    echo "Conflicting process(es) detected (PID: $conflicting_pids):" >&2
    ps -p $conflicting_pids -o pid,comm,args >&2 2>/dev/null || true
    echo "To avoid a macOS kernel panic, aborting." >&2
    echo "Please terminate conflicting processes or run: tools/rt1170-flash.sh --unlock" >&2
    exit 1
  fi
}

if [ "${1:-}" = "--check-lock" ]; then
  check_lock_and_conflicts
  echo "LOCK_OK"
  exit 0
fi
```

- [ ] **Step 4: Run test to verify it passes**

Run: `bash tools/rt1170-flash.test.sh`  
Expected: PASS for all tests.

- [ ] **Step 5: Commit**

```bash
git add tools/rt1170-flash.sh tools/rt1170-flash.test.sh
git commit -m "feat(flash): implement cooperative lock, strict reader conflict abort, and --unlock"
```

---

### Task 3: Board Power Pre-flight & Flash with HEX Fallback

**Files:**
- Modify: `tools/rt1170-flash.sh`
- Test: `tools/rt1170-flash.test.sh`

**Interfaces:**
- Consumes: Target image path (`.elf` or `.hex`), `$LINKSERVER`, `$DEVICE`, `$PORT`.
- Produces: Log output to `/tmp/rt1170/flash.log`, non-invasive SWD power ping via `LinkServer probe '#0' dapinfo`.

- [ ] **Step 1: Write test for power-check failure detection and hex fallback**

Extend `tools/rt1170-flash.test.sh`:
```bash
# Test 5: Verify hex fallback logic
cat <<'EOF' > "$TEST_TMP/mock_linkserver.sh"
#!/usr/bin/env bash
if [ "$1" = "probe" ]; then
  if [ -n "${MOCK_POWER_OFF:-}" ]; then
    echo "SWD transfer error: Wire not connected"
    exit 1
  fi
  echo "DP ID: 0x6BA02477"
  exit 0
fi
if [ "$1" = "flash" ]; then
  if [[ "$4" == *.elf ]] && [ -n "${MOCK_ELF_FAIL:-}" ]; then
    echo "Flash operation exited with code -11" >&2
    exit 245 # 256 - 11
  fi
  echo "Flash operation successful"
  exit 0
fi
EOF
chmod +x "$TEST_TMP/mock_linkserver.sh"

# Test ELF failure falling back to HEX
touch "$TEST_TMP/sample.elf" "$TEST_TMP/sample.hex"
OUT="$(MOCK_ELF_FAIL=1 LINKSERVER="$TEST_TMP/mock_linkserver.sh" "$FLASH_SH" --test-flash "$TEST_TMP/sample.elf")"
echo "$OUT" | grep -q "Falling back to .hex image" || { echo "FAIL: hex fallback not triggered"; exit 1; }
echo "PASS: test_hex_fallback"
```

- [ ] **Step 2: Run test to verify it fails**

Run: `bash tools/rt1170-flash.test.sh`  
Expected: FAIL on `--test-flash`.

- [ ] **Step 3: Implement SWD power check and HEX fallback in `tools/rt1170-flash.sh`**

```bash
check_board_power() {
  if [ ! -c "$PORT" ] && [ ! -e "$PORT" ]; then
    echo "ERROR: MCU-Link VCOM port $PORT not found." >&2
    echo "Please connect the EVKB debug USB cable (J17)." >&2
    exit 1
  fi

  local attempts=0
  while [ $attempts -lt 3 ]; do
    attempts=$((attempts + 1))
    local probe_out
    probe_out="$("$LINKSERVER" probe '#0' dapinfo 2>&1 || true)"
    if echo "$probe_out" | grep -q "Wire not connected"; then
      echo "==> WARNING: MCU-Link detected on USB, but RT1170 target is unpowered." >&2
      echo "==> Please switch ON the board power switch (SW1) or check the 5V DC barrel jack." >&2
      if [ -t 0 ]; then
        read -r -p "Press [Enter] to re-check power (attempt $attempts/3) or Ctrl-C to abort..."
      else
        sleep 1
      fi
    else
      return 0
    fi
  done
  echo "ERROR: Target unpowered after 3 checks. Aborting flash." >&2
  exit 1
}

flash_image() {
  local target_img="$1"
  local flash_log="/tmp/rt1170/flash.log"
  echo "==> Flashing $DEVICE with $target_img"
  echo "==> Tail debugger output in another window with:"
  echo "    tail -f $flash_log"
  echo

  set +e
  "$LINKSERVER" flash "$DEVICE" load "$target_img" --erase-all > "$flash_log" 2>&1
  local rc=$?
  set -e

  if [ $rc -ne 0 ]; then
    if [[ "$target_img" == *.elf ]]; then
      local hex_alt="${target_img%.elf}.hex"
      if [ -f "$hex_alt" ] && grep -q -E "code -11|LOAD_EXIT=245" "$flash_log"; then
        echo "==> LinkServer ELF parser failed (exit -11). Falling back to .hex image: $hex_alt"
        "$LINKSERVER" flash "$DEVICE" load "$hex_alt" --erase-all >> "$flash_log" 2>&1
        echo "==> Flash OK (via HEX fallback)."
        return 0
      fi
    fi
    echo "ERROR: LinkServer flash failed (exit code $rc). Check $flash_log" >&2
    tail -n 20 "$flash_log" >&2
    exit $rc
  fi
  echo "==> Flash OK."
}
```

- [ ] **Step 4: Run test to verify it passes**

Run: `bash tools/rt1170-flash.test.sh`  
Expected: PASS for all tests.

- [ ] **Step 5: Commit**

```bash
git add tools/rt1170-flash.sh tools/rt1170-flash.test.sh
git commit -m "feat(flash): add non-invasive SWD power pre-flight check and HEX fallback"
```

---

### Task 4: Post-Flash Background Console & Live Log Tailing

**Files:**
- Modify: `tools/rt1170-flash.sh`
- Test: `tools/rt1170-flash.test.sh`

**Interfaces:**
- Consumes: Flash success status, `$PORT`, `$PY`.
- Produces: Background `rt1170-console.py` process, `/tmp/rt1170/serial.log`, `/tmp/rt1170/serial.pid`, updated `/tmp/rt1170/evkb.lock`.

- [ ] **Step 1: Write test for background serial console spawning and pid tracking**

Extend `tools/rt1170-flash.test.sh`:
```bash
# Test 6: Verify background serial launch and lock tracking
"$FLASH_SH" --unlock
"$FLASH_SH" --start-console-bg "$TEST_TMP/fake_port"
[ -f /tmp/rt1170/serial.pid ] || { echo "FAIL: serial.pid not created"; exit 1; }
[ -f /tmp/rt1170/evkb.lock ] || { echo "FAIL: evkb.lock not updated with console pid"; exit 1; }
"$FLASH_SH" --unlock
[ ! -f /tmp/rt1170/serial.pid ] || { echo "FAIL: serial.pid not cleaned up"; exit 1; }
echo "PASS: test_background_console"
```

- [ ] **Step 2: Run test to verify it fails**

Run: `bash tools/rt1170-flash.test.sh`  
Expected: FAIL on `--start-console-bg`.

- [ ] **Step 3: Implement background console launcher in `tools/rt1170-flash.sh`**

```bash
start_console_bg() {
  local port="${1:-$PORT}"
  local serial_log="/tmp/rt1170/serial.log"
  mkdir -p "$LOCK_DIR"
  echo "" > "$serial_log"

  nohup "$PY" "$HERE/rt1170-console.py" "$port" 115200 >> "$serial_log" 2>&1 &
  local cpid=$!
  echo "$cpid" > "$SERIAL_PID_FILE"

  echo "PID=$cpid
COMMAND=rt1170-console.py
OWNER=serial-console
PORT=$port
LOG=$serial_log
START=$(date -u +"%Y-%m-%dT%H:%M:%SZ")" > "$LOCK_FILE"

  echo "==> Background serial console started (PID: $cpid)."
  echo "==> Tail live serial output with:"
  echo "    tail -f $serial_log"
  echo
  echo "==> Press SW4 (RESET) on the EVKB board to boot the new image."
  echo "==> When finished, release the board with:"
  echo "    tools/rt1170-flash.sh --unlock"
}
```

Wire into the main execution path of `tools/rt1170-flash.sh`:
```bash
# Main execution
IMG="${1:-$HOME/Development/zephyr/projects/zepherproject/build-hello/zephyr/zephyr.elf}"
[ -f "$IMG" ] || { echo "Image not found: $IMG" >&2; exit 1; }

check_lock_and_conflicts
check_board_power

# Acquire lock for flash
echo "PID=$$
COMMAND=rt1170-flash.sh
OWNER=flash-script
IMAGE=$IMG
START=$(date -u +"%Y-%m-%dT%H:%M:%SZ")" > "$LOCK_FILE"

# Clean stale daemons safely (no SIGKILL)
pkill LinkServer 2>/dev/null || true
pkill redlinkserv 2>/dev/null || true
pkill crt_emu_cm_redlink 2>/dev/null || true
sleep 0.5

flash_image "$IMG"
start_console_bg "$PORT"
```

- [ ] **Step 4: Run test to verify it passes**

Run: `bash tools/rt1170-flash.test.sh`  
Expected: PASS for all tests.

- [ ] **Step 5: Commit**

```bash
git add tools/rt1170-flash.sh tools/rt1170-flash.test.sh
git commit -m "feat(flash): launch background console with log tailing instructions and clean lock handover"
```

---

### Task 5: Author the Agent Skill (`.claude/skills/flashing-rt1170-evkb/SKILL.md`)

**Files:**
- Create: `.claude/skills/flashing-rt1170-evkb/SKILL.md`

**Interfaces:**
- Consumes: Design spec [docs/superpowers/specs/2026-09-13-rt1170-evkb-flashing-skill-design.md](file:///Users/nicholasnewdigate/Development/rt1170/evkb/docs/superpowers/specs/2026-09-13-rt1170-evkb-flashing-skill-design.md)
- Produces: Complete agent reference skill adhering to `writing-skills` conventions.

- [ ] **Step 1: Write `.claude/skills/flashing-rt1170-evkb/SKILL.md`**

Write the complete skill document with:
1. YAML frontmatter (`name: flashing-rt1170-evkb`, SDO-compliant description starting with `Use when...`).
2. Why this skill exists (macOS kernel panic history, bus-powered MCU-Link, multi-agent collisions).
3. Microcontroller debugging discipline & avoiding unnecessary flashes (versioned banner, QEMU first, SWD state probing, SWD screenshot, dual-clock corroboration).
4. Step-by-step flashing checklist (pre-flight, executing flash, tailing logs, SW4 reset, banner verification, unlock).
5. Diagnostic quick reference table (`rt1170-swdprobe.py --health`, `DHCSR` bitfields, fault registers).
6. Common mistakes & bench traps.

- [ ] **Step 2: Self-review skill against `writing-skills` checklist**
- Verify name uses only letters, numbers, hyphens.
- Verify description is in third-person, starts with "Use when...", contains no workflow summaries.
- Verify no placeholders ("TBD", "TODO", "implement later").
- Verify all file references and commands match actual repo tools (`tools/rt1170-flash.sh`, `tools/rt1170-swdprobe.py`, `tools/rt1170-screenshot.py`).

- [ ] **Step 3: Commit**

```bash
git add .claude/skills/flashing-rt1170-evkb/SKILL.md
git commit -m "feat(skills): add flashing-rt1170-evkb skill for safe hardware deployment and debugging"
```

---

### Task 6: End-to-End Verification & Walkthrough

**Files:**
- Test: Run `bash tools/rt1170-flash.test.sh`
- Create: `walkthrough.md` (or interactive verification with hardware if board is connected)

- [ ] **Step 1: Run full regression test suite**

Run: `bash tools/rt1170-flash.test.sh`  
Expected: All tests pass.

- [ ] **Step 2: Verify `bash -n tools/rt1170-flash.sh` syntax check**

Run: `bash -n tools/rt1170-flash.sh`  
Expected: Clean exit 0.

- [ ] **Step 3: Test `--env-check` against live system**

Run: `tools/rt1170-flash.sh --env-check`  
Verify: Detects `/Applications/LinkServer_26.6.137/LinkServer` and `/dev/cu.usbmodem*` if connected.

- [ ] **Step 4: Commit and finalize**

```bash
git add tools/rt1170-flash.test.sh
git commit -m "test(flash): finalize regression suite for rt1170-flash.sh"
```
