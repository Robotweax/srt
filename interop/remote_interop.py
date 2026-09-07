#!/usr/bin/env python3
"""Run reproducible Robotweax/Haivision interoperability scenarios over SSH."""

from __future__ import annotations

import argparse
import base64
import hashlib
import json
import os
import queue
import random
import re
import shlex
import subprocess
import sys
import threading
import time
import uuid
import xml.etree.ElementTree as ET
from dataclasses import dataclass
from pathlib import Path
from typing import Any


SCHEMA_VERSION = 1
UPLOAD_CHUNK_SIZE = 96 * 1024
_NAME = re.compile(r"^[A-Za-z0-9_.-]{1,96}$")
_SECRET_KEY = re.compile(r"(passphrase|password|secret|private.?key)", re.I)


class ConfigurationError(ValueError):
    pass


class AgentError(RuntimeError):
    pass


def load_json(path: Path) -> dict[str, Any]:
    try:
        value = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError) as error:
        raise ConfigurationError(f"cannot read {path}: {error}") from error
    if not isinstance(value, dict):
        raise ConfigurationError(f"{path} must contain a JSON object")
    if value.get("schema_version") != SCHEMA_VERSION:
        raise ConfigurationError(
            f"{path} uses unsupported schema version "
            f"{value.get('schema_version')!r}"
        )
    return value


def safe_name(value: str) -> str:
    sanitized = re.sub(r"[^A-Za-z0-9_.-]+", "-", value).strip(".-")
    return sanitized[:96] or "scenario"


def redact_value(value: Any) -> Any:
    if isinstance(value, dict):
        return {
            key: (
                "<redacted>"
                if _SECRET_KEY.search(str(key)) and not str(key).endswith("_env")
                else redact_value(item)
            )
            for key, item in value.items()
        }
    if isinstance(value, list):
        return [redact_value(item) for item in value]
    return value


def redact_text(text: str, secrets: list[str]) -> str:
    result = text
    for secret in sorted((item for item in secrets if item), key=len, reverse=True):
        result = result.replace(secret, "<redacted>")
    return result


def parse_peer_events(text: str) -> list[dict[str, Any]]:
    events: list[dict[str, Any]] = []
    for line in text.splitlines():
        if not line.startswith("{"):
            continue
        try:
            value = json.loads(line)
        except json.JSONDecodeError:
            continue
        if isinstance(value, dict) and isinstance(value.get("event"), str):
            events.append(value)
    return events


def validate_inventory(inventory: dict[str, Any]) -> None:
    nodes = inventory.get("nodes")
    if not isinstance(nodes, dict) or len(nodes) < 2:
        raise ConfigurationError("inventory must define at least two nodes")
    for name, node in nodes.items():
        if not isinstance(name, str) or not _NAME.fullmatch(name):
            raise ConfigurationError(f"invalid node name: {name!r}")
        if not isinstance(node, dict):
            raise ConfigurationError(f"node {name!r} must be an object")
        ssh = node.get("ssh")
        if (
            not isinstance(ssh, list)
            or not ssh
            or not all(isinstance(item, str) and item for item in ssh)
        ):
            raise ConfigurationError(f"node {name!r} needs an ssh argv array")
        if not isinstance(node.get("address"), str) or not node["address"]:
            raise ConfigurationError(f"node {name!r} needs a peer address")
        peers = node.get("peers")
        if not isinstance(peers, dict) or not peers:
            raise ConfigurationError(f"node {name!r} needs peer binary paths")
        for implementation, path in peers.items():
            if (
                not isinstance(implementation, str)
                or not _NAME.fullmatch(implementation)
                or not isinstance(path, str)
                or not path.startswith("/")
            ):
                raise ConfigurationError(
                    f"invalid peer path for {name!r}/{implementation!r}"
                )


def validate_scenarios(
    manifest: dict[str, Any], inventory: dict[str, Any]
) -> list[dict[str, Any]]:
    scenarios = manifest.get("scenarios")
    if not isinstance(scenarios, list) or not scenarios:
        raise ConfigurationError("scenario manifest has no scenarios")
    defaults = manifest.get("defaults", {})
    if not isinstance(defaults, dict):
        raise ConfigurationError("scenario defaults must be an object")
    nodes = inventory["nodes"]
    result: list[dict[str, Any]] = []
    seen: set[str] = set()
    for raw in scenarios:
        if not isinstance(raw, dict):
            raise ConfigurationError("each scenario must be an object")
        scenario = {**defaults, **raw}
        name = scenario.get("name")
        if not isinstance(name, str) or not _NAME.fullmatch(name):
            raise ConfigurationError(f"invalid scenario name: {name!r}")
        if name in seen:
            raise ConfigurationError(f"duplicate scenario name: {name}")
        seen.add(name)
        for role in ("listener", "caller"):
            endpoint = scenario.get(role)
            if not isinstance(endpoint, dict):
                raise ConfigurationError(f"{name}: missing {role}")
            node_name = endpoint.get("node")
            implementation = endpoint.get("implementation")
            if node_name not in nodes:
                raise ConfigurationError(f"{name}: unknown {role} node")
            if implementation not in nodes[node_name]["peers"]:
                raise ConfigurationError(
                    f"{name}: {implementation!r} is unavailable on {node_name!r}"
                )
        if scenario["listener"]["node"] == scenario["caller"]["node"]:
            raise ConfigurationError(f"{name}: endpoints must use different nodes")
        port = scenario.get("port")
        if not isinstance(port, int) or port <= 0 or port > 65_535:
            raise ConfigurationError(f"{name}: invalid UDP port")
        payload = scenario.get("payload")
        if not isinstance(payload, dict):
            raise ConfigurationError(f"{name}: missing payload configuration")
        size = payload.get("bytes")
        seed = payload.get("seed")
        if not isinstance(size, int) or size <= 0:
            raise ConfigurationError(f"{name}: payload size must be positive")
        if not isinstance(seed, int) or seed < 0:
            raise ConfigurationError(f"{name}: payload seed must be nonnegative")
        if scenario.get("transport", "live") not in ("live", "file"):
            raise ConfigurationError(f"{name}: unsupported transport")
        timeout = scenario.get("timeout_seconds", 30)
        if not isinstance(timeout, (int, float)) or timeout <= 0 or timeout > 3600:
            raise ConfigurationError(f"{name}: invalid timeout")
        options = scenario.get("options", {})
        if not isinstance(options, dict):
            raise ConfigurationError(f"{name}: options must be an object")
        passphrase_env = options.get("passphrase_env")
        if passphrase_env is not None and (
            not isinstance(passphrase_env, str)
            or not re.fullmatch(r"[A-Za-z_][A-Za-z0-9_]*", passphrase_env)
        ):
            raise ConfigurationError(f"{name}: invalid passphrase environment name")
        result.append(scenario)
    return result


def create_payload(path: Path, size: int, seed: int) -> str:
    generator = random.Random(seed)
    digest = hashlib.sha256()
    remaining = size
    with path.open("wb") as stream:
        while remaining:
            chunk = generator.randbytes(min(remaining, 1_048_576))
            stream.write(chunk)
            digest.update(chunk)
            remaining -= len(chunk)
    return digest.hexdigest()


def peer_option_arguments(options: dict[str, Any]) -> list[str]:
    arguments: list[str] = []
    mappings = (
        ("pbkeylen", "--pbkeylen"),
        ("packet_filter", "--packet-filter"),
        ("latency_ms", "--latency-ms"),
        ("input_bw", "--input-bw"),
        ("max_bw", "--max-bw"),
        ("km_refresh_rate", "--km-refresh-rate"),
        ("km_preannounce", "--km-preannounce"),
        ("chunk_size", "--chunk-size"),
    )
    if "passphrase_env" in options:
        arguments.extend(["--passphrase-env", str(options["passphrase_env"])])
    for key, option in mappings:
        if key in options:
            arguments.extend([option, str(options[key])])
    return arguments


@dataclass(frozen=True)
class Node:
    name: str
    address: str
    ssh: list[str]
    python: str
    workspace: str | None
    peers: dict[str, str]
    capture_command: list[str] | None

    @staticmethod
    def from_inventory(name: str, value: dict[str, Any]) -> "Node":
        capture = value.get("capture_command")
        if capture is not None and (
            not isinstance(capture, list)
            or not all(isinstance(item, str) and item for item in capture)
        ):
            raise ConfigurationError(
                f"node {name!r} capture_command must be an argv array"
            )
        workspace = value.get("workspace")
        if workspace is not None and not isinstance(workspace, str):
            raise ConfigurationError(f"node {name!r} workspace must be a string")
        return Node(
            name=name,
            address=value["address"],
            ssh=list(value["ssh"]),
            python=str(value.get("python", "python3")),
            workspace=workspace,
            peers=dict(value["peers"]),
            capture_command=None if capture is None else list(capture),
        )


class AgentClient:
    def __init__(
        self,
        node: Node,
        agent_source: str,
        run_id: str,
        keep_directory: bool,
    ) -> None:
        self.node = node
        remote_command = shlex.join(
            [node.python, "-u", "-c", agent_source]
        )
        self.process = subprocess.Popen(
            [*node.ssh, remote_command],
            stdin=subprocess.PIPE,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            text=True,
            bufsize=1,
        )
        if self.process.stdin is None or self.process.stdout is None:
            raise AgentError(f"failed to open SSH control streams for {node.name}")
        self._responses: queue.Queue[str | None] = queue.Queue()
        self._stderr: list[str] = []
        self._request_id = 0
        self._request_lock = threading.Lock()
        threading.Thread(target=self._read_stdout, daemon=True).start()
        threading.Thread(target=self._read_stderr, daemon=True).start()
        try:
            initialized = self.request(
                {
                    "op": "init",
                    "run_id": run_id,
                    "keep_directory": keep_directory,
                },
                timeout=20.0,
            )
        except BaseException:
            self.process.terminate()
            try:
                self.process.wait(timeout=2.0)
            except subprocess.TimeoutExpired:
                self.process.kill()
                self.process.wait(timeout=2.0)
            for stream in (
                self.process.stdin,
                self.process.stdout,
                self.process.stderr,
            ):
                if stream is not None:
                    stream.close()
            raise
        self.metadata = dict(initialized)
        self.run_directory = str(initialized["run_directory"])

    def _read_stdout(self) -> None:
        assert self.process.stdout is not None
        for line in self.process.stdout:
            self._responses.put(line)
        self._responses.put(None)

    def _read_stderr(self) -> None:
        assert self.process.stderr is not None
        for line in self.process.stderr:
            self._stderr.append(line)

    def request(
        self, request: dict[str, Any], timeout: float = 10.0
    ) -> dict[str, Any]:
        with self._request_lock:
            self._request_id += 1
            request_id = self._request_id
            outgoing = {**request, "request_id": request_id}
            assert self.process.stdin is not None
            try:
                self.process.stdin.write(
                    json.dumps(outgoing, separators=(",", ":")) + "\n"
                )
                self.process.stdin.flush()
            except BrokenPipeError as error:
                raise AgentError(
                    f"SSH agent on {self.node.name} disconnected: "
                    f"{''.join(self._stderr)}"
                ) from error
            try:
                line = self._responses.get(timeout=timeout)
            except queue.Empty as error:
                raise AgentError(
                    f"SSH agent on {self.node.name} did not respond"
                ) from error
            if line is None:
                raise AgentError(
                    f"SSH agent on {self.node.name} exited: "
                    f"{''.join(self._stderr)}"
                )
            try:
                response = json.loads(line)
            except json.JSONDecodeError as error:
                raise AgentError(
                    f"invalid agent response from {self.node.name}: {line!r}"
                ) from error
            if response.get("request_id") != request_id:
                raise AgentError(f"agent response order mismatch on {self.node.name}")
            if not response.get("ok"):
                raise AgentError(
                    f"{self.node.name}: {response.get('error', 'agent error')}"
                )
            result = response.get("result")
            if not isinstance(result, dict):
                raise AgentError(f"invalid agent result from {self.node.name}")
            return result

    def upload(self, local_path: Path, remote_name: str) -> None:
        self.request({"op": "put_begin", "name": remote_name})
        offset = 0
        with local_path.open("rb") as stream:
            while True:
                data = stream.read(UPLOAD_CHUNK_SIZE)
                if not data:
                    break
                result = self.request(
                    {
                        "op": "put_chunk",
                        "name": remote_name,
                        "offset": offset,
                        "data": base64.b64encode(data).decode("ascii"),
                    },
                    timeout=30.0,
                )
                offset = int(result["size"])

    def download(self, remote_name: str, local_path: Path) -> None:
        offset = 0
        with local_path.open("wb") as stream:
            while True:
                result = self.request(
                    {
                        "op": "get_chunk",
                        "name": remote_name,
                        "offset": offset,
                        "length": UPLOAD_CHUNK_SIZE,
                    },
                    timeout=30.0,
                )
                data = base64.b64decode(result["data"], validate=True)
                stream.write(data)
                offset += len(data)
                if result["eof"]:
                    break

    def start(
        self,
        process_id: str,
        argv: list[str],
        environment: dict[str, str] | None = None,
    ) -> None:
        request: dict[str, Any] = {
            "op": "start",
            "process_id": process_id,
            "argv": argv,
        }
        if self.node.workspace is not None:
            request["cwd"] = self.node.workspace
        if environment:
            request["environment"] = environment
        self.request(request)

    def status(self, process_id: str) -> dict[str, Any]:
        return self.request(
            {"op": "status", "process_id": process_id, "tail_bytes": 131_072}
        )

    def stop(self, process_id: str) -> dict[str, Any]:
        return self.request(
            {"op": "stop", "process_id": process_id, "grace_seconds": 2.0}
        )

    def close(self) -> None:
        if self.process.poll() is not None:
            for stream in (
                self.process.stdin,
                self.process.stdout,
                self.process.stderr,
            ):
                if stream is not None:
                    stream.close()
            return
        try:
            self.request({"op": "shutdown"}, timeout=5.0)
        except AgentError:
            pass
        try:
            self.process.wait(timeout=5.0)
        except subprocess.TimeoutExpired:
            self.process.terminate()
            try:
                self.process.wait(timeout=2.0)
            except subprocess.TimeoutExpired:
                self.process.kill()
                self.process.wait(timeout=2.0)
        for stream in (
            self.process.stdin,
            self.process.stdout,
            self.process.stderr,
        ):
            if stream is not None:
                stream.close()


def wait_for_ready(
    agent: AgentClient, process_id: str, timeout: float
) -> dict[str, Any]:
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        status = agent.status(process_id)
        if '"event":"ready"' in status["stdout_tail"]:
            return status
        if not status["running"]:
            raise RuntimeError(
                f"{process_id} exited before becoming ready: "
                f"{status['stderr_tail']}"
            )
        time.sleep(0.1)
    raise TimeoutError(f"{process_id} did not become ready within {timeout}s")


def wait_for_exit(
    agent: AgentClient, process_id: str, deadline: float
) -> dict[str, Any]:
    while time.monotonic() < deadline:
        status = agent.status(process_id)
        if not status["running"]:
            return status
        time.sleep(0.1)
    raise TimeoutError(f"{process_id} exceeded the scenario timeout")


def capture_argv(node: Node, port: int, pcap_path: str) -> list[str] | None:
    if node.capture_command is None:
        return None
    replacements = {"{port}": str(port), "{pcap}": pcap_path}
    return [replacements.get(item, item) for item in node.capture_command]


def build_peer_argv(
    peer: str,
    role: str,
    address: str,
    port: int,
    transport: str,
    artifact_path: str,
    byte_count: int,
    timeout_seconds: float,
    options: dict[str, Any],
) -> list[str]:
    argv = [
        peer,
        role,
        "--host",
        address,
        "--port",
        str(port),
        "--transport",
        transport,
        "--bytes",
        str(byte_count),
        "--timeout-ms",
        str(max(1, int(timeout_seconds * 1000))),
    ]
    argv.extend(
        ["--output" if role == "listener" else "--input", artifact_path]
    )
    argv.extend(peer_option_arguments(options))
    return argv


def scenario_environment(options: dict[str, Any]) -> tuple[dict[str, str], list[str]]:
    passphrase_env = options.get("passphrase_env")
    if passphrase_env is None:
        return {}, []
    if passphrase_env not in os.environ:
        raise ConfigurationError(
            f"required secret environment variable is unset: {passphrase_env}"
        )
    value = os.environ[passphrase_env]
    if not value:
        raise ConfigurationError(f"{passphrase_env} must not be empty")
    return {passphrase_env: value}, [value]


def run_scenario(
    scenario: dict[str, Any],
    nodes: dict[str, Node],
    agent_source: str,
    artifact_root: Path,
    keep_remote: bool,
) -> dict[str, Any]:
    started_at = time.time()
    monotonic_start = time.monotonic()
    name = scenario["name"]
    scenario_directory = artifact_root / safe_name(name)
    scenario_directory.mkdir(parents=True, exist_ok=True)
    payload_path = scenario_directory / "input.bin"
    expected_sha256 = create_payload(
        payload_path, scenario["payload"]["bytes"], scenario["payload"]["seed"]
    )
    listener_endpoint = scenario["listener"]
    caller_endpoint = scenario["caller"]
    listener_node = nodes[listener_endpoint["node"]]
    caller_node = nodes[caller_endpoint["node"]]
    options = scenario.get("options", {})
    environment, secrets = scenario_environment(options)
    timeout = float(scenario.get("timeout_seconds", 30))
    run_id = f"{safe_name(name)}-{uuid.uuid4().hex[:8]}"
    agents: dict[str, AgentClient] = {}
    processes: list[tuple[AgentClient, str]] = []
    captures: list[tuple[AgentClient, str, str, Path]] = []
    result: dict[str, Any] = {
        "name": name,
        "status": "failed",
        "started_at_unix": started_at,
        "listener": redact_value(listener_endpoint),
        "caller": redact_value(caller_endpoint),
        "transport": scenario.get("transport", "live"),
        "port": scenario["port"],
        "payload": {
            "bytes": scenario["payload"]["bytes"],
            "seed": scenario["payload"]["seed"],
            "sha256": expected_sha256,
        },
        "options": redact_value(options),
    }
    listener_status: dict[str, Any] = {}
    caller_status: dict[str, Any] = {}
    try:
        for node in (listener_node, caller_node):
            if node.name not in agents:
                agents[node.name] = AgentClient(
                    node, agent_source, run_id, keep_remote
                )
        listener_agent = agents[listener_node.name]
        caller_agent = agents[caller_node.name]
        result["nodes"] = {
            name: {
                "hostname": agent.metadata.get("hostname"),
                "python": agent.metadata.get("python"),
            }
            for name, agent in agents.items()
        }
        result["binaries"] = {
            listener_node.name: {
                listener_endpoint["implementation"]: listener_agent.request(
                    {
                        "op": "binary_info",
                        "path": listener_node.peers[
                            listener_endpoint["implementation"]
                        ],
                    }
                )
            },
            caller_node.name: {
                caller_endpoint["implementation"]: caller_agent.request(
                    {
                        "op": "binary_info",
                        "path": caller_node.peers[
                            caller_endpoint["implementation"]
                        ],
                    }
                )
            },
        }
        caller_agent.upload(payload_path, "input.bin")

        if bool(scenario.get("capture", False)):
            for node, agent, label in (
                (listener_node, listener_agent, "listener"),
                (caller_node, caller_agent, "caller"),
            ):
                pcap_name = f"{label}.pcap"
                pcap_remote = f"{agent.run_directory}/{pcap_name}"
                argv = capture_argv(node, scenario["port"], pcap_remote)
                if argv is None:
                    raise ConfigurationError(
                        f"{node.name} has no capture_command for a "
                        "capture-enabled scenario"
                    )
                process_id = f"capture-{label}"
                agent.start(process_id, argv)
                processes.append((agent, process_id))
                time.sleep(0.05)
                capture_status = agent.status(process_id)
                if not capture_status["running"]:
                    raise RuntimeError(
                        f"{label} packet capture exited early: "
                        f"{capture_status['stderr_tail']}"
                    )
                captures.append(
                    (
                        agent,
                        process_id,
                        pcap_name,
                        scenario_directory / pcap_name,
                    )
                )

        listener_output = f"{listener_agent.run_directory}/output.bin"
        listener_argv = build_peer_argv(
            listener_node.peers[listener_endpoint["implementation"]],
            "listener",
            "0.0.0.0",
            scenario["port"],
            scenario.get("transport", "live"),
            listener_output,
            scenario["payload"]["bytes"],
            timeout,
            options,
        )
        listener_agent.start("listener", listener_argv, environment)
        processes.append((listener_agent, "listener"))
        wait_for_ready(listener_agent, "listener", min(timeout, 15.0))

        caller_input = f"{caller_agent.run_directory}/input.bin"
        caller_argv = build_peer_argv(
            caller_node.peers[caller_endpoint["implementation"]],
            "caller",
            listener_node.address,
            scenario["port"],
            scenario.get("transport", "live"),
            caller_input,
            scenario["payload"]["bytes"],
            timeout,
            options,
        )
        caller_agent.start("caller", caller_argv, environment)
        processes.append((caller_agent, "caller"))

        deadline = time.monotonic() + timeout
        caller_status = wait_for_exit(caller_agent, "caller", deadline)
        listener_status = wait_for_exit(listener_agent, "listener", deadline)
        if caller_status["exit_code"] != 0:
            raise RuntimeError(
                f"caller exited with {caller_status['exit_code']}: "
                f"{caller_status['stderr_tail']}"
            )
        if listener_status["exit_code"] != 0:
            raise RuntimeError(
                f"listener exited with {listener_status['exit_code']}: "
                f"{listener_status['stderr_tail']}"
            )
        output_info = listener_agent.request(
            {"op": "file_info", "name": "output.bin"}
        )
        if output_info["size"] != scenario["payload"]["bytes"]:
            raise RuntimeError(
                f"payload size mismatch: expected {scenario['payload']['bytes']}, "
                f"received {output_info['size']}"
            )
        if output_info["sha256"] != expected_sha256:
            raise RuntimeError(
                f"payload digest mismatch: expected {expected_sha256}, "
                f"received {output_info['sha256']}"
            )
        result["status"] = "passed"
        result["received"] = output_info
    except Exception as error:
        result["error"] = redact_text(
            f"{type(error).__name__}: {error}", secrets
        )
    finally:
        for label, agent in (
            ("listener", agents.get(listener_node.name)),
            ("caller", agents.get(caller_node.name)),
        ):
            if agent is None:
                continue
            try:
                status = agent.status(label)
                if status["running"]:
                    status = agent.stop(label)
                if label == "listener":
                    listener_status = status
                else:
                    caller_status = status
            except AgentError:
                pass
        for agent, process_id, pcap_name, local_path in captures:
            try:
                agent.stop(process_id)
                agent.download(pcap_name, local_path)
            except (AgentError, OSError) as error:
                result.setdefault("artifact_errors", []).append(
                    redact_text(str(error), secrets)
                )
        for agent, process_id in reversed(processes):
            try:
                status = agent.status(process_id)
                if status["running"]:
                    agent.stop(process_id)
            except AgentError:
                pass
        for label, status in (
            ("listener", listener_status),
            ("caller", caller_status),
        ):
            if status:
                stdout = redact_text(status.get("stdout_tail", ""), secrets)
                stderr = redact_text(status.get("stderr_tail", ""), secrets)
                (scenario_directory / f"{label}.stdout").write_text(
                    stdout, encoding="utf-8"
                )
                (scenario_directory / f"{label}.stderr").write_text(
                    stderr, encoding="utf-8"
                )
                result[label + "_exit_code"] = status.get("exit_code")
                events = parse_peer_events(stdout)
                if events:
                    result[label + "_events"] = events
        for agent in agents.values():
            agent.close()
        try:
            payload_path.unlink()
        except OSError:
            pass
        result["duration_seconds"] = time.monotonic() - monotonic_start
    return result


def write_junit(results: list[dict[str, Any]], path: Path) -> None:
    failures = sum(result.get("status") != "passed" for result in results)
    duration = sum(float(result.get("duration_seconds", 0.0)) for result in results)
    suite = ET.Element(
        "testsuite",
        {
            "name": "robotweax-srt-remote-interop",
            "tests": str(len(results)),
            "failures": str(failures),
            "time": f"{duration:.6f}",
        },
    )
    for result in results:
        case = ET.SubElement(
            suite,
            "testcase",
            {
                "classname": "interop.remote",
                "name": str(result["name"]),
                "time": f"{float(result.get('duration_seconds', 0.0)):.6f}",
            },
        )
        if result.get("status") != "passed":
            failure = ET.SubElement(
                case,
                "failure",
                {"message": str(result.get("error", "scenario failed"))},
            )
            failure.text = str(result.get("error", "scenario failed"))
    tree = ET.ElementTree(suite)
    ET.indent(tree, space="  ")
    tree.write(path, encoding="utf-8", xml_declaration=True)


def dry_run_plan(
    scenarios: list[dict[str, Any]], inventory: dict[str, Any]
) -> list[dict[str, Any]]:
    nodes = inventory["nodes"]
    return [
        {
            "name": scenario["name"],
            "listener": {
                **scenario["listener"],
                "address": nodes[scenario["listener"]["node"]]["address"],
            },
            "caller": scenario["caller"],
            "port": scenario["port"],
            "transport": scenario.get("transport", "live"),
            "payload": scenario["payload"],
            "options": redact_value(scenario.get("options", {})),
            "capture": bool(scenario.get("capture", False)),
        }
        for scenario in scenarios
    ]


def main() -> int:
    parser = argparse.ArgumentParser(
        description="Run Robotweax/Haivision scenarios on two SSH nodes"
    )
    parser.add_argument("--inventory", type=Path, required=True)
    parser.add_argument("--scenarios", type=Path, required=True)
    parser.add_argument(
        "--artifacts", type=Path, default=Path("interop-artifacts")
    )
    parser.add_argument(
        "--scenario",
        action="append",
        dest="selected",
        help="run only this scenario; repeat to select more",
    )
    parser.add_argument("--dry-run", action="store_true")
    parser.add_argument("--keep-remote", action="store_true")
    arguments = parser.parse_args()

    try:
        inventory = load_json(arguments.inventory)
        manifest = load_json(arguments.scenarios)
        validate_inventory(inventory)
        scenarios = validate_scenarios(manifest, inventory)
        if arguments.selected:
            selected = set(arguments.selected)
            known = {scenario["name"] for scenario in scenarios}
            missing = sorted(selected - known)
            if missing:
                raise ConfigurationError(
                    "unknown selected scenarios: " + ", ".join(missing)
                )
            scenarios = [
                scenario for scenario in scenarios if scenario["name"] in selected
            ]
        if arguments.dry_run:
            print(
                json.dumps(
                    dry_run_plan(scenarios, inventory), indent=2, sort_keys=True
                )
            )
            return 0

        arguments.artifacts.mkdir(parents=True, exist_ok=True)
        run_directory = arguments.artifacts / (
            time.strftime("%Y%m%d-%H%M%S", time.gmtime())
            + "-"
            + uuid.uuid4().hex[:8]
        )
        run_directory.mkdir()
        nodes = {
            name: Node.from_inventory(name, value)
            for name, value in inventory["nodes"].items()
        }
        agent_source = (
            Path(__file__).with_name("remote_agent.py").read_text(encoding="utf-8")
        )
        results: list[dict[str, Any]] = []
        for scenario in scenarios:
            print(f"RUN  {scenario['name']}", flush=True)
            result = run_scenario(
                scenario,
                nodes,
                agent_source,
                run_directory,
                arguments.keep_remote,
            )
            results.append(result)
            print(
                f"{'PASS' if result['status'] == 'passed' else 'FAIL'} "
                f"{scenario['name']} ({result['duration_seconds']:.2f}s)",
                flush=True,
            )
        summary = {
            "schema_version": SCHEMA_VERSION,
            "generated_at_unix": time.time(),
            "inventory": redact_value(inventory),
            "scenario_manifest": str(arguments.scenarios),
            "passed": sum(result["status"] == "passed" for result in results),
            "failed": sum(result["status"] != "passed" for result in results),
            "results": results,
        }
        (run_directory / "report.json").write_text(
            json.dumps(summary, indent=2, sort_keys=True) + "\n",
            encoding="utf-8",
        )
        write_junit(results, run_directory / "junit.xml")
        print(f"Artifacts: {run_directory}")
        return 0 if summary["failed"] == 0 else 1
    except (ConfigurationError, AgentError, OSError) as error:
        print(f"error: {error}", file=sys.stderr)
        return 2
    except KeyboardInterrupt:
        print("interrupted", file=sys.stderr)
        return 130


if __name__ == "__main__":
    raise SystemExit(main())
