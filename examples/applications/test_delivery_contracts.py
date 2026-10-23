# SPDX-License-Identifier: MIT
"""Keep sample acceptance consistent with reliable and expiring delivery."""
import unittest

from test_applications import validate_summaries


class DeliveryContractTests(unittest.TestCase):
    def setUp(self):
        # Observed in macOS CI: one absent sample, newest sample received, and
        # a receive gap longer than the sample's 100-ms lifetime.
        self.summary = dict(event="summary", channel="sensor", expected=160,
                            unique=159, missing=1, duplicates=0, reordered=0,
                            highest_sequence=159, executed=0,
                            max_receive_gap_ms=306.039083)
        self.stats = [dict(event="stats", channel="sensor", retransmitted=0)]

    def check(self, scenario="device", **changes):
        validate_summaries(scenario, [{**self.summary, **changes}], self.stats)

    def test_expiring_sensor_accepts_observed_loss_and_complete_delivery(self):
        self.check()
        self.check(unique=160, missing=0)

    def test_impaired_sensor_must_observe_loss(self):
        self.check("device-impaired")
        with self.assertRaises(AssertionError):
            self.check("device-impaired", unique=160, missing=0)

    def test_sensor_rejects_blackhole_and_poor_delivery(self):
        for unique in (0, 80):
            with self.subTest(unique=unique), self.assertRaises(AssertionError):
                self.check(unique=unique, missing=160 - unique)

    def test_sensor_rejects_stalled_stream(self):
        with self.assertRaises(AssertionError):
            self.check(unique=100, missing=60, highest_sequence=130)

    def test_sensor_rejects_retransmission_and_absent_statistics(self):
        for stats in ([], [dict(event="stats", channel="sensor", retransmitted=1)]):
            self.stats = stats
            with self.subTest(stats=stats), self.assertRaises(AssertionError):
                self.check()

    def test_corrupt_accounting_and_duplicate_delivery_are_rejected(self):
        for changes in (dict(missing=0), dict(unique=161, missing=-1),
                        dict(highest_sequence=160), dict(duplicates=1)):
            with self.subTest(changes=changes), self.assertRaises(AssertionError):
                self.check(**changes)

    def test_live_and_control_still_require_complete_delivery(self):
        for channel, expected in (("video", 160), ("control", 8)):
            with self.subTest(channel=channel):
                self.check(channel=channel, expected=expected, unique=expected,
                           missing=0, highest_sequence=expected - 1)
                with self.assertRaises(AssertionError):
                    self.check(channel=channel, expected=expected, unique=expected - 1,
                               missing=1, highest_sequence=expected - 1)

    def test_recovery_remains_loss_free(self):
        with self.assertRaises(AssertionError):
            self.check("recovery", channel="recovery")


if __name__ == "__main__":
    unittest.main()
