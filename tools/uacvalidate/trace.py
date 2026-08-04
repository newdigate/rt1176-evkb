"""Wire format shared by the UAC validator's device observer and its judge.

The observer (XC, in a separate XMOS-licensed repo) emits a 36-word state
block at 100 Hz over xscope; `xrun --xscope-file` captures it to a VCD. This
module is the contract between the two sides: it owns the word layout, the
VCD emission format, and the reader. Field *meanings* are normative in
`docs/uac-validator-wire-format.md` -- this file is normative for the layout
only, so that a change to the block is a change to one table here rather than
to two hand-written copies that can drift apart.

`synth_vcd` exists so the judge can be built and tested with no hardware
attached, and so both directions of the contract are exercised by the same
`SCALAR_LAYOUT` table.

The block currently ships as 36 scalar xscope probes (probe id 4+i, VCD
signal names `uacv_w00`..`uacv_w35`). A bench spike may replace that with a
single `xscope_bytes` probe carrying all 144 bytes; that is an emission
detail, and `read_blocks` returning `(time, Block)` pairs is unchanged
either way, so callers never see it.

Counters are free-running uint32 and may wrap. This module does NOT unwrap
them -- deciding what a wrap means is the judge's job, not the reader's.
"""
from dataclasses import dataclass, field

MAGIC = 0x55414356  # 'UACV'
VERSION = 1
BLOCK_WORDS = 36

MASK32 = 0xFFFFFFFF

# Word index -> attribute, for the scalar (non-histogram) head of the block.
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

PROBE_BASE = 4  # ids 0..3 are reserved for the existing decoupler probes


def _probe_name(i):
    return f"uacv_w{i:02d}"


@dataclass
class Block:
    """One 100 Hz sample of observer state, as captured (never unwrapped)."""

    magic: int = MAGIC
    version: int = VERSION
    pkt_count: int = 0
    pkt_short_discarded: int = 0
    pkt_not_multiple: int = 0
    or_acc: int = 0
    # AND-accumulator: its identity is all-ones. Defaulting it to 0 would make
    # a fresh Block indistinguishable from a stream in which every sample bit
    # was clear -- the exact stuck-at-zero fault the accumulator exists to find.
    and_acc: int = MASK32
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
        """Populated histogram slots as {packet size in bytes: count}.

        A zero size means the slot was never claimed, so it is dropped rather
        than reported as a zero-byte packet class.
        """
        return {s: c for s, c in zip(self.size_hist_size, self.size_hist_count)
                if s != 0}

    def to_words(self):
        w = [0] * BLOCK_WORDS
        for idx, name in SCALAR_LAYOUT.items():
            w[idx] = getattr(self, name) & MASK32
        for i in range(HIST_SLOTS):
            w[HIST_SIZE_BASE + i] = self.size_hist_size[i] & MASK32
            w[HIST_COUNT_BASE + i] = self.size_hist_count[i] & MASK32
        return w

    @classmethod
    def from_words(cls, w):
        if len(w) != BLOCK_WORDS:
            raise ValueError(f"expected {BLOCK_WORDS} words, got {len(w)}")
        kw = {name: w[idx] & MASK32 for idx, name in SCALAR_LAYOUT.items()}
        kw["size_hist_size"] = [w[HIST_SIZE_BASE + i] & MASK32
                                for i in range(HIST_SLOTS)]
        kw["size_hist_count"] = [w[HIST_COUNT_BASE + i] & MASK32
                                 for i in range(HIST_SLOTS)]
        return cls(**kw)


_UNITS = (("s", 1e12), ("ms", 1e9), ("us", 1e6), ("ns", 1e3), ("ps", 1.0))


def _timescale_text(timescale_ps):
    """Largest unit that still expresses `timescale_ps` as a whole number."""
    for name, ps_per in _UNITS:
        if timescale_ps >= ps_per and timescale_ps % ps_per == 0:
            return f"{int(timescale_ps // ps_per)} {name}"
    return f"{int(timescale_ps)} ps"


def synth_vcd(path, timed_blocks, timescale_ps=1000000):
    """Write `[(time_seconds, Block)]` as a VCD in the observer's own format.

    Models a healthy device: every timestamp carries a full, self-consistent
    block, and no xscope lost-sample markers are emitted.
    """
    tick_s = timescale_ps * 1e-12
    ids = [chr(33 + i) for i in range(BLOCK_WORDS)]
    lines = [
        "$timescale", f" {_timescale_text(timescale_ps)}", "$end",
    ]
    for i in range(BLOCK_WORDS):
        lines.append(f"$var wire 32 {ids[i]} {_probe_name(i)} $end")
    lines.append("$enddefinitions $end")

    prev = None
    for t, blk in timed_blocks:
        words = blk.to_words()
        lines.append(f"#{round(t / tick_s)}")
        for i, v in enumerate(words):
            # Standard VCD semantics: emit only what moved. The reader carries
            # the rest forward, which is also what a real xscope capture looks
            # like once the device stops re-sending idle counters.
            if prev is None or prev[i] != v:
                lines.append(f"b{v:b} {ids[i]}")
        prev = words
    with open(path, "w") as f:
        f.write("\n".join(lines) + "\n")


def _parse_header(lines):
    """-> (tick_seconds, {vcd id: signal name}, index of first data line)."""
    tick_s = None
    names = {}
    i = 0
    while i < len(lines):
        toks = lines[i].split()
        if toks and toks[0] == "$timescale":
            # Magnitude and unit may sit on the same line as the keyword or on
            # the next one; xscope writes the split form, hand-written VCDs the
            # inline one.
            body = toks[1:]
            j = i
            while "$end" not in body and j + 1 < len(lines):
                j += 1
                body = body + lines[j].split()
            body = [t for t in body if t != "$end"]
            text = "".join(body)
            num = "".join(c for c in text if c.isdigit() or c == ".")
            unit = text[len(num):].strip()
            tick_s = float(num) * {"s": 1, "ms": 1e-3, "us": 1e-6,
                                   "ns": 1e-9, "ps": 1e-12}[unit]
            i = j
        elif toks and toks[0] == "$var":
            names[toks[3]] = toks[4]
        elif toks and toks[0] == "$enddefinitions":
            i += 1
            break
        i += 1
    if tick_s is None:
        raise ValueError("VCD has no $timescale")
    return tick_s, names, i


def read_blocks(path):
    """Parse a capture into `[(time_seconds, Block)]`, in capture order.

    Words absent from the file read as zero, and words that did not change at
    a timestamp carry their previous value forward. One Block is produced per
    timestamp at which at least one state word changed -- a timestamp that
    only carried, say, a lost-sample marker adds no new state and is skipped.
    """
    with open(path) as f:
        lines = f.read().split("\n")
    tick_s, names, i = _parse_header(lines)

    # Map by name, not by probe id: the id is an emission detail, the name is
    # the contract.
    word_of = {}
    for sid, name in names.items():
        if name.startswith("uacv_w"):
            word_of[sid] = int(name[len("uacv_w"):])

    out = []
    state = [0] * BLOCK_WORDS
    t = 0
    dirty = False
    for ln in lines[i:]:
        ln = ln.strip()
        if not ln:
            continue
        if ln[0] == "#":
            if dirty:
                out.append((t * tick_s, Block.from_words(state)))
            t = int(ln[1:])
            dirty = False
        elif ln[0] == "b":
            val_s, sid = ln[1:].split()
            if sid in word_of:
                state[word_of[sid]] = int(val_s, 2) & MASK32
                dirty = True
    if dirty:
        out.append((t * tick_s, Block.from_words(state)))
    return out


def count_missing_marks(path):
    """Number of xscope lost-sample records in the capture (0 if none).

    xscope publishes these as a signal named `Missing_Data` (probe id 255).
    Any non-zero count means samples were dropped between the device and the
    host, so every counter in the capture is a LOWER BOUND -- a judge must not
    read a clean count as proof the device behaved.
    """
    with open(path) as f:
        lines = f.read().split("\n")
    _, names, i = _parse_header(lines)
    sids = {sid for sid, name in names.items() if name == "Missing_Data"}
    if not sids:
        return 0
    n = 0
    for ln in lines[i:]:
        ln = ln.strip()
        if not ln or ln[0] == "#":
            continue
        if ln[0] == "b":
            if ln[1:].split()[1] in sids:
                n += 1
        elif ln[1:] in sids:  # scalar change, e.g. "1!"
            n += 1
    return n
