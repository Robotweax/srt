#!/usr/bin/env python3
"""Public API qualification of the provisional, default-off identity extension."""
from __future__ import annotations

import argparse
import json
import os
from pathlib import Path
import secrets
import socket
import subprocess
import tempfile
import time


def free_port(host: str) -> int:
    family = socket.AF_INET6 if host == '::1' else socket.AF_INET
    with socket.socket(family, socket.SOCK_DGRAM) as candidate:
        candidate.bind((host, 0))
        return candidate.getsockname()[1]


def run_case(root, name, programs, policies, *, rendezvous=False, crypto=None, key=16, host='127.0.0.1'):
    directory = root / name
    directory.mkdir()
    data = bytes((i * 17 + 3) % 256 for i in range(4096 if crypto is None else 1316 * 96))
    source, destination = directory / 'input.bin', directory / 'output.bin'
    source.write_bytes(data)
    ports = [free_port(host), free_port(host)]
    while ports[0] == ports[1]:
        ports[1] = free_port(host)
    environment = os.environ.copy()
    environment['SRT_IDENTITY_TEST_SECRET'] = secrets.token_hex(16)
    commands = []
    for index, (program, policy) in enumerate(zip(programs, policies)):
        role = ('rendezvous-sender' if index == 0 else 'rendezvous-receiver') if rendezvous else ('caller' if index == 0 else 'listener')
        command = [str(program), role, '--host', host, '--port', str(ports[1-index] if rendezvous else ports[1]),
                   '--bytes', str(len(data)), '--timeout-ms', '5000', '--shutdown-grace-ms', '250',
                   '--input' if index == 0 else '--output', str(source if index == 0 else destination)]
        if rendezvous:
            command += ['--local-host', host, '--local-port', str(ports[index])]
        if policy is not None:
            command += ['--identity-policy', str(policy), '--expect-peer-identity', str(policies[1-index] or 0)]
        if crypto:
            command += ['--passphrase-env', 'SRT_IDENTITY_TEST_SECRET', '--pbkeylen', str(key),
                        '--crypto-mode', crypto, '--expect-crypto-mode', crypto,
                        '--km-refresh-rate', '32', '--km-preannounce', '8', '--chunk-size', '1316', '--latency-ms', '0']
        commands.append(command)
    processes, handles, codes = [], [], []
    try:
        for index in (1, 0):
            log = directory / f'peer-{index}.log'
            handle = log.open('w')
            handles.append(handle)
            process = subprocess.Popen(commands[index], stdout=handle, stderr=subprocess.STDOUT, env=environment)
            processes.append(process)
            if index == 1 and not rendezvous:
                deadline = time.monotonic() + 5
                while time.monotonic() < deadline:
                    if any('"event":"ready"' in line for line in log.read_text().splitlines()):
                        break
                    if process.poll() is not None:
                        raise RuntimeError(f'{name}: listener exited before ready; see {log}')
                    time.sleep(.01)
                else:
                    raise RuntimeError(f'{name}: listener readiness timeout')
        for process in processes:
            codes.append(process.wait(timeout=12))
        if codes != [0, 0] or not destination.exists() or destination.read_bytes() != data:
            raise RuntimeError(f'{name}: transfer failed, exit codes {codes}; see {directory}')
        observations = []
        for index, policy in enumerate(policies):
            events = []
            for line in (directory / f'peer-{index}.log').read_text().splitlines():
                try:
                    event = json.loads(line)
                except ValueError:
                    continue
                if isinstance(event, dict) and event.get('event') == 'peer_identity':
                    events.append(event)
            if policy is not None and len(events) != 1:
                raise RuntimeError(f'{name}: missing identity API observation')
            observations.extend(events)
        result = dict(name=name, payload_bytes=len(data), integrity=True, rendezvous=rendezvous,
                      policies=policies, host=host, crypto=crypto, key_bytes=key if crypto else None,
                      observations=observations)
        print('PASS', name, flush=True)
        return result
    finally:
        for process in processes:
            if process.poll() is None:
                process.terminate()
                try:
                    process.wait(timeout=1)
                except subprocess.TimeoutExpired:
                    process.kill()
                    process.wait()
        for handle in handles:
            handle.close()


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--robotweax-peer', type=Path, required=True)
    parser.add_argument('--reference-peer', type=Path)
    parser.add_argument('--reference-pin')
    parser.add_argument('--with-aead', action='store_true')
    parser.add_argument('--host', choices=['127.0.0.1', '::1'], default='127.0.0.1')
    parser.add_argument('--output', type=Path)
    args = parser.parse_args()
    if bool(args.reference_peer) != bool(args.reference_pin):
        parser.error('--reference-peer and --reference-pin are required together')
    robot = args.robotweax_peer.resolve(strict=True)
    reference = args.reference_peer.resolve(strict=True) if args.reference_peer else None
    with tempfile.TemporaryDirectory(prefix='srt-identity-') as temporary:
        root = args.output.resolve() if args.output else Path(temporary)
        root.mkdir(parents=True, exist_ok=True)
        results = []
        for rendezvous in (False, True):
            prefix = 'rdv' if rendezvous else 'caller-listener'
            for left in range(3):
                for right in range(3):
                    results.append(run_case(root, f'{prefix}-{left}-{right}', [robot, robot], [left, right], rendezvous=rendezvous, host=args.host))
            if args.with_aead:
                for mode in ('ctr', 'gcm'):
                    for key in (16, 24, 32):
                        results.append(run_case(root, f'{prefix}-{mode}-{key}', [robot, robot], [2, 2], rendezvous=rendezvous, crypto=mode, key=key, host=args.host))
            if reference:
                for policy in range(3):
                    for reverse in (False, True):
                        programs, policies = ([reference, robot], [None, policy]) if reverse else ([robot, reference], [policy, None])
                        results.append(run_case(root, f'{prefix}-reference-{policy}-{int(reverse)}', programs, policies, rendezvous=rendezvous, host=args.host))
        (root / 'results.json').write_text(json.dumps(dict(reference_pin=args.reference_pin, cases=results), indent=2) + '\n')
        print(f'{len(results)} identity API and payload scenarios passed', flush=True)


if __name__ == '__main__':
    main()
