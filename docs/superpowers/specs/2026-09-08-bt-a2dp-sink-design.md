# M2Radio BT: A2DP SINK — the EVKB as a Bluetooth speaker

**Status:** DESIGN, approved 2026-09-08 (brainstorm). **Linear: NEW-41** (Backlog). Plan: `docs/superpowers/plans/2026-09-08-bt-a2dp-sink.md`.
**Builds on:** NEW-9 (A2DP source: L2cap/BtLink/Sdp/Avdtp/Sbc encoder/AudioOutputBluetooth),
NEW-34 (non-blocking link lifecycle, the AVDTP ACCEPTOR path, BtSession, bond store,
AVRCP target, ACL reassembly), and the 2026-09-08 Bose findings (AVDTP 1.2 fallback,
self-clock catch-up).

## 1. Goal

A phone pairs with the EVKB, streams music to it over A2DP, and the audio comes out
of the WM8962 headphone jack cleanly for as long as the phone plays, with the
phone's volume buttons controlling the level. The **acceptance source is an
iPhone** (brainstorm decision): phones are the pickiest sources, so a sink that
satisfies one satisfies the rest, and it is the real use case.

## 2. What already exists

NEW-34 built most of a sink's control plane while making the source robust:
incoming pages + SSP (`BtLink`), page-scan-when-idle + the bond store
(`BtSession`), a multi-record SDP server, an AVDTP **acceptor** that answers
DISCOVER, GET_CAPABILITIES and GET_ALL_CAPABILITIES, SET_CONFIG (with config
adoption), OPEN, START, SUSPEND and CLOSE, a working media channel, ACL
reassembly, and an AVRCP target. The Bose Mini SoundLink drove exactly that
path on 2026-09-08 with the roles inverted (it initiated, we accepted).

Missing: an SBC **decoder**, media **receive**, an audio-graph **input** node with
clock recovery, the sink's **identity** (SDP AudioSink record, SNK endpoint,
class of device), and AVRCP **absolute volume**.

## 3. Decisions (brainstorm, 2026-09-08)

| decision | choice | why |
|---|---|---|
| acceptance source | **iPhone** | pickiest, un-fakeable, the real use case |
| clock recovery | **servo the audio PLL** (PLL4 fractional numerator on jitter-buffer fill) | bit-exact, no CPU, the UAC feedback loop's servo shape; `set_audioClock()` already programs NUM/DENOM |
| sample rates | **44.1 kHz only** | the graph's fixed rate; iPhones configure 44.1; a 48-only source fails SET_CONFIG by name (known limit) |
| AVRCP | **target with absolute volume** | what iOS expects of a speaker; one PDU + one notification on the existing target |
| structure | **A: mirror the source** (`bt/A2dpSink`, `bt/SbcDecoder`, example `AudioInputBluetooth`) | same decomposition and host-testability as the source; the source path three real sinks depend on is untouched |

Rejected: a bidirectional A2DP class with a role flag (muddies piece 2's
one-attempt object and makes every source gate exercise sink code); everything
in the example (the decoder and the servo, the two parts that need tests, would
be untestable); software rate conversion (CPU + quality for no gain when the
PLL can be trimmed); drop/duplicate samples (audible ticks).

## 4. Design

### 4.1 `bt/SbcDecoder` (M2Radio)

The encoder's mirror, clean-room like it: frame header parse (sync 0x9C, rate,
blocks, mode, alloc, subbands, bitpool), CRC-8 check (reuse `Sbc::crc8`), scale
factors, the SAME bit-allocation routine (`Sbc::allocateBits`), sample
unpacking and dequantisation, joint-stereo un-mixing, and the **synthesis
filterbank** (8-subband, the inverse of `Sbc::analyse`, Q15 integer). Output:
`decode(const uint8_t *frame, uint16_t len, int16_t *left, int16_t *right)` →
128 samples per channel per frame at blocks 16 / subbands 8, and the frame
length consumed. Rejects (returns 0) a bad sync, CRC or length so a corrupt
frame never lands in the ring.

Host oracle, the mirror of the encoder's: ffmpeg encodes a known 1 kHz tone to
SBC (44.1, joint, 16/8, loudness, bitpool 53); our decoder's SNR against the
source PCM ≥ 60 dB (the encoder measured 63.9 dB the other way); AND our
encoder → our decoder round trip is within the same bound. A corrupt-CRC frame
and a truncated frame are refused by name.

### 4.2 `bt/A2dpSink` (M2Radio)

One attempt in the ACCEPTOR direction, the mirror of `A2dpSource`:

- Registers the **AudioSink** SDP record (0x110B, AVDTP 1.3) beside the AVRCP
  Target record in `Sdp` (RECORD2; the source record is not registered by a
  sink build).
- Advertises ONE SNK SEP (SEID 1, audio, SBC): 44.1 kHz only, all channel
  modes, blocks 4–16, subbands 4/8, both allocations, bitpool 2–53, delay
  reporting. `Avdtp` gains the SNK personality for its own SEP (today
  `OUR_SEID` is a SRC): the caps body and the DISCOVER reply's TSEP bit.
- Accepts the source's DISCOVER / GET_(ALL_)CAPABILITIES / SET_CONFIG (adopts
  the configured SBC params exactly as piece 2 does) / OPEN / START / SUSPEND /
  CLOSE / ABORT; adopts the media channel; sends **DelayReport** (our ring
  latency, 0.1 ms units) after START if the source configured delay reporting.
- Hands each media PDU to an `onMedia(const uint8_t*, uint16_t)` callback
  (RTP + SBC payload header parsing lives in the audio node, §4.3), and the
  L2CAP RX MTU we advertise (1004) bounds the packet.
- `BtSession` gains a **sink policy**: no boot walk and no inquiry; page scan
  AND inquiry scan on (discoverable) whenever no link is up and no bond exists
  or a bench knob says so; accept incoming from any address (or bonded only —
  a knob); reconnect is the SOURCE's job, so on loss the sink just returns to
  scanning. Class of device **audio/video · loudspeaker** (0x240414 — the same
  the ESP32 sink shows), friendly name `M2_BT_SINK_NAME` (default `EVKB-SINK`),
  SSP Just Works.
- AVRCP target (`bt/Avrcp`) gains **SetAbsoluteVolume** (PDU 0x50: accept,
  report the volume we applied) and the **VOLUME_CHANGED** event
  (RegisterNotification → INTERIM, CHANGED when a local control moves it), on
  top of the Shokz-era GetCapabilities / PLAYBACK_STATUS. The volume callback
  maps 0–127 to the WM8962 headphone gain in the example.

### 4.3 `AudioInputBluetooth` (example-side node, like `AudioOutputBluetooth`)

- `onMedia`: RTP header (V2, PT, seq, timestamp — seq gaps counted), SBC media
  payload header (frame count, fragmentation flags — fragmented frames
  reassembled across packets), each frame → `SbcDecoder` → the **PCM ring**:
  16 blocks of 128 stereo samples (~46 ms), target fill 8 blocks. All in the
  main-loop `poll()` (decode in the ISR is the livelock lesson of 2026-09-04).
- `update()` (the SAI ISR's graph walk) pops one block per period and
  transmits it; on **underrun** it transmits silence and counts; on **overflow**
  the oldest block is dropped and counted.
- **Clock recovery — the PLL servo.** Every `update()` samples the fill level;
  a first-order filtered error (target − fill) drives PLL4's fractional
  numerator (`CCM_ANALOG_PLL_AUDIO_NUM`, via a new `trim_audioClock(int32_t
  ppm)` in the Audio library's `imxrt_hw.cpp`, clamped to ±200 ppm) so the SAI
  consumes at the phone's delivery rate. Loop gain small enough that a packet
  burst does not move the pitch audibly (integral term only; the fill error is
  already the integral of the rate error). Hold the last trim across a SUSPEND
  or link loss; re-centre the ring (drain to target) at each START. The ring
  size and target are the servo's headroom: ±200 ppm on a 46 ms ring gives
  minutes of un-servoed drift before an over/underrun, so a badly tuned loop
  degrades to ticks, never to silence.
- Heartbeat line `bt_sink pkts= frames= seqgaps= under= over= fill= trim_ppm=`
  once a second, on its own line (the card-absent gate anchors the `hb` line).

### 4.4 The example: `audio/bt_sink_test`

`AudioInputBluetooth → AudioOutputI2S` (WM8962, headphones), `BtSession` in
sink policy, AVRCP volume → `sgtl`-style codec gain, console lines mirroring
`bt_tone_test`'s (`bt_fw_download`, `hci_reset`, `sink=discoverable`,
`conn_req`, `a2dp_sink=configured cie=…`, `streaming by=incoming …`, the
heartbeats, `bt_link`, `bt_cred`). Bench knobs: `M2_BT_SINK_NAME`,
`M2_BT_ACCEPT_ANY` (vs bonded only), `M2_BT_ACL_TRACE` (media skipped, as today).

### 4.5 Error handling

Corrupt/short frames refused at the decoder, counted; seq gaps counted (a
lost packet is ~5 frames of silence, not a stall); SUSPEND freezes the servo
and drains to silence; CLOSE/ABORT or link loss tears the media path down
(ring cleared, trim held), the session returns to scanning; a 48 kHz
SET_CONFIG is REJECTED with the media-codec category (the known limit, by
name); AVRCP commands we do not implement stay NOT IMPLEMENTED so a phone never
hangs. The credit/reassembly instruments of NEW-34 apply unchanged.

## 5. Verification (the two-gate rule)

- **Host** (`bt/test/`): `sbcdecoder_test` (ffmpeg oracle SNR ≥ 60 dB; encoder→
  decoder round trip; CRC/truncation refusals; every mode and both subband
  counts); `a2dpsink_test` (the acceptor attempt driven by the fake source's
  byte sequences to STREAMING, SUSPEND/resume, CLOSE, a 48 kHz SET_CONFIG
  rejected by name, DelayReport sent when configured); `avrcp_test` grows the
  absolute-volume arms; a **servo model** test in the example's `tests/`
  (synthetic ±100 ppm drift converges within N seconds, holds on loss, never
  exceeds the clamp). Each pin RED against a mutant.
- **QEMU gate** `audio/bt_sink_test` (ONE new gate, sweep 138 → 139):
  `hci_peer.py` gains a `source` phase — it pages us, pairs (SSP), SDP-queries
  our AudioSink record (asserting its shape), drives DISCOVER → GET_ALL_CAP →
  SET_CONFIG (44.1 joint 16/8 loudness bitpool 53) → OPEN → START, then streams
  `sine.sbc` (the encoder test's 1 kHz frames, checked in) at the media cadence
  with RTP framing; the gate asserts `a2dp_sink=configured` with the exact CIE,
  `streaming by=incoming`, an **audio-clock-referenced** RMS/checksum of the
  decoded blocks (the acid-bass discipline: QEMU and silicon agree on
  audio-clocked measurements), `seqgaps=0`, `under=0`, and the peer's tally
  (`PEER-SOURCE pkts= frames= start_ok=1 delay_report=…`). Negatives
  demonstrated RED by name: a corrupt frame injected (decoder refusal counted),
  a 48 kHz SET_CONFIG (rejected), the AudioSink record missing. QEMU cannot
  judge the servo (no real clocks) — the gate only asserts `trim_ppm` stays
  within the clamp.
- **Silicon**: the iPhone pairs from Settings, plays music to the headphone
  jack, volume follows the phone, **10 min without an audible artefact** with
  `under=0 over=0` and `trim_ppm` settled (the servo's number is the
  measurement); a second EVKB running `bt_tone_test` as the controlled source
  when the phone misbehaves; the Mac with PacketLogger for captures. Recorded
  in `transcript_hw_evkb.txt`.
- Close-out discipline as always: M2Radio + Audio pushed and pinned, fresh-user
  by running the gate on the fetched ELF, every bt-linking ELF rebuilt, sweep,
  vacuity, audit after the sweep, `LICENSE-AUDIT` (the decoder is clean-room
  MIT like the encoder).

## 6. Risks

- **Phone quirks** dominate: iOS needs the AVRCP target with absolute volume or
  refuses to treat us as a speaker; some sources open AVRCP before AVDTP; SDP
  attribute shapes matter (BT-2/BT-3 lessons). Capture-first if anything stalls
  (the trace + `stall_decode.py` named two defects in one attempt each today).
- **48 kHz sources** are rejected by design; if the iPhone ever configures 48
  the decision is revisited (the decoder will already handle it; the graph
  would not).
- **The servo is bench-tuned**: gain/clamp/hold need the phone; QEMU only bounds
  it. A wrong gain shows as pitch wobble, not silence — audible and measurable.
- **Two radios' worth of state in one BtSession** (source policy vs sink
  policy) — keep them as two policies, not one with flags (the piece-2 lesson).

## 7. Non-goals

48 kHz; AVRCP controller (play/pause from the board — a later UI item); AAC or
aptX; SCO/HFP; the sink inside acid_box; simultaneous source+sink.

## 8. Files (expected)

M2Radio: `bt/SbcDecoder.{h,cpp}`, `bt/A2dpSink.{h,cpp}`, `bt/Avdtp.{h,cpp}` (SNK
SEP personality, DelayReport send), `bt/Sdp.{h,cpp}` (AudioSink record),
`bt/Avrcp.{h,cpp}` (absolute volume), `bt/BtSession.{h,cpp}` (sink policy),
`bt/test/{sbcdecoder,a2dpsink}_test.cpp` + `avrcp_test`. Audio:
`utility/imxrt_hw.{h,cpp}` (`trim_audioClock`). evkb:
`examples/audio/bt_sink_test/` (sketch, `AudioInputBluetooth.{h,cpp}`,
`CMakeLists.txt`, `run_qemu.sh`, `tests/servo_test.c`, transcripts),
`examples/networking/m2_hci_probe/hci_peer.py` (`source` phase), `tools/`
vacuity + `license-audit.sh` GATES entry, `evkb.cmake` pins, `CLAUDE.md`, memory.
