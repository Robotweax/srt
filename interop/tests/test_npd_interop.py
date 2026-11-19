"""Verify NPD qualification evidence parsing independently of the native codec."""
import contextlib
import io
import json
import struct
import tempfile
from unittest.mock import patch
import unittest
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))

from run_npd_interop import Relay, corpus, events, expected_source_bytes, main


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

    def test_wire_length_oracle_handles_nulls_passthrough_and_partial_messages(self):
        null = bytes.fromhex("471fff10") + b"\xff" * 184
        self.assertEqual(expected_source_bytes(corpus()), 45184)
        self.assertEqual(expected_source_bytes(null * 7), 1)
        self.assertEqual(expected_source_bytes(null * 8), 2)
        self.assertEqual(expected_source_bytes(null * 7 + b"tail"), 6)
        self.assertEqual(expected_source_bytes(b"x" + null[1:]), 189)
        self.assertEqual(expected_source_bytes(null[:3] + b"\x11" + null[4:]), 189)
        with self.assertRaises(ValueError):
            expected_source_bytes(b"")

    def test_released_baseline_compares_each_direction_and_fec_mode(self):
        fields = ("payload_hashes", "extension_records", "source_payload_bytes", "parity_payload_bytes")
        for mismatch in (None, *fields):
            with self.subTest(mismatch=mismatch), tempfile.TemporaryDirectory() as directory:
                root = Path(directory)
                candidate, baseline = root / "candidate", root / "baseline"
                candidate.write_bytes(b"candidate")
                baseline.write_bytes(b"baseline")

                def fake_case(output, name, caller, listener, caller_policy, listener_policy, **kwargs):
                    active = caller_policy == 1
                    result = {"name": name, "payload_hashes": {}, "extension_records": {},
                              "source_payload_bytes": expected_source_bytes(corpus()) if active else len(corpus()),
                              "parity_payload_bytes": (1321 + int(active)) if kwargs.get("fec") else 0}
                    if mismatch and name.endswith("-candidate"):
                        result[mismatch] = "different"
                    return result

                argv = ["run_npd_interop.py", "--robotweax-peer", str(candidate),
                        "--baseline-peer", str(baseline), "--output", str(root)]
                with patch("sys.argv", argv), patch("run_npd_interop.run_case", side_effect=fake_case), contextlib.redirect_stdout(io.StringIO()):
                    if mismatch:
                        with self.assertRaisesRegex(RuntimeError, f"OFF/OFF changed {mismatch}"):
                            main()
                    else:
                        main()
                        report = json.loads((root / "report.json").read_text())
                        self.assertEqual(len(report["cases"]), 16)
                        self.assertIn(str(baseline.resolve()), report["binaries"])
                        names = {case["name"] for case in report["cases"]}
                        for direction in ("forward", "reverse"):
                            for mode in ("arq", "fec"):
                                for revision in ("released", "candidate"):
                                    self.assertIn(f"baseline-{direction}-{mode}-{revision}", names)

    def test_events_ignore_diagnostics_and_nonobjects(self):
        self.assertEqual(events('noise\n[]\n{"event":"ready"}\n'), [{"event": "ready"}])


if __name__ == "__main__":
    unittest.main()
