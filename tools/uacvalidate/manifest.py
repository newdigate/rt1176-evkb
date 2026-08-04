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
            "sample_rate_hz", "mode", "host_note")
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
        for k in ("channels", "subslot_bytes", "sample_rate_hz"):
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
        """OUT data packets per second.

        One per USB frame at full speed. At high speed lib_xua's iso OUT
        endpoint is serviced once per frame as well on this device, not once
        per microframe -- the transcript's sustained pkts/s=1000 at HS is the
        measurement behind this.
        """
        return 1000

    @property
    def nominal_frames_per_packet(self):
        return self.sample_rate_hz / self.packets_per_second

    @property
    def legal_packet_sizes(self):
        """Sizes a conformant host may send: floor and ceiling of the
        fractional frames-per-packet, in bytes.

        A set, and at an integer rate it collapses to ONE element -- 48 kHz is
        exactly 48 frames per packet, so a conformant host has exactly one
        legal size and any second size is a violation. That is the intended
        result, not a degenerate one: do not "fix" this into always returning
        two sizes, which would make the 48 kHz check unfalsifiable.
        """
        n = self.nominal_frames_per_packet
        lo, hi = math.floor(n), math.ceil(n)
        return {lo * self.frame_bytes, hi * self.frame_bytes}

    @property
    def nominal_bytes_per_second(self):
        return self.sample_rate_hz * self.frame_bytes
