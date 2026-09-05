# M2Radio BT: reconnect a known device (NEW-34, piece 1) — Design

**Date:** 2026-09-05
**Issue:** NEW-34 — "M2Radio BT: reconnect known devices + soak-test connection
resilience (range loss/recovery)". This spec covers the FIRST of that issue's
five pieces; the decomposition and the other four are recorded below so the
boundary is explicit.
**Status:** Design, approved in brainstorming; pending spec review.

## Problem

The A2DP source (`M2Radio/bt`) connects from scratch on every boot: a 12.8 s
inquiry, a name match on `M2_BT_TARGET_NAME`, a page, an SSP pairing from
nothing, encryption, then AVDTP. `BtLink` never keeps the link key: the
`Link_Key_Notification` handler only raises a flag, the `Link_Key_Request`
handler always sends the negative reply, and `connect()` has no path that
pages an address it already knows. Two consequences:

- every boot costs ~20-30 s of bring-up and needs the headset in PAIRING mode
  (a headset that has left pairing mode typically refuses a peer that holds no
  key for it);
- there is nothing for an auto-reconnect (NEW-34 piece 2) to reconnect WITH: a
  headset returning from out-of-range is out of pairing mode by definition.

## Goal

After a device has paired once, a later boot reaches AVDTP STREAMING against it
with **no inquiry and no pairing dance**: page the stored address directly,
answer the controller's `Link_Key_Request` with the stored key, encrypt, then
the unchanged L2CAP/SDP/AVDTP sequence. A stale key (the peer forgot us) is
detected from the authentication status, erased, and replaced by a fresh
pairing in the same attempt. Nothing changes for a build that sets no bond
table.

## Non-goals (and where each goes)

- **Drop detection, media teardown, auto-reconnect** — piece 2. Nothing here
  makes A2dpSource non-blocking; `page()` is the primitive a reconnect state
  machine will call, and the key handlers already live in the event path.
  One note for piece 2: most headsets page the LAST SOURCE themselves when
  they come back into range, and this stack never enables page scan nor
  accepts an incoming connection. Accepting the headset's page is likely the
  faster reconnect; the link-key handler designed here already authorises such
  a link, so nothing here forecloses it.
- **Shokz self-power-off after 1-2 min (suspected missing AVRCP)** — piece 3.
- **Media drops climbing to ~14 % over minutes** — piece 4. The NEW-33 comment
  attributes it to air-link credit starvation; a single lost
  `Number_Of_Completed_Packets` would permanently shrink `L2cap::m_credits`
  and look identical in `bt_hb`. Log the credit floor before believing either.
- **The hours-long soak** (NEW-8 style) — piece 5.
- **NEW-35's on-screen device list / pairing UI** — consumes this table
  (`at(i).name`), adds "forget" over `erase()`.
- Controller-side stored-key commands (`Write_Stored_Link_Key`), encrypting
  keys at rest, Secure Connections (P-256) key handling beyond storing the
  type byte the controller reports, a non-blocking BtLink.

## Decisions taken in brainstorming

1. **Piece 1 first** (of five) — foundation for piece 2, small, QEMU-gateable.
2. **A four-entry bond table**, most-recently-connected first, not a single
   slot: the bench rotates three peers (Shokz OpenMove, OneOdio A70, ESP32
   sink) and NEW-35 wants a list.
3. **Inquiry-plus-pair remains the fallback on EVERY attempt** after the
   bonded pages fail, name-filtered by the target as today. A fresh headset can
   still be paired on the bench with no new knob; a bonded headset returning
   mid-inquiry waits up to ~20 s to be paged again. NEW-35 can later turn the
   inquiry step into a UI action.
4. **Host-side bonds inside BtLink, persisted through an injected EEPROM
   store** (approach A). Rejected: controller-side `Write_Stored_Link_Key`
   (optional in the Core spec, opaque failure on a stale key, whole command
   family to model in the fake peer) and bonds-in-the-examples (duplicated
   store; piece 2 and NEW-35 both need the table inside the stack).

## Facts this design rests on (checked, not assumed)

- The IW416 reports the same `bd_addr=A0:CD:F3:C5:B9:F3` in every transcript
  on record (m2_hci_probe, bt_tone_test, acid_box). Link keys are indexed by
  the pair of addresses, so a key the headset stores against us stays valid
  across our power cycles.
- The imxrt1176 core has EEPROM emulation in the top 256 KB of the 16 MB
  FlexSPI NOR (`imxrt1176/eeprom.c`, `E2END=0x10BB` → 4284 bytes), gated in
  QEMU (`storage-memory/eeprom_test`: `EEPROM_PERSIST=PASS`). `eeprom_write_byte`
  reads before it writes and SKIPS an unchanged byte; each changed byte is one
  journal append; a sector erase happens only when a sector's 2048 entries are
  used up. `LinkServer flash … load` erases only the image's sectors, so the
  region survives reflashing.
- `hci_peer.py` already models Create_Connection (Command Status + Connection
  Complete), Create_Connection_Cancel, Disconnect, `Authentication_Requested`
  → `Link_Key_Request`, the negative reply → IO-capability dance →
  `Link_Key_Notification` (key `bytes(range(16))`, type 4) →
  `Authentication_Complete`, and `Set_Connection_Encryption`. A positive reply
  arm (0x040B) and a rejection arm are what is missing.
- The probe's `[avdtp]` gate asserts `^secure=ok paired_by=ssp` from the
  probe's OWN line (built from `link.pairedBy()`), not from BtLink's log. A
  fresh SSP pairing keeps returning `ssp`; only a bonded authentication
  returns the new value `stored`. No existing gate assertion moves.
- The acid_box `M2_BT_OUT` bench build routes every M2Radio object to FLASH by
  an explicit per-object list in its linker-script edit (Iw416, SdioHost, …,
  BtLink, L2cap, …) because the build has ~1 KB of ITCM left. A new object file
  in `M2Radio/bt` lands in ITCM unless it joins that list.
- `bt/test/run.sh` compiles EVERY `bt/*.cpp` into each host test, so an
  Arduino-dependent `.cpp` in `bt/` would break the host suites; a header-only
  file does not.

## Design

### 1. Data layer: `BondTable` and `BondStoreEeprom`

`M2Radio/bt/BondTable.{h,cpp}` — pure C++11, no heap, no Arduino, host-tested
like L2cap and Avdtp:

```
struct Bond { uint8_t bd[6]; uint8_t key[16]; uint8_t keyType; uint8_t psrm; char name[32]; };  // 56 B
class BondTable {
public:
    static const uint8_t  MAX = 4;
    static const uint16_t IMAGE_SIZE = 4 + 1 + 1 + MAX * sizeof(Bond) + 4;   // 234
    const Bond *find(const uint8_t bd[6]) const;
    void upsert(const Bond &b);        // insert at the front; an existing entry is updated and moved to the front;
                                       // the LAST entry is evicted when full
    void touch(const uint8_t bd[6]);   // move to the front (every successful bonded connect)
    bool erase(const uint8_t bd[6]);
    uint8_t count() const; const Bond &at(uint8_t i) const;   // index 0 = most recent
    uint16_t save(uint8_t *out, uint16_t cap) const;          // serialise; returns the image length (IMAGE_SIZE)
    bool load(const uint8_t *in, uint16_t len);               // false AND an empty table on bad magic/version/length/CRC
    bool dirty() const; void clearDirty();                     // set by upsert/touch/erase; cleared by the store after a write
};
```

- Table order IS the recency order, so the boot policy is "walk from index 0".
- `keyType` is the byte from `Link_Key_Notification` (0 = legacy combination
  key, as the ESP32 sink produces; 4 = unauthenticated P-192 from Just Works,
  as the headsets produce). Logged so the two are distinguishable; not used
  for any decision.
- `psrm` is the Page_Scan_Repetition_Mode we paged with, needed by
  Create_Connection when there has been no inquiry.
- `name` is 31 characters plus NUL, truncated from the inquiry's remote name.
  Both the boot policy's target-name filter and NEW-35's device list need it.
- The clock offset is NOT stored: it goes stale the moment the peer reboots,
  and a page works without it (clock offset 0x0000, valid bit clear).
- Image layout: `"BTBD"` (4) · version 0x01 (1) · count (1) · MAX × 56-byte
  entries (unused entries zero) · CRC-32 (4) over everything before it. Total
  234 bytes. The CRC is the IEEE reflected polynomial (0xEDB88320, init and
  final xor 0xFFFFFFFF; `"123456789"` → `0xCBF43926`), implemented bitwise
  inside `BondTable.cpp`. The loader's `BtFwLoader::crc32` is not reused: a
  bond table should not depend on a firmware loader for ten lines of
  arithmetic, and the bt host suites do not link `BtFwLoader.cpp`.
- Magic plus CRC means a fresh part (0xFF fill), a QEMU run (0x00 fill) or a
  NOR that `eeprom_test` last scribbled over ALL load as an EMPTY table, never
  as a bond. `load()` on a bad image also empties the table, so a caller can
  never keep stale RAM entries by mistake.
- `dirty` is the store's only signal; `save()` (the serialiser) does not clear
  it — the store does, after the EEPROM write.

`M2Radio/bt/BondStoreEeprom.h` — header-only, Arduino-side (the core's
`<avr/eeprom.h>`), included only by hosts:

```
struct BondStoreEeprom {
    static const uint16_t OFFSET = 4000;                       // 4000 + 234 = 4234 <= 4284 (E2END 0x10BB)
    static bool load(BondTable &t);                            // eeprom_read_block -> t.load(); returns t.load()'s verdict
    static bool save(BondTable &t);                            // no-op unless t.dirty(); eeprom_write_block; clearDirty(); true if written
    static void wipe(BondTable &t);                            // bench knob: store the canonical EMPTY image (a later load() reads true, no bonds)
    static const uint16_t END = OFFSET + IMAGE_SIZE;           // 4234: the reserved region is OFFSET..END-1; a second record goes BELOW 4000
};
```

- Header-only so the bt host test glob keeps compiling.
- `load` once after `Hci::begin()`; `save` after EVERY `connect()` return
  (success or failure — an erased stale bond must persist too). Both hosts call
  it at those two points only.
- Cost, CORRECTED in the Task 2 review (2026-09-05): the emulation maps EEPROM
  address `a` to sector `(a>>2) % 63`, so the 234-byte image is spread over 59
  of the 63 sectors, ~4 bytes each. A save that changes nothing writes nothing
  (the emulation skips unchanged bytes); a first pairing on a virgin 0xFF part
  changes every byte (~234 journal appends), on a zeroed/QEMU region ~60; a
  move-to-front changes only the entries that moved. Each changed byte is a
  2-byte journal program with IRQs MASKED (short, per byte), so a changing save
  costs tens of milliseconds of intermittent masking in total. Those 59 journals
  fill in near-lockstep, so their 4 KB sector ERASES (tens of ms each, IRQs
  masked) arrive as a CLUSTER of up to 59, roughly once per ~512 changing saves
  — a pairing or a peer switch is the only thing that changes the image, so
  that is years of bench use, but when it lands it is seconds of masked time.
  Callers therefore keep `load`/`save` in `setup()`/`loop()` context outside
  media streaming (`connect()` returns before media starts) and accept that
  acid_box's always-running SAI ISR can glitch the LOCAL output at a pairing
  event; documented, not engineered around. The per-byte figures are estimates
  from the core's code path, not measurements on this board.
- A failed connect attempt that leaves the table clean costs nothing; BtLink
  dirties the table only on a real change (a notification, a stored-key
  success, an erase), never on a bare failure — otherwise the 5 s retry loop
  would become a flash-write loop.
- Format and offset live in the library, so a bond made in `bt_tone_test` is
  honoured by `acid_box` and vice versa.
- The key is stored in PLAINTEXT in the NOR. Development board, Just Works
  (unauthenticated) key anyway. Recorded here so nobody mistakes it for a
  security design.

### 2. Protocol layer: `BtLink` and `A2dpSource`

**`BtLink::setBonds(BondTable *t)`** is the only new entry point. Null (the
default) means today's behaviour EXACTLY — negative link-key replies, no
bonded paging — which is what keeps the four existing probe gates and both
tone gates byte-identical on the wire.

**`page(const uint8_t bd[6], uint8_t psrm, uint8_t attempts, now, idle)`** is
extracted from the second half of `connect()`: Set_Event_Mask,
Write_Simple_Pairing_Mode, Write_Page_Timeout, then the existing
Create_Connection loop (cancel-a-silent-page with credit reclaim, retry on
Page Timeout 0x04) with the attempt count as a parameter. `connect(name,…)`
becomes inquiry + name choice + `page(hit.bd, hit.psrm, PAGE_ATTEMPTS)`. A pure
refactor: the `[hci]` and `[avdtp]` peers count every command they receive, so
they verify it command-for-command. A bonded page sends clock offset 0x0000
with the valid bit clear. `m_bd`/`m_psrm` are set from the page target either
way (the key handlers below key on `m_bd`).

**Event handlers (all from `onEvent()`, replies via `submit()` only, as today):**

- `Link_Key_Request` (0x17): if a table is set and `find(bd)` hits →
  `Link_Key_Request_Reply` (opcode 0x040B, params bd(6) + key(16); answered by
  Command Complete status + bd) and `m_keyOffered = true`; log
  `link_key_req: bd=… -> reply(stored type=N)`. Otherwise the existing
  negative reply (0x040C) and log line.
- `Link_Key_Notification` (0x18): as today (`m_haveLinkKey = true`, log), plus
  when a table is set: `upsert({bd, key, type, psrm, name})` with `psrm` =
  the mode we paged with when `bd == m_bd`, and `name` = the matching inquiry
  hit's name when this address was in the hit list, else the existing bond's
  name, else empty. Log gains ` bond=saved|updated`.
- `Authentication_Complete` (0x06): captured as today; the ladder reads it.

**`pairAndEncrypt()` — the ladder gains a first rung.** Today: Auth_Requested
→ wait Auth Complete → on failure Write_Simple_Pairing_Mode=0 → Auth_Requested
→ wait → on failure return. New, with `m_keyOffered` cleared before the first
Auth_Requested:

1. Auth_Requested; wait Authentication_Complete (25 s, unchanged).
2. **Success with a key offered and NO new key notified** → a stored-key
   authentication: `m_pairedBy = "stored"`, `bonds->touch(m_bd)`. Success with
   a new key notified → `ssp`/`pin` as today (a fresh pairing on a link we had
   no bond for, or the peer chose to re-pair).
3. **Failure with a key offered and status 0x05 (Authentication Failure) or
   0x06 (PIN or Key Missing)** → the peer no longer holds a matching key:
   `bonds->erase(m_bd)`, log `bond_rejected: status=0x%02X -> erased`, then
   ONE more Auth_Requested (`m_keyOffered` cleared again) with SSP as configured — the peer's next
   Link_Key_Request now gets the negative reply, and the normal SSP dance
   follows. The existing SSP→PIN fallback still applies after that.
4. **Any other failure status with a key offered** (0x08 connection timeout,
   0x22 LMP response timeout, …) is transient: the bond is KEPT and the attempt
   fails as today.
5. Set_Connection_Encryption → Encryption_Change, unchanged.

If the peer tears the ACL down after rejecting the key — real headsets do —
the next command fails with status 0x02 (No Connection), the attempt returns
`PAIRING_FAILED`, `A2dpSource` calls `disconnect()` (harmless with no link),
and the next attempt pairs fresh because the bond is already gone.

`pairedBy()` gains the value `"stored"`. The log line `pairing=… auth=…
link_key=…` keeps its wording; `link_key=stored` there has always meant "a
notification arrived this attempt" and is unchanged.

**`A2dpSource::setBonds(BondTable *t)`** forwards to the link and keeps the
pointer. **`A2dpSource::connect(name, aclNum, now, idle)`** gains the boot
policy in FRONT of the unchanged pairing → L2CAP → SDP → AVDTP sequence:

```
if (bonds) {
    first = true
    for i in 0..count-1:
        b = copy of at(i)                       // copy: page() does not mutate, but the ladder later may
        if (name && name[0] && !strstr(b.name, name)) continue    // target-name filter
        log "bond_try: bd=.. name=\"..\" attempts=N"
        if (link.page(b.bd, b.psrm, first ? PAGE_ATTEMPTS : 1)) == OK → connected; break
        first = false
    if not connected: log "bond_page=none -> inquiry"
}
if not connected: link.connect(name) as today (CONNECT_FAILED on failure)
pairAndEncrypt → L2CAP → SDP → AVDTP, unchanged
```

- Candidates are walked in recency order; the first candidate gets
  `PAGE_ATTEMPTS` (3), each later one a single attempt. Worst case — four
  bonds, nobody home — is ~30 s before the inquiry starts. With no table set,
  today's cycle is unchanged.
- (Added in the Task 4 review.) A bond with an EMPTY stored name is a
  WILDCARD for the target-name filter, never a dead slot: a name can be lost
  legitimately (a bond made by a build with no target name, read by a build
  with one — bench run E does exactly that), and a nameless bond must cost one
  page, not occupy a slot forever. Also: `BtLink` marks a key as OFFERED only
  when the request names the address it paged (`m_bd`); a request for another
  bonded peer is still answered, but the rung's erase can never land on the
  wrong bond.
- A build with no target name (`nullptr`) treats every bond as a candidate.
- A2dpSource stays BLOCKING. Making it a state machine is piece 2's job.

**Build-side:** `BondTable.cpp.obj` joins acid_box's M2Radio flash-routing
list (`*libM2Radio*.a:BondTable.cpp.obj(.text* .fastrun)`), else the bench
build's ITCM overflows. The plan checks `.text.itcm` before and after.

### 3. The QEMU gate: `networking/m2_hci_probe[reconnect]`

One new gate; the sweep goes from 128 to 129. `run_qemu_reconnect.sh` owns
`build-reconnect/`, configured `-DM2_BT_CONNECT=ON
-DM2_BT_TARGET_NAME=FAKE-HEADSET-01 -DM2_BT_RECONNECT=ON` (the last a new
option in the probe's CMakeLists), runs `hci_peer.py reconnect $SOCK` on a
`-serial unix:…,server` socket in `/tmp` (the `sun_path` rule), re-execs under
`gtimeout` via `tools/gate-lib.sh`, and WAITS FOR THE LAST LINE it parses
(`^reconnect=done` or the named failure), per the m2_rx_demo[irq] lesson.

**Firmware (`probeReconnect()` under `M2_BT_RECONNECT`)** replaces the
hand-driven `probeConnect()` with a static `A2dpSource` + `BondTable` +
`BondStoreEeprom`, so the gate exercises the SHIPPED policy rather than a copy.
Three connects in one boot, every one to STREAMING — which also proves L2cap
and Avdtp re-initialise cleanly on a fresh handle three times over, a piece-2
prerequisite:

1. `load()`; print `bonds_boot=0`. `connect("FAKE-HEADSET-01")`: inquiry,
   SSP, key #1 notified, bond saved, `save()`. Keep a copy of the bond's key.
   Print `reconnect_phase=1 result=ok paired_by=ssp`. `link().disconnect()`.
2. **Cold reload with a decoy.** Wipe the RAM table; `load()` (this is the
   EEPROM round trip inside one boot); `upsert(DECOY)` with address
   `AA:BB:CC:DD:EE:99`, name `DECOY`, any key; `save()`; wipe; `load()`. Print
   `bonds_reload=2`. If the first `load()` had failed, the table would hold the
   decoy only and this reads `bonds_reload=1`. `connect("FAKE-HEADSET-01")`:
   the name filter must SKIP the decoy and page the fake headset directly
   (`bond_try: bd=AA:BB:CC:DD:EE:01 …`, no `bond_try` for the decoy), key
   offered, Auth Complete 0x00, encrypt, STREAMING. Print
   `reconnect_phase=2 result=ok paired_by=stored`. Disconnect.
3. `connect(…)` again. The peer rejects the offered key on THIS link
   (Authentication_Complete 0x06). The host erases the bond, pairs fresh,
   receives key #2. Print `reconnect_phase=3 result=ok paired_by=ssp`, then
   `bonds_final=2 key_changed=1` (the fake headset's stored key differs from
   the phase-1 copy; the decoy is still there), then `reconnect=done`; the
   heartbeat continues.

**Peer (`hci_peer.py`, new `reconnect` phase)** extends the avdtp acceptor
with per-link state reset on each Connection Complete, and:

- counts `inquiries`, `create_conns` (and records each target address),
  `key_replies` (0x040B, with the offered key), `neg_replies` (0x040C),
  `iocap_dances`, `notified`;
- answers 0x040B with Command Complete (status 0, bd), then
  Authentication_Complete 0x00 if the key equals the one it last notified for
  that address — or 0x06 ONCE on its THIRD link (`key_rejected=1`);
- notifies key #2 = `bytes(range(16, 32))` on the third link's fresh pairing;
- answers a Create_Connection to the decoy address with Connection Complete
  status 0x04 (Page Timeout) after 100 ms, and raises `PEER-DECOY-PAGED`;
- raises `PEER-KEY-MISMATCH` for a key reply that matches nothing it notified,
  and `PEER-KEY-STALE-REPLAY` for a key reply on the rejected link AFTER the
  rejection;
- writes `PEER-RECONNECT inquiries=1 create_conns=3 key_replies=2 key_ok=1
  key_rejected=1 neg_replies=2 iocap_dances=2 notified=2 started_links=3` when
  it completes (its own line; `PEER-DONE` stays the generic phase footer);
- (added in review) allocates a FRESH handle per link (0x0001, 0x0002, 0x0003)
  and drops any ACL on a stale one with `PEER-ACL-BAD-HANDLE`, resets its link
  state on Disconnect so a stale CID lands on `PEER-ACL-UNKNOWN-CID`, keeps a
  sticky error count in its verdict, answers an authentication with no link
  with `PEER-AUTH-NO-LINK`, and runs under a 60 s deadline that prints
  `PEER-DEADLINE` when it gives up.

**Gate assertions, tripwires FIRST** (the [avdtp] convention: every positive
check downstream of STREAMING fails with the same generic message, so the named
tripwires are what let each RED be identified):

1. none of `PEER-DECOY-PAGED`, `PEER-KEY-MISMATCH`, `PEER-KEY-STALE-REPLAY`,
   `PEER-UNKNOWN-OPCODE`, `PEER-AVDTP-*` in the peer result;
2. the exact `PEER-RECONNECT` tally above (and, among the tripwires,
   `PEER-ACL-BAD-HANDLE`, `PEER-ACL-UNKNOWN-CID`, `PEER-AUTH-NO-LINK`,
   `PEER-DEADLINE`);
3. UART: `^bonds_boot=0`, `^reconnect_phase=1 result=ok paired_by=ssp`,
   `^bonds_reload=2`, `^reconnect_phase=2 result=ok paired_by=stored`,
   `^bond_rejected: status=0x06 -> erased`,
   `^reconnect_phase=3 result=ok paired_by=ssp`, `^bonds_final=2 key_changed=1`,
   `^reconnect=done`;
4. `grep -c '^inquiry=started'` == 1 over the whole capture.

**Demonstrated RED before the gate is trusted** — five mutations, each failing
by its own name, quoted in the script header:

| Mutation | What fails, by name |
|---|---|
| Host always negative-replies (0x040B branch reverted) | `key_replies=0`, phase 2 prints `paired_by=ssp` |
| Store never persists (`save()` skipped, or the store a no-op) | `bonds_reload` reads 0 (dead store) or 1 (phase-1 write lost), never 2, then a second `inquiry=started` |
| Bond kept after rejection (erase reverted) | `PEER-KEY-STALE-REPLAY` on link 3 |
| Target-name filter removed | `PEER-DECOY-PAGED` — the run still reaches phase 2, only the tripwire sees it |
| Codec skips the CRC check | host-suite arm (a corrupted image must load as EMPTY), not the gate |

Budget: one inquiry (~13-15 s) + remote name + three short links ≈ 25-30 s,
inside qrun's 60 s cap with margin; the gate's wait loop is sized to ~50 s.

**Host suites.** New `bt/test/bondtable_test.cpp` (added to `run.sh`'s test
list): CRC-32 known vector; save→load round trip; empty table image;
corrupted byte → `load()` false AND empty; truncated / wrong magic / wrong
version → false and empty; upsert order (most recent at 0); update of an
existing entry moves it to the front; eviction of the fifth (the LAST entry
goes); `touch`; `erase`; `find` miss; dirty set by upsert/touch/erase and
cleared only by `clearDirty()`; 31-character name truncation with NUL.
`bt/test/btlink_test.cpp` gains arms through its scripted `FakeIo`: a bonded
Link_Key_Request → 0x040B with the exact key; unbonded → 0x040C; a
notification upserts with the paged psrm and the inquiry hit's name;
Authentication_Complete 0x06 with a key offered erases the bond and the ladder
re-requests authentication; 0x08 does not erase; `pairedBy()` reads `stored`
after a keyed success.

**Bookkeeping.** `transcript_qemu_reconnect.txt` captured from a green run
(`cp build-reconnect/reconnect.uart …`) as the vacuity fixture;
`tools/gate-vacuity.test.sh` gains THREE negatives, each failing by name -- the
card-absent capture, the fixture with its `bonds_reload=` line stripped, the
fixture with a second `inquiry=started` appended -- and NO green replay: the
gate is peer-driven, and the fake QEMU cannot host the peer, so a green capture
still fails at the peer tally (the same limit `[hci]`, `[baud]` and `[avdtp]`
live with); the CLAUDE.md
gate-count paragraph and a measured sweep line (129 expected); `LICENSE-AUDIT`
run after (never during) the sweep — no new manifest entry is expected, since
`build-avdtp/` needed none. Re-capture the fixture whenever the probe's output
changes (the 2026-08-25 lesson).

### 4. Silicon acceptance and host wiring

**Both hosts** (`audio/bt_tone_test`, `display/acid_box -DM2_BT_OUT=ON`) get
the same six lines: a static `BondTable`; `BondStoreEeprom::load` after
`hci.begin()`; `src.setBonds(&bonds)`; `BondStoreEeprom::save` after every
`connect()` return; `bonds_boot=N` printed at boot and `bonds=N` on the connect
result line; a CMake bench knob `M2_BT_FORGET_BONDS` (default OFF) that calls
`wipe()` at boot and prints `bonds_forgotten=1`. The tone example is the
acceptance host (its output is a tone and a heartbeat); acid_box is the
witness that the bench build still fits and that the UI stays live through a
stored-key connect.

**Five bench runs on `bt_tone_test`**, recorded as a RECONNECT section in
`examples/audio/bt_tone_test/transcript_hw_evkb.txt`, each with its console
capture:

- **A — bond creation.** Shokz in pairing mode. Expect `bonds_boot=0`,
  `inquiry=started`, `paired_by=ssp`, `link_key: … bond=saved`, tone.
- **B — the claim.** Full board POWER CYCLE (not SW4) so the NVM claim is
  honest; Shokz in NORMAL mode, not pairing mode. Expect `bonds_boot=1`, no
  `inquiry=started`, `bond_try: bd=<shokz>`, `link_key_req … reply(stored
  type=4)`, `auth_complete: status=0x00`, `paired_by=stored`,
  `encryption=on`, STREAMING, an audible tone, and the reset-to-`a2dp=ok` time
  (expected single-digit seconds against today's ~20-30).
- **C — the control that makes B un-fakeable.** As B with the bond wiped by
  `M2_BT_FORGET_BONDS`, Shokz still in normal mode. A Shokz in normal mode
  should refuse fresh pairing or not answer inquiry at all, so this run must
  NOT stream (`no_inquiry_hit` or `pair_failed`). If it DOES stream, the Shokz
  accepts pairing at any time, B proves less than hoped, the transcript says
  so, and the log evidence (no inquiry line, `paired_by=stored`, the timing)
  plus run D carry the claim.
- **D — the rejection path, three boots against the ESP32 sink.**
  `tools/esp32-a2dp-sink/esp32-a2dp-sink.ino` gains a `forget` serial command
  (`esp_bt_gap_get_bond_device_list` → `esp_bt_gap_remove_bond_device` for
  each, printing `bonds_cleared=N`). D1: after a legacy-PIN pairing
  (`M2_BT_LEGACY_PIN=ON`), a power cycle reconnects `paired_by=stored` on a
  type-0 combination key. D2: `forget` on the sink, power cycle: expect the
  bonded page to connect, the key offered, `auth_complete: status=0x06` (or
  0x05 — recorded whichever lands), `bond_rejected … erased`, a fresh PIN
  pairing, `bond=saved`, streaming. D3: one more power cycle streams
  `paired_by=stored` on the NEW key.
- **E — recency.** A build with no target name; bonds for both peers with the
  sink most recent; only the Shokz powered. Expect three sink page timeouts
  (`connect=page_timeout attempt=1..3`), then the Shokz paged once and
  streaming `paired_by=stored`; the fall-through latency recorded.

**acid_box witness:** the `M2_BT_OUT` bench build boots with an existing bond,
reaches `paired_by=stored` with the panel live throughout (`idleUi`), and its
`.text.itcm` figure is recorded before and after BondTable joins the
flash-routing list. Appended to `transcript_hw_evkb_bt.txt`.

**Close-out:** M2Radio pushed and the `evkb.cmake` pin bumped;
`-DEVKB_FORCE_FETCH=ON` verified by RUNNING the new gate against the fetched
ELF (a configure proves the subdir resolves; only a gate run proves the
fetched code behaves); the full sweep run once, output captured, reds
dispositioned by name.

## Testing summary

| Layer | Instrument | What it proves |
|---|---|---|
| Codec | `bondtable_test` (host) | image round trip; bad images load EMPTY; recency/eviction/erase semantics |
| Ladder | `btlink_test` arms (host) | keyed reply, negative reply, upsert contents, erase-on-0x05/0x06, keep-on-others |
| Policy + persistence | `m2_hci_probe[reconnect]` (QEMU, 5 REDs) | bonded page without inquiry; EEPROM round trip; name filter; rejection → fresh pairing; three clean re-inits |
| Fixture freshness | `gate-vacuity.test.sh` | the gate fails by name on a stripped capture |
| The claim | bench runs A-E | a real headset OUT of pairing mode streams with no inquiry and no pairing; a forgotten bond is detected and replaced; recency order holds |

## Risks

- **A headset that accepts pairing at any time** makes run C stream and
  weakens B's un-fakeability. Recorded as such; D (the sink's `forget`) and the
  log evidence carry the claim.
- **Peer tears the ACL down on rejection.** Handled: the bond is erased before
  the retry, the attempt fails with No Connection, the next attempt pairs
  fresh.
- **Multipoint headset busy with a phone** may refuse our page or time out.
  Falls through to inquiry; fresh pairing then fails; the 5 s retry loop
  continues. No special handling.
- **Stale `psrm`** (peer changed its page-scan mode) slows the page, does not
  break it.
- **EEPROM writes mask IRQs** per changed byte, and a sector-erase cluster (up
  to 59 erases, seconds) lands roughly once per ~512 changing saves. Only ever
  in `setup()`/`loop()` outside streaming; a glitch on acid_box's LOCAL output
  at a pairing event is possible. Not engineered around; the figures are
  estimates from the core's code path.
- **First-candidate cost:** when the most recent device is absent, its three
  attempts add ~15 s before the next candidate. Accepted with decision 3.
- **ITCM in the acid_box bench build:** ~1 KB left; mitigated by the
  flash-routing list, verified by the `.text.itcm` figure.
- **Gate timing under sweep load:** the three-link run sits at ~half of qrun's
  60 s cap; if it joins the load-sensitivity class, it is dispositioned like
  the others (passes idle), not weakened.

## File-by-file

M2Radio (`~/Development/M2Radio`):
- `bt/BondTable.h`, `bt/BondTable.cpp` — new (table + codec + CRC-32).
- `bt/BondStoreEeprom.h` — new, header-only.
- `bt/BtLink.h`, `bt/BtLink.cpp` — `setBonds`, `page()` extraction, the three
  handlers, the ladder's first rung, `pairedBy()` → `stored`.
- `bt/A2dpSource.h`, `bt/A2dpSource.cpp` — `setBonds`, the candidate walk.
- `bt/test/bondtable_test.cpp` — new; `bt/test/btlink_test.cpp` — new arms;
  `bt/test/run.sh` — test list.

evkb (`~/Development/rt1170/evkb`):
- `examples/networking/m2_hci_probe/CMakeLists.txt` (`M2_BT_RECONNECT`
  option), `m2_hci_probe.cpp` (`probeReconnect()`), `hci_peer.py` (`reconnect`
  phase), `run_qemu_reconnect.sh` (new), `transcript_qemu_reconnect.txt` (new).
- `examples/audio/bt_tone_test/bt_tone_test.cpp`, `CMakeLists.txt`
  (`M2_BT_FORGET_BONDS`), `transcript_hw_evkb.txt` (RECONNECT section).
- `examples/display/acid_box/acid_box.cpp`, `CMakeLists.txt` (knob +
  `BondTable.cpp.obj` in the flash-routing list), `transcript_hw_evkb_bt.txt`.
- `tools/esp32-a2dp-sink/esp32-a2dp-sink.ino` (`forget`).
- `tools/gate-vacuity.test.sh` (three cases).
- `CLAUDE.md` (gate count + measured sweep line), `evkb.cmake` (M2Radio pin).
