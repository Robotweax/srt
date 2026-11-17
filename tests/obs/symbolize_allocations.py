#!/usr/bin/env python3
"""Symbolize qualification allocation frames while runner binaries exist."""
import argparse
from pathlib import Path
import re
import subprocess


def symbolize(root):
    for log in root.glob("*-stdout.log"):
        lines = []
        for line in log.read_text(errors="replace").splitlines():
            match = re.fullmatch(r"OBS_ALLOC_FRAME (0x[0-9a-f]+) (0x[0-9a-f]+) (.+)", line)
            if match:
                address, base, image = match.groups()
                result = subprocess.run(["atos", "-o", image, "-l", base, address],
                                        check=True, capture_output=True, text=True, timeout=10)
                lines.append(line + "\n  " + result.stdout.strip())
            elif line.startswith("OBS_ALLOC"):
                lines.append(line)
        if lines:
            log.with_name(log.stem + "-allocations.txt").write_text("\n".join(lines) + "\n")


if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("root", type=Path)
    symbolize(parser.parse_args().root)
