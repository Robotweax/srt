"""Verify NPD qualification evidence parsing independently of the native codec."""
import struct
import unittest
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))

from run_npd_interop import Relay, corpus, events


class NpdEvidenceTests(unittest.TestCase):
    def test_corpus_has_exact_message_boundaries_and_noncanonical_nulls(self):
        data = corpus()
        self.assertEqual(len(data), 64 * 1316)
        self.assertEqual(data[:4], bytes.fromhex("471fff10"))
        self.assertEqual(data[3 * 1316:3 * 1316 + 4], bytes.fromhex("471fff11"))

    def test_summary_separates_source_parity_retransmission_and_rejection(self):
        relay = Relay(12345)
        try:
            source = struct.pack("!4I", 10, 1, 0, 0) + b"\x7f"
            parity = struct.pack("!4I", 11, 0, 0, 0) + bytes(1321)
            rejection = struct.pack("!4I", 0x80000000, 0, 0, 0)
            rejection += struct.pack("!12I", 5, 0, 0, 1500, 25600, 1004, 20, 123, 0, 0, 0, 0)
            relay.packets = [("caller", source), ("caller", source),
                             ("caller", parity), ("listener", rejection)]
            result = relay.summary()
            self.assertEqual(result["source_payload_bytes"], 1)
            self.assertEqual(result["parity_payload_bytes"], 1321)
            self.assertEqual(result["rejections"]["listener"], [1004])
            self.assertEqual(len(result["payload_hashes"]["caller"]), 1)
        finally:
            relay.socket.close()

    def test_events_ignore_diagnostics_and_nonobjects(self):
        self.assertEqual(events('noise\n[]\n{"event":"ready"}\n'), [{"event": "ready"}])


if __name__ == "__main__":
    unittest.main()
