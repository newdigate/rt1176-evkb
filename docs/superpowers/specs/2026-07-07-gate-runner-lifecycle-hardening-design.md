# Gate-runner lifecycle hardening — design

**Date:** 2026-07-07
**Status:** Approved design, pending spec review → implementation plan
**Author:** Claude (with Nic)

## 1. Context & problem

The `evkb/` tree has **19 QEMU gate-runner scripts** (`evkb/*/run_*.sh`). They are
invoked **by hand** — there is no CI, no top-level orchestrator, no Makefile that
runs them, and the tree is **not a git repo**.

A routine cleanup found one of these runners (`sai_rx_test/run_qemu_sai_rx.sh`,
PID 23226) **orphaned to launchd (PPID 1) and hung for >1 day** at 0% CPU. Its
QEMU child was already gone; the *shell itself* lingered. It was also a **stale,
divergent copy** — it predated the on-disk version of its own script, so its
hung logic wasn't even what was on disk anymore.

### What is already handled (do not rebuild)

`evkb/tools/qrun` — the wrapper all 19 runners launch QEMU through — already fully
protects the **QEMU** side:

- `exec gtimeout --kill-after=5s "$TIMEOUT" qemu-system-arm …` — a hard runtime
  cap (default `QRUN_TIMEOUT=60`s). Even an **orphaned** QEMU self-terminates.
- Caps the `-d guest_errors` log (`-D <file>`) at `QRUN_MAXLOG_MB` (default
  100 MB), keeping the first N MB and draining the rest to `/dev/null`.

So the catastrophic "~100 GB guest-error log fills the disk" failure mode
**cannot recur** through a qrun-wrapped gate. That vector is closed.

### The actual gap

qrun bounds **QEMU**; nothing bounds the **runner shell**. When the launching
terminal/session dies, the runner can orphan and hang forever, and its temp
files (the `mkfifo` pipe, backgrounded `cat` pumps) can leak. This is a
**hygiene** problem, not a catastrophe — but it recurs by design as long as 19
hand-maintained scripts each open-code their own process lifecycle.

## 2. Goals & non-goals

**Goals**
- No runner can hang indefinitely: a self-timeout reaps a wedged/orphaned runner.
- No runner leaks children or temp files on abnormal exit (Ctrl-C, `kill`, HUP,
  timeout).
- Centralize the identical lifecycle logic so the 19 scripts **converge** —
  killing the copy-drift that produced the stale hung script.
- Fix the latent warts the survey surfaced (see §6).

**Non-goals**
- Not touching qrun (QEMU-side protection already works).
- Not unifying the scripts' *bodies* — QEMU args, sleep durations, and
  assertions legitimately vary across 6 outlier shapes; forcing them into a
  shared body is how a passing gate breaks.
- Not building CI/orchestration. Out of scope.
- Not converting the tree to git. (Noted: spec can't be committed here.)

## 3. What the survey found (shared skeleton)

All 19 scripts open identically:

```sh
#!/bin/sh
set -e
QEMU=~/Development/rt1170/evkb/tools/qrun
DIR=$(cd "$(dirname "$0")" && pwd)
```

and share one invariant lifecycle core:

> launch QEMU under `qrun`, backgrounded, capture `P=$!`
> → `sleep N` (or run a foreground driver)
> → `kill $P 2>/dev/null; wait $P 2>/dev/null || true`
> → read the capture file, grep/py-check for PASS markers.

Everything that **varies** — sleep length (1/3/4/5/6/20s), extra chardevs
(file tap / pipe fifo / socket / null), `-icount`, fifo injector pumps,
python input-gen and tap-checkers, assertion style — is script *body*, not
lifecycle. **The lifecycle core is the seam to cut along.**

## 4. Design: `evkb/tools/gate-lib.sh`

A small POSIX-`sh` library sourced by every runner. It owns **only** the two
things that are (a) identical across all 19 and (b) the bug surface:
**teardown on abnormal exit** and a **hang backstop**. It is purely additive to
each script's existing normal-path `sleep N; kill $P; wait $P` and its asserts.

### 4.1 API (4 entry points)

```sh
# Source near the top of a runner, right after computing DIR:
. ~/Development/rt1170/evkb/tools/gate-lib.sh

gate_init [timeout_secs]   # FIRST executable call. Arms the hang backstop +
                           #   installs trap gate_cleanup on EXIT INT TERM HUP.
                           #   Default timeout 600s; also overridable via env
                           #   GATE_TIMEOUT.
gate_pid  PID...           # Register a process to reap on teardown
                           #   (the qrun/gtimeout $P; any fifo pump).
gate_tmp  FILE...          # Register a temp file / fifo to rm on teardown.
# gate_cleanup             # Internal trap handler. Idempotent.
```

### 4.2 Hang backstop — `gtimeout` re-exec (not a hand-rolled watchdog)

`gate_init`, if not already guarded, **re-execs the whole script under
`gtimeout`** — the same coreutils tool + `--kill-after` idiom qrun already uses:

```sh
gate_init() {
  if [ -z "$GATE_GUARDED" ]; then
    exec env GATE_GUARDED=1 \
      gtimeout --kill-after=10s "${1:-${GATE_TIMEOUT:-600}}" "$0" "$@"
  fi
  GATE_MAIN=$$
  GATE_PIDS=
  GATE_TMPS=
  trap gate_cleanup EXIT
  trap 'gate_cleanup; exit 130' INT
  trap 'gate_cleanup; exit 143' TERM
  trap 'gate_cleanup; exit 129' HUP
}
```

Rationale for re-exec over an in-process `sleep &` watchdog:
- The external killer (gtimeout) does not depend on the wedged shell being able
  to fork/schedule anything.
- Avoids the orphaned-`sleep` / PID-reuse subtleties of an in-process watchdog.
- It is the **exact pattern qrun uses** for QEMU — idiomatic in this codebase.
- `gtimeout` is already a hard dependency (qrun uses it), so no new requirement.

At `GATE_TIMEOUT`, gtimeout SIGTERMs the runner → the TERM trap runs
`gate_cleanup` (reaps QEMU + rm temps) → exits non-zero. If the trap itself
hangs, gtimeout SIGKILLs 10s later; even then, qrun's own gtimeout still bounds
QEMU to 60s, so nothing runs away.

**Requirement:** because the re-exec restarts the script from the top,
`gate_init` must run **before any line with a side effect that must not repeat**
— `rm -f`, `mkfifo`, python input-gen, launching QEMU. Pure assignments before
it (`QEMU=`, `DIR=`, sourcing the lib) are fine: they just re-run harmlessly, and
on the second pass `GATE_GUARDED=1` makes `gate_init` skip the re-exec branch.

### 4.3 Teardown — `gate_cleanup`

Runs in the main shell on EXIT / INT / TERM / HUP:

```sh
gate_cleanup() {
  [ -n "$GATE_PIDS" ] && kill $GATE_PIDS 2>/dev/null
  _bg=$(jobs -p 2>/dev/null); [ -n "$_bg" ] && kill $_bg 2>/dev/null  # backstop
  [ -n "$GATE_TMPS" ] && rm -f $GATE_TMPS
  GATE_PIDS=; GATE_TMPS=                                              # idempotent
}
```

- Reaps **explicitly tracked** PIDs (primary), plus `jobs -p` as an automatic
  backstop for any stray background job of the shell.
- Never uses `kill 0` / process-group kill → **zero risk** of nuking the
  caller's terminal (important on macOS `/bin/sh`, which is bash-as-sh).
- Idempotent: clearing the vars makes the EXIT-after-INT double-fire a no-op.
- Runs under `set +e` (saved/restored) so reaping an already-dead PID returning
  non-zero cannot abort teardown under the runners' `set -e`. (The `kill …` are
  also `2>/dev/null`-guarded.)

## 5. Per-script refactor shape (net +2–3 lines)

`serial_test/run_qemu.sh`, before → after:

```sh
  #!/bin/sh
  set -e
  QEMU=~/Development/rt1170/evkb/tools/qrun
  DIR=$(cd "$(dirname "$0")" && pwd)
+ . ~/Development/rt1170/evkb/tools/gate-lib.sh
+ gate_init
  ELF="$DIR/build/serial_test.elf"
  OUT="$DIR/serial.uart"
  rm -f "$OUT"
  "$QEMU" … -serial file:"$OUT" -d guest_errors -D "$DIR/serial.dbg" &
- P=$!; sleep 3; kill $P 2>/dev/null; wait $P 2>/dev/null || true
+ P=$!; gate_pid $P; sleep 3; kill $P 2>/dev/null; wait $P 2>/dev/null || true
  … asserts unchanged …
```

The start-of-run `rm -f "$OUT"` stays (it clears *prior* artifacts — a different
purpose from teardown). The normal-path `sleep N; kill $P; wait $P` stays. Temp
files created by the script get a `gate_tmp` registration so the trap removes
them on any exit path.

### Outlier mapping (all 6 shapes fit — the lib is additive)

| Script(s) | How it maps |
|---|---|
| Group A/B/F (13 plain grep gates) | `gate_pid $P` after `P=$!`; `gate_tmp` the `.uart`/`.dbg` if the script owns them. |
| `interval_timer`, `tone` (20s + `-icount`) | Covered by the 600s default; no special handling. |
| Group C tap gates (`edma`, `i2s`, `audiooutput`) | `gate_pid $P`; `gate_tmp "$TAP"`. `audiooutput`'s SYNTH_OK/TONE_OK aggregation is assert logic — **untouched**. |
| `sai_rx` (one-shot fifo) | `gate_pid` the injector `cat`; `gate_tmp "$INJ.fifo" "$INJ.fifo.in" "$INJ.fifo.out"`. |
| `audioinput` (looping pump) | See §6 wart-fix. |
| `serial_test_rx`, `usb_data` (driver-gated, inverted order) | Ordering unchanged (launch → foreground python driver → kill). Just `gate_pid $P` + `gate_tmp echo.result`. |

## 6. Latent wart-fixes (approved: "fix the warts too")

1. **`audioinput` pump reaping is unreliable.** Today:
   ```sh
   (while true; do cat "$INJ" > "$INJ.fifo" 2>/dev/null; done &)
   PUMP_PID=$!            # <-- $! is NOT the pump (it's inside a ( … &) subshell)
   … kill $PUMP_PID …    # unreliable
   pkill -f "cat $INJ"   # the actual reaper, a fragile hack
   ```
   Fix: background the loop so its **real** PID is captured, register it, and
   drop the `pkill` hack:
   ```sh
   ( while true; do cat "$INJ" > "$INJ.fifo" 2>/dev/null; done ) & PUMP=$!
   gate_pid $PUMP
   ```
2. **`inject.raw` is never cleaned.** Neither fifo gate (`sai_rx`, `audioinput`)
   removes its python-generated `inject.raw`. Register it: `gate_tmp "$INJ"`.

## 7. Cleanup obligations (from survey — the trap must cover all)

Per test dir, as applicable to each script:
- `<name>.uart` / `vcom.uart` — serial capture.
- `<name>.dbg` / `rx.dbg` — guest-error log (qrun truncates; some scripts also
  pre-`rm`).
- `tap.raw` — SAI TX tap (`edma`, `i2s`, `audiooutput`, `sai_rx`).
- `inject.raw` — python-gen injector (`sai_rx`, `audioinput`) — **now cleaned**.
- `inject.raw.fifo`, `.fifo.in`, `.fifo.out` — mkfifo pipe + pipe-backend
  sidecars (`sai_rx`, `audioinput`).
- `echo.result` — python driver stdout redirect (`usb_data`).

Background processes to reap besides QEMU: the one-shot `cat` (`sai_rx`), the
looping pump (`audioinput`). Foreground python drivers (`serial_test_rx`,
`usb_data`) are sequenced by the script, not reaped by the lib.

## 8. Verification plan

1. **Regression** — re-run at least one gate per outlier class and confirm
   identical PASS markers: `serial` (plain), `sai_rx` (one-shot fifo),
   `audioinput` (looping pump), `interval_timer` (20s + icount), `usb_data`
   (driver-gated), `audiooutput` (aggregated asserts). Ideally all 19.
2. **Safety net**
   - **Signal teardown:** SIGINT / SIGTERM / SIGHUP a running gate mid-run →
     assert QEMU reaped **and no leftover `*.fifo` / sidecar / `inject.raw`**.
   - **Hang backstop:** point a gate at a QEMU that won't die (or set a tiny
     `GATE_TIMEOUT`) → assert gtimeout reaps within the timeout and the script
     exits non-zero.
   - **Orphan:** start a gate, kill its parent shell, confirm gtimeout still
     fires and cleans up (no lingering runner, no temp leak).
3. **Idempotency:** confirm INT (cleanup) → EXIT (cleanup again) does not error.

## 9. Location & rollout

- New file: `evkb/tools/gate-lib.sh`, next to `qrun`. Sourced via the same
  `~/Development/rt1170/evkb/tools/…` absolute style scripts already use for
  `QEMU=`.
- Refactor all 19 `evkb/*/run_*.sh`. Roll out in waves by outlier class, running
  the regression check after each wave so a break is isolated to a small batch.
- **Git caveat:** tree is not a repo, so this spec is written to disk but not
  committed. Same for the implementation.

## 10. Open questions

None blocking. Defaults chosen: `GATE_TIMEOUT=600`, wart-fixes included.
