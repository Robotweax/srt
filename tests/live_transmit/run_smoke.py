#!/usr/bin/env python3
"""Bounded loopback qualification of the external Robotweax-backed application."""
from __future__ import annotations
import argparse
import json
from pathlib import Path
import socket
import subprocess
import sys
import threading
import time

ROOT = Path(__file__).resolve().parents[2]
PASSPHRASE = 'synthetic-local-test-only'


class Smoke:
    def __init__(self, binary: Path, artifacts: Path):
        self.binary = binary.resolve()
        self.artifacts = artifacts.resolve()
        self.artifacts.mkdir(parents=True, exist_ok=True)
        self.allocated: set[int] = set()
        self.results: list[dict] = []

    def port(self) -> int:
        for _ in range(128):
            with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as s:
                s.bind(('127.0.0.1', 0))
                value = s.getsockname()[1]
            if value not in self.allocated:
                self.allocated.add(value)
                return value
        raise RuntimeError('unable to allocate distinct loopback ports')

    @staticmethod
    def wait(process: subprocess.Popen, log: Path, marker: str) -> None:
        deadline = time.monotonic() + 10
        while time.monotonic() < deadline:
            if marker in log.read_text(errors='replace'):
                return
            if process.poll() is not None:
                break
            time.sleep(.02)
        raise RuntimeError(f'missing readiness marker {marker!r}; see {log.name}')

    def case(self, name: str, enabled: bool = True, encrypted: bool = False,
             listener_sender: bool = False, drop: int = 0) -> None:
        children: list[subprocess.Popen] = []
        streams = []
        feed = None
        stop = threading.Event()
        sink = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        sink.bind(('127.0.0.1', 0))
        sink.settimeout(.1)
        udp_in, srt_in, relay = self.port(), self.port(), self.port()
        secret = f'&passphrase={PASSPHRASE}&pbkeylen=32' if encrypted else ''
        fec = '&packetfilter=fec,cols:8,rows:8,layout:even,arq:always'
        receiver = (f'srt://127.0.0.1:{relay if listener_sender else srt_in}'
                    f'?mode={"caller" if listener_sender else "listener"}&latency=120'
                    + secret + (fec if listener_sender else ''))
        sender = (f'srt://127.0.0.1:{srt_in if listener_sender else relay}'
                  f'?mode={"listener" if listener_sender else "caller"}&latency=120'
                  f'&arqfirst={"yes" if enabled else "no"}' + secret + ('' if enabled else fec))
        common = [str(self.binary), '-v', '-a:no', '-c:1316']
        udp_source = f'udp://127.0.0.1:{udp_in}'
        udp_target = f'udp://127.0.0.1:{sink.getsockname()[1]}'

        def start(label, args):
            log = self.artifacts / f'{name}-{label}.log'
            stream = log.open('w')
            streams.append(stream)
            process = subprocess.Popen(args, stdout=stream, stderr=subprocess.STDOUT)
            children.append(process)
            return process, log

        try:
            first, first_log = start('sender' if listener_sender else 'receiver',
                [*common, udp_source, sender] if listener_sender else [*common, receiver, udp_target])
            self.wait(first, first_log, 'Opening SRT target listener' if listener_sender else 'Opening SRT source listener')
            proxy, proxy_log = start('proxy', [sys.executable,
                str(ROOT / 'examples/applications/impairment_proxy.py'),
                '--listen', f'127.0.0.1:{relay}', '--target', f'127.0.0.1:{srt_in}',
                '--duration', '8', '--drop-every', str(drop), '--impair-direction',
                'reverse' if listener_sender else 'forward'])
            self.wait(proxy, proxy_log, '"ready"')
            second, second_log = start('receiver' if listener_sender else 'sender',
                [*common, receiver, udp_target] if listener_sender else [*common, udp_source, sender])
            sender_process, sender_log = (first, first_log) if listener_sender else (second, second_log)
            self.wait(sender_process, sender_log,
                'Accepted SRT target connection' if listener_sender else 'SRT target connected')
            assert f'Robotweax ARQ-first={"on" if enabled else "off"} tail-probe=off' in sender_log.read_text()
            payloads = [i.to_bytes(4, 'big') + bytes((i + j) % 256 for j in range(1312)) for i in range(128)]
            received = []

            def send():
                with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as source:
                    for payload in payloads:
                        if stop.is_set():
                            return
                        source.sendto(payload, ('127.0.0.1', udp_in))
                        stop.wait(.005)

            feed = threading.Thread(target=send)
            feed.start()
            deadline = time.monotonic() + 8
            while len(received) < len(payloads) and time.monotonic() < deadline:
                try:
                    received.append(sink.recv(2048))
                except socket.timeout:
                    pass
            feed.join(timeout=2)
            assert not feed.is_alive(), 'UDP feeder did not finish'
            assert received == payloads, f'{name}: received {len(received)}/128 exact ordered datagrams'
            assert proxy.wait(timeout=10) == 0, 'impairment proxy failed'
            summary = next(json.loads(line) for line in proxy_log.read_text().splitlines()
                           if '"proxy_summary"' in line)
            assert summary['dropped'] == (8 if drop else 0), summary
            assert summary['queue_dropped'] == 0, summary
            self.results.append({'name': name, 'received': 128, 'expected': 128,
                'exact_payloads': True, 'arq_first': enabled, 'tail_probe': False,
                'encrypted': encrypted, 'listener_sender': listener_sender,
                'dropped_originals': summary['dropped']})
        except Exception:
            # Keep failure diagnosis in the job console, never in CI artifacts.
            for process, stream in zip(children, streams):
                stream.flush()
                log = Path(stream.name)
                diagnostic = log.read_text(errors='replace')[-6000:].replace(PASSPHRASE, '<redacted>')
                print(f'Failure diagnostic: {log.name}; exit={process.poll()}\n{diagnostic}',
                      file=sys.stderr, flush=True)
            raise
        finally:
            stop.set()
            if feed:
                feed.join(timeout=2)
            for child in reversed(children):
                if child.poll() is None:
                    child.terminate()
                try:
                    child.wait(timeout=3)
                except subprocess.TimeoutExpired:
                    child.kill()
                    child.wait(timeout=3)
            for stream in streams:
                stream.close()
            sink.close()
        for log in self.artifacts.glob(f'{name}-*.log'):
            assert PASSPHRASE not in log.read_text(errors='replace'), 'application exposed a fixture passphrase'
        print(f'PASS {name}', flush=True)

    def negative(self) -> list[dict]:
        results = []
        cases = [
            ('value', 'arqfirst=invalid', 'arqfirst must be'),
            ('file', 'arqfirst=yes&transtype=file', 'arqfirst requires'),
            ('rendezvous', 'arqfirst=yes&mode=rendezvous', 'arqfirst requires'),
            ('implicit-rendezvous', 'arqfirst=yes&adapter=127.0.0.1', 'arqfirst requires'),
            ('tsbpd', 'arqfirst=yes&tsbpdmode=no', 'ConfigurePre'),
            ('row-filter', 'arqfirst=yes&packetfilter=fec,cols:8,rows:1,arq:always', 'ConfigurePre'),
            ('group', 'arqfirst=yes&groupconnect=yes', 'arqfirst requires'),
        ]
        for name, options, marker in cases:
            result = subprocess.run([str(self.binary), '-a:no',
                f'udp://127.0.0.1:{self.port()}', f'srt://127.0.0.1:{self.port()}?{options}'],
                capture_output=True, text=True, timeout=5)
            output = result.stdout + result.stderr
            (self.artifacts / f'invalid-{name}.log').write_text(output)
            assert result.returncode != 0, name
            assert 'TIMEOUT: INTERRUPT' not in output, name
            if marker:
                assert marker in output, name
            results.append({'name': name, 'rejected': True})
        for value in ('yes', 'no'):
            result = subprocess.run([str(self.binary), '-a:no',
                f'srt://127.0.0.1:{self.port()}?mode=listener&arqfirst={value}',
                f'udp://127.0.0.1:{self.port()}'], capture_output=True, text=True, timeout=5)
            assert result.returncode != 0 and 'arqfirst is an SRT output option' in result.stderr
            results.append({'name': f'input-{value}', 'rejected': True})
        return results

    def run(self) -> None:
        version = subprocess.run([str(self.binary), '-version'], capture_output=True,
                                 text=True, timeout=5)
        output = version.stdout + version.stderr
        assert version.returncode == 0 and 'Transport provider: Robotweax SRT' in output
        (self.artifacts / 'version.log').write_text(output)
        self.case('always-control', enabled=False)
        self.case('arqfirst-clear-loss', drop=16)
        self.case('arqfirst-ctr-loss', encrypted=True, drop=16)
        self.case('arqfirst-listener-loss', listener_sender=True, drop=16)
        negative = self.negative()
        (self.artifacts / 'results.json').write_text(json.dumps(
            {'positive': self.results, 'negative': negative, 'passed': True}, indent=2) + '\n')
        print(f'PASS: {len(self.results)} payload cases; {len(negative)} invalid configurations rejected')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--binary', type=Path, required=True)
    parser.add_argument('--artifacts', type=Path, required=True)
    args = parser.parse_args()
    Smoke(args.binary, args.artifacts).run()


if __name__ == '__main__':
    main()
