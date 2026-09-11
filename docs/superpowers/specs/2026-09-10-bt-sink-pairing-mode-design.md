# A2DP sink: a runtime pairing mode (NEW-42 item C)

Design, brainstormed and approved 2026-09-10.  The third item of NEW-42, deliberately left out of that spec
(`2026-09-09-bt-sink-jitter-absorption-design.md`, scope A + B) because it is a user-facing feature with its own
trigger, timeout and feedback questions.  Those are answered here.

## 1. The problem, as the bench recorded it

A bonded sink is not discoverable by design (`BtSinkSession::tick`: inquiry scan is on only while no bond
exists -- a phone that already knows us PAGES, it does not search).  That is the right default and it has a
sharp consequence: **once the phone forgets the sink there is no way back except a reflash** with
`M2_BT_FORGET_BONDS=ON`, which NEW-41's bench notes call out and NEW-42's control arm hit again on its first
minute (`bonds_boot=0` on the board, a stale key on the phone).

NEW-43 makes it worse.  One failed SSP -- a stale key on the phone is enough -- makes `BtLink`'s legacy-PIN
fallback write `Write_Simple_Pairing_Mode = 0`, and only PREPARE re-enables it; so after one bad attempt the
sink is **passcode-only until the board is rebooted**, and a phone user sees a passcode prompt from a speaker.
Measured 2026-09-09, bench RUN 4.

Real speakers answer both with the same gesture: a pairing mode.  Discoverable for a window after power-on
and after a disconnect, and on demand.

## 2. What the board can and cannot offer, measured before designing

* **The EVKB's own user button is unreachable.**  SW7 sits on `GPIO_LPSR_00` (GPIO13), and the 1176 core
  has no pad in the LPSR domain -- zero hits for `GPIO13`/`GPIO_LPSR` in `core_pins.h`; all 22 digital pins
  are Arduino-header pads.  A physical button is an external one on a header pin, or a core change.  QEMU
  cannot press either.
* **A console command is fully viable.**  `serial/serial_test_rx` proves LPUART1 RX works over the MCU-Link
  VCOM on this board and gates it in QEMU through `-chardev socket` and a Python driver.  The sink's console
  is write-only today and `tools/rt1170-console.py` is read-only; both are small changes.  The core's
  `yield()` already dispatches `serialEvent1()` whenever `Serial1.available()`, and the sink's `loop()` calls
  `yield()` every pass, so a command reader is a weak-override with no `loop()` change.
* **A socket console can still be captured to a file.**  qemu2 (11.0.50) supports
  `-chardev socket,...,logfile=`; smoke-tested with the sink ELF, the boot banner landed in the file.  So the
  existing gate's file-based assertions survive a writable console.  ★ The vacuity harness's fake QEMU
  recognises only `-serial file:` (it scans argv for that form), so under `GATE_VACUITY=1` the gate must keep
  a file console; the socket is for real runs.
* **The green User LED is free.**  `LED_BUILTIN` = pin 3 = `GPIO_AD_04` (GPIO3.3, D3); nothing the sink
  links claims it.  The RevC3 header audit names the pad but not its polarity, and the only "active low"
  note in the core is the 1060's D8 -- a different board.  **Polarity is verified at first light on the
  bench and the constant fixed then**, not guessed here.
* **PREPARE can be re-issued without `begin()`.**  `BtLink::tickPrepare()` writes only idempotent things --
  Set_Event_Mask, Write_Simple_Pairing_Mode, Write_Page_Timeout, Class_of_Device, Local_Name -- and never
  touches the scan bookkeeping (`m_haveScan`/`m_wantScan`).  `BtLink::begin()` DOES reset that bookkeeping
  to "off" and clears any in-flight op, so re-calling it mid-session would desync host and controller; the
  clean lever is `link().startPrepare()`, which returns `false` when `m_op != NONE` and so cannot double a
  PREPARE already in flight.  That is the NEW-43 tie-in, and it costs one guarded call.
* `btsinksession_test` has a fake-controller rig with an injected clock, so the policy is host-testable.

## 3. Decisions

| question | decision | why |
|---|---|---|
| trigger | idle-timeout windows **and** a console `pair` command | the timeout is the product behaviour and QEMU exercises it natively; the command is the bench/gate hook and the on-demand path.  A physical button becomes a third caller of the same API, addable in a line |
| window | **2 min** at boot, **2 min** after every link drop (loss or clean close), **bonded or not**; unbonded stays discoverable indefinitely as today | how most speakers behave.  The bonded window is the security trade: for two minutes after power-on or a drop, any phone nearby can pair.  Accepted |
| entry action | discoverable **+ PREPARE re-issued when idle**; bonds untouched | every window guarantees SSP is on, so NEW-43's stranded state heals on the next drop or `pair` without a reboot |
| `forget` | a **separate, explicit** command; never part of a timeout | destructive; an auto-window must never forget the owner's phone |
| feedback | LED blink ~1 Hz while open; `pairing=` console lines on each edge; a `pairing=` field in the heartbeat | the LED is what a person at the speaker sees; the console line is what the gate asserts; the heartbeat field survives a 1-in-30 sampled transcript |
| commands | `pair` / `forget` / `status`, newline-terminated, case-insensitive, exact match, unknown answered `cmd=? "..."` | a NUL or a stray byte must never be a command -- the VCOM capture starts with a NUL (CLAUDE.md), and `forget` is destructive.  Single-character commands rejected for that reason |
| where the policy lives | `BtSinkSession` (M2Radio); parsing, LED and prints in the sketch | policy where the state is, and host-testable there; the gate driving `pair` over the socket is what pins the parser (approach A; the host-tested parser class of approach C was declined) |
| `pair`/`forget` while a link is up | **refused**, `pairing=refused reason=link_up` | "press pair while connected to switch phones" is a disconnect policy, not a pairing one -- noted for later |

## 4. The session

```cpp
enum PairingReason : uint8_t { PAIR_NONE, PAIR_BOOT, PAIR_DROP, PAIR_CMD };
void          setPairingWindowMs(uint32_t ms);              // auto-window length; default 120000; 0 = no auto-windows
bool          enterPairing(uint32_t now,
              PairingReason r = PAIR_CMD);                  // open or extend to now + window; false = refused (link up)
bool          pairingOpen() const;                          // as of the last tick(now): that is where expiry is evaluated
uint32_t      pairingRemainingMs(uint32_t now) const;
PairingReason pairingReason() const;                         // the reason of the CURRENT window; PAIR_NONE when closed
```

`pairingOpen()` takes no clock on purpose: `tick(now)` evaluates the deadline once per pass, latches the
result, and fires the *timeout* edge there -- so the sketch's poll, the discoverability line and the
heartbeat all read the same answer for the same pass.  An `enterPairing()` on an already-open window extends
it to `now + window` and the reason becomes the new caller's.

**Opening.**  `begin()` opens a `PAIR_BOOT` window.  **Every return to `LISTENING`** opens a `PAIR_DROP`
window -- the loss branch, the clean-close branch, **and the failed-attempt branch** (`CONNECTING` ->
`LISTENING`, `rejects++`).  The sketch's `pair` and `forget` call `enterPairing(now, PAIR_CMD)`.  All go through
one private `openWindow()`, so there is one place to get wrong: it sets `m_pairUntil = now + window`, records
the reason, and calls `m_sink.link().startPrepare()`.  On the boot window that call returns `false` because
`begin()`'s own PREPARE is in flight -- no double.

★ **Corrected during planning (2026-09-10): the failed-attempt branch is the NEW-43 path, and the first draft
of this section missed it.**  A stale key on the phone makes SSP fail, `BtLink`'s fallback writes SSP mode 0,
the attempt ends and the session returns to `LISTENING` from `CONNECTING` -- not from `STREAMING`.  "Re-entry
from STREAMING" alone would have re-issued PREPARE on every path EXCEPT the one that needs it.  Host case P3
pins that branch on its own, independently of the loss branch (P2).

★ Also settled while planning: `canPair()` (`LISTENING` with no link up) is public, because `forget` must
test it BEFORE wiping -- a refused `forget` must leave the table exactly as it was; `setPairingWindowMs(0)`
turns the AUTOMATIC windows off and a commanded window is then `PAIR_DEFAULT_MS` (120 s); and the window
closes as *paired* on reaching **`STREAMING`**, not on `CONNECTING` -- a page that fails to pair must not
have closed the window it is about to need.

**The one line that changes in `tick()`:**

```cpp
m_sink.link().wantDiscoverable(listening && (m_alwaysDisc || !m_bonds || m_bonds->count() == 0 || m_pairOpen));
```

**Closing.**  In `tick(now)`, on expiry (`now >= m_pairUntil`), reason kept for the edge print as *timeout*; or the moment a
link reaches `STREAMING` (the `CONNECTING` -> `STREAMING` transition), recorded as *paired*.  A window never
*survives* a link reaching `STREAMING`, and `enterPairing()` refuses rather than queues while a link is up.
It CAN be open through `CONNECTING` -- that is the design (see the ★ above: the window closes on `STREAMING`,
not on `CONNECTING`), and it is not discoverable there either way, because the scan line reads `listening`,
which a link coming up has already made false.

★ **Corrected during the Task 1 reviews (2026-09-10): the *paired* close happens AT the transition, ahead of
every callback; `disconnect()` closes *cancelled* only a window that never saw `STREAMING`; `resume()` opens
none.**  Both callbacks can call `disconnect()` with a window open, and with the close sited at the end of
`tick()` neither shape reached it: the attempt callback on the success edge runs BEFORE that check and has
already left `STREAMING`, so the boot window was never closed as *paired* (`btsinksession_test` Q7 -- open with
the link `LINK_SECURE` through the whole teardown); the stream callback on the loss edge runs one line AFTER
the drop branch opened its window (Q6).  Either way the window rode through `DISCONNECTING` and `MANUAL` with
the scans off: `pairingOpen()` true on a sink that is not discoverable (the LED would blink "pairing" in
`MANUAL`), `enterPairing()` refused at the same time (`canPair()` false), and `resume()` re-entering
`LISTENING` on a stale deadline with no PREPARE.  The first pass answered both shapes with *cancelled*, which
made the END REASON a function of callback timing rather than of the wire: one and the same outcome (stranger
paged, SSP completed, link `LINK_SECURE`, attempt `OK`) read *cancelled* from inside the callback and *paired*
from `loop()` one tick later, and §5's `pairing=off reason=cancelled` would have printed on a successful
pairing.  So the *paired* close moved INTO the transition, beside `links++`/`accepts++` and ahead of the
callbacks -- a link reached `STREAMING`, so it paired, whatever the app did next -- and `tick()` keeps only the
clock arm (nothing else can reach `STREAMING` with a window open: `openWindow()` opens one only in
`LISTENING`).  *Cancelled* is then exactly the Q6 shape and the boot/commanded window torn down from
`LISTENING` or `CONNECTING`, guarded so a `disconnect()` with nothing open leaves the last window's end alone.
Q7 and the new P6 -- Q7's sequence with `disconnect()` called from OUTSIDE the callback -- pin that the two
timings agree; P6 is green either way alone, which is why it only bites beside Q7.  And `resume()` opens **no**
window: "every return to `LISTENING`" above enumerates the ends of ATTEMPTS -- loss, clean close, failed
pairing -- while `resume()` is an app command; the app calls `enterPairing()` if it wants one.

★ **Corrected during the Task 1 code-quality review (2026-09-10): the reason is a LABEL, and the parameter
defaults.**  The signature above handed the caller's enum straight to the private `openWindow()`, where
`r != PAIR_CMD` doubled as the automatic/commanded test -- so the public parameter selected policy behind the
caller's back, two ways, both measured.  `enterPairing(now, PAIR_NONE)` returned true and opened a window whose
`pairingReason()` then read `PAIR_NONE`: `pairingOpen()` and `pairingReason()` contradicting each other, and
§5's heartbeat printing `pairing=off secs=120` with the LED blinking.  And `enterPairing(now, PAIR_DROP)` after
`setPairingWindowMs(0)` was REFUSED, so the "a commanded window is then `PAIR_DEFAULT_MS`" promise above held
only for callers who happened to pass that one value.  `openWindow()` now takes the commanded/automatic
distinction as an argument of its OWN, `enterPairing()` always passes *commanded* whatever the label, and
`PAIR_NONE` -- the one value that cannot be kept, since `pairingReason()` reports it for *closed* -- normalises
to `PAIR_CMD`.  The parameter defaults to `PAIR_CMD`, so §5's `pair` handler can call `enterPairing(now)` and
the call shape written throughout this spec stays valid.  `btsinksession_test` P7 pins all three, RED first.

**Not a new `State`.**  A window is a deadline beside `LISTENING`, not a state of its own: `MANUAL` and
`LISTENING` both compose with it, and every `m_state == LISTENING` test in `tick()` stays as it is.

**Observability.**  No callback.  The sketch polls `pairingOpen()` each pass, exactly as it already polls
`sink.suspended()`, and prints on the edge.

## 5. The sketch

**Commands** via a `serialEvent1()` override: a 16-byte line buffer; `\n` or `\r` dispatches; a NUL or any
other non-printable REFUSES the whole line; overflow discards the line and reports it.  Case-insensitive,
exact match.  **One command per invocation** and **a partial line expires after 2 s** -- both corrected
during Task 3's review, and both in that task's corrections note with the harm each prevents.

| line | link up | LISTENING and idle | neither (IDLE / MANUAL / CONNECTING / DISCONNECTING) |
|---|---|---|---|
| `pair` | `pairing=refused reason=link_up` | `enterPairing(now, PAIR_CMD)` -> `pairing=on reason=cmd secs=120` | `pairing=refused reason=not_listening` |
| `forget` | `pairing=refused reason=link_up` | `BondStoreEeprom::wipe()` -> `bonds_forgotten=N` (the existing line) -> `enterPairing(now, PAIR_CMD)` | `pairing=refused reason=not_listening` |
| `status` | prints the heartbeat block now | same | same |
| anything else | `cmd=? "..."` | same | same |

**Two refusal reasons, not one**, because `canPair()` is `LISTENING && !linkUp` and only one half of that is
a link.  A single `reason=link_up` printed a flatly untrue sentence in the card-absent image -- twice,
directly under a heartbeat reading `state=idle links=0` -- which sends a bench reader hunting a connection
that does not exist.  Two tokens, no trailing field: the while-streaming refusal §6.2 and §6.3 assert is a
genuine link up and its text is unchanged.

**The LED.**  `pinMode(LED_BUILTIN, OUTPUT)` in `setup()`; in `loop()`, while `pairingOpen()` the LED follows
`(millis() / 500) & 1`, otherwise off.  The active level is a named constant the first bench step sets.
QEMU cannot see it; it is a silicon-only witness like `under=`.

**Prints on the edge:** `pairing=on reason=boot|drop secs=119` from `loop()`'s edge detector when an AUTOMATIC
window opens, `pairing=off reason=timeout|paired|cancelled` from the same detector on every close -- and **a commanded
window prints its own `pairing=on reason=cmd secs=N` from the command handler**, because extending an already-
open window is not an edge and the person who typed `pair` deserves an answer either way (corrected during
planning: the first draft had one print site and would have gone silent on a second `pair`).  The heartbeat's `bt_link` line gains
` pairing=boot|drop|cmd|off secs=N` (`secs=0` when off).  The gate greps no `bt_link` field today, so the
addition moves nothing.

**`status`** means the heartbeat block leaves `loop()` for a `printHeartbeat()` that the timer and the
command both call.  That is the one refactor.

**`tools/rt1170-console.py`** gains a stdin-to-port thread: run in the foreground, type `pair`.  Under
`nohup ... &` -- how every capture in this tree was taken -- stdin hits EOF, the thread exits, reading is
unchanged.  Deliberately NO separate one-shot sender: it would re-open the tty while the reader holds it,
and CLAUDE.md records what re-opening this VCOM does.

## 6. Verification

### 6.1 Host tests -- every policy claim RED-pinned first

`M2Radio/bt/test/btsinksession_test.cpp`, on its existing rig with the injected clock:

* the BOOT window opens in `begin()` and expires on the clock;
* **a bonded sink is discoverable while the window is open and NOT after it expires** -- the mutant that
  never closes the window stays green on the first half and reddens on the second, which is what makes the
  pair of checks load-bearing;
* a DROP window opens on loss AND on clean close;
* `enterPairing()` extends; is refused while a link is up; re-issues PREPARE **exactly once**, counted as
  `Write_Simple_Pairing_Mode` commands the fake controller receives, and NOT while the boot PREPARE is
  still in flight;
* the window closes as *paired* on reaching `STREAMING` -- at the transition, ahead of the callbacks, so the
  reason is the same whether the app's `disconnect()` comes from inside the attempt callback or from `loop()`
  a tick later (the second review's correction: those two read differently before it, and the pair of cases
  that pins it is Q7 + P6 -- P6 alone is green either way);
* `setPairingWindowMs(0)` disables the auto-windows and leaves `enterPairing()` working.

Each demonstrated RED by name.  The sketch's parser is not unit-tested (approach A); the gate is what pins it.

### 6.2 Gate 139, extended in place -- count stays 139

The console chardev becomes `-chardev socket,...,logfile=$OUT -serial chardev:c0` in real runs and stays
`-serial file:$OUT` under `GATE_VACUITY=1`.  A small Python driver on the console socket sends the commands.
`hci_peer.py`'s `source` phase gains a drop-and-re-page after the volume set, borrowed from `lifecycle`.
Asserted, in order:

1. `pairing=on reason=boot secs=119` at boot.
2. `pair` **while streaming** -> `pairing=refused reason=link_up` -- proves the whole command path over the
   socket, and that a refusal is a refusal.
3. After the injected drop: `pairing=on reason=drop`; **`PEER-SCAN-ENABLE 0x03`** in the peer's log -- the
   inquiry-scan write is something only the controller side can see, so the firmware cannot invent it; and
   a SECOND `ssp_mode: st=ok status=0x00 mode=1` on the UART -- PREPARE re-issued.
4. `forget` -> `bonds_forgotten=1`, then the peer's re-page is accepted **unbonded** and pairs Just Works
   again (`link_key_req: ... -> neg_reply (no stored key)`, `paired_by=ssp` -- the mechanism, not the initiator; `peer` is not a value this firmware prints) -- the NEW-43 recovery, with
   no reboot.  ★ Safe to exercise in the gate ONLY because QEMU has no NVM behind the FlexSPI window
   (CLAUDE.md); the gate says so where it sends it.
5. Link up -> `pairing=off reason=paired`.
6. `status` -> an `hb` block appears without waiting for the 1 s timer.

**Expiry timing (120 s) is a host claim** -- too long for a gate, and QEMU's clock is a fiction anyway
(NEW-42, section 4.2).  Both new assertion classes DEMONSTRATED RED before trusted.  Vacuity: the fixture
re-captured; a capture in which the drop window never opens must fail by name.

**Two things the gate does NOT cover, named here because the review found them unnamed and this tree's
convention is to state a gap rather than let a `PASS:` line be read as wider than it is** (the `[lifecycle]`
`btout.end()` precedent):

* **The post-`forget` MEDIA path.**  The peer's re-page is deliberately minimal -- it pairs, injects
  `Encryption_Change` and stops; it does not redo SDP, AVDTP or media (a second A2DP bring-up would call
  `AudioInputBluetooth::begin()`, clear `m_primed` with no media left to send, and redden the `[jit]`
  pre-fill assertion on a healthy run).  So point 4 above proves the sink is re-**pairable**, never that it
  is re-**usable**.  Widening the peer was considered and REJECTED: the run is already ~39 s against
  `tools/qrun`'s 60 s cap, and CLAUDE.md records that QEMU cannot carry A2DP at the audio rate anyway.
  Section 6.3's bench is where a real phone re-pairs and plays again -- and NEW-43, the defect this feature
  answers, is a pairing defect, not a streaming one.
* **`pairing=off reason=paired` on the SECOND window.**  Following from the first: the re-page never reaches
  `STREAMING`, so the window `forget` opened is still open at the end of the run and point 5 is exercised
  exactly once, on the boot window.  Per-window it is pinned by 6.1's host suite; on the wire, by every
  bench reconnect.

**And one attribution the gate had to buy with a driver change.**  `BtLink::reconcileScan()` writes only on a
CHANGE, so the drop window's `Write_Scan_Enable 0x03` and the two commanded windows' coalesce into the single
`PEER-SCAN-ENABLE 0x03` the peer logs -- and in the first capture the commanded window came FIRST, which left
point 3's un-fakeable half proving only that *some* window opened.  `sink_console.py` now waits for the sink's
own `scan_enable=0x03` after the drop and logs `DRIVER-SAW` before it types anything; the gate asserts that
line and that it precedes every post-drop `DRIVER-SENT`.  The peer's 0x03 is therefore the DROP window's.

### 6.3 Bench

First light: LED polarity, constant fixed.  Then one session: boot -> LED blinks two minutes -> stops;
connect -> LED off, `pairing=off reason=paired`; out of range -> drop -> LED blinks -> re-select on the phone
(iOS does not re-page after a range loss, NEW-41) -> paired; `forget` + "Forget This Device" on the phone ->
pair fresh, Just Works, **no SW4**; `pair` while streaming -> refused.  And NEW-43 reproduced ON PURPOSE: a
failed SSP (a stale key on the phone), then the drop window's PREPARE, then a clean pair without a reboot.

### 6.4 Close-out

M2Radio pin -> every M2Radio-linking gate image rebuilt before the sweep -> fresh-user by RUNNING the sink
gate on the fetched ELF -> sweep -> audit AFTER -> vacuity -> section 7 written from the capture -> CLAUDE.md,
memory, Linear (NEW-43 marked mitigated-not-fixed: the window heals the symptom; the fallback still writes
mode 0 and should stop) -> merge.

## 7. Silicon: the bench

Written from the capture after 6.3 runs.

## 8. Considered and deferred

* **`pair` while connected switches phones** -- a disconnect policy; its own decision.
* **A physical button** -- a third caller of `enterPairing()`, on an external header-pin button or after a
  core change mapping the LPSR domain.  One line when wanted.
* **A host-testable parser class** (approach C) -- declined; the gate pins the parser end to end.
* **Fixing NEW-43 at its root** (the fallback restoring mode 1) -- stays NEW-43; this spec heals the
  symptom on the next window, which is the user-visible half.

## 9. Files (expected)

M2Radio: `bt/BtSinkSession.{h,cpp}`, `bt/test/btsinksession_test.cpp`.  evkb: `examples/audio/bt_sink_test/
{bt_sink_test.cpp, run_qemu.sh, transcript_qemu.txt, transcript_hw_evkb.txt}` + a console driver script,
`examples/networking/m2_hci_probe/hci_peer.py` (`source` phase drop-and-re-page), `tools/rt1170-console.py`,
`tools/gate-vacuity.test.sh`, `evkb.cmake` (pin), `CLAUDE.md`, memory, this spec.
