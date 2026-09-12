from __future__ import annotations

import json
import os
import shutil
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path
from unittest import mock

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "benchmarks"))
import poll_wake_counters as pc
import throughput_diagnostics as diag


class PollCounterTests(unittest.TestCase):
    def record(self, directory, pid=10, tid=11, **values):
        counters = dict.fromkeys(pc.NAMES, 0)
        counters.update(values)
        header = {"schema": 1, "pid": pid, "tid": tid, "serial": 0}
        row = dict(header, counters=counters, overflow=False, complete=True)
        path = directory / f"{pid}-{tid}.jsonl"
        path.write_text(json.dumps(header) + '\n' + json.dumps(row) + '\n')
        return path, row

    def test_pinned_hooks_and_ambiguous_sources(self):
        for relative, hooks in pc.HOOKS.items():
            text = (pc.ROOT / relative).read_text()
            patched = pc.instrument(text, hooks)
            self.assertIn("RWX_COUNT", patched)
            with self.assertRaises(ValueError):
                pc.instrument(patched, hooks)
        with self.assertRaises(ValueError):
            pc.instrument("same same", [("same", "different")])
        with self.assertRaises(ValueError):
            pc.export_overlay(pc.ROOT, "unreviewed", Path("unused"))

    def test_complete_records_and_reconciled_outcomes(self):
        with tempfile.TemporaryDirectory() as tmp:
            directory = Path(tmp)
            self.record(directory, rx_attempts=5, rx_datagrams=2, rx_eagain=3,
                        channel_polls=3, channel_immediate_io=1, channel_delayed=2,
                        connection_polls=3, batch_0=2, batch_2_4=1, poll_packets=3)
            self.assertEqual(pc.read_counters(directory)[0]['counters']['rx_eagain'], 3)

    def test_missing_overflow_unknown_and_unreconciled_data_fail(self):
        mutations = [lambda r: r.update(complete=False), lambda r: r.update(overflow=True),
                     lambda r: r.update(pid=True), lambda r: r['counters'].pop('rx_eagain'),
                     lambda r: r['counters'].update(rx_errors=-1),
                     lambda r: r['counters'].update(rx_errors=True),
                     lambda r: r['counters'].update(rx_errors=2**64),
                     lambda r: r['counters'].update(rx_attempts=1),
                     lambda r: r['counters'].update(connection_polls=1),
                     lambda r: r['counters'].update(channel_polls=1),
                     lambda r: r['counters'].update(poll_packets=1),
                     lambda r: r['counters'].update(connection_polls=1, batch_2_4=1, poll_packets=1),
                     lambda r: r['counters'].update(readiness_notify_calls=1),
                     lambda r: r['counters'].update(scheduler_waits=1)]
        for mutate in mutations:
            with self.subTest(mutate=mutate), tempfile.TemporaryDirectory() as tmp:
                directory = Path(tmp)
                path, row = self.record(directory)
                header = path.read_text().splitlines()[0]
                mutate(row)
                path.write_text(header + '\n' + json.dumps(row) + '\n')
                with self.assertRaises(ValueError):
                    pc.read_counters(directory)

    def test_incomplete_and_duplicate_thread_records_fail(self):
        with tempfile.TemporaryDirectory() as tmp:
            directory = Path(tmp)
            path, _ = self.record(directory)
            full = path.read_text()
            path.write_text(full.splitlines()[0] + '\n')
            with self.assertRaises(ValueError):
                pc.read_counters(directory)
            path.write_text(full)
            (directory / 'duplicate.jsonl').write_text(full)
            with self.assertRaises(ValueError):
                pc.read_counters(directory)

    def result(self):
        return {'connections': 1, 'messages_per_connection': 4,
                'peer_process_resources': {'sender': {'pid': 10}, 'receiver': {'pid': 20}},
                'wire_statistics': {'sender_packets_unique': 4, 'receiver_packets_unique': 4,
                                    'sender_packets_total': 4, 'retransmitted_packets': 0}}

    def test_endpoint_mapping_and_packet_denominators(self):
        with tempfile.TemporaryDirectory() as tmp:
            directory = Path(tmp)
            for role in ('caller', 'listener'):
                (directory / f'many-socket-{role}.stderr').write_text('')
            counterdir = directory / 'poll-counters'
            counterdir.mkdir()
            self.record(counterdir, tx_originals=4, connection_polls=1, batch_2_4=1, poll_packets=4)
            self.record(counterdir, pid=20, tid=21, rx_attempts=4, rx_datagrams=4, rx_data=4)
            result = pc.analyze_case(directory, self.result(), 'robotweax-self')
            self.assertTrue(result['valid'], result)
            self.assertEqual(result['endpoints']['sender']['per_original_packet']['connection_polls'], .25)
            bad = self.result()
            bad['wire_statistics']['sender_packets_unique'] = 5
            self.assertFalse(pc.analyze_case(directory, bad, 'robotweax-self')['valid'])
            self.assertFalse(pc.analyze_case(directory, self.result(), 'robotweax-to-haivision')['valid'])
            (directory / 'many-socket-caller.stderr').write_text('poll counters: output failure')
            self.assertFalse(pc.analyze_case(directory, self.result(), 'robotweax-self')['valid'])

    def test_missing_endpoint_is_not_zero_and_reference_is_explicit(self):
        with tempfile.TemporaryDirectory() as tmp:
            directory = Path(tmp)
            for role in ('caller', 'listener'):
                (directory / f'many-socket-{role}.stderr').write_text('')
            self.assertFalse(pc.analyze_case(directory, self.result(), 'robotweax-self')['valid'])
            result = pc.analyze_case(directory, self.result(), 'haivision-self')
            self.assertTrue(result['valid'])
            self.assertTrue(result['reference_only'])
            self.assertEqual(result['endpoints'], {})

    def test_counter_capture_is_separate_without_perf_or_warmups(self):
        self.assertEqual(diag.perf_command('counters', Path('unused'), ['driver'], 'unused'), ['driver'])
        self.assertEqual(diag.case_plan(['robotweax-self'], 1, 2, True, 'counters'),
                         [{'profile': 'robotweax-self', 'kind': 'counters'}])

    def test_loss_free_contract_rejects_missing_or_bad_evidence(self):
        entry = {'transfer_pass': True, 'profiling': {'valid': True}, 'result': self.result(),
                 'system_udp_delta': dict.fromkeys(('InErrors', 'RcvbufErrors', 'SndbufErrors', 'InCsumErrors'), 0)}
        self.assertTrue(pc.measurement_contract(entry))
        mutations = [lambda e: e['system_udp_delta'].pop('InErrors'),
                     lambda e: e['system_udp_delta'].update(RcvbufErrors=1),
                     lambda e: e['system_udp_delta'].update(SndbufErrors=False),
                     lambda e: e['profiling'].update(valid=False),
                     lambda e: e['result']['wire_statistics'].update(retransmitted_packets=1),
                     lambda e: e['result']['wire_statistics'].update(sender_packets_total=5),
                     lambda e: e['result']['wire_statistics'].update(receiver_packets_unique=3),
                     lambda e: e['result'].update(messages_per_connection=5)]
        for mutate in mutations:
            altered = json.loads(json.dumps(entry))
            mutate(altered)
            self.assertFalse(pc.measurement_contract(altered))

    def test_invalid_counter_output_keeps_payload_success_but_fails_case(self):
        with tempfile.TemporaryDirectory() as tmp:
            directory = Path(tmp)
            path = directory / 'request.json'
            path.write_text(json.dumps({'capture': 'counters', 'profile': 'robotweax-self',
                'programs': {}, 'options': {'host': '127.0.0.1', 'connections': 1,
                'bytes_per_connection': 5264, 'message_size': 1316, 'timeout_seconds': 15,
                'latency_milliseconds': 120, 'shutdown_grace_milliseconds': 500, 'sampling_interval_seconds': .02},
                'index': 0, 'kind': 'counters', 'target_bps': 0, 'peer_arguments': []}))
            for role in ('caller', 'listener'):
                (directory / f'many-socket-{role}.stdout').write_text('{"event":"capacity-options"}\n')
                (directory / f'many-socket-{role}.stderr').write_text('')
            with mock.patch.dict(os.environ):
                with mock.patch.object(diag.sc, 'run_many_socket_profile', return_value=self.result()):
                    with mock.patch.object(diag, 'udp_snapshot', return_value={}):
                        self.assertEqual(diag.run_case(path), 1)
            report = json.loads((directory / 'case.json').read_text())
            self.assertTrue(report['transfer_pass'], report)
            self.assertFalse(report['profiling']['valid'])

    def test_plain_run_clears_inherited_counter_directory(self):
        with tempfile.TemporaryDirectory() as tmp:
            path = Path(tmp) / 'request.json'
            path.write_text(json.dumps({'capture': 'none', 'profile': 'robotweax-self',
                'programs': {}, 'options': {}, 'index': 0, 'kind': 'measurement', 'peer_arguments': []}))
            with mock.patch.dict(os.environ, {'ROBOTWEAX_POLL_COUNTER_DIR': '/unrelated'}):
                with mock.patch.object(diag, 'udp_snapshot', return_value={}):
                    diag.run_case(path)  # Incomplete request fails after environment isolation.
                self.assertNotIn('ROBOTWEAX_POLL_COUNTER_DIR', os.environ)

    def test_capture_modes_reject_instrumented_plain_or_wrong_overlay(self):
        for enabled, capture in ((True, 'none'), (True, 'transport'), (False, 'counters')):
            with self.subTest(capture=capture), tempfile.TemporaryDirectory() as tmp:
                path = Path(tmp) / 'manifest.json'
                path.write_text(json.dumps({'complete': True, 'poll_counters': {'enabled': enabled}}))
                with self.assertRaises(SystemExit) as caught:
                    diag.main(['--build-manifest', str(path), '--output-directory', str(Path(tmp)/'out'),
                               '--capture', capture])
                self.assertEqual(caught.exception.code, 2)


@unittest.skipUnless(sys.platform in ('linux', 'darwin') and shutil.which('c++'), 'POSIX diagnostic collector')
class NativeCollectorTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.temporary = tempfile.TemporaryDirectory()
        cls.root = Path(cls.temporary.name)
        source = cls.root / 'collector.cpp'
        source.write_text('''#include "poll_wake_counters.hpp"
#include <thread>
#include <cstring>
using namespace robotweax::srt::diagnostics;
int main(int argc, char** argv) {
    RWX_COUNT(rx_attempts); RWX_COUNT(rx_eagain);
    if (argc > 1 && std::strcmp(argv[1], "abrupt") == 0) { _exit(0); }
    if (argc > 1 && std::strcmp(argv[1], "overflow") == 0) {
        poll_counters().add(Counter::rx_eagain, UINT64_MAX); return 0;
    }
    std::thread worker([] { PollScope scope; scope.packets=3; });
    worker.join();
    { PollScope scope; }
    return 0;
}
''')
        cls.program = cls.root / 'collector'
        subprocess.run(['c++', '-std=c++20', '-Wall', '-Wextra', '-Werror', '-pthread',
                        '-I' + str(pc.HEADER.parent), str(source), '-o', str(cls.program)],
                       check=True, capture_output=True)

    @classmethod
    def tearDownClass(cls):
        cls.temporary.cleanup()

    def test_real_thread_registration_flush_histograms_and_failures(self):
        for mode in ('normal', 'abrupt', 'overflow'):
            with self.subTest(mode=mode), tempfile.TemporaryDirectory() as tmp:
                subprocess.run([str(self.program), mode], check=True,
                               env={**os.environ, 'ROBOTWEAX_POLL_COUNTER_DIR': tmp})
                if mode != 'normal':
                    with self.assertRaises(ValueError):
                        pc.read_counters(Path(tmp))
                else:
                    rows = pc.read_counters(Path(tmp))
                    self.assertEqual(len(rows), 2)
                    self.assertEqual(len({r['tid'] for r in rows}), 2)
                    self.assertEqual(sum(r['counters']['batch_2_4'] for r in rows), 1)
                    self.assertEqual(sum(r['counters']['batch_0'] for r in rows), 1)


if __name__ == '__main__':
    unittest.main()
