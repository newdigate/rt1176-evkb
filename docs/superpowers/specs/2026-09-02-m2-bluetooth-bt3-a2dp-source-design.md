# BT-3: A2DP source + SBC encoder (`AudioOutputBluetooth`) — design

**Date:** 2026-09-02
**Linear:** NEW-9 (BT-3). Parent programme: `2026-08-23-m2-bluetooth-a2dp-programme-design.md`.
**Hardware:** MIMXRT1170-EVKB + Embedded Artists Murata 1XK (NXP IW416) in J54, HCI
over LPUART2 (H4); bench sink = `tools/esp32-a2dp-sink/` ("EVKB-SINK", calibrated
2026-09-02); interop peers = Shokz OpenMove, OneOdio A70.
**Status of what this builds on:** BT-1 (HCI transport) and BT-2 (BR/EDR link,
SSP pairing, encryption, L2CAP, SDP) are complete on silicon against two real
headsets and the sink; AVDTP Discover already returns the sink's stream
endpoint from the BT-2 prototype in `examples/networking/m2_hci_probe`.

## Goal

Stream the Audio-library graph to a Bluetooth A2DP sink as an `AudioStream`
output node: `AudioOutputBluetooth`. First a 1 kHz test tone audible on the
sink, then the Acid Box, with Wi-Fi coexistence measured. Clean-room, MIT,
bare-metal, no heap, in `M2Radio/bt/` + the `Audio` sibling repo.

## Decisions taken in the brainstorm (2026-09-02), in order

| Question | Decision | Why |
|---|---|---|
| Transport at speed (the programme's B3) | **folded into BT-3 as phase 0** | the 115 200-baud link the controller stays at after download cannot carry SBC (~328 kbps + framing); nothing else needs the fast link, and the sink's packet counter measures loss on the path that matters |
| First audio | **1 kHz test tone**, Acid Box after | one suspect at a time: a tone isolates the link from the graph |
| API shape | **`connect(name)` blocking in `setup()`**, then `connected()` + counters | fewest moving parts; the async/reconnecting state machine is deferred |
| Media path threading | **B: encode in the audio ISR, send from the main loop** (C — ISR-side UART-TX DMA ring — kept as a later option) | the transport is single-threaded main-context (writing from the RX pump bus-faults, measured in B6); encoding is pure compute; the packet queue becomes the one latency/stall number |
| QEMU side of the two-gate rule | **fake controller grows an AVDTP acceptor + RTP/SBC frame checks**; encoder unit-tested on the host | zero qemu2 changes; the transport/pacing layer gets real automated coverage without a Python SBC decoder |
| Pairing | **SSP first, legacy PIN fallback** | SSP fails between the IW416 and the ESP32 controller at the LMP IO-capability exchange (seven host variables eliminated, 2026-09-02); headsets pair via SSP; the same node must reach both |
| Baud | **921 600 first**, 3 M only if loss says so | the spec's floor; the likeliest exact rate off the clock root |

## Architecture

### Units

| Unit | Does | Depends on |
|---|---|---|
| `M2Radio/hci` (exists) | H4, command queue, events/ACL callbacks; **phase 0 adds** the vendor `0xFC09` set-baud sequence and LPUART2 re-baud in `HciTransport` | core |
| `M2Radio/bt/L2cap` | signalling: Connection/Configuration (options echoed; **Config-Response SCID = the peer's CID**, the receiver-side rule), Information Request/Response, Echo, Disconnect; connection-oriented channel objects (local/remote CID, negotiated MTU); ACL demux by CID; TX framing; **ACL credit accounting** from Number_Of_Completed_Packets | `hci` |
| `M2Radio/bt/BtLink` | inquiry-by-name → Create_Connection → pairing (SSP; on failure legacy PIN, fixed 1234 for the bench) → Set_Connection_Encryption; owns the SSP/PIN/link-key events | `hci` |
| `M2Radio/bt/Sdp` | client: one ServiceSearchAttributeRequest (AudioSink → ProtocolDescriptorList → AVDTP PSM/version) | `L2cap` |
| `M2Radio/bt/Avdtp` | initiator signalling with transaction labels: DISCOVER, GET_CAPABILITIES, SET_CONFIGURATION, OPEN, START (and CLOSE/ABORT/SUSPEND handling); answers a peer's DISCOVER with our single audio-SRC SEP; opens the **media** CO channel after OPEN | `L2cap` |
| `M2Radio/bt/Sbc` | clean-room encoder from A2DP v1.3 §12: analysis filterbank (4/8 subbands, 4–16 blocks), scale factors, loudness/SNR bit allocation, quantiser, CRC-8, frame header; fixed-point; `encode(L[128], R[128], out)` → one frame | — |
| `M2Radio/bt/Rtp` | RTP v2 header (dynamic PT 96, sequence, timestamp in samples, SSRC) + the A2DP media payload header (frame count) | — |
| `Audio/AudioOutputBluetooth` | the `AudioStream(2 inputs)` node: `connect(name)`, `update()`, `poll()`, `connected()`, counters | all of the above |

Everything BT-2 proved stays behind `M2_BT_*` knobs in the probe until phase 1
moves it into these units; after phase 1 the probe links the library.

### Data flow (approach B)

```
audio ISR, every 128 samples (~2.9 ms @ 44.1 kHz)
  update(): receiveReadOnly(L), receiveReadOnly(R)
            Sbc::encode(L, R) -> one SBC frame            (16 blocks x 8 subbands = 128 samples: one block, one frame)
            append to the packet under construction
            packet full (N frames)? -> push to the packet queue (single producer, lock-free)
            release blocks
main loop
  poll():   pop packet -> Rtp header + payload header -> L2cap frame on the media channel
            write ACL ONLY while the controller has credits (acl_num, tracked from 0x13 events)
```

Packet arithmetic at the calibration configuration (SBC 44.1 kHz, joint stereo,
8 subbands, 16 blocks, loudness, bitpool 53): frame length
`4 + (4·8·2)/8 + ceil((8 + 16·53)/8) = 119` bytes; **N = 5** frames per packet
gives `12 + 1 + 5·119 = 608` bytes, under the 672-byte default MTU — one packet
per 5 blocks ≈ 14.5 ms. N is computed from the **negotiated** media MTU at OPEN
(the OneOdio configures 335, the Shokz 895), never assumed.

Queue depth 16 packets ≈ 230 ms; its high-water mark and drop count are
counters. Nothing in the ISR touches the transport; nothing in the loop touches
audio blocks.

### Clocking

No clock recovery in the source direction: pacing is the audio ISR at
44.1 kHz; the sink's jitter buffer absorbs the ppm-level difference between our
clock and its own (the ESP32 sink holds 32 KB; headsets hold their own). This is
the reason the programme chose source-first.

### Transport at speed (phase 0)

The IW416 keeps HCI at the firmware-download rate (115 200). Phase 0 issues the
vendor set-baud command (`0xFC09`, the sequence NXP's own stack uses — protocol
fact only, nothing transcribed), re-programs LPUART2 to 921 600, and
re-validates HCI at the new rate. This board's flow control is one-sided: we
assert the card's CTS permanently, the card cannot throttle us (its RTS pin is
the gigabit PHY's interrupt), so lossless operation in the source direction is
**measured**, not assumed — first with the controller's local loopback mode
(N ACL packets sent = N echoed, Wi-Fi running), then with the sink's counters
in phase 4. If 921 600 shows loss, 3 M is the fallback and the LPUART2 clock
root / eDMA question the programme's B3 row raised is reopened; if not, it
stays closed.

## Phases and assertions

Each phase ends with something the firmware cannot invent; QEMU gates exist
wherever QEMU can honestly reach the assertion.

| Phase | Builds | Silicon assertion | QEMU gate |
|---|---|---|---|
| **0 — transport at speed** | `0xFC09` + re-baud in `HciTransport`; loopback loss test in the probe | `hci_reset=ok attempts=1` at 921 600; loopback N sent = N echoed with Wi-Fi up | `m2_hci_probe[baud]`: fake controller accepts `0xFC09`, gate asserts the re-validation sequence; states that a chardev has no baud, so the rate is silicon-only |
| **1 — extraction, credits, fake acceptor** | `bt/L2cap`, `bt/BtLink`, `bt/Sdp`, `bt/Avdtp` skeleton; probe re-pointed at the library; Python fake controller gains an AVDTP acceptor | existing probe gates pass unchanged (refactor baseline); B4/B5/B6 re-run on both headsets + sink | host tests: SCID rule, Information Response, credit accounting |
| **2 — AVDTP signalling (B7)** | `Avdtp` initiator complete; media channel | sink's capabilities off the wire; the sink's **own** `a2dp_audio_cfg` equals what we sent; `a2dp_audio: state=started`; same sequence on both headsets over SSP | `[avdtp]`: fake acceptor asserts our codec-info bytes = `21 15 02 35` and no START before OPEN is acknowledged |
| **3 — SBC encoder (B8)** | `bt/Sbc` | `.sbc` from a sine vector decoded by ffmpeg on the Mac (a test tool; nothing links it) and audible | host tests: frame length, header bits, CRC-8, scale factors, loudness allocation, known vector |
| **4 — first sound (B9a)** | `bt/Rtp`, `AudioOutputBluetooth`, `examples/audio/bt_tone_test` | 1 kHz audible on the sink's DAC; sink heartbeat ≈176 400 PCM B/s with `dropped=0`; **packets sent = packets received** over 60 s | `audio/bt_tone_test[media]`: fake acceptor counts RTP packets + SBC frames, checks sequence continuity, bitpool, CRC, frames received = blocks produced; demonstrated red against a broken packetiser first |
| **5 — capstone + coexistence (B9b)** | Acid Box → `AudioOutputBluetooth` | Acid Box audible in the headphones and the sink; uAP throughput soak concurrently; queue high-water, drops, Wi-Fi throughput delta reported | card-absent fallback gate only (hardware-only assertion, like every m2 example) |

Dependencies: 0, 1 and 3 are independent; 2 needs 1; 4 needs 0, 2, 3; 5 needs 4.
Phase 5's numbers decide whether approach C is ever needed.

## Error handling

`connect(name)` walks fixed stages, each with its own timeout and a named
outcome — `no_inquiry_hit`, `connect_status=0x..`,
`pairing_ssp_failed→pin_fallback`, `pin_failed`, `encryption_failed`,
`sdp_timeout`, `avdtp_rejected(signal, error_code)`, `open_timeout` — returns an
enum, keeps `lastError()`, and prints one `key=value` line per stage in the
probe's grammar, so a transcript reads the same whether a phase passed or where
it stopped. The pairing method that succeeded is reported, never assumed.

At run time the node degrades and never stalls the audio ISR: credit starvation
fills the queue → the oldest packet is dropped and counted; a peer disconnect
(0x05) or an AVDTP SUSPEND/CLOSE puts the node in `idle` (`update()` releases
blocks without encoding, `connected()` false) and the sketch decides — **no
auto-reconnect in BT-3**. Counters: `packets`, `frames`, `dropped`,
`queue_hwm`, `credits_min`.

## Testing

* **Host tests** (`M2Radio/bt/test/run.sh`, twin of `hci/test/run.sh`): `Sbc`,
  `L2cap` encoding (the SCID rule, Information Response, credits), `Rtp`.
* **QEMU gates**: the Python fake controller + AVDTP acceptor on LPUART2 via
  `-serial unix:` — zero qemu2 changes. New ids: `m2_hci_probe[baud]`,
  `m2_hci_probe[avdtp]`, `audio/bt_tone_test` (card-absent) and
  `audio/bt_tone_test[media]`. Every new gate is demonstrated red against a
  deliberate break before it is trusted and gets a `gate-vacuity` fixture; the
  sweep moves from 121 by exactly the number added, and `-l` is what says so.
* **Silicon**: the sink instrument's log is the oracle at every phase; both
  commercial headsets at phases 2 and 5.
* **House checks**: `license-audit.sh` (the encoder is where "clean-room from
  the spec" must be visibly true — no `libsbc`, no bluedroid source read),
  pins bumped with the fresh-user verification, transcript evidence per phase.

## Non-goals (deferred, by decision)

AVRCP; auto-reconnect / async connection state machine; the sink direction
(BT-4); an SDP server of our own (the sinks query us for DeviceID — BT-4 needs
it, BT-3 does not); approach C (ISR-side UART-TX DMA ring) unless phase 5's
numbers demand it; any codec but SBC.

## Risks, named

* **One-sided flow control at speed** — measured at phase 0 and again at phase 4;
  3 M and the eDMA/clock-root work stay the fallback.
* **IW416↔ESP32 SSP incompatibility** — bounded to the LMP IO-capability
  exchange; the PIN fallback covers the bench; real headsets are unaffected.
* **Per-peer MTU** (335 / 672 / 895 seen) — N frames per packet is computed at
  OPEN, never fixed.
* **Wi-Fi driver blocking the main loop** — the packet queue is sized for it and
  its high-water mark is reported; approach C is the answer if it is not enough.

## References

Bluetooth Core v5.2 Vol 3 Part A (L2CAP), Vol 4 Part E (HCI); AVDTP v1.3;
A2DP v1.3 §12 (SBC); RFC 3550 (RTP). NXP EdgeFast / `controller_wifi_nxp.c`
are LA_OPT — protocol facts only, never transcribed. The Linux `btnxpuart`
driver is GPL and is not read.
