#!/usr/bin/env python3
"""Pinned listener- and connect-callback interoperability matrix."""

from __future__ import annotations

import argparse
import json
import os
import secrets
import subprocess
import sys
import tempfile
import time
from dataclasses import dataclass
from pathlib import Path

from interop_common import (
    file_sha256,
    free_udp_port,
    resolve_program_path,
    terminate,
    write_deterministic_payload,
)


APPLICATION_REJECTION_REASON = 1_404
PASSPHRASE_ENVIRONMENT = "SRT_LISTEN_CALLBACK_PASSPHRASE"


@dataclass(frozen=True)
class Scenario:
    name: str
    caller: Path
    listener: Path
    stream_id: str
    seed: int
    require_connect_callback: bool


@dataclass(frozen=True)
class RunOptions:
    byte_count: int = 263_200
    chunk_size: int = 1_316
    timeout_seconds: int = 20
    host: str = "127.0.0.1"


def scenario_matrix(robotweax: Path, reference: Path) -> list[Scenario]:
    return [
        Scenario(
            name="listen-callback-robotweax-listener",
            caller=reference,
            listener=robotweax,
            stream_id="#!::r=callback/robotweax,m=request",
            seed=70_001,
            require_connect_callback=False,
        ),
        Scenario(
            name="listen-callback-haivision-listener",
            caller=robotweax,
            listener=reference,
            stream_id="#!::r=callback/haivision,m=request",
            seed=70_002,
            require_connect_callback=True,
        ),
    ]


def peer_command(
    program: Path,
    role: str,
    port: int,
    payload_path: Path,
    scenario: Scenario,
    options: RunOptions,
    *,
    reject: bool = False,
) -> list[str]:
    path_option = "--output" if role == "listener" else "--input"
    command = [
        str(program),
        role,
        "--host",
        options.host,
        "--port",
        str(port),
        "--bytes",
        str(options.byte_count),
        path_option,
        str(payload_path),
        "--transport",
        "live",
        "--timeout-ms",
        str(options.timeout_seconds * 1_000),
        "--chunk-size",
        str(options.chunk_size),
        "--shutdown-grace-ms",
        "250",
    ]
    if role == "listener":
        command.extend(
            [
                "--listen-callback-stream-id",
                scenario.stream_id,
            ]
        )
        if not reject:
            command.extend(["--expect-stream-id", scenario.stream_id])
            command.extend(
                [
                    "--listen-callback-passphrase-env",
                    PASSPHRASE_ENVIRONMENT,
                    "--pbkeylen",
                    "16",
                ]
            )
        else:
            command.extend(
                [
                    "--listen-callback-reject",
                    str(APPLICATION_REJECTION_REASON),
                ]
            )
    else:
        command.extend(["--stream-id", scenario.stream_id])
        if reject:
            command.extend(
                ["--expect-reject", str(APPLICATION_REJECTION_REASON)]
            )
        else:
            command.append("--connect-callback")
            if scenario.require_connect_callback:
                command.append("--require-connect-callback")
            command.extend(
                [
                    "--passphrase-env",
                    PASSPHRASE_ENVIRONMENT,
                    "--pbkeylen",
                    "16",
                ]
            )
    return command


def json_event(output: str, event_name: str) -> dict[str, object] | None:
    for line in output.splitlines():
        try:
            event = json.loads(line)
        except json.JSONDecodeError:
            continue
        if event.get("event") == event_name:
            return event
    return None


def callback_was_valid(output: str, scenario: Scenario, reject: bool) -> bool:
    event = json_event(output, "listen_callback")
    return event is not None and all(
        (
            event.get("valid") is True,
            event.get("handshake_version") == 5,
            event.get("stream_id_bytes")
            == len(scenario.stream_id.encode("utf-8")),
            event.get("option_updated") is True,
            event.get("security_updated") is True,
            event.get("rejected") is reject,
            event.get("reject_reason")
            == (APPLICATION_REJECTION_REASON if reject else 0),
        )
    )


def connect_callback_was_valid(output: str) -> bool:
    event = json_event(output, "connect_callback")
    return event is not None and all(
        (
            event.get("valid") is True,
            event.get("error_code") == 0,
            event.get("peer_family") in (2, 10),
            event.get("token") == -1,
            event.get("socket_state") == 5,
            event.get("peer_name_available") is True,
        )
    )


def connect_callback_meets_policy(output: str, *, required: bool) -> bool:
    """Keep Robotweax strict without inheriting a v1.5.5 callback defect."""
    return not required or connect_callback_was_valid(output)


def wait_for_ready(
    process: subprocess.Popen[str],
    stdout_path: Path,
    timeout_seconds: float = 5.0,
) -> None:
    deadline = time.monotonic() + timeout_seconds
    while time.monotonic() < deadline:
        if process.poll() is not None:
            break
        output = stdout_path.read_text(encoding="utf-8", errors="replace")
        if json_event(output, "ready") is not None:
            return
        time.sleep(0.02)
    output = stdout_path.read_text(encoding="utf-8", errors="replace")
    raise RuntimeError(
        f"listener did not become ready (exit={process.poll()}):\n{output}"
    )


def render_failure(
    scenario: Scenario,
    phase: str,
    reason: str,
    caller: subprocess.CompletedProcess[str] | None,
    listener_stdout: str,
    listener_stderr: str,
) -> str:
    return (
        f"{scenario.name}-{phase}: {reason}\n"
        f"--- caller stdout ---\n"
        f"{caller.stdout if caller is not None else '<not run>'}\n"
        f"--- caller stderr ---\n"
        f"{caller.stderr if caller is not None else '<not run>'}\n"
        f"--- listener stdout ---\n{listener_stdout or '<empty>'}\n"
        f"--- listener stderr ---\n{listener_stderr or '<empty>'}\n"
    )


def run_phase(
    scenario: Scenario,
    options: RunOptions,
    directory: Path,
    *,
    reject: bool,
) -> None:
    phase = "reject" if reject else "accept"
    port = free_udp_port(options.host)
    input_path = directory / f"{scenario.name}-{phase}.input"
    output_path = directory / f"{scenario.name}-{phase}.output"
    expected_digest = write_deterministic_payload(
        input_path, options.byte_count, scenario.seed + int(reject)
    )
    stdout_path = directory / f"{scenario.name}-{phase}.listener.stdout"
    stderr_path = directory / f"{scenario.name}-{phase}.listener.stderr"

    with (
        stdout_path.open("w", encoding="utf-8") as stdout_stream,
        stderr_path.open("w", encoding="utf-8") as stderr_stream,
    ):
        environment = os.environ.copy()
        environment[PASSPHRASE_ENVIRONMENT] = secrets.token_hex(24)
        listener = subprocess.Popen(
            peer_command(
                scenario.listener,
                "listener",
                port,
                output_path,
                scenario,
                options,
                reject=reject,
            ),
            stdout=stdout_stream,
            stderr=stderr_stream,
            text=True,
            env=environment,
        )
        caller: subprocess.CompletedProcess[str] | None = None

        def listener_output() -> tuple[str, str]:
            stdout_stream.flush()
            stderr_stream.flush()
            return (
                stdout_path.read_text(encoding="utf-8", errors="replace"),
                stderr_path.read_text(encoding="utf-8", errors="replace"),
            )

        try:
            wait_for_ready(listener, stdout_path)
            caller = subprocess.run(
                peer_command(
                    scenario.caller,
                    "caller",
                    port,
                    input_path,
                    scenario,
                    options,
                    reject=reject,
                ),
                capture_output=True,
                text=True,
                timeout=options.timeout_seconds + 5,
                check=False,
                env=environment,
            )
            if reject:
                deadline = time.monotonic() + 2.0
                while time.monotonic() < deadline:
                    listener_stdout, _ = listener_output()
                    if json_event(listener_stdout, "listen_callback"):
                        break
                    time.sleep(0.02)
                terminate(listener)
            else:
                listener.wait(timeout=options.timeout_seconds + 5)

            listener_stdout, listener_stderr = listener_output()
            if caller.returncode != 0 or (
                not reject and listener.returncode != 0
            ):
                raise RuntimeError(
                    render_failure(
                        scenario,
                        phase,
                        "peer process failed",
                        caller,
                        listener_stdout,
                        listener_stderr,
                    )
                )
            if not callback_was_valid(
                listener_stdout, scenario, reject
            ):
                raise RuntimeError(
                    render_failure(
                        scenario,
                        phase,
                        "listener did not report a valid callback",
                        caller,
                        listener_stdout,
                        listener_stderr,
                    )
                )
            if not reject and not connect_callback_meets_policy(
                caller.stdout,
                required=scenario.require_connect_callback,
            ):
                raise RuntimeError(
                    render_failure(
                        scenario,
                        phase,
                        "caller did not report a valid connect callback",
                        caller,
                        listener_stdout,
                        listener_stderr,
                    )
                )
            if reject:
                rejection = json_event(caller.stdout, "rejection_match")
                if rejection is None or rejection.get("reason") != (
                    APPLICATION_REJECTION_REASON
                ):
                    raise RuntimeError(
                        render_failure(
                            scenario,
                            phase,
                            "caller did not receive the application reason",
                            caller,
                            listener_stdout,
                            listener_stderr,
                        )
                    )
            elif file_sha256(output_path) != expected_digest:
                raise RuntimeError(
                    render_failure(
                        scenario,
                        phase,
                        "payload SHA-256 mismatch",
                        caller,
                        listener_stdout,
                        listener_stderr,
                    )
                )
            print(
                f"PASS {scenario.name}-{phase} "
                f"stream_id_bytes={len(scenario.stream_id)} "
                f"reject_reason="
                f"{APPLICATION_REJECTION_REASON if reject else 0}"
            )
        finally:
            terminate(listener)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--robotweax-peer", required=True, type=Path)
    parser.add_argument("--reference-peer", required=True, type=Path)
    arguments = parser.parse_args()
    robotweax = resolve_program_path(arguments.robotweax_peer)
    reference = resolve_program_path(arguments.reference_peer)
    options = RunOptions()

    failures: list[str] = []
    with tempfile.TemporaryDirectory(
        prefix="robotweax-srt-listen-callback-"
    ) as temporary:
        directory = Path(temporary)
        for scenario in scenario_matrix(robotweax, reference):
            for reject in (False, True):
                try:
                    run_phase(
                        scenario,
                        options,
                        directory,
                        reject=reject,
                    )
                except Exception as error:  # noqa: BLE001
                    failures.append(str(error))
    if failures:
        print("callback interoperability failures:\n", file=sys.stderr)
        print("\n".join(failures), file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
