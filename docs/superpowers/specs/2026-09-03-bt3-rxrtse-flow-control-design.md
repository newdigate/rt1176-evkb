# BT-3 RXRTSE Hardware Flow Control — Design

**Date:** 2026-09-03
**Programme:** M.2 Bluetooth A2DP (NEW-9, BT-3). Sub-project of phase 4
(`AudioOutputBluetooth`).
**Status:** Design, pending user review.

## Problem

On silicon, BT-3 phase 4 brings up cleanly (connect → legacy-PIN pair → encrypt
→ SDP → AVDTP START, the ESP32 sink echoes our exact SBC config and decodes 24
packets), but **host media send hard-stalls after 0–43 packets and no audio is
heard.** The sink is ruled out — it reaches `state=started` and decodes what it
receives. The stall is on the host transmit path.

### Root cause (measured, 2026-09-03)

The A2DP media flow is: host streams SBC-over-L2CAP-over-ACL to the card; the
card returns `HCI_Number_Of_Completed_Packets` (NCP) events on its UART TX →
host RX; each NCP returns ACL credits. The driver assigns
`Num_HCI_Command_Packets`/ACL credit **absolutely** from each event, so a single
lost NCP event leaves credit stuck — no credit means no send, no send means no
further NCP, and the stream wedges.

The host RX path cannot keep up during media:

- LPUART2's RX FIFO is **4 bytes deep** (core-hardcoded: `RXFIFOSIZE=1`), drained
  by the RX ISR (`RIE`) into a 1 KB software ring. At the 3 Mbaud HCI rate, 4
  bytes is ~13 µs of slack.
- Any window where the LPUART2 RX IRQ is masked or delayed longer than that —
  the audio software ISR running the SBC encode, another peripheral ISR, a core
  critical section — overruns the 4-byte FIFO and drops bytes (`STAT[OR]`).
- When the dropped bytes fall inside an NCP event, credit never returns.

The board offers **no flow control today**: the host's RTS→card-CTS net
(`BT_UART_RTS`) is held statically LOW by a GPIO hack (`m2AssertBtCts`), meaning
"card always clear to send" — the card is never asked to pause, so the 4-byte
FIFO is the only backstop and it is not enough at media rate.

The i.MX RT1176 LPUART has the exact hardware feature for this: **RXRTSE**
(receiver request-to-send). With it, the receiver auto-deasserts `LPUART2_RTS_B`
as the RX FIFO nears full, pausing the card *before* overrun, then re-asserts as
the ISR drains. `GPIO_DISP_B2_13` ALT3 **is** `LPUART2_RTS_B` — the wire is
already routed to the card (J54 pin 36); we have only ever driven it as a static
GPIO, never as hardware RTS.

## Goal

Land hardware RXRTSE flow control on LPUART2 and carry BT-3 phase 4 to its
**audible acceptance**: a 1 kHz tone streamed from `bt_tone_test` plays on the
ESP32 sink, sustained for minutes, with `drops=0` and ACL credit that never
sticks at 0.

## Non-goals

- Bidirectional (TX) CTS flow control (`TXCTSE`). The card→host direction is the
  overrun path; the host→card direction is not credit-starved. RM forbids setting
  both `RXRTSE` and `TXRTSE`; we set only `RXRTSE`.
- Making ENET and Bluetooth coexist. On this board RXRTSE and 1 GbE ENET are
  mutually exclusive (see Board constraint); this design accepts that and keeps
  flow control opt-in and default-off.
- eDMA RX for LPUART2 (a larger alternative that would avoid the PHY side effect
  entirely). Recorded as a future option; out of scope here.
- Headset (SSP) media playback. Phase 4 acceptance is against the ESP32 sink over
  legacy PIN, as in phase 2.

## Board constraint (decisive, and why this is opt-in)

`R1866` ties the `BT_UART_RTS` net to `ETHPHY_RST_B`. RX RTS is **fixed
active-low** (RM: `TXRTSPOL` does not affect the receiver RTS). Asserted RTS =
LOW = the gigabit ENET PHY held in reset. Therefore:

- Enabling RXRTSE holds/toggles the ENET PHY reset. **Fine for a Bluetooth
  example, fatal for any ENET example.**
- Flow control must be **explicit opt-in, default OFF**, so no existing example
  changes behaviour. Nothing in the core's `begin()` enables it; only an explicit
  `attachRts()` call does.

## RM facts that shape the code (i.MX RT1170 RM Rev 5, §71.3.5.3 / MODIR)

- `MODIR.RXRTSE` (bit 3): "RTS is deasserted if the receiver data register is
  full or a start bit has been detected that would cause it to become full;
  asserted otherwise." Enables receiver flow control.
- `MODIR.RTSWATER` (bits 9:8): configures the FIFO threshold. "RX RTS_B negates
  when the number of *empty* words in the receive FIFO is ≥ RTSWATER; if RTSWATER
  = 0, RTS_B negates when the receive FIFO is full." The prose is ambiguous about
  polarity/sense, so the **exact RTSWATER value is tuned on the bench** by
  watching for overruns (`STAT[OR]`) and `drops` — starting conservative (leave
  ≥1–2 empty words of headroom before the card is paused).
- **"This bit should be changed only when the receiver is disabled."** MODIR must
  be programmed with `CTRL[RE]=0`. This is why the core is the right home: only
  `begin()`/`attachRts()` cleanly own the RE-down/RE-up window.
- "Do not set both RXRTSE and TXRTSE." We set only RXRTSE.
- "Even if RTS_B is deasserted, the receiver continues to receive characters
  until the FIFO is overrun." So there is still a latency race between deassert
  and the card ceasing — headroom (RTSWATER) plus the card's own UART latency
  must cover it. Bench-verified, not assumed.

## Architecture

Four changes, smallest blast radius first.

### 1. Core — `HardwareSerialIMXRT::attachRts()` (`teensy-cores/imxrt1176`)

New opt-in method on the core UART class, wired for Serial2 only.

**API**

```cpp
// Enable hardware receiver RTS flow control (RXRTSE) on this port, using the
// port's dedicated LPUART RTS pad. rtswater = the MODIR RTSWATER threshold
// (0..3; FIFO depth is 4). Returns false if this port has no RTS pad configured.
// ★ Board side effect on Serial2 (MIMXRT1170-EVKB): the RTS pad also drives the
// gigabit ENET PHY reset (R1866), so do NOT call this in an ENET example.
// Opt-in, default off: begin() never enables RTS on its own.
bool attachRts(uint8_t rtswater = 1);
void detachRts();   // clears RXRTSE, returns the pad to input (deasserted)
```

**`hardware_t` gains RTS pad fields** (populated for Serial2, left
`iomuxc_no_daisy`/0 for Serial1 which has no usable RTS pad):

```cpp
volatile uint32_t &rts_mux_reg;  uint32_t rts_mux_val;  volatile uint32_t &rts_pad_reg;
```

For Serial2: `IOMUXC_SW_MUX_CTL_PAD_GPIO_DISP_B2_13`, ALT `3` (`LPUART2_RTS_B`),
`IOMUXC_SW_PAD_CTL_PAD_GPIO_DISP_B2_13`.

**New register/pad defines in `imxrt1176.h`** (currently absent — the pad table
stops at `DISP_B2_12`, and there are no `LPUART_MODIR_*` bit defines):

```c
#define IOMUXC_SW_MUX_CTL_PAD_GPIO_DISP_B2_13 (*(volatile uint32_t *)0x400E8248u)
#define IOMUXC_SW_PAD_CTL_PAD_GPIO_DISP_B2_13 (*(volatile uint32_t *)0x400E848Cu)
#define LPUART_MODIR_RXRTSE     ((uint32_t)(1u << 3))
#define LPUART_MODIR_TXRTSE     ((uint32_t)(1u << 1))
#define LPUART_MODIR_RTSWATER(n)((uint32_t)(((n) & 0x3u) << 8))
```

(Addresses continue the existing +4 pattern and match the values the
`m2_hci_probe` example already uses as literals.)

**Pure, host-testable register logic** in a new dependency-free header
`imxrt1176/lpuart_rts.h` (no Arduino includes):

```cpp
namespace lpuart_rts {
    // The MODIR value for receiver flow control: RXRTSE set, TXRTSE clear,
    // RTSWATER = watermark. This is the whole of the register decision, isolated
    // so it host-compiles and is unit-tested.
    inline uint32_t modir(uint8_t rtswater) {
        return (1u << 3) | (((uint32_t)(rtswater & 0x3u)) << 8);  // RXRTSE | RTSWATER
    }
}
```

`attachRts()` behaviour:

1. If `rts_mux_reg` is the `iomuxc_no_daisy` sentinel → return false (no RTS pad).
2. Mux the RTS pad: `rts_mux_reg = rts_mux_val;` `rts_pad_reg = PAD_CFG (0x02)`.
3. RE-safe MODIR write: read `CTRL`, clear `RE`, write
   `MODIR = lpuart_rts::modir(rtswater)`, restore `CTRL`. (Wrapped in
   `__disable_irq()/__enable_irq()`.)
4. Set `rts_enabled_ = true; rts_water_ = rtswater;` so `begin()` re-applies it.
5. Return true.

**`begin()` re-applies MODIR** so the 3 Mbaud `rebaud()` (which is
`end()`+`begin()` and rewrites `WATER`/`CTRL` but not `MODIR`) does not silently
drop flow control. Between the FIFO-enable (line 111) and the `CTRL` enable (line
114) — i.e. while `RE` is still 0 — add:

```cpp
if (rts_enabled_) {
    hardware->rts_mux_reg = hardware->rts_mux_val;  hardware->rts_pad_reg = 0x02u;
    port->MODIR = lpuart_rts::modir(rts_water_);
}
```

`detachRts()` clears `rts_enabled_`, writes `MODIR = 0` (RE-safe), and returns
the pad to a deasserted input.

### 2. Example — `bt_tone_test` uses real RTS, not the GPIO hack

Replace the `m2AssertBtCts()` static-LOW GPIO hack with `Serial2.attachRts()`,
called **after** the boot-sleep wake pulse and after the transport's `begin()`
(so RE exists to toggle). `A2dpSource` and `HciTransport` need no new API — the
core's `rts_enabled_` flag carries flow control across the 3 Mbaud `rebaud()`
automatically. The card's CTS input is now driven by hardware, deasserted when
the host FIFO fills.

Guard the call so `bt_tone_test` still builds/behaves on the card-absent QEMU
path (it already gates media bring-up on `A2dpSource::connect()` succeeding).

### 3. `MediaPacketizer` — close the documented `m_rd` two-writer race
(`M2Radio/bt/MediaPacketizer.{h,cpp}`)

The ring's own comment flags it: `m_rd` has two writers (the ISR's drop-oldest
path in `push()`, and `drain()`'s send-commit), safe **only while `drops==0`**.
Phase-4 audible acceptance *depends on* `drops==0`, so we do the documented FIX
PATH now rather than rely on a race being quiet:

Wrap `drain()`'s `m_rd` commit in an injected critical section that **rebases**
onto the current `m_rd` instead of clobbering it — so a concurrent ISR drop is
never lost. The critical-section primitive is injected so the host test stays
pure:

```cpp
// no-op on host; __disable_irq/__enable_irq on ARM (provided by the caller/build)
```

Add `mediapacketizer_test` checks that a drop concurrent with a drain does not
resurrect a sacrificed frame nor lose a counted drop (simulated by invoking the
drop path between gather and commit).

### 4. Batching — verify, don't rebuild

`MediaPacketizer` **already** batches up to `m_perPkt` (capped at 8) whole SBC
frames per RTP/L2CAP packet, sized from the negotiated media MTU. This cuts the
ACL-packet rate — and thus the NCP rate the host RX must absorb — by ~5–8×. This
is a **verification** task: confirm the negotiated media MTU yields
`m_perPkt ≥ 5` (log it in the heartbeat), and only revisit if it does not. No new
batching code is planned. (The earlier failed "batch when pending ≥ perPkt"
drain-cadence change was correctly reverted; drain still sends whatever is queued
each poll.)

## Testing

**QEMU cannot see this fix or the bug** (verified in qemu2 `hw/char/imxrt_lpuart.c`):
the model stores `MODIR` but never acts on it, and its `can_receive` already
backpressures the socket peer via `fifo32_num_free`, so the overrun-loss never
reproduces and RXRTSE has no observable effect. This is the same situation as the
GC355 GPU: **the automated gate is a host unit test; silicon is the real proof.**

1. **Host unit test (automated, deterministic, demonstrated-red)** — new
   `imxrt1176/extras/lpuart_rts_test.cpp` + `run.sh` (mirrors the M2Radio
   `bt/test/run.sh` style), testing `lpuart_rts::modir()`:
   - `RXRTSE` set, `TXRTSE` **clear** (RM: never both).
   - `RTSWATER` field = the requested watermark, for 0..3.
   - Demonstrated red by asserting a wrong bit before the real value.

2. **`mediapacketizer_test`** — the new race checks above (host, in M2Radio
   `bt/test/run.sh`, which already runs `mediapacketizer_test`).

3. **No-regression** — the existing `[hci]`, `[media]`, `[avdtp]`, `[baud]` QEMU
   gates stay green; `bt_tone_test`'s card-absent `run_qemu.sh` vacuity
   assertions still hold (RTS is never asserted with no card).

4. **Silicon acceptance (the real proof)** — on the EVKB + IW416, streaming
   `bt_tone_test` to the ESP32 sink over legacy PIN:
   - **Audible** 1 kHz tone, sustained ≥ 5 minutes without stall.
   - `drops=0` in the heartbeat over the whole run.
   - ACL credit observed to return steadily (NCP events parsed; credit never
     sticks at 0; `packets` climbs monotonically for minutes, not to 43 and stop).
   - `RTSWATER` value recorded in `transcript_hw_evkb.txt` with the overrun/drops
     evidence that justified it.

## Acceptance criteria

- [ ] `attachRts()` on the core, opt-in, default off; `begin()`/`rebaud()`
      re-apply MODIR; host unit test green and demonstrated-red.
- [ ] `bt_tone_test` uses `Serial2.attachRts()` in place of the static-GPIO CTS
      hack; card-absent QEMU gate still green.
- [ ] `MediaPacketizer` `m_rd` race closed; `mediapacketizer_test` green with the
      new checks.
- [ ] Negotiated media MTU yields `m_perPkt ≥ 5` (logged); no new batching code
      needed, or the shortfall is explained.
- [ ] **Silicon: audible tone on the ESP32 sink, ≥ 5 min, `drops=0`, credit never
      sticks.** `transcript_hw_evkb.txt` captures it.
- [ ] Full QEMU sweep green (target count unchanged; this adds host tests, not
      QEMU gates); `LICENSE-AUDIT: PASS`.
- [ ] `CLAUDE.md` records the silicon-only nature and why QEMU can't gate it;
      memory (`m2-bluetooth-a2dp-programme.md`) + NEW-9 updated.

## Risks & open (bench-tuned) values

- **RTSWATER polarity/value.** RM prose is ambiguous; the working value is
  determined on the bench from `STAT[OR]`/`drops`, starting conservative. Risk:
  too aggressive throttles NCP delivery; too slack still overruns. Mitigated by
  measuring both extremes.
- **RTS-deassert-to-card-cease latency.** The card keeps sending after deassert;
  4-byte FIFO + chosen headroom must cover the card's UART latency. Verified by
  observing zero overruns at the chosen RTSWATER, not assumed.
- **PHY reset toggling.** Expected and accepted (no ENET in this example). Noted
  so a future ENET+BT coexistence effort knows RXRTSE is off-limits without the
  eDMA-RX alternative.
- **`rebaud()` regression.** Explicitly covered by re-applying MODIR in `begin()`;
  the host test cannot exercise the silicon `rebaud()`, so the silicon soak (which
  runs *after* the 3 Mbaud switch) is the guard.

## File-by-file changes

**`teensy-cores` (sibling repo — pin bump in `evkb.cmake` after push):**
- `imxrt1176/imxrt1176.h` — add `DISP_B2_13` mux/pad defines + `LPUART_MODIR_*`.
- `imxrt1176/lpuart_rts.h` — new, pure `modir()` helper.
- `imxrt1176/HardwareSerial.h` — `hardware_t` RTS fields; `attachRts()`/
  `detachRts()` decls; `rts_enabled_`/`rts_water_` members.
- `imxrt1176/HardwareSerial.cpp` — `attachRts()`/`detachRts()`; `begin()`
  re-applies MODIR while RE=0.
- `imxrt1176/HardwareSerial2.cpp` — populate Serial2's RTS pad fields.
- `imxrt1176/extras/lpuart_rts_test.cpp` + `imxrt1176/extras/run.sh` — host test.

**`M2Radio` (sibling repo — pin bump after push):**
- `bt/MediaPacketizer.cpp` (+ `.h` if a crit-section hook is added) — close the
  `m_rd` race.
- `bt/test/mediapacketizer_test.cpp` — new race checks.

**`rt1176-evkb` (this repo):**
- `examples/audio/bt_tone_test/bt_tone_test.cpp` — `Serial2.attachRts()` replaces
  the CTS GPIO hack; heartbeat logs `m_perPkt`/`RTSWATER`/overrun.
- `examples/audio/bt_tone_test/transcript_hw_evkb.txt` — silicon evidence.
- `evkb.cmake` — bump `cores` and `M2Radio` pins after their pushes; verify
  fresh-user with `-DEVKB_FORCE_FETCH=ON`.
- `CLAUDE.md`, `docs/superpowers/…` — notes.

## Bench / hygiene reminders (from the programme memory)

- Detach the VCOM reader before any LinkServer op; don't hold VCOM while
  programming; never `pkill -9` mid-flash-program; read `DHCSR` first if the
  image looks dead.
- Open the ESP32 sink's serial with `dtr=False, rts=False` or its reader
  auto-resets it to the bootloader.
- `loop()` heartbeat is the source of truth across a reset (setup() output is
  missed).
- Clean-room only: RXRTSE/MODIR facts are from the NXP RM (permissive to cite);
  no NXP driver source transcribed. Stage specific files, never `git add -A`.
