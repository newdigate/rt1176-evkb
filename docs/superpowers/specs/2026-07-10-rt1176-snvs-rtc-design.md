# RT1176 SNVS RTC (real time-of-day, battery-backed) — Design

**Status:** approved (design), ready for implementation plan
**Date:** 2026-07-10

## Goal

Replace the placeholder `cores/imxrt1176/rtc.c` (which returns `0` → 1970 → zero FAT
timestamps) with a **real SNVS RTC**: an SNVS_HP real-time counter synced from an
SNVS_LP *secure* real-time counter, both 47-bit up-counters clocked by the 32.768 kHz
RTC crystal in the always-on (VBAT / coin-cell) power domain. Wire it to the Teensy
`Time` API so `rtc_get()` returns **live Unix-epoch seconds that tick at 1 Hz** and
**survive a warm reset** (HP reloads from the still-running, battery-backed LP counter).
Keep it **minimal** — a faithful teensy4 port, not the full `fsl_snvs_hp` alarm / tamper /
calibration surface. The end-to-end proof is that **SD/FAT file timestamps become real**
through the existing PaulS_SD path (the one live consumer of the stub).

The signatures are frozen by existing callers and MUST NOT change:
`unsigned long rtc_get(void)` / `void rtc_set(unsigned long)` / `void rtc_compensate(int)`
(declared `core_pins.h:2948-2950`, wrapped by the `Teensy3Clock` class at
`core_pins.h:2982-2986`), all in Unix epoch seconds.

## Why this shape (exploration findings)

- **This is a CORE bring-up, not a library move.** `rtc.c` is already a core file (as on
  teensy4); `Time.cpp` (`breakTime`/`makeTime`) and the `Teensy3Clock` wrapper already
  live in the core. No library changes.
- **The port source is checked out and near-1:1.** `cores/teensy4/rtc.c` (91 lines)
  implements the exact behaviour on the **same SNVS block**. `rtc_get` double-reads the HP
  RTC tear-free (`sec = (HPRTCMR<<17) | (HPRTCLR>>15)`); `rtc_set` stops HP+LP, writes the
  LP secure counter (`LPSRTCLR = t<<15`, `LPSRTCMR = t>>17`), starts LP, then starts HP
  with `HPCR = RTC_EN | HP_TS` so **HP synchronises from LP**; `rtc_compensate` is a no-op;
  a weak `_gettimeofday` gives newlib sub-second time. **The only silicon delta is the base
  address** — RT1062 `0x400D4000` → RT1176 **`0x40C90000`** (cm7-header-confirmed). Register
  offsets and bit defs are identical.
- **The persistence mechanism is in `startup.c`, not `rtc.c`.** teensy4 `startup.c:178-185`
  runs on *every* boot: if the LP secure RTC isn't already running, start it at a default
  epoch (Jan 1 2019 = `1546300800`); then unconditionally `HPCR |= RTC_EN | HP_TS` to reload
  the HP counter from the (persisted) LP counter. The RT1176 core's `ResetHandler`
  (`startup.c`) currently has **no** SNVS init. So the port must add this init, mirroring
  teensy4, after `set_arm_clock_rt1176()` (clocks up) and before `__libc_init_array()`.
- **The core header has zero SNVS defs.** `grep SNVS imxrt1176.h` → nothing. teensy4
  `imxrt.h` has the full block + bit masks to port. `imxrt1176.h` is **auto-generated** —
  defs go in **both** the header and `tools/gen_imxrt1176_h.py`.
- **QEMU already models SNVS** (`hw/misc/imxrt_snvs.c`, 330 lines; instantiated in
  `hw/arm/fsl-imxrt1170.c` at `0x40C90000`, IRQ 66). It models HP + LP as 47-bit
  32.768 kHz up-counters advancing in `QEMU_CLOCK_VIRTUAL`, the enable bits, the RTC
  MSB/LSB set/read paths, the HP time-alarm IRQ, the battery-backed LP-GPR, and a
  `reset_hold` that resets the HP counter but **preserves the LP secure counter + LP-GPR**
  (the persistence primitive is already present). **Confirmed gap:** the `SNVS_HPCR` write
  handler (`imxrt_snvs.c:207-212`) keys enable only off `RTC_EN` and does **not** honour
  `HP_TS` (bit 16 — not even defined). So a teensy4-style `rtc_set` (which loads the *LP*
  counter and relies on `HP_TS` to copy HP←LP) leaves the modelled HP counter at 0 and
  `rtc_get` reads wrong. **Honouring `HP_TS` is the one required QEMU refinement.**
- **No new core files** — `rtc.c` and `startup.c` both already exist and are edited in
  place, so there is **no CMake `file(GLOB)` reconfigure** needed (unlike the EEPROM
  bring-up, which added a new `eeprom.c`).

## Scope

**In scope:** the real `rtc.c` (port of `cores/teensy4/rtc.c`, base retargeted to
`0x40C90000`) — `rtc_get`, `rtc_set`, `rtc_compensate`, weak `_gettimeofday`; the per-boot
SNVS init in `startup.c` (LP-default-start-if-stopped + HP sync); the SNVS register block +
bit masks in `imxrt1176.h` **and** `tools/gen_imxrt1176_h.py`; the QEMU `HP_TS` refinement;
a QEMU gate + HW verification. `Time.cpp` / `breakTime` / `makeTime` / `Teensy3Clock` /
`core_pins.h` signatures unchanged.

**Explicitly deferred (YAGNI):** SNVS alarm / periodic interrupt / tamper / security-violation
handling; `rtc_compensate` calibration (stays a no-op, as on teensy4); any SNVS ON/OFF-button
(`SNVS_PULSE_EVENT_IRQn`) handling; a coin-cell true-power-off HW test (per decision:
**warm-reset persistence only**). The SNVS IRQ line (66) is modelled by QEMU but the port
uses **polled** counters only — no ISR is registered.

## Decisions (resolved during brainstorming)

- **Cold-boot default:** match teensy4 — start the LP SRTC at **Jan 1 2019 (`1546300800`)**
  if not already running. Consequence: a never-set board reads ~2019 and stamps SD files
  with 2019 dates (not the stub's zero date). Accepted.
- **Boot-init location:** **Approach A** — in `startup.c` after the clock init. It is a
  handful of SNVS **MMIO** writes, not a memory-pool setup, so the [[rt1176-extmem-malloc]]
  boot-path reset-loop hazard (which was specific to `sm_set_pool()` / the FLASH+0x2000
  layout) very likely does not apply. **Fallback (Approach B):** if HW reset-loops, move the
  init to a lazy once-guard in `rtc_get`/`rtc_set` (application context), mirroring how
  `extmem_malloc` dodged the boot path — at the cost of an unsynced HP=0 before first use.
- **HW persistence depth:** warm reset only (LinkServer `run` / reset button); no coin cell.

## Architecture — components

### 1. SNVS register block (`imxrt1176.h` + `tools/gen_imxrt1176_h.py`)
Auto-generated header → add to **both**. Base `IMXRT_SNVS_ADDRESS = 0x40C90000`. Port the
HP/LP register block from `cores/teensy4/imxrt.h` (offsets identical), following the header's
existing register-block idiom. The registers the port actually touches:
`SNVS_HPCR` (0x08), `SNVS_HPRTCMR` (0x24), `SNVS_HPRTCLR` (0x28), `SNVS_LPCR` (0x38),
`SNVS_LPSRTCMR` (0x50), `SNVS_LPSRTCLR` (0x54) — plus the rest of the block
(`HPLR/HPCOMR/HPSR/HPTAMR/HPTALR/LPLR/LPSR/LPGPR…`) for parity. Bit masks needed:
`SNVS_HPCR_RTC_EN` (`1<<0`), `SNVS_HPCR_HP_TS` (`1<<16`), `SNVS_LPCR_SRTC_ENV` (`1<<0`).
Verify against the cm7 header before writing any offset.

### 2. `cores/imxrt1176/rtc.c` (replace the stub with the port)
Port `cores/teensy4/rtc.c` behaviour-verbatim, base coming from the Task-1 defs:
- `rtc_get()` — tear-free double-read of `HPRTCMR/HPRTCLR`, `(hi<<17)|(lo>>15)`.
- `rtc_set(t)` — stop HP (`HPCR &= ~(RTC_EN|HP_TS)`, wait `RTC_EN` clear) → stop LP
  (`LPCR &= ~SRTC_ENV`, wait) → write LP SRTC (`LPSRTCLR=t<<15`, `LPSRTCMR=t>>17`) → start LP
  (`LPCR |= SRTC_ENV`, wait) → start+sync HP (`HPCR |= RTC_EN|HP_TS`).
- `rtc_compensate(int)` — no-op.
- weak `_gettimeofday` — tear-free read into `tv_sec`/`tv_usec` (grep confirms no existing
  definition in the core; `__attribute__((weak))` keeps it safe if a syscalls stub later
  provides one).

### 3. `cores/imxrt1176/startup.c` (per-boot SNVS init — Approach A)
After `set_arm_clock_rt1176()` and before `__libc_init_array()`, add the teensy4
`startup.c:178-185` sequence: `if (!(SNVS_LPCR & SNVS_LPCR_SRTC_ENV)) { LPSRTCLR = 1546300800u<<15;
LPSRTCMR = 1546300800u>>17; LPCR |= SRTC_ENV; }` then `SNVS_HPCR |= RTC_EN | HP_TS`. **SDK-first
check before writing:** confirm SNVS is clocked and the 32.768 kHz oscillator is running by
default on the EVKB (SNVS is always-on, so this is expected — but read the SDK
`snvs_hp_rtc/cm7` `clock_config` / `fsl_snvs_hp.c` `SNVS_HP_RTC_Init` to be sure; add any
required LPCG/oscillator enable if the SDK does one).

## Data flow / behavior

Set: `setTime()`/`Teensy3Clock.set(t)` → `rtc_set(t)` → LP secure counter loaded, HP synced.
Read: `now()`/`Teensy3Clock.get()` → `rtc_get()` → HP counter (advancing at 1 Hz).
Consumer (unchanged): PaulS_SD `SD.cpp::dateTime()` → `Teensy3Clock.get()` → `rtc_get()`
(inline in `core_pins.h`); it treats `now < 315532800` (before 1980) as "no clock" → zero FAT
date. With the clock set, files get real dates; with the teensy4 cold-boot default they get
2019 dates until set. `breakTime`/`makeTime` (core `Time.cpp`) untouched.
Persistence: warm reset resets the HP domain but leaves the battery-backed LP secure counter
running; on reboot `startup.c` sees `LPCR[SRTC_ENV]` already set (skips the default re-init)
and re-syncs HP←LP, so `rtc_get` returns the persisted time.

## QEMU refinement (first-class deliverable, gate-driven)

**Required (confirmed gap): honour `HP_TS`.** Define `HPCR_HP_TS (1u<<16)`. On a `SNVS_HPCR`
write with `HP_TS` set, synchronise the HP counter from the LP secure counter —
`hp_rtc.base = snvs_rtc_now(&s->lp_rtc)` at the current virtual instant — then apply the
`RTC_EN` enable as today; store the register with `HP_TS` auto-cleared (`regs[HPCR/4] = val &
~HPCR_HP_TS`) to mirror the silicon self-clearing bit. Re-evaluate alarm/IRQ as now. This is
what makes a teensy4-style `rtc_set` (and the `startup.c` boot sync) land the correct time in
HP. **Fail-first:** the gate must fail against the unpatched model (HP reads ~0 after set),
then pass once `HP_TS` is honoured.

**Already correct — no change expected:** `reset_hold` already preserves `lp_rtc` + `gpr`
across a machine reset, which is the persistence primitive. Traced end-to-end, warm-reset
persistence works without further model change: on reboot `startup.c` re-runs, but its LP
default-re-init writes are ignored while `lp_rtc` stays enabled, so the persisted counter is
kept and HP re-syncs from it. (`reset_hold` does `memset` the LP-domain *register mirrors*,
which on silicon persist — but this is **not guest-observable** with the teensy4 startup,
which rewrites `LPCR` every boot before any sketch reads it. Treat a `reset_hold`
LP-register-preserve tweak as *optional faithfulness*, applied only if the gate exposes a
real divergence — do not add it speculatively.)

## Testing

**QEMU gate `evkb/rtc_test/`** (copy an existing gate + its `qrun` runner; run under
**`-icount shift=auto`** so the 1 Hz advance and the DWT-based `delay()` are deterministically
coupled — the same lever the PIT / tone / SdWav gates needed). A single self-contained,
self-resetting sketch driven by a boot-phase token in a battery-backed `SNVS_LPGPR` register
(the only in-process way to test warm-reset persistence — a fresh QEMU process is a cold
boot, so the reset must be an in-process machine reset via guest `SYSRESETREQ`):
- **Phase 1** (token absent): `RTC_SET` — `rtc_set(KNOWN_EPOCH)`, read back == `KNOWN_EPOCH`
  → `RTC_SETGET=PASS`. `RTC_TICK` — record `rtc_get()`, `delay(~2 s)`, assert `rtc_get()`
  advanced by ~2 (±1 tolerance), cross-checked against the `micros()` delta →
  `RTC_TICK=PASS`. Write the phase token to `SNVS_LPGPR[0]` and the expected post-reset time,
  then trigger `SYSRESETREQ` (`SCB_AIRCR = VECTKEY | SYSRESETREQ`).
- **Phase 2** (token present after reboot): assert `rtc_get()` ≈ the stored expected time
  (persisted across the machine reset — NOT reverted to the Jan 2019 default) and that the
  `LPGPR` token itself survived → `RTC_PERSIST=PASS`; clear the token; print `RTC_ALL=PASS`
  and stop (no further reset).

**Hardware (final arbiter):** flash via LinkServer; print `rtc_get()`/`now()` twice a few
seconds apart and confirm ~1 Hz advance vs the `micros()` delta; `rtc_set()` a known epoch and
read it back; **warm reset** (LinkServer `run` / reset button — not a power cycle) and confirm
the time **persisted** (HP reloaded from the battery-backed LP counter). Finally, confirm the
**SD/FAT file timestamps are now real** (non-zero, current date) through the PaulS_SD path —
the end-to-end proof the stub replacement didn't regress its one live consumer. (The same
self-resetting gate firmware also works here: `SYSRESETREQ` is a warm reset on silicon too.)

## Risks

- **Boot-path fragility** — adding init to `ResetHandler`/`startup.c` has reset-looped this
  core before ([[rt1176-extmem-malloc]]). Mitigated by the A-is-MMIO reasoning and the proven
  B fallback (lazy first-use init). Watch for a reset loop on first HW flash; if it appears,
  switch to B before debugging further.
- **SNVS clocking / 32 kHz oscillator** — verify via the SDK that SNVS is accessible and the
  RTC oscillator runs by default on the EVKB (expected, since SNVS is always-on). Don't guess
  a clock-gate register; read `fsl_snvs_hp.c` + the board `clock_config`.
- **Base-address / bit-mask correctness** — a wrong SNVS offset silently no-ops (QEMU) or
  faults (HW). Cross-check every def against the cm7 header before writing.
- **Auto-generated header** — SNVS defs must go in **both** `imxrt1176.h` and
  `tools/gen_imxrt1176_h.py`, or a regenerate silently drops them.
- **Circular false-pass** — the gate is built to our own model, so it proves consistency, not
  correctness. HW is the arbiter for the 1 Hz rate, the set/read round-trip, and warm-reset
  persistence; QEMU only proves the register plumbing + the `HP_TS` sync logic.
- **`SYSRESETREQ` reset semantics** — confirm a guest-issued `SYSRESETREQ` in QEMU performs a
  machine reset that invokes the SNVS `reset_hold` (preserving `lp_rtc`/`gpr`) and re-runs
  `ResetHandler` from the reset vector. If it instead resets only the CPU (SNVS untouched),
  persistence would trivially "pass" for the wrong reason — sanity-check by also confirming a
  broken-persistence variant (temporarily clearing `lp_rtc`) would fail.

## References

- Port source: `cores/teensy4/rtc.c` (the whole port) + `cores/teensy4/imxrt.h` lines
  ~8770-8820 (the SNVS register block + bit masks to retarget) + `cores/teensy4/startup.c:178-185`
  (the per-boot LP-default-start + HP-sync sequence).
- Core: `cores/imxrt1176/rtc.c` (the stub to replace), `startup.c` (ResetHandler tail
  ~230-255, where the init goes), `core_pins.h:2948-2950` + `2982-2986` (the frozen
  `rtc_*` signatures + `Teensy3Clock`), `Time.cpp` (`breakTime`/`makeTime`, untouched),
  `imxrt1176.h` (no SNVS defs today) + `tools/gen_imxrt1176_h.py` (extend both).
- SDK (read first, before any register): `_boards/evkbmimxrt1170/driver_examples/snvs/snvs_hp_rtc/cm7/`
  (+ the `snvs_lp_srtc` sibling); `drivers/snvs_hp/fsl_snvs_hp.{c,h}` (`SNVS_HP_RTC_Init` /
  `SetDatetime` / `StartTimer`=`HPCR[RTC_EN]`); `drivers/snvs_lp/fsl_snvs_lp.{c,h}`;
  cm7 header `MIMXRT1176_cm7_COMMON.h` (`SNVS_BASE=0x40C90000`, IRQs 66/67/68). Cross-check
  Zephyr `nxp,imx-snvs-rtc`.
- qemu2: `hw/misc/imxrt_snvs.c` (+ `include/hw/misc/imxrt_snvs.h`) — the model to refine
  (`HP_TS` in the `SNVS_HPCR` write case); `hw/arm/fsl-imxrt1170.c:230-232, 416, 1142-1148`
  (SNVS base/IRQ + instantiation, unchanged).
- Consumer to not regress: PaulS_SD `SD.cpp::dateTime()` → `Teensy3Clock.get()` → `rtc_get()`
  ([[rt1176-sd-usdhc]] installed the stub + the FAT-timestamp path).
- Method / HW: gate-first TDD ([[rt1170-qemu]] `qrun`), LinkServer flash + VCOM
  ([[rt1170-evkb-flashing]], [[macos-serial-capture]]), the hybrid-port precedent
  ([[rt1176-eeprom]] — most recent silicon-vs-QEMU divergence; core runs `.text` from ITCM so
  no `.fastrun` needed here; `arm_dcache_*` is a core no-op).
```
