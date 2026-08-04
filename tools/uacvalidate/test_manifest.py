import unittest

from manifest import Manifest, ManifestError


VALID = {
    "audio_class": 2,
    "speed": "HS",
    "channels": 8,
    "subslot_bytes": 4,
    "sample_rate_hz": 44100,
    "mode": "passive",
    "host_note": "RT1176 EVKB, master",
    # The endpoint's declared wMaxPacketSize, with headroom above the
    # fractional ceiling of 1440 B. The headroom is deliberate: it is what
    # separates R4a (did the host exceed the descriptor's limit -- FAIL) from
    # W2 (was the sizing lumpier than nominal -- WARN).
    "max_packet_size_bytes": 1536,
}


class TestManifest(unittest.TestCase):
    def test_parses_valid(self):
        m = Manifest.from_dict(VALID)
        self.assertEqual(m.audio_class, 2)
        self.assertEqual(m.channels, 8)
        self.assertEqual(m.mode, "passive")

    def test_frame_bytes_is_channels_times_subslot(self):
        self.assertEqual(Manifest.from_dict(VALID).frame_bytes, 32)

    def test_packets_per_second_hs_is_1000(self):
        self.assertEqual(Manifest.from_dict(VALID).packets_per_second, 1000)

    def test_packets_per_second_fs_is_1000(self):
        d = dict(VALID, audio_class=1, speed="FS", channels=2, subslot_bytes=2)
        self.assertEqual(Manifest.from_dict(d).packets_per_second, 1000)

    def test_nominal_frames_per_packet_is_fractional(self):
        m = Manifest.from_dict(VALID)
        self.assertAlmostEqual(m.nominal_frames_per_packet, 44.1, places=6)

    def test_legal_packet_sizes_are_floor_and_ceil_frames(self):
        m = Manifest.from_dict(VALID)
        self.assertEqual(m.legal_packet_sizes, {44 * 32, 45 * 32})

    def test_legal_packet_sizes_collapse_to_one_at_integer_rate(self):
        """48 kHz is exactly 48 frames per packet, so a conformant host has
        exactly one legal size. The one-element set is the point: a second size
        at this rate is a violation, and a property that always returned two
        would make that check unfalsifiable."""
        m = Manifest.from_dict(dict(VALID, sample_rate_hz=48000))
        self.assertEqual(m.legal_packet_sizes, {48 * 32})

    def test_nominal_bytes_per_second(self):
        m = Manifest.from_dict(VALID)
        self.assertAlmostEqual(m.nominal_bytes_per_second, 44100 * 32, places=3)

    def test_rejects_unknown_mode(self):
        with self.assertRaises(ManifestError):
            Manifest.from_dict(dict(VALID, mode="magic"))

    def test_rejects_missing_field(self):
        d = dict(VALID)
        del d["channels"]
        with self.assertRaises(ManifestError):
            Manifest.from_dict(d)

    def test_rejects_class_1_at_high_speed(self):
        with self.assertRaises(ManifestError):
            Manifest.from_dict(dict(VALID, audio_class=1, speed="HS"))

    def test_max_packet_size_is_required_and_carried(self):
        """It comes off the endpoint descriptor and is derivable from nothing
        else in the manifest. R4a's whole assertion rests on it, so a run that
        forgets it must fail at parse rather than fall back to some computed
        number -- falling back is how R4a came to compare against the
        fractional ceiling and FAIL conformant hosts."""
        self.assertEqual(Manifest.from_dict(VALID).max_packet_size_bytes, 1536)
        d = dict(VALID)
        del d["max_packet_size_bytes"]
        with self.assertRaises(ManifestError) as cm:
            Manifest.from_dict(d)
        self.assertIn("max_packet_size_bytes", str(cm.exception))

    def test_max_packet_size_is_not_the_fractional_ceiling(self):
        """Pinned as a distinction, not an equality. If someone later
        "simplifies" the field away by deriving it from legal_packet_sizes,
        this is the test that says no."""
        m = Manifest.from_dict(VALID)
        self.assertNotEqual(m.max_packet_size_bytes, max(m.legal_packet_sizes))
        self.assertGreater(m.max_packet_size_bytes, max(m.legal_packet_sizes))

    # The four below were added because deleting the corresponding branch from
    # from_dict left the suite green. An unexercised validation branch is worse
    # than no branch: it reads as protection that was never demonstrated, and
    # every one of these rejections exists to stop a mis-set field being taken
    # at face value by the rules downstream.

    def test_rejects_unknown_speed(self):
        with self.assertRaises(ManifestError):
            Manifest.from_dict(dict(VALID, speed="SS"))

    def test_rejects_unknown_audio_class(self):
        with self.assertRaises(ManifestError):
            Manifest.from_dict(dict(VALID, audio_class=3))

    def test_rejects_non_positive_dimension(self):
        for k in ("channels", "subslot_bytes", "sample_rate_hz",
                  "max_packet_size_bytes"):
            with self.subTest(field=k), self.assertRaises(ManifestError):
                Manifest.from_dict(dict(VALID, **{k: 0}))

    def test_rejects_bool_dimension(self):
        """bool is a subclass of int and `true` is a JSON literal, so a typo
        in the manifest would otherwise validate as 1 and produce a plausible
        frame size -- a silently wrong verdict about a USB host, which is the
        exact quiet misreading the manifest exists to prevent."""
        for k in ("channels", "subslot_bytes", "sample_rate_hz",
                  "max_packet_size_bytes"):
            with self.subTest(field=k), self.assertRaises(ManifestError):
                Manifest.from_dict(dict(VALID, **{k: True}))

    def test_rejects_non_integer_dimension(self):
        """A string sample rate would otherwise reach nominal_frames_per_packet
        and raise there, far from the field that was wrong."""
        for k in ("channels", "subslot_bytes", "sample_rate_hz",
                  "max_packet_size_bytes"):
            with self.subTest(field=k), self.assertRaises(ManifestError):
                Manifest.from_dict(dict(VALID, **{k: str(VALID[k])}))


if __name__ == "__main__":
    unittest.main()
