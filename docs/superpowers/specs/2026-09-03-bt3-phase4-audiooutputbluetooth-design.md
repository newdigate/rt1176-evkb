# BT-3 phase 4: `AudioOutputBluetooth` — first sound over A2DP — design

**Goal:** stream the audio graph to a paired A2DP sink as an Audio-library output
node, so a 1 kHz test tone is audible on the sink's speaker/DAC. First sound over
Bluetooth; the Acid Box capstone is phase 5.

**Status of the ground it stands on (measured on silicon 2026-09-03):**
- **Phase 0** — HCI runs at **3 Mbaud** (`0xFC09` switch; the host switches
  immediately, the IW416 returns the CC at the new rate). evkb `37427e7`.
- **Phase 2** — the A2DP *signalling* source works end to end against the ESP32
  sink (`tools/esp32-a2dp-sink`, "EVKB-SINK"): connect → **legacy-PIN** pair →
  encrypt → SDP → AVDTP DISCOVER→SET_CONFIG→OPEN→START, and the sink echoes our
  exact SBC config (`a2dp_audio_cfg … cie=21 15 02 35`, `a2dp_conn:
  state=connected`). evkb `0876c6c`, M2Radio `2b17bd6` (`BtLink::setLegacyPin`).
- The **clean-room SBC encoder** (`M2Radio/bt/Sbc`, A2DP v1.3 §12) is built and
  ffmpeg-SNR-validated (unity round-trip 63.9 dB). Reused here unchanged.

What is missing is the **media path**: encode the graph's PCM to SBC, RTP-frame
it, and send it on the AVDTP media channel in real time. That is phase 4.

---

## Decisions

| Axis | Decision | Why |
|---|---|---|
| Media-path threading | **B: encode in the audio ISR, send from the main loop** (approach C — an ISR-side UART-TX DMA ring — kept as a later option) | The transport is single-threaded main-context: writing to L2CAP from ISR / RX-pump context bus-faults (measured in phase 2 / B6). Encoding is pure compute. The packet queue becomes the one latency/stall number. |
| Node boundary | **Pure streaming node.** `AudioOutputBluetooth` takes an already-STARTED transport and does ONE job: PCM → SBC → RTP → `l2.send` on the media channel. | Audio-library-idiomatic (like `AudioOutputI2S`); smallest node; host-testable against a fake `L2cap`. |
| Bring-up packaging | **A reusable `M2Radio/bt/A2dpSource` helper.** `connect(name)` → ready media channel. | The bring-up (connect→pair→SDP→AVDTP-START) is non-trivial and proven in phase 2; one tested place for it, reusable by any `AudioOutputBluetooth` consumer. |
| QEMU side of the two-gate rule | **The fake controller grows a media receiver** (RTP + SBC frame checks); the `MediaPacketizer` logic is host-tested against a fake send callback. | Zero qemu2 changes; the transport/pacing layer gets real automated coverage without a Python SBC decoder. |
| Backpressure | **Drop the OLDEST frame, never stall the ISR.** | The audio ISR is the clock; it must never wait on the transport. A real-time sink wants the freshest audio. |
| Encode bitpool | **53** (max quality, ~328 kbps), the top of the negotiated `2..53` range | Phase 0 proved the 3 M link is solid; lowering bitpool is the first lever if phase-4 silicon shows loss. |

---

## Architecture

Four units (plus the reused `Sbc`), each with one responsibility and a clean
interface:

### `M2Radio/bt/A2dpSource` (new)
Wraps `BtLink` + `L2cap` + `Sdp` + `Avdtp` into the A2DP-source bring-up proven in
phase 2.
- `A2dpSource(Hci &hci, HciIo &io)` (constructs the owned `L2cap`/`BtLink`/`Avdtp`).
- `void setLog(LogFn, void*)`, `void setPin(const char*)`, `void setLegacyPin(bool)`.
- `Result connect(const char *name, uint32_t (*now)(), void (*idle)())` — inquiry
  by name → Create_Connection → pair (SSP, or legacy PIN if set) → encrypt →
  L2cap → SDP (AVDTP version) → Avdtp DISCOVER→GET_CAP→SET_CONFIG→OPEN→START. The
  SbcConfig it configures is the phase-2 calibration (`44100`, joint, 16 blocks,
  8 subbands, loudness, bitpool `2..53`).
- Accessors for the node: `L2cap &l2()`, `uint16_t mediaCid()`
  (= `avdtp.mediaRemoteCid()`), `uint16_t mediaMtu()`, `const Sbc::Params
  &sbcParams()` (the negotiated config translated to `Sbc::Params`, bitpool 53),
  `bool started()`.
- The app forwards events/ACL to it: `onEvent(code,p,len)` (→ BtLink + L2cap +
  Avdtp) and an ACL thunk (→ L2cap), exactly as `m2_hci_probe`'s probeConnect
  wires them today.
- `m2_hci_probe`'s `probeConnect` stays as the bench probe (it probes; it is not a
  consumer). `A2dpSource` is the clean production path. No probe refactor in
  phase 4.

### `M2Radio/bt/Rtp` (new)
Pure framing, no I/O.
- `static uint16_t header(uint8_t *out, uint16_t seq, uint32_t timestamp,
  uint8_t frameCount)` — writes the 12-byte RTP v2 header (V=2, P=0, X=0, CC=0,
  M=0, **PT=96** dynamic; `seq`, `timestamp`, `SSRC`, all big-endian) + the 1-byte
  A2DP media payload header (fragmentation flags 0, `frameCount` in the low 4
  bits per A2DP v1.3 §4.3.4). Returns 13. SSRC is a fixed constant per build.

### `M2Radio/bt/Sbc` (reused, unchanged)
`encode(L, R, out)` — one 128-sample block per channel → one SBC frame (119 bytes
at the negotiated config). Already host-tested (228 checks) + ffmpeg SNR 63.9 dB.

### `M2Radio/bt/MediaPacketizer` (new — the host-testable core)
All the ring / drop / RTP-batch logic, with **no Audio-library and no L2cap
dependency**, so it host-compiles and is tested like every other logic unit in the
tree. It owns the SPSC ring and the RTP sequence/timestamp state.
- `void begin(uint16_t mtu, const Sbc::Params&)` — resets seq/timestamp/counters.
- `void push(const uint8_t *sbcFrame, uint16_t len)` — enqueue one frame (called
  from the producer / ISR side). If the ring is full, advance the read index (drop
  the **OLDEST** frame) and bump `drops`. Never blocks.
- `typedef bool (*SendFn)(void *ctx, const uint8_t *pkt, uint16_t len)` (returns
  false when the sink is not ready — e.g. no L2CAP credit — and the packetiser
  keeps the frames for next time). `void drain(SendFn, void *ctx)` — while frames
  remain and `SendFn` accepts: batch whole frames up to `mtu` into one packet
  (`Rtp::header` + N frames), send, `seq++`, `timestamp += 128*N`, `packets++`.
- Counters: `frames()`, `packets()`, `drops()`, `queueHighWater()`.
- Rationale: this is the part most likely to have bugs (index math, batching,
  drop-under-starvation), and it is exactly what the fake-`L2cap` host test needs
  to drive — so it lives free of `AudioStream`, which is ARM/Teensy-only and not
  host-compilable.

### `Audio/AudioOutputBluetooth` (new — the thin AudioStream shell)
The `AudioStream(2 inputs, inputQueueArray)` node — L on input 0, R on input 1 —
wrapping `Sbc` + `MediaPacketizer`.
- `void begin(A2dpSource &src)` — captures `l2()`, `mediaCid()`, `mediaMtu()`,
  `sbcParams()`; `Sbc::begin(params)`; `MediaPacketizer::begin(mtu, params)`.
  (A lower-level `begin(L2cap&, cid, mtu, const Sbc::Params&)` is the real entry;
  the `A2dpSource&` form is sugar.)
- `void update()` (audio ISR): `receiveReadOnly()` both channels (silence if
  absent); `Sbc::encode()` into a scratch frame; `m_pk.push(frame, len)`. Release
  the blocks. Never blocks; never touches the transport.
- `void poll()` (main loop): `m_pk.drain(sendThunk, this)`, where `sendThunk`
  checks L2CAP credit and calls `l2.send(mediaCid, pkt, len)`.
- `bool connected() const`, and counters delegating to `m_pk`:
  `blocks()`, `packets()`, `drops()`, `queueHighWater()`.
- The node itself is tiny glue (encode + wire the packetiser to L2cap); its logic
  lives in the two host-tested units below it, and its integration is proven by the
  `[media]` QEMU gate.

### `examples/audio/bt_tone_test` (new)
`AudioSynthWaveformSine (1 kHz, both channels) → AudioOutputBluetooth`, with
`A2dpSource` for bring-up. `setup()`: firmware download → identity → baud → 3 M →
`A2dpSource::connect(M2_BT_TARGET_NAME)` → `out.begin(src)`. `loop()`:
`l2.service(); avdtp.service(); out.poll();` and a heartbeat echoing
`blocks/packets/drops/highwater` (so a capture shows the outcome without catching
`setup()`, the lesson from phase 2).

---

## Data flow (approach B)

```
Audio ISR, every 128 samples (~2.9 ms @ 44.1 kHz):
  update():  L,R blocks -> Sbc::encode() -> pk.push(frame)   (into SPSC ring[64])
             ring full -> drop OLDEST, drops++   (never blocks, never sends)

Main loop, every pass:
  l2.service(); avdtp.service();                 // pump HCI/ACL + credits
  out.poll() -> pk.drain(sendThunk):
     while ring non-empty && sendThunk accepts (L2CAP credit):
        batch N frames up to mtu (1008) -> Rtp::header + N frames
        -> l2.send(mediaCid); seq++; timestamp += 128*N; packets++
```

One producer (ISR), one consumer (loop); a fixed 64-frame SPSC ring (~190 ms at
344 fps) whose only shared state is the two indices — no locks. Nothing in the ISR
touches the transport; nothing in the loop touches the audio blocks.

**No clock recovery in the source direction:** pacing *is* the audio ISR at
44.1 kHz. The sink renders from the RTP timestamps.

---

## RTP + SBC packetization (A2DP v1.3 §4, RFC 3550)

```
[ RTP header 12 B ] [ A2DP payload header 1 B ] [ SBC frame ]...[ SBC frame ]
  V=2 P=0 X=0 CC=0    frag flags=0, frame_count=N     N frames, back to back
  M=0 PT=96
  sequence  (++ per packet)
  timestamp (+= 128 per SBC frame)
  SSRC      (fixed)
```

- **Batching:** as many whole SBC frames as fit in `mediaMtu`: at MTU 1008,
  bitpool 53 (119 B/frame) → `(1008-12-1)/119 = 8` frames/packet. `frame_count`
  = N. Frames are never split (each ≤ MTU), so fragmentation flags stay 0.
- **Timestamp** advances 128 samples per SBC frame (16 blocks × 8 subbands),
  tracking the audio clock.
- **Stereo:** the tone feeds L and R; encode joint stereo, matching the
  negotiated `chan=joint`.
- `Rtp` writes only the 13-byte header into a caller buffer;
  `AudioOutputBluetooth` owns the packet buffer and appends the SBC frames.

---

## Testing (the two-gate rule + host)

### Host tests (deterministic, no hardware)
- **`Rtp`** (`M2Radio/bt/test/rtp_test.cpp`) — byte-for-byte: known
  (seq, timestamp, frame_count) → the exact 13 bytes; big-endian fields; PT=96;
  payload-header frame_count.
- **`MediaPacketizer`** (`M2Radio/bt/test/mediapacketizer_test.cpp`) — the key
  logic test, with a **fake `SendFn`** that records packets and can refuse (return
  false) to simulate credit starvation. `push()` known SBC frames, `drain()`, then
  assert: RTP sequence continuous, timestamp advances by 128×N, `frame_count`
  correct, every emitted SBC frame carries sync `0x9C` + the right length, batching
  fills to MTU, a short final packet flushes the tail. Then have the `SendFn`
  refuse and over-`push()` and assert the ring drops the **oldest** frame,
  `drops`/`queueHighWater` count correctly, and `push()` never blocks. Host-
  compiles because it depends only on `Rtp` + `Sbc::Params` (no `AudioStream`, no
  `L2cap`). Wired into `M2Radio/bt/test/run.sh`.
- **`AudioOutputBluetooth`** itself is tiny glue over `Sbc` + `MediaPacketizer` +
  `AudioStream`; `AudioStream` is ARM/Teensy-only (not host-compilable), so the node
  is validated by the `[media]` QEMU gate, not a host test — its logic already lives
  in the two host-tested units.
- **`A2dpSource`** — no separate host test; it is orchestration over
  BtLink/L2cap/Sdp/Avdtp whose integration is proven by the QEMU gate (a host
  test would need a whole fake controller, which is what `hci_peer.py` is).

### QEMU gate — `audio/bt_tone_test[media]`
Extends the phase-2 machinery. `hci_peer.py` grows a **media receiver**: after
AVDTP START it reads the media channel, validates each RTP packet (PT 96,
**sequence continuity**, timestamp advance), parses the frame-count header, and
checks each SBC frame (sync `0x9C`, length, bitpool). Assertions: continuous
sequence (no gaps), `frames_received == blocks_produced` over a bounded run, valid
SBC throughout. The audio ISR runs in QEMU (the existing `audio/*` gates prove it,
at ~0.8× wall), so the tone graph really produces and streams. **Demonstrated RED
first** against a broken packetiser (non-incrementing sequence, wrong
frame_count, corrupted sync) — the tree's regression-gate discipline. A
card-absent `run_qemu.sh` fallback asserts the media path is vacuous with no peer.
Plus a `gate-vacuity.test.sh` fixture. Sweep count moves by the gates added.

### Silicon acceptance (bench, the un-fakeable half)
`bt_tone_test` → ESP32 sink: **1 kHz audible on the sink's DAC**; sink heartbeat
`a2dp_audio: state=started`, `pkts` climbing, `pcm_bytes_per_s ≈ 176400`
(44100 × 2 ch × 2 B), `dropped=0`; our side `drops=0` with the queue high-water
reported; **packets sent = packets received over 60 s**. Appended to
`transcript_hw_evkb.txt`.

---

## The one open risk: one-sided flow control at media rate

`M2_BT_ASSERT_CTS` statically drives the card's CTS (card always clear-to-send),
which also holds the 1 GbE PHY in reset (BT-only). It carried signalling fine
(phases 0–2), but streaming pushes ~328 kbps sustained into a link with no
*host*-side RX flow control. Phase 4 **measures** whether 3 Mbaud + static CTS
carries it with `drops=0`. Ordered levers if not: (1) lower the encode bitpool
(fewer bytes/frame); (2) approach C (ISR-side UART-TX DMA ring) and/or dynamic
hardware RXRTSE. The node's `drops`/`queueHighWater` counters are the instrument.
Phase 4 succeeds if `drops=0` at some bitpool ≤ 53; the bitpool that achieves it
is a finding. Whether approach C is ever needed is decided by phase 5 (busier
graph + concurrent Wi-Fi).

---

## Non-goals (phase 4)
- **No clock recovery** source-side — the audio ISR is the clock.
- **No headset-over-SSP media path** — the ESP32 sink is the controllable
  instrument; headsets stay signalling-only for now.
- **No Acid Box** — that is phase 5 (separate spec), plus the uAP-throughput
  coexistence soak.
- **No approach C** unless phase 4/5 measurements demand it.

## References
A2DP v1.3 §4 (media packet format) and §12 (SBC); RFC 3550 (RTP). No NXP
LA_OPT / GPL sources; the SBC encoder and all framing are clean-room from the
specs.
