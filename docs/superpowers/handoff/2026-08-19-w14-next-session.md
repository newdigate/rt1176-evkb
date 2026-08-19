# Continue M.2 Wi-Fi (W14: the data path is CLOSED — pick the next build)

u-blox **M2-MAYA-W161** (IW416/SD8978) on **MIMXRT1170-EVKB RevC3**, repo
`~/Development/rt1176-evkb-m2-maya-w161`, branch **`m2-phase0-serial2`**.
Siblings pushed + pinned: **M2Radio `7aec26b`**, **lwip `c6b2548`**.

**Read first:** `examples/networking/m2_throughput_test/transcript_hw_evkb.txt`.
It is the full W11→W13 record including six of my wrong conclusions and the
measurement that killed each. Read it before forming a theory about this
driver; several obvious-sounding ones are already dead.

## State: the Wi-Fi data path is stable and no longer has an open defect

* **Fault #5 (RX dead until reflash) is fixed and understood.**
  `HOST_INT_STATUS` is clear-on-read; `serviceLink()` drained only on its own
  sighting of `HOST_INT_UP_LD` while four other paths consumed and discarded
  it. Fix = sticky `m_intPending` at every read site + a ring safety net.
  **Measured across W12+W13: 380+ blasts, five soaks, zero freezes**, vs RX
  dead within 1-3 blasts before.
* **The leftover `stranded` counter is FIRMWARE BEHAVIOUR, not a host bug.**
  Three host-side explanations were proposed and all three refuted by
  counters (`drainerr=0` killed the first, inspection killed the second,
  `notready=0` killed the third). With all five interrupt readers verified
  to accumulate, the reading that survives is that the firmware does not
  always raise `HOST_INT_UP_LD` for an upload it has queued — there is no
  interrupt to lose, and the net is the correct fix. ~3 per 80 blasts,
  recovery ≤64 ms. **Do not "fix" this without new evidence; the next
  evidence would need a bus analyser, not another counter.**
* `rxRingResyncs` ~184/blast is NORMAL, and `notready=0` proves each resync
  then reads a real packet. The ring position lags and resync corrects it.
  I twice misread this counter as damage — see the transcript.

## Test infrastructure that now exists (use it)

`tools/wifi-soak.sh` — the hardware soak harness, first used in W13.
QEMU has **no IW416 model**, so every m2_* gate asserts the card-ABSENT
fallback and the entire ring/interrupt layer is uncovered by the sweep.
This script is that missing gate: reflash per boot, blast, record which
blast dies, and dump ring state on a freeze. It carries two guards learned
the hard way — it refuses to soak an image built without creds/blob (which
prints the same heartbeat as a card-absent board), and it fails if zero
blasts ran rather than reporting a healthy link.

```sh
tools/wifi-soak.sh -b 1 -n 0 -m 20        # one boot, blast until 20 min
```

## Pick one (roughly ranked by value)

1. **QEMU IW416 model** (large; the structural win). Model the SD8978 in the
   qemu2 tree: enumeration, fw download handshake, the 32-slot bitmap rings
   and clear-on-read interrupt semantics, plus a loopback peer. Would have
   caught BOTH the W8 ring bug and the W12 stranded-upload bug
   automatically. Turns silicon-only driver work into gated work — the
   single biggest gap in this subsystem.
2. **DAT1 interrupt-driven SDIO** (medium; structural). Replace the ~1 kHz
   CMD52 idle poll with the real interrupt line. Cuts idle CPU/power and is
   the right base for any bitmap amortization. NOTE W11's lesson: a passive
   bitmap cache REGRESSED throughput 2.5x because the read-per-frame pattern
   was accidentally pacing the host to the firmware's batchy free cadence.
3. **Throughput past ~14 Mbps** (medium-large). Deep TX queueing / MPA
   aggregation, mlan-style. Current ceiling is ~10 bus commands per frame at
   ~0.1 ms each, one frame per CMD53. Best after (2).
4. Small: `waitCmdResp` clear-hole hardening (W10, never observed).

## Bench state and traps

* **The house AP password has been rotated by the user** (the W6 credential
  exposure — see below). The throughput example's build cache still holds
  the OLD SSID/PSK, so it will NOT join until reconfigured. The **ESP8266
  bench AP** (`ESP8266TEST`, own throwaway PSK, on `/dev/cu.usbserial-0001`)
  needs nothing from the user and is the right rig for driver work.
* A wiped build cache is INDISTINGUISHABLE from a dead card on serial: no
  blob/creds → the Wi-Fi path compiles out → same `alive=` heartbeat. This
  cost a pointless board power-cycle in W12. Check
  `grep -E "^M2RADIO_(IW416_FW|WIFI)" build/CMakeCache.txt`, and flash
  `m2_sdio_probe` (it re-emits `sdio_begin=`/`cardid=`) before blaming
  hardware. `wifi-soak.sh` now refuses to run against such a build.
* **Never compare absolute counters across runs of different length.** That
  error alone produced two wrong conclusions in one day. Normalize per
  blast.
* Flash `LinkServer flash … load` VCOM-FREE; sweep from `/tmp/ev`; audit
  with `LICENSE_AUDIT_EVKB=$(pwd)`.

## Credential exposure — user has acted, verify before assuming

The W6 probe transcript committed the user's live home Wi-Fi password and an
iPhone hotspot password to the public repo. Redacted at HEAD (5622b68) but
**still in pushed history**. The user said on 2026-08-19 they would rotate
the home AP; confirm that happened, and note the hotspot password is a
separate credential that also needs rotating. A history scrub
(`git filter-repo` + force-push of a published branch) is destructive and
needs explicit instruction.
