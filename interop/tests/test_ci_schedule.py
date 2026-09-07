from __future__ import annotations

import copy
from datetime import datetime, timedelta, timezone
import io
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import textwrap
import unittest
from unittest.mock import patch
from contextlib import redirect_stdout, redirect_stderr

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))

import ci_schedule
from ci_changes import classify
from test_ci_gate import job_blocks


ROOT = Path(__file__).resolve().parents[2]
NOW = datetime(2026, 9, 7, 12, tzinfo=timezone.utc)
SHA = "a" * 40


class ScheduledEvidenceTests(unittest.TestCase):
    def setUp(self):
        self.current = dict(id=20, workflow_id=7, head_sha=SHA,
                            head_branch="main", event="schedule")
        self.run = dict(self.current, id=19, event="push", status="completed",
                        conclusion="success", created_at="2026-09-07T11:00:00Z",
                        head_repository={"full_name": "Robotweax/srt"}, run_attempt=1)
        self.gate = dict(name="Required CI gate", status="completed",
                         conclusion="success", steps=[dict(
                             name=ci_schedule.FULL_MARKER,
                             status="completed", conclusion="success")])
        self.jobs = [self.gate]
        self.refreshed = None
        self.calls = []

    def api(self, endpoint):
        self.calls.append(endpoint)
        if endpoint.endswith("/runs/20"):
            return self.current
        if "/workflows/" in endpoint:
            return {"workflow_runs": [self.run]}
        if "/jobs?" in endpoint:
            page = int(endpoint.rsplit("=", 1)[1])
            return {"jobs": self.jobs[(page - 1) * 100:page * 100],
                    "total_count": len(self.jobs)}
        if endpoint.endswith("/runs/19"):
            return self.refreshed or self.run
        self.fail(endpoint)

    def find(self):
        return ci_schedule.find_evidence(self.api, "Robotweax/srt", 20, SHA, NOW)

    def test_recent_full_main_run_qualifies_for_all_trusted_events(self):
        for event in ("push", "schedule", "workflow_dispatch"):
            self.run["event"] = event
            self.assertEqual(self.find(), 19)

    def test_partial_failed_pending_foreign_and_stale_runs_do_not_qualify(self):
        original = copy.deepcopy(self.run)
        changes = [dict(id=20), dict(workflow_id=8), dict(head_sha="b" * 40),
                   dict(head_branch="topic"), dict(event="pull_request"),
                   dict(head_repository={"full_name": "fork/srt"}),
                   dict(status="in_progress"), dict(conclusion="failure"),
                   dict(conclusion="cancelled"),
                   dict(created_at="2026-09-06T11:59:59Z"),
                   dict(created_at="2026-09-07T12:00:01Z")]
        for change in changes:
            with self.subTest(change=change):
                self.run = dict(original, **change)
                self.assertIsNone(self.find())

    def test_24_hour_boundary_and_no_window_renewal_by_skipped_marker(self):
        self.run["created_at"] = (NOW - timedelta(hours=24)).isoformat()
        self.assertEqual(self.find(), 19)
        self.gate["steps"][0]["conclusion"] = "skipped"
        self.assertIsNone(self.find())

    def test_missing_ambiguous_or_failed_gate_and_marker_reject_evidence(self):
        original = copy.deepcopy(self.gate)
        variants = [[], [original, original],
                    [dict(original, conclusion="failure")],
                    [dict(original, steps=[])],
                    [dict(original, steps=original["steps"] * 2)],
                    [dict(original, steps=[dict(original["steps"][0],
                                               conclusion="failure")])]]
        for jobs in variants:
            with self.subTest(jobs=jobs):
                self.jobs = jobs
                self.assertIsNone(self.find())

    def test_jobs_pagination_and_latest_attempt_filter(self):
        self.jobs = [dict(name="other")] * 100 + [self.gate]
        self.assertEqual(self.find(), 19)
        self.assertTrue(any("filter=latest&per_page=100&page=2" in x
                            for x in self.calls))
        self.jobs += [dict(name="other")] * 100
        self.assertIsNone(self.find())

    def test_restarted_run_cannot_authorize_skip(self):
        self.refreshed = dict(self.run, run_attempt=2)
        self.assertIsNone(self.find())
        self.refreshed = dict(self.run, conclusion=None, status="queued")
        self.assertIsNone(self.find())

    def test_current_event_sha_and_branch_are_verified(self):
        for field, value in (("event", "workflow_dispatch"),
                             ("head_sha", "b" * 40), ("head_branch", "topic")):
            with self.subTest(field=field):
                original = self.current[field]
                self.current[field] = value
                self.assertIsNone(self.find())
                self.current[field] = original

    def test_api_or_metadata_errors_keep_full_ci_without_leaking_stderr(self):
        env = dict(GITHUB_EVENT_NAME="schedule", GITHUB_REPOSITORY="Robotweax/srt",
                   GITHUB_RUN_ID="20", GITHUB_SHA=SHA)
        for error in (KeyError("missing"), ValueError("bad JSON"),
                      subprocess.TimeoutExpired("gh", 10),
                      subprocess.CalledProcessError(1, "gh", stderr="secret")):
            with patch.dict(os.environ, env, clear=True), patch(
                    "ci_schedule.find_evidence", side_effect=error):
                out, err = io.StringIO(), io.StringIO()
                with redirect_stdout(out), redirect_stderr(err):
                    self.assertEqual(ci_schedule.main(), 0)
                self.assertEqual(out.getvalue(), "evidence_run_id=\n")
                self.assertNotIn("secret", err.getvalue())

    def test_non_schedule_events_never_query_api(self):
        for event in ("push", "pull_request", "workflow_dispatch"):
            with patch.dict(os.environ, {"GITHUB_EVENT_NAME": event}, clear=True), \
                    patch("ci_schedule.find_evidence") as find, \
                    redirect_stdout(io.StringIO()):
                self.assertEqual(ci_schedule.main(), 0)
                find.assert_not_called()

    def test_successful_lookup_emits_only_numeric_evidence_id(self):
        env = dict(GITHUB_EVENT_NAME="schedule", GITHUB_REPOSITORY="Robotweax/srt",
                   GITHUB_RUN_ID="20", GITHUB_SHA=SHA)
        with patch.dict(os.environ, env, clear=True), patch(
                "ci_schedule.find_evidence", return_value=19):
            out = io.StringIO()
            with redirect_stdout(out):
                self.assertEqual(ci_schedule.main(), 0)
            self.assertEqual(out.getvalue(), "evidence_run_id=19\n")


class ScheduledWorkflowTests(unittest.TestCase):
    def setUp(self):
        self.workflow = (ROOT / ".github/workflows/ci.yml").read_text()

    def test_evidence_marker_is_bound_to_full_selection_and_strict_gate(self):
        gate = job_blocks(self.workflow)["ci_gate"]
        self.assertIn(f"- name: {ci_schedule.FULL_MARKER}\n"
                      "        if: needs.changes.outputs.full == 'true'\n"
                      "        run: ':'", gate)
        self.assertIn("- name: Require every selected job to pass", gate)
        check = self.workflow.split("      - name: Check recent full CI evidence\n")[1]
        self.assertIn("if: github.event_name == 'schedule'", check.split(
            "      - name:")[0])

    def test_scheduler_logic_changes_select_full_ci(self):
        for path in ("interop/ci_schedule.py", "interop/tests/test_ci_schedule.py"):
            self.assertTrue(classify([path]).full)

    def test_actual_classification_shell_skips_only_schedule_with_evidence(self):
        step = self.workflow.split("      - name: Classify changed paths\n", 1)[1]
        script = textwrap.dedent(step.split("        run: |\n", 1)[1].split(
            "\n  dco:", 1)[0])
        for event, evidence, full in (("schedule", "19", False),
                                      ("schedule", "", True),
                                      ("schedule", "invalid", True),
                                      ("push", "19", True),
                                      ("pull_request", "19", True),
                                      ("workflow_dispatch", "19", True)):
            with self.subTest(event=event, evidence=evidence), \
                    tempfile.TemporaryDirectory() as directory:
                out = Path(directory) / "output"
                summary = Path(directory) / "summary"
                result = subprocess.run(["bash", "-c", script], cwd=ROOT,
                    env={**os.environ, "EVENT_NAME": event, "EVIDENCE_RUN_ID": evidence,
                         "RUNNER_TEMP": directory, "GITHUB_OUTPUT": str(out),
                         "GITHUB_STEP_SUMMARY": str(summary),
                         "GITHUB_SERVER_URL": "https://github.com",
                         "BASE_SHA": "0" * 40, "HEAD_SHA": "HEAD",
                         "GITHUB_REPOSITORY": "Robotweax/srt"},
                    capture_output=True, text=True, timeout=10)
                self.assertEqual(result.returncode, 0, result.stderr)
                flags = dict(line.split("=", 1) for line in out.read_text().splitlines())
                self.assertEqual(flags["full"], str(full).lower())
                self.assertEqual(flags["interop"], str(full).lower())
                if not full:
                    self.assertIn("actions/runs/19", summary.read_text())
                    self.assertEqual(flags["core_tests"], "false")
                else:
                    self.assertNotIn("duplicate omitted", summary.read_text())


if __name__ == "__main__":
    unittest.main()
