# RT1176 USB host → UAC2 audio out ("two transports, one rate brain") — Design

**Date:** 2026-08-02
**Status:** validated design, ready for an implementation plan
**Fulfils:** broad-compatibility UAC2 output for `USBAudioOut`, building directly
on the UAC1 feedback-loop work completed and hardware-verified earlier today
(USBHost_t36 `3bfdafc`, evkb `e97f710`/`51eef79`, lib_xua `26d13ba`; evidence in
`examples/usb/usb_audio_graph_test/transcript_hw_evkb.txt`).

---

## 1. Goal

Drive USB Audio Class 2.0 output devices from the EVKB host port. The driving
requirement is **compatibility with other people's UAC2-only hardware** (most
modern DACs and interfaces enumerate UAC2-only, at high speed), not maximum
channel count: the first hardware-verified stream is **stereo 44.1 kHz from
today's 16-bit Audio-library graph, packed into the device's native 24-bit
subslots**, with remaining device channels zero-filled.

A UAC2 device that advertises no feedback endpoint (adaptive or synchronous
sync types) must stream correctly open-loop — that path already exists and is
the UAC1 fallback behaviour.

**Facts this design stands on, verified in source or on silicon today:**

1. The rate-control brain is done and proven: 10.14/16.16-agnostic mHz sizing
   accumulator, ±2% plausibility gate, 1/8 EMA (raw feedback dithers between
   rails; chasing raw measured +4.8 ppm bias), ~90 ppm/s slew, staleness
   fallback to nominal + trim. Closed-loop soak on UAC1: fill drift +0.1 ppm
   over 25 min, zero block corrections (`transcript_hw_evkb.txt`).
2. The device-side instrument survives unchanged: the lib_xua decoupler probes
   (`out_fifo_fill`, `out_dryout`, `out_overflow`, 100 Hz, `26d13ba`) sit below
   the protocol layer, so the same `tools/driftrun.sh` + `tools/vcdfill.py`
   evidence discipline applies to UAC2 from the first packet. A flash-booted
   XSCOPE build runs standalone; only killing a live `xrun` collector wedges it.
3. The bench witness exists prebuilt: `sw_usb_audio` config `2AMi8o8xxxxxx`
   (UAC2, HS, 8ch in/out, 24-bit-in-4-byte subslots) is in the app's built
   `bin/`; one `xflash` swaps the MC200 between UAC1 and UAC2 personalities.
4. High speed means **iTDs, no splits**: the embedded TT drops out entirely.
   EHCI 1.0 §3.3: iTD = 16 dwords (next link, 8 transaction status/control
   words, 7 buffer page pointers), 32-byte aligned — `sizeof` is exactly 64
   with `aligned(32)`, so the siTD pool/link conventions in `ehci_iso.cpp`
   (USBHOST_DMAMEM, head insertion, `sitd_skip_iso`-style traversal) carry
   over with a second node type. `sitd_skip_iso` already treats iTD-typed
   links as skippable (test_sitd covers it).
5. `PERIODIC_LIST_SIZE` is 32 (`ehci.cpp:61`, overridable). One iTD per frame
   slot carries 8 µframe transactions, so 32 iTDs preserve today's exact ring
   cadence: every slot revisited each 32 ms, serviced from `loop()`.
6. D-cache is off in this core (`cores/imxrt1176/startup.c:258`), so iTD
   payload buffers need no cache maintenance in either direction.
7. `frame_bytes_scaled()` (`usb_audio_parse.cpp`) is already parameterised by
   units-per-frame; a µframe variant (units 8 000 000 in mHz) is arithmetic,
   not architecture.
8. UAC2 restructures the control plane: sample rate is a Clock Source entity
   CUR/RANGE request pair via the AC interface, not an endpoint SET_CUR. The
   clock is found through the terminal chain: the streaming interface's
   AS_GENERAL names `bTerminalLink`, and that terminal's `bCSourceID` (in the
   AC interface) names the Clock Source — so even a minimal parse walks AC
   terminals and clock entities. Format lives in a UAC2-specific FORMAT_TYPE I
   (subslot/resolution, no rate table); IADs are mandatory. HS feedback is
   4 bytes, Q16.16 **samples per microframe**, on an iso IN endpoint whose
   cadence is its own `bInterval`.

## 2. Scope

**In:**

- iTD layer in `ehci_iso.{h,cpp}`: `itd_t`, pool, `itd_fill_out()` (up to 8
  µframe transactions), `itd_fill_in()` (single-transaction feedback read),
  per-transaction status harvest, link/unlink.
- `usb_audio2_parse.{h,cpp}`: UAC2 descriptor walk filling the shared topology
  struct (extended with `clock_source_id`), discriminated by `bcdADC`;
  claim-time **clean rejection** of topologies out of scope (clock selectors/
  multipliers, alts whose per-frame byte ceiling exceeds the ring buffer).
- Clock Source CUR (set 44 100) and RANGE (enumerate rates) request builders.
- `uac2_feedback_to_mhz()` (Q16.16/µframe → mHz) feeding the existing gate/
  EMA/slew unchanged.
- 16-bit → subslot packer for 2/3/4-byte subslots with channel fan-out and
  zero-fill of unused device channels (pure function, host-tested).
- `USBAudioOut` speed dispatch: enumerated device speed selects siTD (FS/UAC1)
  or iTD (HS/UAC2) paths; one class, one FIFO, one rate brain.
- Example: `usb_audio_graph_test` gains the UAC2 witness flow; heartbeat gains
  subslot/speed fields. Evidence appended to `transcript_hw_evkb.txt`.

**Out (explicitly):**

- Audio IN (capture), UAC2 interrupt/status pipe, volume/mute Feature Units,
  clock selectors/multipliers (reject, don't wedge), multichannel graph
  exposure beyond stereo-into-N-zero-filled, rates other than 44 100 on
  hardware (RANGE parsing lands; negotiation of others is future work), UAC3,
  FS UAC2 devices (siTD + UAC2 control plane would compose, but no witness
  exists — rejected at claim with a distinct reason for now), QEMU gate (the
  model has no EHCI isochronous path; hardware-only, like the UAC1 example).

## 3. Phases, each ending at a hardware gate

- **P1 — transport.** Minimal UAC2 enumerate (streaming alt, endpoints,
  format, `bClockSourceID` — no topology walk), Clock CUR 44 100,
  SET_INTERFACE, iTD OUT ring at nominal rate into `2AMi8o8xxxxxx`.
  *Gate:* fill probe emitting at its 100 Hz cadence (one emission per ten
  received packets, so cadence itself proves arrival rate) with byte
  throughput matching 44.1 kHz × 8ch × 4 B; fill ramp/dry-out behaviour
  consistent with open loop at nominal; per-transaction error counters zero.
- **P2 — parser and control breadth.** Full UAC2 walk, RANGE, IADs, rejection
  taxonomy; fixture corpus = MC200 UAC2 dump + third-party configuration
  dumps collected opportunistically (no hardware needed).
  *Gate:* host suite green over the corpus; MC200 re-enumerates through the
  full parser with identical results to P1's minimal one.
- **P3 — feedback.** `itd_fill_in` on the feedback EP at its `bInterval`
  (ceiling 16 ms), 16.16 decode, servo engaged.
  *Gate:* closed-loop fill flat (the UAC1 soak criteria: |drift| ≤ ~1 ppm,
  zero corrections, ≥ 10 min), decoded rate consistent with the device's
  known ~−86 ppm crystal.
- **P4 — graph + full verification.** 16→24-in-4 packing live, stereo tone
  from the graph, locked-bias sweep replayed open-loop on UAC2 (slope ~1,
  intercept near the UAC1-measured rate), then the closed-loop soak,
  transcripted.
  *Gate:* sweep + soak both recorded in `transcript_hw_evkb.txt`.

## 4. Memory and sizing (the one genuinely new budget)

Per-frame payload buffer = 8 µframes × a fixed per-µframe ceiling
(`MAX_UFRAME_BYTES = 224`, covering 48 kHz × 8ch × 4 B), so 1792 B per slot
× 32 slots ≈ 57 KB in the sketch-placed DMAMEM object (OCRAM; fine, but
declared, not discovered). The guard is against the NEGOTIATED rate's
per-µframe need, not the alt's advertised `wMaxPacketSize` — the live
witness advertises 800 B (sized for 192 kHz) on the very alt we drive at
44.1 kHz with ≤192 B µframes, so an advertised-MPS ceiling would reject the
witness itself; rates/alts whose need exceeds the ceiling are refused at
stream start. Stereo third-party DACs are an order of magnitude smaller.
iTD pool: 40 nodes × 64 B hardware area, mirroring the siTD pool's headroom
rationale.

## 5. Error handling

Same taxonomy as UAC1, finer grain: xact/babble/buffer/short harvested per
µframe transaction (8 per iTD) into the existing counters; feedback gate,
staleness, and open-loop fallback shared; claim rejections logged once with a
reason string surfaced in the example's console — a compat driver's failure
mode must be diagnosable from the heartbeat alone.

## 6. Testing

- Host suite additions mirror existing patterns: `test_sitd`-style layout/
  alignment/fill/budget checks for `itd_t`; `usb_audio2_parse` fixture tests;
  packer tests (2/3/4 subslot, zero-fill, channel fan-out);
  `uac2_feedback_to_mhz` cases. Servo/EMA/slew tests already exist.
- Hardware gates per phase (§3), using the unchanged device-side instrument.
- No QEMU gate (§2 Out); `docs/KNOWN-BROKEN-GATES.md` untouched — this example
  category has no gate to regress.

## 7. Risks and open questions

- iTD per-µframe scheduling interacts with a 32-entry frame list on this
  ChipIdea-derived controller — the class of silicon surprise the phase gates
  exist to catch early (P1 is deliberately the riskiest slice first).
- The MC200 UAC2 feedback `bInterval` and startup-garbage behaviour are
  assumed similar to UAC1 (the ±2% gate absorbed 113–987 junk reports there);
  P3's gate checks this rather than assuming it.
- Fixture breadth is aspirational until dumps are collected; compat claims in
  README/commit messages stay scoped to "verified against the MC200, parser
  exercised against N fixtures".
- Refactoring `USBAudioOut` into a shared core with sibling classes was
  considered and deferred until UAC2 is green (approach C in the brainstorm);
  if the single class exceeds good boundaries during implementation, that is
  the moment to revisit, not before P4 passes.
