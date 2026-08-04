"""Wire format shared by the UAC validator's device observer and its judge.

The observer (XC, in a separate XMOS-licensed repo) emits a 37-word state
block at 100 Hz over xscope; `xrun --xscope-file` captures it to a VCD. This
module is the contract between the two sides: it owns the word layout, the
VCD emission format, and the reader. Field *meanings* are normative in
`docs/uac-validator-wire-format.md` -- this file is normative for the layout
only, so that a change to the block is a change to one table here rather than
to two hand-written copies that can drift apart.

`synth_vcd` exists so the judge can be built and tested with no hardware
attached, and so both directions of the contract are exercised by the same
`SCALAR_LAYOUT` table.

The block currently ships as 37 scalar xscope probes (probe id 4+i, VCD
signal names `uacv_w00`..`uacv_w36`). A bench spike may replace that with a
single `xscope_bytes` probe carrying all 144 bytes; that is an emission
detail, and `read_blocks` returning `(time, Block)` pairs is unchanged
either way, so callers never see it.

Counters are free-running uint32 and may wrap. This module does NOT unwrap
them -- deciding what a wrap means is the judge's job, not the reader's. It
does provide `delta32`, because taking a difference in the module's own
arithmetic is a layout concern; interpreting that difference is not.

Emission rule, binding on the observer as well as on `synth_vcd`: a timestamp
must never be empty. Once a device stops re-sending idle counters, a state
block identical to its predecessor would emit a bare `#t` line and the sample
would disappear from the capture entirely -- and a wholly stationary block is
exactly what a stalled host looks like, so the samples that vanish are the
ones that matter most. When nothing else changed, emit word 0 alone as a
heartbeat.
"""
from dataclasses import dataclass, field

MAGIC = 0x55414356  # 'UACV'
VERSION = 1
BLOCK_WORDS = 37

MASK32 = 0xFFFFFFFF

# Word index -> attribute, for the scalar (non-histogram) words of the block.
#
# NOT a contiguous head any more: words 0-19 are scalars, 20-35 are the two
# histogram arrays, and word 36 is a scalar again. `pat_sync_count` was added
# after the histograms were already placed, and appending it was the only
# change that did not renumber every histogram slot -- an existing capture's
# word 20 still means what it meant. Read the indices from this dict; do not
# assume "scalars are 0..19", which was true before word 36 existed.
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
    # Word 36, after the histograms -- see the note above.
    36: "pat_sync_count",
}

HIST_SIZE_BASE = 20
HIST_COUNT_BASE = 28
HIST_SLOTS = 8

PROBE_BASE = 4  # ids 0..3 are reserved for the existing decoupler probes

# How long one block's worth of word-writes may take to arrive.
#
# The block does NOT arrive at a single VCD timestamp. As 37 scalar probes it
# is 37 separate xscope_int() calls, which land microseconds apart and get
# microseconds-apart timestamps; a reader demanding one timestamp per block
# would reject every real scalar capture with "first timestamp writes 1 of 37
# words". So writes are grouped into a block by two rules:
#
#   1. Re-writing a word index the open group already carries closes it. This
#      is exact and needs no threshold: a block writes each word at most once,
#      so the repeat IS the next block beginning. Against a real scalar
#      capture, where every block re-sends all 37 words, this rule alone
#      separates every block correctly.
#   2. Otherwise a write more than `emit_window_s` after the group's first
#      write closes it. This covers the delta-encoded shape -- consecutive
#      blocks touching disjoint sets of words, where rule 1 never fires.
#
# The window must sit above the time one block takes to emit and below the
# emission period. At the observer's 100 Hz the period is 10 ms and 37
# xscope_int calls span microseconds, so 1 ms clears the burst by three orders
# of magnitude and the period by one.
#
# The ambiguity this cannot resolve, stated rather than hidden: a block that
# takes longer than the window to emit splits into two, and two blocks closer
# together than the window merge -- unless rule 1 catches them, which for a
# full 37-word block it always does. Both failures are visible (a split block
# fails the first-block completeness check; merged blocks under-count) rather
# than silent. It is a parameter because the margin is the observer's
# property, not this module's.
EMISSION_WINDOW_S = 0.001


def _signal_name(i):
    return f"uacv_w{i:02d}"


def delta32(prev, cur):
    """Advance of a free-running uint32 counter from `prev` to `cur`.

    The mask is the whole point: an unmasked subtraction turns a counter that
    wrapped by one into -4294967295 rather than 1. It lives here because the
    width is this module's fact, not the judge's -- what the advance *means*
    is still the judge's call.
    """
    return (cur - prev) & MASK32


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
    # Times the pattern achieved lock. Grouped with the other pat_* fields for
    # readability although its word index is the block's last -- the layout is
    # SCALAR_LAYOUT's business, not this declaration's.
    #
    # This is the witness that lets the judge tell "the host's playback path is
    # not bit-exact" from "the host dropped packets". Without it, a host whose
    # OS applies volume or resampling never holds lock, relocks endlessly, and
    # accumulates a large pat_err_count that is indistinguishable from genuine
    # discontinuities -- so the judge would accuse a conformant host of losing
    # audio. 0 means never locked; more than 1 means lock was lost and regained.
    pat_sync_count: int = 0
    pat_first_err_idx: int = 0
    pat_first_expected: int = 0
    pat_first_actual: int = 0
    size_hist_overflow: int = 0
    size_hist_size: list = field(default_factory=lambda: [0] * HIST_SLOTS)
    size_hist_count: list = field(default_factory=lambda: [0] * HIST_SLOTS)

    def __post_init__(self):
        for name in ("size_hist_size", "size_hist_count"):
            v = getattr(self, name)
            if len(v) != HIST_SLOTS:
                raise ValueError(
                    f"{name} must have {HIST_SLOTS} entries, got {len(v)}")

    def sizes(self):
        """Populated histogram slots as {packet size in bytes: count}.

        A zero size means the slot was never claimed, so it is dropped rather
        than reported as a zero-byte packet class.

        Counts for a repeated size are summed. The device claims slots without
        collisions by construction, so this is defence against a malformed
        capture, not expected input -- but overwriting would silently lose
        packets, and a judge cross-checking sum(sizes().values()) against
        pkt_count would see an unexplainable shortfall.
        """
        d = {}
        for s, c in zip(self.size_hist_size, self.size_hist_count):
            if s != 0:
                d[s] = d.get(s, 0) + c
        return d

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
        lines.append(f"$var wire 32 {ids[i]} {_signal_name(i)} $end")
    lines.append("$enddefinitions $end")

    prev = None
    for t, blk in timed_blocks:
        words = blk.to_words()
        # Standard VCD semantics: emit only what moved. The reader carries the
        # rest forward, which is also what a real xscope capture looks like
        # once the device stops re-sending idle counters.
        changed = [i for i, v in enumerate(words)
                   if prev is None or prev[i] != v]
        if not changed:
            # Heartbeat -- see the emission rule in the module docstring. A
            # block identical to its predecessor would otherwise emit a bare
            # "#t" line and vanish from the capture, which is precisely what
            # a stalled host produces for its entire duration.
            changed = [0]
        lines.append(f"#{round(t / tick_s)}")
        for i in changed:
            lines.append(f"b{words[i]:b} {ids[i]}")
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


def read_blocks(path, emit_window_s=EMISSION_WINDOW_S):
    """Parse a capture into `[(time_seconds, Block)]`, in capture order.

    Words that did not change carry their previous value forward. One Block is
    produced per emission group -- see EMISSION_WINDOW_S for how writes are
    grouped -- timestamped at the group's first write. Groups are formed from
    words WRITTEN, not changed: under the heartbeat rule a stationary block
    rewrites word 0 with the value it already had, and that sample must still
    be reported. A timestamp carrying only a lost-sample marker writes no
    state word and contributes to no group.

    Raises ValueError if the capture has no $timescale, if it does not declare
    every `uacv_w*` signal in the block, or if its first group does not write
    all of them.
    """
    with open(path) as f:
        lines = f.read().split("\n")
    tick_s, names, first_data_line = _parse_header(lines)

    # Map by name, not by probe id: the id is an emission detail, the name is
    # the contract.
    word_of = {}
    for sid, name in names.items():
        if name.startswith("uacv_w"):
            word_of[sid] = int(name[len("uacv_w"):])

    # A capture that does not declare the whole block is not a partial reading
    # of this wire format, it is a different wire format. Refuse it rather than
    # carry zeros forward for the absent words: a downstream rule cannot tell a
    # never-declared word from a genuinely zero one, and would read the
    # plausible-looking zero as real data.
    declared = set(word_of.values())
    missing = [w for w in range(BLOCK_WORDS) if w not in declared]
    unknown = sorted(w for w in declared if w >= BLOCK_WORDS)
    if missing or unknown:
        raise ValueError(
            f"capture declares {len(declared)} of {BLOCK_WORDS} uacv_w* "
            f"signals; missing word indices {missing}"
            + (f", unknown word indices {unknown}" if unknown else ""))

    out = []
    state = [0] * BLOCK_WORDS
    t = 0
    written = set()    # word indices written in the open emission group
    group_t = None     # seconds at that group's first write
    checked_first = False

    def flush():
        # Declaring every signal does not mean every one arrived: a capture that
        # lost its opening samples would hand the judge and_acc = 0, the exact
        # stuck-at-zero fault the all-ones default exists to catch, dressed up
        # as real data. Demand a complete first block instead of seeding the
        # state with defaults -- seeding would set magic to MAGIC and defeat
        # the wire-format check that reads it.
        nonlocal checked_first
        if not written:
            return
        if not checked_first:
            absent = [w for w in range(BLOCK_WORDS) if w not in written]
            if absent:
                raise ValueError(
                    f"first timestamp writes {len(written)} of {BLOCK_WORDS} "
                    f"words; missing word indices {absent}")
            checked_first = True
        out.append((group_t, Block.from_words(state)))

    for ln in lines[first_data_line:]:
        ln = ln.strip()
        if not ln:
            continue
        if ln[0] == "#":
            t = int(ln[1:])
            # Rule 2: the open group has run past its emission window.
            if written and t * tick_s - group_t > emit_window_s:
                flush()
                written = set()
                group_t = None
        elif ln[0] == "b":
            # Only the b-form is handled: every uacv_w* $var is 32 bits wide,
            # and the VCD spec permits the scalar form only for 1-bit vars, so
            # a state word can never legally appear as one. (count_missing_marks
            # accepts both because a marker may be declared 1-bit.)
            val_s, sid = ln[1:].split()
            if sid in word_of:
                w = word_of[sid]
                # Rule 1: a word arriving twice is the next block starting.
                if w in written:
                    flush()
                    written = set()
                    group_t = None
                if group_t is None:
                    group_t = t * tick_s
                state[w] = int(val_s, 2) & MASK32
                written.add(w)
    flush()
    return out


def count_missing_marks(path):
    """Number of xscope lost-sample records in the capture (0 if none).

    xscope publishes these as a signal named `Missing_Data` (probe id 255).
    Any non-zero count means samples were dropped between the device and the
    host, so every counter in the capture is a LOWER BOUND -- a judge must not
    read a clean count as proof the device behaved.

    Only non-zero records count. A zero record says no data was missing at
    that instant, and counting it would make a clean capture look untrustworthy
    -- it would also disagree with tools/vcdfill.py, which filters the same
    probe the same way.
    """
    with open(path) as f:
        lines = f.read().split("\n")
    _, names, first_data_line = _parse_header(lines)
    sids = {sid for sid, name in names.items() if name == "Missing_Data"}
    if not sids:
        return 0
    n = 0
    for ln in lines[first_data_line:]:
        ln = ln.strip()
        if not ln or ln[0] == "#":
            continue
        # Both forms: the marker's width is xscope's choice, and a 1-bit $var
        # is emitted scalar. "1" in the value is the non-zero test that also
        # survives x/z bits without raising.
        if ln[0] == "b":
            val_s, sid = ln[1:].split()
            if sid in sids and "1" in val_s:
                n += 1
        elif ln[1:] in sids and ln[0] == "1":
            n += 1
    return n
