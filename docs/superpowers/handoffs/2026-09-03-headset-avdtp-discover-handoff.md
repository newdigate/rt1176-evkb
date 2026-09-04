# Handoff — Headset AVDTP DISCOVER (BT-3 / NEW-9)

**Date:** 2026-09-03. **Status:** diagnosed, banked mid-fix. Resume at the plan's Task 5.

**Docs:** spec `docs/superpowers/specs/2026-09-03-headset-avdtp-discover-design.md`;
plan `docs/superpowers/plans/2026-09-03-headset-avdtp-discover.md` (the execution
guide — subagent-driven).

## One-paragraph situation

BT-3 phase 4 already streams A2DP audio to an ESP32 sink (the phase-4 size-cap fix,
`L2cap::MAX_PAYLOAD`, is done and audible). The path to *clean* audio is a real
headset. The **Shokz "OpenMove by Shokz" pairs over SSP end-to-end** but **AVDTP
hangs at DISCOVER** (`avdtp_state=1`, no response). We ran a diagnosis-first
sub-project: built an ACL-trace instrument, and captured a **Mac→Shokz reference in
Apple PacketLogger** which decodes the exact exchange the Shokz accepts. That gave
three concrete gaps in our `M2Radio/bt` stack. The confirmatory EVKB→Shokz trace was
*not* obtained because the EVKB→Shokz **connect became unreliable** (paging timeout),
which is a separate blocker.

## What is DONE and pushed (plan Tasks 1–3 — the instrument)

- **M2Radio `51e982b`** — `L2cap::onAclTrace(TraceFn, ctx)`: fires for every inbound
  ACL (in `onAcl`, before demux) and every outbound ACL (in `service()`), handing over
  the L2CAP PDU + handle. Off unless set. Host-tested (`l2cap_test` 32 checks).
- **evkb `580feba`** — `tools/acl-trace-to-btsnoop.py` (+ `.test.sh`): converts the
  firmware's `acl_trace` VCOM lines to a btsnoop HCI log that PacketLogger/Wireshark
  decode. And `bt_tone_test` `M2_BT_ACL_TRACE` (off by default) wiring the hook to a
  VCOM hex dumper. Both `bt_tone_test` QEMU gates green; default build byte-identical.
- Pin bumped (M2Radio → `51e982b`); fresh-user `-DEVKB_FORCE_FETCH=ON` builds it.

## The diagnosis (plan Task 4)

Decode any PacketLogger `.pklg` with:
`/Applications/Wireshark.app/Contents/MacOS/tshark -r <file>.pklg -Y "btavdtp" -T fields -e frame.number -e _ws.col.info`

The user's Mac→Shokz reference: `~/Documents/BT-shokz-capture.pklg`. Its working AVDTP
sequence: `Discover → ResponseAccept(2 SEPs) → GetAllCapabilities×2 → SetConfiguration
(SBC 44100/JointStereo/16 blocks/8 subbands/Loudness/bitpool 2..53) → Open → (Shokz
sends DelayReport(200ms), Mac accepts) → Start`. On the L2CAP AVDTP channel (PSM
0x0019) the Mac's `CONFIG_REQ` carries **mtu=1004**.

**Three concrete gaps between our `M2Radio/bt` and the compliant Mac exchange:**

1. **Empty L2CAP CONFIG_REQ.** `M2Radio/bt/L2cap.cpp` `service()`, the
   `if (ch.state == CONFIG && !ch.cfgReqSent)` block, sends
   `uint8_t c[8] = { CFG_REQ, m_nextId++, 4, 0, remoteCid_lo, remoteCid_hi, 0, 0 }` —
   **no options**. The Mac sends an MTU option (`01 02 <mtu_lo> <mtu_hi>`, mtu=1004).
   **Leading DISCOVER-blocker suspect.** Plan Task 5 branch (b) has the exact
   MTU-carrying CONFIG_REQ + a demonstrated-red `l2cap_test` case.
2. **`GetCapabilities` (0x02) vs `GetAllCapabilities` (0x0C).** `M2Radio/bt/Avdtp.cpp`
   `buildGetCapabilities` emits signal id **0x02**; the Mac uses **0x0C**
   (GetAllCapabilities, AVDTP 1.3). A strict sink may only answer 0x0C. `parseSbcCaps`
   loops service categories so it should parse the richer 0x0C reply, but verify.
   *(2nd-order: this is after DISCOVER, so not the DISCOVER blocker — but needed to
   reach STREAMING.)*
3. **Unhandled incoming DelayReport.** The Shokz SENDS a `DelayReport` command (signal
   **0x0D**) after Open that the initiator must ACCEPT (like the peer-DISCOVER already
   handled in `Avdtp::onSignalling`/`service()` via `buildDiscoverAcceptOneSource`).
   Our stack ignores it → stall after OPEN. Add an accept-DelayReport path.

**★ CAVEAT — read before assuming (1) is THE fix:** an earlier EVKB run reached
`avdtp_state=1` = the L2CAP signalling channel reached OPEN *and* we SENT the DISCOVER
(so the Shokz accepted our config on our side). That means the MTU may not be the sole
DISCOVER blocker. The confirmatory **EVKB→Shokz ACL trace** (to see exactly what we
send and whether the Shokz replies) was NOT captured — the connect kept timing out.
So: implement all three, then let silicon say which mattered; and get the EVKB trace if
the connect cooperates (it definitively answers "does our DISCOVER go out / does the
Shokz reply / was our config-response right").

## Two NEW blockers (distinct from AVDTP; both worth fixing)

- **Shokz `Create_Connection` times out** (`connect=timeout (no Connection_Complete)`):
  paging reliability. It connected on the fresh boots early in the session, then
  degraded. `Create_Connection` params are correct (`BtLink.cpp:95-97`: psrm + clk with
  bit15 valid), so it's timing/RF/headset-state, not a param bug. Candidate looks:
  `Write_Page_Timeout`, retry/backoff on connect, role-switch, or a fresh inquiry per
  attempt. **This blocks getting the EVKB→Shokz trace and testing any fix on the Shokz.**
- **Repeated failed connects wedge the HCI**: `inquiry=fail reason=ncmd_starved
  status=0xFF`. The connect-timeout path leaks an HCI command credit
  (Num_HCI_Command_Packets) until the controller can't issue commands. A reflash clears
  it. Real driver-robustness bug — the timeout path must return the command credit.

## How to resume (recommended order)

1. **Implement the 3 AVDTP gaps in `M2Radio/bt`.** Start with plan Task 5 branch (b)
   (CONFIG_REQ MTU — full code + host test in the plan), then add (2) GetAllCapabilities
   and (3) DelayReport-accept, each with an `avdtp_test`/`l2cap_test` host case.
2. **Test against the ESP32 sink FIRST** — it connects reliably, and MTU +
   GetAllCapabilities are peer-agnostic, so this proves no-regression (ESP32 still
   reaches STREAMING) before the Shokz is involved. Build: real fw
   `-DM2RADIO_IW416_BT_FW=…/uartIW416_bt.bin.inc`, `-DM2_BT_RTS_FLOW=ON`,
   `-DM2_BT_FAST_BAUD=ON -DM2_BT_FAST_BAUD_RATE=3000000`, `-DM2_BT_LEGACY_PIN=ON`,
   `-DM2_BT_TARGET_NAME=EVKB-SINK`, `-DM2_BT_CONNECT_RETRY=ON`.
3. **Then the Shokz** — needs the connect/paging blocker addressed first (or luck on a
   fresh boot). Build as (2) but `-DM2_BT_LEGACY_PIN=OFF` (SSP), `-DM2_BT_TARGET_NAME=Shokz`,
   add `-DM2_BT_ACL_TRACE=ON` to capture the exchange; convert with
   `tools/acl-trace-to-btsnoop.py` and diff against the Mac reference.
4. Regression guard (plan Task 6): tighten the `m2_hci_probe` `[avdtp]` fake peer
   (`hci_peer.py`, gate `run_qemu_avdtp.sh`) to require the correct config /
   GetAllCapabilities, demonstrated red-then-green.
5. Close-out (plan Task 8): push, bump pin, fresh-user, sweep + audit, update memory + NEW-9.

## Bench recipe (Shokz — the fiddly bits, all learned the hard way)

- **Real BT firmware required:** `-DM2RADIO_IW416_BT_FW=/Users/nicholasnewdigate/Development/mcuxsdk-ws/mcuxsdk/components/conn_fwloader/fw_bin/inc/IW416/uartIW416_bt.bin.inc` (gate builds use a 1 KB SYNTHETIC image → HCI dead → `streaming=0`).
- **Per Shokz attempt:** factory-reset the Shokz (its bond to the EVKB is stale after any pairing; the EVKB loses its key on reflash → mismatch → paging timeout), put it in **explicit pairing mode** (power-cycle only gives reconnect mode), and turn **Mac Bluetooth OFF** (the Mac grabs the Shokz and it can't be discoverable to both).
- **Flash/reset:** LinkServer `run` is the only reliable reset+VCOM (physical reset halts on a DEMCR vector catch when LinkServer is attached; drops the VCOM CDC when not). Never `pkill -9` mid-program. Don't hold the VCOM during programming.
- **Capture:** the firmware retries connect every 5 s (`M2_BT_CONNECT_RETRY`); read the VCOM with a reopen-on-drop reader; `acl_trace` lines → `tools/acl-trace-to-btsnoop.py out.btsnoop`.
- **ESP32 sink** ("EVKB-SINK") resets itself whenever its serial is opened — don't monitor it live during a test.
- Clean-room: L2CAP/AVDTP facts from the specs + our own captures; no third-party stack source transcribed. Stage specific files, never `git add -A`.

## Repo state at handoff

- M2Radio `master` = `51e982b`, pushed. evkb `master` = `580feba`, pushed. Both clean.
- Plan Tasks 1–3 done; Task 4 diagnosis done (Mac reference; EVKB trace pending on the
  connect blocker); Task 5 is the resume point, now informed by the 3 gaps above.
