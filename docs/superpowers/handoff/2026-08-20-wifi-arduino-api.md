# Handoff: an Arduino-style `WiFi` / `WiFiClient` / `WiFiServer` for the M.2 link

u-blox **M2-MAYA-W161** (IW416/SD8978) on **MIMXRT1170-EVKB RevC3**, repo
`~/Development/rt1176-evkb-m2-maya-w161`, branch **`m2-phase0-serial2`**.
Pins: **M2Radio `1e15f0b`**, **lwip `c6b2548`**.

**Read first:** `examples/networking/m2_lwip_test/m2_lwip_test.cpp` — the
smallest working example of the whole stack (board preamble → `connectStation`
→ netif → DHCP → raw-API TCP), and `docs/superpowers/plans/2026-08-19-m2-lwip-netif.md`.

## The gap this closes

The Wi-Fi works — five faults closed, gated in QEMU, interrupt-driven service
silicon-validated — but it is **not consumable**. Using it today means writing
M2Radio + lwip raw-API glue by hand: board preamble, `connectStation()`,
`netif_add`/`dhcp_start`, an `iw416NetifPoll()`/`sys_check_timeouts()` pump,
and lwip callback discipline. That is driver-author work, not sketch-author
work. This phase puts an Arduino façade on it so a normal sketch can do
`WiFi.begin(ssid, psk); WiFiClient c; c.connect(host, 80); c.println(...)`.

This is **user-facing value per hour**, not more internals — a different kind
of work from W8–W15 and a reasonable place to stop going deeper.

## ★ The landmine to resolve BEFORE writing code

Arduino's `WiFiClient`/`WiFiServer` conventionally derive from the core's
`Client` / `Server` base classes (that is what makes them interchangeable with
`EthernetClient` in user code, and what lets `Print`/`Stream` work).

**The `imxrt1176` core has no `Client.h` or `Server.h`.** They exist only in
the `teensy4` (rt1062) core, and per `CLAUDE.md` they **carry LGPL text and
are deliberately left there uncompiled**: "no link manifest includes them.
Check the manifest, not this note, before adding a header."

So copying them in would put LGPL headers into an rt1176 link manifest and
**break `tools/license-audit.sh`** — the tree is permissive-only, and this
exact class of breakage already cost a day once (the RT1060 board axis
compiled five LGPL core files; they were fixed the only acceptable way, by
**replacing them with MIT clean-room versions**, not by relaxing the audit).

Three viable routes, decide deliberately and record why:

1. **Clean-room MIT `Client.h`/`Server.h` for `imxrt1176`** — follows the
   precedent that already exists in this tree (`WString`, `IPAddress`,
   `Stream`, `WMath`, `Time`, `Printable.h`, `WCharacter.h` were all replaced
   this way). Gives full Arduino polymorphism. Most work, best result.
2. **No base classes** — standalone `WiFiClient`/`WiFiServer` deriving only
   from `Print`/`Stream` (which DO exist in the rt1176 core, MIT). Loses
   drop-in interchangeability with Ethernet code.
3. Derive from nothing; plain classes. Simplest, least Arduino-ish.

Whatever you choose: **run `LICENSE_AUDIT_EVKB=$(pwd) ./tools/license-audit.sh`
early**, not at the end. Green does NOT mean "this tree is MIT" — the audit
greps for copyleft, and Apache-2.0 passes it; it answers *is there copyleft
here*, not *is each file the licence it claims*.

## The central design tension

**Arduino's API is blocking; this lwip is `NO_SYS=1` and must never block.**
`client.connect()`, `client.read()`, `server.available()` are all synchronous
in sketch code, but nothing may stall the single-threaded main loop — the
netif still has to be serviced or the link dies.

The resolution is that every blocking API call must **pump the stack while it
waits**: `iw416NetifPoll(&netif)` + `sys_check_timeouts()` in the wait loop,
with a timeout, exactly as the examples' loops do today. Get this wrong and
you get a sketch that connects once and then goes deaf. Look at
`m2_lwip_test.cpp`'s loop for the required cadence, and note the driver's
service call is also where the **ring safety net** ticks (see below).

## Non-negotiable behaviours to preserve

These are hard-won; a façade that hides them re-opens closed faults.

* **IEEE PS must stay ON** (`connectStation`'s default). It is the W10
  workaround for the firmware idle RX-death erratum — a sparse link with PS
  off dies in 1–44 minutes. PS was investigated and exonerated of every other
  charge in W12; do not expose an "off" switch casually.
* **The service pump must keep running even when the sketch is idle.** The
  ring safety net (which covers the firmware not always raising the upload
  interrupt — W13) ticks on service passes. A façade whose `loop()` only
  services when the user calls a read is a façade that strands frames.
* **The M.2 board preamble** (SDIO_RST/WL_RST release + 1.8 V pad switch)
  currently lives in **examples, not the driver** — an example without it is
  green in QEMU and dead on silicon, because QEMU has no card either way.
  A `WiFi` library is arguably the right home for it at last; if you move it,
  say so loudly in both places, and keep the lesson comment.
* **Credentials never enter the repo.** The established pattern is a
  configure-time CMake cache var rendered into a gitignored generated header,
  with CMake printing "(PSK supplied, not shown)". See
  `examples/networking/m2_lwip_test/CMakeLists.txt`. A live PSK was once
  committed to this public repo and is still in its history — that is why this
  rule is absolute.

## lwip raw-API discipline (learned by shipping the bugs)

* Clear **every** callback (`tcp_recv/sent/err/poll`, arg) **before**
  `tcp_close`; on a failed close, `tcp_abort`.
* An **in-callback** abort must return `ERR_ABRT` — returning `ERR_OK` after
  `tcp_abort` leaves `tcp_input` touching a freed pcb (a real use-after-free
  shipped and was caught in review).
* The `tcp_err` callback fires with the pcb **already freed** — reset state
  only, never call `tcp_*` on it.
* Free pbufs on every path; handle chained pbufs (`pbuf_copy_partial`), never
  assume contiguity.
* Bound work per pass — no unbounded loops inside a callback.
* Add stall safety valves (a peer that vanishes without FIN/RST must not wedge
  a one-connection-at-a-time service forever).

`examples/networking/m2_throughput_test/m2_throughput_test.cpp` implements all
of this correctly after two review rounds — copy its patterns rather than
re-deriving them.

## Suggested shape

* New sibling library (this repo holds no libraries): `~/Development/WiFi101`-
  style repo, imported via `import_evkb_library(...)` with a pin in
  `evkb.cmake`, same as M2Radio/lwip. Push it and bump the pin at close-out;
  verify with a `-DEVKB_FORCE_FETCH=ON` configure (a fresh-clone compile break
  shows up as a **SKIP, not a FAIL**, in the sweep — it hides).
* Minimum surface: `WiFi.begin/status/localIP/RSSI/disconnect`,
  `WiFiClient connect/connected/available/read/write/stop`,
  `WiFiServer begin/available`. Resist scope creep; `WiFiUDP` can be phase 2.
* One example, e.g. `examples/networking/wifi_client_test`, with a QEMU gate
  asserting the **device-absent fallback** (QEMU has no radio and cannot
  associate — the IW416 model deliberately returns **zero** scan results, so
  no gate may assert association) plus a silicon transcript against the ESP
  bench AP.

## Testing conventions

* **Two-gate rule**: QEMU gate + hardware transcript with un-fakeable
  assertions. **Silicon wins**; never weaken a gate or the model to hide a
  divergence — document it.
* QEMU coverage for this layer is inherently thin (no association possible).
  Say so in the gate header rather than implying more than it proves — see
  `run_qemu_wifi.sh` for the convention of stating what a gate does NOT prove.
* Sweep from `/tmp/ev` (macOS `sun_path` 104-byte limit); baseline **102/0/0**;
  read gate **names** on a red, never just the count.
* Bench: ESP8266 AP `ESP8266TEST` on `/dev/cu.usbserial-0001` (its own
  throwaway PSK; the house AP creds are dead after rotation). Flash
  `LinkServer flash … load` **VCOM-free**. Never `cmake -B`/`rm -rf` an
  existing example build dir — they carry creds and the fw-blob path, and a
  wiped cache looks exactly like a dead card on the serial output.

## Working conventions that paid off this session

* Spec → plan → implementer subagent → **spec review then quality review** →
  silicon. Reviewers overrode the controller correctly several times; when a
  reviewer says the brief is wrong, it usually is.
* **Instrument the failure rather than reasoning about it.** Three plausible
  mechanisms were argued and discarded before one dump of the actual failure
  state settled a two-day bug in a single line.
* Keep wrong conclusions **in the record, marked**, with the measurement that
  killed them — the transcripts do this deliberately, and it is why the same
  dead theories are not re-derived.
