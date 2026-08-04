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
    # fractional ceiling of 192 B -- 800 B is what the real device advertises.
    # The headroom is deliberate: it is what separates R4a (did the host exceed
    # the descriptor's limit -- FAIL) from W2 (was the sizing lumpier than
    # nominal -- WARN).
    "max_packet_size_bytes": 800,
}


class TestManifest(unittest.TestCase):
    def test_parses_valid(self):
        m = Manifest.from_dict(VALID)
        self.assertEqual(m.audio_class, 2)
        self.assertEqual(m.channels, 8)
        self.assertEqual(m.mode, "passive")

    def test_frame_bytes_is_channels_times_subslot(self):
        self.assertEqual(Manifest.from_dict(VALID).frame_bytes, 32)

    def test_packets_per_second_hs_is_8000(self):
        """One OUT transaction per 125 us microframe, as the DEVICE counts it.

        Pinned as a literal because it was 1000 here for both speeds and the
        whole suite agreed with it. The host's heartbeat does say pkts/s=1000
        at HS -- it counts iTD frame completions -- but a 220 s capture at
        44.1 kHz/8ch/4 B recorded packets of 160 and 192 B, i.e. 5 and 6 audio
        frames, in a 48.8:51.2 ratio. That is 44100/8000 = 5.5125 frames per
        packet. At 1000/s the sizes would have been 1408/1440 B, which the
        device never saw."""
        self.assertEqual(Manifest.from_dict(VALID).packets_per_second, 8000)

    def test_packets_per_second_fs_is_1000(self):
        """Full speed has no microframes, so 1 ms frames are the transaction
        cadence and 1000/s is right. Held beside the HS case so that
        "8000 everywhere" fails as loudly as "1000 everywhere" did."""
        d = dict(VALID, audio_class=1, speed="FS", channels=2, subslot_bytes=2)
        self.assertEqual(Manifest.from_dict(d).packets_per_second, 1000)

    def test_nominal_frames_per_packet_is_fractional(self):
        m = Manifest.from_dict(VALID)
        self.assertAlmostEqual(m.nominal_frames_per_packet, 5.5125, places=6)

    def test_legal_packet_sizes_are_floor_and_ceil_frames(self):
        """5 and 6 frames of 32 B -- 160 and 192 B, the two sizes silicon
        actually recorded."""
        m = Manifest.from_dict(VALID)
        self.assertEqual(m.legal_packet_sizes, {5 * 32, 6 * 32})

    def test_legal_packet_sizes_collapse_to_one_at_integer_rate(self):
        """48 kHz is exactly 6 frames per microframe packet, so a conformant
        host has exactly one legal size. The one-element set is the point: a
        second size at this rate is a violation, and a property that always
        returned two would make that check unfalsifiable."""
        m = Manifest.from_dict(dict(VALID, sample_rate_hz=48000))
        self.assertEqual(m.legal_packet_sizes, {6 * 32})

    def test_fs_legal_packet_sizes_are_unchanged_by_the_hs_correction(self):
        """The FS arithmetic that the UAC1 bench runs on: 44.1 frames of 4 B
        per 1 ms frame, so 176/180 B. Pinned here because the HS fix must not
        drag full speed with it -- an "8000 at both speeds" slip would make
        these 22/24 B and silently reinterpret every UAC1 capture."""
        m = Manifest.from_dict(dict(VALID, audio_class=1, speed="FS",
                                    channels=2, subslot_bytes=2,
                                    max_packet_size_bytes=192))
        self.assertAlmostEqual(m.nominal_frames_per_packet, 44.1, places=6)
        self.assertEqual(m.legal_packet_sizes, {176, 180})

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
        self.assertEqual(Manifest.from_dict(VALID).max_packet_size_bytes, 800)
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
