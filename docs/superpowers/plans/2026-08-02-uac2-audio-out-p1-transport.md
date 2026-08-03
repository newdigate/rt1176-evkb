# UAC2 Audio Out — P1 Transport Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Stream nominal-rate audio from the RT1176 EVKB host to a high-speed
UAC2 device (XMOS MC200, config `2AMi8o8xxxxxx`) via EHCI iTDs, verified by the
device's own decoupler probes.

**Architecture:** Spec `docs/superpowers/specs/2026-08-02-uac2-audio-out-design.md`
(P1 slice). An iTD layer joins the siTD layer in `ehci_iso.{h,cpp}` (16-dword
descriptor, 8 µframe transactions, no splits at HS). A minimal UAC2 parse
(`usb_audio2_parse.{h,cpp}`) walks AC terminals→clock source and AS alt formats
from a real fixture captured off the MC200. `USBAudioOut` gains a UAC2/HS path:
Clock-Source CUR(44100) → SET_INTERFACE → iTD ring, FIFO 16-bit stereo packed
into 24-in-4 subslots across 8 channels (6 zero-filled). Feedback stays out of
scope (P3); the stream runs open-loop at nominal, which on this bench drifts
+85 ppm-ish and dry-outs every ~25 s — that *known signature* is part of the
P1 gate evidence.

**Tech Stack:** ARM GCC 10 (evkb toolchain), host-side c++11 test suite
(`USBHost_t36/test/Makefile`), XMOS XTC 15.3.1 (`xflash`/`xrun`), LinkServer
26.6.137, `tools/driftrun.sh` conventions for bench discipline (VCOM rule!).

**Bench preconditions:** MC200 currently boots UAC1 `1AMi2o2xxxxxx` from flash;
EVKB boots the closed-loop `usb_audio_graph_test`. Task 8 swaps the MC200 to
UAC2 and Task 13 records how to swap back.

---

### Task 1: iTD hardware layout

**Files:**
- Modify: `~/Development/USBHost_t36/ehci_iso.h` (after the `sitd_t` block)
- Create: `~/Development/USBHost_t36/test/test_itd.cpp`
- Modify: `~/Development/USBHost_t36/test/Makefile`

- [ ] **Step 1: Write the failing layout test**

Create `test/test_itd.cpp`:

```cpp
// Copyright (c) 2026 Nicholas Newdigate
// SPDX-License-Identifier: MIT
#include "ehci_iso.h"
#include <stdio.h>
#include <stddef.h>

static int failures = 0, checks = 0;
#define CHECK_EQ(a, b) do { checks++; long _ck_a = (long)(a), _ck_b = (long)(b); \
	if (_ck_a != _ck_b) { printf("FAIL %s:%d: %s == %s (got %ld, want %ld)\n", \
	__FILE__, __LINE__, #a, #b, _ck_a, _ck_b); failures++; } } while (0)

static void test_itd_layout(void)
{
	// EHCI 1.0 section 3.3: next link, eight transaction words, seven
	// buffer page pointers -- 16 consecutive dwords, 32-byte aligned.
	CHECK_EQ(offsetof(itd_t, next), 0);
	CHECK_EQ(offsetof(itd_t, transaction), 4);
	CHECK_EQ(offsetof(itd_t, bufptr), 36);
	CHECK_EQ(sizeof(((itd_t *)0)->transaction), 32);
	CHECK_EQ(sizeof(((itd_t *)0)->bufptr), 28);
	CHECK_EQ(sizeof(itd_t) % 32, 0);
	CHECK_EQ(alignof(itd_t), 32);

	static itd_t pool[4];
	CHECK_EQ(((size_t)(void *)pool) % 32, 0);
	CHECK_EQ((char *)&pool[1] - (char *)&pool[0], (long)sizeof(itd_t));
}

int main(void)
{
	test_itd_layout();
	printf("%d checks, %d failures\n", checks, failures);
	return failures ? 1 : 0;
}
```

Add to `test/Makefile` (next to the `test_sitd` rule, and add `test_itd` to
the `run` target's dependencies and body, and to `clean`):

```makefile
test_itd: test_itd.cpp ../ehci_iso.cpp ../ehci_iso.h
	$(CXX) $(CXXFLAGS) -o $@ test_itd.cpp ../ehci_iso.cpp
```

- [ ] **Step 2: Run it, expect compile failure**

Run: `make -C ~/Development/USBHost_t36/test test_itd`
Expected: FAIL — `itd_t` not defined.

- [ ] **Step 3: Add the struct to `ehci_iso.h`** (below the `sitd_t` typedef)

```c
// EHCI 1.0 section 3.3. High-speed isochronous: one iTD per frame carries up
// to eight microframe transactions -- no splits, the TT is not involved.
// aligned(32) is the hardware alignment requirement and makes the pool
// stride a multiple of 32, same reasoning as sitd_t.
typedef struct itd_struct {
	uint32_t next;            // next link pointer + type
	uint32_t transaction[8];  // per-microframe status/control
	uint32_t bufptr[7];       // page pointers; low bits carry ep/mps/dir
	// --- software bookkeeping, not read by hardware ---
	struct itd_struct *next_free;
	uint16_t frame;           // frame index this iTD is linked into
	uint16_t reserved;
} __attribute__ ((aligned(32))) itd_t;

// Transaction word fields, EHCI 1.0 Table 3-3.
#define ITD_TXN_ACTIVE       0x80000000u
#define ITD_TXN_ERR_BUFFER   0x40000000u
#define ITD_TXN_ERR_BABBLE   0x20000000u
#define ITD_TXN_ERR_XACT     0x10000000u
#define ITD_TXN_LENGTH_SHIFT 16u        // bits 27:16
#define ITD_TXN_IOC          0x00008000u
#define ITD_TXN_PG_SHIFT     12u        // bits 14:12
#define ITD_TXN_OFFSET_MASK  0x00000FFFu
```

- [ ] **Step 4: Run test, expect pass**

Run: `make -C ~/Development/USBHost_t36/test test_itd && ~/Development/USBHost_t36/test/test_itd`
Expected: `9 checks, 0 failures`

- [ ] **Step 5: Commit**

```bash
cd ~/Development/USBHost_t36 && git add ehci_iso.h test/test_itd.cpp test/Makefile && git commit -m "ehci: iTD hardware layout for high-speed isochronous"
```

---

### Task 2: iTD pool

**Files:**
- Modify: `~/Development/USBHost_t36/ehci_iso.h`, `ehci_iso.cpp`
- Modify: `~/Development/USBHost_t36/test/test_itd.cpp`

- [ ] **Step 1: Write the failing test** (append to `test_itd.cpp`, call from `main`)

```cpp
static void test_itd_pool(void)
{
	itd_pool_init();
	itd_t *seen[64];
	int n = 0;
	itd_t *node;
	while (n < 64 && (node = itd_alloc()) != NULL) {
		CHECK_EQ(((uintptr_t)node) % 32, 0);
		for (int i = 0; i < n; i++) CHECK_EQ(node == seen[i], false);
		seen[n++] = node;
	}
	CHECK_EQ(n, 40);                       // spec section 4: 40-node pool
	CHECK_EQ((void *)itd_alloc(), (void *)0);
	itd_free(seen[0]);
	CHECK_EQ((void *)itd_alloc(), (void *)seen[0]);
}
```

- [ ] **Step 2: Run, expect link failure** (`itd_pool_init` undefined)

- [ ] **Step 3: Implement** — declarations in `ehci_iso.h` mirroring the sitd
pool block; implementation in `ehci_iso.cpp` below the sitd pool:

```c
#define ITD_POOL_SIZE 40
static USBHOST_DMAMEM itd_t itd_pool[ITD_POOL_SIZE] __attribute__ ((aligned(32)));
static itd_t *itd_free_list;

void itd_pool_init(void)
{
	itd_free_list = NULL;
	for (int i = ITD_POOL_SIZE - 1; i >= 0; i--) {
		itd_pool[i].next_free = itd_free_list;
		itd_free_list = &itd_pool[i];
	}
}

itd_t *itd_alloc(void)
{
	itd_t *node = itd_free_list;
	if (node) itd_free_list = node->next_free;
	return node;
}

void itd_free(itd_t *node)
{
	if (!node) return;
	node->next_free = itd_free_list;
	itd_free_list = node;
}
```

- [ ] **Step 4: Run tests, expect pass** (`make -C test test_itd && ./test_itd`)

- [ ] **Step 5: Commit** — `git commit -m "ehci: iTD pool"`

---

### Task 3: itd_fill_out

**Files:** same trio as Task 2.

- [ ] **Step 1: Write the failing tests** (append; note the buffer straddles
pages via a static aligned array, same high-bits reasoning as test_sitd.cpp)

```cpp
static uint8_t itd_buf[2048] __attribute__ ((aligned(32)));

static void test_fill_out(void)
{
	itd_pool_init();
	itd_t *n = itd_alloc();
	CHECK_EQ(n != NULL, true);

	// 8 microframes of 44.1k stereo-into-8ch 24-in-4: 176/208-byte
	// packets (5 or 6 samples x 8ch x 4B), max packet 208.
	uint16_t lens[8] = {176, 208, 176, 176, 208, 176, 176, 208};
	CHECK_EQ(itd_fill_out(n, 3, 1, itd_buf, lens, 208, false), true);

	// bufptr low bits: ep/addr in [0], max packet in [1], OUT + Multi=1
	// in [2] (EHCI Table 3-6).
	CHECK_EQ(n->bufptr[0] & 0x7F, 3);              // device address
	CHECK_EQ((n->bufptr[0] >> 8) & 0x0F, 1);       // endpoint
	CHECK_EQ(n->bufptr[1] & 0x7FF, 208);           // max packet size
	CHECK_EQ((n->bufptr[1] >> 11) & 1, 0);         // direction OUT (I/O bit is bufptr[1] bit 11)
	CHECK_EQ(n->bufptr[2] & 3, 1);                 // Multi = 1
	// page pointers are consecutive 4K pages of one contiguous buffer
	CHECK_EQ(n->bufptr[0] & 0xFFFFF000u, ((uint32_t)(uintptr_t)itd_buf) & 0xFFFFF000u);
	CHECK_EQ(n->bufptr[1] & 0xFFFFF000u, ((((uint32_t)(uintptr_t)itd_buf) & 0xFFFFF000u) + 4096u));

	// transaction 0: active, len 176, PG 0, offset = buf offset in page
	uint32_t t0 = n->transaction[0];
	CHECK_EQ(t0 & ITD_TXN_ACTIVE, ITD_TXN_ACTIVE);
	CHECK_EQ((t0 >> ITD_TXN_LENGTH_SHIFT) & 0xFFF, 176);
	CHECK_EQ((t0 >> ITD_TXN_PG_SHIFT) & 7, 0);
	CHECK_EQ(t0 & ITD_TXN_OFFSET_MASK, ((uint32_t)(uintptr_t)itd_buf) & 0xFFF);
	CHECK_EQ(t0 & ITD_TXN_IOC, 0);

	// cumulative offsets: transaction 1 starts 176 bytes in
	uint32_t addr1 = (((uint32_t)(uintptr_t)itd_buf) & 0xFFF) + 176;
	CHECK_EQ((n->transaction[1] >> ITD_TXN_PG_SHIFT) & 7, addr1 >> 12);
	CHECK_EQ(n->transaction[1] & ITD_TXN_OFFSET_MASK, addr1 & 0xFFF);

	// a zero-length microframe stays inactive
	uint16_t lens2[8] = {176, 0, 176, 176, 176, 176, 176, 176};
	CHECK_EQ(itd_fill_out(n, 3, 1, itd_buf, lens2, 208, true), true);
	CHECK_EQ(n->transaction[1], 0);
	// ioc lands on the LAST active transaction
	CHECK_EQ(n->transaction[7] & ITD_TXN_IOC, ITD_TXN_IOC);

	// rejections: len > max packet, mps > 1024, nulls
	uint16_t bad[8] = {209, 0, 0, 0, 0, 0, 0, 0};
	CHECK_EQ(itd_fill_out(n, 3, 1, itd_buf, bad, 208, false), false);
	CHECK_EQ(itd_fill_out(n, 3, 1, itd_buf, lens, 1025, false), false);
	CHECK_EQ(itd_fill_out(n, 3, 1, NULL, lens, 208, false), false);
	CHECK_EQ(itd_fill_out(NULL, 3, 1, itd_buf, lens, 208, false), false);
}
```

- [ ] **Step 2: Run, expect link failure**

- [ ] **Step 3: Implement** in `ehci_iso.cpp` (declaration in the header with a
doc comment mirroring `sitd_fill_out`'s):

```c
bool itd_fill_out(itd_t *node, uint8_t dev_addr, uint8_t endpoint,
                  const void *buf, const uint16_t len[8], uint16_t max_packet,
                  bool ioc_last)
{
	if (!node || !buf || !len) return false;
	if (max_packet == 0 || max_packet > 1024) return false;

	uint32_t base = (uint32_t)(uintptr_t)buf;
	uint32_t page0 = base & 0xFFFFF000u;

	int last_active = -1;
	uint32_t off = base & 0xFFFu;
	uint32_t txn[8];
	for (int k = 0; k < 8; k++) {
		if (len[k] == 0) { txn[k] = 0; continue; }
		if (len[k] > max_packet) return false;
		uint32_t pg = off >> 12;
		if (pg > 6) return false;   // contiguous buffer outran the pages
		txn[k] = ITD_TXN_ACTIVE
		       | ((uint32_t)len[k] << ITD_TXN_LENGTH_SHIFT)
		       | (pg << ITD_TXN_PG_SHIFT)
		       | (off & ITD_TXN_OFFSET_MASK);
		last_active = k;
		off += len[k];
	}
	if (last_active < 0) return false;
	if (ioc_last) txn[last_active] |= ITD_TXN_IOC;

	for (int k = 0; k < 8; k++) node->transaction[k] = txn[k];

	// Contiguous payload means consecutive physical pages: page i is
	// page0 + i*4K. Low bits per EHCI Table 3-6.
	for (int i = 0; i < 7; i++) node->bufptr[i] = page0 + (uint32_t)i * 4096u;
	node->bufptr[0] |= ((uint32_t)endpoint << 8) | dev_addr;
	node->bufptr[1] |= max_packet;
	node->bufptr[2] |= 1u;              // Multi = 1. Direction is bufptr[1]
	                                    // bit 11; stays 0 = OUT here.
	return true;
}
```

- [ ] **Step 4: Run tests, expect pass**
- [ ] **Step 5: Commit** — `git commit -m "ehci: itd_fill_out -- 8 microframe transactions per frame"`

---

### Task 4: per-transaction status harvest

**Files:** same trio.

- [ ] **Step 1: Failing test**

```cpp
static void test_txn_status(void)
{
	itd_pool_init();
	itd_t *n = itd_alloc();
	uint16_t lens[8] = {176, 176, 176, 176, 176, 176, 176, 176};
	CHECK_EQ(itd_fill_out(n, 3, 1, itd_buf, lens, 208, false), true);

	itd_txn_status_t st;
	itd_get_txn_status(n, 0, &st);
	CHECK_EQ(st.active, true);
	CHECK_EQ(st.err_xact || st.err_babble || st.err_buffer, false);
	CHECK_EQ(st.length, 176);

	// controller retires transaction 2 with a babble error
	n->transaction[2] = (n->transaction[2] & ~ITD_TXN_ACTIVE) | ITD_TXN_ERR_BABBLE;
	itd_get_txn_status(n, 2, &st);
	CHECK_EQ(st.active, false);
	CHECK_EQ(st.err_babble, true);

	itd_get_txn_status(NULL, 0, &st);      // nulls tolerated
	CHECK_EQ(st.active, false);
	itd_get_txn_status(n, 8, &st);         // txn index out of range
	CHECK_EQ(st.active, false);
}
```

- [ ] **Step 2: Run, expect link failure**
- [ ] **Step 3: Implement** (`itd_txn_status_t {bool active, err_xact, err_babble, err_buffer; uint16_t length;}` in the header):

```c
void itd_get_txn_status(const itd_t *node, unsigned txn, itd_txn_status_t *out)
{
	if (!out) return;
	if (!node || txn > 7) {
		out->active = out->err_xact = out->err_babble = out->err_buffer = false;
		out->length = 0;
		return;
	}
	uint32_t t = node->transaction[txn];
	out->active     = (t & ITD_TXN_ACTIVE) != 0u;
	out->err_buffer = (t & ITD_TXN_ERR_BUFFER) != 0u;
	out->err_babble = (t & ITD_TXN_ERR_BABBLE) != 0u;
	out->err_xact   = (t & ITD_TXN_ERR_XACT) != 0u;
	out->length     = (uint16_t)((t >> ITD_TXN_LENGTH_SHIFT) & 0xFFFu);
}
```

- [ ] **Step 4: Run tests, expect pass**
- [ ] **Step 5: Commit** — `git commit -m "ehci: iTD per-transaction status harvest"`

---

### Task 5: itd_link / itd_unlink

**Files:** same trio.

- [ ] **Step 1: Failing test** (mirror `test_link_unlink` in test_sitd.cpp;
iTD link type bits are 0x00, and skip_iso must still walk past it — that case
already passes in test_sitd against a hand-built word, now it runs against the
real linker)

```cpp
static uint32_t slot;

static void test_link_unlink(void)
{
	itd_pool_init();
	itd_t *a = itd_alloc();
	itd_t *b = itd_alloc();

	slot = 0x01u;                          // empty frame slot (terminate)
	itd_link(&slot, a, 7);
	CHECK_EQ(slot & 0x06u, 0x00u);         // type bits: iTD
	CHECK_EQ(slot & 0xFFFFFFE0u, (uint32_t)(uintptr_t)a & 0xFFFFFFE0u);
	CHECK_EQ(a->next, 0x01u);
	CHECK_EQ(a->frame, 7);

	itd_link(&slot, b, 7);
	CHECK_EQ((void *)sitd_skip_iso(&slot), (void *)&a->next);

	CHECK_EQ(itd_unlink(&slot, a), true);
	CHECK_EQ(itd_unlink(&slot, b), true);
	CHECK_EQ(slot, 0x01u);
	CHECK_EQ(itd_unlink(&slot, a), false);
}
```

- [ ] **Step 2: Run, expect link failure**
- [ ] **Step 3: Implement** in `ehci_iso.cpp` (add `#define LINK_TYPE_ITD 0x00u`
next to the other LINK_TYPE defines; bodies mirror sitd_link/sitd_unlink with
the iTD type in the target word — the traversal, guard bound, and high-bits
splice are identical, so factor the shared walk only if it stays obviously
readable; duplication of ~15 lines is acceptable here):

```c
void itd_link(volatile uint32_t *frame_slot, itd_t *node, uint16_t frame)
{
	if (!frame_slot || !node) return;
	node->next = *frame_slot;
	node->frame = frame;
	*frame_slot = ((uint32_t)(uintptr_t)node & LINK_ADDR_MASK) | LINK_TYPE_ITD;
}

bool itd_unlink(volatile uint32_t *frame_slot, itd_t *node)
{
	if (!frame_slot || !node) return false;
	uint32_t target = ((uint32_t)(uintptr_t)node & LINK_ADDR_MASK) | LINK_TYPE_ITD;
	volatile uint32_t *link = frame_slot;
	for (unsigned guard = 0; guard <= ITD_POOL_SIZE; guard++) {
		uint32_t cur = *link;
		if (cur & LINK_TERMINATE) return false;
		if ((cur & LINK_TYPE_MASK) == LINK_TYPE_QH) return false;
		if ((cur & (LINK_ADDR_MASK | LINK_TYPE_MASK)) == target) {
			*link = node->next;
			node->next = LINK_TERMINATE;
			return true;
		}
		uintptr_t same_region = (uintptr_t)link & ~(uintptr_t)0xFFFFFFFFu;
		link = (volatile uint32_t *)(same_region | (uintptr_t)(cur & LINK_ADDR_MASK));
	}
	return false;
}
```

- [ ] **Step 4: Run full suite** — `make -C test run` — all binaries pass.
- [ ] **Step 5: Commit** — `git commit -m "ehci: iTD link/unlink into the periodic frame list"`

---

### Task 6: µframe packet sizing + 16→subslot packer

**Files:**
- Modify: `~/Development/USBHost_t36/usb_audio_parse.h`, `usb_audio_parse.cpp`
- Create tests in: `~/Development/USBHost_t36/test/test_uac1_parse.cpp` (sizing)
  and a new `test/test_pack.cpp` + Makefile rule (packer)

- [ ] **Step 1: Failing sizing test** (append to test_uac1_parse.cpp)

```cpp
static void test_uframe_bytes(void)
{
	// 44.1 kHz across 8 kHz microframes: 5.5125 samples/uframe. Over 80
	// microframes exactly 441 samples must emerge, sizes only 5 or 6.
	uint32_t accum = 0;
	uint32_t total = 0;
	for (int i = 0; i < 80; i++) {
		uint16_t b = uac2_uframe_bytes_mhz(&accum, 44100000u, 8, 4);
		CHECK_EQ(b % 32, 0);               // whole 8ch x 4B frames only
		uint16_t samples = b / 32;
		CHECK_EQ(samples == 5 || samples == 6, true);
		total += samples;
	}
	CHECK_EQ(total, 441);
	CHECK_EQ(accum, 0);                    // exact over the repeat period
}
```

- [ ] **Step 2: Run, expect link failure**
- [ ] **Step 3: Implement** — one line in `usb_audio_parse.cpp` beside
`uac1_frame_bytes_mhz`, declaration + doc comment in the header:

```c
uint16_t uac2_uframe_bytes_mhz(uint32_t *accum, uint32_t rate_mhz, uint8_t channels,
                               uint8_t bytes_per_sample)
{
	return frame_bytes_scaled(accum, rate_mhz, 8000000u, channels, bytes_per_sample);
}
```

- [ ] **Step 4: Failing packer test** — create `test/test_pack.cpp` with the
house CHECK_EQ macro and:

```cpp
#include "usb_audio_parse.h"

static void test_pack(void)
{
	int16_t src[4] = {0x1234, (int16_t)0xFEDC, 0x7FFF, (int16_t)0x8000};
	uint8_t dst[64];

	// 2 stereo frames into 8ch 24-in-4, LEFT-JUSTIFIED within the subslot
	// (USB Audio Data Formats 2.0 section 2.3.1): pad bytes at the low end,
	// the sign is the sample's own high byte.
	uint32_t n = uac_pack16(dst, src, 2, 2, 8, 4);
	CHECK_EQ(n, 64);
	CHECK_EQ(dst[0], 0x00); CHECK_EQ(dst[1], 0x00); CHECK_EQ(dst[2], 0x34); CHECK_EQ(dst[3], 0x12);
	CHECK_EQ(dst[4], 0x00); CHECK_EQ(dst[5], 0x00); CHECK_EQ(dst[6], 0xDC); CHECK_EQ(dst[7], 0xFE);
	for (int i = 8; i < 32; i++) CHECK_EQ(dst[i], 0);   // channels 3..8 zero
	CHECK_EQ(dst[34], 0xFF); CHECK_EQ(dst[35], 0x7F);   // frame 2 left
	CHECK_EQ(dst[38], 0x00); CHECK_EQ(dst[39], 0x80);   // frame 2 right

	// 24-in-3 and native 16-in-2
	n = uac_pack16(dst, src, 1, 2, 2, 3);
	CHECK_EQ(n, 6);
	CHECK_EQ(dst[0], 0x00); CHECK_EQ(dst[1], 0x34); CHECK_EQ(dst[2], 0x12);
	n = uac_pack16(dst, src, 1, 2, 2, 2);
	CHECK_EQ(n, 4);
	CHECK_EQ(dst[0], 0x34); CHECK_EQ(dst[1], 0x12);

	CHECK_EQ(uac_pack16(dst, src, 1, 2, 2, 5), 0);      // unsupported subslot
}
```

- [ ] **Step 5: Implement `uac_pack16`** in `usb_audio_parse.cpp` (header decl:
"Pack 16-bit interleaved frames into a device subslot layout, sample
LEFT-JUSTIFIED within the subslot per USB Audio Data Formats 2.0 section
2.3.1 -- padding at the least-significant end, so a negative sample's sign
is simply its own high byte. Returns bytes written, 0 for unsupported
subslot sizes / nulls / ch_live > ch_total. Device channels beyond ch_live
are zero-filled."):

```c
uint32_t uac_pack16(uint8_t *dst, const int16_t *src, uint32_t frames,
                    uint8_t ch_live, uint8_t ch_total, uint8_t subslot)
{
	if (!dst || !src || subslot < 2 || subslot > 4 || ch_live > ch_total) return 0;
	uint8_t *p = dst;
	for (uint32_t f = 0; f < frames; f++) {
		for (uint8_t c = 0; c < ch_total; c++) {
			int32_t s = (c < ch_live) ? src[f * ch_live + c] : 0;
			uint8_t lo = (uint8_t)s;
			uint8_t hi = (uint8_t)((uint16_t)s >> 8);
			switch (subslot) {
			case 2: *p++ = lo; *p++ = hi; break;
			case 3: *p++ = 0; *p++ = lo; *p++ = hi; break;
			case 4: *p++ = 0; *p++ = 0; *p++ = lo; *p++ = hi; break;
			}
		}
	}
	return (uint32_t)(p - dst);
}
```

- [ ] **Step 6: Run full suite, expect pass; commit**

```bash
git add usb_audio_parse.h usb_audio_parse.cpp test/test_uac1_parse.cpp test/test_pack.cpp test/Makefile
git commit -m "UAC2: microframe packet sizing and 16-to-subslot packing"
```

---

### Task 7: flash the UAC2 witness and capture its config descriptor as a fixture

**Files:**
- Temporary scaffold in `~/Development/USBHost_t36/usb_audio.cpp` (reverted in-task)
- Create: `~/Development/USBHost_t36/test/fixtures/xmos_uac2_2ami8o8.bin`

**Bench rules:** never touch LinkServer with a VCOM reader attached; kill
`rt1170-console.py` first (`tools/driftrun.sh` has the canonical sequence).

- [ ] **Step 1: Add the temporary dump scaffold** at the top of
`USBAudioOut::claim()` (before the bcdADC check):

```c
	// TEMPORARY (this task only): dump the raw configuration descriptors so
	// the real device bytes become a parser fixture. Reverted after capture.
	Serial1.printf("\nCONFIG-DUMP len=%lu\n", (unsigned long)len);
	for (uint32_t i = 0; i < len; i++) {
		Serial1.printf("%02x", descriptors[i]);
		if ((i & 31) == 31) Serial1.println();
	}
	Serial1.println("\nCONFIG-DUMP-END");
```

- [ ] **Step 2: Rebuild + flash the EVKB example** (bias/feedback irrelevant here):

```bash
cd ~/Development/rt1170/evkb/examples/usb/usb_audio_graph_test && cmake --build build
pkill -f rt1170-console; pkill LinkServer; pkill redlinkserv; pkill crt_emu_cm_redlink; sleep 1
/Applications/LinkServer_26.6.137/LinkServer run MIMXRT1176:MIMXRT1170-EVKB build/usb_audio_graph_test.elf &
sleep 90; pkill LinkServer; pkill redlinkserv; pkill crt_emu_cm_redlink
```

- [ ] **Step 3: Flash the MC200 to UAC2**

```bash
cd /Applications/XMOS_XTC_15.3.1 && source SetEnv.sh && cd ~/Development/xmos/sw_usb_audio/app_usb_aud_xk_216_mc
xflash --adapter-id 3LajHPG5 bin/2AMi8o8xxxxxx/app_usb_aud_xk_216_mc_2AMi8o8xxxxxx.xe
```

Expected: `Site 0 has finished successfully.` The device reboots as a
high-speed UAC2 device; the UAC1 claim rejects it (bcdADC 0x0200), which is
fine — the dump happens before the rejection.

- [ ] **Step 4: Capture the dump**

```bash
python3 ~/Development/rt1170/evkb/tools/rt1170-console.py /dev/cu.usbmodem5DQ2DDHVWO5EI3 115200 | tee /tmp/uac2dump.log
# wait for CONFIG-DUMP-END, Ctrl-C
sed -n '/CONFIG-DUMP len/,/CONFIG-DUMP-END/p' /tmp/uac2dump.log | grep -E '^[0-9a-f]+$' | tr -d '\n' | xxd -r -p > ~/Development/USBHost_t36/test/fixtures/xmos_uac2_2ami8o8.bin
xxd ~/Development/USBHost_t36/test/fixtures/xmos_uac2_2ami8o8.bin | head -3
```

Expected: first bytes are an interface or IAD descriptor (claim receives
`enumbuf+9`), and `0200` appears early (bcdADC little-endian in the class
HEADER). Size: several hundred bytes.

- [ ] **Step 5: Revert the scaffold, commit only the fixture**

```bash
cd ~/Development/USBHost_t36 && git checkout usb_audio.cpp
git add test/fixtures/xmos_uac2_2ami8o8.bin && git commit -m "test: MC200 2AMi8o8xxxxxx UAC2 configuration fixture (captured from hardware)"
```

---

### Task 8: minimal UAC2 parse

**Files:**
- Create: `~/Development/USBHost_t36/usb_audio2_parse.h`, `usb_audio2_parse.cpp`
- Create: `~/Development/USBHost_t36/test/test_uac2_parse.cpp` + Makefile rule
- Modify: `~/Development/USBHost_t36/usb_audio_parse.h` (one field)

Add `uint8_t clock_source_id;` to `UAC1Topology` (UAC1 parse leaves it 0; the
struct is shared, discriminated by `bcd_adc` — spec section 2).

- [ ] **Step 1: Failing tests** — `test_uac2_parse.cpp` loads the fixture file
(same pattern as test_uac1_parse.cpp's fixture loader) and asserts:

```cpp
static void test_parse_mc200(void)
{
	UAC1Topology t;
	CHECK_EQ(uac2_parse_config(fixture, fixture_len, &t), true);
	CHECK_EQ(t.bcd_adc, 0x0200);
	CHECK_EQ(t.clock_source_id != 0, true);
	CHECK_EQ(t.alt_count >= 1, true);

	// the 24-bit 8ch OUT alt exists and carries both endpoints
	int alt = uac2_find_alt(&t, 8, 24);
	CHECK_EQ(alt >= 1, true);
	const UAC1AltSetting *a = 0;
	for (uint8_t i = 0; i < t.alt_count; i++)
		if (t.alts[i].alternate_setting == alt) a = &t.alts[i];
	CHECK_EQ(a != 0, true);
	CHECK_EQ(a->endpoint_address & 0x80, 0);        // OUT data EP
	CHECK_EQ(a->subframe_size, 4);
	CHECK_EQ(a->bit_resolution, 24);
	CHECK_EQ(a->channels, 8);
	CHECK_EQ(a->max_packet_size > 0, true);
	CHECK_EQ(a->feedback_endpoint & 0x80, 0x80);    // IN feedback EP
	CHECK_EQ(a->rate_count, 0);                     // rates live behind RANGE
}

static void test_reject_truncated(void)
{
	UAC1Topology t;
	for (size_t l = 0; l < 40 && l < fixture_len; l++)
		CHECK_EQ(uac2_parse_config(fixture, l, &t), false);
}
```

- [ ] **Step 2: Run, expect failure**
- [ ] **Step 3: Implement the walk** in `usb_audio2_parse.cpp`. Structure
(complete descriptor tags; UAC2 spec section 4):

```c
// Walk by bLength like uac1_parse_config. Collect:
//  - AC HEADER (CS_INTERFACE 0x24 subtype 0x01): bcdADC at offset 3..4;
//    reject unless 0x0200.
//  - CLOCK_SOURCE (subtype 0x0A): record ids (bClockID at offset 3).
//  - CLOCK_SELECTOR (subtype 0x0B): if bNrInPins (offset 4) == 1, map the
//    selector id to its single baCSourceID (offset 5) -- the MC200 witness
//    routes terminal -> selector(1 pin) -> source, and rejecting selectors
//    outright rejects the witness itself. Multi-input selectors and
//    multipliers stay rejected. Rate CUR targets the RESOLVED source id.
//  - INPUT_TERMINAL (subtype 0x02): map bTerminalID -> bCSourceID
//    (offsets 3 and 7).
//  - Standard INTERFACE (0x04): track current interface/alt; an AS
//    interface is class 0x01 subclass 0x02.
//  - AS_GENERAL (CS_INTERFACE subtype 0x01 within an AS interface):
//    bTerminalLink at offset 3 -> resolve terminal -> clock_source_id;
//    bNrChannels at offset 10.
//  - FORMAT_TYPE (subtype 0x02): bSubslotSize offset 4, bBitResolution
//    offset 5.
//  - Standard ENDPOINT (0x05): 7-byte form in UAC2. Iso OUT -> data EP +
//    wMaxPacketSize; iso IN with usage bits (bmAttributes 5:4) == 01
//    (feedback) -> feedback_endpoint; ALSO accept an iso IN with usage 00
//    on the same AS interface as feedback (the lib_xua UAC1 lesson --
//    bSynchAddress does not exist in the UAC2 endpoint pairing, usage
//    bits are the primary marker here).
// Fill alts[] exactly like uac1_parse_config does; rate_count = 0,
// rate_min = rate_max = 0 (rates are a runtime RANGE conversation).
// Return false when no AS interface with an iso OUT endpoint was found or
// the header was not 0x0200.
```

The implementation is ~120 lines following `uac1_parse_config`'s loop shape
verbatim — same bounds checks (`if (i + d[i] > len) return false;` etc.), same
alt-slot allocation. `uac2_find_alt(t, channels, bits)` mirrors
`uac1_find_alt` minus the rate check.

- [ ] **Step 4: Run tests, expect pass** (Makefile rule compiles
`test_uac2_parse.cpp ../usb_audio2_parse.cpp`)
- [ ] **Step 5: Commit** — `git commit -m "UAC2: minimal descriptor parse against the MC200 fixture"`

---

### Task 9: Clock Source CUR request builder

**Files:**
- Modify: `~/Development/USBHost_t36/usb_audio2_parse.{h,cpp}`, `test/test_uac2_parse.cpp`

- [ ] **Step 1: Failing test**

```cpp
static void test_clock_cur_setup(void)
{
	// UAC2 5.2.5.1.1: SET CUR of CS_SAM_FREQ_CONTROL on clock entity 5,
	// AC interface 0, rate payload separate (4-byte LE).
	uint8_t s[8];
	uac2_clock_cur_setup(s, 0, 5);
	CHECK_EQ(s[0], 0x21);                  // class request, interface, H->D
	CHECK_EQ(s[1], 0x01);                  // CUR
	CHECK_EQ(s[2], 0x00); CHECK_EQ(s[3], 0x01);   // wValue: CS_SAM_FREQ<<8
	CHECK_EQ(s[4], 0x00); CHECK_EQ(s[5], 0x05);   // wIndex: (clockID<<8)|itf
	CHECK_EQ(s[6], 0x04); CHECK_EQ(s[7], 0x00);   // wLength 4
}
```

- [ ] **Step 2: Run, expect failure**
- [ ] **Step 3: Implement**

```c
void uac2_clock_cur_setup(uint8_t setup[8], uint8_t ac_interface, uint8_t clock_id)
{
	setup[0] = 0x21; setup[1] = 0x01;
	setup[2] = 0x00; setup[3] = 0x01;
	setup[4] = ac_interface; setup[5] = clock_id;
	setup[6] = 0x04; setup[7] = 0x00;
}
```

- [ ] **Step 4: Run tests, pass; commit** — `git commit -m "UAC2: clock CUR setup builder"`

---

### Task 10: claim/control UAC2 path in USBAudioOut

**Files:**
- Modify: `~/Development/USBHost_t36/usb_audio.h`, `usb_audio.cpp`

No host test reaches this (it needs USBHost machinery); the checks are compile
gates plus Task 12's hardware gate. Keep the diff mechanical:

- [ ] **Step 1:** `usb_audio.h`: include `usb_audio2_parse.h`; add
`CTRL_SET_CLOCK` to `CtrlState`; add members `bool is_uac2 = false;
uint8_t rate4_buf[4];` and accessor `bool isUAC2() const { return is_uac2; }`.

- [ ] **Step 2:** `claim()`: replace the hard `bcd_adc != 0x0100` rejection:

```c
	is_uac2 = false;
	if (!uac1_parse_config(descriptors, len, &topo) || topo.bcd_adc == 0x0200) {
		// Not parseable as UAC1 (or the header says 2.0): try the UAC2 walk.
		// NOTE an earlier revision of this snippet returned false when the
		// UAC1 walk failed, which made this branch unreachable for devices
		// the UAC1 parser rejects outright -- caught at implementation.
		if (!uac2_parse_config(descriptors, len, &topo)) return false;
		if (dev->speed != 2) return false;   // HS only (spec: FS UAC2 out of scope)
		is_uac2 = true;
	} else if (topo.bcd_adc != 0x0100) {
		return false;
	}
```

(uac1_parse_config returning false for a UAC2 device is expected — enter the
UAC2 branch on either signal. `Device_t::speed`: 0=FS 1=LS 2=HS, verify the
enum in USBHost_t36.h while editing.)

Then pick the alt: `is_uac2 ? uac2_find_alt(&topo, 8, 24) : uac1_find_alt(...)`
— P1 targets the 8ch/24-bit alt explicitly; stereo third-party alts arrive
with P2 breadth. Control sequence for UAC2: clock CUR first, then
SET_INTERFACE:

```c
	if (is_uac2) {
		uint32_t r = req_rate;
		rate4_buf[0] = r; rate4_buf[1] = r >> 8; rate4_buf[2] = r >> 16; rate4_buf[3] = r >> 24;
		uac2_clock_cur_setup((uint8_t *)&setup, topo.control_interface, topo.clock_source_id);
		ctrl_state = CTRL_SET_CLOCK;
		pending_alt = alt;
		if (!queue_Control_Transfer(dev, &setup, rate4_buf, this)) { ctrl_state = CTRL_IDLE; return false; }
		return true;
	}
```

- [ ] **Step 3:** `control()`: on `CTRL_SET_CLOCK` completion issue
`USBHost::setInterface(device, setup, topo.streaming_interface, pending_alt, this)`
and move to `CTRL_SET_INTERFACE`; the existing `CTRL_SET_INTERFACE` completion
path then finishes as today (UAC2 needs no endpoint SET_CUR:
`uac1_alt_needs_rate_request` sees `rate_count==0 && rate_min==0` — confirm it
returns false for that shape; if not, gate it with `!is_uac2`).

- [ ] **Step 4:** Build the example (`cmake --build build` in
`usb_audio_graph_test`, with `${USBHOST}/usb_audio2_parse.cpp` added to its
CMakeLists sources) — compiles clean. Run `make -C test run` — still green.

- [ ] **Step 5: Commit both repos' changes**

```bash
cd ~/Development/USBHost_t36 && git add -A && git commit -m "UAC2: claim and clock-first control sequence (HS only)"
cd ~/Development/rt1170/evkb && git add examples/usb/usb_audio_graph_test/CMakeLists.txt && git commit -m "usb_audio_graph_test: compile the UAC2 parser"
```

---

### Task 11: high-speed iTD streaming ring

**Files:**
- Modify: `~/Development/USBHost_t36/usb_audio.h`, `usb_audio.cpp`

- [ ] **Step 1:** `usb_audio.h` members (beside the FS ring):

```c
    // HS/UAC2 ring: one iTD per periodic slot, 8 microframe transactions
    // each -- same 32 ms revolution as the siTD ring. Buffers sized for the
    // 8ch 24-in-4 witness at 48k-and-below (8 uframes x 208 B); alts needing
    // more are rejected at claim (spec section 4).
    static const uint16_t MAX_UFRAME_BYTES = 208;
    itd_t   *ring_hs[RING_SLOTS] = {};
    uint8_t  ring_buf_hs[RING_SLOTS][8 * MAX_UFRAME_BYTES];
    uint16_t uframe_len[RING_SLOTS][8];
```

- [ ] **Step 2:** `beginStreaming()`: branch on `is_uac2` into a
`beginStreamingHS()` (private) that mirrors the FS body. Guard on the
NEGOTIATED RATE's per-microframe need, not the advertised ceiling: the live
witness advertises wMaxPacketSize 800 on its 24-bit alt (sized for 192 kHz),
and at 44.1 kHz we use at most 6 samples x 32 B = 192 B of it. Reject when
`(req_rate / 8000 + 1) * req_channels_device * subslot_out > MAX_UFRAME_BYTES`;
pass `min(alt->max_packet_size, MAX_UFRAME_BYTES)` as itd_fill_out's
max_packet so the descriptor's MPS field stays consistent with the buffer
slice. `itd_pool_init()` must be added to init() beside sitd_pool_init();
allocate 32 iTDs, fill each slot:

```c
	frame_accum = 0;
	usb_audio_fifo_reset(&fifo);
	topUpFromTone();
	for (uint32_t i = 0; i < RING_SLOTS; i++) {
		ring_hs[i] = itd_alloc();
		if (!ring_hs[i]) { stopStreaming(); return false; }
		fillFrameHS(i);
		if (!itd_fill_out(ring_hs[i], device->address, iso_endpoint,
		                  ring_buf_hs[i], uframe_len[i],
		                  alt->max_packet_size, false)) {
			stopStreaming(); return false;
		}
		itd_link(periodic_frame_slot(i), ring_hs[i], (uint16_t)i);
	}
```

with `fillFrameHS(i)` computing 8 lengths and packing:

```c
void USBAudioOut::fillFrameHS(uint32_t slot)
{
	uint32_t off = 0;
	for (int k = 0; k < 8; k++) {
		uint16_t bytes = uac2_uframe_bytes_mhz(&frame_accum, fb_sizing_mhz,
		                                       req_channels_device, subslot_out);
		uframe_len[slot][k] = bytes;
		uint32_t frames = bytes / ((uint32_t)req_channels_device * subslot_out);
		int16_t staged[6 * 2];             // up to 6 frames of live stereo
		if (!usb_audio_fifo_read(&fifo, staged, frames * 2)) {
			for (uint32_t b = 0; b < bytes; b++) ring_buf_hs[slot][off + b] = 0;
			underrun_count++;
		} else {
			uac_pack16(ring_buf_hs[slot] + off, staged, frames, 2,
			           req_channels_device, subslot_out);
		}
		off += bytes;
	}
}
```

(`req_channels_device` and `subslot_out` are new members set at claim from the
alt — 8 and 4 for the witness; `fb_sizing_mhz` is already the sizing source
and equals nominal+trim open-loop. The FIFO keeps carrying live stereo only.)

- [ ] **Step 3:** `service()`: branch to a HS harvest loop — an iTD is re-armed
only when ALL its transactions have retired:

```c
	for (uint32_t i = 0; i < RING_SLOTS; i++) {
		itd_t *n = ring_hs[i];
		if (!n) continue;
		bool done = true;
		for (unsigned k = 0; k < 8 && done; k++) {
			itd_txn_status_t st;
			itd_get_txn_status(n, k, &st);
			if (uframe_len[i][k] && st.active) done = false;
		}
		if (!done) continue;
		for (unsigned k = 0; k < 8; k++) {
			if (!uframe_len[i][k]) continue;
			itd_txn_status_t st;
			itd_get_txn_status(n, k, &st);
			if (st.err_xact)   err_xact++;
			if (st.err_babble) err_babble++;
			if (st.err_buffer) err_buffer++;
		}
		fb_frames_since = (fb_frames_since < 0xFFFFFF) ? fb_frames_since + 1 : fb_frames_since;
		uint32_t target = effectiveRateMilliHz();   // open loop until P3
		fb_sizing_mhz = uac1_rate_slew(fb_sizing_mhz ? fb_sizing_mhz : target,
		                               target, FB_SLEW_MHZ_PER_FRAME);
		fillFrameHS(i);
		if (itd_fill_out(n, device->address, iso_endpoint, ring_buf_hs[i],
		                 uframe_len[i], alt_max_packet, false)) {
			packets_sent++;
			if (frame_cb) frame_cb();
		}
	}
```

(`alt_max_packet` cached at beginStreaming. `packets_sent` counts frames, so
the heartbeat's `pkts/s=1000` expectation is unchanged.) `stopStreaming()` and
`topUpFromTone()` gain the obvious HS arms (unlink/free `ring_hs`).

- [ ] **Step 4:** Build example + full host suite; both clean. Commit —
`git commit -m "UAC2: high-speed iTD streaming ring (open loop)"` (both repos
if the example changed).

---

### Task 12: hardware gate

**Files:**
- Modify: `~/Development/rt1170/evkb/examples/usb/usb_audio_graph_test/usb_audio_graph_test.cpp`
  (heartbeat: add `uac2=%d subslot=%u devch=%u` from the new accessors)
- Append: `examples/usb/usb_audio_graph_test/transcript_hw_evkb.txt`

- [ ] **Step 1:** Flash EVKB (driftrun's kill sequence + `LinkServer run`,
90 s window). MC200 is already UAC2 from Task 7.
- [ ] **Step 2:** Start the device under xscope with a fresh VCD + console:

```bash
cd /Applications/XMOS_XTC_15.3.1 && source SetEnv.sh
cd ~/Development/xmos/sw_usb_audio/app_usb_aud_xk_216_mc
nohup xrun --xscope-file /tmp/uac2p1 --id 0 bin/2AMi8o8xxxxxx/app_usb_aud_xk_216_mc_2AMi8o8xxxxxx.xe &
sleep 8
python3 ~/Development/rt1170/evkb/tools/rt1170-console.py /dev/cu.usbmodem5DQ2DDHVWO5EI3 115200 | tee /tmp/uac2p1_console.log
```

- [ ] **Step 3: Gate assertions** (spec section 3, P1), run ≥ 180 s:
  - console: `audio=ready`, `uac2=1`, `pkts/s=1000`, `err/s=0`
  - `python3 ~/Development/rt1170/evkb/tools/vcdfill.py /tmp/uac2p1.vcd`:
    fill emissions present at ~100 Hz cadence; drift slope ≈ +85 ppm
    equivalent scaled for the 8ch stream (open loop at nominal against the
    same −86 ppm crystal; in bytes/s that is 0.1764 × 8 ≈ 1.41 B/s per ppm ×
    ~85 — sanity, not precision); dry-outs/overflows at the period that
    slope predicts, not faster.
  - `xscope_missing_marks: 0`.
- [ ] **Step 4:** Append the numbers to `transcript_hw_evkb.txt` under a
"UAC2 P1 transport (open loop)" heading; commit both repos.

```bash
cd ~/Development/rt1170/evkb && git add examples/usb/usb_audio_graph_test/ && git commit -m "usb_audio_graph_test: UAC2 P1 transport verified open-loop"
```

---

### Task 13: bench restore note + wrap

- [ ] **Step 1:** Decide the flash personality to leave behind and record it in
the transcript: UAC1 closed-loop demo (`xflash ... 1AMi2o2xxxxxx...xe`) or the
UAC2 witness for P2/P3 work (leave as-is). Default: leave UAC2 (P3 continues
here), note the UAC1 restore command inline.
- [ ] **Step 2:** `make -C ~/Development/USBHost_t36/test run` one final time —
all binaries green; run `~/Development/rt1170/evkb/tools/license-audit.sh`
(new source files carry MIT headers — the audit must stay clean).
- [ ] **Step 3:** Final commits pushed only on explicit request (house
convention); remind that `evkb.cmake`'s USBHost_t36 pin needs a bump when
pushed.

---

## Self-review notes (applied)

- Spec coverage: P1 items all mapped (iTD layer T1–5, sizing/packer T6,
  fixture T7, minimal parse T8, CUR builder T9, claim/control T10, ring T11,
  gate T12). RANGE parsing, rejection taxonomy breadth, feedback, and the
  sweep/soak replay are P2–P4 by design.
- The UAC1-vs-UAC2 `claim()` reparse relies on `uac1_parse_config` failing or
  reporting bcd_adc for a UAC2 blob — Task 8's fixture tests pin the UAC2
  walk; Task 10 step 2 handles both signals, and the truncated-input tests
  keep the walk memory-safe on arbitrary descriptors.
- Type consistency: `itd_txn_status_t.length`, `uframe_len[][8]`,
  `uac_pack16` signature, and `uac2_uframe_bytes_mhz` naming are used
  identically across Tasks 3–6 and 11.
