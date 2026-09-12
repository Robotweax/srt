#!/usr/bin/env python3
"""Drop final original Live DATA datagrams and require recovery."""
import argparse
import hashlib
import json
from pathlib import Path
import subprocess
import time
import tempfile

from interop_common import free_udp_port, terminate, write_deterministic_payload
from srt_handshake_trace import CallerListenerFaultProxy, RendezvousFault


def run(sender_peer, receiver_peer, output, tail_packets=1):
    output = output.resolve()
    output.mkdir(parents=True, exist_ok=False)
    sender_peer, receiver_peer = sender_peer.resolve(), receiver_peer.resolve()
    count, size = 600, 1200
    digest = write_deterministic_payload(output / 'input.bin', count * size, 1005)
    port = free_udp_port('127.0.0.1')
    relay = CallerListenerFaultProxy(port, tuple(
        RendezvousFault('drop', 'sender_to_receiver', index)
        for index in range(count - tail_packets + 1, count + 1)))
    processes, handles = [], []
    report = {'scenario': 'live-final-original-data-drop', 'messages': count,
              'tail_packets': tail_packets, 'message_bytes': size, 'latency_ms': 2000, 'input_bytes_per_second': 120000,
              'expected_sha256': digest, 'started_at_unix': time.time(),
              'binary_sha256': {str(p): hashlib.sha256(p.read_bytes()).hexdigest() for p in (sender_peer, receiver_peer)},
              'passed': False}
    def start(role, peer, peer_port):
        argv = [str(peer), role, '--host', '127.0.0.1', '--port', str(peer_port),
                '--bytes', str(count * size), '--transport', 'live', '--chunk-size', str(size),
                '--latency-ms', '2000', '--timeout-ms', '10000', '--tlpktdrop', 'on',
                '--shutdown-grace-ms', '250']
        argv += ['--input', str(output / 'input.bin'), '--source-pacing', '--explicit-source-time', '--input-bw', '120000'] if role == 'caller' else ['--output', str(output / 'output.bin')]
        report.setdefault('argv', {})[role] = argv
        logs = [(output / f'{role}.{suffix}').open('w') for suffix in ('stdout', 'stderr')]
        handles.extend(logs)
        process = subprocess.Popen(argv, stdout=logs[0], stderr=logs[1])
        processes.append(process)
        return process
    try:
        listener = start('listener', receiver_peer, port)
        deadline = time.monotonic() + 5
        while '"event":"ready"' not in (output / 'listener.stdout').read_text():
            if listener.poll() is not None or time.monotonic() >= deadline:
                raise RuntimeError('listener failed to become ready')
            time.sleep(.01)
        relay.start()
        caller = start('caller', sender_peer, relay.port)
        caller.wait(timeout=25)
        listener.wait(timeout=25)
        relay.close()
        report['fault_observations'] = relay.fault_observations()
        (output / 'trace.json').write_text(relay.render('live-final-original-data-drop') + '\n')
        received = (output / 'output.bin').read_bytes()
        report['received_bytes'] = len(received)
        report['received_sha256'] = hashlib.sha256(received).hexdigest()
        faults = report['fault_observations']
        tail_recovered = (
            len(faults) == tail_packets
            and {f.get('occurrence') for f in faults}
                == set(range(count - tail_packets + 1, count + 1))
            and all(f.get('payload_bytes') == size
                    and f.get('retransmission_observed') is True
                    and f.get('retransmission_flag') is True
                    and f.get('cumulative_ack_observed') is True
                    for f in faults)
            and next(f for f in faults if f['occurrence'] == count)
                .get('later_data_observed_before_retransmission') is False
        )
        report['passed'] = (
            all(p.returncode == 0 for p in processes)
            and len(received) == count * size
            and report['received_sha256'] == digest
            and tail_recovered and relay.error() is None
        )
    except (OSError, RuntimeError, subprocess.SubprocessError) as error:
        report['error'] = str(error)
    finally:
        relay.close()
        for process in processes: terminate(process)
        for handle in handles: handle.close()
        report['exit_codes'] = [p.returncode for p in processes]
        report['cleanup_verified'] = all(p.poll() is not None for p in processes)
        report['ended_at_unix'] = time.time()
        (output / 'report.json').write_text(json.dumps(report, indent=2) + '\n')
    print(json.dumps(report, indent=2))
    return 0 if report['passed'] else 1


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--sender-peer', type=Path, required=True)
    parser.add_argument('--receiver-peer', type=Path, required=True)
    parser.add_argument('--tail-packets', type=int, choices=range(1, 17), default=1)
    parser.add_argument('--artifacts', type=Path,
                        help='Retain evidence in a new directory; otherwise use cleaned temporary storage')
    args = parser.parse_args()
    if args.artifacts is not None:
        return run(args.sender_peer, args.receiver_peer, args.artifacts, args.tail_packets)
    with tempfile.TemporaryDirectory(prefix='srt-live-tail-') as temporary:
        return run(args.sender_peer, args.receiver_peer, Path(temporary) / 'run', args.tail_packets)


if __name__ == '__main__':
    raise SystemExit(main())
