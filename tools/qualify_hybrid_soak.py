#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Run native hybrid upper-profile soaks and reject incomplete evidence."""
from __future__ import annotations

import argparse
import hashlib
import importlib.util
import json
import os
from pathlib import Path
import platform
import re
import subprocess

ROOT = Path(__file__).resolve().parents[1]
_spec = importlib.util.spec_from_file_location("multicast_qualification", ROOT / "tools/qualify_multicast.py")
_multicast = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(_multicast)
REQUIRED = {"hybrid_soak_network_upper_profile_ctr", "hybrid_soak_network_upper_profile_gcm"}
NUMERIC_FIELDS = "profile seconds loaded_messages loaded_us actual_pps payload_bytes messages_per_receiver data_receivers key_switches km_material_changes samples pacer_resets retained_peak_uc retained_peak_mc retained_final_uc retained_final_mc uc_queue_peak mc_queue_peak uc_capacity_retries fallback_us rejoin_us return_us".split()


def validate(output: str, seconds: int) -> list[dict]:
    cases = _multicast.validate_native(output, REQUIRED)
    if set(cases) != REQUIRED:
        raise ValueError("unexpected native case selection")
    rows = [json.loads(line.removeprefix("[HYBRID_SOAK] ")) for line in output.splitlines()
            if line.startswith("[HYBRID_SOAK] ")]
    if len(rows) != 2 or any(any(type(row.get(k)) is not int or row[k] < 0 for k in NUMERIC_FIELDS) for row in rows):
        raise ValueError("missing or malformed soak metrics")
    if {r["profile"] for r in rows} != {1, 2}:
        raise ValueError("missing crypto profile")
    for row in rows:
        count = seconds * 500
        if (row["seconds"] != seconds or row["loaded_messages"] != count
                or row["loaded_us"] < seconds * 1_000_000 or row["actual_pps"] < 375
                or row["actual_pps"] != count * 1_000_000 // row["loaded_us"]
                or row["payload_bytes"] != 1316 or row["messages_per_receiver"] != count + 16
                or row["data_receivers"] != 2 or row["key_switches"] != (count + 15) // 32
                or row["km_material_changes"] < row["key_switches"]
                or row["samples"] < seconds * 4):
            raise ValueError("incomplete duration, load, DATA or rotation evidence")
        if (max(row["retained_peak_uc"], row["retained_peak_mc"]) > 512
                or max(row["retained_final_uc"], row["retained_final_mc"]) > 16
                or max(row["uc_queue_peak"], row["mc_queue_peak"]) > 64
                or not 0 < row["fallback_us"] < 600_000
                or not row["fallback_us"] <= row["rejoin_us"] < 1_000_000
                or not 0 < row["return_us"] < 600_000):
            raise ValueError("retention, queue or recovery budget exceeded")
    return sorted(rows, key=lambda row: row["profile"])


def require_source_tree(build: Path) -> None:
    cache = (build / "CMakeCache.txt").read_text()
    homes = re.findall(r"^CMAKE_HOME_DIRECTORY:INTERNAL=(.+)$", cache, re.MULTILINE)
    if len(homes) != 1 or Path(homes[0]).resolve() != ROOT.resolve():
        raise ValueError("build directory is not configured from this source checkout")


def bind_build(build: Path, config: str, allow_dirty: bool, run=subprocess.run,
               output=subprocess.check_output) -> dict:
    """Rebuild this checkout, refusing foreign sources or changes during build."""
    require_source_tree(build)
    commit = output(["git", "rev-parse", "HEAD"], cwd=ROOT, text=True).strip()
    status = output(["git", "status", "--porcelain"], cwd=ROOT, text=True)
    if status and not allow_dirty:
        raise ValueError("uncommitted changes: the binary cannot be bound to a commit")
    result = run(["cmake", "--build", str(build), "--config", config,
                  "--target", "robotweax_srt_tests"], cwd=ROOT,
                 stdout=subprocess.DEVNULL, stderr=subprocess.STDOUT)
    if result.returncode:
        raise RuntimeError(f"build exit {result.returncode}: the binary is not up to date")
    require_source_tree(build)
    after_commit = output(["git", "rev-parse", "HEAD"], cwd=ROOT, text=True).strip()
    after_status = output(["git", "status", "--porcelain"], cwd=ROOT, text=True)
    if after_commit != commit or after_status != status:
        raise ValueError("source checkout changed during the build")
    return {"commit": commit, "worktree_dirty": bool(status), "binary_bound_to_commit": not status}


def digest(path: Path) -> str:
    result = hashlib.sha256()
    with path.open("rb") as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b""):
            result.update(block)
    return result.hexdigest()


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--build", required=True, type=Path)
    parser.add_argument("--output", required=True, type=Path)
    parser.add_argument("--seconds", type=int, default=30, help="loaded seconds per crypto profile, 4..300")
    parser.add_argument("--config", default="Release")
    parser.add_argument("--allow-dirty", action="store_true",
                        help="run with uncommitted changes; the record then says the binary is not bound to a commit")
    args = parser.parse_args()
    if not 4 <= args.seconds <= 300:
        parser.error("--seconds must be 4..300")
    build = args.build.resolve()
    args.output.mkdir(parents=True, exist_ok=False)
    record = {"status": "failed", "seconds_per_profile": args.seconds,
              "platform": platform.platform(), "configuration": args.config,
              "binary_bound_to_commit": False,
              "scope": "Two-receiver IPv4 loopback upper-profile soak; one midpoint recovery pause; sampled retention, not a whole-process memory or WAN claim."}
    try:
        record.update(bind_build(build, args.config, args.allow_dirty))
        cache = (build / "CMakeCache.txt").read_text()
        configured = re.search(r"^CMAKE_BUILD_TYPE:STRING=(.+)$", cache, re.MULTILINE)
        if configured:
            record["configuration"] = configured[1]
        for required in ["ROBOTWEAX_SRT_ENABLE_EXPERIMENTAL_ACP1:BOOL=ON",
                         "ROBOTWEAX_SRT_ENABLE_EXPERIMENTAL_HYBRID_CONTROL:BOOL=ON",
                         "ENABLE_AEAD_API_PREVIEW:BOOL=ON"]:
            if required not in cache.splitlines():
                raise ValueError("required configuration missing: " + required)
        backend = re.search(r"^ROBOTWEAX_SRT_CRYPTO_BACKEND:STRING=(.+)$", cache, re.MULTILINE)
        if not backend or backend[1] not in {"openssl", "mbedtls", "bcrypt"}:
            raise ValueError("unsupported crypto backend")
        native = _multicast.binary(build, "", "robotweax_srt_tests", args.config)
        record.update(backend=backend[1], binary=str(native), binary_sha256=digest(native),
                      cache_sha256=digest(build / "CMakeCache.txt"))
        command = [str(native), "--include", "hybrid_soak_network_"]
        record["command"] = command
        log = args.output / "native.log"
        with log.open("w") as output:
            result = subprocess.run(command, cwd=ROOT, stdout=output, stderr=subprocess.STDOUT,
                                    env={**os.environ, "ROBOTWEAX_TEST_HYBRID_SOAK_SECONDS": str(args.seconds)},
                                    timeout=(args.seconds * 8 + 2) // 3 + 30)
        record["log_sha256"] = digest(log)
        if result.returncode:
            raise RuntimeError(f"native exit {result.returncode}; see {log}")
        record["metrics"] = validate(log.read_text(), args.seconds)
        record["status"] = "passed"
    except Exception as error:
        record["error"] = str(error)
        raise
    finally:
        raw_log = args.output / "native.log"
        if raw_log.is_file():
            record["log_sha256"] = digest(raw_log)
        (args.output / "qualification.json").write_text(json.dumps(record, indent=2) + "\n")
    print(json.dumps(record, indent=2))


if __name__ == "__main__":
    main()
