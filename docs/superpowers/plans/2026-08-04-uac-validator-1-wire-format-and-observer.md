# UAC Host Validator — Plan 1: Wire Format and Observer

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Define the state-block wire format, implement the `lib_xua` observer that emits it, and prove on silicon that a real capture parses.

**Architecture:** The device reduces; it never judges. Reductions accumulate in shared globals on `XUA_XUD_TILE_NUM` (decoupler, EP buffer and EP0 all live there) and are emitted as one atomic 36-word state block at the decoupler's existing 100 Hz cadence. A Python module in `evkb` owns the format definition, a synthetic generator, and a reader — so Plan 2 codes against a stable `Block` type regardless of which probe mechanism the spike selects.

**Tech Stack:** XC (XMOS XTC 15.3.1), xscope; Python 3 (stdlib only) for the format module.

**Design spec:** `docs/superpowers/specs/2026-08-04-uac-host-validator-design.md`

---

## Prerequisites

- XTC tools: `set +u; source /Applications/XMOS_XTC_15.3.1/SetEnv.sh`
- XTAG-3 adapter id `3LajHPG5`
- `~/Development/xmos/lib_xua` on branch `instrumentation/decouple-xscope-probes` at `26d13bab`

**Bench safety rules — violating these has cost real time:**
- Never `kill -9` a live xscope collector. It wedges the device's xscope until a reflash. End with SIGINT.
- SIGINT resets the xcore, so the device drops off USB. That is normal, not a finding.
- Never hot-reload firmware with `xrun` while a host is streaming — it creates a startup race where the USB stack answers standard requests before the audio task answers class requests. Always `xrun` first, then start the host.

---

## File Structure

| File | Responsibility |
|---|---|
| `docs/uac-validator-wire-format.md` | Normative field-by-field definition of the state block. The contract between observer and judge. |
| `tools/uacvalidate/__init__.py` | Package marker. |
| `tools/uacvalidate/trace.py` | `Block` dataclass, `read_blocks()` reader, `synth_vcd()` generator. Owns both emission formats. |
| `tools/uacvalidate/test_trace.py` | Tests for reader and generator. |
| `lib_xua/src/core/buffer/decouple/decouple.xc` *(xmos repo)* | Packet, size-histogram, byte-lane and pattern reductions; block emission. |
| `lib_xua/src/core/buffer/ep/ep_buffer.xc` *(xmos repo)* | Feedback poll count and feedback value. |
| `lib_xua/src/core/endpoint0/xua_endpoint0.c` *(xmos repo)* | Alt setting, alt transitions, class-request bitmap, host-active. |
| `lib_xua/src/core/main.xc` *(xmos repo)* | `xscope_register` — append only. |
| `examples/usb/usb_audio_graph_test/lib_xua-uac-validator.patch` | Regenerated single patch for the whole observer branch. |

---

## The state block

36 words, little-endian uint32, emitted at 100 Hz. This layout is normative; every task below refers to it by word index.

| Word | Name | Meaning |
|---|---|---|
| 0 | `magic` | `0x55414356` (`'UACV'`) |
| 1 | `version` | `1` |
| 2 | `pkt_count` | OUT packets accepted |
| 3 | `pkt_short_discarded` | packets shorter than one audio frame |
| 4 | `pkt_not_multiple` | packets not a whole number of frames |
| 5 | `or_acc` | OR of every sample word |
| 6 | `and_acc` | AND of every sample word |
| 7 | `nonsilent_frames` | frames where any channel was non-zero |
| 8 | `fb_poll_count` | feedback IN completions |
| 9 | `fb_value` | current `fb_clocks[0]` |
| 10 | `alt_out` | current `g_curStreamAlt_Out` |
| 11 | `alt_transitions` | count of alt changes |
| 12 | `class_req_bitmap` | bit0 clock SET_CUR, bit1 clock GET_CUR, bit2 SET_INTERFACE |
| 13 | `host_active` | 0 or 1 |
| 14 | `pat_err_count` | cooperative mode only |
| 15 | `pat_resync_count` | cooperative mode only |
| 16 | `pat_first_err_idx` | cooperative mode only |
| 17 | `pat_first_expected` | cooperative mode only |
| 18 | `pat_first_actual` | cooperative mode only |
| 19 | `size_hist_overflow` | distinct packet sizes seen beyond the 8 slots |
| 20–27 | `size_hist_size[0..7]` | observed packet size in bytes, 0 = slot unused |
| 28–35 | `size_hist_count[0..7]` | count for the matching slot |

All counters are free-running uint32 and may wrap. The reader does not unwrap; Plan 2's rules handle wrap.

---

## Task 1: The emission-format spike

Decides whether the block ships as one `xscope_bytes` probe or 36 scalar probes. No code from later tasks depends on the outcome — `trace.py` hides it — but the observer's emit function does.

**Files:**
- Create: `~/Development/xmos/lib_xua/spike_bytes_probe.md` (scratch notes, not committed)

- [ ] **Step 1: Create the observer branch**

```bash
cd ~/Development/xmos/lib_xua
git checkout instrumentation/decouple-xscope-probes
git checkout -b instrumentation/uac-host-validator
git log --oneline -1
```

Expected: `26d13bab decouple: fix the fill probe, count runtime dry-outs`

- [ ] **Step 2: Add a throwaway bytes probe**

In `lib_xua/src/core/main.xc`, inside `xscope_user_init()`, change the `xscope_register` call to register a fifth probe and emit a known pattern. Replace the existing call with:

```c
    xscope_register(5,
                    XSCOPE_CONTINUOUS, "out_underflow", XSCOPE_UINT, "count",
                    XSCOPE_CONTINUOUS, "out_overflow",  XSCOPE_UINT, "count",
                    XSCOPE_CONTINUOUS, "out_fifo_fill", XSCOPE_UINT, "bytes",
                    XSCOPE_CONTINUOUS, "out_dryout",    XSCOPE_UINT, "count",
                    XSCOPE_CONTINUOUS, "spike_block",   XSCOPE_UINT, "bytes");
```

In `lib_xua/src/core/buffer/decouple/decouple.xc`, in the same `if(++g_fillProbeDiv >= 10)` body that already emits the fill probe (around line 1085 after the existing patch), add after `XUA_PROBE_DRYOUT();`:

```c
#ifdef XSCOPE
                {
                    unsigned char spikeBuf[16];
                    for(int i = 0; i < 16; i++)
                        spikeBuf[i] = 0xA0 + i;
                    xscope_bytes(4, 16, spikeBuf);
                }
#endif
```

- [ ] **Step 3: Build the witness**

```bash
set +u; source /Applications/XMOS_XTC_15.3.1/SetEnv.sh
cd ~/Development/xmos/sw_usb_audio/app_usb_aud_xk_216_mc
cmake -B build -DXSCOPE=1 && cmake --build build --target app_usb_aud_xk_216_mc_2AMi8o8xxxxxx
```

Expected: an `.xe` under `bin/2AMi8o8xxxxxx/`. Note: `-DXSCOPE` is required; `-fxscope` alone defines only `_XSCOPE_PROBES_INCLUDE_FILE` and every probe silently compiles to a no-op.

- [ ] **Step 4: Capture 10 seconds with no host attached**

```bash
xrun --adapter-id 3LajHPG5 --xscope-file spike bin/2AMi8o8xxxxxx/app_usb_aud_xk_216_mc_2AMi8o8xxxxxx.xe
```

Let it run about 10 seconds, then **Ctrl-C** (SIGINT — never `kill -9`).

- [ ] **Step 5: Inspect the VCD**

```bash
grep -c "spike_block" spike.vcd; head -40 spike.vcd
```

Record in `spike_bytes_probe.md`:
- Does a `$var` for `spike_block` appear in the header?
- Do value-change records for it appear in the body?
- If so, in what encoding — one value per byte, a packed vector, or a string?

- [ ] **Step 6: Record the decision**

Write the outcome into `docs/uac-validator-wire-format.md` (Task 2) as either:
- **RECORD format selected** — `xscope_bytes` on probe id 4, block emitted whole; or
- **SCALAR format selected** — 36 scalar probes at ids 4..39, one per word.

- [ ] **Step 7: Revert the spike**

```bash
cd ~/Development/xmos/lib_xua && git checkout -- lib_xua/src/core/main.xc lib_xua/src/core/buffer/decouple/decouple.xc
git status --short
```

Expected: no modified files. The spike is a measurement, not a commit.

---

## Task 2: Write the wire-format document

**Files:**
- Create: `docs/uac-validator-wire-format.md`

- [ ] **Step 1: Write the document**

Create `docs/uac-validator-wire-format.md` containing, in order:

1. A one-paragraph statement that this is the normative contract between the `lib_xua` observer and the `uacvalidate` judge, and that the observer computes no verdicts.
2. The 36-word table exactly as given in this plan's "The state block" section above — copy it verbatim.
3. The emission format selected by the Task 1 spike, stated as the selected one plus a note recording that the other was considered and why it lost.
4. The emission cadence: 100 Hz, from the decoupler's existing `g_fillProbeDiv >= 10` divider, so one block per ten OUT packets.
5. A statement that probe ids 0–3 are unchanged and that new ids are append-only.
6. A statement that all counters are free-running uint32 and may wrap, and that unwrapping is the judge's job.

- [ ] **Step 2: Commit**

```bash
cd ~/Development/rt1170/evkb
git add docs/uac-validator-wire-format.md
git commit -m "docs: UAC validator state-block wire format

The normative contract between the lib_xua observer and the Python judge.
Emission format selected by bench spike; the rejected alternative is
recorded with its reason so the choice is auditable."
```

---

## Task 3: `Block` and the synthetic generator

Plan 2 depends only on this. Written before the observer so the judge is never blocked on hardware.

**Files:**
- Create: `tools/uacvalidate/__init__.py`
- Create: `tools/uacvalidate/trace.py`
- Test: `tools/uacvalidate/test_trace.py`

- [ ] **Step 1: Write the failing test**

Create `tools/uacvalidate/test_trace.py`:

```python
import os
import tempfile
import unittest

from trace import Block, synth_vcd, read_blocks


class TestSynthRoundTrip(unittest.TestCase):
    def test_single_block_round_trips(self):
        b = Block(pkt_count=8000, or_acc=0xFFFFFF00, and_acc=0x00000000,
                  nonsilent_frames=44100, fb_poll_count=63, fb_value=0x000B0000,
                  alt_out=1)
        with tempfile.TemporaryDirectory() as d:
            path = os.path.join(d, "t.vcd")
            synth_vcd(path, [(0.0, b)])
            got = read_blocks(path)
        self.assertEqual(len(got), 1)
        t, blk = got[0]
        self.assertEqual(t, 0.0)
        self.assertEqual(blk.pkt_count, 8000)
        self.assertEqual(blk.or_acc, 0xFFFFFF00)
        self.assertEqual(blk.nonsilent_frames, 44100)
        self.assertEqual(blk.fb_poll_count, 63)
        self.assertEqual(blk.alt_out, 1)

    def test_defaults_are_zero(self):
        b = Block()
        self.assertEqual(b.pkt_count, 0)
        self.assertEqual(b.size_hist_size, [0] * 8)
        self.assertEqual(b.size_hist_count, [0] * 8)

    def test_sizes_helper_pairs_nonzero_slots(self):
        b = Block(size_hist_size=[192, 160, 0, 0, 0, 0, 0, 0],
                  size_hist_count=[7000, 1000, 0, 0, 0, 0, 0, 0])
        self.assertEqual(b.sizes(), {192: 7000, 160: 1000})

    def test_many_blocks_keep_order_and_time(self):
        blocks = [(i * 0.01, Block(pkt_count=i * 10)) for i in range(100)]
        with tempfile.TemporaryDirectory() as d:
            path = os.path.join(d, "t.vcd")
            synth_vcd(path, blocks)
            got = read_blocks(path)
        self.assertEqual(len(got), 100)
        self.assertEqual(got[50][1].pkt_count, 500)
        self.assertAlmostEqual(got[50][0], 0.5, places=6)


if __name__ == "__main__":
    unittest.main()
```

- [ ] **Step 2: Run it to verify it fails**

```bash
cd ~/Development/rt1170/evkb/tools/uacvalidate && python3 -m unittest test_trace -v
```

Expected: FAIL — `ModuleNotFoundError: No module named 'trace'` or `ImportError: cannot import name 'Block'`.

- [ ] **Step 3: Write the implementation**

Create `tools/uacvalidate/__init__.py` as an empty file.

Create `tools/uacvalidate/trace.py`:

```python
"""State-block wire format: the contract between the lib_xua observer and
the judge.

The observer emits 36 little-endian uint32 words at 100 Hz. This module owns
the layout, a reader that recovers blocks from an xscope VCD, and a generator
that writes synthetic VCDs so the judge can be tested without hardware.

Field meanings are normative in docs/uac-validator-wire-format.md.
"""
from dataclasses import dataclass, field, fields

MAGIC = 0x55414356  # 'UACV'
VERSION = 1
BLOCK_WORDS = 36

# Word index -> attribute name. Words 20..35 are the two histogram arrays and
# are handled separately.
SCALAR_LAYOUT = {
    0: "magic",
    1: "version",
    2: "pkt_count",
    3: "pkt_short_discarded",
    4: "pkt_not_multiple",
    5: "or_acc",
    6: "and_acc",
    7: "nonsilent_frames",
    8: "fb_poll_count",
    9: "fb_value",
    10: "alt_out",
    11: "alt_transitions",
    12: "class_req_bitmap",
    13: "host_active",
    14: "pat_err_count",
    15: "pat_resync_count",
    16: "pat_first_err_idx",
    17: "pat_first_expected",
    18: "pat_first_actual",
    19: "size_hist_overflow",
}
HIST_SIZE_BASE = 20
HIST_COUNT_BASE = 28
HIST_SLOTS = 8


@dataclass
class Block:
    magic: int = MAGIC
    version: int = VERSION
    pkt_count: int = 0
    pkt_short_discarded: int = 0
    pkt_not_multiple: int = 0
    or_acc: int = 0
    and_acc: int = 0xFFFFFFFF
    nonsilent_frames: int = 0
    fb_poll_count: int = 0
    fb_value: int = 0
    alt_out: int = 0
    alt_transitions: int = 0
    class_req_bitmap: int = 0
    host_active: int = 0
    pat_err_count: int = 0
    pat_resync_count: int = 0
    pat_first_err_idx: int = 0
    pat_first_expected: int = 0
    pat_first_actual: int = 0
    size_hist_overflow: int = 0
    size_hist_size: list = field(default_factory=lambda: [0] * HIST_SLOTS)
    size_hist_count: list = field(default_factory=lambda: [0] * HIST_SLOTS)

    def sizes(self):
        """Packet size -> count, for slots actually in use."""
        return {s: c for s, c in zip(self.size_hist_size, self.size_hist_count) if s}

    def to_words(self):
        w = [0] * BLOCK_WORDS
        for idx, name in SCALAR_LAYOUT.items():
            w[idx] = getattr(self, name) & 0xFFFFFFFF
        for i in range(HIST_SLOTS):
            w[HIST_SIZE_BASE + i] = self.size_hist_size[i] & 0xFFFFFFFF
            w[HIST_COUNT_BASE + i] = self.size_hist_count[i] & 0xFFFFFFFF
        return w

    @classmethod
    def from_words(cls, w):
        if len(w) != BLOCK_WORDS:
            raise ValueError(f"expected {BLOCK_WORDS} words, got {len(w)}")
        kwargs = {name: w[idx] for idx, name in SCALAR_LAYOUT.items()}
        kwargs["size_hist_size"] = list(w[HIST_SIZE_BASE:HIST_SIZE_BASE + HIST_SLOTS])
        kwargs["size_hist_count"] = list(w[HIST_COUNT_BASE:HIST_COUNT_BASE + HIST_SLOTS])
        return cls(**kwargs)


# Scalar emission: word i rides probe id 4+i. VCD identifiers are assigned in
# probe-id order by xrun, so the reader keys off the declared $var names.
PROBE_BASE = 4


def _probe_name(word_index):
    return f"uacv_w{word_index:02d}"


def synth_vcd(path, timed_blocks, timescale_ps=1000000):
    """Write a synthetic xscope-shaped VCD.

    timed_blocks: iterable of (time_seconds, Block).
    """
    ids = {}
    for i in range(BLOCK_WORDS):
        # VCD identifier codes: printable ASCII from '!' upward.
        ids[i] = chr(33 + i)
    with open(path, "w") as f:
        f.write("$timescale\n  1 us\n$end\n")
        f.write("$scope module xscope $end\n")
        for i in range(BLOCK_WORDS):
            f.write(f"$var wire 32 {ids[i]} {_probe_name(i)} $end\n")
        f.write("$upscope $end\n")
        f.write("$enddefinitions $end\n")
        prev = None
        for t, blk in timed_blocks:
            ticks = int(round(t * 1e6))
            f.write(f"#{ticks}\n")
            words = blk.to_words()
            for i, val in enumerate(words):
                if prev is None or prev[i] != val:
                    f.write(f"b{val:b} {ids[i]}\n")
            prev = words


def read_blocks(path):
    """Recover (time_seconds, Block) pairs from an xscope VCD.

    A block is emitted as a burst of word updates at one timestamp. Words that
    did not change carry forward, matching VCD semantics.
    """
    tick = None
    id_to_word = {}
    current = [0] * BLOCK_WORDS
    out = []
    pending_time = None
    saw_any = False

    with open(path) as f:
        lines = f.read().split("\n")

    i = 0
    while i < len(lines):
        ln = lines[i].strip()
        if ln == "$timescale":
            parts = lines[i + 1].strip().split()
            unit = {"s": 1.0, "ms": 1e-3, "us": 1e-6, "ns": 1e-9, "ps": 1e-12}[parts[1]]
            tick = float(parts[0]) * unit
        elif ln.startswith("$var"):
            parts = ln.split()
            ident, name = parts[3], parts[4]
            if name.startswith("uacv_w"):
                id_to_word[ident] = int(name[len("uacv_w"):])
        elif ln == "$enddefinitions $end":
            i += 1
            break
        i += 1

    if tick is None:
        raise ValueError("no $timescale in VCD")

    for ln in lines[i:]:
        ln = ln.strip()
        if not ln:
            continue
        if ln.startswith("#"):
            if pending_time is not None and saw_any:
                out.append((pending_time, Block.from_words(list(current))))
            pending_time = int(ln[1:]) * tick
            continue
        if ln.startswith("b"):
            val_str, ident = ln[1:].split(" ", 1)
            ident = ident.strip()
            if ident in id_to_word:
                current[id_to_word[ident]] = int(val_str, 2)
                saw_any = True

    if pending_time is not None and saw_any:
        out.append((pending_time, Block.from_words(list(current))))

    return out


def count_missing_marks(path):
    """Count xscope's own lost-sample markers in a VCD.

    xscope publishes these as a signal named Missing_Data (probe id 255). Any
    non-zero count means the capture dropped data, so every counter in it is a
    lower bound and no verdict drawn from it can be trusted.
    """
    ident = None
    marks = 0
    with open(path) as f:
        for ln in f:
            ln = ln.strip()
            if ln.startswith("$var") and "Missing_Data" in ln:
                ident = ln.split()[3]
            elif ln == "$enddefinitions $end" and ident is None:
                return 0
            elif ident and ln.endswith(" " + ident) and ln[0] in "b01":
                marks += 1
    return marks
```

- [ ] **Step 4: Run tests to verify they pass**

```bash
cd ~/Development/rt1170/evkb/tools/uacvalidate && python3 -m unittest test_trace -v
```

Expected: 4 tests, all PASS.

- [ ] **Step 5: Commit**

```bash
cd ~/Development/rt1170/evkb
git add tools/uacvalidate/
git commit -m "uacvalidate: state-block format, reader and synthetic generator

The generator exists so the judge can be built and tested with no hardware
attached, and so both sides of the contract are exercised by the same
layout table rather than by two hand-written copies that can drift."
```

---

## Task 4: Observer — packet counters and size histogram

**Files:**
- Modify: `lib_xua/src/core/buffer/decouple/decouple.xc` *(xmos repo, observer branch)*

- [ ] **Step 1: Add the counter globals**

In `decouple.xc`, immediately after the existing `unsigned g_outDryoutCount = 0;` declaration, add:

```c
/* UAC host validator reductions. Single writer (the decoupler), read only by
 * the block emission below. No verdicts are computed here -- the device
 * reduces, the judge decides. See docs/uac-validator-wire-format.md in the
 * evkb repo for the normative field layout. */
unsigned g_uacvPktCount = 0;
unsigned g_uacvPktShortDiscarded = 0;
unsigned g_uacvPktNotMultiple = 0;
unsigned g_uacvHistSize[8] = {0,0,0,0,0,0,0,0};
unsigned g_uacvHistCount[8] = {0,0,0,0,0,0,0,0};
unsigned g_uacvHistOverflow = 0;
```

- [ ] **Step 2: Add the histogram helper**

In `decouple.xc`, immediately before `void XUA_Buffer_Decouple(`, add:

```c
/* Record one observed packet size. Eight slots is generous: a conformant host
 * uses at most two distinct sizes (the fractional-sample floor and ceiling).
 * A host needing more than eight is itself a finding, which is why the
 * overflow is counted rather than silently folded into a neighbour. */
static inline void uacvRecordSize(unsigned size)
{
    for(int i = 0; i < 8; i++)
    {
        if(g_uacvHistSize[i] == size)
        {
            g_uacvHistCount[i]++;
            return;
        }
        if(g_uacvHistSize[i] == 0)
        {
            g_uacvHistSize[i] = size;
            g_uacvHistCount[i] = 1;
            return;
        }
    }
    g_uacvHistOverflow++;
}
```

- [ ] **Step 3: Instrument the packet-accepted path**

In `decouple.xc`, the block at line 1040 currently reads:

```c
            /* Ignore bad small packets */
            if((datalength >= (g_numUsbChan_Out * g_curSubSlot_Out)) && (released_buffer == aud_from_host_wrptr))
            {
```

Replace that `if` and add an `else`, so the region becomes:

```c
            /* Ignore bad small packets */
            if((datalength >= (g_numUsbChan_Out * g_curSubSlot_Out)) && (released_buffer == aud_from_host_wrptr))
            {
                /* Validator: this packet was accepted. Record its size, and
                 * whether it is a whole number of audio frames -- lib_xua
                 * tolerates a non-multiple silently (see the tail handling in
                 * handle_audio_request), which is exactly the kind of quiet
                 * tolerance this tool exists to make audible. */
                g_uacvPktCount++;
                uacvRecordSize((unsigned)datalength);
                if(datalength % (g_numUsbChan_Out * g_curSubSlot_Out))
                    g_uacvPktNotMultiple++;
```

and add, immediately after the closing brace of that `if` block (after the `SET_SHARED_GLOBAL(g_aud_from_host_wrptr, aud_from_host_wrptr);` line and its closing `}`):

```c
            else if(datalength < (g_numUsbChan_Out * g_curSubSlot_Out))
            {
                /* Validator: shorter than one audio frame. lib_xua discards
                 * these silently; count them so the judge can report it. */
                g_uacvPktShortDiscarded++;
            }
```

- [ ] **Step 4: Build**

```bash
set +u; source /Applications/XMOS_XTC_15.3.1/SetEnv.sh
cd ~/Development/xmos/sw_usb_audio/app_usb_aud_xk_216_mc
cmake --build build --target app_usb_aud_xk_216_mc_2AMi8o8xxxxxx
```

Expected: builds clean, no warnings about the new symbols.

- [ ] **Step 5: Commit**

```bash
cd ~/Development/xmos/lib_xua
git add lib_xua/src/core/buffer/decouple/decouple.xc
git commit -m "decouple: validator packet counters and size histogram

Counts accepted packets, their sizes, packets shorter than one audio frame,
and packets that are not a whole number of frames. The latter two are paths
lib_xua already handles defensively and never reports -- decouple.xc:1040
discards short packets silently, and the tail handling in
handle_audio_request carries a comment naming a bad driver. Reduction only:
no thresholds, no verdicts."
```

---

## Task 5: Observer — byte-lane accumulators and the vacuity witness

**Files:**
- Modify: `lib_xua/src/core/buffer/decouple/decouple.xc` *(xmos repo, observer branch)*

- [ ] **Step 1: Add the globals**

After the counters from Task 4, add:

```c
/* Byte-lane occupancy. or_acc tells us which bit positions were EVER set;
 * and_acc which were ALWAYS set. Together they identify subslot
 * justification without knowing anything about the audio content:
 *   (or_acc & 0xFF) == 0            -> low byte never used -> left-justified
 *   (or_acc >> 24) == (and_acc >> 24) -> top byte constant -> right-justified
 *
 * nonsilent_frames is the vacuity witness. On a silent stream or_acc stays 0
 * and the justification test would pass for the wrong reason, declaring a
 * right-justifying host conformant. The judge refuses to rule without enough
 * non-silent frames. */
unsigned g_uacvOrAcc = 0;
unsigned g_uacvAndAcc = 0xFFFFFFFF;
unsigned g_uacvNonsilentFrames = 0;
unsigned g_uacvFrameNonzero = 0;
```

- [ ] **Step 2: Instrument the 24-in-4 path**

`decouple.xc:190` defines `SendSamples4`, which funnels every 24-in-4 channel count through `_send_sample_4()`. Locate `_send_sample_4`:

```bash
cd ~/Development/xmos/lib_xua && grep -n "_send_sample_4" lib_xua/src/core/buffer/decouple/decouple.xc
```

In the body of `_send_sample_4`, immediately after the sample word is read from the FIFO and before it is sent to `c_mix_out`, add:

```c
    /* Validator: accumulate byte-lane occupancy before anything downstream
     * can normalise it away. This is the ONLY place the raw subslot bytes are
     * visible -- the 3-byte path masks with 0xffffff00, and
     * UserBufferManagement runs on the other tile after unpacking. */
    g_uacvOrAcc |= (unsigned)sample;
    g_uacvAndAcc &= (unsigned)sample;
    if(sample) g_uacvFrameNonzero = 1;
```

Use whatever local name `_send_sample_4` gives the sample word; if it sends directly without a named local, introduce one:

```c
    unsigned s;
    read_via_xc_ptr(s, g_aud_from_host_rdptr);
```

and send `s`.

- [ ] **Step 3: Instrument the 16-bit path**

In the `case 2:` arm of the `switch(g_curSubSlot_Out)` at `decouple.xc:273`, after `sample <<= 16;` add the same three lines:

```c
                    g_uacvOrAcc |= (unsigned)sample;
                    g_uacvAndAcc &= (unsigned)sample;
                    if(sample) g_uacvFrameNonzero = 1;
```

- [ ] **Step 4: Latch the per-frame witness**

`handle_audio_request` emits one audio frame per call. At the end of the function, after the channel loop completes, add:

```c
    /* Validator: one frame's worth of channels has been sent. Count the frame
     * if any channel in it was non-zero. */
    if(g_uacvFrameNonzero)
    {
        g_uacvNonsilentFrames++;
        g_uacvFrameNonzero = 0;
    }
```

- [ ] **Step 5: Build**

```bash
set +u; source /Applications/XMOS_XTC_15.3.1/SetEnv.sh
cd ~/Development/xmos/sw_usb_audio/app_usb_aud_xk_216_mc
cmake --build build --target app_usb_aud_xk_216_mc_2AMi8o8xxxxxx
```

Expected: builds clean.

- [ ] **Step 6: Commit**

```bash
cd ~/Development/xmos/lib_xua
git add lib_xua/src/core/buffer/decouple/decouple.xc
git commit -m "decouple: byte-lane accumulators for subslot justification

Two ALU ops per sample -- 0.7 M ops/s at 8ch x 44.1 kHz against ~62 MIPS
available to this core. Placed inside the decoupler because this is the only
place the raw subslot bytes exist: the 3-byte path masks with 0xffffff00 and
UserBufferManagement runs on the other tile, after unpacking.

nonsilent_frames rides along as the vacuity witness. Without it a silent
stream makes the justification test pass for the wrong reason."
```

---

## Task 6: Observer — feedback poll count and value

**Files:**
- Modify: `lib_xua/src/core/buffer/ep/ep_buffer.xc` *(xmos repo, observer branch)*

- [ ] **Step 1: Declare the globals**

Near the top of `ep_buffer.xc`, after `unsigned g_feedbackValid = 0;` (line 38), add:

```c
/* Validator: how many times the host has actually collected feedback, and
 * what the device is currently asking for. A host that never polls this
 * endpoint is the defect that started this whole investigation, and it is
 * invisible from anywhere except here. */
unsigned g_uacvFbPollCount = 0;
unsigned g_uacvFbValue = 0;
```

- [ ] **Step 2: Count completions**

`ep_buffer.xc:787` is `case XUD_SetData_Select(c_aud_fb, ep_aud_fb, result):`. As the first statement inside that case body, add:

```c
                /* Validator: the host completed an IN on the feedback
                 * endpoint. Counting completions rather than arms is
                 * deliberate -- an armed endpoint the host never reads must
                 * not look like a host that is reading. */
                g_uacvFbPollCount++;
```

- [ ] **Step 3: Publish the feedback value**

At `ep_buffer.xc:692`, inside the `if (usb_speed == XUD_SPEED_HS)` / `else` pair that writes `fb_clocks`, add after both assignments (i.e. after the closing brace of the `else` at line 697):

```c
                            /* Validator: mirror what we are asking the host
                             * for, so the judge can compare it against what
                             * the host actually sent. */
                            g_uacvFbValue = (fb_clocks, unsigned[])[0];
```

- [ ] **Step 4: Build**

```bash
set +u; source /Applications/XMOS_XTC_15.3.1/SetEnv.sh
cd ~/Development/xmos/sw_usb_audio/app_usb_aud_xk_216_mc
cmake --build build --target app_usb_aud_xk_216_mc_2AMi8o8xxxxxx
```

Expected: builds clean.

- [ ] **Step 5: Commit**

```bash
cd ~/Development/xmos/lib_xua
git add lib_xua/src/core/buffer/ep/ep_buffer.xc
git commit -m "ep_buffer: count feedback polls, publish the feedback value

Completions, not arms: an endpoint armed by a device the host never reads
must not be indistinguishable from a host that reads it. Publishing the
value the device is asking for lets the judge compare request against
delivery, which turns a servo-quality problem from an inference into a
measurement."
```

---

## Task 7: Observer — alt setting, transitions, class requests

**Files:**
- Modify: `lib_xua/src/core/endpoint0/xua_endpoint0.c` *(xmos repo, observer branch)*

- [ ] **Step 1: Declare the globals**

After `unsigned g_curStreamAlt_Out = 0;` (line 116), add:

```c
/* Validator: alt-setting state and which class requests the host has ever
 * issued. Alt transitions carry no timestamp of their own -- the state block
 * is emitted at 100 Hz and xscope timestamps each emission, locating a change
 * to within 10 ms. That is ample for the rule this serves, which asks whether
 * packets arrived during alt 0, not exactly when the switch happened. */
unsigned g_uacvAltOut = 0;
unsigned g_uacvAltTransitions = 0;
unsigned g_uacvClassReqBitmap = 0;
unsigned g_uacvHostActive = 0;

#define UACV_REQ_CLOCK_SET_CUR   (1u << 0)
#define UACV_REQ_CLOCK_GET_CUR   (1u << 1)
#define UACV_REQ_SET_INTERFACE   (1u << 2)
```

- [ ] **Step 2: Track alt transitions**

At line 599, inside `if(g_curStreamAlt_Out != newStreamAlt_Out)`, immediately after `g_curStreamAlt_Out = newStreamAlt_Out;` add:

```c
                                        g_uacvAltOut = newStreamAlt_Out;
                                        g_uacvAltTransitions++;
```

- [ ] **Step 3: Record SET_INTERFACE**

At line 581, inside `if(sp.bRequest == USB_SET_INTERFACE)`, as the first statement, add:

```c
                    g_uacvClassReqBitmap |= UACV_REQ_SET_INTERFACE;
```

- [ ] **Step 4: Record clock requests**

Locate where clock-source requests are dispatched:

```bash
cd ~/Development/xmos/lib_xua && grep -n "CUR\|clockUnit\|XUA_CLOCKCMD" lib_xua/src/core/endpoint0/xua_ep0_uacreqs.xc | head -20
```

In the function that handles a Clock Source unit request, at the point where `CUR` is distinguished from `RANGE`, set the corresponding bit:

```c
    /* Validator: record that the host issued this request at all. Set on
     * first arrival and never cleared -- the question is "did the host ever
     * do this", which is what an incomplete configuration sequence needs. */
    if(bRequest == CUR_SET)
        g_uacvClassReqBitmap |= UACV_REQ_CLOCK_SET_CUR;
    else if(bRequest == CUR_GET)
        g_uacvClassReqBitmap |= UACV_REQ_CLOCK_GET_CUR;
```

Adjust the constant names to whatever the surrounding code uses for SET_CUR/GET_CUR, and add `extern unsigned g_uacvClassReqBitmap;` plus the three `UACV_REQ_*` defines to the top of that file if it is a different translation unit.

- [ ] **Step 5: Track host-active**

In `lib_xua/src/core/user/hostactive/hostactive.c`, or wherever `UserHostActive` is invoked from EP0, set the flag. Locate it:

```bash
cd ~/Development/xmos/lib_xua && grep -rn "UserHostActive" lib_xua/src/
```

At each call site, immediately before the call, add:

```c
    g_uacvHostActive = active ? 1 : 0;
```

- [ ] **Step 6: Build**

```bash
set +u; source /Applications/XMOS_XTC_15.3.1/SetEnv.sh
cd ~/Development/xmos/sw_usb_audio/app_usb_aud_xk_216_mc
cmake --build build --target app_usb_aud_xk_216_mc_2AMi8o8xxxxxx
```

Expected: builds clean.

- [ ] **Step 7: Commit**

```bash
cd ~/Development/xmos/lib_xua
git add lib_xua/src/core/endpoint0/ lib_xua/src/core/user/
git commit -m "endpoint0: validator alt state and class-request bitmap

Alt transitions and a set-on-first-arrival bitmap for the three class
requests that matter to a configuration sequence. No timestamps of their
own: the 100 Hz emission cadence locates a change to 10 ms, which is ample
for a rule asking whether packets arrived during alt 0."
```

---

## Task 7b: Observer — cooperative-mode pattern check

Passive mode is the default and does nearly all the work, but R7 needs a
reduction or it can never be more than a permanent SKIP.

**Files:**
- Modify: `lib_xua/src/core/buffer/decouple/decouple.xc` *(xmos repo, observer branch)*

- [ ] **Step 1: Add the pattern state**

Near the other validator globals in `decouple.xc`, add:

```c
/* Cooperative mode: the host plays a known LFSR sequence and we check every
 * sample against it. Enabled at build time -- in passive mode this compiles
 * to nothing, so the default costs zero cycles in the real-time path.
 *
 * The device syncs on first match then free-runs. After 8 consecutive
 * mismatches -- one audio frame at 8 channels -- it resyncs, so a single
 * dropped packet costs one error rather than an unbounded stream of them.
 *
 * A pattern that NEVER syncs is a finding about the host, not a broken tool:
 * it means the playback path is not bit-exact. The judge distinguishes that
 * from "synced then diverged" using the resync count. */
unsigned g_uacvPatErrCount = 0;
unsigned g_uacvPatResyncCount = 0;
unsigned g_uacvPatFirstErrIdx = 0;
unsigned g_uacvPatFirstExpected = 0;
unsigned g_uacvPatFirstActual = 0;

#if (UACV_COOPERATIVE == 1)
#define UACV_LFSR_SEED   0xACE1u
#define UACV_RESYNC_AFTER 8
static unsigned g_uacvLfsr = UACV_LFSR_SEED;
static unsigned g_uacvPatSynced = 0;
static unsigned g_uacvPatRun = 0;      /* consecutive mismatches */
static unsigned g_uacvSampleIdx = 0;

/* Galois LFSR, 32-bit, left-justified into the subslot so it survives the
 * same packing the audio does. The host generates the identical sequence. */
static inline unsigned uacvNextExpected(void)
{
    unsigned lsb = g_uacvLfsr & 1u;
    g_uacvLfsr >>= 1;
    if(lsb) g_uacvLfsr ^= 0xB4BCD35Cu;
    return g_uacvLfsr << 8;
}

static inline void uacvCheckSample(unsigned s)
{
    unsigned expected;
    g_uacvSampleIdx++;
    if(!g_uacvPatSynced)
    {
        /* Not yet locked: look for the seed value to appear. */
        if((s & 0xFFFFFF00u) == (UACV_LFSR_SEED << 8))
        {
            g_uacvLfsr = UACV_LFSR_SEED;
            g_uacvPatSynced = 1;
            g_uacvPatRun = 0;
        }
        return;
    }
    expected = uacvNextExpected();
    if((s & 0xFFFFFF00u) == (expected & 0xFFFFFF00u))
    {
        g_uacvPatRun = 0;
        return;
    }
    g_uacvPatErrCount++;
    if(g_uacvPatFirstErrIdx == 0)
    {
        g_uacvPatFirstErrIdx  = g_uacvSampleIdx;
        g_uacvPatFirstExpected = expected;
        g_uacvPatFirstActual   = s;
    }
    if(++g_uacvPatRun >= UACV_RESYNC_AFTER)
    {
        g_uacvPatSynced = 0;
        g_uacvPatRun = 0;
        g_uacvPatResyncCount++;
    }
}
#else
static inline void uacvCheckSample(unsigned s) { (void)s; }
#endif
```

- [ ] **Step 2: Call it alongside the byte-lane accumulators**

In `_send_sample_4`, immediately after the three lines added in Task 5, add:

```c
    uacvCheckSample((unsigned)sample);
```

Do the same in the `case 2:` 16-bit arm.

- [ ] **Step 3: Build both modes**

```bash
set +u; source /Applications/XMOS_XTC_15.3.1/SetEnv.sh
cd ~/Development/xmos/sw_usb_audio/app_usb_aud_xk_216_mc
cmake --build build --target app_usb_aud_xk_216_mc_2AMi8o8xxxxxx
```

Then rebuild with `-DUACV_COOPERATIVE=1` added to the config's compiler flags
and confirm it still builds. The passive build must be unchanged — confirm
with `xobjdump --size` that the `.text` figure matches the Task 8 build.

- [ ] **Step 4: Commit**

```bash
cd ~/Development/xmos/lib_xua
git add lib_xua/src/core/buffer/decouple/decouple.xc
git commit -m "decouple: cooperative-mode sample-continuity check

Compiled out entirely unless UACV_COOPERATIVE=1, so the default passive mode
pays nothing in the real-time path.

Resync after 8 consecutive mismatches -- one audio frame at 8 channels -- so
one dropped packet costs one error rather than an unbounded stream. A pattern
that never syncs at all is left distinguishable from one that synced and
diverged, because the first means the host's playback path is not bit-exact
and that is a finding about the host rather than a tool failure."
```

---

## Task 8: Observer — emit the state block

**Files:**
- Modify: `lib_xua/src/core/buffer/decouple/decouple.xc` *(xmos repo, observer branch)*
- Modify: `lib_xua/src/core/main.xc` *(xmos repo, observer branch)*

- [ ] **Step 1: Declare the externs in the decoupler**

Near the other validator globals in `decouple.xc`, add:

```c
/* Owned by ep_buffer.xc and xua_endpoint0.c. All three live on
 * XUA_XUD_TILE_NUM, so plain shared globals are sound: each has exactly one
 * writer and this is the only reader. */
extern unsigned g_uacvFbPollCount;
extern unsigned g_uacvFbValue;
extern unsigned g_uacvAltOut;
extern unsigned g_uacvAltTransitions;
extern unsigned g_uacvClassReqBitmap;
extern unsigned g_uacvHostActive;

/* Cooperative-mode pattern state. Zero in passive mode; Plan 2's rules report
 * SKIP when the mode in the manifest says passive. */
unsigned g_uacvPatErrCount = 0;
unsigned g_uacvPatResyncCount = 0;
unsigned g_uacvPatFirstErrIdx = 0;
unsigned g_uacvPatFirstExpected = 0;
unsigned g_uacvPatFirstActual = 0;
```

- [ ] **Step 2: Write the emit function**

In `decouple.xc`, before `XUA_Buffer_Decouple`, add. **If the Task 1 spike selected SCALAR format, use the second variant instead.**

RECORD format:

```c
#ifdef XSCOPE
#define UACV_MAGIC   0x55414356  /* 'UACV' */
#define UACV_VERSION 1
#define UACV_WORDS   36

static inline void uacvEmitBlock(void)
{
    unsigned w[UACV_WORDS];
    w[0]  = UACV_MAGIC;              w[1]  = UACV_VERSION;
    w[2]  = g_uacvPktCount;          w[3]  = g_uacvPktShortDiscarded;
    w[4]  = g_uacvPktNotMultiple;    w[5]  = g_uacvOrAcc;
    w[6]  = g_uacvAndAcc;            w[7]  = g_uacvNonsilentFrames;
    w[8]  = g_uacvFbPollCount;       w[9]  = g_uacvFbValue;
    w[10] = g_uacvAltOut;            w[11] = g_uacvAltTransitions;
    w[12] = g_uacvClassReqBitmap;    w[13] = g_uacvHostActive;
    w[14] = g_uacvPatErrCount;       w[15] = g_uacvPatResyncCount;
    w[16] = g_uacvPatFirstErrIdx;    w[17] = g_uacvPatFirstExpected;
    w[18] = g_uacvPatFirstActual;    w[19] = g_uacvHistOverflow;
    for(int i = 0; i < 8; i++)
    {
        w[20 + i] = g_uacvHistSize[i];
        w[28 + i] = g_uacvHistCount[i];
    }
    xscope_bytes(4, UACV_WORDS * 4, (unsigned char *)w);
}
#else
static inline void uacvEmitBlock(void) {}
#endif
```

SCALAR format — same word assembly, but replace the `xscope_bytes` call with:

```c
    for(int i = 0; i < UACV_WORDS; i++)
        xscope_int(4 + i, w[i]);
```

- [ ] **Step 3: Call it at the existing cadence**

In the `if(++g_fillProbeDiv >= 10)` body, after `XUA_PROBE_DRYOUT();`, add:

```c
                uacvEmitBlock();
```

- [ ] **Step 4: Register the probes**

In `main.xc`, extend `xscope_register`. **RECORD format:**

```c
    xscope_register(5,
                    XSCOPE_CONTINUOUS, "out_underflow", XSCOPE_UINT, "count",
                    XSCOPE_CONTINUOUS, "out_overflow",  XSCOPE_UINT, "count",
                    XSCOPE_CONTINUOUS, "out_fifo_fill", XSCOPE_UINT, "bytes",
                    XSCOPE_CONTINUOUS, "out_dryout",    XSCOPE_UINT, "count",
                    XSCOPE_CONTINUOUS, "uacv_block",    XSCOPE_UINT, "bytes");
```

**SCALAR format:** register 40 probes — the four existing ones followed by `uacv_w00` through `uacv_w35`, each `XSCOPE_CONTINUOUS, XSCOPE_UINT, "value"`. The names must match `trace.py`'s `_probe_name()` exactly, or the reader will not find them.

- [ ] **Step 5: Build and confirm the existing probes still work**

```bash
set +u; source /Applications/XMOS_XTC_15.3.1/SetEnv.sh
cd ~/Development/xmos/sw_usb_audio/app_usb_aud_xk_216_mc
cmake --build build --target app_usb_aud_xk_216_mc_2AMi8o8xxxxxx
```

Expected: builds clean. Probe ids 0–3 must be unchanged — existing host gates and transcripts depend on them.

- [ ] **Step 6: Commit**

```bash
cd ~/Development/xmos/lib_xua
git add lib_xua/src/core/buffer/decouple/decouple.xc lib_xua/src/core/main.xc
git commit -m "decouple: emit the validator state block at the fill cadence

One atomic 36-word block per ten OUT packets, riding the divider that
already exists for the fill probe. Atomicity matters because the judge
reasons across quantities -- fill slope against packet sizes against
feedback value -- and separately-timestamped probes would have to be aligned
with a tolerance window.

Probe ids 0-3 keep their meaning and behaviour; new ids are append-only."
```

---

## Task 9: Silicon verification

The observer gate. A reduction that is correct in a fixture and wrong at 8,000 packets/s is exactly what this catches.

**Files:**
- Create: `examples/usb/usb_audio_graph_test/transcript_uacv_observer.txt`

- [ ] **Step 1: Start the collector with no host streaming**

```bash
set +u; source /Applications/XMOS_XTC_15.3.1/SetEnv.sh
cd ~/Development/xmos/sw_usb_audio/app_usb_aud_xk_216_mc
xrun --adapter-id 3LajHPG5 --xscope-file ~/uacv_obs \
     bin/2AMi8o8xxxxxx/app_usb_aud_xk_216_mc_2AMi8o8xxxxxx.xe
```

Leave it running. Starting the collector before the host is what avoids the startup race.

- [ ] **Step 2: Start the host**

Flash and run the RT1176 host at current master. In a separate shell:

```bash
pkill -9 LinkServer; pkill -9 redlinkserv; pkill -9 crt_emu_cm_redlink
cd ~/Development/rt1170/evkb/examples/usb/usb_audio_graph_test
LinkServer run MIMXRT1176:MIMXRT1170-EVKB build/usb_audio_graph_test.elf
```

Do not hold the VCOM during any LinkServer operation.

- [ ] **Step 3: Run for 120 seconds, then stop the collector**

Let it stream for two minutes, then **Ctrl-C** the `xrun` (SIGINT — never `kill -9`).

- [ ] **Step 4: Parse the real capture**

```bash
cd ~/Development/rt1170/evkb/tools/uacvalidate
python3 -c "
from trace import read_blocks, MAGIC
bs = read_blocks('$HOME/uacv_obs.vcd')
print('blocks:', len(bs))
t, b = bs[-1]
print('t=%.2f magic=0x%08X ver=%d' % (t, b.magic, b.version))
print('pkts', b.pkt_count, 'short', b.pkt_short_discarded, 'notmult', b.pkt_not_multiple)
print('sizes', b.sizes(), 'overflow', b.size_hist_overflow)
print('or=0x%08X and=0x%08X nonsilent=%d' % (b.or_acc, b.and_acc, b.nonsilent_frames))
print('fbpolls', b.fb_poll_count, 'fbvalue', b.fb_value)
print('alt', b.alt_out, 'transitions', b.alt_transitions, 'reqs 0x%X' % b.class_req_bitmap)
"
```

Expected, for a healthy current-master host streaming 8ch 24-in-4 at 44.1 kHz for 120 s:

| Field | Expected | Why |
|---|---|---|
| `magic` | `0x55414356` | proves the block is aligned and the format agreed |
| block count | ≈12,000 | 100 Hz × 120 s |
| `pkt_count` | ≈120,000 | 1000 packets/s × 120 s |
| `pkt_short_discarded` | 0 | |
| `pkt_not_multiple` | 0 | |
| `sizes()` | two entries, both multiples of 32 | 8 ch × 4 bytes; fractional-sample floor and ceiling |
| `size_hist_overflow` | 0 | |
| `or_acc & 0xFF` | 0 | host left-justifies (it does, since `35b0cce`) |
| `nonsilent_frames` | > 1,000,000 | 44.1 kHz × 120 s with signal |
| `fb_poll_count` | ≈7,500 | 62.5/s × 120 s |
| `alt_out` | 1 | |
| `class_req_bitmap` | `0x5` or `0x7` | SET_INTERFACE + clock SET_CUR |

- [ ] **Step 5: Investigate any mismatch before proceeding**

If `magic` is wrong, the emission format and the reader disagree — re-check the probe names against `trace.py`'s `_probe_name()`. If `pkt_count` is roughly 8× or 1/8 of expected, the packet counter is on the wrong path. **Do not weaken the expectation to make it match.** Silicon wins; the observer is what is wrong.

- [ ] **Step 6: Write the transcript**

Create `examples/usb/usb_audio_graph_test/transcript_uacv_observer.txt` recording: date, both firmware SHAs (observer branch and host), the run duration, the table of expected-versus-observed above with real numbers, and any discrepancy with its explanation.

- [ ] **Step 7: Commit**

```bash
cd ~/Development/rt1170/evkb
git add examples/usb/usb_audio_graph_test/transcript_uacv_observer.txt
git commit -m "usb_audio_graph_test: observer silicon verification

Every reduction checked against a 120 s capture from a known-good host, with
expected values derived from the stream's own arithmetic rather than from
what the observer happened to produce."
```

---

## Task 10: Regenerate the patch

**Files:**
- Create: `examples/usb/usb_audio_graph_test/lib_xua-uac-validator.patch`
- Delete: `examples/usb/usb_audio_graph_test/lib_xua-decouple-instrumentation.patch`

- [ ] **Step 1: Generate the patch**

```bash
cd ~/Development/xmos/lib_xua
git format-patch 3e755c57..instrumentation/uac-host-validator --stdout \
  > ~/Development/rt1170/evkb/examples/usb/usb_audio_graph_test/lib_xua-uac-validator.patch
wc -l ~/Development/rt1170/evkb/examples/usb/usb_audio_graph_test/lib_xua-uac-validator.patch
```

`3e755c57` is the last upstream commit before the instrumentation branch, so the patch covers the original four probes plus everything added here — one artifact that reproduces the witness firmware.

- [ ] **Step 2: Verify the patch applies to a clean tree**

```bash
cd /tmp && rm -rf lib_xua_verify && git clone -q ~/Development/xmos/lib_xua lib_xua_verify
cd lib_xua_verify && git checkout -q 3e755c57
git apply --check ~/Development/rt1170/evkb/examples/usb/usb_audio_graph_test/lib_xua-uac-validator.patch && echo APPLIES_CLEAN
```

Expected: `APPLIES_CLEAN`.

- [ ] **Step 3: Replace the old patch and commit**

```bash
cd ~/Development/rt1170/evkb/examples/usb/usb_audio_graph_test
git rm lib_xua-decouple-instrumentation.patch
git add lib_xua-uac-validator.patch
cd ~/Development/rt1170/evkb
git commit -m "usb_audio_graph_test: regenerate the lib_xua patch for the validator

One patch file for the whole observer branch, superseding the four-probe
instrumentation patch. Single-file delivery was chosen over an accumulating
series because applying it is then unordered and hard to get wrong; the
commit-by-commit reasoning stays on the branch in ~/Development/xmos/lib_xua.

No XMOS source enters this repo."
```

- [ ] **Step 4: Push the observer branch**

```bash
cd ~/Development/xmos/lib_xua && git push -u origin instrumentation/uac-host-validator
```

---

## Done when

- The spike has selected an emission format and `docs/uac-validator-wire-format.md` records both the choice and the rejection.
- `python3 -m unittest test_trace -v` passes.
- The observer builds for `2AMi8o8xxxxxx` with probes 0–3 unchanged.
- A 120 s silicon capture parses, `magic` is correct, and every field matches the expected table in Task 9.
- `lib_xua-uac-validator.patch` applies clean to `3e755c57`.
