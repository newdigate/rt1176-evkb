# UAC2 audio out — P3: close the rate loop at high speed — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Read the UAC2 witness's 4-byte Q16.16 feedback endpoint over iTD IN and let the existing EMA/slew servo drive high-speed packet sizing, turning P1's by-design ~9.6 s overflow clicks into a flat closed-loop fill.

**Architecture:** Mirror the proven UAC1/FS feedback loop at high speed (approach A of the 2026-08-03 brainstorm): two dedicated feedback iTDs, one transaction each, in periodic slots 16 frames apart (16 ms cadence — the spec's stated ceiling, and a 16× subsample of the witness's 1 ms bInterval that the EMA's ~128 ms horizon fully absorbs). The decode is a new pure function; the ±2% gate, 1/8 EMA, ~90 ppm/s slew, 250-frame staleness, counters, and heartbeat fields are all reused verbatim. The driver plumbing lands at two seams P1 explicitly marked "(P3)".

**Tech Stack:** C++ (USBHost_t36 fork), host-run C test binaries (`test/Makefile`), ARM GCC 10 firmware build via `evkb.cmake`, LinkServer flash, XMOS xscope VCD via `tools/vcdfill.py`.

**Spec:** `docs/superpowers/specs/2026-08-02-uac2-audio-out-design.md` §3 P3 — gate: closed-loop fill flat (|drift| ≤ ~1 ppm equivalent, zero corrections, ≥ 10 min; we run ≥ 25 for parity with the UAC1 evidence) and decoded rate consistent with the device's known ~−86 ppm crystal. UAC1 hardware regression is deferred to P4 (user decision, 2026-08-03).

---

## Context for a zero-context engineer

**What already exists (verified in source 2026-08-03):**

- `~/Development/USBHost_t36/` is the driver repo. P1 landed the iTD OUT ring
  (32 descriptors × 8 µframe transactions), the full UAC2 parser, clock-first
  control, and the µframe sizing/packing. The stream runs **open-loop**: the
  device drifts at its measured ~+84.6 ppm (host-relative) and block-corrects
  every ~9.6 s — P1's gate recorded 16 overflows in 184 s. P3 exists to make
  that number 0.
- The **feedback state machine is transport-agnostic and finished**:
  `fb_avg_mhz` (1/8 EMA — raw reports dither; chasing raw measured +4.8 ppm
  bias), `fb_sizing_mhz` (slewed at `FB_SLEW_MHZ_PER_FRAME` = 4 mHz/frame ≈
  90 ppm/s), `uac1_feedback_plausible()` (±2% gate), `FB_FRESH_FRAMES` = 250
  staleness fallback, counters `fb_packets/fb_rejects/fb_errors`. The UAC1
  closed-loop soak: +0.1 ppm over 25 min, zero corrections.
- The **HS service path already takes a slew step per frame** — toward
  `effectiveRateMilliHz()` only, at the comment "No feedback target in this
  phase" (`usb_audio.cpp` ~line 524). `beginStreamingHS()` has the matching
  "needs an iTD reader (P3)" comment (~line 416).
- `itd_get_txn_status()` already surfaces `.length` as **bytes received for
  IN** (EHCI 1.0 Table 3-3 write-back). `itd_fill_out()` exists;
  `itd_fill_in()` does not. iTD pool is 40; the ring uses 32, so 2 feedback
  iTDs fit with headroom.
- The FS harvest loop (`usb_audio.cpp` ~lines 547–581) is the line-for-line
  template for the HS one: collect feedback **before** re-arming audio frames,
  error → `fb_errors`, length-check → decode → gate → EMA, else `fb_rejects`,
  re-fill in place (descriptors stay linked for the stream's lifetime).
- `disconnect()` deliberately does **not** clear feedback state (self-heal
  contract). The Task 4 review disproved this plan's original "fb_itd
  inherits this for free" claim: the framework nulls `device` after
  disconnect and clears `is_uac2`, so the HS branch went dead during
  absence and the FS path survived its null `device->address` dereference
  only because address 0 is ITCM on this chip. The landed contract is a
  review fix: service() pauses while `device` is null (state frozen, not
  aged; resumes on re-claim), and disconnect()'s comment now documents
  that honestly. Two pre-existing gaps were explicitly deferred to
  follow-up tasks: the different-class/alt re-attach never re-arms
  (is_streaming wedge), and partial fb-arm failure + fill ACTIVE-first
  write ordering (hardening).
- The example `examples/usb/usb_audio_graph_test/` already prints every P3
  observable in its heartbeat (`fb= sizing= fbpkts= fbrej= fberr= fresh=`) and
  its source comment even predicts "back here as fb=44096218-ish". **No
  example code changes are needed** — only bench evidence and transcript.

**The witness (already flashed, do not touch):** XMOS xcore-200 MC, sw_usb_audio
`2AMi8o8xxxxxx` (UAC2, HS, 8ch/24-bit-in-4) with lib_xua decoupler xscope
probes. Its feedback EP, from the live-captured fixture
`test/fixtures/xmos_uac2_2ami8o8.bin` at offset 0xF2, bytes
`07 05 82 11 04 00 04`: address **0x82**, attributes 0x11 (iso, explicit
feedback usage), **wMaxPacketSize 4**, **bInterval 4** → 2^(4−1) = 8 µframes =
one fresh report per 1 ms. Both alts carry the same EP.

**The number the loop must find:** the device's converter measured three
independent ways — UAC1 locked-bias sweep intercept **−85.7 ppm**, UAC1
feedback EMA **−83..−86 ppm**, UAC2 open-loop fill drift **+84.6 ppm**
(host-relative; same crystal seen from the other side). The decoded HS
feedback must read ≈ **44 096.2 Hz** before the servo is allowed to steer —
that cross-check is the observe-first gate (Task 5).

**Q16.16 arithmetic identity worth knowing:** 44.1 kHz = 5.5125 samples/µframe
= 361267.2 in 16.16; a device rounds to 361267, which decodes to exactly the
same truncated millihertz (44 099 975) as the UAC1 10.14 vector 722534 —
because 722534 = 2 × 361267 and both measure the same rate.

**File map:**

| File | Role in P3 |
|---|---|
| `USBHost_t36/usb_audio_feedback.{h,cpp}` | add `uac2_feedback_to_mhz()` (Task 1) |
| `USBHost_t36/usb_audio_parse.h` | add `feedback_max_packet` field (Task 2) |
| `USBHost_t36/usb_audio2_parse.cpp` | capture that field (Task 2) |
| `USBHost_t36/ehci_iso.{h,cpp}` | add `ITD_BUFPTR1_DIR_IN`, `itd_fill_in()` (Task 3) |
| `USBHost_t36/usb_audio.{h,cpp}` | arm/harvest/servo plumbing (Task 4) |
| `USBHost_t36/test/{test_feedback,test_uac2_parse,test_itd}.cpp` | TDD (Tasks 1–3) |
| `evkb/examples/usb/usb_audio_graph_test/transcript_hw_evkb.txt` | gate evidence (Task 6) |

All test files share the same harness: `static int failures, checks;` +
`CHECK_EQ(a, b)` printing `FAIL file:line` and a final summary; `make -C test
run` builds and runs all binaries and fails on any FAIL.

## Bench safety rules (CRITICAL — violations have kernel-panicked this Mac)

1. **Never let any VCOM reader (`tools/rt1170-console.py`, `cat`, anything)
   hold the MCU-Link serial port during ANY LinkServer operation.** A held
   port during flash/reset re-enumeration triggers an IOSerialFamily
   use-after-free kernel panic (three occurrences on this bench). Before every
   LinkServer command: `pkill -9 -f rt1170-console.py` and wait 2 s.
2. **Purge probe daemons between every LinkServer operation:** `pkill -9 -f
   LinkServer; pkill -9 redlinkserv; pkill -9 crt_emu_cm_redlink`. A resident
   daemon makes the next `LinkServer run` exit silently after 3 INFO lines.
3. `LinkServer flash ... load` is synchronous and reliable. The standalone
   reset is `LinkServer probe <serial> wiretimedreset 200` — reliable on a
   halted (just-flashed) board. Working order: **flash → reset → then attach
   the console reader**.
4. Never `kill` a live `xrun` xscope collector mid-capture — it wedges the
   device's xscope until a reflash. Let captures end on their own (Ctrl-C the
   foreground process is fine; `kill -9` is not).
5. The MC200 stays on its current UAC2 witness firmware for all of P3. No
   `xflash` operations appear anywhere in this plan.

Bench tasks (5 and 6) are controller-executed (they need the physical bench);
code tasks (1–4) are subagent-dispatchable.

---

### Task 1: `uac2_feedback_to_mhz()` — Q16.16/µframe → millihertz

**Files:**
- Modify: `~/Development/USBHost_t36/usb_audio_feedback.h`
- Modify: `~/Development/USBHost_t36/usb_audio_feedback.cpp`
- Test: `~/Development/USBHost_t36/test/test_feedback.cpp`

The one genuinely new hazard vs the UAC1 decoder: a 32-bit 16.16 report can
encode rates whose millihertz **overflow uint32** (the 3-byte 10.14 format
could not). A garbage report must saturate into the plausibility gate's
rejection band, never wrap back into it.

- [ ] **Step 1: Write the failing tests**

In `test/test_feedback.cpp`, add below the existing `decode3()` helper:

```c
static uint32_t decode4(uint32_t v)
{
	uint8_t fb[4] = { (uint8_t)(v & 0xFF), (uint8_t)((v >> 8) & 0xFF),
	                  (uint8_t)((v >> 16) & 0xFF), (uint8_t)((v >> 24) & 0xFF) };
	return uac2_feedback_to_mhz(fb);
}
```

and a new test function (call it from `main()` beside the existing calls):

```c
static void test_decode_hs(void)
{
	// 44.1 kHz is 5.5125 samples/microframe = 361267.2 in 16.16; a device
	// rounds to 361267, which decodes to the same truncated millihertz as
	// the UAC1 vector 722534 (= 2 * 361267): both measure 44.1 kHz.
	CHECK_EQ(decode4(361267), 44099975);

	// 48 kHz exactly: 6.0 * 65536 = 393216 -> precisely 48000000 mHz.
	CHECK_EQ(decode4(393216), 48000000);

	// One 16.16 LSB above 44.1k: resolution is 8e6/65536 = 122 mHz.
	CHECK_EQ(decode4(361268), 44100097);

	// The witness's crystal, measured three independent ways at -83..-86
	// ppm: a device 86 ppm slow of 44.1k reports about 361236.1 ->
	// 361236 -> 44096191 mHz = -86.4 ppm. The hardware gate's Step A
	// expects the live EMA within a few ppm of this value.
	CHECK_EQ(decode4(361236), 44096191);

	// Null input and the all-zero payload devices send before their
	// measurement engine runs: decode to 0, which validity rejects.
	CHECK_EQ(uac2_feedback_to_mhz(0), 0);
	CHECK_EQ(decode4(0), 0);

	// Saturation: unlike 10.14-in-3, a 32-bit 16.16 report can encode
	// rates whose millihertz exceed uint32. Exact boundary: 35184372
	// still fits (4294967285); one LSB more overflows and must clamp to
	// UINT32_MAX -- deep in the plausibility gate's rejection band --
	// not wrap around into a plausible value.
	CHECK_EQ(decode4(35184372), 4294967285u);
	CHECK_EQ(decode4(35184373), 4294967295u);
	CHECK_EQ(decode4(0xFFFFFFFFu), 4294967295u);
	CHECK_EQ(uac1_feedback_plausible(4294967295u, 44100000), false);
}
```

- [ ] **Step 2: Run to verify it fails to build**

Run: `make -C ~/Development/USBHost_t36/test test_feedback && ~/Development/USBHost_t36/test/test_feedback`
Expected: compile error, `uac2_feedback_to_mhz` undeclared.

- [ ] **Step 3: Implement**

In `usb_audio_feedback.h`, after the `uac1_feedback_to_mhz` declaration:

```c
// Decode a high-speed UAC2 feedback payload: 4 bytes little-endian holding
// the device's measured rate in samples per MICROFRAME as Q16.16 fixed
// point (USB 2.0 section 5.12.4.2 at high speed). Returns millihertz,
// truncated; resolution one 16.16 LSB (122 mHz at the 8 kHz microframe
// rate). Returns 0 for a null pointer or all-zero payload, and saturates to
// UINT32_MAX when the encoded rate exceeds the millihertz range -- unlike
// the 3-byte 10.14 format, a 32-bit report can overflow uint32 millihertz,
// and garbage must saturate into the plausibility gate's rejection band
// rather than wrap into it. Callers gate on uac1_feedback_plausible()
// before applying.
uint32_t uac2_feedback_to_mhz(const uint8_t fb[4]);
```

In `usb_audio_feedback.cpp`:

```c
uint32_t uac2_feedback_to_mhz(const uint8_t fb[4])
{
	if (!fb) return 0;
	uint32_t v = (uint32_t)fb[0] | ((uint32_t)fb[1] << 8)
	           | ((uint32_t)fb[2] << 16) | ((uint32_t)fb[3] << 24);
	// samples/microframe * 8000 microframes/s * 1000 mHz/Hz, then drop
	// the 16 fractional bits. 64-bit: 0xFFFFFFFF * 8e6 needs 55 bits.
	uint64_t mhz = ((uint64_t)v * 8000000u) >> 16;
	if (mhz > 0xFFFFFFFFu) return 0xFFFFFFFFu;
	return (uint32_t)mhz;
}
```

- [ ] **Step 4: Run to verify it passes**

Run: `make -C ~/Development/USBHost_t36/test test_feedback && ~/Development/USBHost_t36/test/test_feedback`
Expected: all checks pass, 0 failures (was 27 checks; now ~37).

- [ ] **Step 5: Commit**

```bash
cd ~/Development/USBHost_t36 && git add usb_audio_feedback.h usb_audio_feedback.cpp test/test_feedback.cpp && git commit -m "usb_audio_feedback: add uac2_feedback_to_mhz -- Q16.16/uframe with saturation"
```

---

### Task 2: parser captures the feedback endpoint's wMaxPacketSize

**Files:**
- Modify: `~/Development/USBHost_t36/usb_audio_parse.h` (struct)
- Modify: `~/Development/USBHost_t36/usb_audio2_parse.cpp`
- Test: `~/Development/USBHost_t36/test/test_uac2_parse.cpp`

`itd_fill_in()` must program the endpoint's MPS into bufptr[1]; the topology
struct records the feedback EP's address but not its MPS. Additive field —
both parsers zero the struct, so UAC1 paths see 0 and are unaffected.

- [ ] **Step 1: Write the failing tests**

In `test/test_uac2_parse.cpp`, the fixture asserts
`t.alts[1].feedback_endpoint == 0x82` (~line 147) and the same for `alts[2]`
(~line 162). Beside each, add:

```c
	CHECK_EQ(t.alts[1].feedback_max_packet, 4);
```

```c
	CHECK_EQ(t.alts[2].feedback_max_packet, 4);
```

- [ ] **Step 2: Run to verify it fails to build**

Run: `make -C ~/Development/USBHost_t36/test test_uac2_parse && ~/Development/USBHost_t36/test/test_uac2_parse`
Expected: compile error, no member `feedback_max_packet`.

- [ ] **Step 3: Implement**

In `usb_audio_parse.h`, `struct UAC1AltSetting`, directly after the
`feedback_refresh` member:

```c
	// wMaxPacketSize of the feedback endpoint -- the HS iTD reader must
	// program the endpoint's MPS into bufptr[1]. Captured by the UAC2
	// parser; 0 when no feedback endpoint exists or on the UAC1/FS path,
	// whose siTD reader has no MPS field to program.
	uint16_t feedback_max_packet;
```

In `usb_audio2_parse.cpp` (~line 159), replace:

```c
			} else if (!is_out && is_iso) {
				if (alt->feedback_endpoint == 0) alt->feedback_endpoint = b[2];
			}
```

with:

```c
			} else if (!is_out && is_iso) {
				// First IN iso endpoint on the interface wins, and its
				// wMaxPacketSize rides along for the iTD reader.
				if (alt->feedback_endpoint == 0) {
					alt->feedback_endpoint = b[2];
					alt->feedback_max_packet =
					    (uint16_t)b[4] | ((uint16_t)b[5] << 8);
				}
			}
```

- [ ] **Step 4: Run to verify it passes**

Run: `make -C ~/Development/USBHost_t36/test run`
Expected: every binary passes (this also proves the struct change broke no
UAC1 test).

- [ ] **Step 5: Commit**

```bash
cd ~/Development/USBHost_t36 && git add usb_audio_parse.h usb_audio2_parse.cpp test/test_uac2_parse.cpp && git commit -m "usb_audio2_parse: capture the feedback endpoint's wMaxPacketSize"
```

---

### Task 3: `itd_fill_in()` — single-transaction HS iso IN

**Files:**
- Modify: `~/Development/USBHost_t36/ehci_iso.h`
- Modify: `~/Development/USBHost_t36/ehci_iso.cpp`
- Test: `~/Development/USBHost_t36/test/test_itd.cpp`

The whole difference from `itd_fill_out()` is bufptr[1] bit 11 (Direction —
the bit whose correct home the P1 plan review already established) plus a
single transaction in µframe 0 carrying the expected length. Guards all run
before the first write, so untouched-on-rejection holds by construction — and
a snapshot test proves it anyway, per house style.

- [ ] **Step 1: Write the failing tests**

In `test/test_itd.cpp`, add two functions (call both from `main()`):

```c
static void test_fill_in(void)
{
	static uint8_t buf[8] __attribute__ ((aligned(4)));
	itd_t n;
	memset(&n, 0, sizeof(n));

	CHECK_EQ(itd_fill_in(&n, 9, 2, buf, 8, 4, false), true);

	// Exactly one transaction armed, in microframe 0: active, expected
	// length 8 (the buffer room; the device sends at most max_packet and
	// the controller writes the received count back), PG 0, offset =
	// buffer's low 12 bits, no IOC.
	uint32_t off = (uint32_t)(uintptr_t)buf & 0xFFFu;
	CHECK_EQ(n.transaction[0],
	         ITD_TXN_ACTIVE | (8u << ITD_TXN_LENGTH_SHIFT) | off);
	for (int k = 1; k < 8; k++) CHECK_EQ(n.transaction[k], 0u);

	// Page chain: page i = page 0 + i*4K; ep/dev in bufptr[0], DIRECTION
	// + max packet in bufptr[1], Multi=1 in bufptr[2].
	uint32_t page0 = (uint32_t)(uintptr_t)buf & 0xFFFFF000u;
	CHECK_EQ(n.bufptr[0], page0 | (2u << 8) | 9u);
	CHECK_EQ(n.bufptr[1], (page0 + 4096u) | ITD_BUFPTR1_DIR_IN | 4u);
	CHECK_EQ(n.bufptr[2], (page0 + 8192u) | 1u);
	for (int i = 3; i < 7; i++)
		CHECK_EQ(n.bufptr[i], page0 + (uint32_t)i * 4096u);

	// The direction bit is the whole difference between IN and OUT fills:
	// an OUT fill of the same node must leave bit 11 clear.
	uint16_t lens[8] = { 4, 0, 0, 0, 0, 0, 0, 0 };
	CHECK_EQ(itd_fill_out(&n, 9, 2, buf, lens, 4, false), true);
	CHECK_EQ(n.bufptr[1] & ITD_BUFPTR1_DIR_IN, 0u);

	// IOC lands on the single transaction when asked.
	CHECK_EQ(itd_fill_in(&n, 9, 2, buf, 8, 4, true), true);
	CHECK_EQ(n.transaction[0] & ITD_TXN_IOC, ITD_TXN_IOC);

	// Status read-back surfaces a completed IN's written-back length: the
	// controller clears ACTIVE and replaces the length field with the
	// received count (4 bytes of feedback inside an 8-byte expectation).
	n.transaction[0] = (4u << ITD_TXN_LENGTH_SHIFT);
	itd_txn_status_t st;
	itd_get_txn_status(&n, 0, &st);
	CHECK_EQ(st.active, false);
	CHECK_EQ(st.length, 4);
	CHECK_EQ(st.err_xact || st.err_babble || st.err_buffer, false);
}

static void test_fill_in_rejects(void)
{
	static uint8_t buf[8];
	itd_t n, before;

	// Every rejection leaves the descriptor untouched: fill with a
	// recognisable pattern and compare the whole struct afterwards.
	memset(&n, 0xA5, sizeof(n));
	memcpy(&before, &n, sizeof(n));

	CHECK_EQ(itd_fill_in(0, 9, 2, buf, 8, 4, false), false);
	CHECK_EQ(itd_fill_in(&n, 9, 2, 0, 8, 4, false), false);
	CHECK_EQ(itd_fill_in(&n, 9, 2, buf, 0, 4, false), false);     // len 0
	CHECK_EQ(itd_fill_in(&n, 9, 2, buf, 3073, 4, false), false);  // > uframe max
	CHECK_EQ(itd_fill_in(&n, 9, 2, buf, 8, 0, false), false);     // mps 0
	CHECK_EQ(itd_fill_in(&n, 9, 2, buf, 8, 1025, false), false);  // mps > 1024
	CHECK_EQ(itd_fill_in(&n, 9, 16, buf, 8, 4, false), false);    // endpoint width
	CHECK_EQ(itd_fill_in(&n, 128, 2, buf, 8, 4, false), false);   // address width
	CHECK_EQ(memcmp(&n, &before, sizeof(n)), 0);

	// Boundary acceptances, so the guards reject only what they claim:
	memset(&n, 0, sizeof(n));
	CHECK_EQ(itd_fill_in(&n, 127, 15, buf, 8, 1024, false), true);
	CHECK_EQ(itd_fill_in(&n, 9, 2, buf, 3072, 1024, false), true);
	CHECK_EQ(itd_fill_in(&n, 9, 2, buf, 1, 1, false), true);
}
```

- [ ] **Step 2: Run to verify it fails to build**

Run: `make -C ~/Development/USBHost_t36/test test_itd && ~/Development/USBHost_t36/test/test_itd`
Expected: compile error, `itd_fill_in` / `ITD_BUFPTR1_DIR_IN` undeclared.

- [ ] **Step 3: Implement**

In `ehci_iso.h`, beside the other `ITD_TXN_*` defines:

```c
#define ITD_BUFPTR1_DIR_IN   0x00000800u  // bufptr[1] bit 11: 1 = IN
```

and after the `itd_fill_out` declaration:

```c
// Fill an iTD for one high-speed isochronous IN transaction in microframe 0:
// the feedback-endpoint read (4 bytes of Q16.16 every bInterval, polled at
// the 16 ms slot cadence like the FS reader). `len` is the room available in
// `buf` (DMA-reachable); the device sends at most max_packet per transaction
// and the controller writes the received count back into the transaction
// length field (EHCI 1.0 Table 3-3 IN write-back), surfaced by
// itd_get_txn_status() as .length. Direction is bufptr[1] bit 11, set here
// and only here -- itd_fill_out leaves it clear. Returns false (descriptor
// untouched) on nulls, len 0 or > 3072, max_packet 0 or > 1024,
// endpoint > 15, or device address > 127.
bool itd_fill_in(itd_t *node, uint8_t dev_addr, uint8_t endpoint,
                 void *buf, uint16_t len, uint16_t max_packet, bool ioc);
```

In `ehci_iso.cpp`, after `itd_fill_out()`:

```c
bool itd_fill_in(itd_t *node, uint8_t dev_addr, uint8_t endpoint,
                 void *buf, uint16_t len, uint16_t max_packet, bool ioc)
{
	if (!node || !buf) return false;
	// 3072 is the EHCI ceiling for one microframe (Multi=3 of 1024); the
	// 12-bit length field could encode more, and a larger value would be
	// a lie the controller might act on.
	if (len == 0 || len > 3072) return false;
	if (max_packet == 0 || max_packet > 1024) return false;
	if (endpoint > 15 || dev_addr > 127) return false;

	uint32_t base = (uint32_t)(uintptr_t)buf;
	uint32_t page0 = base & 0xFFFFF000u;
	uint32_t off = base & 0xFFFu;

	// One transaction, microframe 0. PG is 0 by construction (off < 4K);
	// a read crossing into the next page rolls to bufptr[1] in hardware.
	node->transaction[0] = ITD_TXN_ACTIVE
	                     | ((uint32_t)len << ITD_TXN_LENGTH_SHIFT)
	                     | (off & ITD_TXN_OFFSET_MASK)
	                     | (ioc ? ITD_TXN_IOC : 0u);
	for (int k = 1; k < 8; k++) node->transaction[k] = 0;

	// Contiguous payload means consecutive physical pages, same as the
	// OUT fill. Low bits per EHCI Table 3-6.
	for (int i = 0; i < 7; i++) node->bufptr[i] = page0 + (uint32_t)i * 4096u;
	node->bufptr[0] |= ((uint32_t)endpoint << 8) | dev_addr;
	node->bufptr[1] |= ITD_BUFPTR1_DIR_IN | max_packet;
	node->bufptr[2] |= 1u;              // Multi = 1
	return true;
}
```

- [ ] **Step 4: Run to verify it passes**

Run: `make -C ~/Development/USBHost_t36/test test_itd && ~/Development/USBHost_t36/test/test_itd`
Expected: all checks pass, 0 failures.

- [ ] **Step 5: Commit**

```bash
cd ~/Development/USBHost_t36 && git add ehci_iso.h ehci_iso.cpp test/test_itd.cpp && git commit -m "ehci_iso: add itd_fill_in -- single-transaction HS iso IN for the feedback read"
```

---

### Task 4: driver plumbing — arm, harvest, servo target

**Files:**
- Modify: `~/Development/USBHost_t36/usb_audio.h`
- Modify: `~/Development/USBHost_t36/usb_audio.cpp`

No host test covers this plumbing (it needs `USBHost_t36.h`); verification is
the full suite staying green, the firmware example building in both
configurations, and review. Every edit mirrors an existing FS line that the
UAC1 soak proved.

- [ ] **Step 1: Header — new members and comment updates**

In `usb_audio.h`, after the `sitd_t *fb_sitd[FB_SLOTS] = {};` line:

```c
	itd_t   *fb_itd[FB_SLOTS] = {};   // HS reader: same slots, iTD transport
	uint16_t fb_mps_hs = 0;           // feedback EP wMaxPacketSize (HS arm/refill)
```

Replace the feedback-pipe comment block above `FB_SLOTS`:

```c
	// Feedback pipe state. Two descriptors 16 frames apart give a 16 ms
	// cadence on a 32-slot periodic list where each slot recurs every
	// 32 ms: bRefresh=4's exact rate at FS, and a 16x subsample of the
	// witness's 1 ms bInterval at HS -- the EMA's ~128 ms horizon needs
	// no faster feed (UAC1 soak: +0.1 ppm at this cadence). 8 bytes of
	// room per read: the FS report is 3 bytes, the HS report 4, and
	// anything else that arrives is counted as a reject rather than a
	// buffer error.
```

In the public "--- feedback endpoint ---" comment (above `followFeedback()`),
after the sentence ending "(UAC1 3.7.2.2)." insert:

```c
	// At high speed (UAC2) the same idea is 4 bytes of Q16.16 samples per
	// MICROFRAME on the alt's iso IN endpoint (USB 2.0 5.12.4.2), read by
	// an iTD at the same 16 ms slot cadence; the servo below is shared.
```

- [ ] **Step 2: `beginStreamingHS()` — reset comment, then arm after the ring**

Replace the "Leave feedback unarmed (P3)" comment block (keeping the state
resets and their order — the seed-before-arming rule is load-bearing, it broke
the first P1 gate):

```c
	// Reset the rate brain before arming. This block must run BEFORE the
	// arming loop: fillFrameHS() sizes its microframes from fb_sizing_mhz,
	// and an unseeded (zero) rate makes every length zero, which
	// itd_fill_out correctly refuses -- the first hardware gate failed
	// exactly there.
	fb_endpoint = 0;
	fb_rate_mhz = 0;
	fb_avg_mhz = 0;
	fb_frames_since = 0xFFFFFF;
	fb_packets = fb_rejects = fb_errors = 0;
	fb_sizing_mhz = effectiveRateMilliHz();
	fb_mps_hs = 0;
```

Then AFTER the ring arming loop (mirroring the FS function's ordering),
before `packets_sent = 0;`:

```c
	// Arm the feedback reader if this alternate setting advertises one --
	// same contract as the FS arm: failure to arm is not failure to
	// stream, the loop just stays open at nominal + trim. The HS report
	// is 4 bytes of Q16.16 samples-per-microframe on an iso IN endpoint;
	// one iTD transaction in microframe 0 of each polled slot reads it.
	// The witness refreshes every 1 ms (bInterval 4); the 16 ms slot
	// cadence subsamples that, and the EMA's ~128 ms horizon needs no
	// more. An MPS of 0 (malformed descriptor) or beyond the read buffer
	// leaves the loop open rather than arming a read that cannot land.
	if (alt->feedback_endpoint && alt->feedback_max_packet &&
	    alt->feedback_max_packet <= sizeof(fb_buf[0])) {
		fb_endpoint = alt->feedback_endpoint & 0x0F;
		fb_mps_hs = alt->feedback_max_packet;
		for (uint32_t k = 0; k < FB_SLOTS; k++) {
			fb_itd[k] = itd_alloc();
			if (!fb_itd[k] ||
			    !itd_fill_in(fb_itd[k], device->address, fb_endpoint,
			                 fb_buf[k], sizeof(fb_buf[k]), fb_mps_hs,
			                 false)) {
				if (fb_itd[k]) { itd_free(fb_itd[k]); fb_itd[k] = 0; }
				fb_endpoint = 0;
				break;
			}
			// Slots 16 frames apart, like the FS reader.
			uint16_t frame = (uint16_t)(k * 16);
			itd_link(periodic_frame_slot(frame), fb_itd[k], frame);
		}
	}
```

(The failure path deliberately mirrors the FS arm's, including its
leave-earlier-slot-linked shape on mid-loop pool exhaustion — unreachable with
pool 40 vs 34 used, and `stopStreaming()` reclaims it regardless; symmetry
with the proven FS code beats divergence here.)

- [ ] **Step 3: `service()` HS branch — harvest before the ring, real target**

Trim the `is_uac2` branch's opening comment (the "FS feedback-siTD block does
not apply here" sentence is now false):

```c
		// Harvest whatever the controller has finished, then refill and
		// re-link -- same shape as the FS loop below, but a whole iTD (up
		// to eight microframe transactions) is one unit of harvest/refill
		// instead of one siTD per packet.
```

Directly after it, BEFORE the ring loop, insert the feedback harvest:

```c
		// Collect feedback reports before re-arming audio frames, so a
		// report that just landed steers the very next microframe sizing
		// below -- same ordering contract as the FS loop.
		for (uint32_t k = 0; k < FB_SLOTS; k++) {
			itd_t *f = fb_itd[k];
			if (!f) continue;

			itd_txn_status_t fst;
			itd_get_txn_status(f, 0, &fst);
			if (fst.active) continue;

			if (fst.err_xact || fst.err_babble || fst.err_buffer) {
				fb_errors++;
			} else {
				// The HS report is exactly 4 bytes of Q16.16; the
				// controller wrote the received count back into the
				// length field. Anything else -- including a zero-
				// length response from a not-yet-armed endpoint --
				// is counted and skipped, never applied.
				uint32_t mhz = (fst.length == 4)
				             ? uac2_feedback_to_mhz(fb_buf[k]) : 0;
				if (mhz && uac1_feedback_plausible(mhz,
				                                   effectiveRateMilliHz())) {
					fb_rate_mhz = mhz;
					// Average before use: same dither rationale
					// as FS (raw-chasing measured +4.8 ppm on
					// this bench).
					fb_avg_mhz = uac1_fb_average(fb_avg_mhz, mhz);
					fb_frames_since = 0;
					fb_packets++;
				} else {
					fb_rejects++;
				}
			}
			itd_fill_in(f, device->address, fb_endpoint,
			            fb_buf[k], sizeof(fb_buf[k]), fb_mps_hs, false);
		}
```

Then replace the open-loop slew block inside the ring loop ("No feedback
target in this phase..." down to the `uac1_rate_slew(...)` call) with the FS
target selection:

```c
			// One frame consumed: age the feedback and take one slew
			// step toward the device's own report when following and
			// fresh, else the manual nominal + trim -- the same target
			// selection as the FS loop, fed by the iTD reader above.
			if (fb_frames_since < 0xFFFFFF) fb_frames_since++;
			uint32_t target = (follow_fb && fb_frames_since < FB_FRESH_FRAMES
			                   && fb_avg_mhz)
			                  ? fb_avg_mhz : effectiveRateMilliHz();
			fb_sizing_mhz = uac1_rate_slew(fb_sizing_mhz, target,
			                               FB_SLEW_MHZ_PER_FRAME);
```

- [ ] **Step 4: `stopStreaming()` — reclaim the HS reader**

After the existing `fb_sitd` unlink/free loop:

```c
	for (uint32_t k = 0; k < FB_SLOTS; k++) {
		if (!fb_itd[k]) continue;
		itd_unlink(periodic_frame_slot(fb_itd[k]->frame), fb_itd[k]);
		itd_free(fb_itd[k]);
		fb_itd[k] = 0;
	}
```

and beside the existing `fb_endpoint = 0;` add `fb_mps_hs = 0;`.

`disconnect()` needs no change: feedback state deliberately survives detach
(self-heal contract documented in the function), and that now covers the iTD
reader identically.

- [ ] **Step 5: Verify — suite and both firmware builds**

```bash
make -C ~/Development/USBHost_t36/test run
```
Expected: all binaries pass.

```bash
cd ~/Development/rt1170/evkb/examples/usb/usb_audio_graph_test && cmake -B build -DCMAKE_TOOLCHAIN_FILE=toolchain/rt1170-evkb.toolchain.cmake && cmake --build build && cmake -B build-locked -DCMAKE_TOOLCHAIN_FILE=toolchain/rt1170-evkb.toolchain.cmake -DBIAS_LOCKED_PPM=0 && cmake --build build-locked
```
Expected: both exit 0 (local-first resolution picks up the edited
USBHost_t36). `build-locked` compiles with `BIAS_MODE_LOCKED` →
`followFeedback(false)` — measurement on, servo off: Task 5's build.

- [ ] **Step 6: Commit**

```bash
cd ~/Development/USBHost_t36 && git add usb_audio.h usb_audio.cpp && git commit -m "usb_audio: close the rate loop at high speed"
```

---

### Task 5 (bench, controller-executed): Step A — observe-only gate

Prove the iTD IN path and decode against two independent priors BEFORE the
servo may steer. This isolates the one real silicon unknown (does this
ChipIdea-derived controller write received length back into iTD transaction
words as EHCI says?) from servo behaviour.

- [ ] **Step 1: Flash the locked build (VCOM-free — see bench rules)**

```bash
pkill -9 -f rt1170-console.py; pkill -9 -f LinkServer; pkill -9 redlinkserv; pkill -9 crt_emu_cm_redlink; sleep 2
LinkServer flash MIMXRT1176:MIMXRT1170-EVKB load ~/Development/rt1170/evkb/examples/usb/usb_audio_graph_test/build-locked/usb_audio_graph_test.elf
```
Expected: synchronous success. Then reset and only then attach the console:

```bash
pkill -9 -f LinkServer; pkill -9 redlinkserv; pkill -9 crt_emu_cm_redlink; sleep 2
LinkServer probes            # note the MCU-Link serial
LinkServer probe <serial> wiretimedreset 200
python3 ~/Development/rt1170/evkb/tools/rt1170-console.py /dev/cu.usbmodem<...> 115200
```

- [ ] **Step 2: Assert the observe-only signature (≥ 3 min of heartbeats)**

PASS requires ALL of, steady state:
- `uac2=1 alt=1 pkts/s=1000 err/s=0` (P1's transport signature intact)
- `fbpkts` climbing ≈ 62/s (2 descriptors / 32 ms each), `fberr=0`
- `fbrej`: a startup burst while the device's measurement engine spins up
  (UAC1 saw ~113 reports; magnitude may differ), then static
- `fresh=1`, and `fb=` ≈ **44 096 2xx mHz** (± a few ppm — temperature moves
  the crystal; −83..−86 ppm is the known band), dithering by ~122 mHz steps
- `sizing=44100000` and `bias=+0ppm` throughout (servo locked out — the loop
  is still open, so the device keeps overflow-correcting every ~9.6 s; that
  is EXPECTED here)

FAIL modes and what they mean: `fbpkts=0, fberr` climbing → iTD IN transaction
errors (endpoint/direction encoding); `fbpkts=0, fbrej` climbing forever →
reads complete but `st.length != 4` (the length write-back suspicion — dump
`fst.length` values before touching anything else); `fb=` far outside
44 096 2xx → decode or byte-order defect. Stop and debug; do NOT proceed to
Task 6 on a failed Step A.

- [ ] **Step 3: Save the console excerpt**

Keep ~20 heartbeat lines for the transcript (Task 6 wraps evidence together).

---

### Task 6 (bench, controller-executed): Steps B/C — close the loop, soak, transcript

- [ ] **Step 1: Flash the default (follow-on) build**

Same VCOM-free sequence as Task 5 Step 1 with
`build/usb_audio_graph_test.elf`, then reset, then console.

- [ ] **Step 2: Watch convergence**

Expected within ~2 s of stream start (slew is ~90 ppm/s across an ~86 ppm
gap): `sizing=` walks from 44100000 to ≈ 44 096 2xx and then tracks `fb=`
within ~1.5 ppm (UAC1's observed tracking). `fresh=1`, `err/s=0`,
`pkts/s=1000`.

- [ ] **Step 3: Device-side VCD soak, ≥ 25 min**

Collect the decoupler probes exactly as the P1 gate did (xrun
`--xscope-file` flow, adapter 3LajHPG5; let it run ≥ 1500 s; end the capture
cleanly — never `kill -9` a live collector). Analyze:

```bash
python3 ~/Development/rt1170/evkb/tools/vcdfill.py <capture>.vcd
```

PASS requires ALL of:
- steady-state fill slope: |B/s| ≤ ~1.4 (this stream is 1411.2 B/s total, so
  1.4 B/s ≈ 1 ppm — the spec's criterion; vcdfill's printed ppm column
  assumes 4 B frames, so compute ppm = slope_Bps / 1.4112 by hand)
- `overflow_events` / `dryout_events`: ZERO after the stream-start transient
  (P1's open loop: one overflow every ~9.6 s, 16 in 184 s — that cadence must
  be gone)
- fill envelope: bounded wander (UAC1 soak: ±2 packets of jitter), no ramp
- heartbeat over the whole soak: `err/s=0`, `pkts/s=1000`, `fberr=0`, `fbpkts`
  sustained ≈ 62/s, `fresh=1`

- [ ] **Step 4: Append the transcript section and commit (evkb)**

Add to `examples/usb/usb_audio_graph_test/transcript_hw_evkb.txt` a "UAC2 P3
feedback gate" section in the file's established voice, recording: Step A
observe-only numbers (decoded rate vs the two priors), the closed-loop
convergence, soak duration/slope/corrections, the A/B against P1's open-loop
signature, and any surprises (especially anything the iTD IN write-back did
differently from EHCI's letter — silicon findings are this file's purpose).

```bash
cd ~/Development/rt1170/evkb && git add examples/usb/usb_audio_graph_test/transcript_hw_evkb.txt && git commit -m "usb_audio_graph_test: UAC2 P3 gate -- closed-loop soak evidence"
```

---

### Task 7: wrap — full verification and bookkeeping

- [ ] **Step 1: Full host suite + licence gate**

```bash
make -C ~/Development/USBHost_t36/test run
cd ~/Development/rt1170/evkb && ./tools/license-audit.sh
```
Expected: every binary passes; audit PASS. `docs/KNOWN-BROKEN-GATES.md` is
untouched (this example category has no QEMU gate — spec §2).

- [ ] **Step 2: Final review pass**

Dispatch the final code reviewer over the P3 diff range (all USBHost_t36
commits from Task 1 through Task 4, plus the evkb transcript commit); fix
anything found, re-running the suite after each fix.

- [ ] **Step 3: Status report**

Report completion to the user with the gate numbers. Do NOT push or bump the
`evkb.cmake` pin — pushing happens only on explicit request (established
session norm); note that the pin bump will be needed when asked.
