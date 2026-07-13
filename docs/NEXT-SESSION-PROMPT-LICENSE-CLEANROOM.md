# Next session: clean-room the remaining LGPL files — squeaky-clean permissive firmware

We're making the decision Paul didn't: **no copyleft code of any kind compiled
into RT1176 firmware, ever.** The 2026-07-13 license audit (see the
`prefer-permissive-licenses` memory note and `~/Development/SPI/LICENSE.md` for
the pattern already applied) fixed SPI, NativeW5100.h, and Wire's headers. What
remains is the classic Arduino/Wiring-lineage LGPL-2.1 files that every Arduino
core ships and that we vendored via teensy4. PJRC kept them LGPL even after
mostly rewriting them; we will replace them with genuine clean-room MIT
implementations so the question can never reach a lawyer's desk.

## The cases (all LGPL-2.1-or-later per file header, all COMPILED into firmware)

In the core (`~/Development/rt1170/evkb/cores/imxrt1176`, git root `evkb/cores`):

| file | lines | difficulty | what it is |
|---|---|---|---|
| `Printable.h` | 42 | trivial | abstract `printTo(Print&)` interface |
| `Stream.h` + `Stream.cpp` | 76+298 | moderate | `available/read/peek` virtuals + timeout/parse/find/readBytes helpers |
| `WString.h` + `WString.cpp` | 248+823 | **the big one** | the Arduino `String` class |
| `Time.cpp` | 127 | small | `breakTime()`/`makeTime()` civil-time conversion (consumed by SNVS RTC + SD file timestamps) |

In the libraries (vendored copies — fix in BOTH repos, they drifted from one source):

| file | repos | difficulty |
|---|---|---|
| `src/Client.h` (48), `src/Server.h` (33) | `~/Development/Ethernet` AND `~/Development/NativeEthernet` | trivial (abstract interfaces) |
| `src/IPAddress.cpp` (65) | both of the above | small (`IPAddress.h` did NOT hit the audit grep — verify its header before touching; if it's clean, only the .cpp needs rewriting) |

Explicitly OUT of scope (already resolved or accepted):
- `SPI.h`/`SPI.cpp`, `Wire.h`/`Wire.cpp`/`utility/twi.*` — upstream platform
  branches, preprocessor-dead in RT1176 builds, documented in each repo's
  LICENSE.md. Repo *contents* may stay dual/LGPL for other platforms; only the
  **firmware image** must be copyleft-free. Do NOT try to rewrite other
  platforms' code.
- `SdFat/src/DigitalIO/*` (GPLv3, never compiled by our USDHC path): **delete
  the directory and the soft-SPI driver that includes it from the newdigate
  fork** (grep `SdSpiSoftDriver`/`SPI_DRIVER_SELECT` for what to remove), so it
  can never be silently enabled. Verify the SD gates after.
- QEMU (GPLv2 tool, never linked into firmware).

## Clean-room discipline — this is the part that makes it defensible

The output must be *independently authored*, not a paraphrase. For every file:

1. **Spec from documentation, not source.** Write the API contract from the
   public Arduino reference (docs.arduino.cc language reference for `String`,
   `Stream`, `Client`, `Server`, `IPAddress`, `Printable`) plus the signatures
   our ecosystem actually calls. Do NOT open the LGPL file while writing the
   implementation. It is fine (and necessary) to extract the *bare
   declarations* consumers depend on — API surfaces, method names, and virtual
   signatures are facts of interface compatibility — but no comments, no body
   code, no internal helper structure may be carried over.
2. **Harvest the required surface mechanically first**: grep every consumer
   (core, all newdigate libs, all gate sketches) for the methods/operators they
   use (e.g. `grep -rhoE '\.(concat|toInt|toFloat|trim|substring|indexOf|replace|...)\(' `)
   and record the list in the plan. Implement the full documented API, but the
   harvested list defines the *must-pass* test set.
3. **Two-agent separation (subagent-driven-development gives this for free):**
   the agent that writes the spec/tests may read anything; the agent that
   writes the implementation gets ONLY the spec + tests, and its prompt must
   say "do not read WString.cpp / Stream.cpp / IPAddress.cpp / the teensy4
   copies".
4. Well-known algorithms are safe to use by name: days-from-civil /
   civil-from-days (Howard Hinnant's public-domain algorithms) for `Time.cpp`;
   standard grow-by-doubling buffer management for `String`.
5. Every new file gets the MIT header + a provenance paragraph in the style of
   `cores/imxrt1176/WCharacter.h` ("Clean-room MIT implementation … written
   from the documented Arduino API surface, not derived from the LGPL …").

## Behavioral compatibility traps (learned the expensive way elsewhere — read these)

- `String` invariants consumers silently rely on: an invalid/OOM `String` has
  `c_str() != NULL` returning `""`; `operator+` chains; `F()`/
  `__FlashStringHelper*` overloads (flash == RAM on this core but the
  overloads must exist); `toInt` on garbage returns 0; `substring` clamps
  out-of-range; self-assignment and self-append (`s += s`) must work.
  Float formatting goes through `dtostrf` (`avr_functions.h` — already clean).
- `Stream::parseInt/parseFloat` semantics: skip non-numeric lead-in, honor
  `setTimeout` (default 1000 ms), return 0 on timeout; `readBytesUntil`
  consumes but does not store the terminator. Timeout uses `millis()`.
- `Stream.h`'s **virtual method set and order must match what `Print` +
  existing subclasses expect** — HardwareSerial, usb_serial, EthernetClient,
  File all inherit it. Changing the vtable surface breaks every subclass:
  keep exactly `available() / read() / peek()` pure-virtual (+ our existing
  extras — enumerate from the subclasses, not from the old header, e.g.
  `grep -h "virtual" */HardwareSerial.h EthernetClient.h`).
- `Time.cpp`: `breakTime`/`makeTime` use the `DateTimeFields` layout declared
  elsewhere (find the declaring header first); epoch is Unix 1970, `tm.year`
  offset per that struct's convention — the SNVS RTC gate and SD timestamp
  HW test are the behavioral oracle.
- `IPAddress`: implements `Printable`; `operator uint32_t` byte order is
  network-order-in-memory (dotted string prints in storage order) — the
  Ethernet gates (DNS + HTTP GET) will catch it if wrong.
- Ethernet and NativeEthernet each vendor their own copies — after rewriting,
  make the two repos' copies byte-identical (single authored source, copied),
  and say so in each commit message.

## Method — same as every bring-up

`brainstorming` → `writing-plans` → `subagent-driven-development`, gate-first.
Suggested task order (each = write tests first, watch them pass against the
OLD file, then swap in the clean-room file and re-run — the old implementation
is the behavioral oracle, used as a black box):

1. `Printable.h` + `Client.h` + `Server.h` + `IPAddress` (one warm-up task —
   trivial interfaces, establishes the pattern + provenance-header style).
2. `Time.cpp` (small; SNVS RTC gate `evkb/rtc_test` is the oracle).
3. `Stream.{h,cpp}` (new QEMU gate: a RAM-backed Stream stub exercising every
   parse/read/timeout path; then re-run serial + Ethernet + SD gates, which
   are the real consumers).
4. `WString.{h,cpp}` (new QEMU gate `evkb/` or `cores` test with an exhaustive
   String-API sketch built from the harvested-surface list; assert against
   values computed independently, not against old-String output *text* —
   avoid enshrining quirks, but investigate any divergence).
5. SdFat DigitalIO deletion + SD gates re-run.
6. **Final audit gate:** re-run the wrap-tolerant sweep over every ecosystem
   repo — `grep -rIlz -E "GNU[[:space:]]+(General|Lesser)[[:space:]]+(General[[:space:]]+)?Public[[:space:]]+License"`
   (the single-line variant MISSES wrapped headers — this exact mistake
   happened in the 2026-07-13 audit) — and additionally verify the *firmware
   image* claim directly: take one fat gate build (`sd_wav_play_test` links
   cores+SPI+Wire+Audio+SdFat+SD), list every object in the link (`ninja -t
   deps` / the `.map` file), and check each contributing source's header is
   permissive. Consider committing that as `evkb/tools/license-audit.sh` so it
   can run in CI forever — that's the "never on the lawyer's desk" guarantee.

Full QEMU gate suite + at least the RTC/SD/Ethernet HW-relevant gates must be
green before any commit claims completion; the swap of String/Stream touches
literally everything, so budget a full-ecosystem rebuild (file(GLOB) trap: a
replaced core .cpp with a new filename needs `rm -rf build` + reconfigure in
every test dir — keep the SAME filenames to avoid this).

## Done criteria

- Zero files with GPL/LGPL headers compiled into any RT1176 firmware image,
  proven by the link-manifest audit, not by grep alone.
- All ecosystem QEMU gates green; RTC + SD timestamp behavior re-verified on
  HW (the Time.cpp swap is the only one with HW-visible risk).
- LICENSE.md in cores repo documenting the now-fully-MIT status; provenance
  headers in every rewritten file; `prefer-permissive-licenses` memory note
  updated; everything pushed.
