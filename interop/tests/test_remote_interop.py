from __future__ import annotations

import base64
import hashlib
import io
import json
import queue
import resource
import signal
import socket
import threading
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

    def test_capture_argv_rejects_privilege_launchers_and_fixes_bounds(self):
        values = self.inventory()["nodes"]["a"]
        for command in (["sudo", "-n", "tcpdump"], ["tcpdump", "-w", "{pcap}"]):
            node = remote_interop.Node.from_inventory("a", {**values, "capture_command": command})
            with self.assertRaisesRegex(remote_interop.ConfigurationError, "direct tcpdump"):
                remote_interop.capture_argv(node, 9000, "/tmp/capture.pcap")
        node = remote_interop.Node.from_inventory("a", {**values, "capture_command": ["/usr/sbin/tcpdump"]})
        argv = remote_interop.capture_argv(node, 9000, "/tmp/capture.pcap", "interop")
        self.assertEqual(argv, ["/usr/sbin/tcpdump", "-U", "-n", "-i", "any",
                               "-s", "192", "-c", "100000", "-Z", "interop",
                               "-w", "/tmp/capture.pcap", "udp", "port", "9000"])

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
                original_stop = remote_interop.AgentClient.stop
                with mock.patch.object(
                    remote_interop.AgentClient, "stop", autospec=True,
                    side_effect=original_stop,
                ) as stop:
                    result = remote_interop.run_scenario(
                        scenario,
                        nodes,
                        agent_source,
                        root / "artifacts",
                        keep_remote=False,
                    )
                stopped = {
                    (call.args[0].node.name, call.args[1])
                    for call in stop.call_args_list
                }
                self.assertIn(("a", "caller"), stopped)
                self.assertIn(("b", "listener"), stopped)
                with mock.patch.object(remote_interop.AgentClient, "stop",
                                       side_effect=remote_interop.AgentError("EPERM test")):
                    failed_cleanup = remote_interop.run_scenario(
                        scenario, nodes, agent_source, root / "cleanup-failure", keep_remote=False)
                self.assertEqual(failed_cleanup["status"], "failed")
                self.assertIn("EPERM test", " ".join(failed_cleanup["cleanup_errors"]))
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
    def test_capture_inherits_hard_file_limit_and_refuses_set_id(self):
        with tempfile.TemporaryDirectory() as directory:
            executable = Path(directory) / "tcpdump"
            executable.write_text(
                f"#!{sys.executable}\nimport resource, sys\n"
                "from pathlib import Path\n"
                "limit = resource.getrlimit(resource.RLIMIT_FSIZE)\n"
                "print(limit, flush=True)\n"
                "with open(sys.argv[1], 'wb') as output:\n"
                "    output.write(b'x' * (33 * 1024 * 1024))\n")
            executable.chmod(0o755)
            agent = remote_agent.Agent()
            initialized = agent.initialize({"run_id": "bounded-capture"})
            target = Path(initialized["run_directory"]) / "capture.pcap"
            try:
                agent.start({"process_id": "capture", "capture": True,
                             "argv": [str(executable), str(target)]})
                deadline = time.monotonic() + 10
                while time.monotonic() < deadline:
                    status = agent.status({"process_id": "capture"})
                    if not status["running"]:
                        break
                    time.sleep(0.01)
                self.assertFalse(status["running"])
                hard = resource.getrlimit(resource.RLIMIT_FSIZE)[1]
                expected = 32 * 1024 * 1024 if hard == resource.RLIM_INFINITY else min(hard, 32 * 1024 * 1024)
                self.assertLessEqual(target.stat().st_size, expected)
                self.assertIn(f"({expected}, {expected})", status["stdout_tail"])
                executable.chmod(0o4755)
                with self.assertRaisesRegex(ValueError, "non-set-id"):
                    agent.start({"process_id": "unsafe", "capture": True,
                                 "argv": [str(executable), str(target)]})
            finally:
                agent.cleanup()

    def test_cleanup_attempts_all_groups_and_shutdown_reports_failure(self):
        agent = remote_agent.Agent()
        agent.initialize({"run_id": "cleanup-failure"})
        first, second = object(), object()
        agent.processes = {"denied": first, "other": second}
        try:
            with mock.patch.object(agent, "_terminate", side_effect=[PermissionError("EPERM"), None]) as stop:
                with self.assertRaisesRegex(RuntimeError, "cleanup failed"):
                    agent.dispatch({"op": "shutdown"})
                self.assertEqual(stop.call_count, 2)
            self.assertEqual(agent.processes, {"denied": first})
            self.assertTrue(agent.run_directory.exists())
        finally:
            agent.processes.clear()
            agent.cleanup()


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

    def assert_descendant_cleanup(self, leader_exits: bool, disconnect: bool) -> None:
        # A connection held only by the descendant provides a portable death
        # notification, including on hosts that do not promptly reap orphans.
        child_source = r"""
import os, signal, socket, sys
read_fd, write_fd = os.pipe()
child = os.fork()
if child == 0:
    os.close(read_fd)
    if sys.argv[2] == "term":
        signal.signal(signal.SIGTERM, signal.SIG_IGN)
    with socket.create_connection(("127.0.0.1", int(sys.argv[1])), timeout=15) as peer:
        os.write(write_fd, b"R")
        os.close(write_fd)
        peer.sendall((str(os.getpid()) + "\n").encode())
        peer.recv(1)
    os._exit(0)
os.close(write_fd)
assert os.read(read_fd, 1) == b"R"
os.close(read_fd)
if sys.argv[2] == "exit":
    sys.exit(0)
signal.alarm(15)
signal.pause()
"""
        self.request({"op": "init", "run_id": "descendant-cleanup"})
        child_pid = None
        started = None
        with socket.socket() as listener:
            listener.bind(("127.0.0.1", 0))
            listener.listen(1)
            listener.settimeout(5)
            started = self.request({
                "op": "start", "process_id": "peer",
                "argv": [sys.executable, "-c", child_source,
                         str(listener.getsockname()[1]),
                         "exit" if leader_exits else "term"],
            })
            try:
                connection, _ = listener.accept()
                with connection:
                    connection.settimeout(5)
                    with connection.makefile("rb") as ready:
                        child_pid = int(ready.readline())
                        self.assertEqual(os.getpgid(child_pid), started["pid"])
                        if leader_exits:
                            status = self.wait_for_exit("peer")
                            self.assertEqual(status["exit_code"], 0)
                        if disconnect:
                            assert self.process.stdin is not None
                            self.process.stdin.close()
                            self.process.wait(timeout=3)
                        else:
                            status = self.request({
                                "op": "stop", "process_id": "peer",
                                "grace_seconds": 0.05,
                            })
                            self.assertFalse(status["running"])
                            repeated = self.request({
                                "op": "stop", "process_id": "peer",
                                "grace_seconds": 0.0,
                            })
                            self.assertEqual(repeated, status)
                        self.assertEqual(ready.read(1), b"")
            finally:
                # On failure, close the descendant's connection and signal only
                # its verified test group; the child also has a 15-second bound.
                if child_pid is not None:
                    try:
                        if os.getpgid(child_pid) == started["pid"]:
                            os.killpg(started["pid"], signal.SIGKILL)
                    except ProcessLookupError:
                        pass

    def test_stop_terminates_descendant_after_leader_exit(self) -> None:
        self.assert_descendant_cleanup(leader_exits=True, disconnect=False)

    def test_stop_escalates_after_leader_exits_on_sigterm(self) -> None:
        self.assert_descendant_cleanup(leader_exits=False, disconnect=False)

    def test_disconnect_terminates_descendant_after_leader_exit(self) -> None:
        self.assert_descendant_cleanup(leader_exits=True, disconnect=True)

    def test_disconnect_escalates_after_leader_exits_on_sigterm(self) -> None:
        self.assert_descendant_cleanup(leader_exits=False, disconnect=True)

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
    def record(self, root: Path):
        stdout_path = root / "process.stdout"
        stderr_path = root / "process.stderr"
        stdout_path.write_text("stdout", encoding="utf-8")
        stderr_path.write_text("stderr", encoding="utf-8")
        process = mock.Mock(pid=123, returncode=None)
        record = remote_agent.ProcessRecord(
            process, stdout_path, stderr_path,
            stdout_path.open("ab"), stderr_path.open("ab"), process.pid,
        )
        self.addCleanup(record.stdout_file.close)
        self.addCleanup(record.stderr_file.close)
        return record

    def test_status_observes_exit_without_reaping_the_leader(self):
        with tempfile.TemporaryDirectory() as directory:
            record = self.record(Path(directory))
            agent = remote_agent.Agent()
            agent.processes["process"] = record
            cases = [
                (None, None),
                (mock.Mock(si_pid=123, si_code=os.CLD_EXITED, si_status=7), 7),
                (mock.Mock(si_pid=123, si_code=os.CLD_KILLED,
                           si_status=signal.SIGTERM), -signal.SIGTERM),
            ]
            for info, expected in cases:
                with self.subTest(expected=expected), mock.patch.object(
                    remote_agent.os, "waitid", return_value=info, create=True
                ) as waitid:
                    status = agent.status({"process_id": "process"})
                    self.assertEqual(status["exit_code"], expected)
                    self.assertEqual(status["running"], expected is None)
                    waitid.assert_called_once_with(
                        os.P_PID, 123, os.WEXITED | os.WNOHANG | os.WNOWAIT
                    )
            record.process.poll.assert_not_called()
            record.process.wait.assert_not_called()

    def test_retirement_escalates_before_reaping_and_cannot_reuse_pgid(self):
        with tempfile.TemporaryDirectory() as directory:
            record = self.record(Path(directory))
            events = []
            def signal_group(group, sig):
                events.append((group, sig))
            def reap(timeout):
                events.append("reap")
                record.process.returncode = 0
                return 0
            record.process.wait.side_effect = reap
            with mock.patch.object(remote_agent.os, "waitid", create=True), \
                    mock.patch.object(remote_agent.os, "killpg",
                                      side_effect=signal_group), \
                    mock.patch.object(remote_agent.time, "monotonic",
                                      side_effect=[0.0, 1.0]):
                remote_agent.Agent._terminate(record, 0.5)
            self.assertEqual(events, [
                (123, signal.SIGTERM), (123, 0),
                (123, signal.SIGKILL), "reap",
            ])
            with mock.patch.object(remote_agent.os, "killpg") as killpg, \
                    mock.patch.object(remote_agent.os, "waitid", create=True) as waitid:
                remote_agent.Agent._terminate(record, 0.0)
            killpg.assert_not_called()
            waitid.assert_not_called()
            self.assertTrue(record.stdout_file.closed)
            self.assertTrue(record.stderr_file.closed)

    def test_disappeared_group_is_reaped_without_escalation(self):
        with tempfile.TemporaryDirectory() as directory:
            record = self.record(Path(directory))
            with mock.patch.object(remote_agent.os, "waitid", create=True), \
                    mock.patch.object(remote_agent.os, "killpg",
                                      side_effect=ProcessLookupError) as killpg:
                remote_agent.Agent._terminate(record, 1.0)
            killpg.assert_called_once_with(123, signal.SIGTERM)
            record.process.wait.assert_called_once_with(timeout=2.0)
            self.assertTrue(record.terminated)

    def test_darwin_empty_or_zombie_group_can_retire(self):
        record = mock.Mock(group_id=123)
        for listing in ["", "123 Z\n456 S\n"]:
            with self.subTest(listing=listing), \
                    mock.patch.object(remote_agent.sys, "platform", "darwin"), \
                    mock.patch.object(remote_agent.os, "killpg",
                                      side_effect=PermissionError), \
                    mock.patch.object(remote_agent.subprocess, "run",
                                      return_value=mock.Mock(stdout=listing, stderr="", returncode=0)):
                self.assertFalse(remote_agent.Agent._signal_group(
                    record, signal.SIGTERM
                ))

    def test_darwin_live_member_permission_errors_are_preserved(self):
        record = mock.Mock(group_id=123)
        with mock.patch.object(remote_agent.sys, "platform", "darwin"), \
                mock.patch.object(remote_agent.os, "killpg",
                                  side_effect=PermissionError), \
                mock.patch.object(remote_agent.subprocess, "run",
                                  return_value=mock.Mock(stdout="123 S\n", stderr="", returncode=0)):
            with self.assertRaises(PermissionError):
                remote_agent.Agent._signal_group(record, signal.SIGTERM)

    def test_darwin_group_probe_failure_is_not_treated_as_empty(self):
        record = mock.Mock(group_id=123)
        with mock.patch.object(remote_agent.sys, "platform", "darwin"), \
                mock.patch.object(remote_agent.os, "killpg",
                                  side_effect=PermissionError), \
                mock.patch.object(remote_agent.subprocess, "run",
                                  return_value=mock.Mock(stdout="", stderr="error",
                                                         returncode=1)):
            with self.assertRaisesRegex(RuntimeError, "inspect"):
                remote_agent.Agent._signal_group(record, signal.SIGTERM)

    def test_lost_wait_identity_never_signals_a_stale_group(self):
        with tempfile.TemporaryDirectory() as directory:
            record = self.record(Path(directory))
            with mock.patch.object(remote_agent.os, "waitid", create=True,
                                   side_effect=ChildProcessError), \
                    mock.patch.object(remote_agent.os, "killpg") as killpg:
                with self.assertRaises(ChildProcessError):
                    remote_agent.Agent._terminate(record, 1.0)
            killpg.assert_not_called()
            record.process.wait.assert_not_called()

    @unittest.skipUnless(sys.platform == "darwin", "Darwin native waitid binding")
    def test_darwin_native_waitid_retains_the_wait_identity(self):
        process = subprocess.Popen([sys.executable, "-c", "raise SystemExit(7)"])
        try:
            with mock.patch.object(remote_agent.os, "waitid", None, create=True):
                deadline = time.monotonic() + 5.0
                while True:
                    status = remote_agent.Agent._wait_status(process.pid)
                    if status is not None:
                        break
                    if time.monotonic() >= deadline:
                        self.fail("child did not exit before readiness deadline")
                    time.sleep(0.01)
                self.assertEqual(status, (os.CLD_EXITED, 7))
                self.assertEqual(remote_agent.Agent._wait_status(process.pid), status)
            self.assertEqual(process.wait(timeout=2), 7)
            with mock.patch.object(remote_agent.os, "waitid", None, create=True):
                with self.assertRaises(ChildProcessError):
                    remote_agent.Agent._wait_status(process.pid)
        finally:
            if process.poll() is None:
                process.kill()
                process.wait(timeout=2)

    def test_stop_rejects_nonfinite_and_out_of_range_grace_periods(self):
        agent = remote_agent.Agent()
        agent.processes["process"] = mock.Mock()
        for grace in [float("nan"), float("inf"), -0.1, 30.1]:
            with self.subTest(grace=grace), mock.patch.object(
                agent, "_terminate"
            ) as terminate:
                with self.assertRaisesRegex(ValueError, "grace"):
                    agent.stop({"process_id": "process", "grace_seconds": grace})
                terminate.assert_not_called()


class AgentResourceLimitTests(unittest.TestCase):
    def client(self):
        client = remote_interop.AgentClient.__new__(remote_interop.AgentClient)
        client.node = mock.Mock(name="node")
        client.process = mock.Mock()
        client._responses = queue.Queue(maxsize=2)
        client._diagnostic_lock = threading.Lock()
        client._stderr = b""
        client._reader_error = None
        return client

    def test_oversized_unterminated_stdout_fails_before_queueing(self):
        client = self.client()
        client.process.stdout = io.BytesIO(b"x" * 65)
        with mock.patch.object(remote_interop, "MAX_RESPONSE_BYTES", 64):
            client._read_stdout()
        self.assertIn("byte limit", client._reader_error)
        client.process.kill.assert_called_once()
        self.assertIsNone(client._responses.get_nowait())

    def test_stdout_flood_is_bounded_and_terminal(self):
        client = self.client()
        client.process.stdout = io.BytesIO(b"{}\n" * 100)
        client._read_stdout()
        self.assertEqual(client._responses.qsize(), 2)
        with self.assertRaisesRegex(remote_interop.AgentError, "pending responses"):
            client._check_reader()
        client.process.kill.assert_called_once()

    def test_stderr_without_newlines_retains_only_bounded_tail(self):
        client = self.client()
        client.process.stderr = io.BytesIO(b"discard" * 10000 + b"TAIL")
        client._read_stderr()
        self.assertEqual(len(client._stderr), remote_interop.MAX_STDERR_BYTES)
        self.assertTrue(client._diagnostics().endswith("TAIL"))

    def test_download_rejects_nonprogress_overflow_and_malformed_chunks(self):
        cases = [
            {"data": "", "eof": False},
            {"data": "!!!!", "eof": True},
            {"data": "", "eof": "yes"},
            {"data": base64.b64encode(b"x" * 9).decode(), "eof": True},
        ]
        for result in cases:
            with self.subTest(result=result), tempfile.TemporaryDirectory() as directory:
                client = self.client()
                client.request = mock.Mock(return_value=result)
                target = Path(directory) / "capture"
                target.write_bytes(b"original")
                with mock.patch.object(remote_interop, "MAX_DOWNLOAD_BYTES", 8):
                    with self.assertRaises(remote_interop.AgentError):
                        client.download("capture", target)
                self.assertEqual(target.read_bytes(), b"original")
                self.assertEqual(list(Path(directory).iterdir()), [target])

    def test_download_chunk_bound_and_endless_peer(self):
        client = self.client()
        with tempfile.TemporaryDirectory() as directory:
            target = Path(directory) / "capture"
            client.request = mock.Mock(return_value={"data": "eHh4eHg=", "eof": True})
            with mock.patch.object(remote_interop, "UPLOAD_CHUNK_SIZE", 4):
                with self.assertRaises(remote_interop.AgentError):
                    client.download("capture", target)
            self.assertFalse(target.exists())
            client.request = mock.Mock(return_value={"data": "eA==", "eof": False})
            with mock.patch.object(remote_interop, "MAX_DOWNLOAD_BYTES", 8):
                with self.assertRaisesRegex(remote_interop.AgentError, "byte limit"):
                    client.download("capture", target)
            self.assertEqual(client.request.call_count, 9)
            self.assertFalse(target.exists())

    def test_download_deadline_and_success(self):
        client = self.client()
        client.request = mock.Mock(return_value={"data": "eA==", "eof": True})
        with tempfile.TemporaryDirectory() as directory:
            target = Path(directory) / "capture"
            with mock.patch.object(remote_interop.time, "monotonic", side_effect=[0, 1, 301]):
                with self.assertRaisesRegex(remote_interop.AgentError, "time limit"):
                    client.download("capture", target)
            self.assertFalse(target.exists())
            client.download("capture", target)
            self.assertEqual(target.read_bytes(), b"x")
            self.assertEqual(list(Path(directory).iterdir()), [target])


if __name__ == "__main__":
    unittest.main()
