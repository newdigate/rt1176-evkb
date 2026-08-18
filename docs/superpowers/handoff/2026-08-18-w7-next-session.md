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

## First thing to try in W7

`associate()` currently echoes the **raw beacon RSN IE** into the ASSOCIATE
request. NXP's `wlan_cmd_802_11_associate` instead runs it through
`wlan_update_rsn_ie()` (mlan_join.c), which:
1. Reselects a single pairwise cipher and a single AKM suite (by preference).
2. Normalises the RSN Capabilities field (PMF MFPC/MFPR bits) from
   `pmpriv->pmfcfg`, which NXP sets via `wlan_set_pmfcfg(mfpc, mfpr)` before
   connecting.

Reproduce that (or, minimally: build a clean RSN IE with one AKM + one cipher
and RSN caps set for no-PMF, and add the equivalent of `wlan_set_pmfcfg`).
The iPhone (modern WPA2/WPA3-PMF) giving reason 2 points squarely here.
Consider also `HostCmd_CMD_802_11_KEY_MATERIAL` (0x005e) if the RSN-IE fix is
not enough.

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
