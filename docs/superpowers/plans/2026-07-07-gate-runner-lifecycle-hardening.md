# Gate-runner Lifecycle Hardening Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Give the 19 hand-run `evkb/*/run_*.sh` QEMU gate scripts a shared lifecycle safety net so an orphaned/wedged runner self-terminates and never leaks child processes or temp files.

**Architecture:** A small POSIX-`sh` library `evkb/tools/gate-lib.sh` that each runner sources and arms with `gate_init`. It owns *only* teardown (a `trap` on EXIT/INT/TERM/HUP that reaps registered PIDs + rm's registered temp files) and a hang backstop (re-exec under `gtimeout`, the same coreutils tool `qrun` already uses). Each runner's QEMU args, sleeps, and assertions are untouched — the lib is purely additive.

**Tech Stack:** POSIX `sh` (macOS `/bin/sh` = bash-in-posix-mode), GNU coreutils `gtimeout` (already a dependency of `qrun`), the existing `evkb/tools/qrun` QEMU wrapper.

**Spec:** `docs/superpowers/specs/2026-07-07-gate-runner-lifecycle-hardening-design.md`

---

## ⚠️ Repo caveat — read first

This tree (`/Users/nicholasnewdigate/Development/rt1170`) is **not a git repo**, so the
"Checkpoint" steps below **cannot commit**. Each Checkpoint step lists the commit
message you *would* use — run it only if you first `git init`. Otherwise treat the
Checkpoint as a "stop, the working tree is in a known-good state, the tests pass"
marker. Do not skip the verification that precedes each Checkpoint.

All paths are relative to `/Users/nicholasnewdigate/Development/rt1170` unless absolute.

## File Structure

- **Create:** `evkb/tools/gate-lib.sh` — the sourced lifecycle library (≈45 lines). Sole responsibility: teardown + hang backstop. No test/QEMU knowledge.
- **Create:** `evkb/tools/gate-lib.test.sh` — self-contained plain-`sh` unit harness. Uses `sleep` as a process stand-in; needs no QEMU/firmware. Prints per-case `PASS:`/`FAIL:`, exits non-zero on any failure.
- **Modify:** all 19 `evkb/*/run_*.sh` runners — add `source` + `gate_init` + `gate_pid`/`gate_tmp` registrations; fix two latent warts in the fifo gates.

## Reference — final `evkb/tools/gate-lib.sh` (built incrementally in Tasks 1–4)

For your orientation only; do **not** paste this whole file at once — Tasks 1–4 add it slice-by-slice, test-first. The finished file is exactly:

```sh
#!/bin/sh
# gate-lib.sh — shared lifecycle safety net for the evkb QEMU gate runners.
# Source it right after computing DIR, then call gate_init FIRST (before any
# side-effecting line: rm, mkfifo, python input-gen, launching QEMU).
# Spec: docs/superpowers/specs/2026-07-07-gate-runner-lifecycle-hardening-design.md
#
#   gate_init [timeout_secs]  arm hang backstop (gtimeout re-exec) + traps
#                             default 600s; also overridable via env GATE_TIMEOUT
#   gate_pid  PID...          register process(es) to reap on teardown
#   gate_tmp  FILE...         register temp file(s)/fifo(s) to rm on teardown

GATE_PIDS=""
GATE_TMPS=""

gate_pid() {
    for _p in "$@"; do
        [ -n "$_p" ] && GATE_PIDS="$GATE_PIDS $_p"
    done
}

gate_tmp() {
    for _f in "$@"; do
        [ -n "$_f" ] && GATE_TMPS="$GATE_TMPS $_f"
    done
}

gate_cleanup() {
    _rc=$?
    set +e                                          # a dead PID must not abort teardown
    [ -n "$GATE_PIDS" ] && kill $GATE_PIDS 2>/dev/null
    _bg=$(jobs -p 2>/dev/null)                      # backstop: any stray bg job of this shell
    [ -n "$_bg" ] && kill $_bg 2>/dev/null
    [ -n "$GATE_TMPS" ] && rm -f $GATE_TMPS
    GATE_PIDS=""                                     # idempotent: second fire is a no-op
    GATE_TMPS=""
    return $_rc                                      # preserve the script's exit code
}

gate_init() {
    if [ -z "${GATE_GUARDED:-}" ]; then
        export GATE_GUARDED=1
        exec gtimeout --kill-after=10s "${1:-${GATE_TIMEOUT:-600}}" "$0"
    fi
    trap gate_cleanup EXIT
    trap 'gate_cleanup; exit 130' INT
    trap 'gate_cleanup; exit 143' TERM
    trap 'gate_cleanup; exit 129' HUP
}
```

Key correctness notes the tasks rely on:
- **`gate_init` re-execs the whole script once** under `gtimeout`, then the guarded
  second pass installs the traps. Runners take no args, so `"$0"` is re-exec'd with
  none. `GATE_GUARDED` is exported so the child sees it and skips the re-exec.
- **The traps must reap in the main shell**, where `GATE_PIDS`/`GATE_TMPS` are current
  — that's why the backstop is a re-exec (external `gtimeout`), not a forked
  in-process `sleep` watchdog (which would read a stale copy of those vars).
- **`gate_cleanup` never uses `kill 0`** — only explicit PIDs + `jobs -p` — so it can
  never signal the caller's terminal/session.

---

### Task 1: Library scaffold + temp-file teardown

**Files:**
- Create: `evkb/tools/gate-lib.sh`
- Create: `evkb/tools/gate-lib.test.sh`

- [ ] **Step 1: Write the failing test harness with the first case**

Create `evkb/tools/gate-lib.test.sh`:

```sh
#!/bin/sh
# Unit tests for gate-lib.sh. No QEMU needed — uses `sleep` as a process stand-in.
# Usage: sh evkb/tools/gate-lib.test.sh   (prints PASS:/FAIL: per case; exits 1 on any FAIL)
set -u
LIB="$(cd "$(dirname "$0")" && pwd)/gate-lib.sh"
FAILED=0
report() { # <name> <0-pass|1-fail>
    if [ "$2" -eq 0 ]; then echo "PASS: $1"; else echo "FAIL: $1"; FAILED=1; fi
}

# gate_tmp registers files; gate_cleanup removes them.
test_tmp() {
    ( GATE_GUARDED=1; . "$LIB"
      f1=$(mktemp); f2=$(mktemp)
      gate_tmp "$f1" "$f2"
      gate_cleanup
      [ ! -e "$f1" ] && [ ! -e "$f2" ] )
}
test_tmp; report test_tmp $?

exit $FAILED
```

- [ ] **Step 2: Run it and confirm RED**

Run: `sh /Users/nicholasnewdigate/Development/rt1170/evkb/tools/gate-lib.test.sh; echo "exit=$?"`
Expected: `FAIL: test_tmp` and `exit=1` (the lib doesn't exist yet, so `. "$LIB"` fails and cleanup never removes the files).

- [ ] **Step 3: Create the library with just the registries + temp teardown**

Create `evkb/tools/gate-lib.sh`:

```sh
#!/bin/sh
# gate-lib.sh — shared lifecycle safety net for the evkb QEMU gate runners.
# Source it right after computing DIR, then call gate_init FIRST (before any
# side-effecting line: rm, mkfifo, python input-gen, launching QEMU).
# Spec: docs/superpowers/specs/2026-07-07-gate-runner-lifecycle-hardening-design.md

GATE_PIDS=""
GATE_TMPS=""

gate_pid() {
    for _p in "$@"; do
        [ -n "$_p" ] && GATE_PIDS="$GATE_PIDS $_p"
    done
}

gate_tmp() {
    for _f in "$@"; do
        [ -n "$_f" ] && GATE_TMPS="$GATE_TMPS $_f"
    done
}

gate_cleanup() {
    _rc=$?
    set +e
    [ -n "$GATE_TMPS" ] && rm -f $GATE_TMPS
    GATE_PIDS=""
    GATE_TMPS=""
    return $_rc
}
```

- [ ] **Step 4: Run it and confirm GREEN**

Run: `sh /Users/nicholasnewdigate/Development/rt1170/evkb/tools/gate-lib.test.sh; echo "exit=$?"`
Expected: `PASS: test_tmp` and `exit=0`.

- [ ] **Step 5: Checkpoint** (no git repo — see caveat)

Known-good state: lib removes registered temp files. If `git init`'d, commit:
```bash
git add evkb/tools/gate-lib.sh evkb/tools/gate-lib.test.sh
git commit -m "feat(gate-lib): temp-file teardown registry"
```

---

### Task 2: Process reaping in teardown

**Files:**
- Modify: `evkb/tools/gate-lib.sh` (extend `gate_cleanup`)
- Modify: `evkb/tools/gate-lib.test.sh` (add two cases)

- [ ] **Step 1: Add the failing tests**

In `evkb/tools/gate-lib.test.sh`, insert **before** the final `exit $FAILED` line:

```sh
# gate_pid registers a process; gate_cleanup kills it.
test_pid() {
    ( GATE_GUARDED=1; . "$LIB"
      sleep 60 & p=$!
      gate_pid $p
      gate_cleanup
      sleep 1
      ! kill -0 $p 2>/dev/null )
}
test_pid; report test_pid $?

# Untracked background jobs are still reaped via the jobs -p backstop.
test_jobs_backstop() {
    ( GATE_GUARDED=1; . "$LIB"
      sleep 60 & p=$!          # started but NOT registered with gate_pid
      gate_cleanup
      sleep 1
      ! kill -0 $p 2>/dev/null )
}
test_jobs_backstop; report test_jobs_backstop $?
```

- [ ] **Step 2: Run and confirm RED**

Run: `sh /Users/nicholasnewdigate/Development/rt1170/evkb/tools/gate-lib.test.sh; echo "exit=$?"`
Expected: `PASS: test_tmp`, then `FAIL: test_pid`, `FAIL: test_jobs_backstop`, `exit=1` (cleanup doesn't kill anything yet).

- [ ] **Step 3: Extend `gate_cleanup` to reap processes**

In `evkb/tools/gate-lib.sh`, replace the body of `gate_cleanup` so it kills PIDs before removing files:

```sh
gate_cleanup() {
    _rc=$?
    set +e
    [ -n "$GATE_PIDS" ] && kill $GATE_PIDS 2>/dev/null
    _bg=$(jobs -p 2>/dev/null)
    [ -n "$_bg" ] && kill $_bg 2>/dev/null
    [ -n "$GATE_TMPS" ] && rm -f $GATE_TMPS
    GATE_PIDS=""
    GATE_TMPS=""
    return $_rc
}
```

- [ ] **Step 4: Run and confirm GREEN**

Run: `sh /Users/nicholasnewdigate/Development/rt1170/evkb/tools/gate-lib.test.sh; echo "exit=$?"`
Expected: `PASS: test_tmp`, `PASS: test_pid`, `PASS: test_jobs_backstop`, `exit=0`.

- [ ] **Step 5: Checkpoint** (no git repo — see caveat)

If `git init`'d:
```bash
git add evkb/tools/gate-lib.sh evkb/tools/gate-lib.test.sh
git commit -m "feat(gate-lib): reap tracked PIDs + jobs -p backstop"
```

---

### Task 3: Trap wiring, idempotency, exit-code preservation

**Files:**
- Modify: `evkb/tools/gate-lib.sh` (add `gate_init` traps — guarded path only)
- Modify: `evkb/tools/gate-lib.test.sh` (add three cases)

- [ ] **Step 1: Add the failing tests**

In `evkb/tools/gate-lib.test.sh`, insert before the final `exit $FAILED`:

```sh
# A SIGTERM to a running gate fires the trap: registered child reaped AND temp
# removed. GATE_GUARDED=1 bypasses the gtimeout re-exec so we test the trap alone.
# The fixture writes its registered child's exact PID to $pf and foreground-`wait`s
# on it, so the assertion targets a deterministic PID (no pgrep guessing).
test_trap_term() {
    tf=$(mktemp)
    pf=$(mktemp)
    fx=$(mktemp)
    cat > "$fx" <<EOF
#!/bin/sh
GATE_GUARDED=1; . "$LIB"
gate_init
sleep 60 & cp=\$!
echo \$cp > "$pf"
gate_pid \$cp
gate_tmp "$tf"
: > "$fx.ready"
wait \$cp
EOF
    sh "$fx" >/dev/null 2>&1 &
    gpid=$!
    i=0; while [ $i -lt 50 ] && [ ! -f "$fx.ready" ]; do sleep 0.1; i=$((i+1)); done
    child=$(cat "$pf" 2>/dev/null)
    kill -TERM "$gpid" 2>/dev/null
    wait "$gpid" 2>/dev/null
    sleep 1
    result=0
    [ -e "$tf" ] && result=1                                       # temp should be gone
    { [ -n "$child" ] && kill -0 "$child" 2>/dev/null; } && result=1  # child reaped
    rm -f "$fx" "$fx.ready" "$pf" "$tf"
    return $result
}
test_trap_term; report test_trap_term $?

# gate_cleanup is safe to run twice (EXIT-after-INT double fire).
test_idempotent() {
    ( GATE_GUARDED=1; . "$LIB"
      f=$(mktemp); gate_tmp "$f"
      gate_cleanup
      gate_cleanup )   # second call must not error
}
test_idempotent; report test_idempotent $?

# The EXIT trap preserves the script's own exit code.
test_exit_code() {
    fx=$(mktemp)
    cat > "$fx" <<EOF
#!/bin/sh
GATE_GUARDED=1; . "$LIB"
gate_init
exit 7
EOF
    sh "$fx"; rc=$?
    rm -f "$fx"
    [ "$rc" -eq 7 ]
}
test_exit_code; report test_exit_code $?
```

- [ ] **Step 2: Run and confirm RED**

Run: `sh /Users/nicholasnewdigate/Development/rt1170/evkb/tools/gate-lib.test.sh; echo "exit=$?"`
Expected: earlier cases `PASS`, then `FAIL: test_trap_term` and `FAIL: test_exit_code` (no `gate_init`/traps yet, so `gate_init` is an unknown command → fixture errors). `exit=1`.

- [ ] **Step 3: Add `gate_init` with the guarded trap path**

In `evkb/tools/gate-lib.sh`, append at the end of the file:

```sh
gate_init() {
    if [ -z "${GATE_GUARDED:-}" ]; then
        export GATE_GUARDED=1
        exec gtimeout --kill-after=10s "${1:-${GATE_TIMEOUT:-600}}" "$0"
    fi
    trap gate_cleanup EXIT
    trap 'gate_cleanup; exit 130' INT
    trap 'gate_cleanup; exit 143' TERM
    trap 'gate_cleanup; exit 129' HUP
}
```

(The tests in this task all set `GATE_GUARDED=1`, so they exercise the trap path
without triggering the re-exec — the re-exec itself is covered in Task 4.)

- [ ] **Step 4: Run and confirm GREEN**

Run: `sh /Users/nicholasnewdigate/Development/rt1170/evkb/tools/gate-lib.test.sh; echo "exit=$?"`
Expected: all six cases `PASS` (`test_tmp`, `test_pid`, `test_jobs_backstop`, `test_trap_term`, `test_idempotent`, `test_exit_code`), `exit=0`.

- [ ] **Step 5: Checkpoint** (no git repo — see caveat)

If `git init`'d:
```bash
git add evkb/tools/gate-lib.sh evkb/tools/gate-lib.test.sh
git commit -m "feat(gate-lib): trap-based teardown on EXIT/INT/TERM/HUP"
```

---

### Task 4: Hang backstop (gtimeout re-exec)

**Files:**
- Modify: `evkb/tools/gate-lib.test.sh` (add one case; `gate_init` already implements the re-exec from Task 3)

- [ ] **Step 1: Add the failing test**

In `evkb/tools/gate-lib.test.sh`, insert before the final `exit $FAILED`:

```sh
# gate_init WITHOUT GATE_GUARDED re-execs under gtimeout: a wedged runner is
# reaped at GATE_TIMEOUT, its temp removed, and it exits non-zero.
test_hang_backstop() {
    tf=$(mktemp)
    fx=$(mktemp)
    cat > "$fx" <<EOF
#!/bin/sh
. "$LIB"
GATE_TIMEOUT=2 gate_init
gate_tmp "$tf"
sleep 60 & wait
EOF
    chmod +x "$fx"          # gate_init re-execs "$0"; real gates are +x, so must this be
    start=$(date +%s)
    "$fx" > "$fx.out" 2>&1; rc=$?
    end=$(date +%s)
    elapsed=$((end - start))
    result=0
    [ "$rc" -eq 0 ] && result=1            # a reaped hang must be non-zero
    [ "$elapsed" -gt 12 ] && result=1      # should die ~2s (TERM), well under kill-after
    [ -e "$tf" ] && result=1               # temp must be cleaned by the TERM trap
    rm -f "$fx" "$fx.out" "$tf"
    return $result
}
test_hang_backstop; report test_hang_backstop $?
```

- [ ] **Step 2: Run and confirm the case behaves**

Run: `sh /Users/nicholasnewdigate/Development/rt1170/evkb/tools/gate-lib.test.sh; echo "exit=$?"`
Expected: `PASS: test_hang_backstop` along with all prior cases, `exit=0`. The fixture sleeps 60s but is SIGTERM'd by gtimeout at 2s; its TERM trap removes `$tf` and exits 143.

If it FAILS, first confirm `gtimeout` exists: `command -v gtimeout` (coreutils; `qrun` depends on it too). If missing: `brew install coreutils`.

- [ ] **Step 3: Checkpoint** (no git repo — see caveat)

If `git init`'d:
```bash
git add evkb/tools/gate-lib.test.sh
git commit -m "test(gate-lib): hang backstop reaps wedged runner via gtimeout"
```

---

### Task 5: Refactor wave 1 — the 12 plain gates

These gates create only kept output files (`.uart`, `.dbg`) and have no fifo/scratch,
so the only lifecycle need is: arm the safety net and register the QEMU PID. **Net
+3 lines each, teardown/asserts unchanged.**

**The universal edit for every file in this task:**

1. Immediately after the `DIR=$(cd "$(dirname "$0")" && pwd)` line, insert:
   ```sh
   . ~/Development/rt1170/evkb/tools/gate-lib.sh
   gate_init
   ```
2. Find the QEMU launch's `P=$!` and insert `gate_pid $P;` right after it. e.g.
   `P=$!; sleep 3; ...` → `P=$!; gate_pid $P; sleep 3; ...`

**Files (all under `evkb/`), with each one's `sleep N` for orientation:**
- Modify: `analog_test/run_qemu_adc.sh` (sleep 3)
- Modify: `audiostream_test/run_qemu_audiostream.sh` (sleep 5)
- Modify: `eventresponder_test/run_qemu_er.sh` (sleep 5)
- Modify: `irq_attach_test/run_qemu_irq.sh` (sleep 3)
- Modify: `serial_test/run_qemu.sh` (sleep 3)
- Modify: `spi_dma_test/run_qemu_spidma.sh` (sleep 5)
- Modify: `spi_loopback_test/run_qemu_spi.sh` (sleep 3)
- Modify: `wire_master_test/run_qemu_wire.sh` (sleep 3)
- Modify: `wire_slave_test/run_qemu_wire_slave.sh` (sleep 3)
- Modify: `interval_timer_test/run_qemu_intervaltimer.sh` (sleep 20, has `-icount`)
- Modify: `tone_test/run_qemu_tone.sh` (sleep 20, has `-icount`)
- Modify: `usb_enum_test/run_qemu_usb.sh` (sleep 6)

- [ ] **Step 1: Apply the two edits to all 13 files**

Worked example — `evkb/serial_test/run_qemu.sh` becomes:

```sh
#!/bin/sh
set -e
QEMU=~/Development/rt1170/evkb/tools/qrun
DIR=$(cd "$(dirname "$0")" && pwd)
. ~/Development/rt1170/evkb/tools/gate-lib.sh
gate_init
ELF="$DIR/build/serial_test.elf"
OUT="$DIR/serial.uart"
rm -f "$OUT"
"$QEMU" -M mimxrt1170-evk -global fsl-imxrt1170.boot-xip=on -kernel "$ELF" \
    -display none -serial file:"$OUT" -d guest_errors -D "$DIR/serial.dbg" &
P=$!; gate_pid $P; sleep 3; kill $P 2>/dev/null; wait $P 2>/dev/null || true
echo "==== captured UART ===="; cat "$OUT"
grep -q "RT1176 Serial1 up" "$OUT" || { echo "FAIL: banner missing"; exit 1; }
grep -q "count=3" "$OUT" || { echo "FAIL: counter missing"; exit 1; }
echo "PASS: QEMU serial output verified"
```

Apply the identical two-part edit (source+`gate_init` after `DIR=`; `gate_pid $P;`
after `P=$!`) to the other 11 files. Their `sleep N` and asserts stay exactly as-is.

- [ ] **Step 2: Syntax-check every edited script (no QEMU needed)**

Run:
```bash
for f in analog_test/run_qemu_adc.sh audiostream_test/run_qemu_audiostream.sh \
  eventresponder_test/run_qemu_er.sh irq_attach_test/run_qemu_irq.sh \
  serial_test/run_qemu.sh spi_dma_test/run_qemu_spidma.sh \
  spi_loopback_test/run_qemu_spi.sh wire_master_test/run_qemu_wire.sh \
  wire_slave_test/run_qemu_wire_slave.sh interval_timer_test/run_qemu_intervaltimer.sh \
  tone_test/run_qemu_tone.sh usb_enum_test/run_qemu_usb.sh; do
    sh -n "/Users/nicholasnewdigate/Development/rt1170/evkb/$f" \
      && echo "ok: $f" || echo "SYNTAX FAIL: $f"
done
```
Expected: `ok:` for all 12. No `SYNTAX FAIL`.

- [ ] **Step 3: Run one built gate end-to-end to confirm no behavior change**

Pick a gate whose `build/<t>.elf` exists (check with `ls evkb/serial_test/build/*.elf`).
Run e.g.: `cd /Users/nicholasnewdigate/Development/rt1170/evkb/serial_test && ./run_qemu.sh; echo "exit=$?"`
Expected: same `PASS:` output as before the change and `exit=0`. If the ELF is
missing, note "deferred — not built" and rely on the Step 2 syntax check + the
Task 1–4 unit tests (the edit is mechanical and identical across the 13).

- [ ] **Step 4: Confirm no leftover guard state**

Run: `pgrep -fl gtimeout; pgrep -fl 'run_qemu'` → expected: nothing lingering after the gate exits.

- [ ] **Step 5: Checkpoint** (no git repo — see caveat)

If `git init`'d:
```bash
git add evkb/*/run_qemu*.sh
git commit -m "refactor(gates): arm gate-lib safety net in the 12 plain gates"
```

---

### Task 6: Refactor wave 2 — the 3 tap gates

These add a `-chardev file,...,path="$TAP"` SAI-tap output. `tap.raw` is a kept
result (pre-cleaned at start), so it is NOT registered for teardown — only the
QEMU PID needs arming, same as wave 1. (`audiooutput`'s SYNTH_OK/TONE_OK assertion
block is deliberately left untouched — it is assert logic, not lifecycle.)

**Files (under `evkb/`):**
- Modify: `edma_test/run_qemu_edma.sh` (sleep 4)
- Modify: `i2s_audio_test/run_qemu_i2s.sh` (sleep 4)
- Modify: `audiooutput_i2s_test/run_qemu_audiooutput.sh` (sleep 5)

- [ ] **Step 1: Apply the wave-1 edit to all three**

For each file: insert after the `DIR=` line —
```sh
. ~/Development/rt1170/evkb/tools/gate-lib.sh
gate_init
```
and change its `P=$!` to `P=$!; gate_pid $P` (these three have `P=$!` on its own
line, followed by `sleep N; kill $P ...` on the next line; just append `; gate_pid $P`
to the `P=$!` line, giving `P=$!; gate_pid $P`).

- [ ] **Step 2: Syntax-check**

Run:
```bash
for f in edma_test/run_qemu_edma.sh i2s_audio_test/run_qemu_i2s.sh \
  audiooutput_i2s_test/run_qemu_audiooutput.sh; do
    sh -n "/Users/nicholasnewdigate/Development/rt1170/evkb/$f" \
      && echo "ok: $f" || echo "SYNTAX FAIL: $f"
done
```
Expected: `ok:` for all three.

- [ ] **Step 3: Run one built tap gate**

If `evkb/i2s_audio_test/build/*.elf` exists:
`cd /Users/nicholasnewdigate/Development/rt1170/evkb/i2s_audio_test && ./run_qemu_i2s.sh; echo "exit=$?"`
Expected: same `PASS:` markers and `exit=0`. If unbuilt, note "deferred" as in Task 5.

- [ ] **Step 4: Checkpoint** (no git repo — see caveat)

If `git init`'d:
```bash
git add evkb/edma_test/run_qemu_edma.sh evkb/i2s_audio_test/run_qemu_i2s.sh evkb/audiooutput_i2s_test/run_qemu_audiooutput.sh
git commit -m "refactor(gates): arm gate-lib in the 3 SAI-tap gates"
```

---

### Task 7: Refactor wave 3 — the 2 fifo gates (+ wart-fixes)

Both use a `mkfifo` injector. This is the highest-value wave: it arms the safety net
AND registers the fifo/sidecars/`inject.raw` for teardown (they leak today) AND
fixes `audioinput`'s unreliable pump reaping.

**Files (under `evkb/`):**
- Modify: `sai_rx_test/run_qemu_sai_rx.sh`
- Modify: `audioinput_i2s_test/run_qemu_audioinput.sh`

- [ ] **Step 1: Refactor `sai_rx_test/run_qemu_sai_rx.sh`**

Make it read exactly:

```sh
#!/bin/sh
set -e
QEMU=~/Development/rt1170/evkb/tools/qrun
DIR=$(cd "$(dirname "$0")" && pwd)
. ~/Development/rt1170/evkb/tools/gate-lib.sh
gate_init
ELF="$DIR/build/sai_rx_test.elf"
VCOM="$DIR/vcom.uart"; DBG="$DIR/sai_rx.dbg"; INJ="$DIR/inject.raw"; TAP="$DIR/tap.raw"
python3 "$DIR/gen_inject.py" "$INJ"
rm -f "$VCOM" "$DBG" "$TAP"
gate_tmp "$INJ" "$INJ.fifo" "$INJ.fifo.in" "$INJ.fifo.out"
# NOTE: this QEMU build's "file" chardev backend requires path= (the write/out
# side) even when only input-path= is given ("chardev: file: no filename
# given"), so plain input-path=$INJ does not work here. Fall back to a fifo:
# pump the injector file into a named pipe and point the chardev at that.
rm -f "$INJ.fifo"; mkfifo "$INJ.fifo"
( cat "$INJ" > "$INJ.fifo" 2>/dev/null ) & gate_pid $!
"$QEMU" -M mimxrt1170-evk -global fsl-imxrt1170.boot-xip=on -kernel "$ELF" \
    -display none -serial file:"$VCOM" \
    -chardev pipe,id=sai1-rxinject,path="$INJ.fifo" \
    -chardev file,id=sai1-tap,path="$TAP" \
    -d guest_errors -D "$DBG" &
P=$!; gate_pid $P
sleep 6; kill $P 2>/dev/null; wait $P 2>/dev/null || true
rm -f "$INJ.fifo.in" "$INJ.fifo.out" "$INJ.fifo"
echo "==== VCOM ===="; cat "$VCOM" 2>/dev/null || true
grep -q "STAGE_A_PASS" "$VCOM" || { echo "FAIL: stage A polled read"; exit 1; }
echo "PASS: stage A"
grep -q "STAGE_B_DONE" "$VCOM" || { echo "FAIL: stage B not reached"; exit 1; }
grep -q "STAGE_B_PASS" "$VCOM" || { echo "FAIL: stage B DMA capture"; exit 1; }
echo "PASS: stage B"
grep -q "STAGE_C_DONE" "$VCOM" || { echo "FAIL: stage C not reached"; exit 1; }
python3 "$DIR/check_tap.py" "$TAP" || { echo "FAIL: stage C TX tap mismatch"; exit 1; }
grep -q "STAGE_FD_PASS" "$VCOM" || { echo "FAIL: full-duplex block counts"; exit 1; }
echo "PASS: SAI_RX_ALL (A+B+C)"
```

Changes vs. original: added source+`gate_init` (after `DIR=`); `gate_tmp` for the
injector + fifo + sidecars; captured the one-shot `cat`'s PID via `( … ) & gate_pid $!`;
added `gate_pid $P` after the QEMU `P=$!`. The explicit end-of-run `rm -f "$INJ.fifo"*`
stays (immediate cleanup on the happy path); `gate_tmp` guarantees it also happens on
any abnormal exit.

- [ ] **Step 2: Refactor `audioinput_i2s_test/run_qemu_audioinput.sh` (fix the pump wart)**

Make it read exactly:

```sh
#!/bin/sh
set -e
QEMU=~/Development/rt1170/evkb/tools/qrun
DIR=$(cd "$(dirname "$0")" && pwd)
. ~/Development/rt1170/evkb/tools/gate-lib.sh
gate_init
ELF="$DIR/build/audioinput_i2s_test.elf"
VCOM="$DIR/vcom.uart"; DBG="$DIR/audioinput.dbg"; INJ="$DIR/inject.raw"
python3 "$DIR/gen_inject.py" "$INJ"
rm -f "$VCOM" "$DBG"
gate_tmp "$INJ" "$INJ.fifo" "$INJ.fifo.in" "$INJ.fifo.out"
# NOTE: this QEMU build's "file" chardev backend requires path= (the write/out
# side) even when only input-path= is given ("chardev: file: no filename
# given"), so plain input-path=$INJ does not work here. Fall back to a fifo:
# pump the injector file into a named pipe and point the chardev at that.
#
# Unlike sai_rx_test's single cat (that gate only needed a handful of blocks
# total across its whole run), this gate polls for 500ms of *guest* time and
# the SAI model's rx_timer drains the ring continuously the whole time QEMU is
# up -- so loop the pump so the fifo never runs dry mid-poll (the reader side
# blocks between loop iterations, which is fine: a fifo write() only unblocks
# once the guest-side drain has room again).
rm -f "$INJ.fifo"; mkfifo "$INJ.fifo"
( while true; do cat "$INJ" > "$INJ.fifo" 2>/dev/null; done ) & PUMP_PID=$!
gate_pid $PUMP_PID
"$QEMU" -M mimxrt1170-evk -global fsl-imxrt1170.boot-xip=on -kernel "$ELF" \
    -display none -serial file:"$VCOM" \
    -chardev pipe,id=sai1-rxinject,path="$INJ.fifo" \
    -d guest_errors -D "$DBG" &
P=$!; gate_pid $P
sleep 5; kill $P 2>/dev/null; wait $P 2>/dev/null || true
kill $PUMP_PID 2>/dev/null || true
rm -f "$INJ.fifo.in" "$INJ.fifo.out" "$INJ.fifo"
echo "==== VCOM ===="; cat "$VCOM" 2>/dev/null || true
grep -q "^info peak=" "$VCOM" || { echo "FAIL: no info peak= line"; exit 1; }
grep "^info peak=" "$VCOM"
grep -q "STAGE_PEAK=PASS" "$VCOM" || { echo "FAIL: STAGE_PEAK"; exit 1; }
grep -q "AUDIOINPUT_ALL=PASS" "$VCOM" || { echo "FAIL: AUDIOINPUT_ALL"; exit 1; }
echo "PASS: AUDIOINPUT_ALL"
```

Wart-fix: the pump loop is now backgrounded **outside** a `( … &)` subshell, so
`PUMP_PID=$!` captures its **real** PID; that PID is registered with `gate_pid` and
killed directly. The old fragile `pkill -f "cat $INJ"` line is **removed** (the
tracked PID + the trap's `jobs -p` backstop reap it reliably). `inject.raw` + fifo
sidecars are now cleaned on every exit path via `gate_tmp`.

- [ ] **Step 3: Syntax-check both**

Run:
```bash
for f in sai_rx_test/run_qemu_sai_rx.sh audioinput_i2s_test/run_qemu_audioinput.sh; do
    sh -n "/Users/nicholasnewdigate/Development/rt1170/evkb/$f" \
      && echo "ok: $f" || echo "SYNTAX FAIL: $f"
done
```
Expected: `ok:` for both.

- [ ] **Step 4: Run `sai_rx` end-to-end (it is known-built) and check for leaks**

Run:
```bash
cd /Users/nicholasnewdigate/Development/rt1170/evkb/sai_rx_test && ./run_qemu_sai_rx.sh; echo "exit=$?"
ls inject.raw.fifo inject.raw.fifo.in inject.raw.fifo.out 2>/dev/null && echo "LEAK!" || echo "no fifo leaks"
```
Expected: `PASS: SAI_RX_ALL (A+B+C)`, `exit=0`, and `no fifo leaks`. (`inject.raw` is
now removed on exit too — regenerated next run by `gen_inject.py`.)

- [ ] **Step 5: Checkpoint** (no git repo — see caveat)

If `git init`'d:
```bash
git add evkb/sai_rx_test/run_qemu_sai_rx.sh evkb/audioinput_i2s_test/run_qemu_audioinput.sh
git commit -m "refactor(gates): arm gate-lib in fifo gates; fix pump reap + inject.raw leak"
```

---

### Task 8: Refactor wave 4 — the 2 driver-gated gates

These run a foreground python driver that defines the run, then kill QEMU. The lib
is additive: register the QEMU PID and (for `usb_data`) the `echo.result` scratch file.
Ordering is unchanged.

**Files (under `evkb/`):**
- Modify: `serial_test_rx/run_qemu_rx.sh`
- Modify: `usb_data_test/run_qemu_usb_data.sh`

- [ ] **Step 1: Refactor `serial_test_rx/run_qemu_rx.sh`**

Insert after the `DIR=` line:
```sh
. ~/Development/rt1170/evkb/tools/gate-lib.sh
gate_init
```
and change the QEMU launch's `P=$!` to `P=$!; gate_pid $P`. This gate has no capture
file and no scratch to register — the safety net just guarantees the QEMU/gtimeout
child and the (foreground) driver can't outlive an interrupted or wedged run. Leave
the `sleep 1`, the `python3 "$DIR/qemu_rx_driver.py" $PORT || RC=1`, the trailing
`kill $P; wait $P … || true`, and `exit $RC` exactly as they are.

- [ ] **Step 2: Refactor `usb_data_test/run_qemu_usb_data.sh`**

Insert after the `DIR=` line:
```sh
. ~/Development/rt1170/evkb/tools/gate-lib.sh
gate_init
```
Right after the line that defines `RES` (`RES="$DIR/echo.result"`), add:
```sh
gate_tmp "$RES"
```
and change the QEMU launch's `P=$!` to `P=$!; gate_pid $P`. Leave the driver
invocation, the post-driver `sleep 1` grace, the `kill $P; wait $P`, and the
`[ $RC -eq 0 ]` gate exactly as they are. (`RES` is scratch, safe to clean on exit;
it is also already pre-`rm`'d at start.)

- [ ] **Step 3: Syntax-check both**

Run:
```bash
for f in serial_test_rx/run_qemu_rx.sh usb_data_test/run_qemu_usb_data.sh; do
    sh -n "/Users/nicholasnewdigate/Development/rt1170/evkb/$f" \
      && echo "ok: $f" || echo "SYNTAX FAIL: $f"
done
```
Expected: `ok:` for both.

- [ ] **Step 4: Run one if built**

If `evkb/usb_data_test/build/*.elf` exists:
`cd /Users/nicholasnewdigate/Development/rt1170/evkb/usb_data_test && ./run_qemu_usb_data.sh; echo "exit=$?"`
Expected: unchanged pass behavior and `exit=0`. Else note "deferred".

- [ ] **Step 5: Checkpoint** (no git repo — see caveat)

If `git init`'d:
```bash
git add evkb/serial_test_rx/run_qemu_rx.sh evkb/usb_data_test/run_qemu_usb_data.sh
git commit -m "refactor(gates): arm gate-lib in the 2 driver-gated gates"
```

---

### Task 9: Full verification sweep + memory note

**Files:**
- No source changes. Verification + a memory note.

- [ ] **Step 1: Re-run the unit harness (regression on the lib)**

Run: `sh /Users/nicholasnewdigate/Development/rt1170/evkb/tools/gate-lib.test.sh; echo "exit=$?"`
Expected: all seven cases `PASS`, `exit=0`.

- [ ] **Step 2: Syntax-check ALL 19 refactored runners at once**

Run:
```bash
n=0; bad=0
for f in $(find /Users/nicholasnewdigate/Development/rt1170/evkb -name 'run_*.sh' -type f); do
    n=$((n+1)); sh -n "$f" || { echo "SYNTAX FAIL: $f"; bad=1; }
done
echo "checked=$n bad=$bad"
```
Expected: `checked=19 bad=0`.

- [ ] **Step 3: Confirm every runner sources the lib and arms it**

Run:
```bash
for f in $(find /Users/nicholasnewdigate/Development/rt1170/evkb -name 'run_*.sh' -type f); do
    grep -q 'gate-lib.sh' "$f" && grep -q 'gate_init' "$f" \
      && grep -q 'gate_pid' "$f" || echo "MISSING wiring: $f"
done
echo "done"
```
Expected: only `done` — no `MISSING wiring:` lines.

- [ ] **Step 4: Safety-net integration test on a real gate (leak + orphan)**

Uses the known-built `sai_rx` gate. Interrupt it mid-run and assert no leak:
```bash
cd /Users/nicholasnewdigate/Development/rt1170/evkb/sai_rx_test
./run_qemu_sai_rx.sh > /tmp/sairx.out 2>&1 &
g=$!
sleep 2                                   # mid-run (it sleeps 6)
kill -INT $g 2>/dev/null                  # simulate Ctrl-C
wait $g 2>/dev/null
sleep 1
ls inject.raw.fifo inject.raw.fifo.in inject.raw.fifo.out 2>/dev/null && echo "LEAK!" || echo "no fifo leaks"
pgrep -fl 'qemu-system|gtimeout' | grep -v grep && echo "STRAY PROC!" || echo "no stray procs"
```
Expected: `no fifo leaks` and `no stray procs` — the INT trap reaped QEMU/pump and
removed the fifo. (A pre-fix run of this same sequence would have left
`inject.raw.fifo` behind and possibly a stray process — that contrast is the proof.)

- [ ] **Step 5: Run the full gate suite where builds exist (best-effort regression)**

Run each gate that has a `build/*.elf`, collecting pass/fail:
```bash
for d in $(find /Users/nicholasnewdigate/Development/rt1170/evkb -name 'run_*.sh' -type f); do
    dir=$(dirname "$d")
    ls "$dir"/build/*.elf >/dev/null 2>&1 || { echo "SKIP (unbuilt): ${dir##*/}"; continue; }
    ( cd "$dir" && ./"$(basename "$d")" >/tmp/gate.$$ 2>&1 && echo "PASS: ${dir##*/}" \
        || echo "FAIL: ${dir##*/} (see /tmp/gate.$$)" )
done
```
Expected: `PASS:` for every built gate; `SKIP (unbuilt):` for the rest. Any `FAIL:`
is a regression — inspect the referenced log before proceeding.

- [ ] **Step 6: Record the outcome in memory**

Write a `project`-type memory file at
`/Users/nicholasnewdigate/.claude/projects/-Users-nicholasnewdigate-Development-rt1170/memory/rt1170-gate-lib.md`
summarizing: `evkb/tools/gate-lib.sh` now hardens all 19 gate runners (gate_init
gtimeout re-exec @600s + trap teardown reaping tracked PIDs & temps); qrun still
covers the QEMU/log side; the audioinput pump-reap wart and inject.raw leak were
fixed. Add the one-line pointer to that dir's `MEMORY.md`. Link `[[rt1170-qemu]]`
and `[[rt1170-evkb-flashing]]`.

- [ ] **Step 7: Checkpoint** (no git repo — see caveat)

If `git init`'d:
```bash
git add evkb/tools/gate-lib.sh evkb/tools/gate-lib.test.sh evkb/*/run_*.sh docs/superpowers
git commit -m "feat(gate-lib): harden all 19 QEMU gate runners against orphan/hang/leak"
```

---

## Self-Review (completed by plan author)

**Spec coverage**
- gate-lib.sh API (init/pid/tmp/cleanup) → Tasks 1–4. ✅
- gtimeout re-exec backstop @600s default + `GATE_TIMEOUT` override → Task 4 (+ default in the lib, Task 3). ✅
- trap EXIT/INT/TERM/HUP, tracked PIDs + `jobs -p`, no `kill 0`, idempotent, `set +e`, exit-code preserved → Tasks 2–3. ✅
- All 6 outlier classes refactored → Tasks 5–8 (plain/icount/null → 5; tap → 6; fifo → 7; driver-gated → 8). ✅
- Wart-fixes (audioinput pump PID, inject.raw cleanup) → Task 7. ✅
- Verification (regression + signal/hang/orphan safety-net) → Tasks 4 (hang), 7 (fifo leak), 9 (orphan/INT + suite + syntax). ✅
- Location `evkb/tools/gate-lib.sh`; git caveat → header + every Checkpoint. ✅

**Placeholder scan:** no TBD/TODO; every code + command step shows exact content. ✅

**Type/name consistency:** `gate_init` / `gate_pid` / `gate_tmp` / `gate_cleanup`,
vars `GATE_PIDS` / `GATE_TMPS` / `GATE_GUARDED` / `GATE_TIMEOUT` — used identically
across the lib and all refactor tasks. ✅
