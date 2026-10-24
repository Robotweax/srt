#!/usr/bin/env python3
"""Collect only text crash reports matching failed qualification endpoint PIDs."""
from pathlib import Path
import json
import re
import shutil
import sys
import time

root = Path(sys.argv[1])
statuses = [json.loads(p.read_text()) for p in root.glob("*-endpoint-status.json")]
known = {s[k] for s in statuses for k in ("desktop_pid", "receiver_pid")}
if not known:
    raise SystemExit(0)
cutoff = min(s["observed_at"] for s in statuses) - 60
report_roots = [Path.home() / "Library/Logs/DiagnosticReports", Path("/Library/Logs/DiagnosticReports")]
report_roots += list(root.glob("*/home/Library/Logs/DiagnosticReports"))
copied = []
for attempt in range(11):
    for directory in report_roots:
        for pattern in ("OBS*.ips", "macos-reference-peer*.ips"):
            for report in directory.glob(pattern):
                try:
                    if report.is_symlink() or report.stat().st_mtime < cutoff or report.stat().st_size > 8 * 1024 * 1024:
                        continue
                    text = report.read_text(errors="replace")
                    if not any(re.search(r'"(?:pid|procPid)"\s*:\s*' + str(pid) + r'\b', text) for pid in known):
                        continue
                    destination = root / report.name
                    if destination.name not in copied and len(copied) < 4:
                        shutil.copyfile(report, destination)
                        copied.append(destination.name)
                except OSError:
                    continue
    if copied or attempt == 10:
        break
    time.sleep(1)
for log in root.glob("*/home/Library/Application Support/obs-studio/logs/*.txt"):
    destination = root / (log.parents[5].name + "-failure-profile-obs.log")
    shutil.copyfile(log, destination)
(root / "failure-report-inventory.json").write_text(json.dumps({"endpoint_pids": sorted(known), "reports": copied}, indent=2) + "\n")
