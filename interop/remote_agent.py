#!/usr/bin/env python3
"""Minimal JSON-line process agent used by the remote interoperability harness.

The controller sends this source to ``python3 -c`` over an authenticated SSH
connection.  Commands are always argv arrays and are never evaluated by a
shell.  Every process is placed in its own process group and is terminated when
the controller disconnects.
"""

from __future__ import annotations

import base64
import hashlib
import json
import os
import re
import shutil
import signal
import subprocess
import sys
import tempfile
import time
from dataclasses import dataclass
from pathlib import Path
from typing import Any, BinaryIO


_IDENTIFIER = re.compile(r"^[A-Za-z0-9_.-]{1,96}$")
_ARTIFACT_NAME = re.compile(r"^[A-Za-z0-9_.-]{1,128}$")
_ENVIRONMENT_NAME = re.compile(r"^[A-Za-z_][A-Za-z0-9_]*$")


@dataclass
class ProcessRecord:
    process: subprocess.Popen[bytes]
    stdout_path: Path
    stderr_path: Path
    stdout_file: BinaryIO
    stderr_file: BinaryIO


class Agent:
    def __init__(self) -> None:
        self.run_directory: Path | None = None
        self.processes: dict[str, ProcessRecord] = {}
        self.keep_directory = False

    def _require_run_directory(self) -> Path:
        if self.run_directory is None:
            raise ValueError("agent has not been initialized")
        return self.run_directory

    def _artifact_path(self, name: str) -> Path:
        run_directory = self._require_run_directory()
        if not _ARTIFACT_NAME.fullmatch(name):
            raise ValueError("artifact name must be a simple identifier")
        return run_directory / name

    @staticmethod
    def _tail(path: Path, limit: int) -> str:
        if not path.exists():
            return ""
        with path.open("rb") as stream:
            stream.seek(0, os.SEEK_END)
            size = stream.tell()
            stream.seek(max(0, size - limit))
            return stream.read(limit).decode("utf-8", errors="replace")

    def initialize(self, request: dict[str, Any]) -> dict[str, Any]:
        if self.run_directory is not None:
            raise ValueError("agent is already initialized")
        run_id = str(request.get("run_id", "run"))
        if not _IDENTIFIER.fullmatch(run_id):
            raise ValueError("invalid run identifier")
        self.keep_directory = bool(request.get("keep_directory", False))
        self.run_directory = Path(
            tempfile.mkdtemp(prefix=f"robotweax-srt-interop-{run_id}-")
        )
        os.chmod(self.run_directory, 0o700)
        return {
            "hostname": os.uname().nodename,
            "pid": os.getpid(),
            "python": sys.version.split()[0],
            "run_directory": str(self.run_directory),
        }

    def start(self, request: dict[str, Any]) -> dict[str, Any]:
        process_id = str(request.get("process_id", ""))
        if not _IDENTIFIER.fullmatch(process_id):
            raise ValueError("invalid process identifier")
        if process_id in self.processes:
            raise ValueError(f"process {process_id!r} already exists")

        argv_value = request.get("argv")
        if (
            not isinstance(argv_value, list)
            or not argv_value
            or not all(isinstance(item, str) and item for item in argv_value)
        ):
            raise ValueError("argv must be a nonempty string array")

        environment_value = request.get("environment", {})
        if not isinstance(environment_value, dict):
            raise ValueError("environment must be an object")
        environment = os.environ.copy()
        for name, value in environment_value.items():
            if not isinstance(name, str) or not _ENVIRONMENT_NAME.fullmatch(name):
                raise ValueError("invalid environment variable name")
            if not isinstance(value, str):
                raise ValueError("environment values must be strings")
            environment[name] = value

        cwd_value = request.get("cwd")
        cwd = None if cwd_value is None else str(cwd_value)
        if cwd is not None and not Path(cwd).is_dir():
            raise ValueError(f"working directory does not exist: {cwd}")

        stdout_path = self._artifact_path(f"{process_id}.stdout")
        stderr_path = self._artifact_path(f"{process_id}.stderr")
        stdout_file = stdout_path.open("wb")
        stderr_file = stderr_path.open("wb")
        try:
            process = subprocess.Popen(
                argv_value,
                cwd=cwd,
                env=environment,
                stdin=subprocess.DEVNULL,
                stdout=stdout_file,
                stderr=stderr_file,
                start_new_session=True,
            )
        except BaseException:
            stdout_file.close()
            stderr_file.close()
            raise
        self.processes[process_id] = ProcessRecord(
            process, stdout_path, stderr_path, stdout_file, stderr_file
        )
        return {"process_id": process_id, "pid": process.pid}

    def status(self, request: dict[str, Any]) -> dict[str, Any]:
        process_id = str(request.get("process_id", ""))
        record = self.processes.get(process_id)
        if record is None:
            raise ValueError(f"unknown process {process_id!r}")
        tail_bytes = int(request.get("tail_bytes", 32_768))
        if tail_bytes < 0 or tail_bytes > 1_048_576:
            raise ValueError("tail_bytes is outside the supported range")
        exit_code = record.process.poll()
        return {
            "process_id": process_id,
            "running": exit_code is None,
            "exit_code": exit_code,
            "stdout_tail": self._tail(record.stdout_path, tail_bytes),
            "stderr_tail": self._tail(record.stderr_path, tail_bytes),
        }

    def stop(self, request: dict[str, Any]) -> dict[str, Any]:
        process_id = str(request.get("process_id", ""))
        record = self.processes.get(process_id)
        if record is None:
            raise ValueError(f"unknown process {process_id!r}")
        grace_seconds = float(request.get("grace_seconds", 2.0))
        if grace_seconds < 0.0 or grace_seconds > 30.0:
            raise ValueError("invalid stop grace period")
        self._terminate(record, grace_seconds)
        return self.status({"process_id": process_id})

    def put_begin(self, request: dict[str, Any]) -> dict[str, Any]:
        path = self._artifact_path(str(request.get("name", "")))
        path.write_bytes(b"")
        os.chmod(path, 0o600)
        return {"name": path.name, "size": 0}

    def put_chunk(self, request: dict[str, Any]) -> dict[str, Any]:
        path = self._artifact_path(str(request.get("name", "")))
        offset = int(request.get("offset", -1))
        data = base64.b64decode(
            str(request.get("data", "")), validate=True
        )
        current_size = path.stat().st_size if path.exists() else 0
        if offset != current_size:
            raise ValueError(
                f"upload offset mismatch: expected {current_size}, got {offset}"
            )
        with path.open("ab") as stream:
            stream.write(data)
        return {"name": path.name, "size": current_size + len(data)}

    def get_chunk(self, request: dict[str, Any]) -> dict[str, Any]:
        path = self._artifact_path(str(request.get("name", "")))
        offset = int(request.get("offset", -1))
        length = int(request.get("length", 65_536))
        if offset < 0 or length <= 0 or length > 1_048_576:
            raise ValueError("invalid download range")
        size = path.stat().st_size
        with path.open("rb") as stream:
            stream.seek(offset)
            data = stream.read(length)
        return {
            "name": path.name,
            "size": size,
            "offset": offset,
            "data": base64.b64encode(data).decode("ascii"),
            "eof": offset + len(data) >= size,
        }

    def file_info(self, request: dict[str, Any]) -> dict[str, Any]:
        path = self._artifact_path(str(request.get("name", "")))
        return self._digest_path(path)

    @staticmethod
    def _digest_path(path: Path) -> dict[str, Any]:
        digest = hashlib.sha256()
        size = 0
        with path.open("rb") as stream:
            while True:
                data = stream.read(1_048_576)
                if not data:
                    break
                digest.update(data)
                size += len(data)
        return {
            "name": path.name,
            "size": size,
            "sha256": digest.hexdigest(),
        }

    def binary_info(self, request: dict[str, Any]) -> dict[str, Any]:
        path = Path(str(request.get("path", "")))
        if not path.is_absolute() or not path.is_file():
            raise ValueError("binary path must identify an absolute regular file")
        if not os.access(path, os.X_OK):
            raise ValueError("binary path is not executable")
        result = self._digest_path(path)
        result["path"] = str(path)
        result["mtime_ns"] = path.stat().st_mtime_ns
        return result

    @staticmethod
    def _terminate(record: ProcessRecord, grace_seconds: float) -> None:
        if record.process.poll() is None:
            try:
                os.killpg(record.process.pid, signal.SIGTERM)
            except ProcessLookupError:
                pass
            try:
                record.process.wait(timeout=grace_seconds)
            except subprocess.TimeoutExpired:
                try:
                    os.killpg(record.process.pid, signal.SIGKILL)
                except ProcessLookupError:
                    pass
                record.process.wait(timeout=2.0)
        record.stdout_file.close()
        record.stderr_file.close()

    def cleanup(self) -> None:
        for record in self.processes.values():
            self._terminate(record, 1.0)
        self.processes.clear()
        if (
            self.run_directory is not None
            and not self.keep_directory
            and self.run_directory.exists()
        ):
            shutil.rmtree(self.run_directory)

    def dispatch(self, request: dict[str, Any]) -> dict[str, Any]:
        operation = request.get("op")
        if operation == "init":
            return self.initialize(request)
        if operation == "start":
            return self.start(request)
        if operation == "status":
            return self.status(request)
        if operation == "stop":
            return self.stop(request)
        if operation == "put_begin":
            return self.put_begin(request)
        if operation == "put_chunk":
            return self.put_chunk(request)
        if operation == "get_chunk":
            return self.get_chunk(request)
        if operation == "file_info":
            return self.file_info(request)
        if operation == "binary_info":
            return self.binary_info(request)
        if operation == "shutdown":
            return {"shutdown": True}
        raise ValueError(f"unsupported operation: {operation!r}")


def main() -> int:
    agent = Agent()
    shutdown_requested = False
    try:
        for line in sys.stdin:
            request_id: Any = None
            try:
                request = json.loads(line)
                if not isinstance(request, dict):
                    raise ValueError("request must be a JSON object")
                request_id = request.get("request_id")
                result = agent.dispatch(request)
                response = {"request_id": request_id, "ok": True, "result": result}
                shutdown_requested = request.get("op") == "shutdown"
            except BaseException as error:
                response = {
                    "request_id": request_id,
                    "ok": False,
                    "error": f"{type(error).__name__}: {error}",
                }
            sys.stdout.write(json.dumps(response, separators=(",", ":")) + "\n")
            sys.stdout.flush()
            if shutdown_requested:
                break
    finally:
        agent.cleanup()
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
