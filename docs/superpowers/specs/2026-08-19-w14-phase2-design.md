# W14 Phase 2: model the data path, then gate the two bugs that cost us days

Date: 2026-08-19. Follows Phase 1 (enumeration + firmware download modelled;
`hw/sd/iw416-sdio.c` in qemu2, opt-in via `-machine mimxrt1170-evk,m2-wifi=on`).

## The point

Phase 1 proves the card enumerates. It cannot catch a single one of the bugs
this subsystem has actually produced, because all of them live in the data
path. Phase 2 exists to make these two regressions **impossible to reintroduce
unnoticed**:

* **W8 — the 32-port ring bug.** The host read 16-bit bitmaps against a
  32-slot ring, so every managed-mode upload landing on slots 16-31 was
  invisible. Cost: days, and it masked two further faults behind it.
* **W12 — stranded uploads.** `HOST_INT_STATUS` is clear-on-read; a
  non-draining path consumed the upload bit and the firmware never re-raises
  for an already-pending upload, so the frame was stranded forever and RX was
  dead until reflash. Cost: three wrong theories and two days.

A model that only does the happy path would pass in both cases. So the design
rule from Phase 1 hardens here: **the model encodes the firmware's contract
including its hostile behaviours, and those behaviours are selectable so a
gate can demand them.**

## What Phase 2 models

### 1. `HOST_INT_STATUS` clear-on-read (0x0C)

Configured clear-on-read by the driver's `begin()` (`HOST_INT_RSR_REG` ← 0xFF).
The model must implement exactly that: a read returns the bits and atomically
clears them. This single behaviour is what made the W12 bug possible; without
it the model cannot express the bug at all.

`HOST_INT_UP_LD` (0x01) = data upload waiting. `CMD_PORT_UPLD` (1<<6) = a
command/event reply waiting.

### 2. The 32-slot data rings

* `RD_BITMAP_L/U/1L/1U` (0x10-0x13) — 32-bit upload bitmap. A set bit means
  the card has placed a frame at that slot.
* `WR_BITMAP_L/U/1L/1U` (0x14-0x17) — 32-bit TX-credit bitmap.
* `RD_LEN_P0_L/U` (0x18/0x19) **+ (port << 1)** — per-slot length, 32 pairs
  through 0x56/0x57. Verified in Phase 1's investigation and matching NXP's
  `SDIO_RD_LEN_P0_L + (port << 1)`.
* CMD53 to `ioport | slot` reads that slot's frame and clears its bitmap bit.
* TX: CMD53 writes consume a credit; **the card frees credits LAZILY IN A
  BATCH**, which is the W11-measured behaviour (NXP's own bits stayed consumed
  through 20+ idle seconds). A model that frees each credit immediately would
  hide the pacing property that W11 proved is load-bearing.

### 3. Command/event port

Enough to let `Iw416::begin()`-through-`connectStation()` proceed: accept a
host command written to `ioport | CMD_PORT_SLCT`, publish the reply length in
`CMD_RD_LEN_0/1`, raise `CMD_PORT_UPLD`, and return a well-formed response
echoing the command id with `HOSTCMD_RET_BIT` and the request's `seq_num`
(low byte only — the high byte is bss_num/bss_type, per W10's finding).
Minimum command set: `CMD_FUNC_INIT`, `CMD_GET_HW_SPEC` (must yield a MAC
address), `CMD_MAC_CONTROL`. Events are delivered through the same port.

### 4. Frame injection — the test control surface

A test needs to make the card deliver a frame. Preferred mechanism: a QEMU
chardev or a small property-driven generator, so a gate can say "inject N
frames of size S onto slots X..Y". Whatever the mechanism, it must be able
to target **specific slots**, because that is what the W8 regression gate
needs.

### 5. The hostile behaviours, each selectable and each OFF by default

| Property | Behaviour | Proven by |
|---|---|---|
| `stale-rd-bits` | set a bitmap bit with `RD_LEN` 0 — a set bit does NOT imply a waiting packet | W12: ~6100 resyncs found no data |
| `suppress-updl` | queue an upload **without** raising `HOST_INT_UP_LD` | W13: `stranded` survived with all readers verified accumulating |
| `lazy-tx-free` | batch TX credit frees rather than per-write | W11: the reverted cache regressed 2.5x because read-per-frame was pacing the host |

Default off keeps ordinary gates simple; a regression gate turns the relevant
one on deliberately.

## The two regression gates (the deliverable)

**Gate A — W8 ring coverage.** Inject frames onto slots **16-31** and assert
the host receives every one. A driver that reads only 16-bit bitmaps, or that
mis-derives the per-slot length address, reads zero frames and the gate fails.
This is the bug that was invisible on hardware for days.

**Gate B — W12 stranded-upload coverage.** With `suppress-updl` on (or by
raising the upload bit during a window where the driver is polling for a
command reply, so the command-wait poll consumes it), queue a frame and assert
the host still delivers it. A driver without the sticky-accumulator + ring
safety net strands it forever and the gate fails. **This gate must also assert
the recovery counter**: `rxStrandedRecovered()` non-zero proves the net did
the work, which is a stronger assertion than "the frame arrived".

Both gates must be demonstrated to FAIL against a driver without the fix —
that demonstration is the deliverable, not the passing run. A regression gate
never shown to fail is decoration.

## Explicitly out of scope

Association, WPA2, the supplicant, PS, and anything requiring the real
firmware blob (which is NXP-licensed and never in this repo, so no gate may
depend on it). Phase 2 stops at "frames move both directions with the
firmware's real ring and interrupt semantics".

## Risk

The honest one: **a model written by the same reasoning that wrote the driver
can share the driver's misconceptions**, and then both agree and the gate
proves nothing. Mitigation is that every modelled behaviour above is anchored
to a measured silicon fact with the observation cited, not to what the driver
expects. Where no measurement exists, the model must say so in a comment
rather than guessing quietly — Phase 1 set that precedent with its "where the
model could be lying" list, and that list is now part of the deliverable.
