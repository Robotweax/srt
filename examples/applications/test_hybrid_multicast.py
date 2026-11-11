#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Sample processes: authenticated startup, multicast outage, fallback and return."""
import argparse
import socket
import os
import subprocess
import tempfile
import time
from pathlib import Path
from process_support import Process, ports
from test_multicast import wait_event, Collector


def run(binary, crypto, latency):
    base = ports(6)
    common = ['--endpoint', f'127.0.0.1:{base}', '--interface', '127.0.0.1',
              '--latency-ms', str(latency), '--hybrid', '--hybrid-secret-env', 'HYBRID_KEYS']
    keys = ['11' * 32, '22' * 32]
    if crypto != 'plain':
        common += ['--passphrase-env', 'MEDIA_KEY', '--crypto', crypto]
    processes = []
    collectors = []
    with tempfile.TemporaryDirectory(prefix='hybrid-media-') as directory:
        root = Path(directory)
        try:
            sender = Process([str(binary), 'send', *common, '--multicast',
                              f'239.255.133.11:{base+1}', '--ttl', '0', '--min-receivers', '2',
                              '--duration', '10', '--media-udp', f'127.0.0.1:{base+2}',
                              '--test-multicast-outage'],
                             {'HYBRID_KEYS': f'alice={keys[0]};bob={keys[1]}',
                              'MEDIA_KEY': 'hybrid-sample-media-passphrase'})
            processes.append(sender)
            sender.ready()
            receivers = []
            for index, name in enumerate(('alice', 'bob')):
                collectors.append(Collector(base + 3 + index))
                receiver = Process([str(binary), 'receive', *common, '--hybrid-id', name,
                                    '--duration', '9', '--media-udp', f'127.0.0.1:{base+3+index}',
                                    '--record', str(root / f'{name}.ts')],
                                   {'HYBRID_KEYS': f'{name}={keys[index]}',
                                    'MEDIA_KEY': 'hybrid-sample-media-passphrase'})
                receivers.append(receiver)
                processes.append(receiver)
                wait_event(receiver, 'media_ready')
            wait_event(sender, 'media_ready')
            payloads = []
            with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as udp:
                start = time.monotonic()
                for index in range(700):
                    packet = b'\x47\x1f\xff\x10' + index.to_bytes(4, 'big') + bytes(180)
                    payload = packet * 7
                    payloads.append(payload)
                    udp.sendto(payload, ('127.0.0.1', base + 2))
                    time.sleep(max(0, start + (index+1)*.01 - time.monotonic()))
            for index, receiver in enumerate(receivers):
                events = receiver.finish()
                modes = [e['mode'] for e in events if e.get('event') == 'hybrid_path']
                # Initial authenticated UC -> MC, outage -> UC, recovery -> MC.
                stable = [mode for mode in modes if mode != 'overlap']
                assert stable[:4] == ['unicast', 'multicast', 'unicast', 'multicast'], modes
                recorded = (root / ('alice.ts' if index == 0 else 'bob.ts')).read_bytes()
                assert len(recorded) % 1316 == 0
                got = [recorded[i:i+1316] for i in range(0, len(recorded), 1316)]
                indexes = [int.from_bytes(packet[4:8], 'big') for packet in got]
                assert indexes == sorted(set(indexes)), 'duplicate or reordered media'
                assert all(packet == payloads[n] for packet, n in zip(got, indexes)), 'corrupted media'
                assert len(got) >= (560 if latency >= 1000 else 630), f'receiver {index}: excessive live loss ({len(got)}/700)'
                if latency >= 1000:
                    assert any(e.get('event') == 'hybrid_discontinuity' for e in events)
            sender.finish()
            print(f'PASS hybrid {crypto}: two pairwise peers, ordered MPEG-TS payloads, automatic fallback and return')
        except BaseException:
            for process in processes:
                print(process.output(), flush=True)
            raise
        finally:
            for process in processes:
                process.close()
            for collector in collectors:
                collector.close()


def reject_invalid(binary, enabled):
    base = [str(binary), 'receive', '--media-udp', '127.0.0.1:9001']
    cases = [(['--hybrid'], {}),
             (['--hybrid-id', 'alice'], {}),
             (['--hybrid', '--hybrid-secret-env', 'KEYS', '--hybrid-id', 'alice'], {'KEYS': 'alice=not-hex'}),
             (['--hybrid', '--hybrid-secret-env', 'KEYS', '--hybrid-id', 'alice'], {'KEYS': 'alice=' + '00'*32}),
             (['--hybrid', '--hybrid-secret-env', 'KEYS', '--hybrid-id', 'bob'], {'KEYS': 'alice=' + '11'*32}),
             (['--hybrid', '--hybrid-secret-env', 'KEYS', '--hybrid-id', 'alice', '--repeat-threshold', '1'], {'KEYS': 'alice=' + '11'*32}),
             (['--test-multicast-outage'], {})]
    if not enabled:
        cases.append((['--hybrid', '--hybrid-secret-env', 'KEYS', '--hybrid-id', 'alice'], {'KEYS': 'alice=' + '11'*32}))
    for options, env in cases:
        result = subprocess.run(base + options, env={**os.environ, **env},
                                capture_output=True, text=True, timeout=5)
        assert result.returncode != 0, options
        assert 'media_ready' not in result.stdout
        for value in env.values():
            assert value not in result.stdout + result.stderr, 'credential leaked'
    print('PASS hybrid command-line validation and credential redaction')


def reject_wrong_key(binary):
    base = ports(4)
    common = ['--endpoint', f'127.0.0.1:{base}', '--interface', '127.0.0.1',
              '--hybrid', '--hybrid-secret-env', 'KEYS', '--duration', '3']
    processes = []
    try:
        sender = Process([str(binary), 'send', *common, '--multicast',
                          f'239.255.133.11:{base+1}', '--ttl', '0',
                          '--media-udp', f'127.0.0.1:{base+2}'], {'KEYS': 'alice=' + '11'*32})
        processes.append(sender)
        sender.ready()
        receiver = Process([str(binary), 'receive', *common, '--hybrid-id', 'alice',
                            '--media-udp', f'127.0.0.1:{base+3}'], {'KEYS': 'alice=' + '22'*32})
        processes.append(receiver)
        receiver.child.wait(timeout=12)
        for reader in receiver.readers:
            reader.join(timeout=2)
        assert receiver.child.returncode != 0, receiver.output()
        assert 'media_ready' not in receiver.output(), 'unauthorized receiver was published'
        print('PASS mismatched hybrid pairwise secret rejected before media')
    finally:
        for process in processes:
            process.close()


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--binary', type=Path, required=True)
    parser.add_argument('--crypto', choices=['plain', 'ctr', 'gcm'], default='plain')
    parser.add_argument('--latency-ms', type=int, default=120)
    parser.add_argument('--validation', choices=['enabled', 'disabled'])
    parser.add_argument('--wrong-key', action='store_true')
    args = parser.parse_args()
    if args.validation:
        reject_invalid(args.binary.resolve(), args.validation == 'enabled')
    elif args.wrong_key:
        reject_wrong_key(args.binary.resolve())
    else:
        run(args.binary.resolve(), args.crypto, args.latency_ms)
