from pathlib import Path
import importlib.util
import subprocess
import tempfile
import unittest
from unittest import mock

spec = importlib.util.spec_from_file_location('identity_interop', Path(__file__).resolve().parents[1] / 'run_identity_interop.py')
identity = importlib.util.module_from_spec(spec)
spec.loader.exec_module(identity)


class IdentityDiagnosticsTests(unittest.TestCase):
    def test_timeout_keeps_both_peer_logs_and_original_cause_before_cleanup(self):
        processes = []

        def launch(command, stdout, **kwargs):
            stdout.write('ready; diagnostic marker ' + command[1] + '\n')
            stdout.flush()
            child = mock.Mock()
            child.poll.return_value = None
            child.wait.side_effect = [subprocess.TimeoutExpired(command, 12), 0]
            processes.append(child)
            return child

        with tempfile.TemporaryDirectory() as directory, mock.patch.object(identity.subprocess, 'Popen', side_effect=launch):
            with self.assertRaises(RuntimeError) as failure:
                identity.run_case(Path(directory), 'timeout', [Path('/peer')] * 2, [0, 0], rendezvous=True)
            self.assertIsInstance(failure.exception.__cause__, subprocess.TimeoutExpired)
            message = str(failure.exception)
            self.assertIn('diagnostic marker rendezvous-sender', message)
            self.assertIn('diagnostic marker rendezvous-receiver', message)
            self.assertIn('received bytes: 0; expected: 4096', message)
            self.assertIn('peer exit states (receiver, sender): [None, None]', message)
            for process in processes:
                process.terminate.assert_called_once()
