# Continue M.2 Wi-Fi bring-up (W7: finish the WPA2 4-way handshake)

u-blox **M2-MAYA-W161** (NXP **IW416**/SD8978) on **MIMXRT1170-EVKB RevC3**,
repo `~/Development/rt1176-evkb-m2-maya-w161`, branch **`m2-phase0-serial2`**.
Driver in sibling `~/Development/M2Radio` (master, pushed, pinned in
`evkb.cmake` at `d33871a`).

**Read first:** `examples/networking/m2_sdio_probe/transcript_hw_evkb.txt`
(W6 section, especially the "CORRECTION" and reason-code entries) and
`docs/superpowers/specs/2026-08-18-m2-w6-associate-design.md`.

## Where things stand

| Phase | State |
|---|---|
| W1–W5 | ✅ enumerate, firmware, host cmds, scan, monitor RX |
| W6 — WPA2 associate | ✅ **associates cleanly** (assoc_status=0); ⚠️ 4-way handshake does not complete |

## The open problem (this is NOT the password)

Association to a WPA2 AP succeeds (`assoc_status=0`, real cap_info). The
embedded supplicant is confirmed (no EAPOL forwarded to the host,
`eapol_seen=0`). But the 4-way handshake fails and the AP deauthenticates:

* iPhone hotspot "Nicholas's iPhone" — **known-correct password, −66 dBm** —
  deauth **reason 2** (prev auth not valid).
* OnestreamQJN7 — deauth **reason 15** (4-way handshake timeout).

A controlled test with a known-good credential (the iPhone hotspot the user
set up) **refuted** the earlier "wrong PSK" conclusion. It is a code /
firmware-integration gap in how the embedded supplicant is engaged/keyed.

Already tried and REFUTED: adding the Auth-type TLV (0x011F = Open). Ruled
out: password, SSID salt (uses scanned beacon bytes now), plumbing
(ssid_len/psk_len exact), PMF/SHA256 for OnestreamQJN7 (plain PSK), host-vs-
embedded supplicant (embedded).

## The RSN-IE fix is DONE and byte-verified — and it was NOT sufficient

`buildAssocRsnIe()` now reproduces `wlan_update_rsn_ie()`: single cipher/AKM +
RSN-caps PMF bits forced to MFPC=1/MFPR=0. Hardware-verified bytes
(`assoc_rsn_ie=...000FAC02 8000`/`8C00`). It did NOT fix the handshake: the
**known-good iPhone hotspot** still `associate=ok` then deauth (reason 2).
Three fixes now refuted (auth TLV, RSN-IE/PMF, deauth-order).

## STOP — question the model (this is where W7 starts)

The model "SUPPLICANT_PMK caches the PMK → ASSOCIATE with a correct RSN IE →
the firmware's embedded supplicant runs the 4-way handshake" is byte-correct
end to end and the credential is known-good, yet no handshake. So the model is
incomplete. Do NOT add a fourth blind TLV/flag. Resolve the fundamental
question first:

**Does `sduartIW416_wlan_bt.bin` actually run an embedded supplicant, or is
the handshake expected on the host?** `SUPPLICANT_PMK` returns success and is
not `CONFIG_WPA_SUPP`-gated, which suggests embedded — but success could be
vacuous. `eapol_seen=0 / data_frames=0` does not distinguish "embedded
supplicant, handshake internal" from "no supplicant, nothing happens".

### Done: ESP8266 test AP -- failure reproduces (card-side, not AP-specific)

An ESP8266 WPA2 SoftAP was flashed (arduino-cli `esp8266:esp8266`, sketch in
scratch) and the probe pointed at it: `associate=ok` then deauth (reason 6),
`stations=0` on the AP. So the handshake fails against a controlled simple AP
too -- it is firmware/card-side and reproducible on every AP (iPhone reason 2,
ESP reason 6, Onestream reason 15), with a known-good credential.

### The decisive diagnostic still needed: watch the frames

The open question is whether the card TRANSMITS EAPOL msg 2. The ESP8266 could
not answer it: its SoftAP API only surfaces fully-connected stations, and its
**promiscuous/sniffer mode delivered no frames** (Arduino core 3.1.2 -- too
limited). Use instead:
* an **ESP32** in promiscuous mode (proper full-frame delivery), or
* a **monitor-mode Wireshark** capture on ch 4/6 (a Mac with an adapter, or
  `airport`/`tcpdump` in monitor mode), filtered on the card MAC
  `6C:1D:EB:91:0C:45`.

Read the exchange:
* card sends auth-req → assoc-req → **EAPOL msg 2** then AP drops it → the
  embedded supplicant runs; the bug is keying/MIC → check SUPPLICANT_PMK (try
  a precomputed PMK via the PMK TLV; verify the exact SSID salt), OR
* card sends auth/assoc but **no EAPOL** → no supplicant in this blob → run the
  4-way handshake on the host (PBKDF2 + EAPOL + MIC + `KEY_MATERIAL` 0x005e) or
  get an IW416 firmware variant with the embedded supplicant. Large phase --
  confirm first.

The reason-6 clue (nonauthenticated STA) also warrants trying a separate
AUTHENTICATE (0x0011) before ASSOCIATE, but only as a hypothesis backed by
what the frame capture shows -- do not add it blind (that would be fix #4).

Instrumentation already in place to guide it: `diagConnect()` reports the
deauth reason (`lastEventInfo` low 16 bits) and EAPOL presence; the probe
prints `wpa_rsn_ie=` (the captured beacon RSN IE bytes) — decode the AKM
suite and RSN caps to see exactly what to present.

## Test setup that works

* iPhone Personal Hotspot with **Maximize Compatibility ON** (forces 2.4 GHz;
  the scan is 2.4 GHz only). It shows up as `scan_ap` `sec=wpa2`, ~−66 dBm.
* Configure from `examples/networking/m2_sdio_probe/`:
  `-DM2RADIO_WIFI_SSID="Nicholas's iPhone" -DM2RADIO_WIFI_PSK="<pwd>"` (straight
  apostrophe is fine — `ssidLooseMatch` handles the beacon's curly one).
* Success signal: `connect=ok last_event=0x2b` (EVENT_PORT_RELEASE), then W7b:
  data TX (TxPD over WR_BITMAP) + a DHCP round-trip.

## Hardware gotcha (cost real time in W6)

The MCU-Link probe grows unreliable after ~20+ flash cycles: `LinkServer run`
exits right after probe-select, flash fails with `Could not connect to core`,
board left halted. A **full power-cycle** (not just a USB replug) recovers it.
Prefer `flash ... load` (auto-runs via "restart on reset") over `run` when the
probe is being flaky. Avoid `--erase-all` when not needed — the 64 MB mass
erase is slow.

## Unchanged constraints

Blob + PSK via configure-time flags, never committed (build*/ is gitignored;
verified). J15 empty. QEMU gate asserts module-absent; keep green. After
M2Radio changes: push, then bump the `evkb.cmake` pin.
