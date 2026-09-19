from __future__ import annotations

import copy
import json
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path
from unittest import mock

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "benchmarks"))
import analyze_sender_drop_trace as trace
import sender_drop_provenance as provenance


class DropMappingTests(unittest.TestCase):
    def setUp(self):
        self.submissions = [dict(sequence=i, monotonicNs=10 + i) for i in (0, 1)]
        self.drops = [dict(sequence=i, monotonicNs=100 + i,
                           protocolNowUs=1_020_010, enqueueUs=10,
                           cutoffUs=10, ageUs=1_020_000, payloadBytes=1316)
                      for i in (0, 1)]
        self.counters = [dict(firstSequence=0, lastSequence=1, packetDelta=2,
                              payloadByteDelta=2632, monotonicNs=102,
                              protocolNowUs=1_020_010)]

    def check(self):
        trace.validate_drop_mapping(self.submissions, self.drops, self.counters, "fixture")

    def test_valid_exact_mapping(self):
        self.check()

    def test_submit_must_precede_drop(self):
        for timestamp in (0, 100, 101):
            with self.subTest(timestamp=timestamp):
                self.submissions[0]["monotonicNs"] = timestamp
                with self.assertRaisesRegex(SystemExit, "prior UDP submission"):
                    self.check()

    def test_missing_submission(self):
        self.submissions.pop()
        with self.assertRaisesRegex(SystemExit, "prior UDP submission"):
            self.check()

    def test_duplicate_removal(self):
        self.drops.append(self.drops[0].copy())
        with self.assertRaisesRegex(SystemExit, "duplicate"):
            self.check()

    def test_wrong_counter_mapping_despite_same_total(self):
        for changes in (
            dict(firstSequence=5, lastSequence=6),
            dict(packetDelta=1), dict(payloadByteDelta=2631),
            dict(monotonicNs=99), dict(protocolNowUs=1_020_011),
            dict(firstSequence=-1),
        ):
            with self.subTest(changes=changes):
                original = self.counters[0].copy()
                self.counters[0].update(changes)
                with self.assertRaises(SystemExit):
                    self.check()
                self.counters[0] = original

    def test_overlapping_counter_ranges(self):
        self.counters = [dict(firstSequence=0, lastSequence=0, packetDelta=1,
                              payloadByteDelta=1316, monotonicNs=102,
                              protocolNowUs=1_020_010)] * 2
        with self.assertRaisesRegex(SystemExit, "overlapping"):
            self.check()

    def test_missing_counter(self):
        self.counters.clear()
        with self.assertRaisesRegex(SystemExit, "coverage"):
            self.check()

    def test_sequence_wrap(self):
        for events in (self.submissions, self.drops):
            events[0]["sequence"] = (1 << 31) - 1
            events[1]["sequence"] = 0
        self.counters[0].update(firstSequence=(1 << 31) - 1, lastSequence=0)
        self.check()

    def test_multiple_polls(self):
        self.drops[1]["protocolNowUs"] += 1
        self.drops[1]["ageUs"] += 1
        self.counters = [dict(firstSequence=i, lastSequence=i, packetDelta=1,
                              payloadByteDelta=1316, monotonicNs=102 + i,
                              protocolNowUs=1_020_010 + i) for i in (0, 1)]
        self.check()

    def test_inconsistent_age_or_cutoff(self):
        for key, value in (("ageUs", 1_020_001), ("cutoffUs", 9)):
            with self.subTest(key=key):
                original = self.drops[0][key]
                self.drops[0][key] = value
                with self.assertRaisesRegex(SystemExit, "age/cutoff"):
                    self.check()
                self.drops[0][key] = original

    def test_zero_drop_control(self):
        trace.validate_drop_mapping(self.submissions, [], [], "control")


class ProvenanceTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.artifacts = {}
        self.manifest = {"schemaVersion": 1, "telemetryRevision": "c" * 40}
        for role in ("robotweax", "haivision"):
            identity = dict(revision="a" * 40, version="test-build",
                            buildProfile="linux-x64-test")
            for kind in ("library", "peer"):
                path = self.root / f"{role}-{kind}"
                path.write_bytes(f"{role}-{kind}-binary".encode())
                self.artifacts[f"{role}-{kind}"] = path
                identity[f"{kind}Sha256"] = provenance.sha256(path)
            self.manifest[role] = identity

    def validate(self):
        return provenance.validate(self.manifest, self.artifacts, self.root)

    def test_valid_manifest_supplies_declared_identity(self):
        with mock.patch.object(provenance.subprocess, "check_output",
                               side_effect=["c" * 40, ""]):
            args = self.validate()
        self.assertEqual(args[args.index("--robotweax-revision") + 1], "a" * 40)
        self.assertIn("linux-x64-test", args)

    def test_each_changed_binary_is_rejected(self):
        for name, path in self.artifacts.items():
            with self.subTest(name=name):
                data = path.read_bytes()
                path.write_bytes(data + b"changed")
                with self.assertRaisesRegex(ValueError, "does not match"):
                    self.validate()
                path.write_bytes(data)

    def test_missing_hash_or_invalid_revision_rejected(self):
        for changes in ({"librarySha256": "bad"}, {"revision": "main"}):
            original = copy.deepcopy(self.manifest)
            self.manifest["robotweax"].update(changes)
            with self.assertRaises(ValueError):
                self.validate()
            self.manifest = original

    def test_wrong_or_dirty_telemetry_checkout_rejected(self):
        for outputs in (["d" * 40], ["c" * 40, " M source.cpp"]):
            with mock.patch.object(provenance.subprocess, "check_output", side_effect=outputs):
                with self.assertRaisesRegex(ValueError, "Telemetry checkout"):
                    self.validate()

    def test_cli_failure_writes_no_verified_evidence(self):
        manifest = self.root / "manifest.json"
        manifest.write_text(json.dumps(self.manifest))
        self.artifacts["robotweax-peer"].write_bytes(b"different binary")
        output, args_output = self.root / "evidence.json", self.root / "args.bin"
        argv = [sys.executable, provenance.__file__, str(manifest)]
        for name, path in self.artifacts.items():
            argv.extend(["--" + name, str(path)])
        argv.extend(["--telemetry-source", str(self.root), "--output", str(output),
                     "--args-output", str(args_output)])
        result = subprocess.run(argv, capture_output=True, text=True)
        self.assertNotEqual(result.returncode, 0)
        self.assertFalse(output.exists())
        self.assertFalse(args_output.exists())


if __name__ == "__main__":
    unittest.main()
