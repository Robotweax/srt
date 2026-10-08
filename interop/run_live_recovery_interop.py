#!/usr/bin/env python3
"""Check NAK-driven Live recovery and delayed-ACK retransmission suppression."""
import argparse
import hashlib
import json
from pathlib import Path
import subprocess
import time
import tempfile

from interop_common import free_udp_port, parse_complete, terminate, write_deterministic_payload
from srt_handshake_trace import CallerListenerFaultProxy, RendezvousFault


def run(sender_peer, receiver_peer, output, loss_packets=1, scenario="gap"):
    output = output.resolve()
    output.mkdir(parents=True, exist_ok=False)
    sender_peer, receiver_peer = sender_peer.resolve(), receiver_peer.resolve()
    count, size = 600, 1200
    digest = write_deterministic_payload(output / 'input.bin', count * size, 1005)
    port = free_udp_port('127.0.0.1')
    # Keep one later original DATA packet to expose the lost range. A Live
    # stream ending on a loss is not a lossless file-transfer contract.
    faults = tuple(RendezvousFault('drop', 'sender_to_receiver', index)
                   for index in range(count - loss_packets, count)) if scenario == 'gap' else (
        RendezvousFault('delay', 'receiver_to_sender', 1,
                        packet_kind='control', control_type=2,
                        delay_milliseconds=500),)
    relay = CallerListenerFaultProxy(port, faults)
    processes, handles = [], []
    report = {'scenario': f'live-periodic-nak-{scenario}', 'messages': count,
              'loss_packets': loss_packets if scenario == 'gap' else 0, 'message_bytes': size, 'latency_ms': 2000, 'input_bytes_per_second': 120000,
              'expected_sha256': digest, 'started_at_unix': time.time(),
              'binary_sha256': {str(p): hashlib.sha256(p.read_bytes()).hexdigest() for p in (sender_peer, receiver_peer)},
              'passed': False}
    def start(role, peer, peer_port):
        argv = [str(peer), role, '--host', '127.0.0.1', '--port', str(peer_port),
                '--bytes', str(count * size), '--transport', 'live', '--chunk-size', str(size),
                '--latency-ms', '2000', '--timeout-ms', '10000', '--tlpktdrop', 'on',
                '--shutdown-grace-ms', '2500']
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
        (output / 'trace.json').write_text(relay.render(f'live-periodic-nak-{scenario}') + '\n')
        received = (output / 'output.bin').read_bytes()
        report['received_bytes'] = len(received)
        report['received_sha256'] = hashlib.sha256(received).hexdigest()
        faults = report['fault_observations']
        report['retransmissions'] = relay.forwarded_data_retransmissions('sender_to_receiver')
        report['loss_reports'] = relay.loss_report_observations()
        report['forwarded_data'] = relay.forwarded_data_observation('sender_to_receiver')
        report['peer_statistics'] = {
            role: parse_complete((output / f'{role}.stdout').read_text(), role)
            for role in ('caller', 'listener')}
        if scenario == 'delayed-ack':
            wire_clean = (report['forwarded_data'].get('count') == count
                          and report['forwarded_data'].get('sequences_contiguous') is True
                          and report['peer_statistics']['caller']['stats']['pktRetransTotal'] == 0)
        else:
            wire_clean = True
        recovery_valid = validate_recovery(scenario, faults, loss_packets, count, size,
                                           report['retransmissions'], report['loss_reports'])
        report['passed'] = (
            all(p.returncode == 0 for p in processes)
            and len(received) == count * size
            and report['received_sha256'] == digest
            and recovery_valid and wire_clean and relay.error() is None and relay.arq_trace_complete()
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


def validate_recovery(scenario, faults, loss_packets, count, size, retransmissions, loss_reports):
    if scenario == 'delayed-ack':
        return (len(faults) == 1
                and faults[0].get('action') == 'delay'
                and faults[0].get('packet_kind') == 'control'
                and faults[0].get('delay_elapsed_milliseconds', 0) >= 500
                and faults[0].get('delayed_datagrams', 0) > 0
                and retransmissions == 0 and not loss_reports)
    return (len(faults) == loss_packets
            and {f.get('occurrence') for f in faults} == set(range(count - loss_packets, count))
            and all(f.get('action') == 'drop'
                    and f.get('packet_kind') == 'data'
                    and f.get('payload_bytes') == size
                    and f.get('retransmission_observed') is True
                    and f.get('retransmission_flag') is True
                    and f.get('cumulative_ack_observed') is True
                    and f.get('later_data_observed_before_retransmission') is True
                    and f.get('loss_report_observed') is True
                    and isinstance(f.get('loss_report_relay_ordinal'), int)
                    and isinstance(f.get('retransmission_relay_ordinal'), int)
                    and f['loss_report_relay_ordinal'] < f['retransmission_relay_ordinal']
                    for f in faults))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--sender-peer', type=Path, required=True)
    parser.add_argument('--receiver-peer', type=Path, required=True)
    parser.add_argument('--scenario', choices=('gap', 'delayed-ack'), default='gap')
    parser.add_argument('--loss-packets', type=int, choices=range(1, 17), default=1)
    parser.add_argument('--artifacts', type=Path,
                        help='Retain evidence in a new directory; otherwise use cleaned temporary storage')
    args = parser.parse_args()
    if args.artifacts is not None:
        return run(args.sender_peer, args.receiver_peer, args.artifacts, args.loss_packets, args.scenario)
    with tempfile.TemporaryDirectory(prefix='srt-live-nak-') as temporary:
        return run(args.sender_peer, args.receiver_peer, Path(temporary) / 'run', args.loss_packets, args.scenario)


if __name__ == '__main__':
    raise SystemExit(main())
