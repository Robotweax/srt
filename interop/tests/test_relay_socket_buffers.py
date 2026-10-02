"""Relay headroom evidence must survive independent socket-option failures."""

from __future__ import annotations

import errno
import io
import json
import socket
import sys
import unittest
from contextlib import redirect_stderr, redirect_stdout
from pathlib import Path
from unittest.mock import Mock, call

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from interop_common import enlarge_relay_socket_buffers  # noqa: E402


class RelaySocketBufferTests(unittest.TestCase):
    def diagnose(self, sock: Mock) -> dict[str, object]:
        stderr, stdout = io.StringIO(), io.StringIO()
        with redirect_stderr(stderr), redirect_stdout(stdout):
            enlarge_relay_socket_buffers(sock, 8 * 1024 * 1024)
        self.assertEqual(stdout.getvalue(), "")
        lines = stderr.getvalue().splitlines()
        self.assertEqual(len(lines), 1)
        prefix = "RELAY_SOCKET_BUFFERS "
        self.assertTrue(lines[0].startswith(prefix))
        return json.loads(lines[0][len(prefix):])

    def test_kernel_clamping_and_linux_accounting_are_reported_raw(self):
        sock = Mock()
        sock.getsockopt.side_effect = [425984, 16 * 1024 * 1024]
        result = self.diagnose(sock)
        self.assertEqual(result["requested_bytes"], 8 * 1024 * 1024)
        self.assertEqual(result["receive"],
                         {"request_status": "accepted", "kernel_bytes": 425984})
        self.assertEqual(result["send"]["kernel_bytes"], 16 * 1024 * 1024)
        self.assertEqual(sock.method_calls, [
            call.setsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF, 8 * 1024 * 1024),
            call.getsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF),
            call.setsockopt(socket.SOL_SOCKET, socket.SO_SNDBUF, 8 * 1024 * 1024),
            call.getsockopt(socket.SOL_SOCKET, socket.SO_SNDBUF),
        ])

    def test_refusal_still_reports_existing_buffer_and_attempts_send(self):
        sock = Mock()
        sock.setsockopt.side_effect = [OSError(errno.ENOBUFS, "refused"), None]
        sock.getsockopt.side_effect = [212992, 8388608]
        result = self.diagnose(sock)
        self.assertEqual(result["receive"], {
            "request_status": "refused", "set_errno": errno.ENOBUFS,
            "kernel_bytes": 212992,
        })
        self.assertEqual(result["send"]["request_status"], "accepted")
        self.assertEqual(sock.setsockopt.call_count, 2)

    def test_query_failure_is_explicit_and_does_not_abort_other_direction(self):
        sock = Mock()
        sock.getsockopt.side_effect = [OSError(errno.ENOPROTOOPT, "unavailable"), 4096]
        result = self.diagnose(sock)
        self.assertEqual(result["receive"], {
            "request_status": "accepted", "kernel_bytes": None,
            "get_errno": errno.ENOPROTOOPT,
        })
        self.assertEqual(result["send"]["kernel_bytes"], 4096)
        self.assertEqual(sock.setsockopt.call_count, 2)


if __name__ == "__main__":
    unittest.main()
