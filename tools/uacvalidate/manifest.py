"""The operator's declaration of a validation run.

Every expectation the judge applies comes from here, never from the trace.
Inferring the format from the data would let a mis-set expectation silently
reinterpret it, which is exactly how a broken probe once produced a plausible
but meaningless drift slope.
"""
import json
import math
from dataclasses import dataclass

REQUIRED = ("audio_class", "speed", "channels", "subslot_bytes",
            "sample_rate_hz", "mode", "host_note", "max_packet_size_bytes")
MODES = ("passive", "cooperative")
SPEEDS = ("FS", "HS")


class ManifestError(Exception):
    pass


@dataclass(frozen=True)
class Manifest:
    audio_class: int
    speed: str
    channels: int
    subslot_bytes: int
    sample_rate_hz: int
    mode: str
    host_note: str
    # The endpoint's declared wMaxPacketSize, read off the descriptor by the
    # operator. It is NOT derivable from the other fields, which is exactly
    # why it belongs here: the fractional-sample ceiling
    # (max(legal_packet_sizes)) is a different number with a different
    # meaning, and an earlier R4a compared against that one. With an
    # asynchronous feedback loop a host is entitled to vary packet size, so
    # the fractional ceiling FAILed conformant hosts -- and at an integer
    # sample rate, where legal_packet_sizes has one element, it FAILed on any
    # variation at all. Only the descriptor carries the endpoint's real limit.
    max_packet_size_bytes: int

    @classmethod
    def from_dict(cls, d):
        missing = [k for k in REQUIRED if k not in d]
        if missing:
            raise ManifestError(f"manifest missing required fields: {missing}")
        if d["mode"] not in MODES:
            raise ManifestError(f"mode must be one of {MODES}, got {d['mode']!r}")
        if d["speed"] not in SPEEDS:
            raise ManifestError(f"speed must be one of {SPEEDS}, got {d['speed']!r}")
        if d["audio_class"] not in (1, 2):
            raise ManifestError(f"audio_class must be 1 or 2, got {d['audio_class']}")
        if d["audio_class"] == 1 and d["speed"] == "HS":
            raise ManifestError(
                "audio_class 1 at HS: the UAC1 image on this bench is full speed. "
                "A manifest that says otherwise would misread every packet size.")
        for k in ("channels", "subslot_bytes", "sample_rate_hz",
                  "max_packet_size_bytes"):
            # bool is a subclass of int, and `true` is a JSON literal: a typo
            # putting it where a channel count belongs would otherwise pass
            # validation as 1 and yield a plausible frame size. The manifest
            # exists so a wrong expectation fails loudly rather than quietly
            # reinterpreting the capture.
            if not isinstance(d[k], int) or isinstance(d[k], bool) or d[k] <= 0:
                raise ManifestError(f"{k} must be a positive integer, got {d[k]!r}")
        return cls(**{k: d[k] for k in REQUIRED})

    @classmethod
    def load(cls, path):
        with open(path) as f:
            return cls.from_dict(json.load(f))

    @property
    def frame_bytes(self):
        return self.channels * self.subslot_bytes

    @property
    def packets_per_second(self):
        """OUT data packets per second, as the DEVICE counts them.

        One per 1 ms frame at full speed; one per 125 us microframe at high
        speed. The distinction is not academic and an earlier version of this
        property got it wrong, returning 1000 for both.

        The trap: the RT1176 host's heartbeat reports pkts/s=1000 at high
        speed, and that number is real -- but it counts iTD completions, which
        are frames. Each frame carries eight microframe transactions, and the
        decoupler releases a buffer per transaction, so the device sees 8000.

        Silicon settled it. A 220 s capture at 44.1 kHz, 8ch, 4-byte subslots
        recorded packet sizes of exactly 160 and 192 bytes -- 5 and 6 audio
        frames -- in the ratio 828230:869079, i.e. 48.8% fives. 44100/8000 is
        5.5125, which predicts 48.75%. Sizes of 1408/1440 B would have meant
        1000 packets/s; they were never observed.

        Getting this wrong is not a small error: it scales
        legal_packet_sizes by eight, so R4a and W2 would both judge a
        conformant host against sizes it could never send.
        """
        return 8000 if self.speed == "HS" else 1000

    @property
    def nominal_frames_per_packet(self):
        return self.sample_rate_hz / self.packets_per_second

    @property
    def legal_packet_sizes(self):
        """Sizes a well-behaved host sends: floor and ceiling of the
        fractional frames-per-packet, in bytes.

        Read by W2 only, and W2 is a WARN. These are not a legality bound --
        with an asynchronous feedback loop the host is entitled to vary packet
        size, and nothing citable forbids it. R4a's bound is the endpoint's
        declared `max_packet_size_bytes`, which is a different number.

        A set, and at an integer rate it collapses to ONE element: 48 kHz is
        exactly 48 frames per packet, so any second size is worth warning
        about. That is the intended result, not a degenerate one -- do not
        "fix" it into always returning two sizes, which would make the 48 kHz
        warning unreachable.
        """
        n = self.nominal_frames_per_packet
        lo, hi = math.floor(n), math.ceil(n)
        return {lo * self.frame_bytes, hi * self.frame_bytes}

    @property
    def nominal_bytes_per_second(self):
        return self.sample_rate_hz * self.frame_bytes
