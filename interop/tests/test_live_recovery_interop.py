from __future__ import annotations

import copy
from pathlib import Path
import sys
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
import run_live_recovery_interop as fixture


class LiveRecoveryContractTests(unittest.TestCase):
    def gap(self):
        return [{"action": "drop", "packet_kind": "data", "occurrence": 599,
                 "payload_bytes": 1200, "retransmission_observed": True,
                 "retransmission_flag": True, "cumulative_ack_observed": True,
                 "later_data_observed_before_retransmission": True,
                 "loss_report_observed": True, "loss_report_relay_ordinal": 10,
                 "retransmission_relay_ordinal": 11}]

    def validate_gap(self, faults):
        return fixture.validate_recovery('gap', faults, 1, 600, 1200, 1, [{}])

    def test_requires_nak_before_retransmission_and_later_original_data(self):
        self.assertTrue(self.validate_gap(self.gap()))
        for key, value in (("loss_report_observed", False),
                           ("loss_report_relay_ordinal", 12),
                           ("retransmission_relay_ordinal", None),
                           ("later_data_observed_before_retransmission", False),
                           ("cumulative_ack_observed", False),
                           ("retransmission_flag", False)):
            with self.subTest(key=key):
                faults = copy.deepcopy(self.gap())
                faults[0][key] = value
                self.assertFalse(self.validate_gap(faults))

    def test_cannot_use_final_packet_loss_as_nak_recovery_evidence(self):
        faults = self.gap()
        faults[0]['occurrence'] = 600
        self.assertFalse(self.validate_gap(faults))
        self.assertFalse(self.validate_gap([]))
        self.assertFalse(self.validate_gap(self.gap() + self.gap()))

    def test_delay_must_exceed_rto_without_retransmissions_or_naks(self):
        faults = [{"action": "delay", "packet_kind": "control",
                   "delay_elapsed_milliseconds": 500,
                   "delayed_datagrams": 1}]
        self.assertTrue(fixture.validate_recovery('delayed-ack', faults, 1, 600, 1200, 0, []))
        self.assertFalse(fixture.validate_recovery('delayed-ack', faults, 1, 600, 1200, 1, []))
        self.assertFalse(fixture.validate_recovery('delayed-ack', faults, 1, 600, 1200, 0, [{}]))
        faults[0]['delay_elapsed_milliseconds'] = 100
        self.assertFalse(fixture.validate_recovery('delayed-ack', faults, 1, 600, 1200, 0, []))
        faults[0]['delay_elapsed_milliseconds'] = 500
        faults[0]['delayed_datagrams'] = 0
        self.assertFalse(fixture.validate_recovery('delayed-ack', faults, 1, 600, 1200, 0, []))


if __name__ == '__main__':
    unittest.main()
