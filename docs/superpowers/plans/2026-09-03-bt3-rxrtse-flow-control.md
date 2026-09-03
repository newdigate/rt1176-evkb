# BT-3 RXRTSE Hardware Flow Control Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Add opt-in hardware RXRTSE flow control to LPUART2 so the IW416 pauses its UART TX before the host's 4-byte RX FIFO overruns, stop losing `HCI_Number_Of_Completed_Packets`, and carry BT-3 phase 4 to an audible, `drops=0`, minutes-long tone on the ESP32 sink.

**Architecture:** A pure, host-tested register helper (`lpuart_rts::modir`) plus a new opt-in `Serial2.attachRts()` on the imxrt1176 core that muxes `GPIO_DISP_B2_13`→ALT3 (`LPUART2_RTS_B`) and sets `MODIR.RXRTSE`+`RTSWATER` while `RE=0`, re-applied by `begin()` so the 3 Mbaud `rebaud()` cannot drop it. The `MediaPacketizer` ring's documented two-writer race on `m_rd` is closed with a monotonic-forward, IRQ-guarded commit. The example switches from the static-GPIO CTS hack to `attachRts()`. QEMU cannot model any of this, so the automated proof is host unit tests; silicon is the real proof.

**Tech Stack:** C++11 (ARM GCC 10 firmware + host `c++` unit tests), i.MX RT1176 LPUART2, CMake, custom QEMU gate harness, LinkServer, ESP32 A2DP sink.

**Repos touched (all sibling checkouts):**
- `~/Development/teensy-cores` — the `imxrt1176` core (Tasks 1, 2).
- `~/Development/M2Radio` — `bt/MediaPacketizer` (Task 3).
- `~/Development/rt1170/evkb` — the example + pins + docs (Tasks 4, 5, 7). This repo commits to **`master`** (all prior BT-3 work is on master in the main checkout).

**Execution note:** Tasks 1–5 and 7 are subagent-executable (code + host tests + build + sweep). **Task 6 is bench work** on real hardware, driven interactively (LinkServer + ESP32), not by a subagent.

---

### Task 1: Core — pure `lpuart_rts::modir()` helper + host unit test

**Files:**
- Create: `~/Development/teensy-cores/imxrt1176/lpuart_rts.h`
- Create: `~/Development/teensy-cores/imxrt1176/extras/lpuart_rts_test/lpuart_rts_test.cpp`
- Create: `~/Development/teensy-cores/imxrt1176/extras/lpuart_rts_test/run.sh`

This is the deterministic automated gate for the whole sub-project. It isolates the *only* register decision (which MODIR bits) into a dependency-free function so it host-compiles and is unit-tested — the register plumbing in Task 2 cannot be host-tested (it pokes CMSIS/IOMUXC), so this is where the proof lives.

- [ ] **Step 1: Write the pure helper header**

Create `imxrt1176/lpuart_rts.h`:

```cpp
// Pure, dependency-free register logic for LPUART receiver RTS (RXRTSE) hardware
// flow control.  No Arduino/CMSIS includes, so it host-compiles and is unit
// tested (imxrt1176/extras/lpuart_rts_test/).  MIT.
#pragma once
#include <stdint.h>

namespace lpuart_rts {
    // The MODIR value for RECEIVER flow control on an i.MX RT1170 LPUART:
    //   bit3   RXRTSE  = 1  (receiver deasserts RTS as the RX FIFO nears full)
    //   bit1   TXRTSE  = 0  (RM 71.5.1.11: never set both RXRTSE and TXRTSE)
    //   bits9:8 RTSWATER = watermark (0..3; the RX FIFO is 4 deep)
    // This is the whole register decision, isolated for testing.
    inline uint32_t modir(uint8_t rtswater) {
        return (1u << 3) | (((uint32_t)(rtswater & 0x3u)) << 8);
    }
}
```

- [ ] **Step 2: Write the failing test**

Create `imxrt1176/extras/lpuart_rts_test/lpuart_rts_test.cpp`:

```cpp
#include "lpuart_rts.h"
#include <stdio.h>
static int g_fails = 0, g_checks = 0;
#define CHECK(c) do { g_checks++; if (!(c)) { g_fails++; printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); } } while (0)

int main() {
    // RXRTSE (bit3) set for every watermark; TXRTSE (bit1) never set; RTSWATER carried.
    for (uint8_t w = 0; w <= 3; w++) {
        uint32_t m = lpuart_rts::modir(w);
        CHECK(m & (1u << 3));               // RXRTSE set
        CHECK(!(m & (1u << 1)));            // TXRTSE clear (RM: never both)
        CHECK(((m >> 8) & 0x3u) == w);      // RTSWATER field == w
    }
    // No stray bits outside RXRTSE | RTSWATER.
    CHECK(lpuart_rts::modir(0) == (1u << 3));
    CHECK(lpuart_rts::modir(3) == ((1u << 3) | (3u << 8)));
    printf("lpuart_rts_test: %d checks, %d failures\n", g_checks, g_fails);
    return g_fails ? 1 : 0;
}
```

Create `imxrt1176/extras/lpuart_rts_test/run.sh`:

```sh
#!/bin/sh
set -e
DIR=$(cd "$(dirname "$0")" && pwd); OUT=$(mktemp -d); trap 'rm -rf "$OUT"' EXIT
CXX=${CXX:-c++}
$CXX -std=c++11 -Wall -Wextra -Werror -I"$DIR/../.." "$DIR/lpuart_rts_test.cpp" -o "$OUT/t"
"$OUT/t"
echo "LPUART-RTS-HOST-TEST: PASS"
```

Then make it executable and run it — it should PASS (the helper already exists):

Run: `chmod +x ~/Development/teensy-cores/imxrt1176/extras/lpuart_rts_test/run.sh && ~/Development/teensy-cores/imxrt1176/extras/lpuart_rts_test/run.sh`
Expected: `lpuart_rts_test: 15 checks, 0 failures` then `LPUART-RTS-HOST-TEST: PASS`

- [ ] **Step 3: Demonstrate the test can fail (mutation check)**

Temporarily edit `imxrt1176/lpuart_rts.h` to also set TXRTSE — change the return to `(1u << 3) | (1u << 1) | (((uint32_t)(rtswater & 0x3u)) << 8)`.

Run: `~/Development/teensy-cores/imxrt1176/extras/lpuart_rts_test/run.sh`
Expected: FAIL on `!(m & (1u << 1))`, non-zero exit. Then **revert** the mutation and re-run — back to `15 checks, 0 failures`.

- [ ] **Step 4: Commit**

```bash
cd ~/Development/teensy-cores
git add imxrt1176/lpuart_rts.h imxrt1176/extras/lpuart_rts_test/lpuart_rts_test.cpp imxrt1176/extras/lpuart_rts_test/run.sh
git commit -m "feat(imxrt1176): pure lpuart_rts::modir() + host unit test

RXRTSE|RTSWATER register logic isolated dependency-free so it host-compiles
and is unit-tested; the automated proof for hardware flow control, which QEMU
cannot model.

Co-Authored-By: Claude Opus 4.8 <noreply@anthropic.com>"
```

---

### Task 2: Core — `attachRts()`/`detachRts()`, `hardware_t` RTS fields, `begin()` re-apply, defines

**Files:**
- Modify: `~/Development/teensy-cores/imxrt1176/imxrt1176.h` (add `DISP_B2_13` + `LPUART_MODIR_*` defines)
- Modify: `~/Development/teensy-cores/imxrt1176/HardwareSerial.h` (`hardware_t` fields, members, method decls)
- Modify: `~/Development/teensy-cores/imxrt1176/HardwareSerial.cpp` (`attachRts`/`detachRts`, `begin()` re-apply)
- Modify: `~/Development/teensy-cores/imxrt1176/HardwareSerial2.cpp` (populate Serial2 RTS pad)
- Modify: `~/Development/teensy-cores/imxrt1176/HardwareSerial1.cpp` (fill the new fields with the no-RTS sentinel)

This is firmware register plumbing — it cannot be host-unit-tested (touches IOMUXC/CMSIS). Its correctness is: (a) it compiles into `libcores`, verified by building `bt_tone_test` in Task 4; (b) the MODIR value it writes is the Task-1-tested `lpuart_rts::modir()`; (c) silicon in Task 6.

- [ ] **Step 1: Add the missing register/pad defines**

In `imxrt1176/imxrt1176.h`, immediately after the `IOMUXC_SW_PAD_CTL_PAD_GPIO_DISP_B2_12` define (line ~769), add:

```c
#define IOMUXC_SW_MUX_CTL_PAD_GPIO_DISP_B2_13 (*(volatile uint32_t *)0x400E8248u)
#define IOMUXC_SW_PAD_CTL_PAD_GPIO_DISP_B2_13 (*(volatile uint32_t *)0x400E848Cu)
```

Near the other `LPUART_*` defines (after `LPUART_WATER_RXWATER`, line ~338), add:

```c
#define LPUART_MODIR_RXRTSE      ((uint32_t)(1u << 3))
#define LPUART_MODIR_TXRTSE      ((uint32_t)(1u << 1))
#define LPUART_MODIR_RTSWATER(n) ((uint32_t)(((n) & 0x3u) << 8))
```

(Addresses continue the existing +4 pattern; they are the literals the `m2_hci_probe` example already hard-codes.)

- [ ] **Step 2: Extend `hardware_t`, add members and method decls**

In `imxrt1176/HardwareSerial.h`, inside the `hardware_t` struct (after the `rx_select_input_reg`/`rx_select_input_val` line, ~132), add:

```cpp
		volatile uint32_t &rts_mux_reg;	uint32_t rts_mux_val;	volatile uint32_t &rts_pad_reg;
```

In the public method section (after `addMemoryForWrite`, ~169), add:

```cpp
	// Enable hardware RECEIVER RTS flow control (RXRTSE) on this port's dedicated
	// LPUART RTS pad: the receiver auto-deasserts RTS as the RX FIFO nears full,
	// pausing the peer before overrun.  rtswater = MODIR RTSWATER (0..3; FIFO is
	// 4 deep).  Returns false if this port has no RTS pad.  Opt-in: begin() never
	// enables RTS on its own; once attached, begin()/rebaud re-apply it.
	// ★ Board side effect on Serial2 (MIMXRT1170-EVKB): the RTS pad also drives
	// the gigabit ENET PHY reset (R1866) -- do NOT use in an ENET example.
	bool attachRts(uint8_t rtswater = 1);
	void detachRts();
```

In the `private:` members (near `transmitting_`, ~195), add:

```cpp
	bool				rts_enabled_ = false;
	uint8_t				rts_water_ = 0;
```

- [ ] **Step 3: Implement `attachRts`/`detachRts` and re-apply in `begin()`**

In `imxrt1176/HardwareSerial.cpp`, add near the top include block:

```cpp
#include "lpuart_rts.h"
```

In `begin()`, between `port->FIFO |= LPUART_FIFO_TXFE | LPUART_FIFO_RXFE;` (line ~111) and `port->CTRL = LPUART_CTRL_TE | LPUART_CTRL_RE | LPUART_CTRL_RIE;` (line ~114) — i.e. while RE is still 0 — insert:

```cpp
	// Re-apply hardware RTS if attachRts() enabled it: rebaud() is end()+begin(),
	// and this is the only point where RE is still 0 (MODIR must be written with
	// the receiver disabled, RM 71.5.1.11), so flow control survives the 3 Mbaud
	// switch instead of being silently dropped.
	if (rts_enabled_) {
		hardware->rts_mux_reg = hardware->rts_mux_val;   // ALT3 = LPUART_RTS_B
		hardware->rts_pad_reg = 0x02u;
		port->MODIR = lpuart_rts::modir(rts_water_);
	}
```

After `end()` (after line ~125), add the two methods:

```cpp
bool HardwareSerialIMXRT::attachRts(uint8_t rtswater)
{
	// Ports with no RTS pad (Serial1) leave rts_mux_reg at the iomuxc_no_daisy
	// sentinel -- refuse rather than mux a wrong pad.
	if (&hardware->rts_mux_reg == &iomuxc_no_daisy) return false;
	IMXRT_LPUART_t *port = (IMXRT_LPUART_t *)port_addr;
	rts_enabled_ = true;
	rts_water_ = rtswater & 0x3u;
	hardware->rts_mux_reg = hardware->rts_mux_val;   // ALT3 = LPUART_RTS_B
	hardware->rts_pad_reg = 0x02u;
	// MODIR "should be changed only when the receiver is disabled" (RM 71.5.1.11).
	__disable_irq();
	uint32_t ctrl = port->CTRL;
	port->CTRL = ctrl & ~LPUART_CTRL_RE;
	port->MODIR = lpuart_rts::modir(rts_water_);
	port->CTRL = ctrl;
	__enable_irq();
	return true;
}

void HardwareSerialIMXRT::detachRts()
{
	IMXRT_LPUART_t *port = (IMXRT_LPUART_t *)port_addr;
	rts_enabled_ = false;
	__disable_irq();
	uint32_t ctrl = port->CTRL;
	port->CTRL = ctrl & ~LPUART_CTRL_RE;
	port->MODIR = 0;                                  // RXRTSE off; RM: RTS then stays deasserted
	port->CTRL = ctrl;
	__enable_irq();
}
```

- [ ] **Step 4: Populate Serial2's RTS pad and Serial1's sentinel**

In `imxrt1176/HardwareSerial2.cpp`, in the `UART2_Hardware` initializer, replace the `iomuxc_no_daisy, 0u,` line (the RXD-daisy line) so the three new fields follow it:

```cpp
	iomuxc_no_daisy, 0u,                      // LPUART2 has no RXD daisy register
	IOMUXC_SW_MUX_CTL_PAD_GPIO_DISP_B2_13, 3u, IOMUXC_SW_PAD_CTL_PAD_GPIO_DISP_B2_13, // RTS: ALT3 = LPUART2_RTS_B (J54 pin 36 = card CTS; also ENET PHY reset via R1866)
	IRQ_PRIORITY,
```

In `imxrt1176/HardwareSerial1.cpp`, find `UART1_Hardware`'s initializer and add the three no-RTS-pad sentinel fields in the same position (after its RXD-daisy fields, before `IRQ_PRIORITY`):

```cpp
	iomuxc_no_daisy, 0u, iomuxc_no_daisy,     // Serial1/LPUART1 has no usable RTS pad on this board
```

(Confirm the exact existing field layout in `HardwareSerial1.cpp` before editing; match Serial2's ordering.)

- [ ] **Step 5: Verify the core still compiles (via the example build in Task 4 dependency)**

The core has no standalone build; it is compiled when an example links it. Defer the compile check to Task 4 Step 3, which builds `bt_tone_test`. If iterating locally now, a fast smoke check is to configure any rt1176 example against this working-tree core (local-first resolution) and build — e.g. `examples/gpio-analog/blink`.

Run: `cd ~/Development/rt1170/evkb/examples/gpio-analog/blink && rm -rf build-rtscheck && cmake -B build-rtscheck -DCMAKE_TOOLCHAIN_FILE=../../../toolchain/rt1170-evkb.toolchain.cmake >/dev/null && cmake --build build-rtscheck 2>&1 | tail -3`
Expected: builds to `blink.elf` with no errors (proves the `hardware_t` change compiles for a port that does NOT call attachRts). Then `rm -rf build-rtscheck`.

- [ ] **Step 6: Commit**

```bash
cd ~/Development/teensy-cores
git add imxrt1176/imxrt1176.h imxrt1176/HardwareSerial.h imxrt1176/HardwareSerial.cpp imxrt1176/HardwareSerial2.cpp imxrt1176/HardwareSerial1.cpp
git commit -m "feat(imxrt1176): opt-in Serial2.attachRts() hardware RXRTSE flow control

Muxes GPIO_DISP_B2_13->ALT3 (LPUART2_RTS_B), sets MODIR RXRTSE|RTSWATER with
RE=0, and re-applies in begin() so the 3 Mbaud rebaud does not drop it.
Default off (the RTS pad also drives the ENET PHY reset via R1866).

Co-Authored-By: Claude Opus 4.8 <noreply@anthropic.com>"
```

---

### Task 3: `MediaPacketizer` — close the `m_rd` two-writer race

**Files:**
- Modify: `~/Development/M2Radio/bt/MediaPacketizer.h` (expose `advanceRd` static for test)
- Modify: `~/Development/M2Radio/bt/MediaPacketizer.cpp` (`advanceRd` + IRQ-guarded commit)
- Test: `~/Development/M2Radio/bt/test/mediapacketizer_test.cpp` (race checks)

The ring's own comment flags `m_rd` as having two writers (the ISR drop-oldest path in `push()`, and `drain()`'s commit), safe only while `drops==0`. Audible acceptance depends on `drops==0`, so close it now with a monotonic-forward commit that never clobbers a concurrent drop, guarded by an IRQ critical section on ARM (no-op on host).

- [ ] **Step 1: Add the failing unit test for `advanceRd`**

In `~/Development/M2Radio/bt/test/mediapacketizer_test.cpp`, add a new block inside `main()` before the final `printf` (RING is 65 → 64 usable):

```cpp
    {   // 5. advanceRd: monotonic-forward commit that never clobbers a concurrent drop.
        //    cur = m_rd now (maybe advanced by an ISR drop since gather); rd0 = index
        //    drain gathered from; n = frames drain consumed.
        CHECK(MediaPacketizer::advanceRd(10, 10, 3) == 13);   // no drop: land on rd0+n
        CHECK(MediaPacketizer::advanceRd(12, 10, 3) == 13);   // ISR dropped 2 (<n): still rd0+n
        CHECK(MediaPacketizer::advanceRd(13, 10, 3) == 13);   // ISR dropped exactly n: equal, keep
        CHECK(MediaPacketizer::advanceRd(14, 10, 3) == 14);   // ISR dropped >n: keep ISR's further pos
        CHECK(MediaPacketizer::advanceRd(64, 63, 3) == 1);    // wrap: (63+3)%65 == 1
        CHECK(MediaPacketizer::advanceRd(1, 63, 3) == 1);     // ISR wrapped past rd0+n: keep cur
    }
```

- [ ] **Step 2: Run the test to verify it fails to compile**

Run: `cd ~/Development/M2Radio/bt/test && CXX=c++ ./run.sh`
Expected: FAIL — compile error, `advanceRd` is not a member of `MediaPacketizer`.

- [ ] **Step 3: Declare `advanceRd` and `framesPerPacket()` in the header**

In `~/Development/M2Radio/bt/MediaPacketizer.h`, in the `public:` section (after `queueHighWater()`, ~33), add:

```cpp
    // Monotonic-forward read-pointer advance used by drain()'s commit so a
    // concurrent push()-drop never clobbers m_rd.  Exposed for unit test.
    static uint8_t advanceRd(uint8_t cur, uint8_t rd0, uint8_t n);
    // Whole SBC frames batched per RTP/L2CAP packet (from the negotiated MTU).
    // Logged by bt_tone_test to verify batching (spec target: >= 5).
    uint16_t framesPerPacket() const { return m_perPkt; }
```

- [ ] **Step 4: Implement `advanceRd` and the IRQ-guarded commit**

In `~/Development/M2Radio/bt/MediaPacketizer.cpp`, after the `#include` lines, add the critical-section primitive (self-contained; no CMSIS header dependency, host no-op):

```cpp
// PRIMASK save/restore critical section: serialises drain()'s m_rd commit against
// push() running in the audio ISR.  ARM-only; a no-op on the host build so the
// unit tests stay pure.
#if defined(__arm__) || defined(__ARM_ARCH)
static inline uint32_t mp_irq_save() { uint32_t p; __asm volatile ("mrs %0, primask\n\tcpsid i" : "=r"(p) :: "memory"); return p; }
static inline void     mp_irq_restore(uint32_t p) { __asm volatile ("msr primask, %0" :: "r"(p) : "memory"); }
#else
static inline uint32_t mp_irq_save() { return 0; }
static inline void     mp_irq_restore(uint32_t) {}
#endif

// drain() wants m_rd = rd0 + n.  If push() (ISR) advanced m_rd past that while we
// were sending (it dropped >= n oldest frames), keep the ISR's further position;
// never move m_rd backward.  All arithmetic is mod RING, so it is wrap-safe.
uint8_t MediaPacketizer::advanceRd(uint8_t cur, uint8_t rd0, uint8_t n) {
    uint8_t moved = (uint8_t)(((int)cur - (int)rd0 + RING) % RING);
    if (moved >= n) return cur;
    return (uint8_t)(((int)rd0 + n) % RING);
}
```

Change `drain()` to capture `rd0` and commit through `advanceRd` under the critical section. Replace the current body:

```cpp
void MediaPacketizer::drain(SendFn send, void *ctx) {
    uint8_t pkt[PKT_MAX];
    while (m_wr != m_rd) {                   // frames available
        uint8_t rd0 = m_rd, rd = rd0, n = 0; uint16_t off = Rtp::HEADER_LEN;
        // gather up to m_perPkt whole frames that still fit the MTU
        while (n < m_perPkt && rd != m_wr && off + m_len[rd] <= m_mtu) {
            memcpy(pkt + off, m_buf[rd], m_len[rd]); off += m_len[rd];
            rd = (uint8_t)((rd + 1) % RING); n++;
        }
        if (n == 0) break;                   // defensive: a frame larger than the MTU (cannot happen at bitpool 53)
        Rtp::header(pkt, m_seq, m_ts, n);
        if (!send(ctx, pkt, off)) return;    // sink refused -> keep frames, try next drain()
        // Commit: advance m_rd forward to rd0+n without clobbering a concurrent
        // ISR drop (which may have already advanced m_rd).
        uint32_t s = mp_irq_save();
        m_rd = advanceRd(m_rd, rd0, n);
        mp_irq_restore(s);
        m_seq++; m_ts += (uint32_t)n * 128; m_packets++;
    }
}
```

- [ ] **Step 5: Run the full BT host test suite (includes the existing drop-oldest test #4)**

Run: `cd ~/Development/M2Radio/bt/test && CXX=c++ ./run.sh`
Expected: `mediapacketizer_test` reports its new higher check count with `0 failures`, the existing tests 1–4 still pass (drop-oldest behaviour unchanged when there is no concurrency), and the run ends `BT-HOST-TESTS: PASS`.

- [ ] **Step 6: Update the ring's header comment**

In `~/Development/M2Radio/bt/MediaPacketizer.cpp`, update the top-of-file comment block that says the `m_rd` race is unfixed ("FIX PATH before relying on the backpressure regime…") to state it is now closed via `advanceRd` under a PRIMASK critical section, so `drops==0` is sound.

- [ ] **Step 7: Commit**

```bash
cd ~/Development/M2Radio
git add bt/MediaPacketizer.h bt/MediaPacketizer.cpp bt/test/mediapacketizer_test.cpp
git commit -m "fix(bt): close MediaPacketizer m_rd two-writer race

drain()'s commit now advances m_rd monotonically-forward via advanceRd under a
PRIMASK critical section, so a concurrent push()-drop is never clobbered.
drops==0 (the phase-4 target) is now sound rather than merely quiet.

Co-Authored-By: Claude Opus 4.8 <noreply@anthropic.com>"
```

---

### Task 4: Example — `bt_tone_test` uses `attachRts()`; CMake option; heartbeat

**Files:**
- Modify: `~/Development/rt1170/evkb/examples/audio/bt_tone_test/bt_tone_test.cpp`
- Modify: `~/Development/rt1170/evkb/examples/audio/bt_tone_test/CMakeLists.txt`

Switch the media build from the static-GPIO CTS hack to real hardware RTS. `attachRts()` idles asserted (LOW = clear to send), identical to the old static assert, but deasserts transiently when the FIFO fills — the whole point. It is called after the wake pulse (module up, `CON[7]` already sampled) and after `hciIo.begin()` (so `RE` exists); the core flag carries it across the 3 Mbaud `rebaud()` automatically.

- [ ] **Step 1: Add the CMake option**

In `~/Development/rt1170/evkb/examples/audio/bt_tone_test/CMakeLists.txt`, after the `M2_BT_ASSERT_CTS` block (~96), add:

```cmake
# Hardware RXRTSE flow control on LPUART2 (preferred over the static M2_BT_ASSERT_CTS
# GPIO hack for A2DP media rates -- see docs/superpowers/specs/2026-09-03-bt3-rxrtse-*).
# Also holds/toggles the 1G ENET PHY reset (R1866); BT-only builds only.
option(M2_BT_RTS_FLOW "Enable hardware RXRTSE flow control on LPUART2 (Serial2.attachRts)" OFF)
set(M2_BT_RTS_WATER "1" CACHE STRING "MODIR RTSWATER watermark for M2_BT_RTS_FLOW (0..3)")
if(M2_BT_RTS_FLOW)
    add_definitions(-DM2_BT_RTS_FLOW=1 -DM2_BT_RTS_WATER=${M2_BT_RTS_WATER})
endif()
```

- [ ] **Step 2: Use `attachRts()` in place of the CTS hack**

In `~/Development/rt1170/evkb/examples/audio/bt_tone_test/bt_tone_test.cpp`, replace the `M2_BT_ASSERT_CTS` block in `setup()` (lines ~277-282) with:

```cpp
#if defined(M2_BT_RTS_FLOW)
    // Hardware RXRTSE: the receiver deasserts LPUART2_RTS_B as the RX FIFO nears
    // full, pausing the card before overrun (the fix for the phase-4 media stall).
    // Idles asserted (clear to send) like the old static assert; the core re-applies
    // it across the 3 Mbaud rebaud.  ★ Holds/toggles the ENET PHY reset (R1866).
    bool rts = Serial2.attachRts((uint8_t)M2_BT_RTS_WATER);
    CONSOLE.print("bt_flow=rxrtse rtswater="); CONSOLE.print((int)M2_BT_RTS_WATER);
    CONSOLE.print(" attached="); CONSOLE.println(rts ? 1 : 0);
#elif defined(M2_BT_ASSERT_CTS)
    m2AssertBtCts();
    CONSOLE.println("bt_cts=asserted_after_reset (PHY held in reset -- see m2_hci_probe.cpp)");
#else
    CONSOLE.println("bt_cts=undriven");
#endif
```

- [ ] **Step 3: Log `frames_per_pkt` in the streaming banner (batching verification)**

`MediaPacketizer::framesPerPacket()` was added in Task 3. Expose it through the sink node: in `~/Development/rt1170/evkb/examples/audio/bt_tone_test/AudioOutputBluetooth.h`, add to the public accessors (near `queueHighWater()`, ~28):

```cpp
    uint16_t framesPerPacket() const { return m_pk.framesPerPacket(); }
```

Then change the streaming line in `setup()` (~324) to report the negotiated per-packet frame count (spec acceptance: `>= 5`):

```cpp
    if (r2 == A2dpSource::OK) {
        btout.begin(src);
        CONSOLE.print("streaming frames_per_pkt="); CONSOLE.print(btout.framesPerPacket());
        CONSOLE.print(" media_mtu="); CONSOLE.println(src.mediaMtu());
    }
```

- [ ] **Step 4: Build the media build with flow control on**

Run:
```bash
cd ~/Development/rt1170/evkb/examples/audio/bt_tone_test
rm -rf build-media
cmake -B build-media -DCMAKE_TOOLCHAIN_FILE=../../../toolchain/rt1170-evkb.toolchain.cmake \
  -DM2_BT_UART_DNLD=ON -DM2_BT_WAKE_PULSE=ON -DM2_BT_RTS_FLOW=ON -DM2_BT_RTS_WATER=1 \
  -DM2_BT_FAST_BAUD=ON -DM2_BT_FAST_BAUD_RATE=3000000 -DM2_BT_LEGACY_PIN=ON \
  -DM2_BT_TARGET_NAME=EVKB-SINK >/dev/null
cmake --build build-media 2>&1 | tail -3
```
Expected: builds to `bt_tone_test.elf` with no errors (this is the first firmware to link the new `attachRts` — proves Task 2 compiles).

- [ ] **Step 5: Build the default (card-absent gate) build and run its QEMU gate**

The default `build/` must stay byte-identical in behaviour (no flow control on the vacuity path). Rebuild it and run the card-absent gate:

Run:
```bash
cd ~/Development/rt1170/evkb/examples/audio/bt_tone_test
rm -rf build && cmake -B build -DCMAKE_TOOLCHAIN_FILE=../../../toolchain/rt1170-evkb.toolchain.cmake >/dev/null && cmake --build build >/dev/null
./run_qemu.sh; echo "exit=$?"
```
Expected: the card-absent vacuity gate PASSES (RTS is never asserted with no card; no streaming lines). `exit=0`.

- [ ] **Step 6: Run the `[media]` QEMU gate**

Run: `cd ~/Development/rt1170/evkb/examples/audio/bt_tone_test && ./run_qemu_media.sh; echo "exit=$?"`
Expected: PASS (`exit=0`) — the fake peer sees framed RTP/SBC; `blocks>=1`, framing intact. QEMU cannot exercise RXRTSE, so this proves no regression, not the fix.

- [ ] **Step 7: Commit**

```bash
cd ~/Development/rt1170/evkb
git add examples/audio/bt_tone_test/bt_tone_test.cpp examples/audio/bt_tone_test/CMakeLists.txt examples/audio/bt_tone_test/AudioOutputBluetooth.h
git commit -m "feat(bt_tone_test): use Serial2.attachRts() hardware flow control

Replaces the static-GPIO CTS hack with opt-in M2_BT_RTS_FLOW (RXRTSE), logs
frames_per_pkt/media_mtu for batching verification. Default/card-absent build
unchanged.

Co-Authored-By: Claude Opus 4.8 <noreply@anthropic.com>"
```

---

### Task 5: Push libraries, bump pins, fresh-user verify, full sweep + audit

**Files:**
- Modify: `~/Development/rt1170/evkb/evkb.cmake` (bump `cores` and `M2Radio` pins)

- [ ] **Step 1: Push the two library repos**

```bash
cd ~/Development/teensy-cores && git push origin master && git rev-parse HEAD
cd ~/Development/M2Radio    && git push origin master && git rev-parse HEAD
```
Record both SHAs.

- [ ] **Step 2: Bump the pins in `evkb.cmake`**

In `~/Development/rt1170/evkb/evkb.cmake`, update the `cores` (teensy-cores) pin to the Task-5-Step-1 cores SHA and the `M2Radio` pin to the M2Radio SHA. (Find them by `grep -n "cores\|M2Radio" evkb.cmake` and replace the `GIT_TAG`/SHA literals.)

- [ ] **Step 3: Fresh-user verify (FORCE_FETCH) that the pins build the new symbols**

`m2_hci_probe` and `bt_tone_test` are the examples that link the new `attachRts`/MediaPacketizer changes. Verify a from-scratch fetch builds `bt_tone_test`'s media build:

Run:
```bash
cd ~/Development/rt1170/evkb/examples/audio/bt_tone_test
rm -rf build-ff
cmake -B build-ff -DCMAKE_TOOLCHAIN_FILE=../../../toolchain/rt1170-evkb.toolchain.cmake \
  -DEVKB_FORCE_FETCH=ON -DM2_BT_RTS_FLOW=ON -DM2_BT_FAST_BAUD=ON -DM2_BT_FAST_BAUD_RATE=3000000 \
  -DM2_BT_LEGACY_PIN=ON -DM2_BT_TARGET_NAME=EVKB-SINK 2>&1 | tail -5
cmake --build build-ff 2>&1 | tail -3
```
Expected: fetches the pinned `cores`+`M2Radio` from GitHub and builds `bt_tone_test.elf` — confirms the pins carry `attachRts` and the MediaPacketizer change. Then `rm -rf build-ff`.

- [ ] **Step 4: Run the host test suites**

Run:
```bash
~/Development/teensy-cores/imxrt1176/extras/lpuart_rts_test/run.sh
cd ~/Development/M2Radio/bt/test && CXX=c++ ./run.sh
cd ~/Development/M2Radio/hci/test && CXX=c++ ./run.sh
```
Expected: `LPUART-RTS-HOST-TEST: PASS`, `BT-HOST-TESTS: PASS`, and the hci suite passes.

- [ ] **Step 5: Full QEMU sweep**

Read `docs/KNOWN-BROKEN-GATES.md` first. Run the sweep through a short-path symlink (macOS `sun_path` 104-byte cap) as the CLAUDE.md notes require. This sub-project adds **no new QEMU gates** (host tests only), so the target count is unchanged from the last measurement (124 per the programme memory).

Run:
```bash
ln -sfn ~/Development/rt1170/evkb /tmp/rx && cd /tmp/rx
# build every gate-owning example first if needed (a gate does not build); then:
./tools/run-all-qemu-gates.sh -l   # confirm the gate count
./tools/run-all-qemu-gates.sh      # the sweep
```
Expected: the recorded count `passed, 0 failed, 0 SKIP` (the nondeterministic `dualcore/cm4_audio_test` may need a re-run; `display/acid_box` green on this machine). A SKIP means an example was not built — build it and re-run.

- [ ] **Step 6: Licence audit**

Run: `cd ~/Development/rt1170/evkb && LICENSE_AUDIT_EVKB=$(pwd) ./tools/license-audit.sh 2>&1 | tail -5`
Expected: `LICENSE-AUDIT: PASS`. (No new example dir → no GATES entry needed; the new files are MIT. If the drift check flags anything, fix the GATES list, do not weaken the check.)

- [ ] **Step 7: Commit the pin bump**

```bash
cd ~/Development/rt1170/evkb
git add evkb.cmake
git commit -m "build: bump cores + M2Radio pins for RXRTSE flow control

cores <sha>: Serial2.attachRts() hardware RXRTSE.
M2Radio <sha>: MediaPacketizer m_rd race close.
Fresh-user verified with -DEVKB_FORCE_FETCH=ON on bt_tone_test's media build.

Co-Authored-By: Claude Opus 4.8 <noreply@anthropic.com>"
```

---

### Task 6: Silicon acceptance (BENCH — interactive, not a subagent)

**Files:**
- Modify: `~/Development/rt1170/evkb/examples/audio/bt_tone_test/transcript_hw_evkb.txt` (capture)

This is the real proof. Driven interactively with the EVKB + IW416 card + ESP32 sink (`EVKB-SINK`). Follow the programme's bench hygiene: detach the VCOM reader before any LinkServer op; do not hold VCOM while programming; never `pkill -9` mid-flash-program; open the ESP32 serial with `dtr=False, rts=False`; read the `loop()` heartbeat (setup() output is missed across a reset); read `DHCSR` first if the image looks dead.

- [ ] **Step 1: Confirm the ESP32 sink is up and discoverable**

Power the ESP32 A2DP sink (`tools/esp32-a2dp-sink`), open its serial `dtr=False, rts=False`, confirm it advertises `EVKB-SINK`.

- [ ] **Step 2: Flash the media build (VCOM detached)**

```bash
pkill LinkServer; pkill redlinkserv; pkill crt_emu_cm_redlink
cd ~/Development/rt1170/evkb/examples/audio/bt_tone_test
LinkServer flash MIMXRT1176:MIMXRT1170-EVKB load build-media/bt_tone_test.elf
LinkServer flash MIMXRT1176:MIMXRT1170-EVKB verify build-media/bt_tone_test.elf
```
Then attach the reader and reset (background `LinkServer run` to trigger a reset), and capture the console with `tools/rt1170-console.py`.

- [ ] **Step 3: Verify bring-up and streaming**

In the console, confirm: `serial2=up_115200`, `bt_flow=rxrtse rtswater=1 attached=1`, `bt_baud_switch=ok rate=3000000`, `a2dp=OK`, `streaming frames_per_pkt=<N> media_mtu=<M>`, and the sink prints `state=started` and its packet counter climbing.

- [ ] **Step 4: Measure the acceptance criteria over ≥ 5 minutes**

Watch the `loop()` heartbeat (`hb streaming=1 blocks=<> packets=<> drops=<> ...`) for at least 5 minutes and confirm:
- **Audible** 1 kHz tone from the sink, continuous, no dropouts.
- `packets` climbs **monotonically** the whole time (not frozen at ~43).
- `drops=0` throughout.
- The card's NCP events keep returning credit (media never stalls); no `starved`/credit-stuck signature.
- `frames_per_pkt >= 5` (batching effective). If `< 5`, record the negotiated MTU and note whether a larger media MTU is worth negotiating (out of scope to change here unless it blocks acceptance).

- [ ] **Step 5: Tune `RTSWATER` if needed**

If overruns/drops appear at `rtswater=1`, rebuild `build-media` with `-DM2_BT_RTS_WATER=0` (deassert earlier / more headroom) or `=2`, reflash, and re-measure. Record the value that yields zero overruns and the evidence for it.

- [ ] **Step 6: Capture the transcript and commit**

Save the console capture (bring-up + a representative heartbeat window showing `drops=0`, monotonic `packets`, the chosen `rtswater`) to `examples/audio/bt_tone_test/transcript_hw_evkb.txt`.

```bash
cd ~/Development/rt1170/evkb
git add examples/audio/bt_tone_test/transcript_hw_evkb.txt
git commit -m "test(bt_tone_test): silicon acceptance -- audible A2DP over RXRTSE flow control

RXRTSE (rtswater=<n>) on LPUART2: NCP events survive, ACL credit never sticks,
media streams >5 min with drops=0, tone audible on the ESP32 sink. Phase-4
audible close.

Co-Authored-By: Claude Opus 4.8 <noreply@anthropic.com>"
```

---

### Task 7: Docs + memory + NEW-9 close-out

**Files:**
- Modify: `~/Development/rt1170/evkb/CLAUDE.md`
- Modify: `~/Development/rt1170/evkb/docs/KNOWN-BROKEN-GATES.md` (only if any standing-red note needs it — likely none)
- Update: programme memory + NEW-9 (via memory tooling / Linear)

- [ ] **Step 1: Update `CLAUDE.md`**

Add a note (near the BT gate discussion) recording: RXRTSE flow control is **silicon-only, QEMU cannot gate it** (qemu2's LPUART model stores `MODIR` but ignores it and its `can_receive` already backpressures the peer); the automated proof is the `imxrt1176/extras/lpuart_rts_test` host test + the `mediapacketizer_test` race checks; silicon is the real proof (see `transcript_hw_evkb.txt`). Note that `Serial2.attachRts()` holds/toggles the ENET PHY reset (R1866) so it is BT-only. Confirm the sweep count line is still accurate (unchanged — no new QEMU gates).

- [ ] **Step 2: Update the programme memory**

Update `~/.claude/projects/-Users-nicholasnewdigate-Development-rt1170-evkb/memory/m2-bluetooth-a2dp-programme.md` (and the `MEMORY.md` index hook) with the phase-4 audible-close result: RXRTSE fix landed, `attachRts()` on the core, MediaPacketizer race closed, silicon result (audible, drops=0, chosen rtswater), pins/SHAs.

- [ ] **Step 3: Update NEW-9**

Post the phase-4 audible-close comment to NEW-9 (Linear) with the silicon evidence and push confirmations; set status as appropriate for the programme.

- [ ] **Step 4: Commit the doc changes**

```bash
cd ~/Development/rt1170/evkb
git add CLAUDE.md docs/KNOWN-BROKEN-GATES.md
git commit -m "docs: BT-3 phase-4 RXRTSE flow control landed (silicon audible close)

Co-Authored-By: Claude Opus 4.8 <noreply@anthropic.com>"
```

- [ ] **Step 5: Final review**

Dispatch a final code review over the whole change set (cores + M2Radio + evkb), then use `superpowers:finishing-a-development-branch` if a branch merge is pending. (BT-3 has been committing to `master` directly per programme convention; confirm that is still intended before any merge step.)
