from contextlib import ExitStack, redirect_stderr
import errno
import importlib.util
import io
from pathlib import Path
import socket
import unittest
from unittest import mock

spec = importlib.util.spec_from_file_location(
    'udp_bridge_fixture', Path(__file__).resolve().parents[2] / 'examples/test_udp_bridge.py')
bridge = importlib.util.module_from_spec(spec)
spec.loader.exec_module(bridge)


class BridgeFixtureTests(unittest.TestCase):
    def test_ports_remain_owned_until_individual_handoff(self):
        with ExitStack() as stack:
            sockets = [bridge.reserve_port(stack) for _ in range(4)]
            ports = [s.getsockname()[1] for s in sockets]
            self.assertEqual(len(set(ports)), 4)
            for port in ports:
                with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as contender:
                    with self.assertRaises(OSError) as error:
                        contender.bind(('127.0.0.1', port))
                    self.assertIn(error.exception.errno, (errno.EADDRINUSE, 10048))
            sockets[0].close()
            with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as owner:
                owner.bind(('127.0.0.1', ports[0]))
                for port in ports[1:]:
                    with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as contender:
                        with self.assertRaises(OSError):
                            contender.bind(('127.0.0.1', port))
        for port in ports:
            with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as owner:
                owner.bind(('127.0.0.1', port))

    def test_cleanup_preserves_primary_failure_and_closes_every_peer(self):
        healthy, broken = mock.Mock(), mock.Mock()
        broken.close.side_effect = AssertionError('shutdown failed')
        output = io.StringIO()
        with redirect_stderr(output), self.assertRaisesRegex(ValueError, 'original bind error'):
            try:
                raise ValueError('original bind error')
            finally:
                bridge.close_peers([healthy, broken])
        healthy.close.assert_called_once()
        broken.close.assert_called_once()
        self.assertIn('shutdown failed', output.getvalue())

    def test_cleanup_failure_is_not_silenced_after_success(self):
        broken = mock.Mock()
        broken.close.side_effect = AssertionError('shutdown failed')
        with self.assertRaisesRegex(AssertionError, 'shutdown failed'):
            bridge.close_peers([broken])
