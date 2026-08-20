# W15: interrupt-driven SDIO service (DAT1) — design

Date: 2026-08-20. Follows W14 (QEMU IW416 model + two regression gates;
sweep 101/0/0). Driver M2Radio `7aec26b`; model qemu2 `0056068fae`.

## Why this is next

The driver services the card by **polling `HOST_INT_STATUS` at ~1 kHz**
(`serviceLink()`'s quiet path runs `delay(1)` per pass). Measured
consequences:

* ~1000 CMD52 reads per second while completely idle — pure bus and CPU
  burn on a link doing nothing.
* It is the reason W11's bitmap-cache optimisation regressed throughput
  2.5×: the read-per-frame pattern was accidentally *pacing* the host to
  the firmware's batchy free cadence, and removing it produced
  burst-then-dry-wait against `delay(1)` granularity. **Any future
  throughput work needs a real completion signal first** — that is the
  structural argument for doing DAT1 before aggregation.

SDIO cards signal exactly this over **DAT1** (the SDIO card interrupt).
The IW416 supports it; the RT1176's uSDHC reports it as
`SDHC_NIS_CARDINT`.

## The blocker, found before committing to this

**QEMU's SDHCI model never raises the card interrupt.** `SDHC_NIS_CARDINT`
(`hw/sd/sdhci-internal.h:153`) occurs exactly once in `hw/sd/sdhci.c` — at
line 1352, in the *write* handler that CLEARS it. There is no path by which
a card asserts it and no plumbing to the controller's IRQ line.

So DAT1 cannot be developed or gated in emulation as things stand, and W15
would otherwise be silicon-only — hand-run soaks again, which is exactly
what W14 existed to end. **Phase 1 closes that gap.**

## Phase 1 — card-interrupt plumbing (qemu2)

1. **SD bus**: a way for a card to assert/deassert its interrupt. QEMU's
   `SDBus`/`SDCardClass` has no such signal today; add the minimal one
   (e.g. an `sdbus_set_irq`-style callback the card calls, mirroring how
   `sdbus_set_inserted` already flows card state up to the controller).
2. **SDHCI**: on card assert, set `SDHC_NIS_CARDINT` in `norintsts` when
   `SDHC_NISEN_CARDINT` is enabled, and raise the controller IRQ; honour
   the existing clear semantics at line 1352 (which are already written and
   currently unreachable). Card interrupts are level-sensitive in SDIO —
   the bit re-asserts while the card still has work — so model that, not an
   edge.
3. **IW416 model**: assert the card interrupt whenever it queues an upload
   or a command reply, i.e. alongside the `HOST_INT_UP_LD` /
   `CMD_PORT_UPLD` bits it already sets. Respect the existing
   `suppress-updl` property — with it on, the card must ALSO withhold the
   card interrupt, or the W12 regression gate stops testing what it says
   it tests.
4. Existing gates must be byte-identical: the RT1062/other machines and the
   SD **memory** card path must be untouched, and the IW416 model stays
   opt-in.

## Phase 2 — driver: interrupt-driven service (M2Radio)

Only after Phase 1 lands, because Phase 2 is untestable without it.

* Enable the card interrupt at the card (`HOST_INT_MASK_REG` already
  written in `begin()`) and at the uSDHC, and attach a handler.
* `serviceLink()` gains an interrupt-driven entry: when the ISR says the
  card has work, service immediately; when idle, **stop polling at 1 kHz**.
* **Keep the ring safety net.** W13 established that this firmware does not
  always raise `HOST_INT_UP_LD` for a queued upload (three host-side
  explanations refuted; `rxStrandedRecovered()` still climbs ~3 per 80
  blasts). An interrupt-driven design that trusts the interrupt completely
  would reintroduce exactly the class of fault W12 fixed. The net stays,
  and its counter stays the health signal.
* The polled path must remain available and correct — silicon is the
  arbiter, and a fallback that has rotted is worse than none.

## Gates

* A new gate on `m2_rx_demo` asserting frames arrive **without** the host
  polling: assert `cmd52PollsSvc()` per received frame drops sharply versus
  the polled build. That is the un-fakeable measurement — the counter
  already exists (W11) and cannot be satisfied by a driver that merely
  polls faster.
* The two W14 regression gates must stay green, and `[stranded]` must
  still fail against a driver with the safety net removed — verify that
  demonstration still reproduces, since Phase 2 touches the same path.

## Success criteria

1. Idle CMD52 rate falls by an order of magnitude (measured, both in QEMU
   and on silicon via `tools/wifi-soak.sh`'s counters).
2. Throughput no worse than the current 3-14 Mbps band, measured with the
   same peer, and `stranded`/`resyncs` no worse.
3. Sweep green with the new gate; the W14 regression gates still
   demonstrated failing against re-broken drivers.

## Risk, stated plainly

The RT1176 uSDHC's card-interrupt behaviour on real silicon is **not
something this project has ever exercised**, and QEMU will now model it
from the specification rather than from measurement — the inverse of every
other behaviour in the IW416 model, which was anchored to captured hardware
output. **A green gate here proves the driver matches the spec as I read
it, not that it matches the card.** Silicon verification is therefore not
optional for W15, and if the two disagree, silicon wins and the model gets
corrected — never the reverse.
