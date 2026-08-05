# USB audio INPUT on the RT1176 host, and full duplex — design

Status: proposed, 2026-08-05. Nothing here is built yet except the parser
defect in §2, which is a bug in shipping code and should be fixed whether or
not the rest proceeds.

## The argument

`USBAudioOut` streams audio to a device and, since the feedback work, tracks
that device's converter rate to −0.05 ppm. It cannot record. Every capability
this repo has demonstrated on the host port is one-directional, and the two
devices on the bench are both full duplex: the MC200 witness is 8-in/8-out,
and the third-party dongle is 2-out/1-in.

Input is worth having on its own, but the reason to take it seriously now is
that it makes the *existing* work verifiable in a way it currently is not. The
OUT direction is judged by a device-side observer that only exists because
lib_xua was patched. An IN stream lets the host measure what a device actually
sends, on any device, with no instrumentation — and on a duplex device, the IN
FIFO's occupancy is a second, independent estimate of the same converter drift
the feedback endpoint reports. Two measurements of one quantity, from opposite
directions, is the pattern that has caught every real defect in this project.

## 1. What already exists

Input is much less new code than it appears, because the transport is built
and hardware-proven:

- `itd_fill_in()` and `sitd_fill_in()` are implemented, tested
  (`test_itd`, `test_sitd`) and have run continuously — the HS feedback reader
  has been polling at ~968/s all day through `itd_fill_in`.
- The periodic schedule already chains several descriptors per frame;
  `sitd_skip_iso()` walks mixed runs bounded by `ISO_RUN_GUARD`, which is
  already sized for both pools.
- Micro-frame bandwidth accounting (`uframe_bandwidth`) exists.
- `usb_audio_fifo` is direction-agnostic.
- `uac1_parse_config` / `uac2_parse` already walk every interface and alt.

So "add input" is, in the main, a second ring, a consumer, and a graph node.

## 2. The parser defect — fix this regardless

`usb_audio_parse.cpp:138`:

```c
} else if (!is_out && is_iso) {
    if (alt->feedback_endpoint == 0) alt->feedback_endpoint = b[2];
```

Any non-OUT isochronous endpoint is recorded as the FEEDBACK endpoint, on
direction alone. On a device that puts audio IN and audio OUT in the same
alternate setting, the microphone endpoint is captured as the feedback
endpoint and the feedback reader is armed against the audio stream. The
decoded "rate" would be audio samples reinterpreted as a Q16.16 rate, which
`uac1_feedback_plausible()` would mostly reject — so the visible symptom is a
climbing `fbrej` and a loop that never closes, not an obvious failure.

Nothing on this bench triggers it: the dongle's IN endpoint lives on a
different interface (interface 2), which is never the selected alt, and the
MC200's alt carries OUT plus a real feedback endpoint.

The fix classifies by `bmAttributes` as well as direction. USB 2.0 section
9.6.6 (read from the primary document, page 298 of the 2024-09-27 revision),
bits 5..4 Usage Type: `00` = Data endpoint, `01` = Feedback endpoint,
`10` = Implicit feedback Data endpoint, `11` = Reserved. So an explicit
feedback endpoint is `(attr & 0x30) == 0x10`.

**Corrected 2026-08-05, after implementing it.** An earlier draft of this
section said `10` was feedback and `01` implicit — wrong, from memory, and the
tests caught the consequence immediately. Worse, usage type alone is ALSO
insufficient: the XMOS UAC1 witness declares its feedback endpoint 0x82 with
`bmAttributes 0x01`, usage type `00` = "data" (fixture
`xmos_uac1_async_feedback.bin`), so believing the usage bits alone loses that
device's feedback entirely. The UAC2 witness by contrast declares `0x11`,
usage `01`, correctly.

The rule that survives contact with real hardware needs both authorities:
`bSynchAddress` on the data endpoint names the feedback endpoint, and the
usage bits are a fallback for a device that declares `01` but names nothing.
An IN endpoint that is neither — plain audio, usage `00` or `10` — is what a
full-duplex device puts there, and is what direction-only classification used
to swallow.

This is a small, self-contained change with a fixture already available: the
dongle's 244 descriptor bytes, captured this session, are a real third-party
config that exercises multi-interface parsing.

## 3. Scope, staged

**Stage A — parser fix.** §2. Ships alone, tested against the dongle fixture
and the existing UAC1/UAC2 fixtures. No behaviour change on any device we own.

**Stage B — input only.** `USBAudioIn`, or an input mode on the existing
driver, streaming IN from a device while nothing streams OUT. This is the
stage that proves the transport, the FIFO discipline and the descriptor
budget without touching clock ownership.

**Stage C — full duplex, one device.** Both directions on the same device,
which is the case worth having and the tractable one: IN and OUT share the
device's converter clock, so one rate estimate serves both.

**Non-goal: duplex across two different devices.** That needs asynchronous
sample-rate conversion between two unrelated crystals. It is a different
project and should not be smuggled into this one.

## 4. Clock ownership — the actual design problem

Bandwidth and descriptors are arithmetic. Clocking is the design.

Today `AudioOutputUSBHost` is the graph's clock owner: `onFrameConsumed()`
runs the graph from the USB frame clock, so the graph is paced by the bus and
the OUT packet sizer is steered by the device's feedback (design spec §8 of
the P1 work). Adding input introduces a producer whose rate nobody controls —
the device sends what its converter produces, and the host must consume it.

Three clocks are then in play: the graph's nominal rate, the device's
converter, and the USB frame clock. The options:

1. **Device is master, graph follows (recommended for Stage C).** The IN
   stream's arrival rate *is* the device's converter rate. Run the graph from
   IN completions instead of OUT completions, and size OUT packets from the
   same feedback estimate already in use. On a duplex device both directions
   share the converter, so the IN FIFO stays flat by construction and the OUT
   loop is unchanged. This is the smallest change to what already works.

2. **Graph is master, input is rate-matched.** Requires dropping or repeating
   input samples, i.e. degrading the recording to protect the graph's timing.
   Rejected: it corrupts the measurement the input direction exists to make.

3. **Both free-running with deep buffers.** Defers the problem until the
   buffer runs out. Rejected on the same grounds as open-loop output was:
   it converts a rate error into a periodic audible correction.

The IN FIFO occupancy should be exported the way OUT's is, because on a
duplex device it is an *independent* estimate of the same drift the feedback
endpoint reports. Those two numbers agreeing is a strong correctness signal;
disagreeing means one of them is wrong, which is worth knowing.

## 5. Descriptor and memory budget

At high speed the iTD pool is currently exhausted by design:

```
ITD_POOL_SIZE 64  =  32 (OUT ring)  +  32 (feedback, 1000 polls/s)
```

An IN ring needs another 32. Two ways to pay for it:

- **Grow the pool to 96** (+32 iTDs ≈ 3 KB) plus an IN payload ring. At the
  current geometry that ring is 32 × 8 × 224 B ≈ 57 KB of DMAMEM, which is the
  dominant cost and should be sized from the negotiated format rather than the
  worst case.
- **Reclaim from feedback.** The A/B this session measured 250 polls/s at a
  296 B envelope against 1000 polls/s at 268 B — 8 slots instead of 32 costs
  about 30 B of envelope and frees 24 descriptors. That is a real trade and
  the measurement to justify it already exists.

Recommendation: grow the pool for Stage B/C and keep feedback at full rate,
because the aliasing finding (`transcript_uacv_servo_isolation.txt`) showed
poll rate is the dominant envelope effect and 1000/s is not a luxury. Revisit
only if DMAMEM becomes tight.

Full speed is easier: `SITD_POOL_SIZE` 40 against 32 OUT + 2 feedback leaves
too little for a 32-slot IN ring, so FS duplex needs the same pool growth.

## 6. Verification

The repo's two-gate rule applies, and this feature is unusually well served:

- **Unit.** The parser fix has a real fixture (the dongle's 244 bytes). The
  IN ring's descriptor building is testable exactly as `test_itd` tests the
  OUT path.
- **QEMU.** No UAC device model exists, so QEMU can only prove the firmware
  boots and does not hang. Say so rather than implying more.
- **Silicon, and this is the strong part.** Three independent instruments now
  exist for this bench:
  - the **validator**, which judges the OUT direction from inside the device
    and would judge the IN direction too once the observer counts IN packets;
  - the **analogue rig** (`xrec` + `tools/analogglitch.py`), which resolves
    single-sample discontinuities at a 0.021 millisample noise floor;
  - and for input specifically, the **loopback test that closes the circle** —
    play a known tone out of the RME into the device's input, record it
    through the host, and compare. That is the first end-to-end check in this
    project that does not depend on the device being instrumented.

The cooperative LFSR pattern (R7) has an obvious analogue here: a host that
can record could verify an input stream is sample-continuous by the same
mechanism, in the opposite direction.

## 7. Open decisions needing sign-off

1. **One driver or two?** `USBAudioOut` currently owns claim, control
   sequencing and the topology. Input could be a second driver (clean
   separation, but two drivers claiming one device is awkward under the
   framework's claim model) or a mode of the existing one (simpler claim,
   larger class). Recommendation: extend the existing driver, because the
   control sequence and clock handling are shared and a duplex device is one
   device.
2. **Stage B before C, or straight to C?** Recommendation: B first. It proves
   transport and budget without touching the clock ownership that currently
   works.
3. **Does the IN payload ring size from the negotiated format** (saves tens of
   KB on a 1-channel dongle) **or the worst case** (simpler, always correct)?
   Recommendation: negotiated, with the same begin-time guard the OUT path
   uses (`worst > MAX_UFRAME_BYTES` → refuse).

## 8. Risks

- **Clock ownership is a behaviour change to code that currently works.**
  The OUT path is at −0.05 ppm with zero corrections over 20 minutes. Stage C
  must not regress that, and the corpus's clean-host cases (`Z_clean_uac1`,
  `Z_clean_uac2`) are the guard — they exist precisely to catch this.
- **DMAMEM.** The IN ring is the largest single allocation the driver would
  own. Size it from the format.
- **The claim model.** `claim(type=0)` currently takes the whole device. A
  duplex device with separate IN and OUT interfaces needs care that both are
  configured before either streams, and the control watchdog (`46c3daf`)
  already exists for exactly this class of stall.
- **Devices that lie.** The dongle declares no synchronisation, offers no
  feedback endpoint, and glitches ~1/s with bidirectional corrections that are
  demonstrably *not* rate drift (net −12.6 ppm over 116 s). Input from such a
  device will be no better behaved than output to it, and the design should
  not assume a well-behaved producer.
