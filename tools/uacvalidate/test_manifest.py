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
        for k in ("channels", "subslot_bytes", "sample_rate_hz"):
            with self.subTest(field=k), self.assertRaises(ManifestError):
                Manifest.from_dict(dict(VALID, **{k: 0}))

    def test_rejects_non_integer_dimension(self):
        """A string sample rate would otherwise reach nominal_frames_per_packet
        and raise there, far from the field that was wrong."""
        for k in ("channels", "subslot_bytes", "sample_rate_hz"):
            with self.subTest(field=k), self.assertRaises(ManifestError):
                Manifest.from_dict(dict(VALID, **{k: str(VALID[k])}))


if __name__ == "__main__":
    unittest.main()
