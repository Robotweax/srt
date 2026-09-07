#!/usr/bin/env python3
"""Reuse recent, successful full CI evidence for scheduled runs only.

Missing, ambiguous, expired, or inaccessible evidence always means full CI.
No artifacts, caches, PR checks, or merely selected partial runs are trusted.
"""

from __future__ import annotations

from datetime import datetime, timedelta, timezone
import json
import os
import subprocess
import sys
import time
from urllib.parse import urlencode


FULL_MARKER = "Full validation selected (v1)"
MAX_AGE = timedelta(hours=24)


def successful(item: dict) -> bool:
    return item.get("status") == "completed" and item.get("conclusion") == "success"


def candidate(run: dict, current: dict, repository: str, now: datetime) -> bool:
    created = datetime.fromisoformat(run["created_at"].replace("Z", "+00:00"))
    return (
        run["id"] != current["id"]
        and run["workflow_id"] == current["workflow_id"]
        and run["head_sha"] == current["head_sha"]
        and run["head_branch"] == "main"
        and run["head_repository"]["full_name"] == repository
        and run["event"] in {"push", "schedule", "workflow_dispatch"}
        and successful(run)
        and now - MAX_AGE <= created <= now
    )


def full_evidence(jobs: list[dict]) -> bool:
    gates = [job for job in jobs if job.get("name") == "Required CI gate"]
    if len(gates) != 1 or not successful(gates[0]):
        return False
    markers = [step for step in gates[0].get("steps", [])
               if step.get("name") == FULL_MARKER]
    return len(markers) == 1 and successful(markers[0])


def find_evidence(api, repository: str, run_id: int, sha: str,
                  now: datetime) -> int | None:
    prefix = f"repos/{repository}/actions"
    current = api(f"{prefix}/runs/{run_id}")
    if (current["event"] != "schedule" or current["head_branch"] != "main"
            or current["head_sha"] != sha or current["id"] != run_id):
        return None
    query = urlencode({"head_sha": sha, "branch": "main", "per_page": 100})
    runs = api(f"{prefix}/workflows/{current['workflow_id']}/runs?{query}")
    # A bounded search may miss evidence, but can never authorize a skip on
    # incomplete evidence. Older runs and excess candidates simply cost CI.
    eligible = [run for run in runs["workflow_runs"]
                if candidate(run, current, repository, now)]
    for run in eligible[:3]:
        jobs = []
        for page in (1, 2):
            data = api(f"{prefix}/runs/{run['id']}/jobs"
                       f"?filter=latest&per_page=100&page={page}")
            jobs.extend(data["jobs"])
            if len(jobs) >= data["total_count"]:
                break
        else:
            continue
        if not full_evidence(jobs):
            continue
        # Do not reuse a run that was restarted during the evidence lookup.
        refreshed = api(f"{prefix}/runs/{run['id']}")
        if (candidate(refreshed, current, repository, now)
                and refreshed["run_attempt"] == run["run_attempt"]):
            return int(run["id"])
    return None


def main() -> int:
    evidence = None
    if os.environ.get("GITHUB_EVENT_NAME") == "schedule":
        deadline = time.monotonic() + 60

        def api(endpoint: str):
            remaining = deadline - time.monotonic()
            if remaining <= 0:
                raise TimeoutError("evidence lookup budget exhausted")
            result = subprocess.run(
                ["gh", "api", endpoint], check=True, capture_output=True,
                text=True, timeout=min(10, remaining),
            )
            return json.loads(result.stdout)

        try:
            evidence = find_evidence(
                api, os.environ["GITHUB_REPOSITORY"],
                int(os.environ["GITHUB_RUN_ID"]), os.environ["GITHUB_SHA"],
                datetime.now(timezone.utc),
            )
        except (KeyError, ValueError, TypeError, AttributeError, OSError, TimeoutError,
                subprocess.SubprocessError):
            # Do not print API bodies, subprocess stderr, or credentials.
            print("Full CI retained: recent full evidence unavailable or invalid.",
                  file=sys.stderr)
    print(f"evidence_run_id={evidence or ''}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
