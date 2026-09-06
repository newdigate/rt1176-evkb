# M2Radio BT: link lifecycle -- drop, teardown, auto-reconnect, resume (NEW-34 piece 2)

**Status:** IMPLEMENTED and QEMU-gated 2026-09-06 (sweep 130/130, 0 SKIP; the ONE
new gate is `audio/bt_tone_test[lifecycle]`; M2Radio `82e9172` pushed + pinned,
fresh-user verified). The silicon bench matrix (§10, plan Task 15) is PENDING the
bench. Three library bugs were caught during implementation (the [lifecycle] gate
and reviews) and fixed with RED-demonstrated regressions: `Avdtp::reset()` left
channel bindings stale (inbound re-adoption); `A2dpSource::start()` did not ack a
stale `LINK_LOST` (first reconnect after a disconnect aborted); `A2dpSource` never
reset the SBC config for an outbound attempt (a reconnect after an inbound stream
encoded the adopted bitpool instead of the initiator default). One accepted gate
gap: the app-level `btout.end()`-on-drop omission is not observable because
`A2dpSource::tick()` resets L2cap/Avdtp on loss before the callback fires, so five
of the gate's six RED demonstrations fail by name and the sixth is documented; the
library-level teardown IS covered (`PEER-ACL-BAD-HANDLE`, proven live).
**Issue:** NEW-34 "M2Radio BT: reconnect known devices + soak-test connection
resilience (range loss/recovery)", piece 2 of 5 (the decomposition is recorded in
the NEW-34 comment of 2026-09-05 and in the piece-1 spec,
`2026-09-05-bt-known-device-reconnect-design.md`).
**Depends on:** piece 1 (bond table + stored-key reconnect; M2Radio `63101a6`,
evkb master `6466627`), whose silicon runs A-E remain PENDING and fold into this
piece's bench session.

## 1. Goal

A2DP streaming that survives the link: when the ACL drops (range loss, headset
powered off, supervision timeout), the source tears the media path down
cleanly, keeps the UI and local audio alive, and gets the stream back without a
reboot -- either because the headset pages us again or because we page it --
and reports what happened in numbers a soak can assert on.

### Decisions taken in the brainstorm (each with the alternative it beat)

1. **Both directions reconnect** (chosen over "we page only" and over a hybrid):
   the source pages the lost peer AND accepts the headset's own incoming page.
   Accepting means enabling page scan, answering Connection_Request, and --
   because headsets typically drive AVDTP after they connect -- a full AVDTP
   acceptor role. This is the largest single addition in the piece.
2. **Page scan is on whenever no link is up and at least one bond exists**,
   boot included (over "only after a drop" and over "always, pair strangers").
   Off while a link is up: the stack is single-link. Unknown addresses are
   rejected in every state.
3. **After a drop we page the lost peer only, one attempt per cycle, forever**
   (over "fall back to the boot walk + inquiry after two minutes" and over
   "give up after ten minutes"). A reboot restores the boot policy; NEW-35's UI
   will offer "connect to X".
4. **Approach B: the whole link layer becomes non-blocking** (over A: a session
   machine reusing the blocking primitives; and over C: lifecycle in the
   sketches). Every wait in BtLink/A2dpSource becomes a state with a deadline,
   advanced by one `tick(now)` per loop pass. Cost accepted knowingly: piece 1's
   just-reviewed `page()`/`connect()` are rewritten, both examples and the
   `[media]`/`[reconnect]` gate transcripts move, and the host suites are
   re-shaped. Benefit: every wait is an observable state a gate can assert, and
   the NEW-33 `idleUi` plumbing is no longer needed below the boot download.

## 2. Architecture

Four layers, one responsibility each, all in M2Radio (`bt/`), Arduino-free,
no heap:

| Layer | Owns | Never does |
|---|---|---|
| `BtLink` | one HCI **operation** at a time (PREPARE, INQUIRY, PAGE, PAIR, DISCONNECT) + the page-scan side channel + the event sink + the incoming-page decision + link state | policy, timers between operations |
| `A2dpSource` | one **attempt** in either direction: LINKING -> PAIRING -> L2CAP -> SDP -> AVDTP -> STREAMING, or FAILED/LOST with teardown | choosing what to try next |
| `Avdtp` | initiator (as today) **and acceptor** on one signalling channel; media-channel adoption | L2CAP channel policy |
| `BtSession` (new) | the **policy**: boot walk + inquiry, lost-peer retry cadence, page-scan wanted, retry cancel on incoming, MANUAL mode, callbacks, stats | EEPROM, printing, audio |

`Hci` is unchanged: `submit(opcode, params, plen, DoneFn, ctx)` is already
asynchronous and its command timeout covers a controller that never answers.
`Hci::run()` (blocking) stays for boot-time identity/reset commands and probes.
`BtFwLoader::run()` stays blocking (it runs once, before any session exists).

Every layer exposes `tick(uint32_t nowMs)`; the session ticks the attempt,
the attempt ticks the link. The apps call `session.tick(millis())` from
`loop()` and never block on Bluetooth again.

## 3. BtLink: the operation engine

### 3.1 Commands and waits
A command is issued with `Hci::submit()`; the done-callback stores
`{err, status, statusEvent}` into a single pending-command slot and clears
`m_cmdPending`. A state that waits for a command simply returns while the slot
is pending. Only **event** waits carry BtLink deadlines (`m_deadline`, compared
with `(int32_t)(now - m_deadline) >= 0`). Each blocking `while (!flag && now()-t0 < T) idle();`
in today's code becomes one sub-state with `T` as its deadline.

### 3.2 Operations (`startX()` returns immediately; `busy()`, `done()`, `result()`)
- **PREPARE** (`startPrepare()`): Set_Event_Mask(all) -> Write_SSP_Mode(legacy
  ? 0 : 1) -> Write_Page_Timeout(0x2000). Once per session start (and after any
  HCI reset), not per page as today.
- **INQUIRY** (`startInquiry(nameSubstr)`): Inquiry (as today, 12.8 s, deadline
  15 s for Inquiry_Complete) -> one Remote_Name_Request per unnamed hit
  (5 s each) -> `target()` = first hit whose name contains the substring (or the
  first hit when no name). Result `OK` / `NO_INQUIRY_HIT`. It does NOT chain a
  page; the attempt does.
- **PAGE** (`startPage(bd, psrm, clk, clkValid, name, attempts)`): per attempt
  Create_Connection (pkt types 0xCC18, no role switch) -> Command_Status ->
  wait Connection_Complete for `bd` (10 s) -> status 0 = `OK` (handle latched);
  0x04 with attempts left = next attempt; other status = `CONNECT_STATUS`;
  no event by the deadline = Create_Connection_Cancel -> its CC -> up to 1 s
  more for the racing Connection_Complete (status 0 = `OK` "raced the cancel")
  -> next attempt or `TIMEOUT`. Log lines identical to today's.
- **PAIR** (`startPair(inbound)`): the piece-1 ladder, each wait a sub-state:
  Authentication_Requested -> wait Authentication_Complete (25 s) -> stored-key
  rung (`OK` -> `pairedBy="stored"` + `touch()`; 0x05/0x06 -> `erase()` +
  `bond_rejected:` line + one fresh Authentication_Requested; other -> 
  `PAIRING_FAILED`, bond kept) -> SSP failure -> Write_SSP_Mode(0) + PIN rung ->
  Set_Connection_Encryption -> wait Encryption_Change (10 s) -> `OK` /
  `ENCRYPTION_FAILED`. **Inbound variant**: first sub-state WAIT_PEER_SECURE
  (deadline `ENC_WAIT_MS`, default 2000): the master normally authenticates us
  (our Link_Key_Request handler answers with the stored key) and enables
  encryption; Encryption_Change(on) ends the op `OK` with `pairedBy` =
  "stored" if a key was offered for this address, else "peer". If the deadline
  passes unencrypted, the ordinary ladder runs from our side.
- **DISCONNECT** (`startDisconnect()`): Disconnect(0x13) -> Command_Status ->
  wait Disconnection_Complete (3 s) -> `OK` / `TIMEOUT`; handle cleared either
  way. No handle = immediate `OK`.
- **Page-scan side channel** (`wantPageScan(bool)`): `tick` issues
  Write_Scan_Enable(0x02 / 0x00) whenever the wanted value differs from the
  last acknowledged one and no command is pending. Logs `page_scan=on|off`.
  It may interleave with any operation (it is a controller setting, and
  Hci's queue orders it).
- **Supervision knob** (`setSupervisionSlots(uint16_t)`, default 0 = leave the
  controller default of 20 s): after a link comes UP on which we are master,
  Write_Link_Supervision_Timeout(handle, slots); logged `supervision=0x%04X st=`.
  Skipped when slave (the command is master-only). A bench knob for range-loss
  detection time; piece 4 owns RF-marginal behaviour.

### 3.3 Link state and events
`LinkState { NONE, UP, SECURE, LOST }`, `handle()`, `peer()`, `incoming()`,
`role()` (MASTER when we paged, SLAVE when we accepted, updated by Role_Change),
`lostReason()`, `ackLost()`.

`onEvent()` remains the single event sink (called from `Hci::service()`; sending
a command from inside it is safe -- the piece-1 link-key reply already does):
- **Connection_Request (0x04)**, bd(6) cod(3) link_type(1) -- decided
  synchronously:
  - link_type != ACL (0x01): reject 0x0F.
  - a link UP/SECURE: reject 0x0D (limited resources).
  - a PAGE in flight: accept only if `bd == m_bd` (both sides page each other
    right after a drop; the Connection_Complete then satisfies the page wait);
    anyone else 0x0D.
  - idle: accept iff `bonds && bonds->find(bd)`; else reject 0x0F (unacceptable
    BD_ADDR). Never pair a stranger from an incoming page.
  - Accept = Accept_Connection_Request(bd, role **0x01 remain slave**); sets
    `m_bd`, `m_incoming = true`, `m_pageName` from the bond, clears the per-link
    key flags. Logs `conn_req: bd=.. -> accept` / `-> reject(0x0F unknown)` /
    `-> reject(0x0D busy)`.
- **Connection_Complete (0x03)** -- now ADDRESS-CHECKED (closes a piece-1
  deferral): only an event for `m_bd` counts. Status 0 latches the handle,
  LinkState UP, and either completes the PAGE op or (no page in flight) raises
  `inboundUp()` for the session. Status 0x0B (ACL already exists) for `m_bd`
  with a link already UP is ignored. Any other address is logged
  (`connection_complete: bd=.. ignored`) and ignored.
- **Disconnection_Complete (0x05)** for our handle: reason recorded, `m_handle = 0`
  at once (today it is kept), `m_encrypted = false`, LinkState LOST; completes a
  DISCONNECT op if one is in flight, otherwise stays LOST until `ackLost()`.
- **Encryption_Change (0x08)**: as today, plus LinkState SECURE when enabled.
- **Role_Change (0x12)**: records `m_role`; logs `role=master|slave`.
- **Link_Key_Request / IO_Capability_Request / User_Confirmation_Request /
  PIN_Code_Request / Link_Key_Notification / Simple_Pairing_Complete /
  Inquiry_Result / Inquiry_Complete / Remote_Name_Request_Complete**: as today.
  `m_keyOffered` and `m_haveLinkKey` are set only when the event's address is
  `m_bd` (closes the second piece-1 deferral); a key request for another bonded
  address is still answered.

### 3.4 Piece-1 deferrals closed here
address-checked Connection_Complete; address-scoped `m_haveLinkKey`; every
failed attempt runs an explicit DISCONNECTING state (the "CONNECT_FAILED
disconnect is unpinned" note); `bonds_forgotten=` prints the pre-wipe count.

## 4. A2dpSource: one attempt

### 4.1 Targets
```
struct Target { enum Kind : uint8_t { PAGE, INQUIRY, INBOUND } kind;
                uint8_t bd[6]; uint8_t psrm; char name[BondTable::NAME_LEN + 1];
                uint8_t attempts; const char *nameFilter; };
bool start(const Target &t, uint8_t aclNum);   // false if busy
void tick(uint32_t now);
void stop();                                    // -> DISCONNECTING -> FAILED(STOPPED)
```
### 4.2 States
`IDLE -> LINKING -> PAIRING -> L2CAP -> SDP -> AVDTP_WAIT (inbound only) ->
AVDTP -> STREAMING`; failure -> `DISCONNECTING -> FAILED(result)`; a link loss
in ANY state -> teardown -> `LOST(reason)`. `Result` keeps today's values
(`OK, CONNECT_FAILED, PAIR_FAILED, L2CAP_FAILED, AVDTP_FAILED`) plus `LOST` and
`STOPPED`.
- LINKING: PAGE -> `link.startPage(...)`; INQUIRY -> `link.startInquiry(filter)`
  then `startPage(target(), PAGE_ATTEMPTS)`; INBOUND -> skip (the link is UP).
- PAIRING: `link.startPair(inbound)`.
- L2CAP: `l2.begin(handle, aclNum)`, `acceptIncoming(true)`, allow-list
  {SDP 0x0001, AVDTP 0x0019}, `onData`.
- SDP (outbound only): our AudioSink PDL query as today, 5 s + 5 s,
  informational. Skipped inbound: the headset is driving and its own reverse
  query of our AudioSource record is served by `SdpServer` in both directions.
- AVDTP_WAIT (inbound only): deadline `AVDTP_WAIT_MS` (default 2000) for the
  peer to open a signalling channel on PSM 0x0019. Opened -> Avdtp acceptor
  role. Expired -> AVDTP as initiator.
- AVDTP: outbound as today (connect 0x0041, `avdtp.start(want)`, 15 s to
  STREAMING); acceptor: STREAMING when the peer's START is accepted or our own
  self-START (section 5) is accepted.
- Teardown (on LOST, on stop(), on any failure after L2CAP): `avdtp.reset()`,
  `l2.reset()`, media pointers cleared; the audio node is ended by the app
  through the session callback, never by the library.
### 4.3 Config adoption
`sbcParams()` returns the negotiated config in either role: initiator = our
`want` (44.1 kHz joint stereo 16/8 loudness, bitpool 53, as today); acceptor =
the peer's SET_CONFIGURATION choice mapped from `Avdtp::SbcConfig` to
`Sbc::Params` (rate -> `Sbc::Rate`, mode bit -> `Sbc::Mode`, alloc, blocks,
subbands, bitpool = the config's max). `configChanged()` is set on every
(re)configuration and consumed by the app, which restarts the audio node.

## 5. Avdtp: the acceptor beside the initiator

`role() { NONE, INITIATOR, ACCEPTOR }`; `reset()`; `isSignalling(ch)`;
`adoptInbound(L2cap&)` scanning the L2cap inbound iterator each tick.

**Our SEP**: SEID 1, audio, source, in-use = state >= CONFIGURED. Capabilities:
media transport (0x01); media codec (0x07) audio/SBC: **44100 Hz only** (the
audio graph runs at 44.1 kHz), channel modes mono/dual/stereo/joint, blocks
4/8/12/16, subbands 4/8, allocation SNR+loudness, bitpool 2..53; delay reporting
(0x08).

Per signal from the peer (COMMAND type), answered in `service()` with the
existing retry-while-TXQ-full pattern:
- 0x01 DISCOVER -> the one SEP (as today).
- 0x02 GET_CAPABILITIES / 0x0C GET_ALL_CAPABILITIES -> the table above for SEID
  1; another SEID -> REJECT `BAD_ACP_SEID` 0x12.
- 0x03 SET_CONFIGURATION [acp][int][categories] -> validated: acp == 1, media
  transport present, media codec = audio/SBC with exactly one choice per field,
  each inside our capabilities; accepted -> role ACCEPTOR, state CONFIGURED,
  config stored (section 4.3); rejected -> REJECT with the failing category byte
  and `UNSUPPORTED_CONFIGURATION` 0x29 (`BAD_MEDIA_TRANSPORT_FORMAT` 0x23 when
  transport is missing).
  **Collision rule**: peer SET_CONFIGURATION while our own SET_CONFIGURATION is
  already sent (initiator state CONFIGURING or later) -> REJECT `BAD_STATE`
  0x31; arriving while we are DISCOVERING/GETTING_CAPS -> it wins: our
  initiator is abandoned (its pending response ignored) and the acceptor path
  continues.
- 0x04 GET_CONFIGURATION -> the stored config; unconfigured -> `BAD_STATE`.
- 0x05 RECONFIGURE -> allowed only in OPEN (not STREAMING -> `BAD_STATE`);
  same validation; accepted -> config replaced, `configChanged()`.
- 0x06 OPEN -> ACCEPT -> OPENING; the peer's next inbound AVDTP channel is
  adopted as media -> OPEN.
- 0x07 START -> ACCEPT -> STREAMING (from OPEN or SUSPENDED).
  **Self-START**: if no START arrives within `START_WAIT_MS` (1000) of the
  media channel opening, we send START (either side may) -> STARTING ->
  STREAMING on its ACCEPT.
- 0x09 SUSPEND -> ACCEPT -> SUSPENDED (`started()` false; the node pauses).
- 0x08 CLOSE / 0x0A ABORT -> ACCEPT -> the SEP returns to idle, the media
  channel is released (the session reports `onStream(false, 0x00)` so the app
  ends the node), and the attempt returns to AVDTP_WAIT with its 2 s deadline
  (the peer may re-open; otherwise we initiate).
- 0x0D DelayReport -> ACCEPT (as today). Anything else -> General Reject.

Media-channel adoption rule: the first inbound AVDTP channel is signalling (when
we have none), the next one while OPENING is media; any further one is left
unadopted (and logged).

## 6. L2cap and the audio node

**L2cap**: `allowPsm(psm)` (up to two; an inbound CONN_REQ for any other PSM is
answered result 0x0002 PSM not supported -- AVCTP from a headset lands here);
`reset()` (== `begin(0, 0)`, every channel FREE, queue emptied);
`nextInbound(psm, const Channel *after)` iterator over `peerInitiated` OPEN
channels; `creditsMin()` with `resetCreditsMin()` (the minimum credit seen
since the last reset -- piece 4's instrument, cheap to carry now).

**AudioOutputBluetooth** (evkb, `examples/audio/bt_tone_test/`): `end()`
(`m_l2 = nullptr`, `m_cid = 0`, ring emptied, packetizer reset, cumulative
`blocks/packets/drops/pcmDrops` kept); `begin(src)` re-callable with the new
params; a paused state = `!m_src->started()` in which `update()` discards and
counts `pausedBlocks()`; `idleBlocks()` counts blocks discarded while not begun
(today's silent drop). The gap across a reconnect is therefore measured, not
inferred from `drops`.

## 7. BtSession: the policy

```
class BtSession {
  void begin(A2dpSource &src, BondTable *bonds, const char *targetName, uint8_t aclNum);
  void tick(uint32_t now);
  void disconnect();          // -> DISCONNECTING -> MANUAL (no attempts, page scan off)
  void resume();              // MANUAL -> boot policy
  void retryNow();            // fire the retry timer (NEW-35 hook)
  enum State : uint8_t { IDLE, CONNECTING, WAITING, STREAMING, DISCONNECTING, MANUAL };
  struct Stats { uint32_t links, lost, attempts, accepts, rejects; uint8_t lastReason;
                 enum By : uint8_t { NONE, PAGED, INQUIRY, INCOMING } by; uint32_t reconnectMs, lostAt; };
  typedef void (*StreamFn)(void *ctx, bool streaming, uint8_t reason, Stats::By by);
  typedef void (*AttemptFn)(void *ctx, A2dpSource::Result r, const char *pairedBy);
  void onStream(StreamFn, void *); void onAttempt(AttemptFn, void *);
  void setRetryMs(uint32_t);  // default 10000
};
```
- **Boot policy** (piece 1's walk, moved here unchanged): candidates = bonds
  MRU-first through the target-name filter (an EMPTY stored name is a
  wildcard), first candidate `PAGE_ATTEMPTS` (3), later ones 1, then INQUIRY
  with the filter. A failed cycle -> WAITING `RETRY_MS` -> repeat. This
  replaces both examples' hand-rolled retry loops and `M2_BT_CONNECT_RETRY`.
- **Reconnect policy** (after a loss of the link to address X): candidates =
  {X} only, 1 attempt per cycle, WAITING `RETRY_MS` between cycles, forever.
  The FIRST retry also waits a full cycle: a headset that pages its last
  source gets the first move, and the both-page collision is avoided on sinks
  that never page (the ESP32).
- **Page scan wanted** = no link UP and `bonds->count() > 0`. Set on entering
  WAITING/CONNECTING, cleared the moment a link is UP (either direction) and
  in MANUAL. BtLink reconciles it.
- **Incoming**: `link.inboundUp()` in WAITING (or during a same-address PAGE,
  where it just satisfies the page) -> the retry timer is cancelled and an
  INBOUND attempt starts. Stats `accepts`/`rejects` come from BtLink.
- **Callbacks**: `onStream(true, 0, by)` on EVERY entry to STREAMING
  (a first link, a reconnect, and a re-OPEN after a CLOSE -- each brings a
  media CID the node must be re-begun on); `onStream(false, reason, by)` on
  STREAMING left: a link loss carries the HCI reason, a peer CLOSE/ABORT
  carries 0x00 with the ACL still up. SUSPEND fires nothing: the media
  channel survives it and the node pauses through `started()`.
  `onAttempt(result, pairedBy)` on every attempt end, where the app saves
  bonds -- the library never touches the EEPROM.
- **Stats**: `links`, `lost`, `lastReason`, `by` (how the current/last link came
  up), `reconnectMs` (loss -> STREAMING, 0 for a first link), `attempts`,
  `accepts`, `rejects`.

## 8. Apps, the health line, knobs

**`bt_tone_test`**: `setup()` ends with `session.begin(...)` when HCI came up;
`loop()` = `session.tick(millis()); src.service(); btout.poll();` + the
heartbeat. `onStream(true)` -> `btout.begin(src)` and
`streaming by=<inquiry|paged|incoming> bitpool=N frames_per_pkt=N media_mtu=N`;
`onStream(false)` -> `btout.end()` and `bt_dropped reason=0x%02X links=N`;
`onAttempt` -> `BondStoreEeprom::save(bonds)`, `a2dp=<result>` and
`bonds=N paired_by=<x>` (today's lines). `M2_BT_CONNECT_RETRY` removed;
`M2_BT_FORGET_BONDS` kept and `bonds_forgotten=` fixed. Card-absent: the session
is never begun, so the existing vacuity assertions hold.

**`acid_box`** (`-DM2_BT_OUT=ON`): the 5 s retry block becomes
`session.tick(millis())` in the BT loop slot; the same three callbacks around
`btout.begin()/end()` (`setSelfClock(false)` preserved); `bt_hb` gains the
session fields. `idleUi` stays only for `BtFwLoader::run`/`Hci::run` at boot.
Expected: LOOPSTAT `svc max_us` from 16.8 s to low milliseconds.

**`m2_hci_probe[reconnect]`**: the four `rcConnect()` phases keep their prints;
the helper ticks the session to a verdict (STREAMING or attempt failure, 20 s
budget), then `session.disconnect()` and the cold reload as today.

**Health line**, once a second, on both examples' existing heartbeats:
```
bt_link state=<idle|connecting|waiting|streaming|disconnecting|manual> links=N lost=N reason=0x%02X by=<none|paged|inquiry|incoming> reconnect_ms=N attempts=N accepts=N rejects=N scan=<0|1> role=<m|s|->
bt_hci ncmd=N timeouts=N starved=N reclaimed=N l2drop=N credmin=N
bt_mem heap=N stack_free_min=N idle_blocks=N paused_blocks=N
```
`heap` = `mallinfo().uordblks` (the imxrt1176 core provides `_sbrk`, so newlib's
allocator is live); `stack_free_min` = the minimum of `MSP - &_ebss` sampled
every tick. Nothing in the link layer or the acceptor allocates, so a soak
asserts `heap` FLAT.

**Knobs**: setters with defaults in the library (`setRetryMs` 10000,
`setAvdtpWaitMs` 2000, `setEncWaitMs` 2000, `setStartWaitMs` 1000,
`setSupervisionSlots` 0); the examples map CMake options onto them
(`M2_BT_RETRY_MS`, `M2_BT_SUPERVISION_MS`).

## 9. Testing

### 9.1 Host suites (`M2Radio/bt/test`, run FROM `~/Development/M2Radio`)
- `btlink_test`, `a2dpsource_test`: re-shaped onto
  `runUntil(pred, ms)` = `{ now += 10; hci.service(); x.tick(now); }`; every
  piece-1 pin kept (stored rung, 0x05/0x06 erase, touch, psrm, six-byte compare,
  cancel race, name wildcard, walk order).
- NEW `btsession_test`: walk order and attempt counts; lost-peer-only cadence
  (Create_Connection count over fake time at `RETRY_MS`); page-scan wanted
  transitions (on in WAITING, off on UP, off in MANUAL); retry cancelled by an
  incoming link; the three accept/reject rules (unknown 0x0F, busy 0x0D,
  same-address-during-page accept); stats (links/lost/reason/by/reconnectMs);
  MANUAL/resume; `onAttempt` fires on every attempt end.
- `avdtp_test` (+acceptor): every signal's accept path; every validation
  reject with its category byte; the collision rule both ways; SUSPEND/START;
  CLOSE -> wait; RECONFIGURE only while open; the 1 s self-START; media-channel
  adoption order; a third channel left alone.
- `l2cap_test` (+allow-list refusal 0x0002, reset, iterator, creditsMin).
- Mutation testing per piece 1's discipline: each new pin shown RED by a mutant
  on a SCRATCH COPY (never the working tree; one reviewer per tree).

### 9.2 The QEMU gate: `audio/bt_tone_test[lifecycle]`
Third gate on the example: `run_qemu_lifecycle.sh`, `build-lifecycle/`
configured `-DM2_BT_TARGET_NAME=FAKE-HEADSET-01 -DM2_BT_RETRY_MS=3000`, peer
phase `lifecycle` in `hci_peer.py`. Three legs, ~35 s of guest time, inside the
peer's 50 s deadline and qrun's 60 s:
1. Empty store -> inquiry -> SSP -> our initiator -> media validated (>= 20
   packets at bitpool 53 = 119-byte frames) -> the peer injects
   Disconnection_Complete(status 0, handle, reason 0x08).
2. One second later the peer pages us: Connection_Request(bd, cod, ACL). It
   asserts Accept_Connection_Request with **role 0x01**, hands out a FRESH
   handle, sends Link_Key_Request and asserts our reply carries the key it
   notified in leg 1, sends Encryption_Change(on), then drives DISCOVER,
   GET_ALL_CAPABILITIES (asserting 44.1-only, bitpool 2..53, delay reporting),
   SET_CONFIGURATION at **bitpool 35** (joint 16/8 loudness -> 83-byte frames,
   a length our initiator never produces, so the media validator proves the
   config was ADOPTED), OPEN, its media channel, START, and validates >= 20
   packets at 83 bytes. Then it drops with reason 0x13 and goes SILENT.
3. Half a second later the peer sends Connection_Request from an UNKNOWN
   address and asserts Reject_Connection_Request reason 0x0F. Then our
   Create_Connection must arrive within `RETRY_MS` + 2 s, authenticate with the
   stored key, run our initiator, and stream >= 20 packets at 119 bytes.
Tripwires (peer side, each a named line): ACL on a dead handle
(`PEER-ACL-BAD-HANDLE`, exists); `create_conns` must be EXACTLY 2 (leg 1 and
leg 3 -- a retry timer not cancelled by the incoming link makes it 3); the
scan-enable sequence must read on/off/on/off/on/off (0x02 after each drop and
at boot, 0x00 after each link-up); role byte != 0x01; reject reason != 0x0F;
media at the wrong frame length in leg 2. Tally line:
`PEER-LIFECYCLE links=3 drops=2 accepts=1 rejects=1 create_conns=2 key_replies=2 scan_on=3 scan_off=3 media1=N media2=N media3=N bitpool2=35`.
UART positives: `bt_dropped reason=0x08`, `bt_dropped reason=0x13`,
`streaming by=inquiry`, `streaming by=incoming bitpool=35`,
`streaming by=paged`, `conn_req: ... -> reject(0x0F unknown)`,
`page_scan=on` x3 / `page_scan=off` x3, a `reconnect_ms=` on the last
`bt_link` line below `RETRY_MS + 5000`, and `bt_link state=streaming links=3 lost=2`.
Infrastructure verdicts (PEER-EOF/PEER-DEADLINE/Traceback) are checked AFTER
the UART checks (piece 1's ordering lesson). **Demonstrated RED by name before
the gate is trusted**, at least: node not ended on drop; acceptor streaming our
config instead of the peer's; Accept with role 0x00; retry not cancelled on
incoming; unknown address accepted; page scan never enabled.

### 9.3 What moves
`[media]` (its `a2dp=ok`/`streaming` lines stay; the heartbeat gains lines) and
`m2_hci_probe[reconnect]` (PREPARE runs once, so the peer sees one
Write_Page_Timeout instead of four; its tally does not count it) are RE-CAPTURED
(`transcript_qemu*.txt`) -- the vacuity suite is what catches a stale fixture.
Vacuity gains the `[lifecycle]` negatives (absent capture; a capture with no
`bt_dropped`; a peer file with `create_conns=3`; the bitpool tripwire).
Sweep 129 -> **130**; `LICENSE-AUDIT` gains the `build-lifecycle` GATES entry
and runs AFTER the sweep, never during.

## 10. Bench matrix (silicon-only claims)
Recorded in `examples/audio/bt_tone_test/transcript_hw_evkb.txt` and
`examples/display/acid_box/transcript_hw_evkb_bt.txt`; each run's `bt_link`
lines are the evidence:
- **R1 range loss x5** (Shokz OpenMove): carry it out of range until
  `bt_dropped reason=0x08` (~20 s supervision), bring it back; record
  `by=` and `reconnect_ms`. This answers the question the design leaves to the
  bench: whether the Shokz pages its last source (`by=incoming`) or waits to
  be paged (`by=paged`).
- **R2 power off/on x5** (Shokz): expect reason 0x13 or 0x16, then resume.
- **R3 ESP32 sink reset x5**: reason 0x08, then `by=paged` within one cycle.
- **R4 board reset** with the headset live and bonded: piece 1's boot page vs
  the headset's incoming page; ONE link, no duplicate handle, `by=` recorded.
- **R5 30 min** with five induced drops: `heap` flat, `stack_free_min` stable,
  `starved=0`, `l2drop` bounded, `pcmdrops=0` between drops, tone audible by
  ear after every recovery.
- **acid_box witness** (`M2_BT_OUT=ON` + `ACIDBOX_LOOPSTAT`): UI alive
  throughout (`ACIDBOX_VSYNC timeouts=0`, `svc max_us` in the low ms), touch p95
  unchanged, recovery audible.
- **Supervision knob**: R1 repeated at `M2_BT_SUPERVISION_MS=5000` on a link we
  paged; record detection time and any spurious drops.
- Piece 1's runs A-E fold into the same session (R1-R4 exercise the stored-key
  path).

## 11. Risks and unknowns (bench answers them)
- The IW416 has never been sent Accept_Connection_Request or asked to
  page-scan by this stack; NXP's stack does both, so the risk is ours, not the
  card's.
- Whether a real headset pages its last source at all, and how long it keeps
  trying, is per-model (R1/R2 record it). A headset that role-switches to
  master on our page is accepted passively (Role_Change recorded).
- A controller cannot page-scan while it pages; our attempt window blinds the
  headset for up to 5 s per cycle. `RETRY_MS` is the knob, R1 the measurement.
- A headset that re-configures at 48 kHz cannot be served (we advertise 44.1
  only); it gets 0x29 and, per AVDTP, retries within the advertised set.

## 12. Non-goals
AVRCP/AVCTP (piece 3 -- the PSM is refused here), role switch and sniff, the
automatic flush timeout and the credit floor (piece 4 -- `credmin` is its
instrument), soak automation (piece 5), LE, more than one link, NEW-35's UI
beyond the session hooks (`disconnect`, `resume`, `retryNow`, `Stats`).

## 13. File map
M2Radio: `bt/BtLink.{h,cpp}` (rewrite), `bt/A2dpSource.{h,cpp}` (rewrite),
`bt/Avdtp.{h,cpp}` (+acceptor), `bt/L2cap.{h,cpp}` (+allow-list, reset,
iterator, creditsMin), NEW `bt/BtSession.{h,cpp}`, tests `bt/test/{btlink,
a2dpsource, avdtp, l2cap}_test.cpp` (+ NEW `btsession_test.cpp`), `bt/test/run.sh`.
evkb: `examples/audio/bt_tone_test/{bt_tone_test.cpp, AudioOutputBluetooth.{h,cpp},
CMakeLists.txt, run_qemu_lifecycle.sh (NEW), transcript_qemu_lifecycle.txt (NEW),
transcript_qemu_media.txt (recaptured)}`, `examples/networking/m2_hci_probe/{hci_peer.py,
m2_hci_probe.cpp, transcript_qemu_reconnect.txt (recaptured)}`,
`examples/display/acid_box/acid_box.cpp`, `tools/gate-vacuity.test.sh`,
`tools/license-audit.sh` (GATES), `evkb.cmake` (M2Radio pin), `CLAUDE.md`,
`docs/KNOWN-BROKEN-GATES.md` if anything is dispositioned.
