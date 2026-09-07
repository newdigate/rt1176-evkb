# M2Radio BT: L2CAP reassembly of ACL continuation fragments (NEW-34 piece 5 follow-up)

**Status:** IMPLEMENTED 2026-09-07 (M2Radio `0233d53` pushed + pinned; `[media]` RED→GREEN, host R1–R9, sweep 132 passed / 4 SKIP-of-another-stream / 0 failed, vacuity 43/43, fresh-user verified). Silicon re-run (§3.4, plan Task 7) PENDING. Plan: `docs/superpowers/plans/2026-09-07-bt-acl-reassembly.md`.
**Issue:** NEW-34 "M2Radio BT: reconnect known devices + soak-test connection
resilience (range loss/recovery)" — the defect the piece-5 silicon soak found.
**Depends on:** piece 5 (`bt_tone_test` `M2_BT_SOAK` driver, the `[media]` gate, `hci_peer.py`).

## 1. The defect, in bytes

The 2 h piece-5 soak against the Shokz OpenMove (spec
`2026-09-06-bt-resilience-soak-design.md` §8.1–§8.2) failed its functional
acceptance on ONE class: the ACL link and stored-key authentication complete in
0.3 s and then AVDTP stalls to its 15 s deadline (`a2dp=avdtp_failed`), 105 of 274
attempts, with a rate that grew with the headset's uptime and reset only on a
headset power-cycle. A traced run (`M2_BT_ACL_TRACE`, media excluded) caught one
and it is unambiguous:

```
out AVDTP DISCOVER cmd
 in  acl  hex=12 00 80 00 06 00 01 00 0D 35 03 19 11 0A 00 20 35      (17 bytes, L2CAP length 0x12 = 18 → 22 expected)
 in  acl  hex=03 09 00 09 00                                          ( 5 bytes: the tail)
out acl  hex=07 00 42 1D 01 00 01 00 02 00 03                         (SDP ErrorResponse, 0x0003 invalid syntax)
 ... 15 s of heartbeats, AVRCP completes meanwhile ...
disconnection_complete reason=0x16                                    (ours, at the AVDTP deadline)
```

The two pieces concatenate exactly to the 22-byte ServiceSearchAttributeRequest a
healthy attempt receives in ONE packet (the peer's SDP query of our AudioSource
record, `hci_peer.py` line 641 has the same bytes verbatim from the Mac
reference). So this is genuine on-air fragmentation: the peer's host handed its
controller the PDU in two HCI ACL data packets, first (PB 0b10) then continuation
(PB 0b01). Bluetooth Core, Vol 4 Part E §5.4.2 requires the receiving HOST to
reassemble on the Packet_Boundary flag; this host never has:

- `Hci::onPacket` masks the handle with `0x0FFF` (`hci/Hci.cpp:147`), discarding
  PB/BC before the callback.
- `L2cap::onAcl` (`bt/L2cap.cpp:47`) treats every ACL packet as a complete L2CAP
  PDU: it reads the length/CID header and dispatches the bytes it has.

Consequences: the SDP server parsed a 13-byte payload against an 18-byte length
and answered with an error; the 5-byte tail was parsed as an L2CAP header
(length 0x0903, CID 0x0009) and dropped; and the Shokz, which answers AVDTP
DISCOVER only after its own SDP query completes (BT-3/NEW-9 finding), waited. The
AVCTP/AVRCP exchange completing on the same stalled link (90 of 105 stalls) is
consistent: it needs no SDP answer from us.

Two observations are recorded as OPEN, not explained: the Shokz fragments more
as its uptime grows (the soak's rising rate), and the traced build saw 1 stall in
70 attempts against the same degraded headset that gave the untraced build ~50 %
minutes earlier. Neither changes the fix: fragmentation is legal at any time and
the host must cope.

## 2. Design

### 2.1 Where: L2cap, fed the Packet_Boundary flag (approach A)

Three placements were weighed. **(A) L2cap reassembles**, with `Hci::AclFn`
carrying the PB flag — the layer that owns PDU boundaries, and the layer the host
tests already drive through `onAcl`. (B) Hci reassembles by peeking at the L2CAP
length — keeps L2cap untouched but puts L2CAP knowledge in the HCI layer. (C)
H4Parser — wrong layer. **A is chosen.**

`Hci::AclFn` becomes
`void (*)(void *ctx, uint16_t handle, uint8_t pb, const uint8_t *data, uint16_t len)`
with `handle` still masked to 12 bits and `pb = (raw >> 12) & 0x3`.
`L2cap::onAcl(uint16_t handle, const uint8_t *d, uint16_t len, uint8_t pb = PB_FIRST)`
and `A2dpSource::onAcl(...)` take `pb` as a DEFAULTED trailing argument, so the
~30 existing host-test calls are unchanged in meaning; the four firmware thunks
(`bt_tone_test`, `acid_box`, two in `m2_hci_probe`) and `hci_test` cases 11/13
change. Per the spec, ONLY `0b01` is a continuation; `0b00` (first,
non-flushable), `0b10` (first, flushable) and `0b11` (complete) all begin a PDU —
which keeps a bare `onAcl(0x0001, …)` meaning "a whole PDU".

### 2.2 The mechanism

One reassembly buffer per `L2cap` (one ACL handle): `uint8_t m_rx[RX_MTU + 4]`,
`uint16_t m_rxLen` (bytes held), `m_rxNeed` (L2CAP length + 4, 0 = idle).

- **First packet, complete** (`len >= 4` and `len == l2len + 4`): dispatch
  directly from the caller's buffer — zero copy. Media and every healthy PDU
  cost nothing new; a pending partial PDU, if any, is discarded first (counted).
- **First packet, short** (`len < l2len + 4`, or `len < 4` so the length is not
  yet known): copy into `m_rx`, set `m_rxLen`, compute `m_rxNeed` once ≥ 4 bytes
  are held. If `l2len + 4 > sizeof m_rx`: discard, count.
- **Continuation** (`pb == 0b01`): with nothing pending → discard, count. Else
  append; if it would overflow `m_rxNeed` → discard the whole PDU, count.
  When `m_rxLen == m_rxNeed` → dispatch from `m_rx`, clear.
- **Longer than declared** (`len > l2len + 4` on a first packet): dispatch the
  declared PDU only (today's behaviour tolerates this; keep it).
- `begin()` and `reset()` clear the buffer (a stale partial must not survive a
  reconnect).
- Dispatch = today's body of `onAcl`: trace hook, CID demux, `handleSig` or
  `m_onData`. The trace hook is called **per ACL packet as received, before
  reassembly** — unchanged — because that raw view is the instrument that found
  this defect; `tools/acl-trace-to-btsnoop.py` already tolerates it.

Counters: `uint32_t m_reasmFrags` (continuation fragments consumed into a
delivered PDU), `m_reasmDrops` (partials discarded for any of the reasons
above), accessors `reasmFrags()` / `reasmDrops()`, zeroed by `begin()`.
`bt_tone_test`'s per-second `bt_hci` heartbeat gains ` l2frag=<n> l2fragdrop=<n>`
after `credmin=` (the line no gate greps exactly — checked before relying on
it), so the silicon re-run shows fragments being consumed.

### 2.3 What stays as it is

Outgoing fragmentation: `L2cap::send` never exceeds `MAX_PAYLOAD` (700) which is
below every peer's ACL buffer this tree has seen (`acl_len=1021` locally); the
peer's host reassembles anyway. Not built. The peer's other phases keep sending
whole PDUs (brainstorm decision — see §3). `RX_MTU` (1004) is the bound: a PDU
larger than what we advertised is the peer's fault and is dropped.

## 3. Proof

### 3.1 Host tests (the pure surface, RED first)

`bt/test/l2cap_test.cpp` gains scenario **R1** — the Shokz's exact 22 bytes as
17 + 5 (`pb` 2 then 1) through a data channel → `m_onData` fires ONCE with the
18-byte payload (today: fires with 13 bytes, then the tail is dropped — the RED).
**R2** a continuation with nothing pending is dropped and `reasmDrops()==1`,
nothing dispatched. **R3** a new first packet while a partial is pending
discards the partial (counted) and the new PDU completes normally. **R4** a
declared length above `RX_MTU` is discarded on the first packet. **R5** a
first packet shorter than the 4-byte header (2 bytes, then the rest) still
reassembles. **R6** `reset()` between fragments discards the partial. Each pin
demonstrated RED by a mutant on a scratch copy (frags not counted; continuation
appended without a pending check; `reset()` not clearing).

`hci/test/hci_test.cpp` cases 11 and 13 assert `pb == 2` reaches the callback
for a `0x20`-flagged handle, and a new case asserts `pb == 1` for `0x10`.

### 3.2 The QEMU gate (regression, no count change)

`hci_peer.py`'s `media` phase sends its SDP query as the Shokz did: `acl()`
gains a `pb` parameter, a new `acl_frag(handle, cid, payload, at=13)` returns the
two packets (first with `pb=2`, remainder with `pb=1`; `at` is the PAYLOAD split
so the first packet carries 4 + 13 = 17 bytes), and `rev_maybe_query` sends
both, 20 ms apart. **Always on in the media phase** (brainstorm decision): the
modelled headset now fragments, so every `[media]` run exercises reassembly.
Against the pinned library the gate goes RED by name
(`PEER-SDP-QUERY-UNANSWERED` → "the peer's SDP query of our AudioSource record
was never answered", and `PEER-SDP-SOURCE-RECORD ok` absent) — recorded in the
gate header as the demonstration. GREEN on the fix; `transcript_qemu_media.txt`
re-captured. `gate-vacuity.test.sh` keeps its `[media]` cases (the re-captured
fixture must still pass `green_still_passes_*`). Gate count stays **132**.

### 3.3 Close-out discipline

M2Radio pushed, `evkb.cmake` pin bumped, fresh-user `-DEVKB_FORCE_FETCH=ON`
verified by RUNNING `[media]` against the fetched ELF; every bt-linking gate ELF
rebuilt (`bt_tone_test` ×3 gate dirs + `build-soak-hw`, `m2_hci_probe` ×4,
`acid_box` bench) — an old ELF boots fine, so an unrebuilt sweep passes
vacuously; sweep 132/132; vacuity; `LICENSE-AUDIT` after the sweep.

### 3.4 The silicon claim

Re-run the UNTRACED soak build against the still-degraded Shokz for 30 min: the
`avdtp_failed` class should vanish and `l2frag` should count — that is what
closes piece 5's functional acceptance. QEMU proves reassembly; only the
headset proves the stall was this. Record in the soak spec §8.3 and the
transcript's SOAK section.

## 4. Files

M2Radio: `hci/Hci.h` + `hci/Hci.cpp` (AclFn `pb`), `hci/test/hci_test.cpp`,
`bt/L2cap.h` + `bt/L2cap.cpp` (reassembly + counters), `bt/A2dpSource.h`
(forwarder), `bt/test/l2cap_test.cpp`. evkb:
`examples/audio/bt_tone_test/bt_tone_test.cpp` (thunk + heartbeat fields),
`examples/display/acid_box/acid_box.cpp` (thunk),
`examples/networking/m2_hci_probe/m2_hci_probe.cpp` (two thunks),
`examples/networking/m2_hci_probe/hci_peer.py` (`acl` pb, `acl_frag`, media
query), `examples/audio/bt_tone_test/run_qemu_media.sh` (header: the RED
demonstration) + `transcript_qemu_media.txt`, `evkb.cmake`, this spec, the soak
spec §8.3, `CLAUDE.md`, memory.

## 5. Non-goals

Outgoing fragmentation; fragmenting the peer's lifecycle/reconnect/soak/avdtp
phases; explaining the Shokz's fragmentation-with-uptime or the trace-build
observer effect (both recorded open); any change to SDP, AVDTP or the soak
driver.
