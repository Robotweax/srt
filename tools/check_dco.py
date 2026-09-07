#!/usr/bin/env python3
"""Verify DCO sign-offs for every commit in a Git revision range."""

from __future__ import annotations

import argparse
import re
import subprocess
import sys
from dataclasses import dataclass


SIGNOFF_PATTERN = re.compile(
    r"^Signed-off-by:\s*(?P<name>[^<\r\n]+?)\s*"
    r"<(?P<email>[^<>\s]+)>\s*$",
    re.IGNORECASE,
)


@dataclass(frozen=True)
class Commit:
    oid: str
    author_name: str
    author_email: str
    message: str


def _normalized_name(value: str) -> str:
    return " ".join(value.split()).casefold()


def _normalized_email(value: str) -> str:
    return value.strip().casefold()


def is_dependabot(commit: Commit) -> bool:
    name = _normalized_name(commit.author_name)
    email = _normalized_email(commit.author_email)
    recognized_email = email.endswith(
        "+dependabot[bot]@users.noreply.github.com"
    ) or email == "dependabot[bot]@users.noreply.github.com"
    return name == "dependabot[bot]" and recognized_email


def signoff_identities(message: str) -> list[tuple[str, str]]:
    """Return valid identities from the message's final Git trailer block."""
    completed = subprocess.run(
        ["git", "interpret-trailers", "--parse"],
        check=True,
        input=message,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
    )
    identities: list[tuple[str, str]] = []
    for line in completed.stdout.splitlines():
        match = SIGNOFF_PATTERN.fullmatch(line)
        if match is not None:
            identities.append((match.group("name"), match.group("email")))
    return identities


def validation_error(
    commit: Commit, *, allow_dependabot: bool = False
) -> str | None:
    """Return a human-readable DCO error, or None when the commit passes."""
    if allow_dependabot and is_dependabot(commit):
        return None

    signoffs = signoff_identities(commit.message)
    if not signoffs:
        return "missing a valid Signed-off-by: Name <email> trailer"

    author_name = _normalized_name(commit.author_name)
    author_email = _normalized_email(commit.author_email)
    for name, email in signoffs:
        if (
            _normalized_name(name) == author_name
            and _normalized_email(email) == author_email
        ):
            return None
    return "has no Signed-off-by trailer matching the commit author"


def _git(*arguments: str) -> str:
    completed = subprocess.run(
        ["git", *arguments],
        check=True,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
    )
    return completed.stdout


def commits_in_range(base: str, head: str) -> list[Commit]:
    object_ids = [
        line
        for line in _git("rev-list", "--reverse", f"{base}..{head}").splitlines()
        if line
    ]
    commits: list[Commit] = []
    for object_id in object_ids:
        metadata = _git(
            "show",
            "--no-patch",
            "--format=%an%x00%ae%x00%B",
            object_id,
        )
        fields = metadata.split("\x00", 2)
        if len(fields) != 3:
            raise RuntimeError(f"could not parse commit metadata for {object_id}")
        commits.append(
            Commit(
                oid=object_id,
                author_name=fields[0],
                author_email=fields[1],
                message=fields[2],
            )
        )
    return commits


def main() -> int:
    parser = argparse.ArgumentParser(
        description="verify DCO sign-offs in BASE..HEAD"
    )
    parser.add_argument("base", help="exclusive base commit")
    parser.add_argument("head", help="inclusive head commit")
    parser.add_argument(
        "--allow-dependabot",
        action="store_true",
        help="exempt the recognized Dependabot identity for a trusted bot PR",
    )
    arguments = parser.parse_args()

    try:
        commits = commits_in_range(arguments.base, arguments.head)
    except (subprocess.CalledProcessError, RuntimeError) as error:
        print(f"DCO check could not inspect the revision range: {error}", file=sys.stderr)
        return 2

    if not commits:
        print("DCO check found no commits in the requested range", file=sys.stderr)
        return 2

    try:
        failures = [
            (commit, error)
            for commit in commits
            if (
                error := validation_error(
                    commit, allow_dependabot=arguments.allow_dependabot
                )
            )
            is not None
        ]
    except subprocess.CalledProcessError as error:
        print(f"DCO trailer parsing failed: {error}", file=sys.stderr)
        return 2
    if failures:
        print("DCO sign-off check failed:", file=sys.stderr)
        for commit, error in failures:
            print(
                f"  {commit.oid[:12]} {commit.author_name} "
                f"<{commit.author_email}>: {error}",
                file=sys.stderr,
            )
        print(
            "Amend each listed commit with `git commit --amend --signoff` "
            "and update the pull request branch.",
            file=sys.stderr,
        )
        return 1

    exempt = sum(
        1
        for commit in commits
        if arguments.allow_dependabot and is_dependabot(commit)
    )
    print(
        f"DCO sign-off check passed for {len(commits)} commit(s)"
        + (f" ({exempt} Dependabot exemption(s))" if exempt else "")
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
