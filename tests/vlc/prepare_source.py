#!/usr/bin/env python3
"""Remove a conflicting obsolete option from the pinned VLC input module."""

from __future__ import annotations

import argparse
import hashlib
from pathlib import Path
import re


# VLC 6de05adcbaf2e8b85fe86aad4169393098628119, modules/access/srt.c.
# The active output option must not compete with an obsolete input option.
ORIGINAL_SHA256 = "1b7f921463045907859f3106ea70ae0414063643d1dc5a8245217c393df0c64d"
PREPARED_SHA256 = "d7bf2ff54253c33510f4cd61ac026b3ad8578d7c3d04e8b6fe05efecaf455520"
OBSOLETE_OPTION = re.compile(
    rb"^[ \t]*add_obsolete_integer\([ \t]*SRT_PARAM_PAYLOAD_SIZE[ \t]*\)[ \t]*\n",
    re.MULTILINE,
)


def prepare(source: Path) -> bool:
    target = source / "modules/access/srt.c"
    original = target.read_bytes()
    digest = hashlib.sha256(original).hexdigest()
    if digest == PREPARED_SHA256:
        return False
    if digest != ORIGINAL_SHA256:
        raise RuntimeError("unrecognized VLC SRT input source; refusing to modify it")
    prepared, count = OBSOLETE_OPTION.subn(b"", original)
    if count != 1 or hashlib.sha256(prepared).hexdigest() != PREPARED_SHA256:
        raise RuntimeError("unexpected VLC payload option declaration")
    target.write_bytes(prepared)
    return True


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("source", type=Path)
    args = parser.parse_args()
    try:
        changed = prepare(args.source)
    except (OSError, RuntimeError) as error:
        parser.exit(1, f"VLC source preparation failed: {error}\n")
    print(
        "VLC payload option compatibility: "
        + ("applied" if changed else "already applied")
    )


if __name__ == "__main__":
    main()
