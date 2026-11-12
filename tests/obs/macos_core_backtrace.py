#!/usr/bin/env python3
"""Extract LLDB text evidence from cores of failed qualification endpoints."""

import argparse
import json
from pathlib import Path
import signal
import subprocess
import tempfile


def backtrace(core, output):
    with output.open("w") as log:
        result = subprocess.run(
            ["xcrun", "lldb", "--no-lldbinit", "--batch", "--core", str(core),
             "-o", "thread backtrace all", "-o", "register read",
             "-o", "image list", "-o", "disassemble --frame"],
            stdout=log, stderr=subprocess.STDOUT, timeout=120,
        )
    text = output.read_text(errors="replace")
    if result.returncode or "thread #" not in text or "frame #0:" not in text:
        raise RuntimeError(f"LLDB did not produce a core backtrace: {output}")


def self_test(cores, output):
    """Verify core generation and symbolized LLDB capture before building OBS."""
    with tempfile.TemporaryDirectory(prefix="obs-core-probe-") as directory:
        source = Path(directory) / "probe.c"
        executable = Path(directory) / "probe"
        source.write_text(
            "#include <signal.h>\n"
            "__attribute__((noinline)) void crash_probe(void) { raise(SIGSEGV); }\n"
            "int main(void) { crash_probe(); return 0; }\n"
        )
        subprocess.run(["xcrun", "clang", "-g", "-O0", str(source),
                        "-o", str(executable)], check=True, timeout=30)
        child = subprocess.Popen([str(executable)])
        status = child.wait(timeout=60)
        core = cores / f"core.{child.pid}"
        if status != -signal.SIGSEGV or not core.is_file():
            raise RuntimeError(f"macOS core capture unavailable: exit={status}, {core}")
        backtrace(core, output)
        if "crash_probe" not in output.read_text():
            raise RuntimeError("LLDB core probe lacks the expected crash frame")
        core.unlink()


def collect(artifacts, cores):
    artifacts.mkdir(parents=True, exist_ok=True)
    statuses = [json.loads(path.read_text())
                for path in artifacts.glob("*-endpoint-status.json")]
    fatal = {signal.SIGSEGV, signal.SIGBUS, signal.SIGILL,
             signal.SIGABRT, signal.SIGFPE, signal.SIGTRAP}
    fatal_exits = {-value for value in fatal}
    crashed = {status[f"{role}_pid"] for status in statuses
               for role in ("desktop", "receiver")
               if status.get(f"{role}_pid")
               and status.get(f"{role}_exit") in fatal_exits}
    inventory = {"crashed_pids": sorted(crashed), "backtraces": [], "missing": []}
    # Never load an unrelated process core or upload binary core contents.
    for pid in sorted(crashed):
        core = cores / f"core.{pid}"
        if not core.is_file() or core.is_symlink():
            inventory["missing"].append(pid)
            continue
        output = artifacts / f"endpoint-{pid}-lldb.txt"
        backtrace(core, output)
        inventory["backtraces"].append(output.name)
    (artifacts / "core-backtrace-inventory.json").write_text(
        json.dumps(inventory, indent=2) + "\n"
    )
    if inventory["missing"]:
        raise RuntimeError(f"missing cores for crashed endpoints: {inventory['missing']}")


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("mode", choices=("self-test", "collect"))
    parser.add_argument("cores", type=Path)
    parser.add_argument("output", type=Path)
    args = parser.parse_args()
    if args.mode == "self-test":
        self_test(args.cores, args.output)
    else:
        collect(args.output, args.cores)
