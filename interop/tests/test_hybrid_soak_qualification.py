# SPDX-License-Identifier: MIT
from __future__ import annotations
import copy
import importlib.util
import json
from pathlib import Path
import unittest
import tempfile
from unittest.mock import Mock

ROOT = Path(__file__).resolve().parents[2]
spec = importlib.util.spec_from_file_location("hybrid_soak_qualification", ROOT / "tools/qualify_hybrid_soak.py")
qualification = importlib.util.module_from_spec(spec)
spec.loader.exec_module(qualification)


class HybridSoakQualificationTests(unittest.TestCase):
    def rows(self):
        return [dict(profile=profile, seconds=4, loaded_messages=2000, loaded_us=4_001_000,
                     actual_pps=499, payload_bytes=1316, messages_per_receiver=2016,
                     data_receivers=2, key_switches=62, km_material_changes=63, samples=18,
                     pacer_resets=0, retained_peak_uc=90, retained_peak_mc=120,
                     retained_final_uc=0, retained_final_mc=0, uc_queue_peak=64,
                     mc_queue_peak=25, uc_capacity_retries=15, fallback_us=170_000,
                     rejoin_us=195_000, return_us=310_000) for profile in [1, 2]]

    def transcript(self, rows):
        return ("".join(f"[RUN] {name}\n[PASS] {name}\n" for name in sorted(qualification.REQUIRED))
                + "".join("[HYBRID_SOAK] " + json.dumps(row) + "\n" for row in rows)
                + "2/2 tests passed\n")

    def binding(self, status: str, build_exit: int = 0, allow_dirty: bool = False,
                foreign: bool = False, changed: bool = False):
        calls = []

        revisions = iter(["0123abcd\n", "changed\n" if changed else "0123abcd\n"])

        def output(command, **_):
            return next(revisions) if command[1] == "rev-parse" else status

        def run(command, **_):
            calls.append(command)
            return type("Result", (), {"returncode": build_exit})()

        with tempfile.TemporaryDirectory() as directory:
            build = Path(directory)
            source = build if foreign else ROOT
            (build / "CMakeCache.txt").write_text(f"CMAKE_HOME_DIRECTORY:INTERNAL={source}\n")
            record = qualification.bind_build(build, "Release", allow_dirty,
                                              run=run, output=output)
            for call in calls:
                call[2] = "build-x"
            return record, calls

    def test_binds_an_up_to_date_binary_of_a_clean_worktree_to_the_commit(self):
        record, calls = self.binding("")
        self.assertEqual(record, {"commit": "0123abcd", "worktree_dirty": False,
                                  "binary_bound_to_commit": True})
        self.assertEqual(calls, [["cmake", "--build", "build-x", "--config", "Release",
                                  "--target", "robotweax_srt_tests"]])

    def test_refuses_uncommitted_changes_and_a_failed_build(self):
        with self.assertRaises(ValueError):
            self.binding(" M src/session.cpp\n")
        with self.assertRaises(RuntimeError):
            self.binding("", build_exit=2)

    def test_allowed_uncommitted_changes_are_recorded_as_not_bound(self):
        record, _ = self.binding(" M src/session.cpp\n", allow_dirty=True)
        self.assertEqual(record, {"commit": "0123abcd", "worktree_dirty": True,
                                  "binary_bound_to_commit": False})

    def test_rejects_foreign_source_even_when_dirty_builds_are_allowed(self):
        for allow_dirty in (False, True):
            with self.subTest(allow_dirty=allow_dirty), self.assertRaisesRegex(ValueError, "source checkout"):
                self.binding("", foreign=True, allow_dirty=allow_dirty)

    def test_rejects_checkout_changes_during_build(self):
        with self.assertRaisesRegex(ValueError, "changed during"):
            self.binding("", changed=True)

    def test_missing_or_ambiguous_source_identity_never_starts_build(self):
        with tempfile.TemporaryDirectory() as directory:
            build = Path(directory)
            for cache in ("", f"CMAKE_HOME_DIRECTORY:INTERNAL={ROOT}\n" * 2):
                (build / "CMakeCache.txt").write_text(cache)
                run = Mock()
                with self.assertRaisesRegex(ValueError, "source checkout"):
                    qualification.bind_build(build, "Release", False, run=run)
                run.assert_not_called()

    def test_accepts_complete_bounded_capacity_run(self):
        self.assertEqual(qualification.validate(self.transcript(self.rows()), 4), self.rows())

    def test_rejects_skips_subsets_and_duplicate_profiles(self):
        valid = self.transcript(self.rows())
        for output in [valid + "[SKIP] unavailable\n", valid.replace("2/2 tests passed", "1/2 tests passed"),
                       self.transcript(self.rows()[:1]), self.transcript([self.rows()[0]] * 2)]:
            with self.subTest(output=output), self.assertRaises(ValueError):
                qualification.validate(output, 4)

    def test_rejects_short_slow_or_incomplete_delivery(self):
        for key, value in [("loaded_us", 1_000_000), ("actual_pps", 300),
                           ("loaded_messages", 1999), ("messages_per_receiver", 2015),
                           ("key_switches", 5), ("samples", 2), ("payload_bytes", 1)]:
            rows = copy.deepcopy(self.rows())
            rows[0][key] = value
            with self.subTest(key=key), self.assertRaises(ValueError):
                qualification.validate(self.transcript(rows), 4)
        with self.assertRaises(ValueError):
            qualification.validate(self.transcript(self.rows()), 30)

    def test_rejects_retention_and_recovery_budget_violations(self):
        for key, value in [("retained_peak_uc", 513), ("retained_final_mc", 17),
                           ("uc_queue_peak", 65), ("fallback_us", 600_000),
                           ("rejoin_us", 1_000_000), ("return_us", 600_000)]:
            rows = copy.deepcopy(self.rows())
            rows[0][key] = value
            with self.subTest(key=key), self.assertRaises(ValueError):
                qualification.validate(self.transcript(rows), 4)
