#!/usr/bin/env python3
"""Reject incompatible explicit modes through the real public demo CLIs."""

from __future__ import annotations

import argparse
import os
from pathlib import Path
import subprocess
import tempfile

from test_message_demo import reserve_udp_port, wait_for_ready


def connection_rejected(kind: str, caller: subprocess.CompletedProcess[str]) -> bool:
    if caller.returncode != 1:
        return False
    if kind.startswith("group-"):
        # srt_connect_group aggregates failures as ECONNSETUP. Require both
        # configured members to report SRT_REJ_CRYPTO, not just that aggregate.
        return ("srt_connect_group failed:" in caller.stderr
                and all(f"ENDPOINT_REJECT token={token} reason=17 " in caller.stderr
                        for token in (1001, 1002)))
    return ("srt_connect failed:" in caller.stderr
            and "connection rejected" in caller.stderr.lower())


def run_mismatch(demo: Path, kind: str, caller_mode: str,
                 listener_mode: str, directory: Path) -> None:
    port = reserve_udp_port()
    secret_name = "ROBOTWEAX_DEMO_CRYPTO_TEST_PASSPHRASE"
    environment = {**os.environ, secret_name: "synthetic-demo-crypto-test"}
    common = ["--port", str(port), "--passphrase-env", secret_name,
              "--pbkeylen", "32", "--timeout-ms", "3000"]
    listener_args = ["listener", "--bind", "127.0.0.1",
                     "--crypto", listener_mode, *common]
    caller_args = ["caller", "--host", "127.0.0.1",
                   "--crypto", caller_mode, *common]
    output = directory / "received.bin"
    if kind == "file":
        source = directory / "source.bin"
        source.write_bytes(b"synthetic crypto-mode regression payload")
        listener_args += ["--output", str(output)]
        caller_args += ["--input", str(source)]
    elif kind.startswith("group-"):
        policy = kind.removeprefix("group-")
        group_args = ["--policy", policy, "--members", "2", "--messages", "1"]
        listener_args += group_args
        caller_args += group_args

    listener = subprocess.Popen(
        [str(demo), *listener_args], stdout=subprocess.PIPE,
        stderr=subprocess.PIPE, text=True, env=environment,
    )
    try:
        ready = wait_for_ready(listener)
        caller = subprocess.run(
            [str(demo), *caller_args], capture_output=True, text=True,
            env=environment, timeout=10,
        )
    finally:
        # A rejected connection leaves the single-use listener accepting.
        # Terminate that test-owned process even when the caller crashes/hangs.
        if listener.poll() is None:
            listener.kill()
        listener_stdout, listener_stderr = listener.communicate(timeout=5)

    diagnostic = (f"{kind}: caller={caller_mode}, listener={listener_mode}\n"
                  f"caller exit={caller.returncode}\n{caller.stdout}\n"
                  f"{caller.stderr}\nlistener:\n{ready}{listener_stdout}\n"
                  f"{listener_stderr}")
    # A generic timeout, argument error, or missing library is not evidence
    # that the modes were rejected during connection establishment.
    if not connection_rejected(kind, caller):
        raise RuntimeError("expected connection rejection\n" + diagnostic)
    for marker in ("ECHO verified", "ECHOED ", "TRANSFER ", "COMPLETE ",
                   "SEND index=", "DELIVERY index="):
        if marker in caller.stdout or marker in listener_stdout:
            raise RuntimeError("mismatched peers transferred payload\n" + diagnostic)
    if output.exists() and output.stat().st_size:
        raise RuntimeError("mismatched file peer wrote payload\n" + diagnostic)
    print(f"PASS {kind}: {caller_mode} caller / {listener_mode} listener rejected")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--demo", type=Path, required=True)
    parser.add_argument("--kind", required=True,
                        choices=("message", "file", "group-broadcast", "group-backup"))
    arguments = parser.parse_args()
    with tempfile.TemporaryDirectory(prefix="srt-demo-crypto-") as temporary:
        for caller, listener in (("gcm", "ctr"), ("ctr", "gcm")):
            run_mismatch(arguments.demo.resolve(), arguments.kind,
                         caller, listener, Path(temporary))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
