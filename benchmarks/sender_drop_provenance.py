#!/usr/bin/env python3
"""Bind builder-declared source identities to the actual diagnostic binaries."""

import argparse
import hashlib
import json
from pathlib import Path
import re
import subprocess


def sha256(path):
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def validate(manifest, artifacts, telemetry_source):
    if manifest.get("schemaVersion") != 1:
        raise ValueError("unsupported build manifest schema")
    arguments = []
    for role in ("robotweax", "haivision"):
        identity = manifest[role]
        revision = identity["revision"]
        if not re.fullmatch(r"[0-9a-f]{40}", revision):
            raise ValueError(f"invalid {role} revision")
        for kind in ("library", "peer"):
            expected = identity[f"{kind}Sha256"]
            if (not re.fullmatch(r"[0-9a-f]{64}", expected)
                    or sha256(artifacts[f"{role}-{kind}"]) != expected):
                raise ValueError(f"{role} {kind} does not match build manifest")
        for field, option in (("version", "version"), ("revision", "revision"),
                              ("buildProfile", "build-profile")):
            value = identity[field]
            if not isinstance(value, str) or not value or "\0" in value:
                raise ValueError(f"invalid {role} {field}")
            arguments.extend([f"--{role}-{option}", value])
    revision = manifest["telemetryRevision"]
    if not re.fullmatch(r"[0-9a-f]{40}", revision):
        raise ValueError("invalid telemetry revision")
    def git(*args):
        return subprocess.check_output(
            ["git", "-C", str(telemetry_source), *args], text=True).strip()
    if git("rev-parse", "HEAD") != revision or git("status", "--porcelain"):
        raise ValueError("Telemetry checkout does not match clean manifest revision")
    arguments.extend(["--telemetry-revision", revision])
    return arguments


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("manifest", type=Path)
    for name in ("robotweax-library", "robotweax-peer", "haivision-library",
                 "haivision-peer", "telemetry-source", "output", "args-output"):
        parser.add_argument(f"--{name}", type=Path, required=True)
    args = parser.parse_args()
    try:
        manifest = json.loads(args.manifest.read_text())
        artifacts = {name: getattr(args, name.replace("-", "_"))
                     for name in ("robotweax-library", "robotweax-peer",
                                  "haivision-library", "haivision-peer")}
        arguments = validate(manifest, artifacts, args.telemetry_source)
        # Preserve the declarations and state exactly what was verified.
        args.output.write_text(json.dumps({
            "schemaVersion": 1,
            "builderManifest": manifest,
            "artifactHashesVerified": True,
            "telemetryCheckoutVerified": True,
            "sourceToBinaryMapping": "builder-declared; not independently reproduced",
        }, indent=2) + "\n")
        args.args_output.write_bytes(b"\0".join(
            value.encode() for value in arguments) + b"\0")
    except (OSError, ValueError, KeyError, TypeError, subprocess.CalledProcessError) as error:
        parser.exit(1, f"Invalid build provenance: {error}\n")


if __name__ == "__main__":
    main()
