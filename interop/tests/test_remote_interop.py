from __future__ import annotations

import base64
import hashlib
import json
import os
import subprocess
import sys
import tempfile
import time
import unittest
import xml.etree.ElementTree as ET
from pathlib import Path
from unittest import mock


INTEROP_DIRECTORY = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(INTEROP_DIRECTORY))

import remote_interop  # noqa: E402
import remote_agent  # noqa: E402


class RemoteInteropUnitTests(unittest.TestCase):
    def inventory(self) -> dict:
        return {
            "schema_version": 1,
            "nodes": {
                "a": {
                    "address": "192.0.2.10",
                    "ssh": ["ssh", "a"],
                    "peers": {
                        "robotweax": "/opt/robotweax",
                        "haivision": "/opt/haivision",
                    },
                },
                "b": {
                    "address": "192.0.2.20",
                    "ssh": ["ssh", "b"],
                    "peers": {
                        "robotweax": "/opt/robotweax",
                        "haivision": "/opt/haivision",
                    },
                },
            },
        }

    def manifest(self) -> dict:
        return {
            "schema_version": 1,
            "defaults": {"timeout_seconds": 10, "transport": "live"},
            "scenarios": [
                {
                    "name": "robotweax-to-haivision",
                    "listener": {
                        "node": "b",
                        "implementation": "haivision",
                    },
                    "caller": {
                        "node": "a",
                        "implementation": "robotweax",
                    },
                    "port": 10000,
                    "payload": {"bytes": 4096, "seed": 7},
                    "options": {},
                }
            ],
        }

    def test_validation_and_dry_run(self) -> None:
        inventory = self.inventory()
        remote_interop.validate_inventory(inventory)
        scenarios = remote_interop.validate_scenarios(
            self.manifest(), inventory
        )
        plan = remote_interop.dry_run_plan(scenarios, inventory)
        self.assertEqual(plan[0]["listener"]["address"], "192.0.2.20")
        self.assertEqual(plan[0]["payload"]["bytes"], 4096)

    def test_validation_rejects_same_node(self) -> None:
        inventory = self.inventory()
        manifest = self.manifest()
        manifest["scenarios"][0]["caller"]["node"] = "b"
        with self.assertRaises(remote_interop.ConfigurationError):
            remote_interop.validate_scenarios(manifest, inventory)

    def test_checked_in_manifests_are_valid(self) -> None:
        inventory = remote_interop.load_json(
            INTEROP_DIRECTORY / "remote_inventory.example.json"
        )
        remote_interop.validate_inventory(inventory)
        expected_counts = {
            "remote_scenarios.smoke.json": 6,
            "remote_scenarios.secure-fec.json": 9,
            "remote_scenarios.file.json": 4,
        }
        for filename, expected_count in expected_counts.items():
            manifest = remote_interop.load_json(INTEROP_DIRECTORY / filename)
            scenarios = remote_interop.validate_scenarios(manifest, inventory)
            self.assertEqual(len(scenarios), expected_count)
            self.assertEqual(
                len(remote_interop.dry_run_plan(scenarios, inventory)),
                expected_count,
            )

    def test_payload_is_deterministic_and_hashed(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            first = Path(directory) / "first.bin"
            second = Path(directory) / "second.bin"
            first_hash = remote_interop.create_payload(first, 131_071, 99)
            second_hash = remote_interop.create_payload(second, 131_071, 99)
            self.assertEqual(first.read_bytes(), second.read_bytes())
            self.assertEqual(first_hash, second_hash)
            self.assertEqual(
                first_hash, hashlib.sha256(first.read_bytes()).hexdigest()
            )

    def test_secret_redaction_and_peer_arguments(self) -> None:
        options = {
            "passphrase_env": "SRT_INTEROP_PASSPHRASE",
            "password": "must-not-appear",
            "pbkeylen": 32,
            "packet_filter": "fec,cols:10,rows:1",
        }
        redacted = remote_interop.redact_value(options)
        self.assertEqual(redacted["password"], "<redacted>")
        self.assertEqual(
            redacted["passphrase_env"], "SRT_INTEROP_PASSPHRASE"
        )
        arguments = remote_interop.peer_option_arguments(options)
        self.assertNotIn("must-not-appear", arguments)
        self.assertIn("SRT_INTEROP_PASSPHRASE", arguments)
        self.assertEqual(
            remote_interop.redact_text(
                "prefix highly-secret suffix", ["highly-secret"]
            ),
            "prefix <redacted> suffix",
        )
        events = remote_interop.parse_peer_events(
            'diagnostic\n{"event":"ready","port":10000}\n'
            '{"event":"complete","bytes":4096}\n'
        )
        self.assertEqual([event["event"] for event in events], ["ready", "complete"])

    def test_junit_report(self) -> None:
        results = [
            {"name": "pass", "status": "passed", "duration_seconds": 0.5},
            {
                "name": "fail",
                "status": "failed",
                "duration_seconds": 1.0,
                "error": "digest mismatch",
            },
        ]
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "junit.xml"
            remote_interop.write_junit(results, path)
            root = ET.parse(path).getroot()
            self.assertEqual(root.attrib["tests"], "2")
            self.assertEqual(root.attrib["failures"], "1")
            self.assertIsNotNone(root.find("./testcase/failure"))

    def test_complete_controller_flow_with_two_local_agents(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            fake_peer = root / "fake_peer.py"
            fake_peer.write_text(
                f"""#!{sys.executable}
import json
import shutil
import sys
import tempfile
import time
from pathlib import Path

def option(name):
    return sys.argv[sys.argv.index(name) + 1]

role = sys.argv[1]
port = option("--port")
shared = Path(tempfile.gettempdir()) / ("robotweax-srt-fake-" + port + ".bin")
if role == "listener":
    print(json.dumps({{"event": "ready", "port": int(port)}},
                     separators=(",", ":")), flush=True)
    deadline = time.monotonic() + 5
    while not shared.exists() and time.monotonic() < deadline:
        time.sleep(0.01)
    if not shared.exists():
        raise SystemExit(3)
    shutil.copyfile(shared, option("--output"))
else:
    temporary = shared.with_suffix(".temporary")
    shutil.copyfile(option("--input"), temporary)
    temporary.replace(shared)
print(json.dumps({{"event": "complete", "role": role,
                  "bytes": int(option("--bytes"))}},
                 separators=(",", ":")), flush=True)
""",
                encoding="utf-8",
            )
            fake_peer.chmod(0o700)
            port = 20_000 + os.getpid() % 10_000
            shared = Path(tempfile.gettempdir()) / (
                f"robotweax-srt-fake-{port}.bin"
            )
            shared.unlink(missing_ok=True)
            nodes = {
                name: remote_interop.Node(
                    name=name,
                    address="127.0.0.1",
                    ssh=["sh", "-c"],
                    python=sys.executable,
                    workspace=None,
                    peers={"fake": str(fake_peer)},
                    capture_command=None,
                )
                for name in ("a", "b")
            }
            scenario = {
                "name": "local-controller-flow",
                "listener": {"node": "b", "implementation": "fake"},
                "caller": {"node": "a", "implementation": "fake"},
                "port": port,
                "transport": "live",
                "timeout_seconds": 10,
                "capture": False,
                "payload": {"bytes": 32_768, "seed": 1234},
                "options": {"chunk_size": 1200},
            }
            agent_source = (
                INTEROP_DIRECTORY / "remote_agent.py"
            ).read_text(encoding="utf-8")
            try:
                result = remote_interop.run_scenario(
                    scenario,
                    nodes,
                    agent_source,
                    root / "artifacts",
                    keep_remote=False,
                )
            finally:
                shared.unlink(missing_ok=True)
            self.assertEqual(result["status"], "passed", result.get("error"))
            self.assertEqual(result["received"]["size"], 32_768)
            self.assertEqual(
                result["received"]["sha256"],
                result["payload"]["sha256"],
            )
            self.assertIn("listener_events", result)
            self.assertIn("caller_events", result)
            self.assertIn("sha256", result["binaries"]["a"]["fake"])


class RemoteAgentProtocolTests(unittest.TestCase):
    def setUp(self) -> None:
        self.process = subprocess.Popen(
            [sys.executable, "-u", str(INTEROP_DIRECTORY / "remote_agent.py")],
            stdin=subprocess.PIPE,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            text=True,
        )
        self.request_id = 0

    def tearDown(self) -> None:
        if self.process.poll() is None:
            try:
                self.request({"op": "shutdown"})
            except (BrokenPipeError, RuntimeError):
                pass
        try:
            self.process.wait(timeout=3)
        except subprocess.TimeoutExpired:
            self.process.kill()
            self.process.wait(timeout=2)
        for stream in (
            self.process.stdin,
            self.process.stdout,
            self.process.stderr,
        ):
            if stream is not None:
                stream.close()

    def request(self, value: dict) -> dict:
        self.request_id += 1
        request = {**value, "request_id": self.request_id}
        assert self.process.stdin is not None
        assert self.process.stdout is not None
        self.process.stdin.write(json.dumps(request) + "\n")
        self.process.stdin.flush()
        line = self.process.stdout.readline()
        if not line:
            assert self.process.stderr is not None
            raise RuntimeError(self.process.stderr.read())
        response = json.loads(line)
        self.assertEqual(response["request_id"], self.request_id)
        self.assertTrue(response["ok"], response.get("error"))
        return response["result"]

    def wait_for_exit(
        self, process_id: str, timeout_seconds: float = 5.0
    ) -> dict:
        deadline = time.monotonic() + timeout_seconds
        while True:
            status = self.request(
                {"op": "status", "process_id": process_id, "tail_bytes": 4096}
            )
            if not status["running"]:
                return status
            remaining = deadline - time.monotonic()
            if remaining <= 0.0:
                self.fail(
                    f"process {process_id!r} did not exit within "
                    f"{timeout_seconds}s; stderr: {status['stderr_tail']}"
                )
            time.sleep(min(0.01, remaining))

    def test_upload_process_status_hash_and_download(self) -> None:
        initialized = self.request(
            {"op": "init", "run_id": "agent-test", "keep_directory": False}
        )
        run_directory = initialized["run_directory"]
        payload = b"deterministic remote payload"
        self.request({"op": "put_begin", "name": "input.bin"})
        self.request(
            {
                "op": "put_chunk",
                "name": "input.bin",
                "offset": 0,
                "data": base64.b64encode(payload).decode("ascii"),
            }
        )
        info = self.request({"op": "file_info", "name": "input.bin"})
        self.assertEqual(info["size"], len(payload))
        self.assertEqual(info["sha256"], hashlib.sha256(payload).hexdigest())
        binary = self.request({"op": "binary_info", "path": sys.executable})
        self.assertGreater(binary["size"], 0)
        self.assertEqual(binary["path"], sys.executable)

        self.request(
            {
                "op": "start",
                "process_id": "hello",
                "argv": [
                    sys.executable,
                    "-c",
                    "print('remote-agent-output', flush=True)",
                ],
            }
        )
        status = self.wait_for_exit("hello")
        self.assertEqual(status["exit_code"], 0)
        self.assertIn("remote-agent-output", status["stdout_tail"])
        downloaded = self.request(
            {
                "op": "get_chunk",
                "name": "hello.stdout",
                "offset": 0,
                "length": 4096,
            }
        )
        self.assertIn(
            b"remote-agent-output", base64.b64decode(downloaded["data"])
        )
        self.request({"op": "shutdown"})
        self.process.wait(timeout=3)
        self.assertFalse(Path(run_directory).exists())

    def test_controller_can_inject_agent_over_transport_command(self) -> None:
        node = remote_interop.Node(
            name="local",
            address="127.0.0.1",
            ssh=["sh", "-c"],
            python=sys.executable,
            workspace=None,
            peers={"robotweax": "/unused"},
            capture_command=None,
        )
        source = (INTEROP_DIRECTORY / "remote_agent.py").read_text(
            encoding="utf-8"
        )
        client = remote_interop.AgentClient(
            node, source, "transport-test", keep_directory=False
        )
        try:
            client.start(
                "command",
                [
                    sys.executable,
                    "-c",
                    "import time; time.sleep(0.75); "
                    "print('transport-command-output', flush=True)",
                ],
            )
            status = remote_interop.wait_for_exit(
                client, "command", time.monotonic() + 5.0
            )
            self.assertEqual(status["exit_code"], 0)
            self.assertIn(
                "transport-command-output", status["stdout_tail"]
            )
        finally:
            client.close()


class RemoteAgentStatusTests(unittest.TestCase):
    def test_status_samples_process_state_once(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            stdout_path = root / "process.stdout"
            stderr_path = root / "process.stderr"
            stdout_path.write_text("stdout", encoding="utf-8")
            stderr_path.write_text("stderr", encoding="utf-8")

            process = mock.Mock()
            process.poll.return_value = None
            with stdout_path.open("ab") as stdout_file, \
                    stderr_path.open("ab") as stderr_file:
                agent = remote_agent.Agent()
                agent.run_directory = root
                agent.processes["process"] = remote_agent.ProcessRecord(
                    process,
                    stdout_path,
                    stderr_path,
                    stdout_file,
                    stderr_file,
                )

                status = agent.status(
                    {"process_id": "process", "tail_bytes": 4096}
                )

            process.poll.assert_called_once_with()
            self.assertTrue(status["running"])
            self.assertIsNone(status["exit_code"])


if __name__ == "__main__":
    unittest.main()
