"""Port reuse must not make the two group paths share an endpoint."""
from contextlib import redirect_stdout
import io
import json
from pathlib import Path
import sys
import tempfile
from types import SimpleNamespace
import unittest
from unittest.mock import patch

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
import run_mbedtls_peers as peers


class MbedTlsRendezvousPortsTests(unittest.TestCase):
    def test_group_paths_remain_distinct_when_released_ports_are_reused(self):
        active = set()
        server_commands = []

        class EphemeralSocket:
            def __init__(self, *_args):
                self.port = None

            def bind(self, _address):
                # Model a legal allocator that immediately reuses a free port.
                self.port = next(p for p in range(20000, 20100) if p not in active)
                active.add(self.port)

            def getsockname(self):
                return '127.0.0.1', self.port

            def close(self):
                if self.port is not None:
                    active.remove(self.port)
                    self.port = None

            def __enter__(self):
                return self

            def __exit__(self, *_args):
                self.close()

        def result(command):
            backend = 1 if 'openssl-peer' in command[0] else 3
            return json.dumps(dict(backend=backend, acp1=True, rendezvous=True,
                                   group='backup', integrity=True)) + '\n'

        test = self

        class Server:
            returncode = 0

            def __init__(self, command, **_kwargs):
                endpoints = [int(command[i]) for i in (2, 7, 8, 9)]
                test.assertEqual(len(set(endpoints)), 4,
                                 'first and replacement paths reused an endpoint')
                test.assertFalse(active, 'probes must be released before peers bind')
                self.command = command
                self.stdout = io.StringIO('ready\n')
                server_commands.append(command)

            def communicate(self, **_kwargs):
                return result(self.command), ''

            def poll(self):
                return self.returncode

        def client(command, **_kwargs):
            server = server_commands[-1]
            self.assertEqual([command[i] for i in (2, 7, 8, 9)],
                             [server[i] for i in (7, 2, 9, 8)])
            return SimpleNamespace(returncode=0, stdout=result(command), stderr='')

        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            argv = ['run_mbedtls_peers.py', '--acp1', '--rendezvous', '--group', 'backup',
                    '--openssl-peer', str(root/'openssl-peer/peer'),
                    '--mbedtls-peer', str(root/'mbedtls-peer/peer'),
                    '--output', str(root/'result.json')]
            with patch.object(sys, 'argv', argv), \
                    patch.object(peers.socket, 'socket', EphemeralSocket), \
                    patch.object(peers.subprocess, 'Popen', Server), \
                    patch.object(peers.subprocess, 'run', client), \
                    redirect_stdout(io.StringIO()):
                peers.main()
            self.assertEqual(len(server_commands), 12)
            self.assertEqual(len(json.loads((root/'result.json').read_text())), 12)
        self.assertFalse(active)
