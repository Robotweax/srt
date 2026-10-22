# SPDX-License-Identifier: MIT
"""Control API tests: local access boundary, argument validation, and media ranges."""
import argparse
import http.client
import json
from pathlib import Path
import tempfile
import threading
import types
import unittest
from unittest.mock import patch

from console import Console, ConsoleServer, Handler, Job, configuration, path_labels


class PathStatusTests(unittest.TestCase):
    def test_broken_idle_socket_and_removed_member_replace_cached_paths(self):
        events = [{"event": "path", "process": "sender", "member": 10, "token": 1,
                   "socket_state": 5, "member_state": 2},
                  {"event": "path_snapshot", "process": "sender", "members": [
                      {"member": 20, "token": 2, "socket_state": 6, "member_state": 1}]}]
        self.assertEqual(path_labels(events, "running", [False, False]),
                         ["Path unavailable · member removed", "Member 20 · broken"])

    def test_empty_snapshot_and_replacement_are_authoritative(self):
        events = [{"event": "path_snapshot", "process": "sender", "members": []}]
        self.assertEqual(path_labels(events, "running", [False, False]), ["Path unavailable · member removed"] * 2)
        events.append({"event": "path_snapshot", "process": "sender", "members": [
            {"member": 30, "token": 1, "socket_state": 4, "member_state": 1}]})
        events.append({"event": "path_snapshot", "process": "receiver", "members": []})
        self.assertEqual(path_labels(events, "running", [False, False]),
                         ["Member 30 · connecting", "Path unavailable · member removed"])

    def test_waiting_paused_and_terminal_states(self):
        self.assertEqual(path_labels([], "starting", [False, False]), ["Awaiting path snapshot"] * 2)
        self.assertEqual(path_labels([], "running", [True, False]),
                         ["Relay paused · simulated outage", "Awaiting path snapshot"])
        self.assertEqual(path_labels([], "failed", [True, True]), ["Session failed"] * 2)
        self.assertEqual(path_labels([], "complete", [False, False]), ["Session closed after run"] * 2)

    def test_job_snapshot_uses_latest_membership_without_discarding_history(self):
        with tempfile.TemporaryDirectory() as directory:
            args = argparse.Namespace(output_dir=Path(directory))
            job = Job(args, configuration({}))
            job.state = "running"
            old = {"event": "path_snapshot", "members": [{"member": 1, "token": 1, "socket_state": 5, "member_state": 2}]}
            new = {"event": "path_snapshot", "members": []}
            job.children = [("sender", types.SimpleNamespace(observations=[(1, json.dumps(old)), (2, json.dumps(new))]))]
            snapshot = job.snapshot()
            self.assertEqual(snapshot["path_labels"], ["Path unavailable · member removed"] * 2)
            self.assertEqual(len(snapshot["events"]), 2)
            self.assertEqual(len(snapshot["latest_events"]), 1)


class ConsoleTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        args = argparse.Namespace(output_dir=Path(self.temp.name), binary_dir=Path(self.temp.name), ffmpeg=None, media=None)
        with patch("socket.getfqdn", side_effect=AssertionError("loopback startup must not use DNS")):
            self.server = ConsoleServer(("127.0.0.1", 0), Handler)
        self.server.console = Console(args)
        self.thread = threading.Thread(target=self.server.serve_forever)
        self.thread.start()

    def tearDown(self):
        self.server.shutdown()
        self.server.server_close()
        self.thread.join(timeout=3)
        self.temp.cleanup()

    def request(self, path, body=None, auth=True, headers=None):
        connection = http.client.HTTPConnection("127.0.0.1", self.server.server_port, timeout=3)
        fields = {"Authorization": "Bearer " + self.server.console.token} if auth else {}
        fields.update(headers or {})
        connection.request("GET" if body is None else "POST", path, body=None if body is None else json.dumps(body), headers=fields)
        response = connection.getresponse()
        result = response.status, dict(response.getheaders()), response.read()
        connection.close()
        return result

    def test_access_and_origin(self):
        self.assertEqual(self.request("/api/state", auth=False)[0], 401)
        self.assertEqual(self.request("/api/state")[0], 200)
        self.assertEqual(self.request("/api/stop", {}, headers={"Origin": "https://untrusted.invalid"})[0], 403)
        self.assertEqual(self.request("/api/state", headers={"Host": "untrusted.invalid"})[0], 403)
        status, headers, _ = self.request("/api/session", {})
        self.assertEqual(status, 200)
        self.assertIn("HttpOnly", headers["Set-Cookie"])
        self.assertIn("SameSite=Strict", headers["Set-Cookie"])
        self.assertEqual(self.request("/api/session", {}, headers={"Cookie": "srt_sample_session=previous-launch"})[0], 200)

    def test_reject_arbitrary_commands_and_invalid_controls(self):
        self.assertEqual(self.request("/api/start", {"command": "arbitrary"})[0], 400)
        self.assertEqual(self.request("/api/start", {"duration": True})[0], 400)
        self.assertEqual(self.request("/api/start", {"drop_every": 2, "burst": 3})[0], 400)
        self.assertEqual(self.request("/api/path", {"index": -1, "paused": True})[0], 400)
        self.assertEqual(self.request("/api/path", {"index": 0, "paused": True})[0], 400)
        self.assertEqual(self.request("/../../etc/passwd")[0], 404)

    def test_recording_range(self):
        directory = Path(self.temp.name)
        (directory / "playback.mp4").write_bytes(b"0123456789")
        self.server.console.job = types.SimpleNamespace(state="complete", directory=directory)
        code, headers, body = self.request("/api/playback", headers={"Range": "bytes=2-5"})
        self.assertEqual((code, body), (206, b"2345"))
        self.assertEqual(headers["Content-Range"], "bytes 2-5/10")
        self.assertEqual(self.request("/api/playback", headers={"Range": "bytes=20-"})[0], 416)
        self.assertEqual(self.request("/api/playback", headers={"Range": "bytes=0-1,4-5"})[0], 416)


if __name__ == "__main__":
    unittest.main()
