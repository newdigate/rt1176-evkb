# RT1062 USB Audio (Phase 4) — Design

**Goal.** The `usb_audio*.cpp` transport compiles for `__IMXRT1062__` for the
first time; `examples/usb/usb_audio_uac1_test` is gated on both boards and plays
an audible 1 kHz tone from the MIMXRT1060-EVKB's J47 host port.

**Predecessors.** Phase 2 (`2026-08-08-rt1062-usb-host-qemu-design.md`) put the
RT1062 USB host in QEMU. Phase 3 proved it on silicon and fixed the two things
that were stopping it — see §1.

---

## 1. What Phase 3 established, and why Phase 4 depends on it

Two silicon findings from 2026-08-08 are load-bearing here. Both were reasoned
about wrongly first and settled by measurement, so they are stated with their
evidence rather than as assertions.

**DTCM is unreachable by the OTG2 DMA master.** With `periodictable` at
`0x20002000`, the controller reported `USBSTS = 0x0000d09a` — `HCH=1` plus
`SEI=1`, a system error on its own descriptor fetch. USBHost_t36's DMA
structures now carry `DMAMEM` on `__IMXRT1062__` (USBHost_t36 `fa939cc`).

**OCRAM must also be uncached on this board.** `cores/teensy4` enables the
D-cache and maps OCRAM write-back/write-allocate, while DTCM is already
`MEM_NOCACHE`. So upstream's descriptors are coherent *for free* in `.bss`, and
moving them to OCRAM — which the first finding forces — creates a coherency
exposure. The EHCI then walked a stale periodic list and halted one frame in
(`FRINDEX` frozen at `0x8`, and **no error bit**, because the port-connect ISR
had already acked the fatal status). Fixed by mapping OCRAM `MEM_NOCACHE` under
`ARDUINO_MIMXRT1060_EVKB` (`cores` `a090c9d`).

The two are one bug, not two: fixing either alone leaves the port dead.

**Consequence for this phase.** `DMAMEM` on `hub1` and `audioOut` in the example
was previously an RT1176-only concern. On rt1062 it is now *also* required, and
all of DMAMEM on this board is uncached — which is why §5's follow-ups exist.

**The device in J47** (measured, Phase 3): GeneralPlus `1B3F:2008`, UAC 1.00,
244 descriptor bytes. Interface 1 alt 1 offers **2 ch, 16-bit, 44100 and
48000**, iso OUT endpoint `0x05`, mps 192. It also has a 1 ch IN and a HID
interface, neither used here.

---

## 2. Scope

| Decision | Choice | Rejected |
|---|---|---|
| Examples | **`usb_audio_uac1_test` only** | + `usb_audio_capture_test`; all four |
| QEMU gate | **Gate the claim path** (see §3) | no-claim shape only; no QEMU gate |
| Silicon bar | **Audible tone + `SITD PASS`** | tone + recorded `pkts/s`; + a cache A/B |

`usb_audio_capture_test` was considered and rejected on evidence. It is
hardcoded to **8 channels / 24-bit** and its comments record that it was built
around an "MC200 witness" that "captures 8 channels of 24-in-4 and offers no
other input". The J47 device offers 1 ch / 16-bit IN, so it would never claim.
Its QEMU gate also runs deliberately with *no device attached*, so porting it
would add a gate that exercises no device on either board. The IN direction
needs either a matching device or a purpose-built example; both belong in a
later phase.

Not in this phase: the IN direction, `usb_audio_duplex_test`, `usb_audio_graph_test`,
and the Phase 5 capstone (USB audio → AudioStream → WM8960).

---

## 3. The sample rate, and why one binary serves both worlds

`usb_audio_uac1_test` ships requesting **44100** ("matches the Audio library").
QEMU's `usb-audio` model offers **48000 only** — `USBAUDIO_SAMPLE_RATE` is a
compile-time `#define` in `hw/usb/dev-audio.c` with no property to change it. So
as it ships, the example enumerates in QEMU and never claims.

Gate builds and silicon builds are **the same build directories** (`build/`,
`build-rt1062/` — `gate-lib.sh` `gate_build_dir`), so there is no separate
"gate-only" build in which to override the rate.

**The J47 device supports both rates.** So the example moves to
`#define UAC1_RATE_HZ 48000`: one binary that claims in QEMU *and* plays on
silicon, with no divergence between the gated artifact and the flashed one.

It stays a `#define` rather than a literal because **Phase 5 will want 44100** —
the capstone feeds an AudioStream graph, and the Audio library is 44.1 kHz. This
is the knob that phase turns.

---

## 4. Changes

### 4.1 Firmware — `examples/usb/usb_audio_uac1_test/`

| Change | Why |
|---|---|
| `CONSOLE` alias (`Serial1` on rt1176, `Serial6` on rt1062) | Both are LPUART1. On the EVKB the DAPLink VCOM is LPUART1; `Serial1` there is LPUART6 and reaches only header pins D0/D1. 26 call sites. Same pattern as the three existing two-board examples. |
| Guard `TEENSY_VERSION` | Unguarded it caches 117 and silently builds an **RT1176** image into `build-rt1062/`, which boots the wrong machine and fails looking like a board problem. |
| `#define UAC1_RATE_HZ 48000` | §3. |
| New `toolchain/rt1062-evkb.toolchain.cmake` | Verbatim copy; same directory depth as `serial_test`, so the `../../../evkb.cmake` walk is identical. |
| New `boards` (`rt1176`, `rt1062`) | Declares the gate on both boards. |

`DMAMEM` on `hub1`/`audioOut` is already present and is now required on rt1062
too (§1). `print.cpp` is already in this example's source list.

### 4.2 The gate — new, covers both boards

The example has **no gate today**, so one script yields two gate ids.

The gate **attaches QEMU's emulated device** —
`-audiodev none,id=snd0 -device usb-audio,bus=usbhost.0,port=1,audiodev=snd0`,
the same line `usb_descriptor_survey` uses, and it resolves on both machines.
This is the opposite of `usb_audio_capture_test`, which deliberately runs with
*no* device attached; do not copy that shape here. Machine, build directory and
`-serial` chain all come from `gate-lib.sh`, never spelled out in the script.

Asserts, in order: banner → device attaches → `audio=ready` → the parsed
topology line → `alt=1` → siTD posted → siTD error flags all clear →
`streaming started` → heartbeat reaches `seq=2`.

Vacuity guards: the capture must **not** contain `siTD POST FAILED` or
`STREAM START FAILED`.

★ **CORRECTION, measured 2026-08-08 (Task 2).** An earlier revision of this
section asserted `SITD PASS` and named `SITD FAIL` as a vacuity guard. **Both
were wrong**, and the evidence was second-hand — taken from
`usb_descriptor_survey`'s gate comments, which describe a *different* example.
Measured directly, on **both** boards identically:

```
UAC1-TEST: siTD posted, 180 bytes
UAC1-TEST: siTD active=1 xact_err=0 babble=0 buf_err=0 bytes_left=180
UAC1-TEST: SITD FAIL - see flags above
```

The controller never executes the descriptor — `active=1`, `bytes_left` still
180 — with **no error flags set**. That is the same "iso does not flow" fact
stated another way, so `SITD PASS` is unreachable under QEMU and `SITD FAIL` is
the *expected* output, not a failure signal.

The gate therefore asserts `xact_err=0 babble=0 buf_err=0` instead. That is the
part which is both true and meaningful: a malformed siTD would set those bits,
so it still catches a real regression, and it stays correct if QEMU ever gains
working isochronous transfer (`SITD PASS` requires those same flags clear).
`SITD PASS` remains the **silicon** bar, where it is reachable.

★ **The gate must not assert `pkts/s > 0`.** Isochronous data does not flow
against QEMU's model — measured, and already recorded in
`usb_descriptor_survey/run_qemu.sh`: an OUT sketch built for 48000 "enumerates,
claims, selects alt 1, completes the whole control sequence with `ctrl=0/0/0`
and reports *streaming started*, and then sits at `pkts/s=0`". This gate proves
the **control plane**. Silicon is the sole proof that audio moves, and that
limitation belongs in the gate's own header comment, not only here.

### 4.3 Close-out

- `examples/usb/usb_audio_uac1_test/build-rt1062:usb_audio_uac1_test` added to
  `tools/license-audit.sh` `GATES`.
- Sweep **84 → 86**, zero SKIP.
- `transcript_hw_evkb.txt` committed with the silicon run.
- `CLAUDE.md` and `docs/KNOWN-BROKEN-GATES.md` updated.

---

## 5. Follow-ups — recorded, not done here

**B — the 600 MHz stall-headroom number.** §1.4 of the board-axis design says
the RT1062 has *less* stall headroom than the RT1176: the duty cost scales to
~1.9 % at 600 MHz, but the stall ceiling is wall-clock (USB frames are 1 ms
regardless of core speed) while everything between service points takes 1.66×
longer. The instrument already exists in this example — `pkts/s` is documented
as "the correctness measure for the ring: at 48 kHz with one frame per packet it
must sit near 1000. Lower means frames went out empty because `service()` did
not get round the ring in time." Recording `pkts/s` and the siTD error counts on
silicon answers the question at no extra build cost.

**C — a valid cache A/B.** The obvious experiment (flip OCRAM back to
write-back and compare) **is not available**: with OCRAM cached the descriptors
go stale and the device does not enumerate at all, so there is no working cached
build to measure. The valid form is to give `USBHOST_DMAMEM` its own non-cached
section with its own MPU region, restore OCRAM to `MEM_CACHE_WBWA`, and compare
`pkts/s` between that and today's board-wide uncached mapping. That is also the
refinement to reach for if B shows the ring starving.

---

## 6. Risks

| Risk | Handling |
|---|---|
| `usb_audio*.cpp` hides an RT1176 assumption | This is its first compile for `__IMXRT1062__`, which is what surfaces one. Per the board-axis design §6, if one appears it belongs in the transport layer, where the pattern already exists — not in the example. |
| Uncached DMAMEM starves the ring at 600 MHz | `pkts/s` is the detector; follow-up B quantifies it, C is the fix if needed. Not blocking: the tone is audible or it is not. |
| The gate passes while audio never moves | Accepted and documented. The QEMU model cannot move iso data; this is the same split `usb_descriptor_survey` already lives with, and it is why the silicon bar exists. |
| A fresh clone sees the rt1062 gate red | Same GPL-firewall situation as `usb_descriptor_survey` and `cm4_usb_irq_probe`: the rt1062 half needs local-only qemu2 changes. Document, do not work around. |

---

## 7. Definition of done

- [ ] `usb_audio_uac1_test` builds for both boards; rt1062 entry point `0x60001000`
- [ ] Gate green on both boards, asserting the control plane per §4.2
- [ ] Audible 1 kHz tone from J47 on the MIMXRT1060-EVKB, `SITD PASS`
- [ ] `transcript_hw_evkb.txt` committed
- [ ] Sweep 86 gates, zero SKIP
- [ ] `LICENSE-AUDIT: PASS` with `build-rt1062` walked
- [ ] `CLAUDE.md` and `KNOWN-BROKEN-GATES.md` updated
