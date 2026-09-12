from __future__ import annotations

import sys
import unittest
from pathlib import Path
from unittest.mock import Mock, patch

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from run_encrypted_interop import wait_for_listener_ready


class ListenerReadyTests(unittest.TestCase):
    def test_waits_for_complete_matching_event(self):
        listener = Mock()
        listener.poll.return_value = None
        output = Mock(side_effect=[('{"event":"ready",', ''),
                                   ('{"event":"ready","port":999}', ''),
                                   ('noise\n{"event":"ready","port":123}', '')])
        with patch('run_encrypted_interop.time.sleep') as sleep:
            wait_for_listener_ready(listener, output, 123, 10)
        self.assertEqual(sleep.call_count, 2)

    def test_exited_peer_is_not_ready(self):
        listener = Mock()
        listener.poll.return_value = 1
        with self.assertRaisesRegex(RuntimeError, 'exited'):
            wait_for_listener_ready(listener, Mock(), 123, 10)

    def test_live_process_without_ready_times_out(self):
        listener = Mock()
        listener.poll.return_value = None
        with self.assertRaisesRegex(RuntimeError, 'readiness timed out'):
            wait_for_listener_ready(listener, lambda: ('[]\n{}\nnull', ''), 123, 0)
