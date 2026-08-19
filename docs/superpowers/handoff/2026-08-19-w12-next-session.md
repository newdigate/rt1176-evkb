# Continue M.2 Wi-Fi (W12: resolve the PS-vs-throughput tension — fault #5)

u-blox **M2-MAYA-W161** (IW416/SD8978) on **MIMXRT1170-EVKB RevC3**, repo
`~/Development/rt1176-evkb-m2-maya-w161`, branch **`m2-phase0-serial2`**.
Sibling repos, both pushed + pinned: **M2Radio `e03d0c5`** (bus counters +
the REVERTED ring-view fix), **lwip `c6b2548`** (8*MSS windows).

**Read first:** `examples/networking/m2_throughput_test/transcript_hw_evkb.txt`
— the whole W11 story is there (baseline, the regressed fix, fault #5).

## What W11 shipped

* **`networking/m2_throughput_test`** + committed Mac peer `tput_peer.py`:
  4-cell measurement rig (TCP/UDP × TX/RX), Mac-authoritative, byte-exact
  cross-checks, re-runnable without reflash. Sweep = **97 gates** (measured
  97/0/0 on close-out). House-AP rig: EVKB on `OnestreamQJN7` (2.4 GHz),
  Mac peer on the `_5G` side, WPA2. W11's own handling of the house PSK was
  scratchpad + gitignored caches only — but see the CREDENTIAL EXPOSURE
  section below: an EARLIER phase had already committed it.
* **Baseline capability (PS-on, the lucky clean run):** tcp-rx 9.69 /
  tcp-tx 4.91 / udp-rx 7.33 delivered / udp-tx 6.72 Mbps. 2.4 GHz air
  variance spans ~4.8-9.7 on identical configs — treat as a floor.
* **Driver bus counters** (M2Radio): cmd52PollsTx/Svc, cmd53Count/Bytes/
  ByteMode + accessors; reset per fw download. They attributed the ceiling:
  ONE frame per CMD53, ~5.6 CMD52/TX frame, **~10 bus commands per frame at
  ~0.1 ms each** (measured under load; the idle ~1 kHz c52svc rate is
  serviceLink's own `delay(1)` pacing, NOT command cost — don't budget
  DAT1/aggregation gains off that number), ~840 frames/s sustained.
* **A failed fix, kept as history:** cached bitmap views (M2Radio `3a34d2d`,
  REVERTED in `e03d0c5`). Counter-proven regression: burst-then-dry-wait
  vs the fw's batchy free cadence + delay(1) poll granularity ≈ 5 ms/frame.
  The pre-fix read-per-frame pattern was accidentally load-bearing pacing.
  **Lesson: amortizing bitmap reads needs DAT1 interrupts or mlan-style
  deep TX queueing, not a passive cache.**

## FAULT #5 (new, A/B/A-confirmed): IEEE PS + busy RX kills fw RX

On the known-good driver, fresh fw each boot, house AP:

| Arm | PS | Load | Outcome |
|---|---|---|---|
| tcp-rx control #1 | on | ~7-10 Mbps RX | froze at 5.5 MB, RX fully dead |
| tcp-rx control #2 | on | same | froze at ~1.0 MB, same signature |
| tcp-rx ×3 | **off** | same | **ALL CLEAN** (7.6/8.1/4.8 Mbps) |
| tcp-rx ×3 (A/B/A) | on | same | 1 completed (degraded), 2 froze |

Signature = the W10 erratum's (RX dead incl. broadcasts, assoc held, card
reset revives) but the OPPOSITE regime. Standing tension: **PS on is
required idle (W10: sparse+PS-off dies 1-44 min) but kills busy RX; PS
off is the reverse.**

**W12 step 1 — the hypothesis to test first:** our `sendSleepConfirm`
answers EVENT_PS_SLEEP UNCONDITIONALLY (Iw416.cpp, W10 code). NXP's mlan
defers the sleep-confirm while RX/TX is pending. Confirming sleep mid-
upload-burst plausibly wedges the fw's RX path. Fix shape: conditional/
deferred confirm while the data path is active (e.g. frames seen within
the last N ms, or rd_bitmap non-empty). Verify: PS-on tcp-rx ×5 clean AND
a repeat of the W10 sparse soak still clean (don't regress the idle fix!).
An mcuxsdk cross-read of `wlan_send_sleep_confirm` / `ps_sleep_confirmed`
gating logic is the reference (same place the W10 byte layouts came from).

## ⚠ CREDENTIAL EXPOSURE — do this before any other W12 work

Found by W11's final review, verified on silicon-era evidence:
`examples/networking/m2_sdio_probe/transcript_hw_evkb.txt` committed, in
W6, **the user's live home Wi-Fi password** (3 occurrences) and **a live
iPhone-hotspot password** (2). W6 excused the first as "almost certainly
WRONG" — W11 disproved that by associating to OnestreamQJN7 with exactly
that value.

* **Redacted at HEAD** in W11 (with a correction note in that file).
* **Still in PUSHED history** on `github.com/newdigate/rt1176-evkb`
  (branch `m2-phase0-serial2`) — a redaction commit does NOT remove it.
* **The only real fix is ROTATION** at the router and the phone. The user
  has been told; if they have not rotated, say so again before starting.
* Optional additionally: history scrub (`git filter-repo` + force-push) —
  destructive to a published branch, so ONLY on explicit user instruction.
* Standing rule this violated, now explicit: a captured credential is
  redacted BEFORE it is committed, and "we think this value is wrong" is
  never a reason to commit it.

## Also open (ranked)

* **DAT1 interrupt-driven SDIO service** — now doubly motivated: kills the
  ~1 kHz CMD52 idle poll AND is the correct base for any future bitmap
  amortization (see the reverted-fix lesson).
* Deep TX queueing / MPA aggregation (mlan-style) — the real throughput
  unlock beyond ~10 Mbps; W13+ scale.
* waitCmdResp clear-hole hardening (W10 note; soak signature psSleeps flat
  + psHostWakes stalled — never yet observed).
* PSK rotation — see the CREDENTIAL EXPOSURE section above (house + hotspot
  passwords, the urgent ones). The bench ESP PSK is also in history at
  b80ecdc, but it is a throwaway on a bench-only AP: lowest priority of the
  three.

## The test rig (state as left)

* EVKB: freshly reflashed `m2_throughput_test` (PS ON committed default —
  SAFE IDLE, but expect tcp-rx freezes if you blast it; reflash revives).
  Joined 192.168.1.102 via `OnestreamQJN7`.
* Mac peer: `python3 tput_peer.py all 192.168.1.102` from the example dir
  (firewall may prompt Allow for python3). Mac on `OnestreamQJN7_5G`.
* ESP8266 bench AP still available on `/dev/cu.usbserial-0001` (unused in
  W11; the W10 sparse-soak rig if fault-#5 work needs the idle regime).
* Flash `LinkServer flash … load` VCOM-FREE (kill any read_port.py first
  — holding the VCOM during flash is the documented trap and it was
  nearly re-learned this session); sweep from `/tmp/ev`; audit with
  `LICENSE_AUDIT_EVKB=$(pwd)`.
