# W11: Throughput Measurement + Bottleneck Fix — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Measure real TCP/UDP throughput both directions against the house
AP with Mac-side authoritative counting, then fix the single top bottleneck
the new driver counters attribute.

**Architecture:** New passive-server example `networking/m2_throughput_test`
(lwip raw API) sequenced by a committed Mac peer script; bus-level counters
added to the M2Radio driver; shared lwipopts tuned so lwip is not the
artificial ceiling.  Spec: `docs/superpowers/specs/2026-08-19-w11-throughput-design.md`.

**Tech stack:** M2Radio (SDIO/IW416 + lwip netif glue), lwip NO_SYS raw
API, Python 3 stdlib peer, CMake/QEMU gate harness.

**Hard rules for every task:** subagents never touch LinkServer, serial
ports, or the boards (build + QEMU only — hardware is the controller's).
The house PSK exists ONLY in the session scratchpad and gitignored build
caches; never in a tracked file, commit message, or configure output.
Never weaken a gate or the licence audit.

---

### Task 1: lwip — raise the throughput-relevant limits (sibling repo `~/Development/lwip`)

**Files:** Modify: `port/lwipopts.h` (the shared port used by every evkb lwip example)

- [x] **Step 1:** In `port/lwipopts.h` change exactly these values (current → new):

```c
#define MEM_SIZE                     (48 * 1024)   /* was 24*1024 */
#define MEMP_NUM_TCP_SEG             40            /* was 24; >= (4*TCP_SND_BUF+TCP_MSS-1)/TCP_MSS = 32 exactly */
#define PBUF_POOL_SIZE               32            /* was 16 */
#define TCP_SND_BUF                  (8 * TCP_MSS) /* was 6* */
#define TCP_WND                      (8 * TCP_MSS) /* was 2* -- the RX throttle */
```

Update the comment above `MEMP_NUM_TCP_SEG` to the new arithmetic.  Touch
nothing else.

- [x] **Step 2:** Rebuild BOTH existing lwip consumers in this tree and re-run their gates (both must PASS unchanged — they assert reachability/fallback, not window sizes):

```sh
cd ~/Development/rt1176-evkb-m2-maya-w161/examples/networking/lwip_test    && cmake --build build && ./run_qemu.sh
cd ~/Development/rt1176-evkb-m2-maya-w161/examples/networking/m2_lwip_test && cmake --build build && ./run_qemu.sh
```

(If `lwip_test` is named differently, find every consumer with
`grep -rl "import_evkb_library(lwip" examples/` and rebuild+gate each.)

- [x] **Step 3:** Commit in `~/Development/lwip` (do NOT push; the controller pushes and bumps the pin at close-out):

```sh
git -C ~/Development/lwip add port/lwipopts.h && git -C ~/Development/lwip commit -m "port: raise TCP window/sndbuf + pools for throughput work (W11)"
```

---

### Task 2: M2Radio — bus counters for bottleneck attribution (sibling repo `~/Development/M2Radio`)

**Files:** Modify: `iw416/Iw416.h`, `iw416/Iw416.cpp`

- [x] **Step 1:** Add zero-cost counters (plain `uint32_t`, no behaviour change), reset alongside the existing counters on fw download:
  - `m_cmd52PollsTx` — CMD52 reads spent inside `sendDataFrame`'s wr_bitmap wait loop (increment per CMD52 issued there).
  - `m_cmd52PollsSvc` — CMD52 reads issued by `serviceLink` per call (interrupt-status reads, bitmap reads; increment per CMD52).
  - `m_cmd53Count`, `m_cmd53Bytes` — every data CMD53 issued (TX and RX), and its byte total.
  - `m_cmd53ByteMode` — how many of those went in byte mode rather than block mode (read what SdioHost actually does; if it is 100% one mode, the counter proves it).
  Accessors: `cmd52PollsTx() / cmd52PollsSvc() / cmd53Count() / cmd53Bytes() / cmd53ByteMode()`, `const`, next to the ps accessors.
- [x] **Step 2:** Build the probe + lwip examples against the local driver; run both QEMU gates (PASS):

```sh
cd ~/Development/rt1176-evkb-m2-maya-w161/examples/networking/m2_sdio_probe && cmake --build build && ./run_qemu.sh
cd ~/Development/rt1176-evkb-m2-maya-w161/examples/networking/m2_lwip_test  && cmake --build build && ./run_qemu.sh
```

- [x] **Step 3:** Commit in `~/Development/M2Radio` (no push):

```sh
git -C ~/Development/M2Radio add -A && git -C ~/Development/M2Radio commit -m "iw416: bus counters -- cmd52 polls (tx/svc), cmd53 count/bytes/mode (W11 attribution)"
```

---

### Task 3: example `networking/m2_throughput_test` + Mac peer + gate

**Files:**
- Create: `examples/networking/m2_throughput_test/CMakeLists.txt` (copy `../m2_lwip_test/CMakeLists.txt`; substitute the project/target name; identical fw-blob and creds blocks)
- Create: `examples/networking/m2_throughput_test/m2_throughput_test.cpp`
- Create: `examples/networking/m2_throughput_test/tput_peer.py` (committed tooling, no credentials)
- Create: `examples/networking/m2_throughput_test/run_qemu.sh` (+ `transcript_qemu.txt` from its first pass)
- Modify: `tools/license-audit.sh` — add `examples/networking/m2_throughput_test:m2_throughput_test` to `GATES`

**The .cpp** reuses `m2_lwip_test.cpp`'s skeleton verbatim where possible:
same board preamble (`m2ReleaseWifiReset` + `useIoVoltage1V8` at the top of
`setup()` with the lesson comment), same fw/creds `#if` guards, same
`wifiConnect()` via `connectStation`, same netif+`dhcp_start` bring-up,
same `iw416NetifPoll`/`sys_check_timeouts` loop, same reconnect throttle.
Delete the echo-client machinery; add the four passive services:

```c
enum { TPUT_TCP_RX_PORT = 5001, TPUT_TCP_TX_PORT = 5002, TPUT_UDP_PORT = 5003 };
enum { UDP_PAYLOAD = 1400, TX_BLAST_MS = 10000 };
/* UDP data datagram: bytes 0..3 = big-endian sequence, rest pattern.
 * UDP control (ASCII, prefix "TPUT "):
 *   "TPUT STATS?"        -> reply "TPUT STATS rx=<n> hi=<h>" to sender, reset udp-rx counters
 *   "TPUT GO <secs>"     -> blast seq'd datagrams to sender addr:port for <secs>,
 *                           then send "TPUT DONE tx=<n>"; ~4 datagrams per loop pass max
 *                           so the netif keeps getting polled (NO_SYS: never block).
 */
```

Behaviour contracts (the W9 lessons apply):
- TCP RX sink on 5001: `tcp_recv` counts `p->tot_len`, `tcp_recved`, frees,
  discards; peer FIN (`p == NULL`) → print
  `tput: tcp_rx bytes=<n> ms=<m> kbps=<k>` (ms measured first-byte→FIN),
  clear all callbacks BEFORE `tcp_close`; in-callback abort paths return
  `ERR_ABRT` (never touch the pcb after).
- TCP TX source on 5002: on accept, enqueue from a static 1024-B pattern
  while `tcp_sndbuf(pcb) >= 1024` (bounded writes per poll pass), `tcp_output`,
  continue from the sent callback; at `TX_BLAST_MS` elapsed stop enqueuing,
  close after the last sent ack; print `tput: tcp_tx bytes= ms= kbps=`.
- UDP on 5003: one `udp_recv` callback demuxes control vs data by the
  `TPUT ` prefix.  Data: count + track highest seq.  GO blast: allocate
  PBUF_RAM per datagram, `udp_sendto` the requester; if alloc or send
  fails, count `udp_tx_drop` and yield the pass (back-pressure, don't spin).
- One second cadence status line, extending the m2_lwip_test shape:
  `tput: ip=<ip> ps=<...> bus=c52tx:<n>,c52svc:<n>,c53:<n>/<bytes>,byte:<n> <test-results-so-far>`
  using the new Task 2 accessors.
- Between tests everything stays up; services re-arm (each test runnable
  repeatedly without reflash).

**tput_peer.py** (Python 3 stdlib only, ~150 lines): subcommands
`tcp-rx <ip>` (connect :5001, blast 10 s of 64 KiB writes, FIN, print),
`tcp-tx <ip>` (connect :5002, recv+count to FIN, print),
`udp-rx <ip>` (blast seq'd 1400-B datagrams 10 s → `TPUT STATS?` ×3
retries → parse reply, print sent/rx/loss),
`udp-tx <ip>` (bind one socket, send `TPUT GO 10`, recv+count 12 s,
parse `TPUT DONE`, print rx/tx/loss), and `all <ip>` running the four in
that order, 2 s apart, ending with a 4-line machine-readable summary
`TPUT <test> mbps=<x> [loss=<y>]` — the Mac side is authoritative;
compute mbps from ITS byte counts and wall clock.

**run_qemu.sh:** copy `m2_lwip_test/run_qemu.sh`; the pass condition is
the same device-absent fallback (expected tokens: the banner, the
cmd5-no-response fallback line, `alive=` heartbeat, and NO `tput: ip=` /
no link claimed).  Generate `transcript_qemu.txt` from the first passing
run.

- [x] **Step 1:** Write the four files; configure + build WITHOUT creds/fw (fallback image):

```sh
cd examples/networking/m2_throughput_test
cmake -B build -DCMAKE_TOOLCHAIN_FILE=../../../toolchain/rt1170-evkb.toolchain.cmake
cmake --build build
```

- [x] **Step 2:** `./run_qemu.sh` → PASS; commit `transcript_qemu.txt` with the rest.
- [x] **Step 3:** Add the GATES entry to `tools/license-audit.sh`; run the audit:

```sh
LICENSE_AUDIT_EVKB=$(pwd) ./tools/license-audit.sh   # from repo root; expect PASS
```

- [x] **Step 4:** `python3 -m py_compile tput_peer.py` (syntax gate) and a loopback smoke of the peer's arg parsing (`./tput_peer.py --help` exits 0).
- [x] **Step 5:** Commit (rt1176-evkb repo):

```sh
git add examples/networking/m2_throughput_test tools/license-audit.sh
git commit -m "networking: m2_throughput_test -- passive TCP/UDP blast services + Mac peer (W11)"
```

---

### Task 4 (controller-run, hardware): first measurement vs the house AP

- [x] Configure the example WITH creds (PSK read from the session scratchpad at configure time, lands only in the gitignored build cache + generated header) + the fw blob; build; flash with `LinkServer flash … load` (VCOM free).
- [x] First silicon step: watch the scan/connect on serial.  **STOP CONDITION:** if `OnestreamQJN7` (2.4 GHz) is not in our scan, stop the phase and re-plan (5 GHz scan support is its own work item — do not blind-patch).
- [x] On join + DHCP: note ip, run `python3 tput_peer.py all <ip>` on the Mac (firewall Allow prompt possible — user clicks).  Record the 4-cell table + `ps=` + `bus=` counters.
- [x] Commit the raw numbers into `transcript_hw_evkb.txt` as the "first measurement" section (even before any fix — the baseline is evidence).

### Task 5: attribute + fix the top bottleneck (data-driven; scope = ONE fix)

- [x] From the `bus=` counters compute per-frame and per-byte bus cost (e.g. CMD52 polls per TX frame; CMD53 bytes vs wire bytes; byte-mode share).  Write the attribution as 3-5 sentences in the transcript draft.
- [x] Dispatch an implementer for ONLY the top item (expected candidates, in advance-guess order: CMD53 byte-mode → block mode; CMD52 poll loop tightening/batching; multi-frame service per pass).  Full review loops.  Gates re-run.
- [x] Controller reflash + re-run `tput_peer.py all`; record the before/after delta in the transcript.  If the fix moved nothing, say so and stop (no second fix without a new plan).

### Task 6 (controller): close-out

- [x] Final transcript section (table, delta, PS-under-load observation, WMM note — first association against a WMM AP is itself a data point for the erratum record).
- [x] Push `~/Development/lwip` and `~/Development/M2Radio`; bump BOTH pins in `evkb.cmake`; verify with a `-DEVKB_FORCE_FETCH=ON` configure of the new example.
- [x] Sweep from `/tmp/ev`: expect **97 passed, 0 failed, 0 SKIP** (or 96/1/0 only if the known nondeterministic dual-core gate is the red one).  Update CLAUDE.md's gate count + measured line.
- [x] `LICENSE_AUDIT_EVKB=$(pwd) ./tools/license-audit.sh` → PASS.
- [x] W12 handoff doc; final branch review of controller-authored commits (done: CHANGES REQUIRED -> doc fixes landed, incl. the credential-exposure finding); commit + push branch.
