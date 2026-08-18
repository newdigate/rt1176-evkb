# W6: associate to an open AP — design (step 1 landed, association gated on a target)

Continues `docs/superpowers/handoff/2026-08-18-w6-next-session.md`.
W1–W5 pass in a single boot (enumerate → firmware → host cmds → scan →
monitor-mode RX).

## The hard dependency, stated up front

W6 (association + bidirectional data) needs a **specific AP** the firmware can
join. Association to a real AP is not a fact this code can default or discover
without a human: which SSID, and — critically — whether it is OPEN (no key
exchange) or WPA2 (a 4-way EAPOL handshake, a much larger step). The handoff
says so three times. So W6 splits:

* **Step 1 — security survey (AP-independent, DONE this session).** Extend the
  scan to classify each BSS OPEN / WEP / WPA / WPA2 from the beacon capability
  Privacy bit and the RSN (IE 48) / WPA (vendor 00:50:F2:01) IEs. This is
  hardware-verifiable now and answers the gating question: *is any open AP in
  range?* Committed and gated with the rest of the scan.
* **Steps 2–4 — association + TX (BLOCKED on a target AP).** Not written,
  because this project ships only what silicon verifies (the two-gate rule),
  and association cannot be verified without an AP to join. Writing untested
  ASSOCIATE/TX firmware would violate that discipline.

## Step 1 result

See `examples/networking/m2_sdio_probe/transcript_hw_evkb.txt` (W6 section) for
the measured survey. Each `scan_ap` line now carries `sec=open|wep|wpa|wpa2`.

## Steps 2–4, ready to build once a target exists

Sequence (NXP `wlan_cmd_802_11_associate`, `mlan_sta_cmd.c`):

1. From the chosen scan entry: BSSID, channel, capability, supported-rates IE,
   and (for WPA2) the RSN IE — all already captured by the step-1 scan.
2. **ASSOCIATE (0x0012)** — TLVs: SSID, PHY param (channel), rates, RSN (WPA2
   only), plus the capability word. Open-system auth is folded in on this
   firmware; confirm whether a separate AUTHENTICATE (0x0011) is needed.
   `resp_result` and the association-result IE name a reject.
3. On success, data frames flow on the DATA ports (RX already proven by W5's
   `readDataPacket`; associated frames are ethernet-typed, not 802DOT11 — an
   802.3/LLC payload behind the RxPD). TX: poll WR_BITMAP (0x14–0x17),
   CMD53-write an MLAN_TYPE_DATA packet with a TxPD header to `ioport|port`.
4. Un-fakeable proof: a broadcast ARP or DHCP DISCOVER out, and the reply
   (DHCP OFFER, or an ARP for our address) captured via `readDataPacket`.

**Scope for the next session:** OPEN association + one DHCP/ARP round-trip
FIRST. WPA2 (EAPOL 4-way, PMK/PTK) is a separate, larger phase after that.

## What the user must provide

The next session needs, from the user: an OPEN AP in range (a phone hotspot
set to no-password, or a spare router with an open SSID), and its SSID. If the
step-1 survey shows an open AP already present, that AP can be the target with
the user's confirmation.

## What does not change

* QEMU gate: all of this runs only after `fw_download=ok`, unreachable by the
  module-absent path. Gate stays green unmodified.
* Licence posture: NXP source is a protocol reference; nothing vendored.
* After M2Radio changes: push, then bump the `evkb.cmake` pin.
