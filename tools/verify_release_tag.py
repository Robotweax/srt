#!/usr/bin/env python3
"""Fail closed unless a GitHub release tag still names the tested commit."""
from __future__ import annotations

import argparse
import json
import re
import subprocess
from urllib.parse import quote


def github_object(repo: str, path: str) -> dict:
    result = subprocess.run(
        ["gh", "api", f"repos/{repo}/{path}"],
        check=True, capture_output=True, text=True, timeout=30,
    )
    return json.loads(result.stdout)


def verify(repo: str, tag: str, expected_commit: str, expected_object: str | None = None) -> dict:
    if not re.fullmatch(r"[A-Za-z0-9_.-]+/[A-Za-z0-9_.-]+", repo):
        raise ValueError("invalid repository")
    if not re.fullmatch(r"v[0-9]+\.[0-9]+\.[0-9]+", tag):
        raise ValueError("invalid release tag")
    if not re.fullmatch(r"[0-9a-f]{40}", expected_commit):
        raise ValueError("invalid expected commit")
    obj = github_object(repo, f"git/ref/tags/{quote(tag, safe='')}")["object"]
    tag_object = obj["sha"]
    if expected_object is not None and tag_object != expected_object:
        raise ValueError("release tag object changed")
    for _ in range(8):
        if not re.fullmatch(r"[0-9a-f]{40}", obj["sha"]):
            raise ValueError("invalid tag object SHA")
        if obj["type"] == "commit":
            if obj["sha"] != expected_commit:
                raise ValueError("release tag no longer points to tested commit")
            return {"tag": tag, "object": tag_object, "commit": obj["sha"]}
        if obj["type"] != "tag":
            raise ValueError("release tag does not reference a commit")
        obj = github_object(repo, f"git/tags/{obj['sha']}")["object"]
    raise ValueError("excessive annotated tag nesting")


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--repo", required=True)
    parser.add_argument("--tag", required=True)
    parser.add_argument("--commit", required=True)
    parser.add_argument("--object")
    args = parser.parse_args()
    print(json.dumps(verify(args.repo, args.tag, args.commit, args.object)))


if __name__ == "__main__":
    main()
