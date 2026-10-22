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

from console import Console, Handler, ThreadingHTTPServer, configuration


class ConsoleTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        args = argparse.Namespace(output_dir=Path(self.temp.name), binary_dir=Path(self.temp.name), ffmpeg=None, media=None)
        self.server = ThreadingHTTPServer(("127.0.0.1", 0), Handler)
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
