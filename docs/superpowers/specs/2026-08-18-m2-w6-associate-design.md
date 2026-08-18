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

## Decision (2026-08-18): WPA2 to "OnestreamQJN7"

The step-1 survey found NO open AP in range — all three reachable APs are
WPA2. Asked how to proceed, the user chose **WPA2 to "OnestreamQJN7"**
(BSSID 78:20:51:8F:19:5E, ch 4). So W6's target is a WPA2-PSK network.

### The WPA2 path is tractable — the firmware has an embedded supplicant

Researched in NXP's `mlan_sta_cmd.c`: `HostCmd_CMD_802_11_SUPPLICANT_PMK`
(**0x00C4**) takes an SSID + **Passphrase** TLV (`MrvlIEtypes_Passphrase_t`),
and the firmware derives the PMK (PBKDF2 internally) and runs the 4-way EAPOL
handshake itself. So the host does **no** WPA2 crypto — no PBKDF2, no
PTK/MIC, no EAPOL state machine. This is the difference between a medium phase
and a very large one, and it is why WPA2 here is only somewhat bigger than the
open case.

### Sequence

1. **SUPPLICANT_PMK (0x00C4) SET** — TLVs: SSID (`OnestreamQJN7`), Passphrase
   (the PSK). Firmware caches the derived PMK for that SSID.
2. From the step-1 scan entry: BSSID, channel, capability, supported-rates IE,
   and the RSN IE — all already captured.
3. **ASSOCIATE (0x0012)** — TLVs: SSID, PHY param (channel), rates, the RSN
   IE, capability. The firmware associates AND completes the 4-way handshake
   using the cached PMK. `resp_result` / the assoc-result IE name a reject
   (and a wrong PSK shows here).
4. Data frames then flow on the DATA ports (RX proven by W5's
   `readDataPacket`; associated frames are ethernet-typed, an 802.3/LLC
   payload behind the RxPD — not 802DOT11). TX: poll WR_BITMAP (0x14–0x17),
   CMD53-write an MLAN_TYPE_DATA packet with a TxPD header to `ioport|port`.
5. Un-fakeable proof: a DHCP DISCOVER out, DHCP OFFER back (the AP's DHCP
   server hands us an IP) — captured via `readDataPacket`. A successful
   handshake + a routable IP from the real AP is unforgeable.

### The passphrase — how it is supplied, and never committed

The PSK is a credential. It is supplied the same way the NXP firmware blob is:
a **configure-time CMake cache variable**, compiled into the test image, and
kept out of git. Never pasted into chat, never written to a tracked file.

    cmake -B build -DCMAKE_TOOLCHAIN_FILE=... \
      -DM2RADIO_IW416_FW=<blob> \
      -DM2RADIO_WIFI_SSID="OnestreamQJN7" \
      -DM2RADIO_WIFI_PSK="<the password>"

The example gates the WPA2 attempt on both being set (absent → the probe stops
after the W5 monitor block, exactly as the no-blob build stops after
enumerate). The QEMU gate never sees these and stays green.

## Blockers before implementation can be verified

Two things only the user can provide, both hard prerequisites:

1. **The PSK for OnestreamQJN7**, via `-DM2RADIO_WIFI_PSK` (not chat).
2. **Confirmation the network is the user's to join.** "Onestream" is a UK
   ISP and the SSID looks like a default ISP-router name; connecting to a
   WPA2 network is authorised only by its owner. The user's selection of it
   is a strong signal, but it is worth an explicit confirmation.

Until the PSK is available the association firmware is **not written** — this
project ships only what silicon verifies, and WPA2 association cannot be
verified without the credential. When the PSK is supplied, steps 1–5 above are
a straight execution + one hardware run, following NXP's `mlan_sta_cmd.c`
exactly (the same method W3–W5 used).

## What does not change

* QEMU gate: all of this runs only after `fw_download=ok`, unreachable by the
  module-absent path. Gate stays green unmodified.
* Licence posture: NXP source is a protocol reference; nothing vendored.
* After M2Radio changes: push, then bump the `evkb.cmake` pin.
