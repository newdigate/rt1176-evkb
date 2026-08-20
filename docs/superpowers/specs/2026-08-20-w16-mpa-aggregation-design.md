# W16: multiport aggregation (MPA) — design

Goal: break the ~3–14 Mbps ceiling by cutting **SDIO bus commands per frame**,
the only quantity that has ever moved this number. Target subsystem: M2Radio's
`Iw416` data path; QEMU counterpart: `hw/sd/iw416-sdio.c` on
gitlab.com/Newdigate/qemu-rt1170.

Everything below is anchored to NXP's own driver (mcuxsdk
`middleware/wifi_nxp`, BSD-3-Clause) or to a silicon capture already in this
tree. Where neither exists it is called out in "Where this could be lying".

---

## 1. What the reference actually says

Read: `wifidriver/wifi-sdio.c`, `wifidriver/sdio.c`,
`sdio_nxp_abs/mlan_sdio.c`, `sdio_nxp_abs/incl/mlan_sdio.h`,
`sdio_nxp_abs/incl/mlan_sdio_defs.h`, `wifidriver/incl/mlan_main_defs.h`.

### 1a. The multiport REGISTER-BLOCK read — the piece the handoff did not name

`wlan_interrupt()` (`wifi-sdio.c:2658`) does **not** poll registers with CMD52.
It reads the card's whole multiport register group in **one CMD53**:

```c
ret = sdio_drv_read(REG_PORT | MLAN_SDIO_BYTE_MODE_MASK, 1, 1, MAX_MP_REGS,
                    mp_regs, &resp);
```

* `REG_PORT` = 0 (`mlan_sdio_defs.h:95`) — function 1, address 0.
* `MLAN_SDIO_BYTE_MODE_MASK` = 0x80000000 (`mlan_decl.h:276`) is an internal
  flag for the `sdio_drv` layer; it is masked off the 17-bit CMD53 address.
* `bcnt = 1` ⇒ `sdio_drv_read` takes the **byte-mode** branch
  (`mlan_sdio.c:167-176`): no `SDIO_EXTEND_CMD_BLOCK_MODE_MASK`, `param =
  bsize = MAX_MP_REGS`, and **`flags = 0`, so OP Code = 0 (fixed address)**
  (`fsl_sdio.c:576`).
* `MAX_MP_REGS` = **196** for SD8978 (`mlan_main_defs.h:33`) ⇒ registers
  0x00..0xC3.

That single command yields everything the service loop needs:

| offset | content |
|---|---|
| 0x0C | `HOST_INT_STATUS` (clear-on-read, as we configure it) |
| 0x10–0x13 | `RD_BITMAP` (32-bit) |
| 0x14–0x17 | `WR_BITMAP` (32-bit) |
| 0x18–0x57 | `RD_LEN_P0..P31`, 32 LE pairs at `0x18 + (port<<1)` |
| 0xC0–0xC1 | `CMD_RD_LEN` (command-port reply length) |

Our driver currently pays, per RX frame: 1 CMD52 (status) + 4 (rd bitmap) +
2 (RD_LEN) = **7 CMD52**, and per TX frame 4 CMD52 per wr-bitmap poll. At the
measured ~0.1 ms per SDIO command (software-inclusive, W11) that is where the
~10 commands/frame come from. One CMD53 replaces all of them.

**This is a prerequisite for RX aggregation, not an optional extra**: deciding
how many consecutive slots to read requires every candidate slot's `RD_LEN`,
and fetching those with CMD52 pairs would cost 2 CMD52 per aggregated slot and
eat the win. NXP gets them free from the block read.

**It is not a cache, and that matters.** W11's regression came from a *stale*
bitmap view that let the host burn credits and then dry-wait. Here every
service pass still issues a real read of the real registers; only the
transport changes. The accidental pacing property W11 discovered is preserved
by construction.

### 1b. The aggregated CMD53 address

Both directions, SD8978 arm (`wifi-sdio.c:2355-2359` TX, `2931-2935` RX):

```c
port_count = ports - 1;                       /* ports = slot count */
cmd53_port = (ioport | SDIO_MPA_ADDR_BASE | (port_count << 8)) + start_port;
```

with `SDIO_MPA_ADDR_BASE` = 0x1000 (`mlan_sdio.h:31`) and `ioport` = `MEM_PORT`
= 0x10000. A single-slot transfer keeps the plain `ioport + port` form. So the
address is `0x11000 | (port_count << 8) | start_port`, always inside the 17-bit
CMD53 address field.

### 1c. RX: how many slots, and how the buffer is split

`wlan_get_rd_port()` (`wifi-sdio.c:2742`):

* walk **from `curr_rd_port`, in ring order**, while that slot's rd-bitmap bit
  is set — never "lowest set bit";
* per slot `rx_len = RD_LEN[slot]` (unpadded), `rx_blocks = ceil(rx_len/256)`,
  and the slot contributes `rx_blocks * 256` to the aggregate;
* clear each consumed bit, `curr_rd_port++ % MAX_PORT`, `ports++`;
* **stop** on: `INBUF_SIZE` would overflow, `pkt_cnt == SDIO_MP_AGGR_DEF_PKT_LIMIT_MAX`
  (16), or the span condition
  `(curr_rd_port - start_port) >= (mp_end_port >> 1)` (16) including its wrap form;
* if `ports > 1`, one CMD53 read of `rxblocks` blocks at the aggregated address.

Split host-side (`wifi-sdio.c:3264-3288`): walk the buffer, at each step take
`size = SDIOPkt.size`, round **up to a 256-byte multiple**, deliver
`SDIOPkt.size` bytes, advance by the rounded size, until `total_size >=
datalen`. A zero size ends the walk.

### 1d. TX: accumulate, then one write

`wlan_xmit_wmm_pkt()` / `wifi_tx_data()` (`wifi-sdio.c:2384-2462`, `2333`):

* `wlan_get_wr_port_data()` takes **our own ring port** `txportno` if its
  wr-bitmap bit is set, clears the bit, advances the ring — this is the same
  ring discipline `sendDataFrame` already implements;
* each frame is padded to `tx_blocks * 256` and appended at `buf_block_len`;
* `start_port` is the first port of the batch, `ports`/`pkt_cnt` count it;
* `wlan_flush_wmm_pkt()` issues one CMD53 write of the whole buffer at the
  aggregated address.

`MP_TX_AGGR_*` in `mlan_sdio.h` gives the flush conditions in the mlan (Linux)
form: buffer has no room, `pkt_cnt == pkt_aggr_limit`, or the port span wrapped
past the limit. `SDIO_MP_AGGR_DEF_PKT_LIMIT` = 8 with WMM, else 4;
`_MAX` = 16 (`mlan_main_defs.h:36-43`).

**Nothing in NXP's flow ever waits to fill the buffer.** It aggregates only
what the queue already holds and flushes when the queue drains. That is the
property that keeps this from re-creating W11's burst-then-dry-wait, and our
port must keep it: *aggregate what is already offered, never sit on a frame
hoping for a companion*.

---

## 2. What we build

### Phase A — MP register-block read

* `SdioHost::cmd53ReadBytes(fn, addr, incrAddr, dst, bytes)` — byte-mode
  CMD53 (`blockMode = 0`, count = bytes).
  ★ `WTMK_LVL[RD_WML]` resets to **16 words** (RM 32.7.1.19) and the PIO loop
  gates each word on `PRES_STATE[BREN]`; 196 bytes = 49 words is not a multiple
  of 16. The RM says the controller *does* announce a short tail (32.6.2, with
  a worked 4,4,2 example), so a 16-word watermark should be fine — the
  byte-mode path sets `RD_WML = 1` anyway as belt and braces, at no cost to a
  loop that already polls per word, and restores it so the proven 256-byte
  block path is untouched. (Stated this way round because an earlier draft
  asserted the opposite mechanism, which the RM contradicts.)
* `Iw416::readMpRegs()` — one `cmd53ReadBytes(1, 0, false, m_mpRegs, 196)`,
  filling a snapshot: `intStatus`, `rdBitmap`, `wrBitmap`, `rdLen[32]`,
  `cmdRdLen`. `intStatus` is accumulated into `m_intPending` under the existing
  rule (masked to the serviced bits), because this read **consumes** the
  clear-on-read register exactly like the CMD52 did.
* `serviceLink()` takes its status, bitmap and lengths from the snapshot;
  `sendDataFrame()`'s wr-bitmap wait uses it too.
* New counters `cmd53RegsTx()/cmd53RegsSvc()`, deliberately **separate** from
  the data-CMD53 counters so "data CMD53s per frame" stays the clean
  aggregation metric — and `busCommands()` as the sum of every per-frame term.
  ★ That sum has to name every term **by member**: W16 split `m_cmd53Count`
  into `m_cmd53Rx`/`m_cmd53Tx` and the sum kept adding the old member, which
  no longer had an increment site, so it silently stopped counting data CMD53s
  — the one term aggregation exists to move — while still compiling and still
  looking like a total. Caught in review, not by a gate.
* QEMU: serve a byte-mode fn1 read at address 0 from the register file, with
  `HOST_INT_STATUS`'s clear-on-read applying exactly as on the CMD52 path.

### Phase B — RX aggregation

`readRingBatch()` is now **the** ring read: walk consecutive occupied slots
from `m_rxPort` using the snapshot's `rdLen[]`, cap at a packet limit and a
buffer limit, one CMD53 at the aggregated address, advance `m_rxPort` by the
slot count. `readRingPacket()` is a thin wrapper over the `maxSlots == 1` case,
so the aggregating and non-aggregating paths are the same code with a different
bound — one ring model, one resync, one positive-length check.

★ **The split walks by the card's per-slot `RD_LEN`, not by each packet's own
size field.** Those lengths are what sized the transfer, so they are the only
values guaranteed to land on the boundaries the card used. Stepping by the
payload's self-report makes every packet's position depend on the one before
it: one slot where the two round differently desynchronises the rest of the
batch and silently drops frames whose ring slots have *already* been consumed.
A packet whose size disagrees with its slot is skipped and counted
(`rxSplitMismatch()`); the ones behind it still arrive.

Invariants kept, checked explicitly rather than assumed:

* the **positive** RD_LEN test stays — a set bitmap bit with `RD_LEN == 0`
  ends the batch (it is not evidence of a packet; W13);
* ring order stays — the batch starts at `m_rxPort`, never at "lowest set
  bit"; the resync path is unchanged and still owned by one place;
* the W12/W13 **safety net** keeps its quiet-pass trigger and its ~64 ms
  cadence. Aggregation changes the *pass* structure, so this is re-derived,
  not inherited (W15 nearly killed the net silently).

### Phase C — TX aggregation

`sendDataFrame()` appends into an aggregation buffer and returns; the bus
write happens in `flushTx()`. Flush on: packet limit, buffer would overflow,
ring wrap past the batch start, **at both ends of every `iw416NetifPoll()`**,
and before any command-port traffic (after that path's PS wake gate, so the
batch is never pushed at a sleeping card). The wr-bitmap wait flushes rather
than blocking with frames held. A frame is never held waiting for a companion.

★ **The netif is what makes the feature safe, and it had to be written**: the
driver ships TX aggregation OFF because `sendDataFrame()` then means *queued*,
and a caller that sends and blocks without flushing waits forever. The first
draft of this work enabled nothing and added no flush caller — review caught
that the feature as shipped was unusable.

### Phase A.5 — the register port's own safety net (added after review)

The design above had one assumption with no measurement behind it (§3.1) and
**no way to notice it was wrong**. Register 0 is `HOST_POWER_UP`, which reads
back 0, so a card that obeys OP Code 0 literally returns 196 zero bytes — an
all-zero snapshot that is byte-identical to a healthy idle card. RX would go
permanently silent (the safety net sees an empty bitmap and never fires), TX
would time out on an empty wr-bitmap, and the read would still be eating the
card's clear-on-read interrupt status underneath. Every health counter would
read clean.

So: the **first** snapshot of each firmware life is checked against
`CARD_STATUS` (0x5C), which sits inside NXP's 196-byte window and which
`begin()` has already read by CMD52 — a comparison between the two *transports*
rather than against a constant this driver invented. On a mismatch the register
port is abandoned for the firmware's life and every caller falls back to the
pre-W16 CMD52 reads (`readMpRegsCmd52`, filling the same snapshot struct, so
aggregation still works — only the transport degrades). `mpRegsUsable()` and
`mpRegsRejected()` say so out loud.

The QEMU model implements the alternative reading as a deliberate hostile mode
(`-global iw416-sdio.reg-port-literal=on`), so the detection and the fallback
are both gated rather than argued.

### Phase D — gates (QEMU), then Phase E — silicon

New `run_qemu_aggr.sh` on `networking/m2_rx_demo`: burst-inject N frames onto
consecutive slots, then assert **both** that every frame arrived in order and
that `c53` (data CMD53s) is **less than** the frame count — a driver that does
not aggregate cannot satisfy the second, whatever else it does. Same shape for
TX using `tx-loopback`. Each gate demonstrated failing against a deliberately
non-aggregating driver, with the red output kept in its header.

The model needs an `inject-burst` control so several slots are occupied at
once; a card taking a burst off the air does fill consecutive slots, so this
is a scheduler control, not a change to the contract.

---

## 2b. Measured in QEMU (2026-08-20)

| gate | result |
|---|---|
| `m2_rx_demo[rxaggr]` | 8 uploads burst onto slots 0–7 → **1 RX data CMD53**, `rxaggr=1/8`, all 8 in order. Baseline before W16: 8 CMD53 + 56 CMD52. |
| `m2_rx_demo[txaggr]` | 6 frames staged → **1 TX data CMD53**, `txaggr=1/6`; the card split the run and returned all 6 on their own slots, read back in 1 aggregated CMD53. |
| `m2_rx_demo[regfallback]` | with `reg-port-literal=on`: `mpregs=0` after exactly 1 register read, all 8 frames still delivered over CMD52. |
| `m2_rx_demo[irq]` | service polls per frame 240 → 5 (was 9.6× in W15, now ~45× — the safety net's 4 CMD52 per check became 1 register read). |

Each of the three new gates was **demonstrated to fail**: `AGGR_PKT_LIMIT` set
to 1 reds both aggregation gates while every delivery assertion still passes,
and disabling the snapshot sanity check reds the fallback gate with the exact
silent-deaf signature it exists to catch (`frames=0`, every health counter
clean, only `rd_bitmap=0xFF` from the CMD52 diagnostic to contradict it).

## 3. Where this could be lying

1. **OP Code 0 on a 196-byte register read.** NXP reads the register block
   with the address held *fixed* (`flags = 0`), which for an ordinary SDIO
   function would re-read one register 196 times. The SD8978's register port
   evidently streams the register file instead. This is taken from NXP's
   source, not from a capture on our board.
   ★ **The failure is invisible, not obvious** — an earlier draft of this line
   claimed the snapshot would be "obviously wrong (bitmaps and lengths all
   equal)". Register 0 reads back 0, so all-equal means ALL ZERO, which is what
   a healthy idle card looks like. That is what Phase A.5 exists for, and it is
   gated (`[regfallback]`). If silicon takes the literal reading, the driver
   says `mpregs=0` and keeps running on CMD52; trying OP Code 1 is then the
   experiment, not a guess.
2. **`MAX_MP_REGS` = 196 and the tail registers.** We read exactly NXP's 196
   bytes and nothing beyond, so no register outside their window is touched.
3. **RD_LEN unpadded.** Already flagged as inferred (model NOTE 11). RX
   aggregation now *depends* on that inference for its split points, so a
   wrong assumption here shows up as a mis-split batch rather than a single
   bad length.
4. **The aggregated address encoding** is from NXP's source and has never been
   observed on our wire. A wrong `port_count` field would present as the card
   returning fewer/more slots than asked.
5. **TX aggregation vs. the firmware's credit cadence** (W11's lesson). The
   flush rules above are designed to preserve pacing, but only silicon can
   confirm it; the QEMU model's `tx-free-batch` reproduces the *shape* of the
   lazy free, not its trigger (model NOTE 9).
